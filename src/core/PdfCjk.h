// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// 本地化新增：为中文 PDF 报告做 TrueType 字体子集化（详见 README.zh-CN.md）。
// Subsets a system TrueType font to the characters a report uses, so the PDF report can show Chinese.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ess::pdf
{
	// a TrueType font trimmed to a set of code points, with the tables a PDF CIDFontType2 needs
	struct Subset
	{
		std::vector<std::uint8_t> file;                     // the subset font program (sfnt)
		std::map<std::uint32_t, std::uint16_t> gid;         // code point -> glyph id in the subset
		std::vector<std::uint16_t> glyphOf;                 // glyph id -> code point (for ToUnicode)
		std::vector<int> widths;                            // glyph id -> advance width in 1/1000 em
		int upem = 1000;
		int ascent = 800, descent = -200;
		int bbox[4] = { 0, -200, 1000, 800 };
		bool ok = false;
	};

	// the first usable Chinese TrueType font on this machine (empty when there is none)
	std::filesystem::path SystemFont();

	// false when the file is not a TrueType font or has nothing for these code points
	bool BuildSubset(const std::filesystem::path& a_font, const std::set<std::uint32_t>& a_codePoints, Subset& a_out);
}
