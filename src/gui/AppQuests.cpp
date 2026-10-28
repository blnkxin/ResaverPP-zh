// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Quests page: every quest in the save with its stages, journal entries and objectives (read from the
// plugins and the save), what its completion stage's script does, and - with ReSaverHelper.dll in the
// running game - completing a quest, setting a stage and refreshing the journal in game.
#include "App.h"

#include "Theme.h"
#include "Tips.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>
#include <unordered_map>
#include <functional>

using namespace ess;

namespace
{
	std::string Fmt(const char* f, auto... args)
	{
		char buf[2048];
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

	void Dim(const std::string& s) { TextColored(theme::Dim(), s); }

	// The helper answers in English; those answers end up in our status bar, so the known ones get Chinese.
	std::string LiveErrorText(const std::string& a_error)
	{
		static const std::pair<const char*, const char*> kKnown[] = {
			{ "the game is not running with ReSaverHelper.dll", "游戏没在运行（或者没装 ReSaverHelper.dll）" },
			{ "the connection to the game was lost", "和游戏的连接断了" },
			{ "no reply", "游戏没有回应" },
			{ "no game loaded yet (load a save first)", "游戏里还没读档（请先读一个存档）" },
			{ "the game did not answer in time", "游戏没有及时回应" },
			{ "the game is busy (it did not answer within 30 seconds)", "游戏正忙（30 秒内没有回应）" },
			{ "the game is busy (it did not answer within 30 seconds); the refresh starts when it can",
			  "游戏正忙（30 秒内没有回应），能处理时会开始刷新" },
			{ "bad quest or stage", "任务或阶段参数不对" },
			{ "bad quest", "任务参数不对" },
			{ "unknown command ", "不认识的命令：" },
			{ "invalid option or value: ", "设置项或值不对：" },
			{ "unknown option ", "不认识的设置项：" },
			{ "could not write ", "写入失败：" },
		};
		for (const auto& [en, zh] : kKnown) {
			const std::string key = en;
			if (a_error == key) {
				return zh;
			}
			if (key.back() == ' ' && a_error.rfind(key, 0) == 0) {
				return std::string(zh) + a_error.substr(key.size());
			}
		}
		if (a_error.rfind("no quest ", 0) == 0) {
			return "游戏里没有这个任务（" + a_error.substr(9) + "）";
		}
		return a_error;
	}

	// "needs the running game" hint on a disabled button
	void LiveHint(const LiveLink::Info& a_info)
	{
		tips::Tipf("%s", !a_info.connected ? "安装 ReSaverHelper.dll 并用 SKSE 启动游戏后才能用（ReSaver Plus 页面上的可选下载）。"
													: "请先在游戏里读一个存档。");
	}
}

// ---- data ---------------------------------------------------------------------------------------

const std::vector<quests::QuestState>& App::questStates()
{
	if (_questGen != _editGen) {
		_questStates = _save ? quests::ReadStates(*_save) : std::vector<quests::QuestState>{};
		_questGen = _editGen;
		_questPreviews.clear();
		_chains.clear();
	}
	return _questStates;
}

const App::QuestPreview& App::questPreview(std::uint32_t a_formID)
{
	if (auto it = _questPreviews.find(a_formID); it != _questPreviews.end()) {
		return it->second;
	}
	// not ready yet: answer without keeping the answer, so it is read again once the plugins and scripts are in
	if (!_db) {
		_previewPending = true;
		_previewTmp = QuestPreview{};
		_previewTmp.error = namesLoading() ? "正在读取插件…" : "插件还没有读取（选项 > 设置：模组文件夹，之后会自动读取名称）。";
		return _previewTmp;
	}
	if (!ensureSourcesScanned()) {
		_previewPending = true;
		_previewTmp = QuestPreview{};
		_previewTmp.error = "正在后台读取你的模组脚本…";
		return _previewTmp;
	}
	auto& pv = _questPreviews[a_formID];
	pv.done = true;
	const auto* def = _db->quest(a_formID);
	if (!def) {
		pv.error = "这个任务不在你的插件里。";
		return pv;
	}
	if (def->fragmentScript.empty()) {
		pv.error = "这个任务没有阶段脚本。";
		return pv;
	}
	try {
		const auto px = _sources.findPex(def->fragmentScript);
		if (!px) {
			pv.error = "它的阶段脚本 " + def->fragmentScript + ".pex 没在你的模组里找到。";
			return pv;
		}
		const auto file = pex::Read(ScriptSources::readPex(*px));
		const auto code = pex::Decompile(file);
		for (auto st : def->completionStages()) {
			if (const auto* fr = def->fragmentFor(st)) {
				pv.bodies.push_back({ st, quests::FunctionBody(code, fr->function) });
			} else {
				pv.bodies.push_back({ st, {} });
			}
		}
	} catch (const std::exception& e) {
		pv.error = std::string("读不出它的阶段脚本：") + e.what();
	}
	return pv;
}

std::string App::liveNotifyMode() const
{
	switch (_liveNotify) {
	case 1: return "box";
	case 2: return "none";
	default: return "hud";
	}
}

void App::liveQuest(std::uint32_t a_formID)
{
	_live.request({ "QUEST", Fmt("%08X", a_formID) }, [this, a_formID](const LiveLink::Reply& r) {
		auto& lq = _liveQuests[a_formID];
		lq = LiveQuest{};
		if (!LiveLink::Ok(r)) {
			lq.error = LiveLink::Error(r);
			return;
		}
		lq.valid = true;
		lq.editorID = LiveLink::Field(r, 0);
		lq.stage = std::atoi(LiveLink::Field(r, 2).c_str());
		lq.running = LiveLink::Field(r, 3) == "1";
		lq.completed = LiveLink::Field(r, 4) == "1";
	});
}

void App::liveSetStage(std::uint32_t a_formID, std::uint16_t a_stage, const std::string& a_what, std::function<void(bool)> a_then)
{
	// The helper's own texts are English: asking it with "none" silences them, and liveNotify() below sends our
	// own text instead, so what the game shows is Chinese as well.
	_status = "正在请求游戏：" + a_what + "…";
	_live.request({ "SETSTAGE", Fmt("%08X", a_formID), std::to_string(a_stage), "none", "" },
		[this, a_formID, a_what, a_then](const LiveLink::Reply& r) {
			if (!LiveLink::Ok(r)) {
				_status = "游戏没能执行：" + LiveErrorText(LiveLink::Error(r));
				if (a_then) {
					a_then(false);
				}
				return;
			}
			const auto f = LiveLink::Field(r, 0);
			std::string why;
			if (f == "0") {
				// helper 3 also tells where the quest stands after the refusal
				const auto running = LiveLink::Field(r, 1), completed = LiveLink::Field(r, 2), stage = LiveLink::Field(r, 3);
				if (completed == "1") {
					why = " 这个任务在游戏里已经完成了。";
				} else if (running == "0") {
					why = " 任务没能启动：游戏找不到它需要的人、地点或物品（这些由任务正常开始时的剧情设置）。"
						  "可以用「标记为完成」在日志里把它标成完成。";
				} else if (!stage.empty()) {
					why = " 任务停在阶段 " + stage + "，那个阶段的条件没有满足。可以用「标记为完成」在日志里把它标成完成。";
				}
			}
			_status = f == "1" ? "游戏里已完成：" + a_what + "。记得在游戏里存档。" :
			          f == "?" ? "游戏还在运行那个阶段的脚本：" + a_what + "。" :
			                     "游戏拒绝了那个阶段：" + a_what + "。" + why;
			if (_chainRun.empty()) {
				if (f == "1") {
					liveNotify("完成：" + a_what);
				} else if (f == "?") {
					liveNotify("游戏还在执行：" + a_what);
				} else {
					liveNotify("游戏拒绝了：" + a_what);
				}
			}
			liveQuest(a_formID);
			if (a_then) {
				a_then(f != "0");
			}
		});
}

bool App::helperAtLeast(int a_version) const
{
	const auto info = _live.info();
	return info.connected && std::atoi(info.helperVersion.c_str()) >= a_version;
}

void App::liveComplete(std::uint32_t a_formID, const std::string& a_name)
{
	const std::string what = "「" + a_name + "」";
	_status = "正在请求游戏：把 " + what + " 标记为完成…";
	_live.request({ "COMPLETE", Fmt("%08X", a_formID), "none", "" }, [this, a_formID, what](const LiveLink::Reply& r) {
		if (!LiveLink::Ok(r)) {
			_status = "游戏没能执行：" + LiveErrorText(LiveLink::Error(r));
			return;
		}
		const auto f = LiveLink::Field(r, 0);
		_status = f == "1" ? "游戏里已把 " + what + " 标记为完成。记得在游戏里存档。" :
		          f == "?" ? "游戏正忙，稍后就会处理：" + what + "。" :
		                     "游戏没能把 " + what + " 标成完成。";
		if (f == "1") {
			liveNotify("已标记完成：" + what);
		}
		liveQuest(a_formID);
	});
}

// The game shows what we send here: the helper's own English messages are silenced with "none", and the mode
// (notification / message box / nothing) is still the user's choice.
void App::liveNotify(const std::string& a_text)
{
	if (_liveNotify == 2 || a_text.empty() || !_live.info().connected) {
		return;
	}
	_live.request({ "NOTIFY", liveNotifyMode(), "ReSaver++：" + a_text }, [](const LiveLink::Reply&) {});
}

std::vector<App::ChainItem>& App::questChain(std::uint32_t a_formID)
{
	if (auto it = _chains.find(a_formID); it != _chains.end()) {
		return it->second;
	}
	if (!_db) {
		static std::vector<ChainItem> none;
		none.clear();
		return none;  // not cached: the plugins are read later
	}
	std::vector<ChainItem> out;
	_previewPending = false;
	const auto& defs = _db->questDefs();
	std::unordered_map<std::uint32_t, const quests::QuestState*> state;
	for (const auto& q : questStates()) {
		state[q.formID] = &q;
	}
	auto nameOf = [&](std::uint32_t id) {
		const auto* d = _db->quest(id);
		return d ? (d->name.empty() ? d->edid : d->name + " (" + d->edid + ")") : Fmt("%08X", id);
	};
	auto predecessors = [&](std::uint32_t a_target) {
		return quests::LeadsInto(a_target, defs, [this](std::uint32_t id) { return questPreview(id).bodies; });
	};
	// walk back, oldest first; stop at quests already completed
	std::set<std::uint32_t> seen{ a_formID };
	std::function<void(std::uint32_t, int)> walk = [&](std::uint32_t a_target, int a_depth) {
		if (a_depth > 12) {
			return;
		}
		std::vector<std::uint32_t> pre;
		for (auto id : predecessors(a_target)) {
			if (!seen.contains(id)) {
				pre.push_back(id);  // quests already on the list (reached another way) do not count as alternatives
			}
		}
		for (auto id : pre) {
			if (!seen.insert(id).second) {
				continue;
			}
			auto       it = state.find(id);
			const bool done = it != state.end() && it->second->completed();
			if (!done) {
				walk(id, a_depth + 1);  // its own earlier quests come first
			}
			ChainItem c;
			c.formID = id;
			c.stage = _db->quest(id)->completionStages().front();
			c.name = nameOf(id);
			c.done = done;
			c.alternative = pre.size() > 1;
			c.tick = !done && !c.alternative;
			out.push_back(std::move(c));
		}
	};
	walk(a_formID, 0);
	if (_previewPending) {
		// some stage scripts could not be read yet: show what is known, look again next frame
		_chainPending = std::move(out);
		return _chainPending;
	}
	return _chains[a_formID] = std::move(out);
}

void App::runChain()
{
	if (_chainPos >= _chainRun.size()) {
		_status = Fmt("任务线在游戏里已完成（%zu 个任务）。记得在游戏里存档。", _chainRun.size());
		liveNotify(Fmt("任务线已完成：%zu 个任务", _chainRun.size()));
		_chainRun.clear();
		_chainPos = 0;
		return;
	}
	const auto c = _chainRun[_chainPos];
	liveSetStage(c.formID, c.stage, Fmt("完成「%s」（阶段 %u，第 %zu / %zu 个）", c.name.c_str(), c.stage, _chainPos + 1, _chainRun.size()),
		[this](bool ok) {
			if (!ok) {
				_status += "  任务线在这里停下了。";
				_chainRun.clear();
				_chainPos = 0;
				return;
			}
			++_chainPos;
			runChain();
		});
}

void App::drawQuestNotifyChoice()
{
	const char* modes[] = { "左上角通知", "消息框", "不提示" };
	ImGui::SetNextItemWidth(180);
	if (ImGui::Combo("##qnotify", &_liveNotify, modes, 3)) {
		saveSettings();
	}
	tips::Tip("游戏用什么方式告诉你结果（选项 > 设置里的「实时游戏」也能改）。");
}

// ---- live status (status bar) -------------------------------------------------------------------

void App::drawLiveStatus()
{
	_live.poll();
	const auto info = _live.info();
	if (!info.connected) {
		ImGui::TextDisabled("实时游戏：未连接");
		tips::Tip("安装 ReSaverHelper.dll 并用 SKSE 启动游戏后，就能在这里完成任务、设置阶段、刷新任务日志。");
		return;
	}
	TextColored(theme::Good(), Fmt("实时游戏：%s %s%s", info.runtime.c_str(), info.gameVersion.c_str(), info.inGame ? "" : "（主菜单）"));
}

// ---- journal refresh (live) -----------------------------------------------------------------------

void App::startLiveRefresh()
{
	_live.request({ "REFRESH", liveNotifyMode() }, [this](const LiveLink::Reply& r) {
		if (!LiveLink::Ok(r)) {
			_status = "游戏没能刷新任务日志：" + LiveLink::Error(r);
			return;
		}
		const auto n = std::strtoull(LiveLink::Field(r, 0).c_str(), nullptr, 10);
		if (n == 0) {
			_status = "没有可刷新的：没有进行中的任务显示目标。";
			return;
		}
		_liveRefresh = LiveRefresh{ true, 0, n, 0 };
	});
}

void App::drawRefreshJournal(bool a_compact)
{
	const auto info = _live.info();
	const bool can = info.connected && info.inGame;
	// progress of a refresh started from here (or by the helper on load)
	if (_liveRefresh.running && ImGui::GetTime() > _liveRefresh.nextPoll) {
		_liveRefresh.nextPoll = ImGui::GetTime() + 0.7;
		_live.request({ "PROGRESS" }, [this](const LiveLink::Reply& r) {
			if (!LiveLink::Ok(r)) {
				_liveRefresh.running = false;
				return;
			}
			_liveRefresh.done = std::strtoull(LiveLink::Field(r, 0).c_str(), nullptr, 10);
			_liveRefresh.total = std::strtoull(LiveLink::Field(r, 1).c_str(), nullptr, 10);
			if (LiveLink::Field(r, 2) != "1") {
				_liveRefresh.running = false;
				_status = Fmt("游戏里已刷新任务日志：%zu 个目标。记得在游戏里存档。", _liveRefresh.done);
				liveNotify(Fmt("任务日志已刷新：%zu 个目标", _liveRefresh.done));
			}
		});
	}
	ImGui::BeginDisabled(!can || _liveRefresh.running);
	if (ImGui::Button("在游戏中刷新任务日志")) {
		startLiveRefresh();
	}
	ImGui::EndDisabled();
	if (!can) {
		LiveHint(info);
	} else tips::Tip("对每个进行中的任务，把日志里显示的目标先隐藏再显示（已完成/已失败的复原），让游戏重建列表。\n"
					  "能把日志不再显示的任务和目标找回来（和 Quest Journal Limit Bug Fixer 是同一个修法）。\n\n"
					  "先在游戏里存个档，并在它做完之前不要打开菜单（helper 4 会等菜单关掉）。");
	if (_liveRefresh.running) {
		ImGui::SameLine();
		const float frac = _liveRefresh.total ? float(_liveRefresh.done) / float(_liveRefresh.total) : 0.0f;
		ImGui::ProgressBar(frac, ImVec2(a_compact ? 160.0f : 240.0f, 0), Fmt("%zu / %zu", _liveRefresh.done, _liveRefresh.total).c_str());
	}
	// the helper's own refresh messages are English: keep them silent, our own notifications carry the news
	if (info.connected) {
		const int want = _liveSilentRefresh ? 0 : 1;
		if (_liveIniNotifyApplied != want) {
			_liveIniNotifyApplied = want;
			_live.request({ "SETOPTION", "sNotify", want ? "hud" : "none" }, [](const LiveLink::Reply&) {});
		}
	} else {
		_liveIniNotifyApplied = -1;
	}
	// the helper's own option: refresh after every save load
	if (info.connected && !_liveOptionAsked) {
		_liveOptionAsked = true;
		_live.request({ "GETOPTION", "bRefreshJournalOnLoad" }, [this](const LiveLink::Reply& r) {
			_liveAutoRefresh = LiveLink::Ok(r) ? (LiveLink::Field(r, 0) == "1" ? 1 : 0) : -1;
		});
	}
	if (!info.connected) {
		_liveOptionAsked = false;
		_liveAutoRefresh = -1;
	}
	if (!a_compact) {
		ImGui::SameLine();
	}
	bool autoOn = _liveAutoRefresh == 1;
	ImGui::BeginDisabled(!info.connected || _liveAutoRefresh < 0);
	if (ImGui::Checkbox("每次读档后自动刷新任务日志", &autoOn)) {
		_live.request({ "SETOPTION", "bRefreshJournalOnLoad", autoOn ? "1" : "0" }, [this, autoOn](const LiveLink::Reply& r) {
			if (LiveLink::Ok(r)) {
				_liveAutoRefresh = autoOn ? 1 : 0;
				_status = autoOn ? "ReSaverHelper 会在每次读档后刷新任务日志（已写进 ReSaverHelper.ini）。"
				                 : "ReSaverHelper 不再在每次读档后刷新任务日志。";
			} else {
				_status = "改不了 ReSaverHelper 的设置：" + LiveErrorText(LiveLink::Error(r));
			}
		});
	}
	ImGui::EndDisabled();
	tips::Tipf("%s", info.connected ? "写在 ReSaverHelper.ini 的 bRefreshJournalOnLoad 里。读档后等几秒再刷新。\n"
											   "游戏里的提示由 ReSaver++ 发，可以在设置里关掉。"
											   : "装上 ReSaverHelper.dll 启动游戏才能改；也可以直接改 ReSaverHelper.ini 的 bRefreshJournalOnLoad=1。");
}

// ---- list -----------------------------------------------------------------------------------------

void App::drawQuestsList()
{
	const auto& states = questStates();
	std::size_t shown = 0, shownDone = 0;
	for (const auto& q : states) {
		const auto n = q.shownObjectives();
		shown += n;
		if (q.completed() || !q.running()) {
			shownDone += n;
		}
	}
	ImGui::TextUnformatted(Fmt("任务日志里显示了 %zu 个目标。", shown).c_str());
	if (shownDone) {
		ImGui::SameLine();
		TextColored(theme::Warn(), Fmt("其中 %zu 个属于已完成或已停止的任务。", shownDone));
	}
	drawRefreshJournal(true);
	if (!_db) {
		Dim(namesLoading() ? "正在读取插件，以取得任务名称、阶段和任务日志文本…"
						   : "任务名称、阶段和日志文本来自你的插件：请在 选项 > 设置 里设置模组文件夹。");
	}
	ImGui::Separator();

	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("显示");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1);
	const char* shows[] = { "在日志中", "进行中", "已完成", "本存档全部", "游戏中的全部任务" };
	ImGui::Combo("##qshow", &_questShow, shows, 5);
	tips::Tip("列出哪些任务：你的日志显示的、进行中的、已完成的、这个存档里的全部，或者你插件里的全部任务。");
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("查找");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(_questFilter[0] ? -60.0f : -1.0f);
	ImGui::InputTextWithHint("##qfilter", "任务名称里的任意词、编辑器 ID 或 Form ID", _questFilter, sizeof(_questFilter));
	tips::Tip("输入任务名称里的任意词、编辑器 ID 或 Form ID 就能找到它。");
	if (_questFilter[0]) {
		ImGui::SameLine();
		if (ImGui::Button("清空", ImVec2(-1, 0))) {
			_questFilter[0] = 0;
		}
		tips::Tip("清空查找内容。");
	}
	if (_questShow == 4 && !_db) {
		Dim("「游戏中的全部任务」来自你的插件：插件还没有读取。");
	}

	struct Row
	{
		std::uint32_t             formID = 0;
		const quests::QuestState* q = nullptr;  // null: never started in this save
		const quests::QuestDef*   def = nullptr;
		std::string               label, status;
		int                       stage = -1;
		std::size_t               shown = 0;
	};
	std::vector<Row> rows;
	const auto       needle = Lower(_questFilter);
	auto             add = [&](std::uint32_t a_formID, const quests::QuestState* q) {
        const auto* def = _db ? _db->quest(a_formID) : nullptr;
        if (q) {
            switch (_questShow) {
            case 0:
                if (q->shownObjectives() == 0) {
                    return;
                }
                break;
            case 1:
                if (!q->running() || q->completed()) {
                    return;
                }
                break;
            case 2:
                if (!q->completed()) {
                    return;
                }
                break;
            default:
                break;
            }
        }
        // name and editor ID from the plugins; until they are read, the plugin the quest comes from
        std::string label = def ? (def->name.empty() ? def->edid : def->name + "  (" + def->edid + ")") : "(" + _save->pluginOf(a_formID) + ")";
        if (!needle.empty()) {
            // every word typed must appear somewhere in the name, editor ID or form ID
            const auto hay = Lower(label) + " " + Lower(Fmt("%08X", a_formID));
            std::size_t pos = 0;
            while (pos < needle.size()) {
                const auto start = needle.find_first_not_of(' ', pos);
                if (start == std::string::npos) {
                    break;
                }
                const auto end = std::min(needle.find(' ', start), needle.size());
                if (hay.find(needle.substr(start, end - start)) == std::string::npos) {
                    return;
                }
                pos = end;
            }
        }
        Row r;
        r.formID = a_formID;
        r.q = q;
        r.def = def;
        r.label = std::move(label);
        r.status = q ? q->statusText() : "未开始（不在本存档中）";
        r.stage = q ? q->highestDoneStage() : -1;
        r.shown = q ? q->shownObjectives() : 0;
        rows.push_back(std::move(r));
	};
	std::unordered_map<std::uint32_t, const quests::QuestState*> byID;
	for (const auto& q : states) {
		byID[q.formID] = &q;
	}
	if (_questShow == 4 && _db) {
		// every QUST record of the load order, started or not
		for (const auto& [id, def] : _db->questDefs()) {
			auto it = byID.find(id);
			add(id, it == byID.end() ? nullptr : it->second);
		}
	} else {
		for (const auto& q : states) {
			add(q.formID, &q);
		}
	}

	if (ImGui::BeginTable("quests", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
										   ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Sortable)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		// the form ID is always 8 hex digits: a stretch weight squeezed it to 7 ("0003372...")
		ImGui::TableSetupColumn("任务", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 2.6f, 0);
		ImGui::TableSetupColumn("Form ID", ImGuiTableColumnFlags_WidthFixed, 110.0f, 1);
		ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthStretch, 1.4f, 2);
		ImGui::TableSetupColumn("阶段", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 84.0f, 3);
		ImGui::TableSetupColumn("显示", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 84.0f, 4);
		ImGui::TableHeadersRow();
		// click a column header to sort by it (again to reverse); ties keep the name order
		int  col = 0;
		bool asc = true;
		if (const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsCount > 0) {
			col = static_cast<int>(specs->Specs[0].ColumnUserID);
			asc = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
		}
		std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return Lower(a.label) < Lower(b.label); });
		std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
			auto cmp = [&]() -> int {
				switch (col) {
				case 1: return a.formID < b.formID ? -1 : a.formID > b.formID ? 1 : 0;
				case 2: return a.status < b.status ? -1 : a.status > b.status ? 1 : 0;
				case 3: return a.stage < b.stage ? -1 : a.stage > b.stage ? 1 : 0;
				case 4: return a.shown < b.shown ? -1 : a.shown > b.shown ? 1 : 0;
				default:
					{
						const auto la = Lower(a.label), lb = Lower(b.label);
						return la < lb ? -1 : la > lb ? 1 : 0;
					}
				}
			}();
			return asc ? cmp < 0 : cmp > 0;
		});
		ImGuiListClipper clip;
		clip.Begin(static_cast<int>(rows.size()));
		while (clip.Step()) {
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
				const auto& r = rows[i];
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::PushID(static_cast<int>(r.formID));
				if (ImGui::Selectable(r.label.c_str(), _questSel == r.formID, ImGuiSelectableFlags_SpanAllColumns)) {
					_questSel = r.formID;
					_setStageIdx = 0;
					if (_live.info().inGame) {
						liveQuest(_questSel);
					}
				}
				ImGui::PopID();
				ImGui::TableNextColumn();
				ImGui::TextDisabled("%08X", r.formID);
				ImGui::TableNextColumn();
				const bool odd = r.q && r.shown && (r.q->completed() || !r.q->running());
				if (odd) {
					TextColored(theme::Warn(), r.status);
				} else if (!r.q) {
					ImGui::TextDisabled("%s", r.status.c_str());
				} else {
					ImGui::TextUnformatted(r.status.c_str());
				}
				ImGui::TableNextColumn();
				if (r.stage >= 0) {
					ImGui::Text("%d", r.stage);
				}
				ImGui::TableNextColumn();
				if (r.shown) {
					ImGui::Text("%zu", r.shown);
				}
			}
		}
		ImGui::EndTable();
	}
}

// ---- detail ---------------------------------------------------------------------------------------

void App::drawQuestDetail()
{
	const auto& states = questStates();
	const quests::QuestState* q = nullptr;
	for (const auto& s : states) {
		if (s.formID == _questSel) {
			q = &s;
		}
	}
	quests::QuestState notStarted;  // a quest of the load order this save never touched
	if (!q && _db && _db->quest(_questSel)) {
		notStarted.formID = _questSel;
		q = &notStarted;
	}
	if (!q) {
		Dim("点击一个任务，查看它的阶段、日志条目和目标，以及完成它会带来什么。");
		return;
	}
	const auto* def = _db ? _db->quest(q->formID) : nullptr;
	const auto  info = _live.info();
	const bool  live = info.connected && info.inGame;

	ImGui::TextUnformatted(def && !def->name.empty() ? def->name.c_str() : (def ? def->edid.c_str() : Fmt("%08X", q->formID).c_str()));
	Dim(Fmt("%s  |  %08X  |  %s", def ? def->edid.c_str() : "?", q->formID, def ? _db->plugins()[def->plugin].c_str() : _save->pluginOf(q->formID).c_str()));
	ImGui::Text("本存档中：%s，已完成最高阶段 %d，日志里显示 %zu 个目标", q->statusText(), q->highestDoneStage(), q->shownObjectives());
	if (q->journalKnown) {
		std::string js;
		for (const auto& o : q->journal) {
			js += (js.empty() ? "" : "，") + Fmt("目标 %d %s", o.index, quests::ObjectiveStateText(o.state));
		}
		Dim(js.empty() ? std::string("日志里没有列出它。") : "日志里列出：" + js);
	}
	if (live) {
		auto it = _liveQuests.find(q->formID);
		if (it != _liveQuests.end() && it->second.valid) {
			TextColored(theme::Good(), Fmt("游戏里的状态：阶段 %d，%s", it->second.stage,
										   it->second.completed ? "已完成" : it->second.running ? "进行中" : "没有运行"));
		} else if (it != _liveQuests.end()) {
			Dim("游戏里的状态：" + LiveErrorText(it->second.error));
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("在游戏里查一下")) {
			liveQuest(q->formID);
		}
		tips::Tip("向正在运行的游戏询问这个任务当前的阶段和状态。");
	}

	// ---- actions
	const auto completion = def ? def->completionStages() : std::vector<std::uint16_t>{};
	const std::string name = def ? (def->name.empty() ? def->edid : def->name) : Fmt("%08X", q->formID);
	ImGui::Spacing();
	ImGui::BeginDisabled(!live || completion.empty());
	if (ImGui::Button("在游戏中完成该任务")) {
		const auto st = completion.front();
		const auto& pv = questPreview(q->formID);
		std::string effects;
		for (const auto& [s, body] : pv.bodies) {
			if (s != st) {
				continue;
			}
			for (const auto& e : quests::ScanEffects(body)) {
				effects += "\n    " + e.line;
			}
		}
		std::string earlier;
		for (const auto& c : questChain(q->formID)) {
			if (!c.done) {
				earlier += "\n    " + c.name;
			}
		}
		if (!earlier.empty()) {
			earlier = "\n\n这条任务线里前面的任务还没完成：" + earlier +
					  "\n这个按钮只完成当前任务。要连它们一起完成，用下面的「在游戏中完成前面的 N 个任务和这一个」。";
		}
		confirm("完成该任务",
			Fmt("把「%s」在游戏里设到它的完成阶段 %u？\n\n游戏会运行那个阶段自己的脚本，所以奖励会正常发放，后续任务也会正常开始。%s\n\n"
				"跳过任务前面的阶段可能留下没做完的事（东西没拿到、人没见过）。请先在游戏里存个档。%s",
				name.c_str(), st, effects.empty() ? "" : ("\n\n这个阶段的脚本会做：" + effects).c_str(),
				q->hasFlags ? "" :
				              "\n\n这个任务在本存档里还没开始：设阶段会立刻启动并完成它。但它的人和地点还没设置好，"
				              "脚本的部分内容可能不起作用，后续任务也可能仍然会启动。") + earlier,
			[this, fid = q->formID, st, name] { liveSetStage(fid, st, Fmt("完成「%s」（阶段 %u）", name.c_str(), st)); });
	}
	ImGui::EndDisabled();
	if (!live) {
		LiveHint(info);
	} else if (completion.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("这个任务没有任何阶段被标记为完成它。");
	} else if (live) {
		tips::Tip("在运行中的游戏里把任务设到它的完成阶段。该阶段自己的脚本会运行，所以奖励会正常发放，后续任务也会照常开始。先在游戏里存个档。");
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(!live || !helperAtLeast(3));
	if (ImGui::Button("标记为完成")) {
		confirm("标记为完成",
			Fmt("在游戏里把「%s」标成完成，但不运行它的阶段脚本？\n\n它的目标会被完成，并移动到日志的已完成列表。"
				"和「在游戏中完成该任务」不同：不会给奖励，也不会启动本来会跟着来的任务。\n"
				"游戏拒绝执行完成阶段时可以用这个。请先在游戏里存个档。",
				name.c_str()),
			[this, fid = q->formID, name] { liveComplete(fid, name); });
	}
	ImGui::EndDisabled();
	if (!live) {
		LiveHint(info);
	} else if (!helperAtLeast(3) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("需要 ReSaverHelper 3 或更新的版本（你的是 %s）。", info.helperVersion.c_str());
	} else if (live) {
		tips::Tip("在运行中的游戏里把任务标记为完成，但不运行它的阶段脚本：没有奖励，后续任务也不会开始。用于游戏拒绝完成阶段的时候。");
	}
	// the Chinese labels are wider than the English ones: the stage controls start a new line
	ImGui::NewLine();
	// set any stage
	std::vector<std::uint16_t> stageList;
	if (def) {
		for (const auto& s : def->stages) {
			stageList.push_back(s.index);
		}
	}
	_setStageIdx = std::clamp(_setStageIdx, 0, std::max(0, static_cast<int>(stageList.size()) - 1));
	ImGui::SetNextItemWidth(150);  // "阶段 1000" needs more room than "stage 1000"
	ImGui::BeginDisabled(stageList.empty());
	const bool tipOpen722 = ImGui::BeginCombo("##stage", stageList.empty() ? "阶段" : Fmt("阶段 %u", stageList[_setStageIdx]).c_str());
	tips::Tip("「在游戏中设置阶段」和「复制控制台命令」用的是这个阶段。阶段来自任务的插件。");
	if (tipOpen722) {
		for (int i = 0; i < static_cast<int>(stageList.size()); ++i) {
			if (ImGui::Selectable(Fmt("阶段 %u", stageList[i]).c_str(), i == _setStageIdx)) {
				_setStageIdx = i;
			}
		}
		ImGui::EndCombo();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(!live || stageList.empty());
	if (ImGui::Button("在游戏中设置阶段") && !stageList.empty()) {
		const auto st = stageList[_setStageIdx];
		confirm("设置阶段", Fmt("在游戏里把「%s」设到阶段 %u？该阶段的脚本会运行。请先在游戏里存个档。", name.c_str(), st),
			[this, fid = q->formID, st, name] { liveSetStage(fid, st, Fmt("把「%s」设到阶段 %u", name.c_str(), st)); });
	}
	ImGui::EndDisabled();
	if (!live) {
		LiveHint(info);
	} else {
		tips::Tip("在运行中的游戏里把任务设到左侧选中的阶段。该阶段的脚本会运行。跳过阶段可能"
				  "让一些事情永远做不完；先在游戏里存个档。");
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(!def || def->edid.empty() || (completion.empty() && stageList.empty()));
	if (ImGui::Button("复制控制台命令") && def) {
		const auto st = completion.empty() ? stageList[_setStageIdx] : completion.front();
		ImGui::SetClipboardText(Fmt("setstage %s %u", def->edid.c_str(), st).c_str());
		_status = Fmt("已复制：setstage %s %u （在游戏控制台里按 Ctrl+V 粘贴）", def->edid.c_str(), st);
	}
	ImGui::EndDisabled();
	tips::Tip("复制能完成该任务的控制台命令（用它的完成阶段），或设置左侧选中的阶段。");
	ImGui::TextDisabled("游戏如何告知结果：");
	ImGui::SameLine();
	drawQuestNotifyChoice();

	if (!def) {
		ImGui::Spacing();
		Dim("设置好模组文件夹后，阶段、日志条目和目标就从你的插件里读取。");
		return;
	}

	// ---- earlier quests of the quest line
	if (!completion.empty()) {
		auto&       chain = questChain(q->formID);
		std::size_t open = 0;
		for (const auto& c : chain) {
			open += c.done ? 0 : 1;
		}
		if (open) {
			ImGui::SeparatorText(Fmt("这条任务线里前面的任务（%zu 个未完成）", open).c_str());
			Dim("这些任务通向当前任务：它们的日志把当前任务列为下一个，或它们的最后一个阶段会启动当前任务。"
				"勾选的会按从早到晚的顺序先完成，然后是当前任务。如果到达当前任务有几条路（两个阵营、两个结局），"
				"那几条不会自动勾选——请自己勾想要的那条。");
			for (auto& c : chain) {
				if (c.done) {
					continue;
				}
				ImGui::PushID(static_cast<int>(c.formID));
				ImGui::Checkbox(c.name.c_str(), &c.tick);
				tips::Tip("勾选的前置任务会先被完成（最早的优先），然后才是这个任务。");
				ImGui::SameLine();
				ImGui::TextDisabled("阶段 %u%s", c.stage, c.alternative ? "   （此处有多种达成方式之一）" : "");
				ImGui::PopID();
			}
			std::vector<ChainItem> run;
			for (const auto& c : chain) {
				if (c.tick && !c.done) {
					run.push_back(c);
				}
			}
			ChainItem self;
			self.formID = q->formID;
			self.stage = completion.front();
			self.name = name;
			run.push_back(self);
			ImGui::BeginDisabled(!live || run.size() < 2 || !_chainRun.empty());
			if (ImGui::Button(Fmt("在游戏中完成前面的 %zu 个任务和这一个", run.size() - 1).c_str())) {
				std::string list;
				for (const auto& c : run) {
					list += Fmt("\n    %s (stage %u)", c.name.c_str(), c.stage);
				}
				confirm("完成整条任务线",
					"在游戏里按下面的顺序，一个接一个完成这些任务？" + list +
						"\n\n每个阶段的脚本都会运行（奖励、后续任务）。如果游戏拒绝了某个阶段，就在那里停下。"
						"跳过任务可能留下没做完的事。请先在游戏里存个档。",
					[this, run] {
						_chainRun = run;
						_chainPos = 0;
						runChain();
					});
			}
			tips::Tip("把这条任务线上勾选的、更早的任务一个个完成，然后完成当前这个，每个都走它自己的完成阶段。游戏拒绝某个阶段时就停住。");
			ImGui::EndDisabled();
			if (!live) {
				LiveHint(info);
			}
			if (!_chainRun.empty()) {
				ImGui::SameLine();
				ImGui::TextDisabled("正在执行：%zu / %zu", _chainPos + 1, _chainRun.size());
			}
		} else if (_previewPending) {
			Dim("正在查找这条任务线里更早的任务（你的模组脚本还在读取）…");
		}
	}

	// ---- what completing it does
	if (!completion.empty()) {
		ImGui::SeparatorText("完成它会发生什么");
		const auto& pv = questPreview(q->formID);
		if (!pv.error.empty()) {
			Dim(pv.error);
		}
		for (const auto& [st, body] : pv.bodies) {
			ImGui::Text("阶段 %u 完成该任务。", st);
			const auto effects = quests::ScanEffects(body);
			if (body.empty()) {
				Dim("  这个阶段没有脚本。");
			} else if (effects.empty()) {
				Dim("  它的脚本不直接给出任何东西（奖励如有，来自其他脚本或对话）。");
			}
			for (const auto& e : effects) {
				const auto col = e.kind == quests::Effect::Kind::kReward ? theme::Good() : e.kind == quests::Effect::Kind::kQuest ? theme::Warn() : theme::Dim();
				TextColored(col, (e.kind == quests::Effect::Kind::kReward ? "  给予 / 改变：  " : e.kind == quests::Effect::Kind::kQuest ? "  任务：  " : "  调用：  ") + e.line);
			}
			if (!body.empty() && ImGui::TreeNode(Fmt("##body%u", st).c_str(), "它的完整脚本（%zu 行）", body.size())) {
				if (monoFont) {
					ImGui::PushFont(monoFont);
				}
				for (const auto& l : body) {
					ImGui::TextUnformatted(l.c_str());
				}
				if (monoFont) {
					ImGui::PopFont();
				}
				ImGui::TreePop();
			}
		}
		// the properties the script uses, so names like JarlRewardLightArmor can be looked up
		if (!def->properties.empty() && ImGui::TreeNode("props", "它的阶段脚本的属性（%zu）", def->properties.size())) {
			for (const auto& p : def->properties) {
				std::string v = p.type == 1 ? formText(p.form) : p.text;
				if (p.type == 1 && p.alias >= 0) {
					v = Fmt("本任务的别名 %d", p.alias);
				}
				ImGui::BulletText("%s = %s", p.name.c_str(), v.c_str());
			}
			ImGui::TreePop();
		}
	}

	// ---- stages
	ImGui::SeparatorText(Fmt("阶段（%zu）", def->stages.size()).c_str());
	if (ImGui::BeginTable("stages", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("阶段", ImGuiTableColumnFlags_WidthStretch, 0.7f);
		ImGui::TableSetupColumn("完成", ImGuiTableColumnFlags_WidthFixed, 80.0f);  // "已完成" wrapped to 3 lines at 0.5
		ImGui::TableSetupColumn("日志条目", ImGuiTableColumnFlags_WidthStretch, 6.0f);
		ImGui::TableHeadersRow();
		for (const auto& s : def->stages) {
			bool done = false;
			for (const auto& [st, d] : q->stages) {
				done |= st == static_cast<std::int16_t>(s.index) && d;
			}
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("%u", s.index);
			ImGui::TableNextColumn();
			if (done) {
				TextColored(theme::Good(), "已完成");
			}
			ImGui::TableNextColumn();
			std::string text;
			for (const auto& l : s.logs) {
				if (!l.text.empty()) {
					text += (text.empty() ? "" : "\n") + l.text;
				}
			}
			if (s.completes()) {
				TextColored(theme::Good(), "［完成任务］");
			}
			if (s.fails()) {
				TextColored(theme::Bad(), "［使任务失败］");
			}
			if (!text.empty()) {
				ImGui::TextWrapped("%s", text.c_str());
			}
		}
		ImGui::EndTable();
	}

	// ---- objectives
	ImGui::SeparatorText(Fmt("目标（%zu）", def->objectives.size()).c_str());
	if (ImGui::BeginTable("objs", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthStretch, 0.5f);
		ImGui::TableSetupColumn("本存档中", ImGuiTableColumnFlags_WidthStretch, 1.4f);
		ImGui::TableSetupColumn("目标", ImGuiTableColumnFlags_WidthStretch, 5.0f);
		ImGui::TableHeadersRow();
		for (const auto& o : def->objectives) {
			int state = 0;
			for (const auto& os : q->objectives) {
				if (os.index == o.index) {
					state = os.state;
				}
			}
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("%u", o.index);
			ImGui::TableNextColumn();
			const bool shown = state == 1 || state == 3 || state == 5;
			if (shown && (q->completed() || !q->running())) {
				TextColored(theme::Warn(), quests::ObjectiveStateText(state));
			} else {
				ImGui::TextUnformatted(quests::ObjectiveStateText(state));
			}
			ImGui::TableNextColumn();
			ImGui::TextWrapped("%s", o.text.c_str());
		}
		ImGui::EndTable();
	}
}
