// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Papyrus.h"

#include <algorithm>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <unordered_set>

namespace ess
{
	const char* VTypeName(VType t)
	{
		switch (t) {
		case VType::kNull: return "None";
		case VType::kRef: return "Ref";
		case VType::kString: return "String";
		case VType::kInt: return "Int";
		case VType::kFloat: return "Float";
		case VType::kBool: return "Bool";
		case VType::kVariant: return "Variant";
		case VType::kStruct: return "Struct";
		case VType::kRefArray: return "Ref[]";
		case VType::kStringArray: return "String[]";
		case VType::kIntArray: return "Int[]";
		case VType::kFloatArray: return "Float[]";
		case VType::kBoolArray: return "Bool[]";
		case VType::kVariantArray: return "Variant[]";
		case VType::kStructArray: return "Struct[]";
		default: return "?";
		}
	}

	static VType ReadVType(Reader& r)
	{
		const auto pos = r.pos();
		const auto v = r.u8();
		if (v > 17 || (v >= 8 && v <= 10)) {
			throw FormatError("invalid variable type " + std::to_string(v), pos);
		}
		return static_cast<VType>(v);
	}

	Variable Variable::read(Reader& r)
	{
		Variable v;
		v.type = ReadVType(r);
		switch (v.type) {
		case VType::kNull:
		case VType::kString:
		case VType::kInt:
		case VType::kFloat:
		case VType::kBool:
			v.value = r.u32();
			break;
		case VType::kRef:
		case VType::kStruct:
			v.refType = r.u32();
			v.value = r.u64();
			break;
		case VType::kVariant:
			v.inner = std::make_unique<Variable>(Variable::read(r));
			break;
		default:  // arrays
			if (IsRefType(v.type)) {
				v.refType = r.u32();
			}
			v.value = r.u64();
			break;
		}
		return v;
	}

	void Variable::write(Writer& w) const
	{
		w.u8(static_cast<std::uint8_t>(type));
		switch (type) {
		case VType::kNull:
		case VType::kString:
		case VType::kInt:
		case VType::kFloat:
		case VType::kBool:
			w.u32(static_cast<std::uint32_t>(value));
			break;
		case VType::kRef:
		case VType::kStruct:
			w.u32(refType);
			w.u64(value);
			break;
		case VType::kVariant:
			if (inner) {
				inner->write(w);
			} else {
				Variable().write(w);
			}
			break;
		default:
			if (IsRefType(type)) {
				w.u32(refType);
			}
			w.u64(value);
			break;
		}
	}

	namespace
	{
		std::vector<Variable> ReadVars(Reader& r, std::uint32_t n)
		{
			std::vector<Variable> v;
			v.reserve(n);
			for (std::uint32_t i = 0; i < n; ++i) {
				v.push_back(Variable::read(r));
			}
			return v;
		}
		void WriteVars(Writer& w, const std::vector<Variable>& v)
		{
			for (const auto& x : v) {
				x.write(w);
			}
		}

		InstanceData ReadInstanceData(Reader& r)
		{
			InstanceData d;
			d.flag = r.u8();
			d.state = r.u32();
			d.unknown1 = r.u32();
			d.unknown2 = (d.flag & 0x04) ? r.u32() : 0;
			const auto n = r.u32();
			d.vars = ReadVars(r, n);
			return d;
		}
		void WriteInstanceData(Writer& w, EID id, const InstanceData& d)
		{
			w.u64(id);
			w.u8(d.flag);
			w.u32(d.state);
			w.u32(d.unknown1);
			if (d.flag & 0x04) {
				w.u32(d.unknown2);
			}
			w.u32(static_cast<std::uint32_t>(d.vars.size()));
			WriteVars(w, d.vars);
		}

		// Papyrus opcode argument counts (negative = that many fixed args + variable tail).
		constexpr int kOpcodeArgs[48] = {
			0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 1, 2, 2, -3, -2, -3, 1, 3, 3, 3, 2, 2, 3, 3, 4, 4, 3, 1, 3, 3, 5, 3, 3, 3, 1, 3, 1, 6
		};

		Param ReadParam(Reader& r)
		{
			Param p;
			const auto pos = r.pos();
			p.type = r.u8();
			switch (p.type) {
			case 0: break;
			case 1:
			case 2:
			case 8: p.value = r.u32(); break;
			case 3:
			case 4: p.value = r.u32(); break;
			case 5: p.value = r.u8(); break;
			default: throw FormatError("invalid opcode parameter type " + std::to_string(p.type), pos);
			}
			return p;
		}
		void WriteParam(Writer& w, const Param& p)
		{
			w.u8(p.type);
			switch (p.type) {
			case 0: break;
			case 5: w.u8(static_cast<std::uint8_t>(p.value)); break;
			default: w.u32(p.value); break;
			}
		}

		Opcode ReadOpcode(Reader& r)
		{
			Opcode o;
			const auto pos = r.pos();
			o.op = r.u8();
			if (o.op >= 48) {
				throw FormatError("invalid opcode " + std::to_string(o.op), pos);
			}
			const int  args = kOpcodeArgs[o.op];
			const bool extra = args < 0;
			const int  fixed = extra ? 1 - args : args;
			for (int i = 0; i < fixed; ++i) {
				o.params.push_back(ReadParam(r));
			}
			if (extra && !o.params.empty()) {
				const auto& last = o.params.back();
				if (last.type != 3) {
					throw FormatError("variable-argument opcode without an integer count", r.pos());
				}
				const auto count = static_cast<std::int32_t>(last.value);
				for (std::int32_t i = 0; i < count; ++i) {
					o.params.push_back(ReadParam(r));
				}
			}
			return o;
		}

		StackFrame ReadFrame(Reader& r)
		{
			StackFrame f;
			const auto varCount = r.u32();
			if (varCount > 50000) {
				throw FormatError("invalid stack frame variable count", r.pos());
			}
			f.flag = r.u8();
			f.fnType = r.u8();
			f.scriptName = r.u32();
			f.baseName = r.u32();
			f.event = r.u32();
			if (!(f.flag & 1) && f.fnType == 0) {
				f.status = r.u32();
			}
			f.opMajor = r.u8();
			f.opMinor = r.u8();
			f.returnType = r.u32();
			f.docString = r.u32();
			f.userFlags = r.u32();
			f.fnFlags = r.u8();
			const auto pc = r.u16();
			for (std::uint16_t i = 0; i < pc; ++i) {
				MemberDesc m;
				m.name = r.u32();
				m.type = r.u32();
				f.params.push_back(m);
			}
			const auto lc = r.u16();
			for (std::uint16_t i = 0; i < lc; ++i) {
				MemberDesc m;
				m.name = r.u32();
				m.type = r.u32();
				f.locals.push_back(m);
			}
			const auto oc = r.u16();
			f.code.reserve(oc);
			for (std::uint16_t i = 0; i < oc; ++i) {
				f.code.push_back(ReadOpcode(r));
			}
			f.ptr = r.u32();
			f.owner = Variable::read(r);
			f.vars = ReadVars(r, varCount);
			return f;
		}
		void WriteFrame(Writer& w, const StackFrame& f)
		{
			w.u32(static_cast<std::uint32_t>(f.vars.size()));
			w.u8(f.flag);
			w.u8(f.fnType);
			w.u32(f.scriptName);
			w.u32(f.baseName);
			w.u32(f.event);
			if (f.status) {
				w.u32(*f.status);
			}
			w.u8(f.opMajor);
			w.u8(f.opMinor);
			w.u32(f.returnType);
			w.u32(f.docString);
			w.u32(f.userFlags);
			w.u8(f.fnFlags);
			w.u16(static_cast<std::uint16_t>(f.params.size()));
			for (const auto& m : f.params) {
				w.u32(m.name);
				w.u32(m.type);
			}
			w.u16(static_cast<std::uint16_t>(f.locals.size()));
			for (const auto& m : f.locals) {
				w.u32(m.name);
				w.u32(m.type);
			}
			w.u16(static_cast<std::uint16_t>(f.code.size()));
			for (const auto& o : f.code) {
				w.u8(o.op);
				for (const auto& p : o.params) {
					WriteParam(w, p);
				}
			}
			w.u32(f.ptr);
			f.owner.write(w);
			WriteVars(w, f.vars);
		}

		FragmentTask ReadFragment(Reader& r, std::uint8_t kind)
		{
			FragmentTask t;
			t.kind = kind;
			std::size_t len = 0;
			if (kind != 2) {
				t.typeCode = r.lstring();
				if (t.typeCode == "QuestStage") {
					len = 3 + 2 + 1;
				} else if (t.typeCode == "ScenePhaseResults" || t.typeCode == "SceneActionResults") {
					len = 3 + 4;
				} else if (t.typeCode == "SceneResults") {
					len = 3;
				} else if (t.typeCode == "TerminalRunResults") {
					len = 1 + 4 + 3 + 4;
				} else if (t.typeCode == "TopicInfo") {
					len = 0;
				} else {
					throw FormatError("unknown fragment task type '" + t.typeCode + "'", r.pos());
				}
			}
			t.payload = r.bytes(len);
			if (kind == 2 || kind == 3) {
				t.variable = Variable::read(r);
			}
			return t;
		}
		void WriteFragment(Writer& w, const FragmentTask& t)
		{
			if (t.kind != 2) {
				w.lstring(t.typeCode);
			}
			w.bytes(t.payload);
			if (t.variable) {
				t.variable->write(w);
			}
		}

		ActiveScriptData ReadActiveData(Reader& r)
		{
			ActiveScriptData d;
			d.major = r.i8();
			d.minor = r.i8();
			if (d.major > 0 && (d.minor < 1 || d.minor > 2)) {
				throw FormatError("bad active script minor version " + std::to_string(d.minor), r.pos());
			}
			d.unknownVar = Variable::read(r);
			d.flag = r.u8();
			d.unknownByte = r.u8();
			d.unknown2 = (d.flag & 1) ? r.u32() : 0;
			d.unknown3 = r.u8();
			if (d.unknown3 >= 1 && d.unknown3 <= 3) {
				d.fragment = ReadFragment(r, d.unknown3);
			}
			const auto count = r.u32();
			if (count > 100000) {
				throw FormatError("invalid stack frame count", r.pos());
			}
			d.frames.reserve(count);
			for (std::uint32_t i = 0; i < count; ++i) {
				d.frames.push_back(ReadFrame(r));
			}
			if (!d.frames.empty()) {
				d.unknown5 = r.u8();
			}
			return d;
		}
		void WriteActiveData(Writer& w, ThreadID id, const ActiveScriptData& d)
		{
			w.u32(id);
			w.i8(d.major);
			w.i8(d.minor);
			d.unknownVar.write(w);
			w.u8(d.flag);
			w.u8(d.unknownByte);
			if (d.flag & 1) {
				w.u32(d.unknown2);
			}
			w.u8(d.unknown3);
			if (d.fragment) {
				WriteFragment(w, *d.fragment);
			}
			w.u32(static_cast<std::uint32_t>(d.frames.size()));
			for (const auto& f : d.frames) {
				WriteFrame(w, f);
			}
			if (d.unknown5) {
				w.u8(*d.unknown5);
			}
		}

		FunctionMessageData ReadFMData(Reader& r)
		{
			FunctionMessageData d;
			d.fnFlags = r.u8();
			d.scriptName = r.u32();
			d.event = r.u32();
			d.unknownVar = Variable::read(r);
			const auto n = r.u32();
			d.vars = ReadVars(r, n);
			return d;
		}
		void WriteFMData(Writer& w, const FunctionMessageData& d)
		{
			w.u8(d.fnFlags);
			w.u32(d.scriptName);
			w.u32(d.event);
			d.unknownVar.write(w);
			w.u32(static_cast<std::uint32_t>(d.vars.size()));
			WriteVars(w, d.vars);
		}

		std::vector<SuspendedStack> ReadSuspended(Reader& r)
		{
			std::vector<SuspendedStack> v;
			const auto                  n = r.u32();
			v.reserve(n);
			for (std::uint32_t i = 0; i < n; ++i) {
				SuspendedStack s;
				s.id = r.u32();
				s.flag = r.u8();
				if (s.flag != 0) {
					s.data = ReadFMData(r);
				}
				v.push_back(std::move(s));
			}
			return v;
		}
		void WriteSuspended(Writer& w, const std::vector<SuspendedStack>& v)
		{
			w.u32(static_cast<std::uint32_t>(v.size()));
			for (const auto& s : v) {
				w.u32(s.id);
				w.u8(s.flag);
				if (s.data) {
					WriteFMData(w, *s.data);
				}
			}
		}

		template <class T, class ID>
		void WriteInOrder(const std::vector<ID>& a_order, const std::vector<T>& a_items, auto&& a_writeOne)
		{
			// Items keep their recorded data order; anything not in it (new items) goes last.
			std::unordered_map<ID, const T*> byId;
			byId.reserve(a_items.size());
			for (const auto& it : a_items) {
				byId.emplace(it.id, &it);
			}
			std::unordered_set<ID> done;
			done.reserve(a_items.size());
			for (ID id : a_order) {
				auto f = byId.find(id);
				if (f != byId.end() && done.insert(id).second) {
					a_writeOne(*f->second);
				}
			}
			for (const auto& it : a_items) {
				if (!done.contains(it.id)) {
					a_writeOne(it);
				}
			}
		}
	}

	void Papyrus::read(Reader& r)
	{
		header = r.u16();

		const auto strCount = r.u32();
		strings.reserve(strCount);
		for (std::uint32_t i = 0; i < strCount; ++i) {
			strings.push_back(r.wstring());
		}

		const auto scriptCount = r.u32();
		scripts.reserve(scriptCount);
		for (std::uint32_t i = 0; i < scriptCount; ++i) {
			Script s;
			s.name = r.u32();
			s.parent = r.u32();
			const auto mc = r.u32();
			s.members.reserve(mc);
			for (std::uint32_t m = 0; m < mc; ++m) {
				MemberDesc d;
				d.name = r.u32();
				d.type = r.u32();
				s.members.push_back(d);
			}
			scripts.push_back(std::move(s));
		}

		const auto instCount = r.u32();
		instances.reserve(instCount);
		for (std::uint32_t i = 0; i < instCount; ++i) {
			ScriptInstance s;
			s.id = r.u64();
			s.script = r.u32();
			s.unknown2bits = r.u16();
			s.unknown = r.u16();
			s.refID = RefID::read(r);
			s.unknownByte = r.u8();
			instances.push_back(std::move(s));
		}

		const auto refCount = r.u32();
		references.reserve(refCount);
		for (std::uint32_t i = 0; i < refCount; ++i) {
			Reference x;
			x.id = r.u64();
			x.script = r.u32();
			references.push_back(std::move(x));
		}

		const auto arrCount = r.u32();
		arrays.reserve(arrCount);
		for (std::uint32_t i = 0; i < arrCount; ++i) {
			ArrayInfo a;
			a.id = r.u64();
			a.type = ReadVType(r);
			if (IsRefType(a.type)) {
				a.refType = r.u32();
			}
			a.length = r.u32();
			arrays.push_back(std::move(a));
		}

		runtime = r.u32();

		const auto activeCount = r.u32();
		activeScripts.reserve(activeCount);
		for (std::uint32_t i = 0; i < activeCount; ++i) {
			ActiveScript a;
			a.id = r.u32();
			a.type = r.u8();
			activeScripts.push_back(std::move(a));
		}

		reindex();

		instanceDataOrder.clear();
		for (std::size_t i = 0; i < instances.size(); ++i) {
			const auto pos = r.pos();
			const EID  id = r.u64();
			auto*      inst = findInstance(id);
			if (!inst) {
				throw FormatError("script instance data for unknown id", pos);
			}
			inst->data = ReadInstanceData(r);
			instanceDataOrder.push_back(id);
		}

		referenceDataOrder.clear();
		for (std::size_t i = 0; i < references.size(); ++i) {
			const auto pos = r.pos();
			const EID  id = r.u64();
			auto*      ref = findReference(id);
			if (!ref) {
				throw FormatError("reference data for unknown id", pos);
			}
			ref->data = ReadInstanceData(r);
			referenceDataOrder.push_back(id);
		}

		arrayDataOrder.clear();
		for (std::size_t i = 0; i < arrays.size(); ++i) {
			const auto pos = r.pos();
			const EID  id = r.u64();
			auto*      arr = findArray(id);
			if (!arr) {
				throw FormatError("array data for unknown id", pos);
			}
			arr->vars = ReadVars(r, arr->length);
			arr->hasData = true;
			arrayDataOrder.push_back(id);
		}

		activeDataOrder.clear();
		for (std::size_t i = 0; i < activeScripts.size(); ++i) {
			const auto pos = r.pos();
			const ThreadID id = r.u32();
			auto*          as = findActive(id);
			if (!as) {
				throw FormatError("active script data for unknown id", pos);
			}
			as->data = ReadActiveData(r);
			activeDataOrder.push_back(id);
		}

		const auto fmCount = r.u32();
		functionMessages.reserve(fmCount);
		for (std::uint32_t i = 0; i < fmCount; ++i) {
			FunctionMessage m;
			m.flag = r.u8();
			if (m.flag <= 2) {
				m.id = r.u32();
			}
			m.flags = r.u8();
			if (m.flags != 0) {
				m.data = ReadFMData(r);
			}
			functionMessages.push_back(std::move(m));
		}

		suspended1 = ReadSuspended(r);
		suspended2 = ReadSuspended(r);

		unk1 = r.u32();
		if (unk1 != 0) {
			unk2 = r.u32();
		}
		const auto unkCount = r.u32();
		unknownIDs.reserve(unkCount);
		for (std::uint32_t i = 0; i < unkCount; ++i) {
			unknownIDs.push_back(r.u64());
		}

		const auto unbindCount = r.u32();
		unbinds.reserve(unbindCount);
		for (std::uint32_t i = 0; i < unbindCount; ++i) {
			QueuedUnbind u;
			u.id = r.u64();
			u.unknown = r.u32();
			unbinds.push_back(u);
		}

		saveFileVersion = r.u16();
		tail = r.bytes(r.remaining());
	}

	std::vector<std::string> Papyrus::check() const
	{
		std::vector<std::string> out;
		std::size_t              badStr = 0;
		auto                     str = [&](TStr t) {
            if (t >= strings.size()) {
                ++badStr;
            }
		};
		auto vars = [&](const std::vector<Variable>& vs) {
			for (const auto& v : vs) {
				const Variable* x = &v;
				if (x->type == VType::kVariant && x->inner) {
					x = x->inner.get();
				}
				if (IsRefType(x->type)) {
					str(x->refType);
				}
				if (x->type == VType::kString) {
					str(static_cast<TStr>(x->value));
				}
			}
		};
		for (const auto& sc : scripts) {
			str(sc.name);
			str(sc.parent);
			for (const auto& m : sc.members) {
				str(m.name);
				str(m.type);
			}
		}
		for (const auto& i : instances) {
			str(i.script);
			if (i.data) {
				str(i.data->state);
				vars(i.data->vars);
			}
		}
		for (const auto& x : references) {
			str(x.script);
			if (x.data) {
				vars(x.data->vars);
			}
		}
		for (const auto& a : arrays) {
			if (IsRefType(a.type)) {
				str(a.refType);
			}
			vars(a.vars);
		}
		for (const auto& a : activeScripts) {
			if (!a.data) {
				continue;
			}
			for (const auto& f : a.data->frames) {
				str(f.scriptName);
				str(f.baseName);
				str(f.event);
				vars(f.vars);
			}
		}
		auto fm = [&](const std::optional<FunctionMessageData>& d) {
			if (d) {
				str(d->scriptName);
				str(d->event);
				vars(d->vars);
			}
		};
		for (const auto& m : functionMessages) {
			fm(m.data);
		}
		for (const auto* list : { &suspended1, &suspended2 }) {
			for (const auto& st : *list) {
				fm(st.data);
			}
		}
		if (badStr) {
			out.push_back(std::to_string(badStr) + " string reference(s) point past the end of the string table (" + std::to_string(strings.size()) +
						  " strings)");
		}

		auto dups = [&](const char* what, auto ids) {
			std::sort(ids.begin(), ids.end());
			const auto n = static_cast<std::size_t>(std::adjacent_find(ids.begin(), ids.end()) != ids.end());
			if (n) {
				std::size_t c = 0;
				for (std::size_t i = 1; i < ids.size(); ++i) {
					c += ids[i] == ids[i - 1] ? 1 : 0;
				}
				out.push_back(std::to_string(c) + " duplicate " + what + " id(s)");
			}
		};
		{
			std::vector<EID> v;
			for (const auto& i : instances) {
				v.push_back(i.id);
			}
			dups("script instance", v);
			dups("script instance data", instanceDataOrder);
			v.clear();
			for (const auto& x : references) {
				v.push_back(x.id);
			}
			dups("reference", v);
			dups("reference data", referenceDataOrder);
			v.clear();
			for (const auto& a : arrays) {
				v.push_back(a.id);
			}
			dups("array", v);
			dups("array data", arrayDataOrder);
			std::vector<ThreadID> t;
			for (const auto& a : activeScripts) {
				t.push_back(a.id);
			}
			dups("thread", t);
			dups("thread data", activeDataOrder);
		}
		std::size_t noData = 0;
		for (const auto& i : instances) {
			noData += i.data ? 0 : 1;
		}
		for (const auto& x : references) {
			noData += x.data ? 0 : 1;
		}
		for (const auto& a : arrays) {
			noData += a.hasData ? 0 : 1;
		}
		for (const auto& a : activeScripts) {
			noData += a.data ? 0 : 1;
		}
		if (noData) {
			out.push_back(std::to_string(noData) + " element(s) listed without their saved data");
		}
		return out;
	}

	void Papyrus::write(Writer& w) const
	{
		w.u16(header);
		w.u32(static_cast<std::uint32_t>(strings.size()));
		for (const auto& s : strings) {
			w.wstring(s);
		}

		w.u32(static_cast<std::uint32_t>(scripts.size()));
		for (const auto& s : scripts) {
			w.u32(s.name);
			w.u32(s.parent);
			w.u32(static_cast<std::uint32_t>(s.members.size()));
			for (const auto& m : s.members) {
				w.u32(m.name);
				w.u32(m.type);
			}
		}

		w.u32(static_cast<std::uint32_t>(instances.size()));
		for (const auto& s : instances) {
			w.u64(s.id);
			w.u32(s.script);
			w.u16(s.unknown2bits);
			w.u16(s.unknown);
			s.refID.write(w);
			w.u8(s.unknownByte);
		}

		w.u32(static_cast<std::uint32_t>(references.size()));
		for (const auto& x : references) {
			w.u64(x.id);
			w.u32(x.script);
		}

		w.u32(static_cast<std::uint32_t>(arrays.size()));
		for (const auto& a : arrays) {
			w.u64(a.id);
			w.u8(static_cast<std::uint8_t>(a.type));
			if (IsRefType(a.type)) {
				w.u32(a.refType);
			}
			w.u32(a.hasData ? static_cast<std::uint32_t>(a.vars.size()) : a.length);
		}

		w.u32(runtime);

		w.u32(static_cast<std::uint32_t>(activeScripts.size()));
		for (const auto& a : activeScripts) {
			w.u32(a.id);
			w.u8(a.type);
		}

		WriteInOrder(instanceDataOrder, instances, [&](const ScriptInstance& s) {
			WriteInstanceData(w, s.id, s.data ? *s.data : InstanceData{});
		});
		WriteInOrder(referenceDataOrder, references, [&](const Reference& x) {
			WriteInstanceData(w, x.id, x.data ? *x.data : InstanceData{});
		});
		WriteInOrder(arrayDataOrder, arrays, [&](const ArrayInfo& a) {
			w.u64(a.id);
			WriteVars(w, a.vars);
		});
		WriteInOrder(activeDataOrder, activeScripts, [&](const ActiveScript& a) {
			if (!a.data) {
				throw std::runtime_error("active script without data cannot be written");
			}
			WriteActiveData(w, a.id, *a.data);
		});

		w.u32(static_cast<std::uint32_t>(functionMessages.size()));
		for (const auto& m : functionMessages) {
			w.u8(m.flag);
			if (m.id) {
				w.u32(*m.id);
			}
			w.u8(m.flags);
			if (m.data) {
				WriteFMData(w, *m.data);
			}
		}

		WriteSuspended(w, suspended1);
		WriteSuspended(w, suspended2);

		w.u32(unk1);
		if (unk2) {
			w.u32(*unk2);
		}
		w.u32(static_cast<std::uint32_t>(unknownIDs.size()));
		for (EID id : unknownIDs) {
			w.u64(id);
		}

		w.u32(static_cast<std::uint32_t>(unbinds.size()));
		for (const auto& u : unbinds) {
			w.u64(u.id);
			w.u32(u.unknown);
		}

		if (saveFileVersion) {
			w.u16(*saveFileVersion);
		}
		w.bytes(tail);
	}

	bool EqualsNoCase(const std::string& a, const std::string& b)
	{
		if (a.size() != b.size()) {
			return false;
		}
		for (std::size_t i = 0; i < a.size(); ++i) {
			const unsigned char x = static_cast<unsigned char>(a[i]), y = static_cast<unsigned char>(b[i]);
			if (x != y && std::tolower(x) != std::tolower(y)) {
				return false;
			}
		}
		return true;
	}

	TStr Papyrus::findString(const std::string& s) const
	{
		for (std::size_t i = 0; i < strings.size(); ++i) {
			if (EqualsNoCase(strings[i], s)) {
				return static_cast<TStr>(i);
			}
		}
		return UINT32_MAX;
	}

	TStr Papyrus::internString(const std::string& s)
	{
		for (std::size_t i = 0; i < strings.size(); ++i) {
			if (strings[i] == s) {
				return static_cast<TStr>(i);
			}
		}
		strings.push_back(s);
		return static_cast<TStr>(strings.size() - 1);
	}

	void Papyrus::reindex()
	{
		_scriptIdx.clear();
		_instIdx.clear();
		_refIdx.clear();
		_arrIdx.clear();
		_activeIdx.clear();
		for (std::size_t i = 0; i < scripts.size(); ++i) {
			_scriptIdx[scripts[i].name] = i;
		}
		for (std::size_t i = 0; i < instances.size(); ++i) {
			_instIdx[instances[i].id] = i;
		}
		for (std::size_t i = 0; i < references.size(); ++i) {
			_refIdx[references[i].id] = i;
		}
		for (std::size_t i = 0; i < arrays.size(); ++i) {
			_arrIdx[arrays[i].id] = i;
		}
		for (std::size_t i = 0; i < activeScripts.size(); ++i) {
			_activeIdx[activeScripts[i].id] = i;
		}
	}

	const Script* Papyrus::findScript(TStr name) const
	{
		auto it = _scriptIdx.find(name);
		return it == _scriptIdx.end() ? nullptr : &scripts[it->second];
	}
	ScriptInstance* Papyrus::findInstance(EID id)
	{
		auto it = _instIdx.find(id);
		return it == _instIdx.end() ? nullptr : &instances[it->second];
	}
	Reference* Papyrus::findReference(EID id)
	{
		auto it = _refIdx.find(id);
		return it == _refIdx.end() ? nullptr : &references[it->second];
	}
	ArrayInfo* Papyrus::findArray(EID id)
	{
		auto it = _arrIdx.find(id);
		return it == _arrIdx.end() ? nullptr : &arrays[it->second];
	}
	ActiveScript* Papyrus::findActive(ThreadID id)
	{
		auto it = _activeIdx.find(id);
		return it == _activeIdx.end() ? nullptr : &activeScripts[it->second];
	}
}
