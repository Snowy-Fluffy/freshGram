#include "fgpl/cbor.h"
#include "fgpl/ui_tree.h"

#include <cstdlib>

namespace {

using namespace fgpl;

void CheckEncodes(const cbor::Value &value) {
	auto bytes = cbor::Bytes();
	if (!cbor::Encode(value, bytes, 32)) {
		std::abort();
	}
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	auto limits = cbor::Limits();
	limits.maxItems = 4096;
	limits.maxString = 1 << 16;
	limits.maxBytes = 1 << 18;
	const auto decoded = cbor::Decode({ data, size }, limits);
	if (!decoded.ok()) {
		return 0;
	}
	const auto &input = decoded.value();

	const auto page = ui::ValidatePage(input);
	if (page.ok()) {
		CheckEncodes(page.value());
		if (!ui::ValidatePage(page.value()).ok()) {
			std::abort();
		}
	}
	const auto badges = ui::ValidateBadges(input);
	if (badges.ok()) {
		CheckEncodes(badges.value());
		const auto items = badges.value().asArray();
		if (!items || items->size() > ui::kMaxBadges) {
			std::abort();
		}
	}
	return 0;
}
