// ReSaver++ test: decodes every change form of a save and reports coverage; then applies
// Reset Havok + Purify FormLists, writes, re-reads and checks the result decodes again.
#include "core/ChangeFormData.h"

#include <cstdio>
#include <map>

using namespace ess;

int main(int argc, char** argv)
{
	if (argc < 2) {
		std::puts("usage: rs_cf_test <save.ess> [out.ess]");
		return 1;
	}
	auto save = SaveFile::load(argv[1]);
	std::map<std::string, std::pair<std::size_t, std::size_t>> byType;  // complete, incomplete
	std::map<std::string, std::size_t>                         errors;
	for (const auto& cf : save->changeForms) {
		const auto d = cf::Decode(*save, cf);
		if (!d.supported) {
			continue;
		}
		auto& t = byType[ChangeFormTypeName(cf.typeCode())];
		if (d.complete) {
			++t.first;
		} else {
			++t.second;
			std::string e = d.error;
			if (auto p = e.find("（位于第 "); p != std::string::npos) {
				e = e.substr(0, p);
			}
			++errors[std::string(ChangeFormTypeName(cf.typeCode())) + ": " + e];
		}
	}
	for (const auto& [t, c] : byType) {
		std::printf("%-5s decoded %7zu  incomplete %6zu\n", t.c_str(), c.first, c.second);
	}
	std::vector<std::pair<std::size_t, std::string>> top;
	for (const auto& [e, n] : errors) {
		top.push_back({ n, e });
	}
	std::sort(top.rbegin(), top.rend());
	for (std::size_t i = 0; i < top.size() && i < 60; ++i) {
		std::printf("  %6zu  %s\n", top[i].first, top[i].second.c_str());
	}
	const auto h = cf::ResetHavok(*save, {}, true);
	const auto f = cf::PurifyFormLists(*save, true);
	std::printf("havok: %zu resettable, %zu not decodable | formlists: %zu null entries in %zu lists (%zu not decodable)\n", h.reset, h.failed,
		f.entries, f.forms, f.failed);
	if (argc > 2) {
		const auto h2 = cf::ResetHavok(*save);
		const auto f2 = cf::PurifyFormLists(*save);
		save->save(argv[2]);
		auto back = SaveFile::load(argv[2]);
		std::size_t bad = 0;
		for (const auto& cf : back->changeForms) {
			const auto t = static_cast<ChangeFormType>(cf.typeCode());
			if (t == ChangeFormType::REFR || t == ChangeFormType::FLST) {
				const auto d = cf::Decode(*back, cf);
				if (t == ChangeFormType::FLST && d.complete && cf::NullEntries(*back, cf)) {
					++bad;
				}
				if (t == ChangeFormType::REFR && d.complete && d.hasHavok) {
					for (std::size_t i = 0; i < d.havokSize; ++i) {
						if (cf::Body(cf)[d.havokOffset + i]) {
							++bad;
							break;
						}
					}
				}
			}
		}
		std::printf("applied: havok %zu, formlist entries %zu; re-read %zu change forms, %zu problems\n", h2.reset, f2.entries,
			back->changeForms.size(), bad);
		return bad ? 2 : 0;
	}
	return 0;
}
