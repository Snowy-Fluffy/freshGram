// This is the source code of freshGram.
#pragma once

class QKeyEvent;

namespace FreshPlatform {

// Whether CapsLockOn() asks the system, otherwise the state comes from events.
[[nodiscard]] bool CapsLockQueryable();
[[nodiscard]] bool CapsLockOn();

// The state after the key event, when the event tells it.
[[nodiscard]] std::optional<bool> CapsLockFromEvent(
	not_null<QKeyEvent*> e,
	bool current);

} // namespace FreshPlatform
