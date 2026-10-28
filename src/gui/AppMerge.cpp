// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Tools > Merged patch: one plugin that combines the changes of mods that edit the same records.
#include "App.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "core/LoadOrder.h"
#include "core/ScriptSources.h"

#include "imgui.h"

#include <cstdio>
#include <fstream>

using namespace ess;

namespace
{
	std::string Fmt(const char* f, auto... args)
	{
		char buf[1024];
		std::snprintf(buf, sizeof(buf), f, args...);
		return buf;
	}

	void Colored(std::uint32_t a_col, const std::string& a_text)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, a_col);
		ImGui::TextWrapped("%s", a_text.c_str());
		ImGui::PopStyleColor();
	}

	const char* const kModFolder = "ReSaver Plus Merged Patch";
	const char* const kPatchName = "ReSaverPP Merged Patch.esp";
}

void App::drawMergedPatch()
{
	prepareWindow("Merged patch", ImVec2(0.8f, 0.85f));
	if (!ImGui::Begin("合并补丁", &_showMerge, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Merged patch");

	// a finished build
	if (!_mergeBusy && _mergeThread.joinable()) {
		_mergeThread.join();
		_mergeResult = std::move(_mergeIncoming);
		_mergeSaved.clear();
	}

	const std::filesystem::path pluginsTxt =
		_mo2 && !_profile.empty() ? _mo2->profilesDir / platform::FromUtf8(_profile) / "plugins.txt" : loadorder::GamePluginsTxt(_vr);
	const std::filesystem::path outFile =
		_mo2 ? _mo2->modsDir / kModFolder / kPatchName : (gameFolder().empty() ? std::filesystem::path() : gameFolder() / "Data" / kPatchName);

	ImGui::NewLine();
	ImGui::TextWrapped("当两个模组修改同一个分级列表、容器、NPC、关键字列表、FormList 或套装时，游戏只会用最后加载的那个，"
					   "另一个模组的添加内容就丢了。合并补丁会读取你加载顺序里的每个插件，写出一个把两者都保留的插件："
					   "每个模组添加或移除的内容都会合并起来。其余的值保持获胜模组的样子，和游戏没有补丁时一样。");
	ImGui::TextWrapped("补丁是一个轻量（带 ESL 标记）插件，因此不占用加载顺序的位置。只有当某条被补丁的记录来自某个模组、"
					   "或指向该模组的某个表单时，才需要那个模组作为主文件。在你添加、移除或调整模组顺序之后，请重新构建它。");
	ImGui::Spacing();
	ImGui::TextDisabled("加载顺序：%s", platform::ToUtf8(pluginsTxt).c_str());
	if (_mo2 && !_profile.empty()) {
		ImGui::TextDisabled("Mod Organizer 配置：%s", _profile.c_str());
	}

	const bool noMods = _sourcesPath.empty() && !_sources.scanned();
	if (noMods) {
		Colored(theme::Warn(), "ReSaver Plus 还不知道你的模组在哪里：请在 选项 > 设置 里设置模组文件夹（Mod Organizer）"
							   "或游戏文件夹，然后回到这里。");
		ImGui::End();
		return;
	}

	// waiting for the scan of the mods, then the build starts by itself
	auto start = [this, pluginsTxt] {
		auto opt = std::make_shared<merge::Options>();
		opt->loadOrder = merge::ReadLoadOrder(pluginsTxt, gameFolder());
		opt->patchName = kPatchName;
		auto src = std::make_shared<ScriptSources>(_sources);
		merge::UseSources(*opt, *src);
		_mergeLoadOrder = opt->loadOrder.size();
		_mergeProgress = std::make_unique<merge::Progress>();
		_mergeBusy = true;
		auto* prog = _mergeProgress.get();
		_mergeThread = std::thread([this, opt, src, prog] {
			auto r = std::make_unique<merge::Result>(merge::Build(*opt, prog));
			_mergeIncoming = std::move(r);
			_mergeBusy = false;
		});
	};
	if (_mergeWaitScan && !scanning() && _sources.scanned()) {
		_mergeWaitScan = false;
		start();
	}

	ImGui::Spacing();
	ImGui::BeginDisabled(_mergeBusy || _mergeWaitScan);
	if (ImGui::Button(_mergeResult ? "重新构建合并补丁" : "构建合并补丁")) {
		if (ensureSourcesScanned()) {
			start();
		} else {
			_mergeWaitScan = true;
		}
	}
	ImGui::EndDisabled();
	tips::Tip("读取你加载顺序里的每个插件，算出合并补丁。在你保存它之前不会写入任何东西。");
	if (_mergeWaitScan) {
		ImGui::SameLine();
		ImGui::TextDisabled("正在先找出你的模组…");
		drawBackgroundProgress(0);
	}
	if (_mergeBusy && _mergeProgress) {
		ImGui::SameLine();
		const auto total = _mergeProgress->total.load(), done = _mergeProgress->done.load();
		ImGui::ProgressBar(total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f, ImVec2(-1, 0),
			Fmt("%s  %zu / %zu", _mergeProgress->current().c_str(), done, total).c_str());
		if (ImGui::Button("取消")) {
			_mergeProgress->cancel = true;
		}
	}
	if (!_mergeResult || _mergeBusy) {
		ImGui::End();
		return;
	}

	const auto& r = *_mergeResult;
	ImGui::Separator();
	if (!r.ok) {
		Colored(theme::Bad(), "补丁无法构建：" + r.error);
		ImGui::End();
		return;
	}
	ImGui::Text("已读取 %zu / %zu 个插件。有 %zu 条记录被两个或更多插件改成了不同的样子。", r.pluginsRead, _mergeLoadOrder, r.conflicts);
	Colored(theme::Good(), Fmt("已把 %zu 条记录合并进补丁，共 %zu 个主文件。", r.merged.size(), r.masters.size()));
	if (!r.skipped.empty()) {
		ImGui::TextDisabled("有 %zu 条无法合并；这些保持获胜模组的版本，和没有补丁时一样。", r.skipped.size());
	}
	if (!r.missing.empty()) {
		Colored(theme::Warn(), Fmt("加载顺序里有 %zu 个插件没有找到或读不出来（见下面的列表）；它们的改动不在补丁里。",
								   r.missing.size()));
	}

	// save
	ImGui::Spacing();
	ImGui::BeginDisabled(outFile.empty() || r.merged.empty());
	auto write = [this](const std::filesystem::path& a_file) {
		std::error_code ec;
		std::filesystem::create_directories(a_file.parent_path(), ec);
		std::ofstream f(a_file, std::ios::binary);
		f.write(reinterpret_cast<const char*>(_mergeResult->file.data()), static_cast<std::streamsize>(_mergeResult->file.size()));
		if (!f) {
			_error = "无法写入 " + platform::ToUtf8(a_file);
			return;
		}
		f.close();
		auto rep = a_file;
		rep.replace_extension(".txt");
		std::ofstream(rep) << _mergeResult->report();
		_mergeSaved = platform::ToUtf8(a_file);
		_status = "已写出合并补丁：" + _mergeSaved;
	};
	if (ImGui::Button(_mo2 ? "在 Mod Organizer 里保存为新模组" : "保存到游戏的 Data 文件夹")) {
		std::error_code ec;
		if (std::filesystem::exists(outFile, ec)) {
			confirm("替换合并补丁", "替换之前做好的合并补丁？\n\n" + platform::ToUtf8(outFile),
				[write, outFile] { write(outFile); });
		} else {
			write(outFile);
		}
	}
	ImGui::EndDisabled();
	tips::Tipf("把 %s 以及一份合并内容的报告（.txt）写入 %s", kPatchName, platform::ToUtf8(outFile.parent_path()).c_str());
	ImGui::SameLine();
	ImGui::BeginDisabled(r.merged.empty());
	if (ImGui::Button("另存为…")) {
		if (auto f = platform::SaveFileDialog(outFile.empty() ? std::filesystem::path(kPatchName) : outFile, "esp", "合并补丁")) {
			write(*f);
		}
	}
	ImGui::EndDisabled();
	tips::Tip("自己选择补丁写入的位置。");
	if (!_mergeSaved.empty()) {
		Colored(theme::Good(), "已保存：" + _mergeSaved);
		if (_mo2) {
			ImGui::TextWrapped("在 Mod Organizer 里：按 F5 刷新，在左侧勾选模组“%s”，并确认右侧插件列表底部的 %s 已勾选。"
							   "让它留在最后。",
				kModFolder, kPatchName);
		} else {
			ImGui::TextWrapped("在你的模组管理器或游戏启动器里启用 %s，并让它留在加载顺序的最后。", kPatchName);
		}
		ImGui::TextWrapped("进游戏之前，可以用 SSEEdit 打开它（Check for Errors）看看它改了什么。");
	}

	// details
	ImGui::Spacing();
	if (ImGui::BeginChild("mergedetails", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
		if (ImGui::CollapsingHeader(Fmt("已合并的记录（%zu）###merged", r.merged.size()).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGuiListClipper clip;
			clip.Begin(static_cast<int>(r.merged.size()));
			while (clip.Step()) {
				for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
					const auto& m = r.merged[static_cast<std::size_t>(i)];
					std::string changes;
					for (const auto& c : m.changes) {
						changes += (changes.empty() ? "" : ";  ") + c;
					}
					ImGui::Text("%s %s [%s]", m.type.c_str(), m.edid.c_str(), m.id.c_str());
					tips::Tipf("以 %s 为基础（获胜的模组）。%s%s", m.winner.c_str(), changes.empty() ? "" : "\n从其他模组加入：",
						changes.c_str());
					ImGui::SameLine();
					ImGui::TextDisabled("获胜模组 %s%s%s", m.winner.c_str(), changes.empty() ? "" : "  |  ", changes.c_str());
				}
			}
		}
		if (ImGui::CollapsingHeader(Fmt("补丁的主文件（%zu）###masters", r.masters.size()).c_str())) {
			for (const auto& m : r.masters) {
				auto it = r.masterWhy.find(m);
				ImGui::BulletText("%s", m.c_str());
				if (it != r.masterWhy.end()) {
					ImGui::SameLine();
					ImGui::TextDisabled("被 %s 需要", it->second.c_str());
				}
			}
		}
		if (!r.skipped.empty() && ImGui::CollapsingHeader(Fmt("未合并（%zu）###skipped", r.skipped.size()).c_str())) {
			for (const auto& s : r.skipped) {
				ImGui::BulletText("%s", s.c_str());
			}
		}
		if (!r.missing.empty() && ImGui::CollapsingHeader(Fmt("找不到的插件（%zu）###missing", r.missing.size()).c_str())) {
			for (const auto& s : r.missing) {
				ImGui::BulletText("%s", s.c_str());
			}
		}
	}
	ImGui::EndChild();
	ImGui::End();
}
