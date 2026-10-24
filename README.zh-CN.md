# ReSaver++ 简体中文版

[ReSaver++](https://github.com/Tonycubed2/ResaverPP) 的简体中文分支（当前对应上游 **v19**）。界面、提示、分析结论和
导出的报告都是中文，存档读写与清理规则未做改动。

- `zh-cn`：汉化版（本仓库的默认分支）
- `main`：与上游一致，用来合并作者的更新

## 汉化范围

- 全部界面文字：菜单、列表、详情、按钮、悬浮提示、确认框、错误提示、状态栏、设置、日志。
- 分析结论（Firefly 死循环、SkyUI MCM 注册卡死、未定义脚本、未附加实例等）。
- 导出的 `.txt` 与 `.pdf` 报告。PDF 会内嵌所用汉字的字体子集，文字可搜索、可复制。

保留英文的部分：Papyrus 关键字与反编译输出、存档格式字段名（`REFR`、`CHANGE_FORM_FLAGS` 等）、
插件与脚本名、文件路径、`resaverpp.ini` 的键名、命令行程序 `resaverpp-cli`。

## 上游 v19 带来的新功能

- **任务页**：存档里每个任务的状态、已完成阶段、日志条目与目标（从插件读取），完成阶段的脚本会给什么、
  会启动哪些任务；「复制控制台命令」给出 `setstage` 命令。
- **只读模式**：结构对不上（可能写回会损坏）的存档以只读打开，能看不能存，并列出具体问题。
- **更安全的保存**：先写临时文件并回读校验，保留一代 `.ess.bak` / `.skse.bak`，协同存档写失败时自动回滚。
- **「已不再安装的模组与脚本…」**：新增「扫描没找到、但游戏做存档时还在加载」的脚本分组（默认不勾选）；
  加载顺序或模组文件夹与存档对不上时，所有项都不勾选并说明原因。
- **v18 的清理修复**：挂起/排队调用递归清理、Creation Club 插件识别更严格。

### 关于「实时游戏」（ReSaverHelper）

上游的「实时游戏」功能依赖一个**可选的 SKSE 插件** `ReSaverHelper.dll`（在游戏里执行任务阶段、刷新任务日志）。
作者目前**尚未在 Nexus 发布这个 DLL**，发布包也只有两个 exe。本分支因此：

- 代码原样同步，不删不改；
- 不提供、也不构建该 DLL（`helper/` 是独立的 CMake 工程，需要 vcpkg + CommonLibSSE-NG）；
- 相关界面文字（设置里的 Live game 区块、任务页的「在游戏中完成」按钮、状态栏的 Live 状态）**暂时保留英文**，
  等作者发布 DLL 后再汉化。

没有装该 DLL 时，这些按钮是灰的，状态栏显示 `Live game: not connected`，其它功能完全不受影响。

## 构建

需要 Visual Studio 的 C++ 桌面工具链和 CMake ≥ 3.21，依赖都在 `third_party/`，不需要 vcpkg：

```bat
rem Visual Studio 2022
cmake -S . -B build/vs -G "Visual Studio 17 2022" -A x64
rem 或者 Visual Studio 2026（没有装 2022 时）
cmake -S . -B build/vs -G "Visual Studio 18 2026" -A x64

cmake --build build/vs --config Release --target ReSaverPP resaverpp-cli
```

也可以直接运行 `build.cmd`（会调用 vcvars64 后构建）。产物在 `build\vs\Release\`。

> 注意：`ALL_BUILD` 目标在部分受限环境里会异常退出，请像上面那样指定 target。

## 与上游同步

```bat
git fetch upstream
git checkout zh-cn
git merge upstream/main
```

冲突基本集中在译过的字符串行上（v19 这次只有 13 个冲突块）。上游新增界面文字后，记得重跑
`python i18n/gen_glyph_ranges.py` 重新生成 `src/gui/UiGlyphs.h`，否则新汉字会缺字形
（脚本现在扫描 `src/` 下全部源码，不再有需要手工维护的白名单）。

## 目录说明

| 路径 | 说明 |
|---|---|
| `src/gui/UiGlyphs.h` | 界面用到的汉字码位表（由 `i18n/gen_glyph_ranges.py` 生成，勿手改） |
| `src/core/PdfCjk.{h,cpp}` | TrueType 字体子集化 + PDF CIDFontType2，用于中文 PDF |
| `src/core/Report.cpp` | 报告排版改为按显示列宽（中文占两列），PDF 支持中文 |
| `i18n/` | 术语表与辅助脚本：抽取字符串、生成字形表、校验 PDF、批量截图 |

## 许可与署名

Apache-2.0（见 `LICENSE`）。原程序为 Tonycubed2 的 ReSaver++；存档格式知识与清理规则来自
Mark Fairchild 的 ReSaver / FallrimTools。第三方库声明见 `THIRD-PARTY-NOTICES.md` 与 `NOTICE.md`。

本分支只改动了文字与排版相关代码，没有改动存档读写与清理逻辑。
