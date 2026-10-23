> **简体中文汉化分支**：本仓库是 [ReSaver++](https://github.com/Tonycubed2/ResaverPP) 的简体中文版本，界面与报告已汉化（分支 `zh-cn`，`main` 与上游保持一致）。中文说明见 [README.zh-CN.md](README.zh-CN.md)。
> This is a Simplified-Chinese localization branch of ReSaver++ — see [README.zh-CN.md](README.zh-CN.md).

# ReSaver++

A C++ port of ReSaver (FallrimTools, by Mark Fairchild) for **Skyrim Special Edition / Anniversary
Edition / VR** saves. Apache License 2.0, see `LICENSE` and `NOTICE.md`.

## What it does

- Reads and writes `.ess` saves (LZ4, zlib or uncompressed) plus the `.skse` co-save.
  An unedited save writes back byte-for-byte identical; edits produce the same bytes as ReSaver.
- Shows everything ReSaver shows: plugins, change forms, the Papyrus string table, scripts,
  script instances (with named variables), references, arrays, active scripts (threads, with
  every stack frame and its variables), function messages, suspended stacks, queued unbinds.
- Cross-links: click `go` on any reference/array value to jump to it; every instance and array
  lists who refers to it.
- Edits: delete any element, terminate threads, delete instances together with the spawned
  objects they sit on, purge a script (threads + suspended stacks + messages + instances).
- Clean menu (same rules as ReSaver): remove unattached instances, remove undefined elements,
  terminate undefined threads, remove instances on missing spawned objects (grouped by the mod
  that provides their script, with its status: not installed, disabled, installed or game link;
  the leftovers of mods that are gone are ticked, and the ones another script still points to
  are kept unless you choose otherwise).
- Enchanted items: the Mods view lists each mod's enchanted and charged items in every inventory
  (enchantment, max charge and charge stored on the item in the save, which xEdit does not show),
  and fixes items whose max charge is 0 so they can be recharged with soul gems.
- Mods view: every mod with how many scripts are on its objects, how many script instances
  and how many threads are running right now. Click a mod to see its scripts; terminate or
  kill one script or all of them. "Find script sources" scans your Mod Organizer mods folder
  (loose .pex and BSA contents, in your MO2 profile's priority order) so scripts on spawned
  objects are credited to the mod that ships them, and adds a "Provides" column.
  Per script: Terminate (threads end when resumed), Stop (delete its threads and queued calls,
  keep the script's data - safe for a mod you keep, e.g. a stuck MCM), Kill (threads + instances).
- Remove mod from save: a safety check first (refuses if another loaded plugin needs it as a
  master; warns about its DLL, running scripts, other mods' links to it, and whether it is still
  enabled in MO2), then removes its scripts, threads, queued calls, arrays only it used and the
  saved state of its forms, and writes a NEW save file next to the old one.
- Edit variable values (int, float, bool, string; object/array links can be cleared to None).
- Plugin names (ReSaver's "Parse ESP/ESMs"): once the mods folder is scanned, every plugin is read in
  the background (MO2 mods, the game's Data folder, localized names from the .STRINGS files in BSAs),
  so forms show as `00013BBF Nazeem 'Nazeem' [NPC_]`. Tools > Find form looks forms up by editor ID,
  name or form ID and shows the base object of placed references.
- Change forms are decoded (references, actors, NPCs, quests, form lists, leveled lists,
  relationships): change flags by name, base object, inventory, extra data, Havok data.
- Clean menu: Fix SkyUI MCM registration jam, Reset Havok, Purify FormLists, Batch Clean (ReSaver's
  list format, with Prefix* wildcards); "Delete all listed" deletes everything the filter shows.
- File > Compare to another save: what changed (threads, instances per script, change forms, plugins).
- File > Watch saves folder: new saves open automatically when the game writes them.
- Report (Mods view or Tools > Report): every mod with its scripts and instances, and the
  anomalies found, as a .txt or a PDF.
- ReSaver's filters (Filters button): scripts with no / missing parent, memberless instances, zeroed
  canaries, forms with scripts, long strings, form lists with null entries, cell(-1) change forms,
  empty REFR, decoding level, change-flag and form-flag masks, and the change form content search;
  "Set filter for this plugin". Sorting: alphabetical, size, mass or save order.
- Plugins: delete a plugin's change forms or script instances, purge one or several plugins,
  export the plugin list.
- Arrays: add, remove and move elements; "Find owner"; Tools > Analyze arrays block.
- Compression when saving: keep, LZ4, zlib or uncompressed (File menu or Options > Settings).
- Scripts: drop a .pex on the window (or File > Open script) to see it decompiled to Papyrus
  source and disassembled. The decompiler was checked against the Creation Kit compiler on 1,496
  vanilla scripts: every decompiled script compiles again, and 1,478 (98.8%) give exactly the same
  code; the other 18 do the same thing with temporaries numbered or cast in a different order.
  When the game's .psc sources are installed (Data/Source/Scripts or Data/Scripts/Source), the
  decompiler uses them for parameter defaults and named arguments. Comments and the original
  layout are not stored in a .pex.
- Script viewer: double-click a script anywhere (Scripts, Script Instances, Active Scripts,
  Function Messages, Suspended Stacks, a mod's script list) or drop a .psc/.pex on the window.
  It opens the script's .psc source from your mods or the game's Data folder; when there is none
  it offers to decompile the .pex (loose or inside a BSA). Double-clicking a thread opens the
  script at the line where the thread is stopped (from the .pex debug info) and marks it.
  - Papyrus syntax colours as in the npp-papyrus Notepad++ plugin (its dark and light themes),
    matching If/EndIf, Function/EndFunction... keywords highlighted, fold markers to collapse
    blocks, an outline of properties/functions/events/states, find (Ctrl+F, F3), go to line
    (Ctrl+G), Ctrl+double-click a script name to open it, Ctrl+mouse wheel to zoom.
  - Editing and saving (a .psc.bak of the original is kept the first time).
  - Compile (F7) with the game's own Papyrus compiler (PapyrusCompiler.exe, installed with the
    Creation Kit in the game folder; ReSaver++ only runs it). Errors are listed and marked on
    their lines; click one to go to it. Compile settings: compiler location, flags file,
    optimize, anonymize, where the .pex goes (the script's mod, MO2 Overwrite, or a folder),
    extra import folders. A .pex.bak of the replaced script is kept once.
  - The editor is ImGuiColorTextEdit by BalazsJako (MIT License), with changes for this.
- Uninstalled mods: when a save opens, ReSaver++ compares the plugins in it with your load order
  (the MO2 profile's or the game's plugins.txt) and lists the ones no longer enabled, telling
  uninstalled mods from disabled ones, plus scripts whose files are installed nowhere. After you
  review and confirm, their leftovers are removed with the "Remove mod from save" safety checks
  and a NEW save is written: no need to load the game and save again first.
- Skyrim VR: a VR save (it lists SkyrimVR.esm) switches to the VR setup by itself: its own mods
  folder, game folder, MO2 instance or Vortex staging folder, load order (Skyrim VR\plugins.txt) and
  saves folder, kept apart from the Special Edition ones (Options > Settings > Game).
- Mod managers: Mod Organizer 2 (mods folder, profile order, Overwrite; also when run from inside MO2)
  and Vortex (its staging folder and vortex.deployment.json: each file is credited to the mod Vortex
  deployed it from). Both are found by themselves on first start; without either, the game's Data
  folder is used.
- Non-English saves: names and text in the game's code page or UTF-8 are shown correctly
  (Options > Settings > Text encoding).
- Themes: Light (default), Grey and Dark (Options > Theme); the script editor follows the theme.
- Help > Show log, Options > Settings, Help > Support (links to ReSaver's author).
- Analysis: plain-language findings (runaway looping scripts such as the Firefly bug, a stuck
  SkyUI MCM registration, undefined scripts, ...) with one-click purge buttons.
- Saving keeps the previous file as `name.ess.bak` and re-reads the written save to verify it.

## Programs

- `ReSaverPP.exe` - the GUI. Open a save with File > Open, by dropping a `.ess` on the window,
  or by passing it on the command line.
- `resaverpp-cli.exe` - command line: `resaverpp-cli save.ess` prints the analysis report;
  `resaverpp-cli save.ess --purge-script Firefly --delete-forms -o cleaned.ess` edits.
  Run it with `--help` for all options.
- `rs_roundtrip.exe` - test: loads saves and checks they write back identical.
- `rs_edit_test.exe` - test: edits variables, saves, reloads and checks the values.

`resaverpp-cli save.ess --mods --sources F:\SkyrimSEMOModfolder` prints the mod table.

`resaverpp-cli save.ess --purge-script QuickLootIEMCM --keep-instances -o fixed.ess` removes a stuck
script's threads and queued calls but keeps its data.

More command line options: `resaverpp-cli script.pex --psc` (decompile), `--fix-mcm`, `--reset-havok`, `--purify-formlists`,
`--batch list.txt`, `--compare other.ess`, `--report report.pdf`, `--find Nazeem --sources <folder>`.

`resaverpp-cli save.ess --remove-mod QuickLootIE.esp --sources F:\SkyrimSEMOModfolder -o new.ess`
removes a mod (add `--dry-run` to only see the safety check).

## Building (Visual Studio 2022)

No vcpkg packages are needed; all dependencies are in `third_party/`.
From the "x64 Native Tools Command Prompt for VS 2022":

```
cd /d C:\dllmaker\ReSaverPP
```

```
cmake --preset release
```

```
cmake --build --preset release
```

The programs are written to `build\release\`.

`prebuilt\` holds MinGW-built copies of both programs that run without building anything.
