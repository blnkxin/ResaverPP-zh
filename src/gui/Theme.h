// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Colour themes: light (default), grey and dark. Text colours that carry meaning (warnings, errors,
// good news, dimmed text) come from here so they stay readable on every background.
#pragma once

#include "imgui.h"

#include <cstdint>

namespace theme
{
	enum class Kind
	{
		kLight = 0,
		kGrey = 1,
		kDark = 2
	};

	void        Apply(Kind a_kind);  // sets the ImGui style colours
	Kind        Current();
	const char* Name(Kind a_kind);

	std::uint32_t Warn();
	std::uint32_t Caution();  // a true yellow (Warn is orange): the "caution" colour of the Save check
	std::uint32_t Bad();
	std::uint32_t Good();
	std::uint32_t Dim();
	ImVec4        BannerBg();  // background of a warning banner
	ImVec4        Clear();     // behind every window
	bool          IsLight();   // light backgrounds (the script editor uses its light colours)
}
