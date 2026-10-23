// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// 本地化修改：界面、提示与报告文字改为简体中文（详见 README.zh-CN.md）。
#include "PapyrusCompiler.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#endif

namespace ess::papc
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

		std::string Quote(const std::string& s) { return "\"" + s + "\""; }

		void SplitInto(std::string& pending, const char* data, std::size_t n, std::vector<std::string>& out)
		{
			for (std::size_t i = 0; i < n; ++i) {
				const char c = data[i];
				if (c == '\n') {
					if (!pending.empty() && pending.back() == '\r') {
						pending.pop_back();
					}
					out.push_back(pending);
					pending.clear();
				} else {
					pending.push_back(c);
				}
			}
		}

#ifdef _WIN32
		std::wstring Wide(const std::string& s)
		{
			if (s.empty()) {
				return {};
			}
			const int    n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
			std::wstring w(static_cast<std::size_t>(n), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
			return w;
		}

		std::string Narrow(const std::string& oem)
		{
			// the compiler writes in the console code page; convert to UTF-8
			if (oem.empty()) {
				return {};
			}
			const UINT   cp = GetOEMCP();
			const int    n = MultiByteToWideChar(cp, 0, oem.data(), static_cast<int>(oem.size()), nullptr, 0);
			std::wstring w(static_cast<std::size_t>(n), L'\0');
			MultiByteToWideChar(cp, 0, oem.data(), static_cast<int>(oem.size()), w.data(), n);
			const int   m = WideCharToMultiByte(CP_UTF8, 0, w.data(), n, nullptr, 0, nullptr, nullptr);
			std::string u(static_cast<std::size_t>(m), '\0');
			WideCharToMultiByte(CP_UTF8, 0, w.data(), n, u.data(), m, nullptr, nullptr);
			return u;
		}

		int Run(const std::string& a_cmd, const std::filesystem::path& a_dir, std::vector<std::string>& a_out, std::string& a_error)
		{
			SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
			HANDLE              rd = nullptr, wr = nullptr;
			if (!CreatePipe(&rd, &wr, &sa, 0)) {
				a_error = "无法创建管道";
				return -1;
			}
			SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
			STARTUPINFOW si{};
			si.cb = sizeof(si);
			si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
			si.wShowWindow = SW_HIDE;
			si.hStdOutput = wr;
			si.hStdError = wr;
			si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
			PROCESS_INFORMATION pi{};
			std::wstring        cmd = Wide(a_cmd);
			const std::wstring  dir = a_dir.wstring();
			if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
				a_error = "无法启动编译器（错误 " + std::to_string(GetLastError()) + "）";
				CloseHandle(rd);
				CloseHandle(wr);
				return -1;
			}
			CloseHandle(wr);
			std::string pending, raw;
			char        buf[4096];
			DWORD       got = 0;
			while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) {
				raw.append(buf, got);
			}
			CloseHandle(rd);
			WaitForSingleObject(pi.hProcess, INFINITE);
			DWORD code = 0;
			GetExitCodeProcess(pi.hProcess, &code);
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
			const auto text = Narrow(raw);
			SplitInto(pending, text.data(), text.size(), a_out);
			if (!pending.empty()) {
				a_out.push_back(pending);
			}
			return static_cast<int>(code);
		}
#else
		int Run(const std::string& a_cmd, const std::filesystem::path& a_dir, std::vector<std::string>& a_out, std::string& a_error)
		{
			// (for testing on Linux: the compiler runs under mono)
			const std::string full = (a_dir.empty() ? std::string() : "cd " + Quote(U8(a_dir)) + " && ") + a_cmd + " 2>&1";
			FILE*             p = popen(full.c_str(), "r");
			if (!p) {
				a_error = "无法启动编译器";
				return -1;
			}
			std::string pending;
			char        buf[4096];
			std::size_t got = 0;
			while ((got = std::fread(buf, 1, sizeof(buf), p)) > 0) {
				SplitInto(pending, buf, got, a_out);
			}
			if (!pending.empty()) {
				a_out.push_back(pending);
			}
			const int st = pclose(p);
			return st < 0 ? -1 : (st >> 8) & 0xFF;
		}
#endif
	}

	std::vector<Message> ParseOutput(const std::vector<std::string>& a_lines)
	{
		std::vector<Message> out;
		for (const auto& l : a_lines) {
			// "<path>.psc(12,5): text" (or .pas with -op); "<unknown>(0,0): text"
			const auto open = l.find('(');
			if (open == std::string::npos) {
				continue;
			}
			const auto close = l.find("):", open);
			if (close == std::string::npos) {
				continue;
			}
			const auto file = l.substr(0, open);
			const auto lf = Lower(file);
			if (!(lf.ends_with(".psc") || lf.ends_with(".pas") || lf == "<unknown>")) {
				continue;
			}
			Message m;
			m.file = file;
			if (std::sscanf(l.c_str() + open + 1, "%d,%d", &m.line, &m.column) < 1) {
				continue;
			}
			m.text = l.substr(close + 2);
			while (!m.text.empty() && m.text.front() == ' ') {
				m.text.erase(m.text.begin());
			}
			bool dup = false;
			for (const auto& x : out) {
				dup = dup || (x.file == m.file && x.line == m.line && x.column == m.column && x.text == m.text);
			}
			if (!dup) {
				out.push_back(std::move(m));
			}
		}
		return out;
	}

	bool Anonymize(const std::filesystem::path& a_pex, std::string& a_error)
	{
		std::fstream f(a_pex, std::ios::binary | std::ios::in | std::ios::out);
		if (!f) {
			a_error = "无法打开 " + U8(a_pex.filename());
			return false;
		}
		unsigned char magic[4]{};
		f.read(reinterpret_cast<char*>(magic), 4);
		if (!(magic[0] == 0xFA && magic[1] == 0x57 && magic[2] == 0xC0 && magic[3] == 0xDE)) {
			a_error = U8(a_pex.filename()) + " 不是天际 .pex 文件";
			return false;
		}
		f.seekg(16);  // magic, version (2), game (2), compile time (8)
		for (int field = 0; field < 3; ++field) {
			unsigned char len[2]{};
			f.read(reinterpret_cast<char*>(len), 2);
			const int n = (len[0] << 8) | len[1];
			if (!f || n < 0) {
				a_error = "文件意外结束： " + U8(a_pex.filename());
				return false;
			}
			const auto pos = f.tellg();
			if (n > 0) {  // source path, user name, machine name
				f.seekp(pos);
				const std::string dashes(static_cast<std::size_t>(n), '-');
				f.write(dashes.data(), n);
			}
			f.seekg(pos + static_cast<std::streamoff>(n));
		}
		return static_cast<bool>(f);
	}

	std::optional<std::filesystem::path> FindCompiler(const std::filesystem::path& a_game)
	{
		if (a_game.empty()) {
			return std::nullopt;
		}
		std::error_code ec;
		for (const auto& sub : { "Papyrus Compiler", "Papyrus compiler" }) {
			const auto p = a_game / sub / "PapyrusCompiler.exe";
			if (std::filesystem::exists(p, ec)) {
				return p;
			}
		}
		return std::nullopt;
	}

	Result Compile(const Request& a_req)
	{
		Result          r;
		std::error_code ec;
		if (!std::filesystem::exists(a_req.compiler, ec)) {
			r.error = "在以下位置没有找到 PapyrusCompiler.exe： " + U8(a_req.compiler) +
					  "。它随 Creation Kit 一起提供（游戏目录 \\Papyrus Compiler）；请在脚本查看器的编译设置中指定它的位置。";
			return r;
		}
		std::filesystem::create_directories(a_req.output, ec);

		std::string head;
#ifndef _WIN32
		if (Lower(U8(a_req.compiler.extension())) == ".exe") {
			head = "mono ";
		}
#endif
		head += Quote(U8(a_req.compiler)) + " " + Quote(U8(a_req.source));
		std::string tail = " -o=" + Quote(U8(a_req.output)) + " -f=" + Quote(a_req.flags) + (a_req.optimize ? " -op" : "");
		// Windows command lines are limited to 32767 characters: leave out the lowest priority folders
		std::string imports;
		for (std::size_t i = 0; i < a_req.imports.size(); ++i) {
			const auto add = (imports.empty() ? "" : ";") + U8(a_req.imports[i]);
			if (head.size() + tail.size() + imports.size() + add.size() + 8 > 32000) {
				r.importsDropped = a_req.imports.size() - i;
				break;
			}
			imports += add;
		}
		r.commandLine = head + " -i=" + Quote(imports) + tail;

		const auto pex = a_req.output / (U8(a_req.source.stem()) + ".pex");
		const auto before = std::filesystem::exists(pex, ec) ? std::filesystem::last_write_time(pex, ec) : std::filesystem::file_time_type{};
		r.exitCode = Run(r.commandLine, a_req.compiler.parent_path(), r.output, r.error);
		if (r.exitCode < 0 && !r.error.empty()) {
			return r;
		}
		r.ran = true;
		r.messages = ParseOutput(r.output);
		bool failedText = false;
		for (const auto& l : r.output) {
			const auto low = Lower(l);
			failedText = failedText || low.find("compilation failed") != std::string::npos || low.find("assembly failed") != std::string::npos;
		}
		const bool written = std::filesystem::exists(pex, ec) && std::filesystem::last_write_time(pex, ec) != before;
		r.ok = written && !failedText && r.messages.empty();
		if (r.ok) {
			r.pex = pex;
			if (a_req.anonymize) {
				std::string err;
				if (!Anonymize(pex, err)) {
					r.error = "已编译，但无法匿名化： " + err;
				}
			}
		}
		return r;
	}
}
