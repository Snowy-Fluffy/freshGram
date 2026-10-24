// This is the source code of freshGram.
#include "ayu/features/passcode_guard/lock_triggers.h"

#include "ayu/ayu_settings.h"
#include "base/platform/base_platform_info.h"
#include "base/timer.h"
#include "core/application.h"
#include "core/core_screenshot_protection.h"
#include "main/main_domain.h"
#include "storage/storage_domain.h"
#include "window/main_window.h"
#include "window/window_controller.h"

#include <QtGui/QGuiApplication>

namespace FreshLockTriggers {
namespace {

constexpr auto kMinimizedCheckEach = crl::time(1000);

enum class CaptureMode {
	Off = 0,
	WhileLocked = 1,
	Always = 2,
};

struct State {
	base::Timer minimizedTimer;
	crl::time minimizedSince = 0;
	rpl::lifetime lifetime;
};

[[nodiscard]] State &GetState() {
	static auto result = State();
	return result;
}

[[nodiscard]] bool PrimaryMinimized() {
	const auto window = Core::App().activePrimaryWindow();
	if (!window) {
		return false;
	}
	const auto widget = window->widget();
	if (widget->isHidden()) {
		return true;
	} else if (Platform::IsWayland()) {
		// WHY: Wayland does not tell a window that it is minimized.
		return (qGuiApp->applicationState() != Qt::ApplicationActive);
	}
	return widget->isMinimized();
}

void CheckMinimized() {
	auto &state = GetState();
	const auto delay = AyuSettings::getInstance().lockOnMinimize();
	if (delay < 0 || !CanLock()) {
		state.minimizedTimer.cancel();
		state.minimizedSince = 0;
		return;
	} else if (!PrimaryMinimized()) {
		state.minimizedSince = 0;
		return;
	}
	const auto now = crl::now();
	if (!state.minimizedSince) {
		state.minimizedSince = now;
	}
	if (now - state.minimizedSince >= delay * crl::time(1000)) {
		state.minimizedTimer.cancel();
		state.minimizedSince = 0;
		Lock();
	}
}

void ApplicationStateChanged(Qt::ApplicationState applicationState) {
	auto &state = GetState();
	if (applicationState == Qt::ApplicationActive) {
		state.minimizedTimer.cancel();
		state.minimizedSince = 0;
	} else if (AyuSettings::getInstance().lockOnMinimize() >= 0
		&& !state.minimizedTimer.isActive()) {
		state.minimizedTimer.callEach(kMinimizedCheckEach);
		CheckMinimized();
	}
}

} // namespace

void Init() {
	auto &state = GetState();
	state.minimizedTimer.setCallback(CheckMinimized);
	QObject::connect(
		qGuiApp,
		&QGuiApplication::applicationStateChanged,
		ApplicationStateChanged);

	auto &app = Core::App();
	auto protect = rpl::combine(
		AyuSettings::getInstance().hideFromCaptureValue(),
		app.passcodeLockValue(),
		rpl::single(0) | rpl::then(
			app.domain().local().localPasscodeChanged() | rpl::map_to(0)),
		[](int mode, bool locked, int) {
			switch (CaptureMode(mode)) {
			case CaptureMode::WhileLocked:
				return locked;
			case CaptureMode::Always:
				return locked
					|| (Core::App().domain().started()
						&& Core::App().domain().local().hasLocalPasscode());
			case CaptureMode::Off:
				break;
			}
			return false;
		});
	app.screenshotProtection().addContentReason(
		std::move(protect),
		state.lifetime);
}

bool CanLock() {
	auto &app = Core::App();
	return app.domain().started()
		&& app.domain().local().hasLocalPasscode()
		&& !app.passcodeLocked();
}

void Lock() {
	if (CanLock()) {
		Core::App().lockByPasscode();
	}
}

void SystemLocked() {
	// WHY: it comes from a system callback, outside of the Qt event loop.
	crl::on_main(&Core::App(), [] {
		if (AyuSettings::getInstance().lockOnSystemLock()) {
			Lock();
		}
	});
}

} // namespace FreshLockTriggers
