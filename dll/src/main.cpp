// I'm Using This Now: click a crafting station an NPC is using and it's yours.
#include "pch.h"

#include "Handoff.h"
#include "Papyrus.h"
#include "SelfTest.h"
#include "Settings.h"

namespace
{
	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
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

	Settings::Get().Load();
	Handoff::Install();
	Papyrus::Install();
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);

	logs::info("I'm Using This Now loaded, game {}{}", a_skse->RuntimeVersion().string(),
#ifdef IUTN_SELFTEST
		" (SELF-TEST BUILD)"
#else
		""
#endif
	);
	return true;
}
