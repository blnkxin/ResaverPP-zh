// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// 本地化修改：界面、提示与报告文字改为简体中文（详见 README.zh-CN.md）。
// App features: plugin names, form lookup, decoded change forms, Reset Havok / Purify
// FormLists, batch clean, compare, reports and the save-folder watcher.
#include "App.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "imgui.h"

#include <cstdlib>

#include <thread>

#include <chrono>

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <unordered_map>

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
}

App::~App()
{
	if (_dbProgress) {
		_dbProgress->cancel = true;
	}
	if (_scanProgress) {
		_scanProgress->cancel = true;
	}
	_historyCancel = true;
	if (_mergeProgress) {
		_mergeProgress->cancel = true;
	}
	// Background work only reads files. Give it a moment to stop, then leave without it: inside Mod
	// Organizer reading one large archive can take long, and MO2 stays locked while this process exists.
	auto busy = [&] {
		return (_dbThread.joinable() && !_dbDone) || (_scanThread.joinable() && !_scanDone) || scriptJobsBusy() || _historyBusy || _mergeBusy;
	};
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (busy() && std::chrono::steady_clock::now() < until) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	if (busy()) {
		std::fflush(nullptr);
		std::_Exit(0);
	}
	if (_dbThread.joinable()) {
		_dbThread.join();
	}
	stopScriptJobs();
	if (_scanThread.joinable()) {
		_scanThread.join();
	}
	if (_historyThread.joinable()) {
		_historyThread.join();
	}
	if (_mergeThread.joinable()) {
		_mergeThread.join();
	}
}

// ==========================================================================================
// plugin names
// ==========================================================================================

std::string App::formText(std::uint32_t a_formID) const
{
	std::string s = Fmt("%08X", a_formID);
	if (_db) {
		const auto d = _db->describe(a_formID);
		if (!d.empty()) {
			s += "  " + d;
		}
	}
	return s;
}

std::string App::gameLanguage() const
{
	// sLanguage from the MO2 profile's Skyrim.ini, else English
	if (_mo2 && !_profile.empty()) {
		for (const char* ini : { "skyrim.ini", "Skyrim.ini" }) {
			std::ifstream f(_mo2->profilesDir / platform::FromUtf8(_profile) / ini);
			for (std::string line; std::getline(f, line);) {
				const auto low = Lower(line);
				if (low.rfind("slanguage=", 0) == 0) {
					auto v = Lower(line.substr(10));
					while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) {
						v.pop_back();
					}
					if (!v.empty()) {
						return v;
					}
				}
			}
		}
	}
	return "english";
}

void App::startNames(bool a_force)
{
	if (!_save || !_sources.scanned()) {
		return;
	}
	const auto key = platform::ToUtf8(_path) + "|" + _sourcesPath + "|" + _profile;
	if (!a_force && key == _dbKey) {
		return;  // already loaded / loading for this save
	}
	if (_dbProgress) {
		_dbProgress->cancel = true;
	}
	if (_dbThread.joinable()) {
		_dbThread.join();
	}
	_dbKey = key;
	_dbDone = false;
	_dbProgress = std::make_unique<PluginDB::Progress>();
	_dbBuilding = std::make_unique<PluginDB>();
	auto in = std::make_shared<PluginDB::Input>();
	PrepareInput(*_save, *in);
	in->sources = _sources;
	in->language = gameLanguage();
	_dbThread = std::thread([this, in] {
		_dbBuilding->load(*in, *_dbProgress);
		_dbDone = true;
	});
}

void App::pollNames()
{
	if (_dbThread.joinable() && _dbDone) {
		_dbThread.join();
		if (!_dbProgress->cancel) {
			_db = std::move(_dbBuilding);
			_rowsDirty = true;
			_status = Fmt("已读取 %zu/%zu 个插件：%zu 个已命名表单%s", _db->pluginsRead, _db->plugins().size(), _db->size(),
				_db->missing.empty() ? "" : Fmt("（%zu 个插件未找到）", _db->missing.size()).c_str());
		}
		_dbBuilding.reset();
	}
}

bool App::namesLoading() const { return _dbThread.joinable() && !_dbDone; }

// ==========================================================================================
// form lookup (ReSaver: Lookup ID by name / Lookup base object)
// ==========================================================================================

void App::drawLookup()
{
	prepareWindow("Find form", ImVec2(0.7f, 0.75f));
	if (!ImGui::Begin("查找表单", &_showLookup, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Find form");
	if (!_db) {
		if (namesLoading()) {
			ImGui::TextUnformatted("正在读取插件…");
		} else {
			ImGui::TextWrapped("插件名尚未加载。请在“模组”视图中使用一次 \"查找脚本源码（.pex）…\"（选择你的 MO2 模组 "
							   "文件夹）；之后插件会被自动读取。");
		}
		ImGui::End();
		return;
	}
	ImGui::TextDisabled("Editor ID、名称或十六进制表单 ID。已放置的引用会在 \"->\" 之后显示其基础对象。");
	ImGui::SetNextItemWidth(-120);
	bool go = ImGui::InputTextWithHint("##look", "例如 Nazeem、WhiterunGuard、00013BBF", _lookupBuf, sizeof(_lookupBuf), ImGuiInputTextFlags_EnterReturnsTrue);
	tips::Tip("输入编辑器 ID 的一部分（例如 WhiterunGuard）、游戏里显示的名字（例如 Nazeem）或十六进制表单 ID，然后按回车。会搜索这个存档加载顺序里的所有插件。");
	ImGui::SameLine();
	go |= ImGui::Button("搜索", ImVec2(110, 0));
	tips::Tip("在插件里搜索匹配这段文字的表单。每个结果都会显示存档是否改动过它、上面挂了几个脚本。");
	if (_lookupAuto) {
		go = true;
		_lookupAuto = false;
	}
	if (go) {
		_lookupResults = _db->search(_lookupBuf, 2000);
		// where each result appears in the save (one pass over the save)
		_lookupInfo.clear();
		std::unordered_map<std::uint32_t, std::size_t> idx;
		for (std::size_t i = 0; i < _lookupResults.size(); ++i) {
			idx.emplace(_lookupResults[i], i);
		}
		_lookupInfo.resize(_lookupResults.size());
		for (const auto& c : _save->changeForms) {
			if (auto it = idx.find(_save->formIDOf(c.refID)); it != idx.end()) {
				_lookupInfo[it->second].cfRaw = c.refID.data;
				_lookupInfo[it->second].hasCf = true;
			}
		}
		for (const auto& i : _save->papyrus()->instances) {
			if (auto it = idx.find(_save->formIDOf(i.refID)); it != idx.end()) {
				auto& li = _lookupInfo[it->second];
				if (!li.scripts++) {
					li.firstInstance = i.id;
				}
			}
		}
	}
	ImGui::Text("%zu 个结果%s", _lookupResults.size(), _lookupResults.size() >= 2000 ? "（仅显示前 2000 个）" : "");
	if (ImGui::BeginTable("look", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("表单 ID", ImGuiTableColumnFlags_WidthFixed, 80);
		ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, 45);
		ImGui::TableSetupColumn("Editor ID / 名称", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("最后定义者", ImGuiTableColumnFlags_WidthFixed, 200);
		ImGui::TableSetupColumn("在存档中", ImGuiTableColumnFlags_WidthFixed, 140);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 120);
		ImGui::TableHeadersRow();
		ImGuiListClipper clip;
		clip.Begin(static_cast<int>(_lookupResults.size()));
		while (clip.Step()) {
			for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
				const auto  id = _lookupResults[row];
				const auto* f = _db->find(id);
				if (!f) {
					continue;
				}
				ImGui::TableNextRow();
				ImGui::PushID(row);
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%08X", id);
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(_db->type(*f).c_str());
				ImGui::TableSetColumnIndex(2);
				ImGui::TextUnformatted(_db->describe(id).c_str());
				ImGui::TableSetColumnIndex(3);
				ImGui::TextUnformatted(_db->pluginName(*f).c_str());
				const auto& li = _lookupInfo[row];
				ImGui::TableSetColumnIndex(4);
				ImGui::TextUnformatted(((li.hasCf ? std::string("已变更") : std::string("-")) + (li.scripts ? Fmt(", %zu 个脚本", li.scripts) : std::string())).c_str());
				ImGui::TableSetColumnIndex(5);
				if (li.hasCf && ImGui::SmallButton("表单")) {
					select(Cat::kChangeForms, li.cfRaw);
				}
				if (li.hasCf) {
					tips::Tip("打开变更表单：游戏保存的关于此表单的一切（位置、物品栏、AI、标志……）。");
				}
				if (li.scripts) {
					ImGui::SameLine();
					if (ImGui::SmallButton("脚本")) {
						select(Cat::kInstances, li.firstInstance);
						std::snprintf(_filter, sizeof(_filter), "%08X", id);
						_rowsDirty = true;
					}
					tips::Tip("显示挂在此表单上的脚本实例（“脚本实例”列表，已按此表单过滤）。");
				}
				ImGui::PopID();
			}
		}
		ImGui::EndTable();
	}
	ImGui::End();
}

// ==========================================================================================
// decoded change forms
// ==========================================================================================

void App::drawCfNode(const cf::Node& n, int a_depth, int a_index)
{
	// the decoded tree is rebuilt every frame: IDs must not depend on where it sits in memory, or an
	// opened section would close again on the next frame (it flickered)
	ImGui::PushID(a_index);
	if (n.kids.empty()) {
		if (n.isRef) {
			if (n.refRaw == 0) {
				ImGui::BulletText("%s：无", n.label.c_str());
			} else {
				RefID id;
				id.data = n.refRaw;
				const auto* target = _save->findChangeForm(id);
				ImGui::BulletText("%s：%s", n.label.c_str(), n.formID ? formText(n.formID).c_str() : Fmt("RefID %06X", n.refRaw).c_str());
				tips::Tip(tips::Explain(n.label));
				if (target) {
					ImGui::SameLine();
					if (ImGui::SmallButton("跳转")) {
						select(Cat::kChangeForms, n.refRaw);
					}
					tips::Tip("跳转到此指向对象的变更表单。");
				}
			}
		} else {
			ImGui::BulletText("%s：%s", n.label.c_str(), n.value.c_str());
			tips::Tip(tips::Explain(n.label));
		}
	} else if (const bool open = ImGui::TreeNodeEx(n.label.c_str(), a_depth < 2 ? ImGuiTreeNodeFlags_DefaultOpen : 0);
			   tips::Tip(tips::Explain(n.label)), open) {
		for (std::size_t i = 0; i < n.kids.size(); ++i) {
			drawCfNode(n.kids[i], a_depth + 1, static_cast<int>(i));
		}
		ImGui::TreePop();
	}
	ImGui::PopID();
}

void App::detailChangeFormDecoded(const ChangeForm& c)
{
	const auto fid = _save->formIDOf(c.refID);
	if (_db) {
		const auto d = _db->describe(fid);
		if (!d.empty()) {
			ImGui::TextWrapped("%s", d.c_str());
		}
	}
	cf::TypeLookup types;
	if (_db) {
		types = [this](std::uint32_t id) {
			const auto* f = _db->find(id);
			return f ? _db->type(*f) : std::string();
		};
	}
	const auto d = cf::Decode(*_save, c, types);

	ImGui::SeparatorText("变更标志");
	const auto names = cf::FlagNames(c.typeCode(), c.changeFlags);
	if (names.empty()) {
		ImGui::TextDisabled("（无）");
	}
	for (const auto& n : names) {
		ImGui::BulletText("%s", n.c_str());
		tips::Tip(tips::Explain(n));
		const int bit = std::atoi(n.c_str());
		if (d.complete && cf::CanRemoveFlag(c, bit) && fid != 0x7) {  // not the player: that would reset your own character
			ImGui::SameLine();
			ImGui::PushID(bit);
			if (ImGui::SmallButton("移除")) {
				const auto refRaw = c.refID.data;
				const auto effect = cf::RemoveFlagEffect(c.typeCode(), bit);
				confirm("移除变更标志",
					Fmt("从此变更表单移除“%s”？\n\n此标志会被清除，它保存的数据会被删掉；其余内容"
						"保留。%s\n\n之后另存为新文件，并保留原文件。",
						n.c_str(), effect.c_str()),
					[this, refRaw, bit, n] {
						RefID id;
						id.data = refRaw;
						if (auto* cfp = _save->findChangeForm(id)) {
							const auto why = cf::RemoveFlag(*_save, *cfp, bit);
							if (why.empty()) {
								markEdited("已移除 " + n + "，来源 " + formText(_save->formIDOf(id)));
							} else {
								_error = "无法移除 " + n + "：" + why;
							}
						}
					});
			}
			if (const char* what = tips::Explain(n)) {
				tips::Tipf("%s\n\n“移除”会清除此标志并删掉它保存的数据，游戏因此改用插件里的内容。"
						   "会先询问。只在此存档未被游戏占用时编辑；另存为新文件并保留原文件。", what);
			} else {
				tips::Tip("“移除”会清除此标志并删掉它保存的数据，游戏因此改用插件里的内容。会"
						  "先询问。另存为新文件并保留原文件。");
			}
			ImGui::PopID();
		}
	}
	if (d.baseObject) {
		ImGui::Text("基础对象：%s", formText(d.baseObject).c_str());
	}
	if (d.hasHavok) {
		TextColored(theme::Warn(), Fmt("含已保存的物理（Havok）状态：%zu 字节。", d.havokSize));
		if (d.complete && ImGui::SmallButton("重置此对象的 Havok")) {
			const auto refRaw = c.refID.data;
			confirm("重置 Havok", "把此对象已保存的物理状态归零？载入后它会回到静止位置。",
				[this, refRaw] {
					RefID id;
					id.data = refRaw;
					if (auto* cf = _save->findChangeForm(id)) {
						const auto dd = cf::Decode(*_save, *cf);
						if (dd.complete && dd.hasHavok) {
							auto body = cf::Body(*cf);
							std::fill(body.begin() + static_cast<std::ptrdiff_t>(dd.havokOffset),
								body.begin() + static_cast<std::ptrdiff_t>(dd.havokOffset + dd.havokSize), std::uint8_t(0));
							cf::SetBody(*cf, body);
							markChanged("重置了一个对象的 Havok");
						}
					}
				});
		}
		if (d.complete) {
			tips::Tip("把此对象已保存的物理状态归零；单元格载入后它会回到静止位置。可修复乱飞、"
					  "抖动或陷穿地板的物体。");
		}
	}
	if (static_cast<ChangeFormType>(c.typeCode()) == ChangeFormType::ACHR && d.complete) {
		actorFixes(c, d);
	}
	if (d.isFormList) {
		std::size_t nulls = 0;
		for (auto v : d.listRaw) {
			nulls += v == 0;
		}
		ImGui::Text("FormList：%zu 个条目，其中 %zu 个为空", d.listRaw.size(), nulls);
		if (nulls) {
			TextColored(theme::Warn(), "这个列表含空条目。");
			if (ImGui::SmallButton("净化此 FormList")) {
				const auto r = cf::PurifyFormLists(*_save, false, c.refID.data);
				markChanged(Fmt("从 FormList 中移除了 %zu 个空条目", r.entries));
			}
			tips::Tip("移除此列表里的空（null）条目。当它保存的某个表单已不存在时（例如卸载模组之后），这些条目会被留下；"
					  "遍历此列表的脚本可能会被它们绊住。");
		}
	}
	ImGui::SeparatorText("已解码数据");
	if (!d.supported) {
		ImGui::TextDisabled("此变更表单类型未解码（ReSaver 也不解码它）；原始字节见下方。");
	} else if (d.complete) {
		TextColored(theme::Good(), Fmt("已完整解码（%zu 字节）。", d.bodySize));
	} else {
		TextColored(theme::Warn(), Fmt("部分解码（%zu / %zu 字节）：%s", d.consumed, d.bodySize, d.error.c_str()));
	}
	for (std::size_t i = 0; i < d.root.kids.size(); ++i) {
		drawCfNode(d.root.kids[i], 0, static_cast<int>(i));
	}
}

// Actor (ACHR) fixes: remove single extra data entries, reset the saved AI
void App::actorFixes(const ChangeForm& c, const cf::Decoded& d)
{
	const auto key = Fmt("%06X/%08X/%u/%u/%zu", c.refID.data, c.changeFlags, c.length1, c.length2, d.extras.size());
	if (key != _actorFixKey) {  // try each removable entry on a copy, once per version of this change form
		_actorFixKey = key;
		_actorExtraOk.assign(d.extras.size(), 0);
		for (std::size_t i = 0; i < d.extras.size(); ++i) {
			if (cf::CanRemoveExtraFrom(c, d.extras[i].name)) {
				ChangeForm copy = c;
				_actorExtraOk[i] = cf::RemoveExtra(*_save, copy, i).empty() ? 1 : 0;
			}
		}
	}
	const auto refRaw = c.refID.data;
	const auto who = formText(_save->formIDOf(c.refID));
	if (!d.extras.empty()) {
		ImGui::SeparatorText("额外数据");
		for (std::size_t i = 0; i < d.extras.size(); ++i) {
			const auto& name = d.extras[i].name;
			ImGui::PushID(static_cast<int>(i) + 1000);
			ImGui::BulletText("%s", name.c_str());
			tips::Tip(tips::Explain(name));
			if (i < _actorExtraOk.size() && _actorExtraOk[i]) {
				ImGui::SameLine();
				if (ImGui::SmallButton("移除")) {
					const auto effect = cf::RemoveExtraEffect(name);
					const bool player = refRaw == 0x400014;
					confirm("移除额外数据",
						Fmt("移除“%s”（来自 %s）？\n\n%s\n\n此变更表单的其余内容保留。%s 之后另存为新文件"
							"并保留原文件。",
							name.c_str(), player ? "你的角色" : "此角色", effect.c_str(),
							player ? "\n\n这是你自己的角色：继续游玩前请先测试新存档。" : ""),
						[this, refRaw, i, name, who] {
							RefID id;
							id.data = refRaw;
							if (auto* cfp = _save->findChangeForm(id)) {
								const auto why = cf::RemoveExtra(*_save, *cfp, i);
								if (why.empty()) {
									markEdited("已移除 " + name + "，来源 " + who);
								} else {
									_error = "无法移除 " + name + "：" + why;
								}
							}
						});
				}
				{
					const char* what = tips::Explain(name);
					const auto  effect = cf::RemoveExtraEffect(name);
					tips::Tipf("%s%s%s\n\n“移除”只会把这一条从角色的已保存数据中删掉（先在副本上测试过）。会先询问；"
							   "另存为新文件并保留原文件。",
						what ? what : "", what ? "\n\n" : "", effect.c_str());
				}
			}
			ImGui::PopID();
		}
	}
	if (cf::CanResetAI(c, d)) {
		ImGui::SeparatorText("AI");
		ImGui::TextWrapped("已保存的 AI：进程等级 %d%s%s。", d.processLevel,
			d.movement.second > d.movement.first + 1 ? "，含移动控制器" : "",
			d.combat.second > d.combat.first + 1 ? "，含战斗控制器" : "");
		if (ImGui::SmallButton("重置此角色的 AI")) {
			confirm("重置 AI",
				"重置此角色已保存的 AI？\n\n它的 AI 进程（当前包、过程、目标、装备状态、对话与知识）、移动与"
				"战斗状态都会被丢弃，进程等级设为 -1，也就是游戏为没有加载 AI 的角色保存的那个状态。"
				"角色载入时游戏会重新构建 AI。它的属性、法术、物品栏、派系"
				"和位置保持不变。\n\n适用于卡在战斗中、卡在某个包中或原地不动的 NPC。\n\n之后另存"
				"为新文件，并保留原文件。",
				[this, refRaw, who] {
					RefID id;
					id.data = refRaw;
					if (auto* cfp = _save->findChangeForm(id)) {
						const auto why = cf::ResetAI(*_save, *cfp);
						if (why.empty()) {
							markEdited("已重置 AI：" + who);
						} else {
							_error = "无法重置 AI：" + why;
						}
					}
				});
		}
		tips::Tip("丢弃此角色已保存的 AI（包、过程、目标、移动与战斗状态），让游戏在"
				  "角色载入时重新构建。可修复卡在战斗中、卡在某个包中或原地不动的 NPC。属性、物品栏和位置保持不变。");
	}
}

// ==========================================================================================
// Clean: Reset Havok, Purify FormLists, Batch Clean
// ==========================================================================================

void App::resetHavok()
{
	const auto dry = cf::ResetHavok(*_save, {}, true);
	if (!dry.reset) {
		_status = "没有可解码的、含已保存 Havok 数据的对象引用；无需重置。";
		return;
	}
	confirm("重置 Havok",
		Fmt("把 %zu 个对象引用的已保存物理（Havok）状态归零？\n\n有 %zu 个引用的数据无法完整解码，"
			"这些引用会保持原样（与 ReSaver 规则相同）。载入存档时物体会回到静止位置，这有助于解决载入时的"
			"物理爆炸和物体乱飞问题。",
			dry.reset, dry.failed),
		[this] {
			cf::TypeLookup types;
			if (_db) {
				types = [this](std::uint32_t id) {
					const auto* f = _db->find(id);
					return f ? _db->type(*f) : std::string();
				};
			}
			const auto r = cf::ResetHavok(*_save, types);
			markEdited(Fmt("重置了 %zu 个对象的 Havok（%zu 个保持原样）", r.reset, r.failed));
		});
}

void App::purifyFormLists()
{
	const auto dry = cf::PurifyFormLists(*_save, true);
	if (!dry.entries) {
		_status = Fmt("没有 FormList 含有空条目%s。", dry.failed ? Fmt("（%zu 个无法解码）", dry.failed).c_str() : "");
		return;
	}
	confirm("净化 FormList", Fmt("从 %zu 个 FormList 中移除 %zu 个空条目？", dry.entries, dry.forms), [this] {
		const auto r = cf::PurifyFormLists(*_save);
		markEdited(Fmt("从 %zu 个 FormList 中移除了 %zu 个空条目", r.entries, r.forms));
	});
}

void App::drawBatchModal()
{
	if (_batchOpen) {
		ImGui::OpenPopup("批量清理");
		_batchOpen = false;
		_batchResolved = false;
	}
	ImGui::SetNextWindowSize(ImVec2(760, 600), ImGuiCond_Appearing);
	if (!ImGui::BeginPopupModal("批量清理")) {
		return;
	}
	if (!_batchResolved) {
		ImGui::TextWrapped("每行一个脚本（ReSaver 的批量清单格式）。\"Name*\" 匹配所有以 Name 开头的脚本；"
						   "\"Name @@ reason\" 会先询问；以 ; 或 # 开头的行会被忽略。");
		ImGui::InputTextMultiline("##batch", _batchBuf, sizeof(_batchBuf), ImVec2(-1, -ImGui::GetFrameHeightWithSpacing() * 1.5f));
		tips::Tip("每行一个脚本名。Name* 匹配所有以 Name 开头的脚本；“Name @@ reason” 会在清除前先询问；以 ; 或 # 开头的行是注释。");
		if (ImGui::Button("查找这些脚本", ImVec2(180, 0))) {
			_batchMatches = batch::Resolve(*_save, _batchBuf);
			_batchResolved = true;
		}
		tips::Tip("查找清单里的哪些脚本在此存档中。暂时不会做任何修改：先给你一份可核对的列表。");
		ImGui::SameLine();
		if (ImGui::Button("取消", ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
		}
		tips::Tip("关闭，不做任何修改。");
	} else {
		ImGui::Text("本存档中有 %zu 个脚本匹配。要保留的请取消勾选：", _batchMatches.size());
		ImGui::BeginChild("bm", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.5f), ImGuiChildFlags_Borders);
		for (auto& m : _batchMatches) {
			ImGui::PushID(&m);
			ImGui::Checkbox(Fmt("%s   （%zu 个实例，%zu 个线程）", m.script.c_str(), m.instances, m.threads).c_str(), &m.selected);
			tips::Tip("勾选的脚本会被清除：正在运行的线程、排队调用和实例都会删除。取消勾选可保留。");
			if (!m.prompt.empty()) {
				ImGui::SameLine();
				TextColored(theme::Warn(), "@@ " + m.prompt);
			}
			ImGui::PopID();
		}
		ImGui::EndChild();
		const auto n = std::count_if(_batchMatches.begin(), _batchMatches.end(), [](const auto& m) { return m.selected; });
		ImGui::BeginDisabled(n == 0);
		if (ImGui::Button(Fmt("清除 %zu 个脚本", static_cast<std::size_t>(n)).c_str(), ImVec2(180, 0))) {
			const auto r = batch::Apply(*_save, _batchMatches);
			ImGui::CloseCurrentPopup();
			markEdited(Fmt("批量清理：清除了 %zu 个脚本（%zu 个线程、%zu 个堆栈、%zu 个排队调用、%zu 个实例）", r.scripts, r.threads,
				r.stacks, r.messages, r.instances));
		}
		ImGui::EndDisabled();
		tips::Tip("删除勾选脚本在存档中留下的一切：线程、挂起堆栈、排队调用和实例。只对已卸载的"
				  "模组使用；另存为新文件并保留原文件。");
		ImGui::SameLine();
		if (ImGui::Button("返回", ImVec2(120, 0))) {
			_batchResolved = false;
		}
		tips::Tip("返回并编辑脚本清单。");
		ImGui::SameLine();
		if (ImGui::Button("取消", ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
		}
	}
	ImGui::EndPopup();
}

// ==========================================================================================
// Compare To
// ==========================================================================================

void App::compareWith(const std::filesystem::path& a_other)
{
	try {
		auto other = SaveFile::load(a_other);
		_compareLines = compare::Compare(*_save, *other).text();
		_compareTitle = platform::ToUtf8(_path.filename()) + "  ->  " + platform::ToUtf8(a_other.filename());
		_showCompare = true;
	} catch (const std::exception& e) {
		_error = "无法读取 " + platform::ToUtf8(a_other.filename()) + "：\n\n" + e.what();
	}
}

void App::drawCompare()
{
	prepareWindow("Compare saves", ImVec2(0.7f, 0.75f));
	if (!ImGui::Begin("对比存档", &_showCompare, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Compare saves");
	ImGui::TextUnformatted(("A = " + _compareTitle).c_str());
	if (ImGui::Button("复制到剪贴板")) {
		std::string all;
		for (const auto& l : _compareLines) {
			all += l + "\n";
		}
		ImGui::SetClipboardText(all.c_str());
	}
	tips::Tip("把整份对比复制为文本，例如粘贴到问题报告或论坛帖子里。");
	ImGui::Separator();
	ImGui::BeginChild("cmp");
	for (const auto& l : _compareLines) {
		if (!l.empty() && l.back() == ':') {
			ImGui::Spacing();
			TextColored(theme::Warn(), l);
		} else {
			ImGui::TextUnformatted(l.c_str());
		}
	}
	ImGui::EndChild();
	ImGui::End();
}

// ==========================================================================================
// reports
// ==========================================================================================

void App::drawReportModal()
{
	if (_reportOpen) {
		ImGui::OpenPopup("报告");
		_reportOpen = false;
	}
	ImGui::SetNextWindowSize(ImVec2(640, 0));
	if (!ImGui::BeginPopupModal("报告", nullptr, ImGuiWindowFlags_NoResize)) {
		return;
	}
	ImGui::TextWrapped("为这个存档写一份报告。选择要包含的内容：");
	ImGui::Spacing();
	ImGui::Checkbox("模组与插件，及其脚本和实例", &_reportOpt.mods);
	tips::Tip("包含每个模组及其脚本、实例和运行中的线程。");
	ImGui::Indent(32);
	ImGui::PushTextWrapPos(0);
	ImGui::TextDisabled("每个模组、其对象上的每个脚本、脚本实例与运行中的线程（与“模组”视图相同）。");
	ImGui::PopTextWrapPos();
	ImGui::Unindent(32);
	ImGui::Checkbox("发现的异常", &_reportOpt.anomalies);
	tips::Tip("包含 ReSaver++ 发现的问题：未定义或未附加的脚本、卡住的线程、MCM 卡死、FormList 空条目等等。");
	ImGui::Indent(32);
	ImGui::PushTextWrapPos(0);
	ImGui::TextDisabled("分析结果、未定义 / 未附加 / 循环脚本、已终止的线程、MCM 卡死、含空条目的 "
						"FormList、最繁忙的脚本。");
	ImGui::PopTextWrapPos();
	ImGui::Unindent(32);
	ImGui::Separator();
	ImGui::Checkbox("把报告导出为 PDF（否则为 .txt 文件）", &_reportOpt.pdf);
	tips::Tip("勾选：生成任意阅读器都能打开的 PDF。不勾选：纯文本文件，方便贴到论坛帖子里。");
	if (!_db && _sources.scanned()) {
		ImGui::TextDisabled(namesLoading() ? "插件名仍在读取中；报告只会显示表单 ID。" : "");
	}
	ImGui::Spacing();
	ImGui::BeginDisabled(!_reportOpt.mods && !_reportOpt.anomalies);
	if (ImGui::Button("写报告…", ImVec2(160, 0))) {
		const char* ext = _reportOpt.pdf ? "pdf" : "txt";
		auto        suggested = _path;
		suggested.replace_filename(_path.stem().u8string() + u8"_report." + std::u8string(reinterpret_cast<const char8_t*>(ext)));
		std::optional<std::filesystem::path> target;
		if (platform::HasNativeDialogs()) {
			target = platform::SaveFileDialog(suggested, ext, "报告另存为");
		} else {
			target = suggested;
		}
		if (target) {
			writeReport(*target);
			ImGui::CloseCurrentPopup();
		}
	}
	ImGui::EndDisabled();
	tips::Tip("选择报告的保存位置并写入。存档本身不会被修改。");
	ImGui::SameLine();
	if (ImGui::Button("取消", ImVec2(120, 0))) {
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void App::writeReport(const std::filesystem::path& a_path)
{
	try {
		ensureMods();
		const auto lines = report::Build(*_save, *_mods, _db.get(), _reportOpt, platform::ToUtf8(_path.filename()));
		auto       ext = Lower(platform::ToUtf8(a_path.extension()));
		if (ext == ".pdf") {
			report::WritePdf(a_path, "ReSaver++ 报告 - " + platform::ToUtf8(_path.filename()), lines);
		} else {
			report::WriteText(a_path, lines);
		}
		_status = Fmt("报告已写入：%s（%zu 行）", platform::ToUtf8(a_path.filename()).c_str(), lines.size());
		platform::OpenWithDefaultApp(a_path);
	} catch (const std::exception& e) {
		_error = std::string("无法写入报告：\n\n") + e.what();
	}
}

// ==========================================================================================
// watch the saves folder (ReSaver: Watch Savefile Directory)
// ==========================================================================================

std::vector<std::filesystem::path> App::watchFolders() const
{
	std::vector<std::filesystem::path> v;
	if (auto d = platform::DefaultSavesFolder(_vr); !d.empty()) {
		v.push_back(d);
		// Vortex's profile-specific saves (and similar tools) use sub-folders of Saves
		std::error_code ec;
		for (const auto& e : std::filesystem::directory_iterator(d, ec)) {
			if (e.is_directory(ec)) {
				v.push_back(e.path());
			}
		}
	}
	if (_mo2 && !_profile.empty()) {
		v.push_back(_mo2->profilesDir / platform::FromUtf8(_profile) / "saves");  // MO2 profile-local saves
	}
	for (auto& d : iniSaveFolders()) {
		v.push_back(std::move(d));
	}
	if (const auto& s = savesDirSetting(); !s.empty()) {
		v.insert(v.begin(), platform::FromUtf8(s));  // chosen in Settings
	}
	// each folder once
	std::vector<std::filesystem::path> out;
	for (auto& p : v) {
		const auto key = Lower(platform::ToUtf8(p.lexically_normal()));
		if (std::none_of(out.begin(), out.end(), [&](const std::filesystem::path& o) { return Lower(platform::ToUtf8(o.lexically_normal())) == key; })) {
			out.push_back(std::move(p));
		}
	}
	return out;
}

std::vector<std::filesystem::path> App::iniSaveFolders() const
{
	// sLocalSavePath in Skyrim.ini / SkyrimCustom.ini moves the saves; it is relative to Documents\My Games\<game>
	std::vector<std::filesystem::path> v;
	const auto saves = platform::DefaultSavesFolder(_vr);
	if (saves.empty()) {
		return v;
	}
	const auto              myGames = saves.parent_path();
	std::vector<std::filesystem::path> inis = { myGames / "Skyrim.ini", myGames / "SkyrimCustom.ini", myGames / "SkyrimVR.ini" };
	if (_mo2 && !_profile.empty()) {  // Mod Organizer profile-specific game ini files
		const auto prof = _mo2->profilesDir / platform::FromUtf8(_profile);
		inis.push_back(prof / "Skyrim.ini");
		inis.push_back(prof / "SkyrimCustom.ini");
	}
	std::error_code ec;
	for (const auto& ini : inis) {
		std::ifstream f(ini);
		for (std::string line; std::getline(f, line);) {
			const auto low = Lower(line);
			const auto eq = low.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			auto key = low.substr(0, eq);
			key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) { return std::isspace(c); }), key.end());
			if (key != "slocalsavepath") {
				continue;
			}
			auto val = line.substr(eq + 1);
			while (!val.empty() && (std::isspace(static_cast<unsigned char>(val.back())) || val.back() == '\\' || val.back() == '/')) {
				val.pop_back();
			}
			while (!val.empty() && std::isspace(static_cast<unsigned char>(val.front()))) {
				val.erase(val.begin());
			}
			if (val.empty() || Lower(val).rfind("__mo_", 0) == 0) {  // Mod Organizer's own redirect: its profile saves are added already
				continue;
			}
			auto p = platform::FromUtf8(val);
			if (p.is_relative()) {
				p = myGames / p;
			}
			p = p.lexically_normal();
			if (std::filesystem::is_directory(p, ec)) {
				v.push_back(p);
			}
		}
	}
	return v;
}

void App::setWatching(bool a_on)
{
	_watch = a_on;
	_watchPending.clear();
	_watchSeen.clear();
	if (a_on) {
		// remember what is there now; only saves written from now on are opened
		std::error_code ec;
		for (const auto& dir : watchFolders()) {
			for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
				if (e.is_regular_file(ec) && Lower(platform::ToUtf8(e.path().extension())) == ".ess") {
					_watchSeen[e.path()] = e.last_write_time(ec);
				}
			}
		}
		_status = "正在监视存档文件夹：新存档会自动打开。";
	}
	saveSettings();
}

void App::pollWatch()
{
	if (!_watch) {
		return;
	}
	const double now = ImGui::GetTime();
	if (now < _watchNext) {
		return;
	}
	_watchNext = now + 1.5;
	std::error_code ec;
	for (const auto& dir : watchFolders()) {
		for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
			if (!e.is_regular_file(ec) || Lower(platform::ToUtf8(e.path().extension())) != ".ess") {
				continue;
			}
			const auto t = e.last_write_time(ec);
			auto       it = _watchSeen.find(e.path());
			if (it != _watchSeen.end() && it->second == t) {
				continue;
			}
			_watchSeen[e.path()] = t;
			_watchPending[e.path()] = { e.file_size(ec), now };
		}
	}
	// open a new save once its size has stopped changing (the game writes .ess then .skse)
	for (auto it = _watchPending.begin(); it != _watchPending.end();) {
		const auto size = std::filesystem::file_size(it->first, ec);
		if (ec) {
			it = _watchPending.erase(it);
			continue;
		}
		if (size != it->second.first) {
			it->second = { size, now };
			++it;
			continue;
		}
		if (now - it->second.second < 2.0) {
			++it;
			continue;
		}
		const auto path = it->first;
		it = _watchPending.erase(it);
		if (_dirty) {
			_status = "出现了新存档 " + platform::ToUtf8(path.filename()) + "；当前存档有未保存的修改，因此未打开。";
		} else {
			open(path);
		}
	}
}

// ==========================================================================================
// Player editor: gold, skills, bounties
// ==========================================================================================

void App::drawPlayer()
{
	prepareWindow("Edit player", ImVec2(0.7f, 0.85f));
	if (!ImGui::Begin("编辑玩家", &_showPlayer, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Edit player");
	if (!_pvLoaded) {
		_pv = cf::ReadPlayer(*_save);
		_pvLoaded = true;
		_pvGold = _pv.gold;
		for (int i = 0; i < 18; ++i) {
			_pvSkills[i] = _pv.skills[i];
			_pvLegend[i] = _pv.legendary[i];
		}
		_pvBounty.clear();
		for (const auto& b : _pv.bounties) {
			_pvBounty.push_back({ b.violent, b.nonViolent });
		}
		_pvLevel = _pv.level;
		_pvXP = _pv.xp;
		_pvPerks = _pv.perkPoints;
		_pvSouls = 0;
		_pvAVEdit.clear();
		for (const auto& a : _pv.avs) {
			_pvAVEdit[a.id] = a.base;
			if (a.id == 133) {
				_pvSouls = static_cast<int>(a.current());
			}
		}
		_pvItemEdit.clear();
		for (const auto& it : _pv.items) {
			_pvItemEdit[it.item] = it.count;
		}
	}
	if (!_pv.error.empty()) {
		TextColored(theme::Warn(), "无法编辑玩家：" + _pv.error);
		ImGui::End();
		return;
	}
	ImGui::NewLine();
	ImGui::TextWrapped("修改会作用在已载入的存档上；在你使用“文件 > 保存”或“另存为”之前不会写入任何文件。请保留原"
					   "存档，并在继续游玩前先测试新存档。");
	auto done = [this](const std::string& a_why, const std::string& a_what) {
		if (a_why.empty()) {
			markChanged(a_what);
		} else {
			_error = "无法修改玩家：" + a_why;
		}
		_pvLoaded = false;  // read the values again
	};
	auto matches = [](const std::string& a_text, const char* a_filter) {
		if (!a_filter[0]) {
			return true;
		}
		std::string t = a_text, f = a_filter;
		std::transform(t.begin(), t.end(), t.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		std::transform(f.begin(), f.end(), f.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		return t.find(f) != std::string::npos;
	};
	auto setButton = [](bool a_changed, const char* a_label = "设定") {
		ImGui::BeginDisabled(!a_changed);
		const bool b = ImGui::SmallButton(a_label);
		ImGui::EndDisabled();
		return b;
	};

	// ---- character ----
	const bool hChar = ImGui::CollapsingHeader("角色", ImGuiTreeNodeFlags_DefaultOpen);
	tips::Tip("金币、角色等级、升级进度、未花费的专长点和龙魂。");
	if (hChar) {
		if (ImGui::BeginTable("char", 3, ImGuiTableFlags_SizingFixedFit)) {
			auto row = [&](const char* a_name) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(a_name);
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(150);
			};
			row("金币");
			ImGui::InputInt("##gold", &_pvGold, 100, 1000);
			tips::Tip("你的角色随身携带的金币。输入新数量，然后按“设定”。");
			_pvGold = std::max(_pvGold, 0);
			ImGui::TableNextColumn();
			if (setButton(_pvGold != _pv.gold, "设定##gold")) {
				done(cf::SetPlayerGold(*_save, _pvGold), Fmt("把玩家的金币设为 %d", _pvGold));
			}
			tips::Tip("把新的金币数量写入已载入的存档（在“文件 > 保存”之前不会写盘）。");
			if (_pv.hasLevel) {
				row("等级");
				ImGui::InputInt("##level", &_pvLevel, 1, 5);
				tips::Tip("你的角色等级。修改它不会补发所跳过等级的专长点、生命、魔法或耐力。");
				_pvLevel = std::clamp(_pvLevel, 1, 65535);
				ImGui::TableNextColumn();
				if (setButton(_pvLevel != _pv.level, "设定##level")) {
					done(cf::SetPlayerLevel(*_save, _pvLevel), Fmt("把玩家的等级设为 %d", _pvLevel));
				}
				tips::Tip("把新的角色等级写入已载入的存档。随你等级缩放的敌人会跟随新等级。");
				ImGui::SameLine();
				ImGui::TextDisabled("（跳过的等级不会补发专长点和属性）");
			}
			row("升级经验");
			ImGui::InputFloat("##xp", &_pvXP, 10.0f, 100.0f, "%.0f");
			tips::Tip("距离下一个角色等级的经验（技能菜单里的进度条）。达到右侧显示的数值即可升级。");
			_pvXP = std::max(_pvXP, 0.0f);
			ImGui::TableNextColumn();
			if (setButton(_pvXP != _pv.xp, "设定##xp")) {
				done(cf::SetPlayerXP(*_save, _pvXP), Fmt("把玩家的升级经验设为 %.0f", _pvXP));
			}
			tips::Tip("把新的升级进度写入已载入的存档。");
			ImGui::SameLine();
			ImGui::TextDisabled("下一级需要 %.0f", _pv.xpNext);
			row("专长点");
			ImGui::InputInt("##perks", &_pvPerks, 1, 5);
			tips::Tip("你尚未花费的专长点（0 到 255）。");
			_pvPerks = std::clamp(_pvPerks, 0, 255);
			ImGui::TableNextColumn();
			if (setButton(_pvPerks != _pv.perkPoints, "设定##perks")) {
				done(cf::SetPlayerPerkPoints(*_save, _pvPerks), Fmt("把玩家的专长点设为 %d", _pvPerks));
			}
			tips::Tip("把新的未花费专长点数量写入已载入的存档。");
			const bool hasSouls = std::any_of(_pv.avs.begin(), _pv.avs.end(), [](const auto& a) { return a.id == 133 && a.hasBase; });
			if (hasSouls) {
				int current = 0;
				for (const auto& a : _pv.avs) {
					if (a.id == 133) {
						current = static_cast<int>(a.current());
					}
				}
				row("龙魂");
				ImGui::InputInt("##souls", &_pvSouls, 1, 5);
				tips::Tip("你尚未使用的龙魂；每个可解锁一段力量之语。");
				_pvSouls = std::max(_pvSouls, 0);
				ImGui::TableNextColumn();
				if (setButton(_pvSouls != current, "设定##souls")) {
					done(cf::SetPlayerDragonSouls(*_save, _pvSouls), Fmt("把玩家的龙魂设为 %d", _pvSouls));
				}
				tips::Tip("把新的龙魂数量写入已载入的存档。");
			}
			ImGui::EndTable();
		}
	}

	// ---- skills ----
	const bool hSkills = ImGui::CollapsingHeader("技能");
	tips::Tip("你的 18 个技能：各自的等级，以及每个被重置为传奇的次数。");
	if (hSkills) {
		ImGui::TextDisabled("设定技能等级会让它距下一级的进度从头开始。角色等级不会改变。");
		if (ImGui::BeginTable("skills", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
			ImGui::TableSetupColumn("技能");
			ImGui::TableSetupColumn("等级");
			ImGui::TableSetupColumn("");
			ImGui::TableSetupColumn(_pv.hasLegendary ? "传奇次数" : "");
			ImGui::TableSetupColumn("");
			ImGui::TableHeadersRow();
			for (int i = 0; i < 18; ++i) {
				ImGui::PushID(i);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(cf::SkillNameZh(i));
				ImGui::TableNextColumn();
				ImGui::BeginDisabled(!_pv.hasSkill[i]);
				ImGui::SetNextItemWidth(120);
				ImGui::InputFloat("##lvl", &_pvSkills[i], 1.0f, 10.0f, "%.0f");
				tips::Tipf("%s 的技能等级。正常提升技能也会计入角色等级；在这里设定则不会。", cf::SkillNameZh(i));
				_pvSkills[i] = std::clamp(_pvSkills[i], 0.0f, 1000.0f);
				ImGui::TableNextColumn();
				if (setButton(_pvSkills[i] != _pv.skills[i])) {
					done(cf::SetPlayerSkill(*_save, i, _pvSkills[i]), Fmt("把玩家的 %s 设为 %.0f", cf::SkillNameZh(i), _pvSkills[i]));
				}
				tips::Tip("把新的技能等级写入已载入的存档。它距下一级技能的进度会从头开始。");
				ImGui::EndDisabled();
				if (_pv.hasLegendary) {
					ImGui::TableNextColumn();
					ImGui::SetNextItemWidth(100);
					ImGui::InputInt("##leg", &_pvLegend[i], 1, 1);
					tips::Tip("此技能被重置为传奇的次数（重置回 15 级可拿回专长点）。");
					_pvLegend[i] = std::max(_pvLegend[i], 0);
					ImGui::TableNextColumn();
					if (setButton(_pvLegend[i] != _pv.legendary[i], "设定##leg")) {
						done(cf::SetPlayerLegendary(*_save, i, _pvLegend[i]),
							Fmt("把玩家的 %s 传奇次数设为 %d", cf::SkillNameZh(i), _pvLegend[i]));
					}
					tips::Tip("把新的传奇次数写入已载入的存档。它只改变计数（以及游戏显示的属性），不会改动你的专长。");
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}

	// ---- stats and other actor values ----
	const bool hAV = ImGui::CollapsingHeader("属性与角色数值");
	tips::Tip("存档为你保存的每一项角色数值（负重、恢复速率、抗性……），以及正在生效的效果和物品对它造成的改变。模组留下的异常数值可用“重置”处理。");
	if (hAV) {
		ImGui::TextWrapped("基础值是存档里保存的数值；永久、临时和损伤是在它之上的改变。重置会丢弃已保存的数值"
						   "及其永久与损伤改动，让你的种族和插件重新给出该值（用于模组留下的"
						   "异常数值）。生命、魔法、耐力、技能和龙魂承载着你的进度，因此只能设定、不能重置。"
						   "临时改动通常来自正在生效的效果：只在确实不该有任何影响时才清除。");
		ImGui::SetNextItemWidth(250);
		ImGui::InputTextWithHint("##avf", "过滤器（例如 rate、resist、carry）", _pvAVFilter, sizeof(_pvAVFilter));
		tips::Tip("只显示名称包含这段文字的角色数值。");
		if (ImGui::BeginTable("avs", 7, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 380))) {
			ImGui::TableSetupScrollFreeze(0, 1);
			for (const char* h : { "角色数值", "基础值", "", "永久", "临时", "损伤", "当前" }) {
				ImGui::TableSetupColumn(h);
			}
			ImGui::TableHeadersRow();
			for (const auto& a : _pv.avs) {
				if (a.id >= 6 && a.id <= 23) {
					continue;  // the skills have their own section
				}
				const std::string name = cf::ActorValueNameZh(a.id);
				if (!matches(name, _pvAVFilter) && !matches(cf::ActorValueName(a.id), _pvAVFilter)) {
					continue;
				}
				ImGui::PushID(static_cast<int>(a.id) + 5000);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(name.c_str());
				ImGui::TableNextColumn();
				if (a.hasBase) {
					auto& e = _pvAVEdit[a.id];
					ImGui::SetNextItemWidth(110);
					ImGui::InputFloat("##b", &e, 0, 0, "%.2f");
					tips::Tip(tips::Explain(name) ? tips::Explain(name) : "此角色数值在存档里保存的基础值。输入新数值，然后按“设定”。");
					ImGui::TableNextColumn();
					if (setButton(e != a.base)) {
						done(cf::SetPlayerAV(*_save, a.id, e), Fmt("把玩家的 %s 设为 %g", name.c_str(), e));
					}
					tips::Tip("把新的基础值写入已载入的存档。");
				} else {
					ImGui::TextDisabled("（来自插件）");
					ImGui::TableNextColumn();
				}
				if (cf::CanResetPlayerAV(a.id) && (a.hasBase || a.permanent != 0 || a.damage != 0)) {
					ImGui::SameLine();
					if (ImGui::SmallButton("重置")) {
						done(cf::ResetPlayerAV(*_save, a.id), "已重置玩家的 " + name);
					}
					tips::Tip("丢弃已保存的数值及其永久与损伤改动，让你的种族、专长和插件重新决定它。用于模组留下的异常数值。");
				}
				ImGui::TableNextColumn();
				ImGui::Text("%g", a.permanent);
				if (a.permanent != 0) {
					ImGui::SameLine();
					if (ImGui::SmallButton("清除##p")) {
						done(cf::ClearPlayerAVModifiers(*_save, a.id, 1), "已清除玩家的永久改动：" + name);
					}
					tips::Tip("把永久改动归零（来自专长、任务奖励或模组）。仍生效的效果可能会重新加上它们的那部分。");
				}
				ImGui::TableNextColumn();
				ImGui::Text("%g", a.temporary);
				if (a.temporary != 0) {
					ImGui::SameLine();
					if (ImGui::SmallButton("清除##t")) {
						done(cf::ClearPlayerAVModifiers(*_save, a.id, 2), "已清除玩家的临时改动：" + name);
					}
					tips::Tip("把临时改动归零（来自正在生效的效果：穿戴的附魔、药水、法术）。只在确实不该有任何影响时才用：穿戴的物品要重新装备后才会重新加上它们的那部分。");
				}
				ImGui::TableNextColumn();
				ImGui::Text("%g", a.damage);
				if (a.damage != 0) {
					ImGui::SameLine();
					if (ImGui::SmallButton("清除##d")) {
						done(cf::ClearPlayerAVModifiers(*_save, a.id, 4), "已清除玩家的损伤：" + name);
					}
					tips::Tip("把损伤归零（例如损失的生命或魔法）：数值恢复满值。");
				}
				ImGui::TableNextColumn();
				if (a.hasBase) {
					ImGui::Text("%g", a.current());
				} else {
					ImGui::TextDisabled("插件 %+g", a.permanent + a.temporary + a.damage);
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}

	// ---- inventory ----
	const bool hInv = ImGui::CollapsingHeader("物品栏");
	tips::Tip("你的角色在初始物品栏之外携带的物品：可修改数量或移除物品。");
	if (hInv) {
		ImGui::TextWrapped("这里的数量是存档加到角色初始物品栏上的数量。“设定”改的是普通那一堆（附魔、"
						   "强化或穿戴中的堆各自保留数量）；“移除”会拿走该物品的所有堆。移除任务物品可能"
						   "使其任务无法完成。");
		ImGui::SetNextItemWidth(250);
		ImGui::InputTextWithHint("##itf", "按名称或表单 ID 过滤", _pvItemFilter, sizeof(_pvItemFilter));
		tips::Tip("只显示名称或表单 ID 包含这段文字的物品。");
		if (ImGui::BeginTable("items", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 380))) {
			ImGui::TableSetupScrollFreeze(0, 1);
			for (const char* h : { "物品", "数量", "", "" }) {
				ImGui::TableSetupColumn(h);
			}
			ImGui::TableHeadersRow();
			for (const auto& it : _pv.items) {
				const auto name = formText(it.item);
				if (!matches(name, _pvItemFilter)) {
					continue;
				}
				ImGui::PushID(static_cast<int>(it.item));
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(name.c_str());
				if (it.hasExtra) {
					ImGui::SameLine();
					ImGui::TextDisabled("（特殊）");
				}
				ImGui::TableNextColumn();
				auto& e = _pvItemEdit[it.item];
				ImGui::SetNextItemWidth(110);
				ImGui::InputInt("##c", &e, 1, 10);
				tips::Tip("存档加到你的初始物品栏里的数量。输入新数量，然后按“设定”。");
				e = std::max(e, 0);
				ImGui::TableNextColumn();
				if (setButton(e != it.count)) {
					done(cf::SetPlayerItemCount(*_save, it.item, e), Fmt("把玩家的 %s 设为 %d", name.c_str(), e));
				}
				tips::Tip("把普通那一堆的新数量写入已载入的存档。附魔、强化或穿戴中的堆会保留各自的数量。");
				ImGui::TableNextColumn();
				if (ImGui::SmallButton("移除")) {
					const auto item = it.item;
					confirm("移除物品", "移除玩家携带的所有 " + name + "？",
						[this, item, name, done] { done(cf::RemovePlayerItem(*_save, item), "已移除玩家携带的 " + name); });
				}
				tips::Tip("从你的角色身上移除该物品的所有堆（会先询问）。移除任务物品可能使其任务无法完成。");
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}

	// ---- spells, powers, abilities: the ones added in game, by their type in the plugins ----
	{
		struct Group
		{
			const char* title;
			const char* note;
			std::vector<std::size_t> rows;
		};
		Group groups[] = {
			{ "游戏内添加的法术", "你在魔法菜单里学到的、或模组给你的法术。", {} },
			{ "游戏内添加的能力", "你魔法菜单里的高级与次级能力。", {} },
			{ "游戏内添加的被动能力",
				"恒定效果（显示在“激活效果”下，不算法术）。模组常会留下这些；移除一项即可停止其效果。", {} },
			{ "疾病、成瘾与其他效果", "疾病、成瘾、毒药以及其他法术类型。", {} },
			{ "在你的插件里找不到",
				"此存档没有任何插件定义它们（或插件名尚未加载）：通常是已移除模组的残留。", {} },
		};
		auto groupOf = [this](std::uint32_t a_id) -> int {
			const auto* f = _db ? _db->find(a_id) : nullptr;
			if (!f) {
				return 4;
			}
			if (_db->type(*f) != "SPEL") {
				return 3;
			}
			switch (f->spellType) {
			case 0:
				return 0;
			case 2:
			case 3:
				return 1;
			case 4:
				return 2;
			default:
				return 3;
			}
		};
		for (std::size_t i = 0; i < _pv.spells.size(); ++i) {
			groups[groupOf(_pv.spells[i])].rows.push_back(i);
		}
		if (!_db) {
			TextColored(theme::Warn(),
				"插件名尚未加载，因此无法把法术与能力区分开，也无法按名称显示。加载插件名（工具 > 读取"
				"插件名称）即可拆分列表。");
		}
		ImGui::SetNextItemWidth(250);
		ImGui::InputTextWithHint("##spf", "过滤法术、能力与被动能力", _pvSpellFilter, sizeof(_pvSpellFilter));
		tips::Tip("只显示名称、编辑器 ID 或表单 ID 包含这段文字的法术、能力与被动能力。");
		int gi = 0;
		for (auto& g : groups) {
			++gi;
			if (g.rows.empty()) {
				continue;
			}
			ImGui::PushID(gi + 70000);
			const bool hSp = ImGui::CollapsingHeader(Fmt("%s (%zu)", g.title, g.rows.size()).c_str());
			tips::Tip(g.note);
			if (hSp) {
				ImGui::TextDisabled("%s", g.note);
				if (ImGui::BeginTable("sp", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
						ImVec2(0, std::min(360.0f, 30.0f + 26.0f * static_cast<float>(g.rows.size()))))) {
					ImGui::TableSetupScrollFreeze(0, 1);
					for (const char* h : { "", "游戏内名称", "编辑器 ID", "表单" }) {
						ImGui::TableSetupColumn(h);
					}
					ImGui::TableHeadersRow();
					for (const auto i : g.rows) {
						const auto  sp = _pv.spells[i];
						const auto* f = _db ? _db->find(sp) : nullptr;
						const auto  inGame = f ? _db->name(*f) : std::string();
						const auto  edid = f ? _db->edid(*f) : std::string();
						const auto  form = Fmt("%08X", sp) + (f ? " (" + _db->pluginName(*f) + ")" : std::string());
						if (!matches(inGame + " " + edid + " " + form, _pvSpellFilter)) {
							continue;
						}
						ImGui::PushID(static_cast<int>(i) + 90000);
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						if (ImGui::SmallButton("移除")) {
							const auto label = inGame.empty() ? (edid.empty() ? form : edid) : inGame;
							confirm("从玩家身上移除", "从玩家身上移除 " + label + "？",
								[this, sp, label, done] { done(cf::RemovePlayerSpell(*_save, sp), "已移除玩家身上的 " + label); });
						}
						tips::Tip("从你的角色身上拿走它（会先询问）。若是被动能力，其恒定效果会停止。你的种族或插件给你的法术不在此列出，会保留。");
						ImGui::TableNextColumn();
						if (inGame.empty()) {
							ImGui::TextDisabled("（无名称）");
						} else {
							ImGui::TextUnformatted(inGame.c_str());
						}
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(edid.c_str());
						ImGui::TableNextColumn();
						ImGui::TextDisabled("%s", form.c_str());
						ImGui::PopID();
					}
					ImGui::EndTable();
				}
			}
			ImGui::PopID();
		}
		if (_pv.spells.empty()) {
			ImGui::TextDisabled("游戏内没有添加任何法术、能力或被动能力。");
		}
	}

	// ---- bounty ----
	const bool hCrime = ImGui::CollapsingHeader("赏金与犯罪");
	tips::Tip("你在各领地的赏金，以及各派系记录在案的犯罪。可清除赏金，也可抹掉记录。");
	if (hCrime) {
		ImGui::Checkbox("显示所有派系（不只是有赏金或犯罪记录的）", &_pvAllFactions);
		tips::Tip("同时列出你没有赏金或犯罪记录的派系，例如可以给自己加一笔赏金。");
		if (ImGui::BeginTable("bounty", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
			for (const char* h : { "派系", "暴力", "非暴力", "", "犯罪总计" }) {
				ImGui::TableSetupColumn(h);
			}
			ImGui::TableHeadersRow();
			int shown = 0;
			for (std::size_t i = 0; i < _pv.bounties.size() && i < _pvBounty.size(); ++i) {
				const auto& b = _pv.bounties[i];
				const bool  any = b.violent || b.nonViolent || b.violentTotal || b.nonViolentTotal;
				if (!_pvAllFactions && !any) {
					continue;
				}
				++shown;
				ImGui::PushID(static_cast<int>(i));
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(formText(b.faction).c_str());
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(110);
				ImGui::InputInt("##v", &_pvBounty[i][0], 100, 1000);
				tips::Tip("暴力犯罪的赏金（袭击、谋杀）。超过 1000 时守卫会见到就攻击。");
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(110);
				ImGui::InputInt("##n", &_pvBounty[i][1], 100, 1000);
				tips::Tip("非暴力犯罪的赏金（偷窃、擅闯、扒窃）。");
				_pvBounty[i][0] = std::max(_pvBounty[i][0], 0);
				_pvBounty[i][1] = std::max(_pvBounty[i][1], 0);
				ImGui::TableNextColumn();
				if (setButton(_pvBounty[i][0] != b.violent || _pvBounty[i][1] != b.nonViolent)) {
					done(cf::SetPlayerBounty(*_save, b.faction, _pvBounty[i][0], _pvBounty[i][1]),
						Fmt("把玩家与 %s 的赏金设为 %d", formText(b.faction).c_str(), _pvBounty[i][0] + _pvBounty[i][1]));
				}
				tips::Tip("把与该派系的新赏金写入已载入的存档。");
				ImGui::SameLine();
				ImGui::BeginDisabled(b.violent == 0 && b.nonViolent == 0);
				if (ImGui::SmallButton("清除")) {
					done(cf::SetPlayerBounty(*_save, b.faction, 0, 0), "已清除玩家的赏金：" + formText(b.faction));
				}
				ImGui::EndDisabled();
				tips::Tip("把与该派系的两种赏金都设为 0。你的犯罪记录（右侧的总计）会保留。");
				ImGui::TableNextColumn();
				ImGui::Text("%d 项暴力，%d 项非暴力", b.violentTotal, b.nonViolentTotal);
				if (b.violentTotal || b.nonViolentTotal) {
					ImGui::SameLine();
					if (ImGui::SmallButton("全部重置")) {
						done(cf::ClearPlayerCrimes(*_save, b.faction), "已清除玩家的赏金与犯罪记录：" + formText(b.faction));
					}
					tips::Tip("清除与该派系的赏金和犯罪总计：记录变干净，就像从未报过案。");
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
			if (!shown) {
				ImGui::TextDisabled("没有记录任何赏金或犯罪。");
			}
		}
	}
	ImGui::End();
}
