// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
//
// The card shapes and the switch are ported from FAgram Desktop
// (https://github.com/fagramdesktop/fadesktop), licensed under GPLv3.
#include "ayu/fresh_card_segment.h"

#include "ui/effects/animation_value.h"
#include "ui/effects/animation_value_f.h"
#include "ui/effects/ripple_animation.h"
#include "ui/painter.h"
#include "ui/style/style_core_direction.h"
#include "ui/style/style_core_icon.h"
#include "ui/style/style_core_scale.h"
#include "styles/palette.h"

#include <QtCore/QVariant>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QWidget>

namespace AyuUiSettings {

int CardRadius() {
	return style::ConvertScale(24);
}

int CardSegmentRadius() {
	return style::ConvertScale(4);
}

int CardSegmentHalfGap() {
	return style::ConvertScale(1);
}

QPainterPath CardSegmentPath(const QRectF &r, CardSegment position) {
	const auto large = double(CardRadius());
	const auto small = double(CardSegmentRadius());
	const auto flat = (position == CardSegment::Flat);
	const auto top = (position == CardSegment::Top
		|| position == CardSegment::Single);
	const auto bottom = (position == CardSegment::Bottom
		|| position == CardSegment::Single);
	const auto rtl = flat ? 0. : top ? large : small;
	const auto rtr = rtl;
	const auto rbr = flat ? 0. : bottom ? large : small;
	const auto rbl = rbr;

	auto path = QPainterPath();
	const auto arc = [&](double x, double y, double radius, double from) {
		if (radius > 0.) {
			path.arcTo(QRectF(x, y, 2 * radius, 2 * radius), from, -90);
		}
	};
	path.moveTo(r.left() + rtl, r.top());
	path.lineTo(r.right() - rtr, r.top());
	arc(r.right() - 2 * rtr, r.top(), rtr, 90);
	path.lineTo(r.right(), r.bottom() - rbr);
	arc(r.right() - 2 * rbr, r.bottom() - 2 * rbr, rbr, 0);
	path.lineTo(r.left() + rbl, r.bottom());
	arc(r.left(), r.bottom() - 2 * rbl, rbl, 270);
	path.lineTo(r.left(), r.top() + rtl);
	arc(r.left(), r.top(), rtl, 180);
	path.closeSubpath();
	return path;
}

QRectF CardSegmentRect(const QSize &size, CardSegment position) {
	const auto gap = (position == CardSegment::Flat)
		? 0.
		: double(CardSegmentHalfGap());
	const auto top = (position == CardSegment::Top
		|| position == CardSegment::Single) ? 0. : gap;
	const auto bottom = (position == CardSegment::Bottom
		|| position == CardSegment::Single) ? 0. : gap;
	return QRectF(0., top, size.width(), size.height() - top - bottom);
}

QImage CardSegmentMask(const QSize &size, CardSegment position) {
	return Ui::RippleAnimation::MaskByDrawer(size, false, [&](QPainter &p) {
		PainterHighQualityEnabler hq(p);
		p.setPen(Qt::NoPen);
		p.setBrush(Qt::white);
		p.drawPath(CardSegmentPath(
			CardSegmentRect(size, position),
			position));
	});
}

bool InCards(const QWidget *widget) {
	for (auto w = widget; w; w = w->parentWidget()) {
		if (w->property("fa_cards").toBool()) {
			return true;
		}
	}
	return false;
}

void PaintMd3Switch(
		QPainter &p,
		double x,
		double y,
		double toggled,
		double switchWidth,
		double switchHeight,
		const style::internal::Icon *lockIcon) {
	PainterHighQualityEnabler hq(p);

	const auto opacity = p.opacity();
	const auto trackRadius = switchHeight / 2.0;
	const auto border = style::ConvertScaleExact(2.0);

	if (toggled > 0.0) {
		p.setOpacity(opacity * toggled);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowActiveTextFg);
		p.drawRoundedRect(
			QRectF(x, y, switchWidth, switchHeight),
			trackRadius,
			trackRadius);
	}

	if (toggled < 1.0) {
		const auto half = border / 2.0;
		p.setOpacity(opacity * (1.0 - toggled));
		p.setPen(QPen(st::windowSubTextFg->c, border));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(
			QRectF(
				x + half,
				y + half,
				switchWidth - border,
				switchHeight - border),
			trackRadius - half,
			trackRadius - half);
	}
	p.setOpacity(opacity);

	const auto thumbDiameter = std::round(switchHeight * 0.72);
	const auto margin = (switchHeight - thumbDiameter) / 2.0;
	const auto offLeft = x + margin;
	const auto onLeft = x + switchWidth - margin - thumbDiameter;
	const auto thumbX = style::RightToLeft()
		? anim::interpolateF(onLeft, offLeft, toggled)
		: anim::interpolateF(offLeft, onLeft, toggled);
	const auto thumbY = y + margin;
	const auto thumbColor = anim::color(
		st::windowSubTextFg->c,
		st::windowBg->c,
		toggled);

	p.setPen(Qt::NoPen);
	p.setBrush(thumbColor);
	p.drawEllipse(QRectF(thumbX, thumbY, thumbDiameter, thumbDiameter));

	const auto cx = thumbX + thumbDiameter / 2.0;
	const auto cy = thumbY + thumbDiameter / 2.0;
	const auto scale = thumbDiameter / 20.0;

	if (lockIcon) {
		const auto color = anim::color(
			st::windowBg->c,
			st::windowActiveTextFg->c,
			toggled);
		lockIcon->paint(
			p,
			QPoint(
				int(std::round(cx - lockIcon->width() / 2.0)),
				int(std::round(cy - lockIcon->height() / 2.0))),
			int(std::round(2 * cx)),
			color);
		return;
	}

	if (toggled < 0.95) {
		p.setOpacity(opacity * (1.0 - toggled));
		p.setPen(QPen(
			st::windowBg->c,
			1.8 * scale,
			Qt::SolidLine,
			Qt::RoundCap,
			Qt::RoundJoin));
		p.setBrush(Qt::NoBrush);

		const auto crossRadius = 3.2 * scale;
		p.drawLine(
			QPointF(cx - crossRadius, cy - crossRadius),
			QPointF(cx + crossRadius, cy + crossRadius));
		p.drawLine(
			QPointF(cx + crossRadius, cy - crossRadius),
			QPointF(cx - crossRadius, cy + crossRadius));
	}

	if (toggled > 0.05) {
		p.setOpacity(opacity * toggled);
		p.setPen(QPen(
			st::windowActiveTextFg->c,
			1.8 * scale,
			Qt::SolidLine,
			Qt::RoundCap,
			Qt::RoundJoin));
		p.setBrush(Qt::NoBrush);

		auto check = QPainterPath();
		check.moveTo(cx - 3.5 * scale, cy - 0.2 * scale);
		check.lineTo(cx - 1.0 * scale, cy + 2.5 * scale);
		check.lineTo(cx + 4.0 * scale, cy - 2.5 * scale);
		if (style::RightToLeft()) {
			check = QTransform(-1, 0, 0, 1, 2 * cx, 0).map(check);
		}
		p.drawPath(check);
	}
	p.setOpacity(opacity);
}

} // namespace AyuUiSettings
