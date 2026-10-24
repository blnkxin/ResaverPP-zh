// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Quests page: every quest in the save with its stages, journal entries and objectives (read from the
// plugins and the save), what its completion stage's script does, and - with ReSaverHelper.dll in the
// running game - completing a quest, setting a stage and refreshing the journal in game.
#include "App.h"

#include "Theme.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

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

	// "needs the running game" hint on a disabled button
	void LiveHint(const LiveLink::Info& a_info)
	{
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::SetTooltip("%s", !a_info.connected ? "Start Skyrim with ReSaverHelper.dll installed to use this (optional download on the ReSaver Plus page)."
														: "Load a save in the game first.");
		}
	}
}

// ---- data ---------------------------------------------------------------------------------------

const std::vector<quests::QuestState>& App::questStates()
{
	if (_questGen != _editGen) {
		_questStates = _save ? quests::ReadStates(*_save) : std::vector<quests::QuestState>{};
		_questGen = _editGen;
		_questPreviews.clear();
	}
	return _questStates;
}

const App::QuestPreview& App::questPreview(std::uint32_t a_formID)
{
	auto& pv = _questPreviews[a_formID];
	if (pv.done) {
		return pv;
	}
	pv.done = true;
	const auto* def = _db ? _db->quest(a_formID) : nullptr;
	if (!def) {
		pv.error = "The plugins are not read yet (Options > Settings: mods folder, then names are read automatically).";
		return pv;
	}
	if (def->fragmentScript.empty()) {
		pv.error = "This quest has no stage scripts.";
		return pv;
	}
	try {
		const auto px = _sources.findPex(def->fragmentScript);
		if (!px) {
			pv.error = "Its stage script " + def->fragmentScript + ".pex was not found in your mods.";
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
		pv.error = std::string("Could not read its stage script: ") + e.what();
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

void App::liveSetStage(std::uint32_t a_formID, std::uint16_t a_stage, const std::string& a_what)
{
	const auto msg = "ReSaver++: " + a_what;
	_status = "Asking the game: " + a_what + "...";
	_live.request({ "SETSTAGE", Fmt("%08X", a_formID), std::to_string(a_stage), liveNotifyMode(), msg },
		[this, a_formID, a_what](const LiveLink::Reply& r) {
			if (!LiveLink::Ok(r)) {
				_status = "The game could not do it: " + LiveLink::Error(r);
				return;
			}
			const auto f = LiveLink::Field(r, 0);
			_status = f == "1" ? "Done in game: " + a_what + ". Save in the game to keep it." :
			          f == "?" ? "The game is still running that stage's script: " + a_what + "." :
			                     "The game refused that stage (its conditions were not met): " + a_what + ".";
			liveQuest(a_formID);
		});
}

// ---- live status (status bar) -------------------------------------------------------------------

void App::drawLiveStatus()
{
	_live.poll();
	const auto info = _live.info();
	if (!info.connected) {
		ImGui::TextDisabled("Live game: not connected");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Start Skyrim with ReSaverHelper.dll installed to complete quests, set stages and refresh the journal from here.");
		}
		return;
	}
	TextColored(theme::Good(), Fmt("Live game: %s %s%s", info.runtime.c_str(), info.gameVersion.c_str(), info.inGame ? "" : " (main menu)"));
}

// ---- journal refresh (live) -----------------------------------------------------------------------

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
				_status = Fmt("Journal refreshed in game: %zu objective(s). Save in the game to keep it.", _liveRefresh.done);
			}
		});
	}
	ImGui::BeginDisabled(!can || _liveRefresh.running);
	if (ImGui::Button("Refresh journal in game")) {
		_live.request({ "REFRESH", liveNotifyMode() }, [this](const LiveLink::Reply& r) {
			if (!LiveLink::Ok(r)) {
				_status = "The game could not refresh the journal: " + LiveLink::Error(r);
				return;
			}
			const auto n = std::strtoull(LiveLink::Field(r, 0).c_str(), nullptr, 10);
			if (n == 0) {
				_status = "Nothing to refresh: no running quest shows an objective.";
				return;
			}
			_liveRefresh = LiveRefresh{ true, 0, n, 0 };
		});
	}
	ImGui::EndDisabled();
	if (!can) {
		LiveHint(info);
	} else if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("For every running quest, each objective the journal shows is hidden and shown again (completed / failed\n"
						  "put back), so the game rebuilds its list. Brings back quests and objectives the journal stopped showing\n"
						  "(the same fix as Quest Journal Limit Bug Fixer).");
	}
	if (_liveRefresh.running) {
		ImGui::SameLine();
		const float frac = _liveRefresh.total ? float(_liveRefresh.done) / float(_liveRefresh.total) : 0.0f;
		ImGui::ProgressBar(frac, ImVec2(a_compact ? 160.0f : 240.0f, 0), Fmt("%zu / %zu", _liveRefresh.done, _liveRefresh.total).c_str());
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
	if (ImGui::Checkbox("Refresh automatically after every save load", &autoOn)) {
		_live.request({ "SETOPTION", "bRefreshJournalOnLoad", autoOn ? "1" : "0" }, [this, autoOn](const LiveLink::Reply& r) {
			if (LiveLink::Ok(r)) {
				_liveAutoRefresh = autoOn ? 1 : 0;
				_status = autoOn ? "ReSaverHelper will refresh the journal after every save load (saved in ReSaverHelper.ini)."
				                 : "ReSaverHelper will no longer refresh the journal on load.";
			} else {
				_status = "Could not change the helper's setting: " + LiveLink::Error(r);
			}
		});
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("%s", info.connected ? "Saved in ReSaverHelper.ini (bRefreshJournalOnLoad). The helper waits a few seconds after the load,\n"
											   "then refreshes and tells you when it is done."
											   : "Start the game with ReSaverHelper.dll to change this, or set bRefreshJournalOnLoad=1 in ReSaverHelper.ini.");
	}
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
	ImGui::TextUnformatted(Fmt("Your journal shows %zu objective(s).", shown).c_str());
	if (shownDone) {
		ImGui::SameLine();
		TextColored(theme::Warn(), Fmt("%zu of them belong to quests that are completed or stopped.", shownDone));
	}
	drawRefreshJournal(true);
	if (!_db) {
		Dim(namesLoading() ? "Reading your plugins for quest names, stages and journal text..."
						   : "Quest names, stages and journal text come from your plugins: set your mods folder in Options > Settings.");
	}
	ImGui::Separator();

	ImGui::SetNextItemWidth(-220);
	ImGui::InputTextWithHint("##qfilter", "filter by name, editor ID or form ID", _questFilter, sizeof(_questFilter));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1);
	const char* shows[] = { "In the journal", "Running", "Completed", "All" };
	ImGui::Combo("##qshow", &_questShow, shows, 4);

	struct Row
	{
		const quests::QuestState* q;
		const quests::QuestDef*   def;
		std::string               label;
	};
	std::vector<Row> rows;
	const auto       needle = Lower(_questFilter);
	for (const auto& q : states) {
		const auto* def = _db ? _db->quest(q.formID) : nullptr;
		switch (_questShow) {
		case 0:
			if (q.shownObjectives() == 0) {
				continue;
			}
			break;
		case 1:
			if (!q.running() || q.completed()) {
				continue;
			}
			break;
		case 2:
			if (!q.completed()) {
				continue;
			}
			break;
		default:
			break;
		}
		// name and editor ID from the plugins; until they are read, the plugin the quest comes from
		std::string label = def ? (def->name.empty() ? def->edid : def->name + "  (" + def->edid + ")") : "(" + _save->pluginOf(q.formID) + ")";
		if (!needle.empty() && Lower(label).find(needle) == std::string::npos && Lower(Fmt("%08X", q.formID)).find(needle) == std::string::npos) {
			continue;
		}
		rows.push_back({ &q, def, std::move(label) });
	}
	std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return Lower(a.label) < Lower(b.label); });

	if (ImGui::BeginTable("quests", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
										   ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Quest", ImGuiTableColumnFlags_WidthStretch, 4.0f);
		ImGui::TableSetupColumn("Form ID", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.6f);
		ImGui::TableSetupColumn("Stage", ImGuiTableColumnFlags_WidthStretch, 0.7f);
		ImGui::TableSetupColumn("Shown", ImGuiTableColumnFlags_WidthStretch, 0.7f);
		ImGui::TableHeadersRow();
		ImGuiListClipper clip;
		clip.Begin(static_cast<int>(rows.size()));
		while (clip.Step()) {
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
				const auto& r = rows[i];
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::PushID(static_cast<int>(r.q->formID));
				if (ImGui::Selectable(r.label.c_str(), _questSel == r.q->formID, ImGuiSelectableFlags_SpanAllColumns)) {
					_questSel = r.q->formID;
					_setStageIdx = 0;
					if (_live.info().inGame) {
						liveQuest(_questSel);
					}
				}
				ImGui::PopID();
				ImGui::TableNextColumn();
				ImGui::TextDisabled("%08X", r.q->formID);
				ImGui::TableNextColumn();
				const bool odd = r.q->shownObjectives() && (r.q->completed() || !r.q->running());
				if (odd) {
					TextColored(theme::Warn(), r.q->statusText());
				} else {
					ImGui::TextUnformatted(r.q->statusText());
				}
				ImGui::TableNextColumn();
				const auto st = r.q->highestDoneStage();
				if (st >= 0) {
					ImGui::Text("%d", st);
				}
				ImGui::TableNextColumn();
				if (const auto n = r.q->shownObjectives()) {
					ImGui::Text("%zu", n);
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
	if (!q) {
		Dim("Click a quest to see its stages, journal entries and objectives, and what completing it gives.");
		return;
	}
	const auto* def = _db ? _db->quest(q->formID) : nullptr;
	const auto  info = _live.info();
	const bool  live = info.connected && info.inGame;

	ImGui::TextUnformatted(def && !def->name.empty() ? def->name.c_str() : (def ? def->edid.c_str() : Fmt("%08X", q->formID).c_str()));
	Dim(Fmt("%s  |  %08X  |  %s", def ? def->edid.c_str() : "?", q->formID, def ? _db->plugins()[def->plugin].c_str() : _save->pluginOf(q->formID).c_str()));
	ImGui::Text("In this save: %s, highest stage done %d, %zu objective(s) shown in the journal", q->statusText(), q->highestDoneStage(), q->shownObjectives());
	if (live) {
		auto it = _liveQuests.find(q->formID);
		if (it != _liveQuests.end() && it->second.valid) {
			TextColored(theme::Good(), Fmt("In the running game: stage %d, %s", it->second.stage,
										   it->second.completed ? "completed" : it->second.running ? "running" : "not running"));
		} else if (it != _liveQuests.end()) {
			Dim("In the running game: " + it->second.error);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Check in game")) {
			liveQuest(q->formID);
		}
	}

	// ---- actions
	const auto completion = def ? def->completionStages() : std::vector<std::uint16_t>{};
	const std::string name = def ? (def->name.empty() ? def->edid : def->name) : Fmt("%08X", q->formID);
	ImGui::Spacing();
	ImGui::BeginDisabled(!live || completion.empty());
	if (ImGui::Button("Complete quest in game")) {
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
		confirm("Complete quest",
			Fmt("Set \"%s\" to its completion stage %u in the running game?\n\nThe game runs that stage's own script, so its rewards are given and "
				"follow-up quests start the normal way.%s\n\nSkipping a quest's earlier stages can leave things unfinished (items not "
				"collected, people not met). Make a save in the game first.",
				name.c_str(), st, effects.empty() ? "" : ("\n\nWhat the stage script does:" + effects).c_str()),
			[this, fid = q->formID, st, name] { liveSetStage(fid, st, Fmt("completed \"%s\" (stage %u)", name.c_str(), st)); });
	}
	ImGui::EndDisabled();
	if (!live) {
		LiveHint(info);
	} else if (completion.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("No stage of this quest is marked as completing it.");
	}
	ImGui::SameLine();
	// set any stage
	std::vector<std::uint16_t> stageList;
	if (def) {
		for (const auto& s : def->stages) {
			stageList.push_back(s.index);
		}
	}
	_setStageIdx = std::clamp(_setStageIdx, 0, std::max(0, static_cast<int>(stageList.size()) - 1));
	ImGui::SetNextItemWidth(110);
	ImGui::BeginDisabled(stageList.empty());
	if (ImGui::BeginCombo("##stage", stageList.empty() ? "stage" : Fmt("stage %u", stageList[_setStageIdx]).c_str())) {
		for (int i = 0; i < static_cast<int>(stageList.size()); ++i) {
			if (ImGui::Selectable(Fmt("stage %u", stageList[i]).c_str(), i == _setStageIdx)) {
				_setStageIdx = i;
			}
		}
		ImGui::EndCombo();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(!live || stageList.empty());
	if (ImGui::Button("Set stage in game") && !stageList.empty()) {
		const auto st = stageList[_setStageIdx];
		confirm("Set stage", Fmt("Set \"%s\" to stage %u in the running game? The stage's script runs. Make a save in the game first.", name.c_str(), st),
			[this, fid = q->formID, st, name] { liveSetStage(fid, st, Fmt("\"%s\" set to stage %u", name.c_str(), st)); });
	}
	ImGui::EndDisabled();
	if (!live) {
		LiveHint(info);
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(!def || def->edid.empty() || (completion.empty() && stageList.empty()));
	if (ImGui::Button("Copy console command") && def) {
		const auto st = completion.empty() ? stageList[_setStageIdx] : completion.front();
		ImGui::SetClipboardText(Fmt("setstage %s %u", def->edid.c_str(), st).c_str());
		_status = Fmt("Copied: setstage %s %u  (paste it into the game's console with Ctrl+V)", def->edid.c_str(), st);
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("Copies the console command that completes the quest (its completion stage), or sets the stage picked on the left.");
	}

	if (!def) {
		ImGui::Spacing();
		Dim("Stages, journal entries and objectives are read from your plugins once your mods folder is set.");
		return;
	}

	// ---- what completing it does
	if (!completion.empty()) {
		ImGui::SeparatorText("What completing it does");
		const auto& pv = questPreview(q->formID);
		if (!pv.error.empty()) {
			Dim(pv.error);
		}
		for (const auto& [st, body] : pv.bodies) {
			ImGui::Text("Stage %u completes the quest.", st);
			const auto effects = quests::ScanEffects(body);
			if (body.empty()) {
				Dim("  Its stage has no script.");
			} else if (effects.empty()) {
				Dim("  Its script gives nothing directly (rewards, if any, come from other scripts or dialogue).");
			}
			for (const auto& e : effects) {
				const auto col = e.kind == quests::Effect::Kind::kReward ? theme::Good() : e.kind == quests::Effect::Kind::kQuest ? theme::Warn() : theme::Dim();
				TextColored(col, (e.kind == quests::Effect::Kind::kReward ? "  gives / changes:  " : e.kind == quests::Effect::Kind::kQuest ? "  quests:  " : "  calls:  ") + e.line);
			}
			if (!body.empty() && ImGui::TreeNode(Fmt("##body%u", st).c_str(), "Its full script (%zu lines)", body.size())) {
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
		if (!def->properties.empty() && ImGui::TreeNode("props", "Properties of its stage script (%zu)", def->properties.size())) {
			for (const auto& p : def->properties) {
				std::string v = p.type == 1 ? formText(p.form) : p.text;
				if (p.type == 1 && p.alias >= 0) {
					v = Fmt("alias %d of this quest", p.alias);
				}
				ImGui::BulletText("%s = %s", p.name.c_str(), v.c_str());
			}
			ImGui::TreePop();
		}
	}

	// ---- stages
	ImGui::SeparatorText(Fmt("Stages (%zu)", def->stages.size()).c_str());
	if (ImGui::BeginTable("stages", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Stage", ImGuiTableColumnFlags_WidthStretch, 0.7f);
		ImGui::TableSetupColumn("Done", ImGuiTableColumnFlags_WidthStretch, 0.5f);
		ImGui::TableSetupColumn("Journal entry", ImGuiTableColumnFlags_WidthStretch, 6.0f);
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
				TextColored(theme::Good(), "done");
			}
			ImGui::TableNextColumn();
			std::string text;
			for (const auto& l : s.logs) {
				if (!l.text.empty()) {
					text += (text.empty() ? "" : "\n") + l.text;
				}
			}
			if (s.completes()) {
				TextColored(theme::Good(), "[completes the quest]");
			}
			if (s.fails()) {
				TextColored(theme::Bad(), "[fails the quest]");
			}
			if (!text.empty()) {
				ImGui::TextWrapped("%s", text.c_str());
			}
		}
		ImGui::EndTable();
	}

	// ---- objectives
	ImGui::SeparatorText(Fmt("Objectives (%zu)", def->objectives.size()).c_str());
	if (ImGui::BeginTable("objs", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthStretch, 0.5f);
		ImGui::TableSetupColumn("In this save", ImGuiTableColumnFlags_WidthStretch, 1.4f);
		ImGui::TableSetupColumn("Objective", ImGuiTableColumnFlags_WidthStretch, 5.0f);
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
