#pragma once

// ReSaverHelper.ini, next to the DLL. ReSaver++ can change these through the pipe (SETOPTION); the
// helper writes the file back keeping its comments.
namespace Settings
{
	struct Values
	{
		std::string notify = "hud";           // sNotify: hud | box | none - how the helper confirms what it did
		bool        refreshOnLoad = false;    // bRefreshJournalOnLoad: refresh the quest journal after every save load
		int         refreshDelay = 5;         // iRefreshDelaySeconds: wait after the load before refreshing
		bool        listen = true;            // bListen: accept commands from ReSaver++ at all
	};

	Values&               Get();
	void                  Load();
	bool                  Save();  // writes the current values into the INI (creates it if missing)
	std::filesystem::path IniPath();

	// option by its INI key; false if the name or value is not valid
	bool        SetOption(const std::string& a_name, const std::string& a_value);
	std::string GetOption(const std::string& a_name);
}
