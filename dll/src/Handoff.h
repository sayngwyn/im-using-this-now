#pragma once

// The hand-off: when the player activates a crafting station an NPC is using,
// vanilla makes the NPC get up; this finishes the job by using the station for
// the player as soon as it's free, and swaps the "in use" message.
namespace Handoff
{
	void Install();         // hooks; call from SKSEPlugin_Load
	void OnDataLoaded();    // reads the game's "in use" texts; call at kDataLoaded
	void OnGameEvent(std::uint32_t a_type);  // kSaveGame / kPreLoadGame / kNewGame: lets a held NPC go, ends any hand-off

	RE::Actor* FindOccupant(RE::TESObjectREFR* a_furniture);
	bool       IsCraftingStation(RE::TESObjectREFR* a_ref);
	const char* WhyNot(RE::Actor* a_npc);     // why the mod leaves this NPC alone; nullptr if it takes over
	bool       IsEligible(RE::Actor* a_npc);  // false: the mod leaves this NPC alone
	void       LogActor(const char* a_label, RE::Actor* a_actor);
#ifdef IUTN_SELFTEST
	void       TestClearInUseTexts();  // test build only: pretend the game's texts weren't found
	RE::Actor* TestHeldNpc();                 // the NPC held still right now, if any
	bool       TestPending();                 // a hand-off is running
	void       TestSetHoldDelay(float a_seconds);  // stay held this long before the player's activation
#endif
}
