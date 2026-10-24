// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// .pex -> .psc decompiler (own implementation; the approach is the usual one used by
// Champollion and others: fold the compiler's temporaries back into expressions and rebuild
// If/ElseIf/Else/While from the jump pattern the Papyrus compiler emits).
#include "Pex.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

namespace ess::pex
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
		bool StartsI(const std::string& s, const char* p) { return Lower(s).rfind(p, 0) == 0; }
		bool IsTemp(const std::string& n)
		{
			return StartsI(n, "::temp") && n.size() > 6 && n.find_first_not_of("0123456789", 6) == std::string::npos;
		}
		bool IsMangled(const std::string& n) { return StartsI(n, "::mangled_"); }
		// the compiler renames a local that shadows another one to "::mangled_<name>_<n>"
		std::string Unmangle(const std::string& n) { return IsMangled(n) ? n.substr(10) : n; }
		bool        IsSourceLocal(const std::string& n) { return !StartsI(n, "::") || IsMangled(n); }
		bool IsNoneVar(const std::string& n) { return Lower(n) == "::nonevar"; }

		// a parameter as declared in a .psc source
		struct Param
		{
			std::string name, def;  // def: default value as written ("" = none)
		};
		// parameters of a function of a script (looked up in the sources and their parents); null if unknown
		using Signature = std::function<const std::vector<Param>*(const std::string& a_type, const std::string& a_fn)>;

		// does a literal argument equal a default value as written in a source?
		bool LiteralEquals(const std::string& a_literal, std::uint8_t a_type, const std::string& a_def)
		{
			const auto d = Lower(a_def);
			switch (a_type) {
			case 0: return d == "none";
			case 5: return d == Lower(a_literal);
			case 3:
			case 4:
				{
					char*        end = nullptr;
					const double x = std::strtod(d.c_str(), &end);
					if (end == d.c_str() || *end) {
						return false;
					}
					return std::strtod(a_literal.c_str(), nullptr) == x;
				}
			case 2: return a_def == a_literal;
			default: return false;
			}
		}

		std::string Cap(std::string s)
		{
			if (!s.empty()) {
				s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
			}
			return s;
		}

		std::string FloatText(float f) { return FloatLiteral(f); }

		std::string StringText(const std::string& s)
		{
			std::string o = "\"";
			for (char c : s) {
				switch (c) {
				case '"': o += "\\\""; break;
				case '\\': o += "\\\\"; break;
				case '\n': o += "\\n"; break;
				case '\t': o += "\\t"; break;
				default: o.push_back(c);
				}
			}
			return o + "\"";
		}

		// precedence: 0 atom/member/call/index, 3 cast, 4 unary, 5 mul, 6 add, 7 compare, 8 and, 9 or
		struct Expr
		{
			std::string text;
			int         prec = 0;
			std::string type;          // Papyrus type when known (lowercase)
			bool        hasCall = false;
			std::size_t seq = 0;       // creation order of the instruction that produced it
			bool        isCast = false;
			std::string eqLeft, eqRight;  // set for "a == b" (so "!(a == b)" can print as "a != b")
			std::size_t innerSeq = 0;  // for casts: seq of the value being cast (0 = not a temporary)
			std::string uncast;        // the expression without its cast
			int         uncastPrec = 0;
		};

		struct Stmt
		{
			enum class K
			{
				kText,
				kIf,
				kWhile
			} k = K::kText;
			std::string                                          text;
			std::vector<std::pair<std::string, std::vector<Stmt>>> branches;  // If: (condition, body); condition "" = Else
			std::vector<Stmt>                                    body;      // While
		};

		std::string Wrap(const Expr& e, int maxPrec) { return e.prec > maxPrec ? "(" + e.text + ")" : e.text; }

		class FnDecompiler
		{
		public:
			FnDecompiler(const File& f, const Object& o, const Function& fn, const std::map<std::string, std::string>& autoVars,
				const std::map<std::string, std::string>& objectTypes, const Signature& signature) :
				_f(f), _o(o), _fn(fn), _autoVars(autoVars), _types(objectTypes), _signature(signature)
			{
				for (const auto& [n, t] : fn.params) {
					_types[Lower(f.str(n))] = Lower(f.str(t));
					_typeText[Lower(f.str(n))] = f.str(t);
				}
				for (const auto& [n, t] : fn.locals) {
					_types[Lower(f.str(n))] = Lower(f.str(t));
					_typeText[Lower(f.str(n))] = f.str(t);
				}
				_types["self"] = Lower(f.str(o.name));
			}

			std::vector<Stmt> run()
			{
				auto b = block(0, _fn.code.size());
				flushAll(b);
				return b;
			}

			std::vector<std::string> warnings;

		private:
			const File&                        _f;
			const Object&                      _o;
			const Function&                    _fn;
			const std::map<std::string, std::string>& _autoVars;
			std::map<std::string, std::string> _types, _typeText;  // lowercase name -> lowercase type / type as written
			const Signature&                   _signature;
			std::map<std::string, Expr>        _pending;  // temp (lowercase) -> expression
			std::size_t                        _seq = 0;
			std::size_t                        _cur = 0;  // instruction being decompiled

			std::string name(const Value& v) const { return v.type == 1 || v.type == 2 ? _f.str(v.s) : std::string(); }
			std::string typeText(const std::string& n) const
			{
				auto it = _typeText.find(Lower(n));
				return it == _typeText.end() ? std::string("?") : it->second;
			}
			std::string typeOfName(const std::string& n) const
			{
				auto it = _types.find(Lower(n));
				return it == _types.end() ? std::string() : it->second;
			}

			Expr value(const Value& v)
			{
				switch (v.type) {
				case 0: return { "None", 0, "none" };
				case 2: return { StringText(_f.str(v.s)), 0, "string" };
				case 3: return { std::to_string(v.i), v.i < 0 ? 4 : 0, "int" };
				case 4: return { FloatText(v.f), v.f < 0 ? 4 : 0, "float" };
				case 5: return { v.b ? "True" : "False", 0, "bool" };
				default: break;
				}
				const auto n = _f.str(v.s);
				const auto l = Lower(n);
				if (IsTemp(n)) {
					auto it = _pending.find(l);
					if (it != _pending.end()) {
						Expr e = it->second;
						_pending.erase(it);
						return e;
					}
					warnings.push_back("temporary " + n + " read before it was set");
					return { n, 0, typeOfName(n) };
				}
				if (IsNoneVar(n)) {
					return { "None", 0, "none" };
				}
				if (auto it = _autoVars.find(l); it != _autoVars.end()) {
					return { it->second, 0, typeOfName(n) };
				}
				if (l.size() > 6 && l.rfind("::", 0) == 0 && l.ends_with("_var")) {
					return { n.substr(2, n.size() - 6), 0, typeOfName(n) };  // a parent script's Auto property
				}
				return { Unmangle(n), 0, typeOfName(n) };
			}

			// a condition: the compiler adds "cast to Bool" itself, so drop it
			Expr cond(Expr e)
			{
				if (e.prec == 3 && e.text.size() > 8 && Lower(e.text).ends_with(" as bool")) {
					e.text = e.text.substr(0, e.text.size() - 8);
					if (e.text.size() > 1 && e.text.front() == '(' && e.text.back() == ')') {
						e.text = e.text.substr(1, e.text.size() - 2);
						e.prec = 9;  // conservative: was parenthesized for a reason
					} else {
						e.prec = 0;
					}
				}
				return e;
			}

			// a_type / a_fn: the function called (for its parameter names and defaults, when the source is known)
			std::string args(const Instruction& in, std::size_t from, const std::string& a_type = {}, const std::string& a_fn = {})
			{
				// from = index of the count value
				const int         n = in.args.size() > from ? in.args[from].i : 0;
				std::vector<Expr> v;
				std::vector<bool> literalNone;
				for (int k = 0; k < n && from + 1 + k < in.args.size(); ++k) {
					const auto& raw = in.args[from + 1 + k];
					literalNone.push_back(raw.type == 0);
					v.push_back(value(raw));
				}
				// the compiler adds implicit casts after evaluating all arguments: a cast made after a
				// later argument's value is one it added itself
				for (std::size_t k = 0; k < v.size(); ++k) {
					if (!v[k].isCast) {
						continue;
					}
					// something else happened between computing the value and casting it
					bool implicit = v[k].innerSeq && v[k].seq > v[k].innerSeq + 1;
					for (std::size_t m = k + 1; m < v.size() && !implicit; ++m) {
						implicit = v[m].seq && v[m].seq < v[k].seq;
					}
					if (implicit) {
						v[k].text = v[k].uncast;
						v[k].prec = v[k].uncastPrec;
					}
				}
				// a literal None argument is a default the compiler filled in (an explicit "None" is cast to the
				// parameter type first). With the callee's source, trailing literals equal to their defaults are
				// dropped too, and arguments after a skipped one are passed by name.
				const std::vector<Param>* sig = _signature && !a_type.empty() ? _signature(a_type, a_fn) : nullptr;
				if (sig && sig->size() != v.size()) {
					sig = nullptr;
				}
				std::vector<bool> skip(v.size(), false);
				for (std::size_t k = 0; k < v.size(); ++k) {
					skip[k] = literalNone[k] && (!sig || !(*sig)[k].def.empty());
				}
				std::size_t count = v.size();
				while (count > 0) {
					const auto  k = count - 1;
					const auto& raw = in.args[from + 1 + k];
					const bool  isDefault = skip[k] || (sig && !(*sig)[k].def.empty() && raw.type >= 2 && raw.type <= 5 &&
                                                         LiteralEquals(v[k].text, raw.type, (*sig)[k].def));
					if (!isDefault) {
						break;
					}
					count = k;
				}
				std::string s;
				bool        named = false;
				for (std::size_t k = 0; k < count; ++k) {
					if (skip[k]) {
						if (!sig) {
							s += std::string(s.empty() ? "" : ", ") + "None";  // can't name the parameters: pass it
							continue;
						}
						named = true;
						continue;
					}
					s += (s.empty() ? "" : ", ") + (named ? (*sig)[k].name + " = " : std::string()) + v[k].text;
				}
				return s;
			}

			void flushCalls(std::vector<Stmt>& out)
			{
				std::vector<const Expr*> order;
				for (const auto& [k, e] : _pending) {
					order.push_back(&e);  // unused results: calls, and the odd expression statement ("a == b")
				}
				std::sort(order.begin(), order.end(), [](const Expr* a, const Expr* b) { return a->seq < b->seq; });
				for (const auto* e : order) {
					out.push_back({ Stmt::K::kText, e->text });
				}
				_pending.clear();
			}
			void flushAll(std::vector<Stmt>& out) { flushCalls(out); }

			void emit(std::vector<Stmt>& out, const std::string& text)
			{
				flushCalls(out);
				out.push_back({ Stmt::K::kText, text });
			}

			// write an expression to a destination
			void store(std::vector<Stmt>& out, const Value& dest, Expr e)
			{
				const auto d = name(dest);
				if (IsTemp(d)) {
					const auto key = Lower(d);
					if (auto it = _pending.find(key); it != _pending.end()) {
						// overwritten before use: it was a statement, and so was any earlier call whose result
						// is never read either (keep them in order)
						const auto               limit = it->second.seq;
						std::vector<std::string> dead{ key };
						for (const auto& [k, pe] : _pending) {
							if (k != key && pe.hasCall && pe.seq < limit && !readLater(k)) {
								dead.push_back(k);
							}
						}
						std::sort(dead.begin(), dead.end(), [&](const std::string& x, const std::string& y) { return _pending[x].seq < _pending[y].seq; });
						for (const auto& k : dead) {
							out.push_back({ Stmt::K::kText, _pending[k].text });
							_pending.erase(k);
						}
					}
					e.type = typeOfName(d).empty() ? e.type : typeOfName(d);
					e.seq = ++_seq;
					_pending[key] = e;
					return;
				}
				if (IsNoneVar(d)) {
					emit(out, e.text);
					return;
				}
				std::string target = Unmangle(d);
				if (auto it = _autoVars.find(Lower(d)); it != _autoVars.end()) {
					target = it->second;
				} else if (Lower(d).size() > 6 && Lower(d).rfind("::", 0) == 0 && Lower(d).ends_with("_var")) {
					target = d.substr(2, d.size() - 6);
				}
				emit(out, target + " = " + e.text);
			}

			Expr binary(const Value& a, const Value& b, const char* op, int prec, const std::string& type)
			{
				const auto l = value(a);
				const auto r = value(b);
				const bool nonAssoc = prec == 7;
				Expr       e{ Wrap(l, nonAssoc ? prec - 1 : prec) + " " + op + " " + Wrap(r, prec - 1), prec, type, l.hasCall || r.hasCall };
				if (std::string(op) == "==") {
					e.eqLeft = Wrap(l, prec - 1);
					e.eqRight = Wrap(r, prec - 1);
				}
				return e;
			}

			Expr member(const Expr& obj, const std::string& tail, bool call)
			{
				return { Wrap(obj, 0) + "." + tail, 0, "", call || obj.hasCall };
			}

			// is (i, t) the second half of "a && b" / "a || b" for temp T?
			bool shortCircuit(std::size_t i, std::size_t t, const std::string& T) const
			{
				if (t <= i + 1 || t > _fn.code.size()) {
					return false;
				}
				// the last instruction before the label must write T (assign, cast, not, compare)
				const auto& last = _fn.code[t - 1];
				if (!(last.op == 10 || last.op == 13 || last.op == 14 || (last.op >= 15 && last.op <= 19))) {
					return false;
				}
				if (last.args.empty() || Lower(name(last.args[0])) != Lower(T)) {
					return false;
				}
				// nothing in between may be a statement (writes to non-temps, discarded calls, returns, plain jumps)
				for (std::size_t k = i + 1; k < t; ++k) {
					const auto& in = _fn.code[k];
					switch (in.op) {
					case 20:
					case 26:
					case 29:
					case 33:
						return false;
					case 21:
					case 22:
						break;  // nested short circuits
					case 23:
					case 25:
					case 28:
						if (!IsTemp(name(in.args[2]))) {
							return false;
						}
						break;
					case 24:
						if (!IsTemp(name(in.args[1]))) {
							return false;
						}
						break;
					case 34:
					case 35:
						if (!IsTemp(name(in.args[1]))) {
							return false;
						}
						break;
					default:
						if (!in.args.empty() && !IsTemp(name(in.args[0]))) {
							return false;
						}
						break;
					}
				}
				return true;
			}

			// destination argument of an instruction (-1: none)
			static int DestIndex(std::uint8_t op)
			{
				if ((op >= 1 && op <= 19) || op == 27 || op == 30 || op == 31 || op == 32) {
					return 0;
				}
				switch (op) {
				case 23:
				case 25:
				case 28: return 2;
				case 24:
				case 34:
				case 35: return 1;
				default: return -1;
				}
			}

			// is temp T (lowercase) read after the current instruction before it is written again?
			bool readLater(const std::string& T) const
			{
				for (std::size_t k = _cur + 1; k < _fn.code.size(); ++k) {
					const auto& in = _fn.code[k];
					const int   d = DestIndex(in.op);
					bool        written = false;
					for (std::size_t a = 0; a < in.args.size(); ++a) {
						if (in.args[a].type != 1 || Lower(_f.str(in.args[a].s)) != T) {
							continue;
						}
						if (static_cast<int>(a) == d) {
							written = true;
						} else {
							return true;
						}
					}
					if (written) {
						return false;
					}
				}
				return false;
			}

			std::size_t target(std::size_t i) const
			{
				const auto& in = _fn.code[i];
				return static_cast<std::size_t>(static_cast<long long>(i) + in.args.back().i);
			}

			std::vector<Stmt> block(std::size_t b, std::size_t e)
			{
				std::vector<Stmt> out;
				std::size_t       i = b;
				while (i < e) {
					const auto& in = _fn.code[i];
					switch (in.op) {
					case 0:  // nop
						++i;
						continue;
					case 21:  // jmpt
					case 22:  // jmpf
						{
							const auto c = name(in.args[0]);
							const auto t = target(i);
							if (IsTemp(c) && t > i && t <= e && shortCircuit(i, t, c)) {
								const auto A = cond(value(in.args[0]));
								auto       sub = block(i + 1, t);
								for (auto& s : sub) {
									out.push_back(std::move(s));  // should be none
								}
								auto Bv = _pending.find(Lower(c));
								Expr B = Bv != _pending.end() ? cond(Bv->second) : Expr{ c };
								if (Bv != _pending.end()) {
									_pending.erase(Bv);
								}
								const bool isAnd = in.op == 22;
								const int  p = isAnd ? 8 : 9;
								_pending[Lower(c)] = { Wrap(A, p) + (isAnd ? " && " : " || ") + Wrap(B, p - 1), p, "bool", A.hasCall || B.hasCall };
								i = t;
								continue;
							}
							if (in.op == 22 && t > i && t <= e) {
								const auto condExpr = cond(value(in.args[0]));
								flushCalls(out);
								// while: the block ends with a jump back to (or before) the condition
								if (t - 1 > i && _fn.code[t - 1].op == 20 && target(t - 1) <= i && target(t - 1) >= b) {
									Stmt w{ Stmt::K::kWhile, condExpr.text };
									w.body = block(i + 1, t - 1);
									flushCalls(w.body);
									out.push_back(std::move(w));
									i = t;
									continue;
								}
								Stmt        s{ Stmt::K::kIf };
								std::size_t end = t;
								std::vector<Stmt> thenBody, elseBody;
								if (t - 1 > i && _fn.code[t - 1].op == 20 && target(t - 1) >= t && target(t - 1) <= e) {
									end = target(t - 1);
									thenBody = block(i + 1, t - 1);
									flushCalls(thenBody);
									elseBody = block(t, end);
									flushCalls(elseBody);
								} else {
									thenBody = block(i + 1, t);
									flushCalls(thenBody);
								}
								s.branches.push_back({ condExpr.text, std::move(thenBody) });
								if (elseBody.size() == 1 && elseBody[0].k == Stmt::K::kIf) {
									for (auto& br : elseBody[0].branches) {
										s.branches.push_back(std::move(br));  // ElseIf chain
									}
								} else if (!elseBody.empty()) {
									s.branches.push_back({ "", std::move(elseBody) });
								}
								out.push_back(std::move(s));
								i = end;
								continue;
							}
							emit(out, "; unstructured " + std::string(OpcodeName(in.op)) + " " + c);
							warnings.push_back("unstructured jump");
							++i;
							continue;
						}
					case 20:
						if (target(i) == i + 1) {
							++i;  // jump to the next instruction (empty Else)
							continue;
						}
						emit(out, "; unstructured jmp");
						warnings.push_back("unstructured jump");
						++i;
						continue;
					default:
						_cur = i;
						linear(out, in);
						++i;
						continue;
					}
				}
				return out;
			}

			void linear(std::vector<Stmt>& out, const Instruction& in)
			{
				const auto& a = in.args;
				switch (in.op) {
				case 1: store(out, a[0], binary(a[1], a[2], "+", 6, "int")); break;
				case 2: store(out, a[0], binary(a[1], a[2], "+", 6, "float")); break;
				case 3: store(out, a[0], binary(a[1], a[2], "-", 6, "int")); break;
				case 4: store(out, a[0], binary(a[1], a[2], "-", 6, "float")); break;
				case 5: store(out, a[0], binary(a[1], a[2], "*", 5, "int")); break;
				case 6: store(out, a[0], binary(a[1], a[2], "*", 5, "float")); break;
				case 7: store(out, a[0], binary(a[1], a[2], "/", 5, "int")); break;
				case 8: store(out, a[0], binary(a[1], a[2], "/", 5, "float")); break;
				case 9: store(out, a[0], binary(a[1], a[2], "%", 5, "int")); break;
				case 10:
					{
						const bool inPlace = Lower(name(a[0])) == Lower(name(a[1]));
						const auto v = value(a[1]);
						if (inPlace && !v.eqLeft.empty()) {
							store(out, a[0], { v.eqLeft + " != " + v.eqRight, 7, "bool", v.hasCall });  // "!=" compiles to == then NOT in place
						} else {
							store(out, a[0], { "!" + Wrap(v, 4), 4, "bool", v.hasCall });
						}
						break;
					}
				case 11:
				case 12:
					{
						const auto v = value(a[1]);
						store(out, a[0], { "-" + Wrap(v, 4), 4, in.op == 11 ? "int" : "float", v.hasCall });
						break;
					}
				case 13: store(out, a[0], value(a[1])); break;
				case 14:
					{
						auto       v = value(a[1]);
						const auto dt = typeOfName(name(a[0]));
						// casts the compiler inserts by itself: same type, None, int -> float, anything -> bool / string
						// (a same-type cast of objects is kept: the compiler only emits those for an explicit "as")
						const bool implicit = v.text == "None" || (dt == "float" && v.type == "int") || dt == "bool" || dt == "string";
						if (implicit) {
							v.type = dt.empty() ? v.type : dt;
							store(out, a[0], v);
						} else {
							Expr ce{ Wrap(v, 2) + " as " + typeText(name(a[0])), 3, dt, v.hasCall };
							ce.isCast = true;
							ce.innerSeq = v.seq;
							ce.uncast = v.text;
							ce.uncastPrec = v.prec;
							store(out, a[0], ce);
						}
						break;
					}
				case 15: store(out, a[0], binary(a[1], a[2], "==", 7, "bool")); break;
				case 16: store(out, a[0], binary(a[1], a[2], "<", 7, "bool")); break;
				case 17: store(out, a[0], binary(a[1], a[2], "<=", 7, "bool")); break;
				case 18: store(out, a[0], binary(a[1], a[2], ">", 7, "bool")); break;
				case 19: store(out, a[0], binary(a[1], a[2], ">=", 7, "bool")); break;
				case 23:  // callmethod name obj dest count args...
					{
						const auto fname = name(a[0]);
						auto       obj = value(a[1]);
						const auto ar = args(in, 3, Lower(obj.text) == "self" ? Lower(_f.str(_o.name)) : obj.type, fname);
						Expr       e = Lower(obj.text) == "self" ? Expr{ fname + "(" + ar + ")", 0, "", true } : member(obj, fname + "(" + ar + ")", true);
						e.hasCall = true;
						store(out, a[2], e);
						break;
					}
				case 24:  // callparent name dest count args...
					store(out, a[1], { "Parent." + name(a[0]) + "(" + args(in, 2, Lower(_f.str(_o.parent)), name(a[0])) + ")", 0, "", true });
					break;
				case 25:  // callstatic class name dest count args...
					store(out, a[2], { name(a[0]) + "." + name(a[1]) + "(" + args(in, 3, Lower(name(a[0])), name(a[1])) + ")", 0, "", true });
					break;
				case 26:
					{
						const auto v = value(a[0]);
						const bool none = v.text == "None" && Lower(_f.str(_fn.returnType)) == "none";
						emit(out, none ? "Return" : "Return " + v.text);
						break;
					}
				case 27: store(out, a[0], binary(a[1], a[2], "+", 6, "string")); break;
				case 28:  // propget prop obj dest
					{
						auto obj = value(a[1]);
						Expr e = Lower(obj.text) == "self" ? Expr{ name(a[0]), 0 } : member(obj, name(a[0]), false);
						store(out, a[2], e);
						break;
					}
				case 29:  // propset prop obj value
					{
						auto       obj = value(a[1]);
						const auto v = value(a[2]);
						emit(out, (Lower(obj.text) == "self" ? name(a[0]) : Wrap(obj, 0) + "." + name(a[0])) + " = " + v.text);
						break;
					}
				case 30:  // array_create dest size
					{
						auto t = typeOfName(name(a[0]));
						if (t.size() > 2 && t.ends_with("[]")) {
							t = t.substr(0, t.size() - 2);
						}
						auto tt = typeText(name(a[0]));
						if (tt.size() > 2 && tt.ends_with("[]")) {
							tt = tt.substr(0, tt.size() - 2);
						}
						store(out, a[0], { "new " + tt + "[" + value(a[1]).text + "]", 0, typeOfName(name(a[0])) });
						break;
					}
				case 31:  // array_length dest arr
					store(out, a[0], member(value(a[1]), "Length", false));
					break;
				case 32:  // array_getelement dest arr index
					{
						const auto arr = value(a[1]);
						const auto idx = value(a[2]);
						store(out, a[0], { Wrap(arr, 0) + "[" + idx.text + "]", 0, "", arr.hasCall || idx.hasCall });
						break;
					}
				case 33:  // array_setelement arr index value
					{
						const auto arr = value(a[0]);
						const auto idx = value(a[1]);
						const auto v = value(a[2]);
						emit(out, Wrap(arr, 0) + "[" + idx.text + "] = " + v.text);
						break;
					}
				case 34:  // array_findelement arr dest value start
				case 35:
					{
						const auto arr = value(a[0]);
						const auto v = value(a[2]);
						const auto st = value(a[3]);
						const bool defaultStart = in.op == 34 ? st.text == "0" : st.text == "-1";
						store(out, a[1], member(arr, std::string(in.op == 34 ? "Find(" : "RFind(") + v.text + (defaultStart ? "" : ", " + st.text) + ")", true));
						break;
					}
				default:
					emit(out, std::string("; ") + OpcodeName(in.op));
					warnings.push_back(std::string("unhandled opcode ") + OpcodeName(in.op));
					break;
				}
			}
		};

		void Print(std::vector<std::string>& L, const std::vector<Stmt>& body, int indent)
		{
			const std::string pad(static_cast<std::size_t>(indent) * 4, ' ');
			for (const auto& s : body) {
				switch (s.k) {
				case Stmt::K::kText:
					L.push_back(pad + s.text);
					break;
				case Stmt::K::kWhile:
					L.push_back(pad + "While " + s.text);
					Print(L, s.body, indent + 1);
					L.push_back(pad + "EndWhile");
					break;
				case Stmt::K::kIf:
					for (std::size_t b = 0; b < s.branches.size(); ++b) {
						const auto& [c, body2] = s.branches[b];
						if (b == 0) {
							L.push_back(pad + "If " + c);
						} else if (c.empty()) {
							L.push_back(pad + "Else");
						} else {
							L.push_back(pad + "ElseIf " + c);
						}
						Print(L, body2, indent + 1);
					}
					L.push_back(pad + "EndIf");
					break;
				}
			}
		}

		std::string ValueText(const File& f, const Value& v)
		{
			switch (v.type) {
			case 2: return StringText(f.str(v.s));
			case 3: return std::to_string(v.i);
			case 4: return FloatText(v.f);
			case 5: return v.b ? "True" : "False";
			case 1: return f.str(v.s);
			default: return "None";
			}
		}
	}

	namespace
	{
		struct SourceInfo
		{
			std::string                                     parent;
			std::map<std::string, std::vector<Param>> defaults;  // lowercase function -> parameters
		};

		std::vector<std::string> SplitParams(const std::string& s)
		{
			std::vector<std::string> out;
			std::string              cur;
			bool                     quote = false;
			for (char ch : s) {
				if (ch == '"') {
					quote = !quote;
				}
				if (ch == ',' && !quote) {
					out.push_back(cur);
					cur.clear();
				} else {
					cur.push_back(ch);
				}
			}
			if (!cur.empty()) {
				out.push_back(cur);
			}
			return out;
		}

		std::string Trim(const std::string& s)
		{
			const auto a = s.find_first_not_of(" \t\r\n");
			return a == std::string::npos ? std::string() : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
		}

		SourceInfo ParseSource(const std::string& text)
		{
			SourceInfo info;
			std::string joined;
			// join "\" line continuations, drop ; comments outside strings
			std::vector<std::string> lines;
			std::string              cur;
			for (std::size_t i = 0; i <= text.size(); ++i) {
				const char ch = i < text.size() ? text[i] : '\n';
				if (ch == '\n') {
					std::string l = cur;
					cur.clear();
					bool q = false;
					for (std::size_t k = 0; k < l.size(); ++k) {
						if (l[k] == '"') {
							q = !q;
						}
						if (l[k] == ';' && !q) {
							l = l.substr(0, k);
							break;
						}
					}
					l = Trim(l);
					if (!joined.empty()) {
						l = joined + " " + l;
						joined.clear();
					}
					if (!l.empty() && l.back() == '\\') {
						joined = l.substr(0, l.size() - 1);
						continue;
					}
					lines.push_back(l);
				} else {
					cur.push_back(ch);
				}
			}
			for (const auto& l : lines) {
				const auto low = Lower(l);
				if (low.rfind("scriptname ", 0) == 0) {
					const auto p = low.find(" extends ");
					if (p != std::string::npos) {
						std::string rest = Trim(l.substr(p + 9));
						info.parent = Lower(rest.substr(0, rest.find_first_of(" \t")));
					}
					continue;
				}
				// [Type] Function|Event Name(params)
				std::size_t kw = std::string::npos;
				for (const char* k : { "function ", "event " }) {
					const auto at = low.find(k);
					if (at != std::string::npos && (at == 0 || low[at - 1] == ' ')) {
						kw = at + std::strlen(k);
						break;
					}
				}
				if (kw == std::string::npos) {
					continue;
				}
				const auto open = l.find('(', kw);
				const auto close = l.rfind(')');
				if (open == std::string::npos || close == std::string::npos || close < open) {
					continue;
				}
				const auto              fname = Lower(Trim(l.substr(kw, open - kw)));
				std::vector<Param> defs;
				for (const auto& prm : SplitParams(l.substr(open + 1, close - open - 1))) {
					const auto eq = prm.find('=');
					auto       decl = Trim(prm.substr(0, eq));
					const auto sp = decl.find_last_of(" \t");
					defs.push_back({ sp == std::string::npos ? decl : decl.substr(sp + 1), eq == std::string::npos ? std::string() : Trim(prm.substr(eq + 1)) });
				}
				info.defaults.emplace(fname, std::move(defs));
			}
			return info;
		}
	}

	std::vector<std::string> Decompile(const File& f, std::vector<std::string>* a_warnings, const SourceLookup& a_sources)
	{
		// parameter defaults of overridden functions (a .pex does not store them, the compiler checks them)
		std::map<std::string, SourceInfo> parsed;
		auto                              info = [&](const std::string& script) -> const SourceInfo* {
            if (!a_sources) {
                return nullptr;
            }
            const auto key = Lower(script);
            auto       it = parsed.find(key);
            if (it == parsed.end()) {
                const auto text = a_sources(script);
                it = parsed.emplace(key, text.empty() ? SourceInfo{} : ParseSource(text)).first;
            }
            return &it->second;
		};
		auto defaultsFor = [&](const std::string& parent, const std::string& fname) -> const std::vector<Param>* {
			std::string cur = parent;
			for (int depth = 0; depth < 32 && !cur.empty(); ++depth) {
				const auto* si = info(cur);
				if (!si) {
					return nullptr;
				}
				if (auto it = si->defaults.find(Lower(fname)); it != si->defaults.end()) {
					return &it->second;
				}
				cur = si->parent;
			}
			return nullptr;
		};

		const Signature signature = [&](const std::string& type, const std::string& fname) { return defaultsFor(type, fname); };
		std::vector<std::string> L;
		auto                     flags = [&](std::uint32_t uf) {
            std::string s;
            for (const auto& [n, bit] : f.userFlags) {
                if (uf & (1u << bit)) {
                    s += " " + Cap(f.str(n));
                }
            }
            return s;
		};
		auto doc = [&](std::uint16_t d, const std::string& pad) {
			if (d && !f.str(d).empty()) {
				L.push_back(pad + "{" + f.str(d) + "}");
			}
		};
		L.push_back(";/ Decompiled by ReSaver++ from " + f.source + " /;");
		for (const auto& o : f.objects) {
			L.push_back("ScriptName " + f.str(o.name) + (o.parent && !f.str(o.parent).empty() ? " extends " + f.str(o.parent) : std::string()) +
						flags(o.userFlags));
			doc(o.docString, "");
			L.push_back("");

			// auto property backing variables
			std::map<std::string, std::string> autoVars;  // lower(::x_var) -> property name
			std::set<std::string>              backing;
			for (const auto& p : o.properties) {
				if (p.flags & 4) {
					autoVars[Lower(f.str(p.autoVar))] = f.str(p.name);
					backing.insert(Lower(f.str(p.autoVar)));
				}
			}
			std::map<std::string, std::string> objectTypes;
			for (const auto& v : o.variables) {
				objectTypes[Lower(f.str(v.name))] = Lower(f.str(v.type));
			}
			for (const auto& p : o.properties) {
				objectTypes[Lower(f.str(p.name))] = Lower(f.str(p.type));
			}

			// properties
			for (const auto& p : o.properties) {
				std::string s = f.str(p.type) + " Property " + f.str(p.name);
				if (p.flags & 4) {
					for (const auto& v : o.variables) {
						if (v.name == p.autoVar && v.init.type) {
							s += " = " + ValueText(f, v.init);
						}
					}
					s += " Auto" + flags(p.userFlags);
					L.push_back(s);
					doc(p.docString, "    ");
					continue;
				}
				L.push_back(s + flags(p.userFlags));
				doc(p.docString, "    ");
				auto accessor = [&](const char* what, const Function& fn) {
					FnDecompiler d(f, o, fn, autoVars, objectTypes, signature);
					auto         body = d.run();
					if (a_warnings) {
						for (const auto& w : d.warnings) {
							a_warnings->push_back(f.str(o.name) + "." + f.str(p.name) + "." + what + ": " + w);
						}
					}
					std::string params;
					for (const auto& [n, t] : fn.params) {
						params += (params.empty() ? "" : ", ") + f.str(t) + " " + f.str(n);
					}
					const auto rt = f.str(fn.returnType);
					L.push_back("    " + (Lower(rt) != "none" ? rt + " " : std::string()) + "Function " + what + "(" + params + ")");
					for (const auto& [n, t] : fn.locals) {
						if (!StartsI(f.str(n), "::")) {
							L.push_back("        " + f.str(t) + " " + f.str(n));
						}
					}
					Print(L, body, 2);
					L.push_back("    EndFunction");
				};
				if (p.flags & 1) {
					accessor("Get", p.get);
				}
				if (p.flags & 2) {
					accessor("Set", p.set);
				}
				L.push_back("EndProperty");
			}
			if (!o.properties.empty()) {
				L.push_back("");
			}

			// script variables (not the backing variables of Auto properties)
			bool anyVar = false;
			for (const auto& v : o.variables) {
				if (backing.contains(Lower(f.str(v.name)))) {
					continue;
				}
				L.push_back(f.str(v.type) + " " + f.str(v.name) + (v.init.type ? " = " + ValueText(f, v.init) : std::string()) + flags(v.userFlags));
				anyVar = true;
			}
			if (anyVar) {
				L.push_back("");
			}

			auto function = [&](const NamedFunction& nf, int indent, const std::string& stateName) {
				const auto& fn = nf.fn;
				const auto  fname = f.str(nf.name);
				const std::string pad(static_cast<std::size_t>(indent) * 4, ' ');
				std::string params;
				// the script's own source when there is one, else the parent's (overrides must match it)
				const auto* defs = defaultsFor(f.str(o.name), fname);
				if (!defs || defs->size() != fn.params.size()) {
					defs = defaultsFor(f.str(o.parent), fname);
				}
				for (std::size_t pi = 0; pi < fn.params.size(); ++pi) {
					const auto& [n, t] = fn.params[pi];
					params += (params.empty() ? "" : ", ") + f.str(t) + " " + f.str(n);
					if (defs && defs->size() == fn.params.size() && !(*defs)[pi].def.empty()) {
						params += " = " + (*defs)[pi].def;
					}
				}
				const auto rt = f.str(fn.returnType);
				const bool isEvent = Lower(rt) == "none" && !(fn.flags & 1) && fname.size() > 2 && Lower(fname).rfind("on", 0) == 0 &&
									 std::isupper(static_cast<unsigned char>(fname[2]));
				std::string head = pad + (isEvent ? "Event " : (Lower(rt) != "none" ? rt + " Function " : std::string("Function "))) + fname + "(" + params + ")";
				if (fn.flags & 1) {
					head += " Global";
				}
				if (fn.flags & 2) {
					head += " Native";
				}
				head += flags(fn.userFlags);
				L.push_back(head);
				doc(fn.docString, pad);
				if (fn.flags & 2) {
					L.push_back("");
					return;
				}
				for (const auto& [n, t] : fn.locals) {
					if (IsSourceLocal(f.str(n))) {
						L.push_back(pad + "    " + f.str(t) + " " + Unmangle(f.str(n)));
					}
				}
				FnDecompiler d(f, o, fn, autoVars, objectTypes, signature);
				auto         body = d.run();
				if (a_warnings) {
					for (const auto& w : d.warnings) {
						a_warnings->push_back(f.str(o.name) + "." + (stateName.empty() ? "" : stateName + ".") + fname + ": " + w);
					}
				}
				Print(L, body, indent + 1);
				L.push_back(pad + (isEvent ? "EndEvent" : "EndFunction"));
				L.push_back("");
			};

			for (const auto& s : o.states) {
				const auto sn = f.str(s.name);
				if (sn.empty()) {
					for (const auto& nf : s.functions) {
						const auto n = Lower(f.str(nf.name));
						if (n == "getstate" || n == "gotostate") {
							continue;  // generated by the compiler
						}
						function(nf, 0, "");
					}
				}
			}
			for (const auto& s : o.states) {
				const auto sn = f.str(s.name);
				if (sn.empty()) {
					continue;
				}
				L.push_back(std::string(s.name == o.autoState ? "Auto " : "") + "State " + sn);
				for (const auto& nf : s.functions) {
					function(nf, 1, sn);
				}
				L.push_back("EndState");
				L.push_back("");
			}
		}
		return L;
	}
}

namespace ess::pex
{
std::string FloatLiteral(float f)
	{
		char buf[64];
		for (int prec = 6; prec <= 9; ++prec) {
			std::snprintf(buf, sizeof(buf), "%.*g", prec, f);
			if (std::strtof(buf, nullptr) == f) {
				break;
			}
		}
		std::string s = buf;
		if (s.find_first_of(".eEn") == std::string::npos) {
			s += ".0";
		}
		if (s.find('e') != std::string::npos || s.find('E') != std::string::npos) {
			// Papyrus has no exponent syntax: write it out in plain decimals, with as many digits after
			// the point as it takes to read back as the very same float (a tiny value like 1e-10 needs
			// ten; a fixed precision would round it to 0.0)
			char big[400];
			for (int prec = 1; prec <= 160; ++prec) {
				std::snprintf(big, sizeof(big), "%.*f", prec, f);
				if (std::strtof(big, nullptr) == f) {
					break;
				}
			}
			s = big;
			while (s.size() > 2 && s.back() == '0' && s[s.size() - 2] != '.') {
				s.pop_back();
			}
		}
		return s;
	}
}
