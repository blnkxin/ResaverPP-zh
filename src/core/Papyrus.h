// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Papyrus (global data type 1001) section of a Skyrim SE/AE save.
#pragma once

#include "ByteIO.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ess
{
	using EID = std::uint64_t;   // object/array ids are 64-bit in SE/AE saves
	using ThreadID = std::uint32_t;  // active script (thread) ids stay 32-bit
	using TStr = std::uint32_t;  // index into the Papyrus string table (32-bit in SE)

	// 3-byte packed form reference used throughout the save.
	struct RefID
	{
		enum class Type : std::uint8_t
		{
			kFormIdx = 0,
			kDefault = 1,
			kCreated = 2,
			kInvalid = 3
		};
		std::uint32_t data = 0;

		static RefID read(Reader& r)
		{
			RefID id;
			const std::uint32_t b0 = r.u8(), b1 = r.u8(), b2 = r.u8();
			id.data = (b0 << 16) | (b1 << 8) | b2;
			return id;
		}
		void write(Writer& w) const
		{
			w.u8(static_cast<std::uint8_t>(data >> 16));
			w.u8(static_cast<std::uint8_t>(data >> 8));
			w.u8(static_cast<std::uint8_t>(data));
		}
		Type          type() const { return static_cast<Type>((data >> 22) & 3); }
		std::uint32_t value() const { return data & 0x3FFFFF; }
		bool          isZero() const { return data == 0; }
		bool          operator==(const RefID& o) const { return data == o.data; }
	};

	enum class VType : std::uint8_t
	{
		kNull = 0,
		kRef = 1,
		kString = 2,
		kInt = 3,
		kFloat = 4,
		kBool = 5,
		kVariant = 6,
		kStruct = 7,
		kRefArray = 11,
		kStringArray = 12,
		kIntArray = 13,
		kFloatArray = 14,
		kBoolArray = 15,
		kVariantArray = 16,
		kStructArray = 17,
	};

	inline bool IsRefType(VType t) { return t == VType::kRef || t == VType::kRefArray || t == VType::kStruct || t == VType::kStructArray; }
	inline bool IsArrayType(VType t) { return static_cast<std::uint8_t>(t) >= 11; }
	const char* VTypeName(VType t);
	bool        EqualsNoCase(const std::string& a, const std::string& b);

	// A Papyrus value: script variable, array element, stack-frame variable, ...
	struct Variable
	{
		VType                     type = VType::kNull;
		std::uint64_t             value = 0;    // null payload / int / bool / float bits / string index / EID
		TStr                      refType = 0;  // script name for ref and ref-array types
		std::unique_ptr<Variable> inner;        // variant payload

		Variable() = default;
		Variable(const Variable& o) { *this = o; }
		Variable& operator=(const Variable& o)
		{
			type = o.type;
			value = o.value;
			refType = o.refType;
			inner = o.inner ? std::make_unique<Variable>(*o.inner) : nullptr;
			return *this;
		}
		Variable(Variable&&) = default;
		Variable& operator=(Variable&&) = default;

		static Variable read(Reader& r);
		void            write(Writer& w) const;
		bool            isRef() const { return type == VType::kRef; }
		float           asFloat() const
		{
			float f;
			const auto lo = static_cast<std::uint32_t>(value);
			std::memcpy(&f, &lo, 4);
			return f;
		}
	};

	struct MemberDesc
	{
		TStr name = 0;
		TStr type = 0;
	};

	struct Script
	{
		TStr                    name = 0;
		TStr                    parent = 0;
		std::vector<MemberDesc> members;
	};

	struct InstanceData  // shared by script instances and references
	{
		std::uint8_t          flag = 0;
		TStr                  state = 0;
		std::uint32_t         unknown1 = 0;
		std::uint32_t         unknown2 = 0;  // present when flag & 4
		std::vector<Variable> vars;
	};

	struct ScriptInstance
	{
		EID                         id = 0;
		TStr                        script = 0;
		std::uint16_t               unknown2bits = 0;
		std::uint16_t               unknown = 0;
		RefID                       refID;
		std::uint8_t                unknownByte = 0;
		std::optional<InstanceData> data;
	};

	struct Reference
	{
		EID                         id = 0;
		TStr                        script = 0;
		std::optional<InstanceData> data;
	};

	struct ArrayInfo
	{
		EID                   id = 0;
		VType                 type = VType::kNull;
		TStr                  refType = 0;
		std::uint32_t         length = 0;
		bool                  hasData = false;
		std::vector<Variable> vars;
	};

	struct Param
	{
		std::uint8_t  type = 0;  // 0 null,1 identifier,2 string,3 int,4 float,5 bool,8 unknown8
		std::uint32_t value = 0;
	};

	struct Opcode
	{
		std::uint8_t       op = 0;
		std::vector<Param> params;
	};

	struct StackFrame
	{
		std::uint8_t              flag = 0;
		std::uint8_t              fnType = 0;
		TStr                      scriptName = 0;
		TStr                      baseName = 0;
		TStr                      event = 0;
		std::optional<TStr>       status;
		std::uint8_t              opMajor = 0, opMinor = 0;
		TStr                      returnType = 0;
		TStr                      docString = 0;
		std::uint32_t             userFlags = 0;
		std::uint8_t              fnFlags = 0;
		std::vector<MemberDesc>   params;
		std::vector<MemberDesc>   locals;
		std::vector<Opcode>       code;
		std::uint32_t             ptr = 0;
		Variable                  owner;
		std::vector<Variable>     vars;
	};

	struct FragmentTask
	{
		std::uint8_t              kind = 0;   // the "unknown3" selector: 1, 2 or 3
		std::string               typeCode;   // QuestStage, SceneResults, ... (kinds 1 and 3)
		std::vector<std::uint8_t> payload;    // fragment-type specific data, kept verbatim
		std::optional<Variable>   variable;   // kinds 2 and 3
	};

	struct ActiveScriptData
	{
		std::int8_t                 major = 0, minor = 0;
		Variable                    unknownVar;
		std::uint8_t                flag = 0;
		std::uint8_t                unknownByte = 0;
		std::uint32_t               unknown2 = 0;  // present when flag & 1
		std::uint8_t                unknown3 = 0;
		std::optional<FragmentTask> fragment;
		std::vector<StackFrame>     frames;
		std::optional<std::uint8_t> unknown5;
	};

	struct ActiveScript
	{
		ThreadID                        id = 0;
		std::uint8_t                    type = 0;
		std::optional<ActiveScriptData> data;
	};

	struct FunctionMessageData
	{
		std::uint8_t          fnFlags = 0;
		TStr                  scriptName = 0;
		TStr                  event = 0;
		Variable              unknownVar;
		std::vector<Variable> vars;
	};

	struct FunctionMessage
	{
		std::uint8_t                       flag = 0;
		std::optional<ThreadID>            id;  // present when flag <= 2
		std::uint8_t                       flags = 0;
		std::optional<FunctionMessageData> data;
	};

	struct SuspendedStack
	{
		ThreadID                           id = 0;
		std::uint8_t                       flag = 0;
		std::optional<FunctionMessageData> data;
	};

	struct QueuedUnbind
	{
		EID           id = 0;
		std::uint32_t unknown = 0;
	};

	class Papyrus
	{
	public:
		void read(Reader& r);
		void write(Writer& w) const;
		// Structural problems a read can pass but a write would not reproduce faithfully: string indices
		// out of range, duplicate ids, saved data missing or given twice. Empty for a sound section.
		std::vector<std::string> check() const;

		std::uint16_t                header = 0;
		std::vector<std::string>     strings;
		std::vector<Script>          scripts;
		std::vector<ScriptInstance>  instances;
		std::vector<Reference>       references;
		std::vector<ArrayInfo>       arrays;
		ThreadID                     runtime = 0;
		std::vector<ActiveScript>    activeScripts;
		std::vector<FunctionMessage> functionMessages;
		std::vector<SuspendedStack>  suspended1;
		std::vector<SuspendedStack>  suspended2;
		std::uint32_t                unk1 = 0;
		std::optional<std::uint32_t> unk2;
		std::vector<EID>             unknownIDs;
		std::vector<QueuedUnbind>    unbinds;
		std::optional<std::uint16_t> saveFileVersion;
		std::vector<std::uint8_t>    tail;  // "other data" block, kept verbatim

		// The game writes the per-element data blocks in its own order; keep it so an
		// unmodified save writes back byte-for-byte identical.
		std::vector<EID>      instanceDataOrder, referenceDataOrder, arrayDataOrder;
		std::vector<ThreadID> activeDataOrder;

		const std::string& str(TStr i) const
		{
			static const std::string bad = "<字符串索引无效>";
			return i < strings.size() ? strings[i] : bad;
		}
		TStr findString(const std::string& s) const;  // returns UINT32_MAX when missing
		TStr internString(const std::string& s);      // exact match, or appended to the table

		// lookup helpers (rebuilt by reindex())
		void                  reindex();
		const Script*         findScript(TStr name) const;
		ScriptInstance*       findInstance(EID id);
		Reference*            findReference(EID id);
		ArrayInfo*            findArray(EID id);
		ActiveScript*         findActive(ThreadID id);
		const ScriptInstance* findInstance(EID id) const { return const_cast<Papyrus*>(this)->findInstance(id); }
		const ArrayInfo*      findArray(EID id) const { return const_cast<Papyrus*>(this)->findArray(id); }
		const ActiveScript*   findActive(ThreadID id) const { return const_cast<Papyrus*>(this)->findActive(id); }

	private:
		std::unordered_map<TStr, std::size_t> _scriptIdx;
		std::unordered_map<EID, std::size_t>      _instIdx, _refIdx, _arrIdx;
		std::unordered_map<ThreadID, std::size_t> _activeIdx;
	};
}
