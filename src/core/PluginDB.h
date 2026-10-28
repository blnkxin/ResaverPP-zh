// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Reads the plugins (.esm/.esp/.esl) a save uses, so forms can be shown by editor ID and name
// (ReSaver's "Parse ESP/ESMs"). Only what is needed for naming is kept: editor ID, display
// name, record type, and the base object of placed references the save mentions.
#pragma once

#include "Quests.h"
#include "ScriptSources.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ess
{
	class PluginDB
	{
	public:
		struct Input
		{
			std::vector<std::string>          full, lite;  // the save's plugin lists (load order)
			std::unordered_set<std::uint32_t> wantedRefs;  // runtime form IDs of placed references to keep
			ScriptSources                     sources;     // where the plugin and strings files are
			std::string                       language = "english";
		};

		struct Progress
		{
			std::atomic<std::size_t> done{ 0 }, total{ 0 };
			std::atomic<bool>        cancel{ false };
			std::mutex               m;
			std::string              current;  // guarded by m
		};

		struct Form
		{
			std::uint32_t edid = UINT32_MAX;  // offsets into the string pool
			std::uint32_t name = UINT32_MAX;
			std::uint32_t base = 0;           // NAME of a placed reference (runtime form ID)
			std::uint16_t plugin = 0;         // index into plugins(): last plugin that defines it
			char          type[4]{};
			std::uint8_t  spellType = 0xFF;   // SPEL: 0 spell, 1 disease, 2 power, 3 lesser power, 4 ability, 5 poison,
			                                  // 10 addiction, 11 voice... (0xFF: not read)
		};

		// Reads every plugin of the save that can be found. Safe to call on a worker thread.
		void load(const Input& a_in, Progress& a_progress);

		bool                     loaded() const { return !_forms.empty(); }
		std::size_t              size() const { return _forms.size(); }
		const Form*              find(std::uint32_t a_formID) const;
		std::string              edid(const Form& f) const { return str(f.edid); }
		std::string              name(const Form& f) const { return str(f.name); }
		std::string              type(const Form& f) const { return std::string(f.type, 4); }
		const std::string&       pluginName(const Form& f) const { return _plugins[f.plugin]; }
		const std::vector<std::string>& plugins() const { return _plugins; }

		// "EditorID 'Name' [TYPE]" (whatever is known), empty if the form is unknown.
		std::string describe(std::uint32_t a_formID) const;
		// Forms whose editor ID or name contains the text (case-insensitive). Exact hex form IDs
		// match too.
		std::vector<std::uint32_t> search(const std::string& a_text, std::size_t a_max) const;

		// QUST records (the last override of each), with stages, journal entries, objectives and
		// stage scripts: what the Quests view shows
		const quests::QuestDef* quest(std::uint32_t a_formID) const
		{
			auto it = _quests.find(a_formID);
			return it == _quests.end() ? nullptr : &it->second;
		}
		const std::unordered_map<std::uint32_t, quests::QuestDef>& questDefs() const { return _quests; }

		std::vector<std::string> missing;  // plugins of the save whose file was not found
		std::vector<std::string> errors;
		std::size_t              pluginsRead = 0, recordsRead = 0;

	private:
		std::string str(std::uint32_t off) const { return off == UINT32_MAX ? std::string() : std::string(_pool.data() + off); }
		std::uint32_t addString(const std::string& s);

		std::unordered_map<std::uint32_t, Form> _forms;
		std::unordered_map<std::uint32_t, quests::QuestDef> _quests;
		std::vector<char>                       _pool;
		std::vector<std::string>                _plugins;
	};

	class SaveFile;
	// Fills Input::full/lite/wantedRefs from a save (every form the save refers to).
	void PrepareInput(const SaveFile& a_save, PluginDB::Input& a_in);

	// Text from a plugin or strings file: kept if valid UTF-8, otherwise read as Windows-1252.
	std::string GameTextToUtf8(const std::string& a_text);
}
