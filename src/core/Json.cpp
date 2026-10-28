// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Json.h"

#include <cstdlib>
#include <stdexcept>

namespace ess::json
{
	const Value* Value::get(const std::string& a_key) const
	{
		if (type != Type::kObject) {
			return nullptr;
		}
		auto it = o.find(a_key);
		return it == o.end() ? nullptr : &it->second;
	}

	std::string Value::str(const std::string& a_key) const
	{
		const auto* v = get(a_key);
		return v && v->type == Type::kString ? v->s : std::string();
	}

	namespace
	{
		class Parser
		{
		public:
			explicit Parser(const std::string& t) :
				_t(t)
			{
				// a UTF-8 byte order mark
				if (_t.size() >= 3 && static_cast<unsigned char>(_t[0]) == 0xEF && static_cast<unsigned char>(_t[1]) == 0xBB &&
					static_cast<unsigned char>(_t[2]) == 0xBF) {
					_i = 3;
				}
			}

			Value parse()
			{
				Value v = value(0);
				ws();
				if (_i != _t.size()) {
					fail("结束之后还有多余内容");
				}
				return v;
			}

		private:
			const std::string& _t;
			std::size_t        _i = 0;

			[[noreturn]] void fail(const char* what) const { throw std::runtime_error(std::string("JSON: ") + what + "，偏移 " + std::to_string(_i)); }

			void ws()
			{
				while (_i < _t.size() && (_t[_i] == ' ' || _t[_i] == '\t' || _t[_i] == '\r' || _t[_i] == '\n')) {
					++_i;
				}
			}

			bool lit(const char* w)
			{
				std::size_t k = 0;
				while (w[k]) {
					if (_i + k >= _t.size() || _t[_i + k] != w[k]) {
						return false;
					}
					++k;
				}
				_i += k;
				return true;
			}

			static void put(std::string& out, unsigned cp)
			{
				if (cp < 0x80) {
					out.push_back(static_cast<char>(cp));
				} else if (cp < 0x800) {
					out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
					out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
				} else if (cp < 0x10000) {
					out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
					out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
					out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
				} else {
					out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
					out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
					out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
					out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
				}
			}

			unsigned hex4()
			{
				if (_i + 4 > _t.size()) {
					fail("\\u 转义不完整");
				}
				const unsigned v = static_cast<unsigned>(std::strtoul(_t.substr(_i, 4).c_str(), nullptr, 16));
				_i += 4;
				return v;
			}

			std::string string()
			{
				++_i;  // opening quote
				std::string out;
				while (true) {
					if (_i >= _t.size()) {
						fail("字符串没有结束");
					}
					const char c = _t[_i++];
					if (c == '"') {
						return out;
					}
					if (c != '\\') {
						out.push_back(c);
						continue;
					}
					if (_i >= _t.size()) {
						fail("无效的转义");
					}
					const char e = _t[_i++];
					switch (e) {
					case '"': out.push_back('"'); break;
					case '\\': out.push_back('\\'); break;
					case '/': out.push_back('/'); break;
					case 'b': out.push_back('\b'); break;
					case 'f': out.push_back('\f'); break;
					case 'n': out.push_back('\n'); break;
					case 'r': out.push_back('\r'); break;
					case 't': out.push_back('\t'); break;
					case 'u':
						{
							unsigned cp = hex4();
							if (cp >= 0xD800 && cp < 0xDC00 && _i + 6 <= _t.size() && _t[_i] == '\\' && _t[_i + 1] == 'u') {
								_i += 2;
								const unsigned lo = hex4();
								cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
							}
							put(out, cp);
							break;
						}
					default: fail("无效的转义");
					}
				}
			}

			Value value(int depth)
			{
				if (depth > 200) {
					fail("嵌套层数过多");
				}
				ws();
				if (_i >= _t.size()) {
					fail("意外的结束");
				}
				Value      v;
				const char c = _t[_i];
				if (c == '{') {
					v.type = Value::Type::kObject;
					++_i;
					ws();
					if (_i < _t.size() && _t[_i] == '}') {
						++_i;
						return v;
					}
					while (true) {
						ws();
						if (_i >= _t.size() || _t[_i] != '"') {
							fail("需要键名");
						}
						std::string key = string();
						ws();
						if (_i >= _t.size() || _t[_i] != ':') {
							fail("需要 ':'");
						}
						++_i;
						v.o[std::move(key)] = value(depth + 1);
						ws();
						if (_i < _t.size() && _t[_i] == ',') {
							++_i;
							continue;
						}
						if (_i < _t.size() && _t[_i] == '}') {
							++_i;
							return v;
						}
						fail("需要 ',' 或 '}'");
					}
				}
				if (c == '[') {
					v.type = Value::Type::kArray;
					++_i;
					ws();
					if (_i < _t.size() && _t[_i] == ']') {
						++_i;
						return v;
					}
					while (true) {
						v.a.push_back(value(depth + 1));
						ws();
						if (_i < _t.size() && _t[_i] == ',') {
							++_i;
							continue;
						}
						if (_i < _t.size() && _t[_i] == ']') {
							++_i;
							return v;
						}
						fail("需要 ',' 或 ']'");
					}
				}
				if (c == '"') {
					v.type = Value::Type::kString;
					v.s = string();
					return v;
				}
				if (lit("true")) {
					v.type = Value::Type::kBool;
					v.b = true;
					return v;
				}
				if (lit("false")) {
					v.type = Value::Type::kBool;
					return v;
				}
				if (lit("null")) {
					return v;
				}
				char*       end = nullptr;
				const char* start = _t.c_str() + _i;
				v.n = std::strtod(start, &end);
				if (end == start) {
					fail("意外的字符");
				}
				v.type = Value::Type::kNumber;
				_i += static_cast<std::size_t>(end - start);
				return v;
			}
		};
	}

	Value Parse(const std::string& a_text) { return Parser(a_text).parse(); }
}
