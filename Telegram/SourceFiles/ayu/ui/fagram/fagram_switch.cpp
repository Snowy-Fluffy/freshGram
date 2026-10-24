/*
This file is part of FAgram Desktop,
the unofficial desktop client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/fagramdesktop/fadesktop/blob/dev/LEGAL
*/
#include "ayu/ui/fagram/fagram_switch.h"

#include "ayu/fresh_card_segment.h"

#include "ui/painter.h"
#include "ui/effects/animation_value_f.h"
#include "styles/style_basic.h"
#include "styles/style_window.h"

#include <QtGui/QPainterPath>

namespace FA::Ui {

void PaintMd3Switch(
		QPainter &p,
		float64 x,
		float64 y,
		float64 toggled,
		float64 switchWidth,
		float64 switchHeight) {
	AyuUiSettings::PaintMd3Switch(
		p,
		x,
		y,
		toggled,
		switchWidth,
		switchHeight);
}

MaterialSwitch::MaterialSwitch(QWidget *parent, bool checked)
: RippleButton(parent, st::defaultSettingsButton.ripple)
, _checked(checked) {
	addClickHandler([this] {
		setChecked(!_checked);
	});
	resize(sizeHint());
}

void MaterialSwitch::setChecked(bool checked, anim::type animated) {
	if (_checked == checked) {
		return;
	}
	_checked = checked;
	_checkedChanges.fire_copy(_checked);
	if (animated == anim::type::instant) {
		_animation.stop();
		update();
	} else {
		_animation.start(
			[this] { update(); },
			_checked ? 0.0 : 1.0,
			_checked ? 1.0 : 0.0,
			200,
			anim::easeOutCubic);
	}
}

void MaterialSwitch::paintEvent(QPaintEvent *e) {
	Painter p(this);
	paintRipple(p, 0, 0);

	const auto switchWidth = 48.0;
	const auto switchHeight = 28.0;
	const auto toggleX = (width() - switchWidth) / 2.0;
	const auto toggleY = (height() - switchHeight) / 2.0;
	const auto toggled = _animation.value(_checked ? 1.0 : 0.0);

	PaintMd3Switch(p, toggleX, toggleY, toggled, switchWidth, switchHeight);
}

QSize MaterialSwitch::sizeHint() const {
	return QSize(48, 28);
}

} // namespace FA::Ui
