// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Leftover items from removed mods, save history (bloat over time), the Papyrus log reader and the
// saves folder manager.
#include "App.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "core/LoadOrder.h"
#include "core/Text.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <set>

using namespace ess;

namespace
{
	std::string Fmt(const char* f, auto... args)
	{
		char buf[1024];
		std::snprintf(buf, sizeof(buf), f, args...);
		return buf;
	}

	std::string Lower(std::string s)
	{
		for (auto& c : s) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return s;
	}

	void TextColored(std::uint32_t col, const std::string& s)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, col);
		ImGui::TextWrapped("%s", s.c_str());
		ImGui::PopStyleColor();
	}

	std::string Date(std::int64_t a_unix)
	{
		if (a_unix <= 0) {
			return "?";
		}
		const std::time_t t = static_cast<std::time_t>(a_unix);
		std::tm           tm{};
#ifdef _WIN32
		localtime_s(&tm, &t);
#else
		localtime_r(&t, &tm);
#endif
		char buf[32];
		std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
		return buf;
	}

	std::filesystem::path HistoryFile() { return platform::SettingsFile().parent_path() / "resaverpp-history.txt"; }
}

loadorder::Active App::currentLoadOrder() const
{
	std::filesystem::path txt;
	if (_mo2 && !_profile.empty()) {
		txt = _mo2->profilesDir / platform::FromUtf8(_profile) / "plugins.txt";
	} else {
		txt = loadorder::GamePluginsTxt(_vr);
	}
	return loadorder::Read(txt, gameFolder(), _vr);
}

// ==========================================================================================
// leftover items
// ==========================================================================================

void App::drawLeftovers()
{
	prepareWindow("Leftover items", ImVec2(0.7f, 0.75f));
	if (!ImGui::Begin("残留物品", &_showLeftovers, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Leftover items");
	if (_leftoversDirty) {
		_leftoversDirty = false;
		_leftovers.clear();
		_leftoversNote.clear();
		const auto active = currentLoadOrder();
		std::set<std::string> dbRead;
		if (_db) {
			for (const auto& p : _db->plugins()) {
				dbRead.insert(Lower(p));
			}
		}
		if (!active.known && !_db) {
			_leftoversNote = "还不知道你的加载顺序（请在 选项 > 设置 里设置游戏文件夹与模组管理器），插件名称也还没有读取，"
							 "因此 ReSaver Plus 无法判断哪些物品已经不存在了。";
		}
		std::map<std::string, std::size_t> index;
		for (const auto& e : cf::ListInventories(*_save)) {
			std::string plugin, reason;
			if (e.item == 0) {
				plugin = "（无）";
				reason = "一个没有物品的条目";
			} else {
				plugin = _save->pluginOf(e.item);
				const auto low = Lower(plugin);
				if (plugin == "Created") {
					continue;  // made in game (enchanted, crafted): kept with the save
				}
				if (plugin == "?") {
					reason = "它的插件不在存档的插件列表中";
				} else if (active.known && !active.plugins.contains(low)) {
					reason = "它的插件不在你的加载顺序中";
				} else if (_db && dbRead.contains(low) && !_db->find(e.item)) {
					reason = "该物品已不在它的插件里（模组被更新或改动过）";
				} else {
					continue;
				}
			}
			const auto key = plugin + "\x1f" + reason;
			auto       it = index.find(key);
			if (it == index.end()) {
				it = index.emplace(key, _leftovers.size()).first;
				LeftoverGroup g;
				g.plugin = plugin;
				g.reason = reason;
				_leftovers.push_back(std::move(g));
			}
			auto& g = _leftovers[it->second];
			g.items[e.item].push_back(e.holderRefRaw);
			++g.entries;
		}
	}

	ImGui::NewLine();
	ImGui::TextWrapped("玩家、NPC 和容器的物品栏里，所属模组已经不存在的物品。游戏无法显示它们，还会让物品栏和容器菜单"
					   "出问题或导致崩溃。移除它们会把它们从存档的每个物品栏里拿掉。改动只发生在已载入的存档中："
					   "在你用“文件 > 保存”或“另存为”之前不会写入。");
	if (!_leftoversNote.empty()) {
		TextColored(theme::Warn(), _leftoversNote);
	}
	if (ImGui::Button("重新查看")) {
		_leftoversDirty = true;
	}
	tips::Tip("重新读取每一个物品栏（在你改动了加载顺序或读取了插件名称之后）。");
	ImGui::Spacing();
	if (_leftovers.empty()) {
		TextColored(theme::Good(), _leftoversNote.empty() ? "没有残留物品：每个物品栏里的每件物品都属于你仍安装着的模组。" : "什么也没找到。");
		ImGui::End();
		return;
	}
	std::size_t ticked = 0;
	for (const auto& g : _leftovers) {
		ticked += g.selected ? g.entries : 0;
	}
	if (ImGui::BeginTable("leftovers", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
			ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.6f))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("模组（插件）", ImGuiTableColumnFlags_WidthStretch, 2.0f);
		ImGui::TableSetupColumn("原因", ImGuiTableColumnFlags_WidthStretch, 2.5f);
		ImGui::TableSetupColumn("物品", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("条目", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableHeadersRow();
		for (std::size_t gi = 0; gi < _leftovers.size(); ++gi) {
			auto& g = _leftovers[gi];
			ImGui::PushID(static_cast<int>(gi));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Checkbox("##sel", &g.selected);
			tips::Tip("已勾选：把这些物品从每个物品栏里移除。");
			ImGui::SameLine();
			const bool open = ImGui::TreeNodeEx(g.plugin.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
			tips::Tip("显示这些物品，以及谁持有它们。");
			ImGui::TableNextColumn();
			ImGui::TextWrapped("%s", g.reason.c_str());
			ImGui::TableNextColumn();
			ImGui::Text("%zu", g.items.size());
			ImGui::TableNextColumn();
			ImGui::Text("%zu", g.entries);
			if (open) {
				for (const auto& [item, holders] : g.items) {
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextDisabled("   %s", formText(item).c_str());
					ImGui::TableNextColumn();
					std::string who;
					std::set<std::uint32_t> seen;
					for (auto h : holders) {
						if (seen.insert(h).second && seen.size() <= 4) {
							RefID id;
							id.data = h;
							const auto fid = _save->formIDOf(id);
							who += (who.empty() ? "" : "、") + (fid == 0x14 ? std::string("你") : formText(fid));
						}
					}
					if (seen.size() > 4) {
						who += Fmt(" 还有 %zu 个", seen.size() - 4);
					}
					ImGui::TextDisabled("持有者：%s", who.c_str());
					ImGui::TableNextColumn();
					ImGui::TableNextColumn();
					ImGui::TextDisabled("%zu", holders.size());
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::BeginDisabled(ticked == 0);
	if (ImGui::Button(Fmt("移除勾选的物品（%zu 个条目）", ticked).c_str())) {
		confirm("移除残留物品",
			Fmt("把 %zu 个所属模组已不存在的物品条目，从存档的每一个物品栏里移除？\n\n只对确定永久卸载的模组这样做：如果把模组"
				"又装回去，它的物品也不会回来。\n\n请保留原始存档的副本。",
				ticked),
			[this] {
				// per holder, the items to take out
				std::map<std::uint32_t, std::set<std::uint32_t>> perHolder;
				for (const auto& g : _leftovers) {
					if (!g.selected) {
						continue;
					}
					for (const auto& [item, holders] : g.items) {
						for (auto h : holders) {
							perHolder[h].insert(item);
						}
					}
				}
				std::size_t removed = 0, failed = 0;
				for (const auto& [h, items] : perHolder) {
					RefID id;
					id.data = h;
					auto* cf = _save->findChangeForm(id);
					std::size_t n = 0;
					if (!cf || !cf::RemoveInventoryItems(*_save, *cf, items, &n).empty()) {
						++failed;
						continue;
					}
					removed += n;
				}
				markChanged(Fmt("已移除 %zu 个残留物品条目%s", removed,
					failed ? Fmt("（%zu 个物品栏无法改动，已原样留着）", failed).c_str() : ""));
				_leftoversDirty = true;
			});
	}
	ImGui::EndDisabled();
	tips::Tip("把勾选的模组的物品从已载入的存档的每个物品栏里拿掉。");
	ImGui::End();
}

// ==========================================================================================
// save history
// ==========================================================================================

void App::recordHistory()
{
	if (!_save) {
		return;
	}
	if (!_historyLoaded) {
		_history = extras::LoadHistory(HistoryFile());
		_historyLoaded = true;
	}
	extras::AddToHistory(_history, extras::ComputeStat(*_save, _path));
	extras::SaveHistory(HistoryFile(), _history);
	_historyChar = _save->header.playerName;
}

void App::drawHistory()
{
	prepareWindow("Save history", ImVec2(0.8f, 0.8f));
	if (!ImGui::Begin("存档历史", &_showHistory, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Save history");
	if (!_historyLoaded) {
		_history = extras::LoadHistory(HistoryFile());
		_historyLoaded = true;
	}
	// results of a folder read
	if (!_historyBusy && _historyThread.joinable()) {
		_historyThread.join();
		std::lock_guard lk(_historyMutex);
		for (const auto& s : _historyIncoming) {
			extras::AddToHistory(_history, s);
		}
		_historyIncoming.clear();
		extras::SaveHistory(HistoryFile(), _history);
	}

	std::set<std::string> chars;
	for (const auto& s : _history) {
		chars.insert(s.character);
	}
	if (_historyChar.empty() && !chars.empty()) {
		_historyChar = *chars.begin();
	}
	ImGui::NewLine();
	ImGui::TextWrapped("ReSaver Plus 每打开一个存档都会在这里记一笔（只记数字，写在程序旁的 resaverpp-history.txt 里）。"
					   "连着看几个存档就能看出什么一直在涨：某个模组的脚本实例如果每存一次档都比上一次多，"
					   "通常说明它有失控的脚本，会让游戏变慢，也让存档越来越大。");
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
	if (ImGui::BeginCombo("角色", text::ToUtf8(_historyChar).c_str())) {
		for (const auto& c : chars) {
			if (ImGui::Selectable(text::ToUtf8(c).c_str(), c == _historyChar)) {
				_historyChar = c;
			}
		}
		ImGui::EndCombo();
	}
	tips::Tip("要显示哪个角色的存档。");
	ImGui::SameLine();
	ImGui::BeginDisabled(_historyBusy || _historyChar.empty());
	if (ImGui::Button("读取此角色的所有存档")) {
		const auto folder = !_path.empty() ? _path.parent_path() : savesFolder();
		const auto who = _historyChar;
		_historyBusy = true;
		_historyCancel = false;
		_historyDone = 0;
		_historyTotal = 0;
		if (_historyThread.joinable()) {
			_historyThread.join();
		}
		_historyThread = std::thread([this, folder, who] {
			std::vector<std::filesystem::path> files;
			for (const auto& i : extras::ListSaves(folder)) {
				if (i.ok && i.playerName == who) {
					files.push_back(i.path);
				}
			}
			_historyTotal = files.size();
			for (const auto& f : files) {
				if (_historyCancel) {
					break;
				}
				try {
					const auto save = SaveFile::load(f);
					auto       st = extras::ComputeStat(*save, f);
					std::lock_guard lk(_historyMutex);
					_historyIncoming.push_back(std::move(st));
				} catch (const std::exception&) {
				}
				++_historyDone;
			}
			_historyBusy = false;
		});
	}
	ImGui::EndDisabled();
	tips::Tip("在后台逐个打开存档文件夹里这个角色的每个存档并记下它们的数字，让历史一开始就是完整的，"
			  "而不是边玩边慢慢补。");
	if (_historyBusy) {
		ImGui::SameLine();
		ImGui::Text("正在读取 %zu / %zu 个存档…", _historyDone.load(), _historyTotal.load());
	}

	std::vector<const extras::SaveStat*> rows;
	for (const auto& s : _history) {
		if (s.character == _historyChar) {
			rows.push_back(&s);
		}
	}
	std::sort(rows.begin(), rows.end(), [](const auto* a, const auto* b) {
		return a->saveNumber != b->saveNumber ? a->saveNumber < b->saveNumber : a->modified < b->modified;
	});
	if (rows.empty()) {
		ImGui::TextDisabled("还没有记录这个角色的任何存档。");
		ImGui::End();
		return;
	}

	// what keeps growing
	std::vector<extras::SaveStat> ordered;
	for (const auto* r : rows) {
		ordered.push_back(*r);
	}
	const auto growth = extras::FindGrowth(ordered);
	ImGui::Spacing();
	if (rows.size() < 4) {
		ImGui::TextDisabled("增长按 4 个或更多存档来判断；目前只记录了 %zu 个。", rows.size());
	} else if (growth.empty()) {
		TextColored(theme::Good(), "没有一直在涨的：最近这些存档里，没有任何模组的脚本实例稳定增长。");
	} else {
		TextColored(theme::Warn(), "最近这些存档里一直在涨（该模组对象上的脚本实例）：");
		for (const auto& g : growth) {
			ImGui::BulletText("%s：%zu → %zu（%zu 个存档）", g.plugin == "Created" ? "生成对象（任意模组）" : g.plugin.c_str(), g.first, g.last,
				g.saves);
		}
		ImGui::TextDisabled("到“模组”视图里看看那个模组：某个脚本一直在创建实例或线程，通常就是原因。");
	}

	// small charts
	auto plot = [&](const char* a_label, auto a_value) {
		std::vector<float> v;
		for (const auto* r : rows) {
			v.push_back(static_cast<float>(a_value(*r)));
		}
		const float hi = *std::max_element(v.begin(), v.end());
		ImGui::PlotLines(a_label, v.data(), static_cast<int>(v.size()), 0, Fmt("最大 %.0f", hi).c_str(), 0.0f, hi * 1.1f + 1,
			ImVec2(ImGui::GetContentRegionAvail().x * 0.3f, ImGui::GetFontSize() * 4));
	};
	ImGui::Spacing();
	plot("##inst", [](const extras::SaveStat& s) { return s.instances; });
	ImGui::SameLine();
	plot("##created", [](const extras::SaveStat& s) { return s.created; });
	ImGui::SameLine();
	plot("##size", [](const extras::SaveStat& s) { return s.size / 1048576.0; });
	ImGui::TextDisabled("脚本实例（左）、生成对象（中）、存档大小（MB，右），最早的存档在前。");

	if (ImGui::BeginTable("hist", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		for (const char* h : { "存档", "日期", "大小", "脚本实例", "线程", "生成对象", "变更表单", "数组" }) {
			ImGui::TableSetupColumn(h);
		}
		ImGui::TableHeadersRow();
		const extras::SaveStat* prev = nullptr;
		for (const auto* r : rows) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("#%u", r->saveNumber);
			tips::Tipf("%s", text::ToUtf8(r->file).c_str());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(Date(r->modified).c_str());
			auto cell = [&](double a_now, double a_before, const char* a_fmt) {
				ImGui::TableNextColumn();
				const bool up = prev && a_now > a_before * 1.1 && a_now - a_before > 100;
				if (up) {
					ImGui::PushStyleColor(ImGuiCol_Text, theme::Warn());
				}
				ImGui::Text(a_fmt, a_now);
				if (up) {
					ImGui::PopStyleColor();
				}
			};
			cell(r->size / 1048576.0, prev ? prev->size / 1048576.0 : 0, "%.1f MB");
			cell(static_cast<double>(r->instances), prev ? static_cast<double>(prev->instances) : 0, "%.0f");
			cell(static_cast<double>(r->threads), prev ? static_cast<double>(prev->threads) : 0, "%.0f");
			cell(static_cast<double>(r->created), prev ? static_cast<double>(prev->created) : 0, "%.0f");
			cell(static_cast<double>(r->changeForms), prev ? static_cast<double>(prev->changeForms) : 0, "%.0f");
			cell(static_cast<double>(r->arrays), prev ? static_cast<double>(prev->arrays) : 0, "%.0f");
			prev = r;
		}
		ImGui::EndTable();
	}
	ImGui::End();
}

// ==========================================================================================
// Papyrus log
// ==========================================================================================

void App::drawPapyrusLog()
{
	prepareWindow("Papyrus log", ImVec2(0.8f, 0.8f));
	if (!ImGui::Begin("Papyrus 日志", &_showPapyrusLog, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Papyrus log");
	auto candidates = [&] {
		std::vector<std::filesystem::path> v;
		if (!_path.empty()) {
			v.push_back(_path.parent_path().parent_path() / "Logs" / "Script" / "Papyrus.0.log");
		}
		v.push_back(savesFolder().parent_path() / "Logs" / "Script" / "Papyrus.0.log");
		v.push_back(platform::DefaultSavesFolder(_vr).parent_path() / "Logs" / "Script" / "Papyrus.0.log");
		if (_save && _save->isLE()) {
			v.push_back(platform::DefaultSavesFolder(false).parent_path().parent_path() / "Skyrim" / "Logs" / "Script" / "Papyrus.0.log");
		}
		return v;
	};
	if (!_plogLoaded) {
		_plogLoaded = true;
		_plogOpen = -1;
		_plog = {};
		if (const char* t = std::getenv("RESAVERPP_TEST_PLOG")) {
			_plog = extras::ReadPapyrusLog(t);
		} else {
			for (const auto& c : candidates()) {
				std::error_code ec;
				if (std::filesystem::exists(c, ec)) {
					_plog = extras::ReadPapyrusLog(c);
					break;
				}
			}
		}
	}
	ImGui::NewLine();
	if (ImGui::Button("打开其他日志…")) {
		if (platform::HasNativeDialogs()) {
			if (auto f = platform::OpenFileDialog(candidates().front().parent_path(), "log", "Papyrus 日志")) {
				_plog = extras::ReadPapyrusLog(*f);
				_plogOpen = -1;
			}
		}
	}
	tips::Tip("自己挑一个 Papyrus 日志（Papyrus.1.log 是上一次游戏，Papyrus.2.log 是再上一次）。");
	ImGui::SameLine();
	if (ImGui::Button("重新读取")) {
		_plogLoaded = false;
	}
	tips::Tip("重新读取日志，例如又玩了一会儿之后。");
	if (!_plog.ok) {
		TextColored(theme::Warn(), "没有找到 Papyrus 日志。只有开启日志记录时游戏才会写：在 Skyrim.ini（或 SkyrimCustom.ini）"
								   "的 [Papyrus] 下把 bEnableLogging=1、bEnableTrace=1、bLoadDebugInformation=1 设好，玩一会儿，"
								   "再回来看。日志在 Documents\\My Games\\Skyrim Special Edition\\Logs\\Script\\Papyrus.0.log。");
		ImGui::End();
		return;
	}
	ImGui::TextWrapped("%s", platform::ToUtf8(_plog.file).c_str());
	ImGui::TextDisabled("%s", _plog.started.c_str());
	ImGui::Text("%zu 个错误、%zu 个警告，共 %zu 行，分属 %zu 组。", _plog.errors, _plog.warnings, _plog.lines, _plog.groups.size());
	ImGui::TextWrapped("同一个脚本反复刷同一条错误，通常说明某个模组坏了、缺前置，或者在已经消失的对象上运行。"
					   "点击一行可以看完整的错误和堆栈。");
	ImGui::Spacing();
	const auto* p = _save ? _save->papyrus() : nullptr;
	auto inSave = [&](const std::string& a_script) {
		std::size_t inst = 0, thr = 0;
		if (!p || a_script.empty()) {
			return std::pair<std::size_t, std::size_t>{ 0, 0 };
		}
		const auto low = Lower(a_script);
		for (const auto& i : p->instances) {
			inst += Lower(p->str(i.script)) == low ? 1 : 0;
		}
		for (const auto& a : p->activeScripts) {
			if (a.data) {
				for (const auto& f : a.data->frames) {
					if (Lower(p->str(f.scriptName)) == low) {
						++thr;
						break;
					}
				}
			}
		}
		return std::pair<std::size_t, std::size_t>{ inst, thr };
	};
	if (ImGui::BeginTable("plog", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("次数", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4);
		ImGui::TableSetupColumn("脚本", ImGuiTableColumnFlags_WidthStretch, 1.5f);
		ImGui::TableSetupColumn("模组", ImGuiTableColumnFlags_WidthStretch, 1.5f);
		ImGui::TableSetupColumn("本存档中", ImGuiTableColumnFlags_WidthStretch, 1.2f);
		ImGui::TableSetupColumn("错误", ImGuiTableColumnFlags_WidthStretch, 4.0f);
		ImGui::TableHeadersRow();
		const std::size_t shown = std::min<std::size_t>(_plog.groups.size(), 500);
		for (std::size_t i = 0; i < shown; ++i) {
			const auto& g = _plog.groups[i];
			ImGui::PushID(static_cast<int>(i));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("%zu", g.count);
			ImGui::TableNextColumn();
			if (ImGui::Selectable(g.script.empty() ? "（未命名）" : g.script.c_str(), _plogOpen == static_cast<int>(i),
					ImGuiSelectableFlags_SpanAllColumns)) {
				_plogOpen = _plogOpen == static_cast<int>(i) ? -1 : static_cast<int>(i);
			}
			tips::Tip("点击可以看完整的错误和它的堆栈。");
			ImGui::TableNextColumn();
			std::string mod;
			if (!g.script.empty()) {
				if (const auto* prov = _sources.providers(g.script); prov && !prov->empty()) {
					mod = prov->front().mod;
				}
			}
			ImGui::TextUnformatted(mod.empty() ? "-" : mod.c_str());
			ImGui::TableNextColumn();
			if (!g.script.empty() && p) {
				const auto [inst, thr] = inSave(g.script);
				if (inst || thr) {
					const bool go = ImGui::SmallButton("显示");
					tips::Tip("在主窗口里显示这个脚本的实例（或线程）。");
					ImGui::SameLine();
					ImGui::Text("%zu 个实例，%zu 个线程", inst, thr);
					if (go) {
						_cat = thr ? Cat::kThreads : Cat::kInstances;
						std::snprintf(_filter, sizeof(_filter), "%s", g.script.c_str());
						_rowsDirty = true;
						_focusFirstRow = true;
					}
				} else {
					ImGui::TextDisabled("不在本存档中");
				}
			}
			ImGui::TableNextColumn();
			ImGui::TextWrapped("%s", g.message.c_str());
			if (_plogOpen == static_cast<int>(i)) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TableNextColumn();
				ImGui::TableSetColumnIndex(4);
				ImGui::PushTextWrapPos(0);
				ImGui::TextDisabled("%s", g.example.c_str());
				ImGui::PopTextWrapPos();
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::End();
}

// ==========================================================================================
// saves folder manager
// ==========================================================================================

void App::drawSavesManager()
{
	prepareWindow("Manage saves folder", ImVec2(0.75f, 0.8f));
	if (!ImGui::Begin("管理存档文件夹", &_showSavesMgr, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Manage saves folder");
	const auto folder = !_path.empty() ? _path.parent_path() : savesFolder();
	if (!_savesLoaded) {
		_savesLoaded = true;
		_saves = extras::ListSaves(folder);
		_saveSel.assign(_saves.size(), 0);
	}
	ImGui::NewLine();
	ImGui::TextWrapped("存档文件夹：%s", platform::ToUtf8(folder).c_str());
	ImGui::TextWrapped("挑选要删除的旧存档。每个存档的 .skse 文件会跟着一起走。删掉的存档进回收站，所以还能找回来。"
					   "这里正打开的这个存档永远不会被删除。");
	std::uintmax_t total = 0;
	std::map<std::string, std::vector<std::size_t>> byChar;  // newest first
	for (std::size_t i = 0; i < _saves.size(); ++i) {
		total += _saves[i].size + _saves[i].cosaveSize;
		byChar[_saves[i].ok ? _saves[i].playerName : std::string("（读不出来）")].push_back(i);
	}
	ImGui::Text(total >= 1073741824 ? "%zu 个存档，%.1f GB" : "%zu 个存档，%.0f MB", _saves.size(), total >= 1073741824 ? total / 1073741824.0 : total / 1048576.0);
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
	ImGui::InputInt("##keep", &_keepNewest);
	_keepNewest = std::clamp(_keepNewest, 0, 9999);
	tips::Tip("每个角色最新的多少个存档要保留。");
	ImGui::SameLine();
	if (ImGui::Button(Fmt("勾选除每个角色最新 %d 个之外的全部", _keepNewest).c_str())) {
		for (const auto& [c, idx] : byChar) {
			for (std::size_t k = 0; k < idx.size(); ++k) {
				_saveSel[idx[k]] = k >= static_cast<std::size_t>(_keepNewest) ? 1 : 0;
			}
		}
	}
	tips::Tip("勾选除每个角色最新几个之外的所有存档。在你按下“删除”之前不会删掉任何东西。");
	ImGui::SameLine();
	if (ImGui::Button("全不选")) {
		std::fill(_saveSel.begin(), _saveSel.end(), 0);
	}
	tips::Tip("清空所有勾选。");
	ImGui::SameLine();
	if (ImGui::Button("重新读取文件夹")) {
		_savesLoaded = false;
	}
	tips::Tip("重新列出存档（游戏写了新存档之后）。");

	std::size_t    ticked = 0;
	std::uintmax_t tickedBytes = 0;
	for (std::size_t i = 0; i < _saves.size(); ++i) {
		if (_saveSel[i]) {
			++ticked;
			tickedBytes += _saves[i].size + _saves[i].cosaveSize;
		}
	}
	if (ImGui::BeginChild("savelist", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.6f), ImGuiChildFlags_Borders)) {
		for (auto& [c, idx] : byChar) {
			std::size_t sel = 0;
			for (auto i : idx) {
				sel += _saveSel[i] ? 1 : 0;
			}
			const bool open = ImGui::CollapsingHeader(Fmt("%s（%zu 个存档，已勾选 %zu 个）###%s", text::ToUtf8(c).c_str(), idx.size(), sel, c.c_str()).c_str(),
				byChar.size() == 1 ? ImGuiTreeNodeFlags_DefaultOpen : 0);
			tips::Tip("这个角色的存档，最新的在前。");
			if (!open) {
				continue;
			}
			if (ImGui::BeginTable(c.c_str(), 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
				for (auto i : idx) {
					const auto& s = _saves[i];
					const bool  isOpen = !_path.empty() && std::filesystem::equivalent(s.path, _path);
					ImGui::PushID(static_cast<int>(i));
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					bool b = _saveSel[i] != 0;
					ImGui::BeginDisabled(isOpen);
					if (ImGui::Checkbox("##s", &b)) {
						_saveSel[i] = b ? 1 : 0;
					}
					ImGui::EndDisabled();
					if (isOpen) {
						_saveSel[i] = 0;
					}
					ImGui::SameLine();
					ImGui::TextUnformatted(text::ToUtf8(s.path.filename().string()).c_str());
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(Date(s.modified).c_str());
					ImGui::TableNextColumn();
					ImGui::Text("等级 %u", s.playerLevel);
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(text::ToUtf8(s.location).c_str());
					ImGui::TableNextColumn();
					ImGui::Text("%.1f MB%s", (s.size + s.cosaveSize) / 1048576.0, isOpen ? "  （正在打开）" : "");
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
		}
	}
	ImGui::EndChild();
	ImGui::BeginDisabled(ticked == 0);
	if (ImGui::Button(Fmt("把勾选的 %zu 个存档（%.1f MB）移到回收站", ticked, tickedBytes / 1048576.0).c_str())) {
		confirm("删除存档", Fmt("把 %zu 个存档及其 .skse 文件移到回收站？", ticked), [this] {
			std::vector<std::filesystem::path> files;
			for (std::size_t i = 0; i < _saves.size(); ++i) {
				if (!_saveSel[i]) {
					continue;
				}
				files.push_back(_saves[i].path);
				auto co = _saves[i].path;
				co.replace_extension(".skse");
				std::error_code ec;
				if (std::filesystem::exists(co, ec)) {
					files.push_back(co);
				}
			}
			if (platform::MoveToRecycleBin(files)) {
				_status = Fmt("已把 %zu 个文件移到回收站", files.size());
			} else {
				_error = "有些存档删不掉（正被占用，或者没有权限）。列表里显示的是剩下的。";
			}
			_savesLoaded = false;
		});
	}
	ImGui::EndDisabled();
	tips::Tip("把勾选的存档及其 .skse 文件移到回收站。");
	ImGui::End();
}
