// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// 本地化修改：界面、提示与报告文字改为简体中文（详见 README.zh-CN.md）。
#include "App.h"

#include "Platform.h"
#include "Theme.h"

#include "core/Text.h"

#include "imgui.h"

#include <algorithm>
#include <thread>
#include <chrono>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

using namespace ess;

namespace
{

	const char* CatName(App::Cat c)
	{
		switch (c) {
		case App::Cat::kOverview: return "概览";
		case App::Cat::kMods: return "模组";
		case App::Cat::kPlugins: return "插件";
		case App::Cat::kChangeForms: return "变更表单";
		case App::Cat::kStrings: return "字符串";
		case App::Cat::kScripts: return "脚本";
		case App::Cat::kInstances: return "脚本实例";
		case App::Cat::kReferences: return "引用";
		case App::Cat::kArrays: return "数组";
		case App::Cat::kThreads: return "活动脚本（线程）";
		case App::Cat::kMessages: return "函数消息";
		case App::Cat::kSuspended: return "挂起堆栈";
		case App::Cat::kUnbinds: return "排队解绑";
		case App::Cat::kQuests: return "任务";
		default: return "?";
		}
	}

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

	std::string Hex64(std::uint64_t v) { return Fmt("%016" PRIX64, v); }

	void TextColored(std::uint32_t col, const std::string& s)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, col);
		ImGui::TextUnformatted(s.c_str());
		ImGui::PopStyleColor();
	}

	void KV(const char* k, const std::string& v)
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted(k);
		ImGui::TableSetColumnIndex(1);
		ImGui::TextWrapped("%s", v.c_str());
	}

	bool BeginKV(const char* id)
	{
		if (ImGui::BeginTable(id, 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 170.0f);
			ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
			return true;
		}
		return false;
	}
}

// ==========================================================================================
// file handling
// ==========================================================================================

void App::open(const std::filesystem::path& a_path)
{
	++_editGen;
	try {
		auto s = SaveFile::load(a_path);
		if (!s->papyrus()) {
			throw std::runtime_error("存档中没有 Papyrus 区块");
		}
		_save = std::move(s);
		// a Skyrim VR save lists SkyrimVR.esm: use the VR setup (mods, load order, saves)
		{
			bool vrSave = false;
			for (const auto& p : _save->plugins.full) {
				vrSave = vrSave || Lower(p) == "skyrimvr.esm";
			}
			if (!_settingsLoaded) {
				loadSettings();
			}
			setGame(vrSave);
		}
		_cfFacts.clear();
		_missingDismissed = false;
		_path = a_path;
		if (const auto dir = platform::ToUtf8(a_path.parent_path()); !dir.empty() && dir != lastOpenDir()) {
			lastOpenDir() = dir;  // File > Open starts here next time
			saveSettings();
		}
		_dirty = false;
		_report.reset();
		_selected.clear();
		_focus.reset();
		_rowsDirty = true;
		_cat = Cat::kOverview;
		if (_shotTex && freeTexture) {
			freeTexture(_shotTex);
			_shotTex = 0;
		}
		const auto& h = _save->header;
		if (uploadTexture && h.shotWidth && h.shotHeight && h.screenshot.size() >= std::size_t(h.shotWidth) * h.shotHeight * 4) {
			std::vector<std::uint8_t> px = h.screenshot;
			for (std::size_t i = 3; i < px.size(); i += 4) {
				px[i] = 255;
			}
			_shotTex = uploadTexture(px.data(), static_cast<int>(h.shotWidth), static_cast<int>(h.shotHeight));
			_shotW = static_cast<int>(h.shotWidth);
			_shotH = static_cast<int>(h.shotHeight);
		}
		_report = std::make_unique<ops::Report>(ops::Analyze(*_save));
		_modsDirty = true;
		if (!_settingsLoaded) {
			loadSettings();
		}
		if (!_sourcesPath.empty() && !_sources.scanned()) {
			scanSources(platform::FromUtf8(_sourcesPath));  // checks again for missing mods when done
		} else if (_autoNames && _sources.scanned()) {
			startNames();
		}
		// the load order is known now: the banner shows at once (which mods are still installed is
		// known when the scan of the mods is done; that can take a while inside Mod Organizer)
		checkMissingPlugins();
		_status = "已加载 " + platform::ToUtf8(a_path.filename());
		// a save whose structure does not add up may not be written back faithfully: look, don't save
		_readOnly = !_save->problems.empty();
		if (_readOnly) {
			std::string list;
			for (const auto& pr : _save->problems) {
				list += "\n  - " + pr;
			}
			_error = "这个存档已损坏，或采用了 ReSaver++ 还不能完全理解的布局：\n" + list +
					 "\n\n它已按只读方式打开：所有内容都能查看，但无法从 ReSaver++ 保存（写回可能丢失数据，也可能凭空造出数据）。"
					 "请在游戏中读取一个更早的存档，或带上这个文件反馈问题。";
			_status += "  -  只读（存档损坏）";
		}
		if (!_report->warnings.empty()) {
			_status += Fmt("  -  %zu 个问题：见 工具 > 分析", _report->warnings.size());
		}
	} catch (const std::exception& e) {
		_error = std::string("无法打开 ") + platform::ToUtf8(a_path.filename()) + ":\n\n" + e.what();
	}
}

void App::doSave(const std::filesystem::path& a_path)
{
	if (!_save) {
		return;
	}
	if (_readOnly) {
		_error = "这个存档因为损坏而以只读方式打开（打开时有提示）；无法从 ReSaver++ 保存。";
		return;
	}
	try {
		if (_saveCompression >= 0) {
			_save->header.compression = static_cast<Compression>(_saveCompression);
		}
		// staged, read back, previous files kept as name.ess.bak + name.skse.bak (one generation each),
		// rolled back if the co-save cannot be written
		const auto wr = _save->write(a_path, true);
		// read it straight back: a save we cannot read is never left silently behind
		auto check = SaveFile::load(a_path);
		if (!check->papyrus()) {
			throw std::runtime_error("校验失败：写出的存档没有 Papyrus 区块");
		}
		_path = a_path;
		_save->path = a_path;
		_dirty = false;
		_status = "已保存并通过校验 " + platform::ToUtf8(a_path.filename()) +
				  (wr.backup.empty() ? std::string() : "  （原文件保留为 " + platform::ToUtf8(wr.backup.filename()) +
													   (wr.cosaveBackup.empty() ? "" : " + " + platform::ToUtf8(wr.cosaveBackup.filename())) + "）");
	} catch (const std::exception& e) {
		_error = std::string("保存失败：\n\n") + e.what();
	}
}

void App::startOpenDialog()
{
	auto start = _path.empty() ? savesFolder() : _path.parent_path();
	if (platform::HasNativeDialogs()) {
		if (auto p = platform::OpenSaveDialog(start)) {
			open(*p);
		}
	} else {
		std::snprintf(_pathBuf, sizeof(_pathBuf), "%s", platform::ToUtf8(start).c_str());
		_openPathPopup = true;
	}
}

void App::startSaveAsDialog()
{
	if (!_save) {
		return;
	}
	auto suggested = _path;
	suggested.replace_filename(_path.stem().u8string() + u8"_edited.ess");
	if (platform::HasNativeDialogs()) {
		if (auto p = platform::SaveAsDialog(suggested)) {
			doSave(*p);
		}
	} else {
		std::snprintf(_pathBuf, sizeof(_pathBuf), "%s", platform::ToUtf8(suggested).c_str());
		_savePathPopup = true;
	}
}

void App::requestQuit()
{
	if (_dirty) {
		confirm("未保存的修改", "修改后的存档尚未写入。仍要退出吗？", [this] { _quit = true; });
	} else {
		_quit = true;
	}
}

void App::confirm(const std::string& a_title, const std::string& a_text, std::function<void()> a_action)
{
	_confirmTitle = a_title;
	_confirmText = a_text;
	_confirmAction = std::move(a_action);
	_confirmOpen = true;
}

void App::markEdited(const std::string& a_what)
{
	++_editGen;
	_cfFacts.clear();
	_dirty = true;
	_rowsDirty = true;
	_selected.clear();
	_focus.reset();
	_report = std::make_unique<ops::Report>(ops::Analyze(*_save));
	_modsDirty = true;
	_status = a_what + "  （尚未保存：文件 > 另存为）";
}

void App::markChanged(const std::string& a_what)
{
	++_editGen;
	_cfFacts.clear();
	_dirty = true;
	_rowsDirty = true;
	_modsDirty = true;
	_status = a_what + "  （尚未保存：文件 > 另存为）";
}

void App::select(Cat a_cat, std::uint64_t a_key)
{
	if (_cat != a_cat) {
		_cat = a_cat;
		_filter[0] = 0;
		_rowsDirty = true;
	}
	_selected = { a_key };
	_focus = a_key;
	_scrollToFocus = true;
}

// ==========================================================================================
// labels
// ==========================================================================================

std::string App::formLabel(const RefID& a_id) const
{
	if (a_id.isZero()) {
		return "无";
	}
	const auto fid = _save->formIDOf(a_id);
	if (!fid) {
		return Fmt("RefID %06X（未解析）", a_id.data);
	}
	std::string s = Fmt("%08X (%s)", fid, _save->pluginOf(fid).c_str());
	if (_db) {
		const auto d = _db->describe(fid);
		if (!d.empty()) {
			s += " " + d;
		}
	}
	return s;
}

std::string App::instanceLabel(const ScriptInstance& i) const
{
	const auto& p = *_save->papyrus();
	std::string s = p.str(i.script) + "  于  " + formLabel(i.refID);
	if (ops::IsUnattached(i)) {
		s += "  [UNATTACHED]";
	}
	if (ops::IsInstanceUndefined(p, i.script)) {
		s += "  [UNDEFINED]";
	}
	return s;
}

std::string App::threadLabel(const ActiveScript& a) const
{
	const auto& p = *_save->papyrus();
	std::string s = Fmt("#%08X  ", a.id);
	if (!a.data || a.data->frames.empty()) {
		s += "（无栈帧）";
	} else {
		const auto& f0 = a.data->frames.front();
		const auto& fn = a.data->frames.back();
		s += p.str(f0.scriptName) + "." + p.str(f0.event);
		if (a.data->frames.size() > 1) {
			s += "  ->  " + p.str(fn.baseName) + "." + p.str(fn.event);
		}
	}
	if (ops::IsThreadTerminated(p, a)) {
		s += "  [TERMINATED]";
	} else if (ops::IsThreadSuspended(p, a.id)) {
		s += "  [SUSPENDED]";
	}
	if (ops::IsThreadUndefined(p, a)) {
		s += "  [UNDEFINED]";
	}
	return s;
}

std::vector<std::string> App::memberNames(TStr a_script) const
{
	// ReSaver's "extended members": the parent's members come first.
	const auto&              p = *_save->papyrus();
	std::vector<const Script*> chain;
	for (const Script* s = p.findScript(a_script); s && chain.size() < 32; s = p.findScript(s->parent)) {
		chain.push_back(s);
		if (p.str(s->parent).empty()) {
			break;
		}
	}
	std::vector<std::string> names;
	for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
		for (const auto& m : (*it)->members) {
			names.push_back(p.str(m.name));
		}
	}
	return names;
}

std::string App::describeVar(const Variable& v) const
{
	const auto& p = *_save->papyrus();
	switch (v.type) {
	case VType::kNull: return "无";
	case VType::kInt: return std::to_string(static_cast<std::int32_t>(v.value));
	case VType::kFloat: return Fmt("%g", v.asFloat());
	case VType::kBool: return v.value ? "True" : "False";
	case VType::kString: return "\"" + text::ToUtf8(p.str(static_cast<TStr>(v.value))) + "\"";
	case VType::kVariant: return v.inner ? "变体：" + describeVar(*v.inner) : "变体：无";
	case VType::kRef:
	case VType::kStruct:
		{
			if (v.value == 0) {
				return p.str(v.refType) + ": 无";
			}
			std::string s = p.str(v.refType) + ": ";
			if (const auto* i = p.findInstance(v.value)) {
				return s + formLabel(i->refID);
			}
			return s + "id " + Hex64(v.value);
		}
	default:
		{
			std::string s = std::string(VTypeName(v.type));
			if (IsRefType(v.type)) {
				s = p.str(v.refType) + "[]";
			}
			if (v.value == 0) {
				return s + " 无";
			}
			if (const auto* a = p.findArray(v.value)) {
				return s + Fmt("（%zu 个元素）", a->vars.size());
			}
			return s + " id " + Hex64(v.value) + "（缺失）";
		}
	}
}

void App::varValueWidget(const Variable& v, const char* a_id)
{
	const auto& p = *_save->papyrus();
	const Variable* x = &v;
	if (x->type == VType::kVariant && x->inner) {
		x = x->inner.get();
	}
	const std::string text = describeVar(v);
	ImGui::PushID(a_id);
	if ((x->type == VType::kRef || x->type == VType::kStruct) && x->value != 0) {
		if (p.findInstance(x->value)) {
			if (ImGui::SmallButton("跳转")) {
				select(Cat::kInstances, x->value);
			}
			ImGui::SameLine();
		}
	} else if (IsArrayType(x->type) && x->value != 0 && p.findArray(x->value)) {
		if (ImGui::SmallButton("跳转")) {
			select(Cat::kArrays, x->value);
		}
		ImGui::SameLine();
	}
	ImGui::TextWrapped("%s", text.c_str());
	ImGui::PopID();
}

void App::varsTable(const char* a_id, std::vector<Variable>& a_vars, const std::vector<std::string>& a_names)
{
	if (a_vars.empty()) {
		ImGui::TextDisabled("（没有变量）");
		return;
	}
	if (ImGui::BeginTable(a_id, 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40.0f);
		ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthFixed, 230.0f);
		ImGui::TableSetupColumn("值", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableHeadersRow();
		ImGuiListClipper clip;
		clip.Begin(static_cast<int>(a_vars.size()));
		while (clip.Step()) {
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%d", i);
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(i < static_cast<int>(a_names.size()) ? a_names[i].c_str() : "");
				ImGui::TableSetColumnIndex(2);
				ImGui::PushID(i);
				if (ImGui::SmallButton("编辑")) {
					beginEdit(&a_vars[i]);
				}
				ImGui::PopID();
				ImGui::SameLine();
				varValueWidget(a_vars[i], std::to_string(i).c_str());
			}
		}
		ImGui::EndTable();
	}
}

// ==========================================================================================
// rows
// ==========================================================================================

void App::rebuildRows()
{
	_rows.clear();
	_anchor.reset();
	if (!_save) {
		return;
	}
	auto&       p = *_save->papyrus();
	const auto  needle = Lower(_filter);
	const auto& F = _filters;
	// "show only" filters of this category are alternatives: a row passes if any checked one matches
	auto add = [&](std::uint64_t key, std::string text, std::uint32_t col, bool shown, std::uint64_t size, std::uint64_t mass) {
		if (!shown) {
			return;
		}
		if (!needle.empty() && Lower(text).find(needle) == std::string::npos) {
			return;
		}
		_rows.push_back({ key, std::move(text), col, size, mass });
	};
	auto pluginOk = [&](const RefID& id) {
		if (_pluginFilter.empty()) {
			return true;
		}
		return !id.isZero() && id.type() != RefID::Type::kCreated && EqualsNoCase(_save->pluginOf(_save->formIDOf(id)), _pluginFilter);
	};

	switch (_cat) {
	case Cat::kPlugins:
		for (std::size_t i = 0; i < _save->plugins.full.size(); ++i) {
			add(i, Fmt("%02zX     %s", i, _save->plugins.full[i].c_str()), 0, true, i, i);
		}
		for (std::size_t i = 0; i < _save->plugins.lite.size(); ++i) {
			add(0x10000 + i, Fmt("FE:%03zX  %s", i, _save->plugins.lite[i].c_str()), theme::Dim(), true, 0x10000 + i, 0x10000 + i);
		}
		break;
	case Cat::kChangeForms:
		{
			const bool anyShow = F.nullLists || F.cellMinus1 || F.emptyRef || F.withScripts;
			const bool needFacts = anyShow || F.parseLevel || F.flagMask || F.formFlagMask;
			std::unordered_set<std::uint32_t> withScripts;
			if (F.withScripts) {
				for (const auto& i : p.instances) {
					withScripts.insert(i.refID.data);
				}
			}
			for (const auto& c : _save->changeForms) {
				if (!pluginOk(c.refID)) {
					continue;
				}
				// change flags: (flags & mask) == (value & mask)
				if (F.flagMask && (c.changeFlags & F.flagMask) != (F.flagValue & F.flagMask)) {
					continue;
				}
				const CfFact* fact = needFacts ? &cfFact(c) : nullptr;
				if (fact) {
					if (F.parseLevel && fact->parse != F.parseLevel) {
						continue;
					}
					if (F.formFlagMask && (!fact->hasFormFlags || (fact->formFlags & F.formFlagMask) != (F.formFlagValue & F.formFlagMask))) {
						continue;
					}
					if (anyShow && !((F.nullLists && fact->nulls) || (F.cellMinus1 && fact->cellMinus1) || (F.emptyRef && fact->emptyRef) ||
										(F.withScripts && withScripts.contains(c.refID.data)))) {
						continue;
					}
				}
				if (F.content[0]) {
					const auto d = cf::Decode(*_save, c);
					if (!cf::ContentMatches(d.root, F.content)) {
						continue;
					}
				}
				const auto  fid = _save->formIDOf(c.refID);
				std::string name = _db ? _db->describe(fid) : std::string();
				const auto  nulls = fact ? fact->nulls : 0;
				add(c.refID.data,
					Fmt("%s  %08X  %-28s  flags %08X  %u 字节", ChangeFormTypeName(c.typeCode()), fid, _save->pluginOf(fid).c_str(), c.changeFlags,
						static_cast<unsigned>(c.data.size())) +
						(name.empty() ? "" : "  " + name) + (nulls ? Fmt("  [%zu NULL ENTRIES]", nulls) : std::string()),
					nulls ? theme::Warn() : 0, true, c.data.size(), c.length2 ? c.length2 : c.data.size());
			}
			break;
		}
	case Cat::kStrings:
		for (std::size_t i = 0; i < p.strings.size(); ++i) {
			const auto len = p.strings[i].size();
			add(i, Fmt("%6zu  ", i) + text::ToUtf8(p.strings[i]), len >= 512 ? theme::Warn() : 0, !F.longStrings || len >= 512, len, len);
		}
		break;
	case Cat::kScripts:
		{
			std::unordered_map<TStr, std::size_t> counts, vars;
			for (const auto& i : p.instances) {
				counts[i.script]++;
				vars[i.script] += i.data ? i.data->vars.size() : 0;
			}
			const bool anyShow = _onlyFlagged || F.noParent || F.missingParent;
			for (const auto& s : p.scripts) {
				const bool undef = ops::IsScriptUndefined(p, s);
				const bool noParent = ops::IsNoParent(p, s);
				const bool missingParent = ops::IsMissingParent(p, s);
				std::string t = p.str(s.name) + Fmt("  (%zu)", counts[s.name]);
				if (!p.str(s.parent).empty()) {
					t += "  extends " + p.str(s.parent);
				}
				if (undef) {
					t += "  [UNDEFINED]";
				} else if (missingParent) {
					t += "  [MISSING PARENT]";
				}
				const bool shown = !anyShow || (_onlyFlagged && undef) || (F.noParent && noParent) || (F.missingParent && missingParent);
				add(s.name, t, undef ? theme::Bad() : missingParent ? theme::Warn() : 0, shown, counts[s.name], counts[s.name] + vars[s.name]);
			}
			break;
		}
	case Cat::kInstances:
		{
			const bool anyShow = _onlyFlagged || F.memberless || F.zeroedCanary || F.withScripts;
			for (const auto& i : p.instances) {
				if (!pluginOk(i.refID)) {
					continue;
				}
				const bool unatt = ops::IsUnattached(i);
				const bool undef = ops::IsInstanceUndefined(p, i.script);
				const bool missing = ops::IsCreatedWithoutForm(*_save, i);
				const bool memberless = (F.memberless || anyShow) && ops::IsMemberless(p, i);
				const bool zeroed = F.zeroedCanary && ops::HasCanary(p, i) && ops::CanaryValue(p, i) == 0;
				const bool hasForm = F.withScripts && _save->findChangeForm(i.refID);
				std::string t = instanceLabel(i);
				if (missing) {
					t += "  [CREATED FORM MISSING]";
				}
				if (memberless) {
					t += "  [MEMBERLESS]";
				}
				if (zeroed) {
					t += "  [ZEROED CANARY]";
				}
				const bool shown = !anyShow || (_onlyFlagged && (unatt || undef || missing)) || (F.memberless && memberless) ||
								   (F.zeroedCanary && zeroed) || (F.withScripts && hasForm);
				std::uint64_t arr = 0;
				if (i.data) {
					for (const auto& v : i.data->vars) {
						if (IsArrayType(v.type)) {
							if (const auto* a = p.findArray(v.value)) {
								arr += a->vars.size();
							}
						}
					}
				}
				const std::uint64_t n = i.data ? i.data->vars.size() : 0;
				add(i.id, t, undef ? theme::Bad() : (unatt || missing || memberless || zeroed) ? theme::Warn() : 0, shown, n, n + arr);
			}
			break;
		}
	case Cat::kReferences:
		for (const auto& r : p.references) {
			const bool          undef = ops::IsInstanceUndefined(p, r.script);
			const std::uint64_t n = r.data ? r.data->vars.size() : 0;
			add(r.id, p.str(r.script) + "  id " + Hex64(r.id) + (undef ? "  [UNDEFINED]" : ""), undef ? theme::Bad() : 0, !_onlyFlagged || undef, n, n);
		}
		break;
	case Cat::kArrays:
		for (const auto& a : p.arrays) {
			std::string t = IsRefType(a.type) ? p.str(a.refType) + "[]" : std::string(VTypeName(a.type));
			add(a.id, Fmt("%-24s  %6zu 个元素   id %s", t.c_str(), a.vars.size(), Hex64(a.id).c_str()), 0, true, a.vars.size(), a.vars.size());
		}
		break;
	case Cat::kThreads:
		for (const auto& a : p.activeScripts) {
			if (!_pluginFilter.empty()) {
				bool ok = false;
				if (a.data && !a.data->frames.empty() && a.data->frames.front().owner.type == VType::kRef) {
					if (const auto* inst = p.findInstance(a.data->frames.front().owner.value)) {
						ok = pluginOk(inst->refID);
					}
				}
				if (!ok) {
					continue;
				}
			}
			const bool    undef = ops::IsThreadUndefined(p, a);
			const bool    term = ops::IsThreadTerminated(p, a);
			const bool    susp = ops::IsThreadSuspended(p, a.id);
			std::uint64_t frames = a.data ? a.data->frames.size() : 0, vars = 0;
			if (a.data) {
				for (const auto& f : a.data->frames) {
					vars += f.vars.size();
				}
			}
			add(a.id, threadLabel(a), undef ? theme::Bad() : term ? theme::Dim() : susp ? theme::Warn() : 0, !_onlyFlagged || undef || susp, frames, frames + vars);
		}
		break;
	case Cat::kMessages:
		for (std::size_t i = 0; i < p.functionMessages.size(); ++i) {
			const auto& m = p.functionMessages[i];
			std::string t = Fmt("%4zu  ", i);
			t += m.data ? p.str(m.data->scriptName) + "." + p.str(m.data->event) : std::string("（无数据）");
			if (m.id) {
				t += Fmt("   线程 #%08X", *m.id);
			}
			const std::uint64_t n = m.data ? m.data->vars.size() : 0;
			add(i, t, 0, true, n, n);
		}
		break;
	case Cat::kSuspended:
		for (int table = 1; table <= 2; ++table) {
			const auto& list = table == 1 ? p.suspended1 : p.suspended2;
			for (const auto& s : list) {
				std::string t = Fmt("表 %d  #%08X  ", table, s.id);
				t += s.data ? p.str(s.data->scriptName) + "." + p.str(s.data->event) : std::string("（无数据）");
				const std::uint64_t n = s.data ? s.data->vars.size() : 0;
				add((std::uint64_t(table) << 32) | s.id, t, 0, true, n, n);
			}
		}
		break;
	case Cat::kUnbinds:
		for (const auto& u : p.unbinds) {
			const auto* i = p.findInstance(u.id);
			add(u.id, Hex64(u.id) + "  " + (i ? instanceLabel(*i) : std::string("（实例已不存在）")), i ? 0 : theme::Warn(), !_onlyFlagged || !i, 1, 1);
		}
		break;
	default:
		break;
	}

	// sorting (ReSaver: Alphabetical / Size / Mass / None); plugins always stay in load order
	switch (_cat == Cat::kPlugins ? 0 : _sortMode) {
	case 1:
		std::stable_sort(_rows.begin(), _rows.end(), [](const Row& a, const Row& b) {
			return std::lexicographical_compare(a.text.begin(), a.text.end(), b.text.begin(), b.text.end(),
				[](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y)); });
		});
		break;
	case 2:
		std::stable_sort(_rows.begin(), _rows.end(), [](const Row& a, const Row& b) { return a.size > b.size; });
		break;
	case 3:
		std::stable_sort(_rows.begin(), _rows.end(), [](const Row& a, const Row& b) { return a.mass > b.mass; });
		break;
	default:
		break;
	}

	_rowsCat = _cat;
	_lastFilter = _filter;
	_rowsDirty = false;
	if (_focusFirstRow && !_rows.empty()) {
		_focus = _rows.front().key;
		_selected = { _rows.front().key };
		_focusFirstRow = false;
	}
}

const App::CfFact& App::cfFact(const ChangeForm& c)
{
	auto it = _cfFacts.find(c.refID.data);
	if (it != _cfFacts.end()) {
		return it->second;
	}
	CfFact     f;
	const auto d = cf::Decode(*_save, c);
	f.parse = !d.supported ? 3 : d.complete ? 1 : d.consumed > 0 ? 2 : 3;
	f.emptyRef = d.isEmptyRef;
	f.cellMinus1 = d.inCellMinus1;
	f.hasFormFlags = d.hasFormFlags;
	f.formFlags = d.formFlags;
	for (auto v : d.listRaw) {
		f.nulls += v == 0;
	}
	if (!d.complete) {
		f.nulls = 0;
	}
	return _cfFacts.emplace(c.refID.data, f).first->second;
}

void App::testSelect(int a_cat, const char* a_filter, bool a_analysis)
{
	_cat = static_cast<Cat>(a_cat);
	std::snprintf(_filter, sizeof(_filter), "%s", a_filter ? a_filter : "");
	_rowsDirty = true;
	_focusFirstRow = true;
	_showAnalysis = a_analysis;
}

// ==========================================================================================
// main layout
// ==========================================================================================

void App::draw()
{
	if (!_settingsLoaded) {
		loadSettings();
	}
	_live.setEnabled(_liveEnabled);
	_live.start();
	_live.poll();
	pollScan();
	pollNames();
	pollWatch();
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);
	ImGui::Begin("##main", nullptr,
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar |
			ImGuiWindowFlags_NoBringToFrontOnFocus);
	drawMenu();

	const float statusH = ImGui::GetFrameHeightWithSpacing();
	ImGui::BeginChild("##body", ImVec2(0, -statusH));
	if (!_save) {
		ImGui::Dummy(ImVec2(0, 80));
		ImGui::Indent(40);
		ImGui::TextUnformatted("ReSaver++");
		ImGui::TextDisabled("Skyrim Special Edition / Anniversary Edition / VR 存档编辑器");
		ImGui::Spacing();
		if (ImGui::Button("打开存档…", ImVec2(180, 0))) {
			startOpenDialog();
		}
		ImGui::Spacing();
		ImGui::TextDisabled("或将 .ess 文件拖到此窗口（.pex 脚本会显示其代码）");
		ImGui::Unindent(40);
	} else {
		drawCategories();
		ImGui::SameLine();
		if (_cat == Cat::kOverview) {
			ImGui::BeginChild("##overview", ImVec2(0, 0), ImGuiChildFlags_Borders);
			drawOverview();
			ImGui::EndChild();
		} else {
			const float listW = ImGui::GetContentRegionAvail().x * (_cat == Cat::kMods ? 0.46f : 0.48f);
			ImGui::BeginChild("##list", ImVec2(listW, 0), ImGuiChildFlags_Borders);
			if (_cat == Cat::kMods) {
				drawModsList();
			} else if (_cat == Cat::kQuests) {
				drawQuestsList();
			} else {
				drawList();
			}
			ImGui::EndChild();
			ImGui::SameLine();
			ImGui::BeginChild("##detail", ImVec2(0, 0), ImGuiChildFlags_Borders);
			if (_cat == Cat::kMods) {
				if (_focus) {
					detailMod(*_focus);
				} else {
					ImGui::TextDisabled("点击一个模组查看其脚本。");
				}
			} else if (_cat == Cat::kQuests) {
				drawQuestDetail();
			} else {
				drawDetail();
			}
			ImGui::EndChild();
		}
	}
	ImGui::EndChild();
	drawStatus();
	ImGui::End();

	if (_showAnalysis) {
		drawAnalysis();
	}
	if (_showLookup && _save) {
		drawLookup();
	}
	if (_showCompare && _save) {
		drawCompare();
	}
	if (_showArrays && _save) {
		drawArraysAnalysis();
	}
	if (_showLog) {
		drawLog();
	}
	if (_showSettings) {
		drawSettings();
	}
	if (_showSupport) {
		drawSupport();
	}
	if (_showViewer) {
		drawScriptViewer();
	} else {
		_viewerFocused = false;
	}
	if (_showCompileSettings) {
		drawCompileSettings();
	}
	drawDecompilePrompt();
	if (_save) {
		drawMissingModal();
	}
	pollCompile();
	if (_testCompile && !_docs.empty()) {
		_testCompile = false;
		startCompile(*_docs.back());
	}
	if (_testRowView && _focus) {
		_testRowView = false;
		viewRowScript(*_focus);
	}
	drawModals();
	logStatus();
}

void App::drawMenu()
{
	if (!ImGui::BeginMenuBar()) {
		return;
	}
	if (ImGui::BeginMenu("文件")) {
		if (ImGui::MenuItem("打开…", "Ctrl+O")) {
			startOpenDialog();
		}
		if (ImGui::MenuItem("保存", "Ctrl+S", false, _save != nullptr)) {
			confirm("覆盖存档", "将修改写入 " + platform::ToUtf8(_path.filename()) +
										  "？\n\n当前文件会以 .bak 副本保留在它旁边。",
				[this] { doSave(_path); });
		}
		if (ImGui::MenuItem("另存为…", "Ctrl+Shift+S", false, _save != nullptr)) {
			startSaveAsDialog();
		}
		ImGui::Separator();
		if (ImGui::MenuItem("与另一个存档比较…", nullptr, false, _save != nullptr)) {
			if (platform::HasNativeDialogs()) {
				if (auto p = platform::OpenSaveDialog(_path.parent_path())) {
					compareWith(*p);
				}
			} else if (const char* other = std::getenv("RESAVERPP_TEST_COMPARE")) {
				compareWith(other);
			}
		}
		bool watch = _watch;
		if (ImGui::MenuItem("监视存档文件夹（自动打开新存档）", nullptr, &watch)) {
			setWatching(watch);
		}
		if (ImGui::BeginMenu("保存时的压缩")) {
			const char* comp[] = { "保持存档原有的压缩", "LZ4（游戏默认）", "zlib", "不压缩" };
			for (int i = 0; i < 4; ++i) {
				if (ImGui::MenuItem(comp[i], nullptr, _saveCompression == i - 1)) {
					_saveCompression = i - 1;
					saveSettings();
				}
			}
			ImGui::EndMenu();
		}
		if (ImGui::MenuItem("导出插件列表…", nullptr, false, _save != nullptr)) {
			exportPluginList();
		}
		ImGui::Separator();
		if (ImGui::MenuItem("打开脚本（.pex）…")) {
			if (platform::HasNativeDialogs()) {
				if (auto p = platform::OpenFileDialog(_sourcesPath.empty() ? std::filesystem::path() : platform::FromUtf8(_sourcesPath), "pex",
						"打开已编译的 Papyrus 脚本")) {
					openScript(*p);
				}
			} else if (const char* s = std::getenv("RESAVERPP_TEST_PEX")) {
				openScript(s);
			}
		}
		ImGui::Separator();
		if (ImGui::MenuItem("退出")) {
			requestQuit();
		}
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("清理", _save != nullptr)) {
		auto* p = _save ? _save->papyrus() : nullptr;
		if (ImGui::MenuItem("移除未附加实例")) {
			confirm("移除未附加实例",
				Fmt("删除 %zu 个未附着到任何对象的脚本实例？", _report ? _report->unattached : 0),
				[this, p] { markEdited(Fmt("已移除 %zu 个未附加实例", ops::RemoveUnattachedInstances(*p))); });
		}
		if (ImGui::MenuItem("移除未定义元素")) {
			confirm("移除未定义元素",
				"删除 .pex 已不再安装的脚本、它们的实例与引用，并终止运行它们的线程？",
				[this, p] { markEdited(Fmt("已移除/终止 %zu 个未定义元素", ops::RemoveUndefinedElements(*p))); });
		}
		if (ImGui::MenuItem("终止未定义线程")) {
			confirm("终止未定义线程", "终止所有正在运行已不再安装的脚本的线程？",
				[this, p] { markEdited(Fmt("已终止 %zu 个未定义线程", ops::TerminateUndefinedThreads(*p))); });
		}
		{
			const bool any = !_missing.empty() || !_missingScripts.empty() || !_unfoundScripts.empty();
			if (ImGui::MenuItem("已不再安装的模组与脚本…", nullptr, false, any)) {
				_missingDismissed = false;  // the banner comes back too
				for (auto& m : _missing) {
					m.planned = false;
				}
				_missingOpen = true;
			}
			if (!any && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				ImGui::SetTooltip(_installKnown ? "没有可移除的项：这个存档里的每个模组都在你的加载顺序中，每个脚本也都找到了。"
												: "你的模组还没有扫描完成（或正在扫描）。");
			}
		}
		if (ImGui::MenuItem("移除位于缺失生成对象上的实例…")) {
			openOrphans();
		}
		ImGui::Separator();
		if (ImGui::MenuItem("修复 SkyUI MCM 注册卡死…")) {
			fixMcmJam();
		}
		if (ImGui::MenuItem("重置 Havok…")) {
			resetHavok();
		}
		if (ImGui::MenuItem("净化 FormList…")) {
			purifyFormLists();
		}
		ImGui::Separator();
		if (ImGui::MenuItem("批量清理…")) {
			_batchOpen = true;
		}
		if (ImGui::MenuItem("清除某个脚本…")) {
			_purgeName[0] = 0;
			_purgeOpen = true;
		}
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("工具", _save != nullptr)) {
		ImGui::MenuItem("分析", nullptr, &_showAnalysis);
		if (ImGui::MenuItem("报告…")) {
			_reportOpen = true;
		}
		ImGui::Separator();
		ImGui::MenuItem("按名称/编辑器 ID 查找表单…", nullptr, &_showLookup);
		if (ImGui::MenuItem("分析数组区块…", nullptr, &_showArrays)) {
			_arraysDirty = true;
		}
		if (ImGui::MenuItem(_db ? "重新读取插件名称" : "读取插件名称", nullptr, false, _sources.scanned() && !namesLoading())) {
			startNames(true);
		}
		if (ImGui::MenuItem("自动读取插件名称", nullptr, &_autoNames)) {
			saveSettings();
		}
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("选项")) {
		ImGui::MenuItem("设置…", nullptr, &_showSettings);
		if (ImGui::BeginMenu("主题")) {
			for (int k = 0; k < 3; ++k) {
				if (ImGui::MenuItem(theme::Name(static_cast<theme::Kind>(k)), nullptr, static_cast<int>(theme::Current()) == k)) {
					setTheme(k);
				}
			}
			ImGui::EndMenu();
		}
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("帮助")) {
		ImGui::MenuItem("显示日志", nullptr, &_showLog);
		ImGui::MenuItem("支持…", nullptr, &_showSupport);
		if (ImGui::MenuItem("关于")) {
			_showAbout = true;
		}
		ImGui::EndMenu();
	}

	// keyboard shortcuts
	auto& io = ImGui::GetIO();
	if (!_viewerFocused && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
		startOpenDialog();
	}
	if (_viewerFocused) {
		// the script viewer has its own Ctrl+S
	} else if (_save && io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
		startSaveAsDialog();
	} else if (_save && io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
		confirm("覆盖存档", "将修改写入 " + platform::ToUtf8(_path.filename()) + "?\n\n当前文件会以 .bak 副本保留在它旁边。",
			[this] { doSave(_path); });
	}
	ImGui::EndMenuBar();
}

void App::drawCategories()
{
	ImGui::BeginChild("##cats", ImVec2(250, 0), ImGuiChildFlags_Borders);
	auto& p = *_save->papyrus();
	auto  item = [&](Cat c, std::size_t n, bool warn = false) {
        std::string label = CatName(c);
        if (c != Cat::kOverview) {
            label += Fmt("  (%zu)", n);
        }
        if (warn) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::Warn());
        }
        if (ImGui::Selectable(label.c_str(), _cat == c)) {
            if (_cat != c) {
                _cat = c;
                _filter[0] = 0;
                _onlyFlagged = false;
                _selected.clear();
                _focus.reset();
                _rowsDirty = true;
            }
        }
        if (warn) {
            ImGui::PopStyleColor();
        }
	};
	item(Cat::kOverview, 0, _report && !_report->warnings.empty());
	ensureMods();
	{
		std::size_t withScripts = 0;
		for (const auto& r : _mods->rows) {
			if (!r.scripts.empty()) {
				++withScripts;
			}
		}
		item(Cat::kMods, withScripts);
	}
	{
		std::size_t inJournal = 0, stale = 0;
		for (const auto& q : questStates()) {
			if (q.shownObjectives()) {
				++inJournal;
				stale += (q.completed() || !q.running()) ? 1 : 0;
			}
		}
		item(Cat::kQuests, inJournal, stale > 0);
	}
	ImGui::SeparatorText("存档");
	item(Cat::kPlugins, _save->plugins.full.size() + _save->plugins.lite.size());
	item(Cat::kChangeForms, _save->changeForms.size());
	ImGui::SeparatorText("Papyrus");
	item(Cat::kStrings, p.strings.size());
	item(Cat::kScripts, p.scripts.size(), _report && _report->undefinedScripts > 0);
	item(Cat::kInstances, p.instances.size(), _report && (_report->unattached > 0 || _report->undefinedInstances > 0));
	item(Cat::kReferences, p.references.size());
	item(Cat::kArrays, p.arrays.size());
	item(Cat::kThreads, p.activeScripts.size(), _report && !_report->loopingScripts.empty());
	item(Cat::kMessages, p.functionMessages.size());
	item(Cat::kSuspended, p.suspended1.size() + p.suspended2.size(), p.suspended1.size() + p.suspended2.size() >= 50);
	item(Cat::kUnbinds, p.unbinds.size());
	ImGui::EndChild();
}

void App::drawList()
{
	if (_rowsDirty || _rowsCat != _cat || _lastFilter != _filter) {
		rebuildRows();
	}
	ImGui::SetNextItemWidth(-330);
	if (ImGui::InputTextWithHint("##filter", "过滤（不区分大小写）", _filter, sizeof(_filter))) {
		_rowsDirty = true;
	}
	const bool flaggable = _cat == Cat::kScripts || _cat == Cat::kInstances || _cat == Cat::kReferences || _cat == Cat::kThreads || _cat == Cat::kUnbinds;
	if (flaggable) {
		ImGui::SameLine();
		if (ImGui::Checkbox("仅问题项", &_onlyFlagged)) {
			_rowsDirty = true;
		}
	}
	ImGui::SameLine();
	const bool filtersActive = drawFiltersButton();
	ImGui::SameLine();
	ImGui::SetNextItemWidth(110);
	const char* sorts[] = { "存档顺序", "字母顺序", "大小", "总量" };
	if (ImGui::Combo("##sort", &_sortMode, sorts, 4)) {
		_rowsDirty = true;
		saveSettings();
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("排序（对应 ReSaver 的 Data > Sorting）：\n大小 = 元素自身的大小（变量、元素、栈帧、字节）\n"
						  "总量 = 包含其拥有内容的大小（实例的数组、线程各栈帧的变量等）");
	}
	if (!_pluginFilter.empty()) {
		TextColored(theme::Warn(), "仅显示属于该插件的元素：" + _pluginFilter);
		ImGui::SameLine();
		if (ImGui::SmallButton("清除插件过滤")) {
			_pluginFilter.clear();
			_rowsDirty = true;
		}
	}
	if (filtersActive) {
		TextColored(theme::Warn(), "过滤器生效中（见“过滤器”）。");
	}
	ImGui::TextDisabled("%zu 项显示，%zu 项选中  （Ctrl/Shift 单击多选，Ctrl+A 全选当前列表）", _rows.size(), _selected.size());

	// actions
	if (_cat == Cat::kPlugins) {
		const auto sel = selectedPlugins();
		if (sel.size() > 1) {
			pluginActions(sel);
		}
		if (ImGui::Button("导出插件列表…")) {
			exportPluginList();
		}
	}
	const bool deletable = _cat != Cat::kStrings && _cat != Cat::kPlugins;
	if (deletable) {
		ImGui::BeginDisabled(_rows.empty());
		if (ImGui::Button(Fmt("删除列出的全部（%zu）", _rows.size()).c_str())) {
			// ReSaver's "Kill Listed": everything the filter shows
			_selected.clear();
			for (const auto& r : _rows) {
				_selected.insert(r.key);
			}
			deleteSelected();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(_selected.empty());
		if (ImGui::Button("删除选中")) {
			deleteSelected();
		}
		if (_cat == Cat::kThreads) {
			ImGui::SameLine();
			if (ImGui::Button("终止选中")) {
				auto ids = std::set<ThreadID>();
				for (auto k : _selected) {
					ids.insert(static_cast<ThreadID>(k));
				}
				confirm("终止线程", Fmt("终止 %zu 个线程？游戏恢复它们时就会结束。", ids.size()),
					[this, ids] { markEdited(Fmt("已终止 %zu 个线程", ops::TerminateThreads(*_save->papyrus(), ids))); });
			}
		}
		if (_cat == Cat::kInstances) {
			ImGui::SameLine();
			if (ImGui::Button("删除并连同其生成对象")) {
				std::set<EID>           ids;
				std::set<std::uint32_t> forms;
				for (auto k : _selected) {
					ids.insert(k);
					if (const auto* i = _save->papyrus()->findInstance(k); i && i->refID.type() == RefID::Type::kCreated) {
						forms.insert(i->refID.data);
					}
				}
				confirm("删除实例与生成对象",
					Fmt("删除 %zu 个实例以及它们附着其上的 %zu 个游戏内创建对象？\n\n来自插件的对象永远不会被删除，"
						"只删除生成对象（FF......）。",
						ids.size(), forms.size()),
					[this, ids, forms] {
						const auto a = ops::DeleteInstances(*_save->papyrus(), ids);
						const auto b = ops::DeleteChangeForms(*_save, forms);
						markEdited(Fmt("已删除 %zu 个实例和 %zu 个生成对象", a, b));
					});
			}
		}
		if (_cat == Cat::kScripts) {
			ImGui::SameLine();
			if (ImGui::Button("清除选中脚本")) {
				std::vector<std::string> names;
				for (auto k : _selected) {
					names.push_back(_save->papyrus()->str(static_cast<TStr>(k)));
				}
				confirm("清除脚本",
					Fmt("移除 %zu 个脚本的所有线程、挂起堆栈、函数消息和实例，以及它们"
						"所在的生成对象？脚本定义会保留。",
						names.size()),
					[this, names] {
						ops::PurgeResult tot;
						for (const auto& n : names) {
							auto r = ops::PurgeScript(*_save, n, true);
							tot.threads += r.threads;
							tot.instances += r.instances;
							tot.forms += r.forms;
							tot.stacks += r.stacks;
							tot.messages += r.messages;
						}
						markEdited(Fmt("已清除：%zu 个线程、%zu 个堆栈、%zu 条消息、%zu 个实例、%zu 个生成对象", tot.threads,
							tot.stacks, tot.messages, tot.instances, tot.forms));
					});
			}
		}
		ImGui::EndDisabled();
	}
	ImGui::Separator();

	ImGui::BeginChild("##rows");
	if (ImGui::IsWindowFocused() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
		for (const auto& r : _rows) {
			_selected.insert(r.key);
		}
	}
	int focusRow = -1;
	if (_scrollToFocus && _focus) {
		for (std::size_t i = 0; i < _rows.size(); ++i) {
			if (_rows[i].key == *_focus) {
				focusRow = static_cast<int>(i);
				break;
			}
		}
		if (focusRow >= 0) {
			ImGui::SetScrollY(focusRow * ImGui::GetTextLineHeightWithSpacing() - ImGui::GetWindowHeight() * 0.4f);
		}
		_scrollToFocus = false;
	}
	ImGuiListClipper clip;
	clip.Begin(static_cast<int>(_rows.size()));
	if (focusRow >= 0) {
		clip.IncludeItemByIndex(focusRow);
	}
	while (clip.Step()) {
		for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
			const auto& r = _rows[i];
			ImGui::PushID(i);
			if (r.color) {
				ImGui::PushStyleColor(ImGuiCol_Text, r.color);
			}
			const bool sel = _selected.contains(r.key);
			if (ImGui::Selectable(r.text.c_str(), sel)) {
				auto& io = ImGui::GetIO();
				if (io.KeyShift && _anchor) {
					const auto a = std::min<std::size_t>(*_anchor, i), b = std::max<std::size_t>(*_anchor, i);
					if (!io.KeyCtrl) {
						_selected.clear();
					}
					for (std::size_t k = a; k <= b && k < _rows.size(); ++k) {
						_selected.insert(_rows[k].key);
					}
				} else if (io.KeyCtrl) {
					if (sel) {
						_selected.erase(r.key);
					} else {
						_selected.insert(r.key);
					}
					_anchor = i;
				} else {
					_selected = { r.key };
					_anchor = i;
				}
				_focus = r.key;
			}
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
				viewRowScript(r.key);  // double-click: the script viewer
			}
			if (r.color) {
				ImGui::PopStyleColor();
			}
			ImGui::PopID();
		}
	}
	ImGui::EndChild();
}

void App::deleteSelected()
{
	auto&      p = *_save->papyrus();
	const auto n = _selected.size();
	std::function<void()> act;
	switch (_cat) {
	case Cat::kChangeForms:
		{
			std::set<std::uint32_t> ids;
			for (auto k : _selected) {
				ids.insert(static_cast<std::uint32_t>(k));
			}
			act = [this, ids] { markEdited(Fmt("已删除 %zu 个变更表单", ops::DeleteChangeForms(*_save, ids))); };
			break;
		}
	case Cat::kScripts:
		{
			std::set<TStr> ids;
			for (auto k : _selected) {
				ids.insert(static_cast<TStr>(k));
			}
			act = [this, ids] { markEdited(Fmt("已删除 %zu 个脚本/实例", ops::DeleteScripts(*_save->papyrus(), ids))); };
			break;
		}
	case Cat::kInstances:
		act = [this, ids = std::set<EID>(_selected.begin(), _selected.end())] {
			markEdited(Fmt("已删除 %zu 个脚本实例", ops::DeleteInstances(*_save->papyrus(), ids)));
		};
		break;
	case Cat::kReferences:
		act = [this, ids = std::set<EID>(_selected.begin(), _selected.end())] {
			markEdited(Fmt("已删除 %zu 个引用", ops::DeleteReferences(*_save->papyrus(), ids)));
		};
		break;
	case Cat::kArrays:
		act = [this, ids = std::set<EID>(_selected.begin(), _selected.end())] {
			markEdited(Fmt("已删除 %zu 个数组", ops::DeleteArrays(*_save->papyrus(), ids)));
		};
		break;
	case Cat::kThreads:
		{
			std::set<ThreadID> ids;
			for (auto k : _selected) {
				ids.insert(static_cast<ThreadID>(k));
			}
			act = [this, ids] { markEdited(Fmt("已删除 %zu 个线程", ops::DeleteThreads(*_save->papyrus(), ids))); };
			break;
		}
	case Cat::kMessages:
		{
			std::set<std::size_t> ids(_selected.begin(), _selected.end());
			act = [this, ids] { markEdited(Fmt("已删除 %zu 条函数消息", ops::DeleteFunctionMessages(*_save->papyrus(), ids))); };
			break;
		}
	case Cat::kSuspended:
		{
			std::set<ThreadID> ids;
			for (auto k : _selected) {
				ids.insert(static_cast<ThreadID>(k & 0xFFFFFFFF));
			}
			act = [this, ids] { markEdited(Fmt("已删除 %zu 个挂起堆栈", ops::DeleteSuspendedStacks(*_save->papyrus(), ids))); };
			break;
		}
	case Cat::kUnbinds:
		act = [this, ids = std::set<EID>(_selected.begin(), _selected.end())] {
			markEdited(Fmt("已删除 %zu 个排队解绑", ops::DeleteUnbinds(*_save->papyrus(), ids)));
		};
		break;
	default:
		return;
	}
	(void)p;
	confirm("删除", Fmt("删除选中的 %zu 个%s？", n, CatName(_cat)), act);
}

// ==========================================================================================
// detail pane
// ==========================================================================================

void App::drawDetail()
{
	if (!_focus) {
		ImGui::TextDisabled("选择一个条目以查看其详情。");
		return;
	}
	const auto k = *_focus;
	switch (_cat) {
	case Cat::kPlugins: detailPlugin(k); break;
	case Cat::kChangeForms: detailChangeForm(k); break;
	case Cat::kStrings: detailString(k); break;
	case Cat::kScripts: detailScript(k); break;
	case Cat::kInstances: detailInstance(k); break;
	case Cat::kReferences: detailReference(k); break;
	case Cat::kArrays: detailArray(k); break;
	case Cat::kThreads: detailThread(k); break;
	case Cat::kMessages: detailMessage(k); break;
	case Cat::kSuspended: detailSuspended(k); break;
	case Cat::kUnbinds: detailUnbind(k); break;
	default: break;
	}
}

void App::detailPlugin(std::uint64_t k)
{
	const bool  lite = k >= 0x10000;
	const auto  i = lite ? k - 0x10000 : k;
	const auto& list = lite ? _save->plugins.lite : _save->plugins.full;
	if (i >= list.size()) {
		return;
	}
	ImGui::TextUnformatted(list[i].c_str());
	ImGui::TextDisabled(lite ? "轻型插件（ESL），索引 FE:%03X" : "完整插件，加载索引 %02X", static_cast<unsigned>(i));
	std::size_t forms = 0;
	for (const auto& c : _save->changeForms) {
		const auto fid = _save->formIDOf(c.refID);
		if (lite ? ((fid >> 24) == 0xFE && ((fid >> 12) & 0xFFF) == i) : (fid >> 24) == i) {
			++forms;
		}
	}
	ImGui::Text("%zu 个变更表单属于此插件。", forms);
	ImGui::Spacing();
	pluginActions({ list[i] });
}

void App::detailChangeForm(std::uint64_t k)
{
	RefID id;
	id.data = static_cast<std::uint32_t>(k);
	const auto* c = _save->findChangeForm(id);
	if (!c) {
		return;
	}
	const auto fid = _save->formIDOf(c->refID);
	ImGui::Text("%s  %08X", ChangeFormTypeName(c->typeCode()), fid);
	if (BeginKV("cf")) {
		KV("插件", _save->pluginOf(fid));
		KV("RefID", Fmt("%06X (%s)", c->refID.data,
						c->refID.type() == RefID::Type::kCreated ? "游戏中创建" : c->refID.type() == RefID::Type::kDefault ? "Skyrim.esm" : "表单索引"));
		KV("变更标志", Fmt("%08X", c->changeFlags));
		KV("版本", std::to_string(c->version));
		KV("数据", Fmt("%zu 字节%s", c->data.size(), c->length2 ? Fmt("（zlib，解包后 %u 字节）", c->length2).c_str() : ""));
		ImGui::EndTable();
	}
	// scripts attached to this form
	std::vector<const ScriptInstance*> attached;
	for (const auto& i : _save->papyrus()->instances) {
		if (i.refID == c->refID) {
			attached.push_back(&i);
		}
	}
	ImGui::SeparatorText(Fmt("附加于此的脚本（%zu）", attached.size()).c_str());
	for (const auto* i : attached) {
		ImGui::PushID(i);
		if (ImGui::SmallButton("跳转")) {
			select(Cat::kInstances, i->id);
		}
		ImGui::SameLine();
		ImGui::TextUnformatted(_save->papyrus()->str(i->script).c_str());
		ImGui::PopID();
	}
	detailChangeFormDecoded(*c);
	if (ImGui::CollapsingHeader("原始数据（前 512 字节，按存储原样）")) {
		std::string hex;
		for (std::size_t i = 0; i < c->data.size() && i < 512; ++i) {
			hex += Fmt("%02X%s", c->data[i], (i % 16 == 15) ? "\n" : " ");
		}
		ImGui::TextUnformatted(hex.c_str());
	}
}

void App::detailString(std::uint64_t k)
{
	const auto& p = *_save->papyrus();
	if (k >= p.strings.size()) {
		return;
	}
	ImGui::Text("字符串 #%" PRIu64, k);
	ImGui::TextWrapped("%s", text::ToUtf8(p.strings[k]).c_str());
}

void App::detailScript(std::uint64_t k)
{
	auto&       p = *_save->papyrus();
	const auto* s = p.findScript(static_cast<TStr>(k));
	if (!s) {
		return;
	}
	ImGui::TextUnformatted(p.str(s->name).c_str());
	scriptItem(p.str(s->name));
	ImGui::SameLine();
	if (ImGui::SmallButton("查看脚本")) {
		viewScript(p.str(s->name));
	}
	if (ops::IsScriptUndefined(p, *s)) {
		TextColored(theme::Bad(), "UNDEFINED：存档创建时游戏无法加载该脚本的 .pex（模组已被移除？）。");
	}
	std::size_t n = 0;
	for (const auto& i : p.instances) {
		if (i.script == s->name) {
			++n;
		}
	}
	std::size_t threads = 0;
	for (const auto& a : p.activeScripts) {
		if (a.data) {
			for (const auto& f : a.data->frames) {
				if (f.scriptName == s->name) {
					++threads;
					break;
				}
			}
		}
	}
	if (BeginKV("script")) {
		KV("继承自", p.str(s->parent).empty() ? "（无）" : p.str(s->parent));
		scriptItem(p.str(s->parent));
		KV("实例", std::to_string(n));
		KV("运行它的线程", std::to_string(threads));
		ImGui::EndTable();
	}
	if (ImGui::Button("查看其脚本实例")) {
		_cat = Cat::kInstances;
		std::snprintf(_filter, sizeof(_filter), "%s  于", p.str(s->name).c_str());
		_rowsDirty = true;
		_selected.clear();
		_focus.reset();
		return;
	}
	ImGui::SameLine();
	if (ImGui::Button("查看其线程")) {
		_cat = Cat::kThreads;
		std::snprintf(_filter, sizeof(_filter), "%s.", p.str(s->name).c_str());
		_rowsDirty = true;
		_selected.clear();
		_focus.reset();
		return;
	}
	ImGui::SeparatorText(Fmt("成员（自有 %zu 个）", s->members.size()).c_str());
	if (ImGui::BeginTable("members", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
		ImGui::TableSetupColumn("名称");
		ImGui::TableSetupColumn("类型");
		ImGui::TableHeadersRow();
		for (const auto& m : s->members) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(p.str(m.name).c_str());
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(p.str(m.type).c_str());
		}
		ImGui::EndTable();
	}
}

void App::detailInstance(std::uint64_t k)
{
	auto& p = *_save->papyrus();
	auto* i = p.findInstance(k);
	if (!i) {
		return;
	}
	ImGui::TextUnformatted(p.str(i->script).c_str());
	scriptItem(p.str(i->script));
	ImGui::SameLine();
	if (ImGui::SmallButton("查看脚本")) {
		viewScript(p.str(i->script));
	}
	if (ops::IsUnattached(*i)) {
		TextColored(theme::Warn(), "UNATTACHED：未附加到任何对象。用“清理 > 移除未附加实例”可删除这些。");
	}
	if (ops::IsInstanceUndefined(p, i->script)) {
		TextColored(theme::Bad(), "UNDEFINED：其脚本已不再安装。");
	}
	if (ops::IsCreatedWithoutForm(*_save, *i)) {
		TextColored(theme::Warn(), "其生成对象已不在存档中。“清理 > 移除位于缺失生成对象上的实例”会显示 "
								   "它来自哪个模组，以及该模组是否仍安装。");
	}
	if (BeginKV("inst")) {
		KV("实例 id", Hex64(i->id));
		KV("附加到", formLabel(i->refID));
		if (i->data) {
			KV("状态", p.str(i->data->state).empty() ? "（默认）" : p.str(i->data->state));
			KV("标志", Fmt("%02X  unknown %08X", i->data->flag, i->data->unknown1));
		}
		ImGui::EndTable();
	}
	if (i->refID.type() == RefID::Type::kCreated || i->refID.type() == RefID::Type::kFormIdx) {
		if (_save->findChangeForm(i->refID) && ImGui::Button("转到其变更表单")) {
			select(Cat::kChangeForms, i->refID.data);
			return;
		}
	}
	if (i->data) {
		ImGui::SeparatorText(Fmt("变量（%zu）", i->data->vars.size()).c_str());
		varsTable("vars", i->data->vars, memberNames(i->script));
	}
	const auto refs = ops::FindReferrers(p, i->id);
	ImGui::SeparatorText(Fmt("被引用（%zu）", refs.size()).c_str());
	int n = 0;
	for (const auto& r : refs) {
		if (++n > 200) {
			ImGui::TextDisabled("…");
			break;
		}
		ImGui::PushID(n);
		if (ImGui::SmallButton("跳转")) {
			switch (r.kind) {
			case ops::Referrer::Kind::kInstance: select(Cat::kInstances, r.id); break;
			case ops::Referrer::Kind::kReference: select(Cat::kReferences, r.id); break;
			case ops::Referrer::Kind::kArray: select(Cat::kArrays, r.id); break;
			case ops::Referrer::Kind::kThread: select(Cat::kThreads, r.id); break;
			default: break;
			}
		}
		ImGui::SameLine();
		ImGui::TextUnformatted(r.where.c_str());
		ImGui::PopID();
	}
}

void App::detailReference(std::uint64_t k)
{
	auto& p = *_save->papyrus();
	auto* r = p.findReference(k);
	if (!r) {
		return;
	}
	ImGui::TextUnformatted(p.str(r->script).c_str());
	if (BeginKV("ref")) {
		KV("引用 id", Hex64(r->id));
		if (r->data) {
			KV("类型", p.str(r->data->state));
		}
		ImGui::EndTable();
	}
	if (r->data) {
		ImGui::SeparatorText("变量");
		varsTable("rvars", r->data->vars, memberNames(r->script));
	}
}

void App::detailArray(std::uint64_t k)
{
	auto& p = *_save->papyrus();
	auto* a = p.findArray(k);
	if (!a) {
		return;
	}
	ImGui::Text("%s 数组，%zu 个元素", IsRefType(a->type) ? (p.str(a->refType) + "[]").c_str() : VTypeName(a->type), a->vars.size());
	ImGui::TextDisabled("id %s", Hex64(a->id).c_str());
	const auto refs = ops::FindReferrers(p, a->id);
	ImGui::SeparatorText(Fmt("持有者（%zu）", refs.size()).c_str());
	int n = 0;
	for (const auto& r : refs) {
		if (++n > 100) {
			break;
		}
		ImGui::PushID(n);
		if (ImGui::SmallButton("跳转")) {
			select(r.kind == ops::Referrer::Kind::kThread ? Cat::kThreads : r.kind == ops::Referrer::Kind::kArray ? Cat::kArrays : Cat::kInstances, r.id);
		}
		ImGui::SameLine();
		ImGui::TextUnformatted(r.where.c_str());
		ImGui::PopID();
	}
	if (!refs.empty()) {
		if (ImGui::Button("查找持有者")) {
			const auto& r = refs.front();
			select(r.kind == ops::Referrer::Kind::kThread ? Cat::kThreads : r.kind == ops::Referrer::Kind::kArray ? Cat::kArrays : Cat::kInstances, r.id);
			return;
		}
	} else {
		TextColored(theme::Warn(), "没有任何对象持有此数组（“工具 > 分析数组区块”会列出所有这类数组）。");
	}
	ImGui::SeparatorText("元素");
	varsTable("avars", a->vars, {});
	arrayEditor(*a);
}

void App::detailThread(std::uint64_t k)
{
	auto& p = *_save->papyrus();
	auto* a = p.findActive(static_cast<ThreadID>(k));
	if (!a) {
		return;
	}
	ImGui::TextUnformatted(threadLabel(*a).c_str());
	if (ops::IsThreadTerminated(p, *a)) {
		TextColored(theme::Dim(), "TERMINATED：其代码已被替换为空操作，游戏恢复它时就会结束。");
	}
	if (ops::IsThreadSuspended(p, a->id)) {
		TextColored(theme::Warn(), "SUSPENDED：它正在等待一个排队调用（见“挂起堆栈”）。");
	}
	if (ops::IsThreadUndefined(p, *a)) {
		TextColored(theme::Bad(), "UNDEFINED：运行的脚本已不再安装。");
	}
	if (!a->data) {
		return;
	}
	auto& d = *a->data;
	if (BeginKV("thr")) {
		KV("线程 id", Fmt("%08X  类型 %u", a->id, a->type));
		KV("版本", Fmt("%d.%d", d.major, d.minor));
		KV("标志", Fmt("%02X  unknown3 %u", d.flag, d.unknown3));
		if (d.fragment) {
			KV("片段", d.fragment->typeCode.empty() ? "类型 2" : d.fragment->typeCode);
		}
		KV("栈帧", std::to_string(d.frames.size()));
		ImGui::EndTable();
	}
	for (std::size_t fi = 0; fi < d.frames.size(); ++fi) {
		auto& f = d.frames[fi];
		const auto  title = Fmt("帧 %zu：%s.%s  （%s）%s##f%zu", fi, p.str(f.baseName).c_str(), p.str(f.event).c_str(), p.str(f.scriptName).c_str(),
			 ops::IsFrameNative(f) ? "  原生" : ops::IsFrameZeroed(f) ? "  归零" : "", fi);
		if (ImGui::CollapsingHeader(title.c_str(), fi + 1 == d.frames.size() || fi == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
			ImGui::PushID(static_cast<int>(fi));
			if (ImGui::SmallButton(fi + 1 == d.frames.size() ? "查看停止处的脚本" : "查看此调用处的脚本")) {
				viewScript(p.str(f.baseName), p.str(f.event), f.status ? p.str(*f.status) : std::string(), static_cast<int>(f.ptr));
			}
			if (BeginKV("frame")) {
				KV("所有者", describeVar(f.owner));
				KV("返回类型", p.str(f.returnType));
				KV("指令", Fmt("%u / %zu", f.ptr, f.code.size()));
				if (!p.str(f.docString).empty()) {
					KV("文档", p.str(f.docString));
				}
				ImGui::EndTable();
			}
			std::vector<std::string> names;
			for (const auto& m : f.params) {
				names.push_back(p.str(m.name) + "（参数）");
			}
			for (const auto& m : f.locals) {
				names.push_back(p.str(m.name));
			}
			varsTable("fvars", f.vars, names);
			ImGui::PopID();
		}
	}
}

void App::detailMessage(std::uint64_t k)
{
	auto& p = *_save->papyrus();
	if (k >= p.functionMessages.size()) {
		return;
	}
	auto& m = p.functionMessages[k];
	if (BeginKV("fm")) {
		KV("标志", Fmt("%u / %02X", m.flag, m.flags));
		KV("线程", m.id ? Fmt("%08X", *m.id) : std::string("（无）"));
		if (m.data) {
			KV("调用", p.str(m.data->scriptName) + "." + p.str(m.data->event));
			KV("自身", describeVar(m.data->unknownVar));
		}
		ImGui::EndTable();
	}
	if (m.data && ImGui::Button("查看脚本")) {
		viewScript(p.str(m.data->scriptName), p.str(m.data->event));
	}
	if (m.data) {
		ImGui::SameLine();
	}
	if (m.id && p.findActive(*m.id) && ImGui::Button("转到线程")) {
		select(Cat::kThreads, *m.id);
		return;
	}
	if (m.data) {
		ImGui::SeparatorText("参数");
		varsTable("fmv", m.data->vars, {});
	}
}

void App::detailSuspended(std::uint64_t k)
{
	auto&       p = *_save->papyrus();
	const auto  table = static_cast<int>(k >> 32);
	const auto  id = static_cast<ThreadID>(k & 0xFFFFFFFF);
	auto& list = table == 1 ? p.suspended1 : p.suspended2;
	for (auto& s : list) {
		if (s.id != id) {
			continue;
		}
		if (BeginKV("ss")) {
			KV("表", std::to_string(table));
			KV("线程", Fmt("%08X", s.id));
			if (s.data) {
				KV("调用", p.str(s.data->scriptName) + "." + p.str(s.data->event));
				KV("自身", describeVar(s.data->unknownVar));
			}
			ImGui::EndTable();
		}
		if (s.data && ImGui::Button("查看脚本")) {
			viewScript(p.str(s.data->scriptName), p.str(s.data->event));
		}
		if (s.data) {
			ImGui::SameLine();
		}
		if (p.findActive(id) && ImGui::Button("转到线程")) {
			select(Cat::kThreads, id);
			return;
		}
		if (s.data) {
			ImGui::SeparatorText("参数");
			varsTable("ssv", s.data->vars, {});
		}
		return;
	}
}

void App::detailUnbind(std::uint64_t k)
{
	auto&       p = *_save->papyrus();
	const auto* i = p.findInstance(k);
	ImGui::Text("排队解绑 %s", Hex64(k).c_str());
	if (i) {
		ImGui::TextUnformatted(instanceLabel(*i).c_str());
		if (ImGui::Button("转到实例")) {
			select(Cat::kInstances, k);
		}
	} else {
		TextColored(theme::Warn(), "它引用的实例不在存档中。");
	}
}

// ==========================================================================================
// overview, analysis, modals, status
// ==========================================================================================

void App::drawOverview()
{
	const auto& h = _save->header;
	drawMissingBanner();
	if (_shotTex) {
		const float w = std::min(ImGui::GetContentRegionAvail().x * 0.45f, 480.0f);
		ImGui::Image(static_cast<ImTextureID>(_shotTex), ImVec2(w, w * _shotH / std::max(1, _shotW)));
		ImGui::SameLine();
	}
	ImGui::BeginGroup();
	ImGui::Text("%s，等级 %u", text::ToUtf8(h.playerName).c_str(), h.playerLevel);
	ImGui::TextUnformatted(text::ToUtf8(h.location).c_str());
	ImGui::TextDisabled("%s  |  %s", text::ToUtf8(h.race).c_str(), text::ToUtf8(h.gameDate).c_str());
	ImGui::Spacing();
	if (BeginKV("hdr")) {
		KV("文件", platform::ToUtf8(_path.filename()));
		KV("存档编号", std::to_string(h.saveNumber));
		KV("格式", Fmt("存档版本 %u，表单版本 %u，%s", h.version, _save->formVersion,
						 h.compression == Compression::kLZ4 ? "LZ4" : h.compression == Compression::kZlib ? "zlib" : "未压缩"));
		KV("插件", Fmt("%zu 个完整 + %zu 个轻型", _save->plugins.full.size(), _save->plugins.lite.size()));
		KV("主体大小", Fmt("%.1f MB", _save->originalBodySize / 1048576.0));
		ImGui::EndTable();
	}
	ImGui::EndGroup();

	if (!_report) {
		return;
	}
	const auto& r = *_report;
	ImGui::SeparatorText("健康状况");
	if (r.warnings.empty()) {
		TextColored(theme::Good(), "未发现问题。");
	}
	for (const auto& w : r.warnings) {
		ImGui::Bullet();
		ImGui::PushStyleColor(ImGuiCol_Text, theme::Warn());
		ImGui::TextWrapped("%s", w.c_str());
		ImGui::PopStyleColor();
	}
	for (const auto& s : r.loopingScripts) {
		ImGui::PushID(s.c_str());
		if (ImGui::Button(("清除 " + s).c_str())) {
			const std::string name = s;
			confirm("清除 " + name,
				"移除 “" + name +
					"” 的全部线程、挂起堆栈、函数消息和实例，以及它们所在的生成对象？\n\n这正是修好 Firefly 循环的办法。结果请用“文件 > 另存为”写出。",
				[this, name] {
					auto x = ops::PurgeScript(*_save, name, true);
					markEdited(Fmt("已清除 %s：%zu 个线程、%zu 个实例、%zu 个生成对象", name.c_str(), x.threads, x.instances, x.forms));
				});
		}
		ImGui::PopID();
		ImGui::SameLine();
	}
	if (r.mcmJam) {
		if (ImGui::Button("修复 SkyUI MCM 注册卡死")) {
			fixMcmJam();
		}
		ImGui::SameLine();
	}
	ImGui::NewLine();
	ImGui::SeparatorText("统计");
	if (BeginKV("counts")) {
		KV("脚本实例", Fmt("%zu  （未附加 %zu，未定义 %zu，位于缺失生成对象上 %zu）", r.instances, r.unattached,
								   r.undefinedInstances, r.createdWithoutForm));
		KV("脚本", Fmt("%zu  （未定义 %zu）", r.scripts, r.undefinedScripts));
		KV("线程", Fmt("%zu  （未定义 %zu，已终止 %zu）", r.threads, r.undefinedThreads, r.terminatedThreads));
		KV("挂起堆栈", std::to_string(r.suspended));
		KV("函数消息", std::to_string(r.functionMessages));
		KV("数组", std::to_string(r.arrays));
		KV("变更表单", std::to_string(r.changeForms));
		KV("字符串", std::to_string(r.strings));
		ImGui::EndTable();
	}
}

void App::drawAnalysis()
{
	ImGui::SetNextWindowSize(ImVec2(820, 640), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("分析", &_showAnalysis)) {
		ImGui::End();
		return;
	}
	if (_report) {
		if (ImGui::Button("复制报告到剪贴板")) {
			ImGui::SetClipboardText(ops::FormatReport(*_report, 40).c_str());
		}
		ImGui::Separator();
		ImGui::BeginChild("rep");
		ImGui::TextUnformatted(ops::FormatReport(*_report, 40).c_str());
		ImGui::EndChild();
	}
	ImGui::End();
}

void App::drawModals()
{
	editPopup();
	if (_scanPathPopup) {
		ImGui::OpenPopup("脚本源码文件夹");
		_scanPathPopup = false;
	}
	if (ImGui::BeginPopupModal("脚本源码文件夹", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextUnformatted("Mod Organizer 的 mods 文件夹（或游戏的 Data 文件夹）：");
		ImGui::SetNextItemWidth(700);
		ImGui::InputText("##scanpath", _scanPathBuf, sizeof(_scanPathBuf));
		if (ImGui::Button("扫描", ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
			scanSources(platform::FromUtf8(_scanPathBuf));
		}
		ImGui::SameLine();
		if (ImGui::Button("取消", ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	if (_confirmOpen) {
		ImGui::OpenPopup("##confirm");
		_confirmOpen = false;
	}
	ImGui::SetNextWindowSize(ImVec2(520, 0));
	if (ImGui::BeginPopupModal("##confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
		ImGui::TextUnformatted(_confirmTitle.c_str());
		ImGui::Separator();
		ImGui::TextWrapped("%s", _confirmText.c_str());
		ImGui::Spacing();
		if (ImGui::Button("确定", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
			auto act = std::move(_confirmAction);
			_confirmAction = nullptr;
			ImGui::CloseCurrentPopup();
			if (act) {
				act();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("取消", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
			_confirmAction = nullptr;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	if (!_error.empty()) {
		ImGui::OpenPopup("错误##err");
	}
	ImGui::SetNextWindowSize(ImVec2(560, 0));
	if (ImGui::BeginPopupModal("错误##err", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextWrapped("%s", _error.c_str());
		if (ImGui::Button("确定", ImVec2(120, 0))) {
			_error.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	drawRemoveModModal();
	drawOrphansModal();
	if (_save) {
		drawBatchModal();
		drawReportModal();
	}

	if (_purgeOpen) {
		ImGui::OpenPopup("清除某个脚本");
		_purgeOpen = false;
	}
	ImGui::SetNextWindowSize(ImVec2(520, 0));
	if (ImGui::BeginPopupModal("清除某个脚本", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextWrapped("移除该脚本的所有线程、挂起堆栈、函数消息和实例。脚本本身仍保持已定义。");
		ImGui::InputTextWithHint("##pname", "脚本名，例如 Firefly", _purgeName, sizeof(_purgeName));
		ImGui::Checkbox("只停止它正在运行和排队的工作；保留其实例及其数据", &_purgeKeep);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("适用于你打算继续安装的模组，例如卡住的 MCM 注册（QuickLootIEMCM）。\n"
							  "其他脚本指向该脚本的属性仍然有效，其设置也会保留。");
		}
		ImGui::BeginDisabled(_purgeKeep);
		ImGui::Checkbox("同时删除其实例所在的生成对象（FF......）", &_purgeForms);
		ImGui::EndDisabled();
		if (ImGui::Button("清除", ImVec2(120, 0)) && _purgeName[0]) {
			auto x = ops::PurgeScript(*_save, _purgeName, _purgeForms && !_purgeKeep, _purgeKeep);
			ImGui::CloseCurrentPopup();
			markEdited(Fmt("已清除 %s：%zu 个线程、%zu 个堆栈、%zu 条消息、%zu 个实例、%zu 个生成对象", _purgeName, x.threads, x.stacks,
				x.messages, x.instances, x.forms));
		}
		ImGui::SameLine();
		if (ImGui::Button("取消", ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	if (_openPathPopup) {
		ImGui::OpenPopup("打开存档");
		_openPathPopup = false;
	}
	if (ImGui::BeginPopupModal("打开存档", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::SetNextItemWidth(700);
		ImGui::InputText("##path", _pathBuf, sizeof(_pathBuf));
		if (ImGui::Button("打开", ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
			open(platform::FromUtf8(_pathBuf));
		}
		ImGui::SameLine();
		if (ImGui::Button("取消", ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	if (_savePathPopup) {
		ImGui::OpenPopup("另存为");
		_savePathPopup = false;
	}
	if (ImGui::BeginPopupModal("另存为", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::SetNextItemWidth(700);
		ImGui::InputText("##spath", _pathBuf, sizeof(_pathBuf));
		if (ImGui::Button("保存", ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
			doSave(platform::FromUtf8(_pathBuf));
		}
		ImGui::SameLine();
		if (ImGui::Button("取消", ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	if (_showAbout) {
		ImGui::OpenPopup("关于 ReSaver++");
		_showAbout = false;
	}
	if (ImGui::BeginPopupModal("关于 ReSaver++", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextUnformatted("ReSaver++ 版本 " RESAVERPP_VERSION "  -  Skyrim SE/AE 存档编辑器");
		ImGui::Spacing();
		ImGui::TextUnformatted("ReSaver（FallrimTools，作者 Mark Fairchild）的 C++ 移植版。");
		ImGui::TextUnformatted("与原版一样，按 Apache License 2.0 授权。");
		ImGui::Spacing();
		ImGui::TextDisabled("使用 Dear ImGui（MIT）、GLFW（zlib）、LZ4（BSD-2）、miniz（MIT）、ImGuiColorTextEdit（MIT）。");
		if (ImGui::Button("关闭", ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

void App::drawStatus()
{
	ImGui::Separator();
	const bool  busy = (scanning() && _scanProgress) || (namesLoading() && _dbProgress);
	const float winW = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
	// the status text shares one line with the progress bar and, when idle, with the watcher hint
	// and the live-game status: keep the Chinese status lines clear of them
	const float progW = busy ? std::min(520.0f, winW * 0.42f) : 0.0f;
	const float liveW = 330.0f;  // drawLiveStatus() draws right-aligned in this width
	const float sideW = busy ? progW : (_watch ? 160.0f : 0.0f) + liveW;
	const float textW = std::max(140.0f, winW - sideW - 24.0f);
	const ImVec2 at = ImGui::GetCursorScreenPos();
	ImGui::PushClipRect(ImVec2(at.x, at.y - 2), ImVec2(at.x + textW, at.y + ImGui::GetTextLineHeight() + 4), true);
	if (_dirty) {
		TextColored(theme::Warn(), "未保存的修改");
		ImGui::SameLine();
	}
	ImGui::TextUnformatted(_status.c_str());
	ImGui::PopClipRect();
	// the open save, centred: name and folder (the folder shortened from the left if it does not fit)
	if (_save && !_path.empty()) {
		const float afterStatus = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 30;
		const float rightReserve = 360;
		const float avail = ImGui::GetWindowWidth() - afterStatus - rightReserve;
		const auto  name = platform::ToUtf8(_path.filename());
		std::string folder = platform::ToUtf8(_path.parent_path());
		auto        text = [&] { return name + "   " + folder; };
		while (folder.size() > 8 && ImGui::CalcTextSize(text().c_str()).x > avail) {
			folder = "..." + folder.substr(std::min<std::size_t>(folder.size(), 12));
		}
		const float w = ImGui::CalcTextSize(text().c_str()).x;
		if (w <= avail) {
			float x = (ImGui::GetWindowWidth() - w) * 0.5f;
			x = std::max(x, afterStatus);
			ImGui::SameLine(x);
			ImGui::TextUnformatted(name.c_str());
			ImGui::SameLine();
			ImGui::TextDisabled("%s", folder.c_str());
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", platform::ToUtf8(_path).c_str());
			}
		}
	}
	if (busy) {
		ImGui::SameLine(ImGui::GetWindowWidth() - progW - ImGui::GetStyle().WindowPadding.x);
		drawBackgroundProgress(progW);
	} else {
		float right = 0;
		if (_watch) {
			right = 160;
			ImGui::SameLine(ImGui::GetWindowWidth() - right);
			TextColored(theme::Good(), "正在监视存档");
		}
		ImGui::SameLine(ImGui::GetWindowWidth() - right - liveW);
		drawLiveStatus();
	}
}

// ==========================================================================================
// variable editing
// ==========================================================================================

void App::beginEdit(Variable* a_var)
{
	_editVar = a_var;
	Variable* v = a_var;
	if (v->type == VType::kVariant && v->inner) {
		v = v->inner.get();
	}
	_editInt = static_cast<std::int32_t>(v->value);
	_editFloat = v->asFloat();
	_editBool = v->value != 0;
	std::snprintf(_editStr, sizeof(_editStr), "%s", v->type == VType::kString ? text::ToUtf8(_save->papyrus()->str(static_cast<TStr>(v->value))).c_str() : "");
	_editOpen = true;
}

void App::editPopup()
{
	if (_editOpen) {
		ImGui::OpenPopup("编辑值");
		_editOpen = false;
	}
	ImGui::SetNextWindowSize(ImVec2(520, 0));
	if (!ImGui::BeginPopupModal("编辑值", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		return;
	}
	if (!_editVar || !_save) {
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	auto&     p = *_save->papyrus();
	Variable* v = _editVar;
	if (v->type == VType::kVariant && v->inner) {
		v = v->inner.get();
	}
	ImGui::Text("类型：%s", VTypeName(v->type));
	ImGui::TextWrapped("当前值：%s", describeVar(*_editVar).c_str());
	ImGui::Separator();

	bool apply = false;
	switch (v->type) {
	case VType::kInt:
		ImGui::InputInt("##int", &_editInt);
		apply = ImGui::Button("应用", ImVec2(120, 0));
		if (apply) {
			v->value = static_cast<std::uint32_t>(_editInt);
		}
		break;
	case VType::kFloat:
		ImGui::InputFloat("##flt", &_editFloat, 0, 0, "%.6g");
		apply = ImGui::Button("应用", ImVec2(120, 0));
		if (apply) {
			std::uint32_t bits;
			std::memcpy(&bits, &_editFloat, 4);
			v->value = bits;
		}
		break;
	case VType::kBool:
		ImGui::Checkbox("True", &_editBool);
		apply = ImGui::Button("应用", ImVec2(120, 0));
		if (apply) {
			v->value = _editBool ? 1 : 0;
		}
		break;
	case VType::kString:
		ImGui::SetNextItemWidth(480);
		ImGui::InputText("##str", _editStr, sizeof(_editStr));
		apply = ImGui::Button("应用", ImVec2(120, 0));
		if (apply) {
			v->value = p.internString(text::FromUtf8(_editStr, p.str(static_cast<TStr>(v->value))));
		}
		break;
	case VType::kNull:
		ImGui::TextDisabled("此变量为空，没有可编辑的值。");
		break;
	default:
		ImGui::TextWrapped("对象、数组和结构体链接只能清除。清除后脚本在这里看到的是 None。");
		apply = v->value != 0 && ImGui::Button("设为无", ImVec2(160, 0));
		if (apply) {
			v->value = 0;
		}
		break;
	}
	if (apply) {
		markChanged("已编辑一个变量");
		_editVar = nullptr;
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("取消", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
		_editVar = nullptr;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

// ==========================================================================================
// settings and script sources
// ==========================================================================================

void App::loadSettings()
{
	GameSetup se, vr;
	_settingsLoaded = true;
	std::ifstream f(platform::SettingsFile());
	for (std::string line; std::getline(f, line);) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.rfind("sources=", 0) == 0) {
			se.sourcesPath = line.substr(8);
		} else if (line.rfind("profile=", 0) == 0) {
			se.profile = line.substr(8);
		} else if (line.rfind("sources_vr=", 0) == 0) {
			vr.sourcesPath = line.substr(11);
		} else if (line.rfind("profile_vr=", 0) == 0) {
			vr.profile = line.substr(11);
		} else if (line.rfind("game_vr=", 0) == 0) {
			vr.gamePath = line.substr(8);
		} else if (line.rfind("vr=", 0) == 0) {
			_vr = line.substr(3) == "1";
		} else if (line.rfind("sort=", 0) == 0) {
			_sortMode = std::clamp(std::atoi(line.c_str() + 5), 0, 3);
		} else if (line.rfind("compression=", 0) == 0) {
			_saveCompression = std::clamp(std::atoi(line.c_str() + 12), -1, 2);
		} else if (line.rfind("names=", 0) == 0) {
			_autoNames = line.substr(6) != "0";
		} else if (line.rfind("watch=", 0) == 0 && line.substr(6) == "1") {
			_watch = true;
		} else if (line.rfind("live=", 0) == 0) {
			_liveEnabled = line.substr(5) != "0";
		} else if (line.rfind("live_notify=", 0) == 0) {
			_liveNotify = std::clamp(std::atoi(line.substr(12).c_str()), 0, 2);
		} else if (line.rfind("live_silent=", 0) == 0) {
			_liveSilentRefresh = line.substr(12) != "0";
		} else if (line.rfind("theme=", 0) == 0) {
			const int k = std::clamp(std::atoi(line.c_str() + 6), 0, 2);
			theme::Apply(static_cast<theme::Kind>(k));
			_editorLight = theme::IsLight();
		} else if (line.rfind("codepage=", 0) == 0) {
			text::SetCodePage(static_cast<unsigned>(std::strtoul(line.c_str() + 9, nullptr, 10)));
		} else if (line.rfind("game=", 0) == 0) {
			se.gamePath = line.substr(5);
		} else if (line.rfind("saves_dir=", 0) == 0) {
			_savesDirSe = line.substr(10);
		} else if (line.rfind("saves_dir_vr=", 0) == 0) {
			_savesDirVr = line.substr(13);
		} else if (line.rfind("last_dir=", 0) == 0) {
			_lastDirSe = line.substr(9);
		} else if (line.rfind("last_dir_vr=", 0) == 0) {
			_lastDirVr = line.substr(12);
		} else if (line.rfind("decompile_always=", 0) == 0) {
			_decompileAlways = line.substr(17) == "1";
		} else if (line.rfind("editor_light=", 0) == 0) {
			// (older versions: the script editor now follows the theme)
		} else if (line.rfind("compiler=", 0) == 0) {
			_compilerSetting = line.substr(9);
		} else if (line.rfind("compile_out_mode=", 0) == 0) {
			_compileOutMode = std::clamp(std::atoi(line.c_str() + 17), 0, 2);
		} else if (line.rfind("compile_out_dir=", 0) == 0) {
			_compileOutDir = line.substr(16);
		} else if (line.rfind("compile_flags=", 0) == 0) {
			_compileFlags = line.substr(14);
		} else if (line.rfind("compile_imports=", 0) == 0) {
			_compileImports = line.substr(16);  // ';'-separated
		} else if (line.rfind("compile_optimize=", 0) == 0) {
			_compileOptimize = line.substr(17) == "1";
		} else if (line.rfind("compile_anonymize=", 0) == 0) {
			_compileAnonymize = line.substr(18) == "1";
		} else if (line.rfind("compile_backup=", 0) == 0) {
			_compileBackup = line.substr(15) == "1";
		}
	}
	// the active game's setup, the other one kept aside
	const auto& act = _vr ? vr : se;
	_sourcesPath = act.sourcesPath;
	_profile = act.profile;
	_gamePath = act.gamePath;
	_otherGame = _vr ? se : vr;
	autoDetectPaths(false);  // first start: find Mod Organizer / the game by themselves
	if (_watch) {
		setWatching(true);
	}
}

void App::saveSettings()
{
	std::ofstream f(platform::SettingsFile(), std::ios::trunc);
	const GameSetup cur{ _sourcesPath, _gamePath, _profile };
	const auto&     se = _vr ? _otherGame : cur;
	const auto&     vr = _vr ? cur : _otherGame;
	f << "vr=" << (_vr ? 1 : 0) << "\n";
	f << "sources=" << se.sourcesPath << "\n";
	f << "profile=" << se.profile << "\n";
	f << "sources_vr=" << vr.sourcesPath << "\n";
	f << "profile_vr=" << vr.profile << "\n";
	f << "game_vr=" << vr.gamePath << "\n";
	f << "names=" << (_autoNames ? 1 : 0) << "\n";
	f << "sort=" << _sortMode << "\n";
	f << "compression=" << _saveCompression << "\n";
	f << "watch=" << (_watch ? 1 : 0) << "\n";
	f << "game=" << se.gamePath << "\n";
	f << "saves_dir=" << _savesDirSe << "\n";
	f << "saves_dir_vr=" << _savesDirVr << "\n";
	f << "last_dir=" << _lastDirSe << "\n";
	f << "last_dir_vr=" << _lastDirVr << "\n";
	f << "codepage=" << text::CodePage() << "\n";
	f << "theme=" << static_cast<int>(theme::Current()) << "\n";
	f << "live=" << (_liveEnabled ? 1 : 0) << "\n";
	f << "live_notify=" << _liveNotify << "\n";
	f << "live_silent=" << (_liveSilentRefresh ? 1 : 0) << "\n";
	f << "decompile_always=" << (_decompileAlways ? 1 : 0) << "\n";
	f << "compiler=" << _compilerSetting << "\n";
	f << "compile_out_mode=" << _compileOutMode << "\n";
	f << "compile_out_dir=" << _compileOutDir << "\n";
	f << "compile_flags=" << _compileFlags << "\n";
	std::string imports = _compileImports;
	for (auto& c : imports) {
		if (c == '\n') {
			c = ';';
		}
	}
	imports.erase(std::remove(imports.begin(), imports.end(), '\r'), imports.end());
	f << "compile_imports=" << imports << "\n";
	f << "compile_optimize=" << (_compileOptimize ? 1 : 0) << "\n";
	f << "compile_anonymize=" << (_compileAnonymize ? 1 : 0) << "\n";
	f << "compile_backup=" << (_compileBackup ? 1 : 0) << "\n";
}

void App::scanSources(const std::filesystem::path& a_root)
{
	// Mod Organizer: use the profile's mod order so a script is credited to the mod whose copy
	// the game really loads (and disabled mods are ignored).
	std::vector<std::string> order;
	std::filesystem::path    overwrite, gameData;
	_mo2 = FindMo2Instance(a_root);
	_vortex.reset();
	if (!_mo2) {
		// Vortex: the staging folder, the game folder or its Data folder all lead to the deployment manifest
		_vortex = FindVortexDeployment(a_root);
		if (_vortex && _gamePath.empty()) {
			_gamePath = platform::ToUtf8(_vortex->dataPath.parent_path());
		}
	}
	if (_mo2) {
		if (_profile.empty() || std::find(_mo2->profiles.begin(), _mo2->profiles.end(), _profile) == _mo2->profiles.end()) {
			_profile = _mo2->selectedProfile;
		}
		order = LoadModList(_mo2->profilesDir / platform::FromUtf8(_profile));
		overwrite = _mo2->overwriteDir;
		if (!_mo2->gamePath.empty()) {
			gameData = _mo2->gamePath / "Data";
		}
	}
	// scan on a worker thread: reading hundreds of mod folders and their BSAs takes a while (much longer
	// inside Mod Organizer's virtual file system), and the window must keep responding meanwhile
	if (_scanThread.joinable()) {
		_scanProgress->cancel = true;
		_scanThread.join();
	}
	// with Vortex the mods folder is its staging folder, whichever folder led to it
	_scanRoot = _vortex ? _vortex->stagingPath : a_root;
	_scanDone = false;
	_scanResult = std::make_unique<ScriptSources>();
	_scanResult->gameDataVanillaOnly = platform::RunningUnderMO2();
	_scanProgress = std::make_unique<ScanProgress>();
	auto* res = _scanResult.get();
	auto* prog = _scanProgress.get();
	std::optional<VortexDeployment> vortex = _vortex;
	_scanThread = std::thread([this, res, prog, a_root, order, overwrite, gameData, vortex] {
		if (const char* d = std::getenv("RESAVERPP_TEST_SLOWSCAN")) {
			prog->testDelayMs = std::atoi(d);  // (test: a scan that takes long)
		}
		if (vortex) {
			res->scanVortex(*vortex, prog);
		} else {
			res->scan(a_root, order.empty() ? nullptr : &order, overwrite, gameData, prog);
		}
		_scanDone = true;
	});
	_status = "正在扫描模组中的脚本…";
}

void App::pollScan()
{
	if (!_scanThread.joinable() || !_scanDone) {
		return;
	}
	_scanThread.join();
	if (_scanProgress->cancel) {
		return;
	}
	const auto a_root = _scanRoot;
	if (_scanResult->modsScanned == 0 && !_scanResult->errors.empty()) {
		_error = "无法扫描 " + platform::ToUtf8(a_root) + "：\n\n" + _scanResult->errors.front();
		return;
	}
	_sources = std::move(*_scanResult);
	_scanResult.reset();
	_sourcesPath = platform::ToUtf8(a_root);
	saveSettings();
	_modsDirty = true;
	if (_save && _autoNames) {
		startNames(true);
	}
	_status = Fmt("已扫描 %zu 个模组%s：找到 %zu 个脚本（读取了 %zu 个 BSA%s）", _sources.modsScanned,
		_sources.vortex ? "（由 Vortex 部署）" : _sources.ranked ? ("（按 MO2 配置 “" + _profile + "” 的顺序）").c_str() : "", _sources.pexFound, _sources.bsasRead,
		_sources.errors.empty() ? "" : Fmt("，%zu 个无法读取", _sources.errors.size()).c_str());
	checkMissingPlugins();  // the load order and the installed mods are known now
	if (_pendingView) {
		const auto v = *_pendingView;
		_pendingView.reset();
		viewScript(v.script, v.function, v.state, v.instruction);
	}
}

// ==========================================================================================
// mods view
// ==========================================================================================

void App::ensureMods()
{
	if (!_save) {
		return;
	}
	if (_modsDirty || !_mods) {
		_mods = std::make_unique<mods::Table>(mods::Build(*_save, _sources.empty() ? nullptr : &_sources));
		_scriptTotals.clear();
		for (const auto& r : _mods->rows) {
			for (const auto& [k, s] : r.scripts) {
				auto& t = _scriptTotals[k];
				t.first += s.instances.size();
				t.second += s.threads.size();
			}
		}
		_modsDirty = false;
		_modOrder.clear();
	}
}

namespace
{
	const char* KindLabel(mods::Kind k)
	{
		switch (k) {
		case mods::Kind::kPlugin: return "插件";
		case mods::Kind::kLight: return "轻型插件（ESL）";
		case mods::Kind::kCreated: return "游戏过程中生成的对象（FF......）";
		case mods::Kind::kUnattached: return "未附加到任何对象的实例";
		case mods::Kind::kGlobal: return "没有对象、也没有已知模组的线程";
		case mods::Kind::kFolder: return "没有插件的模组文件夹（仅脚本）";
		}
		return "";
	}
}

void App::drawModsList()
{
	ensureMods();
	auto& rows = _mods->rows;

	if (scanning()) {
		drawBackgroundProgress(0);  // the lists fill in when the scan is done
	}
	ImGui::SetNextItemWidth(-360);  // room for the Chinese checkbox label, not just "Only with scripts"
	bool changed = ImGui::InputTextWithHint("##modfilter", "过滤模组", _filter, sizeof(_filter));
	ImGui::SameLine();
	changed |= ImGui::Checkbox("只显示带脚本或附魔物品的模组", &_modsOnlyScripts);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("隐藏既没有脚本（本存档中没有，且扫描模组后也没有随附）\n也没有任何物品栏中的附魔或充能物品的插件。取消勾选可列出存档中的所有插件。");
	}

	if (ImGui::Button(_sources.empty() ? "查找脚本源码（.pex）…" : "重新扫描脚本源码…")) {
		if (platform::HasNativeDialogs()) {
			if (auto p = platform::PickFolderDialog("选择你的 Mod Organizer “mods” 文件夹（或游戏的 Data 文件夹）")) {
				scanSources(*p);
			}
		} else {
			std::snprintf(_scanPathBuf, sizeof(_scanPathBuf), "%s", _sourcesPath.c_str());
			_scanPathPopup = true;
		}
	}
	ImGui::SameLine();
	if (_sources.empty()) {
		ImGui::TextDisabled("尚未扫描：脚本会归到运行它的对象所属的模组");
	} else {
		ImGui::TextDisabled("%s  （%zu 个模组，%zu 个脚本）", _sourcesPath.c_str(), _sources.modsScanned, _sources.pexFound);
		if (_mo2 && !_mo2->profiles.empty()) {
			ImGui::TextDisabled("MO2 配置：");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(260);
			if (ImGui::BeginCombo("##profile", _profile.c_str())) {
				for (const auto& pr : _mo2->profiles) {
					const std::string label = pr + (pr == _mo2->selectedProfile ? "  （MO2 中启用）" : "");
					if (ImGui::Selectable(label.c_str(), pr == _profile) && pr != _profile) {
						_profile = pr;
						scanSources(platform::FromUtf8(_sourcesPath));
					}
				}
				ImGui::EndCombo();
			}
		} else if (!_sources.ranked) {
			if (_sources.vortex && _vortex) {
				ImGui::TextDisabled("Vortex：已部署 %zu 个模组；每个脚本会归到 Vortex 部署它的那个模组", _vortex->mods.size());
			} else {
				ImGui::TextDisabled("未找到 MO2 模组顺序：当多个模组包含同一脚本时，可能归错模组");
			}
		}
	}
	ImGui::Separator();

	if (changed || _modOrder.empty()) {
		_modOrder.clear();
		const auto needle = Lower(_filter);
		// plugins with enchanted / charged items in some inventory are listed too (their items show in the detail)
		std::set<std::string> withCharged;
		if (_save && _modsOnlyScripts) {
			for (const auto& c : chargedItems()) {
				withCharged.insert(Lower(_save->pluginOf(c.inv.item)));
				if (c.inv.ench) {
					withCharged.insert(Lower(_save->pluginOf(c.inv.ench)));
				}
			}
		}
		for (std::size_t i = 0; i < rows.size(); ++i) {
			const auto& r = rows[i];
			if (_modsOnlyScripts && r.scriptCount == 0 && r.threadCount == 0 && r.provided.empty() && r.shipped.empty() &&
				!withCharged.contains(Lower(r.name))) {
				continue;
			}
			if (!needle.empty() && Lower(r.name).find(needle) == std::string::npos) {
				continue;
			}
			_modOrder.push_back(i);
		}
		if (_focusFirstRow && !_modOrder.empty()) {
			_focus = _modOrder.front();
			_focusFirstRow = false;
		}
	}

	const int cols = 5;  // the provider column is in the details pane, the list stays readable in Chinese
	if (!ImGui::BeginTable("mods", cols,
			ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
				ImGuiTableFlags_SizingStretchProp)) {
		return;
	}
	ImGui::TableSetupScrollFreeze(0, 1);
	// 2 字中文表头 + 排序箭头在 0.6 的权重下会被裁成「加」/「运…」: fixed widths for the short columns
	ImGui::TableSetupColumn("加载", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthFixed, 84.0f);
	ImGui::TableSetupColumn("模组", ImGuiTableColumnFlags_WidthStretch, 2.48f);
	ImGui::TableSetupColumn("脚本", ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthStretch, 1.0f);
	ImGui::TableSetupColumn("实例", ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthStretch, 1.0f);
	ImGui::TableSetupColumn("运行", ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthFixed, 84.0f);
	ImGui::TableHeadersRow();

	if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && (specs->SpecsDirty || changed)) {
		if (specs->SpecsCount > 0) {
			const auto& sp = specs->Specs[0];
			const bool  asc = sp.SortDirection == ImGuiSortDirection_Ascending;
			auto        key = [&](const mods::ModRow& r) -> long long {
                switch (sp.ColumnIndex) {
                case 0: return r.kind == mods::Kind::kPlugin ? r.index : r.kind == mods::Kind::kLight ? 1000 + r.index : 100000 + static_cast<int>(r.kind);
                case 2: return static_cast<long long>(r.scriptCount);
                case 3: return static_cast<long long>(r.instanceCount);
                case 4: return static_cast<long long>(r.threadCount);
                case 5: return static_cast<long long>(r.providedInSave);
                default: return 0;
                }
			};
			std::stable_sort(_modOrder.begin(), _modOrder.end(), [&](std::size_t a, std::size_t b) {
				if (sp.ColumnIndex == 1) {
					const auto x = Lower(rows[a].name), y = Lower(rows[b].name);
					return asc ? x < y : x > y;
				}
				const auto x = key(rows[a]), y = key(rows[b]);
				return asc ? x < y : x > y;
			});
		}
		specs->SpecsDirty = false;
	}

	ImGuiListClipper clip;
	clip.Begin(static_cast<int>(_modOrder.size()));
	while (clip.Step()) {
		for (int n = clip.DisplayStart; n < clip.DisplayEnd; ++n) {
			const auto  idx = _modOrder[n];
			const auto& r = rows[idx];
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::PushID(static_cast<int>(idx));
			std::string load = r.kind == mods::Kind::kPlugin ? Fmt("%02X", r.index) : r.kind == mods::Kind::kLight ? Fmt("FE:%03X", r.index) : "";
			const bool  sel = _focus && *_focus == idx;
			if (ImGui::Selectable(load.empty() ? "--" : load.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) {
				_focus = idx;
			}
			ImGui::TableSetColumnIndex(1);
			if (r.kind != mods::Kind::kPlugin && r.kind != mods::Kind::kLight) {
				TextColored(theme::Dim(), r.name);
			} else {
				ImGui::TextUnformatted(r.name.c_str());
			}
			ImGui::TableSetColumnIndex(2);
			ImGui::Text("%zu", r.scriptCount);
			ImGui::TableSetColumnIndex(3);
			ImGui::Text("%zu", r.instanceCount);
			ImGui::TableSetColumnIndex(4);
			if (r.threadCount >= 20) {
				TextColored(theme::Warn(), std::to_string(r.threadCount));
			} else {
				ImGui::Text("%zu", r.threadCount);
			}
			ImGui::PopID();
		}
	}
	ImGui::EndTable();
}

void App::killThreads(const std::set<ThreadID>& a_threads, const std::set<EID>& a_instances, const std::string& a_what, bool a_terminateOnly)
{
	if (a_terminateOnly) {
		confirm("终止 " + a_what,
			Fmt("终止 %zu 个正在运行的线程（%s）？\n\n它们的代码会被替换为空操作，游戏一恢复它们就会结束。"
				"脚本仍保持附加状态，可在下一次事件时再次启动。",
				a_threads.size(), a_what.c_str()),
			[this, a_threads, a_what] { markChanged(Fmt("已终止 %zu 个线程（%s）", ops::TerminateThreads(*_save->papyrus(), a_threads), a_what.c_str())); });
		return;
	}
	if (a_instances.empty()) {
		confirm("停止 " + a_what,
			Fmt("停止 %s？\n\n- 删除 %zu 个运行中的线程及其挂起堆栈和排队调用\n\n"
				"脚本实例及其数据（设置、其他脚本指向的属性）都会保留，所以对于你继续安装的模组"
				"（例如卡住的 MCM 注册）这是安全的选择。脚本可在下次事件时再次运行。\n"
				"结果请用“文件 > 另存为”写出。",
				a_what.c_str(), a_threads.size()),
			[this, a_threads, a_what] {
				const auto t = ops::DeleteThreadsFully(*_save->papyrus(), a_threads);
				markChanged(Fmt("已停止 %s：删除了 %zu 个线程", a_what.c_str(), t));
				_report = std::make_unique<ops::Report>(ops::Analyze(*_save));
			});
		return;
	}
	// spawned objects: their instances are deleted together with the object itself
	std::set<std::uint32_t> forms;
	for (auto id : a_instances) {
		if (const auto* i = _save->papyrus()->findInstance(id); i && i->refID.type() == RefID::Type::kCreated) {
			forms.insert(i->refID.data);
		}
	}
	confirm("强制结束 " + a_what,
		Fmt("强制结束 %s？\n\n- 删除 %zu 个运行中的线程及其挂起堆栈和排队调用\n- 删除 %zu 个脚本实例%s\n\n"
			"来自插件的对象会在游戏下次读取此存档时重新获得一份脚本副本（属性为默认值）。结果请用"
			"“文件 > 另存为”写出。",
			a_what.c_str(), a_threads.size(), a_instances.size(),
			forms.empty() ? "" : Fmt("\n- 删除它们所在的 %zu 个生成对象", forms.size()).c_str()),
		[this, a_threads, a_instances, forms, a_what] {
			auto&      p = *_save->papyrus();
			const auto t = ops::DeleteThreadsFully(p, a_threads);
			const auto i = ops::DeleteInstances(p, a_instances);
			const auto f = ops::DeleteChangeForms(*_save, forms);
			markChanged(Fmt("已强制结束 %s：%zu 个线程、%zu 个实例、%zu 个生成对象", a_what.c_str(), t, i, f));
			_report = std::make_unique<ops::Report>(ops::Analyze(*_save));
		});
}

void App::detailMod(std::uint64_t k)
{
	ensureMods();
	if (k >= _mods->rows.size()) {
		return;
	}
	const auto& r = _mods->rows[k];
	ImGui::TextUnformatted(r.name.c_str());
	ImGui::TextDisabled("%s%s", KindLabel(r.kind),
		r.kind == mods::Kind::kPlugin ? Fmt("，加载索引 %02X", r.index).c_str() : r.kind == mods::Kind::kLight ? Fmt("，FE:%03X", r.index).c_str() : "");
	if (!_sources.empty() && (r.kind == mods::Kind::kPlugin || r.kind == mods::Kind::kLight)) {
		auto it = _sources.pluginFolder.find(Lower(r.name));
		if (it != _sources.pluginFolder.end()) {
			ImGui::TextDisabled("安装在模组文件夹：%s", it->second.c_str());
		}
	}
	ImGui::Text("%zu 个脚本在其对象上  |  %zu 个实例  |  %zu 个运行中的线程", r.scriptCount, r.instanceCount, r.threadCount);

	std::set<ThreadID> allThreads;
	std::set<EID>      allInst, everyInst;
	for (const auto& [key, s] : r.scripts) {
		allThreads.insert(s.threads.begin(), s.threads.end());
		everyInst.insert(s.instances.begin(), s.instances.end());
		if (!s.engineType) {
			allInst.insert(s.instances.begin(), s.instances.end());
		}
	}
	// The Chinese labels are wider than the English ones were: lay the buttons out by their real
	// width and start a new line when a button would not fit (the pane is resizable).
	const std::string btnTerminate = Fmt("终止线程（%zu）", allThreads.size());
	const std::string btnStop = Fmt("停止线程（%zu）", allThreads.size());
	const std::string btnKillScripts = Fmt("强制结束脚本（%zu）", allInst.size());
	const std::string btnKillAll = Fmt("强制结束实例（%zu）", everyInst.size());
	const std::string btnReport = "报告…";
	const std::string btnRemove = "从存档移除模组…";
	// leave room for the pane's scrollbar/padding: at the exact boundary ImGui overflowed the child
	const float rowEnd = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
	// A button leaves the cursor at the start of the NEXT line, so the end of the row has to be tracked
	// here (GetCursorPosX() after a button is the line start, not the button's right edge).
	float rowX = ImGui::GetCursorPosX();
	bool  firstOnRow = true;
	auto  place = [&](const std::string& a_label) {
        const float w = ImGui::CalcTextSize(a_label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (!firstOnRow) {
            const float nextX = rowX + ImGui::GetStyle().ItemSpacing.x;
            if (nextX + w > rowEnd) {
                ImGui::NewLine();
                rowX = ImGui::GetCursorPosX();
            } else {
                ImGui::SameLine(nextX);
                rowX = nextX;
            }
        } else {
            rowX = ImGui::GetCursorPosX();
        }
        firstOnRow = false;
        rowX += w;
	};
	ImGui::BeginDisabled(allThreads.empty());
	place(btnTerminate);
	if (ImGui::Button(btnTerminate.c_str())) {
		killThreads(allThreads, {}, r.name + " 的所有脚本", true);
	}
	ImGui::EndDisabled();
	ImGui::BeginDisabled(allThreads.empty());
	place(btnStop);
	if (ImGui::Button(btnStop.c_str())) {
		killThreads(allThreads, {}, r.name + " 的所有运行中的脚本", false);
	}
	ImGui::EndDisabled();
	ImGui::BeginDisabled(allThreads.empty() && allInst.empty());
	place(btnKillScripts);
	if (ImGui::Button(btnKillScripts.c_str())) {
		killThreads(allThreads, allInst, r.name + " 的所有脚本", false);
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("删除它运行中的线程及其自有脚本的实例。\n游戏的引擎绑定（Form、Quest、ReferenceAlias 等）不受影响。");
	}
	ImGui::BeginDisabled(allThreads.empty() && everyInst.empty());
	place(btnKillAll);
	if (ImGui::Button(btnKillAll.c_str())) {
		killThreads(allThreads, everyInst, r.name + " 的对象上的所有脚本实例（含引擎绑定）", false);
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("删除它运行中的线程，以及其对象上的每一个脚本实例，包括游戏的\n"
						  "引擎绑定（Form、Quest、ReferenceAlias 等）。游戏会在下次读档时用默认值为插件\n"
						  "对象重建这些绑定；它们所在的生成对象会被删除。");
	}
	place(btnReport);
	if (ImGui::Button(btnReport.c_str())) {
		_reportOpen = true;
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("生成报告：每个模组及其脚本和实例，以及发现的异常（文本或 PDF）。");
	}
	if (r.kind == mods::Kind::kPlugin || r.kind == mods::Kind::kLight || r.kind == mods::Kind::kFolder) {
		place(btnRemove);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.24f, 0.24f, 1.0f));
		if (ImGui::Button(btnRemove.c_str())) {
			_removeMod = r.name;
			_removeOpen = true;
			_removePlan = std::make_unique<modremove::Plan>(modremove::MakePlan(*_save, _removeMod, _sources.empty() ? nullptr : &_sources, _removeOpt));
		}
		ImGui::PopStyleColor(2);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("先做安全检查，然后移除它的脚本、线程、排队调用及其表单的已保存状态，\n"
							  "并把结果写入一个新的存档文件。当前存档不会被覆盖。");
		}
	}

	ImGui::SeparatorText("运行在此模组对象上的脚本");
	if (r.scripts.empty()) {
		ImGui::TextDisabled("（无）");
	} else if (ImGui::BeginTable("modscripts", _mods->hasSources ? 5 : 4,
				   ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
					   ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
		// click a header to sort; click again to reverse
		ImGui::TableSetupColumn("脚本", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortAscending, 1.3f, 0);
		ImGui::TableSetupColumn("实例", ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthFixed, 84.0f, 1);
		ImGui::TableSetupColumn("运行", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthFixed,
			84.0f, 2);
		if (_mods->hasSources) {
			ImGui::TableSetupColumn("来源", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortAscending, 0.8f, 3);
		}
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, 220.0f, 4);
		ImGui::TableHeadersRow();

		std::vector<const mods::ScriptRow*> list;
		for (const auto& [key, s] : r.scripts) {
			list.push_back(&s);
		}
		int  sortCol = -1;
		bool ascending = false;
		if (const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsCount > 0) {
			sortCol = static_cast<int>(specs->Specs[0].ColumnUserID);
			ascending = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
		}
		auto lessNoCase = [](const std::string& a, const std::string& b) {
			return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
				return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
			});
		};
		// returns <0, 0, >0 for the chosen column
		auto cmp = [&](const mods::ScriptRow* a, const mods::ScriptRow* b) -> int {
			switch (sortCol) {
			case 0:
				return lessNoCase(a->script, b->script) ? -1 : (lessNoCase(b->script, a->script) ? 1 : 0);
			case 1:
				return a->instances.size() < b->instances.size() ? -1 : (a->instances.size() > b->instances.size() ? 1 : 0);
			case 2:
				return a->threads.size() < b->threads.size() ? -1 : (a->threads.size() > b->threads.size() ? 1 : 0);
			case 3:
				return lessNoCase(a->provider, b->provider) ? -1 : (lessNoCase(b->provider, a->provider) ? 1 : 0);
			default:
				return 0;
			}
		};
		std::stable_sort(list.begin(), list.end(), [&](const mods::ScriptRow* a, const mods::ScriptRow* b) {
			if (a->engineType != b->engineType) {
				return b->engineType;  // engine-type bindings always last
			}
			if (const int c = cmp(a, b); c != 0) {
				return ascending ? c < 0 : c > 0;
			}
			// unsorted / ties: busiest first, then by name
			if (a->threads.size() != b->threads.size()) {
				return a->threads.size() > b->threads.size();
			}
			if (a->instances.size() != b->instances.size()) {
				return a->instances.size() > b->instances.size();
			}
			return lessNoCase(a->script, b->script);
		});
		for (const auto* s : list) {
			ImGui::TableNextRow();
			ImGui::PushID(s);
			ImGui::TableSetColumnIndex(0);
			if (s->engineType) {
				TextColored(theme::Dim(), s->script + "  （引擎类型）");
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("不是模组脚本：这是游戏自身在 %s 与 Papyrus 之间的绑定。\n“强制结束”不会动它。", s->script.c_str());
				}
			} else {
				ImGui::TextUnformatted(s->script.c_str());
				scriptItem(s->script);
			}
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%zu", s->instances.size());
			ImGui::TableSetColumnIndex(2);
			if (s->threads.size() >= 20) {
				TextColored(theme::Warn(), std::to_string(s->threads.size()));
			} else {
				ImGui::Text("%zu", s->threads.size());
			}
			int col = 3;
			if (_mods->hasSources) {
				ImGui::TableSetColumnIndex(col++);
				ImGui::TextUnformatted(s->provider.empty() ? "?" : s->provider.c_str());
			}
			ImGui::TableSetColumnIndex(col);
			// tighter spacing: four two-character Chinese buttons have to fit the action column
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 3));
			bool firstBtn = true;
			auto placeBtn = [&](const char*) {
				if (!firstBtn) {
					ImGui::SameLine();
				}
				firstBtn = false;
			};
			placeBtn("终止");
			ImGui::BeginDisabled(s->threads.empty());
			if (ImGui::SmallButton("终止")) {
				killThreads(s->threads, {}, s->script + " 于 " + r.name, true);
			}
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				ImGui::SetTooltip("终止：线程在游戏恢复它们时结束（线程保留，代码被替换为空操作）。");
			}
			placeBtn("停止");
			ImGui::BeginDisabled(s->threads.empty());
			if (ImGui::SmallButton("停止")) {
				killThreads(s->threads, {}, s->script + " 于 " + r.name, false);
			}
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				ImGui::SetTooltip("停止：删除线程与排队调用，但保留脚本实例及其数据。\n对打算继续安装的模组安全（例如卡住的 MCM）。");
			}
			placeBtn("结束");
			ImGui::BeginDisabled(s->engineType && s->threads.empty());
			if (ImGui::SmallButton("结束")) {
				killThreads(s->threads, s->engineType ? std::set<EID>{} : s->instances, s->script + " 于 " + r.name, false);
			}
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				ImGui::SetTooltip("结束：线程与实例一起删除（引擎绑定除外）。");
			}
			placeBtn("查看");
			const bool view = ImGui::SmallButton("查看");
			ImGui::PopStyleVar(2);
			if (view) {
				_cat = s->threads.empty() ? Cat::kInstances : Cat::kThreads;
				std::snprintf(_filter, sizeof(_filter), "%s", s->script.c_str());
				_rowsDirty = true;
				_selected.clear();
				_focus.reset();
				ImGui::PopID();
				ImGui::EndTable();
				return;
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}

	if (_mods->hasSources) {
		ImGui::SeparatorText(Fmt("此模组提供且在存档中的脚本（.pex），共 %zu 个", r.provided.size()).c_str());
		if (r.provided.empty()) {
			ImGui::TextDisabled("（无）");
		} else if (ImGui::BeginTable("provided", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
			ImGui::TableSetupColumn("脚本", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("实例（全部模组）", 0, 140);
			ImGui::TableSetupColumn("运行（全部模组）", 0, 130);
			ImGui::TableSetupColumn("", 0, 140);
			ImGui::TableHeadersRow();
			for (const auto& name : r.provided) {
				const auto it = _scriptTotals.find(Lower(name));
				const auto inst = it == _scriptTotals.end() ? 0 : it->second.first;
				const auto thr = it == _scriptTotals.end() ? 0 : it->second.second;
				ImGui::TableNextRow();
				ImGui::PushID(name.c_str());
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(name.c_str());
				scriptItem(name);
				if (const auto* prov = _sources.providers(name); prov && prov->size() > 1) {
					ImGui::SameLine();
					std::string others;
					for (const auto& x : *prov) {
						others += (others.empty() ? "" : ", ") + x.mod;
					}
					TextColored(theme::Dim(), "（也在：" + others + "）");
				}
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%zu", inst);
				ImGui::TableSetColumnIndex(2);
				ImGui::Text("%zu", thr);
				ImGui::TableSetColumnIndex(3);
				ImGui::BeginDisabled(inst == 0 && thr == 0);
				if (ImGui::SmallButton("全局强制结束")) {
					const std::string n = name;
					confirm("全局强制结束 " + n,
						"移除整个存档中 “" + n + "” 的所有线程、挂起堆栈、排队调用和实例，以及它们所在的生成对象？",
						[this, n] {
							auto x = ops::PurgeScript(*_save, n, true);
							markChanged(Fmt("已全局强制结束 %s：%zu 个线程、%zu 个实例、%zu 个生成对象", n.c_str(), x.threads, x.instances, x.forms));
							_report = std::make_unique<ops::Report>(ops::Analyze(*_save));
						});
				}
				ImGui::EndDisabled();
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		// scripts it ships that the save does not mention at all
		std::vector<std::string> idle;
		for (const auto& n : r.shipped) {
			if (!r.provided.contains(n) && !r.scripts.contains(Lower(n))) {
				bool inSave = false;
				for (const auto& pv : r.provided) {
					inSave = inSave || Lower(pv) == Lower(n);
				}
				if (!inSave) {
					idle.push_back(n);
				}
			}
		}
		ImGui::SeparatorText(Fmt("此模组随附但不在本存档中的其他脚本（%zu）", idle.size()).c_str());
		if (idle.empty()) {
			ImGui::TextDisabled("（无）");
		} else {
			ImGui::TextDisabled("未运行且没有已保存的数据，例如仅在效果激活期间存在的魔法效果脚本。\n"
								"双击可查看。");
			for (const auto& n : idle) {
				ImGui::BulletText("%s", n.c_str());
				scriptItem(n);
			}
		}
	}
	if (r.kind == mods::Kind::kPlugin || r.kind == mods::Kind::kLight) {
		drawModCharged(r.name);
	}
}

// ==========================================================================================
// remove mod from save
// ==========================================================================================

void App::drawRemoveModModal()
{
	if (_removeOpen) {
		ImGui::OpenPopup("从存档移除模组");
		_removeOpen = false;
	}
	ImGui::SetNextWindowSize(ImVec2(820, 620), ImGuiCond_Appearing);
	if (!ImGui::BeginPopupModal("从存档移除模组")) {
		return;
	}
	if (!_removePlan || !_save) {
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	const auto& plan = *_removePlan;
	bool        replan = false;
	replan |= ImGui::Checkbox("同时从其他模组的对象上移除它的脚本（仅此模组提供的脚本）", &_removeOpt.otherObjects);
	replan |= ImGui::Checkbox("删除仅承载其脚本的生成对象", &_removeOpt.spawnedObjects);
	if (replan) {
		_removePlan = std::make_unique<modremove::Plan>(modremove::MakePlan(*_save, _removeMod, _sources.empty() ? nullptr : &_sources, _removeOpt));
		ImGui::EndPopup();
		return;
	}
	if (_sources.empty()) {
		TextColored(theme::Warn(), "提示：先使用“查找脚本源码（.pex）…”，这样才能检查主文件、DLL 以及它在其他对象上的脚本。");
	}
	ImGui::Separator();
	ImGui::BeginChild("plan", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.2f), ImGuiChildFlags_None);
	if (!plan.blockers.empty()) {
		for (const auto& b : plan.blockers) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::Bad()));
			ImGui::TextWrapped("无法移除：%s", b.c_str());
			ImGui::PopStyleColor();
		}
		ImGui::Spacing();
	}
	ImGui::TextWrapped("%s", plan.summary().c_str());
	ImGui::EndChild();
	ImGui::Separator();

	auto target = _path;
	{
		auto modStem = std::filesystem::path(platform::FromUtf8(_removeMod)).stem().u8string();
		for (auto& ch : modStem) {
			if (ch == u8'[' || ch == u8']' || ch == u8' ') {
				ch = u8'_';
			}
		}
		const auto base = _path.stem().u8string() + u8"_without_" + modStem;
		target.replace_filename(base + u8".ess");
		std::error_code ec;
		for (int n = 2; std::filesystem::exists(target, ec); ++n) {
			target.replace_filename(base + u8"_" + std::u8string(reinterpret_cast<const char8_t*>(std::to_string(n).c_str())) + u8".ess");
		}
	}
	ImGui::TextDisabled("新文件：%s", platform::ToUtf8(target.filename()).c_str());
	ImGui::BeginDisabled(!plan.canApply());
	if (ImGui::Button("移除并写出新存档", ImVec2(240, 0))) {
		const auto res = modremove::Apply(*_save, plan);
		const auto what = _removeMod;
		_removePlan.reset();
		ImGui::CloseCurrentPopup();
		markEdited("已移除 " + what);
		_modsDirty = true;
		doSave(target);
		_report = std::make_unique<ops::Report>(ops::Analyze(*_save));
		if (_error.empty()) {
			_status = "已移除 " + what + "（" + res.text() + "），并写出 " + platform::ToUtf8(target.filename());
		}
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("取消", ImVec2(120, 0))) {
		_removePlan.reset();
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void App::fixMcmJam()
{
	if (!_save) {
		return;
	}
	const auto dry = ops::FixMcmJam(*_save->papyrus(), true);
	if (!dry.threads && !dry.stacks && !dry.messages && !dry.stateReset) {
		_status = "本存档中没有正在进行的 SkyUI MCM 注册工作，无需修复。";
		return;
	}
	confirm("修复 SkyUI MCM 注册卡死",
		Fmt("存档创建时 SkyUI 正在注册 MCM 菜单。此操作会移除卡住的注册工作：\n\n"
			"- %zu 个正在注册菜单或运行于 SKI_ConfigManager 内的线程\n"
			"- %zu 个挂起的 RegisterMod 调用和 %zu 个排队调用\n"
			"%s"
			"\n已注册的 %zu 个菜单保持注册状态。所有 MCM 都保留其设置。下次读档时 SkyUI "
			"会重新宣告自身，其余菜单会正常注册（请在游戏内不暂停等待约一分钟，再"
			"打开 MCM 或存档）。\n\n结果请用“文件 > 另存为”写出。",
			dry.threads, dry.stacks, dry.messages,
			dry.stateReset ? "- SKI_ConfigManager 被保存为 BUSY 状态；现已恢复为正常状态\n" : "", dry.registered),
		[this] {
			const auto r = ops::FixMcmJam(*_save->papyrus());
			markEdited("已修复 MCM 卡死：" + r.text());
		});
}

bool App::drawBackgroundProgress(float a_width)
{
	std::size_t done = 0, total = 0;
	std::string what, cur;
	if (scanning() && _scanProgress) {
		done = _scanProgress->done.load();
		total = _scanProgress->total.load();
		cur = _scanProgress->current();
		what = "正在扫描模组";
	} else if (namesLoading() && _dbProgress) {
		done = _dbProgress->done.load();
		total = _dbProgress->total.load();
		{
			std::lock_guard l(_dbProgress->m);
			cur = _dbProgress->current;
		}
		what = "正在读取插件名称";
	} else {
		return false;
	}
	const float       f = total ? std::min(1.0f, static_cast<float>(done) / static_cast<float>(total)) : 0.0f;
	const std::string label = total ? Fmt("%s：%zu / %zu（%d%%）%s%s", what.c_str(), done, total, static_cast<int>(f * 100.0f + 0.5f), cur.empty() ? "" : "  -  ",
										  cur.c_str())
									: what + "…";
	ImGui::ProgressBar(total ? f : -1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(a_width > 0 ? a_width : -1.0f, 0), "");
	// the text inside the bar, from its left edge (cut at the right end when it does not fit)
	const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
	auto*        dl = ImGui::GetWindowDrawList();
	dl->PushClipRect(a, b, true);
	const float pad = ImGui::GetStyle().FramePadding.x;
	dl->AddText(ImVec2(a.x + pad, a.y + (b.y - a.y - ImGui::GetFontSize()) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
	dl->PopClipRect();
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("%s", label.c_str());
	}
	return true;
}
