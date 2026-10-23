// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Mods.h"

#include <algorithm>

#include "Ops.h"

#include <cctype>
#include <unordered_map>

namespace ess::mods
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
	}

	Table Build(SaveFile& a_save, const ScriptSources* a_sources)
	{
		Table t;
		t.hasSources = a_sources && !a_sources->empty();
		auto* p = a_save.papyrus();
		if (!p) {
			return t;
		}

		std::unordered_map<std::string, std::size_t> rowOf;  // lowercase name -> row
		auto row = [&](const std::string& name, Kind kind, int index) -> ModRow& {
			const auto key = Lower(name);
			auto       it = rowOf.find(key);
			if (it != rowOf.end()) {
				return t.rows[it->second];
			}
			rowOf.emplace(key, t.rows.size());
			ModRow r;
			r.name = name;
			r.kind = kind;
			r.index = index;
			t.rows.push_back(std::move(r));
			return t.rows.back();
		};

		for (std::size_t i = 0; i < a_save.plugins.full.size(); ++i) {
			row(a_save.plugins.full[i], Kind::kPlugin, static_cast<int>(i));
		}
		for (std::size_t i = 0; i < a_save.plugins.lite.size(); ++i) {
			row(a_save.plugins.lite[i], Kind::kLight, static_cast<int>(i));
		}
		const std::string kCreatedName = "（游戏中生成）";
		const std::string kUnattachedName = "（未附加到任何对象）";
		const std::string kGlobalName = "（无对象／未知模组）";
		row(kCreatedName, Kind::kCreated, -1);
		row(kUnattachedName, Kind::kUnattached, -1);
		row(kGlobalName, Kind::kGlobal, -1);

		auto providerOf = [&](const std::string& script) -> std::string {
			if (!t.hasSources) {
				return {};
			}
			const auto* v = a_sources->providers(script);
			if (!v || v->empty()) {
				return {};
			}
			return a_sources->ranked ? v->front().mod : v->back().mod;  // ranked: front() is the copy the game loads
		};

		// the row a mod folder maps to: its first plugin if it has one, else the folder itself
		std::unordered_map<std::string, std::size_t> folderRow;  // folder -> row index
		if (t.hasSources) {
			for (const auto& [plugin, folder] : a_sources->pluginFolder) {
				if (auto it = rowOf.find(plugin); it != rowOf.end()) {
					folderRow.emplace(folder, it->second);
				}
			}
		}
		auto rowForFolder = [&](const std::string& folder) -> ModRow* {
			// vanilla scripts say nothing about which mod spawned an object
			if (folder.empty() || folder == "Game Data") {
				return nullptr;
			}
			if (auto it = folderRow.find(folder); it != folderRow.end()) {
				return &t.rows[it->second];
			}
			auto& r = row("[" + folder + "]", Kind::kFolder, -1);
			const auto idx = static_cast<std::size_t>(&r - t.rows.data());
			folderRow.emplace(folder, idx);
			return &t.rows[idx];
		};

		auto rowForForm = [&](const RefID& id) -> ModRow& {
			if (id.isZero()) {
				return row(kUnattachedName, Kind::kUnattached, -1);
			}
			if (id.type() == RefID::Type::kCreated) {
				return row(kCreatedName, Kind::kCreated, -1);
			}
			const auto fid = a_save.formIDOf(id);
			const auto plugin = a_save.pluginOf(fid);
			if (auto it = rowOf.find(Lower(plugin)); it != rowOf.end()) {
				return t.rows[it->second];
			}
			return row(kGlobalName, Kind::kGlobal, -1);
		};

		// Objects spawned during play belong to whichever mod spawned them; the best evidence
		// for that is the mod that ships the script, so credit it when sources were scanned.
		auto rowForInstance = [&](const ScriptInstance& i, const std::string& script) -> ModRow& {
			if (i.refID.type() == RefID::Type::kCreated && t.hasSources) {
				if (auto* r = rowForFolder(providerOf(script))) {
					return *r;
				}
			}
			return rowForForm(i.refID);
		};

		for (const auto& i : p->instances) {
			auto&       r = rowForInstance(i, p->str(i.script));
			const auto& name = p->str(i.script);
			auto&       sr = r.scripts[Lower(name)];
			if (sr.script.empty()) {
				sr.script = name;
				sr.provider = providerOf(name);
			}
			sr.instances.insert(i.id);
		}

		for (const auto& a : p->activeScripts) {
			if (!a.data || a.data->frames.empty()) {
				continue;
			}
			const auto& f0 = a.data->frames.front();
			const auto& name = p->str(f0.scriptName);
			ModRow*     r = nullptr;
			if (f0.owner.type == VType::kRef && f0.owner.value != 0) {
				if (const auto* inst = p->findInstance(f0.owner.value)) {
					r = &rowForInstance(*inst, name);
				}
			}
			if (!r && t.hasSources) {
				r = rowForFolder(providerOf(name));
			}
			if (!r) {
				r = &row(kGlobalName, Kind::kGlobal, -1);
			}
			auto& sr = r->scripts[Lower(name)];
			if (sr.script.empty()) {
				sr.script = name;
				sr.provider = providerOf(name);
			}
			sr.threads.insert(a.id);
		}

		if (t.hasSources) {
			// every script each loaded plugin's mod ships, in the save or not (e.g. magic effect scripts that
			// only exist while the effect is active)
			for (const auto& [folder, names] : a_sources->scriptsByMod) {
				if (auto it = folderRow.find(folder); it != folderRow.end()) {
					auto& sh = t.rows[it->second].shipped;
					sh.insert(sh.end(), names.begin(), names.end());
				}
			}
			for (auto& r : t.rows) {
				std::sort(r.shipped.begin(), r.shipped.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
			}
			// which scripts present in this save each mod provides
			std::set<std::string> seen;
			for (const auto& s : p->scripts) {
				const auto& name = p->str(s.name);
				if (!seen.insert(Lower(name)).second) {
					continue;
				}
				if (auto* r = rowForFolder(providerOf(name))) {
					r->provided.insert(name);
				}
			}
		}

		for (auto& r : t.rows) {
			for (auto& [k, s] : r.scripts) {
				s.engineType = ops::IsEngineType(s.script);
				r.threadCount += s.threads.size();
				if (!s.engineType) {
					++r.scriptCount;
					r.instanceCount += s.instances.size();
				}
			}
			r.providedInSave = r.provided.size();
		}
		return t;
	}
}
