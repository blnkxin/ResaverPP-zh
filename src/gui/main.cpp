// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// 本地化修改：界面、提示与报告文字改为简体中文（详见 README.zh-CN.md）。
#ifdef _WIN32
#	include <windows.h>
#	include <shellapi.h>
#	include <objbase.h>
#	include <timeapi.h>
#endif

#include "App.h"
#include "Platform.h"
#include "Theme.h"
#include "UiGlyphs.h"  // generated: the code points the Chinese UI text uses (i18n/gen_glyph_ranges.py)

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
static FILE* _wfopen_compat(const std::filesystem::path& p, const char*) { return _wfopen(p.c_str(), L"ab"); }
#else
static FILE* _wfopen_compat(const std::filesystem::path& p, const char* m) { return std::fopen(p.c_str(), m); }
#endif

namespace
{
	// ---- start-up log: ReSaverPP-startup.log next to the exe (else in %TEMP%) -------------------------
	// A window that opens and closes at once says nothing: this file says how far the start got, what
	// OpenGL the PC has, and where it crashed. Overwritten at every start.
	std::filesystem::path g_logPath;
	std::string           g_step = "starting";

	void Log(const std::string& a_line)
	{
		if (g_logPath.empty()) {
			return;
		}
		if (FILE* f = _wfopen_compat(g_logPath, "ab")) {
			std::fprintf(f, "%s\r\n", a_line.c_str());
			std::fclose(f);
		}
	}

	void Step(const std::string& a_step)
	{
		g_step = a_step;
		Log("step: " + a_step);
	}

	void OpenLog()
	{
#ifdef _WIN32
		wchar_t exe[MAX_PATH * 4] = L"";
		GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
		std::vector<std::filesystem::path> where{ std::filesystem::path(exe).parent_path() / "ReSaverPP-startup.log" };
		wchar_t tmp[MAX_PATH + 1] = L"";
		if (GetTempPathW(MAX_PATH, tmp)) {
			where.push_back(std::filesystem::path(tmp) / "ReSaverPP-startup.log");
		}
		for (const auto& w : where) {
			if (FILE* f = _wfopen(w.c_str(), L"wb")) {
				std::fclose(f);
				g_logPath = w;
				break;
			}
		}
		OSVERSIONINFOW vi{};  // the build number is enough to tell Windows 10 from 11
		vi.dwOSVersionInfoSize = sizeof(vi);
		using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
		if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) {
			fn(&vi);
		}
		Log(std::string("ReSaver++ ") + RESAVERPP_VERSION + " starting; Windows " + std::to_string(vi.dwMajorVersion) + "." + std::to_string(vi.dwMinorVersion) +
			" build " + std::to_string(vi.dwBuildNumber) + "; exe " + platform::ToUtf8(std::filesystem::path(exe)));
#endif
	}

#ifdef _WIN32
	void Fatal(const std::string& a_text)
	{
		Log("FATAL: " + a_text);
		const auto msg = a_text + "\n\nDetails are in " + (g_logPath.empty() ? std::string("(no log could be written)") : platform::ToUtf8(g_logPath)) +
						 ".\nPlease send that file with your report.";
		MessageBoxW(nullptr, std::filesystem::path(platform::FromUtf8(msg)).wstring().c_str(), L"ReSaver++ could not start", MB_OK | MB_ICONERROR);
	}

	LONG WINAPI OnCrash(EXCEPTION_POINTERS* a_ex)
	{
		const auto* rec = a_ex ? a_ex->ExceptionRecord : nullptr;
		const auto  addr = rec ? reinterpret_cast<std::uintptr_t>(rec->ExceptionAddress) : 0;
		std::string where = "?";
		HMODULE     mod = nullptr;
		if (addr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(addr), &mod)) {
			wchar_t name[MAX_PATH] = L"";
			GetModuleFileNameW(mod, name, MAX_PATH);
			char off[32];
			std::snprintf(off, sizeof(off), "+%llX", static_cast<unsigned long long>(addr - reinterpret_cast<std::uintptr_t>(mod)));
			where = platform::ToUtf8(std::filesystem::path(name).filename()) + off;
		}
		char code[16];
		std::snprintf(code, sizeof(code), "%08lX", rec ? rec->ExceptionCode : 0ul);
		Fatal("ReSaver++ crashed while " + g_step + " (exception " + code + " in " + where + ").");
		return EXCEPTION_EXECUTE_HANDLER;
	}
#endif

	App*                     g_app = nullptr;
	std::vector<std::string> g_dropped;

	void OnDrop(GLFWwindow*, int count, const char** paths)
	{
		for (int i = 0; i < count; ++i) {
			g_dropped.emplace_back(paths[i]);
		}
	}

	ImFont* g_mono = nullptr;

	// Characters beyond basic Latin: names and places in saves from other languages (Polish, Czech,
	// Russian, Greek, Vietnamese, ...). Segoe UI / Consolas cover these.
	const ImWchar* WideRanges()
	{
		static const ImWchar r[] = { 0x0020, 0x024F, 0x0250, 0x02FF, 0x0300, 0x036F, 0x0370, 0x03FF, 0x0400, 0x052F, 0x1E00, 0x1EFF, 0x2000, 0x206F,
			0x20A0, 0x20CF, 0x2100, 0x218F, 0x2190, 0x21FF, 0x2200, 0x22FF, 0x2500, 0x25FF, 0xFFFD, 0xFFFD, 0 };
		return r;
	}

	// East Asian text needs a font with those characters: picked from the game text code page (the
	// "codepage=" setting, else the system's)
	struct AsianFont
	{
		std::vector<const char*> files;
		const ImWchar*           ranges = nullptr;
	};

	unsigned TextCodePage()
	{
		std::ifstream f(platform::SettingsFile());
		for (std::string line; std::getline(f, line);) {
			if (line.rfind("codepage=", 0) == 0) {
				if (const unsigned cp = static_cast<unsigned>(std::strtoul(line.c_str() + 9, nullptr, 10))) {
					return cp;
				}
			}
		}
#ifdef _WIN32
		return GetACP();
#else
		return 1252;
#endif
	}

	AsianFont PickAsianFont()
	{
		auto& io = ImGui::GetIO();
		switch (TextCodePage()) {
		case 932: return { { "C:\\Windows\\Fonts\\YuGothM.ttc", "C:\\Windows\\Fonts\\meiryo.ttc", "C:\\Windows\\Fonts\\msgothic.ttc" }, io.Fonts->GetGlyphRangesJapanese() };
		case 936: return { { "C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\simsun.ttc" }, io.Fonts->GetGlyphRangesChineseFull() };
		case 950: return { { "C:\\Windows\\Fonts\\msjh.ttc", "C:\\Windows\\Fonts\\mingliu.ttc" }, io.Fonts->GetGlyphRangesChineseFull() };
		case 949: return { { "C:\\Windows\\Fonts\\malgun.ttf", "C:\\Windows\\Fonts\\gulim.ttc" }, io.Fonts->GetGlyphRangesKorean() };
		case 874: return { { "C:\\Windows\\Fonts\\LeelawUI.ttf", "C:\\Windows\\Fonts\\tahoma.ttf" }, io.Fonts->GetGlyphRangesThai() };
		default: return {};
		}
	}

	// The interface itself is Chinese: its characters must be there even when the save's text is not
	// in a Chinese code page (in which case PickAsianFont() merges nothing, or a Japanese/Korean one).
	const ImWchar* UiRanges()
	{
		static std::vector<ImWchar> ranges = [] {
			std::vector<ImWchar> r;
			for (const unsigned* p = kUiGlyphRanges; p[0] != 0; p += 2) {
				r.push_back(static_cast<ImWchar>(p[0]));
				r.push_back(static_cast<ImWchar>(p[1]));
			}
			if (!r.empty()) {
				r.push_back(0);
			}
			return r;
		}();
		return ranges.empty() ? nullptr : ranges.data();
	}

	bool MergeFont(ImFont* a_font, float a_size, const std::vector<const char*>& a_files, const ImWchar* a_ranges)
	{
		if (!a_font || !a_ranges) {
			return false;
		}
		ImGuiIO& io = ImGui::GetIO();
		for (const char* f : a_files) {
			std::error_code ec;
			if (!std::filesystem::exists(f, ec)) {
				continue;
			}
			ImFontConfig cfg;
			cfg.MergeMode = true;
			if (io.Fonts->AddFontFromFileTTF(f, a_size, &cfg, a_ranges) != nullptr) {
				return true;
			}
		}
		return false;
	}

	ImFont* AddFont(const std::vector<const char*>& a_files, float a_size, bool a_fallbackDefault)
	{
		ImGuiIO& io = ImGui::GetIO();
		ImFont*  font = nullptr;
		for (const char* f : a_files) {
			std::error_code ec;
			if (std::filesystem::exists(f, ec) && (font = io.Fonts->AddFontFromFileTTF(f, a_size, nullptr, WideRanges())) != nullptr) {
				break;
			}
		}
		if (!font && a_fallbackDefault) {
			ImFontConfig cfg;
			cfg.SizePixels = a_size * 13.0f / 17.0f;
			font = io.Fonts->AddFontDefault(&cfg);
		}
		// merge East Asian characters into it when the game text is in such a language
		if (font) {
			const auto asian = PickAsianFont();
			const bool merged = MergeFont(font, a_size, asian.files, asian.ranges);
			// the complete Chinese set covers the interface too; anything less needs the interface's own glyphs
			if (!(merged && asian.ranges == io.Fonts->GetGlyphRangesChineseFull())) {
				MergeFont(font, a_size,
						{ "C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\msyhl.ttc", "C:\\Windows\\Fonts\\simhei.ttf",
								"C:\\Windows\\Fonts\\Deng.ttf", "C:\\Windows\\Fonts\\simsun.ttc" },
						UiRanges());
			}
		}
		return font;
	}

	// fixed-width font for the script viewer
	void LoadMonoFont(float a_scale)
	{
		g_mono = AddFont({ "C:\\Windows\\Fonts\\consola.ttf", "C:\\Windows\\Fonts\\cour.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
							 "/usr/share/fonts/TTF/DejaVuSansMono.ttf" },
			16.0f * a_scale, true);
	}

	// the start page logo: a big bold font with only the characters it needs (a big font with every
	// character would take a lot of texture memory)
	ImFont* g_title = nullptr;
	void    LoadTitleFont(float a_scale)
	{
		static const ImWchar ranges[] = { 0x0020, 0x005A, 0 };  // space to Z: capitals, digits, +
		for (const char* f : { "C:\\Windows\\Fonts\\segoeuib.ttf", "C:\\Windows\\Fonts\\arialbd.ttf",
				 "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf" }) {
			std::error_code ec;
			if (std::filesystem::exists(f, ec) && (g_title = ImGui::GetIO().Fonts->AddFontFromFileTTF(f, 110.0f * a_scale, nullptr, ranges)) != nullptr) {
				return;
			}
		}
	}

	void LoadFonts(float a_scale)
	{
		AddFont({ "C:\\Windows\\Fonts\\segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf" },
			17.0f * a_scale, true);
	}

	void Style()
	{
		theme::Apply(theme::Kind::kLight);  // the default; the saved theme is applied with the settings
		ImGuiStyle& s = ImGui::GetStyle();
		s.WindowRounding = 4;
		s.FrameRounding = 3;
		s.ChildRounding = 3;
		s.GrabRounding = 3;
		s.FramePadding = ImVec2(8, 4);
		s.ItemSpacing = ImVec2(8, 5);
	}
}

int RunApp(int argc, char** argv)
{
#ifdef _WIN32
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // for the folder picker
	// Version 12: no Windows error boxes for DLLs that only *might* be loaded. GLFW probes for gamepad DLLs at
	// start-up (xinput1_4.dll first). On Windows 7 a xinput1_4.dll copied from Windows 10 (games and controller
	// tools install one) can't load there - "GetTickCount could not be located in api-ms-win-core-sysinfo-l1-2-1.dll".
	// GLFW just falls back to xinput1_3, but Windows showed that message box first. ReSaver++ doesn't use gamepads.
	SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
	OpenLog();
	SetUnhandledExceptionFilter(OnCrash);
#endif
	static std::string glfwError;
	glfwSetErrorCallback([](int a_code, const char* a_text) {
		glfwError = std::to_string(a_code) + ": " + (a_text ? a_text : "");
		Log("GLFW error " + glfwError);
	});
	Step("starting the window system");
	if (!glfwInit()) {
		std::fputs("初始化 GLFW 失败\n", stderr);
#ifdef _WIN32
		Fatal("窗口系统无法启动（GLFW " + glfwError + "）。");
#endif
		return 1;
	}
	Step("creating the window (OpenGL 3.0)");
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	GLFWwindow* window = glfwCreateWindow(1600, 950, "ReSaver++ 中文版", nullptr, nullptr);
	if (!window) {
		std::fputs("无法创建窗口（需要 OpenGL 3.0）\n", stderr);
#ifdef _WIN32
		Fatal("无法创建窗口：ReSaver++ 需要显卡驱动的 OpenGL 3.0（" + glfwError +
			  "）。\n\n请更新显卡驱动（从 NVIDIA / AMD / Intel 官网，不要用 Windows 更新）。双显卡笔记本上，"
			  "把 ReSaver++ 设为使用高性能显卡。远程桌面和一些虚拟机没有 OpenGL 3。");
#endif
		glfwTerminate();
		return 1;
	}
	glfwMakeContextCurrent(window);
	{
		auto str = [](GLenum a_what) {
			const auto* t = glGetString(a_what);
			return t ? std::string(reinterpret_cast<const char*>(t)) : std::string("?");
		};
		Log("OpenGL " + str(GL_VERSION) + "; " + str(GL_RENDERER) + " (" + str(GL_VENDOR) + ")");
	}
	glfwSwapInterval(1);
#ifdef _WIN32
	timeBeginPeriod(1);  // 1 ms timer resolution, so the frame cap below waits ~16 ms, not 31
#endif
	glfwSetDropCallback(window, OnDrop);

	float xs = 1, ys = 1;
	glfwGetWindowContentScale(window, &xs, &ys);

	Step("loading fonts and drawing set-up");
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::GetIO().IniFilename = nullptr;
	Style();
	ImGui::GetStyle().ScaleAllSizes(xs);
	LoadFonts(xs);
	LoadMonoFont(xs);
	LoadTitleFont(xs);
	ImGui_ImplGlfw_InitForOpenGL(window, true);
	ImGui_ImplOpenGL3_Init("#version 130");

	Step("reading the settings and starting ReSaver++");
	App app;
	g_app = &app;
	app.monoFont = g_mono;
	app.titleFont = g_title;
	app.uploadTexture = [](const std::uint8_t* px, int w, int h) -> std::uint64_t {
		GLuint tex = 0;
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
		return tex;
	};
	app.freeTexture = [](std::uint64_t t) {
		GLuint tex = static_cast<GLuint>(t);
		glDeleteTextures(1, &tex);
	};

	if (argc > 1) {
		Step("opening " + std::string(argv[1]));
		app.openAny(platform::FromUtf8(argv[1]));
	} else if (!std::getenv("RESAVERPP_SCREENSHOT")) {
		Step("opening the newest save");
		app.openNewestAtStart();
	}
	Step("drawing the first frames");
	int drawn = 0;
	if (const char* t = std::getenv("RESAVERPP_TEST_CAT")) {
		app.testSelect(std::atoi(t), std::getenv("RESAVERPP_TEST_FILTER"), std::getenv("RESAVERPP_TEST_ANALYSIS") != nullptr);
		if (const char* rm = std::getenv("RESAVERPP_TEST_REMOVE")) {
			app.testRemove(rm);
		}
		if (const char* w = std::getenv("RESAVERPP_TEST_OPEN")) {
			app.testOpen(w);
		}
		if (std::getenv("RESAVERPP_TEST_COMPILE")) {
			app.testOpen("compile");
		}
		if (std::getenv("RESAVERPP_TEST_ROWVIEW")) {
			app.testOpen("rowview");
		}
	}

	// optional: RESAVERPP_SCREENSHOT=<file.ppm> renders a few frames, saves the window and exits (used for testing)
	const char* shotPath = std::getenv("RESAVERPP_SCREENSHOT");
	int         frame = 0;
	const double shotDelay = std::getenv("RESAVERPP_SHOT_DELAY") ? std::atof(std::getenv("RESAVERPP_SHOT_DELAY")) : 0.0;

	while (!app.wantsQuit()) {
		glfwPollEvents();
		if (glfwWindowShouldClose(window)) {
			glfwSetWindowShouldClose(window, GLFW_FALSE);
			app.requestQuit();
		}
		if (!g_dropped.empty()) {
			app.openAny(platform::FromUtf8(g_dropped.back()));
			g_dropped.clear();
		}
		if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
			ImGui_ImplGlfw_Sleep(50);
			continue;
		}

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
		// an error while drawing (a file name the system cannot convert, a damaged file...) is reported in
		// the error box and the window keeps running, instead of closing
		{
			ImGuiErrorRecoveryState state;
			ImGui::ErrorRecoveryStoreState(&state);
			std::string failed;
			try {
				app.draw();
			} catch (const std::exception& e) {
				failed = e.what();
			} catch (...) {
				failed = "unknown error";
			}
			if (!failed.empty()) {
				ImGui::ErrorRecoveryTryToRecoverState(&state);
				static std::string last;
				if (failed != last) {  // once per different error, not every frame
					last = failed;
					Log("ERROR while " + g_step + ": " + failed);
					app.reportError("Something went wrong (ReSaver++ keeps running): " + failed + "\n\nPlease report it with ReSaverPP-startup.log.");
				}
				// the app's own error box may be past the point that failed: say it here too
				const ImGuiViewport* vp = ImGui::GetMainViewport();
				ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 60), ImGuiCond_Always, ImVec2(0.5f, 0));
				ImGui::SetNextWindowSize(ImVec2(std::min(760.0f, vp->WorkSize.x - 40), 0));
				ImGui::Begin("ReSaver++ hit an error", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
				ImGui::TextWrapped("While %s: %s", g_step.c_str(), failed.c_str());
				ImGui::Spacing();
				ImGui::TextWrapped("Please report this on the mod page with the file ReSaverPP-startup.log (next to ReSaverPP.exe, or in %%TEMP%%).");
				ImGui::End();
			}
		}
		ImGui::Render();

		int w, h;
		glfwGetFramebufferSize(window, &w, &h);
		glViewport(0, 0, w, h);
		const auto bg = theme::Clear();
		glClearColor(bg.x, bg.y, bg.z, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

		if (shotPath && ++frame >= 6 && glfwGetTime() >= shotDelay) {
			std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 3);
			glPixelStorei(GL_PACK_ALIGNMENT, 1);
			glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
			if (FILE* f = std::fopen(shotPath, "wb")) {
				std::fprintf(f, "P6\n%d %d\n255\n", w, h);
				for (int y = h - 1; y >= 0; --y) {
					std::fwrite(px.data() + static_cast<std::size_t>(y) * w * 3, 1, static_cast<std::size_t>(w) * 3, f);
				}
				std::fclose(f);
			}
			break;
		}
		glfwSwapBuffers(window);
		if (++drawn == 60) {
			Step("running (60 frames drawn)");
		}

		// Frame cap of our own: some drivers ignore the swap interval (hybrid-GPU laptops, vsync forced off
		// in the NVIDIA / AMD control panel), and the window would then draw thousands of frames a second
		// and flood the desktop. 60 a second while the window has focus, 15 while it does not. Waiting for
		// events rather than sleeping, so input is taken in while waiting.
		{
			static double next = glfwGetTime();
			const double  fps = glfwGetWindowAttrib(window, GLFW_FOCUSED) ? 60.0 : 15.0;
			next += 1.0 / fps;
			double now = glfwGetTime();
			if (next < now - 0.25) {
				next = now;  // far behind (a long frame): do not try to catch up
			}
			while (now < next) {
				glfwWaitEventsTimeout(next - now);
				now = glfwGetTime();
			}
		}
	}

#ifdef _WIN32
	timeEndPeriod(1);
#endif
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwDestroyWindow(window);
	glfwTerminate();
	Step("closed normally");
	return 0;
}

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	int    argc = 0;
	LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
	std::vector<std::string> args;
	std::vector<char*>       argv;
	for (int i = 0; i < argc; ++i) {
		args.push_back(platform::ToUtf8(std::filesystem::path(wargv[i])));
	}
	LocalFree(wargv);
	for (auto& a : args) {
		argv.push_back(a.data());
	}
	try {
		return RunApp(static_cast<int>(argv.size()), argv.data());
	} catch (const std::exception& e) {
		Fatal("ReSaver++ stopped while " + g_step + ": " + e.what());
	} catch (...) {
		Fatal("ReSaver++ stopped while " + g_step + " (unknown error).");
	}
	return 1;
}
#else
int main(int argc, char** argv)
{
	return RunApp(argc, argv);
}
#endif
