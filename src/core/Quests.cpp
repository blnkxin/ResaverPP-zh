// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Quests.h"

#include "ByteIO.h"
#include "ChangeFormData.h"

#include <algorithm>
#include <unordered_map>
#include <cctype>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ess::quests
{
	bool Stage::completes() const
	{
		return std::any_of(logs.begin(), logs.end(), [](const LogEntry& l) { return l.completes(); });
	}
	bool Stage::fails() const
	{
		return std::any_of(logs.begin(), logs.end(), [](const LogEntry& l) { return l.fails(); });
	}

	const Stage* QuestDef::stage(std::uint16_t a_index) const
	{
		for (const auto& s : stages) {
			if (s.index == a_index) {
				return &s;
			}
		}
		return nullptr;
	}

	const Fragment* QuestDef::fragmentFor(std::uint16_t a_stage) const
	{
		for (const auto& f : fragments) {
			if (f.stage == a_stage) {
				return &f;
			}
		}
		return nullptr;
	}

	std::vector<std::uint16_t> QuestDef::completionStages() const
	{
		std::vector<std::uint16_t> out;
		for (const auto& s : stages) {
			if (s.completes()) {
				out.push_back(s.index);
			}
		}
		std::sort(out.begin(), out.end());
		return out;
	}

	namespace
	{
		std::string WString(Reader& r)
		{
			const auto n = r.u16();
			r.need(n);
			std::string s(reinterpret_cast<const char*>(r.here()), n);
			r.seek(r.pos() + n);
			return s;
		}

		bool EqualsNoCase(const std::string& a, const std::string& b)
		{
			return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
				return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
			});
		}

		std::string FloatText(float f)
		{
			char b[48];
			std::snprintf(b, sizeof(b), "%g", f);
			return b;
		}

		// one object value: 8 bytes, field order depends on the object format
		void ReadObject(Reader& r, std::int16_t a_objFormat, const ToRuntime& a_rt, std::uint32_t& a_form, std::int16_t& a_alias)
		{
			std::uint32_t raw = 0;
			if (a_objFormat == 1) {
				raw = r.u32();
				a_alias = r.i16();
				r.u16();
			} else {
				r.u16();
				a_alias = r.i16();
				raw = r.u32();
			}
			a_form = 0;
			if (raw && a_rt) {
				a_rt(raw, a_form);
			}
		}

		Property ReadProperty(Reader& r, std::int16_t a_version, std::int16_t a_objFormat, const ToRuntime& a_rt)
		{
			Property p;
			p.name = WString(r);
			p.type = r.u8();
			if (a_version >= 4) {
				r.u8();  // status
			}
			switch (p.type) {
			case 1:
				ReadObject(r, a_objFormat, a_rt, p.form, p.alias);
				break;
			case 2:
				p.text = WString(r);
				break;
			case 3:
				p.text = std::to_string(r.i32());
				break;
			case 4:
				p.text = FloatText(r.f32());
				break;
			case 5:
				p.text = r.u8() ? "true" : "false";
				break;
			case 11:
				{
					const auto n = r.u32();
					for (std::uint32_t i = 0; i < n; ++i) {
						std::uint32_t f = 0;
						std::int16_t  a = -1;
						ReadObject(r, a_objFormat, a_rt, f, a);
						p.forms.push_back(f);
					}
					p.text = std::to_string(n) + " 个对象";
					break;
				}
			case 12:
				{
					const auto n = r.u32();
					for (std::uint32_t i = 0; i < n; ++i) {
						p.text += (i ? ", " : "") + WString(r);
					}
					break;
				}
			case 13:
				{
					const auto n = r.u32();
					for (std::uint32_t i = 0; i < n; ++i) {
						p.text += (i ? ", " : "") + std::to_string(r.i32());
					}
					break;
				}
			case 14:
				{
					const auto n = r.u32();
					for (std::uint32_t i = 0; i < n; ++i) {
						p.text += (i ? ", " : "") + FloatText(r.f32());
					}
					break;
				}
			case 15:
				{
					const auto n = r.u32();
					for (std::uint32_t i = 0; i < n; ++i) {
						p.text += std::string(i ? ", " : "") + (r.u8() ? "true" : "false");
					}
					break;
				}
			default:
				throw std::runtime_error("未知的 VMAD 属性类型 " + std::to_string(p.type));
			}
			return p;
		}

		struct Script
		{
			std::string           name;
			std::vector<Property> props;
		};

		std::vector<Script> ReadScripts(Reader& r, std::int16_t a_version, std::int16_t a_objFormat, const ToRuntime& a_rt)
		{
			std::vector<Script> out;
			const auto          n = r.u16();
			for (std::uint16_t i = 0; i < n; ++i) {
				Script s;
				s.name = WString(r);
				if (a_version >= 4) {
					r.u8();  // status
				}
				const auto pc = r.u16();
				for (std::uint16_t k = 0; k < pc; ++k) {
					s.props.push_back(ReadProperty(r, a_version, a_objFormat, a_rt));
				}
				out.push_back(std::move(s));
			}
			return out;
		}
	}

	bool ParseQuestVMAD(const std::uint8_t* a_data, std::size_t a_size, const ToRuntime& a_rt, QuestDef& a_out)
	{
		try {
			Reader     r(a_data, a_size);
			const auto version = r.i16();
			const auto objFormat = r.i16();
			const auto scripts = ReadScripts(r, version, objFormat, a_rt);
			for (const auto& s : scripts) {
				a_out.scripts.push_back(s.name);
			}
			if (r.remaining() == 0) {
				return true;  // no fragments
			}
			r.u8();  // extra bind data version
			const auto fragCount = r.u16();
			a_out.fragmentScript = WString(r);
			for (std::uint16_t i = 0; i < fragCount; ++i) {
				Fragment f;
				f.stage = r.u16();
				r.i16();
				f.logEntry = r.i32();
				r.u8();
				f.script = WString(r);
				f.function = WString(r);
				a_out.fragments.push_back(std::move(f));
			}
			// the properties the fragments use are those of the fragment script
			for (const auto& s : scripts) {
				if (EqualsNoCase(s.name, a_out.fragmentScript)) {
					a_out.properties = s.props;
				}
			}
			return true;
		} catch (const std::exception&) {
			return false;
		}
	}

	// ---- stage scripts -------------------------------------------------------------------------------

	namespace
	{
		std::string Trim(const std::string& a)
		{
			std::size_t b = 0, e = a.size();
			while (b < e && std::isspace(static_cast<unsigned char>(a[b]))) {
				++b;
			}
			while (e > b && std::isspace(static_cast<unsigned char>(a[e - 1]))) {
				--e;
			}
			return a.substr(b, e - b);
		}

		std::string LowerStr(std::string a)
		{
			for (auto& c : a) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return a;
		}
	}

	std::vector<std::string> FunctionBody(const std::vector<std::string>& a_source, const std::string& a_function)
	{
		std::vector<std::string> out;
		const auto               head = "function " + LowerStr(a_function) + "(";
		bool                     in = false;
		for (const auto& raw : a_source) {
			const auto t = LowerStr(Trim(raw));
			if (!in) {
				in = t.rfind(head, 0) == 0;
				continue;
			}
			if (t.rfind("endfunction", 0) == 0) {
				break;
			}
			if (!Trim(raw).empty() && t.rfind(";", 0) != 0) {
				out.push_back(Trim(raw));
			}
		}
		return out;
	}

	std::vector<Effect> ScanEffects(const std::vector<std::string>& a_body)
	{
		static const char* rewards[] = { ".additem(", ".addperk(", ".addspell(", ".addshout(", ".teachword(", ".unlockword(", ".modactorvalue(",
			".modav(", ".setactorvalue(", ".setav(", ".addtofaction(", ".setrelationshiprank(", ".modvalue(", ".setvalue(", ".removeitem(",
			"advanceskill(", "incrementskill(", ".modfavorpoints(", ".setplayerresistingarrest", ".modreputation(" };
		static const char* questCalls[] = { ".start()", ".stop()", ".setstage(", "completequest(", ".completeallobjectives(", ".reset()",
			".setobjectivecompleted(", ".setobjectivedisplayed(", ".failallobjectives(" };
		std::vector<Effect> out;
		for (const auto& line : a_body) {
			const auto l = LowerStr(line);
			Effect     e;
			e.line = line;
			bool hit = false;
			for (const char* r : rewards) {
				if (l.find(r) != std::string::npos) {
					e.kind = Effect::Kind::kReward;
					hit = true;
					break;
				}
			}
			if (!hit) {
				for (const char* q : questCalls) {
					if (l.find(q) != std::string::npos) {
						e.kind = Effect::Kind::kQuest;
						hit = true;
						break;
					}
				}
			}
			// a call into another script (often where the reward is handed out: "(GetOwningQuest() as X).Reward()")
			if (!hit && l.find(" as ") != std::string::npos && l.find(").") != std::string::npos) {
				e.kind = Effect::Kind::kOther;
				hit = true;
			}
			if (hit) {
				out.push_back(std::move(e));
			}
		}
		return out;
	}

	std::vector<std::uint32_t> LeadsInto(std::uint32_t a_target, const std::unordered_map<std::uint32_t, QuestDef>& a_defs, const StageBodies& a_bodies)
	{
		std::vector<std::uint32_t> pre;
		for (const auto& [id, d] : a_defs) {
			if (id == a_target || d.completionStages().empty()) {
				continue;
			}
			bool leads = false;
			for (const auto& st : d.stages) {
				for (const auto& l : st.logs) {
					leads |= l.nextQuest == a_target;
				}
			}
			if (!leads && a_bodies) {
				std::vector<std::string> props;
				for (const auto& p : d.properties) {
					if (p.type == 1 && p.form == a_target && p.alias < 0) {
						props.push_back(LowerStr(p.name));
					}
				}
				if (!props.empty()) {
					for (const auto& [stage, body] : a_bodies(id)) {
						for (const auto& line : body) {
							const auto l = LowerStr(line);
							for (const auto& p : props) {
								if (l.find(p + ".setstage(") != std::string::npos || l.find(p + ".start()") != std::string::npos ||
									l.find(p + ".setcurrentstageid(") != std::string::npos) {
									leads = true;
								}
							}
						}
					}
				}
			}
			if (leads) {
				pre.push_back(id);
			}
		}
		std::sort(pre.begin(), pre.end());
		return pre;
	}

	// ---- save ----------------------------------------------------------------------------------------

	std::int32_t QuestState::highestDoneStage() const
	{
		std::int32_t best = -1;
		for (const auto& [s, done] : stages) {
			if (done && s > best) {
				best = s;
			}
		}
		return best;
	}

	bool QuestState::inJournal() const
	{
		for (const auto& o : journal) {
			if (o.state == 1 || o.state == 3 || o.state == 5) {
				return true;
			}
		}
		return false;
	}

	std::size_t QuestState::shownObjectives() const
	{
		std::size_t n = 0;
		for (const auto& o : journalKnown ? journal : objectives) {
			n += (o.state == 1 || o.state == 3 || o.state == 5) ? 1 : 0;
		}
		return n;
	}

	const char* QuestState::statusText() const
	{
		if (!hasFlags) {
			return "未开始 / 未知";
		}
		if (failed()) {
			return "失败";
		}
		if (completed()) {
			return running() ? "已完成（仍在运行）" : "已完成";
		}
		return running() ? "进行中" : "已停止";
	}

	const char* ObjectiveStateText(std::int32_t a_state)
	{
		switch (a_state) {
		case 0: return "未显示";
		case 1: return "已显示";
		case 2: return "已完成";
		case 3: return "已完成，已显示";
		case 4: return "已失败";
		case 5: return "已失败，已显示";
		default: return "?";
		}
	}

	namespace
	{
		void Walk(const cf::Node& n, QuestState& q)
		{
			if (n.label == "QUEST_FLAGS") {
				q.hasFlags = true;
				q.flags = static_cast<std::uint16_t>(std::strtoul(n.value.c_str(), nullptr, 16));
				return;
			}
			if (n.label.rfind("QUEST_STAGES", 0) == 0) {
				for (const auto& e : n.kids) {
					std::int16_t s = 0;
					bool         done = false;
					for (const auto& f : e.kids) {
						if (f.label == "STAGE") {
							s = static_cast<std::int16_t>(std::atoi(f.value.c_str()));
						} else if (f.label == "STATUS") {
							done = std::strtoul(f.value.c_str(), nullptr, 16) & 1;
						}
					}
					q.stages.push_back({ s, done });
				}
				return;
			}
			if (n.label.rfind("QUEST_OBJECTIVES", 0) == 0) {
				for (const auto& e : n.kids) {
					ObjectiveState o;
					for (const auto& f : e.kids) {
						if (f.label == "UNK1") {
							o.index = std::atoi(f.value.c_str());
						} else if (f.label == "UNK2") {
							o.state = std::atoi(f.value.c_str());
						}
					}
					q.objectives.push_back(o);
				}
				return;
			}
			for (const auto& k : n.kids) {
				Walk(k, q);
			}
		}
	}

	std::vector<JournalEntry> ReadJournal(const SaveFile& a_save, bool* a_found)
	{
		if (a_found) {
			*a_found = false;
		}
		const ChangeForm* player = nullptr;
		for (const auto& c : a_save.changeForms) {
			if (static_cast<ChangeFormType>(c.typeCode()) == ChangeFormType::ACHR && a_save.formIDOf(c.refID) == 0x14) {
				player = &c;
				break;
			}
		}
		if (!player) {
			return {};
		}
		const auto d = cf::Body(*player);
		auto       u32 = [&](std::size_t p) { return static_cast<std::uint32_t>(d[p]) | (d[p + 1] << 8) | (d[p + 2] << 16) | (static_cast<std::uint32_t>(d[p + 3]) << 24); };
		// a vsval count: the low 2 bits say 1, 2 or 3 bytes
		auto vsval = [&](std::size_t p, std::size_t& a_len) -> std::uint32_t {
			const auto sz = d[p] & 3;
			if (sz == 0) {
				a_len = 1;
				return d[p] >> 2;
			}
			if (sz == 1 && p + 1 < d.size()) {
				a_len = 2;
				return (d[p] | (d[p + 1] << 8)) >> 2;
			}
			if (sz == 2 && p + 2 < d.size()) {
				a_len = 3;
				return (d[p] | (d[p + 1] << 8) | (d[p + 2] << 16)) >> 2;
			}
			a_len = 0;
			return 0;
		};
		auto entryOk = [&](std::size_t r) {
			const std::uint32_t ref = (d[r] << 16) | (d[r + 1] << 8) | d[r + 2];
			return ref != 0 && (ref >> 22) != 3 && u32(r + 3) < 1000 && u32(r + 7) <= 0xFFFF && u32(r + 11) <= 5;
		};
		// the longest run of well-formed entries preceded by its own count
		std::size_t bestAt = 0, bestN = 0;
		for (std::size_t p = 0; p + 4 < d.size(); ++p) {
			std::size_t len = 0;
			const auto  n = vsval(p, len);
			if (n == 0 || n > 5000 || n <= bestN || p + len + 15 * static_cast<std::size_t>(n) > d.size()) {
				continue;
			}
			bool good = true;
			for (std::uint32_t k = 0; k < n && good; ++k) {
				good = entryOk(p + len + 15 * k);
			}
			if (good) {
				bestAt = p + len;
				bestN = n;
			}
		}
		std::vector<JournalEntry> out;
		for (std::size_t k = 0; k < bestN; ++k) {
			const auto r = bestAt + 15 * k;
			RefID      id;
			id.data = (d[r] << 16) | (d[r + 1] << 8) | d[r + 2];
			out.push_back({ a_save.formIDOf(id), u32(r + 3), u32(r + 7), u32(r + 11) });
		}
		if (a_found) {
			*a_found = bestN > 0;
		}
		return out;
	}

	std::vector<QuestState> ReadStates(const SaveFile& a_save)
	{
		std::vector<QuestState> out;
		for (const auto& c : a_save.changeForms) {
			if (static_cast<ChangeFormType>(c.typeCode()) != ChangeFormType::QUST) {
				continue;
			}
			QuestState q;
			q.formID = a_save.formIDOf(c.refID);
			const auto d = cf::Decode(a_save, c);
			q.decoded = d.complete;
			Walk(d.root, q);
			out.push_back(std::move(q));
		}
		bool       found = false;
		const auto journal = ReadJournal(a_save, &found);
		if (found) {
			std::unordered_map<std::uint32_t, std::size_t> at;
			for (std::size_t i = 0; i < out.size(); ++i) {
				out[i].journalKnown = true;
				at[out[i].formID] = i;
			}
			for (const auto& e : journal) {
				auto it = at.find(e.quest);
				if (it == at.end()) {
					QuestState q;
					q.formID = e.quest;
					q.journalKnown = true;
					it = at.emplace(e.quest, out.size()).first;
					out.push_back(std::move(q));
				}
				out[it->second].journal.push_back({ static_cast<std::int32_t>(e.index), static_cast<std::int32_t>(e.state) });
			}
		}
		return out;
	}
}
