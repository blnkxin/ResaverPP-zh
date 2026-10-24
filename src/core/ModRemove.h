// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// "Remove mod from save": every Papyrus trace of one mod plus the saved state of its forms,
// with a safety check first. The plan is computed without touching the save; Apply() performs it.
#pragma once

#include "Save.h"
#include "ScriptSources.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ess::modremove
{
	struct Options
	{
		bool otherObjects = true;    // also remove the mod's own scripts from objects of other mods / vanilla
		bool spawnedObjects = true;  // delete spawned (FF) objects whose only scripts are being removed
	};

	struct Plan
	{
		std::string name;          // as given (plugin file name or "[mod folder]")
		bool        isPlugin = false;
		std::string folder;        // MO2 mod folder, when known

		// what Apply() deletes
		std::set<ThreadID>      threads;       // active scripts
		std::set<ThreadID>      stacks;        // suspended stacks (by thread id)
		std::set<std::size_t>   messages;      // function message indices
		std::set<EID>           instances;
		std::set<EID>           arrays;        // arrays only the removed elements used
		std::set<EID>           unbinds;
		std::set<std::uint32_t> changeForms;   // the plugin's own forms (refID.data)
		std::set<std::uint32_t> spawnedForms;  // created objects that only carried its scripts

		// facts for the report
		std::size_t                        instancesOnOwnForms = 0, instancesElsewhere = 0;
		std::set<std::string>              ownScripts;          // scripts only this mod provides
		std::map<std::string, std::size_t> changeFormsByType;
		std::size_t                        threadsOfOtherMods = 0;  // threads started by other scripts that are inside this mod right now
		std::map<std::string, std::size_t> linksFromOthers;         // owner script -> variables that will read None
		std::vector<std::string>           dependents;             // loaded plugins that list this one as a master
		std::size_t                        uncheckedPlugins = 0;   // loaded plugins whose file could not be read
		std::vector<std::string>           dlls;                   // SKSE plugins in the mod folder
		int                                enabledInProfile = -1;  // 1 yes, 0 no, -1 unknown

		std::vector<std::string> blockers;  // removal refused
		std::vector<std::string> warnings;  // removal allowed, but read these
		std::vector<std::string> notes;     // informational

		bool        canApply() const { return blockers.empty(); }
		std::string summary() const;  // multi-line, plain language
	};

	// Official Creation Club plugin names ("cc" + creator code + "sse" + 3 digits); lowercase input.
	bool IsCreationClubPlugin(const std::string& a_lower);

	Plan MakePlan(SaveFile& a_save, const std::string& a_mod, const ScriptSources* a_sources, const Options& a_opt);

	struct Result
	{
		std::size_t threads = 0, stacks = 0, messages = 0, instances = 0, arrays = 0, unbinds = 0, changeForms = 0;
		std::string text() const;
	};
	// The save must be unchanged since MakePlan.
	Result Apply(SaveFile& a_save, const Plan& a_plan);

	// Master files listed in a plugin's TES4 header (lowercase). Throws on unreadable files.
	std::vector<std::string> ReadMasters(const std::filesystem::path& a_plugin);
}
