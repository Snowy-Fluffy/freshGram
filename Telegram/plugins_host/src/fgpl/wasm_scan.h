#pragma once

#include "fgpl/result.h"

#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace fgpl {

struct WasmInfo {
	std::vector<std::pair<std::string, std::string>> imports;
	std::set<std::string> exports;
};

[[nodiscard]] Result<WasmInfo, std::string> ScanWasm(
	std::span<const uint8_t> module);

} // namespace fgpl
