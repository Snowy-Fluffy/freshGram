#include "fgpl/text_markup.h"

#include <cstdlib>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	using namespace fgpl;
	const auto source = std::string(reinterpret_cast<const char*>(data), size);
	for (const auto format : { TextFormat::Markdown, TextFormat::Html }) {
		const auto parsed = ParseMarkup(format, source);
		if (!parsed.ok()) {
			continue;
		}
		const auto units = Utf16Length(parsed.value().text);
		auto previous = int64_t(-1);
		for (const auto &entity : parsed.value().entities) {
			if (entity.offset < 0
				|| entity.length <= 0
				|| entity.offset + entity.length > units
				|| entity.offset < previous
				|| !KnownEntityType(entity.type)) {
				std::abort();
			}
			previous = entity.offset;
		}
		if (parsed.value().entities.size() > kMaxTextEntities) {
			std::abort();
		}
	}
	return 0;
}
