// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Transplant page: bring a character's progress from a bad save into a good one. The bad save is
// read through the game (ReSaverHelper's snapshot, complete) or from its file (partial); the good one
// is the save loaded in the game now. What the bad save has and the good one does not is listed,
// coloured by risk; the ticked things are put into the running game one by one by ReSaverHelper, and
// the player saves in the game.
#include "App.h"

#include "Platform.h"
#include "Theme.h"

#include "imgui.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <cctype>
#include <cmath>
#include <cstdio>

using namespace ess;
namespace tp = ess::transplant;

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
		ImGui::TextUnformatted(s.c_str());
		ImGui::PopStyleColor();
	}

	void Wrapped(std::uint32_t col, const std::string& s)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, col);
		ImGui::TextWrapped("%s", s.c_str());
		ImGui::PopStyleColor();
	}

	std::uint32_t RiskCol(int r) { return r == 0 ? theme::Good() : r == 1 ? theme::Caution() : theme::Bad(); }

	void Dot(std::uint32_t col)
	{
		const float sz = ImGui::GetTextLineHeight();
		const auto  p = ImGui::GetCursorScreenPos();
		ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + sz * 0.5f, p.y + sz * 0.55f), sz * 0.33f, col);
		ImGui::Dummy(ImVec2(sz, sz));
		ImGui::SameLine();
	}

	std::string Summary(const tp::Snapshot& s)
	{
		if (!s.valid) {
			return s.error.empty() ? "尚未选择" : s.error;
		}
		std::size_t items = 0, done = 0;
		for (const auto& i : s.items) {
			items += static_cast<std::size_t>(std::max(0, i.count));
		}
		for (const auto& [k, q] : s.quests) {
			done += q.completed ? 1 : 0;
		}
		return Fmt("%s%s%s：%zu 个任务已完成，%zu 件物品%s", s.player.empty() ? "" : (s.player + " ").c_str(), s.level > 0 ? Fmt("等级 %d", s.level).c_str() : "",
			s.fromFile ? "（从存档文件读出）" : "（来自游戏）", done, items,
			s.fromFile ? "" : Fmt("，%zu 个专长、%zu 个法术", s.perks.size(), s.spells.size()).c_str());
	}

	// a moving dashed frame around the last item: "do this next"
	void Marquee()
	{
		const float pad = 4.0f;
		const auto  mn = ImVec2(ImGui::GetItemRectMin().x - pad, ImGui::GetItemRectMin().y - pad);
		const auto  mx = ImVec2(ImGui::GetItemRectMax().x + pad, ImGui::GetItemRectMax().y + pad);
		const float w = mx.x - mn.x, h = mx.y - mn.y, per = 2 * (w + h);
		auto        at = [&](float d) {
            d = std::fmod(d + per, per);
            if (d < w) {
                return ImVec2(mn.x + d, mn.y);
            }
            d -= w;
            if (d < h) {
                return ImVec2(mx.x, mn.y + d);
            }
            d -= h;
            if (d < w) {
                return ImVec2(mx.x - d, mx.y);
            }
            return ImVec2(mn.x, mx.y - (d - w));
		};
		const float t = static_cast<float>(ImGui::GetTime());
		const auto  alpha = static_cast<int>(170 + 85 * std::sin(t * 5.0f));
		const auto  col = IM_COL32(255, 196, 40, alpha);
		auto*       dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(mn, mx, IM_COL32(255, 196, 40, 22 + static_cast<int>(18 * std::sin(t * 5.0f))), 4.0f);
		const float dash = 12.0f, gap = 7.0f, offset = std::fmod(t * 60.0f, dash + gap);
		for (float d = offset - (dash + gap); d < per; d += dash + gap) {
			ImVec2 pts[7];
			for (int k = 0; k < 7; ++k) {
				pts[k] = at(d + dash * static_cast<float>(k) / 6.0f);
			}
			dl->AddPolyline(pts, 7, col, 0, 2.5f);
		}
	}

	// what a snapshot holds, group by group (shown under step 1 while there is no good save yet)
	void SnapshotContents(const tp::Snapshot& s)
	{
		auto list = [](const char* a_title, const std::vector<std::string>& a_names) {
			if (a_names.empty()) {
				return;
			}
			if (ImGui::TreeNodeEx(a_title, ImGuiTreeNodeFlags_SpanAvailWidth, "%s (%zu)", a_title, a_names.size())) {
				std::string line;
				for (const auto& n : a_names) {
					line += (line.empty() ? "" : ",   ") + n;
				}
				ImGui::TextWrapped("%s", line.c_str());
				ImGui::TreePop();
			}
		};
		auto names = [](const std::map<tp::FormKey, tp::Entry>& a_m) {
			std::vector<std::string> v;
			for (const auto& [k, e] : a_m) {
				v.push_back(e.name.empty() ? Fmt("%06X", k.local) : e.name);
			}
			std::sort(v.begin(), v.end());
			return v;
		};
		if (s.level > 0) {
			ImGui::Text("等级 %d%s%s", s.level, s.perkPoints > 0 ? Fmt("，%d 点未用的专长点", s.perkPoints).c_str() : "",
				s.look.empty() ? "" : "，外观已保存（RaceMenu）");
		}
		std::vector<std::string> skills;
		for (int i = 0; i < 18; ++i) {
			if (s.skills[i].level >= 0) {
				skills.push_back(Fmt("%s %.0f", tp::SkillName(i), s.skills[i].level));
			}
		}
		list("技能", skills);
		list("专长", names(s.perks));
		list("法术与能力", names(s.spells));
		list("龙吼", names(s.shouts));
		list("力量之语", names(s.words));
		list("地图地点", names(s.markers));
		list("派系", names(s.factions));
		std::vector<std::string> active, completed;
		for (const auto& [k, q] : s.quests) {
			const bool journal = s.journalKnown ? q.inJournal : q.known ? q.shown() : q.completed;
			if (!journal) {
				continue;
			}
			const auto n = !q.name.empty() ? q.name : !q.edid.empty() ? q.edid : Fmt("%06X", q.local);
			(q.completed ? completed : active).push_back(n);
		}
		std::sort(active.begin(), active.end());
		std::sort(completed.begin(), completed.end());
		list("进行中的任务（日志）", active);
		list("已完成的任务（日志）", completed);
		std::map<std::string, std::vector<std::string>> kinds;
		for (const auto& it : s.items) {
			const auto kind = it.record.size() > 12 && !it.record[12].empty() ? it.record[12] : std::string("物品");
			kinds[kind].push_back(it.count > 1 ? Fmt("%s x%d", it.name.c_str(), it.count) : it.name);
		}
		for (auto& [k, v] : kinds) {
			std::sort(v.begin(), v.end());
			list(("物品栏：" + k).c_str(), v);
		}
		std::vector<std::string> made, worn;
		for (const auto& p : s.potions) {
			made.push_back(Fmt("%s x%d", p.name.c_str(), p.count));
		}
		for (const auto& e : s.equips) {
			worn.push_back(e.name);
		}
		list("你制作的药水与毒药", made);
		list("穿戴与手持", worn);
		std::vector<std::string> followers;
		for (const auto& f : s.followers) {
			followers.push_back(f.name);
		}
		list("随从", followers);
	}
}

// ---- getting the two snapshots --------------------------------------------------------------------

void App::transplantSnapshot(bool a_bad)
{
	_status = "正在向游戏索取已读档的快照：请切到天际一会儿（它在后台会暂停）……";
	_tpWaiting = true;
	_tpWaitStart = ImGui::GetTime();
	_live.request({ "SNAPSHOT" }, [this, a_bad](const LiveLink::Reply& r) {
		_tpWaiting = false;
		if (!LiveLink::Ok(r)) {
			_status = "游戏没能生成快照：" + LiveLink::Error(r);
			return;
		}
		auto s = tp::ReadSnapshot(platform::FromUtf8(LiveLink::Field(r, 0)));
		if (!s.valid) {
			_status = s.error;
			return;
		}
		_status = Fmt("已生成 %s 的快照（%s 条记录）。", s.player.c_str(), LiveLink::Field(r, 1).c_str());
		(a_bad ? _tpBad : _tpGood) = std::move(s);
		_tpDirty = true;
	});
}

void App::transplantFromFile(const std::filesystem::path& a_path)
{
	try {
		if (Lower(platform::ToUtf8(a_path.extension())) == ".txt") {
			_tpBad = tp::ReadSnapshot(a_path);
		} else {
			auto save = SaveFile::load(a_path);
			_tpBad = tp::FromSave(*save, platform::ToUtf8(a_path.filename()));
			transplantNames(_tpBad, *save);
		}
		_tpDirty = true;
		_status = _tpBad.valid ? "已读取 " + platform::ToUtf8(a_path.filename()) : _tpBad.error;
	} catch (const std::exception& e) {
		_error = "无法读取 " + platform::ToUtf8(a_path.filename()) + ":\n\n" + e.what();
	}
}

// names for a snapshot read from a save file (the file has form IDs only)
void App::transplantNames(tp::Snapshot& a_snap, const SaveFile& a_save)
{
	if (!_db || !_save || _save->plugins.full != a_save.plugins.full) {
		return;  // the plugin names read belong to another load order
	}
	auto nameOf = [&](const tp::Record& r) {
		const auto id = static_cast<std::uint32_t>(std::strtoul((r.size() > 3 ? r[3] : std::string()).c_str(), nullptr, 16));
		const auto* f = _db->find(id);
		if (!f) {
			return std::string();
		}
		auto n = _db->name(*f);
		return n.empty() ? _db->edid(*f) : n;
	};
	for (auto* m : { &a_snap.perks, &a_snap.spells, &a_snap.shouts, &a_snap.words, &a_snap.factions, &a_snap.markers }) {
		for (auto& [k, e] : *m) {
			if (e.name.empty()) {
				e.name = nameOf(e.record);
				if (e.record.size() > 4) {
					e.record[4] = e.name;
				}
			}
		}
	}
	for (auto& it : a_snap.items) {
		if (it.name.empty()) {
			it.name = nameOf(it.record);
			if (it.record.size() > 4) {
				it.record[4] = it.name;
			}
		}
	}
	for (auto& [k, q] : a_snap.quests) {
		if (const auto* d = _db->quest(q.runtime)) {
			q.edid = d->edid;
			q.name = d->name;
		}
	}
}

// ---- applying -------------------------------------------------------------------------------------

std::int32_t App::completionStageOf(const tp::Quest& a_q) const
{
	// the quest's completion stage, from the plugins (the open save's load order must hold the quest)
	if (!_db || !_save) {
		return -1;
	}
	for (const auto& [id, def] : _db->questDefs()) {
		const auto local = (id >> 24) == 0xFE ? (id & 0xFFF) : (id & 0xFFFFFF);
		if (local == a_q.local && Lower(_save->pluginOf(id)) == Lower(a_q.plugin)) {
			const auto st = def.completionStages();
			return st.empty() ? -1 : st.front();
		}
	}
	return -1;
}

void App::transplantStep()
{
	if (_tpPos >= _tpRun.size()) {
		std::size_t failed = 0;
		for (const auto& r : _tpResults) {
			failed += r.ok ? 0 : 1;
		}
		_status = Fmt("移植完成：%zu / %zu 已带过去%s。任务可能还要继续加载一两分钟：等它们的"
					  "提示停下后再在游戏里存档。",
			_tpRun.size() - failed, _tpRun.size(), failed ? Fmt("，%zu 个没能带过去（见“移植”页）", failed).c_str() : "");
		bool quests = false;
		for (auto i : _tpRun) {
			quests = quests || (i < _tpChanges.size() && _tpChanges[i].quest);
		}
		if (quests && helperAtLeast(9)) {
			// a quest's stage script can leave the controls off (the player cannot move until a reload)
			_live.request({ "APPLY", "controls" }, [](const LiveLink::Reply&) {});
		}
		_live.request({ "NOTIFY", liveNotifyMode(),
						  Fmt("ReSaver++：移植完成（%zu / %zu）。任务可能还在加载：等提示停下后再存档。",
							  _tpRun.size() - failed, _tpRun.size()) },
			[](const LiveLink::Reply&) {});
		_tpRunning = false;
		if (quests && helperAtLeast(9)) {
			transplantCheckJournal();
		}
		return;
	}
	const auto idx = _tpRun[_tpPos];
	if (idx >= _tpChanges.size()) {
		++_tpPos;
		transplantStep();
		return;
	}
	const auto& ch = _tpChanges[idx];
	_status = Fmt("正在移植 %zu / %zu：%s", _tpPos + 1, _tpRun.size(), ch.label.c_str());
	auto done = [this, idx](bool a_ok, const std::string& a_why) {
		_tpResults.push_back({ idx, a_ok, a_why });
		++_tpPos;
		transplantStep();
	};

	if (!ch.quest) {
		std::vector<std::string> cmd{ "APPLY" };
		cmd.insert(cmd.end(), ch.record.begin(), ch.record.end());
		_live.request(cmd, [done](const LiveLink::Reply& r) {
			if (!LiveLink::Ok(r)) {
				done(false, LiveLink::Error(r));
			} else if (LiveLink::Field(r, 0) == "0") {
				done(false, LiveLink::Field(r, 1));
			} else {
				done(true, LiveLink::Field(r, 0) == "?" ? "游戏还在处理" : "");
			}
		});
		return;
	}

	// a quest: find it in the game, then its completion stage (or the stage it had), else mark it completed.
	// Helper 9: the objectives are shown first (the quest started if needed), so a quest that ends at its
	// stage is in the journal before it stops; then again afterwards, for the states its script changed.
	const auto* q = ch.quest;
	const bool  complete = ch.complete;
	const bool  journalOnly = ch.journalOnly;
	const bool  markRefused = _tpMarkRefused && complete;  // only quests completed in the bad save
	const bool  before = !q->objectives.empty() && helperAtLeast(9);
	// after the stage / mark: show the quest's objectives as the bad save had them, so the journal lists it
	auto        shown = [this, q, done](bool a_ok, const std::string& a_why) {
        if (q->objectives.empty() || !helperAtLeast(8)) {
            done(a_ok, a_why);
            return;
        }
        _live.request({ "APPLY", "questobj", q->plugin, Fmt("%06X", q->local), "", q->objectives }, [done, a_ok, a_why](const LiveLink::Reply& r) {
            const bool ok = LiveLink::Ok(r) && LiveLink::Field(r, 0) != "0";
            done(a_ok || ok, a_why + (a_why.empty() ? "" : ", ") + (ok ? "目标已在日志里显示" : "目标未显示：" + LiveLink::Field(r, 1)));
        });
	};
	auto        run = [this, q, complete, markRefused, journalOnly, before, shown, done](std::uint32_t a_id) {
        const int stage = complete ? completionStageOf(*q) : q->stage;
        // marks the quest completed: no stage script, no rewards, follow-up quests not started
        auto      mark = [this, a_id, shown](const std::string& a_before) {
            _live.request({ "COMPLETE", Fmt("%08X", a_id), "none", "" }, [shown, a_before](const LiveLink::Reply& r) {
                const bool ok = LiveLink::Ok(r) && LiveLink::Field(r, 0) != "0";
                shown(ok, ok ? a_before + "，已标记为完成" : a_before + "，而且也没能标记为完成");
            });
        };
        auto      stageStep = [this, a_id, stage, complete, markRefused, journalOnly, mark, shown, done]() {
            if (journalOnly) {
                shown(true, "");
                return;
            }
            if (stage < 0) {
                if (complete || markRefused) {
                    mark("完成阶段未知（未读取插件）");
                } else {
                    shown(false, "没有可设置的阶段");
                }
                return;
            }
            _live.request({ "SETSTAGE", Fmt("%08X", a_id), std::to_string(stage), "none", "" }, [markRefused, mark, shown, done, stage](const LiveLink::Reply& r) {
                if (!LiveLink::Ok(r)) {
                    done(false, LiveLink::Error(r));
                    return;
                }
                const auto f = LiveLink::Field(r, 0);
                if (f == "0") {
                    if (markRefused) {
                        mark(Fmt("游戏拒绝了阶段 %d", stage));
                    } else {
                        shown(false, Fmt("游戏拒绝了阶段 %d", stage));
                    }
                    return;
                }
                shown(true, f == "?" ? Fmt("阶段 %d 已设置；游戏还在运行它的脚本", stage) : Fmt("阶段 %d 已设置", stage));
            });
        };
        if (!before) {
            stageStep();
            return;
        }
        _live.request({ "APPLY", "questobj", q->plugin, Fmt("%06X", q->local), "start", q->objectives }, [stageStep](const LiveLink::Reply&) { stageStep(); });
	};
	if (ch.goodRuntime) {
		run(ch.goodRuntime);
	} else {
		_live.request({ "RESOLVE", q->plugin, Fmt("%06X", q->local) }, [run, done](const LiveLink::Reply& r) {
			if (!LiveLink::Ok(r)) {
				done(false, LiveLink::Error(r));
				return;
			}
			run(static_cast<std::uint32_t>(std::strtoul(LiveLink::Field(r, 0).c_str(), nullptr, 16)));
		});
	}
}

// after a run: a new snapshot of the game, and the quests the bad save's journal has that the game's
// journal does not, each with why (from the run's results)
void App::transplantCheckJournal()
{
	_tpChecking = true;
	_live.request({ "SNAPSHOT" }, [this](const LiveLink::Reply& r) {
		_tpChecking = false;
		if (!LiveLink::Ok(r)) {
			_status = "无法核对任务日志：" + LiveLink::Error(r);
			return;
		}
		transplantMissing(tp::ReadSnapshot(platform::FromUtf8(LiveLink::Field(r, 0))));
	});
}

void App::transplantMissing(const ess::transplant::Snapshot& now)
{
		if (!now.valid || !now.journalKnown) {
			_status = "无法核对任务日志（快照里没有日志列表：需要 ReSaverHelper 9）。";
			return;
		}
		// what the run did with each quest
		std::map<const tp::Quest*, std::string> why;
		for (std::size_t i = 0; i < _tpChanges.size(); ++i) {
			if (const auto* q = _tpChanges[i].quest) {
				why[q] = !_tpChanges[i].available ? "它的模组不在这个游戏里" : !_tpChanges[i].tick ? "没有勾选它" : "";
			}
		}
		for (const auto& res : _tpResults) {
			if (res.index < _tpChanges.size() && _tpChanges[res.index].quest && !res.ok) {
				why[_tpChanges[res.index].quest] = res.why;
			}
		}
		_tpMissing.clear();
		for (const auto& [k, q] : _tpBad.quests) {
			const bool inBad = _tpBad.journalKnown ? q.inJournal : q.known ? q.shown() : q.completed;
			if (!inBad) {
				continue;
			}
			const auto n = now.quests.find(k);
			if (n != now.quests.end() && n->second.inJournal) {
				continue;
			}
			TpMissing m;
			m.name = !q.name.empty() ? q.name : !q.edid.empty() ? q.edid : Fmt("%06X", q.local);
			m.edid = q.edid;
			m.quest = &q;
			m.completed = q.completed;
			auto w = why.find(&q);
			m.why = w == why.end() ? "它不在列表里（没什么可带的：好档已经进行到这里）"
					: !w->second.empty() ? w->second
					: q.completed ? "游戏在这里不肯完成它"
								  : "游戏在这里不肯启动它：它是由游戏里的某样东西启动的（一张纸条、一个人、一个地点）";
			_tpMissing.push_back(std::move(m));
		}
		std::sort(_tpMissing.begin(), _tpMissing.end(), [](const TpMissing& a, const TpMissing& b) { return a.name < b.name; });
		_tpChecked = true;
		_status = _tpMissing.empty() ? "坏档日志里的每个任务现在都在游戏的日志里了。"
									 : Fmt("坏档日志里有 %zu 个任务不在游戏的日志里：见列表。", _tpMissing.size());
}

// quests in quest-line order: a quest that leads into another one (its journal names the next quest, or
// its last stage starts it) is done first. Needs the plugins read for the open save's load order.
void App::transplantOrderQuests(std::vector<std::size_t>& a_run)
{
	if (!_db || !_save) {
		return;
	}
	// the open save's form ID of each quest in the run
	std::map<std::pair<std::string, std::uint32_t>, std::uint32_t> idOf;
	for (const auto& [id, def] : _db->questDefs()) {
		const auto local = (id >> 24) == 0xFE ? (id & 0xFFF) : (id & 0xFFFFFF);
		idOf[{ Lower(_save->pluginOf(id)), local }] = id;
	}
	std::vector<std::size_t>                  quests;
	std::map<std::uint32_t, std::size_t>      byId;
	std::vector<std::size_t>                  rest;
	for (auto i : a_run) {
		const auto* q = _tpChanges[i].quest;
		if (!q) {
			rest.push_back(i);
			continue;
		}
		quests.push_back(i);
		if (auto it = idOf.find({ Lower(q->plugin), q->local }); it != idOf.end()) {
			byId[it->second] = i;
		}
	}
	const auto&                                     defs = _db->questDefs();
	std::map<std::size_t, std::vector<std::size_t>> before;  // change -> changes to run before it
	for (const auto& [id, i] : byId) {
		for (auto p : quests::LeadsInto(id, defs, [this](std::uint32_t q) { return questPreview(q).bodies; })) {
			if (auto it = byId.find(p); it != byId.end() && it->second != i) {
				before[i].push_back(it->second);
			}
		}
	}
	std::vector<std::size_t> ordered;
	std::set<std::size_t>    placed, visiting;
	std::function<void(std::size_t)> place = [&](std::size_t i) {
		if (placed.contains(i) || visiting.contains(i)) {
			return;  // (a loop in the quest data: keep the order found)
		}
		visiting.insert(i);
		for (auto p : before[i]) {
			place(p);
		}
		visiting.erase(i);
		placed.insert(i);
		ordered.push_back(i);
	};
	for (auto i : quests) {
		place(i);
	}
	a_run = rest;
	a_run.insert(a_run.end(), ordered.begin(), ordered.end());
}

// ---- the window -----------------------------------------------------------------------------------

// the group a change is listed under inside its category: items by kind, the rest by mod
std::string App::transplantGroup(const tp::Change& a_ch) const
{
	auto at = [&](std::size_t i) { return i < a_ch.record.size() ? a_ch.record[i] : std::string(); };
	if (a_ch.cat == tp::Category::kItems) {
		if (!at(12).empty()) {
			return at(12);  // written by ReSaverHelper 6
		}
		if (Lower(at(1)) == "skyrim.esm" && std::strtoul(at(2).c_str(), nullptr, 16) == 0xF) {
			return "金币";
		}
		if (_db) {
			if (const auto* f = _db->find(static_cast<std::uint32_t>(std::strtoul(at(3).c_str(), nullptr, 16)))) {
				const auto t = _db->type(*f);
				return t == "WEAP" ? "武器" : t == "ARMO" ? "护甲与衣物" : t == "AMMO" ? "箭与弩矢" : t == "BOOK" ? "书与法术书"
					   : t == "ALCH" ? "药水与食物" : t == "INGR" ? "材料" : t == "SLGM" ? "灵魂石" : t == "KEYM" ? "钥匙"
					   : t == "SCRL" ? "卷轴" : t == "MISC" ? "杂项" : t == "LIGH" ? "火把与灯具" : "其他";
			}
		}
		return "其他";
	}
	if (a_ch.cat == tp::Category::kMade) {
		return at(0) == "potion" ? (at(3) == "1" ? "毒药" : "药水") : "附魔装备";
	}
	if (a_ch.cat == tp::Category::kCharacter) {
		return {};
	}
	if (a_ch.cat == tp::Category::kLook || a_ch.cat == tp::Category::kAppearance) {
		return {};
	}
	if (a_ch.quest) {
		return a_ch.background ? "~ 后台任务（不在日志里）：通常不要勾选" : a_ch.quest->plugin;
	}
	return at(1);
}

void App::maximizeControl(const char* a_id)
{
	// a Maximize / Restore button at the top right, and a double-click on the title bar does the same
	const bool  maxed = _maximized.contains(a_id);
	const auto  wp = ImGui::GetWindowPos();
	const auto  ws = ImGui::GetWindowSize();
	const float title = ImGui::GetFrameHeight();
	bool        toggle = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(wp, ImVec2(wp.x + ws.x, wp.y + title), false) &&
				  ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
	const char* label = maxed ? "还原大小" : "最大化";
	const float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
	const auto  back = ImGui::GetCursorPos();
	ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - w, back.y));
	toggle |= ImGui::SmallButton(label);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("%s（或双击标题栏）", maxed ? "回到原来的大小" : "填满整个 ReSaver++ 窗口");
	}
	ImGui::SetCursorPos(back);
	if (toggle) {
		if (maxed) {
			_restoreRects[a_id] = _maximized[a_id];
			_maximized.erase(a_id);
		} else {
			_maximized[a_id] = { wp, ws };
		}
	}
}

void App::prepareWindow(const char* a_id, ImVec2 a_defaultFraction)
{
	const auto* vp = ImGui::GetMainViewport();
	if (_maximized.contains(a_id)) {
		ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
		ImGui::SetNextWindowSize(vp->WorkSize, ImGuiCond_Always);
	} else if (auto it = _restoreRects.find(a_id); it != _restoreRects.end()) {
		ImGui::SetNextWindowPos(it->second.first, ImGuiCond_Always);
		ImGui::SetNextWindowSize(it->second.second, ImGuiCond_Always);
		_restoreRects.erase(it);
	} else {
		ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * a_defaultFraction.x, vp->WorkSize.y * a_defaultFraction.y), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
	}
}

void App::drawTransplant()
{
	prepareWindow("Transplant", ImVec2(0.9f, 0.9f));
	if (!ImGui::Begin("移植", &_showTransplant, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Transplant");
	const auto info = _live.info();
	const bool live = info.connected && info.inGame;
	const bool helperOk = helperAtLeast(5);
	const bool busy = _tpRunning || _tpWaiting;

	ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 140);
	ImGui::TextWrapped("把角色进度从坏档移植到之前的好档里。ReSaver++ 会列出坏档有、"
					   "好档没有的内容；你来勾选要带什么；由游戏自己按正常方式放进好档，所以结果"
					   "是一个由游戏写出的干净存档。需要天际正在运行，并装有 ReSaverHelper.dll 5 或更新版本。");
	ImGui::PopTextWrapPos();
	if (!helperOk) {
		Wrapped(theme::Caution(), info.connected ? Fmt("ReSaverHelper %s 正在运行：需要 5 或更新版本（模组页的 Optional files）。", info.helperVersion.c_str())
												 : "ReSaverHelper 没有连上：请安装 ReSaverHelper.dll（5 或更新版本）后启动天际。");
	}
	if (helperOk && helperAtLeast(9) && !helperAtLeast(10)) {
		Wrapped(theme::Caution(), Fmt("ReSaverHelper %s 正在运行：随从需要 10 版。请安装最新的 ReSaverHelper 并重新生成快照。",
									  info.helperVersion.c_str()));
	}
	if (helperOk && !helperAtLeast(9)) {
		Wrapped(theme::Caution(), Fmt("ReSaverHelper %s 正在运行：外观和穿戴装备需要 7 版，把任务带进日志需要"
									  "9 版。请安装最新的 ReSaverHelper 并重新生成快照。",
									  info.helperVersion.c_str()));
	}
	if (_tpWaiting && ImGui::GetTime() - _tpWaitStart > 75.0) {
		_tpWaiting = false;
		_status = "游戏没有回应快照请求。天际有没有在前台保持几秒？再试一次；如果仍然不"
				  "回应，请重启游戏，并把 ReSaverHelper.log 发出来（Documents\\My Games\\Skyrim Special Edition\\SKSE）。";
	}
	if (_tpWaiting) {
		Wrapped(theme::Caution(), Fmt("正在等游戏回应（%.0f 秒）：请切到天际几秒（它不在前台时会暂停），"
									  "然后回来。", ImGui::GetTime() - _tpWaitStart));
	}

	// steps 1 and 2 side by side
	if (ImGui::BeginTable("steps", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
		ImGui::TableNextColumn();
		ImGui::SeparatorText("1. 坏档：取出它的进度");
		ImGui::TextWrapped("%s", Summary(_tpBad).c_str());
		ImGui::BeginDisabled(!live || !helperOk || busy);
		const bool badBtn = ImGui::Button("给游戏做快照（游戏里读的是坏档）");
		if (!_tpBad.valid && !busy) {
			Marquee();
			ImGui::SameLine();
			TextColored(IM_COL32(255, 196, 40, 255), "← 第 1 步：从这里开始");
		}
		if (badBtn) {
			confirm("给坏档做快照", "现在游戏里读的是坏档吗？\n\nReSaverHelper 会把已读档游戏里关于你角色的所有内容记下来："
											 "等级、技能、专长、法术、龙吼、物品、任务、地图地点。\n\n"
											 "天际不在前台时会暂停：点确定后，请切到游戏里待一会儿。",
				[this] { transplantSnapshot(true); });
		}
		ImGui::EndDisabled();
		ImGui::BeginDisabled(busy);
		if (ImGui::Button("坏档读不出来？从存档文件读取…") && platform::HasNativeDialogs()) {
			if (auto p = platform::OpenSaveDialog(savesFolder())) {
				transplantFromFile(*p);
			}
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("给游戏完全读不出来的存档用。没有游戏时只能读出其中一部分：\n"
							  "任务、物品、法术和龙吼、等级。不含技能、专长、力量之语、地图地点和你制作的东西。");
		}
		if (_save) {
			if (ImGui::SmallButton("用 ReSaver++ 里打开的这个存档")) {
				_tpBad = tp::FromSave(*_save, platform::ToUtf8(_path.filename()));
				transplantNames(_tpBad, *_save);
				_tpDirty = true;
			}
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("打开快照文件…") && platform::HasNativeDialogs()) {
			if (auto p = platform::OpenFileDialog({}, "txt", "ReSaverHelper 快照（Documents\\My Games\\...\\SKSE\\ReSaverHelper）")) {
				transplantFromFile(*p);
			}
		}
		ImGui::EndDisabled();
		if (_tpBad.valid && _tpBad.fromFile) {
			std::string parts;
			for (const auto& m : _tpBad.missingParts) {
				parts += (parts.empty() ? "" : ", ") + m;
			}
			Wrapped(theme::Caution(), "没有游戏、只从文件读出：" + parts + " 不含在内。");
		}

		ImGui::TableNextColumn();
		ImGui::SeparatorText("2. 好档：在游戏里读取它，然后做快照");
		ImGui::TextWrapped("%s", Summary(_tpGood).c_str());
		ImGui::BeginDisabled(!live || !helperOk || !_tpBad.valid || busy);
		const bool goodBtn = ImGui::Button(_tpGood.valid ? "再给游戏做一次快照（游戏里读的是好档）" : "给游戏做快照（游戏里读的是好档）");
		if (_tpBad.valid && !_tpGood.valid && !busy) {
			Marquee();
			ImGui::SameLine();
			TextColored(IM_COL32(255, 196, 40, 255), "← 第 2 步：现在做这个");
		}
		if (goodBtn) {
			confirm("给好档做快照", "现在游戏里读的是好档吗？（先在游戏菜单里读取它，并做一个备份存档。）\n\n"
											  "点确定后，请切到游戏里待一会儿：它不在前台时会暂停。",
				[this] { transplantSnapshot(false); });
		}
		ImGui::EndDisabled();
		if (_tpBad.valid && _tpGood.valid && !_tpBad.player.empty() && !_tpGood.player.empty() && _tpBad.player != _tpGood.player) {
			Wrapped(theme::Bad(), "这是两个不同的角色（" + _tpBad.player + " / " + _tpGood.player + "）。");
		}
		ImGui::EndTable();
	}

	if (_tpDirty && _tpBad.valid && _tpGood.valid) {
		_tpChanges = tp::Diff(_tpGood, _tpBad);
		_tpResults.clear();
		_tpDirty = false;
	}
	if (!_tpBad.valid || !_tpGood.valid) {
		if (_tpBad.valid) {
			ImGui::SeparatorText("坏档有什么（这些就是能带过去的）");
			ImGui::BeginChild("##tpbad", ImVec2(0, 0), ImGuiChildFlags_Borders);
			SnapshotContents(_tpBad);
			ImGui::EndChild();
		}
		ImGui::End();
		return;
	}

	// step 3: what to bring, in folded groups
	ImGui::SeparatorText(Fmt("3. 要带过去的内容（%zu）", _tpChanges.size()).c_str());
	Dot(theme::Good());
	ImGui::TextUnformatted("安全");
	ImGui::SameLine(0, 18);
	Dot(theme::Caution());
	ImGui::TextUnformatted("谨慎（悬停看原因）");
	ImGui::SameLine(0, 18);
	Dot(theme::Bad());
	ImGui::TextUnformatted("不能：它的模组不在这个游戏里");
	ImGui::SameLine(0, 30);
	int openAll = -1;
	if (ImGui::SmallButton("全部展开")) {
		openAll = 1;
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("全部折叠")) {
		openAll = 0;
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(_tpRunning);
	if (ImGui::SmallButton("全选")) {
		for (auto& c : _tpChanges) {
			c.tick = c.available;
		}
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("全不选")) {
		for (auto& c : _tpChanges) {
			c.tick = false;
		}
	}
	ImGui::EndDisabled();
	ImGui::SameLine(0, 20);
	ImGui::SetNextItemWidth(-1);
	ImGui::InputTextWithHint("##tpfilter", "搜索（展开匹配组）", _tpFilter, sizeof(_tpFilter));
	const auto needle = Lower(_tpFilter);
	if (!needle.empty()) {
		openAll = 1;
	}

	std::map<std::size_t, const TpResult*> results;
	for (const auto& r : _tpResults) {
		results[r.index] = &r;
	}
	auto row = [&](std::size_t i) {
		auto& ch = _tpChanges[i];
		ImGui::PushID(static_cast<int>(i));
		ImGui::BeginDisabled(!ch.available || _tpRunning);
		ImGui::Checkbox("##t", &ch.tick);
		ImGui::EndDisabled();
		ImGui::SameLine();
		Dot(RiskCol(ch.risk));
		ImGui::TextUnformatted(ch.label.c_str());
		if (ImGui::IsItemHovered() && !ch.why.empty()) {
			ImGui::SetTooltip("%s", ch.why.c_str());
		}
		if (!ch.detail.empty()) {
			ImGui::SameLine();
			ImGui::TextDisabled("%s", ch.detail.c_str());
		}
		if (auto it = results.find(i); it != results.end()) {
			ImGui::SameLine();
			TextColored(it->second->ok ? theme::Good() : theme::Bad(),
				it->second->ok ? (it->second->why.empty() ? "完成" : "完成：" + it->second->why) : "未完成：" + it->second->why);
		}
		ImGui::PopID();
	};
	auto tickButtons = [&](const std::vector<std::size_t>& a_in) {
		ImGui::BeginDisabled(_tpRunning);
		if (ImGui::SmallButton("全选")) {
			for (auto i : a_in) {
				_tpChanges[i].tick = _tpChanges[i].available;
			}
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("全不选")) {
			for (auto i : a_in) {
				_tpChanges[i].tick = false;
			}
		}
		ImGui::EndDisabled();
	};
	auto counts = [&](const std::vector<std::size_t>& a_in) {
		std::size_t t = 0;
		for (auto i : a_in) {
			t += _tpChanges[i].tick && _tpChanges[i].available ? 1 : 0;
		}
		return Fmt("已勾选 %zu / %zu", t, a_in.size());
	};

	ImGui::BeginChild("##tplist", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.4f), ImGuiChildFlags_Borders);
	if (_tpChecking) {
		Wrapped(theme::Caution(), "正在核对游戏的任务日志（如果很久，请切到天际一会儿）……");
	} else if (_tpChecked) {
		if (_tpMissing.empty()) {
			Wrapped(theme::Good(), "坏档日志里的每个任务现在都在游戏的日志里了。");
		} else {
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::Caution()));
			ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
			const bool open = ImGui::CollapsingHeader(Fmt("没能带过去的任务（%zu）###tpmissing", _tpMissing.size()).c_str());
			ImGui::PopStyleColor();
			if (open) {
				ImGui::Indent();
				ImGui::TextWrapped("这些在坏档的日志里，但现在的游戏日志里没有。未完成的，等你在游戏里"
								   "做启动它们的事（读纸条、跟人说话、到那个地点）就会回来。已完成的"
								   "可以在这里标记为完成（和“被拒绝的任务标记为完成”一样：没有奖励，后续任务不会启动）。");
				std::string all;
				for (std::size_t mi = 0; mi < _tpMissing.size(); ++mi) {
					auto& m = _tpMissing[mi];
					ImGui::PushID(static_cast<int>(mi));
					if (m.completed && m.quest) {
						ImGui::BeginDisabled(!_live.info().connected || _tpRunning || !m.done.empty());
						if (ImGui::SmallButton("标记为完成")) {
							m.done = "标记中…";
							const auto* q = m.quest;
							_live.request({ "RESOLVE", q->plugin, Fmt("%06X", q->local) }, [this, mi, q](const LiveLink::Reply& r) {
								auto set = [this, mi](const std::string& a_done) {
									if (mi < _tpMissing.size()) {
										_tpMissing[mi].done = a_done;
									}
								};
								if (!LiveLink::Ok(r)) {
									set("未完成：" + LiveLink::Error(r));
									return;
								}
								_live.request({ "COMPLETE", LiveLink::Field(r, 0), "none", "" }, [this, q, set](const LiveLink::Reply& c) {
									const bool ok = LiveLink::Ok(c) && LiveLink::Field(c, 0) != "0";
									if (!ok) {
										set("游戏不肯把它标记为完成");
										return;
									}
									set("已标记为完成");
									if (!q->objectives.empty() && helperAtLeast(9)) {  // and listed in the journal as it was
										_live.request({ "APPLY", "questobj", q->plugin, Fmt("%06X", q->local), "", q->objectives }, [](const LiveLink::Reply&) {});
									}
								});
							});
						}
						ImGui::EndDisabled();
						ImGui::SameLine();
					}
					Dot(m.completed ? theme::Caution() : theme::Bad());
					ImGui::TextUnformatted(m.name.c_str());
					ImGui::SameLine();
					ImGui::TextDisabled("[%s]  %s  -  %s", m.edid.c_str(), m.completed ? "在坏档中已完成" : "在坏档中进行中", m.why.c_str());
					all += m.name + "  [" + m.edid + "]  (" + (m.completed ? "已完成" : "进行中") + "): " + m.why + "\n";
					if (!m.done.empty()) {
						ImGui::SameLine();
						TextColored(m.done == "已标记为完成" ? theme::Good() : theme::Caution(), m.done);
					}
					ImGui::PopID();
				}
				if (ImGui::SmallButton("复制列表")) {
					ImGui::SetClipboardText(all.c_str());
				}
				ImGui::SameLine();
				ImGui::BeginDisabled(!_live.info().connected || _tpRunning || _tpChecking);
				if (ImGui::SmallButton("再核对一次日志")) {
					transplantCheckJournal();
				}
				ImGui::EndDisabled();
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
					ImGui::SetTooltip("移植后任务可能还要加载一两分钟：过一会儿再核对。");
				}
				ImGui::Unindent();
			}
		}
		ImGui::Spacing();
	}
	// shown in this order: your character's look, then your followers (both stand out), then the rest.
	// (They are still put into the game in category order: followers last, after the quests.)
	std::vector<int> shown{ static_cast<int>(tp::Category::kAppearance), static_cast<int>(tp::Category::kFollowers) };
	for (int c = 0; c < static_cast<int>(tp::Category::kCount); ++c) {
		if (c != shown[0] && c != shown[1]) {
			shown.push_back(c);
		}
	}
	for (const int c : shown) {
		std::vector<std::size_t> in;
		for (std::size_t i = 0; i < _tpChanges.size(); ++i) {
			const auto& ch = _tpChanges[i];
			if (static_cast<int>(ch.cat) == c && (needle.empty() || Lower(ch.label + " " + ch.detail).find(needle) != std::string::npos)) {
				in.push_back(i);
			}
		}
		if (in.empty()) {
			continue;
		}
		ImGui::PushID(c);
		if (openAll >= 0) {
			ImGui::SetNextItemOpen(openAll == 1);
		}
		// the character's look stands out: it is the one thing that changes how your character looks
		const bool appearance = c == static_cast<int>(tp::Category::kAppearance) || c == static_cast<int>(tp::Category::kFollowers);
		if (appearance) {
			const bool light = theme::IsLight();
			ImGui::PushStyleColor(ImGuiCol_Header, light ? ImVec4(0.12f, 0.12f, 0.14f, 1) : ImVec4(0.02f, 0.02f, 0.03f, 1));
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, light ? ImVec4(0.2f, 0.2f, 0.24f, 1) : ImVec4(0.10f, 0.08f, 0.02f, 1));
			ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.18f, 0.14f, 0.04f, 1));
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.82f, 0.30f, 1));
			if (openAll < 0) {
				ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
			}
		}
		const bool open = ImGui::CollapsingHeader(Fmt("%s   (%s)###cat", tp::CategoryName(static_cast<tp::Category>(c)), counts(in).c_str()).c_str());
		if (appearance) {
			// a glow around the header
			const auto mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
			auto*      dl = ImGui::GetWindowDrawList();
			for (int g = 1; g <= 3; ++g) {
				dl->AddRect(ImVec2(mn.x - g, mn.y - g), ImVec2(mx.x + g, mx.y + g), IM_COL32(255, 200, 70, 140 / g), 4.0f, 0, 1.5f);
			}
			ImGui::PopStyleColor(4);
		}
		if (open) {
			ImGui::Indent();
			tickButtons(in);
			if (c == static_cast<int>(tp::Category::kQuests)) {
				ImGui::SameLine();
				ImGui::TextDisabled("任务按任务链顺序运行，排在其他所有内容之后。");
			}
			// groups: items by kind, the rest by mod (when there are many)
			std::map<std::string, std::vector<std::size_t>> groups;
			for (auto i : in) {
				groups[transplantGroup(_tpChanges[i])].push_back(i);
			}
			const bool flat = groups.size() <= 1 || in.size() <= 12;
			if (flat) {
				for (auto i : in) {
					row(i);
				}
			} else {
				for (const auto& [name, gi] : groups) {
					ImGui::PushID(name.c_str());
					if (openAll >= 0) {
						ImGui::SetNextItemOpen(openAll == 1);
					}
					if (ImGui::TreeNodeEx("##grp", ImGuiTreeNodeFlags_SpanAvailWidth, "%s   (%s)", name.empty() ? "其他" : name.c_str(), counts(gi).c_str())) {
						tickButtons(gi);
						for (auto i : gi) {
							row(i);
						}
						ImGui::TreePop();
					}
					ImGui::PopID();
				}
			}
			ImGui::Unindent();
		}
		ImGui::PopID();
	}
	if (_tpChanges.empty()) {
		Wrapped(theme::Good(), "好档已经拥有坏档（能读到的部分）的全部内容。");
	}
	ImGui::EndChild();

	// apply
	std::vector<std::size_t> run;
	for (int c = 0; c < static_cast<int>(tp::Category::kCount); ++c) {  // character first, quests last
		for (std::size_t i = 0; i < _tpChanges.size(); ++i) {
			if (static_cast<int>(_tpChanges[i].cat) == c && _tpChanges[i].tick && _tpChanges[i].available) {
				run.push_back(i);
			}
		}
	}
	ImGui::BeginDisabled(_tpRunning);
	ImGui::Checkbox("被拒绝的任务标记为完成", &_tpMarkRefused);
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("游戏拒绝某个任务的阶段时（它的人物或地点在这个存档里没有设置好），就把这个任务\n"
						  "标记为完成，列表里也会写明（“游戏拒绝了阶段 10，已标记为完成”）。标记的任务没有奖励，\n"
						  "也不会启动通常跟在后面的任务。只有坏档进度更靠前的任务也适用这一条。\n"
						  "不想这样就不勾选，被拒绝的任务保持原样。");
	}
	ImGui::SameLine(0, 30);
	ImGui::BeginDisabled(!live || !helperOk || run.empty() || busy);
	const bool goBtn = ImGui::Button(Fmt("把勾选的 %zu 项移植进游戏", run.size()).c_str());
	if (!busy && _tpResults.empty() && !run.empty() && live && helperOk) {
		Marquee();
		ImGui::SameLine();
		TextColored(IM_COL32(255, 196, 40, 255), "← 第 3 步：开始");
	}
	if (goBtn) {
		confirm("移植",
			Fmt("现在把勾选的 %zu 项放进游戏？\n\n"
				"- 游戏里读的必须是那个好档（第 2 步做快照的那个）。\n"
				"- 先在游戏里存个档，方便回退。\n"
				"- 点确定后，切到天际并留在那里，不要暂停、不要开菜单，直到它做完。\n"
				"- 之后任务可能还要加载一两分钟：等它们的提示停下后在游戏里存档。",
				run.size()),
			[this, run] {
				_tpRun = run;
				transplantOrderQuests(_tpRun);
				_tpPos = 0;
				_tpResults.clear();
				_tpMissing.clear();
				_tpChecked = false;
				_tpRunning = true;
				transplantStep();
			});
	}
	ImGui::EndDisabled();
	if (_tpRunning) {
		ImGui::SameLine();
		ImGui::ProgressBar(_tpRun.empty() ? 0.0f : static_cast<float>(_tpPos) / static_cast<float>(_tpRun.size()), ImVec2(260, 0),
			Fmt("%zu / %zu", _tpPos, _tpRun.size()).c_str());
	} else if (!_tpResults.empty()) {
		std::size_t failed = 0;
		for (const auto& r : _tpResults) {
			failed += r.ok ? 0 : 1;
		}
		ImGui::SameLine();
		TextColored(failed ? theme::Caution() : theme::Good(),
			Fmt("完成：%zu 项已带过去，%zu 项没有（见列表）。任务可能还要继续加载一两分钟：等它们的"
				"提示停下后再在游戏里存档。",
				_tpResults.size() - failed, failed));
	}
	ImGui::TextDisabled("不会从好档里拿走任何东西：只添加内容，等级和技能只会提高。");
	ImGui::End();
}
