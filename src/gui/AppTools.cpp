// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// App features: plugin names, form lookup, decoded change forms, Reset Havok / Purify
// FormLists, batch clean, compare, reports and the save-folder watcher.
#include "App.h"

#include "Platform.h"
#include "Theme.h"

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
	// Background work only reads files. Give it a moment to stop, then leave without it: inside Mod
	// Organizer reading one large archive can take long, and MO2 stays locked while this process exists.
	auto busy = [&] {
		return (_dbThread.joinable() && !_dbDone) || (_scanThread.joinable() && !_scanDone) || scriptJobsBusy();
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
	ImGui::SetNextWindowSize(ImVec2(900, 560), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("查找表单", &_showLookup)) {
		ImGui::End();
		return;
	}
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
	ImGui::SameLine();
	go |= ImGui::Button("搜索", ImVec2(110, 0));
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
				if (li.scripts) {
					ImGui::SameLine();
					if (ImGui::SmallButton("脚本")) {
						select(Cat::kInstances, li.firstInstance);
						std::snprintf(_filter, sizeof(_filter), "%08X", id);
						_rowsDirty = true;
					}
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
				if (target) {
					ImGui::SameLine();
					if (ImGui::SmallButton("跳转")) {
						select(Cat::kChangeForms, n.refRaw);
					}
				}
			}
		} else {
			ImGui::BulletText("%s：%s", n.label.c_str(), n.value.c_str());
		}
	} else if (ImGui::TreeNodeEx(n.label.c_str(), a_depth < 2 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
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
		if (ImGui::Button("查找这些脚本", ImVec2(180, 0))) {
			_batchMatches = batch::Resolve(*_save, _batchBuf);
			_batchResolved = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("取消", ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
		}
	} else {
		ImGui::Text("本存档中有 %zu 个脚本匹配。要保留的请取消勾选：", _batchMatches.size());
		ImGui::BeginChild("bm", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.5f), ImGuiChildFlags_Borders);
		for (auto& m : _batchMatches) {
			ImGui::PushID(&m);
			ImGui::Checkbox(Fmt("%s   （%zu 个实例，%zu 个线程）", m.script.c_str(), m.instances, m.threads).c_str(), &m.selected);
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
		ImGui::SameLine();
		if (ImGui::Button("返回", ImVec2(120, 0))) {
			_batchResolved = false;
		}
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
	ImGui::SetNextWindowSize(ImVec2(820, 620), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("对比存档", &_showCompare)) {
		ImGui::End();
		return;
	}
	ImGui::TextUnformatted(("A = " + _compareTitle).c_str());
	if (ImGui::Button("复制到剪贴板")) {
		std::string all;
		for (const auto& l : _compareLines) {
			all += l + "\n";
		}
		ImGui::SetClipboardText(all.c_str());
	}
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
	ImGui::Indent(32);
	ImGui::PushTextWrapPos(0);
	ImGui::TextDisabled("每个模组、其对象上的每个脚本、脚本实例与运行中的线程（与“模组”视图相同）。");
	ImGui::PopTextWrapPos();
	ImGui::Unindent(32);
	ImGui::Checkbox("发现的异常", &_reportOpt.anomalies);
	ImGui::Indent(32);
	ImGui::PushTextWrapPos(0);
	ImGui::TextDisabled("分析结果、未定义 / 未附加 / 循环脚本、已终止的线程、MCM 卡死、含空条目的 "
						"FormList、最繁忙的脚本。");
	ImGui::PopTextWrapPos();
	ImGui::Unindent(32);
	ImGui::Separator();
	ImGui::Checkbox("把报告导出为 PDF（否则为 .txt 文件）", &_reportOpt.pdf);
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
