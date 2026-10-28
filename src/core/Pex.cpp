// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Pex.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <stdexcept>

namespace ess::pex
{
	namespace
	{
		// .pex files from Skyrim are big-endian
		class BE
		{
		public:
			BE(const std::vector<std::uint8_t>& d) :
				_d(d) {}
			void need(std::size_t n) const
			{
				if (_p + n > _d.size()) {
					throw std::runtime_error("脚本文件意外结束（第 " + std::to_string(_p) + " 字节）");
				}
			}
			std::uint8_t u8()
			{
				need(1);
				return _d[_p++];
			}
			std::uint16_t u16()
			{
				need(2);
				const std::uint16_t v = std::uint16_t(_d[_p] << 8 | _d[_p + 1]);
				_p += 2;
				return v;
			}
			std::uint32_t u32()
			{
				need(4);
				const std::uint32_t v = std::uint32_t(_d[_p]) << 24 | std::uint32_t(_d[_p + 1]) << 16 | std::uint32_t(_d[_p + 2]) << 8 | _d[_p + 3];
				_p += 4;
				return v;
			}
			std::uint64_t u64()
			{
				const std::uint64_t hi = u32();
				return hi << 32 | u32();
			}
			float f32()
			{
				const auto u = u32();
				float      f;
				std::memcpy(&f, &u, 4);
				return f;
			}
			std::string wstr()
			{
				const auto n = u16();
				need(n);
				std::string s(reinterpret_cast<const char*>(_d.data() + _p), n);
				_p += n;
				return s;
			}

		private:
			const std::vector<std::uint8_t>& _d;
			std::size_t                      _p = 0;
		};

		// argument counts for Skyrim's 36 opcodes; -1 marks "plus a counted list"
		struct OpInfo
		{
			const char* name;
			int         fixed;
			bool        varargs;
		};
		const OpInfo kOps[] = { { "nop", 0, false }, { "iadd", 3, false }, { "fadd", 3, false }, { "isub", 3, false }, { "fsub", 3, false },
			{ "imul", 3, false }, { "fmul", 3, false }, { "idiv", 3, false }, { "fdiv", 3, false }, { "imod", 3, false }, { "not", 2, false },
			{ "ineg", 2, false }, { "fneg", 2, false }, { "assign", 2, false }, { "cast", 2, false }, { "cmp_eq", 3, false },
			{ "cmp_lt", 3, false }, { "cmp_le", 3, false }, { "cmp_gt", 3, false }, { "cmp_ge", 3, false }, { "jmp", 1, false },
			{ "jmpt", 2, false }, { "jmpf", 2, false }, { "callmethod", 3, true }, { "callparent", 2, true }, { "callstatic", 3, true },
			{ "return", 1, false }, { "strcat", 3, false }, { "propget", 3, false }, { "propset", 3, false }, { "array_create", 2, false },
			{ "array_length", 2, false }, { "array_getelement", 3, false }, { "array_setelement", 3, false }, { "array_findelement", 4, false },
			{ "array_rfindelement", 4, false } };
		constexpr std::size_t kOpCount = sizeof(kOps) / sizeof(kOps[0]);

		Value ReadValue(BE& r)
		{
			Value v;
			v.type = r.u8();
			switch (v.type) {
			case 0: break;
			case 1:
			case 2: v.s = r.u16(); break;
			case 3: v.i = static_cast<std::int32_t>(r.u32()); break;
			case 4: v.f = r.f32(); break;
			case 5: v.b = r.u8() != 0; break;
			default: throw std::runtime_error("无效的值类型 " + std::to_string(v.type));
			}
			return v;
		}

		Function ReadFunction(BE& r)
		{
			Function f;
			f.returnType = r.u16();
			f.docString = r.u16();
			f.userFlags = r.u32();
			f.flags = r.u8();
			for (int n = r.u16(); n > 0; --n) {
				const auto a = r.u16();
				f.params.push_back({ a, r.u16() });
			}
			for (int n = r.u16(); n > 0; --n) {
				const auto a = r.u16();
				f.locals.push_back({ a, r.u16() });
			}
			for (int n = r.u16(); n > 0; --n) {
				Instruction in;
				in.op = r.u8();
				if (in.op >= kOpCount) {
					throw std::runtime_error("未知的指令码 " + std::to_string(in.op));
				}
				for (int k = 0; k < kOps[in.op].fixed; ++k) {
					in.args.push_back(ReadValue(r));
				}
				if (kOps[in.op].varargs) {
					const auto count = ReadValue(r);
					in.args.push_back(count);
					for (int k = 0; k < count.i; ++k) {
						in.args.push_back(ReadValue(r));
					}
				}
				f.code.push_back(std::move(in));
			}
			return f;
		}

		std::string Fmt(const char* f, auto... a)
		{
			char buf[512];
			std::snprintf(buf, sizeof(buf), f, a...);
			return buf;
		}
	}

	const std::string& File::str(std::uint32_t i) const
	{
		static const std::string bad = "<字符串无效>";
		return i < strings.size() ? strings[i] : bad;
	}

	const char* OpcodeName(std::uint8_t a_op) { return a_op < kOpCount ? kOps[a_op].name : "?"; }

	File Read(const std::vector<std::uint8_t>& a_bytes)
	{
		BE         r(a_bytes);
		const auto magic = r.u32();
		if (magic == 0xDEC057FA) {
			throw std::runtime_error("这是 Fallout 4 的脚本（小端 .pex）；只支持天际的脚本");
		}
		if (magic != 0xFA57C0DE) {
			throw std::runtime_error("不是编译过的 Papyrus 脚本（.pex）");
		}
		File f;
		f.major = r.u8();
		f.minor = r.u8();
		f.gameID = r.u16();
		f.compileTime = r.u64();
		f.source = r.wstr();
		f.user = r.wstr();
		f.machine = r.wstr();
		for (int n = r.u16(); n > 0; --n) {
			f.strings.push_back(r.wstr());
		}
		f.hasDebug = r.u8() != 0;
		if (f.hasDebug) {
			f.modTime = r.u64();
			for (int n = r.u16(); n > 0; --n) {
				DebugFunction d;
				d.object = r.u16();
				d.state = r.u16();
				d.function = r.u16();
				d.type = r.u8();
				for (int k = r.u16(); k > 0; --k) {
					d.lines.push_back(r.u16());
				}
				f.debug.push_back(std::move(d));
			}
		}
		for (int n = r.u16(); n > 0; --n) {
			const auto name = r.u16();
			f.userFlags.push_back({ name, r.u8() });
		}
		for (int n = r.u16(); n > 0; --n) {
			Object o;
			o.name = r.u16();
			r.u32();  // object size
			o.parent = r.u16();
			o.docString = r.u16();
			o.userFlags = r.u32();
			o.autoState = r.u16();
			for (int k = r.u16(); k > 0; --k) {
				Variable v;
				v.name = r.u16();
				v.type = r.u16();
				v.userFlags = r.u32();
				v.init = ReadValue(r);
				o.variables.push_back(v);
			}
			for (int k = r.u16(); k > 0; --k) {
				Property p;
				p.name = r.u16();
				p.type = r.u16();
				p.docString = r.u16();
				p.userFlags = r.u32();
				p.flags = r.u8();
				if (p.flags & 4) {
					p.autoVar = r.u16();
				} else {
					if (p.flags & 1) {
						p.get = ReadFunction(r);
					}
					if (p.flags & 2) {
						p.set = ReadFunction(r);
					}
				}
				o.properties.push_back(std::move(p));
			}
			for (int k = r.u16(); k > 0; --k) {
				State s;
				s.name = r.u16();
				for (int m = r.u16(); m > 0; --m) {
					NamedFunction nf;
					nf.name = r.u16();
					nf.fn = ReadFunction(r);
					s.functions.push_back(std::move(nf));
				}
				o.states.push_back(std::move(s));
			}
			f.objects.push_back(std::move(o));
		}
		return f;
	}

	File ReadFile(const std::filesystem::path& a_path)
	{
		std::ifstream in(a_path, std::ios::binary);
		if (!in) {
			throw std::runtime_error("无法打开脚本文件");
		}
		std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		return Read(bytes);
	}

	std::vector<std::string> Disassemble(const File& f)
	{
		std::vector<std::string> L;
		auto                     val = [&](const Value& v) -> std::string {
            switch (v.type) {
            case 0: return "None";
            case 1: return f.str(v.s);
            case 2: return "\"" + f.str(v.s) + "\"";
            case 3: return std::to_string(v.i);
            case 4: return Fmt("%g", v.f);
            case 5: return v.b ? "True" : "False";
            default: return "?";
            }
		};
		auto flagsText = [&](std::uint32_t uf) {
			std::string s;
			for (const auto& [name, bit] : f.userFlags) {
				if (uf & (1u << bit)) {
					s += " " + f.str(name);
				}
			}
			return s;
		};
		auto time = [](std::uint64_t t) {
			char       buf[64] = "?";
			const auto tt = static_cast<std::time_t>(t);
			if (const auto* tm = std::gmtime(&tt)) {
				std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", tm);
			}
			return std::string(buf);
		};
		auto lineOf = [&](std::uint16_t obj, std::uint16_t state, std::uint16_t fn, std::size_t ip) -> int {
			for (const auto& d : f.debug) {
				if (d.object == obj && d.state == state && d.function == fn && d.type == 0 && ip < d.lines.size()) {
					return d.lines[ip];
				}
			}
			return -1;
		};
		auto function = [&](const std::string& title, const Function& fn, std::uint16_t obj, std::uint16_t state, std::uint16_t fnName) {
			std::string params;
			for (const auto& [n, t] : fn.params) {
				params += (params.empty() ? "" : ", ") + f.str(t) + " " + f.str(n);
			}
			std::string head = "  " + (fn.returnType && f.str(fn.returnType) != "None" ? f.str(fn.returnType) + " " : std::string()) + "Function " +
							   title + "(" + params + ")";
			if (fn.flags & 1) {
				head += " global";
			}
			if (fn.flags & 2) {
				head += " native";
			}
			head += flagsText(fn.userFlags);
			L.push_back(head);
			if (fn.docString && !f.str(fn.docString).empty()) {
				L.push_back("    ; " + f.str(fn.docString));
			}
			for (const auto& [n, t] : fn.locals) {
				L.push_back("    .local " + f.str(t) + " " + f.str(n));
			}
			for (std::size_t ip = 0; ip < fn.code.size(); ++ip) {
				const auto& in = fn.code[ip];
				std::string s = Fmt("    %03zu  %-18s", ip, OpcodeName(in.op));
				const bool  isJump = in.op >= 20 && in.op <= 22;
				for (std::size_t a = 0; a < in.args.size(); ++a) {
					// jumps: the last argument is a relative offset
					if (isJump && a + 1 == in.args.size() && in.args[a].type == 3) {
						s += Fmt(" -> %03lld", static_cast<long long>(ip) + in.args[a].i);
					} else {
						s += " " + val(in.args[a]);
					}
				}
				if (const int line = lineOf(obj, state, fnName, ip); line >= 0) {
					do {
						s.push_back(' ');
					} while (s.size() < 72);
					s += Fmt("; line %d", line);
				}
				L.push_back(s);
			}
			L.push_back("  EndFunction");
			L.push_back("");
		};

		L.push_back(Fmt("; PEX %u.%u, game %u%s", f.major, f.minor, f.gameID, f.gameID == 1 ? " (Skyrim)" : ""));
		L.push_back("; source:   " + f.source);
		L.push_back("; compiled: " + time(f.compileTime) + " by " + f.user + " on " + f.machine);
		if (f.hasDebug) {
			L.push_back("; debug info: yes (source modified " + time(f.modTime) + ")");
		}
		L.push_back("");
		for (const auto& o : f.objects) {
			L.push_back("ScriptName " + f.str(o.name) + (o.parent && !f.str(o.parent).empty() ? " extends " + f.str(o.parent) : std::string()) +
						flagsText(o.userFlags));
			if (o.docString && !f.str(o.docString).empty()) {
				L.push_back("{" + f.str(o.docString) + "}");
			}
			L.push_back("");
			L.push_back(Fmt("; ---- variables (%zu)", o.variables.size()));
			for (const auto& v : o.variables) {
				L.push_back("  " + f.str(v.type) + " " + f.str(v.name) + (v.init.type ? " = " + val(v.init) : std::string()) + flagsText(v.userFlags));
			}
			L.push_back("");
			L.push_back(Fmt("; ---- properties (%zu)", o.properties.size()));
			for (const auto& p : o.properties) {
				std::string s = "  " + f.str(p.type) + " Property " + f.str(p.name);
				if (p.flags & 4) {
					s += " Auto  ; -> " + f.str(p.autoVar);
				}
				s += flagsText(p.userFlags);
				L.push_back(s);
				if (p.docString && !f.str(p.docString).empty()) {
					L.push_back("    ; " + f.str(p.docString));
				}
				if (!(p.flags & 4)) {
					if (p.flags & 1) {
						function("Get", p.get, o.name, 0, p.name);
					}
					if (p.flags & 2) {
						function("Set", p.set, o.name, 0, p.name);
					}
				}
			}
			L.push_back("");
			for (const auto& s : o.states) {
				const auto& sn = f.str(s.name);
				L.push_back(Fmt("; ---- state \"%s\"%s (%zu functions)", sn.c_str(), s.name == o.autoState && !sn.empty() ? " (auto)" : "",
					s.functions.size()));
				if (!sn.empty()) {
					L.push_back(std::string(s.name == o.autoState ? "Auto " : "") + "State " + sn);
				}
				for (const auto& nf : s.functions) {
					function(f.str(nf.name), nf.fn, o.name, s.name, nf.name);
				}
				if (!sn.empty()) {
					L.push_back("EndState");
					L.push_back("");
				}
			}
		}
		return L;
	}
}
