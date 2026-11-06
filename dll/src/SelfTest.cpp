// Test builds only (build.bat test): from the main menu, go to Riverwood, find an
// NPC at a crafting station, click it once as the player, then log what the mod
// does, close the menu and watch where the NPC goes. Quits the game when done.
#include "pch.h"

#ifdef IUTN_SELFTEST

#	include "Handoff.h"
#	include "SelfTest.h"
#	include "Settings.h"

#	include <functional>

namespace SelfTest
{
	namespace
	{
		using namespace std::chrono_literals;

		void RunConsole(std::string_view a_command)
		{
			const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
			if (const auto script = factory ? factory->Create() : nullptr) {
				script->SetCommand(a_command);
				script->CompileAndRun(nullptr);
				delete script;
				logs::info("Test: console '{}'", a_command);
			}
		}

		template <class F>
		void OnMain(F&& a_fn)
		{
			std::promise<void> done;
			auto future = done.get_future();
			SKSE::GetTaskInterface()->AddTask([&]() { a_fn(); done.set_value(); });
			future.wait();
		}

		// A crafting station with a menu (these scenarios check that the menu opens), not
		// a chopping block or mining spot.
		bool IsBench(RE::TESObjectREFR* a_ref)
		{
			const auto base = a_ref && a_ref->GetBaseObject() ? a_ref->GetBaseObject()->As<RE::TESFurniture>() : nullptr;
			return base && base->workBenchData.benchType.get() != RE::TESFurniture::WorkBenchData::BenchType::kNone;
		}

		bool MenuOpen() { return RE::UI::GetSingleton()->IsMenuOpen(RE::CraftingMenu::MENU_NAME); }

		std::string ReadIniWait()
		{
			std::ifstream in("Data\\SKSE\\Plugins\\ImUsingThisNow.ini");
			std::string   line;
			while (std::getline(in, line)) {
				if (line.starts_with("fMaxWaitSeconds=")) {
					return line;
				}
			}
			return "(not found)";
		}

		// MCM edition only (skipped when ImUsingThisNow.esp isn't loaded): the
		// quest runs, the menu script registered with SkyUI, and a setting
		// changed through the menu's functions applies and lands in the ini.
		void CheckMcm()
		{
			RE::TESQuest* quest = nullptr;
			OnMain([&] { quest = RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESQuest>(0x800, "ImUsingThisNow.esp"); });
			if (!quest) {
				logs::info("Test: MCM plugin not loaded, skipping the MCM check");
				return;
			}
			int configId = -1;
			for (int s = 0; s < 60 && configId < 0; s += 2) {
				std::this_thread::sleep_for(2s);
				OnMain([&] {
					const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
					const auto handle = vm->GetObjectHandlePolicy()->GetHandleForObject(RE::FormType::Quest, quest);
					RE::BSTSmartPointer<RE::BSScript::Object> obj;
					if (!vm->FindBoundObject(handle, "ImUsingThisNow_MCM", obj) || !obj) {
						return;
					}
					const auto id = obj->GetVariable("_configID");
					configId = id ? id->GetSInt() : -1;
					if (configId >= 0) {
						const auto name = obj->GetProperty("ModName");
						logs::info("Test: RESULT MCM quest running={} registered as '{}' (config id {})", quest->IsRunning(),
							name ? name->GetString() : "?", configId);
					}
				});
			}
			if (configId < 0) {
				logs::info("Test: RESULT MCM did NOT register with SkyUI within 60 s (quest running={})", quest->IsRunning());
				return;
			}

			const float before = Settings::Get().maxWaitSeconds;
			auto        call = [](float a_value) {
				OnMain([&] {
					RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
					const auto args = RE::MakeFunctionArguments(static_cast<float>(a_value));
					RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("ImUsingThisNow", "SetMaxWaitSeconds", args, none);
					delete args;
				});
				std::this_thread::sleep_for(2s);
			};
			call(17.0f);
			logs::info("Test: RESULT menu set wait to 17: setting now {}, ini '{}'", Settings::Get().maxWaitSeconds, ReadIniWait());
			call(before);
			logs::info("Test: wait restored: setting {}, ini '{}'", Settings::Get().maxWaitSeconds, ReadIniWait());
		}

		// Finds an NPC the mod would take a station from, stands next to it and
		// makes sure they're still on it at the moment of the click.
		bool FindStation(RE::TESObjectREFR*& a_station, RE::Actor*& a_npc)
		{
			a_station = nullptr;
			a_npc = nullptr;
			for (int attempt = 0; attempt < 45 && !a_station; ++attempt) {
				std::this_thread::sleep_for(2s);
				OnMain([&] {
					RE::ProcessLists::GetSingleton()->ForEachHighActor([&](RE::Actor* a_actor) {
						const auto furniture = a_actor->GetOccupiedFurniture().get();
						if (furniture && IsBench(furniture.get()) && Handoff::IsEligible(a_actor)) {
							a_station = furniture.get();
							a_npc = a_actor;
							return RE::BSContainer::ForEachResult::kStop;
						}
						return RE::BSContainer::ForEachResult::kContinue;
					});
					if (a_station) {
						RE::PlayerCharacter::GetSingleton()->MoveTo(a_station);
					}
				});
				if (a_station) {
					std::this_thread::sleep_for(3s);
					OnMain([&] {
						if (Handoff::FindOccupant(a_station) != a_npc) {
							logs::info("Test: {} left {:08X} before the click; looking again", a_npc->GetDisplayFullName(), a_station->GetFormID());
							a_station = nullptr;
						}
					});
				}
			}
			return a_station != nullptr;
		}

		// Closes the crafting menu and gets the player off the station.
		void LeaveStation()
		{
			std::this_thread::sleep_for(2s);
			OnMain([] {
				if (MenuOpen()) {
					RE::UIMessageQueue::GetSingleton()->AddMessage(RE::CraftingMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
				}
			});
			std::this_thread::sleep_for(2s);
			OnMain([] {
				const auto player = RE::PlayerCharacter::GetSingleton();
				if (player->GetOccupiedFurniture().get()) {
					player->StopInteractingQuick(true);
				}
			});
			std::this_thread::sleep_for(2s);
		}

		// One click on a station an NPC is using, after `before` has set the scene.
		// The scenario markers and RESULT lines are for the test report; the
		// mod's own lines between them are what a player's log would hold.
		void Scenario(const char* a_name, const std::function<void(RE::Actor*)>& a_before, const std::function<void()>& a_after, int a_listenMs = 8000)
		{
			RE::TESObjectREFR* station = nullptr;
			RE::Actor*         npc = nullptr;
			logs::info("Test: === SCENARIO {} ===", a_name);
			if (!FindStation(station, npc)) {
				logs::info("Test: RESULT {}: no NPC at a crafting station found", a_name);
				logs::info("Test: === END {} ===", a_name);
				return;
			}
			OnMain([&] {
				if (a_before) {
					a_before(npc);
				}
				logs::info("Test: player clicks {:08X} once", station->GetFormID());
				station->ActivateRef(RE::PlayerCharacter::GetSingleton(), 0, nullptr, 1, false);
			});
			bool opened = false;
			int  ms = 0;
			for (; ms < a_listenMs && !opened; ms += 100) {
				std::this_thread::sleep_for(100ms);
				OnMain([&] { opened = MenuOpen(); });
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(opened ? 3000 : 1000));
			logs::info("Test: RESULT {}: crafting menu {} ({} ms after the click)", a_name, opened ? "OPENED" : "did NOT open", ms);
			OnMain([&] {
				if (a_after) {
					a_after();
				}
			});
			LeaveStation();
			logs::info("Test: === END {} ===", a_name);
		}

		// A click on a crafting station nobody is using.
		void FreeStationScenario()
		{
			logs::info("Test: === SCENARIO free-station ===");
			RE::TESObjectREFR* station = nullptr;
			OnMain([&] {
				const auto player = RE::PlayerCharacter::GetSingleton();
				RE::TES::GetSingleton()->ForEachReferenceInRange(player, 3000.0f, [&](RE::TESObjectREFR* a_ref) {
					if (IsBench(a_ref) && !Handoff::FindOccupant(a_ref) && a_ref->Is3DLoaded()) {
						station = a_ref;
						return RE::BSContainer::ForEachResult::kStop;
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
				if (station) {
					player->MoveTo(station);
				}
			});
			if (!station) {
				logs::info("Test: RESULT free-station: no free crafting station found");
			} else {
				std::this_thread::sleep_for(2s);
				OnMain([&] {
					logs::info("Test: player clicks {:08X} once", station->GetFormID());
					station->ActivateRef(RE::PlayerCharacter::GetSingleton(), 0, nullptr, 1, false);
				});
				std::this_thread::sleep_for(4s);
				bool opened = false;
				OnMain([&] { opened = MenuOpen(); });
				logs::info("Test: RESULT free-station: crafting menu {}", opened ? "OPENED" : "did NOT open");
				LeaveStation();
			}
			logs::info("Test: === END free-station ===");
		}

		// The cases a player is likely to report. Each runs once in one game session.
		void Scenarios()
		{
			auto& cfg = Settings::Get();
			Scenario("normal", nullptr, nullptr);
			Scenario("mod-off", [&](RE::Actor*) { cfg.enabled = false; }, [&] { cfg.enabled = true; });
			Scenario("short-wait", [&](RE::Actor*) { cfg.maxWaitSeconds = 1.0f; }, [&] { cfg.maxWaitSeconds = 10.0f; }, 12000);
			Scenario("no-message-swap", [&](RE::Actor*) { Handoff::TestClearInUseTexts(); }, nullptr);
			Scenario("npc-in-combat", [&](RE::Actor* a_npc) { a_npc->StartCombat(RE::PlayerCharacter::GetSingleton()); },
				[&] {}, 4000);
			FreeStationScenario();
		}

		// Research mode (a file named IUTN_discover.txt next to the DLL): list the
		// game's furniture and activators that look like veins, quarries, clay
		// pits and chopping blocks, then quit. Never in a shipped build.
		bool NameMatches(const char* a_name)
		{
			if (!a_name || !*a_name) {
				return false;
			}
			std::string n(a_name);
			std::ranges::transform(n, n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			for (const char* w : { "vein", "ore ", " ore", "quarr", "clay", "chop", "geode", "deposit", "log", "wood", "stone", "mine", "visible", "harvest", "sluice", "lumber", "saw" }) {
				if (n.find(w) != std::string::npos) {
					return true;
				}
			}
			return false;
		}

		void DiscoverForms()
		{
			OnMain([] {
				const auto handler = RE::TESDataHandler::GetSingleton();
				int        furn = 0;
				for (const auto f : handler->GetFormArray<RE::TESFurniture>()) {
					++furn;
					if (f && NameMatches(f->GetName())) {
						logs::info("Discover: FURN {:08X} '{}' file={} bench={} flags={:X}", f->GetFormID(), f->GetName(),
							f->GetFile(0) ? f->GetFile(0)->GetFilename() : "?", std::to_underlying(f->workBenchData.benchType.get()),
							f->furnFlags.underlying());
					}
				}
				logs::info("Discover: {} furniture forms in all", furn);
				int acti = 0;
				for (const auto f : handler->GetFormArray<RE::TESObjectACTI>()) {
					++acti;
					if (f && NameMatches(f->GetName())) {
						logs::info("Discover: ACTI {:08X} '{}' file={}", f->GetFormID(), f->GetName(),
							f->GetFile(0) ? f->GetFile(0)->GetFilename() : "?");
					}
				}
				logs::info("Discover: {} activator forms in all", acti);
			});
		}

		bool IsVein(RE::TESObjectREFR* a_ref)
		{
			const auto base = a_ref ? a_ref->GetBaseObject() : nullptr;
			if (!base || !base->Is(RE::FormType::Activator)) {
				return false;
			}
			const std::string_view n = base->GetName();
			return n.contains("Vein") || n.contains("Deposit") || n.contains("Quarry");
		}

		std::string Markers(RE::TESObjectREFR* a_ref)
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

		// The game counts this actor as on one of the furniture's spots (not just booked).
		bool OnMarker(RE::TESObjectREFR* a_ref, RE::Actor* a_actor)
		{
			const auto used = a_ref ? a_ref->extraList.GetByType<RE::ExtraUsedMarkers>() : nullptr;
			if (!used) {
				return false;
			}
			const auto handle = a_actor->GetHandle();
			return std::ranges::any_of(used->usedMarkers, [&](const RE::MarkerUsedData& a_m) { return a_m.actorinMarker == handle; });
		}

		// Reads (never sets) whether the spot's vanilla script listens to the player's
		// animation events, which is what pays out ore or firewood.
		std::string ScriptRegistered(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref) {
				return "no ref";
			}
			const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			const auto handle = vm->GetObjectHandlePolicy()->GetHandleForObject(RE::FormType::Reference, a_ref);
			for (const char* script : { "MineOreFurnitureScript", "WoodChoppingScript" }) {
				RE::BSTSmartPointer<RE::BSScript::Object> obj;
				if (vm->FindBoundObject(handle, script, obj) && obj) {
					auto value = obj->GetProperty("isRegisteredForEvents");
					if (!value) {
						value = obj->GetVariable("isRegisteredForEvents");
					}
					return std::format("{} registered={}", script, value ? (value->GetBool() ? "yes" : "no") : "?");
				}
			}
			return "no known script";
		}

		// Research mode, one place: who is working at a vein and what clicking it does.
		void DiscoverCell(const std::string& a_spec)
		{
			const auto        bar = a_spec.find('|');
			const std::string mode = bar == std::string::npos ? std::string() : a_spec.substr(bar + 1);
			const std::string a_cell = a_spec.substr(0, bar);
			const bool        chop = mode == "chop";
			const bool        saveTest = mode == "save";
			const bool        menuTest = mode == "menu";
			const bool        walkTest = mode == "walk";  // a miner who has booked a vein but is still walking to it
			RE::FormID        watchNpc = 0;
			OnMain([&] { RunConsole("coc " + a_cell); });
			for (int i = 0; i < 15; ++i) {
				std::this_thread::sleep_for(2s);
			}
			OnMain([] {
				RunConsole("set gamehour to 10");
				RunConsole("player.additem 000E3C16 1");  // pickaxe
				RunConsole("player.additem 0002F2F4 1");  // woodcutter's axe
			});
			RE::TESObjectREFR* veinClicked = nullptr;
			bool               clicked = false;
			int                round = 0;
			for (int attempt = 0; attempt < 6 && !clicked; ++attempt) {
			veinClicked = nullptr;
			for (; round < (chop ? 280 : 100) && !veinClicked; ++round) {
				std::this_thread::sleep_for(chop ? 1s : 3s);
				OnMain([&] {
					const auto                       player = RE::PlayerCharacter::GetSingleton();
					std::vector<RE::TESObjectREFR*> veins;
					RE::TES::GetSingleton()->ForEachReferenceInRange(player, 4000.0f, [&](RE::TESObjectREFR* a_ref) {
						if (IsVein(a_ref) && a_ref->Is3DLoaded()) {
							veins.push_back(a_ref);
						}
						return RE::BSContainer::ForEachResult::kContinue;
					});
					logs::info("Discover: round {}: {} veins/deposits near the player", round, veins.size());
					RE::ProcessLists::GetSingleton()->ForEachHighActor([&](RE::Actor* a_actor) {
						if (a_actor->IsPlayerRef() || a_actor->IsDead()) {
							return RE::BSContainer::ForEachResult::kContinue;
						}
						RE::TESObjectREFR* nearest = nullptr;
						float              best = 1e9f;
						for (const auto v : veins) {
							const float d = a_actor->GetPosition().GetDistance(v->GetPosition());
							if (d < best) {
								best = d;
								nearest = v;
							}
						}
						const auto pkg = a_actor->GetCurrentPackage();
						const auto state = a_actor->AsActorState();
						const auto furn = a_actor->GetOccupiedFurniture().get();
						if (best < 400.0f || furn) {
							logs::info("Discover:   {:08X} '{}' pkg={:08X} type={} sit={} furniture={:08X} '{}' nearest vein {:08X} '{}' {:.0f} away",
								a_actor->GetFormID(), a_actor->GetDisplayFullName(), pkg ? pkg->GetFormID() : 0,
								pkg ? std::to_underlying(pkg->packData.packType.get()) : -1,
								state ? std::to_underlying(state->GetSitSleepState()) : -1, furn ? furn->GetFormID() : 0,
								furn && furn->GetBaseObject() ? furn->GetBaseObject()->GetName() : "",
								nearest ? nearest->GetFormID() : 0, nearest ? nearest->GetBaseObject()->GetName() : "-", best);
						}
						if (a_spec.ends_with("|free") && !veinClicked) {
							for (const auto v : veins) {
								const auto l = v->GetLinkedRef(nullptr);
								if (l && !Handoff::FindOccupant(l)) {
									veinClicked = v;
									break;
								}
							}
						}
						if (chop && !veinClicked && furn && OnMarker(furn.get(), a_actor) &&  // really on the block, not just walking to it
							std::string_view(furn->GetBaseObject()->GetName()).contains("Chopping Block")) {
							veinClicked = furn.get();
						}
						if (!chop && !veinClicked && furn && state &&
							(walkTest ? state->GetSitSleepState() == RE::SIT_SLEEP_STATE::kNormal && !OnMarker(furn.get(), a_actor) &&
											a_actor->GetPosition().GetDistance(furn->GetPosition()) > 250.0f :
										state->GetSitSleepState() == RE::SIT_SLEEP_STATE::kIsSitting)) {
							for (const auto v : veins) {
								if (v->GetLinkedRef(nullptr) == furn.get()) {
									veinClicked = v;
									break;
								}
							}
						}
						return RE::BSContainer::ForEachResult::kContinue;
					});
				});
			}
			if (!veinClicked) {
				break;
			}
				OnMain([&] {
					const auto player = RE::PlayerCharacter::GetSingleton();
					if (chop) {
						// Stand beside the block, not on it: a player standing on the spot makes
						// the woodcutter abandon it.
						auto pos = veinClicked->GetPosition();
						if (const auto user = Handoff::FindOccupant(veinClicked)) {
							auto away = pos - user->GetPosition();
							away.z = 0.0f;
							const float len = away.Length();
							pos += len > 1.0f ? away * (160.0f / len) : RE::NiPoint3(160.0f, 0.0f, 0.0f);
						} else {
							pos += RE::NiPoint3(160.0f, 0.0f, 0.0f);
						}
						player->SetPosition(pos, true);  // the block is loaded, so it's in the player's loaded area
					} else {
						player->MoveTo(veinClicked);
					}
					logs::info("Discover: player moved to vein {:08X}", veinClicked->GetFormID());
				});
				std::this_thread::sleep_for(chop ? 300ms : 1000ms);
				OnMain([&] {
					const auto spotNow = chop ? veinClicked : veinClicked->GetLinkedRef(nullptr);
					if (!a_spec.ends_with("|free") && spotNow && !Handoff::FindOccupant(spotNow)) {
						logs::info("Discover: the worker left {:08X} before the click; looking again", spotNow->GetFormID());
						return;
					}
					clicked = true;
					RE::Actor* occupant = nullptr;
					RE::ProcessLists::GetSingleton()->ForEachHighActor([&](RE::Actor* a) {
						const auto f = a->GetOccupiedFurniture().get();
						if (f && f->GetPosition().GetDistance(veinClicked->GetPosition()) < 400.0f) {
							logs::info("Discover: {:08X} '{}' uses furniture ref {:08X} base {:08X} '{}' type={} distance to vein {:.0f}", a->GetFormID(),
								a->GetDisplayFullName(), f->GetFormID(), f->GetBaseObject()->GetFormID(), f->GetBaseObject()->GetName(),
								std::to_underlying(f->GetBaseObject()->GetFormType()), f->GetPosition().GetDistance(veinClicked->GetPosition()));
							occupant = a;
						}
						return RE::BSContainer::ForEachResult::kContinue;
					});
					const auto linked = veinClicked->GetLinkedRef(nullptr);
					logs::info("Discover: vein {:08X} base {:08X} linked ref {:08X}; occupant found: {}", veinClicked->GetFormID(),
						veinClicked->GetBaseObject()->GetFormID(), linked ? linked->GetFormID() : 0, occupant != nullptr);
					const auto spot = chop ? veinClicked : linked;
					logs::info("Discover: before the click: {};{}", ScriptRegistered(spot), Markers(spot));
					if (saveTest || menuTest) {
						Handoff::TestSetHoldDelay(4.0f);
					}
					logs::info("Discover: player clicks vein {:08X} once", veinClicked->GetFormID());
					veinClicked->ActivateRef(RE::PlayerCharacter::GetSingleton(), 0, nullptr, 1, false);
				});
			}
			if (clicked && (saveTest || menuTest)) {
				for (int ms = 0; ms < 12000 && !watchNpc; ms += 50) {
					std::this_thread::sleep_for(50ms);
					OnMain([&] {
						if (const auto npc = Handoff::TestHeldNpc()) {
							watchNpc = npc->GetFormID();
						}
					});
				}
				logs::info("Test: held NPC seen: {:08X}", watchNpc);
				auto report = [&](const char* a_when) {
					OnMain([&] {
						const auto npc = watchNpc ? RE::TESForm::LookupByID<RE::Actor>(watchNpc) : nullptr;
						const auto pkg = npc ? npc->GetCurrentPackage() : nullptr;
						const auto pos = npc ? npc->GetPosition() : RE::NiPoint3();
						logs::info("Test: {}: hand-off running={} NPC AI enabled={} pkg={:08X} pos=({:.0f},{:.0f},{:.0f}) furniture={:08X} menu paused={}", a_when,
							Handoff::TestPending(), npc ? npc->IsAIEnabled() : false, pkg ? pkg->GetFormID() : 0, pos.x, pos.y, pos.z,
							npc && npc->GetOccupiedFurniture().get() ? npc->GetOccupiedFurniture().get()->GetFormID() : 0,
							RE::UI::GetSingleton()->GameIsPaused());
					});
				};
				if (watchNpc && saveTest) {
					report("held, before the save");
					OnMain([] { RunConsole("save iutntest"); });
					std::this_thread::sleep_for(4s);
					report("after the save");
					OnMain([] { RunConsole("load iutntest"); });
					std::this_thread::sleep_for(5s);
					bool inGame = false;
					for (int i = 0; i < 40 && !inGame; ++i) {
						std::this_thread::sleep_for(1s);
						OnMain([&] {
							const auto player = RE::PlayerCharacter::GetSingleton();
							inGame = player && player->Is3DLoaded() && player->GetParentCell() && !RE::UI::GetSingleton()->GameIsPaused();
						});
					}
					std::this_thread::sleep_for(3s);
					report("after loading the save");
					std::this_thread::sleep_for(8s);
					report("8 s later");
					std::this_thread::sleep_for(8s);
					report("16 s later");
					Handoff::TestSetHoldDelay(0.0f);
					return;
				}
				if (watchNpc && menuTest) {
					report("held, before the menu");
					OnMain([] { RE::UIMessageQueue::GetSingleton()->AddMessage(RE::JournalMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr); });
					std::this_thread::sleep_for(2s);
					report("menu open");
					OnMain([] {
						logs::info("Test: journal open={}", RE::UI::GetSingleton()->IsMenuOpen(RE::JournalMenu::MENU_NAME));
						RE::UIMessageQueue::GetSingleton()->AddMessage(RE::JournalMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
					});
					std::this_thread::sleep_for(1s);
					report("menu closed");
				}
				Handoff::TestSetHoldDelay(0.0f);
			}
			if (clicked) {
				for (int t = 2; t <= 40; t += 2) {
					std::this_thread::sleep_for(2s);
					OnMain([&] {
						const auto player = RE::PlayerCharacter::GetSingleton();
						const auto seat = player->GetOccupiedFurniture().get();
						int ore = 0;
						int wood = 0;
						for (const auto& [item, count] : player->GetInventoryCounts()) {
							if (item && std::string_view(item->GetName()).contains("Ore")) {
								ore += count;
							}
							if (item && std::string_view(item->GetName()).contains("Firewood")) {
								wood += count;
							}
						}
						const auto spot = chop ? veinClicked : veinClicked->GetLinkedRef(nullptr);
						const auto watched = watchNpc ? RE::TESForm::LookupByID<RE::Actor>(watchNpc) : nullptr;
						logs::info("Discover: +{}s player at furniture {:08X}, ore of any kind in bag {}, firewood {}, vein disabled={}, {};{}{}", t,
							seat ? seat->GetFormID() : 0, ore, wood, veinClicked->IsDisabled(), ScriptRegistered(spot), Markers(spot),
							watched ? std::format(" held-NPC AI enabled={} hand-off running={}", watched->IsAIEnabled(), Handoff::TestPending()) : std::string());
					});
				}
			} else {
				logs::info("Discover: nobody found working at a vein");
			}
		}

		void Run()
		{
			std::this_thread::sleep_for(5s);
			if (std::filesystem::exists("Data\\SKSE\\Plugins\\IUTN_discover.txt")) {
				std::string cell;
				{
					std::ifstream in("Data\\SKSE\\Plugins\\IUTN_discover.txt");
					std::getline(in, cell);
				}
				if (cell.starts_with("cell:")) {
					while (!cell.empty() && (cell.back() == '\r' || cell.back() == '\n')) {
						cell.pop_back();
					}
					DiscoverCell(cell.substr(5));
				} else {
					DiscoverForms();
				}
				logs::info("Test: done, quitting");
				std::this_thread::sleep_for(1s);
				OnMain([] { RE::Main::GetSingleton()->quitGame = true; });
				return;
			}
			OnMain([] { RunConsole("coc Riverwood"); });
			bool inGame = false;
			for (int i = 0; i < 60 && !inGame; ++i) {
				std::this_thread::sleep_for(2s);
				OnMain([&] {
					const auto player = RE::PlayerCharacter::GetSingleton();
					inGame = player && player->Is3DLoaded() && player->GetParentCell();
				});
			}
			OnMain([] { RunConsole("set gamehour to 10"); });
			std::this_thread::sleep_for(15s);
			CheckMcm();

			Scenarios();
			logs::info("Test: done, quitting");
			std::this_thread::sleep_for(1s);
			OnMain([] { RE::Main::GetSingleton()->quitGame = true; });
		}
	}

	void Start() { std::thread(Run).detach(); }
}

#endif
