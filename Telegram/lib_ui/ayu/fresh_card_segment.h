// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
//
// The card shapes and the switch are ported from FAgram Desktop
// (https://github.com/fagramdesktop/fadesktop), licensed under GPLv3.
#pragma once

#include <QtCore/QRectF>
#include <QtCore/QSize>
#include <QtGui/QImage>
#include <QtGui/QPainterPath>

class QPainter;
class QWidget;

namespace AyuUiSettings {

enum class CardSegment : unsigned char {
	None,
	Single,
	Top,
	Middle,
	Bottom,
	// A button inside a card block: no rounding and no gaps.
	Flat,
};

// The radii of the Fagram cards in pixels, scaled by the interface scale.
[[nodiscard]] int CardRadius();
[[nodiscard]] int CardSegmentRadius();
// Half of the gap between two neighbour rows of a card.
[[nodiscard]] int CardSegmentHalfGap();

[[nodiscard]] QPainterPath CardSegmentPath(
	const QRectF &rect,
	CardSegment position);

// The part of a row (in its own coordinates) that the segment occupies.
[[nodiscard]] QRectF CardSegmentRect(
	const QSize &size,
	CardSegment position);

[[nodiscard]] QImage CardSegmentMask(
	const QSize &size,
	CardSegment position);

// Is the widget inside a layout that shows its rows as the Fagram cards.
[[nodiscard]] bool InCards(const QWidget *widget);

void PaintMd3Switch(
	QPainter &p,
	double x,
	double y,
	double toggled,
	double width,
	double height);

} // namespace AyuUiSettings
