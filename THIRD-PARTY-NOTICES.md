# 第三方声明 / Third-Party Notices

ReSaver++ 使用以下第三方代码，各自的许可证原文见 `third_party/` 内对应目录：

| 组件 | 许可证 | 作者 |
|---|---|---|
| Dear ImGui | MIT | Omar Cornut |
| GLFW | zlib/libpng | Marcus Geelnard, Camilla Löwy |
| LZ4 | BSD 2-Clause | Yann Collet |
| miniz | MIT | Rich Geldreich, Tenacious Software |
| ImGuiColorTextEdit | MIT | BalazsJako（为脚本查看器做过修改） |

其它归属：

- **ReSaver / FallrimTools** — Mark Fairchild（<https://github.com/mdfairch/FallrimTools>，Apache-2.0）。
  存档格式知识与清理规则来自该项目；ReSaver++ 是用 C++ 重新实现的。
- **npp-papyrus**（Notepad++ 插件，作者 blu3mania）— 脚本查看器的配色与关键字分类参考了它，未包含其代码。
- **Bethesda / Creation Kit** — Papyrus 编译器由游戏自带，本程序只调用你已安装的副本，不随附。

本汉化分支新增的代码（`src/core/PdfCjk.*`、`src/gui/UiGlyphs.h` 及各处字符串改动）同样以 Apache-2.0 发布。
