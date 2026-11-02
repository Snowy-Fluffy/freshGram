/*
This file is part of FAgram Desktop,
the unofficial desktop client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/fagramdesktop/fadesktop/blob/dev/LEGAL
*/
#include "ayu/ui/fagram/fagram_nav_drawer.h"
#include "ayu/ui/fagram/fagram_design.h"

#include "base/flat_map.h"
#include "ui/painter.h"
#include "ui/widgets/shadow.h"
#include "ui/effects/ripple_animation.h"
#include "styles/style_ayu_styles.h"
#include "styles/style_basic.h"
#include "styles/style_window.h"
#include "styles/style_widgets.h"
#include "styles/style_window_main_menu.h"

#include <QtGui/QPainterPath>

namespace FA::Ui {
namespace {

// The badges and the toggle animation read the row geometry from the style.
[[nodiscard]] const style::SettingsButton &NavDrawerStyle(
		const style::SettingsButton &st) {
	static auto cache = base::flat_map<
		const style::SettingsButton*,
		std::unique_ptr<style::SettingsButton>>();
	auto &result = cache[&st];
	if (!result) {
		result = std::make_unique<style::SettingsButton>(st);
		const auto top = (st::fagramNavItemHeight - st.style.font->height) / 2;
		result->padding = style::margins(
			st::fagramNavTextLeft,
			top,
			st::fagramNavTextRight,
			st::fagramNavItemHeight - top - st.height);
		result->toggleSkip = st::fagramNavToggleSkip;
	}
	return *result;
}

[[nodiscard]] QRect PillRect(QSize size) {
	return QRect(QPoint(), size).marginsRemoved(st::fagramNavPillMargins);
}

} // namespace

NavDrawerButton::NavDrawerButton(
	QWidget *parent,
	rpl::producer<QString> text,
	const style::SettingsButton &st,
	Settings::IconDescriptor &&descriptor)
: SettingsButton(parent, std::move(text), NavDrawerStyle(st))
, _descriptor(std::move(descriptor))
, _icon(_descriptor.icon) {
}

NavDrawerButton::NavDrawerButton(
	QWidget *parent,
	rpl::producer<TextWithEntities> text,
	const style::SettingsButton &st,
	Settings::IconDescriptor &&descriptor)
: SettingsButton(parent, std::move(text), NavDrawerStyle(st))
, _descriptor(std::move(descriptor))
, _icon(_descriptor.icon) {
}

void NavDrawerButton::setIcon(const style::icon *icon) {
	_icon = icon;
	update();
}

void NavDrawerButton::setDescriptor(Settings::IconDescriptor &&descriptor) {
	_descriptor = std::move(descriptor);
	_icon = _descriptor.icon;
	update();
}

int NavDrawerButton::IconLeft() {
	return st::fagramNavIconLeft;
}

void NavDrawerButton::paintEvent(QPaintEvent *e) {
	Painter p(this);
	const auto outerw = width();
	const auto outerh = height();

	const auto pillRect = PillRect(size());
	const auto radius = pillRect.height() / 2.0;

	const auto paintOver = (isOver() || isDown()) && !isDisabled();
	if (paintOver) {
		PainterHighQualityEnabler hq(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(pillRect, radius, radius);
	}

	paintRipple(p, 0, 0);

	if (_icon) {
		const auto iconLeft = IconLeft();
		const auto iconTop = (outerh - _icon->height()) / 2;
		if (paintOver) {
			_icon->paint(p, QPoint(iconLeft, iconTop), outerw, st::windowBoldFgOver->c);
		} else {
			_icon->paint(p, QPoint(iconLeft, iconTop), outerw);
		}
	}

	paintText(p, paintOver, outerw);
	paintToggle(p, outerw);
}

QImage NavDrawerButton::prepareRippleMask() const {
	const auto pillRect = PillRect(size());
	const auto radius = pillRect.height() / 2.0;
	return ::Ui::RippleAnimation::MaskByDrawer(size(), false, [&](QPainter &p) {
		PainterHighQualityEnabler hq(p);
		p.setPen(Qt::NoPen);
		p.setBrush(Qt::white);
		p.drawRoundedRect(pillRect, radius, radius);
	});
}

QPoint NavDrawerButton::prepareRippleStartPosition() const {
	const auto local = mapFromGlobal(QCursor::pos());
	return PillRect(size()).contains(local)
		? local
		: DisabledRippleStartPosition();
}

object_ptr<NavDrawerButton> CreateNavDrawerButton(
		not_null<QWidget*> parent,
		rpl::producer<QString> text,
		const style::SettingsButton &st,
		Settings::IconDescriptor &&descriptor) {
	return object_ptr<NavDrawerButton>(
		parent,
		std::move(text),
		st,
		std::move(descriptor));
}

not_null<NavDrawerButton*> AddNavDrawerButton(
		not_null<::Ui::VerticalLayout*> container,
		rpl::producer<QString> text,
		const style::SettingsButton &st,
		Settings::IconDescriptor &&descriptor) {
	return container->add(
		CreateNavDrawerButton(
			container,
			std::move(text),
			st,
			std::move(descriptor)));
}

not_null<::Ui::RpWidget*> AddNavDrawerDivider(
		not_null<::Ui::VerticalLayout*> container) {
	const auto divider = container->add(
		object_ptr<::Ui::RpWidget>(container),
		st::fagramNavDividerMargins);
	divider->resize(divider->width(), st::lineWidth);
	divider->paintRequest(
	) | rpl::on_next([divider] {
		Painter p(divider);
		p.fillRect(divider->rect(), st::shadowFg);
	}, divider->lifetime());
	return divider;
}

object_ptr<::Ui::SettingsButton> CreateMainMenuButton(
		not_null<QWidget*> parent,
		rpl::producer<QString> text,
		const style::SettingsButton &st,
		Settings::IconDescriptor &&descriptor) {
	if (Enabled()) {
		return CreateNavDrawerButton(
			parent,
			std::move(text),
			st,
			std::move(descriptor));
	}
	return Settings::CreateButtonWithIcon(
		parent,
		std::move(text),
		st,
		std::move(descriptor));
}

not_null<::Ui::SettingsButton*> AddMainMenuButton(
		not_null<::Ui::VerticalLayout*> container,
		rpl::producer<QString> text,
		const style::SettingsButton &st,
		Settings::IconDescriptor &&descriptor) {
	if (Enabled()) {
		return AddNavDrawerButton(
			container,
			std::move(text),
			st,
			std::move(descriptor));
	}
	return Settings::AddButtonWithIcon(
		container,
		std::move(text),
		st,
		std::move(descriptor));
}

void AddMainMenuDivider(not_null<::Ui::VerticalLayout*> container) {
	if (Enabled()) {
		AddNavDrawerDivider(container);
		return;
	}
	container->add(
		object_ptr<::Ui::PlainShadow>(container),
		{ 0, st::mainMenuSkip, 0, st::mainMenuSkip });
}

} // namespace FA::Ui
