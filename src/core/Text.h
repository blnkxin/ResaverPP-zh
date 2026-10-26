// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Text in saves and plugins is stored as the game wrote it: UTF-8, or the 8-bit code page of the
// player's Windows language (Windows-1251 for Russian, 1250 for Polish/Czech, 932 for Japanese...).
// These turn it into UTF-8 for display, and back for edits.
#pragma once

#include <filesystem>
#include <string>

namespace ess::text
{
	// 0 = automatic: the system code page on Windows (Windows-1252 elsewhere). 65001 = UTF-8 only.
	void        SetCodePage(unsigned a_codePage);
	unsigned    CodePage();           // the setting (0 = automatic)
	unsigned    EffectiveCodePage();  // the code page actually used for text that is not UTF-8

	bool        IsUtf8(const std::string& a_text);  // ASCII counts as UTF-8
	std::string ToUtf8(const std::string& a_gameText);
	// For an edited value: back to the encoding a_original used (UTF-8 stays UTF-8).
	std::string FromUtf8(const std::string& a_utf8, const std::string& a_original = {});

	// An environment variable holding a folder (LOCALAPPDATA, APPDATA...), read as Unicode. getenv() gives
	// it in the Windows code page, and a path made from that breaks on a user name with letters outside
	// ASCII ("Cannot convert character sequence"). Empty when the variable is not set.
	std::filesystem::path EnvPath(const char* a_name);

	// A path from text that may be UTF-8 or the Windows code page (names read from saves, archives, INIs):
	// never throws, unlike std::filesystem::path(std::string) with text that is not UTF-8.
	std::filesystem::path PathFrom(const std::string& a_text);
}
