#include "package.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	auto limits = fgpl::PackageLimits();
	limits.zip.maxTotalBytes = uint64_t(8) << 20;
	limits.zip.maxEntryBytes = uint64_t(4) << 20;
	limits.zip.maxArchiveBytes = uint64_t(1) << 20;
	const auto result = fgpl::OpenPackage({ data, size }, limits);
	if (result.ok()) {
		for (const auto &file : result.value().files) {
			if (file.first.find("..") != std::string::npos
				|| file.first.front() == '/') {
				__builtin_trap();
			}
		}
	}
	return 0;
}
