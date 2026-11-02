/*
This file is part of FAgram Desktop,
the unofficial desktop client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/fagramdesktop/fadesktop/blob/dev/LEGAL
*/
#pragma once

#include "ayu/ui/fagram/fagram_design.h"
#include "settings/settings_common.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"

namespace FA::Ui {

class NavDrawerButton : public ::Ui::SettingsButton {
public:
	NavDrawerButton(
		QWidget *parent,
		rpl::producer<QString> text,
		const style::SettingsButton &st,
		Settings::IconDescriptor &&descriptor = {});
	NavDrawerButton(
		QWidget *parent,
		rpl::producer<TextWithEntities> text,
		const style::SettingsButton &st,
		Settings::IconDescriptor &&descriptor = {});

	void setIcon(const style::icon *icon);
	void setDescriptor(Settings::IconDescriptor &&descriptor);

	[[nodiscard]] static int IconLeft();

protected:
	void paintEvent(QPaintEvent *e) override;
	QImage prepareRippleMask() const override;
	QPoint prepareRippleStartPosition() const override;

private:
	Settings::IconDescriptor _descriptor;
	const style::icon *_icon = nullptr;

};

[[nodiscard]] object_ptr<NavDrawerButton> CreateNavDrawerButton(
	not_null<QWidget*> parent,
	rpl::producer<QString> text,
	const style::SettingsButton &st,
	Settings::IconDescriptor &&descriptor = {});

not_null<NavDrawerButton*> AddNavDrawerButton(
	not_null<::Ui::VerticalLayout*> container,
	rpl::producer<QString> text,
	const style::SettingsButton &st,
	Settings::IconDescriptor &&descriptor = {});

not_null<::Ui::RpWidget*> AddNavDrawerDivider(
	not_null<::Ui::VerticalLayout*> container);

// Main menu entries that follow the current design style.
[[nodiscard]] object_ptr<::Ui::SettingsButton> CreateMainMenuButton(
	not_null<QWidget*> parent,
	rpl::producer<QString> text,
	const style::SettingsButton &st,
	Settings::IconDescriptor &&descriptor = {});

not_null<::Ui::SettingsButton*> AddMainMenuButton(
	not_null<::Ui::VerticalLayout*> container,
	rpl::producer<QString> text,
	const style::SettingsButton &st,
	Settings::IconDescriptor &&descriptor = {});

void AddMainMenuDivider(not_null<::Ui::VerticalLayout*> container);

} // namespace FA::Ui
