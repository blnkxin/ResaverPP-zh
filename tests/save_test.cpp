// ReSaver++ fixture test for one real save: it must load without structural problems, write back
// byte-identical, and the safe writer must keep matching backup generations of the save and its co-save.
// usage: rs_save_test <save.ess> <scratch folder>      exit 0 = passed
#include "core/Save.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace ess;

namespace
{
	int g_fail = 0;

	void Check(bool a_ok, const std::string& a_what)
	{
		std::printf("%s: %s\n", a_ok ? "ok  " : "FAIL", a_what.c_str());
		g_fail += a_ok ? 0 : 1;
	}

	std::vector<char> Bytes(const std::filesystem::path& p)
	{
		std::ifstream f(p, std::ios::binary);
		return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
	}
}

int main(int argc, char** argv)
{
	if (argc < 3) {
		std::puts("usage: rs_save_test <save.ess> <scratch folder>");
		return 2;
	}
	const std::filesystem::path in = argv[1], dir = argv[2];
	if (!std::filesystem::exists(in)) {
		std::printf("FAIL: fixture %s is missing\n", argv[1]);
		return 1;
	}
	try {
		auto s = SaveFile::load(in);
		Check(s->problems.empty(), "loads with no structural problems");
		Check(s->papyrus() != nullptr, "has a Papyrus section");

		// unchanged save: writing it gives the same file
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
		std::filesystem::create_directories(dir);
		const auto out = dir / "test.ess";
		s->write(out, false);
		const auto again = SaveFile::load(out);
		Check(again->serialize() == s->serialize(), "re-read save serializes identically");
		Check(Bytes(out) == Bytes(in) || s->header.compression != Compression::kNone,
			"uncompressed save writes back byte-identical (compressed saves: body compared above)");
		Check(again->buildBody() == s->buildBody(), "body is identical after write and re-read");

		// backups: two overwrites make two generations, each with its co-save
		{
			std::ofstream(dir / "test.skse", std::ios::binary) << "CO-SAVE-1";
		}
		auto withCo = SaveFile::load(out);  // picks up the co-save
		const auto w1 = withCo->write(out, true);
		Check(w1.backup == dir / "test.ess.bak" && w1.cosaveBackup == dir / "test.skse.bak", "first overwrite: test.ess.bak + test.skse.bak");
		const auto w2 = withCo->write(out, true);
		Check(w2.backup == dir / "test.ess.bak2" && w2.cosaveBackup == dir / "test.skse.bak2", "second overwrite: test.ess.bak2 + test.skse.bak2");
		Check(std::filesystem::exists(dir / "test.skse.bak") && std::filesystem::exists(dir / "test.skse.bak2"), "earlier co-save backup kept");
		Check(!std::filesystem::exists(dir / "test.ess.tmp") && !std::filesystem::exists(dir / "test.skse.tmp"), "no temporary files left");
		Check(Bytes(dir / "test.skse") == std::vector<char>{ 'C', 'O', '-', 'S', 'A', 'V', 'E', '-', '1' }, "co-save written");
	} catch (const std::exception& e) {
		Check(false, std::string("exception: ") + e.what());
	}
	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
	return g_fail ? 1 : 0;
}
