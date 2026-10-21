#include "fgpl/vfs.h"

#include <cstdlib>
#include <string>

namespace {

[[nodiscard]] std::shared_ptr<const fgpl::Vfs> Tree() {
	static const auto tree = [] {
		auto builder = fgpl::VfsBuilder();
		const auto names = {
			"a/b/c.txt", "a/d.txt", "a/b/e/f.txt", "g/h.txt", "i.txt", "g/j/k",
		};
		for (const auto name : names) {
			if (builder.addFile(name, { 1, 2, 3 }) != fgpl::Errc::Ok) {
				std::abort();
			}
		}
		return builder.finish();
	}();
	return tree;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	const auto tree = Tree();
	const auto path = std::string(reinterpret_cast<const char *>(data), size);
	for (const auto base : { "a", "a/b", "g" }) {
		const auto node = tree->resolve(fgpl::Vfs::kRoot, base);
		const auto found = tree->resolve(node.value(), path);
		if (found.ok() && !tree->within(found.value(), node.value())) {
			std::abort();
		}
	}
	return 0;
}
