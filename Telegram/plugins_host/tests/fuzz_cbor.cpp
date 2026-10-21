#include "cbor.h"

#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	using namespace fgpl::cbor;
	auto limits = Limits();
	limits.maxItems = 4096;
	limits.maxString = 1 << 16;
	const auto decoded = Decode({ data, size }, limits);
	if (!decoded.ok()) {
		return 0;
	}
	auto first = Bytes();
	if (!Encode(decoded.value(), first, limits.maxDepth)) {
		std::abort();
	}
	const auto again = Decode(first, limits);
	if (!again.ok()) {
		std::abort();
	}
	auto second = Bytes();
	if (!Encode(again.value(), second, limits.maxDepth) || first != second) {
		std::abort();
	}
	return 0;
}
