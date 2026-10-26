// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Transplant.h"

#include "ChangeFormData.h"
#include "Quests.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>

namespace ess::transplant
{
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

		// one line of a snapshot: tab-separated, \t \n \\ escaped (as in ReSaverHelper's Protocol.h)
		Record SplitLine(const std::string& a_line)
		{
			Record out(1);
			for (std::size_t i = 0; i < a_line.size(); ++i) {
				const char c = a_line[i];
				if (c == '\t') {
					out.emplace_back();
				} else if (c == '\\' && i + 1 < a_line.size()) {
					const char n = a_line[++i];
					out.back() += n == 't' ? '\t' : n == 'n' ? '\n' : n;
				} else if (c != '\r') {
					out.back() += c;
				}
			}
			return out;
		}

		std::string At(const Record& r, std::size_t i) { return i < r.size() ? r[i] : std::string(); }
		std::uint32_t Hex(const std::string& s) { return static_cast<std::uint32_t>(std::strtoul(s.c_str(), nullptr, 16)); }
		float         Num(const std::string& s) { return std::strtof(s.c_str(), nullptr); }
		int           Int(const std::string& s) { return static_cast<int>(std::strtol(s.c_str(), nullptr, 10)); }
		FormKey       Key(const std::string& a_plugin, const std::string& a_local) { return { Lower(a_plugin), Hex(a_local) }; }

		std::string RoundedSignature(const Record& r)
		{
			// custom name, temper (rounded), charge (rounded), enchantment, poison
			auto round = [](const std::string& v) { return v.empty() ? std::string() : Fmt("%.2f", Num(v)); };
			return At(r, 6) + "|" + round(At(r, 7)) + "|" + (At(r, 8).empty() ? "" : "c") + "|" + At(r, 9) + "|" + At(r, 10);
		}

		const char* kSkillNames[18] = { "One-handed", "Two-handed", "Archery", "Block", "Smithing", "Heavy Armor", "Light Armor", "Pickpocket", "Lockpicking",
			"Sneak", "Alchemy", "Speech", "Alteration", "Conjuration", "Destruction", "Illusion", "Restoration", "Enchanting" };
	}

	bool Quest::shown() const
	{
		for (std::size_t p = 0; p < objectives.size();) {
			const auto colon = objectives.find(':', p);
			if (colon == std::string::npos) {
				break;
			}
			const int st = std::atoi(objectives.c_str() + colon + 1);
			if (st & 1) {
				return true;
			}
			const auto comma = objectives.find(',', colon);
			if (comma == std::string::npos) {
				break;
			}
			p = comma + 1;
		}
		return false;
	}

	const char* SkillName(int a_index) { return a_index >= 0 && a_index < 18 ? kSkillNames[a_index] : "?"; }

	const char* CategoryName(Category a_cat)
	{
		switch (a_cat) {
		case Category::kAppearance: return "YOUR CHARACTER'S LOOK: face, hair, body (RaceMenu)";
		case Category::kCharacter: return "Character: level, skills, attributes";
		case Category::kPerks: return "Perks";
		case Category::kSpells: return "Spells and powers";
		case Category::kShouts: return "Shouts and words of power";
		case Category::kMarkers: return "Map locations found";
		case Category::kItems: return "Items and gold";
		case Category::kMade: return "Things you made: enchanted gear, potions, poisons";
		case Category::kLook: return "Worn and held gear";
		case Category::kFactions: return "Factions";
		case Category::kQuests: return "Quests";
		case Category::kFollowers: return "YOUR FOLLOWERS: tick the ones to bring with you";
		default: return "?";
		}
	}

	// ---- reading -----------------------------------------------------------------------------------

	Snapshot ReadSnapshot(const std::filesystem::path& a_file)
	{
		Snapshot s;
		s.source = a_file.filename().string();
		std::ifstream f(a_file, std::ios::binary);
		if (!f) {
			s.error = "cannot open " + a_file.string();
			return s;
		}
		std::string line;
		std::map<FormKey, std::string> jobj;  // journal objectives per quest
		if (!std::getline(f, line) || At(SplitLine(line), 0) != "RSH-SNAPSHOT") {
			s.error = a_file.filename().string() + " is not a ReSaverHelper snapshot";
			return s;
		}
		{
			const auto h = SplitLine(line);
			s.game = At(h, 2);
			s.player = At(h, 3);
		}
		while (std::getline(f, line)) {
			const auto r = SplitLine(line);
			const auto& k = r[0];
			if (k == "plugin") {
				s.plugins.push_back(At(r, 1));
			} else if (k == "player") {
				s.level = Int(At(r, 1));
				s.xp = Num(At(r, 2));
				s.threshold = Num(At(r, 3));
				s.perkPoints = Int(At(r, 4));
				s.playerRecord = r;
			} else if (k == "skill") {
				const int i = Int(At(r, 1));
				if (i >= 0 && i < 18) {
					s.skills[i] = { Num(At(r, 2)), r };
				}
			} else if (k == "av") {
				s.avs[At(r, 1)] = { Num(At(r, 2)), r };
			} else if (k == "perk" || k == "spell" || k == "shout" || k == "word" || k == "faction" || k == "marker") {
				Entry e;
				e.record = r;
				e.name = At(r, 4);
				e.a = Int(At(r, 5));
				e.b = Int(At(r, 6));
				auto& m = k == "perk" ? s.perks : k == "spell" ? s.spells : k == "shout" ? s.shouts : k == "word" ? s.words : k == "faction" ? s.factions : s.markers;
				m[Key(At(r, 1), At(r, 2))] = std::move(e);
			} else if (k == "quest") {
				Quest q;
				q.record = r;
				q.plugin = At(r, 1);
				q.local = Hex(At(r, 2));
				q.runtime = Hex(At(r, 3));
				const auto names = At(r, 4);
				const auto bar = names.find('|');
				q.edid = names.substr(0, bar);
				q.name = bar == std::string::npos ? std::string() : names.substr(bar + 1);
				q.running = At(r, 5) == "1";
				q.completed = At(r, 6) == "1";
				q.stage = Int(At(r, 7));
				for (std::size_t p = 0, e; p < At(r, 8).size(); p = e + 1) {
					const auto t = At(r, 8);
					e = t.find(',', p);
					if (e == std::string::npos) {
						e = t.size();
					}
					q.done.push_back(Int(t.substr(p, e - p)));
				}
				q.known = r.size() > 10;
				q.objectives = At(r, 9);
				q.type = q.known ? Int(At(r, 10)) : -1;
				auto& slot = s.quests[Key(q.plugin, At(r, 2))];
				q.inJournal = slot.inJournal;
				slot = std::move(q);
			} else if (k == "jobj") {
				// an objective the player's journal lists: <plugin> <local> <runtime> <index> <state>. The journal is
				// the truth (a radiant quest done and restarted since keeps its old entry there only).
				s.journalKnown = true;
				auto& q = s.quests[Key(At(r, 1), At(r, 2))];
				if (q.plugin.empty()) {
					q.plugin = At(r, 1);
					q.local = Hex(At(r, 2));
					q.runtime = Hex(At(r, 3));
				}
				// only objectives the journal shows count (state 1, 3 or 5); 0, 2 and 4 are hidden entries
				q.inJournal = q.inJournal || (Int(At(r, 5)) & 1) != 0;
				jobj[Key(At(r, 1), At(r, 2))] += (jobj[Key(At(r, 1), At(r, 2))].empty() ? "" : ",") + At(r, 4) + ":" + At(r, 5);
			} else if (k == "item") {
				Item it;
				it.record = r;
				it.key = Key(At(r, 1), At(r, 2));
				it.name = At(r, 4);
				it.count = Int(At(r, 5));
				it.special = !(At(r, 6).empty() && At(r, 7).empty() && At(r, 8).empty() && At(r, 9).empty() && At(r, 10).empty());
				it.signature = it.special ? RoundedSignature(r) : std::string();
				it.questItem = At(r, 11) == "1";
				it.madeEnchantment = At(r, 9).rfind("C:", 0) == 0 || At(r, 10).rfind("C:", 0) == 0;
				s.items.push_back(std::move(it));
			} else if (k == "equip" || k == "equipspell" || k == "equipshout") {
				Entry e;
				e.record = r;
				e.name = !At(r, 6).empty() ? At(r, 6) : At(r, 4);
				s.equips.push_back(std::move(e));
			} else if (k == "follower") {
				Entry e;
				e.record = r;
				e.name = At(r, 4);
				e.a = At(r, 5) == "1";
				e.b = Int(At(r, 6));
				s.followers.push_back(std::move(e));
			} else if (k == "look") {
				s.look = At(r, 1);
			} else if (k == "potion") {
				Potion p;
				p.record = r;
				p.name = At(r, 1);
				p.count = Int(At(r, 2));
				p.poison = At(r, 3) == "1";
				p.effects = At(r, 4);
				s.potions.push_back(std::move(p));
			}
		}
		// the journal's objective states win over the quest's own (they are what the journal shows)
		for (const auto& [key, objs] : jobj) {
			auto& q = s.quests[key];
			q.objectives = objs;
			q.known = true;
		}
		s.valid = true;
		return s;
	}

	// A save file read without the game: what the save itself shows. Skills, perks, words of power,
	// map markers and things the player made are kept in parts of the save ReSaver++ does not decode.
	Snapshot FromSave(const SaveFile& a_save, const std::string& a_name)
	{
		Snapshot s;
		s.fromFile = true;
		s.source = a_name;
		s.player = a_save.header.playerName;
		s.missingParts = { "skills and experience", "perks", "words of power", "map locations", "enchanted gear, potions and poisons you made",
			"custom names and tempering of items" };
		auto local = [&](std::uint32_t id) { return (id >> 24) == 0xFE ? (id & 0xFFF) : (id & 0xFFFFFF); };
		auto put = [&](Record& r, std::uint32_t id) {
			r.push_back(a_save.pluginOf(id));
			r.push_back(Fmt("%06X", local(id)));
			r.push_back(Fmt("%08X", id));
		};
		auto keyOf = [&](std::uint32_t id) { return FormKey{ Lower(a_save.pluginOf(id)), local(id) }; };
		auto dynamic = [](std::uint32_t id) { return (id >> 24) == 0xFF || id == 0; };

		for (const auto& c : a_save.changeForms) {
			const auto id = a_save.formIDOf(c.refID);
			const auto t = static_cast<ChangeFormType>(c.typeCode());
			if (t == ChangeFormType::NPC_ && id == 0x7) {
				// the player's base form: level, spells, shouts, factions
				const auto d = cf::Decode(a_save, c);
				auto walk = [&](auto&& self, const cf::Node& n) -> void {
					if (n.label == "ACBS" && !n.value.empty()) {
						// hex bytes; the level is the int16 at offset 8
						std::vector<int> bytes;
						for (std::size_t i = 0; i + 1 < n.value.size(); ++i) {
							if (std::isxdigit(static_cast<unsigned char>(n.value[i])) && std::isxdigit(static_cast<unsigned char>(n.value[i + 1]))) {
								bytes.push_back(static_cast<int>(std::strtol(n.value.substr(i, 2).c_str(), nullptr, 16)));
								++i;
							}
						}
						if (bytes.size() >= 10) {
							s.level = bytes[8] | (bytes[9] << 8);
						}
					}
					if (n.isRef && n.formID && !dynamic(n.formID)) {
						const bool spell = n.label == "SPELL", shout = n.label == "SHOUT", faction = n.label == "FACTION";
						if (spell || shout) {
							Entry e;
							e.record = { spell ? "spell" : "shout" };
							put(e.record, n.formID);
							e.record.push_back("");
							(spell ? s.spells : s.shouts)[keyOf(n.formID)] = std::move(e);
						}
						(void)faction;
					}
					for (const auto& k : n.kids) {
						self(self, k);
					}
				};
				walk(walk, d.root);
			} else if (t == ChangeFormType::ACHR && id == 0x14) {
				// the player's inventory
				const auto d = cf::Decode(a_save, c);
				auto walk = [&](auto&& self, const cf::Node& n) -> void {
					if (n.label.rfind("INVENTORY", 0) == 0) {
						for (const auto& el : n.kids) {
							std::uint32_t item = 0;
							int           count = 0;
							for (const auto& f : el.kids) {
								if (f.label == "ITEM") {
									item = f.formID;
								} else if (f.label == "COUNT") {
									count = Int(f.value);
								}
							}
							if (item && !dynamic(item) && count > 0) {
								Item it;
								it.record = { "item" };
								put(it.record, item);
								it.record.push_back("");
								it.record.push_back(std::to_string(count));
								it.record.insert(it.record.end(), { "", "", "", "", "", "0" });
								it.key = keyOf(item);
								it.count = count;
								s.items.push_back(std::move(it));
							}
						}
						return;
					}
					for (const auto& k : n.kids) {
						self(self, k);
					}
				};
				walk(walk, d.root);
			}
		}
		// the inventory in the save holds changes against the base container: with no game, counts are what
		// the save lists (usually everything the player picked up)
		for (const auto& q : quests::ReadStates(a_save)) {
			if (!q.hasFlags || dynamic(q.formID)) {
				continue;
			}
			Quest x;
			x.plugin = a_save.pluginOf(q.formID);
			x.local = local(q.formID);
			x.runtime = q.formID;
			x.running = q.running();
			x.completed = q.completed();
			x.stage = std::max(0, q.highestDoneStage());
			if (q.journalKnown) {
				// the journal as the save lists it: which quests it shows, and how
				s.journalKnown = true;
				x.inJournal = q.inJournal();
				for (const auto& o : q.journal) {
					x.objectives += (x.objectives.empty() ? "" : ",") + std::to_string(o.index) + ":" + std::to_string(o.state);
				}
				x.known = !x.objectives.empty();
			}
			for (const auto& [st, d] : q.stages) {
				if (d) {
					x.done.push_back(st);
				}
			}
			x.record = { "quest" };
			put(x.record, q.formID);
			s.quests[keyOf(q.formID)] = std::move(x);
		}
		if (s.level > 0) {
			s.playerRecord = { "player", std::to_string(s.level), "", "", "" };  // the level only: no experience or perk points from a file
		}
		s.valid = true;
		return s;
	}

	// ---- comparing ---------------------------------------------------------------------------------

	std::vector<Change> Diff(const Snapshot& a_good, const Snapshot& a_bad)
	{
		std::vector<Change> v;
		std::set<std::string> loaded;
		for (const auto& p : a_good.plugins) {
			loaded.insert(Lower(p));
		}
		auto has = [&](const std::string& a_plugin) { return loaded.empty() || loaded.contains(Lower(a_plugin)); };
		auto add = [&](Category c, std::string label, std::string detail, int risk, std::string why, Record rec, const std::string& plugin) {
			Change ch;
			ch.cat = c;
			ch.label = std::move(label);
			ch.detail = std::move(detail);
			ch.risk = risk;
			ch.why = std::move(why);
			ch.record = std::move(rec);
			ch.available = plugin.empty() || has(plugin);
			if (!ch.available) {
				ch.why = plugin + " is not in the game's load order now, so this cannot be brought over.";
				ch.risk = 2;
				ch.tick = false;
			}
			v.push_back(std::move(ch));
			return v.size() - 1;  // an index: later additions move the vector
		};

		// character
		if (a_bad.level > a_good.level && !a_bad.playerRecord.empty()) {
			add(Category::kCharacter, Fmt("Level %d (now %d)", a_bad.level, a_good.level), a_bad.perkPoints >= 0 ? Fmt("with its experience and %d unspent perk point(s)", a_bad.perkPoints) : std::string(),
				0, "Raises the level; the game's own level-up rules do not run, so health/magicka/stamina come from the attributes below.", a_bad.playerRecord, "");
		}
		for (int i = 0; i < 18; ++i) {
			const auto& b = a_bad.skills[i];
			if (b.level > a_good.skills[i].level + 0.01f && !b.record.empty()) {
				add(Category::kCharacter, Fmt("%s %d (now %d)", SkillName(i), static_cast<int>(b.level), static_cast<int>(std::max(0.0f, a_good.skills[i].level))), "",
					0, "Sets the skill's level and progress.", b.record, "");
			}
		}
		for (const auto& [name, vr] : a_bad.avs) {
			const auto it = a_good.avs.find(name);
			const float g = it == a_good.avs.end() ? 0.0f : it->second.first;
			if (vr.first > g + 0.01f) {
				add(Category::kCharacter, Fmt("%s %d (now %d)", name == "CarryWeight" ? "Carry weight" : name == "DragonSouls" ? "Dragon souls" : name.c_str(),
							  static_cast<int>(vr.first), static_cast<int>(g)),
					"", 0, "Sets the base value.", vr.second, "");
			}
		}

		// perks, spells, shouts, words, markers, factions: what the bad save has and the good one does not
		auto entries = [&](const std::map<FormKey, Entry>& bad, const std::map<FormKey, Entry>& good, Category c, int risk, const char* why) {
			for (const auto& [k, e] : bad) {
				const auto g = good.find(k);
				bool       missing = g == good.end();
				std::string extra;
				if (c == Category::kShouts && At(e.record, 0) == "word") {
					missing = g == good.end() || (e.a && !g->second.a) || (e.b && !g->second.b);
					extra = e.b ? "learned and unlocked" : "learned";
				}
				if (c == Category::kFactions && !missing) {
					missing = e.a > g->second.a;
				}
				if (c == Category::kFactions) {
					extra = Fmt("rank %d", e.a);
				}
				if (missing) {
					add(c, e.name.empty() ? At(e.record, 2) + " (" + At(e.record, 1) + ")" : e.name, extra.empty() ? At(e.record, 1) : extra + "  -  " + At(e.record, 1), risk,
						why, e.record, At(e.record, 1));
				}
			}
		};
		entries(a_bad.perks, a_good.perks, Category::kPerks, 0, "Adds the perk.");
		entries(a_bad.spells, a_good.spells, Category::kSpells, 0, "Adds the spell or power.");
		entries(a_bad.shouts, a_good.shouts, Category::kShouts, 0, "Adds the shout.");
		entries(a_bad.words, a_good.words, Category::kShouts, 0, "Teaches the word (and unlocks it if it was unlocked).");
		entries(a_bad.markers, a_good.markers, Category::kMarkers, 0, "Shows the location on the map (and lets you fast-travel there if you could).");
		entries(a_bad.factions, a_good.factions, Category::kFactions, 1,
			"Joins the faction. Quests that make you a member (guilds, Companions...) may not expect it: transplant their quests too.");

		// items: plain stacks by count, special items (named, tempered, enchanted, poisoned) one by one
		{
			std::map<FormKey, int> goodPlain;
			std::map<std::string, int> goodSpecial;
			for (const auto& it : a_good.items) {
				if (it.special) {
					goodSpecial[it.key.plugin + Fmt(":%X|", it.key.local) + it.signature] += it.count;
				} else {
					goodPlain[it.key] += it.count;
				}
			}
			std::map<FormKey, std::pair<int, const Item*>> badPlain;
			for (const auto& it : a_bad.items) {
				if (!it.special) {
					auto& b = badPlain[it.key];
					b.first += it.count;
					b.second = &it;
				}
			}
			for (const auto& [k, b] : badPlain) {
				const int more = b.first - goodPlain[k];
				if (more <= 0) {
					continue;
				}
				auto rec = b.second->record;
				if (rec.size() > 5) {
					rec[5] = std::to_string(more);
				}
				const bool gold = k.plugin == "skyrim.esm" && k.local == 0xF;
				const auto name = b.second->name.empty() ? At(rec, 2) : b.second->name;
				add(Category::kItems, gold ? Fmt("%d gold", more) : Fmt("%s x%d", name.c_str(), more), At(rec, 1), b.second->questItem ? 1 : 0,
					b.second->questItem ? "A quest item: its quest may not expect a second one. Bring it only if that quest is brought over too."
										: "Adds the item(s) to your inventory.",
					rec, At(rec, 1));
			}
			for (const auto& it : a_bad.items) {
				if (!it.special) {
					continue;
				}
				auto& left = goodSpecial[it.key.plugin + Fmt(":%X|", it.key.local) + it.signature];
				const int more = it.count - left;
				left = std::max(0, left - it.count);
				if (more <= 0) {
					continue;
				}
				auto rec = it.record;
				rec[5] = std::to_string(more);
				std::string what;
				if (!At(rec, 6).empty()) {
					what += "named \"" + At(rec, 6) + "\", ";
				}
				if (!At(rec, 7).empty()) {
					what += "tempered, ";
				}
				if (!At(rec, 9).empty()) {
					what += it.madeEnchantment ? "your own enchantment, " : "enchanted, ";
				}
				if (!At(rec, 10).empty()) {
					what += "poisoned, ";
				}
				if (what.size() > 2) {
					what.resize(what.size() - 2);
				}
				const auto name = !At(rec, 6).empty() ? At(rec, 6) : it.name;
				add(it.madeEnchantment ? Category::kMade : Category::kItems, Fmt("%s x%d", name.c_str(), more), what + "  -  " + At(rec, 1), it.questItem ? 1 : 0,
					it.madeEnchantment ? "Made again from the same magic effects and strengths, then given to you." : "Adds the item with its name, temper and magic.",
					rec, At(rec, 1));
			}
		}
		// potions and poisons the player made
		{
			std::map<std::string, int> good;
			for (const auto& p : a_good.potions) {
				good[p.effects + (p.poison ? "|p" : "")] += p.count;
			}
			for (const auto& p : a_bad.potions) {
				auto&     left = good[p.effects + (p.poison ? "|p" : "")];
				const int more = p.count - left;
				left = std::max(0, left - p.count);
				if (more > 0) {
					auto rec = p.record;
					rec[2] = std::to_string(more);
					add(Category::kMade, Fmt("%s x%d", p.name.c_str(), more), p.poison ? "a poison you made" : "a potion you made", 0,
						"Made again from the same magic effects and strengths, then given to you.", rec, "");
				}
			}
		}

		// the look (RaceMenu preset) and what the character wore and held
		if (!a_bad.look.empty()) {
			add(Category::kAppearance, "Your look from the bad save (face, hair, body, overlays)", "saved by RaceMenu as preset " + a_bad.look, 1,
				"RaceMenu loads the preset it saved from the bad save onto your character: face, hair, body sliders, overlays. It replaces "
				"the good save's look. Your race is not changed.",
				{ "look", a_bad.look }, "");
		}
		{
			std::set<std::string> worn;
			for (const auto& e : a_good.equips) {
				worn.insert(Lower(At(e.record, 1)) + Fmt(":%X:", Hex(At(e.record, 2))) + At(e.record, 5));
			}
			for (const auto& e : a_bad.equips) {
				if (worn.contains(Lower(At(e.record, 1)) + Fmt(":%X:", Hex(At(e.record, 2))) + At(e.record, 5))) {
					continue;
				}
				const auto& kind = At(e.record, 0);
				const auto  slot = At(e.record, 5);
				const auto  label = kind == "equip"      ? "Wear / hold: " + e.name + (slot == "left" ? " (left hand)" : "")
									: kind == "equipshout" ? "Shout selected: " + e.name
														   : (slot == "voice" ? "Power selected: " : "Spell in " + slot + " hand: ") + e.name;
				add(Category::kLook, label, At(e.record, 1), 0, "Equips it after everything else is brought over (it has to be in your inventory / known by then).",
					e.record, At(e.record, 1));
			}
		}

		// followers: brought back to you, back in their follower mod's quest, with what they carried
		{
			std::set<std::string> following;
			for (const auto& f : a_good.followers) {
				following.insert(Lower(At(f.record, 1)) + Fmt(":%X", Hex(At(f.record, 2))));
			}
			for (const auto& f : a_bad.followers) {
				const bool already = following.contains(Lower(At(f.record, 1)) + Fmt(":%X", Hex(At(f.record, 2))));
				const auto name = f.name.empty() ? At(f.record, 2) : f.name;
				add(Category::kFollowers, already ? name + " - already following here: give back what they carried" : name + " - bring back as your follower",
					At(f.record, 1), already ? 0 : 1,
					already ? "Gives them the items they carried in the bad save that they do not have now. Nothing is taken from them."
							: "Brings them to you (brought back to life and enabled if needed), puts them back in the quest aliases that made them "
							  "follow (their follower mod's own system), makes them your teammate and gives back what they carried. Followers with "
							  "their own mod (Inigo, Serana, Vilja...) may also need their quest brought over (Quests). If one does not follow, talk "
							  "to them and ask them to follow you.",
					f.record, At(f.record, 1));
			}
		}

		// quests: completed in the bad save, or further along there
		for (const auto& [k, q] : a_bad.quests) {
			const auto  g = a_good.quests.find(k);
			const bool  goodDone = g != a_good.quests.end() && g->second.completed;
			const int   goodStage = g != a_good.quests.end() ? g->second.stage : 0;
			const auto  name = !q.name.empty() ? q.name : !q.edid.empty() ? q.edid : Fmt("%06X", q.local);
			// a quest of the journal: the journal lists it (helper 9 snapshots list the journal), else it showed an
			// objective. Snapshots without objectives do not say: then only completed quests count.
			const bool journal = a_bad.journalKnown ? q.inJournal : q.known ? q.shown() : q.completed;
			std::size_t ch = static_cast<std::size_t>(-1);
			if (q.completed && !goodDone) {
				ch = add(Category::kQuests, name + " - complete it", q.edid + "  -  " + q.plugin, journal ? 1 : 2,
					journal ? "Its completion stage runs in the game (rewards and follow-up quests happen normally), then its objectives are shown "
							  "as they were, so it is listed in the journal. If the game refuses the stage, it is marked as done instead "
							  "(when \"Mark refused quests as done\" is ticked)."
							: "A background quest (radiant job, scene, mod setup): the journal never showed it. Bringing it over can start or end "
							  "things you never saw. Leave it unless you know it.",
					{}, q.plugin);
			} else if (!q.completed && q.running && q.stage > goodStage && !goodDone) {
				ch = add(Category::kQuests, name + Fmt(" - stage %d (now %d)", q.stage, goodStage), q.edid + "  -  " + q.plugin, journal ? 1 : 2,
					journal ? "Sets the quest to the stage it had (its script runs) and shows its objectives as they were. If the game refuses "
							  "the stage, it is left as it is (it was not completed in the bad save)."
							: "A background quest (radiant job, scene, mod setup) still running in the bad save: the journal never showed it. "
							  "Leave it unless you know it.",
					{}, q.plugin);
			}
			bool onlyShow = false;
			if (ch == static_cast<std::size_t>(-1) && journal && a_good.journalKnown && !goodDone && !q.objectives.empty() &&
				(g == a_good.quests.end() || !g->second.inJournal)) {
				// in the bad save's journal, not in the good one's, and no stage to move: show it in the journal
				ch = add(Category::kQuests, name + " - show it in the journal", q.edid + "  -  " + q.plugin, 1,
					"The good save is as far in this quest but its journal does not list it: starts it if needed and shows its "
					"objectives as they were.",
					{}, q.plugin);
				onlyShow = true;
			}
			if (ch != static_cast<std::size_t>(-1)) {
				v[ch].journalOnly = onlyShow;
				v[ch].quest = &q;
				v[ch].complete = q.completed;
				v[ch].goodRuntime = g != a_good.quests.end() ? g->second.runtime : 0;
				v[ch].background = !journal;
				v[ch].tick = journal && v[ch].available;
			}
		}
		return v;
	}
}
