// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// 本地化修改：界面、提示与报告文字改为简体中文（详见 README.zh-CN.md）。
// Clean > Remove instances on missing spawned objects: the instances grouped by the mod that provides
// their script (not installed / disabled / installed), with how many something else still points to,
// so the player removes the leftovers of mods that are gone and keeps what an installed mod may use.
#include "App.h"

#include "Theme.h"
#include "Tips.h"

#include "core/Orphans.h"

#include "imgui.h"

#include <algorithm>
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

	void TextColored(std::uint32_t col, const std::string& s)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, col);
		ImGui::TextWrapped("%s", s.c_str());
		ImGui::PopStyleColor();
	}

	std::uint32_t StatusColor(orphans::Status s)
	{
		switch (s) {
		case orphans::Status::kNotInstalled: return theme::Bad();
		case orphans::Status::kDisabled: return theme::Warn();
		case orphans::Status::kInstalled: return theme::Good();
		case orphans::Status::kEngine: return theme::Dim();
		default: return theme::Dim();
		}
	}

	// ticked by default: what no installed mod can use any more
	bool DefaultTick(orphans::Status s)
	{
		return s == orphans::Status::kNotInstalled || s == orphans::Status::kDisabled || s == orphans::Status::kEngine;
	}
}

void App::buildOrphans()
{
	_orphans.clear();
	_orphanSel.clear();
	if (!_save) {
		return;
	}
	_orphansBuiltScanning = scanning();
	_orphans = orphans::Find(*_save, _orphansBuiltScanning ? nullptr : &_sources);
	for (const auto& g : _orphans) {
		_orphanSel.push_back(DefaultTick(g.status) ? 1 : 0);
	}
}

void App::openOrphans()
{
	buildOrphans();
	_orphansOpen = true;
}

void App::drawOrphansModal()
{
	if (_orphansOpenWhenReady && _save && !scanning()) {
		_orphansOpenWhenReady = false;  // (test hook)
		openOrphans();
	}
	if (_orphansOpen) {
		ImGui::OpenPopup("位于缺失生成对象上的实例");
		_orphansOpen = false;
	}
	ImGui::SetNextWindowSize(ImVec2(1000, 660), ImGuiCond_Appearing);
	if (!ImGui::BeginPopupModal("位于缺失生成对象上的实例")) {
		return;
	}
	// the mods scan finished while this was open: now "installed" can be told
	if (_orphansBuiltScanning && !scanning()) {
		buildOrphans();
	}

	ImGui::TextWrapped("模组在游戏过程中生成某个东西时（召唤物、掉落的物品、标记），它的脚本会随之被保存。 "
					   "这些脚本实例属于存档中已不存在的生成对象，因此世界中的任何东西都 "
					   "不会使用它们。下面按提供其脚本的模组分组。");
	TextColored(theme::Dim(), "未安装 / 已禁用：已消失的模组的残留，可以安全移除（已勾选）。游戏对象类型（"
							  "ActorBase、ObjectReference…）只是游戏指向已删除对象的链接，同样已勾选。已安装：你仍在使用的模组，"
							  "它可能故意保留对它们的引用，所以未勾选。“被引用”统计的是另一个脚本、"
							  "数组或调用仍然指向的实例：移除这些实例会把该引用变成“无”。");
	if (scanning()) {
		TextColored(theme::Warn(), "正在扫描你的模组；扫描完成后“状态”列会被填充。");
		drawBackgroundProgress(0);
	}

	std::size_t total = 0, totalHeld = 0;
	for (const auto& g : _orphans) {
		total += g.instances;
		totalHeld += g.held;
	}
	if (_orphans.empty()) {
		ImGui::Spacing();
		ImGui::TextUnformatted("此存档中没有位于缺失生成对象上的脚本实例。");
	} else {
		ImGui::Text("%zu 个实例，来自 %zu 个模组分组；其中 %zu 个被其他对象引用。", total, _orphans.size(), totalHeld);
	}

	const float footer = ImGui::GetFrameHeightWithSpacing() * 3.2f;
	if (ImGui::BeginTable("orphans", 5,
			ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp |
				ImGuiTableFlags_Resizable,
			ImVec2(0, -footer))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
		ImGui::TableSetupColumn("模组 / 脚本", ImGuiTableColumnFlags_WidthStretch, 4.0f);
		ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthStretch, 1.8f);
		ImGui::TableSetupColumn("实例数", ImGuiTableColumnFlags_WidthStretch, 0.9f);
		ImGui::TableSetupColumn("被引用", ImGuiTableColumnFlags_WidthStretch, 0.7f);
		ImGui::TableHeadersRow();
		for (std::size_t gi = 0; gi < _orphans.size(); ++gi) {
			const auto& g = _orphans[gi];
			ImGui::PushID(static_cast<int>(gi));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			bool tick = _orphanSel[gi] != 0;
			if (ImGui::Checkbox("##sel", &tick)) {
				_orphanSel[gi] = tick ? 1 : 0;
			}
			tips::Tip("勾选：移除这个模组的、位于已不存在的生成对象上的脚本实例。");
			ImGui::TableNextColumn();
			const bool open = ImGui::TreeNodeEx("##mod", ImGuiTreeNodeFlags_SpanAvailWidth, "%s  （%zu 个脚本）", g.mod.c_str(),
				g.scripts.size());
			tips::Tip("展开可以看到这个模组里、在缺失的生成对象上留有实例的脚本。");
			ImGui::TableNextColumn();
			ImGui::PushStyleColor(ImGuiCol_Text, StatusColor(g.status));
			ImGui::TextUnformatted(orphans::StatusText(g.status));
			ImGui::PopStyleColor();
			ImGui::TableNextColumn();
			ImGui::Text("%zu", g.instances);
			ImGui::TableNextColumn();
			if (g.held) {
				ImGui::Text("%zu", g.held);
			} else {
				ImGui::TextDisabled("0");
			}
			if (open) {
				for (const auto& s : g.scripts) {
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TableNextColumn();
					ImGui::Indent();
					ImGui::Selectable(s.script.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
					if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
						viewScript(s.script);
					}
					tips::Tip("双击查看脚本");
					ImGui::Unindent();
					ImGui::TableNextColumn();
					ImGui::TableNextColumn();
					ImGui::Text("%zu", s.ids.size());
					ImGui::TableNextColumn();
					if (!s.held.empty()) {
						ImGui::Text("%zu", s.held.size());
					} else {
						ImGui::TextDisabled("0");
					}
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}

	if (ImGui::Button("勾选未安装项")) {
		for (std::size_t i = 0; i < _orphans.size(); ++i) {
			_orphanSel[i] = DefaultTick(_orphans[i].status) ? 1 : 0;
		}
	}
	tips::Tip("只勾选已不再安装的模组：最安全的选择。");
	ImGui::SameLine();
	if (ImGui::Button("全选")) {
		std::fill(_orphanSel.begin(), _orphanSel.end(), 1);
	}
	tips::Tip("勾选列表里的每个模组，包括仍然安装着的。");
	ImGui::SameLine();
	if (ImGui::Button("全不选")) {
		std::fill(_orphanSel.begin(), _orphanSel.end(), 0);
	}
	tips::Tip("取消勾选所有模组。");
	ImGui::SameLine();
	ImGui::Checkbox("保留被引用的实例（更安全）", &_orphansKeepHeld);
	tips::Tip("被引用的实例仍被某个脚本变量或数组指着。保留它们可以避免弄坏还在使用它们的脚本。");

	std::set<EID> ids;
	for (std::size_t i = 0; i < _orphans.size(); ++i) {
		if (!_orphanSel[i]) {
			continue;
		}
		for (const auto& s : _orphans[i].scripts) {
			for (EID id : s.ids) {
				if (!_orphansKeepHeld || !s.held.contains(id)) {
					ids.insert(id);
				}
			}
		}
	}
	ImGui::BeginDisabled(ids.empty());
	if (ImGui::Button(Fmt("移除 %zu 个实例", ids.size()).c_str(), ImVec2(220, 0))) {
		const auto n = orphans::Remove(*_save, ids);
		ImGui::CloseCurrentPopup();
		_orphans.clear();
		_orphanSel.clear();
		markEdited(Fmt("已移除 %zu 个位于缺失生成对象上的实例", n));
	}
	ImGui::EndDisabled();
	tips::Tip("从已载入的存档里删除勾选的脚本实例。在 文件 > 另存为 之前不会写入磁盘；保留原始存档。");
	ImGui::SameLine();
	if (ImGui::Button("取消", ImVec2(120, 0))) {
		ImGui::CloseCurrentPopup();
	}
	tips::Tip("关闭，不做任何改动。");
	ImGui::SameLine();
	TextColored(theme::Dim(), "只有内存中已编辑的存档会变化；请用文件 > 另存为写入。");
	ImGui::EndPopup();
}
