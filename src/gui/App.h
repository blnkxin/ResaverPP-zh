// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// 本地化修改：界面、提示与报告文字改为简体中文（详见 README.zh-CN.md）。
#pragma once

#include "core/Batch.h"
#include "core/ChangeFormData.h"
#include "core/Compare.h"
#include "core/ModRemove.h"
#include "core/Orphans.h"
#include "core/Pex.h"
#include "core/PluginDB.h"
#include "core/Quests.h"
#include "core/Report.h"
#include "core/Mods.h"
#include "core/Ops.h"
#include "core/ScriptSources.h"
#include "Live.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <thread>
#include <tuple>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

struct ImFont;
struct ScriptDoc;   // AppScripts.cpp
struct CompileJob;  // AppScripts.cpp

class App
{
public:
	enum class Cat
	{
		kOverview,
		kMods,
		kPlugins,
		kChangeForms,
		kStrings,
		kScripts,
		kInstances,
		kReferences,
		kArrays,
		kThreads,
		kMessages,
		kSuspended,
		kUnbinds,
		kQuests,
		kCount
	};

	~App();
	void open(const std::filesystem::path& a_path);
	void openAny(const std::filesystem::path& a_path);  // .ess, .pex or ModOrganizer.ini
	void draw();  // whole UI for one frame
	bool wantsQuit() const { return _quit; }
	bool hasUnsavedChanges() const { return _dirty; }
	void requestQuit();
	// test hook: jump to a category, filter, and focus the first matching row
	void testSelect(int a_cat, const char* a_filter, bool a_analysis);
	void testOpen(const std::string& w)
	{
		if (w == "report") {
			_reportOpen = true;
		} else if (w == "batch") {
			_batchOpen = true;
			std::snprintf(_batchBuf, sizeof(_batchBuf), "QuickLootIEMCM\nSKI_Config* @@ only the SkyUI ones\n");
		} else if (w.rfind("lookup", 0) == 0) {
			_showLookup = true;
			if (w.size() > 7) {
				std::snprintf(_lookupBuf, sizeof(_lookupBuf), "%s", w.substr(7).c_str());
				_lookupAuto = true;
			}
		} else if (w == "watch") {
			setWatching(true);
		} else if (w == "pex") {
			if (const char* s = std::getenv("RESAVERPP_TEST_PEX")) {
				openScript(s);
			}
		} else if (w == "arrays") {
			_showArrays = true;
		} else if (w == "settings") {
			_showSettings = true;
		} else if (w == "support") {
			_showSupport = true;
		} else if (w == "log") {
			_showLog = true;
		} else if (w.rfind("script:", 0) == 0) {
			// script:Name[|function|state|instruction]
			std::vector<std::string> f;
			std::string              cur;
			for (char c : w.substr(7) + "|") {
				if (c == '|') {
					f.push_back(cur);
					cur.clear();
				} else {
					cur.push_back(c);
				}
			}
			f.resize(4);
			viewScript(f[0], f[1], f[2], f[3].empty() ? -1 : std::atoi(f[3].c_str()));
		} else if (w == "rowview") {
			_testRowView = true;
		} else if (w.rfind("quest:", 0) == 0) {
			_cat = Cat::kQuests;
			_questShow = 3;
			_questSel = static_cast<std::uint32_t>(std::strtoul(w.substr(6).c_str(), nullptr, 16));
		} else if (w == "orphans") {
			_orphansOpenWhenReady = true;
		} else if (w == "missing") {
			_missingOpenWhenReady = true;
		} else if (w == "compile") {
			_testCompile = true;
		} else if (w == "compilesettings") {
			_showCompileSettings = true;
		}
	}
	void testRemove(const char* a_mod)
	{
		if (!_save) {
			return;
		}
		_removeMod = a_mod;
		_removeOpen = true;
		_removePlan = std::make_unique<ess::modremove::Plan>(ess::modremove::MakePlan(*_save, _removeMod, _sources.empty() ? nullptr : &_sources, _removeOpt));
	}

	// script viewer (AppScripts.cpp): opens a script by name, preferring its .psc source; without one it
	// offers to decompile the .pex. a_function/a_state/a_instruction place the cursor (a thread's position).
	void viewScript(const std::string& a_script, const std::string& a_function = {}, const std::string& a_state = {}, int a_instruction = -1);
	void viewScriptFile(const std::filesystem::path& a_path);  // a .psc or .pex file
	ImFont* monoFont = nullptr;  // set by main.cpp: fixed-width font for the editor

	// set by main.cpp: uploads RGBA pixels, returns an ImTextureID-compatible handle
	std::function<std::uint64_t(const std::uint8_t*, int, int)> uploadTexture;
	std::function<void(std::uint64_t)>                          freeTexture;

private:
	struct Row
	{
		std::uint64_t key = 0;
		std::string   text;
		std::uint32_t color = 0;  // 0 = default text colour
		std::uint64_t size = 0, mass = 0;  // for sorting
	};

	// ReSaver's filters
	struct Filters
	{
		bool          noParent = false, missingParent = false;           // scripts
		bool          memberless = false, zeroedCanary = false;          // instances
		bool          withScripts = false;                               // instances on changed forms / changed forms with scripts
		bool          longStrings = false;                               // strings
		bool          nullLists = false, cellMinus1 = false, emptyRef = false;  // change forms
		int           parseLevel = 0;                                    // 0 any, 1 fully decoded, 2 partly, 3 not decoded
		std::uint32_t flagMask = 0, flagValue = 0, formFlagMask = 0, formFlagValue = 0;
		char          content[256] = "";
	};
	struct CfFact
	{
		int           parse = 0;
		bool          emptyRef = false, cellMinus1 = false, hasFormFlags = false;
		std::uint32_t formFlags = 0;
		std::size_t   nulls = 0;
	};
	const CfFact& cfFact(const ess::ChangeForm& c);

	// UI pieces
	void drawMenu();
	void drawCategories();
	void drawList();
	void drawDetail();
	void drawOverview();
	void drawAnalysis();
	void drawModals();
	void drawStatus();

	// details per category
	void detailPlugin(std::uint64_t k);
	void detailChangeForm(std::uint64_t k);
	void detailString(std::uint64_t k);
	void detailScript(std::uint64_t k);
	void detailInstance(std::uint64_t k);
	void detailReference(std::uint64_t k);
	void detailArray(std::uint64_t k);
	void detailThread(std::uint64_t k);
	void detailMessage(std::uint64_t k);
	void detailSuspended(std::uint64_t k);
	void detailUnbind(std::uint64_t k);

	// helpers
	void        rebuildRows();
	void        markEdited(const std::string& a_what);
	void        select(Cat a_cat, std::uint64_t a_key);
	std::string describeVar(const ess::Variable& v) const;
	void        varValueWidget(const ess::Variable& v, const char* a_id);
	void        varsTable(const char* a_id, std::vector<ess::Variable>& a_vars, const std::vector<std::string>& a_names);
	std::vector<std::string> memberNames(ess::TStr a_script) const;
	std::string formLabel(const ess::RefID& a_id) const;
	std::string instanceLabel(const ess::ScriptInstance& i) const;
	std::string threadLabel(const ess::ActiveScript& a) const;
	void        doSave(const std::filesystem::path& a_path);
	void        startOpenDialog();
	void        startSaveAsDialog();
	void        confirm(const std::string& a_title, const std::string& a_text, std::function<void()> a_action);
	void        deleteSelected();
	void        markChanged(const std::string& a_what);  // edit that keeps the current selection

	// variable editing
	void editPopup();
	void beginEdit(ess::Variable* a_var);

	// mods view
	void drawModsList();
	void detailMod(std::uint64_t k);
	void ensureMods();
	void scanSources(const std::filesystem::path& a_root);
	void loadSettings();
	void saveSettings();
	void drawRemoveModModal();
	// mods still in the save but no longer in the load order (AppMissing.cpp)
	void checkMissingPlugins();
	void drawMissingBanner();
	void drawMissingModal();
	struct MissingMod
	{
		std::string plugin, folder;
		bool        installed = false;  // the file exists (disabled, or in another profile)
		bool        selected = false;
		std::size_t changeForms = 0, scripts = 0, instances = 0, threads = 0;
		bool        planned = false, blocked = false;
		std::string planText;
	};
	std::vector<MissingMod> _missing;
	std::string             _missingSource;  // which load order it was compared with
	bool                    _missingOpen = false, _missingDismissed = false;
	bool                    _installKnown = false, _missingOpenWhenReady = false;  // the mods were scanned: "installed" can be told
	std::vector<std::pair<std::string, std::size_t>> _missingScripts;  // scripts with no file anywhere -> instances + threads
	bool                    _removeMissingScripts = true;
	// scripts the game loaded when it saved but whose file the scan did not find (uninstalled since, or the
	// scan missed it): listed apart, never removed unless the player ticks them
	std::vector<std::pair<std::string, std::size_t>> _unfoundScripts;
	bool                    _removeUnfoundScripts = false;
	// sanity checks: the load order or the mods scan does not match this save (wrong profile, stale
	// plugins.txt, wrong mods folder). Then nothing is ticked and a warning says why.
	bool                    _orderSuspect = false, _scanSuspect = false;
	std::size_t             _activeInSave = 0, _activeFound = 0, _activeChecked = 0;
	std::filesystem::path   _missingFile;

	// Quests page and the live link to the running game (AppQuests.cpp)
	void                                  drawQuestsList();
	void                                  drawQuestDetail();
	void                                  drawLiveStatus();
	void                                  drawRefreshJournal(bool a_compact);
	const std::vector<ess::quests::QuestState>& questStates();
	struct QuestPreview
	{
		bool                                  done = false;
		std::string                           error;
		std::vector<std::pair<std::uint16_t, std::vector<std::string>>> bodies;  // completion stage -> script lines
	};
	const QuestPreview&                   questPreview(std::uint32_t a_formID);
	void                                  liveQuest(std::uint32_t a_formID);  // asks the game for the quest's live state
	void                                  liveSetStage(std::uint32_t a_formID, std::uint16_t a_stage, const std::string& a_what);
	std::string                           liveNotifyMode() const;
	LiveLink                              _live;
	bool                                  _liveEnabled = true;
	int                                   _liveNotify = 0;  // 0 notification, 1 message box, 2 none
	std::vector<ess::quests::QuestState>  _questStates;
	std::uint64_t                         _questGen = UINT64_MAX;
	char                                  _questFilter[128] = "";
	int                                   _questShow = 0;  // 0 in the journal, 1 running, 2 completed, 3 all
	std::uint32_t                         _questSel = 0;
	std::map<std::uint32_t, QuestPreview> _questPreviews;
	struct LiveQuest
	{
		bool        valid = false;
		std::string error, editorID;
		int         stage = 0;
		bool        running = false, completed = false;
	};
	std::map<std::uint32_t, LiveQuest>    _liveQuests;
	int                                   _setStageIdx = 0;
	struct LiveRefresh
	{
		bool        running = false;
		std::size_t done = 0, total = 0;
		double      nextPoll = 0;
	} _liveRefresh;
	int                                   _liveAutoRefresh = -1;  // helper's bRefreshJournalOnLoad: -1 unknown, 0, 1
	bool                                  _liveOptionAsked = false;

	// enchanted / charged items in inventories, per mod, with a fix for ones that cannot be recharged (AppCharges.cpp)
	void                               drawModCharged(const std::string& a_plugin);
	const std::vector<ess::cf::ChargedItem>& chargedItems();
	std::vector<ess::cf::ChargedItem>  _charged;
	std::uint64_t                      _chargedGen = UINT64_MAX, _editGen = 0;
	int                                _chargeFixValue = 3000;
	bool                               _readOnly = false;  // the open save has structural problems: it is never written

	// Clean > Remove instances on missing spawned objects, grouped by mod (AppOrphans.cpp)
	void                             buildOrphans();
	void                             openOrphans();
	void                             drawOrphansModal();
	std::vector<ess::orphans::Group> _orphans;
	std::vector<char>                _orphanSel;
	bool                             _orphansOpen = false, _orphansOpenWhenReady = false, _orphansKeepHeld = true, _orphansBuiltScanning = false;

	// plugin names, lookup, change forms, clean tools, compare, report, watcher (AppTools.cpp)
	std::string formText(std::uint32_t a_formID) const;
	std::string gameLanguage() const;
	void        startNames(bool a_force = false);
	void        pollNames();
	bool        namesLoading() const;
	void        drawLookup();
	void        drawCfNode(const ess::cf::Node& n, int a_depth, int a_index);
	void        detailChangeFormDecoded(const ess::ChangeForm& c);
	void        resetHavok();
	void        purifyFormLists();
	void        drawBatchModal();
	void        compareWith(const std::filesystem::path& a_other);
	void        drawCompare();
	void        drawReportModal();
	void        writeReport(const std::filesystem::path& a_path);
	std::vector<std::filesystem::path> watchFolders() const;
	void        setWatching(bool a_on);
	void        pollWatch();

	// AppMore.cpp
	bool                  drawFiltersButton();
	void                  pluginActions(const std::set<std::string>& a_plugins);
	std::set<std::string> selectedPlugins() const;
	void                  exportPluginList();
	void                  arrayEditor(ess::ArrayInfo& a);
	void                  drawArraysAnalysis();
	void                  logStatus();
	void                  drawLog();
	void                  drawSettings();
	void                  drawSupport();
	void                  openScript(const std::filesystem::path& a_path);
	// script viewer (AppScripts.cpp)
	void scriptItem(const std::string& a_script, const std::string& a_function = {}, const std::string& a_state = {}, int a_instruction = -1);  // after an item: double-click opens the script
	void drawScriptViewer();
	void viewRowScript(std::uint64_t a_key);  // double-click on a row of the current list
	void drawDoc(ScriptDoc& d);
	void drawDecompilePrompt();
	void drawCompileSettings();
	void openDoc(const std::string& a_script, const std::filesystem::path& a_psc, bool a_decompile, const std::string& a_function, const std::string& a_state,
		int a_instruction, const std::filesystem::path& a_pexFile = {});
	void placeCursor(ScriptDoc& d, const std::string& a_function, const std::string& a_state, int a_instruction);
	bool saveDoc(ScriptDoc& d, bool a_saveAs);
	void startCompile(ScriptDoc& d);
	void pollCompile();
	void stopScriptJobs();
	bool scriptJobsBusy() const;  // a compile is running
	std::filesystem::path gameFolder() const;
	std::filesystem::path compilerPath() const;
	std::filesystem::path compileOutputFolder(const ScriptDoc& d) const;
	bool ensureSourcesScanned();
	void setTheme(int a_kind);  // 0 light, 1 grey, 2 dark  // false while the mods are being scanned (in the background)
	void pollScan();
	// progress of the background work (scan of the mods, then plugin names) as a bar; false if idle.
	// a_width 0: the rest of the line.
	bool drawBackgroundProgress(float a_width);
	bool scanning() const { return _scanThread.joinable(); }
	void autoDetectPaths(bool a_force);  // Mod Organizer, game folder (AppScripts.cpp)
	std::filesystem::path savesFolder() const;  // where the newest saves are
	void fixMcmJam();
	void killThreads(const std::set<ess::ThreadID>& a_threads, const std::set<ess::EID>& a_instances, const std::string& a_what, bool a_terminateOnly);

	std::unique_ptr<ess::SaveFile> _save;
	std::unique_ptr<ess::ops::Report> _report;
	std::filesystem::path          _path;
	std::string                    _status = "打开 Skyrim SE/AE 存档（文件 > 打开，或将 .ess 拖到本窗口）。";
	bool                           _dirty = false;
	bool                           _quit = false;
	bool                           _quitPending = false;

	Cat                     _cat = Cat::kOverview;
	char                    _filter[256] = "";
	std::string             _lastFilter;
	Cat                     _rowsCat = Cat::kCount;
	bool                    _rowsDirty = true;
	bool                    _onlyFlagged = false;
	Filters                 _filters;
	std::string             _pluginFilter;  // ReSaver "Set filter for this plugin"
	int                     _sortMode = 1;   // 0 none (save order), 1 alphabetical, 2 size, 3 mass
	std::unordered_map<std::uint32_t, CfFact> _cfFacts;
	std::vector<Row>        _rows;
	std::set<std::uint64_t> _selected;
	std::optional<std::uint64_t> _focus;
	std::optional<std::size_t>   _anchor;  // shift-click anchor (row index)
	bool                    _scrollToFocus = false;
	bool                    _focusFirstRow = false;

	std::uint64_t _shotTex = 0;
	int           _shotW = 0, _shotH = 0;

	bool _showAnalysis = false;
	bool _showAbout = false;

	// modal state
	bool                  _openPathPopup = false, _savePathPopup = false;
	char                  _pathBuf[1024] = "";
	bool                  _confirmOpen = false;
	std::string           _confirmTitle, _confirmText;
	std::function<void()> _confirmAction;
	bool                  _purgeOpen = false;
	char                  _purgeName[256] = "";
	bool                  _purgeForms = true;
	bool                  _purgeKeep = false;
	std::string           _error;

	// variable editing
	ess::Variable* _editVar = nullptr;
	bool           _editOpen = false;
	int            _editInt = 0;
	float          _editFloat = 0;
	bool           _editBool = false;
	char           _editStr[1024] = "";

	// mods view
	std::unique_ptr<ess::mods::Table> _mods;
	bool                              _modsDirty = true;
	bool                              _modsOnlyScripts = true;
	ess::ScriptSources                _sources;
	// the mods scan runs on its own thread (it can take a while, much longer inside Mod Organizer)
	std::thread                                _scanThread;
	std::unique_ptr<ess::ScriptSources>        _scanResult;
	std::unique_ptr<ess::ScanProgress>         _scanProgress;
	std::atomic<bool>                          _scanDone{ false };
	std::filesystem::path                      _scanRoot;
	struct PendingView
	{
		std::string script, function, state;
		int         instruction = -1;
	};
	std::optional<PendingView> _pendingView;  // a script double-clicked while scanning
	std::string                       _sourcesPath;
	std::string                       _profile;
	std::optional<ess::Mo2Instance>   _mo2;
	std::optional<ess::VortexDeployment> _vortex;  // when the mods are managed by Vortex
	bool                              _settingsLoaded = false;
	std::vector<std::size_t>          _modOrder;  // filtered + sorted row indices
	std::unordered_map<std::string, std::pair<std::size_t, std::size_t>> _scriptTotals;  // lowercase script -> (instances, threads)
	char                              _scanPathBuf[1024] = "";

	// plugin names
	std::unique_ptr<ess::PluginDB>           _db, _dbBuilding;
	std::unique_ptr<ess::PluginDB::Progress> _dbProgress;
	std::thread                              _dbThread;
	std::atomic<bool>                        _dbDone{ false };
	std::string                              _dbKey;
	bool                                     _autoNames = true;
	// lookup
	bool                       _showLookup = false;
	char                       _lookupBuf[256] = "";
	bool                       _lookupAuto = false;
	std::vector<std::uint32_t> _lookupResults;
	struct LookupInfo
	{
		bool          hasCf = false;
		std::uint32_t cfRaw = 0;
		std::size_t   scripts = 0;
		ess::EID      firstInstance = 0;
	};
	std::vector<LookupInfo> _lookupInfo;
	// compare
	bool                     _showCompare = false;
	std::vector<std::string> _compareLines;
	std::string              _compareTitle;
	// batch clean
	bool                           _batchOpen = false, _batchResolved = false;
	char                           _batchBuf[16384] = "";
	std::vector<ess::batch::Match> _batchMatches;
	// report
	bool                _reportOpen = false;
	ess::report::Options _reportOpt;
	// watcher
	bool                                                              _watch = false;
	double                                                            _watchNext = 0;
	std::map<std::filesystem::path, std::filesystem::file_time_type>  _watchSeen;
	std::map<std::filesystem::path, std::pair<std::uintmax_t, double>> _watchPending;

	// AppMore state
	int                      _arrayIndex = 0;
	bool                     _showArrays = false, _arraysDirty = true;
	std::vector<std::tuple<std::string, std::size_t, std::size_t>> _arrStats;
	std::vector<std::pair<std::size_t, ess::EID>>                  _arrBiggest;
	std::vector<ess::EID>                                          _arrOrphans;
	std::vector<std::string> _log;
	std::string              _lastLogged, _lastLoggedError;
	bool                     _showLog = false, _showSettings = false, _showSupport = false;
	int                      _saveCompression = -1;  // -1 keep, else ess::Compression
	// script viewer
	std::vector<std::shared_ptr<ScriptDoc>> _docs;
	int                                     _docSelect = -1;  // tab to bring to front
	bool                                    _showViewer = false, _viewerFocused = false;
	std::shared_ptr<CompileJob>             _compileJob;
	std::shared_ptr<void>                   _scriptNamesCache;  // psc::Context shared by the editors
	std::size_t                             _scriptNamesKey = 0;
	bool        _decompileAsk = false, _decompileAlways = false, _editorLight = true;
	std::string _decompileScript, _decompileFn, _decompileState, _decompileWhere;
	int         _decompileInstr = -1;
	bool        _showCompileSettings = false;
	std::string _gamePath, _detectNote;
	// Skyrim SE/AE or Skyrim VR: each has its own mods folder, game folder and profile. The game of the
	// opened save decides which set is active; the other is kept here.
	bool _vr = false;
	struct GameSetup
	{
		std::string sourcesPath, gamePath, profile;
	};
	GameSetup _otherGame;
	void      setGame(bool a_vr);
	const char* gameName() const { return _vr ? "Skyrim VR" : "Skyrim Special Edition"; }
	std::string _compilerSetting, _compileOutDir, _compileFlags = "TESV_Papyrus_Flags.flg", _compileImports;
	int         _compileOutMode = 0;  // 0 the script's mod, 1 MO2 overwrite, 2 custom folder
	bool        _compileOptimize = false, _compileAnonymize = false, _compileBackup = true;
	char        _findBuf[256] = "";
	bool        _findFocus = false, _gotoOpen = false, _testRowView = false, _testCompile = false;
	int         _gotoLine = 1;

	// remove mod from save
	bool                                    _removeOpen = false;
	std::string                             _removeMod;
	ess::modremove::Options                 _removeOpt;
	std::unique_ptr<ess::modremove::Plan>   _removePlan;
	bool                              _scanPathPopup = false;
};
