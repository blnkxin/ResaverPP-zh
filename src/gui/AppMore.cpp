// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// 本地化修改：界面、提示与报告文字改为简体中文（详见 README.zh-CN.md）。
// The rest of ReSaver's features: filters, plugin actions, array editing, arrays analysis,
// plugin list export, log, settings, support links, script (.pex) viewer.
#include "App.h"

#include "Platform.h"
#include "Theme.h"

#include "core/Text.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <unordered_set>

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

	bool HexInput(const char* label, std::uint32_t& v)
	{
		char buf[16];
		std::snprintf(buf, sizeof(buf), "%08X", v);
		ImGui::SetNextItemWidth(90);
		if (ImGui::InputText(label, buf, sizeof(buf), ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase)) {
			v = static_cast<std::uint32_t>(std::strtoul(buf, nullptr, 16));
			return true;
		}
		return false;
	}
}

// ==========================================================================================
// filters (ReSaver's Filter menu)
// ==========================================================================================

bool App::drawFiltersButton()
{
	auto&      F = _filters;
	const bool active = F.noParent || F.missingParent || F.memberless || F.zeroedCanary || F.withScripts || F.longStrings || F.nullLists ||
						F.cellMinus1 || F.emptyRef || F.parseLevel || F.flagMask || F.formFlagMask || F.content[0];
	if (ImGui::Button(active ? "过滤器 *" : "过滤器")) {
		ImGui::OpenPopup("filters");
	}
	if (ImGui::BeginPopup("filters")) {
		bool ch = false;
		ImGui::TextDisabled("同一组内勾选的过滤器为“或”关系（显示匹配任一项的行）。");
		ImGui::SeparatorText("脚本");
		ch |= ImGui::Checkbox("无父级的脚本", &F.noParent);
		ch |= ImGui::Checkbox("父级缺失的脚本", &F.missingParent);
		ImGui::SeparatorText("脚本实例");
		ch |= ImGui::Checkbox("无成员实例（脚本有变量，实例没有）", &F.memberless);
		ch |= ImGui::Checkbox("归零的 canary（::iPapyrusDataVerification_var = 0）", &F.zeroedCanary);
		ch |= ImGui::Checkbox("带脚本的表单（变更表单上的实例／带脚本的变更表单）", &F.withScripts);
		ImGui::SeparatorText("字符串");
		ch |= ImGui::Checkbox("长字符串（512 个字符或更多）", &F.longStrings);
		ImGui::SeparatorText("变更表单");
		ch |= ImGui::Checkbox("含空项的 FormList", &F.nullLists);
		ch |= ImGui::Checkbox("Cell(-1) 变更表单（被移到已删除的单元格）", &F.cellMinus1);
		ch |= ImGui::Checkbox("空 REFR（无变更但初始数据为空）", &F.emptyRef);
		const char* levels[] = { "忽略", "完全解码", "部分解码", "未解码" };
		ImGui::SetNextItemWidth(160);
		ch |= ImGui::Combo("解码状态", &F.parseLevel, levels, 4);
		ImGui::TextUnformatted("变更标志：掩码");
		ImGui::SameLine();
		ch |= HexInput("##fm", F.flagMask);
		ImGui::SameLine();
		ImGui::TextUnformatted("值");
		ImGui::SameLine();
		ch |= HexInput("##fv", F.flagValue);
		ImGui::TextUnformatted("表单标志：掩码");
		ImGui::SameLine();
		ch |= HexInput("##ffm", F.formFlagMask);
		ImGui::SameLine();
		ImGui::TextUnformatted("值");
		ImGui::SameLine();
		ch |= HexInput("##ffv", F.formFlagValue);
		ImGui::TextDisabled("当（flags & mask）==（value & mask）时该行通过；掩码为 0 即关闭。");
		ImGui::SetNextItemWidth(420);
		ch |= ImGui::InputTextWithHint("##content", "内容：例如 EXTRADATA/Ownership；INITIAL/CELL/0000003C", F.content, sizeof(F.content));
		ImGui::TextDisabled("内容：“;” 表示与，“,” 表示或，“!” 表示非；以值或字段名结尾的字段路径。");
		ImGui::Separator();
		if (ImGui::Button("清除所有过滤器")) {
			F = Filters{};
			ch = true;
		}
		if (ch) {
			_rowsDirty = true;
		}
		ImGui::EndPopup();
	}
	return active;
}

// ==========================================================================================
// plugin actions (ReSaver's plugin context menu)
// ==========================================================================================

void App::pluginActions(const std::set<std::string>& a_plugins)
{
	if (a_plugins.empty()) {
		return;
	}
	const auto label = a_plugins.size() == 1 ? *a_plugins.begin() : Fmt("%zu 个插件", a_plugins.size());
	const auto both = ops::SelectPluginElements(*_save, a_plugins, true, true);
	if (ImGui::Button(Fmt("删除其变更表单（%zu）", both.changeForms.size()).c_str())) {
		confirm("删除 " + label + " 的变更表单",
			Fmt("删除 %zu 个表单的已保存状态（这些表单由 %s 定义）？\n\n下次加载时，这些表单会回到插件定义的状态。",
				both.changeForms.size(), label.c_str()),
			[this, a_plugins] {
				const auto s = ops::SelectPluginElements(*_save, a_plugins, true, false);
				ops::ApplyPluginPurge(*_save, s);
				markEdited(Fmt("已删除 %zu 个变更表单", s.changeForms.size()));
			});
	}
	ImGui::SameLine();
	if (ImGui::Button(Fmt("删除其脚本实例（%zu）", both.instances.size()).c_str())) {
		confirm("删除 " + label + " 的脚本实例",
			Fmt("删除 %zu 个脚本实例（位于 %s 的表单上），以及运行在其上的 %zu 个线程？", both.instances.size(), label.c_str(),
				both.threads.size()),
			[this, a_plugins] {
				const auto s = ops::SelectPluginElements(*_save, a_plugins, false, true);
				ops::ApplyPluginPurge(*_save, s);
				markEdited(Fmt("已删除 %zu 个实例和 %zu 个线程", s.instances.size(), s.threads.size()));
			});
	}
	if (ImGui::Button(Fmt("清除 %s（全部）", label.c_str()).c_str())) {
		confirm("清除 " + label,
			Fmt("删除 %zu 个变更表单、%zu 个脚本实例和 %zu 个线程（属于 %s）？\n\n若要安全地移除单个模组，请使用 "
				"“模组 > 从存档移除模组”。",
				both.changeForms.size(), both.instances.size(), both.threads.size(), label.c_str()),
			[this, a_plugins] {
				const auto s = ops::SelectPluginElements(*_save, a_plugins, true, true);
				ops::ApplyPluginPurge(*_save, s);
				markEdited(Fmt("已清除 %zu 个变更表单、%zu 个实例、%zu 个线程", s.changeForms.size(), s.instances.size(), s.threads.size()));
			});
	}
	if (a_plugins.size() == 1) {
		ImGui::SameLine();
		if (ImGui::Button("为此插件设置过滤器")) {
			_pluginFilter = *a_plugins.begin();
			_cat = Cat::kChangeForms;
			_filter[0] = 0;
			_selected.clear();
			_focus.reset();
			_rowsDirty = true;
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("“变更表单”“脚本实例”和“线程”页随后只显示此插件的元素。");
		}
	}
}

std::set<std::string> App::selectedPlugins() const
{
	std::set<std::string> out;
	for (auto k : _selected) {
		const bool lite = k >= 0x10000;
		const auto i = lite ? k - 0x10000 : k;
		const auto& list = lite ? _save->plugins.lite : _save->plugins.full;
		if (i < list.size()) {
			out.insert(list[i]);
		}
	}
	return out;
}

void App::exportPluginList()
{
	auto suggested = _path;
	suggested.replace_filename(_path.stem().u8string() + u8"_plugins.txt");
	std::optional<std::filesystem::path> target =
		platform::HasNativeDialogs() ? platform::SaveFileDialog(suggested, "txt", "导出插件列表") : std::optional{ suggested };
	if (!target) {
		return;
	}
	std::ofstream f(*target, std::ios::binary | std::ios::trunc);
	f << "# Plugins of " << platform::ToUtf8(_path.filename()) << "\r\n";
	for (std::size_t i = 0; i < _save->plugins.full.size(); ++i) {
		f << Fmt("%02zX      ", i) << _save->plugins.full[i] << "\r\n";
	}
	for (std::size_t i = 0; i < _save->plugins.lite.size(); ++i) {
		f << Fmt("FE:%03zX  ", i) << _save->plugins.lite[i] << "\r\n";
	}
	_status = f ? "插件列表已导出到 " + platform::ToUtf8(target->filename()) : "无法写出插件列表";
}

// ==========================================================================================
// array editing (ReSaver: Add Entry / Remove Entry / Shift Up / Shift Down)
// ==========================================================================================

void App::arrayEditor(ArrayInfo& a)
{
	ImGui::SeparatorText("编辑元素");
	const int n = static_cast<int>(a.vars.size());
	if (_arrayIndex >= n) {
		_arrayIndex = n ? n - 1 : 0;
	}
	ImGui::SetNextItemWidth(110);
	ImGui::InputInt("元素编号", &_arrayIndex);
	_arrayIndex = std::clamp(_arrayIndex, 0, std::max(0, n - 1));
	ImGui::SameLine();
	ImGui::BeginDisabled(n == 0);
	if (ImGui::Button("移除")) {
		a.vars.erase(a.vars.begin() + _arrayIndex);
		markChanged(Fmt("已移除数组的第 %d 个元素", _arrayIndex));
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(_arrayIndex == 0);
	if (ImGui::Button("上移")) {
		std::swap(a.vars[_arrayIndex], a.vars[_arrayIndex - 1]);
		--_arrayIndex;
		markChanged("已上移一个数组元素");
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(_arrayIndex + 1 >= n);
	if (ImGui::Button("下移")) {
		std::swap(a.vars[_arrayIndex], a.vars[_arrayIndex + 1]);
		++_arrayIndex;
		markChanged("已下移一个数组元素");
	}
	ImGui::EndDisabled();
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(a.type == VType::kStructArray || a.type == VType::kStruct || a.type == VType::kNull);
	if (ImGui::Button("添加条目")) {
		// ArrayInfo::type is the element type in SE saves (Ref, String, Int, ...); array codes are mapped too
		auto t = a.type;
		if (IsArrayType(t)) {
			t = static_cast<VType>(static_cast<std::uint8_t>(t) - 10);
		}
		Variable v;
		v.type = t;
		switch (t) {
		case VType::kRef:
			v.refType = a.refType;
			break;
		case VType::kString:
			v.value = _save->papyrus()->internString("");
			break;
		case VType::kVariant:
			v.inner = std::make_unique<Variable>();
			break;
		default:
			break;
		}
		a.vars.push_back(std::move(v));
		_arrayIndex = static_cast<int>(a.vars.size()) - 1;
		markChanged("已添加一个数组元素（在上方编辑其值）");
	}
	ImGui::EndDisabled();
}

// ==========================================================================================
// arrays analysis (ReSaver: Analyze Arrays Block)
// ==========================================================================================

void App::drawArraysAnalysis()
{
	ImGui::SetNextWindowSize(ImVec2(820, 600), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("数组区块", &_showArrays)) {
		ImGui::End();
		return;
	}
	auto& p = *_save->papyrus();
	if (_arraysDirty) {
		_arraysDirty = false;
		_arrStats.clear();
		_arrBiggest.clear();
		_arrOrphans.clear();
		std::unordered_set<EID> held;
		auto                    hold = [&](const std::vector<Variable>& vars) {
            for (const auto& v : vars) {
                const Variable* x = v.type == VType::kVariant && v.inner ? v.inner.get() : &v;
                if (IsArrayType(x->type)) {
                    held.insert(x->value);
                }
            }
		};
		for (const auto& i : p.instances) {
			if (i.data) {
				hold(i.data->vars);
			}
		}
		for (const auto& r : p.references) {
			if (r.data) {
				hold(r.data->vars);
			}
		}
		for (const auto& a : p.arrays) {
			hold(a.vars);
		}
		for (const auto& t : p.activeScripts) {
			if (t.data) {
				for (const auto& f : t.data->frames) {
					hold(f.vars);
				}
			}
		}
		for (const auto* list : { &p.suspended1, &p.suspended2 }) {
			for (const auto& s : *list) {
				if (s.data) {
					hold(s.data->vars);
				}
			}
		}
		for (const auto& m : p.functionMessages) {
			if (m.data) {
				hold(m.data->vars);
			}
		}
		std::map<std::string, std::pair<std::size_t, std::size_t>> stats;
		for (const auto& a : p.arrays) {
			const auto t = IsRefType(a.type) ? p.str(a.refType) + "[]" : std::string(VTypeName(a.type));
			auto&      s = stats[t];
			++s.first;
			s.second += a.vars.size();
			_arrBiggest.push_back({ a.vars.size(), a.id });
			if (!held.contains(a.id)) {
				_arrOrphans.push_back(a.id);
			}
		}
		for (const auto& [t, s] : stats) {
			_arrStats.push_back({ t, s.first, s.second });
		}
		std::sort(_arrStats.begin(), _arrStats.end(), [](const auto& a, const auto& b) { return std::get<2>(a) > std::get<2>(b); });
		std::sort(_arrBiggest.rbegin(), _arrBiggest.rend());
		if (_arrBiggest.size() > 50) {
			_arrBiggest.resize(50);
		}
	}
	std::size_t total = 0;
	for (const auto& a : p.arrays) {
		total += a.vars.size();
	}
	ImGui::Text("%zu 个数组，共 %zu 个元素，%zu 个未被任何对象持有", p.arrays.size(), total, _arrOrphans.size());
	if (ImGui::Button("刷新")) {
		_arraysDirty = true;
	}
	if (!_arrOrphans.empty()) {
		ImGui::SameLine();
		if (ImGui::Button(Fmt("删除 %zu 个无人持有的数组", _arrOrphans.size()).c_str())) {
			const std::set<EID> ids(_arrOrphans.begin(), _arrOrphans.end());
			confirm("删除未被引用的数组",
				Fmt("删除 %zu 个没有任何实例、引用、数组、线程或排队调用引用的数组？", ids.size()), [this, ids] {
					markEdited(Fmt("已删除 %zu 个未被引用的数组", ops::DeleteArrays(*_save->papyrus(), ids)));
					_arraysDirty = true;
				});
		}
	}
	if (ImGui::BeginTabBar("arr")) {
		if (ImGui::BeginTabItem("按元素类型")) {
			if (ImGui::BeginTable("as", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV)) {
				ImGui::TableSetupColumn("类型");
				ImGui::TableSetupColumn("数组", ImGuiTableColumnFlags_WidthFixed, 90);
				ImGui::TableSetupColumn("元素", ImGuiTableColumnFlags_WidthFixed, 90);
				ImGui::TableHeadersRow();
				for (const auto& [t, n, e] : _arrStats) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(t.c_str());
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%zu", n);
					ImGui::TableSetColumnIndex(2);
					ImGui::Text("%zu", e);
				}
				ImGui::EndTable();
			}
			ImGui::EndTabItem();
		}
		auto idList = [&](const char* tab, const std::vector<EID>& ids, bool showSize) {
			if (ImGui::BeginTabItem(tab)) {
				ImGui::BeginChild("ids");
				for (const auto id : ids) {
					const auto* a = p.findArray(id);
					if (!a) {
						continue;
					}
					ImGui::PushID(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(id)));
					if (ImGui::SmallButton("跳转")) {
						select(Cat::kArrays, id);
					}
					ImGui::SameLine();
					const auto t = IsRefType(a->type) ? p.str(a->refType) + "[]" : std::string(VTypeName(a->type));
					ImGui::Text("%016" PRIX64 "  %s%s", id, t.c_str(), showSize ? Fmt("  %zu 个元素", a->vars.size()).c_str() : "");
					ImGui::PopID();
				}
				ImGui::EndChild();
				ImGui::EndTabItem();
			}
		};
		std::vector<EID> big;
		for (const auto& [n, id] : _arrBiggest) {
			big.push_back(id);
		}
		idList("最大的数组", big, true);
		idList("无人持有的数组", _arrOrphans, true);
		ImGui::EndTabBar();
	}
	ImGui::End();
}

// ==========================================================================================
// log, settings, support
// ==========================================================================================

void App::logStatus()
{
	auto add = [&](const std::string& s) {
		const auto now = std::time(nullptr);
		char       t[16];
		std::strftime(t, sizeof(t), "%H:%M:%S", std::localtime(&now));
		_log.push_back(std::string(t) + "  " + s);
		if (_log.size() > 5000) {
			_log.erase(_log.begin(), _log.begin() + 1000);
		}
	};
	if (_status != _lastLogged) {
		_lastLogged = _status;
		add(_status);
	}
	if (!_error.empty() && _error != _lastLoggedError) {
		_lastLoggedError = _error;
		add("错误：" + _error);
	}
}

void App::drawLog()
{
	ImGui::SetNextWindowSize(ImVec2(820, 420), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("日志", &_showLog)) {
		ImGui::End();
		return;
	}
	if (ImGui::Button("复制")) {
		std::string all;
		for (const auto& l : _log) {
			all += l + "\n";
		}
		ImGui::SetClipboardText(all.c_str());
	}
	ImGui::SameLine();
	if (ImGui::Button("清空")) {
		_log.clear();
	}
	ImGui::Separator();
	ImGui::BeginChild("log");
	for (const auto& l : _log) {
		ImGui::TextUnformatted(l.c_str());
	}
	if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
		ImGui::SetScrollHereY(1.0f);
	}
	ImGui::EndChild();
	ImGui::End();
}

void App::drawSettings()
{
	ImGui::SetNextWindowSize(ImVec2(680, 0), ImGuiCond_Appearing);
	if (!ImGui::Begin("设置", &_showSettings, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::End();
		return;
	}
	bool ch = false;
	ImGui::SeparatorText("游戏");
	{
		int g = _vr ? 1 : 0;
		const char* games[] = { "Skyrim Special Edition / Anniversary Edition", "Skyrim VR" };
		ImGui::SetNextItemWidth(380);
		if (ImGui::Combo("##game", &g, games, 2)) {
			setGame(g == 1);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("（会自动切换到你所打开存档所属的游戏）");
	}
	ImGui::SeparatorText("脚本来源与插件");
	ImGui::TextWrapped("模组文件夹：%s", _sourcesPath.empty() ? "（未设置）" : _sourcesPath.c_str());
	ImGui::TextDisabled("你的 Mod Organizer “mods” 文件夹、Vortex 的 staging 文件夹（或游戏文件夹：Vortex 就由它找到），\n"
						"或游戏的 Data 文件夹。用于脚本来源、脚本查看器、插件名称和移除模组。");
	if (ImGui::Button("选择模组文件夹…")) {
		if (platform::HasNativeDialogs()) {
			if (auto p = platform::PickFolderDialog("请选择你的 Mod Organizer “mods” 文件夹（或游戏的 Data 文件夹）")) {
				scanSources(*p);
			}
		} else {
			std::snprintf(_scanPathBuf, sizeof(_scanPathBuf), "%s", _sourcesPath.c_str());
			_scanPathPopup = true;
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("使用 ModOrganizer.ini…") && platform::HasNativeDialogs()) {
		if (auto p = platform::OpenFileDialog({}, "ini", "选择 ModOrganizer.ini")) {
			if (auto m = ReadMo2Ini(*p)) {
				scanSources(m->modsDir);
			} else {
				_error = "无法读取 " + platform::ToUtf8(*p);
			}
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("自动检测")) {
		autoDetectPaths(true);
	}
	if (!_detectNote.empty()) {
		ImGui::TextDisabled("%s", _detectNote.c_str());
	}
	ImGui::TextWrapped("游戏文件夹：%s", gameFolder().empty() ? "（未找到）" : platform::ToUtf8(gameFolder()).c_str());
	ImGui::SameLine();
	if (ImGui::SmallButton("更改…##game") && platform::HasNativeDialogs()) {
		if (auto p = platform::PickFolderDialog(_vr ? "选择 Skyrim VR 文件夹（含 SkyrimVR.exe 的那个）" : "选择 Skyrim Special Edition 文件夹（含 SkyrimSE.exe 的那个）")) {
			_gamePath = platform::ToUtf8(*p);
			ch = true;
		}
	}
	{
		const auto comp = compilerPath();
		std::error_code ec;
		ImGui::TextDisabled("Papyrus 编译器：%s", !comp.empty() && std::filesystem::exists(comp, ec) ? platform::ToUtf8(comp).c_str()
																										: "未找到（它随 Creation Kit 一起提供）");
		ImGui::TextDisabled("存档：%s", platform::ToUtf8(savesFolder()).c_str());
	}
	// Mod Organizer instances installed for this user (%LOCALAPPDATA%\ModOrganizer\<instance>)
	if (const char* la = std::getenv("LOCALAPPDATA")) {
		std::error_code ec;
		const auto      root = platform::FromUtf8(la) / "ModOrganizer";
		for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
			const auto ini = e.path() / "ModOrganizer.ini";
			if (!e.is_directory(ec) || !std::filesystem::exists(ini, ec)) {
				continue;
			}
			if (auto m = ReadMo2Ini(ini); m && !m->modsDir.empty()) {
				const auto mods = platform::ToUtf8(m->modsDir);
				if (mods == _sourcesPath) {
					continue;
				}
				ImGui::PushID(mods.c_str());
				if (ImGui::SmallButton("使用")) {
					scanSources(m->modsDir);
				}
				ImGui::SameLine();
				ImGui::TextDisabled("Mod Organizer “%s”：%s", platform::ToUtf8(e.path().filename()).c_str(), mods.c_str());
				ImGui::PopID();
			}
		}
	}
	if (!_vortex && !_mo2 && !gameFolder().empty()) {
		std::error_code ec;
		if (std::filesystem::exists(gameFolder() / "Data" / "vortex.deployment.json", ec)) {
			TextColored(theme::Warn(), "游戏的 Data 文件夹中有 Vortex 的 vortex.deployment.json，但无法读取：模组会被归到 Data 文件夹。");
		}
	}
	if (_vortex && !_mo2) {
		ImGui::TextWrapped("Vortex：staging 文件夹 %s，%zu 个模组已部署到 %s（%s）", platform::ToUtf8(_vortex->stagingPath).c_str(), _vortex->mods.size(),
			platform::ToUtf8(_vortex->dataPath).c_str(), _vortex->method.empty() ? "?" : _vortex->method.c_str());
	}
	if (_mo2) {
		ImGui::TextWrapped("Mod Organizer：%s", platform::ToUtf8(_mo2->ini).c_str());
		if (ImGui::BeginCombo("MO2 配置", _profile.c_str())) {
			for (const auto& pr : _mo2->profiles) {
				if (ImGui::Selectable(pr.c_str(), pr == _profile)) {
					_profile = pr;
					ch = true;
					if (!_sourcesPath.empty()) {
						scanSources(platform::FromUtf8(_sourcesPath));
					}
				}
			}
			ImGui::EndCombo();
		}
	}
	if (!_sourcesPath.empty() && ImGui::Button("重新扫描模组文件夹")) {
		scanSources(platform::FromUtf8(_sourcesPath));
	}
	ch |= ImGui::Checkbox("自动读取插件名称", &_autoNames);
	ImGui::SeparatorText("游戏文本语言");
	{
		struct Cp
		{
			unsigned    cp;
			const char* name;
		};
		static const Cp cps[] = { { 0, "自动" }, { 65001, "UTF-8" }, { 1250, "中欧（波兰语、捷克语、匈牙利语…）1250" },
			{ 1251, "西里尔文（俄语、乌克兰语…）1251" }, { 1252, "西欧 1252" }, { 1253, "希腊语 1253" }, { 1254, "土耳其语 1254" },
			{ 1257, "波罗的海 1257" }, { 1258, "越南语 1258" }, { 874, "泰语 874" }, { 932, "日语 932" }, { 936, "简体中文 936" },
			{ 949, "韩语 949" }, { 950, "繁体中文 950" } };
		std::string current = "自动";
		for (const auto& c : cps) {
			if (c.cp == text::CodePage()) {
				current = c.name;
			}
		}
		if (text::CodePage() == 0) {
			current = Fmt("自动（本系统：%u）", text::EffectiveCodePage());
		}
		ImGui::SetNextItemWidth(420);
		if (ImGui::BeginCombo("文本编码", current.c_str())) {
			for (const auto& c : cps) {
				if (ImGui::Selectable(c.name, c.cp == text::CodePage())) {
					text::SetCodePage(c.cp);
					_rowsDirty = true;
					ch = true;
				}
			}
			ImGui::EndCombo();
		}
		ImGui::TextDisabled("存档中的名称和地点在不是 UTF-8 时按此编码显示。\n"
							"更改后，日文、中文、韩文和泰文字符需要重启（字体）。");
	}
	ImGui::SeparatorText("保存");
	const char* comp[] = { "保持存档原有的压缩", "LZ4（游戏默认）", "zlib", "不压缩" };
	int         c = _saveCompression + 1;
	if (ImGui::Combo("写入时的压缩方式", &c, comp, 4)) {
		_saveCompression = c - 1;
		ch = true;
	}
	ImGui::SeparatorText("其他");
	bool watch = _watch;
	{
		int k = static_cast<int>(theme::Current());
		const char* names[] = { "浅色", "灰色", "深色" };
		ImGui::SetNextItemWidth(160);
		if (ImGui::Combo("主题", &k, names, 3)) {
			setTheme(k);
		}
	}
	if (ImGui::Checkbox("监视存档文件夹（自动打开新存档）", &watch)) {
		setWatching(watch);
	}
	const char* sorts[] = { "存档顺序", "字母顺序", "大小", "总量" };
	if (ImGui::Combo("列表排序", &_sortMode, sorts, 4)) {
		_rowsDirty = true;
		ch = true;
	}
	ImGui::TextDisabled("设置保存在 %s", platform::ToUtf8(platform::SettingsFile()).c_str());
	if (ch) {
		saveSettings();
	}
	ImGui::End();
}

void App::drawSupport()
{
	ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
	if (!ImGui::Begin("支持", &_showSupport, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::End();
		return;
	}
	ImGui::TextWrapped("ReSaver++ 是 Mark Fairchild 的 FallrimTools ReSaver 的 C++ 移植版。以下是他的支持链接，"
					   "与 ReSaver 自带的“支持”对话框中所列的一致：");
	ImGui::Spacing();
	const char* links[][2] = { { "Patreon", "https://www.patreon.com/user?u=18285974" }, { "PayPal", "https://www.paypal.me/MarkDFSoftware" },
		{ "Nexus Mods", "https://www.nexusmods.com/users/723902" } };
	for (const auto& l : links) {
		if (ImGui::Button(l[0], ImVec2(120, 0))) {
			platform::OpenWithDefaultApp(platform::FromUtf8(l[1]));
		}
		ImGui::SameLine();
		ImGui::TextDisabled("%s", l[1]);
	}
	ImGui::End();
}

// ==========================================================================================
// scripts (.pex): ReSaver opens a dropped .pex and shows its disassembly
// ==========================================================================================

void App::openScript(const std::filesystem::path& a_path)
{
	viewScriptFile(a_path);  // the script viewer decompiles it (AppScripts.cpp)
}

void App::openAny(const std::filesystem::path& a_path)
{
	const auto ext = Lower(platform::ToUtf8(a_path.extension()));
	if (ext == ".pex" || ext == ".psc") {
		viewScriptFile(a_path);
	} else if (Lower(platform::ToUtf8(a_path.filename())) == "modorganizer.ini") {
		// ReSaver: dropping ModOrganizer.ini selects that MO2 instance
		if (auto m = ReadMo2Ini(a_path)) {
			scanSources(m->modsDir);
		} else {
			_error = "无法读取 " + platform::ToUtf8(a_path);
		}
	} else {
		open(a_path);
	}
}
