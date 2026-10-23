// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "ModRemove.h"

#include "ByteIO.h"
#include "Ops.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace ess::modremove
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

		template <class... A>
		std::string Fmt(const char* f, A... a)
		{
			char buf[1024];
			std::snprintf(buf, sizeof(buf), f, a...);
			return buf;
		}

		// the EID a variable points at (ref / array / struct), 0 if none
		EID Target(const Variable& v)
		{
			const Variable* x = &v;
			if (x->type == VType::kVariant && x->inner) {
				x = x->inner.get();
			}
			if (x->type == VType::kRef || x->type == VType::kStruct || IsArrayType(x->type)) {
				return x->value;
			}
			return 0;
		}

		bool IsVanillaPlugin(const std::string& lower)
		{
			static const char* v[] = { "skyrim.esm", "update.esm", "dawnguard.esm", "hearthfires.esm", "dragonborn.esm" };
			for (const char* n : v) {
				if (lower == n) {
					return true;
				}
			}
			return lower.rfind("cc", 0) == 0;  // Creation Club content never depends on mods
		}
	}

	std::vector<std::string> ReadMasters(const std::filesystem::path& a_plugin)
	{
		std::ifstream f(a_plugin, std::ios::binary);
		if (!f) {
			throw std::runtime_error("无法打开 " + U8(a_plugin.filename()));
		}
		std::uint8_t hdr[24];
		if (!f.read(reinterpret_cast<char*>(hdr), 24) || std::memcmp(hdr, "TES4", 4) != 0) {
			throw std::runtime_error(U8(a_plugin.filename()) + " 不是插件文件");
		}
		std::uint32_t size;
		std::memcpy(&size, hdr + 4, 4);
		if (size > (16u << 20)) {
			throw std::runtime_error(U8(a_plugin.filename()) + "：文件头过大");
		}
		std::vector<std::uint8_t> data(size);
		f.read(reinterpret_cast<char*>(data.data()), size);
		data.resize(static_cast<std::size_t>(f.gcount()));
		std::vector<std::string> out;
		Reader                   r(data.data(), data.size());
		std::uint32_t            bigSize = 0;
		while (r.remaining() >= 6) {
			auto        t = r.bytes(4);
			std::string type(t.begin(), t.end());
			std::uint32_t len = r.u16();
			if (bigSize) {
				len = bigSize;
				bigSize = 0;
			}
			if (r.remaining() < len) {
				break;
			}
			auto body = r.bytes(len);
			if (type == "XXXX" && len == 4) {
				std::memcpy(&bigSize, body.data(), 4);
			} else if (type == "MAST") {
				std::string m(body.begin(), body.end());
				while (!m.empty() && m.back() == '\0') {
					m.pop_back();
				}
				out.push_back(Lower(m));
			}
		}
		return out;
	}

	Plan MakePlan(SaveFile& a_save, const std::string& a_mod, const ScriptSources* a_src, const Options& a_opt)
	{
		Plan plan;
		plan.name = a_mod;
		auto* p = a_save.papyrus();
		if (!p) {
			plan.blockers.push_back("存档里没有 Papyrus 区块。");
			return plan;
		}
		const bool haveSources = a_src && !a_src->empty();
		const auto lowerName = Lower(a_mod);

		// ---- what is being removed: a plugin, or a script-only mod folder ("[folder]") --------
		for (const auto* list : { &a_save.plugins.full, &a_save.plugins.lite }) {
			for (const auto& pl : *list) {
				if (Lower(pl) == lowerName) {
					plan.isPlugin = true;
				}
			}
		}
		if (plan.isPlugin) {
			if (IsVanillaPlugin(lowerName)) {
				plan.blockers.push_back(a_mod + " 属于游戏本体或 Creation Club 内容，无法移除。");
				return plan;
			}
			if (haveSources) {
				if (auto it = a_src->pluginFolder.find(lowerName); it != a_src->pluginFolder.end()) {
					plan.folder = it->second;
				}
			}
		} else if (a_mod.size() > 2 && a_mod.front() == '[' && a_mod.back() == ']') {
			plan.folder = a_mod.substr(1, a_mod.size() - 2);
		} else {
			plan.folder = a_mod;
		}
		if (!plan.isPlugin && (!haveSources || plan.folder.empty())) {
			plan.blockers.push_back(a_mod + " 在这个存档里不是插件；没有扫描过模组文件夹，它的脚本就无法识别。");
			return plan;
		}
		if (plan.folder == "Game Data" || plan.folder == "Overwrite") {
			plan.blockers.push_back("\"" + plan.folder + "\" 不是模组，无法移除。");
			return plan;
		}

		// ---- scripts that only this mod provides ------------------------------------------------
		std::unordered_set<std::string> ownLower;
		if (haveSources && plan.folder.empty() && plan.isPlugin) {
			plan.notes.push_back(a_mod + " 已不在扫描的模组文件夹里（可能已经卸载）。它在其他对象上的 "
										 "脚本无法识别，因此只移除挂在它自己对象上的内容。");
			if (a_src->ranked) {
				plan.enabledInProfile = 0;
			}
		}
		if (haveSources && !plan.folder.empty()) {
			if (!a_src->gameDataScanned) {
				plan.notes.push_back(
					"没有扫描游戏的 Data 文件夹，因此分不清它覆盖了哪些原版脚本；"
					"只移除挂在它自己对象上的脚本。");
			} else if (a_opt.otherObjects) {
				std::unordered_set<std::string> seen;
				for (const auto& s : p->scripts) {
					const auto& name = p->str(s.name);
					const auto  key = Lower(name);
					if (!seen.insert(key).second || ops::IsEngineType(name) || ops::IsNativeScriptName(name)) {
						continue;
					}
					const auto* prov = a_src->providers(name);
					if (!prov) {
						continue;
					}
					bool mine = false, others = false;
					for (const auto& pr : *prov) {
						if (pr.mod == plan.folder) {
							mine = true;
						} else if (pr.enabled) {
							others = true;
						}
					}
					if (mine && !others) {
						ownLower.insert(key);
						plan.ownScripts.insert(name);
					}
				}
			}
		} else if (!haveSources) {
			plan.notes.push_back("未扫描模组文件夹：只移除这个插件自己对象上的脚本，不检查主文件和 DLL。");
		}
		auto isOwn = [&](TStr t) { return !ownLower.empty() && ownLower.contains(Lower(p->str(t))); };

		// ---- instances ------------------------------------------------------------------------
		auto onOwnForm = [&](const RefID& id) {
			if (!plan.isPlugin || id.isZero() || id.type() == RefID::Type::kCreated) {
				return false;
			}
			return Lower(a_save.pluginOf(a_save.formIDOf(id))) == lowerName;
		};
		for (const auto& i : p->instances) {
			if (onOwnForm(i.refID)) {
				plan.instances.insert(i.id);
				++plan.instancesOnOwnForms;
			} else if (isOwn(i.script)) {
				plan.instances.insert(i.id);
				++plan.instancesElsewhere;
			}
		}
		auto removedInst = [&](const Variable& v) {
			const auto t = Target(v);
			return t && plan.instances.contains(t);
		};

		// ---- threads ---------------------------------------------------------------------------
		for (const auto& a : p->activeScripts) {
			if (!a.data) {
				continue;
			}
			bool hit = false;
			for (const auto& f : a.data->frames) {
				if (isOwn(f.scriptName) || removedInst(f.owner)) {
					hit = true;
					break;
				}
			}
			if (!hit) {
				continue;
			}
			plan.threads.insert(a.id);
			const auto& f0 = a.data->frames.front();
			if (!isOwn(f0.scriptName) && !removedInst(f0.owner)) {
				++plan.threadsOfOtherMods;  // started elsewhere, currently inside this mod
			}
		}
		// suspended stacks and queued calls: of those threads, of its scripts, or called on its
		// instances. A thread whose pending call is removed is removed with it (it would wait forever).
		auto addThread = [&](ThreadID id) {
			if (!plan.threads.contains(id) && p->findActive(id)) {
				plan.threads.insert(id);
				++plan.threadsOfOtherMods;
				return true;
			}
			return false;
		};
		for (bool changed = true; changed;) {
			changed = false;
			for (const auto* list : { &p->suspended1, &p->suspended2 }) {
				for (const auto& st : *list) {
					if (plan.stacks.contains(st.id)) {
						continue;
					}
					if (plan.threads.contains(st.id) || (st.data && (isOwn(st.data->scriptName) || removedInst(st.data->unknownVar)))) {
						plan.stacks.insert(st.id);
						changed |= addThread(st.id);
					}
				}
			}
			for (std::size_t i = 0; i < p->functionMessages.size(); ++i) {
				if (plan.messages.contains(i)) {
					continue;
				}
				const auto& m = p->functionMessages[i];
				if ((m.id && (plan.threads.contains(*m.id) || plan.stacks.contains(*m.id))) ||
					(m.data && (isOwn(m.data->scriptName) || removedInst(m.data->unknownVar)))) {
					plan.messages.insert(i);
					if (m.id) {
						changed |= addThread(*m.id);
					}
				}
			}
		}
		for (const auto& u : p->unbinds) {
			if (plan.instances.contains(u.id)) {
				plan.unbinds.insert(u.id);
			}
		}

		// ---- arrays only the removed elements use; links that other scripts lose -------------
		std::unordered_set<EID> candidates;
		auto                    collect = [&](const std::vector<Variable>& vars) {
            for (const auto& v : vars) {
                const auto t = Target(v);
                if (t && IsArrayType(v.type == VType::kVariant && v.inner ? v.inner->type : v.type)) {
                    candidates.insert(t);
                }
            }
		};
		for (const auto& i : p->instances) {
			if (plan.instances.contains(i.id) && i.data) {
				collect(i.data->vars);
			}
		}
		for (const auto& a : p->activeScripts) {
			if (plan.threads.contains(a.id) && a.data) {
				for (const auto& f : a.data->frames) {
					collect(f.vars);
				}
			}
		}
		std::unordered_set<EID> keepArrays;
		auto                    scanRemaining = [&](const std::vector<Variable>& vars, const std::string& owner) {
            for (const auto& v : vars) {
                const auto t = Target(v);
                if (!t) {
                    continue;
                }
                if (candidates.contains(t)) {
                    keepArrays.insert(t);
                }
                if (plan.instances.contains(t)) {
                    ++plan.linksFromOthers[owner];
                }
            }
		};
		for (const auto& i : p->instances) {
			if (!plan.instances.contains(i.id) && i.data) {
				scanRemaining(i.data->vars, p->str(i.script));
			}
		}
		for (const auto& r : p->references) {
			if (r.data) {
				scanRemaining(r.data->vars, p->str(r.script));
			}
		}
		for (const auto& a : p->activeScripts) {
			if (!plan.threads.contains(a.id) && a.data) {
				for (const auto& f : a.data->frames) {
					scanRemaining(f.vars, p->str(f.scriptName) + "（运行中）");
				}
			}
		}
		// arrays that stay: if they hold one of our arrays (not possible in Papyrus) or instances
		std::unordered_set<EID> removedArrays;
		for (auto id : candidates) {
			if (!keepArrays.contains(id)) {
				removedArrays.insert(id);
			}
		}
		for (const auto& a : p->arrays) {
			if (!removedArrays.contains(a.id)) {
				scanRemaining(a.vars, std::string("数组 ") + VTypeName(a.type));
			}
		}
		for (auto id : removedArrays) {
			if (p->findArray(id)) {
				plan.arrays.insert(id);
			}
		}

		// ---- change forms ----------------------------------------------------------------------
		if (plan.isPlugin) {
			for (const auto& cf : a_save.changeForms) {
				if (cf.refID.type() == RefID::Type::kCreated || cf.refID.isZero()) {
					continue;
				}
				if (Lower(a_save.pluginOf(a_save.formIDOf(cf.refID))) == lowerName) {
					plan.changeForms.insert(cf.refID.data);
					++plan.changeFormsByType[ChangeFormTypeName(cf.typeCode())];
				}
			}
		}
		if (a_opt.spawnedObjects) {
			// a spawned object goes only when every script on it is being removed
			std::unordered_map<std::uint32_t, bool> allRemoved;
			for (const auto& i : p->instances) {
				if (i.refID.type() != RefID::Type::kCreated) {
					continue;
				}
				const bool gone = plan.instances.contains(i.id);
				auto [it, fresh] = allRemoved.emplace(i.refID.data, gone);
				if (!fresh) {
					it->second = it->second && gone;
				}
			}
			for (const auto& [ref, gone] : allRemoved) {
				if (gone) {
					plan.spawnedForms.insert(ref);
				}
			}
		}

		// ---- safety checks ---------------------------------------------------------------------
		if (plan.isPlugin) {
			if (haveSources) {
				for (const auto* list : { &a_save.plugins.full, &a_save.plugins.lite }) {
					for (const auto& pl : *list) {
						const auto low = Lower(pl);
						if (low == lowerName || IsVanillaPlugin(low)) {
							continue;
						}
						const auto path = a_src->pluginPath(pl);
						if (!path) {
							++plan.uncheckedPlugins;
							continue;
						}
						try {
							const auto masters = ReadMasters(*path);
							if (std::find(masters.begin(), masters.end(), lowerName) != masters.end()) {
								plan.dependents.push_back(pl);
							}
						} catch (const std::exception&) {
							++plan.uncheckedPlugins;
						}
					}
				}
				if (!plan.dependents.empty()) {
					std::string list;
					for (const auto& d : plan.dependents) {
						list += "\n    " + d;
					}
					plan.blockers.push_back(Fmt("存档里有 %zu 个插件把 %s 当作主文件；缺少它，游戏加载时会崩溃：", plan.dependents.size(),
												a_mod.c_str()) +
											list);
				}
				if (plan.uncheckedPlugins) {
					plan.notes.push_back(Fmt("存档里有 %zu 个插件在扫描的文件夹中找不到，因此没有检查它们的主文件。",
						plan.uncheckedPlugins));
				}
			} else {
				plan.warnings.push_back("没有检查主文件（请先扫描模组文件夹）。如果有别的插件需要 " + a_mod +
										" 作为主文件，游戏加载时会崩溃。");
			}
		}
		if (haveSources && !plan.folder.empty()) {
			std::error_code ec;
			const auto      dir = a_src->folderPath(plan.folder);
			for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
				if (!e.is_directory(ec) || Lower(U8(e.path().filename())) != "skse") {
					continue;
				}
				for (const auto& s : std::filesystem::directory_iterator(e.path(), ec)) {
					if (!s.is_directory(ec) || Lower(U8(s.path().filename())) != "plugins") {
						continue;
					}
					for (const auto& d : std::filesystem::directory_iterator(s.path(), ec)) {
						if (d.is_regular_file(ec) && Lower(U8(d.path().extension())) == ".dll") {
							plan.dlls.push_back(U8(d.path().filename()));
						}
					}
				}
			}
			if (a_src->ranked) {
				plan.enabledInProfile = a_src->enabledFolders.contains(plan.folder) ? 1 : 0;
			}
		}
		if (plan.enabledInProfile == 1) {
			plan.warnings.push_back("\"" + plan.folder +
									"\" 在你的 MO2 配置里仍然启用。载入新存档前请把它禁用，否则这个模组会从头重新运行。");
		}
		if (!plan.dlls.empty()) {
			std::string list;
			for (const auto& d : plan.dlls) {
				list += (list.empty() ? "" : ", ") + d;
			}
			plan.warnings.push_back("这个模组带有 SKSE 插件（" + list +
									"）。它在 .skse 协同存档里存的东西，存档编辑器无法移除；请禁用整个模组，别让这个 DLL 被加载。");
		}
		if (!plan.threads.empty()) {
			plan.warnings.push_back(Fmt("它有 %zu 个脚本正在运行，会被中途打断。", plan.threads.size()));
		}
		if (plan.threadsOfOtherMods) {
			plan.warnings.push_back(Fmt("目前有 %zu 个线程由其他模组启动、却正运行在这个模组里；它们也会被一起移除，所以那些 "
										"模组的事件会提前结束。",
				plan.threadsOfOtherMods));
		}
		if (!plan.linksFromOthers.empty()) {
			std::vector<std::pair<std::string, std::size_t>> v(plan.linksFromOthers.begin(), plan.linksFromOthers.end());
			std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
			std::string list;
			for (std::size_t i = 0; i < v.size() && i < 8; ++i) {
				list += Fmt("\n    %s (%zu)", v[i].first.c_str(), v[i].second);
			}
			if (v.size() > 8) {
				list += Fmt("\n    …另有 %zu 个", v.size() - 8);
			}
			plan.warnings.push_back(Fmt("其他脚本里有 %zu 个变量指向它的脚本，读取时会得到 None：", [&] {
				std::size_t n = 0;
				for (auto& x : v) {
					n += x.second;
				}
				return n;
			}()) + list);
		}
		if (plan.instances.empty() && plan.threads.empty() && plan.changeForms.empty()) {
			plan.blockers.push_back("在这个存档里没有找到 " + a_mod + " 的任何内容。");
		}
		plan.notes.push_back("存档里的插件列表保持原样；加载时游戏会自己丢弃缺失插件对应的表单。");
		plan.notes.push_back("它给过玩家的物品、法术、天赋或效果，会在插件消失后由游戏丢弃；它们造成的变化无法恢复。");
		return plan;
	}

	std::string Plan::summary() const
	{
		std::string s;
		s += Fmt("移除 %s%s\n\n", name.c_str(), folder.empty() || folder == name ? "" : ("（模组文件夹：" + folder + "）").c_str());
		s += "将删除：\n";
		s += Fmt("  - %zu 个脚本实例：%zu 个在它自己的对象上，%zu 个它的脚本在其他对象上\n", instances.size(), instancesOnOwnForms,
			instancesElsewhere);
		s += Fmt("  - %zu 个运行中的线程，%zu 个挂起堆栈，%zu 个排队调用\n", threads.size(), stacks.size(), messages.size());
		if (!arrays.empty() || !unbinds.empty()) {
			s += Fmt("  - %zu 个只有它用过的数组，%zu 个排队解绑\n", arrays.size(), unbinds.size());
		}
		if (!changeForms.empty()) {
			s += Fmt("  - 其中 %zu 个表单的已保存状态：", changeForms.size());
			for (const auto& [t, n] : changeFormsByType) {
				s += Fmt(" %s %zu", t.c_str(), n);
			}
			s += "\n";
		}
		if (!spawnedForms.empty()) {
			s += Fmt("  - %zu 个只承载它脚本的生成对象\n", spawnedForms.size());
		}
		if (!ownScripts.empty()) {
			s += Fmt("  （%zu 个只有这个模组提供的脚本，无论在哪儿附加都会被移除）\n", ownScripts.size());
		}
		if (!blockers.empty()) {
			s += "\n无法移除：\n";
			for (const auto& b : blockers) {
				s += "  * " + b + "\n";
			}
		}
		if (!warnings.empty()) {
			s += "\n继续之前请先阅读：\n";
			for (const auto& w : warnings) {
				s += "  * " + w + "\n";
			}
		}
		if (!notes.empty()) {
			s += "\n说明：\n";
			for (const auto& n : notes) {
				s += "  - " + n + "\n";
			}
		}
		return s;
	}

	Result Apply(SaveFile& a_save, const Plan& a_plan)
	{
		Result r;
		auto*  p = a_save.papyrus();
		if (!p || !a_plan.canApply()) {
			return r;
		}
		// message indices first: every later step leaves functionMessages alone
		r.messages = ops::DeleteFunctionMessages(*p, a_plan.messages);
		std::set<ThreadID> stacks = a_plan.stacks;
		stacks.insert(a_plan.threads.begin(), a_plan.threads.end());
		r.stacks = ops::DeleteSuspendedStacks(*p, stacks);
		r.threads = ops::DeleteThreads(*p, a_plan.threads);
		r.instances = ops::DeleteInstances(*p, a_plan.instances);
		r.arrays = ops::DeleteArrays(*p, a_plan.arrays);
		r.unbinds = ops::DeleteUnbinds(*p, a_plan.unbinds);
		std::set<std::uint32_t> forms = a_plan.changeForms;
		forms.insert(a_plan.spawnedForms.begin(), a_plan.spawnedForms.end());
		r.changeForms = ops::DeleteChangeForms(a_save, forms);
		return r;
	}

	std::string Result::text() const
	{
		return Fmt("%zu 个实例，%zu 个线程，%zu 个挂起堆栈，%zu 个排队调用，%zu 个数组，%zu 个解绑，%zu 个变更表单", instances, threads,
			stacks, messages, arrays, unbinds, changeForms);
	}
}
