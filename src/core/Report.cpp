// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Report.h"
#include "Text.h"

#include "ChangeFormData.h"
#include "Ops.h"
#include "PdfCjk.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <cstdio>
#include <ctime>
#include <fstream>

#include "miniz.h"
#include <map>
#include <set>
#include <unordered_map>

namespace ess::report
{
	namespace
	{
		template <class... A>
		std::string Fmt(const char* f, A... a)
		{
			char buf[1024];
			std::snprintf(buf, sizeof(buf), f, a...);
			return buf;
		}

		// The report is laid out on a fixed-pitch grid and the text is Chinese: a character that is
		// not ASCII takes two columns, the way it does in a terminal, so the columns stay aligned.
		// Everything below therefore measures in columns, never in bytes (slicing UTF-8 by bytes
		// would cut a character in half).
		std::vector<std::uint32_t> Decode(const std::string& s)
		{
			std::vector<std::uint32_t> out;
			for (std::size_t i = 0; i < s.size();) {
				const auto    c = static_cast<unsigned char>(s[i]);
				std::uint32_t u = 0xFFFD;
				std::size_t   n = 1;
				if (c < 0x80) {
					u = c;
				} else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
					u = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu);
					n = 2;
				} else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
					u = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu);
					n = 3;
				} else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
					u = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3Fu);
					n = 4;
				}
				out.push_back(u);
				i += n;
			}
			return out;
		}

		void AppendUtf8(std::string& s, std::uint32_t a_cp)
		{
			if (a_cp < 0x80) {
				s.push_back(static_cast<char>(a_cp));
			} else if (a_cp < 0x800) {
				s.push_back(static_cast<char>(0xC0 | (a_cp >> 6)));
				s.push_back(static_cast<char>(0x80 | (a_cp & 0x3F)));
			} else if (a_cp < 0x10000) {
				s.push_back(static_cast<char>(0xE0 | (a_cp >> 12)));
				s.push_back(static_cast<char>(0x80 | ((a_cp >> 6) & 0x3F)));
				s.push_back(static_cast<char>(0x80 | (a_cp & 0x3F)));
			} else {
				s.push_back(static_cast<char>(0xF0 | (a_cp >> 18)));
				s.push_back(static_cast<char>(0x80 | ((a_cp >> 12) & 0x3F)));
				s.push_back(static_cast<char>(0x80 | ((a_cp >> 6) & 0x3F)));
				s.push_back(static_cast<char>(0x80 | (a_cp & 0x3F)));
			}
		}

		int Columns(std::uint32_t a_cp) { return a_cp < 0x80 ? 1 : 2; }

		std::size_t TextColumns(const std::string& s)
		{
			std::size_t n = 0;
			for (auto cp : Decode(s)) {
				n += Columns(cp);
			}
			return n;
		}

		std::string Pad(const std::string& a_text, std::size_t a_width)
		{
			std::string out;
			std::size_t cols = 0;
			for (auto cp : Decode(a_text)) {
				const auto w = Columns(cp);
				if (cols + w > a_width) {
					break;
				}
				AppendUtf8(out, cp);
				cols += w;
			}
			const auto total = TextColumns(a_text);
			if (total > a_width && a_width > 0) {
				// the text does not fit: cut it down to a_width - 1 columns and mark that with "~"
				while (!out.empty() && TextColumns(out) > a_width - 1) {
					out.erase(out.size() - 1);
					while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) {
						out.erase(out.size() - 1);  // drop the rest of the UTF-8 sequence
					}
				}
				out += "~";
				cols = TextColumns(out);
			}
			out.append(a_width > cols ? a_width - cols : 0, ' ');
			return out;
		}

		const char* KindText(mods::Kind k)
		{
			switch (k) {
			case mods::Kind::kPlugin: return "插件";
			case mods::Kind::kLight: return "轻量插件";
			case mods::Kind::kCreated: return "游戏中生成的对象";
			case mods::Kind::kUnattached: return "未附加到任何对象";
			case mods::Kind::kGlobal: return "无对象／未知模组";
			case mods::Kind::kFolder: return "模组文件夹（无插件）";
			}
			return "";
		}

		std::string Index(const mods::ModRow& r)
		{
			if (r.kind == mods::Kind::kPlugin) {
				return Fmt("%02X", r.index);
			}
			if (r.kind == mods::Kind::kLight) {
				return Fmt("FE:%03X", r.index);
			}
			return "";
		}

		void Wrap(std::vector<std::string>& out, const std::string& prefix, const std::string& text, std::size_t width = 110)
		{
			const std::size_t indent = std::min<std::size_t>(TextColumns(prefix), width);
			std::deque<std::uint32_t> rest;
			for (auto cp : Decode(text)) {
				rest.push_back(cp);
			}
			for (;;) {
				std::string line(indent, ' ');
				std::size_t cols = indent, used = 0;
				while (used < rest.size()) {
					const auto w = Columns(rest[used]);
					if (cols + w > width) {
						break;
					}
					AppendUtf8(line, rest[used]);
					cols += w;
					++used;
				}
				if (used < rest.size()) {
					// there is more text: break at the last space of this line when there is one,
					// otherwise right here (Chinese has no spaces between words)
					for (std::size_t k = used; k > indent + 1; --k) {
						if (rest[k - 1] == ' ') {
							used = k - 1;
							line = std::string(indent, ' ');
							for (std::size_t j = 0; j < used; ++j) {
								AppendUtf8(line, rest[j]);
							}
							break;
						}
					}
				}
				rest.erase(rest.begin(), rest.begin() + std::min(used, rest.size()));
				while (!rest.empty() && rest.front() == ' ') {
					rest.pop_front();
				}
				if (used == 0 && !rest.empty()) {
					// the indent alone fills the line: put one character on it anyway, or we would spin
					AppendUtf8(line, rest.front());
					rest.pop_front();
					used = 1;
				}
				if (used > 0) {
					while (!line.empty() && line.back() == ' ') {
						line.pop_back();
					}
				}
				if (used > 0 || rest.empty()) {
					out.push_back(line);
				}
				if (rest.empty()) {
					break;
				}
			}
		}
	}

	std::vector<std::string> Build(SaveFile& a_save, const mods::Table& a_mods, const PluginDB* a_names, const Options& a_opt,
		const std::string& a_saveName)
	{
		std::vector<std::string> L;
		const auto&              h = a_save.header;
		const auto               now = std::time(nullptr);
		char                     when[64];
		std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", std::localtime(&now));

		L.push_back("#ReSaver++ 存档报告");
		L.push_back("存档：     " + a_saveName);
		L.push_back(Fmt("角色：     %s，等级 %u，%s", text::ToUtf8(h.playerName).c_str(), h.playerLevel, text::ToUtf8(h.location).c_str()));
		L.push_back("游戏日期：" + text::ToUtf8(h.gameDate));
		L.push_back(Fmt("插件：     %zu 个完整 + %zu 个轻量", a_save.plugins.full.size(), a_save.plugins.lite.size()));
		L.push_back(std::string("报告：     ") + when + (a_mods.hasSources ? "  （脚本归属依据已扫描的模组文件夹）" : ""));
		L.push_back("");

		auto* p = a_save.papyrus();
		auto  formText = [&](std::uint32_t id) {
            std::string s = Fmt("%08X", id);
            if (a_names) {
                const auto d = a_names->describe(id);
                if (!d.empty()) {
                    s += " " + d;
                }
            }
            return s;
		};

		if (a_opt.mods) {
			L.push_back("#第 1 部分：模组及其运行的脚本");
			std::vector<const mods::ModRow*> rows;
			std::size_t                      totalInst = 0, totalThreads = 0;
			for (const auto& r : a_mods.rows) {
				if (r.scriptCount || r.threadCount || r.instanceCount) {
					rows.push_back(&r);
					totalInst += r.instanceCount;
					totalThreads += r.threadCount;
				}
			}
			L.push_back(Fmt("%zu 个模组在此存档中有脚本：%zu 个脚本实例，%zu 个运行中的线程。", rows.size(), totalInst, totalThreads));
			L.push_back("实例数只统计每个模组自有脚本的副本；引擎绑定（Form、Quest、ReferenceAlias 等）会列在每个模组下");
			L.push_back("但不计入统计。运行中 = 此刻在存档中活动的线程。");
			L.push_back("");
			L.push_back("##概要");
			L.push_back(Pad("序号", 8) + Pad("模组", 58) + Pad("脚本", 9) + Pad("实例", 11) + "运行");
			for (const auto* r : rows) {
				L.push_back(Pad(Index(*r), 8) + Pad(r->name, 58) + Pad(std::to_string(r->scriptCount), 9) + Pad(std::to_string(r->instanceCount), 11) +
							std::to_string(r->threadCount));
			}
			L.push_back("");
			L.push_back("##明细");
			for (const auto* r : rows) {
				L.push_back("");
				L.push_back("##" + r->name + "   （" + KindText(r->kind) + (Index(*r).empty() ? "" : "，加载序号 " + Index(*r)) + "）");
				L.push_back(Fmt("%zu 个脚本挂在该模组的对象上 | %zu 个实例 | %zu 个运行中的线程", r->scriptCount, r->instanceCount, r->threadCount));
				std::vector<const mods::ScriptRow*> list;
				for (const auto& [k, s] : r->scripts) {
					list.push_back(&s);
				}
				std::sort(list.begin(), list.end(), [](auto a, auto b) {
					if (a->engineType != b->engineType) {
						return b->engineType;
					}
					if (a->threads.size() != b->threads.size()) {
						return a->threads.size() > b->threads.size();
					}
					return a->instances.size() > b->instances.size();
				});
				L.push_back("    " + Pad("脚本", 52) + Pad("实例", 11) + Pad("运行", 9) + (a_mods.hasSources ? ".pex 来源" : ""));
				for (const auto* s : list) {
					L.push_back("    " + Pad(s->script + (s->engineType ? "（引擎）" : ""), 52) + Pad(std::to_string(s->instances.size()), 11) +
								Pad(std::to_string(s->threads.size()), 9) + (a_mods.hasSources ? (s->provider.empty() ? "?" : s->provider) : ""));
				}
				if (!r->provided.empty()) {
					std::string all;
					for (const auto& s : r->provided) {
						all += (all.empty() ? "" : ", ") + s;
					}
					Wrap(L, "    提供的 .pex：", all);
				}
			}
			std::string none;
			std::size_t noneCount = 0;
			for (const auto& r : a_mods.rows) {
				if ((r.kind == mods::Kind::kPlugin || r.kind == mods::Kind::kLight) && !r.scriptCount && !r.threadCount && !r.instanceCount) {
					none += (none.empty() ? "" : ", ") + r.name;
					++noneCount;
				}
			}
			L.push_back("");
			L.push_back(Fmt("##此存档中没有脚本的插件（%zu）", noneCount));
			Wrap(L, "    ", none);
			L.push_back("");
		}

		if (a_opt.anomalies && p) {
			const auto rep = ops::Analyze(a_save);
			L.push_back("#第 2 部分：异常");
			L.push_back("##发现");
			if (rep.warnings.empty()) {
				L.push_back("    分析未发现任何问题。");
			}
			for (const auto& w : rep.warnings) {
				Wrap(L, "  * ", w);
			}
			L.push_back("");
			L.push_back("##统计");
			L.push_back(Fmt("    脚本实例 %zu 个：未附加 %zu 个，未定义 %zu 个，位于缺失生成对象上 %zu 个", rep.instances, rep.unattached,
				rep.undefinedInstances, rep.createdWithoutForm));
			L.push_back(Fmt("    脚本 %zu 个：未定义（缺少 .pex）%zu 个", rep.scripts, rep.undefinedScripts));
			L.push_back(Fmt("    线程 %zu 个：未定义 %zu 个，已终止 %zu 个", rep.threads, rep.undefinedThreads, rep.terminatedThreads));
			L.push_back(Fmt("    挂起堆栈 %zu 个，函数消息 %zu 个，数组 %zu 个，变更表单 %zu 个，字符串 %zu 个", rep.suspended,
				rep.functionMessages, rep.arrays, rep.changeForms, rep.strings));
			if (const auto mcm = ops::CountMcmJam(*p); rep.mcmJam) {
				L.push_back(Fmt("    SkyUI MCM 正在注册中：%zu 个线程（清理 > 修复 SkyUI MCM 注册卡死）", mcm));
			}

			// undefined scripts, unattached / missing-object instances by script
			std::map<std::string, std::size_t> undefinedInst, unattached, missing;
			for (const auto& i : p->instances) {
				const auto& n = p->str(i.script);
				if (ops::IsInstanceUndefined(*p, i.script)) {
					undefinedInst[n]++;
				}
				if (ops::IsUnattached(i)) {
					unattached[n]++;
				}
				if (ops::IsCreatedWithoutForm(a_save, i)) {
					missing[n]++;
				}
			}
			auto listCounts = [&](const char* title, const std::map<std::string, std::size_t>& m, const char* note) {
				if (m.empty()) {
					return;
				}
				std::vector<std::pair<std::size_t, std::string>> v;
				std::size_t                                      total = 0;
				for (const auto& [k, n] : m) {
					v.push_back({ n, k });
					total += n;
				}
				std::sort(v.rbegin(), v.rend());
				L.push_back("");
				L.push_back(Fmt("##%s（%zu 个实例，%zu 个脚本）", title, total, v.size()));
				Wrap(L, "    ", note);
				for (std::size_t i = 0; i < v.size() && i < 200; ++i) {
					L.push_back("    " + Pad(std::to_string(v[i].first), 8) + v[i].second);
				}
				if (v.size() > 200) {
					L.push_back(Fmt("    ……另有 %zu 个", v.size() - 200));
				}
			};
			listCounts("未定义脚本的实例", undefinedInst,
				"存档生成时游戏无法加载这些脚本的 .pex（模组已被移除？）。清理 > 移除未定义元素。");
			listCounts("未附加实例", unattached, "未附加到任何对象。清理 > 移除未附加实例。");
			listCounts("位于缺失生成对象上的实例", missing,
				"清理 > 移除位于缺失生成对象上的实例（按模组分组：已安装或未安装）。");

			std::vector<std::string> undefThreads, termThreads;
			for (const auto& a : p->activeScripts) {
				if (!a.data || a.data->frames.empty()) {
					continue;
				}
				const auto& f0 = a.data->frames.front();
				const auto  name = Fmt("线程 %u：%s.%s", a.id, p->str(f0.scriptName).c_str(), p->str(f0.event).c_str());
				if (ops::IsThreadUndefined(*p, a)) {
					undefThreads.push_back(name);
				}
				if (ops::IsThreadTerminated(*p, a)) {
					termThreads.push_back(name);
				}
			}
			auto listLines = [&](const std::string& title, const std::vector<std::string>& v) {
				if (v.empty()) {
					return;
				}
				L.push_back("");
				L.push_back(Fmt("##%s（%zu）", title.c_str(), v.size()));
				for (std::size_t i = 0; i < v.size() && i < 200; ++i) {
					L.push_back("    " + v[i]);
				}
				if (v.size() > 200) {
					L.push_back(Fmt("    ……另有 %zu 个", v.size() - 200));
				}
			};
			listLines("运行未定义脚本的线程", undefThreads);
			listLines("已终止的线程（仍留在存档中）", termThreads);
			listLines("在 Utility.Wait 中循环的脚本（20 个以上副本）", rep.loopingScripts);

			std::vector<std::string> nullLists;
			for (const auto& cf : a_save.changeForms) {
				if (const auto n = cf::NullEntries(a_save, cf)) {
					nullLists.push_back(Fmt("%zu 个空条目：", n) + formText(a_save.formIDOf(cf.refID)));
				}
			}
			listLines("含空条目的 FormList（清理 > 净化 FormList）", nullLists);

			auto top = [&](const char* title, const std::vector<ops::Count>& v) {
				if (v.empty()) {
					return;
				}
				L.push_back("");
				L.push_back(std::string("##") + title);
				for (std::size_t i = 0; i < v.size() && i < 20; ++i) {
					L.push_back("    " + Pad(std::to_string(v[i].n), 8) + v[i].name);
				}
			};
			top("脚本实例最多的（前 20）", rep.instancesByScript);
			top("按事件脚本统计的线程（前 20）", rep.threadsByTopScript);
			top("按等待位置统计的线程（前 20）", rep.threadsByCurrentFrame);
			top("按函数统计的挂起堆栈（前 20）", rep.suspendedByFunction);
			top("按函数统计的函数消息（前 20）", rep.messagesByFunction);
		}
		return L;
	}

	void WriteText(const std::filesystem::path& a_path, const std::vector<std::string>& a_lines)
	{
		std::ofstream f(a_path, std::ios::binary | std::ios::trunc);
		if (!f) {
			throw std::runtime_error("无法写入报告文件");
		}
		for (const auto& l : a_lines) {
			if (l.rfind("##", 0) == 0) {
				f << "\r\n" << l.substr(2) << "\r\n" << std::string(std::min<std::size_t>(l.size() - 2, 100), '-') << "\r\n";
			} else if (!l.empty() && l[0] == '#') {
				f << std::string(100, '=') << "\r\n" << l.substr(1) << "\r\n" << std::string(100, '=') << "\r\n";
			} else {
				f << l << "\r\n";
			}
		}
	}

	// ---- minimal PDF writer: US Letter pages, Courier, WinAnsi text ---------------------------
	namespace
	{
		// UTF-8 -> Windows-1252 (unknown characters become '?')
		std::string ToAnsi(const std::string& s)
		{
			std::string out;
			for (std::size_t i = 0; i < s.size();) {
				const auto c = static_cast<unsigned char>(s[i]);
				char32_t   u;
				int        n;
				if (c < 0x80) {
					u = c;
					n = 1;
				} else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
					u = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
					n = 2;
				} else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
					u = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
					n = 3;
				} else {
					u = '?';
					n = (c & 0xF8) == 0xF0 ? 4 : 1;
				}
				i += n;
				if (u < 0x80 || (u >= 0xA0 && u <= 0xFF)) {
					out.push_back(static_cast<char>(u));
				} else {
					static const std::pair<char32_t, unsigned char> map[] = { { 0x2013, 0x96 }, { 0x2014, 0x97 }, { 0x2018, 0x91 }, { 0x2019, 0x92 },
						{ 0x201C, 0x93 }, { 0x201D, 0x94 }, { 0x2022, 0x95 }, { 0x2026, 0x85 }, { 0x20AC, 0x80 } };
					char r = '?';
					for (const auto& [k, v] : map) {
						if (k == u) {
							r = static_cast<char>(v);
						}
					}
					out.push_back(r);
				}
			}
			return out;
		}

		// zlib (FlateDecode) compression for the embedded font
		std::string Deflate(const std::vector<std::uint8_t>& a_data)
		{
			mz_ulong    len = mz_compressBound(static_cast<mz_ulong>(a_data.size()));
			std::string out(static_cast<std::size_t>(len), '\0');
			if (mz_compress2(reinterpret_cast<unsigned char*>(&out[0]), &len, a_data.data(), static_cast<mz_ulong>(a_data.size()), 9) != MZ_OK) {
				return {};
			}
			out.resize(static_cast<std::size_t>(len));
			return out;
		}

		std::string Esc(const std::string& s)
		{
			std::string o;
			for (char c : s) {
				if (c == '(' || c == ')' || c == '\\') {
					o.push_back('\\');
				}
				o.push_back(c);
			}
			return o;
		}
	}

	void WritePdf(const std::filesystem::path& a_path, const std::string& a_title, const std::vector<std::string>& a_lines)
	{
		constexpr double  kW = 612, kH = 792, kMargin = 36, kSize = 7.6, kLead = 9.2;
		constexpr double  kCell = 0.6 * kSize;  // one column of the grid: Courier is 0.6 em wide
		const std::size_t columns = static_cast<std::size_t>((kW - 2 * kMargin) / kCell);
		const std::size_t perPage = static_cast<std::size_t>((kH - 2 * kMargin - 14) / kLead);

		struct Line
		{
			int         font;  // 1 regular, 2 bold
			std::string text;
		};

		// the ## / # markers of the line list mark bold headings (the .txt writer does the same)
		std::vector<Line> pre;
		for (const auto& raw : a_lines) {
			int         font = 1;
			std::string t = raw;
			if (t.rfind("##", 0) == 0) {
				font = 2;
				t = t.substr(2);
				pre.push_back({ 1, "" });
			} else if (!t.empty() && t[0] == '#') {
				font = 2;
				t = t.substr(1);
				pre.push_back({ 1, "" });
				t = "== " + t + " ==";
			}
			pre.push_back({ font, t });
		}

		// Chinese text needs a font the PDF can carry: subset one of the system fonts to the
		// characters this document actually uses (a few kilobytes).
		std::set<std::uint32_t> codePoints;
		for (const auto& l : pre) {
			for (auto cp : Decode(l.text)) {
				if (cp >= 0x80) {
					codePoints.insert(cp);
				}
			}
		}
		for (auto cp : Decode(a_title)) {
			if (cp >= 0x80) {
				codePoints.insert(cp);
			}
		}
		codePoints.insert('?');  // the fallback for characters the font does not have
		for (auto cp : Decode("   -   第 %zu 页 / 共 %zu 页")) {  // the page footer, built below
			if (cp >= 0x80) {
				codePoints.insert(cp);
			}
		}
		const auto  fontFile = pdf::SystemFont();
		pdf::Subset font;
		const bool  cjk = !fontFile.empty() && pdf::BuildSubset(fontFile, codePoints, font);

		// wrap to the page width in grid columns
		std::vector<Line> lines;
		for (const auto& l : pre) {
			const std::string text = cjk ? l.text : ToAnsi(l.text);
			const auto        cps = Decode(text);
			if (cps.empty()) {
				lines.push_back({ l.font, "" });
				continue;
			}
			std::size_t i = 0;
			bool        first = true;
			while (i < cps.size()) {
				std::string part = first ? std::string() : std::string(6, ' ');
				std::size_t cols = first ? 0 : 6;
				while (i < cps.size() && cols + Columns(cps[i]) <= columns) {
					AppendUtf8(part, cps[i]);
					cols += Columns(cps[i]);
					++i;
				}
				lines.push_back({ l.font, part });
				first = false;
			}
		}
		const std::size_t pages = std::max<std::size_t>(1, (lines.size() + perPage - 1) / perPage);

		std::vector<std::string> objs;
		auto                     add = [&](std::string a_body) {
            objs.push_back(std::move(a_body));
            return static_cast<int>(objs.size());
		};
		const int idCatalog = add("");  // 1
		const int idPages = add("");    // 2
		const int idF1 = add("<< /Type /Font /Subtype /Type1 /BaseFont /Courier /Encoding /WinAnsiEncoding >>");
		const int idF2 = add("<< /Type /Font /Subtype /Type1 /BaseFont /Courier-Bold /Encoding /WinAnsiEncoding >>");
		const int idType0 = add("");    // 5: the Type0 font that carries Chinese
		const int idCid = add("");      // 6
		const int idDescr = add("");    // 7
		const int idFile = add("");     // 8
		const int idToUni = add("");    // 9

		if (cjk) {
			std::vector<std::string> pairs;  // <glyph> <code point>, for text extraction and search
			for (std::uint32_t g = 0; g < font.glyphOf.size(); ++g) {
				const auto cp = font.glyphOf[g];
				if (cp == 0) {
					continue;
				}
				char buf[32];
				std::snprintf(buf, sizeof(buf), "<%04X> <%04X>\n", static_cast<unsigned>(g), static_cast<unsigned>(cp));
				pairs.emplace_back(buf);
			}
			std::string tounicode = "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 "
									">> def\n/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
			for (std::size_t i = 0; i < pairs.size(); i += 100) {
				const auto n = std::min<std::size_t>(100, pairs.size() - i);
				tounicode += std::to_string(n) + " beginbfchar\n";
				for (std::size_t k = 0; k < n; ++k) {
					tounicode += pairs[i + k];
				}
				tounicode += "endbfchar\n";
			}
			tounicode += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
			objs[idToUni - 1] = "<< /Length " + std::to_string(tounicode.size()) + " >>\nstream\n" + tounicode + "\nendstream";

			std::string widths = "[";
			for (std::uint32_t g = 0; g < font.glyphOf.size(); ++g) {
				if (font.glyphOf[g] != 0) {
					widths += std::to_string(g) + " [" + std::to_string(font.widths[g] > 0 ? font.widths[g] : 1000) + "] ";
				}
			}
			widths += "]";

			const std::string file = Deflate(font.file);
			objs[idFile - 1] = "<< /Length " + std::to_string(file.size()) + " /Length1 " + std::to_string(font.file.size()) +
							   " /Filter /FlateDecode >>\nstream\n" + file + "\nendstream";
			objs[idDescr - 1] = Fmt("<< /Type /FontDescriptor /FontName /ReSaverCJK /Flags 4 /FontBBox [%d %d %d %d] /ItalicAngle 0 /Ascent %d /Descent "
									"%d /CapHeight %d /StemV 80 /FontFile2 %d 0 R >>",
				font.bbox[0], font.bbox[1], font.bbox[2], font.bbox[3], font.ascent, font.descent, font.ascent, idFile);
			objs[idCid - 1] = "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /ReSaverCJK /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) "
							  "/Supplement 0 >> /FontDescriptor " +
							  std::to_string(idDescr) + " 0 R /CIDToGIDMap /Identity /DW 1000 /W " + widths + " >>";
			objs[idType0 - 1] = "<< /Type /Font /Subtype /Type0 /BaseFont /ReSaverCJK /Encoding /Identity-H /DescendantFonts [" + std::to_string(idCid) +
								" 0 R] /ToUnicode " + std::to_string(idToUni) + " 0 R >>";
		}

		// one line of text: ASCII in Courier, Chinese from the subset font, both on the same grid
		auto emitText = [&](std::string& a_out, const std::string& a_text, double a_x, double a_y, bool a_bold) {
			char        buf[160];
			const auto  cps = Decode(a_text);
			std::size_t i = 0;
			if (a_bold) {
				a_out += "2 Tr 0.28 w\n";  // fake bold: fill and stroke
			}
			while (i < cps.size()) {
				if (cps[i] < 0x80 || !cjk) {
					std::string run;
					std::size_t n = 0;
					while (i < cps.size() && (cps[i] < 0x80 || !cjk)) {
						run.push_back(cps[i] < 0x80 ? static_cast<char>(cps[i]) : '?');
						++i;
						++n;
					}
					std::snprintf(buf, sizeof(buf), "/F%d %.2f Tf\n1 0 0 1 %.2f %.2f Tm\n", a_bold ? 2 : 1, kSize, a_x, a_y);
					a_out += buf;
					a_out += "(" + Esc(run) + ") Tj\n";
					a_x += static_cast<double>(n) * kCell;
				} else {
					const auto cp = cps[i++];
					auto       it = font.gid.find(cp);
					if (it == font.gid.end()) {
						it = font.gid.find('?');
					}
					const auto gid = it != font.gid.end() ? it->second : 0;
					const auto w = (gid < font.widths.size() && font.widths[gid] > 0) ? font.widths[gid] : 1000;
					// a wide glyph fills its two columns; a narrow one keeps the Latin size
					const double size = w >= 800 ? 2.0 * kCell * 1000.0 / static_cast<double>(w) : kSize;
					std::snprintf(buf, sizeof(buf), "/F3 %.2f Tf\n1 0 0 1 %.2f %.2f Tm\n<%04X> Tj\n", size, a_x, a_y, static_cast<unsigned>(gid));
					a_out += buf;
					a_x += 2.0 * kCell;
				}
			}
			if (a_bold) {
				a_out += "0 Tr\n";
			}
		};

		std::vector<int> pageIds;
		for (std::size_t pg = 0; pg < pages; ++pg) {
			std::string s = "BT\n";
			emitText(s, a_title + Fmt("   -   第 %zu 页 / 共 %zu 页", pg + 1, pages), kMargin, kMargin - 14, false);
			for (std::size_t i = 0; i < perPage; ++i) {
				const auto idx = pg * perPage + i;
				if (idx >= lines.size()) {
					break;
				}
				if (lines[idx].text.empty()) {
					continue;
				}
				emitText(s, lines[idx].text, kMargin, kH - kMargin - kLead * static_cast<double>(i + 1), lines[idx].font == 2);
			}
			s += "ET\n";
			// the page content is compressed: a big report is a few hundred pages of text
			const std::string packed = Deflate(std::vector<std::uint8_t>(s.begin(), s.end()));
			const int         contentId = packed.empty()
											? add("<< /Length " + std::to_string(s.size()) + " >>\nstream\n" + s + "\nendstream")
											: add("<< /Length " + std::to_string(packed.size()) + " /Filter /FlateDecode >>\nstream\n" + packed + "\nendstream");
			const int pageId = add("");  // filled in below: it needs the ids that follow it
			pageIds.push_back(pageId);
			objs[pageId - 1] = "<< /Type /Page /Parent " + std::to_string(idPages) + " 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 " +
							   std::to_string(idF1) + " 0 R /F2 " + std::to_string(idF2) + " 0 R" + (cjk ? " /F3 " + std::to_string(idType0) + " 0 R" : "") +
							   " >> >> /Contents " + std::to_string(contentId) + " 0 R >>";
		}
		objs[idCatalog - 1] = "<< /Type /Catalog /Pages " + std::to_string(idPages) + " 0 R >>";
		std::string kids;
		for (auto id : pageIds) {
			kids += std::to_string(id) + " 0 R ";
		}
		objs[idPages - 1] = "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pageIds.size()) + " >>";

		// a PDF text string is Latin-1, so a Chinese title goes in as UTF-16BE
		std::string titleText;
		bool        ascii = true;
		for (char c : a_title) {
			if (static_cast<unsigned char>(c) >= 0x80) {
				ascii = false;
			}
		}
		if (ascii) {
			titleText = "(" + Esc(a_title) + ")";
		} else {
			titleText = "<FEFF";
			for (auto cp : Decode(a_title)) {
				if (cp >= 0x10000) {
					cp = '?';
				}
				char buf[8];
				std::snprintf(buf, sizeof(buf), "%04X", static_cast<unsigned>(cp));
				titleText += buf;
			}
			titleText += ">";
		}
		const int idInfo = add("<< /Title " + titleText + " /Producer (ReSaver++) >>");

		// serialize with the cross-reference table
		std::string              pdf = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
		std::vector<std::size_t> offsets;
		for (std::size_t i = 0; i < objs.size(); ++i) {
			offsets.push_back(pdf.size());
			pdf += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
		}
		const auto xref = pdf.size();
		pdf += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
		for (auto o : offsets) {
			char b[32];
			std::snprintf(b, sizeof(b), "%010zu 00000 n \n", o);
			pdf += b;
		}
		pdf += "trailer\n<< /Size " + std::to_string(objs.size() + 1) + " /Root " + std::to_string(idCatalog) + " 0 R /Info " + std::to_string(idInfo) +
			   " 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
		std::ofstream f(a_path, std::ios::binary | std::ios::trunc);
		if (!f || !f.write(pdf.data(), static_cast<std::streamsize>(pdf.size()))) {
			throw std::runtime_error("无法写入 PDF 文件");
		}
	}
}
