// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace ess
{
	class FormatError : public std::runtime_error
	{
	public:
		FormatError(const std::string& a_msg, std::size_t a_pos) :
			std::runtime_error(a_msg + "（位于第 " + std::to_string(a_pos) + " 字节）"), pos(a_pos) {}
		std::size_t pos;
	};

	// Little-endian reader over a byte range. Every read is bounds-checked.
	class Reader
	{
	public:
		Reader(const std::uint8_t* a_data, std::size_t a_size) :
			_data(a_data), _size(a_size) {}

		std::size_t pos() const { return _pos; }
		std::size_t size() const { return _size; }
		std::size_t remaining() const { return _size - _pos; }
		void        seek(std::size_t a_pos)
		{
			if (a_pos > _size) {
				throw FormatError("定位超出数据末尾", a_pos);
			}
			_pos = a_pos;
		}
		const std::uint8_t* here() const { return _data + _pos; }

		void need(std::size_t a_n) const
		{
			if (_size - _pos < a_n) {
				throw FormatError("数据意外结束（需要 " + std::to_string(a_n) + " 字节）", _pos);
			}
		}

		std::uint8_t u8()
		{
			need(1);
			return _data[_pos++];
		}
		std::int8_t  i8() { return static_cast<std::int8_t>(u8()); }
		std::uint16_t u16()
		{
			need(2);
			std::uint16_t v;
			std::memcpy(&v, _data + _pos, 2);
			_pos += 2;
			return v;
		}
		std::int16_t  i16() { return static_cast<std::int16_t>(u16()); }
		std::uint32_t u32()
		{
			need(4);
			std::uint32_t v;
			std::memcpy(&v, _data + _pos, 4);
			_pos += 4;
			return v;
		}
		std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
		float        f32()
		{
			std::uint32_t u = u32();
			float         f;
			std::memcpy(&f, &u, 4);
			return f;
		}
		std::uint64_t u64()
		{
			need(8);
			std::uint64_t v;
			std::memcpy(&v, _data + _pos, 8);
			_pos += 8;
			return v;
		}
		std::vector<std::uint8_t> bytes(std::size_t a_n)
		{
			need(a_n);
			std::vector<std::uint8_t> v(_data + _pos, _data + _pos + a_n);
			_pos += a_n;
			return v;
		}
		// u16 length-prefixed string (raw bytes, Windows-1252 in practice)
		std::string wstring()
		{
			const std::uint16_t n = u16();
			need(n);
			std::string s(reinterpret_cast<const char*>(_data + _pos), n);
			_pos += n;
			return s;
		}
		// Skyrim Legendary Edition saves: Papyrus string indices are 16-bit (0xFFFF = a 32-bit index
		// follows) and object/array ids 32-bit. Special Edition: 32-bit strings, 64-bit ids.
		bool          le = false;
		std::uint32_t tstr()
		{
			if (!le) {
				return u32();
			}
			const std::uint16_t v = u16();
			return v == 0xFFFF ? u32() : v;
		}
		std::uint64_t eid() { return le ? u32() : u64(); }

		// u32 length-prefixed string
		std::string lstring()
		{
			const std::uint32_t n = u32();
			need(n);
			std::string s(reinterpret_cast<const char*>(_data + _pos), n);
			_pos += n;
			return s;
		}

	private:
		const std::uint8_t* _data;
		std::size_t         _size;
		std::size_t         _pos = 0;
	};

	class Writer
	{
	public:
		std::vector<std::uint8_t> buf;

		std::size_t size() const { return buf.size(); }
		void        u8(std::uint8_t v) { buf.push_back(v); }
		void        i8(std::int8_t v) { buf.push_back(static_cast<std::uint8_t>(v)); }
		void        u16(std::uint16_t v) { raw(&v, 2); }
		void        i16(std::int16_t v) { raw(&v, 2); }
		void        u32(std::uint32_t v) { raw(&v, 4); }
		void        i32(std::int32_t v) { raw(&v, 4); }
		void        f32(float v) { raw(&v, 4); }
		void        u64(std::uint64_t v) { raw(&v, 8); }
		void        raw(const void* p, std::size_t n)
		{
			const auto* b = static_cast<const std::uint8_t*>(p);
			buf.insert(buf.end(), b, b + n);
		}
		void bytes(const std::vector<std::uint8_t>& v) { buf.insert(buf.end(), v.begin(), v.end()); }

		bool le = false;  // see Reader::le
		void tstr(std::uint32_t v)
		{
			if (!le) {
				u32(v);
			} else if (v <= 0xFFF0) {  // the cutoff the game uses (with Crash Fixes / LE Light Support)
				u16(static_cast<std::uint16_t>(v));
			} else {
				u16(0xFFFF);
				u32(v);
			}
		}
		void eid(std::uint64_t v)
		{
			if (le) {
				u32(static_cast<std::uint32_t>(v));
			} else {
				u64(v);
			}
		}
		void wstring(const std::string& s)
		{
			u16(static_cast<std::uint16_t>(s.size()));
			raw(s.data(), s.size());
		}
		void lstring(const std::string& s)
		{
			u32(static_cast<std::uint32_t>(s.size()));
			raw(s.data(), s.size());
		}
		void patch32(std::size_t a_at, std::uint32_t v) { std::memcpy(buf.data() + a_at, &v, 4); }
	};
}
