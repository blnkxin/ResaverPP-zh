# ReSaver++ 简体中文版

[ReSaver++](https://github.com/Tonycubed2/ResaverPP) 的简体中文分支。界面、提示、分析结论和导出的报告都是中文，
存档读写与清理规则未做改动。

- `zh-cn`：汉化版（本仓库的默认分支）
- `main`：与上游一致，用来合并作者的更新

## 汉化范围

- 全部界面文字：菜单、列表、详情、按钮、悬浮提示、确认框、错误提示、状态栏、设置、日志。
- 分析结论（Firefly 死循环、SkyUI MCM 注册卡死、未定义脚本、未附加实例等）。
- 导出的 `.txt` 与 `.pdf` 报告。PDF 会内嵌所用汉字的字体子集，文字可搜索、可复制。

保留英文的部分：Papyrus 关键字与反编译输出、存档格式字段名（`REFR`、`CHANGE_FORM_FLAGS` 等）、
插件与脚本名、文件路径、`resaverpp.ini` 的键名、命令行程序 `resaverpp-cli`。

## 构建

需要 Visual Studio 2022（或 2026）的 C++ 桌面工具链和 CMake ≥ 3.21，依赖都在 `third_party/`，不需要 vcpkg：

```bat
cmake -S . -B build/vs -G "Visual Studio 17 2022" -A x64
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

冲突基本集中在字符串那一行。上游新增界面文字后，记得重跑 `python i18n/gen_glyph_ranges.py`
重新生成 `src/gui/UiGlyphs.h`，否则新汉字会缺字形。

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
