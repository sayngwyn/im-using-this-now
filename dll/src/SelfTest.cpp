// Test builds only (build.bat test): from the main menu, go to Riverwood, find an
// NPC at a crafting station, click it once as the player, then log what the mod
// does, close the menu and watch where the NPC goes. Quits the game when done.
#include "pch.h"

#ifdef IUTN_SELFTEST

#	include "Handoff.h"
#	include "SelfTest.h"
#	include "Settings.h"

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

		void Run()
		{
			std::this_thread::sleep_for(5s);
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

			RE::TESObjectREFR* station = nullptr;
			RE::Actor*         npc = nullptr;
			// Find an NPC the mod would take a station from, stand next to it, and
			// make sure they're still on it at the moment of the click.
			for (int attempt = 0; attempt < 45 && !station; ++attempt) {
				std::this_thread::sleep_for(2s);
				OnMain([&] {
					RE::ProcessLists::GetSingleton()->ForEachHighActor([&](RE::Actor* a_actor) {
						const auto furniture = a_actor->GetOccupiedFurniture().get();
						if (furniture && Handoff::IsCraftingStation(furniture.get()) && Handoff::IsEligible(a_actor)) {
							station = furniture.get();
							npc = a_actor;
							return RE::BSContainer::ForEachResult::kStop;
						}
						return RE::BSContainer::ForEachResult::kContinue;
					});
					if (station) {
						RE::PlayerCharacter::GetSingleton()->MoveTo(station);
					}
				});
				if (station) {
					std::this_thread::sleep_for(3s);
					OnMain([&] {
						if (Handoff::FindOccupant(station) != npc) {
							logs::info("Test: {} left {:08X} before the click; looking again", npc->GetDisplayFullName(), station->GetFormID());
							station = nullptr;
						}
					});
				}
			}
			if (!station) {
				logs::info("Test: RESULT no NPC at a crafting station found");
			} else {
				OnMain([&] {
					logs::info("Test: player clicks {:08X} once", station->GetFormID());
					station->ActivateRef(RE::PlayerCharacter::GetSingleton(), 0, nullptr, 1, false);
				});
				bool opened = false;
				int  ms = 0;
				for (; ms < 15000 && !opened; ms += 100) {
					std::this_thread::sleep_for(100ms);
					OnMain([&] { opened = MenuOpen(); });
				}
				logs::info("Test: RESULT crafting menu {} ({} ms after the one click)", opened ? "OPENED" : "did NOT open", ms);
				OnMain([&] { logs::info("Test: {} is {:.0f} units from the station", npc->GetDisplayFullName(), npc->GetPosition().GetDistance(station->GetPosition())); });

				if (opened) {
					std::this_thread::sleep_for(2s);
					OnMain([] {
						RE::UIMessageQueue::GetSingleton()->AddMessage(RE::CraftingMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
					});
					std::this_thread::sleep_for(2s);
					OnMain([] {
						const auto player = RE::PlayerCharacter::GetSingleton();
						const auto seat = player->GetOccupiedFurniture().get();
						logs::info("Test: menu closed; player still at {:08X}", seat ? seat->GetFormID() : 0);
						if (seat) {
							player->StopInteractingQuick(true);  // what a real Escape would end in
						}
					});
					// Watch whether the NPC walks straight back once the player leaves.
					for (int s = 1; s <= 45; ++s) {
						std::this_thread::sleep_for(1s);
						OnMain([&] {
							const auto occupant = Handoff::FindOccupant(station);
							if (s % 5 == 0 || occupant) {
								logs::info("Test: +{}s after leaving, station used by {}; {} is {:.0f} units away", s,
									occupant ? (occupant->IsPlayerRef() ? "player" : occupant->GetDisplayFullName()) : "nobody",
									npc->GetDisplayFullName(), npc->GetPosition().GetDistance(station->GetPosition()));
							}
						});
					}
					OnMain([&] { Handoff::LogActor("npc at end", npc); });
				}
			}
			logs::info("Test: done, quitting");
			std::this_thread::sleep_for(1s);
			OnMain([] { RE::Main::GetSingleton()->quitGame = true; });
		}
	}

	void Start() { std::thread(Run).detach(); }
}

#endif
