// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Theme.h"

namespace theme
{
	namespace
	{
		Kind g_kind = Kind::kLight;

		ImVec4 Rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }
	}

	const char* Name(Kind k)
	{
		switch (k) {
		case Kind::kLight: return "浅色";
		case Kind::kGrey: return "灰色";
		default: return "深色";
		}
	}

	Kind Current() { return g_kind; }
	bool IsLight() { return g_kind == Kind::kLight; }

	void Apply(Kind k)
	{
		g_kind = k;
		auto& s = ImGui::GetStyle();
		auto* c = s.Colors;
		switch (k) {
		case Kind::kLight:
			ImGui::StyleColorsLight();
			c[ImGuiCol_Text] = Rgb(28, 28, 32);
			c[ImGuiCol_TextDisabled] = Rgb(120, 120, 128);
			c[ImGuiCol_WindowBg] = Rgb(240, 241, 244);
			c[ImGuiCol_ChildBg] = Rgb(250, 250, 252);
			c[ImGuiCol_PopupBg] = Rgb(252, 252, 253);
			c[ImGuiCol_MenuBarBg] = Rgb(228, 230, 235);
			c[ImGuiCol_Border] = Rgb(200, 202, 210);
			c[ImGuiCol_FrameBg] = Rgb(255, 255, 255);
			c[ImGuiCol_FrameBgHovered] = Rgb(232, 238, 248);
			c[ImGuiCol_FrameBgActive] = Rgb(220, 230, 246);
			c[ImGuiCol_TitleBg] = Rgb(214, 218, 226);
			c[ImGuiCol_TitleBgActive] = Rgb(186, 202, 228);
			c[ImGuiCol_Header] = Rgb(196, 214, 240);
			c[ImGuiCol_HeaderHovered] = Rgb(178, 202, 236);
			c[ImGuiCol_HeaderActive] = Rgb(158, 188, 232);
			c[ImGuiCol_Button] = Rgb(206, 218, 236);
			c[ImGuiCol_ButtonHovered] = Rgb(184, 202, 230);
			c[ImGuiCol_ButtonActive] = Rgb(160, 184, 222);
			c[ImGuiCol_Tab] = Rgb(214, 222, 236);
			c[ImGuiCol_TabHovered] = Rgb(184, 202, 230);
			c[ImGuiCol_TabSelected] = Rgb(170, 194, 230);
			c[ImGuiCol_TableHeaderBg] = Rgb(222, 226, 234);
			c[ImGuiCol_TableRowBgAlt] = Rgb(0, 0, 0, 0.035f);
			c[ImGuiCol_CheckMark] = Rgb(40, 100, 200);
			break;
		case Kind::kGrey:
			ImGui::StyleColorsDark();
			c[ImGuiCol_Text] = Rgb(236, 236, 238);
			c[ImGuiCol_TextDisabled] = Rgb(170, 170, 176);
			c[ImGuiCol_WindowBg] = Rgb(66, 67, 72);
			c[ImGuiCol_ChildBg] = Rgb(74, 75, 80);
			c[ImGuiCol_PopupBg] = Rgb(62, 63, 68);
			c[ImGuiCol_MenuBarBg] = Rgb(58, 59, 64);
			c[ImGuiCol_Border] = Rgb(104, 106, 114);
			c[ImGuiCol_FrameBg] = Rgb(90, 92, 99);
			c[ImGuiCol_FrameBgHovered] = Rgb(104, 108, 118);
			c[ImGuiCol_FrameBgActive] = Rgb(114, 120, 132);
			c[ImGuiCol_TitleBg] = Rgb(54, 55, 60);
			c[ImGuiCol_TitleBgActive] = Rgb(78, 92, 118);
			c[ImGuiCol_Header] = Rgb(96, 114, 144);
			c[ImGuiCol_HeaderHovered] = Rgb(108, 128, 162);
			c[ImGuiCol_HeaderActive] = Rgb(120, 142, 180);
			c[ImGuiCol_Button] = Rgb(92, 108, 134);
			c[ImGuiCol_ButtonHovered] = Rgb(108, 126, 158);
			c[ImGuiCol_ButtonActive] = Rgb(124, 144, 180);
			c[ImGuiCol_Tab] = Rgb(80, 90, 108);
			c[ImGuiCol_TabHovered] = Rgb(108, 126, 158);
			c[ImGuiCol_TabSelected] = Rgb(96, 114, 144);
			c[ImGuiCol_TableHeaderBg] = Rgb(84, 86, 94);
			c[ImGuiCol_TableRowBgAlt] = Rgb(255, 255, 255, 0.04f);
			break;
		case Kind::kDark:
			ImGui::StyleColorsDark();
			c[ImGuiCol_WindowBg] = ImVec4(0.11f, 0.11f, 0.12f, 1);
			c[ImGuiCol_ChildBg] = ImVec4(0.13f, 0.13f, 0.14f, 1);
			c[ImGuiCol_Header] = ImVec4(0.28f, 0.36f, 0.48f, 0.85f);
			c[ImGuiCol_HeaderHovered] = ImVec4(0.32f, 0.42f, 0.56f, 0.9f);
			c[ImGuiCol_HeaderActive] = ImVec4(0.36f, 0.48f, 0.64f, 1);
			c[ImGuiCol_Button] = ImVec4(0.24f, 0.32f, 0.44f, 1);
			c[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.40f, 0.55f, 1);
			c[ImGuiCol_ButtonActive] = ImVec4(0.34f, 0.46f, 0.62f, 1);
			break;
		}
	}

	std::uint32_t Warn()
	{
		switch (g_kind) {
		case Kind::kLight: return IM_COL32(170, 92, 0, 255);
		case Kind::kGrey: return IM_COL32(255, 206, 110, 255);
		default: return IM_COL32(255, 190, 80, 255);
		}
	}
	std::uint32_t Bad()
	{
		switch (g_kind) {
		case Kind::kLight: return IM_COL32(190, 28, 28, 255);
		case Kind::kGrey: return IM_COL32(255, 140, 140, 255);
		default: return IM_COL32(255, 110, 110, 255);
		}
	}
	std::uint32_t Good()
	{
		switch (g_kind) {
		case Kind::kLight: return IM_COL32(16, 128, 40, 255);
		case Kind::kGrey: return IM_COL32(150, 232, 150, 255);
		default: return IM_COL32(130, 220, 130, 255);
		}
	}
	std::uint32_t Dim()
	{
		switch (g_kind) {
		case Kind::kLight: return IM_COL32(112, 112, 120, 255);
		case Kind::kGrey: return IM_COL32(178, 178, 184, 255);
		default: return IM_COL32(150, 150, 150, 255);
		}
	}
	ImVec4 BannerBg()
	{
		switch (g_kind) {
		case Kind::kLight: return Rgb(255, 238, 204);
		case Kind::kGrey: return Rgb(104, 84, 44);
		default: return ImVec4(0.30f, 0.22f, 0.08f, 1);
		}
	}
	ImVec4 Clear()
	{
		switch (g_kind) {
		case Kind::kLight: return Rgb(226, 228, 233);
		case Kind::kGrey: return Rgb(52, 53, 58);
		default: return ImVec4(0.08f, 0.08f, 0.09f, 1);
		}
	}
}
