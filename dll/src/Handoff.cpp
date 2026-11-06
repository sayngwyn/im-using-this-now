#include "pch.h"

#include "Handoff.h"
#include "Log.h"
#include "Settings.h"

namespace Handoff
{
	namespace
	{
		// A hand-off in progress: the player clicked, the NPC is getting up.
		struct Pending
		{
			RE::ObjectRefHandle furniture;
			RE::ActorHandle     npc;
			float               elapsed = 0.0f;
		};

		std::optional<Pending> g_pending;

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
				if (a_target && IsPlayer(a_activator) &&
					a_this->workBenchData.benchType.get() != RE::TESFurniture::WorkBenchData::BenchType::kNone) {
					Decide(a_this, a_target);
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
			static void Decide(RE::TESFurniture* a_this, RE::TESObjectREFR* a_target)
			{
				if (g_ownActivation) {
					return;  // the mod's own hand-off activation; already logged
				}
				const auto id = a_target->GetFormID();
				const auto name = a_this->GetName();
				if (!Settings::Get().enabled) {
					Log::Event("Player clicked {:08X} '{}': mod is switched off in the settings, left alone (vanilla)", id, name);
					return;
				}
				if (g_pending) {
					Log::Event("Player clicked {:08X} '{}' while a hand-off is already running: ignored", id, name);
					return;
				}
				const auto npc = FindOccupant(a_target);
				if (!npc) {
					Log::Event("Player clicked {:08X} '{}': nobody is using it, nothing to do", id, name);
					return;
				}
				const char* why = WhyNot(npc);
				Log::Event("Player clicked {:08X} '{}' used by {:08X} '{}': {}", id, name, npc->GetFormID(),
					npc->GetDisplayFullName(), why ? "left alone (vanilla)" : "taking over");
				LogActor(why ? why : "occupant", npc);
				if (!why) {
					g_pending = Pending{ a_target->GetHandle(), npc->GetHandle() };
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
				const char* stop = nullptr;
				if (!furniture || !npc) {
					stop = "station or NPC gone";
				} else if (p.elapsed > Settings::Get().maxWaitSeconds) {
					stop = "NPC didn't get up in time";
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
					return;  // a menu is open: wait, don't count it as failure
				} else if (const auto occupant = FindOccupant(furniture.get()); occupant && occupant != npc.get()) {
					stop = "someone else took it";
				} else if (!occupant) {
					Log::Event("Station free after {:.2f}s; using it for the player", p.elapsed);
					g_pending.reset();
					g_loggedMenuWait = false;
					g_ownActivation = true;
					const bool ok = furniture->ActivateRef(a_player, 0, nullptr, 1, false);
					g_ownActivation = false;
					Log::Event("Activation for the player {}", ok ? "went through" : "was refused by the game");
					return;
				}
				if (stop) {
					Log::Event("Hand-off stopped after {:.2f}s (wait limit {:g}s): {}", p.elapsed, Settings::Get().maxWaitSeconds, stop);
					if (npc && std::string_view(stop) == "NPC didn't get up in time") {
						LogActor("NPC still on the station", npc.get());
					}
					g_pending.reset();
					g_loggedMenuWait = false;
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
		const auto furniture = base ? base->As<RE::TESFurniture>() : nullptr;
		return furniture && furniture->workBenchData.benchType.get() != RE::TESFurniture::WorkBenchData::BenchType::kNone;
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
