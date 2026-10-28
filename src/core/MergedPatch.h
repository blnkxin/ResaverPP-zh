// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Merged patch: reads every plugin of the load order, finds records that two or more plugins
// override, and writes one plugin that combines their changes, the way Mator Smash and a manual
// xEdit patch do:
//  - the winning override is copied into the patch;
//  - every other override is compared with the version its own plugin was built on (the last
//    version among that plugin's masters), and what it added to or removed from a list is applied;
//  - a mod becomes a master of the patch only when a patched record comes from it, or a value in a
//    patched record points at one of its forms (like xEdit's "copy as override").
// Stage 1 merges lists: leveled items/NPCs/spells, container and NPC inventories, NPC factions,
// spells and perks, keywords (NPCs, armour, weapons, ammo), form lists and
// outfits. Every other value keeps the winning override, as the game does without a patch.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ess
{
	class ScriptSources;
}

namespace ess::merge
{
	struct Progress
	{
		std::atomic<std::size_t> done{ 0 }, total{ 0 };
		std::atomic<bool>        cancel{ false };
		void set(const std::string& a)
		{
			std::lock_guard l(_m);
			_cur = a;
		}
		std::string current() const
		{
			std::lock_guard l(_m);
			return _cur;
		}

	private:
		mutable std::mutex _m;
		std::string        _cur;
	};

	struct Options
	{
		std::vector<std::string> loadOrder;  // enabled plugins, in load order
		// where a plugin file is (empty: not found, the plugin is left out and reported)
		std::function<std::optional<std::filesystem::path>(const std::string&)> findPlugin;
		// the bytes of a strings file by lowercase name ("skyrim_english.strings"); empty if none
		std::function<std::vector<std::uint8_t>(const std::string&)> readStrings;
		std::string language = "english";
		std::string patchName = "ReSaverPP Merged Patch.esp";
	};

	struct MergedRecord
	{
		std::string              type, edid;
		std::string              id;       // "Skyrim.esm:0x01A2B3"
		std::string              winner;   // the plugin whose version the patch starts from
		std::vector<std::string> changes;  // "ModA.esp: +2 entries", ...
	};

	struct Result
	{
		bool                      ok = false;
		std::string               error;
		std::vector<std::uint8_t> file;  // the patch plugin
		std::vector<std::string>  masters;
		std::map<std::string, std::string> masterWhy;  // master -> a record that needs it
		std::vector<MergedRecord> merged;
		std::vector<std::string>  skipped;  // records with conflicts that could not be merged, and why
		std::vector<std::string>  missing;  // plugins of the load order whose file was not found
		std::size_t               pluginsRead = 0, recordsRead = 0, conflicts = 0;

		std::string report() const;
	};

	Result Build(const Options& a_opt, Progress* a_progress = nullptr);

	// Finds plugins and strings files (loose or in BSAs) through a scan of the mods (MO2 or Vortex) or
	// the game's Data folder. a_sources must outlive the build.
	void UseSources(Options& a_opt, const ScriptSources& a_sources);

	// The load order from a plugins.txt (Mod Organizer profile or the game's own): the plugins the
	// game always loads first (base game masters, then the Creation Club files in Skyrim.ccc), then
	// the enabled lines in file order.
	std::vector<std::string> ReadLoadOrder(const std::filesystem::path& a_pluginsTxt, const std::filesystem::path& a_gameFolder);

	// Low level, for tests: one record of a plugin and a minimal plugin writer.
	struct Sub
	{
		char                      sig[4];
		std::vector<std::uint8_t> data;
	};
	struct Record
	{
		char             sig[4];
		std::uint32_t    flags = 0, formID = 0;
		std::uint16_t    version = 44;
		std::vector<Sub> subs;
	};
	std::vector<std::uint8_t> WritePlugin(const std::vector<std::string>& a_masters, std::uint32_t a_headerFlags,
		const std::vector<Record>& a_records, const std::string& a_author = "ReSaver Plus", const std::string& a_desc = {});
	struct PluginFile
	{
		std::vector<std::string> masters;
		std::uint32_t            flags = 0;
		std::vector<Record>      records;  // top-level records of the wanted types (all types if a_types is empty)
	};
	PluginFile ReadPlugin(const std::filesystem::path& a_file, const std::vector<std::string>& a_types = {});
}
