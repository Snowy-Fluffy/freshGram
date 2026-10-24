// This is the source code of freshGram.
#include "ayu/ui/components/recognition_picker.h"

#include "ayu/features/recognition/text_recognition.h"
#include "lang/lang_keys.h"
#include "lang_auto.h"
#include "ui/painter.h"
#include "ui/text/text_entity.h"
#include "ui/toast/toast.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "styles/style_ayu_styles.h"
#include "styles/style_basic.h"
#include "styles/style_widgets.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>

#include <algorithm>
#include <cmath>

namespace AyuUi {
namespace {

constexpr auto kDimAlpha = 150;

struct Sides {
	bool left = false;
	bool top = false;
	bool right = false;
	bool bottom = false;
	bool move = false;
};

[[nodiscard]] QRectF AdjustedRect(
		Sides sides,
		QRectF start,
		QPointF delta,
		QRectF bounds,
		qreal minSide) {
	if (sides.move) {
		auto result = start.translated(delta);
		if (result.left() < bounds.left()) {
			result.moveLeft(bounds.left());
		}
		if (result.top() < bounds.top()) {
			result.moveTop(bounds.top());
		}
		if (result.right() > bounds.right()) {
			result.moveRight(bounds.right());
		}
		if (result.bottom() > bounds.bottom()) {
			result.moveBottom(bounds.bottom());
		}
		return result;
	}
	auto left = start.left();
	auto top = start.top();
	auto right = start.right();
	auto bottom = start.bottom();
	if (sides.left) {
		left = std::clamp(
			left + delta.x(),
			bounds.left(),
			std::max(right - minSide, bounds.left()));
	}
	if (sides.right) {
		right = std::clamp(
			right + delta.x(),
			std::min(left + minSide, bounds.right()),
			bounds.right());
	}
	if (sides.top) {
		top = std::clamp(
			top + delta.y(),
			bounds.top(),
			std::max(bottom - minSide, bounds.top()));
	}
	if (sides.bottom) {
		bottom = std::clamp(
			bottom + delta.y(),
			std::min(top + minSide, bounds.bottom()),
			bounds.bottom());
	}
	return QRectF(QPointF(left, top), QPointF(right, bottom));
}

} // namespace

RecognitionPicker::RecognitionPicker(
	not_null<Ui::RpWidget*> parent,
	QImage image,
	Fn<QRect()> contentRect,
	QString languages,
	Fn<void()> closeRequested)
: RpWidget(parent)
, _image(std::move(image))
, _contentRect(std::move(contentRect))
, _languages(std::move(languages))
, _closeRequested(std::move(closeRequested)) {
	setFocusPolicy(Qt::StrongFocus);
	setMouseTracking(true);

	_recognize = Ui::CreateChild<Ui::RoundButton>(
		this,
		tr::fresh_RecognizeButton(),
		st::defaultActiveButton);
	_cancelButton = Ui::CreateChild<Ui::RoundButton>(
		this,
		tr::lng_cancel(),
		st::defaultLightButton);
	_recognize->setClickedCallback([=] { recognize(); });
	_cancelButton->setClickedCallback([=] { cancelOrClose(); });

	_panel = Ui::CreateChild<Ui::RpWidget>(this);
	_panel->hide();
	_panel->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(_panel);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(20, 20, 20, 235));
		p.drawRoundedRect(
			_panel->rect(),
			st::recognitionPanelRadius,
			st::recognitionPanelRadius);
	}, _panel->lifetime());
	_scroll = Ui::CreateChild<Ui::ScrollArea>(_panel, st::defaultScrollArea);
	_text = _scroll->setOwnedWidget(object_ptr<Ui::FlatLabel>(
		_scroll,
		QString(),
		st::defaultFlatLabel));
	_text->setSelectable(true);
	_text->setTextColorOverride(QColor(255, 255, 255));
	_copy = Ui::CreateChild<Ui::RoundButton>(
		_panel,
		tr::lng_context_copy_text(),
		st::defaultActiveButton);
	_close = Ui::CreateChild<Ui::RoundButton>(
		_panel,
		tr::lng_close(),
		st::defaultLightButton);
	_copy->setClickedCallback([=] {
		if (_resultText.isEmpty()) {
			return;
		}
		TextUtilities::SetClipboardText(
			TextForMimeData::Simple(_resultText));
		Ui::Toast::Show(this, tr::lng_text_copied(tr::now));
	});
	_close->setClickedCallback([=] { cancelOrClose(); });

	parent->sizeValue(
	) | rpl::on_next([=](QSize size) {
		setGeometry(0, 0, size.width(), size.height());
	}, lifetime());

	updateControls();
	show();
	raise();
	setFocus();
}

RecognitionPicker::~RecognitionPicker() {
	if (_cancel) {
		_cancel->store(true);
	}
}

QRectF RecognitionPicker::selectionRect() const {
	const auto c = QRectF(_contentRect());
	return QRectF(
		c.left() + _selection.left() * c.width(),
		c.top() + _selection.top() * c.height(),
		_selection.width() * c.width(),
		_selection.height() * c.height());
}

void RecognitionPicker::setSelectionRect(QRectF rect) {
	const auto c = QRectF(_contentRect());
	if (c.isEmpty()) {
		return;
	}
	_selection = QRectF(
		(rect.left() - c.left()) / c.width(),
		(rect.top() - c.top()) / c.height(),
		rect.width() / c.width(),
		rect.height() / c.height());
}

auto RecognitionPicker::partAt(QPoint position) const -> Part {
	const auto s = selectionRect();
	const auto tolerance = qreal(st::recognitionHandleTolerance);
	const auto p = QPointF(position);
	const auto isNear = [&](qreal a, qreal b) {
		return std::abs(a - b) <= tolerance;
	};
	const auto inX = (p.x() >= s.left() - tolerance)
		&& (p.x() <= s.right() + tolerance);
	const auto inY = (p.y() >= s.top() - tolerance)
		&& (p.y() <= s.bottom() + tolerance);
	if (!inX || !inY) {
		return Part::None;
	}
	const auto left = isNear(p.x(), s.left());
	const auto right = isNear(p.x(), s.right());
	const auto top = isNear(p.y(), s.top());
	const auto bottom = isNear(p.y(), s.bottom());
	if (left && top) {
		return Part::TopLeft;
	} else if (right && top) {
		return Part::TopRight;
	} else if (right && bottom) {
		return Part::BottomRight;
	} else if (left && bottom) {
		return Part::BottomLeft;
	} else if (top) {
		return Part::Top;
	} else if (bottom) {
		return Part::Bottom;
	} else if (left) {
		return Part::Left;
	} else if (right) {
		return Part::Right;
	}
	return s.contains(p) ? Part::Move : Part::None;
}

void RecognitionPicker::updateCursor(Part part) {
	switch (part) {
	case Part::TopLeft:
	case Part::BottomRight: setCursor(Qt::SizeFDiagCursor); break;
	case Part::TopRight:
	case Part::BottomLeft: setCursor(Qt::SizeBDiagCursor); break;
	case Part::Top:
	case Part::Bottom: setCursor(Qt::SizeVerCursor); break;
	case Part::Left:
	case Part::Right: setCursor(Qt::SizeHorCursor); break;
	case Part::Move: setCursor(Qt::SizeAllCursor); break;
	default: setCursor(Qt::ArrowCursor); break;
	}
}

void RecognitionPicker::updateControls() {
	if (!_recognize || !_panel) {
		return;
	}
	const auto skip = st::recognitionButtonsSkip;
	const auto bottom = height() - st::recognitionButtonsBottom;
	const auto working = (_state == State::Working);
	const auto selecting = (_state == State::Selecting);
	_recognize->setVisible(selecting);
	_cancelButton->setVisible(selecting || working);
	if (selecting) {
		const auto total = _recognize->width() + skip + _cancelButton->width();
		const auto left = (width() - total) / 2;
		_recognize->move(left, bottom - _recognize->height());
		_cancelButton->move(
			left + _recognize->width() + skip,
			bottom - _cancelButton->height());
	} else if (working) {
		_cancelButton->move(
			(width() - _cancelButton->width()) / 2,
			bottom - _cancelButton->height());
	}

	_panel->setVisible(_state == State::Result);
	if (_state != State::Result) {
		return;
	}
	auto area = _contentRect().intersected(rect());
	if (area.width() < st::recognitionPanelMinWidth
		|| area.height() < st::recognitionPanelMinHeight) {
		area = rect();
	}
	const auto c = area.marginsRemoved(QMargins(
		st::recognitionPanelMargin,
		st::recognitionPanelMargin,
		st::recognitionPanelMargin,
		st::recognitionPanelMargin));
	const auto pad = st::recognitionPanelPadding;
	const auto outer = std::min(c.width(), st::recognitionPanelMaxWidth);
	const auto inner = std::max(outer - pad.left() - pad.right(), 1);
	_text->resizeToWidth(inner);
	const auto buttons = _close->height();
	const auto maxContent = std::max(
		st::recognitionPanelMaxHeight
			- pad.top()
			- pad.bottom()
			- st::recognitionPanelButtonsTop
			- buttons,
		1);
	const auto content = std::min(_text->height(), maxContent);
	const auto total = pad.top()
		+ content
		+ st::recognitionPanelButtonsTop
		+ buttons
		+ pad.bottom();
	const auto top = c.top() + std::max((c.height() - total) / 2, 0);
	_panel->setGeometry(
		c.left() + (c.width() - outer) / 2,
		top,
		outer,
		total);
	_scroll->setGeometry(pad.left(), pad.top(), inner, content);
	const auto buttonsTop = pad.top()
		+ content
		+ st::recognitionPanelButtonsTop;
	_close->move(outer - pad.right() - _close->width(), buttonsTop);
	_copy->move(
		_close->x() - st::recognitionButtonsSkip - _copy->width(),
		buttonsTop);
}

void RecognitionPicker::recognize() {
	const auto selection = selectionRect();
	const auto content = QRectF(_contentRect());
	if (content.isEmpty() || _image.isNull()) {
		return;
	}
	const auto scaleX = _image.width() / content.width();
	const auto scaleY = _image.height() / content.height();
	const auto crop = QRectF(
		(selection.left() - content.left()) * scaleX,
		(selection.top() - content.top()) * scaleY,
		selection.width() * scaleX,
		selection.height() * scaleY
	).toAlignedRect().intersected(_image.rect());
	if (crop.width() < 4 || crop.height() < 4) {
		return;
	}
	_state = State::Working;
	_status = tr::fresh_Recognizing(tr::now);
	updateControls();
	update();

	const auto generation = ++_generation;
	_cancel = FreshRecognition::RecognizeTextAsync({
		.image = _image.copy(crop),
		.languages = _languages,
	}, crl::guard(this, [=](FreshRecognition::TextResult result) {
		if (generation != _generation) {
			return;
		}
		_cancel = nullptr;
		if (result.cancelled) {
			return;
		} else if (!result.ok) {
			showText(tr::fresh_RecognizeFailed(
				tr::now,
				lt_error,
				FreshRecognition::RecognitionErrorText(result.error)), false);
		} else if (result.text.isEmpty()) {
			showText(tr::fresh_RecognizeNoText(tr::now), false);
		} else {
			showText(result.text, true);
		}
	}));
}

void RecognitionPicker::showText(const QString &text, bool copyable) {
	_resultText = copyable ? text : QString();
	_state = State::Result;
	_text->setText(text);
	updateControls();
	_copy->setVisible(copyable);
	update();
}

void RecognitionPicker::cancelOrClose() {
	if (_state == State::Working) {
		++_generation;
		if (_cancel) {
			_cancel->store(true);
			_cancel = nullptr;
		}
		_state = State::Selecting;
		updateControls();
		update();
	} else if (_closeRequested) {
		_closeRequested();
	}
}

void RecognitionPicker::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto selection = selectionRect();

	auto dim = QPainterPath();
	dim.addRect(QRectF(rect()));
	dim.addRect(selection);
	dim.setFillRule(Qt::OddEvenFill);
	p.fillPath(dim, QColor(0, 0, 0, kDimAlpha));

	p.setPen(QPen(Qt::white, st::recognitionBorder));
	p.setBrush(Qt::NoBrush);
	p.drawRect(selection);

	if (_state == State::Selecting) {
		const auto radius = qreal(st::recognitionHandleRadius);
		const auto points = {
			selection.topLeft(),
			QPointF(selection.center().x(), selection.top()),
			selection.topRight(),
			QPointF(selection.right(), selection.center().y()),
			selection.bottomRight(),
			QPointF(selection.center().x(), selection.bottom()),
			selection.bottomLeft(),
			QPointF(selection.left(), selection.center().y()),
		};
		p.setPen(QPen(QColor(40, 40, 40), 1));
		p.setBrush(Qt::white);
		for (const auto &point : points) {
			p.drawEllipse(point, radius, radius);
		}
		p.setPen(Qt::white);
		p.setFont(st::semiboldFont);
		const auto text = tr::fresh_RecognizeHint(tr::now);
		const auto hint = QRect(
			(width() - st::semiboldFont->width(text)) / 2,
			st::recognitionHintTop,
			st::semiboldFont->width(text),
			st::semiboldFont->height);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, 170));
		p.drawRoundedRect(
			hint.marginsAdded(QMargins(12, 6, 12, 6)),
			10,
			10);
		p.setPen(Qt::white);
		p.drawText(hint, Qt::AlignCenter, text);
	} else if (_state == State::Working) {
		p.setFont(st::semiboldFont);
		const auto text = _status;
		const auto size = QSize(
			st::semiboldFont->width(text),
			st::semiboldFont->height);
		const auto box = QRect(
			QPoint(
				int(selection.center().x() - size.width() / 2),
				int(selection.center().y() - size.height() / 2)),
			size);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, 190));
		p.drawRoundedRect(
			box.marginsAdded(QMargins(14, 8, 14, 8)),
			10,
			10);
		p.setPen(Qt::white);
		p.drawText(box, Qt::AlignCenter, text);
	}
}

void RecognitionPicker::mousePressEvent(QMouseEvent *e) {
	setFocus();
	if (e->button() != Qt::LeftButton) {
		e->accept();
		return;
	}
	if (_state == State::Selecting) {
		_dragging = partAt(e->pos());
		_dragStart = e->pos();
		_dragStartRect = selectionRect();
	}
	e->accept();
}

void RecognitionPicker::mouseMoveEvent(QMouseEvent *e) {
	if (!(e->buttons() & Qt::LeftButton)) {
		_dragging = Part::None;
	}
	if (_state != State::Selecting) {
		updateCursor(Part::None);
		return;
	}
	if (_dragging == Part::None) {
		updateCursor(partAt(e->pos()));
		return;
	}
	const auto delta = QPointF(e->pos() - _dragStart);
	const auto part = _dragging;
	setSelectionRect(AdjustedRect(
		Sides{
			.left = (part == Part::TopLeft
				|| part == Part::Left
				|| part == Part::BottomLeft),
			.top = (part == Part::TopLeft
				|| part == Part::Top
				|| part == Part::TopRight),
			.right = (part == Part::TopRight
				|| part == Part::Right
				|| part == Part::BottomRight),
			.bottom = (part == Part::BottomLeft
				|| part == Part::Bottom
				|| part == Part::BottomRight),
			.move = (part == Part::Move),
		},
		_dragStartRect,
		delta,
		QRectF(_contentRect()),
		qreal(st::recognitionMinSide)));
	update();
}

void RecognitionPicker::mouseReleaseEvent(QMouseEvent *e) {
	_dragging = Part::None;
	updateCursor(partAt(e->pos()));
	e->accept();
}

void RecognitionPicker::mouseDoubleClickEvent(QMouseEvent *e) {
	e->accept();
}

void RecognitionPicker::wheelEvent(QWheelEvent *e) {
	e->accept();
}

void RecognitionPicker::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Escape) {
		cancelOrClose();
		e->accept();
	} else if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
		if (_state == State::Selecting) {
			recognize();
		}
		e->accept();
	} else {
		e->accept();
	}
}

void RecognitionPicker::resizeEvent(QResizeEvent *e) {
	_dragging = Part::None;
	updateControls();
	Ui::PostponeCall(this, [=] { updateControls(); });
}

void RecognitionPicker::refreshLayout() {
	updateControls();
	update();
}

} // namespace AyuUi
