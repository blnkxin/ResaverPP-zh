// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Save check: everything ReSaver++ noticed about the open save, in one window that opens with the
// save. Each finding says what it means, and each thing that can be done about it says how likely
// it is to break the game, and why:
//   green  = safe: removes only what the game can no longer use, or changes nothing in the save
//   yellow = caution: safe in the normal case, read why before doing it
//   red    = risky: likely to break a mod or quest, or ReSaver++ cannot be sure; better not
#include "App.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

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

	void Wrapped(std::uint32_t col, const std::string& s)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, col);
		ImGui::TextWrapped("%s", s.c_str());
		ImGui::PopStyleColor();
	}

	std::uint32_t RiskColor(App::Risk r)
	{
		switch (r) {
		case App::Risk::kSafe: return theme::Good();
		case App::Risk::kCaution: return theme::Caution();
		case App::Risk::kRisky: return theme::Bad();
		default: return theme::Dim();
		}
	}

	const char* RiskWord(App::Risk r)
	{
		switch (r) {
		case App::Risk::kSafe: return "安全";
		case App::Risk::kCaution: return "谨慎";
		case App::Risk::kRisky: return "危险";
		default: return "提示";
		}
	}

	// a filled dot in the risk colour, on the current line
	void Dot(App::Risk r)
	{
		const float sz = ImGui::GetTextLineHeight();
		const auto  p = ImGui::GetCursorScreenPos();
		ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + sz * 0.5f, p.y + sz * 0.55f), sz * 0.36f, RiskColor(r));
		ImGui::Dummy(ImVec2(sz, sz));
		ImGui::SameLine();
	}

	// a button tinted with the risk colour
	bool RiskButton(const std::string& a_label, App::Risk a_risk, bool a_enabled)
	{
		ImVec4 c = ImGui::ColorConvertU32ToFloat4(RiskColor(a_risk));
		const bool light = theme::IsLight();
		auto       mix = [&](float k, float a) { return ImVec4(c.x * k, c.y * k, c.z * k, a); };
		ImGui::PushStyleColor(ImGuiCol_Button, mix(light ? 1.0f : 0.55f, light ? 0.22f : 0.55f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mix(light ? 1.0f : 0.7f, light ? 0.38f : 0.75f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, mix(light ? 1.0f : 0.85f, light ? 0.55f : 0.9f));
		ImGui::BeginDisabled(!a_enabled);
		const bool pressed = ImGui::Button(a_label.c_str(), ImVec2(330, 0));
		ImGui::EndDisabled();
		ImGui::PopStyleColor(3);
		return pressed && a_enabled;
	}
}

// ---- what is in the save -------------------------------------------------------------------------

std::vector<App::CheckItem> App::checkItems()
{
	std::vector<CheckItem> v;
	if (!_save) {
		return v;
	}
	auto* p = _save->papyrus();
	const auto info = _live.info();

	// damaged save: nothing can be written
	if (_readOnly) {
		CheckItem it;
		it.level = Risk::kRisky;
		it.title = "这个存档已损坏：以只读方式打开";
		std::string list;
		for (const auto& pr : _save->problems) {
			list += "\n  - " + pr;
		}
		it.what = "它的结构对不上，或采用了 ReSaver++ 还不能完全理解的布局：" + list +
				  "\n所有内容都能查看，但 ReSaver++ 不会写回它（那可能丢失数据，也可能凭空造出数据）。"
				  "请在游戏中读取一个更早的存档，或带上这个文件反馈问题。";
		v.push_back(std::move(it));
	}

	// mods no longer in the load order, scripts no longer installed
	if (!_missing.empty() || !_missingScripts.empty() || !_unfoundScripts.empty()) {
		std::size_t gone = 0, disabled = 0;
		for (const auto& m : _missing) {
			(m.installed ? disabled : gone) += 1;
		}
		CheckItem it;
		const bool suspect = _orderSuspect || _scanSuspect;
		it.level = suspect ? Risk::kRisky : Risk::kCaution;
		it.title = !_missing.empty() ? Fmt("本存档里有 %zu 个模组不在你的加载顺序中", _missing.size())
									 : Fmt("本存档里有 %zu 个脚本已不再安装", _missingScripts.size() + _unfoundScripts.size());
		std::string w;
		if (!_missing.empty()) {
			w = _installKnown ? Fmt("%s 中不存在：%zu 个已卸载，%zu 个已安装但未启用。 ", _missingSource.c_str(), gone, disabled)
							  : Fmt("%s 中不存在。 ", _missingSource.c_str());
		}
		if (!_missingScripts.empty()) {
			w += Fmt("%zu 个脚本因文件已不存在而无法被游戏加载。 ", _missingScripts.size());
		}
		if (!_unfoundScripts.empty()) {
			w += Fmt("%zu 个脚本在你的模组里找不到，但游戏存档时还带着它们（大概没问题）。 ", _unfoundScripts.size());
		}
		w += "它们的脚本和存档数据是游戏已不再使用的残留。";
		if (!_installKnown && scanning()) {
			w += "\n仍在扫描你的模组：扫描完成之前，分不清哪些模组是已卸载、哪些只是已停用。";
		}
		if (suspect) {
			w += "\n但 ReSaver++ 读到的加载顺序或模组文件夹与本存档对不上（选错了 MO2 配置、plugins.txt 过期，"
				 "或有些归档读不出来）。现在清理可能移除你仍在使用的模组。请先看“选项 > 设置”。";
		}
		it.what = w;
		CheckOption o;
		o.label = "检查并移除它们的残留…";
		o.risk = suspect ? Risk::kRisky : Risk::kCaution;
		o.why = suspect ? "模组列表看起来不对，所以什么都没勾选。移除你仍在使用的模组会删掉它的进度和设置。"
						: "写出一个不含这些内容的新存档；原文件不动。对已彻底卸载的模组是安全的。只是暂时停用的模组"
						  "（为了测试，或在另一个配置里）会丢掉进度和设置：这类不会被勾选。";
		o.act = [this] {
			_missingDismissed = false;
			for (auto& m : _missing) {
				m.planned = false;
			}
			_missingOpen = true;
		};
		it.options.push_back(std::move(o));
		v.push_back(std::move(it));
	}

	if (_report && p) {
		const auto& r = *_report;

		// scripts stuck in wait loops
		for (const auto& s : r.loopingScripts) {
			CheckItem it;
			it.level = Risk::kCaution;
			it.title = "脚本 “" + s + "” 正在同时运行很多份";
			it.what = "它有几十个线程停在等待循环里。永不停止的脚本（比如 Firefly 那个 bug）会拖慢整个脚本"
					  "引擎，而且每存一次档就更糟。";
			CheckOption o;
			o.label = "清除 " + s;
			o.risk = Risk::kCaution;
			o.why = "移除它的全部线程和数据，以及它们所在的生成对象。对失控的脚本这是对的。如果它属于你在用的"
					"模组，那个模组的功能会重新启动，或者一直停到模组再次启动它。";
			const std::string name = s;
			o.act = [this, name] {
				confirm("清除 " + name,
					"移除 “" + name +
						"” 的全部线程、挂起堆栈、函数消息和实例，以及它们所在的生成对象？\n\n只改内存里的存档：请用“文件 > 另存为”写出。",
					[this, name] {
						auto x = ops::PurgeScript(*_save, name, true);
						markEdited(Fmt("已清除 %s：%zu 个线程、%zu 个实例、%zu 个生成对象", name.c_str(), x.threads, x.instances, x.forms));
					});
			};
			it.options.push_back(std::move(o));
			v.push_back(std::move(it));
		}

		// SkyUI MCM registration jam
		if (r.mcmJam) {
			CheckItem it;
			it.level = Risk::kCaution;
			it.title = "存档创建时 SkyUI 仍在注册 MCM 菜单";
			it.what = "读档会把那些工作全部重放一遍，可能导致菜单缺失，或读档后游戏卡顿。";
			CheckOption o;
			o.label = "修复 SkyUI MCM 注册卡死…";
			o.risk = Risk::kSafe;
			o.why = "只移除卡住的注册工作。已注册的菜单保留设置；下次读档时 SkyUI 会重新注册其余的。";
			o.act = [this] { fixMcmJam(); };
			it.options.push_back(std::move(o));
			v.push_back(std::move(it));
		}

		// scripts that are no longer installed
		if (r.undefinedInstances || r.undefinedThreads) {
			CheckItem it;
			it.level = Risk::kCaution;
			it.title = Fmt("%zu 个脚本实例和 %zu 个线程属于游戏无法加载的脚本", r.undefinedInstances, r.undefinedThreads);
			it.what = "存档创建时找不到它们的脚本文件，所以游戏无法运行它们：死数据。";
			CheckOption o;
			o.label = "移除未定义元素";
			o.risk = Risk::kCaution;
			o.why = "游戏已无法使用这些脚本，但终止它们的线程也会结束调用它们的脚本，可能让任务或模组永远等待。"
					"如果那个模组又装回来了，或者它继承的某个脚本只是暂时缺失，这些数据仍然有意义。请先备份存档，再进游戏确认。";
			o.act = [this, p] {
				confirm("移除未定义元素",
					"删除 .pex 已不再安装的脚本、它们的实例和引用，并终止正在运行它们的线程？"
					"\n\n只改内存里的存档：请用“文件 > 另存为”写出。",
					[this, p] { markEdited(Fmt("已移除/终止 %zu 个未定义元素", ops::RemoveUndefinedElements(*p))); });
			};
			it.options.push_back(std::move(o));
			v.push_back(std::move(it));
		}

		// instances on spawned objects that are gone
		if (r.createdWithoutForm) {
			CheckItem it;
			it.level = Risk::kCaution;
			it.title = Fmt("%zu 个脚本实例位于已不存在的生成对象上", r.createdWithoutForm);
			it.what = "模组在游戏过程中创建的对象已被删除，但它们的脚本还留在存档里。";
			CheckOption o;
			o.label = "检查缺失的生成对象…";
			o.risk = Risk::kCaution;
			o.why = "先按模组分组列出它们。勾选的是未安装模组的那些；仍被其他脚本指向的会保留，除非你自己改。";
			o.act = [this] { openOrphans(); };
			it.options.push_back(std::move(o));
			v.push_back(std::move(it));
		}

		// instances attached to nothing
		if (r.unattached >= 100) {
			CheckItem it;
			it.level = Risk::kCaution;
			it.title = Fmt("%zu 个脚本实例没有附加到任何对象", r.unattached);
			it.what = "对象已不存在的脚本数据。少数几个是正常的；多了会拖慢读档。";
			CheckOption o;
			o.label = "移除未附加实例";
			o.risk = Risk::kSafe;
			o.why = "游戏里已经没有任何东西能访问它们。";
			o.act = [this, p] {
				confirm("移除未附加实例",
					Fmt("删除 %zu 个未附加到任何对象的脚本实例？\n\n只改内存里的存档：请用“文件 > 另存为”写出。",
						_report ? _report->unattached : 0),
					[this, p] { markEdited(Fmt("已移除 %zu 个未附加实例", ops::RemoveUnattachedInstances(*p))); });
			};
			it.options.push_back(std::move(o));
			v.push_back(std::move(it));
		}

		// a busy script engine with no single cause
		std::vector<std::string> heavy;
		if (r.threads >= 300) {
			heavy.push_back(Fmt("%zu 个运行中的脚本线程（健康的存档通常不到 ~150）", r.threads));
		}
		if (r.suspended >= 50) {
			heavy.push_back(Fmt("%zu 个挂起堆栈（正常只有几个）", r.suspended));
		}
		if (r.instances >= 150000) {
			heavy.push_back(Fmt("%zu 个脚本实例（非常高：每次读档都要重建）", r.instances));
		}
		if (!heavy.empty()) {
			CheckItem it;
			it.level = Risk::kCaution;
			it.title = "这个存档的脚本引擎非常繁忙";
			for (const auto& h : heavy) {
				it.what += "- " + h + "\n";
			}
			it.what += "找不到单一原因。“模组”页会显示哪个模组运行的脚本最多。";
			CheckOption o;
			o.label = "显示“模组”页";
			o.risk = Risk::kSafe;
			o.why = "只显示信息。";
			o.act = [this] {
				_cat = Cat::kMods;
				_showCheck = false;
			};
			it.options.push_back(std::move(o));
			v.push_back(std::move(it));
		}
	}

	// enchanted items that can never be recharged
	{
		std::size_t broken = 0;
		for (const auto& c : chargedItems()) {
			broken += cf::NeedsChargeFix(c.inv) ? 1 : 0;
		}
		if (broken) {
			CheckItem it;
			it.level = Risk::kCaution;
			it.title = Fmt("%zu 个附魔物品无法再充能", broken);
			it.what = "它们有附魔，但最大充能为 0（或充能值大于最大值），所以灵魂石不起作用。";
			CheckOption o;
			o.label = "把最大充能设为 3000";
			o.risk = Risk::kSafe;
			o.why = "只给这些物品补上缺少的最大充能；其他都不变。（可按模组设成自己的值：见“模组”页。）";
			o.act = [this] {
				std::vector<std::pair<std::uint32_t, std::uint32_t>> only;
				for (const auto& c : chargedItems()) {
					if (cf::NeedsChargeFix(c.inv)) {
						only.push_back({ c.holderRefID, c.inv.item });
					}
				}
				const auto n = cf::FixCharges(*_save, 3000, only);
				markEdited(Fmt("已给 %zu 个物品设置最大充能 3000", n));
			};
			it.options.push_back(std::move(o));
			v.push_back(std::move(it));
		}
	}

	// the journal shows objectives of quests that are over
	{
		std::size_t shownDone = 0;
		for (const auto& q : questStates()) {
			if (q.completed() || !q.running()) {
				shownDone += q.shownObjectives();
			}
		}
		if (shownDone) {
			CheckItem it;
			it.level = Risk::kInfo;
			it.title = Fmt("你的任务日志里有 %zu 个目标属于已完成或已停止的任务", shownDone);
			it.what = "已结束任务的残留。无害，但会占满日志，而日志太满会让新任务显示不出来。";
			CheckOption o;
			o.label = "在游戏中刷新任务日志";
			o.risk = Risk::kCaution;
			o.why = std::string(info.connected && info.inGame ? "在运行中的游戏里做；这个文件不会变。 "
															  : "需要游戏在运行、装了 ReSaverHelper.dll 并读了档。 ") +
					"先在游戏里存个档，运行期间不要打开菜单。使用日志类界面模组（如 QUI）时，用 helper 3 曾报告过之后打开菜单"
					"会崩溃；helper 4 更谨慎。如果游戏崩溃了，读取之前的那个存档。";
			o.enabled = info.connected && info.inGame && !_liveRefresh.running;
			o.act = [this] { startLiveRefresh(); };
			it.options.push_back(std::move(o));
			CheckOption q;
			q.label = "显示“任务”页";
			q.risk = Risk::kSafe;
			q.why = "只显示信息。";
			q.act = [this] {
				_cat = Cat::kQuests;
				_showCheck = false;
			};
			it.options.push_back(std::move(q));
			v.push_back(std::move(it));
		}
	}

	std::stable_sort(v.begin(), v.end(), [](const CheckItem& a, const CheckItem& b) { return static_cast<int>(a.level) > static_cast<int>(b.level); });
	return v;
}

App::Risk App::checkWorst()
{
	Risk worst = Risk::kSafe;
	for (const auto& it : checkItems()) {
		if (static_cast<int>(it.level) > static_cast<int>(worst)) {
			worst = it.level;
		}
	}
	return worst;
}

// ---- the window ----------------------------------------------------------------------------------

void App::drawSaveCheck()
{
	if (!_save) {
		_showCheck = false;
		return;
	}
	prepareWindow("Save check", ImVec2(0.62f, 0.72f));
	if (!ImGui::Begin("存档检查", &_showCheck, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Save check");
	// the file name may be long: wrap before the Maximize button instead of running under it
	ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 96);
	ImGui::TextWrapped("存档检查：%s", platform::ToUtf8(_path.filename()).c_str());
	ImGui::PopTextWrapPos();
	// legend
	Dot(Risk::kSafe);
	ImGui::TextUnformatted("安全");
	ImGui::SameLine(0, 24);
	Dot(Risk::kCaution);
	ImGui::TextUnformatted("谨慎：先读原因");
	ImGui::SameLine(0, 24);
	Dot(Risk::kRisky);
	ImGui::TextUnformatted("危险：可能弄坏模组或任务");
	ImGui::TextDisabled("在你用“文件 > 保存”或“另存为”之前，这里不会改动你的存档文件（旧文件会保留为 .bak）。\n"
						"移除模组会在旧存档旁边写出一个新存档。在意的存档还是先备份一下。");
	if (scanning()) {
		ImGui::TextDisabled("正在后台读取你的模组：读完后这个列表会更新。");
		drawBackgroundProgress(0);
	}
	ImGui::Separator();

	const auto items = checkItems();
	ImGui::BeginChild("##items", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.4f));
	if (items.empty()) {
		ImGui::Spacing();
		Dot(Risk::kSafe);
		Wrapped(theme::Good(), "这个存档没有发现问题。");
		ImGui::TextDisabled("其他内容（脚本、线程、任务、插件）都在左侧各页里。");
	}
	int id = 0;
	for (const auto& it : items) {
		ImGui::PushID(id++);
		ImGui::Spacing();
		Dot(it.level);
		ImGui::PushStyleColor(ImGuiCol_Text, it.level == Risk::kInfo ? ImGui::GetColorU32(ImGuiCol_Text) : RiskColor(it.level));
		ImGui::TextWrapped("%s", it.title.c_str());
		ImGui::PopStyleColor();
		ImGui::Indent(ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.x);
		ImGui::TextWrapped("%s", it.what.c_str());
		for (const auto& o : it.options) {
			if (RiskButton(o.label, o.risk, o.enabled)) {
				o.act();
			}
			tips::Tip(o.risk == Risk::kRisky ? "危险：可能弄坏某个模组或任务。请先读它旁边的说明，并备份存档。" : o.risk == Risk::kCaution ? "先看清楚：通常没事，但用之前读一下旁边的说明。" : "安全：不会伤到游戏。保存之前不会写入磁盘（文件 > 保存 或 另存为）。");
			ImGui::SameLine();
			const float x = ImGui::GetCursorPosX();
			TextWrappedAt(RiskColor(o.risk), std::string(RiskWord(o.risk)) + ": " + o.why, x);
		}
		ImGui::Unindent(ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.x);
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::PopID();
	}
	ImGui::EndChild();
	if (ImGui::Checkbox("打开存档时显示", &_checkOnOpen)) {
		saveSettings();
	}
	tips::Tip("每次打开存档时自动打开这个存档检查窗口。");
	ImGui::SameLine();
	ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 140);
	if (ImGui::Button("关闭", ImVec2(120, 0))) {
		_showCheck = false;
	}
	tips::Tip("关闭存档检查。想再打开：工具 > 存档检查（F2），或概览页上的那个按钮。");
	ImGui::End();
}

void App::TextWrappedAt(std::uint32_t a_col, const std::string& a_text, float a_x)
{
	// wrapped text that keeps its left edge at a_x on every line (beside a button)
	ImGui::PushStyleColor(ImGuiCol_Text, a_col);
	ImGui::PushTextWrapPos(0.0f);
	ImGui::BeginGroup();
	ImGui::SetCursorPosX(a_x);
	ImGui::TextWrapped("%s", a_text.c_str());
	ImGui::EndGroup();
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
}

// one line for the Overview page and the menu bar: how the save looks, and the way back to the window
void App::drawCheckSummary()
{
	const auto  items = checkItems();
	std::size_t red = 0, yellow = 0;
	for (const auto& it : items) {
		red += it.level == Risk::kRisky ? 1 : 0;
		yellow += it.level == Risk::kCaution ? 1 : 0;
	}
	const Risk worst = red ? Risk::kRisky : yellow ? Risk::kCaution : Risk::kSafe;
	Dot(worst);
	if (items.empty()) {
		Wrapped(theme::Good(), "存档检查：没有发现问题。");
	} else {
		ImGui::PushStyleColor(ImGuiCol_Text, RiskColor(worst));
		ImGui::TextUnformatted(Fmt("存档检查：%zu 处需要看一下%s", items.size(),
								   red ? Fmt("（%zu 处危险）", red).c_str() : yellow ? Fmt("（%zu 处要先读原因）", yellow).c_str() : "")
								   .c_str());
		ImGui::PopStyleColor();
	}
	ImGui::SameLine();
	if (ImGui::Button("打开存档检查")) {
		_showCheck = true;
		ImGui::SetWindowFocus("存档检查");
	}
	tips::Tip("打开 ReSaver++ 对这个存档注意到的一切，以及每一处可以安全地怎么处理（F2）。");
}
