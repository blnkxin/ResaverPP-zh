// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Extras.h"

#include "ByteIO.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace ess::extras
{
	namespace
	{
		std::int64_t ToUnix(std::filesystem::file_time_type a_t)
		{
			const auto sys = std::chrono::time_point_cast<std::chrono::seconds>(a_t - std::filesystem::file_time_type::clock::now() +
																			   std::chrono::system_clock::now());
			return sys.time_since_epoch().count();
		}
	}

	SaveInfo ReadSaveInfo(const std::filesystem::path& a_ess)
	{
		SaveInfo info;
		info.path = a_ess;
		std::error_code ec;
		info.size = std::filesystem::file_size(a_ess, ec);
		if (const auto t = std::filesystem::last_write_time(a_ess, ec); !ec) {
			info.modified = ToUnix(t);
		}
		auto co = a_ess;
		co.replace_extension(".skse");
		if (std::filesystem::exists(co, ec)) {
			info.cosaveSize = std::filesystem::file_size(co, ec);
		}
		std::ifstream f(a_ess, std::ios::binary);
		std::vector<std::uint8_t> buf(4096);
		f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
		buf.resize(static_cast<std::size_t>(f.gcount()));
		try {
			Reader r(buf.data(), buf.size());
			if (buf.size() < 17 || std::memcmp(buf.data(), "TESV_SAVEGAME", 13) != 0) {
				return info;
			}
			r.seek(17);
			const auto version = r.u32();
			info.le = version < 12;
			info.saveNumber = r.u32();
			info.playerName = r.wstring();
			info.playerLevel = r.u32();
			info.location = r.wstring();
			r.wstring();  // game date
			info.race = r.wstring();
			info.ok = true;
		} catch (const std::exception&) {
		}
		return info;
	}

	std::vector<SaveInfo> ListSaves(const std::filesystem::path& a_folder)
	{
		std::vector<SaveInfo> out;
		std::error_code       ec;
		for (std::filesystem::directory_iterator it(a_folder, ec), end; !ec && it != end; it.increment(ec)) {
			if (!it->is_regular_file(ec)) {
				continue;
			}
			auto ext = it->path().extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (ext == ".ess") {
				out.push_back(ReadSaveInfo(it->path()));
			}
		}
		std::sort(out.begin(), out.end(), [](const SaveInfo& a, const SaveInfo& b) { return a.modified > b.modified; });
		return out;
	}

	// ---- history ------------------------------------------------------------------------------

	SaveStat ComputeStat(const SaveFile& a_save, const std::filesystem::path& a_path)
	{
		SaveStat s;
		s.file = a_path.filename().string();
		s.character = a_save.header.playerName;
		s.saveNumber = a_save.header.saveNumber;
		std::error_code ec;
		s.size = std::filesystem::file_size(a_path, ec);
		if (const auto t = std::filesystem::last_write_time(a_path, ec); !ec) {
			s.modified = ToUnix(t);
		}
		s.changeForms = a_save.changeForms.size();
		for (const auto& c : a_save.changeForms) {
			s.created += c.refID.type() == RefID::Type::kCreated ? 1 : 0;
		}
		if (const auto* p = a_save.papyrus()) {
			s.instances = p->instances.size();
			s.threads = p->activeScripts.size();
			s.arrays = p->arrays.size();
			for (const auto& i : p->instances) {
				if (i.refID.isZero()) {
					continue;
				}
				const auto plugin = i.refID.type() == RefID::Type::kCreated ? std::string("Created") : a_save.pluginOf(a_save.formIDOf(i.refID));
				++s.instancesByPlugin[plugin];
			}
		}
		return s;
	}

	std::string FormatStat(const SaveStat& a)
	{
		std::ostringstream o;
		o << a.file << '\t' << a.character << '\t' << a.saveNumber << '\t' << a.modified << '\t' << a.size << '\t' << a.instances << '\t'
		  << a.threads << '\t' << a.created << '\t' << a.changeForms << '\t' << a.arrays << '\t';
		// the plugins with the most instances (the file stays small)
		std::vector<std::pair<std::string, std::size_t>> v(a.instancesByPlugin.begin(), a.instancesByPlugin.end());
		std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
		for (std::size_t i = 0; i < v.size() && i < 60; ++i) {
			o << (i ? "|" : "") << v[i].first << '=' << v[i].second;
		}
		return o.str();
	}

	bool ParseStat(const std::string& a_line, SaveStat& a)
	{
		std::vector<std::string> f;
		std::string              cur;
		for (char c : a_line) {
			if (c == '\t') {
				f.push_back(cur);
				cur.clear();
			} else if (c != '\r') {
				cur += c;
			}
		}
		f.push_back(cur);
		if (f.size() < 11) {
			return false;
		}
		try {
			a.file = f[0];
			a.character = f[1];
			a.saveNumber = static_cast<std::uint32_t>(std::stoul(f[2]));
			a.modified = std::stoll(f[3]);
			a.size = std::stoull(f[4]);
			a.instances = std::stoul(f[5]);
			a.threads = std::stoul(f[6]);
			a.created = std::stoul(f[7]);
			a.changeForms = std::stoul(f[8]);
			a.arrays = std::stoul(f[9]);
			std::stringstream ss(f[10]);
			std::string       kv;
			while (std::getline(ss, kv, '|')) {
				const auto eq = kv.rfind('=');
				if (eq != std::string::npos) {
					a.instancesByPlugin[kv.substr(0, eq)] = std::stoul(kv.substr(eq + 1));
				}
			}
		} catch (const std::exception&) {
			return false;
		}
		return true;
	}

	std::vector<SaveStat> LoadHistory(const std::filesystem::path& a_file)
	{
		std::vector<SaveStat> out;
		std::ifstream         f(a_file);
		std::string           line;
		while (std::getline(f, line)) {
			SaveStat s;
			if (ParseStat(line, s)) {
				out.push_back(std::move(s));
			}
		}
		return out;
	}

	void SaveHistory(const std::filesystem::path& a_file, const std::vector<SaveStat>& a_all)
	{
		std::ofstream f(a_file, std::ios::trunc);
		for (const auto& s : a_all) {
			f << FormatStat(s) << '\n';
		}
	}

	void AddToHistory(std::vector<SaveStat>& a_all, const SaveStat& a_stat)
	{
		for (auto& s : a_all) {
			if (s.file == a_stat.file && s.character == a_stat.character && s.saveNumber == a_stat.saveNumber) {
				s = a_stat;
				return;
			}
		}
		a_all.push_back(a_stat);
	}

	std::vector<Growth> FindGrowth(const std::vector<SaveStat>& a)
	{
		std::vector<Growth> out;
		if (a.size() < 4) {
			return out;
		}
		// over the last saves (up to 10): the count never drops and grows by a lot
		const std::size_t from = a.size() > 10 ? a.size() - 10 : 0;
		std::map<std::string, bool> plugins;
		for (std::size_t i = from; i < a.size(); ++i) {
			for (const auto& [p, n] : a[i].instancesByPlugin) {
				plugins[p] = true;
			}
		}
		for (const auto& [p, _] : plugins) {
			bool        never = true;
			std::size_t prev = 0, first = 0, last = 0, rises = 0;
			for (std::size_t i = from; i < a.size(); ++i) {
				const auto it = a[i].instancesByPlugin.find(p);
				const std::size_t n = it == a[i].instancesByPlugin.end() ? 0 : it->second;
				if (i == from) {
					first = n;
				} else if (n < prev) {
					never = false;
					break;
				} else if (n > prev) {
					++rises;
				}
				prev = last = n;
			}
			if (never && last >= first + 200 && last >= first * 3 / 2 && rises >= 2) {
				out.push_back({ p, first, last, a.size() - from });
			}
		}
		std::sort(out.begin(), out.end(), [](const Growth& x, const Growth& y) { return x.last - x.first > y.last - y.first; });
		return out;
	}

	// ---- Papyrus log --------------------------------------------------------------------------

	LogSummary ReadPapyrusLog(const std::filesystem::path& a_file)
	{
		LogSummary s;
		s.file = a_file;
		std::ifstream f(a_file);
		if (!f) {
			return s;
		}
		s.ok = true;
		// "[10/27/2026 - 01:02:03PM] error: Cannot call X() on a None object, aborting function call"
		// followed by stack lines "\t[ (FF001234)].ScriptName.Function() - "Script.psc" Line 12"
		static const std::regex head(R"(^\[[^\]]*\]\s*(error|warning):\s*(.*)$)", std::regex::icase);
		static const std::regex frame(R"(^\s+\[[^\]]*\]\.([A-Za-z0-9_:]+)\.([A-Za-z0-9_]+)\(\))");
		static const std::regex frame2(R"(^\s+([A-Za-z0-9_:]+)\.([A-Za-z0-9_]+)\(\))");
		static const std::regex numbers(R"(\(([0-9A-Fa-f]{8})\)|0x[0-9A-Fa-f]+|\b[0-9]+(\.[0-9]+)?\b)");
		std::unordered_map<std::string, std::size_t> index;  // script + message -> group
		std::string line, kind, msg, example, script;
		bool        inEntry = false;
		auto        flush = [&] {
            if (!inEntry) {
                return;
            }
            auto norm = std::regex_replace(msg, numbers, "#");
            const auto key = script + "\x1f" + norm;
            auto it = index.find(key);
            if (it == index.end()) {
                it = index.emplace(key, s.groups.size()).first;
                LogGroup g;
                g.script = script;
                g.message = (kind == "warning" ? "warning: " : "") + norm;
                g.example = example;
                s.groups.push_back(std::move(g));
            }
            ++s.groups[it->second].count;
            if (!script.empty()) {
                ++s.byScript[script];
            }
            inEntry = false;
		};
		while (std::getline(f, line)) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			++s.lines;
			if (s.lines == 1) {
				s.started = line;
			}
			std::smatch m;
			if (std::regex_search(line, m, head)) {
				flush();
				kind = m[1].str();
				std::transform(kind.begin(), kind.end(), kind.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				msg = m[2].str();
				example = line;
				script.clear();
				inEntry = true;
				(kind == "warning" ? s.warnings : s.errors)++;
				continue;
			}
			if (inEntry && line.rfind("stack:", 0) == 0) {
				example += "\n" + line;
				continue;
			}
			if (inEntry && !line.empty() && (line[0] == '\t' || line[0] == ' ')) {
				if (example.size() < 4000) {
					example += "\n" + line;
				}
				if (script.empty() && (std::regex_search(line, m, frame) || std::regex_search(line, m, frame2))) {
					script = m[1].str();
				}
				continue;
			}
			flush();
		}
		flush();
		std::sort(s.groups.begin(), s.groups.end(), [](const LogGroup& a, const LogGroup& b) { return a.count > b.count; });
		return s;
	}
}
