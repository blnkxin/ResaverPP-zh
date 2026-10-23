// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// 本地化修改：界面、提示与报告文字改为简体中文（详见 README.zh-CN.md）。
#include "Compare.h"
#include "Text.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

namespace ess::compare
{
	namespace
	{
		template <class... A>
		std::string Fmt(const char* f, A... a)
		{
			char buf[512];
			std::snprintf(buf, sizeof(buf), f, a...);
			return buf;
		}

		std::vector<Delta> Diff(const std::map<std::string, long long>& a, const std::map<std::string, long long>& b, std::size_t top)
		{
			std::set<std::string> keys;
			for (const auto& [k, v] : a) {
				keys.insert(k);
			}
			for (const auto& [k, v] : b) {
				keys.insert(k);
			}
			std::vector<Delta> out;
			for (const auto& k : keys) {
				const auto ia = a.find(k), ib = b.find(k);
				Delta      d{ k, ia == a.end() ? 0 : ia->second, ib == b.end() ? 0 : ib->second };
				if (d.diff()) {
					out.push_back(d);
				}
			}
			std::sort(out.begin(), out.end(), [](const Delta& x, const Delta& y) {
				return std::llabs(x.diff()) != std::llabs(y.diff()) ? std::llabs(x.diff()) > std::llabs(y.diff()) : x.name < y.name;
			});
			if (out.size() > top) {
				out.resize(top);
			}
			return out;
		}

		std::map<std::string, long long> InstancesByScript(const Papyrus* p)
		{
			std::map<std::string, long long> m;
			if (p) {
				for (const auto& i : p->instances) {
					m[p->str(i.script)]++;
				}
			}
			return m;
		}

		std::map<std::string, long long> ThreadsByScript(const Papyrus* p)
		{
			std::map<std::string, long long> m;
			if (p) {
				for (const auto& a : p->activeScripts) {
					m[a.data && !a.data->frames.empty() ? p->str(a.data->frames.front().scriptName) : std::string("<无栈帧>")]++;
				}
			}
			return m;
		}
	}

	Result Compare(const SaveFile& a_a, const SaveFile& a_b, std::size_t a_top)
	{
		Result r;
		r.identical = a_a.buildBody() == a_b.buildBody();
		const auto& ha = a_a.header;
		const auto& hb = a_b.header;
		r.header.push_back(Fmt("A：存档 #%u  %s，等级 %u，%s，%s", ha.saveNumber, text::ToUtf8(ha.playerName).c_str(), ha.playerLevel,
			text::ToUtf8(ha.location).c_str(), text::ToUtf8(ha.gameDate).c_str()));
		r.header.push_back(Fmt("B：存档 #%u  %s，等级 %u，%s，%s", hb.saveNumber, text::ToUtf8(hb.playerName).c_str(), hb.playerLevel,
			text::ToUtf8(hb.location).c_str(), text::ToUtf8(hb.gameDate).c_str()));

		auto plugSet = [](const SaveFile& s) {
			std::set<std::string> v(s.plugins.full.begin(), s.plugins.full.end());
			v.insert(s.plugins.lite.begin(), s.plugins.lite.end());
			return v;
		};
		const auto pa = plugSet(a_a), pb = plugSet(a_b);
		for (const auto& x : pb) {
			if (!pa.contains(x)) {
				r.pluginsAdded.push_back(x);
			}
		}
		for (const auto& x : pa) {
			if (!pb.contains(x)) {
				r.pluginsRemoved.push_back(x);
			}
		}

		const auto* A = a_a.papyrus();
		const auto* B = a_b.papyrus();
		auto        count = [](const Papyrus* p, int which) -> long long {
            if (!p) {
                return 0;
            }
            switch (which) {
            case 0: return static_cast<long long>(p->scripts.size());
            case 1: return static_cast<long long>(p->instances.size());
            case 2: return static_cast<long long>(p->arrays.size());
            case 3: return static_cast<long long>(p->activeScripts.size());
            case 4: return static_cast<long long>(p->suspended1.size() + p->suspended2.size());
            case 5: return static_cast<long long>(p->functionMessages.size());
            case 6: return static_cast<long long>(p->strings.size());
            default: return 0;
            }
		};
		const char* names[] = { "脚本", "脚本实例", "数组", "运行中的线程", "挂起堆栈", "函数消息", "字符串" };
		for (int i = 0; i < 7; ++i) {
			r.totals.push_back({ names[i], count(A, i), count(B, i) });
		}
		r.totals.push_back({ "变更表单", static_cast<long long>(a_a.changeForms.size()), static_cast<long long>(a_b.changeForms.size()) });
		r.totals.push_back({ "主体大小（字节）", static_cast<long long>(a_a.buildBody().size()), static_cast<long long>(a_b.buildBody().size()) });

		r.instancesByScript = Diff(InstancesByScript(A), InstancesByScript(B), a_top);
		r.threadsByScript = Diff(ThreadsByScript(A), ThreadsByScript(B), a_top);
		std::map<std::string, long long> ca, cb;
		for (const auto& c : a_a.changeForms) {
			ca[ChangeFormTypeName(c.typeCode())]++;
		}
		for (const auto& c : a_b.changeForms) {
			cb[ChangeFormTypeName(c.typeCode())]++;
		}
		r.changeFormsByType = Diff(ca, cb, a_top);
		return r;
	}

	std::vector<std::string> Result::text() const
	{
		std::vector<std::string> L = header;
		L.push_back(identical ? "两个存档的内容完全相同。" : "");
		auto deltas = [&](const char* title, const std::vector<Delta>& v, bool all) {
			L.push_back(title);
			std::size_t shown = 0;
			for (const auto& d : v) {
				if (!all && !d.diff()) {
					continue;
				}
				L.push_back(Fmt("  %-48s %10lld -> %-10lld (%+lld)", d.name.c_str(), d.a, d.b, d.diff()));
				++shown;
			}
			if (!shown) {
				L.push_back("  （无变化）");
			}
			L.push_back("");
		};
		deltas("总计（A -> B）：", totals, true);
		if (!pluginsAdded.empty() || !pluginsRemoved.empty()) {
			L.push_back("插件：");
			for (const auto& p : pluginsAdded) {
				L.push_back("  + " + p);
			}
			for (const auto& p : pluginsRemoved) {
				L.push_back("  - " + p);
			}
			L.push_back("");
		}
		deltas("按脚本统计的运行中线程（变化最大）：", threadsByScript, false);
		deltas("按脚本统计的脚本实例（变化最大）：", instancesByScript, false);
		deltas("按类型统计的变更表单：", changeFormsByType, false);
		return L;
	}
}
