// This is the source code of freshGram.
#include "ayu/utils/caps_lock.h"

#include <QtGui/QKeyEvent>

#ifdef Q_OS_WIN
#include <windows.h>
#elif defined Q_OS_MAC // Q_OS_WIN
#include <CoreGraphics/CoreGraphics.h>
#endif // Q_OS_WIN || Q_OS_MAC

namespace FreshPlatform {
namespace {

#if !defined Q_OS_WIN && !defined Q_OS_MAC
// The Lock bit of the xkb modifiers that both X11 and Wayland report.
constexpr auto kLockMask = quint32(0x02);
#endif // !Q_OS_WIN && !Q_OS_MAC

} // namespace

bool CapsLockQueryable() {
#if defined Q_OS_WIN || defined Q_OS_MAC
	return true;
#else // Q_OS_WIN || Q_OS_MAC
	return false;
#endif // Q_OS_WIN || Q_OS_MAC
}

bool CapsLockOn() {
#ifdef Q_OS_WIN
	return (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
#elif defined Q_OS_MAC // Q_OS_WIN
	const auto flags = CGEventSourceFlagsState(
		kCGEventSourceStateCombinedSessionState);
	return (flags & kCGEventFlagMaskAlphaShift) != 0;
#else // Q_OS_WIN || Q_OS_MAC
	return false;
#endif // Q_OS_WIN || Q_OS_MAC
}

std::optional<bool> CapsLockFromEvent(
		[[maybe_unused]] not_null<QKeyEvent*> e,
		[[maybe_unused]] bool current) {
#if defined Q_OS_WIN || defined Q_OS_MAC
	return std::nullopt;
#else // Q_OS_WIN || Q_OS_MAC
	if (e->key() == Qt::Key_CapsLock) {
		// WHY: the modifiers of the Caps Lock key itself are the old ones.
		if (e->type() == QEvent::KeyPress && !e->isAutoRepeat()) {
			return !current;
		}
		return std::nullopt;
	}
	return (e->nativeModifiers() & kLockMask) != 0;
#endif // Q_OS_WIN || Q_OS_MAC
}

} // namespace FreshPlatform
