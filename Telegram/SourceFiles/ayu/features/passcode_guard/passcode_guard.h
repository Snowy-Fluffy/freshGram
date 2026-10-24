// This is the source code of freshGram.
#pragma once

namespace FreshPasscodeGuard {

enum class WipeAction : uchar {
	Logout = 0,
	DeleteAll = 1,
};

// Restores the bad tries of the lock screen saved before a restart.
void EnsureLoaded();

// Returns true when the failure triggered the wipe.
bool RegisterFailure();
void RegisterSuccess();

[[nodiscard]] crl::time WaitLeft();
// -1 when the wipe after failed attempts is off.
[[nodiscard]] int AttemptsBeforeWipe();

} // namespace FreshPasscodeGuard
