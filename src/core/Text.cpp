// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Text.h"

#include <cstdlib>

#include <atomic>

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#else
#	include <iconv.h>
#endif

namespace ess::text
{
	namespace
	{
		std::atomic<unsigned> g_codePage{ 0 };

		void PutUtf8(std::string& out, char32_t u)
		{
			if (u < 0x80) {
				out.push_back(static_cast<char>(u));
			} else if (u < 0x800) {
				out.push_back(static_cast<char>(0xC0 | (u >> 6)));
				out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
			} else if (u < 0x10000) {
				out.push_back(static_cast<char>(0xE0 | (u >> 12)));
				out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
			} else {
				out.push_back(static_cast<char>(0xF0 | (u >> 18)));
				out.push_back(static_cast<char>(0x80 | ((u >> 12) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
			}
		}

		// Windows-1252 (used where the system code page is not available)
		std::string From1252(const std::string& s)
		{
			static const char16_t hi[32] = { 0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D,
				0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178 };
			std::string out;
			out.reserve(s.size() + s.size() / 2);
			for (unsigned char c : s) {
				PutUtf8(out, c < 0x80 ? c : (c < 0xA0 ? hi[c - 0x80] : c));
			}
			return out;
		}
	}

	void     SetCodePage(unsigned a_codePage) { g_codePage = a_codePage; }
	unsigned CodePage() { return g_codePage; }

	unsigned EffectiveCodePage()
	{
		const unsigned cp = g_codePage;
		if (cp) {
			return cp;
		}
#ifdef _WIN32
		return GetACP();
#else
		return 1252;
#endif
	}

	bool IsUtf8(const std::string& a)
	{
		for (std::size_t i = 0; i < a.size();) {
			const auto c = static_cast<unsigned char>(a[i]);
			if (c < 0x80) {
				++i;
				continue;
			}
			const int n = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
			if (n < 0 || c == 0xC0 || c == 0xC1) {
				return false;
			}
			for (int k = 1; k <= n; ++k) {
				if (i + k >= a.size() || (static_cast<unsigned char>(a[i + k]) & 0xC0) != 0x80) {
					return false;
				}
			}
			i += static_cast<std::size_t>(n) + 1;
		}
		return true;
	}

	std::string ToUtf8(const std::string& a)
	{
		if (IsUtf8(a)) {
			return a;
		}
		const unsigned cp = EffectiveCodePage();
#ifdef _WIN32
		if (cp != 65001) {
			const int n = MultiByteToWideChar(cp, 0, a.data(), static_cast<int>(a.size()), nullptr, 0);
			if (n > 0) {
				std::wstring w(static_cast<std::size_t>(n), L'\0');
				MultiByteToWideChar(cp, 0, a.data(), static_cast<int>(a.size()), w.data(), n);
				const int   m = WideCharToMultiByte(CP_UTF8, 0, w.data(), n, nullptr, 0, nullptr, nullptr);
				std::string u(static_cast<std::size_t>(m), '\0');
				WideCharToMultiByte(CP_UTF8, 0, w.data(), n, u.data(), m, nullptr, nullptr);
				return u;
			}
		}
#else
		if (cp != 65001 && cp != 1252) {
			// (for testing on Linux) iconv knows the Windows code pages as "CP<n>"
			const std::string from = "CP" + std::to_string(cp);
			iconv_t           cd = iconv_open("UTF-8", from.c_str());
			if (cd != reinterpret_cast<iconv_t>(-1)) {
				std::string in = a, out(a.size() * 4 + 4, '\0');
				char*       ip = in.data();
				char*       op = out.data();
				std::size_t il = in.size(), ol = out.size();
				const auto  rc = iconv(cd, &ip, &il, &op, &ol);
				iconv_close(cd);
				if (rc != static_cast<std::size_t>(-1)) {
					out.resize(out.size() - ol);
					return out;
				}
			}
		}
#endif
		(void)cp;
		return From1252(a);
	}

	std::string FromUtf8(const std::string& u, const std::string& a_original)
	{
		auto ascii = [](const std::string& t) {
			for (unsigned char c : t) {
				if (c >= 0x80) {
					return false;
				}
			}
			return true;
		};
		if (ascii(u)) {
			return u;
		}
		// the original was UTF-8: keep UTF-8
		if (!ascii(a_original) && IsUtf8(a_original)) {
			return u;
		}
		const unsigned cp = EffectiveCodePage();
		if (cp == 65001) {
			return u;
		}
#ifdef _WIN32
		const int n = MultiByteToWideChar(CP_UTF8, 0, u.data(), static_cast<int>(u.size()), nullptr, 0);
		std::wstring w(static_cast<std::size_t>(n), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, u.data(), static_cast<int>(u.size()), w.data(), n);
		const int   m = WideCharToMultiByte(cp, 0, w.data(), n, nullptr, 0, nullptr, nullptr);
		std::string out(static_cast<std::size_t>(m), '\0');
		WideCharToMultiByte(cp, 0, w.data(), n, out.data(), m, nullptr, nullptr);
		return out;
#else
		// Latin-1 range only
		std::string out;
		for (std::size_t i = 0; i < u.size();) {
			const auto c = static_cast<unsigned char>(u[i]);
			if (c < 0x80) {
				out.push_back(static_cast<char>(c));
				++i;
			} else if ((c & 0xE0) == 0xC0 && i + 1 < u.size()) {
				const unsigned v = ((c & 0x1F) << 6) | (static_cast<unsigned char>(u[i + 1]) & 0x3F);
				out.push_back(v < 0x100 ? static_cast<char>(v) : '?');
				i += 2;
			} else {
				out.push_back('?');
				++i;
				while (i < u.size() && (static_cast<unsigned char>(u[i]) & 0xC0) == 0x80) {
					++i;
				}
			}
		}
		return out;
#endif
	}

	std::filesystem::path EnvPath(const char* a_name)
	{
#ifdef _WIN32
		std::wstring w;
		for (const char* c = a_name; *c; ++c) {
			w += static_cast<wchar_t>(*c);
		}
		const wchar_t* v = _wgetenv(w.c_str());
		return v ? std::filesystem::path(std::wstring(v)) : std::filesystem::path();
#else
		const char* v = std::getenv(a_name);
		return v ? std::filesystem::path(v) : std::filesystem::path();
#endif
	}

	std::filesystem::path PathFrom(const std::string& a_text)
	{
		const auto utf8 = IsUtf8(a_text) ? a_text : ToUtf8(a_text);
		try {
			return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
		} catch (...) {
			std::string ascii;
			for (char c : utf8) {
				ascii += static_cast<unsigned char>(c) < 0x80 ? c : '_';
			}
			return std::filesystem::path(ascii);
		}
	}
}
