/*
This file is part of FAgram Desktop,
the unofficial desktop client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/fagramdesktop/fadesktop/blob/dev/LEGAL
*/
#include "ayu/ui/fagram/fagram_cards.h"

#include "ayu/fresh_card_segment.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/wrap/vertical_layout.h"
#include "base/flat_set.h"
#include "styles/palette.h"
#include "styles/style_ayu_styles.h"

#include <QtGui/QPainterPath>

namespace FA::Ui {
namespace {

using Segment = AyuUiSettings::CardSegment;

constexpr auto kRowProperty = "fa_card_row";
constexpr auto kBlockProperty = "fa_card_block";

struct Item {
	QWidget *row = nullptr;
	int top = 0;
	int bottom = 0;
	bool isRow = false;
};

// A button that lies over a row without being a row of the layout, like the
// toggle half of a split toggle.
struct Overlay {
	::Ui::RippleButton *button = nullptr;
	int center = 0;
};

[[nodiscard]] bool IsRowButton(const QWidget *widget) {
	if (dynamic_cast<const ::Ui::SettingsButton*>(widget)
		|| dynamic_cast<const ::Ui::Checkbox*>(widget)) {
		return true;
	}
	// Other list rows, the folders or the sessions, span the whole width.
	const auto button = dynamic_cast<const ::Ui::RippleButton*>(widget);
	const auto parent = widget->parentWidget();
	return button
		&& !dynamic_cast<const ::Ui::RoundButton*>(widget)
		&& !dynamic_cast<const ::Ui::FlatButton*>(widget)
		&& parent
		&& (widget->width() * 10 >= parent->width() * 9);
}

[[nodiscard]] bool IsRowType(const QWidget *widget) {
	return IsRowButton(widget)
		|| widget->property(kRowProperty).toBool()
		|| widget->property(kBlockProperty).toBool();
}

void CollectButtons(
		not_null<QWidget*> parent,
		std::vector<::Ui::SettingsButton*> &buttons) {
	for (const auto child : parent->children()) {
		const auto widget = qobject_cast<QWidget*>(child);
		if (!widget || widget->isHidden()) {
			continue;
		}
		if (const auto button = dynamic_cast<::Ui::SettingsButton*>(widget)) {
			buttons.push_back(button);
		} else {
			CollectButtons(widget, buttons);
		}
	}
}

// The buttons inside a block have no background of their own and are
// rounded only where they touch the rounded corners of the block.
void ApplyBlockSegments(not_null<QWidget*> block, Segment position) {
	const auto height = block->height();
	const auto roundTop = (position == Segment::Top
		|| position == Segment::Single);
	const auto roundBottom = (position == Segment::Bottom
		|| position == Segment::Single);
	auto buttons = std::vector<::Ui::SettingsButton*>();
	CollectButtons(block, buttons);
	for (const auto button : buttons) {
		const auto top = button->mapTo(block, QPoint(0, 0)).y();
		const auto atTop = roundTop && (top <= 1);
		const auto atBottom = roundBottom
			&& (top + button->height() >= height - 1);
		button->setCardSegment(
			(atTop && atBottom)
			? Segment::Single
			: atTop
			? Segment::Top
			: atBottom
			? Segment::Bottom
			: Segment::Flat);
	}
}

// Follows the wrappers (slide, fade, padding) that hold a single widget.
[[nodiscard]] QWidget *Unwrap(QWidget *widget) {
	while (!IsRowType(widget)
		&& !dynamic_cast<::Ui::VerticalLayout*>(widget)) {
		auto only = (QWidget*)nullptr;
		for (const auto child : widget->children()) {
			const auto inner = qobject_cast<QWidget*>(child);
			if (!inner || inner->isHidden()) {
				continue;
			} else if (only) {
				return widget;
			}
			only = inner;
		}
		if (!only) {
			return widget;
		}
		widget = only;
	}
	return widget;
}

void Collect(
		not_null<QWidget*> root,
		not_null<::Ui::VerticalLayout*> layout,
		std::vector<Item> &items,
		std::vector<Overlay> &overlays) {
	auto rows = base::flat_set<QWidget*>();
	for (auto i = 0, count = layout->count(); i != count; ++i) {
		const auto widget = layout->widgetAt(i).get();
		rows.emplace(widget);
		if (widget->isHidden() || widget->height() <= 0) {
			continue;
		}
		const auto y = widget->mapTo(root, QPoint(0, 0)).y();
		const auto inner = Unwrap(widget);
		if (IsRowType(inner)) {
			const auto margin = layout->rowMargin(i);
			items.push_back({
				.row = inner,
				.top = y - margin.top(),
				.bottom = y + widget->height() + margin.bottom(),
				.isRow = true,
			});
		} else if (const auto nested = dynamic_cast<::Ui::VerticalLayout*>(
				inner)) {
			Collect(root, nested, items, overlays);
		} else {
			items.push_back({
				.top = y,
				.bottom = y + widget->height(),
			});
		}
	}
	for (const auto child : layout->children()) {
		const auto button = dynamic_cast<::Ui::SettingsButton*>(child);
		if (button && !button->isHidden() && !rows.contains(button)) {
			const auto y = button->mapTo(root, QPoint(0, 0)).y();
			overlays.push_back({ button, y + button->height() / 2 });
		}
	}
}

class Backdrop final : public ::Ui::RpWidget {
public:
	Backdrop(
		QWidget *parent,
		not_null<::Ui::VerticalLayout*> container,
		int inset)
	: RpWidget(parent)
	, _container(container)
	, _inset(inset) {
		setAttribute(Qt::WA_TransparentForMouseEvents);
		container->sizeValue(
		) | rpl::on_next([=](QSize size) {
			setGeometry(0, 0, size.width(), size.height());
			update();
		}, lifetime());
		lower();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto items = std::vector<Item>();
		auto overlays = std::vector<Overlay>();
		Collect(_container, _container, items, overlays);

		auto p = QPainter(this);
		PainterHighQualityEnabler hq(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);

		const auto left = double(_inset);
		const auto width = double(this->width() - 2 * _inset);
		const auto halfGap = double(AyuUiSettings::CardSegmentHalfGap());
		const auto count = int(items.size());
		for (auto from = 0; from != count;) {
			if (!items[from].isRow) {
				++from;
				continue;
			}
			auto till = from + 1;
			while (till != count && items[till].isRow) {
				++till;
			}
			for (auto i = from; i != till; ++i) {
				const auto first = (i == from);
				const auto last = (i == till - 1);
				const auto position = (first && last)
					? Segment::Single
					: first
					? Segment::Top
					: last
					? Segment::Bottom
					: Segment::Middle;
				const auto top = first
					? items[i].top
					: ((items[i - 1].bottom + items[i].top) / 2);
				const auto bottom = last
					? items[i].bottom
					: ((items[i].bottom + items[i + 1].top) / 2);
				const auto rect = QRectF(
					left,
					top + (first ? 0. : halfGap),
					width,
					bottom - top - (first ? 0. : halfGap) - (last ? 0. : halfGap));
				p.drawPath(AyuUiSettings::CardSegmentPath(rect, position));
				if (items[i].row->property(kBlockProperty).toBool()) {
					ApplyBlockSegments(items[i].row, position);
				} else if (const auto button = dynamic_cast<
						::Ui::RippleButton*>(items[i].row)) {
					button->setCardSegment(position);
				}
			}
			from = till;
		}
		for (const auto &overlay : overlays) {
			const auto inside = ranges::any_of(items, [&](const Item &item) {
				return item.isRow
					&& (item.top <= overlay.center)
					&& (item.bottom > overlay.center);
			});
			overlay.button->setCardSegment(inside
				? Segment::Flat
				: Segment::None);
		}
	}

private:
	const not_null<::Ui::VerticalLayout*> _container;
	const int _inset;

};

} // namespace

void AttachCards(not_null<::Ui::VerticalLayout*> container) {
	AttachCards(container, st::fagramCardInset);
}

void AttachCards(not_null<::Ui::VerticalLayout*> container, int inset) {
	if (InCards(container)) {
		return;
	}
	container->setProperty("fa_cards", true);
	container->setRowInset(inset);
	::Ui::CreateChild<Backdrop>(container.get(), container, inset);
}

void MarkAsBlock(not_null<QWidget*> widget) {
	widget->setProperty(kBlockProperty, true);
}

bool InCards(not_null<QWidget*> widget) {
	return AyuUiSettings::InCards(widget);
}

} // namespace FA::Ui
