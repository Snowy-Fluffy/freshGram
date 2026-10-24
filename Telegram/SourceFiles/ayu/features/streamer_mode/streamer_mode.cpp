// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026

#include "ayu/features/streamer_mode/streamer_mode.h"

#include "ayu/features/streamer_mode/platform/platform_streamer_mode.h"
#include "ayu/ayu_settings.h"
#include "core/application.h"
#include "rpl/combine.h"
#include "rpl/map.h"
#include "ui/effects/spoiler_mess.h"
#include "ui/text/text.h"
#include "ui/text/text_utilities.h"
#include "window/window_controller.h"

#include <QtCore/QVariant>
#include <algorithm>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

namespace AyuFeatures::StreamerMode {

namespace {

constexpr auto kHiddenProperty = "FreshStreamerModeHidden";

[[nodiscard]] bool IsWindowCaptureExcluded(not_null<QWidget*> widget) {
	return widget->property(kHiddenProperty).toBool();
}

void SetWindowCaptureExcluded(QWidget *widget, bool excluded) {
	const auto window = widget->window();
	Platform::SetWindowCaptureExcluded(window, excluded);
	window->setProperty(kHiddenProperty, excluded);
}

} // namespace

void apply(bool enabled) {
	enabled = enabled
		&& AyuSettings::getInstance().streamerHideWholeWindow();
	Core::App().enumerateWindows([=](not_null<Window::Controller*> window) {
		SetWindowCaptureExcluded(window->widget(), enabled);
	});
	for (const auto widget : QApplication::topLevelWidgets()) {
		if (!widget->windowHandle()) {
			continue;
		}
		if (enabled) {
			if (widget->isVisible()
				&& !IsWindowCaptureExcluded(widget)) {
				SetWindowCaptureExcluded(widget, true);
			}
		} else if (IsWindowCaptureExcluded(widget)) {
			SetWindowCaptureExcluded(widget, false);
		}
	}
}

void hideWidgetWindow(QWidget *widget) {
	SetWindowCaptureExcluded(
		widget,
		AyuSettings::getInstance().streamerHideWholeWindow());
}

void showWidgetWindow(QWidget *widget) {
	SetWindowCaptureExcluded(widget, false);
}

bool spoilersActive() {
	const auto &settings = AyuSettings::getInstance();
#ifdef Q_OS_LINUX
	return settings.streamerMode();
#else // Q_OS_LINUX
	return settings.streamerMode() && settings.streamerSpoilers();
#endif // Q_OS_LINUX
}

void refresh() {
	Ui::Text::SetNamesSpoilered(spoilersActive());
	Ui::SetSpoilersStatic(spoilersActive());
}

rpl::producer<bool> spoilersActiveValue() {
	auto &settings = AyuSettings::getInstance();
#ifdef Q_OS_LINUX
	return settings.streamerModeValue();
#else // Q_OS_LINUX
	return rpl::combine(
		settings.streamerModeValue(),
		settings.streamerSpoilersValue(),
		[](bool mode, bool spoilers) { return mode && spoilers; }
	) | rpl::distinct_until_changed();
#endif // Q_OS_LINUX
}

rpl::producer<TextWithEntities> spoilered(
		rpl::producer<TextWithEntities> text) {
	return rpl::combine(
		std::move(text),
		spoilersActiveValue()
	) | rpl::map([](TextWithEntities &&text, bool active) {
		const auto spoilered = std::any_of(
			text.entities.begin(),
			text.entities.end(),
			[](const EntityInText &entity) {
				return entity.type() == EntityType::Spoiler;
			});
		return (active && !text.empty() && !spoilered)
			? Ui::Text::Wrapped(std::move(text), EntityType::Spoiler)
			: std::move(text);
	});
}

void spoilerImage(QImage &image) {
	if (image.isNull()) {
		return;
	}
	constexpr auto kFormat = QImage::Format_ARGB32_Premultiplied;
	const auto size = image.size();
	const auto small = QSize(
		std::max(2, size.width() / 8),
		std::max(2, size.height() / 8));
	auto blurred = image.convertToFormat(kFormat).scaled(
		small,
		Qt::IgnoreAspectRatio,
		Qt::SmoothTransformation).scaled(
			size,
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation);
	blurred.setDevicePixelRatio(image.devicePixelRatio());
	{
		auto p = QPainter(&blurred);
		p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
		p.drawImage(QRect(QPoint(), size / image.devicePixelRatio()), image);
		p.setCompositionMode(QPainter::CompositionMode_SourceAtop);
		Ui::FillSpoilerRect(
			p,
			QRect(QPoint(), size / image.devicePixelRatio()),
			Ui::DefaultImageSpoiler().frame(0));
	}
	image = std::move(blurred);
}

} // namespace AyuFeatures::StreamerMode
