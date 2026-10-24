// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Quests: what a quest is (from its QUST record in the plugins: stages, journal entries, objectives,
// stage scripts and their properties) and where it stands in a save (its change form: quest flags,
// stages done, objective states).
#pragma once

#include "Save.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ess::quests
{
	// ---- from the plugin -------------------------------------------------------------------------
	struct LogEntry
	{
		std::uint8_t  flags = 0;  // QSDT: 1 complete quest, 2 fail quest
		std::string   text;       // CNAM journal text
		std::uint32_t nextQuest = 0;
		bool          completes() const { return flags & 1; }
		bool          fails() const { return flags & 2; }
	};

	struct Stage
	{
		std::uint16_t         index = 0;
		std::uint8_t          flags = 0;  // INDX: 2 start-up stage, 4 shut-down stage
		std::vector<LogEntry> logs;
		bool                  completes() const;  // one of its journal entries completes the quest
		bool                  fails() const;
	};

	struct Objective
	{
		std::uint16_t index = 0;
		std::string   text;  // NNAM
	};

	struct Fragment
	{
		std::uint16_t stage = 0;
		std::int32_t  logEntry = 0;
		std::string   script, function;  // e.g. QF_MQ101_0003372B, Fragment_12
	};

	struct Property
	{
		std::string                name;
		std::uint8_t               type = 0;  // 1 object, 2 string, 3 int, 4 float, 5 bool, 11-15 arrays
		std::uint32_t              form = 0;  // object: runtime form ID
		std::int16_t               alias = -1;
		std::string                text;      // string / number / bool as text
		std::vector<std::uint32_t> forms;     // object arrays
	};

	struct QuestDef
	{
		std::string            edid, name;
		std::uint16_t          flags = 0;  // DNAM
		std::uint8_t           priority = 0, type = 0;
		std::vector<Stage>     stages;
		std::vector<Objective> objectives;
		std::string            fragmentScript;   // the quest's stage-fragment script
		std::vector<Fragment>  fragments;
		std::vector<Property>  properties;       // of the fragment script (what its fragments use)
		std::vector<std::string> scripts;        // every script attached to the quest
		std::uint16_t          plugin = 0;       // index into PluginDB::plugins() (last override)

		const Stage*    stage(std::uint16_t a_index) const;
		const Fragment* fragmentFor(std::uint16_t a_stage) const;
		// the stages whose journal entry completes the quest, lowest first
		std::vector<std::uint16_t> completionStages() const;
	};

	using ToRuntime = std::function<bool(std::uint32_t a_raw, std::uint32_t& a_out)>;
	// Reads the VMAD of a QUST record (scripts, their properties, the stage fragments). False if it
	// cannot be read; whatever was read before the problem is kept.
	bool ParseQuestVMAD(const std::uint8_t* a_data, std::size_t a_size, const ToRuntime& a_toRuntime, QuestDef& a_out);

	// ---- what a stage script does -----------------------------------------------------------------
	// The lines of one function ("Fragment_12") in a decompiled script, without its Function/EndFunction.
	std::vector<std::string> FunctionBody(const std::vector<std::string>& a_source, const std::string& a_function);

	struct Effect
	{
		enum class Kind
		{
			kReward,    // AddItem, AddPerk, AddSpell, AddShout, TeachWord, ModActorValue...
			kQuest,     // starts, stops, completes or sets the stage of a quest
			kOther      // calls into other scripts
		} kind = Kind::kOther;
		std::string line;  // the source line, trimmed
	};
	// Lines of a stage script worth telling the player about. Rewards may also be given by other
	// scripts the stage calls; those calls are listed as kOther.
	std::vector<Effect> ScanEffects(const std::vector<std::string>& a_body);

	// Quests that lead into a_target: a journal entry names it as the next quest, or a completion
	// stage script starts it / sets its stage (through a property that is the quest). a_bodies gives the
	// script lines of a quest's completion stages (empty if unknown).
	using StageBodies = std::function<std::vector<std::pair<std::uint16_t, std::vector<std::string>>>(std::uint32_t a_quest)>;
	std::vector<std::uint32_t> LeadsInto(std::uint32_t a_target, const std::unordered_map<std::uint32_t, QuestDef>& a_defs, const StageBodies& a_bodies);

	// ---- from the save ---------------------------------------------------------------------------
	struct ObjectiveState
	{
		std::int32_t index = 0;
		std::int32_t state = 0;  // 0 dormant, 1 shown, 2 completed, 3 completed + shown, 4 failed, 5 failed + shown
	};

	struct QuestState
	{
		std::uint32_t                                  formID = 0;
		bool                                           hasFlags = false;
		std::uint16_t                                  flags = 0;  // 1 running, 2 completed, 0x40 failed, 0x800 active
		std::vector<std::pair<std::int16_t, bool>>     stages;     // stage, done
		std::vector<ObjectiveState>                    objectives;
		bool                                           decoded = false;

		bool          running() const { return hasFlags && (flags & 1); }
		bool          completed() const { return hasFlags && (flags & 2); }
		bool          failed() const { return hasFlags && (flags & 0x40); }
		std::int32_t  highestDoneStage() const;  // -1 if none
		std::size_t   shownObjectives() const;   // objectives the journal shows (states 1, 3, 5)
		const char*   statusText() const;
	};

	// Every quest with saved state in the save (its QUST change form), in change form order.
	std::vector<QuestState> ReadStates(const SaveFile& a_save);
	const char*             ObjectiveStateText(std::int32_t a_state);
}
