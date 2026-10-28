// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "ChangeFormData.h"
#include <cmath>
#include <map>
#include <iterator>
#include <set>

#include "ByteIO.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ess::cf
{
	namespace
	{
		template <class... A>
		std::string Fmt(const char* f, A... a)
		{
			char buf[256];
			std::snprintf(buf, sizeof(buf), f, a...);
			return buf;
		}

		bool Bit(std::uint32_t flags, int n) { return (flags >> n) & 1; }

		struct Stop : std::runtime_error
		{
			using std::runtime_error::runtime_error;
		};

		// ---- tree-building reader ----------------------------------------------------------
		class P
		{
		public:
			P(const SaveFile& s, const std::vector<std::uint8_t>& body, const TypeLookup& types, Decoded& out) :
				_save(s), r(body.data(), body.size()), _types(types), _out(out)
			{
				_stack.push_back(&out.root);
			}

			Reader r;
			std::uint8_t                 version = 0;  // the change form's version (some fields depend on it)
			std::set<std::uint64_t>      pathIDs;      // pathing stream objects already read in this change form

			// one change flag's data: where it starts and ends in the body
			int nested = 0;  // > 0 while reading a form embedded in this one (e.g. a leveled actor's NPC)

			template <class F>
			void section(int a_bit, F f)
			{
				const auto start = r.pos();
				f();
				if (nested == 0) {  // an embedded form's flags are not this change form's
					_out.sections[a_bit] = { start, r.pos() };
				}
			}

			std::string lastLabel() { return top().kids.empty() ? std::string() : top().kids.back().label; }

			Node& add(const std::string& label, std::string value = {})
			{
				auto& k = top().kids;
				k.push_back(Node{ label, std::move(value) });
				return k.back();
			}
			template <class F>
			void group(const std::string& label, F f)
			{
				auto& n = add(label);
				_stack.push_back(&n);
				try {
					f();
				} catch (...) {
					_stack.pop_back();
					throw;
				}
				_stack.pop_back();
			}

			std::uint8_t u8(const std::string& l)
			{
				const auto v = r.u8();
				add(l, std::to_string(v));
				return v;
			}
			std::int16_t i16(const std::string& l)
			{
				const auto v = r.i16();
				add(l, std::to_string(v));
				return v;
			}
			std::uint16_t flags16(const std::string& l)
			{
				const auto v = r.u16();
				add(l, Fmt("0x%04X", v));
				return v;
			}
			std::uint8_t flags8(const std::string& l)
			{
				const auto v = r.u8();
				add(l, Fmt("0x%02X", v));
				return v;
			}
			std::uint32_t flags32(const std::string& l)
			{
				const auto v = r.u32();
				add(l, Fmt("0x%08X", v));
				return v;
			}
			std::int32_t i32(const std::string& l)
			{
				const auto v = r.i32();
				add(l, std::to_string(v));
				return v;
			}
			float f32(const std::string& l)
			{
				const auto v = r.f32();
				add(l, Fmt("%g", v));
				return v;
			}
			void floats(const std::string& l, std::size_t n)
			{
				std::string s;
				for (std::size_t i = 0; i < n; ++i) {
					s += (i ? ", " : "") + Fmt("%g", r.f32());
				}
				add(l, s);
			}
			void ints(const std::string& l, std::size_t n)
			{
				std::string s;
				for (std::size_t i = 0; i < n; ++i) {
					if (i < 16) {
						s += (i ? ", " : "") + std::to_string(r.i32());
					} else {
						r.i32();
					}
				}
				if (n > 16) {
					s += Fmt(", ... (%zu)", n);
				}
				add(l, s);
			}
			std::size_t bytes(const std::string& l, std::size_t n)
			{
				r.need(n);
				const auto  at = r.pos();
				std::string s;
				for (std::size_t i = 0; i < n && i < 24; ++i) {
					s += Fmt("%02X ", r.here()[i]);
				}
				if (n > 24) {
					s += Fmt("…（%zu 字节）", n);
				}
				r.seek(r.pos() + n);
				add(l, s);
				return at;
			}
			std::uint32_t vs(const std::string& l)
			{
				const auto    b0 = r.u8();
				std::uint32_t v = b0;
				switch (b0 & 3) {
				case 0:
					break;
				case 1:
					v |= std::uint32_t(r.u8()) << 8;
					break;
				case 2:  // four bytes, as the game reads it (ReSaver read three)
					v |= std::uint32_t(r.u8()) << 8;
					v |= std::uint32_t(r.u8()) << 16;
					v |= std::uint32_t(r.u8()) << 24;
					break;
				default:
					throw Stop("vsval 长度无效");
				}
				v >>= 2;
				if (!l.empty()) {
					add(l, std::to_string(v));
				}
				return v;
			}
			// returns {offset, size}
			std::pair<std::size_t, std::size_t> bytesVS(const std::string& l)
			{
				const auto n = vs(l + "_COUNT");
				const auto at = bytes(l, n);
				return { at, n };
			}
			void intsVS(const std::string& l) { ints(l, vs(l + "_COUNT")); }
			void floatsVS(const std::string& l)
			{
				const auto n = vs(l + "_COUNT");
				floats(l, n);
			}
			void longsVS(const std::string& l)
			{
				const auto n = vs(l + "_COUNT");
				r.need(std::size_t(n) * 8);
				r.seek(r.pos() + std::size_t(n) * 8);
				add(l, Fmt("%u values", n));
			}
			std::uint32_t ref(const std::string& l)
			{
				const auto id = RefID::read(r);
				auto&      n = add(l);
				n.isRef = true;
				n.refRaw = id.data;
				n.formID = id.isZero() ? 0 : _save.formIDOf(id);
				lastFormID = n.formID;
				return id.data;
			}
			std::uint32_t refResolved(const std::string& l)
			{
				ref(l);
				return top().kids.back().formID;
			}
			std::string wstring(const std::string& l)
			{
				const auto n = r.u16();
				r.need(n);
				std::string s(reinterpret_cast<const char*>(r.here()), n);
				r.seek(r.pos() + n);
				add(l, "\"" + s + "\"");
				return s;
			}
			template <class F>
			void vsArray(const std::string& l, F f)
			{
				const auto n = vs("");
				if (n > 100000) {
					throw Stop(Fmt("%s：计数 %u 不合理", l.c_str(), n));
				}
				group(Fmt("%s (%u)", l.c_str(), n), [&] {
					for (std::uint32_t i = 0; i < n; ++i) {
						group(Fmt("[%u]", i), [&] { f(); });
					}
				});
			}
			template <class F>
			void countedArray(const std::string& l, std::uint32_t n, F f)
			{
				if (n > 100000) {
					throw Stop(Fmt("%s：计数 %u 不合理", l.c_str(), n));
				}
				group(Fmt("%s (%u)", l.c_str(), n), [&] {
					for (std::uint32_t i = 0; i < n; ++i) {
						group(Fmt("[%u]", i), [&] { f(); });
					}
				});
			}

			void changeFormFlags()
			{
				group("CHANGE_FORM_FLAGS", [&] {
					flags32("FLAGS");
					i16("UNKNOWN");
				});
			}

			InvCharge*    inv = nullptr;  // inventory entry being read (captures Enchantment / Charge)
			std::uint32_t lastFormID = 0;  // of the last ref() read
			int        extraDepth = 0;  // nesting of extra data groups below the inventory entry

			std::size_t mark() { return top().kids.size(); }
			void        rollback(std::size_t a_mark) { top().kids.resize(a_mark); }

			std::string typeOf(std::uint32_t formID) const { return _types && formID ? _types(formID) : std::string(); }
			Decoded&    out() { return _out; }

		private:
			Node& top() { return *_stack.back(); }

			const SaveFile&    _save;
			const TypeLookup&  _types;
			Decoded&           _out;
			std::vector<Node*> _stack;
		};

		// ---- shared pieces --------------------------------------------------------------------

		void ExtraData(P& p);
		void NPC(P& p, std::uint32_t flags, bool inlined);
		void CombatController(P& p);
		void CombatObjectData(P& p, std::uint32_t a_cls);
		void CombatPathingRequest(P& p);
		void PolicyTail(P& p);
		void CombatObject(P& p, const char* a_label);
		void PathObject(P& p, const char* a_label);
		void PathingObject(P& p, const char* a_label);

		void NonActorMagicCaster(P& p)
		{
			p.i32("unk1");
			p.ref("ref1");
			p.i32("unk2");
			p.i32("unk3");
			p.ref("ref2");
			p.f32("unk4");
			p.ref("ref3");
			p.ref("ref4");
		}

		// PackageLocation / PackageTarget of a package created in game
		void PackageLocation(P& p)
		{
			p.group("LOCATION", [&] {
				const auto type = p.u8("TYPE");
				p.i32("RADIUS");
				switch (type) {
				case 0:
				case 1:
				case 2:
				case 3:
				case 4:
					p.ref("REF");
					break;
				case 5:
				case 8:
				case 9:
				case 10:
					p.i32("VALUE");
					break;
				case 11:
					p.u8("VALUE");
					break;
				default:
					break;
				}
			});
		}

		void PackageTarget(P& p)
		{
			p.group("TARGET", [&] {
				const auto type = p.u8("TYPE");
				p.i32("COUNT");
				switch (type) {
				case 0:
				case 1:
				case 3:
					p.ref("REF");
					break;
				case 2:
				case 4:
				case 5:
					p.i32("VALUE");
					break;
				default:
					break;
				}
			});
		}

		// a package created in game (FF form): TESPackage::LoadGame of its class
		void CreatedPackage(P& p, std::int8_t a_type)
		{
			p.group(Fmt("CREATED PACKAGE (type %d)", a_type), [&] {
				switch (a_type) {
				case 0x0f:
				case 0x15:
				case 0x17:
				case 0x18:
				case 0x1c:
					throw Stop(Fmt("游戏内创建的 AI 包（类型 %d）尚未解码", a_type));
				default:
					break;
				}
				p.bytes("PACKAGE DATA", 12);
				const auto mask = p.u8("PARTS");
				if (mask & 1) {
					PackageLocation(p);
				}
				if (mask & 2) {
					PackageTarget(p);
				}
				if (mask & 4) {
					p.group("TYPE DATA", [&] {
						auto optLocation = [&] {
							if (p.u8("HAS LOCATION")) {
								PackageLocation(p);
							}
						};
						switch (a_type) {
						case 1:   // Follow
						case 2:   // Escort
							p.i32("UNK10");
							optLocation();
							break;
						case 3:   // Eat
						case 8:   // UseItemAt
						case 9:   // Ambush
							optLocation();
							break;
						case 13:  // Patrol
							p.u8("UNK8");
							p.u8("UNK9");
							break;
						case 15:  // Dialogue
						case 16:  // UseWeapon
							throw Stop(Fmt("游戏内创建的包数据（类型 %d）尚未解码", a_type));
						default:
							break;
						}
					});
				}
				p.i32("UNKD8");
				if (a_type == 0x16) {  // FleePackage
					p.u8("UNK110");
					p.u8("UNK111");
					p.floats("POS", 3);
					p.f32("UNK104");
					p.u8("UNK112");
					p.u8("UNK113");
					p.u8("UNK114");
					p.ref("REF1");
					p.ref("REF2");
					const auto n = p.vs("");
					p.countedArray("REFS", n, [&] { p.ref("REF"); });
				}
			});
		}

		void PackageDataExtra(P& p, std::int8_t a_type);

		void ExtraDataData(P& p)
		{
			const auto type = p.r.u8();
			auto       one = [&](const char* name, auto body) { p.group(name, body); };
			switch (type) {
			case 0: one("NULL", [] {}); break;
			case 4: one("ExtraExtraData1", [&] { p.group("SUB_DATA", [&] { ExtraDataData(p); }); }); break;
			case 8:
				one("ExtraExtraData2", [&] {
					p.group("SUB_DATA1", [&] { ExtraDataData(p); });
					p.group("SUB_DATA2", [&] { ExtraDataData(p); });
				});
				break;
			case 12:
				one("ExtraExtraData3", [&] {
					p.group("SUB_DATA1", [&] { ExtraDataData(p); });
					p.group("SUB_DATA2", [&] { ExtraDataData(p); });
					p.group("SUB_DATA3", [&] { ExtraDataData(p); });
				});
				break;
			case 22: one("Worn", [] {}); break;
			case 23: one("WornLeft", [] {}); break;
			case 24:
				one("PackageStartLocation", [&] {
					p.ref("UNK");
					p.floats("POS", 3);
					p.f32("UNK1");
				});
				break;
			case 25:
				one("Package", [&] {
					p.ref("UNK1");
					p.ref("UNK2");
					p.i32("UNK3");
					p.bytes("UNK4", 3);
				});
				break;
			case 26:
				one("TrespassPackage", [&] {
					if (p.ref("PACK") == 0) {
						throw Stop("TrespassPackage 不完整");
					}
				});
				break;
			case 27: one("RunOncePacks", [&] { p.intsVS("PACKS"); }); break;
			case 28: one("ReferenceHandle", [&] { p.ref("ID"); }); break;
			case 29: one("Unknown29", [] {}); break;
			case 30: one("LevCreaModifier", [&] { p.i32("MOD"); }); break;
			case 31: one("Ghost", [&] { p.u8("UNK"); }); break;
			case 32: one("UNKNOWN32", [] {}); break;
			case 33: one("Ownership", [&] { p.ref("OWNER"); }); break;
			case 34: one("Global", [&] { p.ref("UNK"); }); break;
			case 35: one("Rank", [&] { p.i32("RANK"); }); break;  // an i32 (ReSaver read a RefID)
			case 36:
				one("Count", [&] {
					const auto v = p.i16("COUNT");
					if (p.inv) {
						p.inv->count = v;  // this stack's count
					}
				});
				break;
			case 37: one("Health", [&] { p.f32("HEALTH"); }); break;
			case 39: one("TimeLeft", [&] { p.i32("TIME"); }); break;
			case 40:
				one("Charge", [&] {
					const auto at = p.r.pos();
					const auto v = p.f32("CHARGE");
					if (p.inv) {
						p.inv->hasCharge = true;
						p.inv->charge = v;
						p.inv->chargeOffset = at;
					}
				});
				break;
			case 42:
				one("Lock", [&] {
					p.bytes("UNKS", 2);
					p.ref("KEY");
					p.ints("UNKS2", 2);
				});
				break;
			case 43:
				one("Teleport", [&] {
					p.floats("POS", 3);
					p.floats("ROT", 3);
					p.u8("UNK");
					p.ref("REF");
				});
				break;
			case 44: one("MapMarker", [&] { p.u8("UNK"); }); break;
			case 45:
				one("LeveledCreature", [&] {
					p.ref("UNK1");
					p.ref("UNK2");
					const auto flags = p.flags32("NPCChangeFlags");
					++p.nested;
					try {
						p.group("NPC", [&] { NPC(p, flags, true); });
					} catch (...) {
						--p.nested;
						throw;
					}
					--p.nested;
				});
				break;
			case 46:
				one("LeveledItem", [&] {
					p.i32("UNK");
					p.u8("UNK2");
				});
				break;
			case 47: one("Scale", [&] { p.f32("scale"); }); break;
			case 49: one("NonActorMagicCaster", [&] { p.group("CASTER", [&] { NonActorMagicCaster(p); }); }); break;
			case 50:
				one("NonActorMagicTarget", [&] {
					p.ref("ref");
					p.vsArray("targets", [&] {
						p.ref("REF");
						p.u8("unk1");
						p.vs("unk2");
						p.bytesVS("data");
					});
				});
				break;
			case 52: one("PlayerCrimeList", [&] { p.longsVS("list"); }); break;
			case 53: one("Unknown53", [] {}); break;
			case 56: one("ItemDropper", [&] { p.ref("unk"); }); break;
			case 61: one("CannotWear", [] {}); break;
			case 62:
				one("ExtraPoison", [&] {
					p.ref("ref");
					p.i32("unk");
				});
				break;
			case 68: one("FriendHits", [&] { p.floatsVS("unk"); }); break;
			case 69: one("HeadingTarget", [&] { p.ref("targetID"); }); break;
			case 72: one("StartingWorldOrCell", [&] { p.ref("worldOrCellID"); }); break;
			case 73: one("HotKey", [&] { p.u8("unk"); }); break;
			case 76:
				one("InfoGeneralTopic", [&] {
					p.group("TOPIC", [&] {
						p.wstring("NAME");
						p.bytes("UNK", 5);
						for (int i = 0; i < 4; ++i) {
							p.ref(Fmt("REF%d", i));
						}
					});
				});
				break;
			case 77: one("HasNoRumors", [&] { p.u8("FLAG"); }); break;
			case 79: one("TerminalState", [&] { p.bytes("STATE", 2); }); break;
			case 83: one("Unknown83", [&] { p.i32("unk"); }); break;
			case 84: one("CanTalkToPlayer", [&] { p.u8("FLAG"); }); break;
			case 85: one("ObjectHealth", [&] { p.f32("HEALTH"); }); break;
			case 88:
				one("ModelSwap", [&] {
					p.ref("REF");
					p.i32("UNK");
				});
				break;
			case 89: one("Radius", [&] { p.f32("RADIUS"); }); break;
			case 91:
				one("FactionChanges", [&] {
					p.vsArray("FactionChanges", [&] {
						p.ref("FACTION");
						p.u8("RANK");
					});
					p.ref("FACTION2");
					p.u8("RANK2");
				});
				break;
			case 92:
				one("DismemberedLimbs", [&] {
					p.i16("UNK1");
					p.i32("UNK2");
					p.i32("UNK3");
					p.u8("UNK4");
					p.ref("REF");
					p.vsArray("LIMBS", [&] {
						p.bytes("UNK", 4);
						p.vsArray("REFS", [&] { p.ref("REF"); });
					});
				});
				break;
			case 93: one("ActorCause", [&] { p.i32("ID"); }); break;
			case 101: one("CombatStyle", [&] { p.ref("REF"); }); break;
			case 102:
				one("MYSTERIOUS", [&] {
					p.ref("REF1");
					p.ref("REF2");
				});
				break;
			case 104: one("OpenCloseActivateRef", [&] { p.ref("REF"); }); break;
			case 108:
				one("PackageData", [&] {
					const auto type = static_cast<std::int8_t>(p.u8("TYPE"));
					if (type != -1) {
						PackageDataExtra(p, type);
					}
				});
				break;
			case 106:
				one("Ammo", [&] {
					p.ref("REF");
					p.i32("COUNT");
				});
				break;
			case 111:
				one("SayTopicInfoOnceADay", [&] {
					p.vsArray("INFOS", [&] {
						p.ref("INFO");
						p.i32("UNK1");
						p.i32("UNK2");
					});
				});
				break;
			case 112: one("EncounterZone", [&] { p.ref("REF"); }); break;
			case 113:
				one("SayToTopicInfo", [&] {
					p.ref("REF1");
					p.u8("UNK1");
					p.i32("UNK2");
					p.ref("REF2");
					p.group("DATA2", [&] {
						p.wstring("TEXT1");
						p.wstring("TEXT2");
						p.i32("UNK1");
						p.i32("UNK2");
						p.u8("UNK3");
						p.ref("REF1");
						p.ref("REF2");
						p.ref("REF3");
						p.u8("UNK4");
					});
				});
				break;
			case 120:
				one("GuardedRefData", [&] {
					p.vsArray("DATA", [&] {
						p.ref("INFO");
						p.i32("UNK1");
						p.u8("UNK2");
					});
				});
				break;
			case 133: one("AshPileRef", [&] { p.ref("REF"); }); break;
			case 135:
				// ReSaver's "TEST" (read as four RefIDs = 12 bytes). It is the game's FollowerSwimBreadcrumbs:
				// the leader's position and navmesh, a state, then the trail of crumbs left behind
				one("FollowerSwimBreadcrumbs", [&] {
					p.floats("LEADER POS", 3);
					p.ref("NAVMESH");
					p.i32("STATE");
					p.vsArray("CRUMBS", [&] {
						p.floats("FROM", 3);
						p.ref("FROM NAVMESH");
						p.floats("TO", 3);
						p.ref("TO NAVMESH");
						p.u8("FLAG");
					});
				});
				break;
			case 136:
				one("AliasInstanceArray", [&] {
					p.vsArray("ALIASES", [&] {
						p.ref("QUEST");
						p.i32("ALIAS");
					});
				});
				break;
			case 140: one("PromotedRef", [&] { p.vsArray("REFS", [&] { p.ref("REF"); }); }); break;
			case 142: one("OutfitItem", [&] { p.ref("REF"); }); break;
			case 146: one("SceneData", [&] { p.ref("REF"); }); break;
			case 149:
				one("FromAlias", [&] {
					p.ref("REF");
					p.i32("VAL");
				});
				break;
			case 150: one("ShouldWear", [&] { p.u8("UNK"); }); break;
			case 152:
				one("AttachedArrows3D", [&] {
					p.vsArray("ARROWS", [&] {
						if (p.ref("REF") != 0) {
							if (p.i16("unknU16") != -1) {
								p.i32("UNK2");
								p.floats("LOCATION", 8);
							}
						}
					});
					p.i16("UNK1");
					p.i16("UNK2");
				});
				break;
			case 153:
				one("TextDisplayData", [&] {
					p.group("TEXTDISPLAY", [&] {
						const auto r1 = p.ref("REF1");
						const auto r2 = p.ref("REF2");
						const auto unk = p.i32("UNK");
						if (r1 == 0 && r2 == 0 && unk == -2) {
							p.wstring("TEXT");
						}
					});
				});
				break;
			case 155:
				one("Enchantment", [&] {
					const auto raw = p.ref("ENCH");
					const auto at = p.r.pos();
					const auto v = p.i16("CHARGE");
					if (p.inv) {
						p.inv->hasEnchant = true;
						p.inv->enchRaw = raw;
						p.inv->ench = raw ? p.lastFormID : 0;
						p.inv->maxCharge = static_cast<std::uint16_t>(v);
						p.inv->maxChargeOffset = at;
					}
				});
				break;
			case 156: one("Soul", [&] { p.u8("SIZE"); }); break;
			case 157: one("ForcedTarget", [&] { p.ref("TARGET"); }); break;
			case 159:
				one("UniqueId", [&] {
					p.i32("ID1");
					p.i16("ID2");
				});
				break;
			case 160: one("Flags", [&] { p.flags32("FLAGS"); }); break;
			case 161:
				one("RefrPath", [&] {
					p.floats("UNK1", 18);
					p.ints("UNK2", 4);
				});
				break;
			case 164: one("ForcedLandingMarker", [&] { p.ref("SITE"); }); break;
			case 169:
				one("Interaction", [&] {
					p.i32("UNK1");
					p.ref("Target1");
					p.ref("Target2");
					p.u8("UNK2");
				});
				break;
			case 174:
				one("GroupConstraint", [&] {
					p.group("CONSTRAINT", [&] {
						p.i32("UNK1");
						p.ref("REF");
						p.wstring("NAME1");
						p.wstring("NAME2");
						p.floats("POS", 3);
						p.floats("ROT", 3);
						p.i32("UNK2");
						p.f32("UNK3");
					});
				});
				break;
			case 175:
				one("ScriptedAnimDependence", [&] {
					const auto n = static_cast<std::uint32_t>(p.r.i32());
					p.countedArray("CONSTRAINT", n, [&] {
						p.ref("REF");
						p.i32("UNK1");
					});
				});
				break;
			case 176:
				one("CachedScale", [&] {
					p.f32("SCALE1");
					p.f32("SCALE2");
				});
				break;
			default:
				throw Stop(Fmt("未知的额外数据类型 %u", type));
			}
		}

		void ExtraData(P& p)
		{
			const auto n = p.vs("");
			if (n > 1024) {
				throw Stop(Fmt("额外数据条数 %u 过多", n));
			}
			p.group(Fmt("EXTRADATA (%u)", n), [&] {
				for (std::uint32_t i = 0; i < n; ++i) {
					ExtraDataData(p);
				}
			});
		}

		// the reference's own extra data list, with where each entry sits (for removing single entries)
		void TopExtraData(P& p)
		{
			auto& o = p.out();
			o.hasExtras = true;
			o.extrasCountAt = p.r.pos();
			const auto n = p.vs("");
			o.extrasCountEnd = p.r.pos();
			if (n > 1024) {
				throw Stop(Fmt("额外数据条数 %u 过多", n));
			}
			p.group(Fmt("EXTRADATA (%u)", n), [&] {
				for (std::uint32_t i = 0; i < n; ++i) {
					ExtraEntry e;
					e.start = p.r.pos();
					ExtraDataData(p);
					e.end = p.r.pos();
					e.name = p.lastLabel();
					o.extras.push_back(e);
				}
			});
		}

		void Inventory(P& p)
		{
			p.out().inventoryCountAt = p.r.pos();
			p.vsArray("INVENTORY", [&] {
				const auto entryAt = p.r.pos();
				const auto item = p.refResolved("ITEM");
				const auto countAt = p.r.pos();
				const auto count = p.i32("COUNT");
				p.out().inventory.push_back({ item, entryAt, countAt, 0 });
				const auto entryIndex = p.out().inventory.size() - 1;
				// the entry's extra data is a list of extra data lists, one per distinct stack (each its
				// own enchantment, charge, health...)
				const auto lists = p.vs("");
				if (lists > 1024) {
					throw Stop(Fmt("额外数据列表条数 %u 过多", lists));
				}
				p.group(Fmt("EXTRADATA (%u)", lists), [&] {
					for (std::uint32_t i = 0; i < lists; ++i) {
						const auto k = p.vs("");
						if (k > 1024) {
							throw Stop(Fmt("额外数据条数 %u 过多", k));
						}
						InvCharge e;
						e.item = item;
						e.count = 1;  // a stack without a Count extra holds one
						p.inv = &e;
						try {
							p.group(Fmt("ExtraExtraData%u", k), [&] {
								for (std::uint32_t j = 0; j < k; ++j) {
									p.group(k == 1 ? std::string("SUB_DATA") : Fmt("SUB_DATA%u", j + 1), [&] { ExtraDataData(p); });
								}
							});
						} catch (...) {
							p.inv = nullptr;
							throw;
						}
						p.inv = nullptr;
						if (e.hasEnchant || e.hasCharge) {
							p.out().charged.push_back(e);
						}
					}
				});
				(void)count;
				p.out().inventory[entryIndex].endAt = p.r.pos();
			});
		}

		int InitialType(const RefID& id, std::uint32_t flags)
		{
			if (id.type() == RefID::Type::kCreated) {
				return 5;
			}
			if (Bit(flags, 25) || Bit(flags, 3)) {
				return 6;
			}
			if (Bit(flags, 2) || Bit(flags, 1)) {
				return 4;
			}
			return 0;
		}

		void Initial(P& p, int t)
		{
			p.group("INITIAL", [&] {
				switch (t) {
				case 4:
					p.ref("CELL");
					p.floats("POS", 3);
					p.floats("ROT", 3);
					break;
				case 5:
					p.ref("CELL");
					p.floats("POS", 3);
					p.floats("ROT", 3);
					p.u8("UNK");
					p.out().baseObject = p.refResolved("BASE_OBJECT");
					break;
				case 6:
					p.ref("CELL");
					p.floats("POS", 3);
					p.floats("ROT", 3);
					p.ref("STARTING CELL");
					p.i16("UNK1");
					p.i16("UNK2");
					break;
				default:
					break;
				}
			});
		}

		void Havok(P& p, std::uint32_t flags)
		{
			if (Bit(flags, 2)) {
				const auto [at, n] = p.bytesVS("HAVOK");
				p.out().hasHavok = true;
				p.out().havokOffset = at;
				p.out().havokSize = n;
			}
		}

		void Refr(P& p, const RefID& id, std::uint32_t flags)
		{
			const int t = InitialType(id, flags);
			Initial(p, t);
			Havok(p, flags);
			if (Bit(flags, 0)) {
				p.changeFormFlags();
			}
			if (Bit(flags, 7)) {
				p.out().baseObject = p.refResolved("BASE_OBJECT");
			}
			if (Bit(flags, 4)) {
				p.f32("SCALE");
			}
			if (Bit(flags, 6) || Bit(flags, 12) || Bit(flags, 29) || Bit(flags, 31) || Bit(flags, 11) || Bit(flags, 17) || Bit(flags, 25) ||
				Bit(flags, 26) || Bit(flags, 10)) {
				ExtraData(p);
			}
			if (Bit(flags, 5) || Bit(flags, 27)) {
				Inventory(p);
			}
			// ReSaver reads a separate promotion list here; SE saves keep it in the PromotedRef extra
			// data instead (checked against real saves), so there is no list to read.
			if (Bit(flags, 28)) {
				p.bytesVS("ANIMATIONS");
			}
			if (t == 5 && p.r.remaining() && p.typeOf(p.out().baseObject) == "EXPL") {
				p.group("EXPLOSION", [&] {
					p.floats("UNKS", 4);
					p.ints("UNK1", 2);
					p.floats("UNK2", 2);
					p.floats("UNK3", 3);
					p.floats("UNK4", 3);
					p.f32("UNK5");
					for (int i = 0; i < 4; ++i) {
						p.ref(Fmt("REF%d", i));
					}
					p.f32("UNK6");
					p.i32("UNK7");
					p.u8("FLAG");
					p.group("CASTER", [&] { NonActorMagicCaster(p); });
				});
			}
		}

		// ---- the actor's own data, after its reference data (Actor::LoadGame in the game) ----
		// Mapped from SkyrimSE.exe 1.6 (Actor::SaveGame / LoadGame and what they call); names from
		// CommonLibSSE where it has them. Nothing here depends on change flags: every ACHR has it.

		// MagicCaster + ActorMagicCaster state (one of the four casters: left, right, other, instant)
		void ActorCaster(P& p)
		{
			p.i32("STATE");
			p.ref("CURRENT SPELL");
			p.f32("CAST TIME");
			p.i32("UNK1");
			p.ref("DESIRED TARGET");
			p.i32("UNK2");
			p.i32("UNK3");
			p.ref("UNK REF");
			p.flags32("FLAGS");
			const auto n = p.vs("DATA_SIZE");
			p.bytes("DATA", n);
		}

		void ActorValueModifiers(P& p, const char* a_name)
		{
			p.group(a_name, [&] {
				const auto n = static_cast<std::uint32_t>(p.r.i32());
				std::string s;
				for (std::uint32_t i = 0; i < n && i < 3; ++i) {
					s += (i ? ", " : "") + Fmt("%g", p.r.f32());
				}
				p.add("PERMANENT, TEMPORARY, DAMAGE", s);
			});
		}

		// ---- packages: instance data and the procedure tree's run state ----
		// Each procedure's run state is stored by the procedure's name ("Sequence", "Sandbox"...).
		void ProcedureState(P& p);

		void ProcedureChild(P& p)
		{
			ProcedureState(p);
		}
		void ProcedureNamed(P& p, const std::string& a_name);

		// a nested state of a fixed kind, present when its flag byte is set
		// a dialogue topic: an i32 kind, then the topic form when the kind is 0, else a 4-byte value
		void Topic(P& p)
		{
			if (p.i32("TOPIC KIND") == 0) {
				p.ref("TOPIC");
			} else {
				p.i32("TOPIC VALUE");
			}
		}

		void OptionalNamed(P& p, const char* a_name)
		{
			if (p.r.u8()) {
				p.group(a_name, [&] { ProcedureNamed(p, a_name); });
			}
		}

		// ---- pathing stream objects. Polymorphic objects are written as a class ID: Bethesda's CRC32 of
		// the class name (e.g. 330EB0E3 = "PathingRequest") ----
		enum : std::uint32_t
		{
			kBSPathingRequest = 0x0534ED31,
			kPathingRequest = 0x330EB0E3,
			kPathingRequestClosePoint = 0xEB5ED874,
			kPathingRequestFlee = 0xFA2763CE,
			kPathingRequestFly = 0x3C5FF134,
			kPathingRequestStopMoving = 0x8B2152AF,
			kPathingRequestClosestGoal = 0xF31543AB,
			kPathingRequestCover = 0x0618E573,
			kPathingRequestFlyAction = 0xA5021751,
			kPathingRequestFlyHover = 0x8353103B,
			kPathingRequestFlyLand = 0xF075EEF7,
			kPathingRequestFlyOrbit = 0xCDF9A2FC,
			kPathingRequestFlyTakeOff = 0x98C4C679,
			kPathingRequestHide = 0x0528E757,
			kPathingRequestLOS = 0x54DACA55,
			kPathingRequestOptimalLocation = 0xCA622528,
			kPathingRequestRotate = 0xC702BB5B,
		};

		// a polymorphic member: class ID, then that class's data (family = which loader it is)
		void PolyMember(P& p, const char* a_label, const char* a_family)
		{
			p.group(a_label, [&] {
				const auto cls = p.r.u32();
				p.add("CLASS", Fmt("%08X", cls));
				if (cls == 0) {
					return;
				}
				switch (cls) {
				case 0xA5E9A03C:  // PathingCell: the cell, or (no cell) the worldspace
					if (!p.ref("CELL")) {
						p.ref("WORLDSPACE");
					} else {
						p.i32("UNK");
					}
					return;
				case 0x5826A5DD:  // PathingLockData
					if (p.u8("LOCKED")) {
						p.ref("REF");
						if (p.version > 0x15) {
							p.u8("UNK");
						}
					}
					return;
				case 0x0C28D1C5:  // MovementParameters
					p.floats("VALUES", 5);
					return;
				default:
					throw Stop(Fmt("%s类 %08X 尚未解码", a_family, cls));
				}
			});
		}

		// BSPathingLocation: a point on the navmesh
		void PathingLocation(P& p, const char* a_label)
		{
			p.group(a_label, [&] {
				p.floats("POS", 3);
				PolyMember(p, "SPACE", "寻路空间");
				p.ref("CELL/WORLD");
				p.i16("TRIANGLE");
				p.u8("FLAG1");
				p.u8("FLAG2");
			});
		}

		void BSPathingRequest(P& p)
		{
			PathingLocation(p, "START");
			p.f32("START UNK");
			p.group("GOAL", [&] {
				PathingLocation(p, "LOCATION");
				p.floats("VALUES", 7);
			});
			const auto n = static_cast<std::uint32_t>(p.r.i32());
			p.countedArray("SEGMENTS", n, [&] { p.ints("VALUES", 9); });
			p.ints("UNK4", 4);
			p.group("PARAMS", [&] {
				p.i32("UNK1");
				p.i32("UNK2");
				p.i16("UNK3");
				p.i16("UNK4");
			});
			p.group("AVOID", [&] {
				p.i32("UNK1");
				p.i32("UNK2");
				p.i32("UNK3");
				PolyMember(p, "OBJECT", "寻路避让");
				p.ref("REF");
			});
			PolyMember(p, "BUILDER", "路径构建器");
			const auto m = static_cast<std::uint32_t>(p.r.i32());
			p.countedArray("REFS", m, [&] { p.ref("REF"); });
			p.i32("UNK_F0");
		}

		void OptionalRef(P& p, const char* a_label)
		{
			if (p.r.u8()) {
				p.ref(a_label);
			}
		}

		// PathingRequestFly: the shared part of every flying request
		void FlyRequest(P& p)
		{
			BSPathingRequest(p);
			OptionalRef(p, "TARGET");
			OptionalRef(p, "NAVMESH REF");
			p.f32("UNK110");
			if (p.version > 0x3b) {
				p.u8("UNK114");
				p.f32("UNK118");
				p.f32("UNK11C");
			}
			if (p.version > 0x44) {
				p.u8("UNK120");
			}
		}

		void PathingObjectData(P& p, std::uint32_t a_cls)
		{
			switch (a_cls) {
			case kBSPathingRequest:
			case kPathingRequest:
			case kPathingRequestStopMoving:
			case kPathingRequestRotate:
			case kPathingRequestCover:
				BSPathingRequest(p);
				break;
			case kPathingRequestFlee:
				BSPathingRequest(p);
				p.f32("FLEE DISTANCE");
				break;
			case kPathingRequestFly:
			case kPathingRequestFlyTakeOff:
				FlyRequest(p);
				break;
			case kPathingRequestFlyLand:
				FlyRequest(p);
				p.floats("LAND POS", 3);
				p.i32("UNK13");
				p.f32("UNK14");
				p.u8("UNK15");
				if (p.version > 1) {
					p.ref("LAND REF");
				}
				break;
			case kPathingRequestFlyAction:
				FlyRequest(p);
				p.i32("ACTION");
				OptionalRef(p, "TARGET");
				p.floats("POS", 3);
				OptionalRef(p, "REF2");
				OptionalRef(p, "REF3");
				OptionalRef(p, "REF4");
				p.u8("UNK158");
				p.i32("UNK15C");
				break;
			case kPathingRequestFlyHover:
			{
				FlyRequest(p);
				p.floats("HOVER", 3);
				const auto n = p.r.u32();
				p.countedArray("GOALS", n, [&] {
					PathingLocation(p, "LOCATION");
					p.floats("VALUES", 7);
				});
				break;
			}
			case kPathingRequestFlyOrbit:
				FlyRequest(p);
				p.f32("RADIUS");
				p.i32("UNK12C");
				if (p.version > 0x42) {
					p.floats("UNK130", 3);
				}
				break;
			case kPathingRequestHide:
				BSPathingRequest(p);
				PathingLocation(p, "HIDE FROM");
				p.f32("UNK130");
				break;
			case kPathingRequestLOS:
			{
				BSPathingRequest(p);
				const auto n = p.r.u32();
				p.countedArray("POINTS", n, [&] { p.floats("POS", 3); });
				p.floats("UNK120", 5);
				break;
			}
			case kPathingRequestOptimalLocation:
				BSPathingRequest(p);
				p.floats("UNK100", 4);
				break;
			case kPathingRequestClosestGoal:
			{
				BSPathingRequest(p);
				const auto n = p.r.u32();
				p.countedArray("GOALS", n, [&] { PathingLocation(p, "LOCATION"); });
				p.i32("UNK118");
				break;
			}
			case kPathingRequestClosePoint:
				BSPathingRequest(p);
				p.f32("UNK130");
				p.f32("UNK134");
				p.u8("UNK138");
				PathingLocation(p, "CLOSE POINT");
				p.u8("UNK139");
				p.u8("UNK13A");
				break;
			// CombatPathingRequestGeneric<goal policy, search policy>: the class ID is the XOR of the three
			// name CRCs. A save writes the combat request part, then the policy's own state
			case 0x6EF0A66B:  // FindTargetLocation
			case 0x89454196:  // FindHideCover
			case 0xEB5BBC23:  // FindCover
				CombatPathingRequest(p);
				CombatObject(p, "SEARCH");
				PolicyTail(p);
				break;
			case 0xD28D1F59:  // ExitWater
				CombatPathingRequest(p);
				PolicyTail(p);
				break;
			case 0x3A33B38C:  // FlankDistant
				CombatPathingRequest(p);
				p.floats("POS1", 3);
				p.floats("POS2", 3);
				p.f32("UNK130");
				PolicyTail(p);
				break;
			case 0x5361450C:  // SearchWander
				CombatPathingRequest(p);
				p.ref("REF");
				p.floats("POS", 3);
				PolicyTail(p);
				break;
			case 0x5C2673E7:  // FindAttackLocation
			case 0xB8D9F89B:  // Flank
			case 0xC55295FF:  // Flee
			case 0xED6B30CF:  // Retreat
			case 0x0E3458E4:  // ReturnToCombatArea
			case 0xECE12544:  // Search
				throw Stop(Fmt("战斗寻路请求 %08X 尚未解码", a_cls));
			default:
				throw Stop(Fmt("寻路对象类 %08X 尚未解码", a_cls));
			}
		}

		// CombatPathingRequest: the pathing request, the actor, two values, its path and a combat object
		void CombatPathingRequest(P& p)
		{
			BSPathingRequest(p);
			p.ref("ACTOR");
			p.i32("UNK104");
			p.i32("UNK108");
			PathObject(p, "PATH");
			CombatObject(p, "OBJECT");
		}

		// the shared end of every combat pathing policy: a pathing object and four flags
		void PolicyTail(P& p)
		{
			PathingObject(p, "RESULT");
			p.bytes("FLAGS", 4);
		}

		// An object written through the game's pathing stream (BSPathingStream): an ID, and the first
		// time an ID appears in a change form, the object itself (class ID + its data)
		void PathingObject(P& p, const char* a_label)
		{
			p.group(a_label, [&] {
				const std::uint64_t id = p.version >= 0x4b ? p.r.u64() : p.r.u32();
				p.add("ID", Fmt("%llX", static_cast<unsigned long long>(id)));
				if (id == 0 || !p.pathIDs.insert(id).second) {
					return;  // none, or written earlier in this change form
				}
				const auto cls = p.r.u32();
				p.add("CLASS", Fmt("%08X", cls));
				PathingObjectData(p, cls);
			});
		}

		// BGSAIWorldLocation*: a place a procedure goes to or watches, stored by kind
		void AIWorldLocation(P& p, const char* a_label)
		{
			const auto kind = p.r.wstring();
			p.group(std::string(a_label) + " (" + kind + ")", [&] {
				if (kind == "LocRef") {
					p.ref("REF");
					p.f32("RADIUS");
					p.f32("UNK1");
					p.f32("UNK2");
					p.u8("FLAG");
				} else if (kind == "LocPt") {
					p.floats("POS", 3);
					p.ref("WORLDSPACE");
					p.ref("CELL");
					p.f32("RADIUS");
					p.f32("UNK");
					p.u8("FLAG1");
					p.u8("FLAG2");
				} else if (kind == "LocCell") {
					p.ref("CELL");
					p.floats("UNK", 3);
				} else {
					throw Stop("AI 世界位置 '" + kind + "' 尚未解码");
				}
			});
		}

		void OptionalLocation(P& p, const char* a_label)
		{
			if (p.r.u8()) {
				AIWorldLocation(p, a_label);
			}
		}

		void ProcedureState(P& p)
		{
			const auto name = p.r.wstring();
			if (name.empty()) {
				p.add("PROCEDURE", "(none)");
				return;
			}
			p.group(name, [&] { ProcedureNamed(p, name); });
		}

		void ProcedureNamed(P& p, const std::string& name)
		{
			{
				if (name == "Sequence" || name == "Stacked") {
					ProcedureChild(p);
					p.i32("CURRENT");
				} else if (name == "Random") {
					ProcedureChild(p);
					p.i32("CURRENT");
					const auto n = p.vs("");
					p.ints("WEIGHTS", n);
				} else if (name == "Simultaneous") {
					const auto n = p.vs("");
					p.countedArray("CHILDREN", n, [&] {
						p.i32("INDEX");
						ProcedureChild(p);
					});
				} else if (name == "Sandbox") {
					p.i32("UNK08");
					p.i32("UNK3C");
					p.i32("UNK68");
					p.i32("UNK70");
					p.u8("UNKAB");
					p.f32("TIME1");
					p.i32("UNK68B");
					p.f32("TIME2");
					p.bytes("FLAGS", 4);
					p.i32("UNK7C");
					p.u8("UNK84");
					p.i32("UNK8C");
					p.i32("UNK90");
					p.i32("UNK94");
					p.i32("UNK98");
					p.i32("UNKA0");
					p.bytes("UNKA4", 7);
					p.ref("REF1");
					p.ref("REF2");
					p.ref("REF3");
					const auto n = p.vs("");
					p.countedArray("ENTRIES", n, [&] {
						p.i32("UNK1");
						p.i32("UNK2");
						p.i32("UNK3");
						p.ref("REF");
					});
					OptionalLocation(p, "LOCATION");
					ProcedureChild(p);
					p.i32("UNK38");
					PathingObject(p, "PATH");
					p.i32("UNK9C");
					if (p.version > 0x30) {
						p.u8("UNKB1");
					}
				} else if (name == "Follow") {
					p.ref("TARGET");
					OptionalLocation(p, "LOCATION1");
					OptionalLocation(p, "LOCATION2");
					OptionalLocation(p, "LOCATION3");
					p.i32("UNK30");
					p.f32("MIN DISTANCE");
					p.f32("MAX DISTANCE");
					PathingObject(p, "PATH");
					p.bytes("FLAGS", 5);
				} else if (name == "UseWeapon") {
					OptionalLocation(p, "LOCATION");
					OptionalLocation(p, "TARGET LOCATION");
					OptionalNamed(p, "Travel");
					p.ref("WEAPON");
					p.i32("UNK28");
					p.ref("TARGET");
					p.i16("UNK34");
					p.i16("UNK36");
					p.i16("UNK38");
					p.i32("UNK3C");
				} else if (name == "UseMagic") {
					OptionalLocation(p, "LOCATION");
					OptionalLocation(p, "TARGET LOCATION");
					OptionalNamed(p, "Travel");
					p.ref("SPELL");
					p.i32("UNK40");
					p.i32("UNK18");
					if (p.version > 10) {
						p.ref("TARGET");
					}
					p.ref("TARGET2");
					p.i16("UNK50");
					p.i32("UNK54");
					if (p.version >= 0x1e) {
						p.i32("UNK10");
						p.i32("UNK14");
					}
					if (p.version > 0x2b) {
						p.i32("UNK4C");
					}
				} else if (name == "Orbit") {
					p.u8("UNK18");
					OptionalLocation(p, "LOCATION");
					p.u8("UNK19");
				} else if (name == "Acquire") {
					p.i32("UNK08");
					p.ref("TARGET");
					p.i32("UNK10");
					p.ref("ITEM");
					OptionalNamed(p, "Activate");
				} else if (name == "Find") {
					OptionalLocation(p, "LOCATION");
					p.i32("UNK");
				} else if (name == "Guard" || name == "HoldPosition") {
					OptionalLocation(p, "LOCATION");
				} else if (name == "KeepAnEyeOn") {
					OptionalLocation(p, "LOCATION");
					OptionalLocation(p, "TARGET LOCATION");
				} else if (name == "LockUnlock") {
					OptionalLocation(p, "LOCATION");
					p.u8("UNK1");
					p.u8("UNK2");
				} else if (name == "Wait") {
					p.f32("TIME");
				} else if (name == "Say") {
					p.u8("DONE");
				} else if (name == "Done") {
				} else if (name == "Eat") {
					p.ref("FOOD");
					p.i32("UNK");
					p.u8("UNK1");
					p.u8("UNK2");
				} else if (name == "Wander") {
					OptionalLocation(p, "LOCATION");
					p.i32("UNK10");
					p.u8("UNK20");
					p.i32("UNK14");
					PathingObject(p, "PATH");
				} else if (name == "Activate") {
					p.u8("TARGET INDEX");
					p.ref("TARGET");
					OptionalLocation(p, "LOCATION");
					p.i32("UNK28");
					p.i32("UNK2C");
					p.u8("UNK30");
					p.u8("UNK31");
					PathingObject(p, "PATH");
				} else if (name == "SitSleep") {
					p.u8("TARGET INDEX");
					p.u8("UNK20");
					OptionalNamed(p, "Activate");
				} else if (name == "UseIdleMarker") {
					p.u8("UNK08");
					p.ref("MARKER");
					p.ref("IDLE");
					p.i32("UNK18");
					PathingObject(p, "PATH");
					p.u8("UNK28");
				} else if (name == "Patrol") {
					p.i32("UNK08");
					p.group("PATROL DATA", [&] {
						p.i32("UNK08");
						p.i32("UNK0C");
						p.i32("UNK38");
						p.u8("UNK35");
						p.u8("UNK34");
						p.i32("UNK30");
						const auto n = p.vs("");
						p.countedArray("REFS", n, [&] { p.ref("REF"); });
					});
					OptionalNamed(p, "UseIdleMarker");
					OptionalNamed(p, "SitSleep");
					PathingObject(p, "PATH");
					p.u8("UNK50");
					p.u8("UNK51");
					p.u8("UNK52");
				} else if (name == "Travel") {
					p.u8("UNK20");
					p.u8("UNK21");
					p.u8("UNK23");
					OptionalLocation(p, "LOCATION");
					p.u8("TARGET INDEX");
					PathingObject(p, "PATH");
				} else if (name == "Dialogue") {
					p.i32("UNK08");
					p.ref("SPEAKER");
					p.ref("TARGET");
					Topic(p);
					p.u8("UNK28");
				} else if (name == "DialogueActivate") {
					OptionalLocation(p, "LOCATION");
					OptionalLocation(p, "TARGET LOCATION");
					p.ref("TARGET");
					Topic(p);
					OptionalNamed(p, "Dialogue");
					p.u8("UNK40");
					PathingObject(p, "PATH");
				} else if (name == "Escort") {
					OptionalLocation(p, "LOCATION");
					const auto n = p.vs("COUNT");
					p.countedArray("ESCORTED", n, [&] { p.ref("REF"); });
					p.u8("UNK34");
					p.u8("UNK35");
					PathingObject(p, "PATH");
					p.i32("UNK30");
					p.u8("UNK36");
					p.i32("UNK38");
					p.i32("UNK3C");
				} else if (name == "Flee") {
					OptionalLocation(p, "LOCATION");
					OptionalNamed(p, "Travel");
					p.floats("FLEE FROM", 3);
					p.ints("UNK2C", 6);
					const auto n = p.vs("COUNT");
					p.countedArray("AVOID", n, [&] { p.ref("REF"); });
					p.ref("REF60");
					p.ref("REF64");
					p.bytes("FLAGS", 4);
					PathingObject(p, "PATH");
				} else if (name == "FlightGrab") {
					p.ref("TARGET");
					PathingObject(p, "PATH");
					p.u8("UNK18");
					p.u8("UNK19");
				} else if (name == "Hover") {
					OptionalLocation(p, "LOCATION");
					p.u8("UNK18");
				} else if (name == "Shout") {
					OptionalLocation(p, "LOCATION");
					OptionalNamed(p, "Travel");
					p.ref("SHOUT");
					p.i32("UNK30");
					p.ref("TARGET");
					p.i32("UNK3C");
					p.i32("UNK10");
					p.i32("UNK14");
					if (p.version > 0x2e) {
						p.ref("UNK40");
					}
				} else {
					throw Stop("AI 包过程 '" + name + "' 尚未解码");
				}
			}
		}

		void PackageLocation(P& p);
		void PackageTarget(P& p);

		// one entry of a package's instance data (BGSPackageData*), by type name
		void PackageValue(P& p)
		{
			p.u8("INDEX");
			const auto type = p.r.wstring();
			p.group(type, [&] {
				if (type == "Bool") {
					p.u8("VALUE");
				} else if (type == "Int") {
					p.i32("VALUE");
				} else if (type == "Float") {
					p.f32("VALUE");
				} else if (type == "SingleRef" || type == "Ref" || type == "TargetSelector") {
					PackageTarget(p);
				} else if (type == "Location") {
					PackageLocation(p);
				} else if (type == "ObjectList") {
					const auto n = p.vs("");
					p.countedArray("ITEMS", n, [&] {
						p.ref("REF");
						p.ref("FORM");
						p.floats("VALUES", 4);
					});
				} else {
					throw Stop("包数据值 '" + type + "' 尚未解码");
				}
			});
		}

		void PackageData(P& p, std::int8_t a_type)
		{
			p.group(Fmt("PACKAGE DATA (%d)", a_type), [&] {
				switch (a_type) {
				case 18:  // CustomActorPackageData: instance values, then the procedure tree's run state
					{
						const auto n = p.vs("");
						p.countedArray("VALUES", n, [&] { PackageValue(p); });
						ProcedureState(p);
					}
					break;
				case 2:  // EscortActorPackageData
					p.ref("ESCORTEE");
					break;
				case 12:  // SandBoxActorPackageData
					{
						p.ints("UNK08", 8);
						p.bytes("FLAGS", 4);
						p.i32("UNK48");
						p.u8("UNK50");
						p.i32("UNK58");
						p.ints("UNK5C", 4);
						p.bytes("UNK6C", 7);
						p.ref("REF14");
						p.ref("REF4C");
						p.ref("REF54");
						const auto n = p.vs("COUNT");
						p.countedArray("MARKERS", n, [&] {
							p.ints("VALUES", 3);
							p.ref("REF");
						});
					}
					break;
				case 13:  // PatrolActorPackageData
					{
						p.i32("STATE");
						p.i32("UNK0C");
						p.i32("UNK38");
						p.u8("UNK35");
						p.u8("UNK34");
						p.i32("UNK30");
						const auto n = p.vs("COUNT");
						p.countedArray("VISITED", n, [&] { p.ref("REF"); });
					}
					break;
				case 14:  // GuardActorPackageData
					p.i32("UNK08");
					p.i32("UNK14");
					p.i32("UNK10");
					p.ref("REF");
					p.i32("UNK18");
					p.i32("UNK1C");
					p.i32("UNK20");
					p.u8("UNK30");
					if (p.u8("HAS LOCATION")) {
						p.group("LOCATION", [&] {
							const auto kind = p.u8("TYPE");
							p.i32("RADIUS");
							if (kind == 0 || kind == 1 || kind == 3) {
								p.ref("REF");
							} else if (kind == 2 || kind == 4 || kind == 5) {
								p.i32("VALUE");
							}
							p.ref("CELL");
						});
					}
					break;
				case 16:  // UseWeaponActorPackageData: nothing saved
					break;
				default:
					break;  // the other package types have no data object
				}
			});
		}

		void PackageDataExtra(P& p, std::int8_t a_type)
		{
			PackageData(p, a_type);
		}

		// ActorPackage: the package an actor is running (AIProcess, and again in the middle-high process)
		void ActorPackage(P& p, const char* a_name)
		{
			p.group(a_name, [&] {
				const auto package = p.ref("PACKAGE");
				if (!package) {
					return;  // no package: nothing else is saved for it
				}
				if ((package >> 22) == 2) {  // a package created in game (FF): its type, or -1 for none
					const auto created = static_cast<std::int8_t>(p.u8("CREATED TYPE"));
					if (created != -1) {
						CreatedPackage(p, created);
					}
				}
				// the package's procedure type, or -1 when there is no data; only some types carry
				// a data object
				const auto type = static_cast<std::int8_t>(p.u8("PACKAGE TYPE"));
				if (type != -1) {
					PackageData(p, type);
				}
				p.f32("UNK1");
				p.f32("UNK2");
				p.f32("UNK3");
				p.i16("UNK4");
				p.u8("UNK5");
				p.u8("FLAGS");
				p.ref("TARGET");
			});
		}

		void ValueList(P& p, const char* a_name)
		{
			const auto n = static_cast<std::uint32_t>(p.r.i32());
			p.countedArray(a_name, n, [&] {
				p.i32("INDEX");
				p.i32("VALUE");
				p.u8("FLAG");
			});
		}

		// active magic effects: each carries its own size, so its data is kept as bytes
		void ActiveEffects(P& p)
		{
			const auto n = p.vs("");
			p.countedArray("ACTIVE EFFECTS", n, [&] {
				p.ref("MAGIC ITEM");
				p.u8("EFFECT INDEX");
				p.vs("TYPE");
				const auto size = p.vs("SIZE");
				p.bytes("DATA", size);
			});
		}

		void MiddleHighProcess(P& p)
		{
			p.f32("UNK2C8");
			p.f32("UNK2CC");
			p.f32("UNK300");
			p.u8("UNK322");
			p.floats("UNKE0", 3);
			p.f32("UNK29C");
			p.f32("UNK2E8");
			p.u8("UNK315");
			p.u8("UNK31B");
			p.u8("UNK313");
			p.f32("UNK290");
			p.u8("UNK31C");
			p.f32("UNK2A0");
			p.f32("UNK2A4");
			p.u8("UNK310");
			p.u8("UNK321");
			p.i16("UNK308");
			p.f32("UNK2E4");
			p.u8("UNK312");
			p.f32("UNK2E0");
			p.f32("UNK298");
			p.u8("UNK31E");
			p.u8("UNK31F");
			p.u8("UNK314");
			p.ref("UNK280");
			p.ref("UNK210");
			p.ref("UNK218");
			p.ref("UNK208");
			p.ref("UNK228");
			p.floats("UNKD4", 3);
			p.f32("UNK294");
			p.u8("UNK320");
			p.f32("UNK2C0");
			p.i32("UNK");
			const auto n = p.vs("");
			p.countedArray("REFS", n, [&] { p.ref("REF"); });
			ActorPackage(p, "PACKAGE 2");
			ActiveEffects(p);
			p.ref("UNK240");
			const auto m = p.vs("");
			p.countedArray("ITEMS", m, [&] {
				p.ref("ITEM");
				p.i32("UNK");
				if (p.version > 0x2f) {
					p.u8("UNK40");
					p.group("DATA", [&] {
						p.i32("UNK08");
						p.ref("REF1");
						p.ref("REF2");
						p.bytes("FLAGS", 5);
					});
				} else {  // the older layout
					p.ref("REF");
					p.i32("UNK18");
					p.bytes("FLAGS", 6);
				}
			});
			p.f32("UNK2B4");
			p.f32("UNK2B0");
			p.f32("UNK2B8");
			p.ref("UNKEC");
			p.f32("UNKF0");
			p.f32("UNK2BC");
			p.u8("UNK325");
			p.f32("UNK2F8");
			p.bytes("FLAGS", 5);
			if (p.version > 7) {
				p.u8("UNK32D");
			}
			if (p.version > 0xc) {
				p.u8("UNK32E");
			}
		}

		// animation files loaded for an actor (resource IDs: folder hash, file hash, "hkx")
		void AnimationFiles(P& p, const char* a_label)
		{
			p.group(a_label, [&] {
				if (!p.u8("HAS ANIMATIONS")) {
					return;
				}
				p.u8("UNK");
				if (p.version > 0x34) {
					p.u8("UNK2");
				}
				const auto n = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("FILES", n, [&] {
					const auto dir = p.r.u32();
					const auto file = p.r.u32();
					const auto ext = p.r.u32();
					p.add("FILE", Fmt("%08X %08X %.4s", dir, file, reinterpret_cast<const char*>(&ext)));
				});
			});
		}

		void HighProcess(P& p)
		{
			p.u8("UNK454");
			p.u8("UNK45A");
			p.u8("UNK45B");
			p.i16("UNK2DA");
			p.floats("UNK1", 9);
			p.i16("UNK448");
			p.i16("UNK44A");
			p.i16("UNK44C");
			p.u8("UNK457");
			p.floats("UNK2E8", 3);
			p.floats("UNK2DC", 3);
			p.floats("UNK2", 6);
			p.u8("UNK45C");
			p.f32("UNK390");
			p.u8("UNK45D");
			p.f32("UNK394");
			p.u8("UNK46A");
			p.f32("UNK3A0");
			p.f32("UNK3A4");
			p.u8("UNK451");
			p.f32("UNK3C0");
			p.floats("UNK2C0", 3);
			p.u8("UNK44F");
			p.u8("UNK44E");
			p.f32("UNK2CC");
			p.f32("UNK328");
			p.u8("UNK44E_2");
			p.u8("UNK44E_3");
			p.i32("UNK3D0");
			p.f32("UNK200");
			p.u8("UNK450");
			p.f32("UNK2A0");
			p.f32("UNK2B0");
			p.f32("UNK160");
			p.f32("UNK1E0");
			p.u8("UNK460");
			if (p.version > 0x33) {
				p.u8("UNK461");
			}
			p.u8("UNK462");
			p.f32("UNK210");
			p.u8("UNK466");
			p.u8("UNK465");
			p.f32("UNK29C");
			p.u8("UNK");
			p.u8("UNK2");
			p.ref("UNK2F4");
			p.ref("UNK");
			p.ref("UNK138");
			p.ref("UNK178");
			p.ref("UNK36C");
			p.ref("UNK348");
			p.ref("UNK350");
			p.ref("UNK17C");
			p.countedArray("SLOTS", 6, [&] {
				p.ref("REF");
				p.u8("FLAG");
			});
			if (p.u8("HAS LIST")) {
				p.group("LIST", [&] {
					const auto n = p.vs("");
					p.countedArray("ENTRIES", n, [&] {
						p.add("NAME1", p.r.wstring());
						p.add("NAME2", p.r.wstring());
						p.i32("UNK10");
						p.i32("UNK14");
						p.u8("UNK38");
						p.ref("REF1");
						p.ref("REF2");
						p.ref("REF3");
						p.u8("UNK39");
					});
					p.i16("CURRENT");
					p.ref("REF20");
					p.ref("REF28");
					p.ref("REF30");
					p.ref("REF38");
				});
			}
			{
				const auto n = p.vs("");
				p.countedArray("AVOID", n, [&] {
					p.ints("VALUES", 9);
					p.f32("UNK24");
					p.ref("REF");
				});
			}
			if (p.u8("HAS UNK3D8")) {
				p.i32("UNK0");
				p.floats("UNK4", 3);
				p.f32("UNK10");
				p.ref("REF");
			}
			{
				const auto n = p.vs("CRIME_SIZE");
				p.bytes("CRIME", n);
			}
			p.u8("UNK45F");
			p.u8("UNK");
			{
				const auto n = p.vs("");
				p.countedArray("PAIRS", n, [&] {
					p.i32("UNK1");
					p.i32("UNK2");
					p.ref("REF1");
					p.ref("REF2");
				});
			}
			{
				const auto n = p.vs("");
				p.countedArray("KNOWLEDGE", n, [&] {
					p.ref("REF");
					p.ref("REF1");
					p.ref("REF2");
					p.i32("UNK40");
					p.f32("UNK44");
					if (p.u8("HAS DATA")) {
						p.group("DATA", [&] {
							p.i32("UNK10");
							p.bytes("FLAGS", 3);
							p.floats("POS1", 3);
							p.floats("POS2", 3);
							p.floats("POS3", 3);
							p.f32("TIME1");
							p.f32("TIME2");
							p.f32("TIME3");
						});
					}
				});
			}
			p.i32("UNK0");
			p.ref("UNK8");
			p.floats("UNK10", 6);
			p.u8("UNK467");
			p.f32("UNK318");
			p.f32("UNK1FC");
			p.f32("UNK1F4");
			p.f32("UNK1F8");
			p.ref("UNK358");
			p.ref("UNKD8");
			p.add("UNK258", p.r.wstring());
			if (p.u8("HAS EQUIP DATA")) {
				p.group("EQUIP DATA", [&] {
					p.u8("UNK1");
					p.u8("UNK455");
					if (p.u8("HAS IDLE")) {
						p.add("IDLE", p.r.wstring());
						p.i32("UNK");
					}
					if (p.version > 0x31 && p.u8("HAS IDLE2")) {
						p.add("IDLE2", p.r.wstring());
					}
					// two animation slots (3 and 4): the animation files they hold (resource IDs: folder
					// hash, file hash, "hkx")
					for (int slot = 3; slot < 5; ++slot) {
						AnimationFiles(p, Fmt("ANIMATION SLOT %d", slot).c_str());
					}
					p.i16("UNK3F8");
					p.i16("UNK3FA");
					if (p.version >= 0x32) {
						p.i32("UNK200");
						p.u8("UNK32F");
					}
				});
			}
			p.bytes("FLAGS", 5);
			p.f32("UNK32C");
			p.u8("UNK470");
			p.u8("UNK459");
			p.f32("UNK2B4");
			p.ref("UNKE0");
			p.ref("UNKE8");
			p.floats("UNK3E8", 2);
			p.u8("UNK471");
			if (p.version > 0xc) {
				p.u8("UNK330");
			}
			if (p.version > 0xf) {
				p.ref("UNK270");
				p.u8("UNK463");
				p.u8("UNK464");
			}
			if (p.version > 0x16) {
				p.f32("UNK3A8");
				p.f32("UNK3B0");
			}
			if (p.version > 0x24) {
				p.u8("UNK472");
			}
			if (p.version > 0x26) {
				p.u8("UNK469");
			}
		}

		// AIProcess: present when the actor has an AI process (process level 0-3). Level 3 (low) saves
		// only this common part; 2 adds cached values, 1 the middle-high process, 0 the high process.
		void AIProcess(P& p, std::int32_t a_level)
		{
			p.group("AI PROCESS", [&] {
				p.f32("UNK48");
				p.f32("UNK4C");
				if (p.u8("HAS UNK DATA")) {
					p.group("UNK DATA", [&] {
						p.floats("VALUES", 12);
						ValueList(p, "LIST1");
						ValueList(p, "LIST2");
					});
				}
				ActorPackage(p, "CURRENT PACKAGE");
				p.u8("UNK136");
				p.f32("HOURS TO SLEEP?");
				p.ref("UNK100");
				p.i32("UNK58");
				p.i32("UNK94");
				p.i32("UNK98");
				p.u8("UNK138");
				p.i16("UNK134");
				p.floats("TIMESTAMP", 2);
				p.ref("UNK120");
				const auto n = p.vs("");
				p.countedArray("REFS", n, [&] { p.ref("REF"); });
				const auto m = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("PAIRS", m, [&] {
					p.ref("REF1");
					p.ref("REF2");
				});
				p.ref("UNKF0");
				p.ref("UNKF8");
				p.u8("UNK139");
				p.ref("UNK128");
				if (a_level == 3) {
					return;
				}
				p.f32("CACHED VALUES");  // level 2 (middle-low) ends here
				if (a_level == 2) {
					return;
				}
				p.group("MIDDLE HIGH", [&] { MiddleHighProcess(p); });
				if (a_level == 0) {
					p.group("HIGH", [&] { HighProcess(p); });
				}
				if (p.version > 0x3d) {
					p.u8("UNK332");
				}
			});
		}

		// ---- MovementControllerNPC (through the pathing stream) ----
		static const char* kArbiters[] = { "MovementHandlerArbiter", "MovementPathManagerArbiter", "MovementPlannerArbiter",
			"MovementPostUpdateArbiter", "MovementTweenerArbiter" };

		const char* AgentName(std::uint8_t a_id)
		{
			switch (a_id) {
			case 0x00: return "MovementStateFollowPath";
			case 0x01: return "MovementStateTweener";
			case 0x02: return "MovementHandlerAgentAngleController";
			case 0x03: return "MovementHandlerAgentAngleGain";
			case 0x04: return "MovementHandlerAgentAnglePassThrough";
			case 0x05: return "MovementHandlerAgentDirectionPassThrough";
			case 0x06: return "MovementHandlerAgentLargeDeltaIdle";
			case 0x07: return "MovementHandlerAgentSpeedPassThrough";
			case 0x08: return "MovementHandlerAgentStrafing";
			case 0x09: return "MovementHandlerAgentTranslationController";
			case 0x0A: return "MovementAgentActorState";
			case 0x0B: return "MovementAgentActorAvoider";
			case 0x0C: return "MovementAgentPathFollowerFlight";
			case 0x0D: return "MovementAgentPathFollowerStandard";
			case 0x0E: return "MovementAgentPathFollowerVirtual";
			case 0x0F: return "MovementPlannerAgentNavmeshBounds";
			case 0x10: return "MovementPathManagerAgentAvoidBox";
			case 0x11: return "MovementPathManagerAgentLoadedAreaMonitor";
			case 0x12: return "MovementPathManagerAgentStaticAvoider";
			case 0x13: return "MovementTweenerAgentFixedDelta";
			case 0x80: return "MovementHandlerAgentDirectControl";
			case 0x81: return "MovementHandlerAgentPlayerControls";
			case 0x82: return "MovementHandlerAgentPlayerControlsActionTrigger";
			case 0x83: return "MovementHandlerAgentSprintActionTrigger";
			case 0x84: return "MovementHandlerAgentStairsHelper";
			case 0x85: return "MovementPlannerAgentDirectControl";
			case 0x86: return "MovementPlannerAgentHorseControls";
			case 0x87: return "MovementPlannerAgentKeepOffset";
			case 0x88: return "MovementPlannerAgentWarp";
			case 0x89: return "MovementTweenerAgentAnimationDriven";
			case 0x8A: return "MovementTweenerAgentNodeFollower";
			default: return nullptr;
			}
		}

		void Stream18(P& p, const char* a_label);
		// PathingDoor, BSPathingDoor
		bool PathingDoorClass(std::uint32_t a_cls) { return a_cls == 0xE48B73F3 || a_cls == 0x72BFDFEF; }

		// a path (the 0x70 kind of shared pathing object): ID; the first time, class ID + data
		void PathObject(P& p, const char* a_label)
		{
			p.group(a_label, [&] {
				const std::uint64_t id = p.version >= 0x4b ? p.r.u64() : p.r.u32();
				p.add("ID", Fmt("%llX", static_cast<unsigned long long>(id)));
				if (id == 0 || !p.pathIDs.insert(id).second) {
					return;
				}
				p.add("CLASS", Fmt("%08X", p.r.u32()));
				const auto n1 = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("NODES", n1, [&] {
					PathingLocation(p, "LOCATION");
					Stream18(p, "EDGE");
				});
				p.i32("UNK88");
				p.i32("UNK8C");
				const auto n2 = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("POINTS", n2, [&] {
					PathingLocation(p, "LOCATION");
					p.floats("VALUES", 3);
					Stream18(p, "EDGE");
				});
				p.u8("UNKA8");
				p.u8("UNKA9");
				const auto n3 = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("POINTS2", n3, [&] {
					PathingLocation(p, "LOCATION");
					p.floats("VALUES", 3);
					Stream18(p, "EDGE");
				});
			});
		}

		void MovementArbiter(P& p, const std::string& a_name)
		{
			auto tweenerEntry = [&] { p.floats("VALUES", 13); };
			if (a_name == "MovementHandlerArbiter") {
			} else if (a_name == "MovementPathManagerArbiter") {
				p.i32("STATE");
				PathingObject(p, "REQUEST");
				PathObject(p, "PATH");
			} else if (a_name == "MovementPlannerArbiter") {
				p.u8("UNK24");
			} else if (a_name == "MovementPostUpdateArbiter") {
				p.floats("VALUES", 14);
			} else if (a_name == "MovementTweenerArbiter") {
				const auto n = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("ENTRIES", n, [&] { tweenerEntry(); });
				p.group("UNKD4", [&] {
					p.i32("UNK");
					p.floats("VALUES", 33);
				});
				p.floats("A", 13);
				p.floats("B", 13);
				p.floats("C", 13);
				p.floats("VALUES", 4);
			} else {
				throw Stop("移动仲裁器 '" + a_name + "' 尚未解码");
			}
		}

		void MovementAgent(P& p, const std::string& a_name)
		{
			auto pointList = [&](const char* a_l) {
				const auto n = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray(a_l, n, [&] { p.floats("POINT", 3); });
				p.i32("UNK");
			};
			static const std::set<std::string> kEmpty = { "MovementHandlerAgentDirectControl", "MovementHandlerAgentPlayerControlsActionTrigger",
				"MovementHandlerAgentStairsHelper", "MovementPlannerAgentDirectControl", "MovementPlannerAgentHorseControls",
				"MovementHandlerAgentAngleGain", "MovementHandlerAgentSpeedPassThrough", "MovementHandlerAgentAnglePassThrough",
				"MovementHandlerAgentDirectionPassThrough", "MovementHandlerAgentStrafing", "MovementStateTweener", "MovementStateFollowPath",
				"MovementHandlerAgentAngleController", "MovementTweenerAgentNodeFollower" };
			if (kEmpty.count(a_name)) {
				return;
			}
			if (a_name == "MovementHandlerAgentPlayerControls") {
				p.i32("UNK38");
				p.f32("UNK48");
				p.u8("UNK4C");
			} else if (a_name == "MovementHandlerAgentSprintActionTrigger") {
				if (p.version >= 0x1d) {
					p.i32("UNK38");
				}
				p.u8("UNK3C");
				p.u8("UNK3D");
			} else if (a_name == "MovementPlannerAgentKeepOffset") {
				p.ref("TARGET");
				p.floats("OFFSET", 8);
			} else if (a_name == "MovementPlannerAgentWarp") {
				PathingLocation(p, "FROM");
				PathingLocation(p, "TO");
				p.f32("UNK88");
				p.ref("REF");
				p.i32("STATE");
				p.i32("UNKB4");
			} else if (a_name == "MovementTweenerAgentAnimationDriven") {
				p.i32("STATE");
			} else if (a_name == "MovementAgentActorState") {
				p.group("STATE", [&] {
					p.i32("UNK0");
					p.floats("VALUES", 5);
				});
				p.f32("UNK1C");
				if (p.version > 0x1a) {
					p.f32("UNK20");
				}
				p.i32("UNK58");
				p.i32("UNK70");
			} else if (a_name == "MovementAgentActorAvoider") {
				PathingObject(p, "REQUEST");
				const auto n = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("ACTORS", n, [&] {
					p.ref("ACTOR");
					p.floats("VALUES", 12);
					p.i32("UNK24");
					p.i32("UNK28");
					p.u8("UNK3C");
				});
				p.i32("UNK60");
				p.i32("UNK64");
				p.i32("UNK68");
			} else if (a_name == "MovementHandlerAgentTranslationController") {
				p.u8("UNK28");
				p.i32("UNK38");
			} else if (a_name == "MovementTweenerAgentFixedDelta") {
				p.floats("VALUES", 31);
			} else if (a_name == "MovementPathManagerAgentLoadedAreaMonitor") {
				PathObject(p, "PATH");
			} else if (a_name == "MovementHandlerAgentLargeDeltaIdle") {
				p.i32("UNK28");
			} else if (a_name == "MovementPathManagerAgentAvoidBox") {
				p.i32("STATE");
				p.f32("UNK34");
				PathingLocation(p, "LOCATION");
				p.floats("VALUES", 3);
				p.u8("UNKA8");
				const auto door = p.r.u32();
				if (door != 0) {
					if (!PathingDoorClass(door)) {
						throw Stop(Fmt("寻路门类 %08X 尚未解码", door));
					}
					p.ref("DOOR");
				}
				PolyMember(p, "LOCK", "寻路避让");
			} else if (a_name == "MovementPathManagerAgentStaticAvoider") {
				if (p.version >= 0x1a) {
					p.f32("UNK18");
				}
				const auto n = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("BOXES", n, [&] { p.floats("VALUES", 13); });
				p.floats("UNK1C", 5);
				const auto m = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("SEGMENTS", m, [&] { p.ints("VALUES", 9); });
				PathingObject(p, "REQUEST");
				p.f32("UNKB0");
				p.f32("UNKB4");
				p.i32("UNKB8");
			} else if (a_name == "MovementAgentPathFollowerStandard") {
				p.i32("STATE");
				PathingObject(p, "REQUEST");
				PathObject(p, "PATH");
				p.floats("VALUES", 7);
				p.add("PATHING STATE ID", Fmt("%llX", static_cast<unsigned long long>(p.version >= 0x4b ? p.r.u64() : p.r.u32())));
				p.floats("VALUES2", 3);
				p.i32("UNK15C");
				p.i32("UNK160");
				p.f32("PARAMETER REACHED");
				p.f32("GOAL REACHED");
				p.group("PLAY IDLE", [&] {
					if (p.u8("HAS IDLE")) {
						p.add("IDLE", p.r.wstring());
					}
					p.i32("UNK10");
				});
				p.group("FOLLOW PATH TO PARAMETER", [&] {
					p.floats("VALUES", 3);
					p.i32("INDEX");
				});
				p.group("TURN TO ANGLE", [&] {
					p.i32("UNK08");
					p.floats("VALUES", 5);
				});
				p.group("KEEP LAST DIRECTION", [&] {
					p.floats("VALUES", 4);
					p.i32("INDEX");
				});
			} else if (a_name == "MovementPlannerAgentNavmeshBounds") {
				p.i32("STATE");
				p.f32("UNK44");
				p.f32("UNK48");
				const auto n = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("BOUNDS", n, [&] {
					p.floats("BOX", 6);
					const auto m = static_cast<std::uint32_t>(p.r.i32());
					p.countedArray("POINTS", m, [&] { p.floats("POINT", 3); });
					p.i32("UNKA0");
				});
				PathObject(p, "PATH");
			} else if (a_name == "MovementAgentPathFollowerFlight") {
				p.i32("STATE");
				p.floats("VALUES", 5);
				PathingObject(p, "REQUEST");
				PathObject(p, "PATH");
				p.f32("UNK3C");
				p.i32("UNK58");
				p.i32("UNK5C");
				pointList("POINTS1");
				pointList("POINTS2");
			} else if (a_name == "MovementAgentPathFollowerVirtual") {
				p.i32("STATE");
				PathingObject(p, "REQUEST");
				PathObject(p, "PATH");
			} else {
				throw Stop("移动代理 '" + a_name + "' 尚未解码");
			}
		}

		// members of the movement/path family written as class ID + data (class IDs: CRC32 of names)
		void Stream18(P& p, const char* a_label)
		{
			p.group(a_label, [&] {
				const auto cls = p.r.u32();
				p.add("CLASS", Fmt("%08X", cls));
				if (cls == 0) {
					return;
				}
				auto movementType = [&](const char* a_l) {
					p.group(a_l, [&] {
						p.add("NAME", p.r.wstring());
						p.floats("SPEEDS", 10);
						p.floats("ROTATION", 4);
					});
				};
				switch (cls) {
				case 0x5CC2A237:  // MovementMessageActivateDoor
				case 0x7663F86A:  // MovementMessageApproachingDoor
				case 0x2D8AD535:  // MovementMessageDoor
					p.add("MESSAGE", "Door");
					if (p.u8("HAS DOOR")) {
						const auto door = p.r.u32();
						if (door == 0x00000000) {
						} else if (PathingDoorClass(door)) {
							p.ref("DOOR");
						} else {
							throw Stop(Fmt("寻路门类 %08X 尚未解码", door));
						}
					}
					break;
				case 0xCDED4F63:  // MovementMessageNewPath
				case 0x0F13E00F:  // MovementMessagePathCleared
				case 0xD7578F99:  // MovementMessagePathComplete
				case 0xABBC80DC:  // MovementMessagePathEvent
				case 0x8BCEF6C4:  // MovementMessagePathFailed
					p.add("MESSAGE", "Path");
					PathingObject(p, "REQUEST");
					break;
				case 0xC8B4153E:  // MovementMessageBlocked
				case 0x77A37BFA:  // MovementMessageActorCollision
					break;
				case 0x0000001D:  // a message filed under a plain number rather than a name CRC; it
					              // saves nothing (the controller fields line up right after it)
					break;
				case 0x3BED430B:  // MovementMessageFreezeDirection
					p.i32("FREEZE");
					break;
				case 0x00DC870E:  // MovementMessageJump
					p.f32("HEIGHT");
					break;
				case 0x88058586:  // MovementMessageMovementTypeChange
					movementType("FROM");
					movementType("TO");
					break;
				case 0x616653D5:  // MovementMessageSetStaticPath
					p.u8("STATIC");
					break;
				case 0x67DA9023:  // MovementMessageWarpToLocation
					PathingLocation(p, "LOCATION");
					p.floats("ANGLES", 3);
					break;
				default:
					throw Stop(Fmt("移动对象类 %08X 尚未解码", cls));
				}
			});
		}

		void MovementController(P& p)
		{
			p.group("MOVEMENT CONTROLLER", [&] {
				const auto n1 = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("MESSAGES", n1, [&] { Stream18(p, "MESSAGE"); });
				if (p.version > 0x38) {
					const auto n2 = static_cast<std::uint32_t>(p.r.i32());
					p.countedArray("LOCATIONS", n2, [&] {
						PathingLocation(p, "LOCATION");
						p.floats("VALUES", 3);
						p.i32("UNK");
					});
				}
				p.i32("STATE");
				p.bytes("FLAGS", 7);
				const auto na = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("ARBITERS", na, [&] {
					std::string name;
					if (p.version < 0x36) {
						name = p.r.wstring();
					} else {
						const auto id = p.r.u8();
						name = id < 5 ? kArbiters[id] : Fmt("arbiter %u", id);
					}
					p.group(name, [&] { MovementArbiter(p, name); });
				});
				const auto ng = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("AGENTS", ng, [&] {
					std::string name;
					if (p.version < 0x36) {
						name = p.r.wstring();
					} else {
						const auto id = p.r.u8();
						const auto* nm = AgentName(id);
						name = nm ? nm : Fmt("agent %u", id);
					}
					p.group(name, [&] { MovementAgent(p, name); });
				});
				if (p.u8("HAS IDLE DATA")) {
					AnimationFiles(p, "ANIMATIONS");
					if (p.u8("HAS IDLE")) {
						p.group("PLAY IDLE", [&] {
							p.add("IDLE", p.r.wstring());
							p.i32("UNK");
							p.floats("VALUES", 33);
							p.add("PATHING STATE ID", Fmt("%llX", static_cast<unsigned long long>(p.version >= 0x4b ? p.r.u64() : p.r.u32())));
						});
					}
				}
			});
		}

		void ActorData(P& p, std::uint8_t a_version, std::int32_t a_processLevel)
		{
			if (a_processLevel != -1) {
				const auto at = p.r.pos();
				AIProcess(p, a_processLevel);
				p.out().aiProcess = { at, p.r.pos() };
			}
			p.out().hasActorData = true;
			p.group("ACTOR", [&] {
				p.u8("UNK_278");
				p.f32("UPDATE TARGET TIMER");
				p.f32("VOICE TIMER");
				p.f32("LAST UPDATE (AGE)");
				p.f32("CHECK DEAD BODY TIMER");
				p.i32("ACTION VALUE");
				p.f32("TIMER ON ACTION");
				p.i32("UNK_11C");
				p.i32("EMOTION TYPE");
				p.i32("EMOTION VALUE");
				p.i32("CRITICAL STAGE");
				p.i32("THIEF CRIME STAMP");
				p.i32("LAST SEEN TIME");
				p.f32("BASE HEIGHT");
				p.ref("KILLER");
				p.group("ACTOR STATE", [&] { p.bytes("BITS", 18); });
				p.ref("UNK HANDLE");
				const auto mask = p.u8("MAGIC CASTERS");
				static const char* kSlots[4] = { "LEFT HAND CASTER", "RIGHT HAND CASTER", "OTHER CASTER", "INSTANT CASTER" };
				for (int i = 0; i < 4; ++i) {
					if (mask & (1u << i)) {
						p.group(kSlots[i], [&] { ActorCaster(p); });
					}
				}
				p.ref("SELECTED SPELL LEFT");
				p.ref("SELECTED SPELL RIGHT");
				p.ref("SELECTED SPELL OTHER");
				p.ref("SELECTED SPELL INSTANT");
				p.ref("SELECTED POWER");
				ActorValueModifiers(p, "HEALTH MODIFIERS");
				ActorValueModifiers(p, "MAGICKA MODIFIERS");
				ActorValueModifiers(p, "STAMINA MODIFIERS");
				ActorValueModifiers(p, "VOICE MODIFIERS");
				p.group("ACTOR VALUES", [&] {
					// the actor value is written as an i32 holding a signed byte (133 = DragonSouls reads as -123)
					p.out().baseAVListAt = p.r.pos();
					p.vsArray("BASE", [&] {
						const auto av = p.i32("AV");
						p.out().baseAVs.push_back({ static_cast<std::uint32_t>(av) & 0xFF, p.r.pos() });
						p.f32("VALUE");
					});
					p.out().avModListAt = p.r.pos();
					p.vsArray("MODIFIERS", [&] {
						const auto av = p.i32("AV");
						p.out().avMods.push_back({ static_cast<std::uint32_t>(av) & 0xFF, p.r.pos() });
						p.floats("PERMANENT, TEMPORARY, DAMAGE", 3);
					});
				});
				p.out().addedSpellsAt = p.r.pos();
				const auto spells = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("ADDED SPELLS", spells, [&] {
					const auto at = p.r.pos();
					p.out().addedSpells.push_back({ p.refResolved("SPELL"), at });
				});
				{
					const auto at = p.r.pos();
					if (p.u8("HAS MOVEMENT CONTROLLER")) {
						MovementController(p);
					}
					p.out().movement = { at, p.r.pos() };
				}
				p.flags32("BOOL FLAGS");
				{
					const auto at = p.r.pos();
					if (p.u8("HAS COMBAT CONTROLLER")) {
						CombatController(p);
					}
					p.out().combat = { at, p.r.pos() };
				}
				const auto n = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("UNK LIST", n, [&] {
					p.ref("REF");
					p.i32("VALUE");
				});
				p.i32("INTIMIDATE / BRIBE DAY");
				if (a_version > 0x27) {
					p.f32("UNDERWATER TIMER");
				}
				if (a_version > 0x2a) {
					p.ref("EXCLUSIVE BRANCH");
				}
				if (a_version > 0x2c) {
					p.u8("UNK_27B");
				}
			});
		}

		// CombatInventoryItemMagicT<item, caster>: the game files each pairing under the XOR of the
		// three name CRCs; every pairing loads like CombatInventoryItemMagic
		constexpr std::uint32_t kMagicItemPairings[] = {
			0x03ADDFBA, 0x091A0CDA, 0x09B2C33F, 0x0A158F4F, 0x0A90173E, 0x0BC670CF,
			0x12C735BF, 0x1448419A, 0x15D7FD71, 0x1A294A22, 0x1B0C8C0D, 0x25B25C08,
			0x2A4CEB5B, 0x2B692D74, 0x2CC9133E, 0x37F333C8, 0x397CB368, 0x397FADA3,
			0x3A702E36, 0x3AF5B647, 0x3D66C842, 0x40AE6A73, 0x42721666, 0x42DAD983,
			0x4AE38186, 0x506496B1, 0x5141509E, 0x59AF2F03, 0x5D061FA6, 0x63CD085E,
			0x67D77374, 0x6958F3D4, 0x69CA8E7A, 0x6EDA46B4, 0x70CBCB0A, 0x7118348A,
			0x719DACFB, 0x7262D322, 0x72BF78FA, 0x81AA2A4D, 0x89C18538, 0x8B689D10,
			0x8BB536C8, 0x8CC8A35A, 0x8DED6575, 0x90C0C048, 0x964FB46D, 0x9EDD3D46,
			0x9EDE238D, 0xAD319910, 0xADB40161, 0xAEB89C3F, 0xB276735F, 0xB5F4C63F,
			0xBB0D3C69, 0xBF613DB5, 0xC075E391, 0xC46C8670, 0xCAC230F1, 0xCE6B0054,
			0xCEF97DFA, 0xD2636346, 0xD346A569, 0xD51EF6D4, 0xD76A4524, 0xDD27AED1,
			0xE5D08683, 0xECDDB343, 0xED420FA8, 0xF06526D5, 0xF31FC17D, 0xF39A590C,
			0xF4092709, 0xFAA79188, 0xFE9CDC83,
		};

		// A combat object (CombatObject subclass), written by reference: a u32 ID where 0 is none, an
		// even ID points at an object written earlier and an odd ID is followed by the class and data
		void CombatObject(P& p, const char* a_label)
		{
			p.group(a_label, [&] {
				const auto id = p.r.u32();
				p.add("ID", Fmt("%X", id));
				if ((id & 1) == 0) {
					return;
				}
				const auto cls = p.r.u32();
				p.add("CLASS", Fmt("%08X", cls));
				CombatObjectData(p, cls);
			});
		}

		void CombatObjectData(P& p, std::uint32_t a_cls)
		{
			const auto aimBase = [&] {
				p.floats("POS", 3);
				p.ref("REF");
				p.i32("UNK34");
				p.i32("UNK38");
				p.i32("UNK3C");
				p.f32("TIMER");
				p.i32("UNK44");
			};
			const auto areaBase = [&] {
				p.ref("REF");
				p.i32("UNK28");
				p.u8("UNK2C");
				p.u8("UNK2D");
			};
			const auto itemBase = [&] {
				p.ref("ITEM");
				p.i32("UNK18");
				p.ref("SOURCE");
				p.i32("UNK28");
			};
			const auto areaLocation = [&] {
				p.bytes("UNK20", 8);
				p.floats("POS", 3);
				p.ref("CELL");
			};
			switch (a_cls) {
			case 0xC8F87A78:  // CombatObject
			case 0x860CC21F:  // CombatAimControllerBase
			case 0xE588E90D:  // CombatPathSpeedController
				break;
			case 0x86D330A0:  // CombatAimController
			case 0xBE236AC9:  // CombatDisableAimController
				aimBase();
				break;
			case 0x64927B7C:  // CombatMeleeAimController
				aimBase();
				p.i32("UNK48");
				p.u8("UNK4C");
				p.i32("UNK50");
				break;
			case 0x5AD9B53F:  // CombatTrackTargetAimController
				aimBase();
				p.i32("UNK48");
				p.u8("UNK4C");
				break;
			case 0xF06E7A91:  // CombatProjectileAimController
				aimBase();
				p.ref("PROJECTILE");
				p.ints("UNK50", 3);
				p.floats("VECTORS", 15);
				p.ints("UNK98", 3);
				p.f32("UNKB0");
				p.floats("UNKB4", 3);
				p.f32("UNKC0");
				p.wstring("NODE");
				break;
			case 0x3888CDCA:  // CombatArea
				areaBase();
				p.f32("TIMER");
				p.i32("UNK24");
				break;
			case 0xA9BDAAFE:  // CombatAreaStandard
			case 0x2E71F18B:  // CombatAreaRadius
				areaBase();
				areaLocation();
				p.i32("UNK48");
				break;
			case 0x55FAB455:  // CombatAreaHoldPosition
			case 0x55C1257B:  // CombatAreaReference
				areaBase();
				areaLocation();
				p.i32("UNK48");
				p.ref("REF2");
				p.f32("TIMER");
				p.i32("UNK58");
				break;
			case 0x9C2C29FA:  // CombatTargetLocationSearch
				{
					CombatObject(p, "OBJECT");
					const auto n = p.vs("COUNT");
					p.countedArray("IDS", n, [&] { p.i32("ID"); });
					const auto m = p.vs("COUNT");
					p.countedArray("CANDIDATES", m, [&] {
						p.ints("VALUES", 5);
						p.floats("POS", 3);
					});
					p.ref("TARGET");
					p.ref("REF2");
					p.ref("REF3");
					p.floats("POS", 3);
					p.ints("UNK60", 3);
					p.i32("UNK6C");
				}
				break;
			case 0xC331439E:  // CombatTargetLocationSearchResult
				CombatObject(p, "SEARCH");
				CombatObject(p, "LOCATION");
				break;
			case 0xD6E95B87:  // CombatTargetSelector
			case 0xF7B17BBC:  // CombatTargetSelectorFixed
				p.ref("TARGET");
				p.i32("UNK1C");
				p.i32("UNK20");
				break;
			case 0xC3719B85:  // CombatTargetSelectorStandard
				p.ref("TARGET");
				p.i32("UNK1C");
				p.i32("UNK20");
				p.f32("TIMER");
				p.i32("UNK2C");
				break;
			case 0x1F7409DD:  // CombatInventoryItem
				itemBase();
				break;
			case 0x4C526266:  // CombatInventoryItemComposite
				itemBase();
				p.ref("REF");
				break;
			case 0x9C2A0B47:  // CombatInventoryItemMagic
			case 0xAC4FAA3E:  // CombatInventoryItemPotion
			case 0x0BEE2410:  // CombatInventoryItemScroll
			case 0x5545E40C:  // CombatInventoryItemShout
			case 0xD74211FB:  // CombatInventoryItemStaff
				itemBase();
				p.ints("UNK30", 5);
				p.i32("EFFECT INDEX");
				break;
			case 0x5ACB99C8:  // CombatInventoryItemMelee
				itemBase();
				p.i32("UNK30");
				break;
			case 0x8121581D:  // CombatInventoryItemOneHandedBlock
			case 0xF86AC87B:  // CombatInventoryItemShield
			case 0x66D5D91E:  // CombatInventoryItemTorch
				itemBase();
				if (p.version > 0x20) {
					p.i32("UNK30");
				}
				break;
			case 0x71B72E31:  // CombatInventoryItemRanged
				itemBase();
				p.ref("AMMO");
				p.i32("UNK38");
				p.i32("UNK3C");
				break;
			case 0x51B14DE6:  // CombatMagicCaster
			case 0x11126C48:  // CombatMagicCasterArmor
			case 0x3DBA3C9A:  // CombatMagicCasterBoundItem
			case 0x22784EA4:  // CombatMagicCasterCloak
			case 0xB6B0FCAD:  // CombatMagicCasterDisarm
			case 0xA3055CFB:  // CombatMagicCasterInvisibility
			case 0xC52FCE43:  // CombatMagicCasterLight
			case 0xEC01479B:  // CombatMagicCasterParalyze
			case 0x02212AB0:  // CombatMagicCasterScript
			case 0xE694BC11:  // CombatMagicCasterStagger
			case 0xD2CA5063:  // CombatMagicCasterSummon
			case 0x5AD2B911:  // CombatMagicCasterTargetEffect
				CombatObject(p, "ITEM");
				break;
			case 0x41A74F91:  // CombatMagicCasterOffensive
				CombatObject(p, "ITEM");
				p.bytes("UNK20", 8);
				p.f32("TIMER");
				p.i32("UNK2C");
				break;
			case 0x0304EC9F:  // CombatMagicCasterReanimate
				CombatObject(p, "ITEM");
				CombatObject(p, "OBJECT");
				p.ref("TARGET");
				break;
			case 0x58A60AE1:  // CombatMagicCasterRestore
				CombatObject(p, "ITEM");
				p.f32("UNK20");
				p.i32("UNK24");
				p.i32("UNK28");
				break;
			case 0x22FDD6D5:  // CombatMagicCasterWard
				CombatObject(p, "ITEM");
				p.f32("TIMER");
				p.i32("UNK24");
				break;
			default:
				if (std::find(std::begin(kMagicItemPairings), std::end(kMagicItemPairings), a_cls) !=
					std::end(kMagicItemPairings)) {
					itemBase();
					p.ints("UNK30", 5);
					p.i32("EFFECT INDEX");
					break;
				}
				throw Stop(Fmt("战斗对象类 %08X 尚未解码", a_cls));
			}
		}

		void CombatObjects(P& p, const char* a_label)
		{
			const auto n = p.vs("COUNT");
			p.countedArray(a_label, n, [&] { CombatObject(p, "OBJECT"); });
		}

		void CombatController(P& p)
		{
			p.group("COMBAT CONTROLLER", [&] {
				p.ref("ATTACKER");
				p.ref("TARGET");
				p.i32("COMBAT GROUP");
				p.bytes("FLAGS", 6);
				p.i32("UNK4C");
				p.bytes("UNK44", 8);
				CombatObjects(p, "AIM CONTROLLERS");
				p.i32("CURRENT AIM CONTROLLER");
				p.i32("PREVIOUS AIM CONTROLLER");
				CombatObjects(p, "AREAS");
				p.i32("CURRENT AREA");
				CombatObjects(p, "TARGET SELECTORS");
				CombatObject(p, "CURRENT TARGET SELECTOR");
				CombatObject(p, "PREVIOUS TARGET SELECTOR");
				p.group("STATE", [&] {
					p.bytes("FLAGS", 6);
					p.i32("UNK0C");
					p.bytes("UNK10", 12);
					p.i32("UNK4C");
					p.u8("UNK6C");
					p.u8("UNK6D");
					p.ints("UNK70", 3);
					p.i32("UNK00");
					p.u8("UNK04");
					p.floats("UNK1C", 4);
					p.bytes("TIMERS", 32);
					const auto n = p.vs("COUNT");
					p.countedArray("POSITIONS", n, [&] {
						p.floats("POS", 3);
						p.ref("CELL");
						p.f32("TIME");
					});
					const auto m = p.vs("COUNT");
					p.countedArray("UNK50", m, [&] { p.ref("REF"); });
					p.u8("UNKA0");
					p.bytes("UNKAC", 8);
					p.bytes("UNKA4", 8);
					p.bytes("UNKB4", 8);
					p.f32("UNK68");
				});
				p.group("INVENTORY", [&] {
					p.ints("UNK19C", 10);
					p.u8("UNK1C4");
					p.i32("UNK190");
					p.bytes("UNK194", 8);
					for (int i = 0; i < 7; ++i) {
						CombatObjects(p, "ITEMS");
					}
					for (int i = 0; i < 2; ++i) {
						p.group("ITEM LIST", [&] {
							CombatObjects(p, "ITEMS");
							p.ints("UNK", 4);
							p.i32("UNK28");
						});
					}
					const auto n = p.vs("COUNT");
					p.countedArray("RATED ITEMS", n, [&] {
						CombatObject(p, "ITEM");
						p.f32("RATING");
					});
					const auto a = p.vs("COUNT");
					p.countedArray("REFS1", a, [&] { p.ref("REF"); });
					const auto b = p.vs("COUNT");
					p.countedArray("REFS2", b, [&] { p.ref("REF"); });
					const auto c = p.vs("COUNT");
					p.countedArray("COUNTS", c, [&] {
						p.ref("REF");
						p.i32("COUNT");
					});
				});
				p.group("BLACKBOARD", [&] {
					const auto n = p.vs("COUNT");
					p.countedArray("VALUES", n, [&] {
						const auto name = p.r.wstring();
						p.group(name, [&] {
							// the game's six blackboard keys and their value types
							if (name == "TargetUnreachableDistance") {
								p.f32("VALUE");
							} else if (name == "AcquireWeaponTimer") {
								p.f32("TIMER");
								p.f32("INTERVAL");
							} else if (name == "CombatDisarmedTimeStamp" || name == "CoverSearchFailedTimeStamp" ||
									   name == "FindAttackLocationFailedTimeStamp") {
								p.f32("TIME STAMP");
							} else if (name == "LastFleeDoor") {
								p.ref("DOOR");
								p.f32("TIME STAMP");
							} else {
								throw Stop("战斗黑板键 '" + name + "' 尚未解码");
							}
						});
					});
					p.i32("UNK40");
				});
				p.group("BEHAVIOR CONTROLLER", [&] {
					p.i32("UNK70");
					if (p.u8("HAS TREE")) {
						// the running behavior tree is written with its size up front
						const auto size = p.vs("TREE SIZE");
						p.bytes("TREE STATE", size);
					}
					const auto n = p.vs("COUNT");
					p.countedArray("THREAD IDS", n, [&] { p.i32("ID"); });
					const auto m = p.vs("COUNT");
					p.countedArray("NAMED", m, [&] {
						p.wstring("NAME");
						p.i32("THREAD");
						const auto a = p.vs("COUNT");
						p.countedArray("THREADS", a, [&] { p.i32("ID"); });
						const auto b = p.vs("COUNT");
						p.countedArray("PAIRS", b, [&] {
							p.i32("UNK");
							p.i32("THREAD");
						});
					});
				});
			});
		}

		// ---- the player's own data, after its actor data (PlayerCharacter::LoadGame) ----

		// a third person camera state (also bleedout); horse and dragon cameras add a target
		void ThirdPersonCamera(P& p)
		{
			p.floats("OFFSET", 3);
			p.f32("UNK74");
			p.bytes("UNKD4", 8);
			p.u8("UNKDC");
			p.f32("UNK84");
			p.wstring("NODE");
		}

		void PlayerCamera(P& p)
		{
			p.group("CAMERA", [&] {
				p.u8("UNK160");
				p.u8("UNK162");
				p.f32("UNK13C");
				p.f32("UNK140");
				p.group("FIRST PERSON", [&] {
					p.f32("UNK68");
					if (p.version > 0x14) {
						p.f32("UNK6C");
						p.f32("UNK70");
						p.u8("UNK85");
						p.u8("UNK86");
					}
				});
				p.group("THIRD PERSON", [&] { ThirdPersonCamera(p); });
				// auto vanity, VATS, free, iron sights, furniture and tween menu save nothing
				p.group("HORSE", [&] {
					ThirdPersonCamera(p);
					p.u8("UNKF0");
					p.ref("HORSE");
					p.i32("UNKEC");
				});
				// the transition state saves nothing
				p.group("BLEEDOUT", [&] { ThirdPersonCamera(p); });
				p.group("ANIMATED", [&] { ThirdPersonCamera(p); });
				if (p.version > 0x40) {
					p.group("DRAGON", [&] {
						ThirdPersonCamera(p);
						p.u8("UNKF0");
						p.ref("DRAGON");
						p.i32("UNKEC");
					});
				}
				p.i32("CURRENT STATE");
				const auto n = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("STATE STACK", n, [&] { p.i32("STATE"); });
				if (p.version > 0x12) {
					p.f32("UNK158");
					p.f32("UNK15C");
					p.u8("UNK164");
				}
			});
		}

		void PlayerSkin(P& p)
		{
			p.group("SKIN", [&] {
				if (p.version < 0x1c) {
					p.ref("RACE");
					p.ref("RACE2");
				} else {
					p.ref("RACE OLD");
					p.ref("RACE");
					p.ref("RACE2");
				}
				if (p.version > 4) {
					p.i32("UNK");
				}
				for (const char* name : { "TINTS", "TINTS2" }) {
					const auto n = static_cast<std::uint32_t>(p.r.i32());
					p.countedArray(name, n, [&] {
						p.i16("INDEX");
						p.i32("COLOR");
						p.f32("ALPHA");
					});
				}
				p.ref("REF");
			});
		}

		void PlayerData(P& p)
		{
			p.group("PLAYER", [&] {
				const auto& v = p.version;
				p.u8("UNK_BOOL1");
				p.i32("UNK8EC");
				p.bytes("FLAGS1", 2);
				p.i32("UNK904");
				p.i32("UNK910");
				p.u8("UNKB08");
				p.i32("UNK914");
				p.bytes("FLAGS2", 4);
				p.i32("UNK928");
				p.floats("POS", 3);
				p.i32("UNK900");
				p.i32("UNK938");
				p.u8("FLAG3");
				p.u8("UNKB0A");
				p.vsArray("TIMES", [&] {
					p.i32("ID");
					if (v < 0x4c) {
						p.i32("TICK");
					} else {
						p.bytes("TICK", 8);
					}
				});
				p.i32("UNK980");
				p.u8("FLAG4");
				p.i32("UNKAFC");
				p.i32("UNK8D4");
				p.i32("UNK8D8");
				p.bytes("FLAGS5", 2);
				p.ints("UNK988", 7);
				p.f32("UNK6E0");
				p.bytes("FLAGS6", 2);
				p.i32("UNK7D8");
				p.i32("UNK8DC");
				p.ints("UNK950", 7);
				p.group("SKILLS", [&] {
					p.out().playerXPAt = p.r.pos();
					p.f32("XP");
					p.f32("XP FOR NEXT LEVEL");
					const auto n = static_cast<std::uint32_t>(p.r.i32());
					const auto total = std::max<std::uint32_t>(n, 18);
					p.add("COUNT", std::to_string(n));
					p.countedArray("SKILLS", total, [&] {
						p.out().playerSkillsAt.push_back(p.r.pos());
						p.f32("LEVEL");
						p.f32("XP");
						p.f32("XP FOR NEXT LEVEL");
						if (v > 0x49) {
							p.i32("LEGENDARY");
						}
					});
				});
				p.floats("UNK498", 3);
				p.f32("UNKAD8");
				p.vsArray("POSITIONS", [&] {
					p.floats("POS", 3);
					p.f32("TIME");
				});
				p.ref("REF940");
				p.ref("REF");
				p.ref("UNK766D20_1");
				p.ref("UNK766D20_2");
				p.ref("REF6B8");
				p.ref("REF6C8");
				p.ref("REF8D0");
				p.ref("REF9C0");
				{
					const auto n = p.vs("COUNT");
					p.countedArray("TELEPORT", n, [&] {
						const auto idx = static_cast<std::uint32_t>(p.i32("INDEX"));
						if (idx < 15) {
							p.i32("VALUE");
							p.ref("REF");
						}
					});
				}
				p.vsArray("QUEST OBJECTIVES", [&] {
					p.ref("QUEST");
					p.i16("OBJECTIVE");
					p.u8("TARGET");
				});
				p.vsArray("QUEST TARGETS", [&] {
					p.ref("QUEST");
					p.i32("UNK");
					p.i32("UNK2");
					p.i32("UNK3");
				});
				p.i32("UNK");
				p.i32("UNK550E80_1");
				p.i32("UNK550E80_2");
				p.i32("UNKCD5750_1");
				p.i32("UNKCD5750_2");
				PlayerCamera(p);
				p.vsArray("CRIME GOLD", [&] {  // per faction: bounties and crime totals
					const auto faction = p.refResolved("FACTION");
					p.out().crimeGold.push_back({ faction, p.r.pos() });
					p.i32("VIOLENT BOUNTY");
					p.i32("NONVIOLENT BOUNTY");
					p.i32("NONVIOLENT TOTAL");
					p.i32("VIOLENT TOTAL");
				});
				p.ref("REF89C");
				p.i32("UNKAF0");
				p.i32("UNKAF4");
				p.ref("REF9F8");
				p.i32("UNKADC");
				p.vsArray("STOLEN ITEM VALUES", [&] {
					p.ref("FACTION");
					p.ints("VALUES", 2);
				});
				p.vsArray("LIST1", [&] {
					p.ref("REF");
					p.u8("FLAG");
				});
				p.vsArray("LIST2", [&] { p.ref("REF"); });
				p.vsArray("LIST3", [&] {
					p.ref("REF");
					p.i32("VALUE");
				});
				p.u8("FLAG7");
				if (static_cast<std::int8_t>(p.u8("UNK_I8")) != -1) {
					p.i16("UNK_I16");
				}
				p.ref("REF630");
				p.floats("UNK638", 3);
				p.i32("UNKAE0");
				p.ref("REF91C");
				p.ref("REF448");
				p.ref("REF920");
				PlayerSkin(p);
				p.i32("UNK728");
				p.ref("REF720");
				p.vsArray("CRIMES3", [&] {
					p.ref("FACTION");
					for (int i = 0; i < 4; ++i) {
						p.i16(Fmt("VALUE%d", i));
					}
				});
				p.bytes("FLAGS8", 3);
				if (v > 6) {
					p.bytes("FLAGS9", 3);
				}
				p.vsArray("UNK5B2BB0", [&] { p.i32("VALUE"); });
				p.ref("REFA00");
				p.ref("REFA08");
				p.out().playerPerkPointsAt = p.r.pos();
				p.u8("PERK POINTS");
				p.vsArray("LIST4", [&] {
					p.ref("REF");
					p.ints("VALUES", 2);
				});
				if (v > 0xd) {
					p.i32("UNK90C");
				}
				if (v > 0x11) {
					p.bytes("FLAGS10", 3);
				}
				if (v > 0x3a) {
					p.i32("UNKAE4");
					p.ref("REFAE8");
					p.u8("FLAG11");
				}
				if (v > 0x3c && p.u8("HAS UNKBD8")) {
					p.group("UNKBD8", [&] {
						for (int i = 0; i < 4; ++i) {
							p.ref(Fmt("REF%d", i));
						}
						p.ref("REF28");
						if (v > 0x3e) {
							p.ref("REF30");
							p.ref("REF38");
						}
						if (v > 0x3f) {
							p.ref("REF20");
						}
					});
				}
				if (v < 0x48) {
					if (v > 0x43) {
						p.u8("UNK19B7D4");
					}
				} else {
					p.i32("UNK19B7D4");
				}
				if (v > 0x45) {
					p.u8("UNK19B7D1");
				}
				if (v > 0x46) {
					p.u8("UNK2013D80");
					p.u8("UNK19B7D2");
					p.i32("UNK19B7D8");
				}
				if (v > 0x4c) {
					p.u8("FLAG12");
				}
			});
		}

		void Achr(P& p, const RefID& id, std::uint32_t flags, std::uint8_t a_version)
		{
			const int t = InitialType(id, flags);
			if (t == 4 && !Bit(flags, 2)) {
				p.section(1, [&] { Initial(p, t); });  // the moved position belongs to REFR_MOVE alone
			} else {
				Initial(p, t);
			}
			Havok(p, flags);
			p.out().levelAt = p.r.pos();
			const auto level = p.i32("PROCESS LEVEL");
			p.out().processLevel = level;
			p.flags32("BOOL BITS");
			if (Bit(flags, 0)) {
				p.changeFormFlags();
			}
			if (Bit(flags, 7)) {
				p.out().baseObject = p.refResolved("BASE_OBJECT");
			}
			if (Bit(flags, 4)) {
				p.section(4, [&] { p.f32("SCALE"); });
			}
			for (int b : { 6, 25, 9, 11, 17, 26, 29, 30, 31, 18 }) {
				if (Bit(flags, b)) {
					TopExtraData(p);
					break;
				}
			}
			if (Bit(flags, 5) && !Bit(flags, 27)) {
				p.section(5, [&] { Inventory(p); });  // the inventory belongs to REFR_INVENTORY alone
			} else if (Bit(flags, 5) || Bit(flags, 27)) {
				Inventory(p);
			}
			if (Bit(flags, 28)) {
				p.section(28, [&] { p.bytesVS("ANIMATIONS"); });
			}
			ActorData(p, a_version, level);
			if (id.data == 0x400014) {  // the player: PlayerCharacter's own data follows
				PlayerData(p);
			}
		}

		void NPC(P& p, std::uint32_t flags, bool)
		{
			if (Bit(flags, 0)) {
				p.section(0, [&] { p.changeFormFlags(); });
			}
			if (Bit(flags, 1)) {
				p.section(1, [&] { p.bytes("ACBS", 24); });
			}
			if (Bit(flags, 6)) {
				p.section(6, [&] {
					p.vsArray("FACTIONRANKS", [&] {
						p.ref("FACTION");
						p.u8("RANK");
					});
				});
			}
			if (Bit(flags, 4)) {
				p.section(4, [&] {
					p.vsArray("SPELLS", [&] { p.ref("SPELL"); });
					p.vsArray("SPELLS_LEVELLED", [&] { p.ref("SPELL"); });
					p.vsArray("SHOUTS", [&] { p.ref("SHOUT"); });
				});
			}
			if (Bit(flags, 3)) {
				p.section(3, [&] { p.bytes("AIDT", 20); });
			}
			if (Bit(flags, 5)) {
				p.section(5, [&] { p.wstring("FULLNAME"); });
			}
			if (Bit(flags, 9)) {
				p.section(9, [&] { p.bytes("DNAM", 52); });
			}
			if (Bit(flags, 10)) {
				p.section(10, [&] { p.ref("CLASS"); });
			}
			if (Bit(flags, 25)) {
				p.section(25, [&] {
					p.ref("RACE");
					p.ref("OLDRACE");
				});
			}
			if (Bit(flags, 11)) {
				p.section(11, [&] {
					p.group("FACEDATA", [&] {
						if (p.u8("FACEPRESENT") != 0) {
							p.ref("HAIRCOLOR");
							p.i32("SKINTONE");
							p.ref("SKIN");
							p.vsArray("HEADPARTS", [&] { p.ref("PART"); });
							if (p.u8("FACEDATAPRESENT") != 0) {
								const auto m = p.i32("MORPHS_COUNT");
								p.floats("MORPHS", static_cast<std::size_t>(m));
								const auto pr = p.i32("PRESETS_COUNT");
								p.ints("PRESETS", static_cast<std::size_t>(pr));
							}
						}
					});
				});
			}
			if (Bit(flags, 24)) {
				p.section(24, [&] { p.u8("GENDER"); });
			}
			if (Bit(flags, 12)) {
				p.section(12, [&] { p.ref("DEFAULT_OUTFIT"); });
			}
			if (Bit(flags, 13)) {
				p.section(13, [&] { p.ref("SLEEP_OUTFIT"); });
			}
		}

		void Qust(P& p, std::uint32_t flags)
		{
			if (Bit(flags, 0)) {
				p.changeFormFlags();
			}
			if (Bit(flags, 1)) {
				p.flags16("QUEST_FLAGS");
			}
			if (Bit(flags, 2)) {
				p.f32("SCRIPT_DELAY");
			}
			if (Bit(flags, 31)) {
				p.vsArray("QUEST_STAGES", [&] {
					p.i16("STAGE");
					p.flags8("STATUS");
				});
			}
			if (Bit(flags, 29)) {
				p.vsArray("QUEST_OBJECTIVES", [&] {
					p.i32("UNK1");
					p.i32("UNK2");
				});
			}
			if (Bit(flags, 28)) {
				p.group("QUEST_RUN_DATA", [&] {
					p.u8("UNK");
					const auto c1 = static_cast<std::uint32_t>(p.i32("COUNT1"));
					p.countedArray("ITEMS1", c1, [&] {
						p.i32("UNK");
						const auto f = p.flags8("FLAG");
						for (int i = 0; i < (f == 0 ? 1 : 5); ++i) {
							p.ref(Fmt("REF%d", i));
						}
					});
					const auto c2 = static_cast<std::uint32_t>(p.i32("COUNT2"));
					p.countedArray("ITEMS2", c2, [&] {
						p.i32("UNK");
						p.ref("REF");
					});
					if (p.flags8("FLAG") != 0) {
						p.group("ITEM3", [&] {
							p.i32("UNK1");
							p.f32("UNK2");
							const auto n = static_cast<std::uint32_t>(p.i32("COUNT"));
							p.countedArray("ITEMS", n, [&] {
								const auto t = p.i32("TYPE");
								if (t == 0 || t == 1 || t == 2 || t == 4) {
									p.ref("UNK_REF");
								} else if (t == 3) {
									p.i32("UNK_INT");
								} else {
									throw Stop(Fmt("意外的任务运行数据类型 %d", t));
								}
							});
						});
					}
				});
			}
			if (Bit(flags, 27)) {
				// CHANGE_QUEST_INSTANCES (not read by ReSaver); layout worked out from Skyrim LE saves
				p.group("QUEST_INSTANCES", [&] {
				p.i32("UNK");
				p.vsArray("INSTANCES", [&] {
					p.i32("ID");
					p.vsArray("ITEMS1", [&] {
						p.i32("UNK");
						p.ref("REF");
					});
					p.vsArray("ITEMS2", [&] {
						p.ref("REF");
						p.f32("VALUE");
					});
					p.i16("UNK2");
					p.u8("UNK3");
				});
				});
			}
			if (Bit(flags, 26)) {
				p.u8("ALREADY_RUN");
			}
		}

		void Leveled(P& p, std::uint32_t flags)
		{
			if (Bit(flags, 0)) {
				p.changeFormFlags();
			}
			if (Bit(flags, 31)) {
				const auto n = p.r.u8();
				p.countedArray("ENTRIES", n, [&] {
					p.ref("REF");
					p.u8("LEVEL");
					p.i16("COUNT");
					p.u8("CHANCE");
				});
			}
		}

		void Rela(P& p, const RefID& id, std::uint32_t flags)
		{
			if (Bit(flags, 0)) {
				p.changeFormFlags();
			}
			if (id.type() == RefID::Type::kCreated) {
				p.ref("PERSON1");
				p.ref("PERSON2");
				p.ref("ASSOCIATION");
			}
			if (Bit(flags, 1)) {
				p.i32("RANK");
			}
		}

		// FLST: ReSaver reads the CHANGE_FORM_FLAGS block when bit 1 is set; the game's own constant
		// is bit 0. Both are tried, and only an interpretation that uses every byte is accepted.
		bool Flst(P& p, std::uint32_t flags, int formFlagsBit)
		{
			const auto start = p.r.pos();
			const auto mark = p.mark();
			try {
				if (Bit(flags, formFlagsBit)) {
					p.changeFormFlags();
				}
				p.out().listRaw.clear();
				if (Bit(flags, 31)) {
					const auto n = p.r.u32();
					if (n > 0x3FFF) {
						throw Stop("FormList 大小不合理");
					}
					p.countedArray("FORMS", n, [&] { p.out().listRaw.push_back(p.ref("FORM")); });
				}
				if (p.r.remaining() == 0) {
					return true;
				}
			} catch (const std::exception&) {
			}
			p.r.seek(start);
			p.rollback(mark);
			return false;
		}

		const char* kRefrFlags[32] = { "CHANGE_FORM_FLAGS", "REFR_MOVE", "REFR_HAVOK_MOVE", "REFR_CELL_CHANGED", "REFR_SCALE", "REFR_INVENTORY",
			"REFR_EXTRA_OWNERSHIP", "REFR_BASEOBJECT", nullptr, nullptr, "OBJECT_EXTRA_ITEM_DATA", "OBJECT_EXTRA_AMMO", "OBJECT_EXTRA_LOCK", nullptr,
			nullptr, nullptr, nullptr, "DOOR_EXTRA_TELEPORT", nullptr, nullptr, nullptr, "OBJECT_EMPTY", "OBJECT_OPEN_DEFAULT_STATE", "OBJECT_OPEN_STATE",
			nullptr, "REFR_PROMOTED", "REFR_EXTRA_ACTIVATING_CHILDREN", "REFR_LEVELED_INVENTORY", "REFR_ANIMATION", "REFR_EXTRA_ENCOUNTER_ZONE",
			"REFR_EXTRA_CREATED_ONLY", "REFR_EXTRA_GAME_ONLY" };
		const char* kAchrFlags[32] = { "CHANGE_FORM_FLAGS", "REFR_MOVE", "REFR_HAVOK_MOVE", "REFR_CELL_CHANGED", "REFR_SCALE", "REFR_INVENTORY",
			"REFR_EXTRA_OWNERSHIP", "REFR_BASEOBJECT", nullptr, "ACTOR_EXTRA_PACKAGE_DATA", "LIFESTATE", "ACTOR_EXTRA_MERCHANT_CONTAINER", nullptr,
			nullptr, nullptr, nullptr, nullptr, "ACTOR_EXTRA_DISMEMBERED_LIMBS", "ACTOR_LEVELED_ACTOR", "ACTOR_DISPOSITION_MODIFIERS",
			"ACTOR_TEMP_MODIFIERS", "ACTOR_DAMAGE_MODIFIERS", "ACTOR_OVERRIDE_MODIFIERS", "ACTOR_PERMANENT_MODIFIERS", nullptr, "REFR_PROMOTED",
			"REFR_EXTRA_ACTIVATING_CHILDREN", "REFR_LEVELED_INVENTORY", "REFR_ANIMATION", "REFR_EXTRA_ENCOUNTER_ZONE", "REFR_EXTRA_CREATED_ONLY",
			"REFR_EXTRA_GAME_ONLY" };
		const char* kNpcFlags[32] = { "CHANGE_FORM_FLAGS", "ACTOR_BASE_DATA", nullptr, "ACTOR_BASE_AIDATA", "ACTOR_BASE_SPELLLIST",
			"ACTOR_BASE_FULLNAME", "ACTOR_BASE_FACTIONS", nullptr, nullptr, "NPC_SKILLS", "NPC_CLASS", "NPC_FACE", "DEFAULT_OUTFIT", "SLEEP_OUTFIT",
			nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "NPC_GENDER", "NPC_RACE" };
		const char* kQustFlags[32] = { "CHANGE_FORM_FLAGS", "QUEST_FLAGS", "QUEST_SCRIPT_DELAY", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
			nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
			nullptr, "QUEST_ALREADY_RUN", "QUEST_INSTANCES", "QUEST_RUNDATA", "QUEST_OBJECTIVES", "QUEST_SCRIPT", "QUEST_STAGES" };
	}

	std::vector<std::uint8_t> Body(const ChangeForm& a_cf)
	{
		if (a_cf.length2 > 0) {
			return DecompressZlib(a_cf.data.data(), a_cf.data.size(), a_cf.length2);
		}
		return a_cf.data;
	}

	void SetBody(ChangeForm& a_cf, const std::vector<std::uint8_t>& a_body)
	{
		if (a_cf.length2 > 0) {
			a_cf.data = CompressZlib(a_body);
			a_cf.length2 = static_cast<std::uint32_t>(a_body.size());
		} else {
			a_cf.data = a_body;
		}
		a_cf.length1 = static_cast<std::uint32_t>(a_cf.data.size());
		// widen the length fields if the new sizes need it (never narrows: keeps the original layout)
		const auto biggest = std::max<std::uint32_t>(a_cf.length1, a_cf.length2);
		const int  need = biggest > 0xFFFF ? 2 : biggest > 0xFF ? 1 : 0;
		if (need > (a_cf.typeField >> 6)) {
			a_cf.typeField = static_cast<std::uint8_t>((a_cf.typeField & 0x3F) | (need << 6));
		}
	}

	Decoded Decode(const SaveFile& a_save, const ChangeForm& a_cf, const TypeLookup& a_types)
	{
		Decoded out;
		out.root.label = ChangeFormTypeName(a_cf.typeCode());
		std::vector<std::uint8_t> body;
		try {
			body = Body(a_cf);
		} catch (const std::exception& e) {
			out.error = std::string("无法解压：") + e.what();
			return out;
		}
		out.bodySize = body.size();
		P          p(a_save, body, a_types, out);
		p.version = a_cf.version;
		const auto flags = a_cf.changeFlags;
		const auto t = static_cast<ChangeFormType>(a_cf.typeCode());
		out.supported = true;
		try {
			switch (t) {
			case ChangeFormType::REFR:
				Refr(p, a_cf.refID, flags);
				break;
			case ChangeFormType::ACHR:
				Achr(p, a_cf.refID, flags, a_cf.version);
				break;
			case ChangeFormType::NPC_:
				NPC(p, flags, false);
				break;
			case ChangeFormType::QUST:
				Qust(p, flags);
				break;
			case ChangeFormType::LVLN:
			case ChangeFormType::LVLI:
				Leveled(p, flags);
				break;
			case ChangeFormType::RELA:
				Rela(p, a_cf.refID, flags);
				break;
			case ChangeFormType::FLST:
				out.isFormList = true;
				if (!Flst(p, flags, 0) && !Flst(p, flags, 1)) {
					out.root.kids.clear();
					throw Stop("无法识别的 FormList 布局");
				}
				break;
			default:
				out.supported = false;
				break;
			}
		} catch (const std::exception& e) {
			out.error = e.what();
		}
		out.consumed = p.r.pos();
		if (out.supported && out.error.empty() && p.r.remaining()) {
			out.error = Fmt("末尾有 %zu 字节未被解码", p.r.remaining());
		}
		if (p.r.remaining()) {
			const auto rest = p.r.remaining();
			p.bytes(out.supported ? "NOT DECODED" : "DATA", rest);
		}
		out.complete = out.supported && out.error.empty();

		// filter facts
		for (const auto& k : out.root.kids) {
			if (k.label == "CHANGE_FORM_FLAGS") {
				for (const auto& f : k.kids) {
					if (f.label == "FLAGS") {
						out.hasFormFlags = true;
						out.formFlags = static_cast<std::uint32_t>(std::strtoul(f.value.c_str(), nullptr, 16));
					}
				}
			}
		}
		if (t == ChangeFormType::REFR || t == ChangeFormType::ACHR) {
			if (Bit(flags, 1) || Bit(flags, 3)) {
				for (const auto& k : out.root.kids) {
					if (k.label == "INITIAL") {
						for (const auto& f : k.kids) {
							if (f.label == "CELL" && f.isRef && f.refRaw &&
								(f.formID == 0xFFFFFFFFu || RefID{ f.refRaw }.type() == RefID::Type::kInvalid)) {
								out.inCellMinus1 = true;
							}
						}
					}
				}
			}
			if ((flags & 0xFF) == 0 && out.complete) {
				const auto& kids = out.root.kids;
				auto        emptyInitial = [](const Node& n) { return n.label == "INITIAL" && n.kids.empty(); };
				if (kids.empty() || (kids.size() == 1 && emptyInitial(kids[0])) ||
					(kids.size() == 2 && emptyInitial(kids[0]) && kids[1].label == "EXTRADATA (0)")) {
					out.isEmptyRef = true;
				}
			}
		}
		return out;
	}

	namespace
	{
		std::string LowerS(std::string s)
		{
			for (auto& c : s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return s;
		}
		std::string BaseLabel(const std::string& l)
		{
			auto s = l;
			if (auto p = s.find(" ("); p != std::string::npos) {
				s = s.substr(0, p);
			}
			return LowerS(s);
		}
		bool ValueEquals(const Node& n, const std::string& want)
		{
			const auto w = LowerS(want);
			if (n.isRef) {
				const auto fid = LowerS(Fmt("%08X", n.formID));
				const auto raw = LowerS(Fmt("%06X", n.refRaw));
				return fid.rfind(w, 0) == 0 || raw == w || (w == "none" && n.refRaw == 0);
			}
			if (LowerS(n.value) == w) {
				return true;
			}
			char*       end = nullptr;
			const double a = std::strtod(n.value.c_str(), &end);
			if (end && *end == '\0' && !n.value.empty()) {
				char*       e2 = nullptr;
				const double b = std::strtod(want.c_str(), &e2);
				return e2 && *e2 == '\0' && a == b;
			}
			return false;
		}
		// path[i..] matches somewhere below node (array elements "[n]" are transparent)
		bool PathMatch(const Node& node, const std::vector<std::string>& path, std::size_t i)
		{
			if (i >= path.size()) {
				return true;
			}
			const bool last = i + 1 == path.size();
			for (const auto& k : node.kids) {
				if (!k.label.empty() && k.label[0] == '[') {
					if (PathMatch(k, path, i)) {
						return true;
					}
					continue;
				}
				if (BaseLabel(k.label) == LowerS(path[i])) {
					if (last) {
						return true;  // the field exists
					}
					if (i + 2 == path.size() && ValueEquals(k, path[i + 1])) {
						return true;  // field = value
					}
					if (PathMatch(k, path, i + 1)) {
						return true;
					}
				}
			}
			return false;
		}
		std::vector<std::string> Split(const std::string& s, char c)
		{
			std::vector<std::string> out;
			std::string              cur;
			for (char ch : s) {
				if (ch == c) {
					out.push_back(cur);
					cur.clear();
				} else {
					cur.push_back(ch);
				}
			}
			out.push_back(cur);
			return out;
		}
		std::string Trim(const std::string& s)
		{
			const auto a = s.find_first_not_of(" \t");
			if (a == std::string::npos) {
				return {};
			}
			return s.substr(a, s.find_last_not_of(" \t") - a + 1);
		}
	}

	bool ContentMatches(const Node& a_root, const std::string& a_expression)
	{
		if (Trim(a_expression).empty()) {
			return true;
		}
		for (const auto& term : Split(a_expression, ';')) {
			if (Trim(term).empty()) {
				continue;
			}
			bool any = false;
			for (auto clause : Split(term, ',')) {
				clause = Trim(clause);
				bool neg = false;
				if (!clause.empty() && clause[0] == '!') {
					neg = true;
					clause = Trim(clause.substr(1));
				}
				std::vector<std::string> path;
				for (const auto& seg : Split(clause, '/')) {
					if (!Trim(seg).empty()) {
						path.push_back(Trim(seg));
					}
				}
				const bool hit = !path.empty() && PathMatch(a_root, path, 0);
				if (hit != neg) {
					any = true;
					break;
				}
			}
			if (!any) {
				return false;
			}
		}
		return true;
	}

	std::vector<std::string> FlagNames(std::uint8_t a_type, std::uint32_t a_flags)
	{
		const char* const* names = nullptr;
		switch (static_cast<ChangeFormType>(a_type)) {
		case ChangeFormType::REFR:
			names = kRefrFlags;
			break;
		case ChangeFormType::ACHR:
			names = kAchrFlags;
			break;
		case ChangeFormType::NPC_:
			names = kNpcFlags;
			break;
		case ChangeFormType::QUST:
			names = kQustFlags;
			break;
		default:
			break;
		}
		std::vector<std::string> out;
		for (int b = 0; b < 32; ++b) {
			if (Bit(a_flags, b)) {
				if (names && names[b]) {
					out.push_back(Fmt("%d: %s", b, names[b]));
				} else if (b == 0) {
					out.push_back("0: CHANGE_FORM_FLAGS");
				} else if (b == 31 && (a_type == static_cast<std::uint8_t>(ChangeFormType::FLST) || a_type == static_cast<std::uint8_t>(ChangeFormType::LVLN) ||
										  a_type == static_cast<std::uint8_t>(ChangeFormType::LVLI))) {
					out.push_back("31: ADDED_FORMS");
				} else {
					out.push_back(Fmt("%d", b));
				}
			}
		}
		return out;
	}

	HavokResult ResetHavok(SaveFile& a_save, const TypeLookup& a_types, bool a_dryRun)
	{
		HavokResult r;
		for (auto& cf : a_save.changeForms) {
			if (static_cast<ChangeFormType>(cf.typeCode()) != ChangeFormType::REFR || !Bit(cf.changeFlags, 2)) {
				continue;
			}
			const auto d = Decode(a_save, cf, a_types);
			if (!d.complete || !d.hasHavok) {
				++r.failed;
				continue;
			}
			++r.reset;
			if (!a_dryRun && d.havokSize) {
				auto body = Body(cf);
				std::memset(body.data() + d.havokOffset, 0, d.havokSize);
				SetBody(cf, body);
			}
		}
		return r;
	}

	std::size_t NullEntries(const SaveFile& a_save, const ChangeForm& a_cf)
	{
		if (static_cast<ChangeFormType>(a_cf.typeCode()) != ChangeFormType::FLST) {
			return 0;
		}
		const auto  d = Decode(a_save, a_cf);
		std::size_t n = 0;
		for (auto v : d.listRaw) {
			n += v == 0;
		}
		return d.complete ? n : 0;
	}

	PurifyResult PurifyFormLists(SaveFile& a_save, bool a_dryRun, std::uint32_t a_onlyRefID)
	{
		PurifyResult r;
		for (auto& cf : a_save.changeForms) {
			if (static_cast<ChangeFormType>(cf.typeCode()) != ChangeFormType::FLST || !Bit(cf.changeFlags, 31)) {
				continue;
			}
			if (a_onlyRefID && cf.refID.data != a_onlyRefID) {
				continue;
			}
			const auto d = Decode(a_save, cf);
			if (!d.complete) {
				++r.failed;
				continue;
			}
			std::size_t nulls = 0;
			for (auto v : d.listRaw) {
				nulls += v == 0;
			}
			if (!nulls) {
				continue;
			}
			r.entries += nulls;
			++r.forms;
			if (a_dryRun) {
				continue;
			}
			// rebuild: everything before the list stays, then count + the non-null entries
			auto       body = Body(cf);
			const auto listStart = body.size() - 4 - 3 * d.listRaw.size();
			std::vector<std::uint8_t> nb(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(listStart));
			const auto                kept = static_cast<std::uint32_t>(d.listRaw.size() - nulls);
			for (int i = 0; i < 4; ++i) {
				nb.push_back(static_cast<std::uint8_t>(kept >> (8 * i)));
			}
			for (auto v : d.listRaw) {
				if (v) {
					nb.push_back(static_cast<std::uint8_t>(v >> 16));
					nb.push_back(static_cast<std::uint8_t>(v >> 8));
					nb.push_back(static_cast<std::uint8_t>(v));
				}
			}
			SetBody(cf, nb);
		}
		return r;
	}
}

namespace ess::cf
{
	std::vector<ChargedItem> FindChargedItems(const SaveFile& a_save)
	{
		std::vector<ChargedItem> out;
		for (const auto& c : a_save.changeForms) {
			const auto t = static_cast<ChangeFormType>(c.typeCode());
			if ((t != ChangeFormType::REFR && t != ChangeFormType::ACHR) || !((c.changeFlags >> 5) & 1 || (c.changeFlags >> 27) & 1)) {
				continue;  // no inventory in this change form
			}
			const auto d = Decode(a_save, c);
			for (const auto& e : d.charged) {
				out.push_back({ c.refID.data, c.refID.isZero() ? 0 : a_save.formIDOf(c.refID), e });
			}
		}
		return out;
	}

	bool NeedsChargeFix(const InvCharge& a_item)
	{
		return (a_item.hasEnchant && a_item.enchRaw != 0 && a_item.maxCharge == 0) ||
			   (a_item.hasEnchant && a_item.hasCharge && a_item.maxCharge > 0 && a_item.charge > a_item.maxCharge);
	}

	std::size_t FixCharges(SaveFile& a_save, std::uint16_t a_maxCharge, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& a_only)
	{
		if (a_maxCharge == 0) {
			return 0;
		}
		std::size_t fixed = 0;
		for (auto& c : a_save.changeForms) {
			const auto t = static_cast<ChangeFormType>(c.typeCode());
			if ((t != ChangeFormType::REFR && t != ChangeFormType::ACHR) || !((c.changeFlags >> 5) & 1 || (c.changeFlags >> 27) & 1)) {
				continue;
			}
			const auto d = Decode(a_save, c);
			bool       any = false;
			auto       body = Body(c);
			for (const auto& e : d.charged) {
				if (!NeedsChargeFix(e)) {
					continue;
				}
				if (!a_only.empty() && std::find(a_only.begin(), a_only.end(), std::make_pair(c.refID.data, e.item)) == a_only.end()) {
					continue;
				}
				std::uint16_t maxc = e.maxCharge;
				if (maxc == 0 && e.maxChargeOffset + 2 <= body.size()) {
					maxc = a_maxCharge;
					body[e.maxChargeOffset] = static_cast<std::uint8_t>(maxc & 0xFF);
					body[e.maxChargeOffset + 1] = static_cast<std::uint8_t>(maxc >> 8);
				}
				if (e.hasCharge && e.charge > maxc && e.chargeOffset + 4 <= body.size()) {
					const float f = static_cast<float>(maxc);
					std::uint32_t bits;
					std::memcpy(&bits, &f, 4);
					for (int i = 0; i < 4; ++i) {
						body[e.chargeOffset + i] = static_cast<std::uint8_t>(bits >> (8 * i));
					}
				}
				++fixed;
				any = true;
			}
			if (any) {
				SetBody(c, body);
			}
		}
		return fixed;
	}

	namespace
	{
		struct RemovableExtra
		{
			const char* name;
			const char* effect;
		};
		// extra data an actor can lose safely: state the game rebuilds, or falls back to the plugin for
		const RemovableExtra kRemovableExtras[] = {
			{ "Package", "角色当前的 AI 包（以及它的包数据）会被丢弃，读档时它的 AI 会重新挑选一个包。适合卡在某个包上的 NPC。" },
			{ "PackageData", "角色当前的包数据（以及它属于的那个包）会被丢弃，读档时它的 AI 会重新挑选一个包。" },
			{ "PackageStartLocation", "角色当前包的起始位置会被丢弃，改用包现在的位置。" },
			{ "RunOncePacks", "角色已经跑过哪些一次性包的记录会被丢弃，于是它们可以再次运行。" },
			{ "SceneData", "角色会从它所在的场景里被拿出来。适合卡在场景里的 NPC；运行这个场景的任务可能需要重新启动它。" },
			{ "Ownership", "角色保存的拥有者会被丢弃，重新使用插件里的拥有者（或者没有拥有者）。" },
			{ "EncounterZone", "角色保存的遭遇区域会被丢弃，重新使用它的单元格或插件里的那个。" },
			{ "FactionChanges", "角色在游戏里做出的所有派系改动都会被丢弃：它加入或退出的派系、以及阶位改动都会回到插件的状态。注意这包括随从和婚姻派系。" },
			{ "DismemberedLimbs", "角色失去的肢体被复原。" },
			{ "AttachedArrows3D", "插在角色身上的箭被移除。" },
			{ "CachedScale", "角色缓存的体型会被丢弃，游戏会重新计算。" },
			{ "CanTalkToPlayer", "保存的“能否和玩家说话”设置会被丢弃，重新使用插件的设置。" },
			{ "FollowerSwimBreadcrumbs", "角色在水中跟随领队时留下的路径会被丢弃，它会重新开始一条。" },
			{ "ForcedLandingMarker", "角色（通常是龙）被强制使用的着陆点标记会被丢弃。" },
			{ "ForcedTarget", "角色被强制攻击的目标会被丢弃。" },
			{ "HeadingTarget", "角色被强制转向的目标会被丢弃。" },
			{ "FriendHits", "玩家击中这个友善角色的次数会被丢弃，它不会再因此即将变成敌对。" },
			{ "Ghost", "角色在游戏里设置的幽灵状态会被丢弃；除非插件把它设成幽灵，否则它又会变成实体。" },
			{ "GuardedRefData", "角色记忆中看守着的东西会被丢弃。" },
			{ "PlayerCrimeList", "角色记得的、玩家对它犯下的罪行会被丢弃。" },
			{ "TextDisplayData", "角色在游戏里设置的显示名会被丢弃，重新显示插件的名字。" },
		};

		const RemovableExtra* FindRemovable(const std::string& a_name)
		{
			for (const auto& r : kRemovableExtras) {
				if (a_name == r.name) {
					return &r;
				}
			}
			return nullptr;
		}

		void PutVsval(std::vector<std::uint8_t>& a_out, std::uint32_t a_v)
		{
			if (a_v < 0x40) {
				a_out.push_back(static_cast<std::uint8_t>(a_v << 2));
			} else if (a_v < 0x4000) {
				const std::uint32_t v = (a_v << 2) | 1;
				a_out.push_back(static_cast<std::uint8_t>(v));
				a_out.push_back(static_cast<std::uint8_t>(v >> 8));
			} else {
				const std::uint32_t v = (a_v << 2) | 2;
				for (int i = 0; i < 4; ++i) {
					a_out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
				}
			}
		}

		bool IsPlayerActor(const ChangeForm& a_cf)
		{
			return static_cast<ChangeFormType>(a_cf.typeCode()) == ChangeFormType::ACHR && a_cf.refID.data == 0x400014;
		}
	}

	// ---- the player's character ----
	const char* const kSkillNames[18] = { "One-handed", "Two-handed", "Archery", "Block", "Smithing", "Heavy Armor", "Light Armor",
		"Pickpocket", "Lockpicking", "Sneak", "Alchemy", "Speech", "Alteration", "Conjuration", "Destruction", "Illusion", "Restoration",
		"Enchanting" };

	namespace
	{
		ChangeForm* PlayerForm(SaveFile& a_save)
		{
			RefID id;
			id.data = 0x400014;
			return a_save.findChangeForm(id);
		}
		const ChangeForm* PlayerForm(const SaveFile& a_save)
		{
			for (const auto& c : a_save.changeForms) {
				if (c.refID.data == 0x400014 && static_cast<ChangeFormType>(c.typeCode()) == ChangeFormType::ACHR) {
					return &c;
				}
			}
			return nullptr;
		}

		std::int32_t GetI32(const std::vector<std::uint8_t>& a_b, std::size_t a_at)
		{
			std::uint32_t v = 0;
			for (int i = 0; i < 4; ++i) {
				v |= std::uint32_t(a_b[a_at + i]) << (8 * i);
			}
			return static_cast<std::int32_t>(v);
		}
		void PutI32(std::vector<std::uint8_t>& a_b, std::size_t a_at, std::int32_t a_v)
		{
			const auto v = static_cast<std::uint32_t>(a_v);
			for (int i = 0; i < 4; ++i) {
				a_b[a_at + i] = static_cast<std::uint8_t>(v >> (8 * i));
			}
		}
		float GetF32(const std::vector<std::uint8_t>& a_b, std::size_t a_at)
		{
			const auto v = static_cast<std::uint32_t>(GetI32(a_b, a_at));
			float      f;
			std::memcpy(&f, &v, 4);
			return f;
		}
		void PutF32(std::vector<std::uint8_t>& a_b, std::size_t a_at, float a_f)
		{
			std::uint32_t v;
			std::memcpy(&v, &a_f, 4);
			PutI32(a_b, a_at, static_cast<std::int32_t>(v));
		}

		// decodes the player, lets a_edit change the body, writes it back only if it still decodes completely
		template <class F>
		std::string EditPlayer(SaveFile& a_save, F a_edit)
		{
			auto* cf = PlayerForm(a_save);
			if (!cf) {
				return "这个存档里没有玩家的变更表单";
			}
			const auto d = Decode(a_save, *cf);
			if (!d.complete) {
				return "玩家的记录无法完整读取（" + d.error + "）：未做任何改动";
			}
			auto body = Body(*cf);
			if (auto why = a_edit(d, body); !why.empty()) {
				return why;
			}
			const auto before = *cf;
			SetBody(*cf, body);
			if (!Decode(a_save, *cf).complete) {
				*cf = before;
				return "这样的结果无法正确读回：未做任何改动";
			}
			return {};
		}

		constexpr std::uint32_t kGold = 0x0000000F;  // Gold001

		// the player's base form (NPC_ 00000007): its ACBS holds the character level
		const ChangeForm* PlayerBase(const SaveFile& a_save)
		{
			for (const auto& c : a_save.changeForms) {
				if (c.refID.data == 0x400007 && static_cast<ChangeFormType>(c.typeCode()) == ChangeFormType::NPC_) {
					return &c;
				}
			}
			return nullptr;
		}

		// replaces the vsval at a_at with a_count (its width may change)
		void RewriteVs(std::vector<std::uint8_t>& a_body, std::size_t a_at, std::uint32_t a_count)
		{
			const auto                b0 = a_body[a_at];
			const std::size_t         len = (b0 & 3) == 0 ? 1 : (b0 & 3) == 1 ? 2 : 4;
			std::vector<std::uint8_t> enc;
			PutVsval(enc, a_count);
			a_body.erase(a_body.begin() + static_cast<std::ptrdiff_t>(a_at), a_body.begin() + static_cast<std::ptrdiff_t>(a_at + len));
			a_body.insert(a_body.begin() + static_cast<std::ptrdiff_t>(a_at), enc.begin(), enc.end());
		}
		std::uint32_t GetVs(const std::vector<std::uint8_t>& a_body, std::size_t a_at)
		{
			const auto b0 = a_body[a_at];
			std::uint32_t v = b0;
			if ((b0 & 3) == 1) {
				v |= std::uint32_t(a_body[a_at + 1]) << 8;
			} else if ((b0 & 3) == 2) {
				v |= std::uint32_t(a_body[a_at + 1]) << 8 | std::uint32_t(a_body[a_at + 2]) << 16 | std::uint32_t(a_body[a_at + 3]) << 24;
			}
			return v >> 2;
		}
		void Erase(std::vector<std::uint8_t>& a_body, std::size_t a_from, std::size_t a_to)
		{
			a_body.erase(a_body.begin() + static_cast<std::ptrdiff_t>(a_from), a_body.begin() + static_cast<std::ptrdiff_t>(a_to));
		}
	}

	PlayerValues ReadPlayer(const SaveFile& a_save)
	{
		PlayerValues out;
		const auto*  cf = PlayerForm(a_save);
		if (!cf) {
			out.error = "这个存档里没有玩家的变更表单";
			return out;
		}
		const auto d = Decode(a_save, *cf);
		if (!d.complete) {
			out.error = "玩家的记录无法完整读取（" + d.error + "）";
			return out;
		}
		const auto body = Body(*cf);
		for (const auto& e : d.inventory) {
			if (e.item == kGold) {
				out.hasGold = true;
				out.gold += GetI32(body, e.countAt);
			}
		}
		for (const auto& v : d.baseAVs) {
			if (v.id >= 6 && v.id < 24) {
				out.skills[v.id - 6] = GetF32(body, v.at);
				out.hasSkill[v.id - 6] = true;
			}
		}
		for (std::size_t i = 0; i < d.playerSkillsAt.size() && i < 18; ++i) {
			if (cf->version > 0x49) {
				out.legendary[i] = GetI32(body, d.playerSkillsAt[i] + 12);
				out.hasLegendary = true;
			}
		}
		if (d.playerXPAt) {
			out.xp = GetF32(body, d.playerXPAt);
			out.xpNext = GetF32(body, d.playerXPAt + 4);
		}
		if (d.playerPerkPointsAt) {
			out.perkPoints = body[d.playerPerkPointsAt];
		}
		if (const auto* npc = PlayerBase(a_save)) {
			const auto nd = Decode(a_save, *npc);
			if (const auto it = nd.sections.find(1); nd.complete && it != nd.sections.end()) {
				const auto nb = Body(*npc);
				out.level = nb[it->second.first + 8] | (nb[it->second.first + 9] << 8);
				out.hasLevel = true;
			}
		}
		{
			std::map<std::uint32_t, PlayerAV> avs;
			for (const auto& v : d.baseAVs) {
				auto& a = avs[v.id];
				a.id = v.id;
				a.hasBase = true;
				a.base = GetF32(body, v.at);
			}
			for (const auto& m : d.avMods) {
				auto& a = avs[m.id];
				a.id = m.id;
				a.hasMod = true;
				a.permanent = GetF32(body, m.at);
				a.temporary = GetF32(body, m.at + 4);
				a.damage = GetF32(body, m.at + 8);
			}
			for (const auto& [id, a] : avs) {
				out.avs.push_back(a);
			}
		}
		for (const auto& e : d.inventory) {
			const bool extra = e.endAt > e.countAt + 4 && body[e.countAt + 4] != 0;
			bool       merged = false;
			for (auto& it : out.items) {
				if (it.item == e.item) {
					it.count += GetI32(body, e.countAt);
					it.hasExtra = it.hasExtra || extra;
					merged = true;
				}
			}
			if (!merged) {
				out.items.push_back({ e.item, GetI32(body, e.countAt), extra });
			}
		}
		for (const auto& sp : d.addedSpells) {
			out.spells.push_back(sp.id);
		}
		for (const auto& c : d.crimeGold) {
			PlayerBounty b;
			b.faction = c.id;
			b.violent = GetI32(body, c.at);
			b.nonViolent = GetI32(body, c.at + 4);
			b.nonViolentTotal = GetI32(body, c.at + 8);
			b.violentTotal = GetI32(body, c.at + 12);
			out.bounties.push_back(b);
		}
		return out;
	}

	std::string SetPlayerGold(SaveFile& a_save, std::int32_t a_gold)
	{
		if (a_gold < 0) {
			return "金币不能为负数";
		}
		return EditPlayer(a_save, [&](const Decoded& d, std::vector<std::uint8_t>& body) -> std::string {
			bool first = true;
			for (const auto& e : d.inventory) {
				if (e.item == kGold) {
					PutI32(body, e.countAt, first ? a_gold : 0);  // one stack holds it all
					first = false;
				}
			}
			if (!first) {
				return {};
			}
			if (!Bit(PlayerForm(a_save)->changeFlags, 5)) {
				return "玩家的存档里没有物品栏，无法添加金币";
			}
			// no gold yet: add an entry (Gold001, the count, no extra data) and raise the inventory's count
			std::size_t len = 0;
			const auto  n = static_cast<std::uint32_t>(d.inventory.size());
			const auto  b0 = body[d.inventoryCountAt];
			len = (b0 & 3) == 0 ? 1 : (b0 & 3) == 1 ? 2 : 4;
			std::vector<std::uint8_t> count;
			PutVsval(count, n + 1);
			std::vector<std::uint8_t> entry{ 0x40, 0x00, 0x0F, 0, 0, 0, 0, 0 };  // RefID 40000F = Gold001
			PutI32(entry, 3, a_gold);
			const auto end = d.inventoryCountAt + len;
			body.insert(body.begin() + static_cast<std::ptrdiff_t>(end), entry.begin(), entry.end());
			body.erase(body.begin() + static_cast<std::ptrdiff_t>(d.inventoryCountAt), body.begin() + static_cast<std::ptrdiff_t>(end));
			body.insert(body.begin() + static_cast<std::ptrdiff_t>(d.inventoryCountAt), count.begin(), count.end());
			return {};
		});
	}

	std::string SetPlayerSkill(SaveFile& a_save, int a_skill, float a_level)
	{
		if (a_skill < 0 || a_skill >= 18) {
			return "没有这个技能";
		}
		if (!(a_level >= 0.0f && a_level <= 1000.0f)) {
			return "技能等级必须在 0 到 1000 之间";
		}
		return EditPlayer(a_save, [&](const Decoded& d, std::vector<std::uint8_t>& body) -> std::string {
			bool found = false;
			for (const auto& v : d.baseAVs) {
				if (v.id == static_cast<std::uint32_t>(a_skill + 6)) {
					PutF32(body, v.at, a_level);
					found = true;
				}
			}
			if (!found) {
				return std::string("玩家保存的数值里没有 ") + SkillNameZh(a_skill) + " 这一项";
			}
			if (static_cast<std::size_t>(a_skill) < d.playerSkillsAt.size()) {
				const auto at = d.playerSkillsAt[a_skill];
				PutF32(body, at, a_level);  // the skill's level in the leveling data
				PutF32(body, at + 4, 0.0f); // and its progress toward the next level starts over
			}
			return {};
		});
	}

	const char* ActorValueName(std::uint32_t a_av)
	{
		static const char* const names[] = { "Aggression", "Confidence", "Energy", "Morality", "Mood", "Assistance", "OneHanded", "TwoHanded",
			"Archery", "Block", "Smithing", "HeavyArmor", "LightArmor", "Pickpocket", "Lockpicking", "Sneak", "Alchemy", "Speech", "Alteration",
			"Conjuration", "Destruction", "Illusion", "Restoration", "Enchanting", "Health", "Magicka", "Stamina", "HealRate", "MagickaRate",
			"StaminaRate", "SpeedMult", "InventoryWeight", "CarryWeight", "CriticalChance", "MeleeDamage", "UnarmedDamage", "Mass",
			"VoicePoints", "VoiceRate", "DamageResist", "PoisonResist", "ResistFire", "ResistShock", "ResistFrost", "ResistMagic",
			"ResistDisease", "PerceptionCondition", "EnduranceCondition", "LeftAttackCondition", "RightAttackCondition",
			"LeftMobilityCondition", "RightMobilityCondition", "BrainCondition", "Paralysis", "Invisibility", "NightEye", "DetectLifeRange",
			"WaterBreathing", "WaterWalking", "IgnoreCrippledLimbs", "Fame", "Infamy", "JumpingBonus", "WardPower", "RightItemCharge",
			"ArmorPerks", "ShieldPerks", "WardDeflection", "Variable01", "Variable02", "Variable03", "Variable04", "Variable05",
			"Variable06", "Variable07", "Variable08", "Variable09", "Variable10", "BowSpeedBonus", "FavorActive", "FavorsPerDay",
			"FavorsPerDayTimer", "LeftItemCharge", "AbsorbChance", "Blindness", "WeaponSpeedMult", "ShoutRecoveryMult", "BowStaggerBonus",
			"Telekinesis", "FavorPointsBonus", "LastBribedIntimidated", "LastFlattered", "MovementNoiseMult", "BypassVendorStolenCheck",
			"BypassVendorKeywordCheck", "WaitingForPlayer", "OneHandedModifier", "TwoHandedModifier", "MarksmanModifier", "BlockModifier",
			"SmithingModifier", "HeavyArmorModifier", "LightArmorModifier", "PickpocketModifier", "LockpickingModifier", "SneakingModifier",
			"AlchemyModifier", "SpeechcraftModifier", "AlterationModifier", "ConjurationModifier", "DestructionModifier", "IllusionModifier",
			"RestorationModifier", "EnchantingModifier", "OneHandedSkillAdvance", "TwoHandedSkillAdvance", "MarksmanSkillAdvance",
			"BlockSkillAdvance", "SmithingSkillAdvance", "HeavyArmorSkillAdvance", "LightArmorSkillAdvance", "PickpocketSkillAdvance",
			"LockpickingSkillAdvance", "SneakingSkillAdvance", "AlchemySkillAdvance", "SpeechcraftSkillAdvance", "AlterationSkillAdvance",
			"ConjurationSkillAdvance", "DestructionSkillAdvance", "IllusionSkillAdvance", "RestorationSkillAdvance", "EnchantingSkillAdvance",
			"LeftWeaponSpeedMultiply", "DragonSouls", "CombatHealthRegenMultiply", "OneHandedPowerModifier", "TwoHandedPowerModifier",
			"MarksmanPowerModifier", "BlockPowerModifier", "SmithingPowerModifier", "HeavyArmorPowerModifier", "LightArmorPowerModifier",
			"PickpocketPowerModifier", "LockpickingPowerModifier", "SneakingPowerModifier", "AlchemyPowerModifier", "SpeechcraftPowerModifier",
			"AlterationPowerModifier", "ConjurationPowerModifier", "DestructionPowerModifier", "IllusionPowerModifier",
			"RestorationPowerModifier", "EnchantingPowerModifier", "DragonRend", "AttackDamageMult", "HealRateMult", "MagickaRateMult",
			"StaminaRateMult", "WerewolfPerks", "VampirePerks", "GrabActorOffset", "Grabbed", "DEPRECATED05", "ReflectDamage" };
		return a_av < std::size(names) ? names[a_av] : "Unknown";
	}

	const char* SkillNameZh(int a_index)
	{
		static const char* const zh[18] = { "单手", "双手", "箭术", "格挡", "锻造", "重甲", "轻甲", "扒窃", "开锁",
			"潜行", "炼金", "口才", "变换", "召唤", "毁灭", "幻术", "恢复", "附魔" };
		return a_index >= 0 && a_index < 18 ? zh[a_index] : "?";
	}

	const char* ActorValueNameZh(std::uint32_t a_av)
	{
		// keyed by the English name: the order of the table above must not matter (actor value ids move between games)
		static const struct
		{
			const char* en;
			const char* zh;
		} table[] = {
			{ "Aggression", "攻击性" }, { "Confidence", "自信" }, { "Energy", "精力" }, { "Morality", "道德" }, { "Mood", "情绪" },
			{ "Assistance", "协助" }, { "OneHanded", "单手" }, { "TwoHanded", "双手" }, { "Archery", "箭术" }, { "Block", "格挡" },
			{ "Smithing", "锻造" }, { "HeavyArmor", "重甲" }, { "LightArmor", "轻甲" }, { "Pickpocket", "扒窃" }, { "Lockpicking", "开锁" },
			{ "Sneak", "潜行" }, { "Alchemy", "炼金" }, { "Speech", "口才" }, { "Alteration", "变换" }, { "Conjuration", "召唤" },
			{ "Destruction", "毁灭" }, { "Illusion", "幻术" }, { "Restoration", "恢复" }, { "Enchanting", "附魔" }, { "Health", "生命" },
			{ "Magicka", "魔法" }, { "Stamina", "耐力" }, { "HealRate", "生命恢复速度" }, { "MagickaRate", "魔法恢复速度" },
			{ "StaminaRate", "耐力恢复速度" }, { "SpeedMult", "速度倍率" }, { "InventoryWeight", "物品栏重量" },
			{ "CarryWeight", "负重上限" }, { "CriticalChance", "暴击几率" }, { "MeleeDamage", "近战伤害" }, { "UnarmedDamage", "徒手伤害" },
			{ "Mass", "质量" }, { "VoicePoints", "龙吼点数" }, { "VoiceRate", "龙吼恢复速度" }, { "DamageResist", "护甲值（伤害抗性）" },
			{ "PoisonResist", "毒素抗性" }, { "ResistFire", "火焰抗性" }, { "ResistShock", "闪电抗性" }, { "ResistFrost", "寒霜抗性" },
			{ "ResistMagic", "魔法抗性" }, { "ResistDisease", "疾病抗性" }, { "PerceptionCondition", "感知状况" },
			{ "EnduranceCondition", "耐力状况" }, { "LeftAttackCondition", "左臂状况" }, { "RightAttackCondition", "右臂状况" },
			{ "LeftMobilityCondition", "左腿状况" }, { "RightMobilityCondition", "右腿状况" }, { "BrainCondition", "头脑状况" },
			{ "Paralysis", "麻痹" }, { "Invisibility", "隐身" }, { "NightEye", "夜视" }, { "DetectLifeRange", "侦测生命范围" },
			{ "WaterBreathing", "水下呼吸" }, { "WaterWalking", "水上行走" }, { "IgnoreCrippledLimbs", "无视残肢" }, { "Fame", "声望" },
			{ "Infamy", "恶名" }, { "JumpingBonus", "跳跃加成" }, { "WardPower", "结界强度" }, { "RightItemCharge", "右手物品充能" },
			{ "ArmorPerks", "护甲专长" }, { "ShieldPerks", "盾牌专长" }, { "WardDeflection", "结界偏转" }, { "BowSpeedBonus", "弓箭速度加成" },
			{ "FavorActive", "人情生效中" }, { "FavorsPerDay", "每日人情数" }, { "FavorsPerDayTimer", "每日人情计时" },
			{ "LeftItemCharge", "左手物品充能" }, { "AbsorbChance", "吸收几率" }, { "Blindness", "失明" }, { "WeaponSpeedMult", "武器速度倍率" },
			{ "ShoutRecoveryMult", "龙吼恢复倍率" }, { "BowStaggerBonus", "弓箭硬直加成" }, { "Telekinesis", "念力" },
			{ "FavorPointsBonus", "人情点加成" }, { "LastBribedIntimidated", "上次贿赂/恐吓" }, { "LastFlattered", "上次奉承" },
			{ "MovementNoiseMult", "移动噪音倍率" }, { "BypassVendorStolenCheck", "绕过商人赃物检查" },
			{ "BypassVendorKeywordCheck", "绕过商人关键字检查" }, { "WaitingForPlayer", "等待玩家" },
			{ "LeftWeaponSpeedMultiply", "左手武器速度倍率" }, { "DragonSouls", "龙魂" },
			{ "CombatHealthRegenMultiply", "战斗中生命恢复倍率" }, { "DragonRend", "龙破" }, { "AttackDamageMult", "攻击伤害倍率" },
			{ "HealRateMult", "生命恢复倍率" }, { "MagickaRateMult", "魔法恢复倍率" }, { "StaminaRateMult", "耐力恢复倍率" },
			{ "WerewolfPerks", "狼人专长" }, { "VampirePerks", "吸血鬼专长" }, { "GrabActorOffset", "抓取角色偏移" },
			{ "Grabbed", "被抓" }, { "DEPRECATED05", "已弃用" }, { "ReflectDamage", "反射伤害" },
		};
		const char* en = ActorValueName(a_av);
		for (const auto& t : table) {
			if (std::strcmp(t.en, en) == 0) {
				return t.zh;
			}
		}
		// "<skill>Modifier" / "<skill>SkillAdvance" / "<skill>PowerModifier"：技能加成、成长、力量修正
		static const struct
		{
			const char* en;
			int         skill;
		} skillPrefixes[] = {
			{ "OneHanded", 0 }, { "TwoHanded", 1 }, { "Marksman", 2 }, { "Archery", 2 }, { "Block", 3 }, { "Smithing", 4 },
			{ "HeavyArmor", 5 }, { "LightArmor", 6 }, { "Pickpocket", 7 }, { "Lockpicking", 8 }, { "Sneaking", 9 }, { "Sneak", 9 },
			{ "Alchemy", 10 }, { "Speechcraft", 11 }, { "Speech", 11 }, { "Alteration", 12 }, { "Conjuration", 13 },
			{ "Destruction", 14 }, { "Illusion", 15 }, { "Restoration", 16 }, { "Enchanting", 17 },
		};
		static const struct
		{
			const char* suffix;
			const char* zh;
		} kinds[] = { { "SkillAdvance", "成长" }, { "PowerModifier", "力量修正" }, { "Modifier", "修正" } };
		const std::string name(en);
		for (const auto& k : kinds) {
			const std::string suf(k.suffix);
			if (name.size() <= suf.size() || name.compare(name.size() - suf.size(), suf.size(), suf) != 0) {
				continue;
			}
			const std::string prefix = name.substr(0, name.size() - suf.size());
			for (const auto& p : skillPrefixes) {
				if (prefix == p.en) {
					thread_local std::string out;
					out = std::string(SkillNameZh(p.skill)) + k.zh;
					return out.c_str();
				}
			}
		}
		// Variable01..10
		if (name.size() == 10 && name.compare(0, 8, "Variable") == 0 && std::isdigit(static_cast<unsigned char>(name[8])) &&
			std::isdigit(static_cast<unsigned char>(name[9]))) {
			thread_local std::string out;
			out = std::string("变量 ") + std::to_string(std::stoi(name.substr(8)));
			return out.c_str();
		}
		return en;
	}

	std::string SetPlayerLegendary(SaveFile& a_save, int a_skill, std::int32_t a_count)
	{
		if (a_skill < 0 || a_skill >= 18 || a_count < 0) {
			return "没有这个技能，或者数量为负";
		}
		return EditPlayer(a_save, [&](const Decoded& d, std::vector<std::uint8_t>& body) -> std::string {
			if (PlayerForm(a_save)->version <= 0x49 || static_cast<std::size_t>(a_skill) >= d.playerSkillsAt.size()) {
				return "这个存档没有记录技能的传奇次数";
			}
			PutI32(body, d.playerSkillsAt[a_skill] + 12, a_count);
			return {};
		});
	}

	std::string ClearPlayerCrimes(SaveFile& a_save, std::uint32_t a_faction)
	{
		return EditPlayer(a_save, [&](const Decoded& d, std::vector<std::uint8_t>& body) -> std::string {
			for (const auto& c : d.crimeGold) {
				if (c.id == a_faction) {
					for (int i = 0; i < 4; ++i) {
						PutI32(body, c.at + 4 * i, 0);
					}
					return {};
				}
			}
			return "玩家没有与那个派系的犯罪记录";
		});
	}

	std::string SetPlayerLevel(SaveFile& a_save, int a_level)
	{
		if (a_level < 1 || a_level > 65535) {
			return "等级必须在 1 到 65535 之间";
		}
		ChangeForm* npc = nullptr;
		for (auto& c : a_save.changeForms) {
			if (c.refID.data == 0x400007 && static_cast<ChangeFormType>(c.typeCode()) == ChangeFormType::NPC_) {
				npc = &c;
			}
		}
		if (!npc) {
			return "这个存档里没有玩家基础记录的变更表单（00000007）";
		}
		const auto nd = Decode(a_save, *npc);
		const auto it = nd.sections.find(1);
		if (!nd.complete || it == nd.sections.end()) {
			return "玩家的基础记录里没有等级数据：未做任何改动";
		}
		const auto before = *npc;
		auto       nb = Body(*npc);
		nb[it->second.first + 8] = static_cast<std::uint8_t>(a_level);
		nb[it->second.first + 9] = static_cast<std::uint8_t>(a_level >> 8);
		SetBody(*npc, nb);
		if (!Decode(a_save, *npc).complete) {
			*npc = before;
			return "这样的结果无法正确读回：未做任何改动";
		}
		// the XP needed for the next level, with the game's default settings: 75 + 25 x level
		const auto why = EditPlayer(a_save, [&](const Decoded& d, std::vector<std::uint8_t>& body) -> std::string {
			if (d.playerXPAt) {
				PutF32(body, d.playerXPAt + 4, 75.0f + 25.0f * static_cast<float>(a_level));
			}
			return {};
		});
		if (!why.empty()) {
			*npc = before;
		}
		return why;
	}

	std::string SetPlayerXP(SaveFile& a_save, float a_xp)
	{
		if (!(a_xp >= 0.0f && a_xp < 1e9f)) {
			return "经验值不能小于 0";
		}
		return EditPlayer(a_save, [&](const Decoded& d, std::vector<std::uint8_t>& body) -> std::string {
			if (!d.playerXPAt) {
				return "找不到经验值数据";
			}
			PutF32(body, d.playerXPAt, a_xp);
			return {};
		});
	}

	std::string SetPlayerPerkPoints(SaveFile& a_save, int a_points)
	{
		if (a_points < 0 || a_points > 255) {
			return "专长点必须在 0 到 255 之间";
		}
		return EditPlayer(a_save, [&](const Decoded& d, std::vector<std::uint8_t>& body) -> std::string {
			if (!d.playerPerkPointsAt) {
				return "找不到专长点数据";
			}
			body[d.playerPerkPointsAt] = static_cast<std::uint8_t>(a_points);
			return {};
		});
	}

	std::string SetPlayerDragonSouls(SaveFile& a_save, int a_souls)
	{
		if (a_souls < 0 || a_souls > 100000) {
			return "龙魂不能小于 0";
		}
		return EditPlayer(a_save, [&](const Decoded& d, std::vector<std::uint8_t>& body) -> std::string {
			constexpr std::uint32_t kSouls = 133;
			float base = 0;
			bool  hasBase = false;
			for (const auto& v : d.baseAVs) {
				if (v.id == kSouls) {
					base = GetF32(body, v.at);
					hasBase = true;
				}
			}
			for (const auto& m : d.avMods) {
				if (m.id == kSouls) {  // spent souls are a permanent modifier: keep the base, adjust what was spent
					PutF32(body, m.at, static_cast<float>(a_souls) - base);
					return {};
				}
			}
			for (const auto& v : d.baseAVs) {
				if (v.id == kSouls) {
					PutF32(body, v.at, static_cast<float>(a_souls));
					return {};
				}
			}
			(void)hasBase;
			return "玩家还没有保存的龙魂数值（先吸收一条龙魂）";
		});
	}

	std::string SetPlayerAV(SaveFile& a_save, std::uint32_t a_av, float a_base)
	{
		if (!std::isfinite(a_base)) {
			return "不是有效数字";
		}
		return EditPlayer(a_save, [&](const Decoded& d, std::vector<std::uint8_t>& body) -> std::string {
			for (const auto& v : d.baseAVs) {
				if (v.id == a_av) {
					PutF32(body, v.at, a_base);
					return {};
				}
			}
			return std::string("玩家没有已保存的基础值：") + ActorValueNameZh(a_av);
		});
	}

	bool CanResetPlayerAV(std::uint32_t a_av)
	{
		// skills, Health, Magicka, Stamina and dragon souls hold progress the race does not know about
		return !(a_av >= 6 && a_av <= 26) && a_av != 133 && a_av < 164;
	}

	std::string ResetPlayerAV(SaveFile& a_save, std::uint32_t a_av)
	{
		if (!CanResetPlayerAV(a_av)) {
			return std::string(ActorValueNameZh(a_av)) + " 记录着你的进度，不能重置";
		}
		return EditPlayer(a_save, [&](const Decoded& d, std::vector<std::uint8_t>& body) -> std::string {
			// the permanent and damage modifiers go too (they are written after the base list, so first)
			for (const auto& m : d.avMods) {
				if (m.id == a_av) {
					PutF32(body, m.at, 0.0f);
					PutF32(body, m.at + 8, 0.0f);
				}
			}
			for (const auto& v : d.baseAVs) {
				if (v.id == a_av) {
					const auto n = GetVs(body, d.baseAVListAt);
					Erase(body, v.at - 4, v.at + 4);  // the AV id and its value
					RewriteVs(body, d.baseAVListAt, n - 1);
					return {};
				}
			}
			return {};  // no saved base value: clearing the modifiers was the reset
		});
	}

	std::string ClearPlayerAVModifiers(SaveFile& a_save, std::uint32_t a_av, int a_which)
	{
		return EditPlayer(a_save, [&](const Decoded& d, std::vector<std::uint8_t>& body) -> std::string {
			for (const auto& m : d.avMods) {
				if (m.id == a_av) {
					for (int i = 0; i < 3; ++i) {
						if (a_which & (1 << i)) {
							PutF32(body, m.at + 4 * i, 0.0f);
						}
					}
					return {};
				}
			}
			return std::string("玩家没有这个属性值的修正：") + ActorValueNameZh(a_av);
		});
	}

	std::string SetPlayerItemCount(SaveFile& a_save, std::uint32_t a_item, std::int32_t a_count)
	{
		if (a_count < 0) {
			return "数量不能为负数";
		}
		if (a_item == kGold) {
			return SetPlayerGold(a_save, a_count);
		}
		return EditPlayer(a_save, [&](const Decoded& d, std::vector<std::uint8_t>& body) -> std::string {
			// the stack without extra data takes the count; stacks with extra data (enchanted, worn...) stay
			std::int32_t inExtras = 0;
			const Decoded::InvAt* plain = nullptr;
			for (const auto& e : d.inventory) {
				if (e.item != a_item) {
					continue;
				}
				if (e.endAt > e.countAt + 4 && body[e.countAt + 4] != 0) {
					inExtras += 0;  // counted by their own Count extra data; left alone
				} else if (!plain) {
					plain = &e;
				}
			}
			if (!plain) {
				return "玩家没有这件物品的普通堆叠可以修改（只有附魔、穿戴或其他特殊状态的）";
			}
			PutI32(body, plain->countAt, a_count);
			(void)inExtras;
			return {};
		});
	}

	std::string RemovePlayerItem(SaveFile& a_save, std::uint32_t a_item)
	{
		return EditPlayer(a_save, [&](const Decoded& d, std::vector<std::uint8_t>& body) -> std::string {
			const auto n = GetVs(body, d.inventoryCountAt);
			std::uint32_t removed = 0;
			for (auto it = d.inventory.rbegin(); it != d.inventory.rend(); ++it) {
				if (it->item == a_item) {
					Erase(body, it->entryAt, it->endAt);
					++removed;
				}
			}
			if (!removed) {
				return "玩家身上没有这件物品";
			}
			RewriteVs(body, d.inventoryCountAt, n - removed);
			return {};
		});
	}

	std::string RemovePlayerSpell(SaveFile& a_save, std::uint32_t a_spell)
	{
		return EditPlayer(a_save, [&](const Decoded& d, std::vector<std::uint8_t>& body) -> std::string {
			for (const auto& sp : d.addedSpells) {
				if (sp.id == a_spell) {
					const auto n = static_cast<std::uint32_t>(GetI32(body, d.addedSpellsAt));
					Erase(body, sp.at, sp.at + 3);
					PutI32(body, d.addedSpellsAt, static_cast<std::int32_t>(n - 1));
					return {};
				}
			}
			return "玩家没有游戏内获得的那个法术（种族或插件给的法术不能在这里移除）";
		});
	}

	std::string SetPlayerBounty(SaveFile& a_save, std::uint32_t a_faction, std::int32_t a_violent, std::int32_t a_nonViolent)
	{
		if (a_violent < 0 || a_nonViolent < 0) {
			return "赏金不能为负数";
		}
		return EditPlayer(a_save, [&](const Decoded& d, std::vector<std::uint8_t>& body) -> std::string {
			for (const auto& c : d.crimeGold) {
				if (c.id == a_faction) {
					PutI32(body, c.at, a_violent);
					PutI32(body, c.at + 4, a_nonViolent);
					return {};
				}
			}
			return "玩家没有与那个派系的犯罪记录";
		});
	}


	bool CanRemoveExtra(const std::string& a_name) { return FindRemovable(a_name) != nullptr; }

	bool CanRemoveExtraFrom(const ChangeForm& a_cf, const std::string& a_name)
	{
		if (!CanRemoveExtra(a_name)) {
			return false;
		}
		if (IsPlayerActor(a_cf)) {  // the player: only what cannot hurt the character
			return a_name == "DismemberedLimbs" || a_name == "AttachedArrows3D" || a_name == "Ghost";
		}
		return true;
	}

	std::string RemoveExtraEffect(const std::string& a_name)
	{
		const auto* r = FindRemovable(a_name);
		return r ? r->effect : std::string();
	}

	std::string RemoveExtra(const SaveFile& a_save, ChangeForm& a_cf, std::size_t a_index)
	{
		if (static_cast<ChangeFormType>(a_cf.typeCode()) != ChangeFormType::ACHR) {
			return "只有角色（ACHR）的额外数据可以这样移除";
		}
		const auto d = Decode(a_save, a_cf);
		if (!d.complete) {
			return "这个变更表单无法完整读取（" + d.error + "）：未做任何改动";
		}
		if (!d.hasExtras || a_index >= d.extras.size()) {
			return "找不到那条额外数据：未做任何改动";
		}
		const auto& name = d.extras[a_index].name;
		if (!CanRemoveExtraFrom(a_cf, name)) {
			return "\"" + name + "\" 不在可以安全移除的条目里";
		}
		// a package and its package data go together
		std::vector<std::size_t> drop{ a_index };
		if (name == "Package" || name == "PackageData") {
			const std::string partner = name == "Package" ? "PackageData" : "Package";
			for (std::size_t i = 0; i < d.extras.size(); ++i) {
				if (d.extras[i].name == partner) {
					drop.push_back(i);
				}
			}
		}
		std::sort(drop.begin(), drop.end());
		const auto before = a_cf;
		const auto body = Body(a_cf);
		std::vector<std::uint8_t> out(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(d.extrasCountAt));
		PutVsval(out, static_cast<std::uint32_t>(d.extras.size() - drop.size()));
		std::size_t at = d.extrasCountEnd;
		for (std::size_t i = 0; i < d.extras.size(); ++i) {
			const auto& e = d.extras[i];
			out.insert(out.end(), body.begin() + static_cast<std::ptrdiff_t>(at), body.begin() + static_cast<std::ptrdiff_t>(e.start));
			if (std::find(drop.begin(), drop.end(), i) == drop.end()) {
				out.insert(out.end(), body.begin() + static_cast<std::ptrdiff_t>(e.start), body.begin() + static_cast<std::ptrdiff_t>(e.end));
			}
			at = e.end;
		}
		out.insert(out.end(), body.begin() + static_cast<std::ptrdiff_t>(at), body.end());
		SetBody(a_cf, out);
		const auto after = Decode(a_save, a_cf);
		if (!after.complete || after.extras.size() != d.extras.size() - drop.size()) {
			a_cf = before;
			return "这样的结果无法正确读回：未做任何改动";
		}
		return {};
	}

	namespace
	{
		std::uint32_t Raw3(const std::vector<std::uint8_t>& b, std::size_t at)
		{
			return (static_cast<std::uint32_t>(b[at]) << 16) | (static_cast<std::uint32_t>(b[at + 1]) << 8) | b[at + 2];
		}
	}

	std::vector<InventoryEntry> ListInventories(const SaveFile& a_save)
	{
		std::vector<InventoryEntry> out;
		for (const auto& c : a_save.changeForms) {
			const auto t = static_cast<ChangeFormType>(c.typeCode());
			if ((t != ChangeFormType::REFR && t != ChangeFormType::ACHR) || !(Bit(c.changeFlags, 5) || Bit(c.changeFlags, 27))) {
				continue;
			}
			const auto d = Decode(a_save, c);
			if (d.inventory.empty()) {
				continue;
			}
			std::vector<std::uint8_t> body;
			try {
				body = Body(c);
			} catch (const std::exception&) {
				continue;
			}
			for (const auto& e : d.inventory) {
				InventoryEntry x;
				x.holderRefRaw = c.refID.data;
				x.holder = a_save.formIDOf(c.refID);
				x.holderType = c.typeCode();
				x.item = e.item;
				if (e.countAt + 4 <= body.size()) {
					std::memcpy(&x.count, &body[e.countAt], 4);
				}
				out.push_back(x);
			}
		}
		return out;
	}

	std::string RemoveInventoryItems(const SaveFile& a_save, ChangeForm& a_cf, const std::set<std::uint32_t>& a_items, std::size_t* a_removed)
	{
		if (a_removed) {
			*a_removed = 0;
		}
		const auto d = Decode(a_save, a_cf);
		if (!d.complete) {
			return "这个变更表单无法完整读取（" + d.error + "）：未做任何改动";
		}
		const auto saved = a_cf;
		auto       body = Body(a_cf);
		const auto n = GetVs(body, d.inventoryCountAt);
		std::uint32_t removed = 0;
		for (auto it = d.inventory.rbegin(); it != d.inventory.rend(); ++it) {
			if (a_items.contains(it->item)) {
				Erase(body, it->entryAt, it->endAt);
				++removed;
			}
		}
		if (!removed) {
			return {};
		}
		RewriteVs(body, d.inventoryCountAt, n - removed);
		SetBody(a_cf, body);
		const auto after = Decode(a_save, a_cf);
		if (!after.complete) {
			a_cf = saved;
			return "物品栏无法完整读回（" + after.error + "）：未做任何改动";
		}
		if (a_removed) {
			*a_removed = removed;
		}
		return {};
	}

	std::vector<ActorPlace> ListActors(const SaveFile& a_save)
	{
		std::vector<ActorPlace> out;
		for (const auto& c : a_save.changeForms) {
			if (static_cast<ChangeFormType>(c.typeCode()) != ChangeFormType::ACHR) {
				continue;
			}
			ActorPlace a;
			a.refRaw = c.refID.data;
			a.formID = a_save.formIDOf(c.refID);
			a.isPlayer = IsPlayerActor(c);
			const int t = InitialType(c.refID, c.changeFlags);
			if (t == 4 || t == 5 || t == 6) {
				std::vector<std::uint8_t> b;
				try {
					b = Body(c);
				} catch (const std::exception&) {
					out.push_back(a);
					continue;
				}
				if (b.size() >= 27) {
					a.hasPosition = true;
					RefID cell;
					cell.data = Raw3(b, 0);
					a.cell = a_save.formIDOf(cell);
					std::memcpy(a.pos, &b[3], 12);
					std::memcpy(a.rot, &b[15], 12);
					if (t == 6 && b.size() >= 34) {
						RefID st;
						st.data = Raw3(b, 27);
						a.startCell = a_save.formIDOf(st);
					}
					if (t == 5 && b.size() >= 31) {
						RefID base;
						base.data = Raw3(b, 28);
						a.baseObject = a_save.formIDOf(base);
					}
				}
			}
			out.push_back(a);
		}
		return out;
	}

	std::string MoveActorToPlayer(SaveFile& a_save, ChangeForm& a_cf)
	{
		if (static_cast<ChangeFormType>(a_cf.typeCode()) != ChangeFormType::ACHR) {
			return "这不是角色";
		}
		if (IsPlayerActor(a_cf)) {
			return "这是玩家本人";
		}
		const auto* player = PlayerForm(a_save);
		if (!player) {
			return "这个存档里没有玩家的变更表单";
		}
		const int pt = InitialType(player->refID, player->changeFlags);
		const auto pb = Body(*player);
		if ((pt != 4 && pt != 6) || pb.size() < 27) {
			return "存档里没有玩家的位置";
		}
		const int t = InitialType(a_cf.refID, a_cf.changeFlags);
		if (t == 0) {
			return "游戏从来没有移动过这个角色，所以存档里没有它的位置（它站在插件摆放的位置）。请在游戏里用控制台代替：prid <它的 form id>，然后 moveto player";
		}
		if (Bit(a_cf.changeFlags, 2)) {
			return "这个角色保存了物理（Havok）数据，不支持移动它";
		}
		const auto before = Decode(a_save, a_cf);
		if (!before.complete) {
			return "这个角色无法完整读取（" + before.error + "）：未做任何改动";
		}
		const auto saved = a_cf;
		auto       body = Body(a_cf);
		// a little in front of the player, facing them
		float ppos[3], prot[3];
		std::memcpy(ppos, &pb[3], 12);
		std::memcpy(prot, &pb[15], 12);
		const float ang = prot[2];
		float       npos[3] = { ppos[0] + 96.0f * std::sin(ang), ppos[1] + 96.0f * std::cos(ang), ppos[2] };
		float       nrot[3] = { 0.0f, 0.0f, ang + 3.14159265f };
		const bool  sameCell = body[0] == pb[0] && body[1] == pb[1] && body[2] == pb[2];
		if (t == 4 && !sameCell) {
			// it changes cell: the game keeps where it started (REFR_CELL_CHANGED and the starting cell)
			const std::uint8_t extra[7] = { body[0], body[1], body[2], 0xFF, 0xFF, 0xFF, 0xFF };
			body.insert(body.begin() + 27, extra, extra + 7);
			a_cf.changeFlags |= 1u << 3;
		}
		std::memcpy(&body[0], &pb[0], 3);
		std::memcpy(&body[3], npos, 12);
		std::memcpy(&body[15], nrot, 12);
		SetBody(a_cf, body);
		const auto moved = Decode(a_save, a_cf);
		if (!moved.complete) {
			a_cf = saved;
			return "移动后的角色无法完整读回（" + moved.error + "）：未做任何改动";
		}
		if (CanResetAI(a_cf, moved)) {
			if (auto why = ResetAI(a_save, a_cf); !why.empty()) {
				a_cf = saved;
				return "无法重置它的 AI（" + why + "）：未做任何改动";
			}
		}
		return {};
	}

	bool CanResetAI(const ChangeForm& a_cf, const Decoded& a_d)
	{
		if (static_cast<ChangeFormType>(a_cf.typeCode()) != ChangeFormType::ACHR || IsPlayerActor(a_cf) || !a_d.complete ||
			!a_d.hasActorData) {
			return false;
		}
		const bool hasMovement = a_d.movement.second > a_d.movement.first + 1;
		const bool hasCombat = a_d.combat.second > a_d.combat.first + 1;
		return a_d.processLevel != -1 || hasMovement || hasCombat;
	}

	std::string ResetAI(const SaveFile& a_save, ChangeForm& a_cf)
	{
		if (IsPlayerActor(a_cf)) {
			return "玩家自己的记录永远不会用这种方式修改：未做任何改动";
		}
		const auto d = Decode(a_save, a_cf);
		if (!d.complete) {
			return "这个变更表单无法完整读取（" + d.error + "）：未做任何改动";
		}
		if (!CanResetAI(a_cf, d)) {
			return "这个角色没有保存的 AI 可以重置：未做任何改动";
		}
		const auto before = a_cf;
		auto       body = Body(a_cf);
		// back to front, so earlier offsets stay valid
		auto clearController = [&](const std::pair<std::size_t, std::size_t>& a_span) {
			if (a_span.second > a_span.first + 1) {
				body.erase(body.begin() + static_cast<std::ptrdiff_t>(a_span.first + 1), body.begin() + static_cast<std::ptrdiff_t>(a_span.second));
			}
			body[a_span.first] = 0;  // "has controller": no
		};
		clearController(d.combat);
		clearController(d.movement);
		if (d.processLevel != -1) {
			body.erase(body.begin() + static_cast<std::ptrdiff_t>(d.aiProcess.first), body.begin() + static_cast<std::ptrdiff_t>(d.aiProcess.second));
			for (int i = 0; i < 4; ++i) {
				body[d.levelAt + i] = 0xFF;  // process level -1
			}
		}
		SetBody(a_cf, body);
		const auto after = Decode(a_save, a_cf);
		if (!after.complete || after.processLevel != -1) {
			a_cf = before;
			return "这样的结果无法正确读回：未做任何改动";
		}
		return {};
	}

	bool CanRemoveFlag(const ChangeForm& a_cf, int a_bit)
	{
		if (a_bit <= 0 || a_bit >= 32 || !Bit(a_cf.changeFlags, a_bit)) {
			return false;
		}
		const auto flags = a_cf.changeFlags;
		switch (static_cast<ChangeFormType>(a_cf.typeCode())) {
		case ChangeFormType::NPC_:
			return true;
		case ChangeFormType::ACHR:
			if (a_cf.refID.data == 0x400014) {  // the player: only what cannot hurt the character
				return a_bit == 4 || a_bit == 28;
			}
			switch (a_bit) {
			case 1:  // REFR_MOVE: only when the saved position is there for it alone
				return a_cf.refID.type() != RefID::Type::kCreated && !Bit(flags, 2) && !Bit(flags, 3) && !Bit(flags, 25);
			case 4:   // REFR_SCALE
			case 28:  // REFR_ANIMATION
				return true;
			case 5:  // REFR_INVENTORY, unless the leveled inventory shares the list
				return !Bit(flags, 27);
			default:
				return false;
			}
		default:
			return false;
		}
	}

	std::string RemoveFlagEffect(std::uint8_t a_type, int a_bit)
	{
		if (static_cast<ChangeFormType>(a_type) == ChangeFormType::ACHR) {
			switch (a_bit) {
			case 1:
				return "角色保存的位置会被丢弃，读档时它会被放回插件摆放的位置（编辑器中设定的位置）。适合卡在某个走不掉的地方的 NPC。";
			case 4:
				return "角色改动过的体型会被丢弃，恢复成插件给的大小。";
			case 5:
				return "角色保存的物品栏改动会被丢弃，恢复成插件给它的物品。它捡到的、别人给的或被拿走的物品都会丢失。";
			case 28:
				return "角色保存的动画状态会被丢弃，读档时它的动画会重新开始。适合卡在某个姿势或动画里的 NPC。";
			default:
				break;
			}
		}
		return "游戏会重新使用插件里那一部分的值（对 NPC_FACE 而言：使用插件给的脸，这也能修好 SetRace 之后不再动的脸）。";
	}

	std::string RemoveFlag(const SaveFile& a_save, ChangeForm& a_cf, int a_bit)
	{
		if (!CanRemoveFlag(a_cf, a_bit)) {
			return "无法从这个变更表单上移除这个标记（只有 NPC_ 和 ACHR 变更表单的部分内容可以，而且只限已经设置的标记）";
		}
		const auto d = Decode(a_save, a_cf);
		if (!d.complete) {
			return "这个变更表单无法完整读取（" + d.error + "）：未做任何改动";
		}
		const auto it = d.sections.find(a_bit);
		if (it == d.sections.end()) {
			return "找不到这个标记的数据：未做任何改动";
		}
		const auto before = a_cf;
		auto       body = Body(a_cf);
		body.erase(body.begin() + static_cast<std::ptrdiff_t>(it->second.first), body.begin() + static_cast<std::ptrdiff_t>(it->second.second));
		a_cf.changeFlags &= ~(1u << a_bit);
		SetBody(a_cf, body);
		const auto after = Decode(a_save, a_cf);
		if (!after.complete) {
			a_cf = before;
			return "这样的结果无法正确读回：未做任何改动";
		}
		return {};
	}
}
