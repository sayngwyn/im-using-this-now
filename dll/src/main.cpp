// I'm Using This Now: click a crafting station an NPC is using and it's yours.
#include "pch.h"

#include "Handoff.h"
#include "Log.h"
#include "Papyrus.h"
#include "SelfTest.h"
#include "Settings.h"

namespace
{
	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kSaveGame:  // sent before the save is written
		case SKSE::MessagingInterface::kPreLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			Handoff::OnGameEvent(a_msg->type);
			break;
		default:
			break;
		}
		if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			Handoff::OnDataLoaded();
#ifdef IUTN_SELFTEST
			SelfTest::Start();
#endif
		}
	}
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);

	logs::info("I'm Using This Now {} loaded, game {}{}",
		SKSE::PluginDeclaration::GetSingleton()->GetVersion().string("."), a_skse->RuntimeVersion().string(),
#ifdef IUTN_SELFTEST
		" (SELF-TEST BUILD)"
#else
		""
#endif
	);
	Settings::Get().Load();
	try {
		Handoff::Install();
	} catch (const std::exception& e) {
		Log::Problem("Hook install threw: {}", e.what());
		return false;
	}
	Papyrus::Install();
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
