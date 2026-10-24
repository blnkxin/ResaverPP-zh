// ReSaver++ test for the .pex -> .psc decompiler, using the Papyrus compiler's assembly output:
//   rs_decomp_test pas2psc <in.pas> <out.psc>   read compiler assembly, decompile it
//   rs_decomp_test cmp <a.pas> <b.pas>          compare the code of two assemblies (temps renamed,
//                                               labels as offsets); exit 0 when identical
// Compiling original source -> a.pas, decompiling a.pas -> x.psc, compiling x.psc -> b.pas and
// comparing a.pas with b.pas checks that the decompiled source compiles to the same code.
#include "core/Pex.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ess::pex;

namespace
{
	std::string Lower(std::string s)
	{
		for (auto& c : s) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return s;
	}

	// tokens: words, "strings" (kept with quotes); comments (;...) dropped
	std::vector<std::string> Tokens(const std::string& line)
	{
		std::vector<std::string> out;
		std::size_t              i = 0;
		while (i < line.size()) {
			const char c = line[i];
			if (std::isspace(static_cast<unsigned char>(c))) {
				++i;
				continue;
			}
			if (c == ';') {
				break;
			}
			if (c == '"') {
				std::string s = "\"";
				++i;
				while (i < line.size() && line[i] != '"') {
					if (line[i] == '\\' && i + 1 < line.size()) {
						const char n = line[i + 1];
						s.push_back(n == 'n' ? '\n' : n == 't' ? '\t' : n);
						i += 2;
						continue;
					}
					s.push_back(line[i++]);
				}
				++i;
				out.push_back(s + "\"");
				continue;
			}
			std::string w;
			while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i])) && line[i] != ';') {
				w.push_back(line[i++]);
			}
			out.push_back(w);
		}
		return out;
	}

	const std::map<std::string, std::uint8_t> kOps = { { "iadd", 1 }, { "fadd", 2 }, { "isubtract", 3 }, { "fsubtract", 4 }, { "imultiply", 5 },
		{ "fmultiply", 6 }, { "idivide", 7 }, { "fdivide", 8 }, { "imod", 9 }, { "not", 10 }, { "inegate", 11 }, { "fnegate", 12 }, { "assign", 13 },
		{ "cast", 14 }, { "compareeq", 15 }, { "comparelt", 16 }, { "comparelte", 17 }, { "comparegt", 18 }, { "comparegte", 19 }, { "jump", 20 },
		{ "jumpt", 21 }, { "jumpf", 22 }, { "callmethod", 23 }, { "callparent", 24 }, { "callstatic", 25 }, { "return", 26 }, { "strcat", 27 },
		{ "propget", 28 }, { "propset", 29 }, { "arraycreate", 30 }, { "arraylength", 31 }, { "arraygetelement", 32 }, { "arraysetelement", 33 },
		{ "arrayfindelement", 34 }, { "arrayrfindelement", 35 } };

	struct PasReader
	{
		File                                  f;
		std::map<std::string, std::uint16_t>  idx;
		std::uint16_t s(const std::string& v)
		{
			auto it = idx.find(v);
			if (it != idx.end()) {
				return it->second;
			}
			f.strings.push_back(v);
			return idx[v] = static_cast<std::uint16_t>(f.strings.size() - 1);
		}
		Value value(const std::string& t)
		{
			Value v;
			const auto l = Lower(t);
			if (t.size() >= 2 && t.front() == '"') {
				v.type = 2;
				v.s = s(t.substr(1, t.size() - 2));
			} else if (l == "none") {
				v.type = 0;
			} else if (l == "true" || l == "false") {
				v.type = 5;
				v.b = l == "true";
			} else if (!t.empty() && (std::isdigit(static_cast<unsigned char>(t[0])) || t[0] == '-') && t.find_first_not_of("-0123456789") == std::string::npos) {
				v.type = 3;
				v.i = static_cast<std::int32_t>(std::strtol(t.c_str(), nullptr, 10));
			} else if (!t.empty() && (std::isdigit(static_cast<unsigned char>(t[0])) || t[0] == '-')) {
				v.type = 4;
				v.f = std::strtof(t.c_str(), nullptr);
			} else {
				v.type = 1;
				v.s = s(t);
			}
			return v;
		}
	};

	File ReadPas(const std::string& path)
	{
		std::ifstream in(path);
		if (!in) {
			throw std::runtime_error("cannot open " + path);
		}
		PasReader R;
		R.s("");
		File&     f = R.f;
		f.gameID = 1;
		f.major = 3;
		f.minor = 2;
		std::vector<std::vector<std::string>> lines;
		for (std::string l; std::getline(in, l);) {
			auto t = Tokens(l);
			if (!t.empty()) {
				lines.push_back(std::move(t));
			}
		}
		std::size_t i = 0;
		auto        readFunction = [&](Function& fn) {
            // at ".function name [static|native]"
            std::vector<std::string> head = lines[i];
            for (std::size_t k = 2; k < head.size(); ++k) {
                if (Lower(head[k]) == "static") {
                    fn.flags |= 1;
                }
                if (Lower(head[k]) == "native") {
                    fn.flags |= 2;
                }
            }
            ++i;
            std::vector<std::pair<std::size_t, std::string>> jumps;  // instruction, label
            std::map<std::string, std::size_t>             labels;
            for (; i < lines.size(); ++i) {
                const auto& t = lines[i];
                const auto  k = Lower(t[0]);
                if (k == ".endfunction") {
                    break;
                } else if (k == ".userflags") {
                    fn.userFlags = static_cast<std::uint32_t>(std::stoul(t[1]));
                } else if (k == ".docstring") {
                    fn.docString = R.s(t.size() > 1 ? t[1].substr(1, t[1].size() - 2) : "");
                } else if (k == ".return") {
                    fn.returnType = R.s(t[1]);
                } else if (k == ".param") {
                    fn.params.push_back({ R.s(t[1]), R.s(t[2]) });
                } else if (k == ".local") {
                    fn.locals.push_back({ R.s(t[1]), R.s(t[2]) });
                } else if (k.back() == ':' && k.rfind("label", 0) == 0) {
                    labels[k.substr(0, k.size() - 1)] = fn.code.size();
                } else if (kOps.contains(k)) {
                    Instruction ins;
                    ins.op = kOps.at(k);
                    std::vector<std::string> a(t.begin() + 1, t.end());
                    auto                     fixed = [&](std::size_t n) {
                        for (std::size_t q = 0; q < n && q < a.size(); ++q) {
                            ins.args.push_back(R.value(a[q]));
                        }
                        Value cnt;
                        cnt.type = 3;
                        cnt.i = static_cast<std::int32_t>(a.size() > n ? a.size() - n : 0);
                        ins.args.push_back(cnt);
                        for (std::size_t q = n; q < a.size(); ++q) {
                            ins.args.push_back(R.value(a[q]));
                        }
                    };
                    if (ins.op == 23 || ins.op == 25) {
                        fixed(3);
                    } else if (ins.op == 24) {
                        fixed(2);
                    } else if (ins.op >= 20 && ins.op <= 22) {
                        for (std::size_t q = 0; q + 1 < a.size(); ++q) {
                            ins.args.push_back(R.value(a[q]));
                        }
                        Value off;
                        off.type = 3;
                        ins.args.push_back(off);
                        jumps.push_back({ fn.code.size(), Lower(a.back()) });
                    } else {
                        for (const auto& x : a) {
                            ins.args.push_back(R.value(x));
                        }
                    }
                    fn.code.push_back(std::move(ins));
                }
            }
            for (const auto& [at, label] : jumps) {
                fn.code[at].args.back().i = static_cast<std::int32_t>(static_cast<long long>(labels.at(label)) - static_cast<long long>(at));
            }
		};
		for (; i < lines.size(); ++i) {
			const auto& t = lines[i];
			const auto  k = Lower(t[0]);
			if (k == ".source") {
				f.source = t[1].substr(1, t[1].size() - 2);
			} else if (k == ".flag") {
				f.userFlags.push_back({ R.s(t[1]), static_cast<std::uint8_t>(std::stoi(t[2])) });
			} else if (k == ".object") {
				Object o;
				o.name = R.s(t[1]);
				o.parent = R.s(t.size() > 2 ? t[2] : "");
				o.autoState = R.s("");
				enum
				{
					kNone,
					kVar,
					kProp,
					kState
				} where = kNone;
				for (++i; i < lines.size(); ++i) {
					const auto& u = lines[i];
					const auto  q = Lower(u[0]);
					if (q == ".endobject") {
						break;
					}
					if (q == ".userflags" && where == kNone) {
						o.userFlags = static_cast<std::uint32_t>(std::stoul(u[1]));
					} else if (q == ".docstring" && where == kNone) {
						o.docString = R.s(u.size() > 1 ? u[1].substr(1, u[1].size() - 2) : "");
					} else if (q == ".autostate") {
						o.autoState = R.s(u.size() > 1 ? u[1] : "");
					} else if (q == ".variable") {
						Variable v;
						v.name = R.s(u[1]);
						v.type = R.s(u[2]);
						for (++i; i < lines.size() && Lower(lines[i][0]) != ".endvariable"; ++i) {
							if (Lower(lines[i][0]) == ".userflags") {
								v.userFlags = static_cast<std::uint32_t>(std::stoul(lines[i][1]));
							} else if (Lower(lines[i][0]) == ".initialvalue") {
								v.init = R.value(lines[i][1]);
							}
						}
						o.variables.push_back(v);
					} else if (q == ".property") {
						Property p;
						p.name = R.s(u[1]);
						p.type = R.s(u[2]);
						if (u.size() > 3 && Lower(u[3]) == "auto") {
							p.flags = 4 | 1 | 2;
						}
						for (++i; i < lines.size() && Lower(lines[i][0]) != ".endproperty"; ++i) {
							const auto w = Lower(lines[i][0]);
							if (w == ".userflags") {
								p.userFlags = static_cast<std::uint32_t>(std::stoul(lines[i][1]));
							} else if (w == ".docstring") {
								p.docString = R.s(lines[i].size() > 1 ? lines[i][1].substr(1, lines[i][1].size() - 2) : "");
							} else if (w == ".autovar") {
								p.autoVar = R.s(lines[i][1]);
							} else if (w == ".function") {
								const auto fn = Lower(lines[i][1]);
								if (fn == "get") {
									p.flags |= 1;
									readFunction(p.get);
								} else {
									p.flags |= 2;
									readFunction(p.set);
								}
							}
						}
						o.properties.push_back(std::move(p));
					} else if (q == ".state") {
						State st;
						st.name = R.s(u.size() > 1 ? u[1] : "");
						for (++i; i < lines.size() && Lower(lines[i][0]) != ".endstate"; ++i) {
							if (Lower(lines[i][0]) == ".function") {
								NamedFunction nf;
								nf.name = R.s(lines[i][1]);
								readFunction(nf.fn);
								st.functions.push_back(std::move(nf));
							}
						}
						o.states.push_back(std::move(st));
					}
				}
				f.objects.push_back(std::move(o));
			}
		}
		return f;
	}

	// normalised code per function: "object.state.function" -> lines
	std::map<std::string, std::vector<std::string>> Code(const File& f)
	{
		std::map<std::string, std::vector<std::string>> out;
		auto                                            one = [&](const std::string& key, const Function& fn) {
            std::map<std::string, std::string> temps;
            std::vector<std::string>           L;
            for (const auto& in : fn.code) {
                std::string s = OpcodeName(in.op);
                for (std::size_t a = 0; a < in.args.size(); ++a) {
                    const auto& v = in.args[a];
                    std::string t;
                    switch (v.type) {
                    case 0: t = "none"; break;
                    case 1:
                        {
                            t = Lower(f.str(v.s));
                            if (t.rfind("::mangled_", 0) == 0) {
                                t = t.substr(10);
                            }
                            if (t.rfind("::temp", 0) == 0 && t.size() > 6 && t.find_first_not_of("0123456789", 6) == std::string::npos) {
                                auto it = temps.find(t);
                                if (it == temps.end()) {
                                    it = temps.emplace(t, "T" + std::to_string(temps.size())).first;
                                }
                                t = it->second;
                            }
                            break;
                        }
                    case 2: t = "\"" + f.str(v.s) + "\""; break;
                    case 3: t = std::to_string(v.i); break;
                    case 4:
                        {
                            // exact bits: "%g" would make distinct floats look equal
                            char          b[32];
                            std::uint32_t bits;
                            std::memcpy(&bits, &v.f, 4);
                            std::snprintf(b, sizeof(b), "f%08X", bits);
                            t = b;
                            break;
                        }
                    case 5: t = v.b ? "true" : "false"; break;
                    }
                    s += " " + t;
                }
                L.push_back(s);
            }
            out[Lower(key)] = L;
		};
		for (const auto& o : f.objects) {
			for (const auto& p : o.properties) {
				if (!(p.flags & 4)) {
					if (p.flags & 1) {
						one(f.str(p.name) + ".get", p.get);
					}
					if (p.flags & 2) {
						one(f.str(p.name) + ".set", p.set);
					}
				}
			}
			for (const auto& st : o.states) {
				for (const auto& nf : st.functions) {
					one(f.str(st.name) + "." + f.str(nf.name), nf.fn);
				}
			}
		}
		return out;
	}
}

int main(int argc, char** argv)
{
	if (argc < 4) {
		std::puts("usage: rs_decomp_test pas2psc <in.pas> <out.psc> | cmp <a.pas> <b.pas>");
		return 1;
	}
	try {
		const std::string mode = argv[1];
		if (mode == "pas2psc") {
			const auto               f = ReadPas(argv[2]);
			std::vector<std::string> warnings;
			// optional 4th argument: folder of .psc sources (parameter defaults of parent scripts)
			SourceLookup src;
			if (argc > 4) {
				const std::string dir = argv[4];
				src = [dir](const std::string& script) {
					std::ifstream in(dir + "/" + script + ".psc", std::ios::binary);
					if (!in) {
						// case-insensitive fallback
						for (const auto& e : std::filesystem::directory_iterator(dir)) {
							if (Lower(e.path().stem().string()) == Lower(script)) {
								std::ifstream in2(e.path(), std::ios::binary);
								return std::string((std::istreambuf_iterator<char>(in2)), std::istreambuf_iterator<char>());
							}
						}
						return std::string();
					}
					return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
				};
			}
			const auto               lines = Decompile(f, &warnings, src);
			std::ofstream            out(argv[3], std::ios::binary);
			for (const auto& l : lines) {
				out << l << "\r\n";
			}
			for (const auto& w : warnings) {
				std::fprintf(stderr, "warning: %s\n", w.c_str());
			}
			return warnings.empty() ? 0 : 3;
		}
		if (mode == "cmp") {
			const auto a = Code(ReadPas(argv[2]));
			const auto b = Code(ReadPas(argv[3]));
			int        bad = 0;
			for (const auto& [k, la] : a) {
				auto it = b.find(k);
				if (it == b.end()) {
					std::printf("  missing function %s\n", k.c_str());
					++bad;
					continue;
				}
				if (la != it->second) {
					++bad;
					std::printf("  differs: %s\n", k.c_str());
					for (std::size_t x = 0; x < std::max(la.size(), it->second.size()); ++x) {
						const auto sa = x < la.size() ? la[x] : "";
						const auto sb = x < it->second.size() ? it->second[x] : "";
						if (sa != sb) {
							std::printf("    %3zu  orig: %s\n         new:  %s\n", x, sa.c_str(), sb.c_str());
							break;
						}
					}
				}
			}
			for (const auto& [k, lb] : b) {
				if (!a.contains(k)) {
					std::printf("  extra function %s\n", k.c_str());
					++bad;
				}
			}
			return bad ? 2 : 0;
		}
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 4;
	}
	return 1;
}
