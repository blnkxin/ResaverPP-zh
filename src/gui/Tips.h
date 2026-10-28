#pragma once
// Hover tips: a short explanation shown after the mouse rests on something for a few seconds.
// Options > Show hover tips turns them off.

#include <string>
#include <string_view>

namespace tips
{
	inline bool  g_enabled = true;    // Options > Show hover tips
	inline float g_delay = 3.0f;      // seconds the mouse must rest on an item

	// call right after the item it explains
	void Tip(const char* a_text);
	inline void Tip(const std::string& a_text) { Tip(a_text.c_str()); }
	// printf-style; formats only when the tip is about to show
	void Tipf(const char* a_fmt, ...);
	// True once the mouse has rested on the last item for the tip delay (and tips are on)
	bool Ready();

	// explanation of a label in the decoded change form view, a sidebar category, a change form type, a
	// change flag, an extra data type or an actor value; nullptr when there is none
	const char* Explain(std::string_view a_label);
}
