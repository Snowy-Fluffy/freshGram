// This is the source code of freshGram.
#pragma once

#include "base/basic_types.h"

namespace fgpl {
class Transport;
} // namespace fgpl

namespace AyuPlugins {

[[nodiscard]] fgpl::Transport *NetworkTransport();
void RefreshProxy();

} // namespace AyuPlugins
