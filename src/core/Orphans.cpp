// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// 本地化修改：界面、提示与报告文字改为简体中文（详见 README.zh-CN.md）。
#include "Orphans.h"

#include "Ops.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace ess::orphans
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

		int Rank(Status s)
		{
			switch (s) {
			case Status::kNotInstalled: return 0;
			case Status::kDisabled: return 1;
			case Status::kEngine: return 2;
			case Status::kUnknown: return 3;
			default: return 4;
			}
		}

		// marks every orphan id a variable points to (a variant's payload included), unless it is the holder itself
		void Mark(const Variable& v, const std::set<EID>& orphans, EID self, std::set<EID>& held)
		{
			const Variable* x = &v;
			if (x->type == VType::kVariant && x->inner) {
				x = x->inner.get();
			}
			if (x->type == VType::kRef && x->value != self && orphans.contains(x->value)) {
				held.insert(x->value);
			}
		}

		void MarkAll(const std::vector<Variable>& vars, const std::set<EID>& orphans, EID self, std::set<EID>& held)
		{
			for (const auto& v : vars) {
				Mark(v, orphans, self, held);
			}
		}
	}

	const char* StatusText(Status a_status)
	{
		switch (a_status) {
		case Status::kNotInstalled: return "未安装";
		case Status::kDisabled: return "已安装，已禁用";
		case Status::kInstalled: return "已安装";
		case Status::kEngine: return "游戏对象链接，无脚本";
		default: return "未知";
		}
	}

	std::vector<Group> Find(SaveFile& a_save, const ScriptSources* a_sources)
	{
		std::vector<Group> out;
		auto*              p = a_save.papyrus();
		if (!p) {
			return out;
		}
		const bool scanned = a_sources && a_sources->scanned();

		// script name (lowercase) -> its instances
		std::map<std::string, ScriptGroup> byScript;
		std::map<std::string, TStr>        nameIndex;
		std::set<EID>                      orphans;
		for (const auto& i : p->instances) {
			if (!ops::IsCreatedWithoutForm(a_save, i)) {
				continue;
			}
			auto& g = byScript[Lower(p->str(i.script))];
			if (g.script.empty()) {
				g.script = p->str(i.script);
				nameIndex[Lower(g.script)] = i.script;
			}
			g.ids.insert(i.id);
			orphans.insert(i.id);
		}
		if (orphans.empty()) {
			return out;
		}

		// what still points to them: other instances' and references' variables, arrays, and running
		// or queued calls
		std::set<EID> held;
		for (const auto& i : p->instances) {
			if (i.data) {
				MarkAll(i.data->vars, orphans, i.id, held);
			}
		}
		for (const auto& r : p->references) {
			if (r.data) {
				MarkAll(r.data->vars, orphans, r.id, held);
			}
		}
		for (const auto& a : p->arrays) {
			MarkAll(a.vars, orphans, 0, held);
		}
		for (const auto& a : p->activeScripts) {
			if (!a.data) {
				continue;
			}
			for (const auto& f : a.data->frames) {
				Mark(f.owner, orphans, 0, held);
				MarkAll(f.vars, orphans, 0, held);
			}
		}
		for (const auto* list : { &p->suspended1, &p->suspended2 }) {
			for (const auto& st : *list) {
				if (st.data) {
					Mark(st.data->unknownVar, orphans, 0, held);
					MarkAll(st.data->vars, orphans, 0, held);
				}
			}
		}
		for (const auto& m : p->functionMessages) {
			if (m.data) {
				Mark(m.data->unknownVar, orphans, 0, held);
				MarkAll(m.data->vars, orphans, 0, held);
			}
		}

		// group by the mod that provides each script
		std::map<std::pair<int, std::string>, Group> groups;  // (rank, lowercase mod)
		for (auto& [key, sg] : byScript) {
			for (EID id : sg.ids) {
				if (held.contains(id)) {
					sg.held.insert(id);
				}
			}
			Status      status = Status::kUnknown;
			std::string mod;
			const bool  undefined = ops::IsInstanceUndefined(*p, nameIndex[key]);
			const auto* prov = scanned ? a_sources->providers(sg.script) : nullptr;
			if (ops::IsEngineType(sg.script) || ops::IsNativeScriptName(sg.script)) {
				status = Status::kEngine;
				mod = "（游戏对象类型）";
			} else if (prov && !prov->empty()) {
				const auto it = std::find_if(prov->begin(), prov->end(), [](const ScriptProvider& x) { return x.enabled; });
				status = it != prov->end() ? Status::kInstalled : Status::kDisabled;
				mod = it != prov->end() ? it->mod : prov->front().mod;
			} else if (undefined || (scanned && a_sources->gameDataScanned)) {
				status = Status::kNotInstalled;
				mod = "（找不到脚本文件）";
			} else {
				status = Status::kUnknown;
				mod = scanned ? "（不在已扫描的模组里）" : "（未扫描模组）";
			}
			auto& g = groups[{ Rank(status), Lower(mod) }];
			g.mod = mod;
			g.status = status;
			g.instances += sg.ids.size();
			g.held += sg.held.size();
			g.scripts.push_back(std::move(sg));
		}
		for (auto& [k, g] : groups) {
			std::sort(g.scripts.begin(), g.scripts.end(), [](const ScriptGroup& a, const ScriptGroup& b) { return a.ids.size() > b.ids.size(); });
			out.push_back(std::move(g));
		}
		return out;
	}

	std::size_t Remove(SaveFile& a_save, const std::set<EID>& a_ids)
	{
		auto* p = a_save.papyrus();
		return p && !a_ids.empty() ? ops::DeleteInstances(*p, a_ids) : 0;
	}
}
