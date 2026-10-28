// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// 本地化修改：界面、提示与报告文字改为简体中文（详见 README.zh-CN.md）。
// The rest of ReSaver's features: filters, plugin actions, array editing, arrays analysis,
// plugin list export, log, settings, support links, script (.pex) viewer.
#include "App.h"
#include <algorithm>

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "core/Text.h"

#include "imgui.h"

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
	tips::Tip("把列表缩窄到有某种问题或内容的行为（ReSaver 的过滤器菜单）。标题带 * 表示有过滤器生效。");
	if (ImGui::BeginPopup("filters")) {
		bool ch = false;
		ImGui::TextDisabled("同一组内勾选的过滤器为“或”关系（显示匹配任一项的行）。");
		ImGui::SeparatorText("脚本");
		ch |= ImGui::Checkbox("无父级的脚本", &F.noParent);
		tips::Tip("父脚本（它继承的脚本）不在这个存档里的脚本。通常无害。");
		ch |= ImGui::Checkbox("父级缺失的脚本", &F.missingParent);
		tips::Tip("继承了某个已不在存档里的脚本：常是卸载模组后留下的。");
		ImGui::SeparatorText("脚本实例");
		ch |= ImGui::Checkbox("无成员实例（脚本有变量，实例没有）", &F.memberless);
		tips::Tip("脚本本身有变量、实例却一个都没有的脚本实例：它们的数据丢了。常见于模组更新或卸载之后。");
		ch |= ImGui::Checkbox("归零的 canary（::iPapyrusDataVerification_var = 0）", &F.zeroedCanary);
		tips::Tip("::iPapyrusDataVerification_var 校验值为 0 的实例，说明游戏读档时丢弃了它们的数据（模组被改过或已损坏）。");
		ch |= ImGui::Checkbox("带脚本的表单（变更表单上的实例／带脚本的变更表单）", &F.withScripts);
		tips::Tip("上面挂着脚本实例的变更表单，以及变更表单上的脚本实例。");
		ImGui::SeparatorText("字符串");
		ch |= ImGui::Checkbox("长字符串（512 个字符或更多）", &F.longStrings);
		tips::Tip("非常长的脚本字符串。巨型字符串常来自用循环拼文本的模组，会把存档撑大。");
		ImGui::SeparatorText("变更表单");
		ch |= ImGui::Checkbox("含空项的 FormList", &F.nullLists);
		tips::Tip("含空项的 FormList（FLST）：来自已移除模组的表单。清理 > 净化 FormList 可以删掉它们。");
		ch |= ImGui::Checkbox("Cell(-1) 变更表单（被移到已删除的单元格）", &F.cellMinus1);
		tips::Tip("被移到已删除（无效）单元格的东西。它们已经丢了，通常可以删除。");
		ch |= ImGui::Checkbox("空 REFR（无变更但初始数据为空）", &F.emptyRef);
		tips::Tip("没有保存任何变更、只有空初始数据的对象引用：无害的残留。");
		const char* levels[] = { "忽略", "完全解码", "部分解码", "未解码" };
		ImGui::SetNextItemWidth(160);
		ch |= ImGui::Combo("解码状态", &F.parseLevel, levels, 4);
		tips::Tip("只显示 ReSaver++ 完全解码、部分解码或未解码的变更表单。部分或未解码的不会被编辑改动。");
		ImGui::TextUnformatted("变更标志：掩码");
		ImGui::SameLine();
		ch |= HexInput("##fm", F.flagMask);
		tips::Tip("变更标志掩码：要测试哪些标志位（十六进制）。当（flags & mask）==（value & mask）时该行通过。0 表示关闭。");
		ImGui::SameLine();
		ImGui::TextUnformatted("值");
		ImGui::SameLine();
		ch |= HexInput("##fv", F.flagValue);
		tips::Tip("变更标志值：被掩码的标志必须等于的位（十六进制）。");
		ImGui::TextUnformatted("表单标志：掩码");
		ImGui::SameLine();
		ch |= HexInput("##ffm", F.formFlagMask);
		tips::Tip("表单标志掩码：要测试哪些记录标志位（十六进制）。0 表示关闭。");
		ImGui::SameLine();
		ImGui::TextUnformatted("值");
		ImGui::SameLine();
		ch |= HexInput("##ffv", F.formFlagValue);
		tips::Tip("表单标志值：被掩码的表单标志必须等于的位（十六进制）。");
		ImGui::TextDisabled("当（flags & mask）==（value & mask）时该行通过；掩码为 0 即关闭。");
		ImGui::SetNextItemWidth(420);
		ch |= ImGui::InputTextWithHint("##content", "内容：例如 EXTRADATA/Ownership；INITIAL/CELL/0000003C", F.content, sizeof(F.content));
		tips::Tip("按字段路径在已解码的变更表单里搜索，例如 EXTRADATA/Ownership。“;” 表示与，“,” 表示或，“!” 表示非。");
		ImGui::TextDisabled("内容：“;” 表示与，“,” 表示或，“!” 表示非；以值或字段名结尾的字段路径。");
		ImGui::Separator();
		if (ImGui::Button("清除所有过滤器")) {
			F = Filters{};
			ch = true;
		}
		tips::Tip("关掉所有过滤器，重新显示全部行。");
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
	tips::Tip("删除这个插件定义的每个表单的已保存状态。下次读档时它们会回到插件定义的样子。");
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
	tips::Tip("删除这个插件表单上的脚本实例，以及跑在它们上面的线程。若模组仍安装着，它的脚本会重新开始。");
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
	tips::Tip("同时删除它的变更表单和脚本实例。要带安全检查地移除整个模组，请用「模组 > 从存档移除模组」。");
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
		tips::Tip("“变更表单”“脚本实例”和“线程”页随后只显示此插件的元素。");
	}
}

// The change forms, script instances and threads a plugin's buttons above would delete, so they can be looked
// through first: in collapsible groups (change forms by type, instances by script), each in form ID order;
// clicking one opens it in its own list.
void App::pluginContents(const std::set<std::string>& a_plugins)
{
	auto& pc = _plContents;
	if (pc.plugins != a_plugins || pc.gen != _editGen || pc.save != _save.get()) {
		pc = PluginContents{};
		pc.plugins = a_plugins;
		pc.gen = _editGen;
		pc.save = _save.get();
		const auto sel = ops::SelectPluginElements(*_save, a_plugins, true, true);

		// change forms, by type
		std::map<std::string, std::vector<std::pair<std::uint32_t, PluginContents::Row>>> forms;
		for (const auto& c : _save->changeForms) {
			if (!sel.changeForms.count(c.refID.data)) {
				continue;
			}
			const auto  fid = _save->formIDOf(c.refID);
			std::string name = _db ? _db->describe(fid) : std::string();
			forms[ChangeFormTypeName(c.typeCode())].push_back({ fid, { c.refID.data, Fmt("%08X", fid), Fmt("%u 字节", static_cast<unsigned>(c.data.size())), std::move(name) } });
		}
		for (auto& [type, rows] : forms) {
			std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
			auto& g = pc.forms.emplace_back(type, std::vector<PluginContents::Row>{});
			for (auto& r : rows) {
				g.second.push_back(std::move(r.second));
			}
			pc.nForms += g.second.size();
		}

		// script instances, by script
		if (const auto* p = _save->papyrus()) {
			std::map<std::string, std::vector<std::pair<std::uint32_t, PluginContents::Row>>> inst;
			for (const auto& i : p->instances) {
				if (!sel.instances.count(i.id)) {
					continue;
				}
				std::string text = "on  " + formLabel(i.refID);
				if (ops::IsUnattached(i)) {
					text += "  [UNATTACHED]";
				}
				inst[p->str(i.script)].push_back(
					{ _save->formIDOf(i.refID), { static_cast<std::uint64_t>(i.id), std::move(text), {}, {} } });
			}
			for (auto& [script, rows] : inst) {
				std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
				auto& g = pc.instances.emplace_back(script, std::vector<PluginContents::Row>{});
				for (auto& r : rows) {
					g.second.push_back(std::move(r.second));
				}
				pc.nInstances += g.second.size();
			}
			for (const auto& a : p->activeScripts) {
				if (sel.threads.count(a.id)) {
					pc.threads.push_back({ static_cast<std::uint64_t>(a.id), threadLabel(a), {}, {} });
				}
			}
			std::sort(pc.threads.begin(), pc.threads.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
		}
	}

	ImGui::Separator();
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("上面那些按钮会删掉的内容。展开分组可以逐项查看；点一行即可打开它。");
	ImGui::PopStyleColor();
	if (!ImGui::BeginChild("##plcontents", ImVec2(0, 0), ImGuiChildFlags_None)) {
		ImGui::EndChild();
		return;
	}
	Cat target = Cat::kChangeForms;
	std::optional<std::uint64_t> go;
	// screenshots: RESAVERPP_TEST_PLOPEN=1 opens everything, =2 everything but the change forms
	static const char* testOpen = std::getenv("RESAVERPP_TEST_PLOPEN");
	bool               skipForms = testOpen && testOpen[0] == '2';
	auto open = [&] {
		if (testOpen && !skipForms) {
			ImGui::SetNextItemOpen(true, ImGuiCond_Once);
		}
	};
	auto rows = [&](const std::vector<PluginContents::Row>& a_rows, Cat a_cat) {
		ImGuiListClipper clip;
		clip.Begin(static_cast<int>(a_rows.size()));
		while (clip.Step()) {
			for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
				const auto& row = a_rows[r];
				ImGui::PushID(r);
				const float x = ImGui::GetCursorPosX();
				if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap)) {
					go = row.key;
					target = a_cat;
				}
				tips::Tip("点击在对应的列表里打开它");
				ImGui::SameLine(x);
				ImGui::TextUnformatted(row.text.c_str());
				float next = x + ImGui::CalcTextSize(row.text.c_str()).x + ImGui::GetFontSize();
				if (!row.right.empty()) {
					const float col = x + ImGui::CalcTextSize("00000000").x + ImGui::CalcTextSize("0000000 bytes").x + ImGui::GetFontSize();
					ImGui::SameLine(col - ImGui::CalcTextSize(row.right.c_str()).x);
					ImGui::TextUnformatted(row.right.c_str());
					next = col + ImGui::GetFontSize();
				}
				if (!row.rest.empty()) {
					ImGui::SameLine(next);
					ImGui::TextUnformatted(row.rest.c_str());
				}
				ImGui::PopID();
			}
		}
	};
	auto groups = [&](const PluginContents::Groups& a_groups, Cat a_cat) {
		for (const auto& [name, list] : a_groups) {
			open();
			if (ImGui::TreeNodeEx(name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth, "%s  (%zu)", name.c_str(), list.size())) {
				rows(list, a_cat);
				ImGui::TreePop();
			}
		}
	};
	if (pc.nForms == 0 && pc.nInstances == 0 && pc.threads.empty()) {
		ImGui::TextDisabled("这个插件在存档里什么都没有。");
	}
	open();
	const bool tipOpen305 = pc.nForms && ImGui::CollapsingHeader(Fmt("变更表单（%zu）###plcf", pc.nForms).c_str());
	if (pc.nForms) {
		tips::Tip("这个插件定义的、在本存档中发生变更的表单。");
	}
	if (tipOpen305) {
		ImGui::PushID("cf");
		groups(pc.forms, Cat::kChangeForms);
		ImGui::PopID();
	}
	skipForms = false;
	open();
	const bool tipOpen312 = pc.nInstances && ImGui::CollapsingHeader(Fmt("脚本实例（%zu），按脚本分组###plsi", pc.nInstances).c_str());
	if (pc.nInstances) {
		tips::Tip("挂在这个插件表单上的脚本，按脚本分组。");
	}
	if (tipOpen312) {
		ImGui::PushID("si");
		groups(pc.instances, Cat::kInstances);
		ImGui::PopID();
	}
	open();
	const bool tipOpen318 = !pc.threads.empty() && ImGui::CollapsingHeader(Fmt("跑在它们上面的线程（%zu）###plth", pc.threads.size()).c_str());
	if (!pc.threads.empty()) {
		tips::Tip("你存档时正跑在这个插件表单上的脚本线程。");
	}
	if (tipOpen318) {
		ImGui::PushID("th");
		rows(pc.threads, Cat::kThreads);
		ImGui::PopID();
	}
	ImGui::EndChild();
	if (go) {
		if (!_pluginFilter.empty()) {
			_pluginFilter.clear();
			_rowsDirty = true;
		}
		select(target, *go);
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
	tips::Tip("按钮作用于数组的哪个元素（从 0 数起）。");
	_arrayIndex = std::clamp(_arrayIndex, 0, std::max(0, n - 1));
	ImGui::SameLine();
	ImGui::BeginDisabled(n == 0);
	if (ImGui::Button("移除")) {
		a.vars.erase(a.vars.begin() + _arrayIndex);
		markChanged(Fmt("已移除数组的第 %d 个元素", _arrayIndex));
	}
	tips::Tip("从数组里删除这个元素。按位置读数组的脚本会看到后面的元素往前挪。");
	ImGui::SameLine();
	ImGui::BeginDisabled(_arrayIndex == 0);
	if (ImGui::Button("上移")) {
		std::swap(a.vars[_arrayIndex], a.vars[_arrayIndex - 1]);
		--_arrayIndex;
		markChanged("已上移一个数组元素");
	}
	tips::Tip("把这个元素和前一个交换位置。");
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(_arrayIndex + 1 >= n);
	if (ImGui::Button("下移")) {
		std::swap(a.vars[_arrayIndex], a.vars[_arrayIndex + 1]);
		++_arrayIndex;
		markChanged("已下移一个数组元素");
	}
	tips::Tip("把这个元素和后一个交换位置。");
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
	tips::Tip("在数组末尾添加一个该类型的空元素。");
	ImGui::EndDisabled();
}

// ==========================================================================================
// arrays analysis (ReSaver: Analyze Arrays Block)
// ==========================================================================================

void App::drawArraysAnalysis()
{
	prepareWindow("Arrays block", ImVec2(0.7f, 0.75f));
	if (!ImGui::Begin("数组区块", &_showArrays, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Arrays block");
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
	tips::Tip("重新统计数组，并找出已经没有任何东西引用的那些。");
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
		tips::Tip("删除没有任何脚本实例、引用、数组、线程或排队调用指向的数组。它们只是存档里的死重量。");
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
					tips::Tip("在「数组」列表里打开这个数组。");
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
	prepareWindow("Log", ImVec2(0.7f, 0.75f));
	if (!ImGui::Begin("日志", &_showLog, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Log");
	if (ImGui::Button("复制")) {
		std::string all;
		for (const auto& l : _log) {
			all += l + "\n";
		}
		ImGui::SetClipboardText(all.c_str());
	}
	tips::Tip("把整个日志复制到剪贴板（例如贴进错误报告里）。");
	ImGui::SameLine();
	if (ImGui::Button("清空")) {
		_log.clear();
	}
	tips::Tip("清空日志。");
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
		const char* games[] = { "Skyrim SE/AE", "Skyrim VR" };
		ImGui::SetNextItemWidth(200);
		if (ImGui::Combo("##game", &g, games, 2)) {
			setGame(g == 1);
		}
		tips::Tip("下面这些文件夹属于哪个游戏。会自动切换到你所打开存档所属的游戏。");
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
	tips::Tip("选择你的 Mod Organizer “mods” 文件夹、Vortex 的 staging 文件夹或游戏的 Data 文件夹。ReSaver++ 从那里读取脚本和插件名称。");
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
	tips::Tip("选择一个 ModOrganizer.ini：会使用它的 mods 文件夹和配置（对便携版 MO2 很方便）。");
	ImGui::SameLine();
	if (ImGui::Button("自动检测")) {
		autoDetectPaths(true);
	}
	tips::Tip("在常见位置寻找 Mod Organizer、Vortex 和游戏，找到什么就用什么。");
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
	tips::Tip("选择游戏文件夹（含 SkyrimSE.exe 或 SkyrimVR.exe 的那个）。Papyrus 编译器和 Data 文件夹需要它。");
	{
		const auto comp = compilerPath();
		std::error_code ec;
		ImGui::TextDisabled("Papyrus 编译器：%s", !comp.empty() && std::filesystem::exists(comp, ec) ? platform::ToUtf8(comp).c_str()
																										: "未找到（它随 Creation Kit 一起提供）");
	}
	{
		auto& custom = savesDirSetting();
		ImGui::TextWrapped("存档文件夹：%s", platform::ToUtf8(savesFolder()).c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton("更改…##saves") && platform::HasNativeDialogs()) {
			if (auto p = platform::PickFolderDialog("选择存放存档的文件夹（「文件 > 打开」会从这里开始）")) {
				custom = platform::ToUtf8(*p);
				ch = true;
				if (_watch) {
					setWatching(true);
				}
			}
		}
		tips::Tip("选择「文件 > 打开」的起始文件夹，也是「监视存档文件夹」监视的文件夹。");
		if (!custom.empty()) {
			ImGui::SameLine();
			if (ImGui::SmallButton("自动查找##saves")) {
				custom.clear();
				ch = true;
				if (_watch) {
					setWatching(true);
				}
			}
			tips::Tip("忘掉你选的文件夹，重新自动查找存档文件夹。");
		}
		ImGui::TextDisabled(custom.empty() ? "自动查找：上次打开存档所在的文件夹；否则是有最新存档的那个\n"
											 "（Documents\\My Games、Skyrim.ini 的 sLocalSavePath、Mod Organizer 配置里的 saves）。"
										   : "由你指定：「文件 > 打开」从这里开始，「监视存档文件夹」也监视它。");
	}
	// Mod Organizer instances installed for this user (%LOCALAPPDATA%\ModOrganizer\<instance>)
	if (const auto la = text::EnvPath("LOCALAPPDATA"); !la.empty()) {
		std::error_code ec;
		const auto      root = la / "ModOrganizer";
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
				tips::Tip("使用这个 Mod Organizer 实例的 mods 文件夹。");
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
		const bool tipOpen767 = ImGui::BeginCombo("MO2 配置", _profile.c_str());
		tips::Tip("使用哪个 Mod Organizer 配置的加载顺序和已启用模组。");
		if (tipOpen767) {
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
	if (!_sourcesPath.empty()) {
		tips::Tip("重新读取模组文件夹（安装或移除模组之后）。");
	}
	ch |= ImGui::Checkbox("自动读取插件名称", &_autoNames);
	tips::Tip("扫描之后读取你的插件，表单就能显示名称和编辑器 ID。加载顺序很大时要花几秒。");
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
		const bool tipOpen805 = ImGui::BeginCombo("文本编码", current.c_str());
		tips::Tip("显示非英文游戏保存的名称时用的编码页。自动 = 用你系统的。");
		if (tipOpen805) {
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
	tips::Tip("ReSaver++ 写出存档时如何压缩。游戏用的是 LZ4；不压缩的存档更大，但读起来一样。");
	ImGui::SeparatorText("实时游戏（ReSaverHelper.dll）");
	ImGui::TextDisabled("游戏里装上可选的 ReSaverHelper.dll 后，任务页就能在运行中的游戏里完成任务、设置阶段、\n"
						"刷新任务日志。没装也不影响其它功能。");
	if (ImGui::Checkbox("连接到正在运行的游戏", &_liveEnabled)) {
		ch = true;
	}
	tips::Tip("和运行中游戏里的 ReSaverHelper.dll 通信，就能从这里完成任务、设置阶段、刷新日志。");
	{
		const char* modes[] = { "左上角通知", "消息框", "不提示" };
		ImGui::SetNextItemWidth(260);
		if (ImGui::Combo("游戏用什么方式告诉你结果", &_liveNotify, modes, 3)) {
			ch = true;
		}
		tips::Tip("游戏用什么方式告诉你 ReSaver++ 做了什么：通知、消息框，或者不提示。");
	}
	{
		// ReSaverHelper's own messages are English, so they are silenced and ReSaver++ sends its own (Chinese).
		bool helperText = !_liveSilentRefresh;
		if (ImGui::Checkbox("读档自动刷新时也提示（英文）", &helperText)) {
			_liveSilentRefresh = !helperText;
			ch = true;
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("默认关：ReSaverHelper 自己的提示是英文，关掉后游戏里的提示都由 ReSaver++ 发（中文）。\n"
							  "勾上会保留它自己的英文提示（主要是读档后自动刷新的那几句）。");
		}
	}
	drawRefreshJournal(true);
	drawLiveStatus();
	ImGui::SeparatorText("其他");
	bool watch = _watch;
	{
		int k = static_cast<int>(theme::Current());
		const char* names[] = { "浅色", "灰色", "深色" };
		ImGui::SetNextItemWidth(160);
		if (ImGui::Combo("主题", &k, names, 3)) {
			setTheme(k);
		}
		tips::Tip("ReSaver++ 的配色。");
	}
	if (ImGui::Checkbox("监视存档文件夹（自动打开新存档）", &watch)) {
		setWatching(watch);
	}
	tips::Tip("游戏写出新存档时立刻打开它。");
	ch |= ImGui::Checkbox("启动 ReSaver++ 时打开最新的存档", &_openNewest);
	tips::Tip("ReSaver++ 启动时自动打开你最新的存档。");
	const char* sorts[] = { "存档顺序", "字母顺序", "大小", "总量" };
	if (ImGui::Combo("列表排序", &_sortMode, sorts, 4)) {
		_rowsDirty = true;
		ch = true;
	}
	tips::Tip("列表如何排序：按存档里的顺序、按名称、按自身大小，或按总量（含它包含的东西）。");
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
		tips::Tip("在浏览器里打开这个支持链接。");
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
