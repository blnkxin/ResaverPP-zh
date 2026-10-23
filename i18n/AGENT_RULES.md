# 汉化任务规则（每个文件必须遵守）

## 目标
把 ReSaver++（Skyrim 存档清理工具，C++ / ImGui 桌面程序）的**用户可见界面文字**改成简体中文。
工作副本：`C:\Users\14034\Documents\deepseek-harness\default-workspace\ResaverPP`
先读 `i18n/glossary.md` 统一术语。

## 允许改的
只改 C++ 源码里**字符串字面量的内容**（`"..."` 里展示给用户看的英文句子、单词、按钮、菜单、列标题、提示、错误信息、确认框文字、报告文字）。

## 绝对不要改的
1. **代码逻辑**：一个字符都不能动（不改变量名、不加删行、不动注释、不调整结构）。
2. 不是给用户看的字符串，包括：
   - 文件名 / 扩展名 / 通配符：`.ess` `.pex` `.psc` `.bsa` `.txt` `.pdf` `.json` `*.esp` `ModOrganizer.ini` `vortex.deployment.json` `resaverpp.ini` 等；
   - 路径（含 `\\`、`/`、`%LOCALAPPDATA%`、盘符）；
   - INI/JSON/设置文件的**键名**（如 `codepage=`、`sources=`、`theme=`），以及会写进文件的格式字符串；
   - 游戏/文件格式的**结构字段名**：全大写（`CHANGE_FORM_FLAGS`、`REFR_INVENTORY`、`SUB_DATA`）以及 `REF` `UNK1` `POS` `COUNT` `ROT` `FLAG` `RANK` `INFO` `DATA` 这类；
   - Papyrus 语言关键字与反编译产物（`scriptname` `extends` `if` `endif` `function` `endfunction` `property` `auto` `state` `event` `int` `float` `bool` `string` `none` `self` `parent` `as` `is` 等）；
   - 插件名、脚本名、游戏名（`Skyrim.esm`、`Skyrim Special Edition`、`Skyrim VR` 这类游戏标识保持原样）；
   - ImGui 的 ID 后缀：`"文字##id"`、`"###id"`、`"##x"` —— **只译 `##` 之前的可见文字**，`##` 与之后原样保留。
3. **格式占位符原样保持**：`{}` `{0}` `%s` `%zu` `%d` `%u` `%f` `%02X` `\n` `\t` `%%` 一个都不能少、不能改名、不能换位置数量；只翻译它们周围的英文。
   例：`"{} scripts in {} mods"` → `"{} 个脚本，分布在 {} 个模组中"`。
4. 用来**比较/查找外部数据**的字符串（例如 `if (x == "...")`、`s.rfind("...", 0) == 0`、`map["..."]`、按 key 查表、`std::filesystem::path("...")`、ImGui 里作为唯一 ID 且被别处引用的名字）：**保持英文**，并在总结里列出。

## 风格
- 简体中文，简洁。按钮/菜单/表头 2–6 字（`Open save...` → `打开存档…`，`Close` → `关闭`）。
- 句子用全角标点：`，。：；！？（）`；英文术语与数字两侧不必强留空格，但要统一。
- 不要出现机翻腔：`Failed to open the save` → `无法打开存档`；`No scripts found` → `没有找到脚本`。
- 省略号用 `…`（与原文 `...` 对应）；不要用中文省略号 `。` 结尾的祈使句。
- 保留原文的大小写风格（标题用词照旧，不要把全部字母大写）。

## 编辑方式
- 用**精确的局部替换**（edit 工具，old_string 唯一）。**不要**整文件重写。
- 文件是 UTF-8（无 BOM），保持 UTF-8 保存；中文直接写字面量，不要用 `\u` 转义。
- 相邻字符串字面量拼接（
  ```
  "first part "
  "second part"
  ```
  ）要分别翻译，**保留原来的首尾空格**，拼起来才是完整的句子。
- 同一个句子被拆成多段、或同一段文字在文件里出现多次：**同样的英文必须译成同样的中文**（术语一致）。
- 不要新增 `\n`、不要引入未转义的 `"` 或行尾反斜杠。

## 交付
改完后报告（简短）：
1. 改了哪些文件的哪些区域（大致行号范围）；
2. **故意保留英文**的字符串清单及原因（尤其是可能参与逻辑比较的）；
3. 任何你不确定是否该改的地方。
不要编译（我会统一编译）。不要动 `third_party/`、`tests/`、`i18n/`。
