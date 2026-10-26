// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// 本地化修改：界面、提示与报告文字改为简体中文（详见 README.zh-CN.md）。
// Mods that are still in a save but no longer in the player's load order: listed when the save is
// opened, and their leftovers removed after the player confirms (each through the same safety checks
// as "Remove mod from save"), into a new save file.
#include "App.h"

#include "Platform.h"
#include "Theme.h"

#include "core/LoadOrder.h"
#include "core/ModRemove.h"
#include "core/Ops.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>
#include <unordered_map>
#include <unordered_set>

using namespace ess;

namespace
{

	std::string Lower(std::string s)
	{
		for (auto& c : s) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return s;
	}

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
}

void App::checkMissingPlugins()
{
	_missing.clear();
	_missingSource.clear();
	_missingScripts.clear();
	_unfoundScripts.clear();
	_removeUnfoundScripts = false;
	_removeMissingScripts = true;
	_orderSuspect = _scanSuspect = false;
	_activeInSave = _activeFound = _activeChecked = 0;
	_missingFile.clear();
	if (!_save) {
		return;
	}
	_installKnown = _sources.scanned() && !scanning();
	// the load order the game uses next: the Mod Organizer profile's, or the game's own plugins.txt
	std::filesystem::path txt;
	if (_mo2 && !_profile.empty()) {
		txt = _mo2->profilesDir / platform::FromUtf8(_profile) / "plugins.txt";
		_missingSource = "Mod Organizer 配置 \"" + _profile + "\"";
	} else {
		txt = loadorder::GamePluginsTxt(_vr);
		_missingSource = _vortex ? "你的加载顺序（Vortex）" : "你的加载顺序";
	}
	const auto active = loadorder::Read(txt, gameFolder(), _vr);
	// scripts whose compiled file is nowhere: not in any mod (enabled or not) and not in the game's own
	// archives. Only when the game's Data folder was scanned too, or every vanilla script would count.
	//  - "missing": the save itself says the game could not load it either (it writes an empty parent
	//    type for those). Certain: removed by default.
	//  - "not found": the game loaded it when it saved, but the scan did not find its file. Either the
	//    mod was uninstalled since, or the scan looked in the wrong place (or missed an archive): not
	//    removed unless the player asks. Not listed at all when it runs on objects of a plugin that is
	//    in the load order (that mod is installed, so its scripts are too).
	if (_installKnown && _sources.gameDataScanned) {
		const auto& p = *_save->papyrus();
		std::unordered_map<std::string, std::size_t> uses;
		std::unordered_set<std::string>              onLoaded;  // scripts with an instance on an object of a loaded plugin
		for (const auto& i : p.instances) {
			const auto key = Lower(p.str(i.script));
			++uses[key];
			if (active.known && !i.refID.isZero() && i.refID.type() != RefID::Type::kCreated) {
				const auto plugin = Lower(_save->pluginOf(_save->formIDOf(i.refID)));
				if (!plugin.empty() && active.plugins.contains(plugin)) {
					onLoaded.insert(key);
				}
			}
		}
		for (const auto& a : p.activeScripts) {
			if (a.data && !a.data->frames.empty()) {
				++uses[Lower(p.str(a.data->frames.front().scriptName))];
			}
		}
		// a script some installed script extends exists somewhere (the game could not load the child
		// without it), even if the scan did not find its file: not listed
		std::unordered_set<std::string> parentsOfInstalled;
		for (const auto& sc : p.scripts) {
			if (!p.str(sc.parent).empty() && _sources.providers(p.str(sc.name))) {
				parentsOfInstalled.insert(Lower(p.str(sc.parent)));
			}
		}
		for (const auto& sc : p.scripts) {
			const auto name = p.str(sc.name);
			const auto key = Lower(name);
			if (name.empty() || ops::IsEngineType(name) || _sources.providers(name) || parentsOfInstalled.contains(key)) {
				continue;
			}
			if (ops::IsScriptUndefined(p, sc)) {
				_missingScripts.emplace_back(name, uses[key]);
			} else if (!onLoaded.contains(key)) {
				_unfoundScripts.emplace_back(name, uses[key]);
			}
		}
		std::sort(_missingScripts.begin(), _missingScripts.end());
		std::sort(_unfoundScripts.begin(), _unfoundScripts.end());
	}
	// archives or folders the scan could not read may hold scripts: "not found" is not proof then
	if (_installKnown && !_sources.errors.empty()) {
		_removeMissingScripts = false;
	}

	if (!active.known) {
		_missingSource = "你的加载顺序";
		return;  // no load order to compare plugins with (missing scripts can still be listed)
	}
	_missingFile = active.file;
	std::vector<std::string> all = _save->plugins.full;
	all.insert(all.end(), _save->plugins.lite.begin(), _save->plugins.lite.end());
	// Sanity: a save made with this load order has nearly all of its plugins in it, and a scan of the right
	// mods folder finds nearly every plugin the load order enables. Far off means the wrong profile, a stale
	// plugins.txt or the wrong mods folder: then nothing is ticked.
	for (const auto& p : all) {
		const auto low = Lower(p);
		if (!active.plugins.contains(low)) {
			continue;
		}
		++_activeInSave;
		const bool gameFile = (low.rfind("cc", 0) == 0 && low.find("sse") != std::string::npos) || low == "skyrim.esm" || low == "update.esm" || low == "dawnguard.esm" ||
							  low == "hearthfires.esm" || low == "dragonborn.esm" || low == "_resourcepack.esl" || low == "skyrimvr.esm";
		if (_installKnown && !gameFile) {
			++_activeChecked;
			_activeFound += _sources.pluginPath(p).has_value() ? 1 : 0;
		}
	}
	_orderSuspect = all.size() >= 10 && _activeInSave * 2 < all.size();
	_scanSuspect = _activeChecked >= 10 && _activeFound * 2 < _activeChecked;
	if (_orderSuspect || _scanSuspect) {
		_removeMissingScripts = false;
	}
	std::unordered_map<std::string, std::size_t> index;
	for (const auto& p : all) {
		if (active.plugins.contains(Lower(p))) {
			continue;
		}
		MissingMod m;
		m.plugin = p;
		m.installed = !_installKnown || _sources.pluginPath(p).has_value();  // unknown: treat as installed (not ticked)
		if (auto it = _sources.pluginFolder.find(Lower(p)); it != _sources.pluginFolder.end()) {
			m.folder = it->second;
		}
		m.selected = !m.installed;  // a disabled mod may come back: not ticked by default
		index[Lower(p)] = _missing.size();
		_missing.push_back(std::move(m));
	}
	if (_missing.empty()) {
		return;
	}
	// what is left of each in the save
	for (const auto& c : _save->changeForms) {
		const auto plugin = Lower(_save->pluginOf(_save->formIDOf(c.refID)));
		if (auto it = index.find(plugin); it != index.end()) {
			++_missing[it->second].changeForms;
		}
	}
	ensureMods();
	if (_mods) {
		for (const auto& r : _mods->rows) {
			if (auto it = index.find(Lower(r.name)); it != index.end()) {
				auto& m = _missing[it->second];
				m.scripts = r.scriptCount;
				m.instances = r.instanceCount;
				m.threads = r.threadCount;
			}
		}
	}
	// nothing left of it: nothing to clean, the game already dropped it
	for (auto& m : _missing) {
		if (m.changeForms == 0 && m.scripts == 0 && m.threads == 0) {
			m.selected = false;
		}
		if (_orderSuspect || _scanSuspect) {
			m.selected = false;
		}
	}
	if (_orderSuspect || _scanSuspect) {
		_removeMissingScripts = false;  // nothing destructive is ticked when the setup does not match the save
	}
}

void App::drawMissingBanner()
{
	if ((_missing.empty() && _missingScripts.empty() && _unfoundScripts.empty()) || _missingDismissed) {
		return;
	}
	std::size_t gone = 0, disabled = 0;
	for (const auto& m : _missing) {
		(m.installed ? disabled : gone) += 1;
	}
	ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::BannerBg());
	ImGui::BeginChild("##missing", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * (scanning() ? 4.6f : 3.2f)), ImGuiChildFlags_Borders);
	std::string msg;
	if (!_missing.empty()) {
		msg = _installKnown ? Fmt("本存档中有 %zu 个模组不在 %s 中：其中 %zu 个已不再安装，%zu 个已安装但未启用。", _missing.size(),
								  _missingSource.c_str(), gone, disabled)
							: Fmt("本存档中有 %zu 个模组不在 %s 中。", _missing.size(), _missingSource.c_str());
	}
	if (!_missingScripts.empty()) {
		msg += (msg.empty() ? "" : " ") + Fmt("其中 %zu 个脚本已不再安装于任何位置。", _missingScripts.size());
	}
	if (!_unfoundScripts.empty()) {
		msg += (msg.empty() ? "" : " ") + Fmt("%zu 个脚本没在你的模组里找到（做这个存档时游戏仍在加载它们）。", _unfoundScripts.size());
	}
	if (!_installKnown && scanning()) {
		msg += "  正在检查哪些仍然安装着（正在扫描你的模组）…";
	}
	if (_orderSuspect || _scanSuspect) {
		msg += "  看起来不对（见复查）：没有勾选任何项。";
	}
	TextColored(theme::Warn(), msg);
	if (scanning()) {
		drawBackgroundProgress(0);
	}
	if (ImGui::Button("查看并移除残留…")) {
		for (auto& m : _missing) {
			m.planned = false;
		}
		_missingOpen = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("隐藏")) {
		_missingDismissed = true;
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("隐藏到下次打开存档为止。清理 > 已不再安装的模组与脚本… 可以让它重新出现。");
	}
	ImGui::EndChild();
	ImGui::PopStyleColor();
}

void App::drawMissingModal()
{
	if (_missingOpenWhenReady && !scanning() && (!_missing.empty() || !_missingScripts.empty() || !_unfoundScripts.empty())) {
		_missingOpenWhenReady = false;
		_missingOpen = true;  // (test hook)
	}
	if (_missingOpen) {
		ImGui::OpenPopup("已不在加载顺序中的模组");
		_missingOpen = false;
	}
	ImGui::SetNextWindowSize(ImVec2(980, 640), ImGuiCond_Appearing);
	if (!ImGui::BeginPopupModal("已不在加载顺序中的模组")) {
		return;
	}
	ImGui::TextWrapped("这些插件记录在存档中，但未在 %s 中启用。游戏不会加载它们，因此它们的脚本 "
					   "和存档数据都是残留。移除它们会像“从存档移除模组”那样清理存档，经过 "
					   "同样的安全检查，并写入一个新的存档文件：原文件不会被改动。",
		_missingSource.c_str());
	TextColored(theme::Dim(), "只为测试而临时禁用、或在另一个配置中启用的模组，之后还会需要这些数据：请不要勾选。");
	if (!_installKnown && scanning()) {
		TextColored(theme::Warn(), "仍在扫描你的模组：扫描完成前 ReSaver++ 无法区分已卸载的模组和已禁用的模组，因此没有 "
								   "任何一项被勾选。扫描完成后列表会自动更新。");
	} else if (!_installKnown) {
		TextColored(theme::Warn(), "尚未设置模组文件夹（选项 > 设置），因此 ReSaver++ 无法区分已卸载的模组和已禁用的模组： "
						   "没有勾选任何一项。");
	}
	TextColored(theme::Dim(), "加载顺序读取自：" + platform::ToUtf8(_missingFile) +
								  (_installKnown ? "    模组文件夹：" + _sourcesPath : std::string()));
	if (_orderSuspect) {
		TextColored(theme::Bad(), Fmt("这个加载顺序和存档对不上：存档的 %zu 个插件里只有 %zu 个在其中。多半是选错了文件"
									  "（别的配置，或旧的 plugins.txt）。没有勾选任何项。请在模组视图里选对 MO2 配置，或检查 "
									  "选项 > 设置。",
									  _activeInSave, _save ? _save->plugins.full.size() + _save->plugins.lite.size() : 0));
	}
	if (_installKnown && !_sources.errors.empty()) {
		std::string list;
		for (std::size_t i = 0; i < _sources.errors.size() && i < 6; ++i) {
			list += "\n    " + _sources.errors[i];
		}
		if (_sources.errors.size() > 6) {
			list += Fmt("\n    … 还有 %zu 个", _sources.errors.size() - 6);
		}
		TextColored(theme::Warn(), Fmt("%zu 个归档或文件夹读不出来，所以“没找到”的脚本可能就在其中一个里。"
									   "缺失的脚本没有勾选：",
									   _sources.errors.size()) +
									   list);
	}
	if (_scanSuspect) {
		TextColored(theme::Bad(), Fmt("模组扫描只找到 %zu 个，而你的加载顺序启用了 %zu 个插件，所以多半是扫错了文件夹。"
									  "没有勾选任何项：请在 选项 > 设置 里检查模组文件夹。",
									  _activeFound, _activeChecked));
	}
	ImGui::Separator();

	ImGui::BeginChild("rows", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.6f));
	if (ImGui::BeginTable("missing", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("插件", 0, 3);
		ImGui::TableSetupColumn("状态", 0, 3);
		ImGui::TableSetupColumn("存档中的残留", 0, 3);
		ImGui::TableSetupColumn("安全检查", 0, 4);
		ImGui::TableHeadersRow();
		for (std::size_t i = 0; i < _missing.size(); ++i) {
			auto& m = _missing[i];
			// the safety check of each ticked mod, on the unchanged save
			if (m.selected && !m.planned) {
				const auto plan = modremove::MakePlan(*_save, m.plugin, _sources.scanned() ? &_sources : nullptr, _removeOpt);
				m.planned = true;
				m.blocked = !plan.canApply();
				m.planText = plan.summary();
			}
			ImGui::TableNextRow();
			ImGui::PushID(static_cast<int>(i));
			ImGui::TableSetColumnIndex(0);
			const bool nothing = m.changeForms == 0 && m.scripts == 0 && m.threads == 0;
			ImGui::BeginDisabled(nothing);
			ImGui::Checkbox(m.plugin.c_str(), &m.selected);
			ImGui::EndDisabled();
			ImGui::TableSetColumnIndex(1);
			if (m.installed) {
				TextColored(theme::Warn(), "已安装但未启用" + (m.folder.empty() ? std::string() : "（" + m.folder + "）"));
			} else {
				ImGui::TextUnformatted("未安装");
			}
			ImGui::TableSetColumnIndex(2);
			if (nothing) {
				ImGui::TextDisabled("无（已经干净）");
			} else {
				ImGui::Text("%zu 个变更表单，%zu 个脚本，%zu 个脚本实例，%zu 个线程", m.changeForms, m.scripts, m.instances, m.threads);
			}
			ImGui::TableSetColumnIndex(3);
			if (!m.selected) {
				ImGui::TextDisabled("-");
			} else if (m.blocked) {
				TextColored(theme::Bad(), "已拒绝（见详情）");
			} else {
				ImGui::TextUnformatted("OK");
			}
			if (m.planned && m.selected && ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", m.planText.c_str());
			}
			if (m.selected && m.planned) {
				ImGui::SameLine();
				if (ImGui::SmallButton("详情")) {
					ImGui::OpenPopup("plan");
				}
				if (ImGui::BeginPopup("plan")) {
					ImGui::PushTextWrapPos(ImGui::GetFontSize() * 45);
					ImGui::TextUnformatted(m.planText.c_str());
					ImGui::PopTextWrapPos();
					ImGui::EndPopup();
				}
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	// scripts whose files are gone (a script-only mod, or a mod's scripts on other mods' objects)
	if (!_missingScripts.empty()) {
		ImGui::Checkbox(Fmt("同时移除在任何地方都已不再安装的 %zu 个脚本（以及它们的线程、排队调用和实例）",
							_missingScripts.size())
							.c_str(),
			&_removeMissingScripts);
		if (ImGui::TreeNode("missing scripts", "显示")) {
			for (const auto& [name, n] : _missingScripts) {
				ImGui::BulletText("%s  （%zu 个仍在使用）", name.c_str(), n);
			}
			ImGui::TreePop();
		}
		TextColored(theme::Dim(), "在任何模组文件夹（无论是否启用）以及游戏自带的归档中都找不到；存档本身也显示游戏当时没能加载它们。"
								  "游戏已经无法运行它们。");
	}
	// the game had these when it saved: only the scan says they are gone, so they are not ticked
	if (!_unfoundScripts.empty()) {
		ImGui::Spacing();
		TextColored(theme::Warn(), Fmt("%zu 个脚本在你的模组里没找到，但游戏做这个存档时仍然加载了它们。", _unfoundScripts.size()));
		TextColored(theme::Dim(), Fmt("可能是做这个存档之后它们的模组被卸载了，也可能是 ReSaver++ 找错了位置。已扫描的模组文件夹："
									  "%s（%zu 个模组文件夹，%zu 个脚本）。如果这些模组仍然安装着，请不要移除它们：先到 选项 > 设置 "
									  "核对模组文件夹。移除模组仍在使用的脚本，会让它的任务和 MCM 重新开始。",
							  _sourcesPath.c_str(), _sources.modsScanned, _sources.pexFound));
		ImGui::Checkbox(Fmt("同时移除这 %zu 个脚本（仅当你卸载了它们的模组）", _unfoundScripts.size()).c_str(), &_removeUnfoundScripts);
		if (ImGui::TreeNode("unfound scripts", "显示")) {
			for (const auto& [name, n] : _unfoundScripts) {
				ImGui::BulletText("%s  （%zu 个仍在使用）", name.c_str(), n);
			}
			ImGui::TreePop();
		}
	}
	ImGui::EndChild();

	std::size_t ticked = 0, refused = 0;
	for (const auto& m : _missing) {
		ticked += m.selected ? 1 : 0;
		refused += m.selected && m.blocked ? 1 : 0;
	}
	auto target = _path;
	{
		target = EditedSaveName(_path, "cleaned");
	}
	ImGui::TextDisabled("新文件：%s%s", platform::ToUtf8(target.filename()).c_str(),
		refused ? Fmt("   （%zu 个被安全检查拒绝，将被跳过）", refused).c_str() : "");
	std::vector<std::pair<std::string, std::size_t>> scriptList;
	if (_removeMissingScripts) {
		scriptList = _missingScripts;
	}
	if (_removeUnfoundScripts) {
		scriptList.insert(scriptList.end(), _unfoundScripts.begin(), _unfoundScripts.end());
	}
	const bool scriptsToo = !scriptList.empty();
	ImGui::BeginDisabled(ticked == refused && !scriptsToo);
	if (ImGui::Button("移除并写入新存档", ImVec2(280, 0))) {
		// one after the other: each plan is made on the save as the previous removal left it
		std::size_t done = 0;
		std::string skipped;
		for (const auto& m : _missing) {
			if (!m.selected) {
				continue;
			}
			const auto plan = modremove::MakePlan(*_save, m.plugin, _sources.scanned() ? &_sources : nullptr, _removeOpt);
			if (!plan.canApply()) {
				skipped += (skipped.empty() ? "" : ", ") + m.plugin;
				continue;
			}
			modremove::Apply(*_save, plan);
			++done;
		}
		std::size_t scriptsDone = 0;
		if (scriptsToo) {
			auto& pap = *_save->papyrus();
			std::set<TStr> defs;
			for (const auto& [name, n] : scriptList) {
				// spawned objects it sat on stay: another (installed) script may be on the same object
				ops::PurgeScript(*_save, name, false);
				++scriptsDone;
				if (const auto t = pap.findString(name); t != UINT32_MAX) {
					defs.insert(t);
				}
			}
			// the script definitions too (PurgeScript leaves them), or the save keeps listing the scripts
			// and they show up again every time. One another remaining script extends stays.
			for (const auto& sc : pap.scripts) {
				if (!defs.contains(sc.name)) {
					defs.erase(sc.parent);
				}
			}
			ops::DeleteScripts(pap, defs);
		}
		ImGui::CloseCurrentPopup();
		if (done || scriptsDone) {
			markEdited(Fmt("已移除 %zu 个模组的残留和 %zu 个缺失脚本", done, scriptsDone));
			_modsDirty = true;
			doSave(target);
			_report = std::make_unique<ops::Report>(ops::Analyze(*_save));
			if (_error.empty()) {
				_status = Fmt("已移除 %zu 个模组的残留和 %zu 个缺失脚本，并写入 ", done, scriptsDone) + platform::ToUtf8(target.filename()) +
						  (skipped.empty() ? "" : "  （已跳过：" + skipped + "）");
			}
			checkMissingPlugins();
		}
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("取消", ImVec2(120, 0))) {
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}
