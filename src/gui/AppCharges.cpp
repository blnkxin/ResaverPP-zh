// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// 本地化修改：界面、提示与报告文字改为简体中文（详见 README.zh-CN.md）。
// Mods view: the enchanted and charged items of the selected mod found in inventories. Skyrim keeps
// an item's own enchantment and its max charge (ExtraEnchantment) and its charge left (ExtraCharge) in
// the save, on that one item, so xEdit never shows them. An item with an enchantment but max charge 0
// can never be recharged with soul gems; it can be fixed here.
#include "App.h"

#include "Theme.h"
#include "Tips.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

using namespace ess;

namespace
{
	std::string Fmt(const char* f, auto... args)
	{
		char buf[1024];
		std::snprintf(buf, sizeof(buf), f, args...);
		return buf;
	}

	bool EqualNoCase(const std::string& a, const std::string& b)
	{
		return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
			return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
		});
	}
}

const std::vector<cf::ChargedItem>& App::chargedItems()
{
	if (_chargedGen != _editGen) {
		_charged = _save ? cf::FindChargedItems(*_save) : std::vector<cf::ChargedItem>{};
		_chargedGen = _editGen;
	}
	return _charged;
}

void App::drawModCharged(const std::string& a_plugin)
{
	if (!_save) {
		return;
	}
	std::vector<const cf::ChargedItem*> mine;
	for (const auto& c : chargedItems()) {
		if (EqualNoCase(_save->pluginOf(c.inv.item), a_plugin) || (c.inv.ench && EqualNoCase(_save->pluginOf(c.inv.ench), a_plugin))) {
			mine.push_back(&c);
		}
	}
	std::size_t broken = 0;
	for (const auto* c : mine) {
		broken += cf::NeedsChargeFix(c->inv) ? 1 : 0;
	}
	ImGui::SeparatorText(Fmt("来自此模组的附魔物品与充能物品（%zu）", mine.size()).c_str());
	if (mine.empty()) {
		ImGui::TextDisabled("（任何物品栏中都没有）");
		return;
	}
	ImGui::PushStyleColor(ImGuiCol_Text, theme::Dim());
	ImGui::TextWrapped("基础记录或附魔来自此模组的物品，位于任何物品栏中（你的、随从的、容器的）。 "
					   "附魔与最大充能保存在存档中该物品自身（ExtraEnchantment），充能值则是剩余的部分 "
					   "（ExtraCharge）；xEdit 不显示这些。“来自插件”：基础记录自身的附魔与 EAMT 生效。 "
					   "有附魔但最大充能为 0 的物品永远无法用灵魂石充能。");
	ImGui::PopStyleColor();
	if (broken) {
		ImGui::PushStyleColor(ImGuiCol_Text, theme::Bad());
		ImGui::TextWrapped("%zu 个物品无法充能。", broken);
		ImGui::PopStyleColor();
	}

	if (ImGui::BeginTable("charged", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable)) {
		ImGui::TableSetupColumn("物品", ImGuiTableColumnFlags_WidthStretch, 2.2f);
		ImGui::TableSetupColumn("持有者", ImGuiTableColumnFlags_WidthStretch, 1.6f);
		ImGui::TableSetupColumn("附魔", ImGuiTableColumnFlags_WidthStretch, 2.0f);
		ImGui::TableSetupColumn("最大充能", ImGuiTableColumnFlags_WidthStretch, 0.9f);
		ImGui::TableSetupColumn("充能值", ImGuiTableColumnFlags_WidthStretch, 0.7f);
		ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthStretch, 1.6f);
		ImGui::TableHeadersRow();
		for (const auto* c : mine) {
			const auto& e = c->inv;
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(formText(e.item).c_str());
			if (e.count > 1) {
				ImGui::SameLine();
				ImGui::TextDisabled("x%d", e.count);
			}
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(c->holder == 0x14 ? "玩家" : formText(c->holder).c_str());
			ImGui::TableNextColumn();
			if (!e.hasEnchant) {
				ImGui::TextDisabled("来自插件");
			} else if (!e.enchRaw) {
				ImGui::TextDisabled("（无）");
			} else {
				ImGui::TextUnformatted(formText(e.ench).c_str());
				if ((e.ench >> 24) == 0xFF) {
					ImGui::SameLine();
					ImGui::TextDisabled("（游戏内制作）");
				}
			}
			ImGui::TableNextColumn();
			if (e.hasEnchant) {
				ImGui::Text("%u", e.maxCharge);
			} else {
				ImGui::TextDisabled("EAMT");
			}
			ImGui::TableNextColumn();
			if (e.hasCharge) {
				ImGui::Text("%.0f", e.charge);
			} else {
				ImGui::TextDisabled("已满");
			}
			ImGui::TableNextColumn();
			if (e.hasEnchant && e.enchRaw && e.maxCharge == 0) {
				ImGui::PushStyleColor(ImGuiCol_Text, theme::Bad());
				ImGui::TextUnformatted("无法充能（最大充能为 0）");
				ImGui::PopStyleColor();
			} else if (cf::NeedsChargeFix(e)) {
				ImGui::PushStyleColor(ImGuiCol_Text, theme::Warn());
				ImGui::TextUnformatted("充能值超过上限");
				ImGui::PopStyleColor();
			} else if (e.hasEnchant && !e.enchRaw) {
				ImGui::TextDisabled("未设置附魔");
			} else {
				ImGui::PushStyleColor(ImGuiCol_Text, theme::Good());
				ImGui::TextUnformatted("OK");
				ImGui::PopStyleColor();
			}
		}
		ImGui::EndTable();
	}

	if (broken) {
		ImGui::SetNextItemWidth(120);
		ImGui::InputInt("要赋予的最大充能", &_chargeFixValue, 100, 500);
		tips::Tip("给破损物品赋予的最大附魔充能。大灵魂石或黑色灵魂石可容纳 3000。");
		_chargeFixValue = std::clamp(_chargeFixValue, 1, 65535);
		ImGui::SameLine();
		ImGui::TextDisabled("（大灵魂石或黑色灵魂石可容纳 3000）");
		if (ImGui::Button(Fmt("修复 %zu 个物品使其可以充能", broken).c_str())) {
			std::vector<std::pair<std::uint32_t, std::uint32_t>> only;
			for (const auto* c : mine) {
				if (cf::NeedsChargeFix(c->inv)) {
					only.push_back({ c->holderRefID, c->inv.item });
				}
			}
			const auto n = cf::FixCharges(*_save, static_cast<std::uint16_t>(_chargeFixValue), only);
			markEdited(Fmt("将 %zu 个物品（属于 %s）的最大充能设为 %d", n, a_plugin.c_str(), _chargeFixValue));
		}
		tips::Tip("给这些附魔物品一个最大充能，使它们能重新用灵魂石充能（它们的最大充能丢失了，通常是移除某个模组之后）。");
		ImGui::TextDisabled("只有内存中已编辑的存档会变化；请用文件 > 另存为写入。");
	}
}
