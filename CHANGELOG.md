# Changelog

## 1.0.1

A better log, so a bug report can be worked out from one file. No change to what the mod does.

- The log (`Documents\My Games\Skyrim Special Edition\SKSE\ImUsingThisNow.log`) now always says the mod's version, your game version, your settings, and that it started.
- Every click on a crafting station is logged with what the mod decided and why, including doing nothing: mod switched off, nobody using the station, or the NPC left alone (in combat, in a scene, talking to you, doing quest work, or a follower you set to keep their station).
- It also logs how long the NPC took to get up, why a hand-off stopped, whether your message replaced the game's, and whether the game accepted the final activation.
- It only writes when something happens, never every frame, and stops at a fixed size.

## 1.0.0

First release.

- Click a crafting station an NPC is using: they get up and the crafting menu opens for you on the first click.
- Your line instead of "Someone else is using this.": "I'm using this now. Get lost, <NPC name>." (set in the ini).
- NPCs in scenes, conversations, combat or quest work keep their station.
- Two choices in the installer: an MCM menu (needs SkyUI) or an ini file with no plugin.
- One DLL for SE 1.5.97 and AE 1.6.x and 1.7.x; tested on 1.5.97, 1.6.640, 1.6.1170 and 1.7.104.
