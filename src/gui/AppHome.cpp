// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// The main screen's shortcut buttons: the Clean and Tools menu entries (and Open script) as buttons under
// the overview, so players do not have to dig through the menus. Choosing one (or anything from the top
// menu) hides them and the tool opens as usual; "Main menu" on the menu bar brings them back.
#include "App.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "core/Ops.h"

#include "imgui.h"

#include <algorithm>
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

	enum class HomeRisk
	{
		kNone,     // looks, does not change the save
		kSafe,     // green
		kCaution,  // yellow: read what it does first
		kRisky     // red
	};
}

bool App::anyToolOpen() const
{
	return _showTransplant || _missingOpen || _orphansOpen || _showAnalysis || _purgeOpen || _showLookup || _showActors || _showPlayer ||
	       _showCompare || _batchOpen || _reportOpen || _showArrays || _showViewer || _removeOpen || _showLeftovers || _showHistory ||
	       _showPapyrusLog || _showSavesMgr || _showMerge;
}

void App::goHome()
{
	_showTransplant = _missingOpen = _orphansOpen = _showAnalysis = _purgeOpen = _showLookup = _showActors = _showPlayer = false;
	_showCompare = _batchOpen = _reportOpen = _showArrays = _removeOpen = false;
	_showViewer = false;
	_showCheck = false;
	_showLeftovers = _showHistory = _showPapyrusLog = _showSavesMgr = _showMerge = false;
	if (_cat != Cat::kOverview) {
		select(Cat::kOverview, 0);
	}
	_homePanel = true;
}

void App::shortcut(Sc a_which)
{
	if (!_save && a_which != Sc::kOpenPex) {
		return;
	}
	_homePanel = false;  // the chosen tool gets the screen
	auto* p = _save ? _save->papyrus() : nullptr;
	switch (a_which) {
	case Sc::kRemoveUnattached:
		confirm("移除未附加实例", Fmt("删除 %zu 个没有附着到任何对象的脚本实例？", _report ? _report->unattached : 0),
			[this, p] { markEdited(Fmt("已移除 %zu 个未附加实例", ops::RemoveUnattachedInstances(*p))); });
		break;
	case Sc::kRemoveUndefined:
		confirm("移除未定义元素",
			"删除 .pex 已不再安装的脚本及其实例与引用，并终止正在运行它们的线程？",
			[this, p] { markEdited(Fmt("已移除/终止 %zu 个未定义元素", ops::RemoveUndefinedElements(*p))); });
		break;
	case Sc::kTerminateUndefined:
		confirm("终止未定义线程", "终止所有运行着已不再安装的脚本的线程？",
			[this, p] { markEdited(Fmt("已终止 %zu 个未定义线程", ops::TerminateUndefinedThreads(*p))); });
		break;
	case Sc::kMissingMods:
		_missingDismissed = false;  // the banner comes back too
		for (auto& m : _missing) {
			m.planned = false;
		}
		_missingOpen = true;
		break;
	case Sc::kOrphans: openOrphans(); break;
	case Sc::kMcm: fixMcmJam(); break;
	case Sc::kHavok: resetHavok(); break;
	case Sc::kPurify: purifyFormLists(); break;
	case Sc::kBatch: _batchOpen = true; break;
	case Sc::kPurge:
		_purgeName[0] = 0;
		_purgeOpen = true;
		break;
	case Sc::kPlayer:
		_showPlayer = true;
		_pvLoaded = false;
		break;
	case Sc::kActors: _showActors = true; break;
	case Sc::kTransplant: _showTransplant = true; break;
	case Sc::kLookup: _showLookup = true; break;
	case Sc::kMods: select(Cat::kMods, 0); break;
	case Sc::kQuests: select(Cat::kQuests, 0); break;
	case Sc::kCompare:
		if (platform::HasNativeDialogs()) {
			if (auto f = platform::OpenSaveDialog(_path.parent_path())) {
				compareWith(*f);
			}
		} else if (const char* other = std::getenv("RESAVERPP_TEST_COMPARE")) {
			compareWith(other);
		}
		break;
	case Sc::kReport: _reportOpen = true; break;
	case Sc::kAnalysis: _showAnalysis = true; break;
	case Sc::kArrays:
		_showArrays = true;
		_arraysDirty = true;
		break;
	case Sc::kLeftovers:
		_showLeftovers = true;
		_leftoversDirty = true;
		break;
	case Sc::kHistory: _showHistory = true; break;
	case Sc::kPapyrusLog:
		_showPapyrusLog = true;
		_plogLoaded = false;
		break;
	case Sc::kSavesMgr:
		_showSavesMgr = true;
		_savesLoaded = false;
		break;
	case Sc::kOpenPex:
		if (platform::HasNativeDialogs()) {
			if (auto f = platform::OpenFileDialog(_sourcesPath.empty() ? std::filesystem::path() : platform::FromUtf8(_sourcesPath), "pex",
					"打开编译后的 Papyrus 脚本")) {
				openScript(*f);
			}
		} else if (const char* s = std::getenv("RESAVERPP_TEST_PEX")) {
			openScript(s);
		}
		break;
	}
}

void App::drawHome()
{
	struct Button
	{
		Sc          which;
		std::string label;
		const char* line;  // one short line under the button
		const char* tip;
		HomeRisk    risk;
		long long   count = -1;  // -1: no count; 0: nothing to do (greyed out)
	};
	const auto* r = _report.get();
	const auto  n = [&](std::size_t v) { return r ? static_cast<long long>(v) : -1LL; };
	const bool  anyMissing = !_missing.empty() || !_missingScripts.empty() || !_unfoundScripts.empty();

	const std::vector<std::pair<const char*, std::vector<Button>>> groups = {
		{ "清理",
			{
				{ Sc::kRemoveUndefined, "移除缺失脚本", "你移除的模组留下的脚本",
					"删除 .pex 文件已不再安装（模组被移除）的脚本及其实例与引用，并结束正在运行它们的线程。先备份存档。",
					HomeRisk::kCaution, r ? n(r->undefinedScripts + r->undefinedInstances) : -1 },
				{ Sc::kRemoveUnattached, "移除未附加实例", "没有附着到任何对象的脚本",
					"删除没有附着到任何对象的脚本实例（它们所属的对象已经没了）。它们只占空间，通常安全。",
					HomeRisk::kSafe, r ? n(r->unattached) : -1 },
				{ Sc::kTerminateUndefined, "停止缺失脚本的线程", "永远跑不完的正在运行的脚本",
					"结束那些 .pex 已不再安装、仍在运行的脚本。它们永远无法结束，还一直占用脚本引擎。", HomeRisk::kSafe,
					r ? n(r->undefinedThreads) : -1 },
				{ Sc::kOrphans, "缺失生成对象", "挂在已消失对象上的脚本",
					"找出挂在已不存在的生成对象（FF......）上的脚本实例，按模组归类并移除。",
					HomeRisk::kCaution, r ? n(r->createdWithoutForm) : -1 },
				{ Sc::kLeftovers, "残留物品", "模组已不在的物品",
					"玩家、NPC 和容器物品栏里那些模组已不再安装的物品：找出来并移除。它们会弄坏物品栏菜单并导致崩溃。",
					HomeRisk::kCaution, -1 },
				{ Sc::kMissingMods, "已不再安装的模组", "被移除的模组留下了什么",
					"存档里有、但加载顺序里没有的模组：看看它们留下了什么，确认后再清理。改过名的插件可以让存档改指向它的新名字。",
					HomeRisk::kCaution, anyMissing ? static_cast<long long>(_missing.size() + _missingScripts.size() + _unfoundScripts.size()) : 0 },
			} },
		{ "修复",
			{
				{ Sc::kMcm, r && r->mcmJam ? "SkyUI MCM 修复（卡死）" : "SkyUI MCM 修复", "一直不出现的模组菜单",
					"清除卡住的 SkyUI 模组菜单（MCM）注册，这是模组菜单一直不出现的常见原因。", HomeRisk::kCaution, -1 },
				{ Sc::kHavok, "重置 Havok", "乱飞或抖动的物体",
					"清除对象已保存的物理状态，让它们读档后重新静止。", HomeRisk::kSafe, -1 },
				{ Sc::kPurify, "净化 FormList", "已移除模组留下的空条目",
					"移除表单列表中指向已不存在东西的条目（已移除模组留下的）。", HomeRisk::kSafe, -1 },
				{ Sc::kPurge, "清除某个脚本", "彻底移除某一个脚本",
					"按名称移除某一个脚本的一切：正在运行的线程、排队调用和实例。用于刷爆存档的脚本。先备份存档。",
					HomeRisk::kRisky, -1 },
				{ Sc::kBatch, "批量清理", "一次完成多项清理",
					"一次运行多个清理步骤，并预览每一步会删掉什么。", HomeRisk::kCaution, -1 },
				{ Sc::kPapyrusLog, "Papyrus 日志", "哪个模组在刷错误",
					"读取 Papyrus.0.log：按脚本和模组归类错误，以及每个错误发生的频繁程度。", HomeRisk::kNone, -1 },
			} },
		{ "你的游戏",
			{
				{ Sc::kPlayer, "编辑玩家", "金币、等级、技能、属性、物品、赏金",
					"编辑你的角色：金币、等级、技能、专长点、属性、物品栏、法术和赏金。", HomeRisk::kCaution, -1 },
				{ Sc::kActors, "查找 NPC", "它们在哪；把走失的随从叫过来",
					"每个 NPC 和生物，以及存档里它们的位置。找出走失的随从，并把他们叫到你身边。", HomeRisk::kCaution, -1 },
				{ Sc::kTransplant, "移植进度", "从坏档搬进好档",
					"把你的进度（任务、属性、物品栏…）从一个坏档搬进一个更早的好档。", HomeRisk::kCaution, -1 },
				{ Sc::kSavesMgr, "管理存档文件夹", "批量删除旧存档",
					"按角色列出所有存档及其大小和日期；批量删除旧存档（连同它们的 .skse 文件），保留最新的。",
					HomeRisk::kCaution, -1 },
				{ Sc::kHistory, "存档历史", "哪些模组一直在变大",
					"这个角色每个存档的脚本实例、生成对象和大小，以及数字一直往上涨的模组。",
					HomeRisk::kNone, -1 },
				{ Sc::kLookup, "查找表单", "按名称或编辑器 ID",
					"在已安装的插件里按游戏内名称或编辑器 ID 搜索表单，并查看本存档是否改过它。", HomeRisk::kNone, -1 },
			} },
		{ "深入了解",
			{
				{ Sc::kMods, "模组视图", "每个模组及其脚本和线程",
					"存档里的每个模组及其脚本、实例和正在运行的线程，以及每个能怎么处理。", HomeRisk::kNone, -1 },
				{ Sc::kQuests, "任务", "进行中、已完成、日志",
					"存档里的每个任务及其阶段、日志条目和目标。", HomeRisk::kNone, -1 },
				{ Sc::kCompare, "与另一个存档比较", "两个存档之间变了什么",
					"把这个存档和另一个（例如更早的好档）比较，看哪些脚本、线程和变更表单变多了。",
					HomeRisk::kNone, -1 },
				{ Sc::kReport, "报告", "文本或 PDF，求助时分享",
					"把存档（模组、脚本、异常）写成文本或 PDF 报告。", HomeRisk::kNone, -1 },
				{ Sc::kAnalysis, "分析", "什么最占空间、跑得最多",
					"关于这个存档的文字报告：哪些模组和脚本最占空间、跑得最多。", HomeRisk::kNone, -1 },
				{ Sc::kOpenPex, "打开脚本（.pex）", "读取编译后脚本的代码",
					"打开一个编译后的 Papyrus 脚本（.pex），读取它反编译后的代码。", HomeRisk::kNone, -1 },
			} },
	};

	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("快捷入口：与“清理”和“工具”菜单相同。绿色安全，黄色：先读读它做什么，红色：有风险。"
	                   "所有改动都只做在已打开的存档里，只有用“文件 > 保存”或“另存为”才会写入存档文件。");
	ImGui::PopStyleColor();
	ImGui::Spacing();
	if (!ImGui::BeginTable("home", static_cast<int>(groups.size()), ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) {
		return;
	}
	for (const auto& [title, buttons] : groups) {
		ImGui::TableNextColumn();
		ImGui::SeparatorText(title);
		for (const auto& b : buttons) {
			ImGui::PushID(static_cast<int>(b.which));
			const bool nothing = b.count == 0;
			std::string label = b.label;
			if (b.count > 0) {
				label += Fmt("  (%lld)", b.count);
			}
			ImU32 col = 0;
			switch (b.risk) {
			case HomeRisk::kSafe: col = theme::Good(); break;
			case HomeRisk::kCaution: col = theme::Caution(); break;
			case HomeRisk::kRisky: col = theme::Bad(); break;
			default: break;
			}
			ImGui::BeginDisabled(nothing);
			// a coloured edge on the left says how risky it is
			const ImVec2 at = ImGui::GetCursorScreenPos();
			const float  h = ImGui::GetFrameHeight();
			if (col) {
				ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + 5, at.y + h), col, 2.0f);
			}
			ImGui::SetCursorScreenPos(ImVec2(at.x + 8, at.y));
			const bool click = ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0));
			ImGui::EndDisabled();
			if (nothing) {
				tips::Tip("没有可处理的项：这个存档里没有。");
			} else {
				tips::Tip(b.tip);
			}
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8);
			ImGui::TextDisabled("%s", b.line);
			ImGui::Spacing();
			if (click) {
				shortcut(b.which);
			}
			ImGui::PopID();
		}
	}
	ImGui::EndTable();
}
