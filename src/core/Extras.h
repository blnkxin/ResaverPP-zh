// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Save history (bloat over time), the Papyrus log reader and the saves folder list.
#pragma once

#include "Save.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ess::extras
{
	// ---- one save's header, read without loading the save -----------------------------------
	struct SaveInfo
	{
		std::filesystem::path path;
		bool                  ok = false;
		bool                  le = false;
		std::string           playerName, location, race;
		std::uint32_t         playerLevel = 0, saveNumber = 0;
		std::uintmax_t        size = 0;      // .ess bytes
		std::uintmax_t        cosaveSize = 0;  // .skse bytes (0 if none)
		std::int64_t          modified = 0;  // file time, seconds since 1970
	};
	SaveInfo ReadSaveInfo(const std::filesystem::path& a_ess);
	std::vector<SaveInfo> ListSaves(const std::filesystem::path& a_folder);  // every .ess in the folder, newest first

	// ---- save history ---------------------------------------------------------------------------
	struct SaveStat
	{
		std::string   file;        // file name only
		std::string   character;
		std::uint32_t saveNumber = 0;
		std::int64_t  modified = 0;
		std::uint64_t size = 0;    // .ess bytes
		std::size_t   instances = 0, threads = 0, created = 0, changeForms = 0, arrays = 0;
		std::map<std::string, std::size_t> instancesByPlugin;  // script instances on forms of each plugin ("Created": spawned forms)
	};
	SaveStat ComputeStat(const SaveFile& a_save, const std::filesystem::path& a_path);
	std::string FormatStat(const SaveStat& a_stat);  // one line of the history file
	bool        ParseStat(const std::string& a_line, SaveStat& a_out);
	// The history file next to the program: one line per save. Stats of the same file and save number are replaced.
	std::vector<SaveStat> LoadHistory(const std::filesystem::path& a_file);
	void                  SaveHistory(const std::filesystem::path& a_file, const std::vector<SaveStat>& a_all);
	void                  AddToHistory(std::vector<SaveStat>& a_all, const SaveStat& a_stat);

	// A plugin whose script instances keep growing across a character's saves (oldest first).
	struct Growth
	{
		std::string plugin;
		std::size_t first = 0, last = 0;
		std::size_t saves = 0;  // over how many saves
	};
	std::vector<Growth> FindGrowth(const std::vector<SaveStat>& a_oldestFirst);

	// ---- Papyrus log --------------------------------------------------------------------------
	struct LogGroup
	{
		std::string script;   // the script the error happened in ("" if the log does not say)
		std::string message;  // the error text, with numbers and form ids taken out so repeats group together
		std::string example;  // one full original error with its stack
		std::size_t count = 0;
	};
	struct LogSummary
	{
		std::filesystem::path file;
		bool                  ok = false;
		std::string           started;  // the log's first line (the game session it is from)
		std::size_t           errors = 0, warnings = 0, lines = 0;
		std::vector<LogGroup> groups;  // most frequent first
		std::map<std::string, std::size_t> byScript;  // errors + warnings per script
	};
	LogSummary ReadPapyrusLog(const std::filesystem::path& a_file);
}
