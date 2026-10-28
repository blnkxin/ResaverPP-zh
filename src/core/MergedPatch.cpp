// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Merged patch. The approach follows Mator Smash (copy the winning override, then apply each
// override's own changes measured against the version its plugin was built on) and xEdit's
// required-masters rule; record layouts follow xEdit's Skyrim definitions (wbDefinitionsTES5).
#include "MergedPatch.h"

#include "Bsa.h"
#include "Save.h"  // DecompressZlib
#include "ScriptSources.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace ess::merge
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
		std::uint16_t U16(const std::uint8_t* p)
		{
			std::uint16_t v;
			std::memcpy(&v, p, 2);
			return v;
		}
		std::uint32_t U32(const std::uint8_t* p)
		{
			std::uint32_t v;
			std::memcpy(&v, p, 4);
			return v;
		}
		void Put16(std::vector<std::uint8_t>& o, std::uint16_t v)
		{
			o.push_back(static_cast<std::uint8_t>(v));
			o.push_back(static_cast<std::uint8_t>(v >> 8));
		}
		void Put32(std::vector<std::uint8_t>& o, std::uint32_t v)
		{
			for (int i = 0; i < 4; ++i) {
				o.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
			}
		}
		void PutSig(std::vector<std::uint8_t>& o, const char* s) { o.insert(o.end(), s, s + 4); }
		bool Is(const char* a, const char* b) { return std::memcmp(a, b, 4) == 0; }
		std::string Sig(const char* s) { return std::string(s, 4); }
		std::string Hex(std::uint32_t v, int w = 6)
		{
			char b[16];
			std::snprintf(b, sizeof(b), "%0*X", w, v);
			return b;
		}

		std::vector<Sub> ParseSubs(const std::uint8_t* d, std::size_t n)
		{
			std::vector<Sub> out;
			std::uint32_t    big = 0;
			std::size_t      p = 0;
			while (p + 6 <= n) {
				Sub s;
				std::memcpy(s.sig, d + p, 4);
				std::uint32_t len = U16(d + p + 4);
				if (big) {
					len = big;
					big = 0;
				}
				p += 6;
				if (p + len > n) {
					throw std::runtime_error("子记录 " + Sig(s.sig) + " 超出了它所在记录的末尾");
				}
				if (Is(s.sig, "XXXX") && len == 4) {
					big = U32(d + p);
				} else {
					s.data.assign(d + p, d + p + len);
					out.push_back(std::move(s));
				}
				p += len;
			}
			return out;
		}

		void WriteSubs(std::vector<std::uint8_t>& o, const std::vector<Sub>& subs)
		{
			for (const auto& s : subs) {
				if (s.data.size() > 0xFFFF) {
					PutSig(o, "XXXX");
					Put16(o, 4);
					Put32(o, static_cast<std::uint32_t>(s.data.size()));
					PutSig(o, s.sig);
					Put16(o, 0);
				} else {
					PutSig(o, s.sig);
					Put16(o, static_cast<std::uint16_t>(s.data.size()));
				}
				o.insert(o.end(), s.data.begin(), s.data.end());
			}
		}

		// Skyrim's top group order (as the Creation Kit writes it)
		const char* const kGroupOrder[] = { "GMST", "KYWD", "LCRT", "AACT", "TXST", "GLOB", "CLAS", "FACT", "HDPT", "HAIR", "EYES", "RACE", "SOUN",
			"ASPC", "MGEF", "SCPT", "LTEX", "ENCH", "SPEL", "SCRL", "ACTI", "TACT", "ARMO", "BOOK", "CONT", "DOOR", "INGR", "LIGH", "MISC", "APPA",
			"STAT", "SCOL", "MSTT", "PWAT", "GRAS", "TREE", "CLDC", "FLOR", "FURN", "WEAP", "AMMO", "NPC_", "LVLN", "KEYM", "ALCH", "IDLM", "COBJ",
			"PROJ", "HAZD", "SLGM", "LVLI", "WTHR", "CLMT", "SPGD", "RFCT", "REGN", "NAVI", "CELL", "WRLD", "DIAL", "QUST", "IDLE", "PACK", "CSTY",
			"LSCR", "LVSP", "ANIO", "WATR", "EFSH", "EXPL", "DEBR", "IMGS", "IMAD", "FLST", "PERK", "BPTD", "ADDN", "AVIF", "CAMS", "CPTH", "VTYP",
			"MATT", "IPCT", "IPDS", "ARMA", "ECZN", "LCTN", "MESG", "RGDL", "DOBJ", "LGTM", "MUSC", "FSTP", "FSTS", "SMBN", "SMQN", "SMEN", "DLBR",
			"MUST", "DLVW", "WOOP", "SHOU", "EQUP", "RELA", "SCEN", "ASTP", "OTFT", "ARTO", "MATO", "MOVT", "SNDR", "DUAL", "SNCT", "SOPM", "COLL",
			"CLFM", "REVB" };
		int GroupRank(const char* sig)
		{
			for (int i = 0; i < static_cast<int>(std::size(kGroupOrder)); ++i) {
				if (Is(sig, kGroupOrder[i])) {
					return i;
				}
			}
			return 1000;
		}
	}

	// ======================================================================================
	// plugin files
	// ======================================================================================

	PluginFile ReadPlugin(const std::filesystem::path& a_file, const std::vector<std::string>& a_types)
	{
		PluginFile    pf;
		std::ifstream f(a_file, std::ios::binary);
		if (!f) {
			throw std::runtime_error("无法打开文件");
		}
		std::uint8_t h[24];
		if (!f.read(reinterpret_cast<char*>(h), 24) || std::memcmp(h, "TES4", 4) != 0) {
			throw std::runtime_error("不是 Skyrim 插件");
		}
		pf.flags = U32(h + 8);
		{
			std::vector<std::uint8_t> d(U32(h + 4));
			if (!f.read(reinterpret_cast<char*>(d.data()), static_cast<std::streamsize>(d.size()))) {
				throw std::runtime_error("插件头被截断");
			}
			for (const auto& s : ParseSubs(d.data(), d.size())) {
				if (Is(s.sig, "MAST")) {
					std::string m(s.data.begin(), s.data.end());
					while (!m.empty() && m.back() == '\0') {
						m.pop_back();
					}
					pf.masters.push_back(m);
				}
			}
		}
		std::vector<std::uint8_t> grp, inflated;
		while (f.read(reinterpret_cast<char*>(h), 24)) {
			if (std::memcmp(h, "GRUP", 4) != 0) {
				throw std::runtime_error("有记录不在任何分组里");
			}
			const auto size = U32(h + 4);
			if (size < 24) {
				throw std::runtime_error("分组的大小不正确");
			}
			bool want = a_types.empty();
			for (const auto& t : a_types) {
				want = want || std::memcmp(h + 8, t.data(), 4) == 0;
			}
			if (!want) {
				f.seekg(size - 24, std::ios::cur);
				continue;
			}
			grp.resize(size - 24);
			if (!f.read(reinterpret_cast<char*>(grp.data()), static_cast<std::streamsize>(grp.size()))) {
				throw std::runtime_error("分组被截断");
			}
			std::size_t p = 0;
			while (p + 24 <= grp.size()) {
				const auto* r = grp.data() + p;
				if (std::memcmp(r, "GRUP", 4) == 0) {  // a nested group (cells, topics...): not read here
					const auto gs = U32(r + 4);
					p += gs < 24 ? 24 : gs;
					continue;
				}
				const auto dataSize = U32(r + 4);
				if (p + 24 + dataSize > grp.size()) {
					throw std::runtime_error("记录超出了它所在分组的末尾");
				}
				Record rec;
				std::memcpy(rec.sig, r, 4);
				rec.flags = U32(r + 8);
				rec.formID = U32(r + 12);
				rec.version = U16(r + 20);
				const std::uint8_t* d = r + 24;
				std::size_t         n = dataSize;
				if (rec.flags & 0x40000) {
					if (n < 4) {
						throw std::runtime_error("压缩记录太短");
					}
					inflated = DecompressZlib(d + 4, n - 4, U32(d));
					d = inflated.data();
					n = inflated.size();
					rec.flags &= ~0x40000u;
				}
				rec.subs = ParseSubs(d, n);
				pf.records.push_back(std::move(rec));
				p += 24 + dataSize;
			}
		}
		return pf;
	}

	std::vector<std::uint8_t> WritePlugin(const std::vector<std::string>& a_masters, std::uint32_t a_headerFlags,
		const std::vector<Record>& a_records, const std::string& a_author, const std::string& a_desc)
	{
		// group the records by type, groups in the game's order
		std::map<int, std::pair<std::string, std::vector<const Record*>>> groups;
		for (const auto& r : a_records) {
			auto& g = groups[GroupRank(r.sig) * 10000 + 0];
			if (g.first.empty() || g.first == Sig(r.sig)) {
				g.first = Sig(r.sig);
				g.second.push_back(&r);
			} else {
				// several unknown types share the rank: keep them apart
				auto& g2 = groups[GroupRank(r.sig) * 10000 + static_cast<int>(std::hash<std::string>{}(Sig(r.sig)) % 9999) + 1];
				g2.first = Sig(r.sig);
				g2.second.push_back(&r);
			}
		}
		std::vector<std::uint8_t> body;
		std::uint32_t             count = 0;
		for (auto& [rank, g] : groups) {
			std::sort(g.second.begin(), g.second.end(), [](const Record* a, const Record* b) { return a->formID < b->formID; });
			std::vector<std::uint8_t> gb;
			for (const auto* r : g.second) {
				std::vector<std::uint8_t> d;
				WriteSubs(d, r->subs);
				PutSig(gb, r->sig);
				Put32(gb, static_cast<std::uint32_t>(d.size()));
				Put32(gb, r->flags & ~0x40000u);
				Put32(gb, r->formID);
				Put32(gb, 0);  // version control
				Put16(gb, r->version);
				Put16(gb, 0);
				gb.insert(gb.end(), d.begin(), d.end());
				++count;
			}
			PutSig(body, "GRUP");
			Put32(body, static_cast<std::uint32_t>(gb.size() + 24));
			PutSig(body, g.first.c_str());
			Put32(body, 0);  // top group
			Put32(body, 0);
			Put32(body, 0);
			body.insert(body.end(), gb.begin(), gb.end());
			++count;
		}
		std::vector<Sub> hs;
		{
			Sub hedr;
			std::memcpy(hedr.sig, "HEDR", 4);
			const float ver = 1.7f;
			std::uint32_t vb;
			std::memcpy(&vb, &ver, 4);
			Put32(hedr.data, vb);
			Put32(hedr.data, count);
			Put32(hedr.data, 0x800);
			hs.push_back(std::move(hedr));
			Sub cnam;
			std::memcpy(cnam.sig, "CNAM", 4);
			cnam.data.assign(a_author.begin(), a_author.end());
			cnam.data.push_back(0);
			hs.push_back(std::move(cnam));
			if (!a_desc.empty()) {
				Sub snam;
				std::memcpy(snam.sig, "SNAM", 4);
				snam.data.assign(a_desc.begin(), a_desc.end());
				snam.data.push_back(0);
				hs.push_back(std::move(snam));
			}
			for (const auto& m : a_masters) {
				Sub mast;
				std::memcpy(mast.sig, "MAST", 4);
				mast.data.assign(m.begin(), m.end());
				mast.data.push_back(0);
				hs.push_back(std::move(mast));
				Sub data;
				std::memcpy(data.sig, "DATA", 4);
				data.data.assign(8, 0);
				hs.push_back(std::move(data));
			}
			Sub intv;
			std::memcpy(intv.sig, "INTV", 4);
			Put32(intv.data, 1);
			hs.push_back(std::move(intv));
		}
		std::vector<std::uint8_t> hd;
		WriteSubs(hd, hs);
		std::vector<std::uint8_t> out;
		PutSig(out, "TES4");
		Put32(out, static_cast<std::uint32_t>(hd.size()));
		Put32(out, a_headerFlags);
		Put32(out, 0);
		Put32(out, 0);
		Put16(out, 44);
		Put16(out, 0);
		out.insert(out.end(), hd.begin(), hd.end());
		out.insert(out.end(), body.begin(), body.end());
		return out;
	}

	std::vector<std::string> ReadLoadOrder(const std::filesystem::path& a_pluginsTxt, const std::filesystem::path& a_gameFolder)
	{
		auto trim = [](std::string s) {
			while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
				s.pop_back();
			}
			std::size_t a = 0;
			if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
				static_cast<unsigned char>(s[2]) == 0xBF) {
				a = 3;
			}
			while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) {
				++a;
			}
			return s.substr(a);
		};
		std::vector<std::string> out;
		std::set<std::string>    seen;
		auto add = [&](const std::string& p) {
			if (!p.empty() && seen.insert(Lower(p)).second) {
				out.push_back(p);
			}
		};
		// enabled plugins (MO2 writes them in load order; the game's own file too)
		std::vector<std::string> enabled;
		std::set<std::string>    enabledLow;
		{
			std::ifstream f(a_pluginsTxt);
			for (std::string line; std::getline(f, line);) {
				line = trim(line);
				if (!line.empty() && line[0] == '*') {
					enabled.push_back(trim(line.substr(1)));
					enabledLow.insert(Lower(enabled.back()));
				}
			}
		}
		for (const char* p : { "Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm" }) {
			add(p);
		}
		// Mod Organizer keeps the full order (Creation Club files included) in loadorder.txt
		const auto loadorderTxt = a_pluginsTxt.parent_path() / "loadorder.txt";
		std::error_code ec;
		if (std::filesystem::exists(loadorderTxt, ec)) {
			std::ifstream f(loadorderTxt);
			for (std::string line; std::getline(f, line);) {
				line = trim(line);
				if (line.empty() || line[0] == '#') {
					continue;
				}
				const auto low = Lower(line);
				if (enabledLow.contains(low) || seen.contains(low)) {
					add(line);
				}
			}
		} else {
			std::ifstream ccc(a_gameFolder / "Skyrim.ccc");
			for (std::string line; std::getline(ccc, line);) {
				line = trim(line);
				if (!line.empty() && std::filesystem::exists(a_gameFolder / "Data" / line, ec)) {
					add(line);
				}
			}
		}
		for (const auto& e : enabled) {
			add(e);
		}
		return out;
	}

	// ======================================================================================
	// record layouts: where the form IDs are
	// ======================================================================================

	namespace
	{
		// a subrecord in comparable form: form ID fields are zeroed in data and listed in ids as
		// load-order-wide keys ((plugin index + 1) << 32 | object id; 0 = none)
		struct CSub
		{
			char                                            sig[4];
			std::vector<std::uint8_t>                       data;
			std::vector<std::pair<std::uint32_t, std::uint64_t>> ids;  // offset, key
		};
		bool operator==(const CSub& a, const CSub& b) { return Is(a.sig, b.sig) && a.data == b.data && a.ids == b.ids; }

		struct CRec
		{
			char              sig[4];
			std::uint32_t     flags = 0;
			std::uint16_t     version = 44;
			std::vector<CSub> subs;
		};
		bool SameSubs(const CRec& a, const CRec& b) { return a.subs == b.subs; }

		enum class Kind
		{
			kUnknown,
			kNone,     // no form IDs
			kLString,  // FULL/DESC/SHRT: a strings-file id in localized plugins
			kForm0,    // one form ID at offset 0
			kFormArr,  // form IDs, 4 bytes each
			kVmad,
			kAltTex,   // MODS / MO2S...: alternate textures
			kCoed,
			kDstd,
			kAtkd,
			kLvlo,
			kCrdt,
		};

		Kind Layout(const char* rec, const char* sub)
		{
			const std::string r(rec, 4), s(sub, 4);
			// type-specific first
			if (r == "LVLI" || r == "LVLN" || r == "LVSP") {
				if (s == "LVLD" || s == "LVLF" || s == "LLCT") return Kind::kNone;
				if (s == "LVLG") return Kind::kForm0;
				if (s == "LVLO") return Kind::kLvlo;
			} else if (r == "CONT") {
				if (s == "DATA") return Kind::kNone;
				if (s == "SNAM" || s == "QNAM") return Kind::kForm0;
			} else if (r == "FLST") {
				if (s == "LNAM") return Kind::kForm0;
			} else if (r == "OTFT") {
				if (s == "INAM") return Kind::kFormArr;
			} else if (r == "ARMO") {
				if (s == "RNAM" || s == "MODL" || s == "TNAM") return Kind::kForm0;
				if (s == "DATA" || s == "DNAM") return Kind::kNone;
			} else if (r == "WEAP") {
				if (s == "EFSD" || s == "INAM" || s == "WNAM" || s == "SNAM" || s == "XNAM" || s == "NAM7" || s == "TNAM" || s == "UNAM" ||
					s == "NAM9" || s == "NAM8" || s == "CNAM")
					return Kind::kForm0;
				if (s == "NNAM" || s == "DATA" || s == "DNAM" || s == "VNAM") return Kind::kNone;
				if (s == "CRDT") return Kind::kCrdt;
			} else if (r == "AMMO") {
				if (s == "DATA") return Kind::kForm0;
				if (s == "ONAM") return Kind::kNone;
			} else if (r == "NPC_") {
				static const char* const forms[] = { "SNAM", "INAM", "VTCK", "TPLT", "RNAM", "SPLO", "WNAM", "ANAM", "ATKR", "SPOR", "OCOR", "GWOR",
					"ECOR", "PRKR", "PKID", "CNAM", "PNAM", "HCLF", "ZNAM", "GNAM", "CSDI", "CSCR", "DOFT", "SOFT", "DPLT", "CRIF", "FTST" };
				for (const char* f : forms) {
					if (s == f) return Kind::kForm0;
				}
				static const char* const none[] = { "ACBS", "SPCT", "ATKE", "PRKZ", "AIDT", "DATA", "DNAM", "NAM5", "NAM6", "NAM7", "NAM8", "CSDT",
					"CSDC", "QNAM", "NAM9", "NAMA", "TINI", "TINC", "TINV", "TIAS" };
				for (const char* f : none) {
					if (s == f) return Kind::kNone;
				}
				if (s == "ATKD") return Kind::kAtkd;
			}
			// shared parts
			if (s == "EDID" || s == "OBND" || s == "MODT" || s == "MOD2" || s == "MO2T" || s == "MOD3" || s == "MO3T" || s == "MOD4" ||
				s == "MO4T" || s == "ICON" || s == "MICO" || s == "ICO2" || s == "MIC2" || s == "DEST" || s == "DMDL" || s == "DMDT" ||
				s == "DSTF" || s == "KSIZ" || s == "COCT" || s == "EAMT" || s == "BODT" || s == "BOD2" || s == "BMCT")
				return Kind::kNone;
			if (s == "MODL") return Kind::kNone;  // a model path (ARMO's MODL is handled above)
			if (s == "FULL" || s == "DESC" || s == "SHRT") return Kind::kLString;
			if (s == "VMAD") return Kind::kVmad;
			if (s == "MODS" || s == "MO2S" || s == "MO3S" || s == "MO4S" || s == "DMDS") return Kind::kAltTex;
			if (s == "DSTD") return Kind::kDstd;
			if (s == "KWDA") return Kind::kFormArr;
			if (s == "CNTO" || s == "EITM" || s == "ETYP" || s == "BIDS" || s == "BAMT" || s == "YNAM" || s == "ZNAM") return Kind::kForm0;
			if (s == "COED") return Kind::kCoed;
			return Kind::kUnknown;
		}

		// form ID offsets inside a script attachment (VMAD) of a record without fragments
		bool VmadForms(const std::vector<std::uint8_t>& d, std::vector<std::uint32_t>& offs, std::string& err)
		{
			std::size_t p = 0;
			auto need = [&](std::size_t n) {
				if (p + n > d.size()) {
					throw std::runtime_error("脚本数据被截断");
				}
			};
			auto u8 = [&] { need(1); return d[p++]; };
			auto u16 = [&] { need(2); auto v = U16(d.data() + p); p += 2; return v; };
			auto u32 = [&] { need(4); auto v = U32(d.data() + p); p += 4; return v; };
			auto wstr = [&] { const auto n = u16(); need(n); p += n; };
			try {
				const auto version = static_cast<std::int16_t>(u16());
				const auto objFormat = static_cast<std::int16_t>(u16());
				auto object = [&] {
					need(8);
					offs.push_back(static_cast<std::uint32_t>(objFormat == 1 ? p : p + 4));
					p += 8;
				};
				const auto scripts = u16();
				for (std::uint16_t i = 0; i < scripts; ++i) {
					wstr();
					if (version >= 4) {
						u8();
					}
					const auto props = u16();
					for (std::uint16_t k = 0; k < props; ++k) {
						wstr();
						const auto type = u8();
						if (version >= 4) {
							u8();
						}
						switch (type) {
						case 1: object(); break;
						case 2: wstr(); break;
						case 3:
						case 4: u32(); break;
						case 5: u8(); break;
						case 11: { const auto n = u32(); for (std::uint32_t j = 0; j < n; ++j) object(); break; }
						case 12: { const auto n = u32(); for (std::uint32_t j = 0; j < n; ++j) wstr(); break; }
						case 13:
						case 14: { const auto n = u32(); need(std::size_t(n) * 4); p += std::size_t(n) * 4; break; }
						case 15: { const auto n = u32(); need(n); p += n; break; }
						default: throw std::runtime_error("未知的脚本属性类型 " + std::to_string(type));
						}
					}
				}
				if (p != d.size()) {
					throw std::runtime_error("脚本数据里有多余的部分（片段），这里不该出现");
				}
				return true;
			} catch (const std::exception& e) {
				err = std::string("脚本（VMAD）：") + e.what();
				return false;
			}
		}

		bool AltTexForms(const std::vector<std::uint8_t>& d, std::vector<std::uint32_t>& offs)
		{
			std::size_t p = 0;
			if (d.size() < 4) {
				return d.empty();
			}
			const auto n = U32(d.data());
			p = 4;
			for (std::uint32_t i = 0; i < n; ++i) {
				if (p + 4 > d.size()) return false;
				const auto len = U32(d.data() + p);
				p += 4 + len;
				if (p + 8 > d.size()) return false;
				offs.push_back(static_cast<std::uint32_t>(p));
				p += 8;
			}
			return p == d.size();
		}
	}

	// ======================================================================================
	// building the patch
	// ======================================================================================

	namespace
	{
		struct PInfo
		{
			std::string           name;
			bool                  found = false, localized = false;
			std::vector<int>      masterLo;  // load order index of each master (-1: not loaded)
			std::set<int>         masterSet;
			bool                  stringsLoaded = false;
			std::unordered_map<std::uint32_t, std::string> strings;
		};

		struct Ver
		{
			int    plugin;
			Record rec;
		};

		// a list inside a record that is merged entry by entry
		struct ListSpec
		{
			const char* entry;     // the subrecord starting an entry (or holding an array)
			const char* follower;  // a subrecord belonging to the entry before it (COED), or nullptr
			const char* count;     // the count subrecord before the list, or nullptr
			int         countSize; // 1 (LLCT) or 4
			bool        byForm;    // entries are identified by their first form ID; the rest is a value
			bool        array;     // one subrecord holding 4-byte form IDs (KWDA, OTFT INAM)
			std::size_t max;       // most entries the game takes (0: no limit)
			const char* what;      // for the report
		};

		std::vector<ListSpec> Lists(const char* rec)
		{
			const std::string r(rec, 4);
			if (r == "LVLI" || r == "LVLN" || r == "LVSP") {
				return { { "LVLO", "COED", "LLCT", 1, false, false, 255, "个条目" } };
			}
			if (r == "CONT") {
				return { { "CNTO", "COED", "COCT", 4, true, false, 0, "件物品" } };
			}
			if (r == "FLST") {
				return { { "LNAM", nullptr, nullptr, 0, false, false, 0, "个表单" } };
			}
			if (r == "OTFT") {
				return { { "INAM", nullptr, nullptr, 0, false, true, 0, "件物品" } };
			}
			if (r == "NPC_") {
				return { { "SNAM", nullptr, nullptr, 0, true, false, 0, "个派系" }, { "SPLO", nullptr, "SPCT", 4, false, false, 0, "个法术" },
					{ "PRKR", nullptr, "PRKZ", 4, true, false, 0, "个专长" }, { "CNTO", "COED", "COCT", 4, true, false, 0, "件物品" },
					{ "KWDA", nullptr, "KSIZ", 4, false, true, 0, "个关键字" } };
			}
			if (r == "ARMO") {
				// armatures (MODL) are not merged: a mod that replaces an armour's look swaps them on purpose, and
				// adding back the old ones would draw both meshes
				return { { "KWDA", nullptr, "KSIZ", 4, false, true, 0, "个关键字" } };
			}
			if (r == "WEAP" || r == "AMMO") {
				return { { "KWDA", nullptr, "KSIZ", 4, false, true, 0, "个关键字" } };
			}
			return {};
		}

		// subrecord order of each type, to place a list the winning override does not have
		std::vector<std::string> Order(const char* rec)
		{
			const std::string r(rec, 4);
			std::string s;
			if (r == "LVLI") s = "EDID OBND LVLD LVLF LVLG LLCT LVLO COED";
			else if (r == "LVLN") s = "EDID OBND LVLD LVLF LVLG LLCT LVLO COED MODL MODT MODS";
			else if (r == "LVSP") s = "EDID OBND LVLD LVLF LLCT LVLO";
			else if (r == "CONT") s = "EDID VMAD OBND FULL MODL MODT MODS COCT CNTO COED DEST DSTD DMDL DMDT DMDS DSTF DATA SNAM QNAM";
			else if (r == "FLST") s = "EDID LNAM";
			else if (r == "OTFT") s = "EDID INAM";
			else if (r == "NPC_")
				s = "EDID VMAD OBND ACBS SNAM INAM VTCK TPLT RNAM SPCT SPLO DEST DSTD DMDL DMDT DMDS DSTF WNAM ANAM ATKR ATKD ATKE SPOR OCOR GWOR "
					"ECOR PRKZ PRKR COCT CNTO COED AIDT PKID KSIZ KWDA CNAM FULL SHRT DATA DNAM PNAM HCLF ZNAM GNAM NAM5 NAM6 NAM7 NAM8 CSDT CSDI "
					"CSDC CSCR DOFT SOFT DPLT CRIF FTST QNAM NAM9 NAMA TINI TINC TINV TIAS";
			else if (r == "ARMO")
				s = "EDID VMAD OBND FULL EITM EAMT MOD2 MO2T MO2S ICON MICO MOD4 MO4T MO4S ICO2 MIC2 BODT BOD2 DEST DSTD DMDL DMDT DMDS DSTF YNAM "
					"ZNAM BMCT ETYP BIDS BAMT RNAM KSIZ KWDA DESC MODL DATA DNAM TNAM";
			else if (r == "WEAP")
				s = "EDID VMAD OBND FULL MODL MODT MODS ICON MICO EITM EAMT DEST DSTD DMDL DMDT DMDS DSTF ETYP BIDS BAMT YNAM ZNAM KSIZ KWDA DESC "
					"MOD3 MO3T MO3S EFSD NNAM INAM WNAM SNAM XNAM NAM7 TNAM UNAM NAM9 NAM8 DATA DNAM CRDT VNAM CNAM";
			else if (r == "AMMO") s = "EDID VMAD OBND FULL MODL MODT MODS ICON MICO DEST DSTD DMDL DMDT DMDS DSTF YNAM ZNAM DESC KSIZ KWDA DATA ONAM";
			std::vector<std::string> out;
			std::istringstream       in(s);
			for (std::string w; in >> w;) {
				out.push_back(w);
			}
			return out;
		}

		const std::vector<std::string> kTypes = { "LVLI", "LVLN", "LVSP", "CONT", "FLST", "OTFT", "NPC_", "ARMO", "WEAP", "AMMO" };

		std::string Serialize(const std::vector<CSub>& subs)
		{
			std::string s;
			for (const auto& c : subs) {
				s.append(c.sig, 4);
				const auto n = static_cast<std::uint32_t>(c.data.size());
				s.append(reinterpret_cast<const char*>(&n), 4);
				s.append(reinterpret_cast<const char*>(c.data.data()), c.data.size());
				for (const auto& [o, k] : c.ids) {
					s.append(reinterpret_cast<const char*>(&o), 4);
					s.append(reinterpret_cast<const char*>(&k), 8);
				}
				s.push_back('|');
			}
			return s;
		}

		struct Entry
		{
			std::string       key;    // identity (with an occurrence number for repeats)
			std::string       value;  // everything, to see a changed value
			std::vector<CSub> subs;
		};

		class Builder
		{
		public:
			Builder(const Options& a_opt, Progress* a_progress, Result& a_res) :
				_opt(a_opt), _progress(a_progress), _res(a_res) {}

			void run()
			{
				load();
				if (cancelled()) {
					_res.error = "已取消";
					return;
				}
				merge();
				if (cancelled()) {
					_res.error = "已取消";
					return;
				}
				write();
			}

		private:
			bool cancelled() const { return _progress && _progress->cancel; }

			std::uint64_t key(int a_plugin, std::uint32_t a_raw, bool& a_ok) const
			{
				a_ok = true;
				if (a_raw == 0) {
					return 0;
				}
				const auto& p = _plugins[a_plugin];
				const auto  m = a_raw >> 24;
				int         lo = a_plugin;
				if (m < p.masterLo.size()) {
					lo = p.masterLo[m];
					if (lo < 0) {
						a_ok = false;
						return 0;
					}
				}
				return (static_cast<std::uint64_t>(lo + 1) << 32) | (a_raw & 0xFFFFFF);
			}

			std::string idText(std::uint64_t k) const
			{
				if (!k) {
					return "NULL";
				}
				const auto lo = static_cast<int>(k >> 32) - 1;
				return _plugins[lo].name + ":0x" + Hex(static_cast<std::uint32_t>(k & 0xFFFFFF));
			}

			void load()
			{
				const auto n = _opt.loadOrder.size();
				_plugins.resize(n);
				std::unordered_map<std::string, int> index;
				for (std::size_t i = 0; i < n; ++i) {
					_plugins[i].name = _opt.loadOrder[i];
					index.emplace(Lower(_opt.loadOrder[i]), static_cast<int>(i));
				}
				if (_progress) {
					_progress->total = n;
					_progress->done = 0;
				}
				for (std::size_t i = 0; i < n && !cancelled(); ++i) {
					auto& p = _plugins[i];
					if (_progress) {
						_progress->set(p.name);
					}
					if (Lower(p.name) == Lower(_opt.patchName)) {
						if (_progress) ++_progress->done;
						continue;  // an older merged patch is not an input
					}
					const auto path = _opt.findPlugin ? _opt.findPlugin(p.name) : std::nullopt;
					if (!path) {
						_res.missing.push_back(p.name);
						if (_progress) ++_progress->done;
						continue;
					}
					PluginFile pf;
					try {
						pf = ReadPlugin(*path, kTypes);
					} catch (const std::exception& e) {
						_res.missing.push_back(p.name + " (" + e.what() + ")");
						if (_progress) ++_progress->done;
						continue;
					}
					p.found = true;
					p.localized = pf.flags & 0x80;
					for (const auto& m : pf.masters) {
						auto it = index.find(Lower(m));
						const int lo = it == index.end() || it->second >= static_cast<int>(i) ? -1 : it->second;
						p.masterLo.push_back(lo);
						if (lo >= 0) {
							p.masterSet.insert(lo);
						}
					}
					++_res.pluginsRead;
					for (auto& r : pf.records) {
						bool ok;
						const auto k = key(static_cast<int>(i), r.formID, ok);
						if (!ok || !k) {
							continue;
						}
						if (Is(r.sig, "NPC_")) {
							_npcs.insert(k);
						}
						auto& vs = _versions[k];
						if (vs.empty()) {
							_order.push_back(k);
						}
						vs.push_back({ static_cast<int>(i), std::move(r) });
						++_res.recordsRead;
					}
					if (_progress) ++_progress->done;
				}
			}

			const std::unordered_map<std::uint32_t, std::string>& strings(int a_plugin)
			{
				auto& p = _plugins[a_plugin];
				if (!p.stringsLoaded) {
					p.stringsLoaded = true;
					auto stem = Lower(std::filesystem::path(p.name).stem().string());
					for (const char* ext : { ".strings", ".dlstrings", ".ilstrings" }) {
						for (const auto& lang : { Lower(_opt.language), std::string("english") }) {
							const auto data = _opt.readStrings ? _opt.readStrings(stem + "_" + lang + ext) : std::vector<std::uint8_t>{};
							if (data.size() < 8) {
								continue;
							}
							const bool lenPrefixed = std::strcmp(ext, ".strings") != 0;
							const auto count = U32(data.data()), size = U32(data.data() + 4);
							const std::size_t start = 8 + std::size_t(count) * 8;
							if (start > data.size()) {
								continue;
							}
							for (std::uint32_t e = 0; e < count; ++e) {
								const auto id = U32(data.data() + 8 + e * 8), off = U32(data.data() + 12 + e * 8);
								std::size_t q = start + off;
								if (off >= size || q >= data.size()) {
									continue;
								}
								std::size_t len = data.size() - q;
								if (lenPrefixed) {
									if (q + 4 > data.size()) continue;
									len = std::min<std::size_t>(U32(data.data() + q), data.size() - q - 4);
									q += 4;
								}
								const auto* s = reinterpret_cast<const char*>(data.data() + q);
								p.strings.emplace(id, std::string(s, strnlen(s, len)));
							}
							break;
						}
					}
				}
				return p.strings;
			}

			// the comparable form of one version of a record; nullopt (with a reason) if it cannot be read safely
			std::optional<CRec> canon(const Ver& v, std::string& err)
			{
				CRec c;
				std::memcpy(c.sig, v.rec.sig, 4);
				c.flags = v.rec.flags;
				c.version = v.rec.version;
				for (const auto& s : v.rec.subs) {
					CSub cs;
					std::memcpy(cs.sig, s.sig, 4);
					cs.data = s.data;
					std::vector<std::uint32_t> offs;
					switch (Layout(v.rec.sig, s.sig)) {
					case Kind::kUnknown:
						err = "有一个部分（" + Sig(s.sig) + "）ReSaver Plus 还不认识";
						return std::nullopt;
					case Kind::kNone: break;
					case Kind::kLString:
						if (_plugins[v.plugin].localized) {
							if (s.data.size() != 4) {
								err = "本地化文本（" + Sig(s.sig) + "）不是字符串 ID";
								return std::nullopt;
							}
							const auto id = U32(s.data.data());
							std::string text;
							if (id) {
								const auto& t = strings(v.plugin);
								auto        it = t.find(id);
								if (it == t.end()) {
									err = "找不到 " + Sig(s.sig) + " 的文本（在 " + _plugins[v.plugin].name + " 的字符串文件里）";
									return std::nullopt;
								}
								text = it->second;
							}
							cs.data.assign(text.begin(), text.end());
							cs.data.push_back(0);
						}
						break;
					case Kind::kForm0:
						if (s.data.size() < 4) {
							err = Sig(s.sig) + " 太短";
							return std::nullopt;
						}
						offs.push_back(0);
						break;
					case Kind::kFormArr:
						if (s.data.size() % 4) {
							err = Sig(s.sig) + " 的大小不正确";
							return std::nullopt;
						}
						for (std::uint32_t o = 0; o < s.data.size(); o += 4) {
							offs.push_back(o);
						}
						break;
					case Kind::kVmad:
						if (!VmadForms(s.data, offs, err)) {
							return std::nullopt;
						}
						break;
					case Kind::kAltTex:
						if (!AltTexForms(s.data, offs)) {
							err = "替换贴图（" + Sig(s.sig) + "）读不出来";
							return std::nullopt;
						}
						break;
					case Kind::kCoed:
						{
							if (s.data.size() < 12) {
								err = "COED 太短";
								return std::nullopt;
							}
							offs.push_back(0);
							bool ok;
							const auto owner = key(v.plugin, U32(s.data.data()), ok);
							if (ok && owner && _npcs.contains(owner)) {
								offs.push_back(4);  // the owner is an NPC: a global variable follows
							}
							break;
						}
					case Kind::kDstd:
						if (s.data.size() < 20) {
							err = "DSTD 太短";
							return std::nullopt;
						}
						offs = { 8, 12 };
						break;
					case Kind::kAtkd:
						if (s.data.size() < 12) {
							err = "ATKD 太短";
							return std::nullopt;
						}
						offs = { 8 };
						break;
					case Kind::kLvlo:
						if (s.data.size() < 8) {
							err = "LVLO 太短";
							return std::nullopt;
						}
						offs = { 4 };
						break;
					case Kind::kCrdt:
						if (s.data.size() == 24) {
							offs = { 16 };
						} else if (s.data.size() == 16) {
							offs = { 12 };
						} else {
							err = "CRDT 的大小出乎意料";
							return std::nullopt;
						}
						break;
					}
					for (const auto o : offs) {
						if (o + 4 > cs.data.size()) {
							err = Sig(s.sig) + " 太短";
							return std::nullopt;
						}
						bool ok;
						const auto k = key(v.plugin, U32(cs.data.data() + o), ok);
						if (!ok) {
							err = "指向了没有加载的插件里的表单";
							return std::nullopt;
						}
						std::memset(cs.data.data() + o, 0, 4);
						cs.ids.push_back({ o, k });
					}
					c.subs.push_back(std::move(cs));
				}
				return c;
			}

			std::vector<Entry> entries(const CRec& c, const ListSpec& sp) const
			{
				std::vector<Entry> out;
				for (std::size_t i = 0; i < c.subs.size(); ++i) {
					const auto& s = c.subs[i];
					if (!Is(s.sig, sp.entry)) {
						continue;
					}
					if (sp.array) {
						for (const auto& [o, k] : s.ids) {
							Entry e;
							CSub  one;
							std::memcpy(one.sig, sp.entry, 4);
							one.data.assign(4, 0);
							one.ids.push_back({ 0, k });
							e.subs.push_back(std::move(one));
							out.push_back(std::move(e));
						}
						continue;
					}
					Entry e;
					e.subs.push_back(s);
					while (sp.follower && i + 1 < c.subs.size() && Is(c.subs[i + 1].sig, sp.follower)) {
						e.subs.push_back(c.subs[++i]);
					}
					out.push_back(std::move(e));
				}
				std::map<std::string, int> seen;
				for (auto& e : out) {
					e.value = Serialize(e.subs);
					std::string id;
					if (sp.byForm && !e.subs[0].ids.empty()) {
						const auto k = e.subs[0].ids[0].second;
						id.assign(reinterpret_cast<const char*>(&k), 8);
					} else {
						id = e.value;
					}
					e.key = id + "#" + std::to_string(seen[id]++);
				}
				return out;
			}

			// a_patch gets what a_ovr changed compared with a_base; returns a short description of it
			std::string apply(std::vector<Entry>& a_patch, const std::vector<Entry>& a_base, const std::vector<Entry>& a_ovr, const ListSpec& sp)
			{
				std::unordered_map<std::string, const Entry*> base, ovr;
				for (const auto& e : a_base) base.emplace(e.key, &e);
				for (const auto& e : a_ovr) ovr.emplace(e.key, &e);
				std::size_t added = 0, removed = 0, changed = 0;
				// removed by the override
				for (const auto& e : a_base) {
					if (ovr.contains(e.key)) {
						continue;
					}
					auto it = std::find_if(a_patch.begin(), a_patch.end(), [&](const Entry& p) { return p.key == e.key; });
					if (it != a_patch.end()) {
						a_patch.erase(it);
						++removed;
					}
				}
				// added or changed by the override
				for (const auto& e : a_ovr) {
					auto b = base.find(e.key);
					auto it = std::find_if(a_patch.begin(), a_patch.end(), [&](const Entry& p) { return p.key == e.key; });
					if (b == base.end()) {
						if (it == a_patch.end()) {
							a_patch.push_back(e);
							++added;
						}
					} else if (sp.byForm && b->second->value != e.value && it != a_patch.end() && it->value != e.value) {
						*it = e;
						++changed;
					}
				}
				std::string out;
				auto part = [&](std::size_t n, const char* sign) {
					if (n) {
						out += (out.empty() ? "" : ", ") + std::string(sign) + std::to_string(n);
					}
				};
				part(added, "+");
				part(removed, "-");
				part(changed, "~");
				return out.empty() ? out : out + " " + sp.what;
			}

			// puts a list back into the record: the count, then the entries, where the list was (or belongs)
			bool place(CRec& c, const ListSpec& sp, const std::vector<Entry>& es, std::string& err) const
			{
				if (sp.max && es.size() > sp.max) {
					err = "合并后的列表会有 " + std::to_string(es.size()) + " " + sp.what + "（游戏最多接受 " +
						  std::to_string(sp.max) + " 个）";
					return false;
				}
				// where: the first count or entry subrecord, else by the type's order
				std::size_t pos = SIZE_MAX;
				std::vector<CSub> kept;
				bool afterEntry = false;
				for (const auto& s : c.subs) {
					const bool isEntry = Is(s.sig, sp.entry);
					const bool isCount = sp.count && Is(s.sig, sp.count);
					const bool isFollower = sp.follower && afterEntry && Is(s.sig, sp.follower);
					if (isEntry || isCount || isFollower) {
						if (pos == SIZE_MAX) {
							pos = kept.size();
						}
						afterEntry = isEntry || isFollower;
						continue;
					}
					afterEntry = false;
					kept.push_back(s);
				}
				if (pos == SIZE_MAX) {
					const auto order = Order(c.sig);
					auto rank = [&](const char* sig) {
						for (std::size_t i = 0; i < order.size(); ++i) {
							if (Is(sig, order[i].c_str())) return static_cast<int>(i);
						}
						return -1;
					};
					const int mine = rank(sp.count ? sp.count : sp.entry);
					pos = kept.size();
					for (std::size_t i = 0; i < kept.size(); ++i) {
						if (rank(kept[i].sig) > mine) {
							pos = i;
							break;
						}
					}
				}
				std::vector<CSub> ins;
				const bool lvl = sp.count && Is(sp.count, "LLCT");
				if (sp.count && (!es.empty() || lvl)) {
					CSub cnt;
					std::memcpy(cnt.sig, sp.count, 4);
					const auto n = static_cast<std::uint32_t>(es.size());
					for (int i = 0; i < sp.countSize; ++i) {
						cnt.data.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
					}
					ins.push_back(std::move(cnt));
				}
				if (sp.array) {
					if (!es.empty()) {
						CSub arr;
						std::memcpy(arr.sig, sp.entry, 4);
						for (const auto& e : es) {
							arr.ids.push_back({ static_cast<std::uint32_t>(arr.data.size()), e.subs[0].ids[0].second });
							arr.data.insert(arr.data.end(), 4, 0);
						}
						ins.push_back(std::move(arr));
					}
				} else {
					for (const auto& e : es) {
						ins.insert(ins.end(), e.subs.begin(), e.subs.end());
					}
				}
				kept.insert(kept.begin() + static_cast<std::ptrdiff_t>(pos), ins.begin(), ins.end());
				c.subs = std::move(kept);
				return true;
			}

			std::string edidOf(const Record& r) const
			{
				for (const auto& s : r.subs) {
					if (Is(s.sig, "EDID")) {
						return std::string(reinterpret_cast<const char*>(s.data.data()), strnlen(reinterpret_cast<const char*>(s.data.data()), s.data.size()));
					}
				}
				return {};
			}

			void merge()
			{
				if (_progress) {
					_progress->set("正在合并记录");
					_progress->total = _order.size();
					_progress->done = 0;
				}
				for (const auto k : _order) {
					if (cancelled()) {
						return;
					}
					if (_progress) ++_progress->done;
					auto& vs = _versions[k];
					const int definer = static_cast<int>(k >> 32) - 1;
					if (vs.size() < 3 || vs.front().plugin != definer) {
						continue;  // fewer than two overrides (or its own plugin is not loaded): nothing to merge
					}
					const auto& win = vs.back();
					const std::string label = Sig(win.rec.sig) + " " + edidOf(win.rec) + " [" + idText(k) + "]";
					// all overrides the same? then there is no conflict
					bool differ = false;
					for (std::size_t i = 2; i < vs.size() && !differ; ++i) {
						differ = !(vs[i].rec.subs.size() == vs[1].rec.subs.size() &&
								   std::equal(vs[i].rec.subs.begin(), vs[i].rec.subs.end(), vs[1].rec.subs.begin(), [](const Sub& a, const Sub& b) {
									   return Is(a.sig, b.sig) && a.data == b.data;
								   }));
					}
					if (!differ) {
						continue;
					}
					++_res.conflicts;
					bool deleted = false;
					for (const auto& v : vs) {
						deleted = deleted || (v.rec.flags & 0x20);
					}
					if (deleted) {
						_res.skipped.push_back(label + "：某个插件把它删除了");
						continue;
					}
					std::vector<CRec> cv;
					std::string       err;
					for (const auto& v : vs) {
						auto c = canon(v, err);
						if (!c) {
							break;
						}
						cv.push_back(std::move(*c));
					}
					if (cv.size() != vs.size()) {
						_res.skipped.push_back(label + ": " + err);
						continue;
					}
					CRec patch = cv.back();
					MergedRecord mr;
					mr.type = Sig(win.rec.sig);
					mr.edid = edidOf(win.rec);
					mr.id = idText(k);
					mr.winner = _plugins[win.plugin].name;
					bool failed = false;
					for (const auto& sp : Lists(win.rec.sig)) {
						auto list = entries(patch, sp);
						const auto before = Serialize([&] { std::vector<CSub> a; for (auto& e : list) a.insert(a.end(), e.subs.begin(), e.subs.end()); return a; }());
						for (std::size_t i = 1; i < vs.size(); ++i) {
							// the version this override was made against: the last one among its plugin's masters
							std::size_t b = 0;
							for (std::size_t j = i; j-- > 1;) {
								if (_plugins[vs[i].plugin].masterSet.contains(vs[j].plugin)) {
									b = j;
									break;
								}
							}
							const auto what = apply(list, entries(cv[b], sp), entries(cv[i], sp), sp);
							if (!what.empty() && i + 1 != vs.size()) {
								mr.changes.push_back(_plugins[vs[i].plugin].name + ": " + what);
							}
						}
						const auto after = Serialize([&] { std::vector<CSub> a; for (auto& e : list) a.insert(a.end(), e.subs.begin(), e.subs.end()); return a; }());
						if (after == before) {
							continue;
						}
						if (!place(patch, sp, list, err)) {
							failed = true;
							break;
						}
					}
					if (failed) {
						_res.skipped.push_back(label + ": " + err);
						continue;
					}
					if (SameSubs(patch, cv.back())) {
						continue;  // the winning override already has everything
					}
					_patched.push_back({ k, std::move(patch) });
					_res.merged.push_back(std::move(mr));
				}
			}

			void write()
			{
				// masters: the plugin of every patched record and of every form a patched record points to
				std::set<int> lo;
				auto need = [&](std::uint64_t k, const std::string& why) {
					if (!k) {
						return;
					}
					const int p = static_cast<int>(k >> 32) - 1;
					if (lo.insert(p).second) {
						_res.masterWhy[_plugins[p].name] = why;
					}
				};
				for (const auto& [k, c] : _patched) {
					const auto why = Sig(c.sig) + " " + idText(k);
					need(k, why);
					for (const auto& s : c.subs) {
						for (const auto& [o, id] : s.ids) {
							need(id, why);
						}
					}
				}
				if (lo.size() > 254) {
					_res.error = "补丁需要 " + std::to_string(lo.size()) + " 个主文件；一个插件最多只能有 254 个";
					return;
				}
				std::unordered_map<int, std::uint32_t> idx;
				for (const int p : lo) {
					idx[p] = static_cast<std::uint32_t>(_res.masters.size());
					_res.masters.push_back(_plugins[p].name);
				}
				auto raw = [&](std::uint64_t k) -> std::uint32_t {
					if (!k) {
						return 0;
					}
					return (idx.at(static_cast<int>(k >> 32) - 1) << 24) | static_cast<std::uint32_t>(k & 0xFFFFFF);
				};
				std::vector<Record> recs;
				for (const auto& [k, c] : _patched) {
					Record r;
					std::memcpy(r.sig, c.sig, 4);
					r.flags = c.flags & ~0x40000u;
					r.version = c.version;
					r.formID = raw(k);
					for (const auto& s : c.subs) {
						Sub o;
						std::memcpy(o.sig, s.sig, 4);
						o.data = s.data;
						for (const auto& [off, id] : s.ids) {
							const auto v = raw(id);
							std::memcpy(o.data.data() + off, &v, 4);
						}
						r.subs.push_back(std::move(o));
					}
					recs.push_back(std::move(r));
				}
				// only overrides, no new forms: flagged light (ESL), so it takes no load order slot
				_res.file = WritePlugin(_res.masters, 0x200, recs, "ReSaver Plus",
					"由 ReSaver Plus 生成的合并补丁。改动模组之后请重新构建它。");
				_res.ok = true;
			}

			const Options&                                   _opt;
			Progress*                                        _progress;
			Result&                                          _res;
			std::vector<PInfo>                               _plugins;
			std::unordered_map<std::uint64_t, std::vector<Ver>> _versions;
			std::vector<std::uint64_t>                       _order;  // record keys in the order first seen
			std::unordered_set<std::uint64_t>                _npcs;
			std::vector<std::pair<std::uint64_t, CRec>>      _patched;
		};
	}

	Result Build(const Options& a_opt, Progress* a_progress)
	{
		Result r;
		try {
			Builder b(a_opt, a_progress, r);
			b.run();
		} catch (const std::exception& e) {
			r.ok = false;
			r.error = e.what();
		}
		return r;
	}

	void UseSources(Options& a_opt, const ScriptSources& a_sources)
	{
		const auto* src = &a_sources;
		a_opt.findPlugin = [src](const std::string& a_name) { return src->pluginPath(a_name); };
		a_opt.readStrings = [src](const std::string& a_file) -> std::vector<std::uint8_t> {
			auto it = src->stringsFiles.find(a_file);
			if (it == src->stringsFiles.end()) {
				return {};
			}
			try {
				if (it->second.bsaEntry.empty()) {
					std::ifstream f(it->second.file, std::ios::binary);
					return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {});
				}
				BsaArchive bsa(it->second.file);
				if (const auto* e = bsa.find(it->second.bsaEntry)) {
					return bsa.extract(*e);
				}
			} catch (const std::exception&) {
			}
			return {};
		};
	}

	std::string Result::report() const
	{
		std::ostringstream o;
		o << "ReSaver Plus 合并补丁\n";
		o << "=========================\n\n";
		o << pluginsRead << " 个插件已读取，" << recordsRead << " 条可合并类型的记录。\n";
		o << conflicts << " 条记录被两个或更多插件改成了不同的样子。\n";
		o << merged.size() << " 条已合并进补丁；" << skipped.size() << " 条无法合并（保持获胜插件的版本）。\n\n";
		if (!missing.empty()) {
			o << "加载顺序里没有找到或读不出来的插件：\n";
			for (const auto& m : missing) o << "  " << m << "\n";
			o << "\n";
		}
		o << "补丁的主文件（" << masters.size() << " 个），每个都有一条需要它的记录：\n";
		for (const auto& m : masters) {
			auto it = masterWhy.find(m);
			o << "  " << m << (it != masterWhy.end() ? "   （" + it->second + "）" : "") << "\n";
		}
		o << "\n已合并的记录（先是获胜插件，然后是其他每个插件的改动所增加（+）、移除（-）或修改（~）的内容）：\n";
		for (const auto& r : merged) {
			o << "  " << r.type << " " << (r.edid.empty() ? "" : r.edid + " ") << "[" << r.id << "]  获胜模组 " << r.winner << "\n";
			for (const auto& c : r.changes) o << "      " << c << "\n";
		}
		if (!skipped.empty()) {
			o << "\n未合并：\n";
			for (const auto& s : skipped) o << "  " << s << "\n";
		}
		return o.str();
	}
}
