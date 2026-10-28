// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// 本地化修改：界面、提示与报告文字改为简体中文（详见 README.zh-CN.md）。
#include "PluginDB.h"
#include "Text.h"

#include "Bsa.h"
#include "ByteIO.h"
#include "Save.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace ess
{
	namespace
	{
		std::string Lower(std::string s)
		{
			for (auto& c : s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return s;
		}

		std::string U8(const std::filesystem::path& p)
		{
			const auto s = p.u8string();
			return std::string(s.begin(), s.end());
		}

		bool ReadFile(const std::filesystem::path& p, std::vector<std::uint8_t>& out)
		{
			std::ifstream f(p, std::ios::binary | std::ios::ate);
			if (!f) {
				return false;
			}
			const auto size = static_cast<std::size_t>(f.tellg());
			out.resize(size);
			f.seekg(0);
			return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size)));
		}

		bool Is(const std::uint8_t* t, const char* s) { return std::memcmp(t, s, 4) == 0; }

		bool IsPlacedRef(const std::uint8_t* t)
		{
			static const char* refs[] = { "REFR", "ACHR", "PGRE", "PMIS", "PHZD", "PARW", "PBAR", "PBEA", "PCON", "PFLA" };
			for (const char* r : refs) {
				if (Is(t, r)) {
					return true;
				}
			}
			return false;
		}

		bool Skip(const std::uint8_t* t) { return Is(t, "LAND") || Is(t, "NAVM") || Is(t, "NAVI"); }

		std::uint32_t U32(const std::uint8_t* p)
		{
			std::uint32_t v;
			std::memcpy(&v, p, 4);
			return v;
		}

		// a .strings table: id -> text
		std::unordered_map<std::uint32_t, std::string> ReadStrings(const std::vector<std::uint8_t>& d)
		{
			std::unordered_map<std::uint32_t, std::string> out;
			if (d.size() < 8) {
				return out;
			}
			const auto count = U32(d.data()), size = U32(d.data() + 4);
			const std::size_t dataStart = 8 + std::size_t(count) * 8;
			if (dataStart > d.size()) {
				return out;
			}
			out.reserve(count);
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto id = U32(d.data() + 8 + i * 8), off = U32(d.data() + 12 + i * 8);
				std::size_t p = dataStart + off;
				if (off >= size || p >= d.size()) {
					continue;
				}
				const auto* s = reinterpret_cast<const char*>(d.data() + p);
				const auto  n = strnlen(s, d.size() - p);
				out.emplace(id, std::string(s, n));
			}
			return out;
		}
	}

	namespace
	{
		// a .dlstrings / .ilstrings table: id -> text, each entry stored as u32 length + text
		std::unordered_map<std::uint32_t, std::string> ReadLengthStrings(const std::vector<std::uint8_t>& d)
		{
			std::unordered_map<std::uint32_t, std::string> out;
			if (d.size() < 8) {
				return out;
			}
			const auto        count = U32(d.data()), size = U32(d.data() + 4);
			const std::size_t dataStart = 8 + std::size_t(count) * 8;
			if (dataStart > d.size()) {
				return out;
			}
			out.reserve(count);
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto  id = U32(d.data() + 8 + i * 8), off = U32(d.data() + 12 + i * 8);
				std::size_t p = dataStart + off;
				if (off >= size || p + 4 > d.size()) {
					continue;
				}
				const auto len = U32(d.data() + p);
				p += 4;
				if (p + len > d.size()) {
					continue;
				}
				const auto* s = reinterpret_cast<const char*>(d.data() + p);
				out.emplace(id, std::string(s, strnlen(s, len)));
			}
			return out;
		}
	}

	std::string GameTextToUtf8(const std::string& a_text) { return text::ToUtf8(a_text); }


	void PrepareInput(const SaveFile& a_save, PluginDB::Input& a_in)
	{
		a_in.full = a_save.plugins.full;
		a_in.lite = a_save.plugins.lite;
		a_in.wantedRefs.clear();
		for (auto id : a_save.formIDs) {
			a_in.wantedRefs.insert(id);
		}
		for (const auto& cf : a_save.changeForms) {
			if (const auto id = a_save.formIDOf(cf.refID)) {
				a_in.wantedRefs.insert(id);
			}
		}
		if (const auto* p = a_save.papyrus()) {
			for (const auto& i : p->instances) {
				if (const auto id = a_save.formIDOf(i.refID)) {
					a_in.wantedRefs.insert(id);
				}
			}
		}
	}

	std::uint32_t PluginDB::addString(const std::string& s)
	{
		const auto off = static_cast<std::uint32_t>(_pool.size());
		_pool.insert(_pool.end(), s.begin(), s.end());
		_pool.push_back('\0');
		return off;
	}

	void PluginDB::load(const Input& a_in, Progress& a_progress)
	{
		_forms.clear();
		_quests.clear();
		_pool.clear();
		_plugins.clear();
		missing.clear();
		errors.clear();
		pluginsRead = recordsRead = 0;

		// runtime base of every plugin: full -> index << 24, light -> 0xFE000000 | index << 12
		struct Slot
		{
			bool          light;
			std::uint32_t index;
		};
		std::unordered_map<std::string, Slot> slots;
		std::vector<std::pair<std::string, Slot>> order;
		for (std::size_t i = 0; i < a_in.full.size(); ++i) {
			order.push_back({ a_in.full[i], { false, static_cast<std::uint32_t>(i) } });
		}
		for (std::size_t i = 0; i < a_in.lite.size(); ++i) {
			order.push_back({ a_in.lite[i], { true, static_cast<std::uint32_t>(i) } });
		}
		for (const auto& [n, s] : order) {
			slots.emplace(Lower(n), s);
		}
		auto runtime = [](const Slot& s, std::uint32_t low) -> std::uint32_t {
			return s.light ? (0xFE000000u | (s.index << 12) | (low & 0xFFF)) : ((s.index << 24) | (low & 0xFFFFFF));
		};

		a_progress.total = order.size();
		_plugins.reserve(order.size());
		std::vector<std::uint8_t> file, inflated;
		for (std::size_t pi = 0; pi < order.size(); ++pi) {
			if (a_progress.cancel) {
				break;
			}
			const auto& [pluginName, self] = order[pi];
			_plugins.push_back(pluginName);
			{
				std::lock_guard l(a_progress.m);
				a_progress.current = pluginName;
			}
			const auto path = a_in.sources.pluginPath(pluginName);
			if (!path || !ReadFile(*path, file) || file.size() < 24 || !Is(file.data(), "TES4")) {
				missing.push_back(pluginName);
				++a_progress.done;
				continue;
			}
			try {
				// ---- TES4 header: flags and masters
				const auto hdrFlags = U32(file.data() + 8);
				const bool localized = hdrFlags & 0x80;
				const auto tes4Size = U32(file.data() + 4);
				std::vector<const Slot*> masters;
				{
					Reader r(file.data() + 24, std::min<std::size_t>(tes4Size, file.size() - 24));
					std::uint32_t big = 0;
					while (r.remaining() >= 6) {
						auto t = r.here();
						r.seek(r.pos() + 4);
						std::uint32_t len = r.u16();
						if (big) {
							len = big;
							big = 0;
						}
						r.need(len);
						if (Is(t, "XXXX") && len == 4) {
							big = U32(r.here());
						} else if (Is(t, "MAST")) {
							std::string m(reinterpret_cast<const char*>(r.here()), len);
							while (!m.empty() && m.back() == '\0') {
								m.pop_back();
							}
							auto it = slots.find(Lower(m));
							masters.push_back(it == slots.end() ? nullptr : &it->second);
						}
						r.seek(r.pos() + len);
					}
				}
				// ---- localized names (.strings); journal text comes from .dlstrings, read only when needed
				const auto stem = Lower(U8(text::PathFrom(pluginName).stem()));
				auto loadStrings = [&](const char* a_ext) {
					std::unordered_map<std::uint32_t, std::string> table;
					for (const auto& lang : { Lower(a_in.language), std::string("english") }) {
						auto it = a_in.sources.stringsFiles.find(stem + "_" + lang + a_ext);
						if (it == a_in.sources.stringsFiles.end()) {
							continue;
						}
						try {
							std::vector<std::uint8_t> data;
							if (it->second.bsaEntry.empty()) {
								ReadFile(it->second.file, data);
							} else {
								BsaArchive bsa(it->second.file);
								if (const auto* e = bsa.find(it->second.bsaEntry)) {
									data = bsa.extract(*e);
								}
							}
							// .dlstrings / .ilstrings entries carry a 4-byte length before the text
							if (std::strcmp(a_ext, ".strings") != 0) {
								table = ReadLengthStrings(data);
							} else {
								table = ReadStrings(data);
							}
						} catch (const std::exception& e) {
							errors.push_back(pluginName + " " + a_ext + "： " + e.what());
						}
						if (!table.empty()) {
							break;
						}
					}
					return table;
				};
				std::unordered_map<std::uint32_t, std::string> strings;
				if (localized) {
					strings = loadStrings(".strings");
				}
				// quests of this plugin whose journal text is a .dlstrings id: (quest, stage index, log index, id)
				struct PendingText
				{
					std::uint32_t quest;
					std::size_t   stage, log;
					std::uint32_t id;
				};
				std::vector<PendingText> pendingLogs;
				struct PendingObj
				{
					std::uint32_t quest;
					std::size_t   obj;
					std::uint32_t id;
				};
				std::vector<PendingObj> pendingObjs;

				auto toRuntime = [&](std::uint32_t raw, std::uint32_t& out) {
					const auto m = raw >> 24;
					const Slot* s = m < masters.size() ? masters[m] : &self;
					if (!s) {
						return false;
					}
					out = runtime(*s, raw);
					return true;
				};

				// ---- records
				std::size_t pos = 24 + tes4Size;
				const auto  plugIdx = static_cast<std::uint16_t>(_plugins.size() - 1);
				while (pos + 24 <= file.size()) {
					const auto* h = file.data() + pos;
					if (Is(h, "GRUP")) {
						pos += 24;  // descend into the group
						continue;
					}
					const auto dataSize = U32(h + 4), flags = U32(h + 8), rawID = U32(h + 12);
					const auto next = pos + 24 + dataSize;
					if (next > file.size()) {
						break;
					}
					pos = next;
					if (Skip(h) || (flags & 0x20)) {  // skipped type or deleted
						continue;
					}
					std::uint32_t id;
					if (!toRuntime(rawID, id)) {
						continue;
					}
					const bool placed = IsPlacedRef(h);
					if (placed && !a_in.wantedRefs.contains(id)) {
						continue;
					}
					const std::uint8_t* d = h + 24;
					std::size_t         n = dataSize;
					if (flags & 0x40000) {  // compressed
						if (n < 4) {
							continue;
						}
						const auto orig = U32(d);
						try {
							inflated = DecompressZlib(d + 4, n - 4, orig);
						} catch (const std::exception&) {
							continue;
						}
						d = inflated.data();
						n = inflated.size();
					}
					std::string   edid, full;
					std::uint32_t base = 0;
					std::uint32_t big = 0;
					const bool     isQuest = Is(h, "QUST");
					const bool     isSpell = Is(h, "SPEL");
					std::uint8_t   spellType = 0xFF;
					quests::QuestDef qd;
					for (std::size_t p = 0; p + 6 <= n;) {
						const auto* st = d + p;
						std::uint32_t len;
						std::uint16_t l16;
						std::memcpy(&l16, st + 4, 2);
						len = big ? big : l16;
						big = 0;
						p += 6;
						if (p + len > n) {
							break;
						}
						if (Is(st, "XXXX") && len == 4) {
							big = U32(d + p);
						} else if (Is(st, "EDID")) {
							edid.assign(reinterpret_cast<const char*>(d + p), strnlen(reinterpret_cast<const char*>(d + p), len));
						} else if (Is(st, "FULL") && full.empty()) {
							if (localized && len == 4) {
								if (auto it = strings.find(U32(d + p)); it != strings.end()) {
									full = it->second;
								}
							} else {
								full.assign(reinterpret_cast<const char*>(d + p), strnlen(reinterpret_cast<const char*>(d + p), len));
							}
						} else if (isSpell && Is(st, "SPIT") && len >= 12) {
							spellType = static_cast<std::uint8_t>(U32(d + p + 8));
						} else if (placed && Is(st, "NAME") && len == 4) {
							std::uint32_t b;
							if (toRuntime(U32(d + p), b)) {
								base = b;
							}
						} else if (isQuest) {
							auto text = [&](std::uint32_t& a_id) {
								if (localized && len == 4) {
									a_id = U32(d + p);
									return std::string();
								}
								a_id = 0;
								return GameTextToUtf8(std::string(reinterpret_cast<const char*>(d + p), strnlen(reinterpret_cast<const char*>(d + p), len)));
							};
							if (Is(st, "DNAM") && len >= 8) {
								std::memcpy(&qd.flags, d + p, 2);
								qd.priority = d[p + 2];
								qd.type = d[p + 7];
							} else if (Is(st, "INDX") && len >= 3) {
								quests::Stage sg;
								std::memcpy(&sg.index, d + p, 2);
								sg.flags = d[p + 2];
								qd.stages.push_back(std::move(sg));
							} else if (Is(st, "QSDT") && len >= 1 && !qd.stages.empty()) {
								quests::LogEntry le;
								le.flags = d[p];
								qd.stages.back().logs.push_back(std::move(le));
							} else if (Is(st, "CNAM") && !qd.stages.empty() && !qd.stages.back().logs.empty()) {
								std::uint32_t tid = 0;
								qd.stages.back().logs.back().text = text(tid);
								if (tid) {
									pendingLogs.push_back({ 0, qd.stages.size() - 1, qd.stages.back().logs.size() - 1, tid });
								}
							} else if (Is(st, "NAM0") && len == 4 && !qd.stages.empty() && !qd.stages.back().logs.empty()) {
								std::uint32_t nq = 0;
								if (toRuntime(U32(d + p), nq)) {
									qd.stages.back().logs.back().nextQuest = nq;
								}
							} else if (Is(st, "QOBJ") && len >= 2) {
								quests::Objective o;
								std::memcpy(&o.index, d + p, 2);
								qd.objectives.push_back(std::move(o));
							} else if (Is(st, "NNAM") && !qd.objectives.empty()) {
								std::uint32_t tid = 0;
								qd.objectives.back().text = text(tid);
								if (tid) {
									if (auto it = strings.find(tid); it != strings.end()) {
										qd.objectives.back().text = GameTextToUtf8(it->second);
									} else {
										pendingObjs.push_back({ 0, qd.objectives.size() - 1, tid });
									}
								}
							} else if (Is(st, "VMAD")) {
								quests::ParseQuestVMAD(d + p, len, toRuntime, qd);
							}
						}
						p += len;
					}
					auto& f = _forms[id];
					std::memcpy(f.type, h, 4);
					f.plugin = plugIdx;
					if (!edid.empty()) {
						f.edid = addString(GameTextToUtf8(edid));
					}
					if (!full.empty()) {
						f.name = addString(GameTextToUtf8(full));
					}
					if (base) {
						f.base = base;
					}
					if (isSpell) {
						f.spellType = spellType;
					}
					if (isQuest) {
						qd.edid = GameTextToUtf8(edid);
						qd.name = GameTextToUtf8(full);
						qd.plugin = plugIdx;
						for (auto it = pendingLogs.rbegin(); it != pendingLogs.rend() && it->quest == 0; ++it) {
							it->quest = id;
						}
						for (auto it = pendingObjs.rbegin(); it != pendingObjs.rend() && it->quest == 0; ++it) {
							it->quest = id;
						}
						_quests[id] = std::move(qd);
					}
					++recordsRead;
				}
				// journal entries (and objective texts not in .strings) of localized plugins
				if (!pendingLogs.empty() || !pendingObjs.empty()) {
					const auto dl = loadStrings(".dlstrings");
					auto       lookup = [&](std::uint32_t a_id) -> std::string {
                        if (auto it = dl.find(a_id); it != dl.end()) {
                            return GameTextToUtf8(it->second);
                        }
                        if (auto it = strings.find(a_id); it != strings.end()) {
                            return GameTextToUtf8(it->second);
                        }
                        return {};
					};
					for (const auto& pl : pendingLogs) {
						auto q = _quests.find(pl.quest);
						if (q != _quests.end() && pl.stage < q->second.stages.size() && pl.log < q->second.stages[pl.stage].logs.size()) {
							q->second.stages[pl.stage].logs[pl.log].text = lookup(pl.id);
						}
					}
					for (const auto& po : pendingObjs) {
						auto q = _quests.find(po.quest);
						if (q != _quests.end() && po.obj < q->second.objectives.size()) {
							q->second.objectives[po.obj].text = lookup(po.id);
						}
					}
				}
				++pluginsRead;
			} catch (const std::exception& e) {
				errors.push_back(pluginName + "： " + e.what());
			}
			++a_progress.done;
		}
	}

	const PluginDB::Form* PluginDB::find(std::uint32_t a_formID) const
	{
		auto it = _forms.find(a_formID);
		return it == _forms.end() ? nullptr : &it->second;
	}

	std::string PluginDB::describe(std::uint32_t a_formID) const
	{
		const auto* f = find(a_formID);
		if (!f) {
			return {};
		}
		std::string s = edid(*f);
		const auto  n = name(*f);
		if (!n.empty()) {
			s += (s.empty() ? "'" : " '") + n + "'";
		}
		if (f->base) {
			if (const auto* b = find(f->base)) {
				auto bn = name(*b);
				if (bn.empty()) {
					bn = edid(*b);
				}
				if (!bn.empty()) {
					s += (s.empty() ? "" : " ") + std::string("-> ") + bn;
				}
			}
		}
		s += (s.empty() ? "[" : " [") + type(*f) + "]";
		return s;
	}

	std::vector<std::uint32_t> PluginDB::search(const std::string& a_text, std::size_t a_max) const
	{
		std::vector<std::uint32_t> out;
		if (a_text.empty()) {
			return out;
		}
		const auto needle = Lower(a_text);
		// a hex form id?
		char*       end = nullptr;
		const auto  hex = std::strtoul(a_text.c_str(), &end, 16);
		if (end && *end == '\0' && find(static_cast<std::uint32_t>(hex))) {
			out.push_back(static_cast<std::uint32_t>(hex));
		}
		auto contains = [&](std::uint32_t off) {
			if (off == UINT32_MAX) {
				return false;
			}
			const char* s = _pool.data() + off;
			const auto  n = std::strlen(s);
			if (n < needle.size()) {
				return false;
			}
			for (std::size_t i = 0; i + needle.size() <= n; ++i) {
				std::size_t k = 0;
				while (k < needle.size() && std::tolower(static_cast<unsigned char>(s[i + k])) == needle[k]) {
					++k;
				}
				if (k == needle.size()) {
					return true;
				}
			}
			return false;
		};
		for (const auto& [id, f] : _forms) {
			if (out.size() >= a_max) {
				break;
			}
			if (contains(f.edid) || contains(f.name)) {
				out.push_back(id);
			}
		}
		std::sort(out.begin(), out.end());
		return out;
	}
}
