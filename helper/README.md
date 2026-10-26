# ReSaverHelper

SKSE plugin that lets ReSaver++ (ReSaver Plus) act in the running game: complete a quest or set a
quest stage (the game's own stage scripts run, so rewards and follow-up quests happen normally), and
refresh the quest journal (brings back quests and objectives the journal stopped showing).

- Talks to ReSaver++ over a local named pipe (`\\.\pipe\ReSaverHelper`). Only the Windows user running
  the game can open it; remote connections are refused. It understands a short fixed list of commands
  (see `src/Protocol.h`).
- Mark a quest completed without its stage script, when the game refuses the completion stage (version 3).
- Transplant (version 9): takes a snapshot of the loaded character (level, skills, perks, spells, shouts,
  items, look via RaceMenu, worn gear, map locations, factions, quests and the journal itself) and puts what
  ReSaver++ sends into another save: quests are started and set to their stage, their journal entries are
  written as the other save had them, and quests the game will not start are left alone. Every step is logged.
- Followers (version 10): the snapshot lists the characters following you, the quest aliases that hold them
  (their follower mod's own system) and what they carry; Transplant brings them to you, back in those aliases,
  as your teammate, with their items (revived and enabled if needed).
- Confirms every action in game: top-left notification or a message box (`sNotify` in ReSaverHelper.ini).
- Optional: refresh the journal automatically after every save load (`bRefreshJournalOnLoad=1`).
- SE 1.5.97, AE 1.6.x / 1.7.x and VR 1.4.15 from one DLL (CommonLibSSE-NG).
- Log: Documents\My Games\Skyrim Special Edition\SKSE\ReSaverHelper.log

## Install

Copy `ReSaverHelper.dll` and `ReSaverHelper.ini` to `Data\SKSE\Plugins` (or a mod folder in MO2 / Vortex).

## Build

From the Developer Command Prompt for VS 2022, in this folder:

    cmake --preset build-release-msvc
    cmake --build build/release-msvc

The DLL is `build\release-msvc\ReSaverHelper.dll`.

Apache License 2.0, like ReSaver++.
