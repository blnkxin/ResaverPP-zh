// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Compiled Papyrus scripts (.pex, Skyrim format): reading and disassembly, like ReSaver's
// "drop a .pex on the window".
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ess::pex
{
	struct Value
	{
		std::uint8_t  type = 0;  // 0 none, 1 identifier, 2 string, 3 int, 4 float, 5 bool
		std::uint32_t s = 0;     // string index (identifier / string)
		std::int32_t  i = 0;
		float         f = 0;
		bool          b = false;
	};

	struct Instruction
	{
		std::uint8_t       op = 0;
		std::vector<Value> args;
	};

	struct Function
	{
		std::uint16_t                                        returnType = 0, docString = 0;
		std::uint32_t                                        userFlags = 0;
		std::uint8_t                                         flags = 0;  // 1 global, 2 native
		std::vector<std::pair<std::uint16_t, std::uint16_t>> params, locals;  // name, type
		std::vector<Instruction>                             code;
	};

	struct NamedFunction
	{
		std::uint16_t name = 0;
		Function      fn;
	};

	struct Variable
	{
		std::uint16_t name = 0, type = 0;
		std::uint32_t userFlags = 0;
		Value         init;
	};

	struct Property
	{
		std::uint16_t name = 0, type = 0, docString = 0, autoVar = 0;
		std::uint32_t userFlags = 0;
		std::uint8_t  flags = 0;  // 1 read, 2 write, 4 auto
		Function      get, set;
	};

	struct State
	{
		std::uint16_t              name = 0;
		std::vector<NamedFunction> functions;
	};

	struct Object
	{
		std::uint16_t         name = 0, parent = 0, docString = 0, autoState = 0;
		std::uint32_t         userFlags = 0;
		std::vector<Variable> variables;
		std::vector<Property> properties;
		std::vector<State>    states;
	};

	struct DebugFunction
	{
		std::uint16_t              object = 0, state = 0, function = 0;
		std::uint8_t               type = 0;
		std::vector<std::uint16_t> lines;
	};

	struct File
	{
		std::uint8_t  major = 0, minor = 0;
		std::uint16_t gameID = 0;
		std::uint64_t compileTime = 0, modTime = 0;
		std::string   source, user, machine;
		std::vector<std::string>                              strings;
		bool                                                  hasDebug = false;
		std::vector<DebugFunction>                            debug;
		std::vector<std::pair<std::uint16_t, std::uint8_t>>   userFlags;  // name, bit
		std::vector<Object>                                   objects;

		const std::string& str(std::uint32_t i) const;
	};

	File                     Read(const std::vector<std::uint8_t>& a_bytes);  // throws on errors
	File                     ReadFile(const std::filesystem::path& a_path);
	std::vector<std::string> Disassemble(const File& a_file);
	const char*              OpcodeName(std::uint8_t a_op);
	// .pex -> .psc source. Warnings list anything that could not be rebuilt cleanly.
	// a_sources (optional) returns the .psc text of a script by name; it is used to give functions
	// that override a parent's the same parameter defaults (a .pex does not store defaults).
	using SourceLookup = std::function<std::string(const std::string& a_script)>;
	// A float as Papyrus source text: no exponent (Papyrus has none), and it reads back as the same float.
	std::string FloatLiteral(float a_value);

	std::vector<std::string> Decompile(const File& a_file, std::vector<std::string>* a_warnings = nullptr, const SourceLookup& a_sources = {});
}
