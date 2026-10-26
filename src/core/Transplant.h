// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Transplant: bring a character's progress from a bad save into a good one. Both are described by a
// snapshot: what the player has (level, skills, perks, spells, shouts, words, factions, map markers,
// quests, items, potions made). A snapshot comes from the running game (ReSaverHelper's SNAPSHOT,
// complete) or from a save file (partial: what the save file shows without the game). Diff() lists
// what the bad save has and the good one does not; each change carries the record ReSaverHelper's
// APPLY understands, so the game itself puts it into the good save.
#pragma once

#include "Save.h"

#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ess::transplant
{
	using Record = std::vector<std::string>;

	struct FormKey
	{
		std::string   plugin;  // lower case for comparing
		std::uint32_t local = 0;
		bool          operator<(const FormKey& o) const { return plugin != o.plugin ? plugin < o.plugin : local < o.local; }
		bool          operator==(const FormKey& o) const { return plugin == o.plugin && local == o.local; }
	};

	struct Entry  // perk, spell, shout, word, faction, marker
	{
		Record      record;
		std::string name;
		int         a = 0, b = 0;  // word: known, unlocked; faction: rank; marker: flags
	};

	struct Quest
	{
		Record           record;
		std::string      plugin, edid, name;
		std::uint32_t    local = 0, runtime = 0;
		bool             running = false, completed = false;
		int              stage = 0;
		std::vector<int> done;
		std::string      objectives;   // "index:state,..." (helper 8+; empty when unknown)
		int              type = -1;    // quest type (0 none, 1 main, 2 Mages Guild ... 8 side); -1 unknown
		bool             known = false;  // objectives and type were in the snapshot
		bool             inJournal = false;  // the player's journal lists it (jobj records, helper 9+)
		bool             shown() const;  // an objective was shown in the journal
	};

	struct Item
	{
		Record      record;
		FormKey     key;
		std::string name, signature;  // signature: the item's extra data (custom name, temper, charge, enchantment, poison)
		int         count = 0;
		bool        special = false, questItem = false, madeEnchantment = false;
	};

	struct Potion
	{
		Record      record;
		std::string name, effects;
		int         count = 0;
		bool        poison = false;
	};

	struct Snapshot
	{
		bool        valid = false, fromFile = false;  // fromFile: read from a save, not from the game (partial)
		std::string source, game, player, error;
		int         level = -1, perkPoints = -1;
		float       xp = 0, threshold = 0;
		Record      playerRecord;
		struct Skill
		{
			float    level = -1;
			Record   record;
		};
		std::array<Skill, 18>            skills;
		std::map<std::string, std::pair<float, Record>> avs;
		std::map<FormKey, Entry>         perks, spells, shouts, words, factions, markers;
		std::map<FormKey, Quest>         quests;
		std::vector<Item>                items;
		std::vector<Potion>              potions;
		std::vector<Entry>               equips;   // equip / equipspell / equipshout records
		std::vector<Entry>               followers;  // follower records (helper 10+)
		std::string                      look;     // RaceMenu preset saved with the snapshot (empty: none)
		std::vector<std::string>         plugins;  // load order (from the game; empty when unknown)
		bool                             journalKnown = false;  // the snapshot lists the journal itself (jobj records)
		std::vector<std::string>         missingParts;  // what a partial snapshot could not read
	};

	Snapshot ReadSnapshot(const std::filesystem::path& a_file);
	Snapshot FromSave(const SaveFile& a_save, const std::string& a_name);  // partial
	const char* SkillName(int a_index);

	enum class Category
	{
		kAppearance,  // the character's look (RaceMenu preset): listed first, highlighted
		kCharacter,
		kPerks,
		kSpells,
		kShouts,
		kMarkers,
		kItems,
		kMade,
		kLook,
		kFactions,
		kQuests,
		kFollowers,  // last: after the quests (a quest stage can move a follower or clear an alias)
		kCount
	};
	const char* CategoryName(Category a_cat);

	struct Change
	{
		Category    cat = Category::kCharacter;
		std::string label, detail;
		int         risk = 0;  // 0 safe, 1 caution, 2 risky
		std::string why;       // why that risk
		bool        available = true;  // false: its mod is not in the good game
		bool        tick = true;
		Record      record;    // APPLY record (empty for quests)
		// quests: set the stage in game (the completion stage, or the bad save's stage), or mark completed
		const Quest* quest = nullptr;
		bool         complete = false;
		std::uint32_t goodRuntime = 0;  // the quest's form ID in the good game (0: look it up)
		bool          background = false;  // a quest the journal never showed (radiant, scenes, mod setup)
		bool          journalOnly = false; // only show it in the journal (no stage to set: the good save is as far)
	};

	std::vector<Change> Diff(const Snapshot& a_good, const Snapshot& a_bad);
}
