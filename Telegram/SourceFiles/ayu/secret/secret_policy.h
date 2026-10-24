#pragma once

namespace FreshSecret {

[[nodiscard]] bool PasscodeSet();
[[nodiscard]] bool Enabled();
void SyncPolicy();
void HandlePasscodeChange();

} // namespace FreshSecret
