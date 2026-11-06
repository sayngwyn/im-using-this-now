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
						if (furniture && Handoff::IsCraftingStation(furniture.get()) && Handoff::IsEligible(a_actor)) {
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
					if (Handoff::IsCraftingStation(a_ref) && !Handoff::FindOccupant(a_ref) && a_ref->Is3DLoaded()) {
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

			Scenarios();
			logs::info("Test: done, quitting");
			std::this_thread::sleep_for(1s);
			OnMain([] { RE::Main::GetSingleton()->quitGame = true; });
		}
	}

	void Start() { std::thread(Run).detach(); }
}

#endif
