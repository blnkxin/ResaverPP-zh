// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Find NPCs: every actor (ACHR) with where the save has it, and "Move to me" for a lost follower.
#include "App.h"

#include "Theme.h"
#include "Tips.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

using namespace ess;

namespace
{
	std::string Fmt(const char* f, auto... args)
	{
		char buf[512];
		std::snprintf(buf, sizeof(buf), f, args...);
		return buf;
	}

	std::string Upper(std::string s)
	{
		for (auto& c : s) {
			c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		}
		return s;
	}
}

void App::drawActors()
{
	prepareWindow("Find NPCs", ImVec2(0.75f, 0.8f));
	if (!ImGui::Begin("查找 NPC", &_showActors, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Find NPCs");

	// (re)build the rows when the save, its edits or the plugin names change
	if (_actorRowsSave != _save.get() || _actorRowsGen != _editGen || _actorRowsDb != _db.get()) {
		_actorRowsSave = _save.get();
		_actorRowsGen = _editGen;
		_actorRowsDb = _db.get();
		_actorRows.clear();
		const auto places = cf::ListActors(*_save);
		const cf::ActorPlace* player = nullptr;
		for (const auto& p : places) {
			if (p.isPlayer) {
				player = &p;
			}
		}
		for (const auto& p : places) {
			ActorRow r;
			r.place = p;
			r.name = p.isPlayer ? "你（玩家）" : formText(p.formID);
			if (p.baseObject) {
				r.name += "  （游戏内生成：" + formText(p.baseObject) + "）";
			}
			if (p.hasPosition) {
				r.where = formText(p.cell) + Fmt("  位于 %.0f, %.0f, %.0f", p.pos[0], p.pos[1], p.pos[2]);
				if (player && player->hasPosition && player->cell == p.cell && !p.isPlayer) {
					const float dx = p.pos[0] - player->pos[0], dy = p.pos[1] - player->pos[1], dz = p.pos[2] - player->pos[2];
					r.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
				}
			} else {
				r.where = "插件摆放的位置（游戏从未移动过它）";
			}
			r.filterKey = Upper(r.name + " " + Fmt("%08X", p.formID) + " " + r.where);
			_actorRows.push_back(std::move(r));
		}
	}

	ImGui::NewLine();
	ImGui::TextWrapped("存档里有记录的每个 NPC 和生物，以及存档把它们放在哪个单元格（室内）或世界空间（室外）。"
					   "“叫到身边”会把一个角色放到你面前并重置它的 AI：失散或卡住的随从就用这个。改动只发生在"
					   "已载入的存档中：在你用“文件 > 保存”或“另存为”之前不会写入。");
	ImGui::Spacing();
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22);
	ImGui::InputTextWithHint("##actfilter", "过滤：名称、表单 ID 或地点", _actorFilter, sizeof(_actorFilter));
	tips::Tip("只显示名称、表单 ID 或地点里有这段文字的角色，例如 Lydia、Breezehome 或 000A2C94。");
	ImGui::SameLine();
	ImGui::Checkbox("只看附近的", &_actorNearOnly);
	tips::Tip("只显示和你在同一单元格（室内）或同一世界空间（室外）的角色。");
	ImGui::SameLine();
	ImGui::Checkbox("只看游戏移动过的", &_actorMovedOnly);
	tips::Tip("隐藏游戏从未移动过的角色：它们站在插件摆放的位置上，存档里没有它们的坐标"
			  "（在这里也就无法移动它们）。");

	const auto filter = Upper(_actorFilter);
	std::vector<const ActorRow*> rows;
	rows.reserve(_actorRows.size());
	for (const auto& r : _actorRows) {
		if (_actorMovedOnly && !r.place.hasPosition && !r.place.isPlayer) {
			continue;
		}
		if (_actorNearOnly && r.distance < 0 && !r.place.isPlayer) {
			continue;
		}
		if (!filter.empty() && r.filterKey.find(filter) == std::string::npos) {
			continue;
		}
		rows.push_back(&r);
	}
	ImGui::SameLine();
	ImGui::TextDisabled("%zu / %zu 个角色", rows.size(), _actorRows.size());

	constexpr ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
	                               ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingStretchProp;
	if (ImGui::BeginTable("actors", 4, tf)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("角色", ImGuiTableColumnFlags_WidthStretch, 3.0f);
		ImGui::TableSetupColumn("存档中的位置", ImGuiTableColumnFlags_WidthStretch, 3.0f);
		ImGui::TableSetupColumn("距离", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, ImGui::GetFontSize() * 6);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, ImGui::GetFontSize() * 8);
		ImGui::TableHeadersRow();
		if (auto* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsCount > 0) {
			_actorSortCol = specs->Specs[0].ColumnIndex;
			_actorSortAsc = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
		}
		std::stable_sort(rows.begin(), rows.end(), [&](const ActorRow* a, const ActorRow* b) {
			if (a->place.isPlayer != b->place.isPlayer) {
				return a->place.isPlayer;  // you first
			}
			bool less = false;
			switch (_actorSortCol) {
			case 0: less = a->name < b->name; break;
			case 1: less = a->where < b->where; break;
			default:
				if ((a->distance < 0) != (b->distance < 0)) {
					return a->distance >= 0;  // another place: last either way
				}
				if (a->distance == b->distance) {
					return a->name < b->name;
				}
				less = a->distance < b->distance;
			}
			return _actorSortAsc ? less : !less;
		});

		ImGuiListClipper clip;
		clip.Begin(static_cast<int>(rows.size()));
		while (clip.Step()) {
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
				const auto& r = *rows[static_cast<std::size_t>(i)];
				ImGui::PushID(static_cast<int>(r.place.refRaw));
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(r.name.c_str());
				ImGui::TableNextColumn();
				if (r.place.hasPosition) {
					ImGui::TextUnformatted(r.where.c_str());
					if (r.place.startCell && r.place.startCell != r.place.cell) {
						tips::Tipf("它的插件把它放在 %s；之后它移动过。", formText(r.place.startCell).c_str());
					}
				} else {
					ImGui::TextDisabled("%s", r.where.c_str());
				}
				ImGui::TableNextColumn();
				if (r.place.isPlayer) {
					ImGui::TextDisabled("-");
				} else if (r.distance >= 0) {
					ImGui::Text("%.0f", r.distance);
					tips::Tip("与你的距离，游戏单位（约 70 单位 = 1 米）。");
				} else {
					ImGui::TextDisabled("其他地方");
				}
				ImGui::TableNextColumn();
				if (ImGui::SmallButton("跳转")) {
					select(Cat::kChangeForms, r.place.refRaw);
				}
				tips::Tip("打开这个角色的变更表单：它保存的数据、额外数据和 NPC 修复。");
				if (!r.place.isPlayer) {
					ImGui::SameLine();
					ImGui::BeginDisabled(!r.place.hasPosition);
					if (ImGui::SmallButton("叫到身边")) {
						const auto raw = r.place.refRaw;
						const auto name = r.name;
						confirm("把角色叫到身边",
							"把 " + name + " 叫到你身边？\n\n存档会把它放到你前面几步的地方，就在你所在的单元格或世界空间里，"
							"并重置它的 AI，这样它就不会继续走原来的路。随从会重新跟着你。它自己的 AI 包之后仍可能把它送回家；"
							"那是游戏的事，不是存档的事。\n\n请保留原始存档的副本。",
							[this, raw, name] {
								RefID id;
								id.data = raw;
								auto* cf = _save->findChangeForm(id);
								if (!cf) {
									_error = "无法移动 " + name + "：它的变更表单已经不在了";
									return;
								}
								if (auto why = cf::MoveActorToPlayer(*_save, *cf); !why.empty()) {
									_error = "无法移动 " + name + "：" + why;
									return;
								}
								markChanged("已把 " + name + " 叫到你身边");
							});
					}
					ImGui::EndDisabled();
					if (r.place.hasPosition) {
						tips::Tip("把这个角色放到你面前并重置它的 AI。用于失散或卡住的随从。");
					} else {
						tips::Tip("游戏从未移动过这个角色，所以存档里没有可改的坐标。在游戏里：打开控制台，输入 prid 加上它的"
								  "表单 ID，再输入 moveto player。");
					}
				}
				ImGui::PopID();
			}
		}
		ImGui::EndTable();
	}
	ImGui::End();
}
