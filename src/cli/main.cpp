// ReSaver++ command line - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "core/Batch.h"
#include "core/ChangeFormData.h"
#include "core/Compare.h"
#include "core/ModRemove.h"
#include "core/Report.h"
#include "core/Pex.h"
#include "core/PluginDB.h"

#include <chrono>
#include "core/Mods.h"
#include "core/Ops.h"
#include "core/ScriptSources.h"
#include "core/Orphans.h"
#include "core/Quests.h"

#include <algorithm>
#include <fstream>
#include <memory>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ess;

static void Usage()
{
	std::puts(
		"ReSaver++ version 19 command line (Skyrim SE/AE/VR saves)\n"
		"usage: resaverpp-cli <save.ess> [actions] [-o <out.ess>]\n"
		"       resaverpp-cli <script.pex>       disassemble a compiled script\n"
		"       resaverpp-cli <script.pex> --psc decompile it to Papyrus source\n"
		"\n"
		"  (no action)               print the analysis report\n"
		"  --purge-script <name>     delete every thread, suspended stack, message and instance of a script\n"
		"  --delete-forms            with --purge-script: also delete the created objects those instances sat on\n"
		"  --keep-instances          with --purge-script: only delete its threads, suspended stacks and queued calls;\n"
		"                            keep the script's instances and data (for a mod you keep, e.g. a stuck MCM)\n"
		"  --remove-unattached       Clean > Remove unattached instances\n"
		"  --remove-undefined        Clean > Remove undefined elements (missing scripts)\n"
		"  --terminate-undefined     Clean > Terminate threads running missing scripts\n"
		"  --reset-havok             Clean > Reset Havok (zero saved physics state of object references)\n"
		"  --purify-formlists        Clean > Purify FormLists (remove null entries)\n"
		"  --batch <list.txt>        purge every script in a ReSaver batch-clean list (\"@@\" lines are skipped\n"
		"                            unless --batch-all is given)\n"
		"  --compare <other.ess>     list what changed from this save to the other one\n"
		"  --report <file.txt|.pdf>  write a report (mods with their scripts + anomalies); --report-mods or\n"
		"                            --report-anomalies limit it to one part. Uses --sources if given.\n"
		"  --fix-mcm                 fix a SkyUI MCM registration jam (stuck OnConfigManagerReady/RegisterMod work)\n"
		"  --remove-nonexistent      Clean > Remove instances on non-existent created forms (all of them)\n"
		"  --quests [text]           list the quests in the save (state, stage, journal objectives); with text,\n"
		"                            details of the matching ones. Uses --sources to read the plugins.\n"
		"  --allow-damaged           edit and write a save even though it has structural problems (risky)\n"
		"  --charged                 list enchanted / charged items in inventories (max charge, charge)\n"
		"  --fix-charges <max>       give enchanted items with max charge 0 this max charge (so they can be\n"
		"                            recharged), and lower charges above the max\n"
		"  --spawned                 list the instances on missing spawned objects by mod (installed or not);\n"
		"                            uses --sources if given\n"
		"  --remove-spawned-gone     remove the ones whose script's mod is not installed or disabled (and bare\n"
		"                            game links), keeping the ones another script still points to (needs --sources)\n"
		"  -o <file>                 write the edited save here (required when an action edits)\n"
		"  --top <n>                 rows per report section (default 20)\n"
		"  --mods                    print scripts per mod (instances / running threads)\n"
		"  --sources <folder>        with --mods: scan an MO2 mods folder (or Data) for .pex/BSA providers\n"
		"  --profile <folder>        MO2 profile folder for mod order (default: the active profile, auto-detected)\n"
		"  --remove-mod <plugin>     remove a mod from the save: its scripts, threads, queued calls and the saved\n"
		"                            state of its forms. Prints a safety check first; refuses if another loaded\n"
		"                            plugin needs it as a master. Use with --sources so masters/DLLs are checked.\n"
		"  --keep-other-objects      with --remove-mod: leave its scripts that sit on other mods' objects\n"
		"  --keep-spawned            with --remove-mod: do not delete spawned objects that only carried its scripts\n"
		"  --game <folder>           game install folder (default: from MO2's ModOrganizer.ini), for vanilla scripts\n"
		"  --names                   with --sources: read the plugins and print form-name statistics\n"
		"  --find <text>             with --sources: find forms by editor ID / name / hex form ID\n"
		"  --dry-run                 with --remove-mod: only print the safety check\n");
}

int main(int argc, char** argv)
{
	if (argc < 2 || !std::strcmp(argv[1], "-h") || !std::strcmp(argv[1], "--help")) {
		Usage();
		return argc < 2 ? 1 : 0;
	}
	std::string              in = argv[1], out;
	{
		std::string low = in;
		for (auto& c : low) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		if (low.size() > 4 && low.ends_with(".pex")) {
			try {
				const auto file = pex::ReadFile(in);
				const bool psc = argc > 2 && std::string(argv[2]) == "--psc";
				std::vector<std::string> warnings;
				for (const auto& l : psc ? pex::Decompile(file, &warnings) : pex::Disassemble(file)) {
					std::puts(l.c_str());
				}
				for (const auto& w : warnings) {
					std::fprintf(stderr, "warning: %s\n", w.c_str());
				}
				return 0;
			} catch (const std::exception& e) {
				std::fprintf(stderr, "error: %s\n", e.what());
				return 2;
			}
		}
	}
	std::vector<std::string> purge;
	bool                     fixMcm = false, delForms = false, keepInst = false, unattached = false, undefined = false, termUndef = false, nonexist = false;
	std::size_t              top = 20;
	bool                     modsView = false;
	std::string              sources, profile, removeMod, game, findText;
	bool                     names = false, havok = false, purify = false, batchAll = false, repMods = false, repAnom = false;
	std::string              batchFile, compareWith, reportFile;
	bool                     dryRun = false, spawned = false, spawnedGone = false, charged = false, allowDamaged = false;
	int                      fixCharges = 0;
	bool                     questsView = false;
	std::string              questFilter;
	modremove::Options       removeOpt;
	for (int i = 2; i < argc; ++i) {
		const std::string a = argv[i];
		auto              next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value after %s\n", a.c_str());
                std::exit(1);
            }
            return argv[++i];
		};
		if (a == "--purge-script") {
			purge.push_back(next());
		} else if (a == "--delete-forms") {
			delForms = true;
		} else if (a == "--fix-mcm") {
			fixMcm = true;
		} else if (a == "--keep-instances") {
			keepInst = true;
		} else if (a == "--remove-unattached") {
			unattached = true;
		} else if (a == "--remove-undefined") {
			undefined = true;
		} else if (a == "--terminate-undefined") {
			termUndef = true;
		} else if (a == "--remove-nonexistent") {
			nonexist = true;
		} else if (a == "--quests") {
			questsView = true;
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				questFilter = argv[++i];
			}
		} else if (a == "--allow-damaged") {
			allowDamaged = true;
		} else if (a == "--charged") {
			charged = true;
		} else if (a == "--fix-charges") {
			fixCharges = std::atoi(next().c_str());
		} else if (a == "--spawned") {
			spawned = true;
		} else if (a == "--remove-spawned-gone") {
			spawnedGone = true;
		} else if (a == "-o") {
			out = next();
		} else if (a == "--mods") {
			modsView = true;
		} else if (a == "--sources") {
			sources = next();
		} else if (a == "--profile") {
			profile = next();
		} else if (a == "--remove-mod") {
			removeMod = next();
		} else if (a == "--keep-other-objects") {
			removeOpt.otherObjects = false;
		} else if (a == "--keep-spawned") {
			removeOpt.spawnedObjects = false;
		} else if (a == "--reset-havok") {
			havok = true;
		} else if (a == "--purify-formlists") {
			purify = true;
		} else if (a == "--batch") {
			batchFile = next();
		} else if (a == "--batch-all") {
			batchAll = true;
		} else if (a == "--compare") {
			compareWith = next();
		} else if (a == "--report") {
			reportFile = next();
		} else if (a == "--report-mods") {
			repMods = true;
		} else if (a == "--report-anomalies") {
			repAnom = true;
		} else if (a == "--names") {
			names = true;
		} else if (a == "--find") {
			findText = next();
			names = true;
		} else if (a == "--game") {
			game = next();
		} else if (a == "--dry-run") {
			dryRun = true;
		} else if (a == "--top") {
			top = std::stoul(next());
		} else {
			std::fprintf(stderr, "unknown option %s\n", a.c_str());
			Usage();
			return 1;
		}
	}

	try {
		auto save = SaveFile::load(in);
		auto* p = save->papyrus();
		if (!save->problems.empty()) {
			std::fprintf(stderr, "warning: this save has structural problems; it may not be written back faithfully:\n");
			for (const auto& pr : save->problems) {
				std::fprintf(stderr, "  - %s\n", pr.c_str());
			}
		}
		const bool edits = !purge.empty() || unattached || undefined || termUndef || nonexist || !removeMod.empty() || fixMcm || havok || purify || !batchFile.empty() || spawnedGone || fixCharges > 0;
		if (edits && !save->problems.empty() && !allowDamaged) {
			std::fprintf(stderr, "error: not editing a damaged save (use --allow-damaged to write it anyway, at your own risk)\n");
			return 3;
		}
		ScriptSources src;
		if (!sources.empty() && (modsView || !removeMod.empty() || names || !reportFile.empty() || spawned || spawnedGone || questsView)) {
			std::filesystem::path profDir = profile, overwrite, gameData;
			if (auto mo2 = FindMo2Instance(sources)) {
				if (profDir.empty()) {
					profDir = mo2->profilesDir / mo2->selectedProfile;
				}
				overwrite = mo2->overwriteDir;
				if (!mo2->gamePath.empty()) {
					gameData = mo2->gamePath / "Data";
				}
				std::printf("MO2 instance %s, profile %s\n", mo2->ini.string().c_str(), profDir.string().c_str());
			}
			if (!game.empty()) {
				gameData = std::filesystem::path(game) / "Data";
			}
			const auto order = profDir.empty() ? std::vector<std::string>{} : LoadModList(profDir);
			if (auto vx = FindMo2Instance(sources) ? std::nullopt : FindVortexDeployment(sources)) {
				std::printf("Vortex: staging %s, %zu deployed mods\n", vx->stagingPath.string().c_str(), vx->mods.size());
				src.scanVortex(*vx);
			} else {
				src.scan(sources, order.empty() ? nullptr : &order, overwrite, gameData);
			}
			std::printf("scanned %zu enabled + %zu disabled mod folders%s, %zu scripts, %zu BSAs, %zu errors\n", src.modsScanned, src.disabledScanned,
				src.gameDataScanned ? " + game Data" : "", src.pexFound, src.bsasRead, src.errors.size());
		}
		if (charged) {
			const auto items = cf::FindChargedItems(*save);
			std::size_t bad = 0;
			for (const auto& c : items) {
				bad += cf::NeedsChargeFix(c.inv) ? 1 : 0;
			}
			std::printf("%zu enchanted/charged inventory item(s), %zu cannot be recharged\n", items.size(), bad);
			for (std::size_t i = 0; i < items.size() && i < top; ++i) {
				const auto& c = items[i];
				std::printf("  holder %08X  item %08X x%d  ench %08X  max %u  charge %s%s\n", c.holder, c.inv.item, c.inv.count, c.inv.ench, c.inv.maxCharge,
					c.inv.hasCharge ? std::to_string(c.inv.charge).c_str() : "(full)", cf::NeedsChargeFix(c.inv) ? "  <- NEEDS FIX" : "");
			}
			if (!edits) {
				return 0;
			}
		}
		if (spawned) {
			const auto groups = orphans::Find(*save, src.scanned() ? &src : nullptr);
			std::size_t total = 0;
			for (const auto& g : groups) {
				total += g.instances;
			}
			std::printf("%zu instance(s) on missing spawned objects in %zu group(s)\n", total, groups.size());
			for (const auto& g : groups) {
				std::printf("\n%-40s %-28s %6zu instance(s), %zu held\n", g.mod.c_str(), orphans::StatusText(g.status), g.instances, g.held);
				for (std::size_t i = 0; i < g.scripts.size() && i < top; ++i) {
					std::printf("    %-50s %6zu  held %zu\n", g.scripts[i].script.c_str(), g.scripts[i].ids.size(), g.scripts[i].held.size());
				}
				if (g.scripts.size() > top) {
					std::printf("    ... %zu more script(s)\n", g.scripts.size() - top);
				}
			}
			if (!edits) {
				return 0;
			}
		}
		if (!compareWith.empty()) {
			auto other = SaveFile::load(compareWith);
			for (const auto& l : compare::Compare(*save, *other).text()) {
				std::puts(l.c_str());
			}
			return 0;
		}
		if (!reportFile.empty()) {
			report::Options ro;
			if (repMods || repAnom) {
				ro.mods = repMods;
				ro.anomalies = repAnom;
			}
			std::unique_ptr<PluginDB> db;
			if (!src.empty()) {
				PluginDB::Input in;
				PrepareInput(*save, in);
				in.sources = src;
				PluginDB::Progress prog;
				db = std::make_unique<PluginDB>();
				db->load(in, prog);
			}
			const auto table = mods::Build(*save, src.empty() ? nullptr : &src);
			const auto lines = report::Build(*save, table, db.get(), ro, std::filesystem::path(in).filename().string());
			std::filesystem::path rp = reportFile;
			std::string ext = rp.extension().string();
			for (auto& c : ext) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			if (ext == ".pdf") {
				report::WritePdf(rp, "ReSaver++ report - " + std::filesystem::path(in).filename().string(), lines);
			} else {
				report::WriteText(rp, lines);
			}
			std::printf("report written: %s (%zu lines)\n", reportFile.c_str(), lines.size());
			return 0;
		}
		if (questsView) {
			std::unique_ptr<PluginDB> db;
			if (!src.empty()) {
				PluginDB::Input in2;
				PrepareInput(*save, in2);
				in2.sources = src;
				PluginDB::Progress prog;
				db = std::make_unique<PluginDB>();
				db->load(in2, prog);
				std::printf("plugins read %zu, quests defined %zu\n", db->pluginsRead, db->questDefs().size());
			}
			const auto  states = quests::ReadStates(*save);
			std::size_t running = 0, shown = 0;
			for (const auto& q : states) {
				running += q.running() && !q.completed() ? 1 : 0;
				shown += q.shownObjectives();
			}
			std::printf("%zu quests in the save, %zu running, %zu objectives shown in the journal\n", states.size(), running, shown);
			auto lower = [](std::string x) {
				for (auto& c : x) {
					c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				}
				return x;
			};
			const auto needle = lower(questFilter);
			std::size_t listed = 0;
			for (const auto& q : states) {
				const auto* def = db ? db->quest(q.formID) : nullptr;
				const std::string edid = def ? def->edid : "", name = def ? def->name : "";
				if (!needle.empty() && lower(edid).find(needle) == std::string::npos && lower(name).find(needle) == std::string::npos) {
					continue;
				}
				if (needle.empty() && !(q.running() && !q.completed())) {
					continue;
				}
				if (++listed > top && needle.empty()) {
					continue;
				}
				std::printf("%08X  %-32s %-30s %-24s stage %d  shown %zu", q.formID, edid.c_str(), name.c_str(), q.statusText(), q.highestDoneStage(),
					q.shownObjectives());
				if (def) {
					std::string cs;
					for (auto st : def->completionStages()) {
						cs += (cs.empty() ? "" : ",") + std::to_string(st);
					}
					std::printf("  completes at %s", cs.empty() ? "-" : cs.c_str());
				}
				std::printf("\n");
				if (!needle.empty() && def) {
					std::printf("    plugin %s, fragment script %s (%zu fragments, %zu properties)\n", db->plugins()[def->plugin].c_str(),
						def->fragmentScript.c_str(), def->fragments.size(), def->properties.size());
					for (const auto& sg : def->stages) {
						bool done = false;
						for (const auto& [st, d] : q.stages) {
							done |= st == sg.index && d;
						}
						const auto* fr = def->fragmentFor(sg.index);
						std::printf("    stage %4u %s%s%s%s\n", sg.index, done ? "[done] " : "", sg.completes() ? "[COMPLETES] " : "", sg.fails() ? "[FAILS] " : "",
							fr ? fr->function.c_str() : "");
						for (const auto& l : sg.logs) {
							if (!l.text.empty()) {
								std::printf("        \"%s\"\n", l.text.substr(0, 140).c_str());
							}
						}
					}
					for (const auto& o : def->objectives) {
						int state = 0;
						for (const auto& os : q.objectives) {
							if (os.index == o.index) {
								state = os.state;
							}
						}
						std::printf("    objective %u (%s): %s\n", o.index, quests::ObjectiveStateText(state), o.text.c_str());
					}
					for (const auto& pr : def->properties) {
						std::printf("    property %s = %s\n", pr.name.c_str(), pr.type == 1 ? db->describe(pr.form).c_str() : pr.text.c_str());
					}
					// what the completion stage's script does
					if (!def->fragmentScript.empty()) {
						try {
							if (auto px = src.findPex(def->fragmentScript)) {
								const auto file = pex::Read(ScriptSources::readPex(*px));
								const auto code = pex::Decompile(file);
								for (auto st : def->completionStages()) {
									const auto* fr = def->fragmentFor(st);
									if (!fr) {
										continue;
									}
									std::printf("    stage %u script (%s):\n", st, fr->function.c_str());
									const auto body = quests::FunctionBody(code, fr->function);
									for (const auto& l : body) {
										std::printf("        %s\n", l.c_str());
									}
									for (const auto& e : quests::ScanEffects(body)) {
										std::printf("      %s: %s\n", e.kind == quests::Effect::Kind::kReward ? "REWARD" : e.kind == quests::Effect::Kind::kQuest ? "QUEST" : "CALLS",
											e.line.c_str());
									}
								}
							} else {
								std::printf("    (fragment script %s not found)\n", def->fragmentScript.c_str());
							}
						} catch (const std::exception& e) {
							std::printf("    (fragment script: %s)\n", e.what());
						}
					}
				}
			}
			return 0;
		}
		if (names) {
			PluginDB::Input in;
			PrepareInput(*save, in);
			in.sources = src;
			PluginDB::Progress prog;
			PluginDB          db;
			const auto        t0 = std::chrono::steady_clock::now();
			db.load(in, prog);
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
			std::printf("plugins read %zu of %zu in %lld ms, %zu records kept, %zu forms, %zu missing, %zu errors\n", db.pluginsRead, db.plugins().size(),
				static_cast<long long>(ms), db.recordsRead, db.size(), db.missing.size(), db.errors.size());
			for (std::size_t i = 0; i < db.missing.size() && i < 10; ++i) {
				std::printf("  missing: %s\n", db.missing[i].c_str());
			}
			for (std::size_t i = 0; i < db.errors.size() && i < 10; ++i) {
				std::printf("  error: %s\n", db.errors[i].c_str());
			}
			for (std::uint32_t id : { 0x00000007u, 0x00000014u, 0x0001A66Au, 0x00013BBFu, 0x0003BC26u }) {
				std::printf("  %08X %s\n", id, db.describe(id).c_str());
			}
			if (!findText.empty()) {
				for (auto id : db.search(findText, 40)) {
					std::printf("  %08X %s (%s)\n", id, db.describe(id).c_str(), db.pluginName(*db.find(id)).c_str());
				}
			}
			if (p) {
				std::size_t shown = 0;
				for (const auto& i : p->instances) {
					if (shown >= 8) {
						break;
					}
					if (const auto id = save->formIDOf(i.refID); id && (id >> 24) != 0xFF) {
						const auto d = db.describe(id);
						if (!d.empty() && d.find('\'') != std::string::npos) {
							std::printf("  instance %s on %08X %s\n", p->str(i.script).c_str(), id, d.c_str());
							++shown;
						}
					}
				}
			}
			return 0;
		}
		if (!removeMod.empty()) {
			if (!p) {
				std::fputs("save has no Papyrus section\n", stderr);
				return 1;
			}
			const auto plan = modremove::MakePlan(*save, removeMod, src.empty() ? nullptr : &src, removeOpt);
			std::fputs(plan.summary().c_str(), stdout);
			if (!plan.canApply()) {
				std::fputs("\nnot removed.\n", stdout);
				return 3;
			}
			if (dryRun) {
				return 0;
			}
			std::error_code ec;
			if (out.empty() || std::filesystem::equivalent(in, out, ec)) {
				std::fputs("\n--remove-mod writes a new file: give -o <new.ess> (not the input file)\n", stderr);
				return 1;
			}
			std::printf("\nremoved: %s\n", modremove::Apply(*save, plan).text().c_str());
		}
		if (modsView) {
			auto t = mods::Build(*save, src.empty() ? nullptr : &src);
			std::vector<const mods::ModRow*> rows;
			for (const auto& r : t.rows) {
				if (r.scriptCount || r.threadCount || !r.provided.empty()) {
					rows.push_back(&r);
				}
			}
			std::sort(rows.begin(), rows.end(), [](auto a, auto b) { return a->threadCount != b->threadCount ? a->threadCount > b->threadCount : a->instanceCount > b->instanceCount; });
			std::printf("%-50s %8s %10s %8s %9s\n", "mod", "scripts", "instances", "running", "provides");
			for (std::size_t i = 0; i < rows.size() && i < top; ++i) {
				const auto* r = rows[i];
				std::printf("%-50.50s %8zu %10zu %8zu %9zu\n", r->name.c_str(), r->scriptCount, r->instanceCount, r->threadCount, r->providedInSave);
			}
			return 0;
		}
		if (!edits) {
			std::fputs(ops::FormatReport(ops::Analyze(*save), top).c_str(), stdout);
			return 0;
		}
		if (out.empty()) {
			std::fputs("an output file (-o) is required when editing\n", stderr);
			return 1;
		}
		if (!p) {
			std::fputs("save has no Papyrus section\n", stderr);
			return 1;
		}
		for (const auto& name : purge) {
			const auto r = ops::PurgeScript(*save, name, delForms, keepInst);
			std::printf("purge %s: %zu threads, %zu suspended stacks, %zu messages, %zu instances, %zu created forms\n", name.c_str(), r.threads,
				r.stacks, r.messages, r.instances, r.forms);
		}
		if (!batchFile.empty()) {
			std::ifstream bf(batchFile);
			std::string   list((std::istreambuf_iterator<char>(bf)), std::istreambuf_iterator<char>());
			auto          matches = batch::Resolve(*save, list);
			for (auto& mt : matches) {
				if (batchAll) {
					mt.selected = true;
				}
				std::printf("  %s %s (%zu instances, %zu threads)%s\n", mt.selected ? "purge" : "skip ", mt.script.c_str(), mt.instances, mt.threads,
					mt.prompt.empty() ? "" : ("  @@ " + mt.prompt).c_str());
			}
			const auto br = batch::Apply(*save, matches);
			std::printf("batch clean: %zu scripts, %zu threads, %zu stacks, %zu messages, %zu instances\n", br.scripts, br.threads, br.stacks,
				br.messages, br.instances);
		}
		if (havok) {
			const auto hr = cf::ResetHavok(*save);
			std::printf("reset havok: %zu references reset, %zu could not be decoded (left alone)\n", hr.reset, hr.failed);
		}
		if (purify) {
			const auto pr = cf::PurifyFormLists(*save);
			std::printf("purify formlists: %zu null entries removed from %zu lists (%zu lists not decodable)\n", pr.entries, pr.forms, pr.failed);
		}
		if (fixMcm) {
			std::printf("fixed MCM jam: %s\n", ops::FixMcmJam(*p).text().c_str());
		}
		if (termUndef) {
			std::printf("terminated undefined threads: %zu\n", ops::TerminateUndefinedThreads(*p));
		}
		if (undefined) {
			std::printf("removed undefined elements: %zu\n", ops::RemoveUndefinedElements(*p));
		}
		if (unattached) {
			std::printf("removed unattached instances: %zu\n", ops::RemoveUnattachedInstances(*p));
		}
		if (nonexist) {
			std::printf("removed instances on non-existent created forms: %zu\n", ops::RemoveNonexistentCreated(*save));
		}
		if (fixCharges > 0) {
			std::printf("fixed charges of %zu item(s)\n", cf::FixCharges(*save, static_cast<std::uint16_t>(std::min(fixCharges, 65535)), {}));
		}
		if (spawnedGone) {
			if (!src.scanned()) {
				std::fprintf(stderr, "--remove-spawned-gone needs --sources (to tell which mods are installed)\n");
				return 1;
			}
			std::set<EID> ids;
			for (const auto& g : orphans::Find(*save, &src)) {
				if (g.status != orphans::Status::kNotInstalled && g.status != orphans::Status::kDisabled && g.status != orphans::Status::kEngine) {
					continue;
				}
				for (const auto& sg : g.scripts) {
					for (EID id : sg.ids) {
						if (!sg.held.contains(id)) {
							ids.insert(id);
						}
					}
				}
			}
			std::printf("removed instances on missing spawned objects of mods no longer installed: %zu\n", orphans::Remove(*save, ids));
		}
		{
			std::error_code ec;
			const auto      wr = save->write(out, std::filesystem::exists(out, ec));
			if (!wr.backup.empty()) {
				std::printf("previous file kept as %s%s\n", wr.backup.filename().string().c_str(),
					wr.cosaveBackup.empty() ? "" : (" + " + wr.cosaveBackup.filename().string()).c_str());
			}
		}
		// verify by reading it back
		auto check = SaveFile::load(out);
		std::printf("written %s and verified (re-read OK: %zu instances, %zu threads)\n", out.c_str(), check->papyrus()->instances.size(),
			check->papyrus()->activeScripts.size());
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 2;
	}
	return 0;
}
