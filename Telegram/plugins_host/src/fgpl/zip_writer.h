#pragma once

#include "fgpl/cbor.h"

#include <string>
#include <utility>
#include <vector>

namespace fgpl {

[[nodiscard]] cbor::Bytes WriteZip(
	const std::vector<std::pair<std::string, cbor::Bytes>> &files);

} // namespace fgpl
