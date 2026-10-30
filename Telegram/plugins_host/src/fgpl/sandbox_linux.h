#pragma once

#include "fgpl/sandbox.h"

namespace fgpl::sandbox {

[[nodiscard]] int LandlockAbi();
[[nodiscard]] bool SpeculationRestricted();

} // namespace fgpl::sandbox
