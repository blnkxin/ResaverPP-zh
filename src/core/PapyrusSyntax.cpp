// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "PapyrusSyntax.h"

#include <cctype>
#include <cstring>

namespace ess::psc
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

		bool IdStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
		bool IdChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

		// keyword lists as in npp-papyrus (instre2, type1, type2, type3; "as"/"is" are operators)
		const std::unordered_set<std::string>& FlowWords()
		{
			static const std::unordered_set<std::string> s{ "if", "else", "elseif", "endif", "while", "endwhile" };
			return s;
		}
		const std::unordered_set<std::string>& TypeWords()
		{
			static const std::unordered_set<std::string> s{ "bool", "float", "int", "string", "var" };
			return s;
		}
		const std::unordered_set<std::string>& KeywordWords()
		{
			static const std::unordered_set<std::string> s{ "scriptname", "extends", "import", "debugonly", "betaonly", "default", "event", "endevent",
				"state", "endstate", "function", "endfunction", "global", "native", "struct", "endstruct", "property", "endproperty", "auto",
				"autoreadonly", "conditional", "hidden", "const", "mandatory", "group", "endgroup", "collapsed", "collapsedonref", "collapsedonbase",
				"new", "return", "length" };
			return s;
		}
		const std::unordered_set<std::string>& Keyword2Words()
		{
			static const std::unordered_set<std::string> s{ "none", "parent", "self", "true", "false" };
			return s;
		}
		bool IsAnyKeyword(const std::string& l)
		{
			return l == "as" || l == "is" || FlowWords().contains(l) || TypeWords().contains(l) || KeywordWords().contains(l) || Keyword2Words().contains(l);
		}
	}

	void ColorLine(const char* s, std::size_t n, Kind* out, const Context& ctx)
	{
		for (std::size_t i = 0; i < n; ++i) {
			out[i] = Kind::kDefault;
		}
		std::string prev;  // previous significant token (lowercase word, or a punctuation character)
		std::size_t i = 0;
		auto        fill = [&](std::size_t a, std::size_t b, Kind k) {
            for (std::size_t x = a; x < b && x < n; ++x) {
                out[x] = k;
            }
		};
		auto nextSignificant = [&](std::size_t from) {
			while (from < n && (s[from] == ' ' || s[from] == '\t')) {
				++from;
			}
			return from;
		};
		while (i < n) {
			const char c = s[i];
			if (c == ' ' || c == '\t' || c == '\r') {
				++i;
				continue;
			}
			if (c == ';') {
				if (i + 1 < n && s[i + 1] == '/') {
					// ;/ block comment /; on this line
					std::size_t e = i + 2;
					while (e + 1 < n && !(s[e] == '/' && s[e + 1] == ';')) {
						++e;
					}
					e = e + 1 < n ? e + 2 : n;
					fill(i, e, Kind::kComment);
					i = e;
					continue;
				}
				fill(i, n, Kind::kComment);
				return;
			}
			if (c == '{') {
				std::size_t e = i + 1;
				while (e < n && s[e] != '}') {
					++e;
				}
				e = e < n ? e + 1 : n;
				fill(i, e, Kind::kDoc);
				i = e;
				continue;
			}
			if (c == '"') {
				std::size_t e = i + 1;
				while (e < n && s[e] != '"') {
					e += s[e] == '\\' ? 2 : 1;
				}
				e = e < n ? e + 1 : n;
				fill(i, e, Kind::kString);
				i = e;
				prev = "\"";
				continue;
			}
			if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(s[i + 1])) && prev != ")" && !IdChar(prev.empty() ? ' ' : prev.back()))) {
				std::size_t e = i;
				if (c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
					e = i + 2;
					while (e < n && std::isxdigit(static_cast<unsigned char>(s[e]))) {
						++e;
					}
				} else {
					while (e < n && (std::isdigit(static_cast<unsigned char>(s[e])) || s[e] == '.')) {
						++e;
					}
				}
				fill(i, e, Kind::kNumber);
				i = e;
				prev = "0";
				continue;
			}
			if (IdStart(c)) {
				std::size_t e = i;
				while (e < n && (IdChar(s[e]) || (s[e] == ':' && e + 1 < n && IdStart(s[e + 1])))) {
					++e;
				}
				const std::string w(s + i, e - i);
				const std::string l = Lower(w);
				Kind              k = Kind::kDefault;
				if (l == "as" || l == "is") {
					k = Kind::kOperator;
				} else if (FlowWords().contains(l)) {
					k = Kind::kFlow;
				} else if (TypeWords().contains(l)) {
					k = Kind::kType;
				} else if (KeywordWords().contains(l)) {
					k = Kind::kKeyword;
				} else if (Keyword2Words().contains(l)) {
					k = Kind::kKeyword2;
				} else {
					const std::size_t nx = nextSignificant(e);
					const char        nc = nx < n ? s[nx] : '\0';
					// the next word (for "Type name" declarations)
					std::string nextWord;
					std::size_t q = nx;
					if (nc == '[' && q + 1 < n && s[q + 1] == ']') {
						q = nextSignificant(q + 2);
					}
					if (q < n && IdStart(s[q])) {
						std::size_t qe = q;
						while (qe < n && IdChar(s[qe])) {
							++qe;
						}
						nextWord = Lower(std::string(s + q, qe - q));
					}
					if (prev == "function" || prev == "event") {
						k = Kind::kFunction;
					} else if (nc == '(') {
						k = Kind::kFunction;
					} else if (prev == "property") {
						k = Kind::kProperty;
					} else if (prev == "scriptname" || prev == "extends" || prev == "import" || prev == "as" || prev == "is" || prev == "new") {
						k = Kind::kClass;
					} else if (prev == ".") {
						k = Kind::kProperty;
					} else if (ctx.properties.contains(l)) {
						k = Kind::kProperty;
					} else if (ctx.scripts.contains(l)) {
						k = Kind::kClass;
					} else if (!nextWord.empty() && (!IsAnyKeyword(nextWord) || nextWord == "property" || nextWord == "function" || nextWord == "event")) {
						k = Kind::kClass;  // "Type name" declaration
					}
				}
				fill(i, e, k);
				prev = l;
				i = e;
				continue;
			}
			if (std::strchr("()[],=+-*/%.!><|&", c)) {
				out[i] = Kind::kOperator;
				prev = std::string(1, c);
				++i;
				continue;
			}
			++i;
		}
	}

	std::vector<std::string> SplitLines(const std::string& a_text)
	{
		std::vector<std::string> out;
		std::string              cur;
		for (std::size_t i = 0; i < a_text.size(); ++i) {
			const char c = a_text[i];
			if (c == '\r') {
				if (i + 1 < a_text.size() && a_text[i + 1] == '\n') {
					continue;
				}
				out.push_back(cur);
				cur.clear();
			} else if (c == '\n') {
				out.push_back(cur);
				cur.clear();
			} else {
				cur.push_back(c);
			}
		}
		out.push_back(cur);
		return out;
	}

	namespace
	{
		// words of each line, outside comments, strings and doc comments
		std::vector<std::vector<Word>> Words(const std::vector<std::string>& lines, std::vector<bool>& cont)
		{
			std::vector<std::vector<Word>> out(lines.size());
			cont.assign(lines.size(), false);
			bool                           block = false, doc = false;
			for (std::size_t li = 0; li < lines.size(); ++li) {
				const auto& s = lines[li];
				std::size_t i = 0;
				while (i < s.size()) {
					if (block) {
						const auto e = s.find("/;", i);
						if (e == std::string::npos) {
							i = s.size();
							break;
						}
						block = false;
						i = e + 2;
						continue;
					}
					if (doc) {
						const auto e = s.find('}', i);
						if (e == std::string::npos) {
							i = s.size();
							break;
						}
						doc = false;
						i = e + 1;
						continue;
					}
					const char c = s[i];
					if (c == ';') {
						if (i + 1 < s.size() && s[i + 1] == '/') {
							block = true;
							i += 2;
							continue;
						}
						break;
					}
					if (c == '\\') {
						// "\" continues the statement on the next line (only spaces / a comment may follow)
						std::size_t k = i + 1;
						while (k < s.size() && (s[k] == ' ' || s[k] == '\t' || s[k] == '\r')) {
							++k;
						}
						if (k == s.size() || s[k] == ';') {
							cont[li] = true;
						}
						++i;
						continue;
					}
					if (c == '{') {
						doc = true;
						++i;
						continue;
					}
					if (c == '"') {
						++i;
						while (i < s.size() && s[i] != '"') {
							i += s[i] == '\\' ? 2 : 1;
						}
						++i;
						continue;
					}
					if (IdStart(c)) {
						std::size_t e = i;
						while (e < s.size() && (IdChar(s[e]) || (s[e] == ':' && e + 1 < s.size() && IdStart(s[e + 1])))) {
							++e;
						}
						Word w;
						w.text = s.substr(i, e - i);
						w.lower = Lower(w.text);
						w.line = static_cast<int>(li);
						w.begin = static_cast<int>(i);
						w.end = static_cast<int>(e);
						out[li].push_back(std::move(w));
						i = e;
						continue;
					}
					if (std::isdigit(static_cast<unsigned char>(c))) {
						if (c == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
							i += 2;
							while (i < s.size() && std::isxdigit(static_cast<unsigned char>(s[i]))) {
								++i;
							}
						} else {
							while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) {
								++i;
							}
						}
						continue;
					}
					++i;
				}
			}
			return out;
		}

		std::string Trim(const std::string& s)
		{
			const auto a = s.find_first_not_of(" \t");
			if (a == std::string::npos) {
				return {};
			}
			return s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
		}

		bool Has(const std::vector<Word>& ws, const char* w)
		{
			for (const auto& x : ws) {
				if (x.lower == w) {
					return true;
				}
			}
			return false;
		}
	}

	Analysis Analyze(const std::vector<std::string>& lines)
	{
		Analysis          a;
		std::vector<bool> cont;
		auto              words = Words(lines, cont);
		// a statement continued with "\" takes the words of the following lines
		for (std::size_t li = 0; li < words.size(); ++li) {
			std::size_t k = li;
			while (cont[k] && k + 1 < words.size()) {
				++k;
				for (auto& w : words[k]) {
					words[li].push_back(std::move(w));
				}
				words[k].clear();
			}
			li = k;
		}
		std::vector<std::size_t> stack;  // indices into a.blocks
		auto        currentState = [&]() -> std::string {
            for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
                const auto& b = a.blocks[*it];
                if (b.type == Block::Type::kState && b.keywords.size() > 0) {
                    // the state name is the word after "State"
                    const auto& ws = words[b.startLine];
                    for (std::size_t k = 0; k + 1 < ws.size(); ++k) {
                        if (ws[k].lower == "state") {
                            return ws[k + 1].text;
                        }
                    }
                }
            }
            return {};
		};
		auto closeTo = [&](Block::Type t, const Word& w) {
			for (std::size_t k = stack.size(); k-- > 0;) {
				if (a.blocks[stack[k]].type == t) {
					for (std::size_t j = stack.size(); j-- > k + 1;) {
						a.problems.push_back("第 " + std::to_string(a.blocks[stack[j]].startLine + 1) + " 行：代码块没有在第 " +
											 std::to_string(w.line + 1) + " 行之前闭合");
					}
					auto& b = a.blocks[stack[k]];
					b.keywords.push_back(w);
					b.endLine = w.line;
					stack.resize(k);
					return;
				}
			}
			a.problems.push_back("第 " + std::to_string(w.line + 1) + " 行：" + w.text + " 没有对应的开头关键字");
		};

		for (std::size_t li = 0; li < words.size(); ++li) {
			const auto& ws = words[li];
			if (ws.empty()) {
				continue;
			}
			// the statement's keyword: the first block keyword, preceded only by type names / "Auto"
			std::size_t kw = ws.size();
			for (std::size_t k = 0; k < ws.size(); ++k) {
				const auto& l = ws[k].lower;
				static const std::unordered_set<std::string> blockWords{ "if", "elseif", "else", "endif", "while", "endwhile", "function", "endfunction",
					"event", "endevent", "state", "endstate", "property", "endproperty", "struct", "endstruct", "group", "endgroup" };
				if (blockWords.contains(l)) {
					kw = k;
					break;
				}
				if (k >= 2 || (IsAnyKeyword(l) && l != "auto" && !TypeWords().contains(l))) {
					break;
				}
			}
			const int line = static_cast<int>(li);
			if (kw == ws.size()) {
				// ScriptName / script variables
				if (ws[0].lower == "scriptname" && ws.size() > 1) {
					a.scriptName = ws[1].text;
					for (std::size_t k = 2; k + 1 < ws.size(); ++k) {
						if (ws[k].lower == "extends") {
							a.parent = ws[k + 1].text;
						}
					}
				} else if (stack.empty() && ws.size() >= 2 && (!IsAnyKeyword(ws[0].lower) || TypeWords().contains(ws[0].lower))) {
					if (ws[0].lower != "import" && !IsAnyKeyword(ws[1].lower)) {
						OutlineItem it{ OutlineItem::Type::kVariable, ws[1].text, Trim(lines[li]), {}, line };
						a.outline.push_back(std::move(it));
					}
				}
				continue;
			}
			const auto& w = ws[kw];
			const auto& l = w.lower;
			auto        open = [&](Block::Type t) {
                Block b;
                b.type = t;
                b.startLine = line;
                b.keywords.push_back(w);
                a.blocks.push_back(std::move(b));
                stack.push_back(a.blocks.size() - 1);
			};
			const std::string name = kw + 1 < ws.size() ? ws[kw + 1].text : std::string();
			if (l == "if") {
				open(Block::Type::kIf);
			} else if (l == "while") {
				open(Block::Type::kWhile);
			} else if (l == "elseif" || l == "else") {
				if (!stack.empty() && a.blocks[stack.back()].type == Block::Type::kIf) {
					a.blocks[stack.back()].keywords.push_back(w);
				} else {
					a.problems.push_back("第 " + std::to_string(line + 1) + " 行：" + w.text + " 不在 If 内");
				}
			} else if (l == "function" || l == "event") {
				const bool native = Has(ws, "native");
				// a property's Get/Set: "Prop.Get"
				std::string owner;
				if (!stack.empty() && a.blocks[stack.back()].type == Block::Type::kProperty) {
					const auto& pw = words[a.blocks[stack.back()].startLine];
					for (std::size_t k = 0; k + 1 < pw.size(); ++k) {
						if (pw[k].lower == "property") {
							owner = pw[k + 1].text + ".";
						}
					}
				}
				OutlineItem it{ l == "function" ? OutlineItem::Type::kFunction : OutlineItem::Type::kEvent, owner + name, Trim(lines[li]), currentState(), line, native };
				a.outline.push_back(std::move(it));
				if (!native) {
					open(l == "function" ? Block::Type::kFunction : Block::Type::kEvent);
				}
			} else if (l == "state") {
				OutlineItem it{ OutlineItem::Type::kState, name, Trim(lines[li]), {}, line };
				a.outline.push_back(std::move(it));
				open(Block::Type::kState);
			} else if (l == "property") {
				a.properties.insert(Lower(name));
				OutlineItem it{ OutlineItem::Type::kProperty, name, Trim(lines[li]), {}, line };
				a.outline.push_back(std::move(it));
				if (!Has(ws, "auto") && !Has(ws, "autoreadonly") && !Has(ws, "native")) {
					open(Block::Type::kProperty);
				}
			} else if (l == "struct") {
				open(Block::Type::kStruct);
			} else if (l == "group") {
				open(Block::Type::kGroup);
			} else if (l == "endif") {
				closeTo(Block::Type::kIf, w);
			} else if (l == "endwhile") {
				closeTo(Block::Type::kWhile, w);
			} else if (l == "endfunction") {
				closeTo(Block::Type::kFunction, w);
			} else if (l == "endevent") {
				closeTo(Block::Type::kEvent, w);
			} else if (l == "endstate") {
				closeTo(Block::Type::kState, w);
			} else if (l == "endproperty") {
				closeTo(Block::Type::kProperty, w);
			} else if (l == "endstruct") {
				closeTo(Block::Type::kStruct, w);
			} else if (l == "endgroup") {
				closeTo(Block::Type::kGroup, w);
			}
		}
		for (const auto i : stack) {
			a.problems.push_back("第 " + std::to_string(a.blocks[i].startLine + 1) + " 行：" + a.blocks[i].keywords.front().text + " 从未闭合");
		}
		return a;
	}

	const Block* BlockAt(const Analysis& a, int line, int index)
	{
		for (const auto& b : a.blocks) {
			if (line < b.startLine || (b.endLine >= 0 && line > b.endLine)) {
				continue;
			}
			for (const auto& w : b.keywords) {
				if (w.line == line && index >= w.begin && index <= w.end) {
					return &b;
				}
			}
		}
		return nullptr;
	}

	int FindFunction(const Analysis& a, const std::string& fn, const std::string& state)
	{
		const auto f = Lower(fn), st = Lower(state);
		int        any = -1;
		for (const auto& it : a.outline) {
			if ((it.type != OutlineItem::Type::kFunction && it.type != OutlineItem::Type::kEvent) || Lower(it.name) != f) {
				continue;
			}
			if (Lower(it.state) == st) {
				return it.line;
			}
			if (any < 0) {
				any = it.line;
			}
		}
		return any;
	}
}
