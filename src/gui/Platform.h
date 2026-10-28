// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace platform
{
	// Native open/save dialogs on Windows. Elsewhere these return nullopt and the app
	// falls back to typing a path.
	bool                                 HasNativeDialogs();
	std::optional<std::filesystem::path> OpenSaveDialog(const std::filesystem::path& a_startDir);
	std::optional<std::filesystem::path> SaveAsDialog(const std::filesystem::path& a_suggested);
	std::filesystem::path                DefaultSavesFolder(bool a_vr = false);  // Documents\My Games\Skyrim Special Edition (or Skyrim VR)\Saves
	std::optional<std::filesystem::path> PickFolderDialog(const std::string& a_title);
	// Save dialog for other files (reports): a_ext without the dot, e.g. "pdf".
	std::optional<std::filesystem::path> SaveFileDialog(const std::filesystem::path& a_suggested, const std::string& a_ext, const std::string& a_title);
	std::optional<std::filesystem::path> OpenFileDialog(const std::filesystem::path& a_startDir, const std::string& a_ext, const std::string& a_title);
	void                                 OpenWithDefaultApp(const std::filesystem::path& a_path);  // e.g. show a PDF
	std::filesystem::path                SettingsFile();  // resaverpp.ini next to the program
	std::string                          ToUtf8(const std::filesystem::path& a_path);
	// Skyrim Special Edition installs found in the registry (Bethesda key, Steam libraries, GOG), existing ones only
	std::vector<std::filesystem::path>   SkyrimInstallFolders(bool a_vr = false);  // Skyrim VR's when a_vr
	std::filesystem::path                ExeFolder();  // folder of ReSaverPP.exe
	bool                                 RunningUnderMO2();  // started from Mod Organizer (its virtual file system is loaded)
	std::filesystem::path                FromUtf8(const std::string& a_utf8);
	// Moves files to the Recycle Bin (Windows); elsewhere deletes them. True if all of them went.
	bool                                 MoveToRecycleBin(const std::vector<std::filesystem::path>& a_files);
}
