#pragma once

// Everything that touches the game. Game state is only read or changed on the game's main thread
// (through SKSE's task interface); Papyrus functions are called through the script VM, the same way
// a script would call them, so the game's own rules apply (stage scripts run, rewards are given).
namespace Game
{
	// a save is loaded (or a new game started): quests can be looked at and changed
	void SetInGame(bool a_inGame);
	bool InGame();

	// Runs a_fn on the main thread and waits for it, up to a_timeout. False on timeout.
	bool RunOnMain(std::function<void()> a_fn, std::chrono::milliseconds a_timeout = 3s);

	// "hud", "box", "none" or "ini" (= the INI's sNotify). Safe from any thread.
	void Notify(const std::string& a_mode, const std::string& a_text);

	struct QuestInfo
	{
		bool          found = false;
		std::string   editorID;
		std::uint16_t flags = 0;
		std::uint16_t currentStage = 0;
		bool          running = false, completed = false;
		std::vector<std::pair<std::uint16_t, std::uint8_t>> objectives;  // index, state
	};
	QuestInfo GetQuest(std::uint32_t a_formID);  // main thread

	// Calls Quest.SetStage on the quest (main thread). a_done gets true if the game accepted the stage.
	// Falls back to the console command when the quest has no script object.
	void SetStage(std::uint32_t a_formID, std::uint16_t a_stage, std::function<void(bool)> a_done);

	// Journal refresh: for every running quest, every objective the journal shows is hidden and shown
	// again (its completed / failed state put back), so the game rebuilds its list of shown objectives.
	// Returns how many objectives will be refreshed; 0 if nothing to do or one is already running.
	std::size_t StartRefresh(const std::string& a_notify);  // main thread
	struct Progress
	{
		std::size_t done = 0, total = 0;
		bool        running = false;
	};
	Progress RefreshProgress();

	// after a save load: refresh automatically if bRefreshJournalOnLoad=1 (after iRefreshDelaySeconds)
	void OnGameLoaded();
}
