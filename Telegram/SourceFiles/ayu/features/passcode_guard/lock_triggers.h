// This is the source code of freshGram.
#pragma once

namespace FreshLockTriggers {

// Watches the minimized window and the screen capture setting.
void Init();

[[nodiscard]] bool CanLock();
void Lock();

// The screen of the computer got locked or the system goes to sleep.
void SystemLocked();

} // namespace FreshLockTriggers
