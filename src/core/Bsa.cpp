// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Bsa.h"

#include "ByteIO.h"
#include "Save.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <lz4.h>

namespace ess
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
	}

	BsaArchive::BsaArchive(const std::filesystem::path& a_path) :
		_path(a_path)
	{
		std::ifstream f(a_path, std::ios::binary);
		if (!f) {
			throw std::runtime_error("无法打开归档");
		}
		std::uint8_t hdr[36];
		if (!f.read(reinterpret_cast<char*>(hdr), 36) || std::memcmp(hdr, "BSA\0", 4) != 0) {
			throw std::runtime_error("不是 BSA 归档");
		}
		Reader h(hdr, 36);
		h.seek(4);
		_version = h.u32();
		const auto offset = h.u32();
		_flags = h.u32();
		const auto folderCount = h.u32();
		const auto fileCount = h.u32();
		const auto folderNamesLen = h.u32();
		const auto fileNamesLen = h.u32();
		if (_version != 104 && _version != 105) {
			throw std::runtime_error("不支持的 BSA 版本 " + std::to_string(_version));
		}
		const bool        dirNames = _flags & 1, fileNames = _flags & 2, compressedByDefault = _flags & 4;
		const std::size_t folderRec = _version == 105 ? 24 : 16;
		const std::size_t need = offset + folderCount * folderRec + (dirNames ? folderNamesLen + folderCount : 0) + std::size_t(fileCount) * 16 + fileNamesLen;
		std::vector<std::uint8_t> buf(need);
		f.seekg(0);
		f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(need));
		buf.resize(static_cast<std::size_t>(f.gcount()));

		Reader r(buf.data(), buf.size());
		r.seek(offset);
		std::vector<std::uint32_t> counts(folderCount);
		for (auto& c : counts) {
			r.u64();  // hash
			c = r.u32();
			if (_version == 105) {
				r.u32();
				r.u64();
			} else {
				r.u32();
			}
		}
		std::vector<std::string> folders(folderCount);
		_entries.reserve(fileCount);
		for (std::uint32_t i = 0; i < folderCount; ++i) {
			if (dirNames) {
				const auto len = r.u8();
				auto       b = r.bytes(len);
				std::string s(b.begin(), b.end());
				while (!s.empty() && s.back() == '\0') {
					s.pop_back();
				}
				folders[i] = Lower(s);
			}
			for (std::uint32_t k = 0; k < counts[i]; ++k) {
				r.u64();
				std::uint32_t size = r.u32();
				const auto    off = r.u32();
				const bool    toggle = (size & (1u << 30)) != 0;
				size &= ~(1u << 30);
				Entry e;
				e.path = folders[i];
				e.size = size;
				e.offset = off;
				e.compressed = compressedByDefault != toggle;
				_entries.push_back(std::move(e));
			}
		}
		if (fileNames) {
			for (auto& e : _entries) {
				std::string name;
				for (char c; r.remaining() && (c = static_cast<char>(r.u8())) != '\0';) {
					name.push_back(c);
				}
				e.path += "\\" + Lower(name);
			}
		}
	}

	const BsaArchive::Entry* BsaArchive::find(const std::string& a_lowerPath) const
	{
		for (const auto& e : _entries) {
			if (e.path == a_lowerPath) {
				return &e;
			}
		}
		return nullptr;
	}

	std::vector<std::uint8_t> BsaArchive::extract(const Entry& a_entry) const
	{
		std::ifstream f(_path, std::ios::binary);
		if (!f) {
			throw std::runtime_error("无法打开归档");
		}
		std::vector<std::uint8_t> raw(a_entry.size);
		f.seekg(a_entry.offset);
		if (!f.read(reinterpret_cast<char*>(raw.data()), a_entry.size)) {
			throw std::runtime_error("无法读取归档，内容不完整");
		}
		std::size_t pos = 0;
		if (_flags & 0x100) {  // embedded file name: bstring
			pos = 1 + std::size_t(raw.empty() ? 0 : raw[0]);
		}
		if (pos > raw.size()) {
			throw std::runtime_error("内嵌文件名无效");
		}
		if (!a_entry.compressed) {
			return { raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.end() };
		}
		if (raw.size() < pos + 4) {
			throw std::runtime_error("压缩条目无效");
		}
		std::uint32_t original;
		std::memcpy(&original, raw.data() + pos, 4);
		pos += 4;
		if (_version == 104) {
			return DecompressZlib(raw.data() + pos, raw.size() - pos, original);
		}
		return DecompressLZ4Frame(raw.data() + pos, raw.size() - pos, original);
	}

	std::vector<std::uint8_t> DecompressLZ4Frame(const std::uint8_t* a_src, std::size_t a_srcLen, std::size_t a_dstLen)
	{
		Reader r(a_src, a_srcLen);
		if (r.u32() != 0x184D2204) {
			throw std::runtime_error("不是 LZ4 帧");
		}
		const auto flg = r.u8();
		r.u8();  // BD
		const bool blockChecksum = flg & 0x10, contentSize = flg & 0x08, dictID = flg & 0x01;
		if (contentSize) {
			r.u64();
		}
		if (dictID) {
			r.u32();
		}
		r.u8();  // header checksum
		std::vector<std::uint8_t> out(a_dstLen);
		std::size_t               pos = 0;
		for (;;) {
			const auto bsize = r.u32();
			if (bsize == 0) {
				break;
			}
			const bool          stored = bsize & 0x80000000u;
			const std::uint32_t len = bsize & 0x7FFFFFFFu;
			r.need(len);
			if (stored) {
				if (pos + len > out.size()) {
					throw std::runtime_error("LZ4 帧大于预期大小");
				}
				std::memcpy(out.data() + pos, r.here(), len);
				pos += len;
			} else {
				// linked blocks: the previous output is the dictionary (it sits right before dst)
				const int n = LZ4_decompress_safe_usingDict(reinterpret_cast<const char*>(r.here()), reinterpret_cast<char*>(out.data() + pos),
					static_cast<int>(len), static_cast<int>(out.size() - pos), reinterpret_cast<const char*>(out.data()), static_cast<int>(pos));
				if (n < 0) {
					throw std::runtime_error("LZ4 帧数据块已损坏");
				}
				pos += static_cast<std::size_t>(n);
			}
			r.seek(r.pos() + len);
			if (blockChecksum) {
				r.u32();
			}
		}
		if (pos != a_dstLen) {
			throw std::runtime_error("LZ4 帧大小不匹配");
		}
		return out;
	}
}
