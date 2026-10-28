#pragma once

// The hand-off: when the player activates a crafting station an NPC is using,
// vanilla makes the NPC get up; this finishes the job by using the station for
// the player as soon as it's free, and swaps the "in use" message.
namespace Handoff
{
	void Install();         // hooks; call from SKSEPlugin_Load
	void OnDataLoaded();    // reads the game's "in use" texts; call at kDataLoaded

	RE::Actor* FindOccupant(RE::TESObjectREFR* a_furniture);
	bool       IsCraftingStation(RE::TESObjectREFR* a_ref);
	bool       IsEligible(RE::Actor* a_npc);  // false: the mod leaves this NPC alone
	void       LogActor(const char* a_label, RE::Actor* a_actor);
}
