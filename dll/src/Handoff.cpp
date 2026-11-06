#include "pch.h"

#include "Handoff.h"
#include "Log.h"
#include "Settings.h"

namespace
{
	// ExtraDataList::GetLock() is private in CommonLibSSE-NG, but the booking list we
	// edit lives in that extra data, so we take the same lock the game and CommonLib
	// take. An explicit template instantiation may name a private member; this hands
	// out a pointer to it.
	struct ExtraListLockTag
	{
		using type = RE::BSReadWriteLock& (RE::ExtraDataList::*)() const noexcept;
	};
	template <ExtraListLockTag::type M>
	struct ExtraListLockAccess
	{
		friend ExtraListLockTag::type GetLockMember(ExtraListLockTag) { return M; }
	};
	ExtraListLockTag::type GetLockMember(ExtraListLockTag);
	template struct ExtraListLockAccess<&RE::ExtraDataList::GetLock>;

	RE::BSReadWriteLock& ExtraListLock(RE::ExtraDataList& a_list) { return (a_list.*GetLockMember(ExtraListLockTag{}))(); }
}

namespace Handoff
{
	namespace
	{
		// A hand-off in progress: the player clicked, the NPC is getting up.
		struct Pending
		{
			RE::ObjectRefHandle furniture;
			RE::ObjectRefHandle activate;  // what to click for the player: the station, or the vein in front of a mining spot
			RE::ActorHandle     npc;
			float               elapsed = 0.0f;
			bool                force = false;  // vanilla doesn't move this NPC (miners, woodcutters): ask it to stop, then try a few times
			bool                asked = false;  // asked the NPC to stop since the last try
			bool                held = false;   // the NPC's AI is paused so their routine can't take the spot back mid-hand-off
			bool                quick = false;  // asked again with the immediate step-off, after the normal one stalled
			float               askedAt = 0.0f;
			float               wait = 0.0f;    // pause after a try, so the game can settle
			bool                delayed = false;  // test builds: the hold delay was used
			bool                holdStarted = false;  // the NPC was held at least once (held again after a menu)
			int                 tries = 0;
		};

		std::optional<Pending> g_pending;
#ifdef IUTN_SELFTEST
		float g_testHoldDelay = 0.0f;  // test only: stay held this long before the player's activation
#endif

		// The message to show instead of vanilla's, and until when (game time,
		// real seconds since the click) the swap applies.
		// Vanilla's "in use" texts, read from the game settings at load so
		// translated games match too.
		std::vector<std::string> g_inUseTexts;

		std::mutex  g_messageLock;
		std::string g_message;
		float       g_messageWindow = 0.0f;
		bool        g_swapExpected = false;  // a swap was prepared and hasn't happened yet
		bool        g_loggedMenuWait = false;
		bool        g_ownActivation = false;  // the mod's own activation for the player is running

		constexpr float kMaxPlayerDistance = 400.0f;  // ~5.7 m; activation reach is ~150-200

		bool IsPlayer(const RE::TESObjectREFR* a_ref) { return a_ref && a_ref->IsPlayerRef(); }

		// Ore veins, geodes, clay, quarries and the like: an activator the player clicks,
		// which hands over to an invisible furniture marker (its linked reference) that
		// miners sit at. The player's click then reaches that marker.
		bool IsVeinLike(RE::TESObjectREFR* a_ref)
		{
			const auto base = a_ref ? a_ref->GetBaseObject() : nullptr;
			if (!base || !base->Is(RE::FormType::Activator)) {
				return false;
			}
			const std::string_view name = base->GetName();
			return name.contains("Vein") || name.contains("Deposit") || name.contains("Quarry");
		}

		RE::TESObjectREFR* FindVeinFor(RE::TESObjectREFR* a_marker)
		{
			RE::TESObjectREFR* found = nullptr;
			if (const auto tes = RE::TES::GetSingleton(); tes && a_marker) {
				tes->ForEachReferenceInRange(a_marker, 600.0f, [&](RE::TESObjectREFR* a_ref) {
					if (a_ref != a_marker && IsVeinLike(a_ref) && a_ref->GetLinkedRef(nullptr) == a_marker) {
						found = a_ref;
						return RE::BSContainer::ForEachResult::kStop;
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}
			return found;
		}

		// The NPC who stepped off keeps a booking on the spot for a few seconds. Vanilla's
		// mining script only listens for the player's pickaxe strikes (which pay out the
		// ore) when it sees the spot unbooked, so drop that NPC's booking before the
		// player takes it. Returns how many were dropped.
		int DropBooking(RE::TESObjectREFR* a_spot, RE::Actor* a_npc)
		{
			const auto reserved = a_spot && a_npc ? a_spot->extraList.GetByType<RE::ExtraReservedMarkers>() : nullptr;
			if (!reserved) {
				return 0;
			}
			const auto           handle = a_npc->GetHandle();
			RE::BSWriteLockGuard locker(ExtraListLock(a_spot->extraList));
			auto&                list = reserved->reservedMarkerArray;
			int                  dropped = 0;
			for (auto it = list.begin(); it != list.end();) {
				if (it->actorinMarker == handle) {
					it = list.erase(it);
					++dropped;
				} else {
					++it;
				}
			}
			return dropped;
		}

		// Who is on the furniture. For mining spots and chopping blocks, an NPC who has
		// already stepped off (standing, and not on any of its spots) doesn't count, even
		// if the game still lists the furniture as theirs for a moment.
		RE::Actor* ActiveOccupant(RE::TESObjectREFR* a_spot, bool a_force)
		{
			const auto actor = FindOccupant(a_spot);
			if (!actor || !a_force) {
				return actor;
			}
			const auto state = actor->AsActorState();
			if (!state || state->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal) {
				return actor;
			}
			const auto used = a_spot->extraList.GetByType<RE::ExtraUsedMarkers>();
			const auto handle = actor->GetHandle();
			const bool onSpot = used && std::ranges::any_of(used->usedMarkers, [&](const RE::MarkerUsedData& a_m) { return a_m.actorinMarker == handle; });
			return onSpot ? actor : nullptr;
		}

#ifdef IUTN_SELFTEST
		// Who the game counts as on or booked for a furniture's spots (research log only).
		std::string DescribeMarkers(RE::TESObjectREFR* a_ref)
		{
			std::string text;
			auto        add = [&](const char* a_label, const RE::BSTArray<RE::MarkerUsedData>& a_list) {
                text += std::format(" {}[", a_label);
                for (const auto& m : a_list) {
                    const auto actor = m.actorinMarker.get();
                    text += std::format(" {:08X}#{}@{:.2f}", actor ? actor->GetFormID() : 0, m.markerID, m.expiration.timeStamp);
                }
                text += " ]";
			};
			if (const auto used = a_ref ? a_ref->extraList.GetByType<RE::ExtraUsedMarkers>() : nullptr) {
				add("used", used->usedMarkers);
			} else {
				text += " used-none";
			}
			if (const auto reserved = a_ref ? a_ref->extraList.GetByType<RE::ExtraReservedMarkers>() : nullptr) {
				add("reserved", reserved->reservedMarkerArray);
			} else {
				text += " reserved-none";
			}
			return text;
		}
#endif

		// The furniture the mod takes over from an NPC: crafting stations, wood chopping
		// blocks, and the markers behind ore veins and the like. Beds and chairs are not.
		// a_label gets what to call it in the log.
		bool IsWorkSpot(RE::TESFurniture* a_base, RE::TESObjectREFR* a_ref, std::string* a_label = nullptr)
		{
			if (!a_base) {
				return false;
			}
			const std::string_view name = a_base->GetName();
			if (a_base->workBenchData.benchType.get() != RE::TESFurniture::WorkBenchData::BenchType::kNone ||
				name.contains("Chopping Block")) {
				if (a_label) {
					*a_label = std::string(name);
				}
				return true;
			}
			if (name == "This should not be visible") {
				if (const auto vein = FindVeinFor(a_ref)) {
					if (a_label) {
						*a_label = std::string(vein->GetBaseObject()->GetName()) + " (mining spot)";
					}
					return true;
				}
			}
			return false;
		}
	}

	// Why the mod leaves this NPC alone (the click then behaves exactly as
	// vanilla), or nullptr if it takes the station.
	const char* WhyNot(RE::Actor* a_npc)
	{
		if (!a_npc) {
			return "no NPC";
		}
		if (a_npc->IsPlayerRef()) {
			return "it is the player";
		}
		if (a_npc->IsDead()) {
			return "NPC is dead";
		}
		if (a_npc->IsInCombat()) {
			return "NPC is in combat";
		}
		if (a_npc->GetCurrentScene()) {
			return "NPC is in a scene";
		}
		if (const auto topics = RE::MenuTopicManager::GetSingleton(); topics && topics->speaker == a_npc->GetHandle()) {
			return "NPC is talking to the player";
		}
		if (const auto pkg = a_npc->GetCurrentPackage()) {
			using T = RE::PACKAGE_TYPE;
			const auto type = pkg->packData.packType.get();
			if (type == T::kDialogue || type == T::kInGameDialogue) {
				return "NPC is in a dialogue package";
			}
			if (pkg->ownerQuest) {
				return "NPC is doing quest work";
			}
		}
		if (!Settings::Get().includeFollowers && a_npc->IsPlayerTeammate()) {
			return "NPC is a follower and followers are set to keep their station";
		}
		return nullptr;
	}

	bool IsEligible(RE::Actor* a_npc) { return !WhyNot(a_npc); }

	namespace
	{
		std::string MakeMessage(RE::Actor* a_npc)
		{
			const auto& settings = Settings::Get();
			const char* name = a_npc ? a_npc->GetDisplayFullName() : nullptr;
			if (!name || !*name) {
				return settings.messageNoName;
			}
			std::string text = settings.message;
			if (const auto pos = text.find("{name}"); pos != std::string::npos) {
				text.replace(pos, 6, name);
			}
			return text;
		}

		struct FurnitureActivate
		{
			static bool thunk(RE::TESFurniture* a_this, RE::TESObjectREFR* a_target, RE::TESObjectREFR* a_activator,
				std::uint8_t a_arg3, RE::TESBoundObject* a_object, std::int32_t a_count)
			{
#ifdef IUTN_SELFTEST
				logs::info("FURN-ACT target {:08X} '{}' base {:08X} bench={} by {:08X} '{}'", a_target ? a_target->GetFormID() : 0, a_this->GetName(),
					a_this->GetFormID(), std::to_underlying(a_this->workBenchData.benchType.get()), a_activator ? a_activator->GetFormID() : 0,
					a_activator ? a_activator->GetDisplayFullName() : "");
#endif
				std::string label;
				if (a_target && IsPlayer(a_activator) && !g_ownActivation && IsWorkSpot(a_this, a_target, &label)) {
					Decide(label.c_str(), a_target);
				}
#ifdef IUTN_SELFTEST
				else if (a_target && a_activator && !IsPlayer(a_activator)) {
					logs::info("NPC {:08X} activated {:08X} (bench={})", a_activator->GetFormID(), a_target->GetFormID(),
						std::to_underlying(a_this->workBenchData.benchType.get()));
				}
#endif
				return func(a_this, a_target, a_activator, a_arg3, a_object, a_count);
			}

			// The player clicked a crafting station: log what was decided and why.
			static void Decide(const char* a_name, RE::TESObjectREFR* a_target)
			{
				const auto id = a_target->GetFormID();
				const auto name = a_name;
				if (!Settings::Get().enabled) {
					Log::Event("Player clicked {:08X} '{}': mod is switched off in the settings, left alone (vanilla)", id, name);
					return;
				}
				if (g_pending) {
					if (g_pending->furniture.get().get() != a_target) {  // the same spot is the hand-off's own activation coming through the game's script
						Log::Event("Player clicked {:08X} '{}' while a hand-off is already running: ignored", id, name);
					}
					return;
				}
				const auto npc = FindOccupant(a_target);
				if (!npc) {
					Log::Event("Player clicked {:08X} '{}': nobody is using it, nothing to do", id, name);
					return;
				}
				if (const auto base = a_target->GetBaseObject() ? a_target->GetBaseObject()->As<RE::TESFurniture>() : nullptr;
					base && base->workBenchData.benchType.get() == RE::TESFurniture::WorkBenchData::BenchType::kNone &&
					!ActiveOccupant(a_target, true)) {
					Log::Event("Player clicked {:08X} '{}': {:08X} '{}' is only on the way to it, nothing to do", id, name, npc->GetFormID(),
						npc->GetDisplayFullName());
					return;
				}
				const char* why = WhyNot(npc);
				Log::Event("Player clicked {:08X} '{}' used by {:08X} '{}': {}", id, name, npc->GetFormID(),
					npc->GetDisplayFullName(), why ? "left alone (vanilla)" : "taking over");
				LogActor(why ? why : "occupant", npc);
				if (!why) {
					const auto vein = FindVeinFor(a_target);
					const auto furnBase = a_target->GetBaseObject() ? a_target->GetBaseObject()->As<RE::TESFurniture>() : nullptr;
					const bool notBench = furnBase && furnBase->workBenchData.benchType.get() == RE::TESFurniture::WorkBenchData::BenchType::kNone;
					g_pending = Pending{ a_target->GetHandle(), (vein ? vein : a_target)->GetHandle(), npc->GetHandle(), 0.0f, notBench };
					std::scoped_lock lock(g_messageLock);
					g_message = MakeMessage(npc);
					g_messageWindow = 2.0f;
					g_swapExpected = true;
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Runs every frame: waits for the station to free up, then uses it for the player.
		struct PlayerUpdate
		{
			static void thunk(RE::PlayerCharacter* a_player, float a_delta)
			{
				func(a_player, a_delta);
				{
					std::scoped_lock lock(g_messageLock);
					if (g_messageWindow > 0.0f) {
						g_messageWindow = (std::max)(0.0f, g_messageWindow - a_delta);
						if (g_messageWindow == 0.0f && g_swapExpected) {
							g_swapExpected = false;
							Log::Event("Message not swapped: the game's 'in use' text never showed in the 2 s after the click");
						}
					}
				}
				if (g_pending) {
					Tick(a_player, a_delta);
				}
			}

			static void Tick(RE::PlayerCharacter* a_player, float a_delta)
			{
				auto& p = *g_pending;
				p.elapsed += a_delta;
				const auto furniture = p.furniture.get();
				const auto npc = p.npc.get();
				if (p.force && furniture && a_player->GetOccupiedFurniture() == p.furniture) {
					Log::Event("Player is on the spot after {} {} ({:.2f}s)", p.tries, p.tries == 1 ? "try" : "tries", p.elapsed);
#ifdef IUTN_SELFTEST
					logs::info("Spot markers with the player on it:{}", DescribeMarkers(furniture.get()));
#endif
					Release(p, npc.get());
					g_pending.reset();
					return;
				}
#ifdef IUTN_SELFTEST
				if (furniture && npc && static_cast<int>(p.elapsed) != static_cast<int>(p.elapsed - a_delta)) {
					const auto state = npc->AsActorState();
					logs::info("Hand-off {:.0f}s: '{}' sit={} pkg type={} furniture={:08X}{}", p.elapsed, npc->GetDisplayFullName(),
						state ? std::to_underlying(state->GetSitSleepState()) : -1,
						npc->GetCurrentPackage() ? std::to_underlying(npc->GetCurrentPackage()->packData.packType.get()) : -1,
						npc->GetOccupiedFurniture().get() ? npc->GetOccupiedFurniture().get()->GetFormID() : 0, DescribeMarkers(furniture.get()));
				}
#endif
				if (p.force && p.holdStarted && !p.held && npc) {
					if (const auto ui = RE::UI::GetSingleton(); !ui || !ui->GameIsPaused()) {
						p.held = true;
						npc->EnableAI(false);
						Log::Event("{:.2f}s in: menu closed; holding '{}' still again", p.elapsed, npc->GetDisplayFullName());
					}
				}
				const char* stop = nullptr;
				if (!furniture || !npc) {
					stop = "station or NPC gone";
				} else if (p.elapsed > Settings::Get().maxWaitSeconds) {
					stop = p.tries > 0 ? "the player didn't get onto the spot in time" : "NPC didn't get up in time";
				} else if (a_player->IsInCombat()) {
					stop = "player in combat";
				} else if (const auto own = a_player->GetOccupiedFurniture(); own && own != p.furniture) {
					stop = "player is using other furniture";  // vanilla books the clicked one for the player
				} else if (a_player->GetPosition().GetDistance(furniture->GetPosition()) > kMaxPlayerDistance) {
					stop = "player walked away";
				} else if (const auto ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused()) {
					if (!g_loggedMenuWait) {
						g_loggedMenuWait = true;
						Log::Event("Hand-off waiting: a menu is open ({:.2f}s in)", p.elapsed);
					}
					Release(p, npc.get());  // never leave an NPC held in a save made from a menu; held again after
					return;                 // a menu is open: wait, don't count it as failure
				} else if (p.wait > 0.0f) {
					p.wait -= a_delta;
					return;
				} else if (const auto occupant = ActiveOccupant(furniture.get(), p.force); occupant && occupant != npc.get()) {
					stop = "someone else took it";
				} else if (!occupant) {
					const auto target = p.activate.get();
					if (!p.force) {
						Log::Event("Station free after {:.2f}s; using it for the player", p.elapsed);
						g_pending.reset();
						g_loggedMenuWait = false;
						g_ownActivation = true;
						const bool ok = (target ? target.get() : furniture.get())->ActivateRef(a_player, 0, nullptr, 1, false);
						g_ownActivation = false;
						Log::Event("Activation for the player {}", ok ? "went through" : "was refused by the game");
						return;
					}
					// A mining spot or chopping block: the NPC's own routine would take it
					// straight back, and the vanilla script needs it unbooked (see DropBooking).
					if (p.tries >= 3) {
						stop = "the player didn't get onto the spot (3 tries)";
					} else {
						if (!p.held) {
							// Their routine would take the spot straight back, so hold them still
							// until the player is on it (released at once after, or on any stop).
							p.held = true;
							p.holdStarted = true;
							npc->EnableAI(false);
							Log::Event("{:.2f}s in: '{}' has stepped off; holding them still while the player takes the spot", p.elapsed,
								npc->GetDisplayFullName());
						}
#ifdef IUTN_SELFTEST
						if (g_testHoldDelay > 0.0f && !p.delayed) {
							p.delayed = true;
							p.wait = g_testHoldDelay;
							logs::info("Test: holding for {:g}s before the player's activation", g_testHoldDelay);
							return;
						}
#endif
						++p.tries;
						const int dropped = DropBooking(furniture.get(), npc.get());
						Log::Event("Spot free after {:.2f}s, try {}: using it for the player{}", p.elapsed, p.tries,
							dropped ? std::format(" (dropped {}'s leftover booking)", npc->GetDisplayFullName()) : std::string());
#ifdef IUTN_SELFTEST
						logs::info("Spot markers before the player's activation:{}", DescribeMarkers(furniture.get()));
#endif
						g_ownActivation = true;
						const bool ok = (target ? target.get() : furniture.get())->ActivateRef(a_player, 0, nullptr, 1, false);
						g_ownActivation = false;
						Log::Event("Activation for the player {}", ok ? "went through" : "was refused by the game");
						p.wait = 1.5f;
						return;
					}
				}
				if (!stop && p.force && p.asked && !p.quick && p.elapsed - p.askedAt >= 3.0f && npc &&
					ActiveOccupant(furniture.get(), true) == npc.get()) {
					// Some routines (wood chopping) can hang in their step-off for many seconds.
					p.quick = true;
					const auto state = npc->AsActorState();
					Log::Event("{:.2f}s in: '{}' is stuck stepping off (sit state {}); making them step off at once", p.elapsed,
						npc->GetDisplayFullName(), state ? std::to_underlying(state->GetSitSleepState()) : -1);
					npc->StopInteractingQuick(true);
				}
				if (!stop && p.force && !p.asked && p.elapsed >= 0.3f && npc && FindOccupant(furniture.get()) == npc.get()) {
					// Vanilla doesn't move this NPC. Send them off; the player takes the spot
					// as soon as it shows free (the check above), before their routine does.
					p.asked = true;
					p.askedAt = p.elapsed;
					Log::Event("{:.2f}s in: '{}' is still on it; asking them to stop", p.elapsed, npc->GetDisplayFullName());
#ifdef IUTN_SELFTEST
					logs::info("Spot markers when asking:{}", DescribeMarkers(furniture.get()));
#endif
					npc->StopInteractingQuick(false);
				}
				if (stop) {
					Log::Event("Hand-off stopped after {:.2f}s (wait limit {:g}s): {}", p.elapsed, Settings::Get().maxWaitSeconds, stop);
					if (npc && std::string_view(stop) == "NPC didn't get up in time") {
						LogActor("NPC still on the station", npc.get());
					}
					Release(p, npc.get());
					g_pending.reset();
					g_loggedMenuWait = false;
				}
			}

			// Lets a held NPC go back to their routine.
			static void Release(Pending& a_pending, RE::Actor* a_npc)
			{
				if (a_pending.held) {
					a_pending.held = false;
					if (a_npc) {
						a_npc->EnableAI(true);
						Log::Event("'{}' is free to go back to their routine", a_npc->GetDisplayFullName());
					}
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Swaps vanilla's "already in use" notification for the mod's message.
		struct HUDProcessMessage
		{
			static RE::UI_MESSAGE_RESULTS thunk(RE::HUDMenu* a_this, RE::UIMessage& a_message)
			{
				if (const auto data = a_message.data ? skyrim_cast<RE::HUDData*>(a_message.data) : nullptr;
					data && data->type.get() == RE::HUD_MESSAGE_TYPE::kNotification) {
#ifdef IUTN_SELFTEST
					logs::info("HUD notification: '{}'", data->text.c_str());
#endif
					std::scoped_lock lock(g_messageLock);
					if (g_messageWindow > 0.0f) {
						if (IsVanillaInUse(data->text.c_str())) {
							Log::Event("Message swapped: '{}' -> '{}'", data->text.c_str(), g_message);
							data->text = g_message.c_str();
							g_messageWindow = 0.0f;
							g_swapExpected = false;
						} else {
							g_swapExpected = false;
						Log::Event("Message not swapped: the notification '{}' is not one of the game's 'in use' texts listed at load", data->text.c_str());
						}
					}
				}
				return func(a_this, a_message);
			}

			static bool IsVanillaInUse(const char* a_text)
			{
				return a_text && std::ranges::find(g_inUseTexts, std::string_view(a_text)) != g_inUseTexts.end();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

	}

	namespace
	{
		// Lets a held NPC move while a menu that pauses the game is open (a save made
		// from it must never hold a frozen NPC); the hand-off holds them again after.
		struct MenuWatch : RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && a_event->opening && g_pending && g_pending->held) {
					const auto ui = RE::UI::GetSingleton();
					const auto menu = ui ? ui->GetMenu(a_event->menuName.c_str()) : nullptr;
					if (menu && menu->PausesGame()) {
						g_pending->held = false;
						if (const auto npc = g_pending->npc.get()) {
							npc->EnableAI(true);
							Log::Event("Menu '{}' opened mid hand-off: '{}' is free to move while it's open", a_event->menuName.c_str(),
								npc->GetDisplayFullName());
						}
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		MenuWatch g_menuWatch;
	}

	void OnGameEvent(std::uint32_t a_type)
	{
		if (!g_pending) {
			return;
		}
		const char* what = a_type == SKSE::MessagingInterface::kSaveGame    ? "the game is saving" :
		                   a_type == SKSE::MessagingInterface::kPreLoadGame ? "a save is loading" :
		                                                                      "a new game is starting";
		if (g_pending->held) {
			g_pending->held = false;
			if (const auto npc = g_pending->npc.get()) {
				npc->EnableAI(true);
				Log::Event("Hand-off stopped: {}; '{}' is free to go back to their routine", what, npc->GetDisplayFullName());
			} else {
				Log::Event("Hand-off stopped: {}", what);
			}
		} else {
			Log::Event("Hand-off stopped: {}", what);
		}
		g_pending.reset();
		g_loggedMenuWait = false;
	}

#ifdef IUTN_SELFTEST
	RE::Actor* TestHeldNpc() { return g_pending && g_pending->held ? g_pending->npc.get().get() : nullptr; }
	bool       TestPending() { return g_pending.has_value(); }
	void       TestSetHoldDelay(float a_seconds) { g_testHoldDelay = a_seconds; }
#endif

	RE::Actor* FindOccupant(RE::TESObjectREFR* a_furniture)
	{
		RE::Actor* found = nullptr;
		const auto handle = a_furniture->GetHandle();
		if (const auto lists = RE::ProcessLists::GetSingleton()) {
			lists->ForEachHighActor([&](RE::Actor* a_actor) {
				if (a_actor && a_actor->GetOccupiedFurniture() == handle) {
					found = a_actor;
					return RE::BSContainer::ForEachResult::kStop;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		return found;
	}

	bool IsCraftingStation(RE::TESObjectREFR* a_ref)
	{
		const auto base = a_ref ? a_ref->GetBaseObject() : nullptr;
		return IsWorkSpot(base ? base->As<RE::TESFurniture>() : nullptr, a_ref);
	}

	void LogActor(const char* a_label, RE::Actor* a_actor)
	{
		if (!a_actor) {
			Log::Event("  {}: none", a_label);
			return;
		}
		const auto pkg = a_actor->GetCurrentPackage();
		const auto state = a_actor->AsActorState();
		Log::Event("  {}: {:08X} '{}' sit={} pkg={:08X} type={} quest={:08X} scene={:08X}",
			a_label, a_actor->GetFormID(), a_actor->GetDisplayFullName(),
			state ? std::to_underlying(state->GetSitSleepState()) : -1,
			pkg ? pkg->GetFormID() : 0, pkg ? std::to_underlying(pkg->packData.packType.get()) : -1,
			pkg && pkg->ownerQuest ? pkg->ownerQuest->GetFormID() : 0,
			a_actor->GetCurrentScene() ? a_actor->GetCurrentScene()->GetFormID() : 0);
	}

#ifdef IUTN_SELFTEST
	void TestClearInUseTexts() { g_inUseTexts.clear(); }
#endif

	void Install()
	{
		REL::Relocation<std::uintptr_t> furniture{ RE::VTABLE_TESFurniture[0] };
		FurnitureActivate::func = furniture.write_vfunc(0x37, FurnitureActivate::thunk);

		REL::Relocation<std::uintptr_t> player{ RE::VTABLE_PlayerCharacter[0] };
		PlayerUpdate::func = player.write_vfunc(0xAD, PlayerUpdate::thunk);

		REL::Relocation<std::uintptr_t> hud{ RE::VTABLE_HUDMenu[0] };
		HUDProcessMessage::func = hud.write_vfunc(0x04, HUDProcessMessage::thunk);

		const bool ok = FurnitureActivate::func.address() && PlayerUpdate::func.address() && HUDProcessMessage::func.address();
		if (ok) {
			logs::info("Hooks installed: station activation, player update, HUD notifications");
		} else {
			Log::Problem("Hook install FAILED (activation={} update={} hud={}): the mod will not work",
				FurnitureActivate::func.address() != 0, PlayerUpdate::func.address() != 0, HUDProcessMessage::func.address() != 0);
		}
	}

	void OnDataLoaded()
	{
		if (const auto ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuWatch);
		}
		if (const auto gmst = RE::GameSettingCollection::GetSingleton()) {
			for (const auto& item : gmst->settings) {
				const char* name = item.first;
				const auto  setting = item.second;
#ifdef IUTN_SELFTEST
				if (setting && setting->GetType() == RE::Setting::Type::kString && setting->GetString() &&
					std::string_view(setting->GetString()).find("using this") != std::string_view::npos) {
					logs::info("Setting with 'using this': {} = '{}'", name ? name : "?", setting->GetString());
				}
#endif
				// sFailedActivation is the one shown for furniture ("Someone else is using this.").
				if (setting && setting->GetType() == RE::Setting::Type::kString && name &&
					(std::string_view(name).find("InUse") != std::string_view::npos || std::string_view(name) == "sFailedActivation")) {
					g_inUseTexts.emplace_back(setting->GetString());
					logs::info("In-use text {}: '{}'", name, setting->GetString());
				}
			}
		}
		if (g_inUseTexts.empty()) {
			Log::Problem("No 'in use' texts found in the game settings: the message swap will never happen");
		}
	}
}
