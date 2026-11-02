// This is the source code of freshGram.
#include "ayu/features/plugins/plugins_log_view.h"

#include "base/timer.h"
#include "crl/crl_on_main.h"
#include "lang/lang_keys.h"
#include "styles/style_ayu_styles.h"
#include "styles/style_basic.h"
#include "styles/style_boxes.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"
#include "ui/controls/jump_down_button.h"
#include "ui/effects/animations.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/ui_utility.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/widgets/multi_select.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/shadow.h"
#include "ui/wrap/vertical_layout.h"

#include <QtGui/QClipboard>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QWheelEvent>

namespace FreshPlugins {
namespace {

constexpr auto kRefreshInterval = crl::time(1000);

struct Row {
	std::vector<ConsolePart> parts;
	int entry = 0;
	int number = 0;
	int width = 0;
	bool first = false;
};

struct Built {
	std::vector<Row> rows;
	std::vector<QString> texts;
	QString plain;
	int maxNumber = 0;
};

struct State {
	Built shown;
	bool problems = false;
	QString query;
	int unread = 0;
	bool downShown = false;
	Ui::Animations::Simple downAnimation;
};

[[nodiscard]] const style::color &ColorOf(ConsoleColor color) {
	switch (color) {
	case ConsoleColor::Dim: return st::windowSubTextFg;
	case ConsoleColor::Good: return st::boxTextFgGood;
	case ConsoleColor::Warning: return st::historyPeer8NameFg;
	case ConsoleColor::Bad: return st::attentionButtonFg;
	}
	return st::windowFg;
}

// Splits an entry by '\n' into rows, only the first one gets a number.
void AppendRows(
		Built &built,
		const style::font &font,
		const ConsoleLine &line,
		int number,
		const QString &plain) {
	const auto entry = int(built.texts.size());
	built.texts.push_back(plain);
	auto row = Row{ .entry = entry, .number = number, .first = true };
	const auto flush = [&] {
		built.rows.push_back(std::move(row));
		row = Row{ .entry = entry, .number = number };
	};
	for (const auto &part : line.parts) {
		auto from = 0;
		while (true) {
			const auto till = part.text.indexOf('\n', from);
			const auto piece = part.text.mid(
				from,
				(till < 0) ? -1 : (till - from));
			if (!piece.isEmpty()) {
				row.width += font->width(piece);
				row.parts.push_back({ piece, part.color });
			}
			if (till < 0) {
				break;
			}
			flush();
			from = till + 1;
		}
	}
	built.rows.push_back(std::move(row));
}

[[nodiscard]] Built Build(
		const std::vector<ConsoleLine> &lines,
		const style::font &font,
		bool problems,
		const QString &query) {
	auto result = Built();
	for (auto i = 0, count = int(lines.size()); i != count; ++i) {
		const auto &line = lines[i];
		if (problems && !line.problem) {
			continue;
		}
		auto plain = QString();
		for (const auto &part : line.parts) {
			plain += part.text;
		}
		if (!query.isEmpty()
			&& !plain.contains(query, Qt::CaseInsensitive)) {
			continue;
		}
		if (!result.texts.empty()) {
			result.plain.push_back('\n');
		}
		result.plain += plain;
		result.maxNumber = i + 1;
		AppendRows(result, font, line, i + 1, plain);
	}
	return result;
}

class ConsoleView final : public Ui::RpWidget {
public:
	ConsoleView(
		QWidget *parent,
		not_null<Ui::ScrollArea*> scroll,
		Fn<void(QString)> copyLine);

	void setContent(const Built *built, const QString &empty);
	void fit();

protected:
	void paintEvent(QPaintEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;

private:
	[[nodiscard]] int gutterWidth() const;

	const not_null<Ui::ScrollArea*> _scroll;
	const Fn<void(QString)> _copyLine;
	const style::font _font;
	const int _lineHeight;
	const Built *_built = nullptr;
	QString _empty;
	int _gutter = 0;
	int _contentWidth = 0;
	base::unique_qptr<Ui::PopupMenu> _menu;

};

ConsoleView::ConsoleView(
	QWidget *parent,
	not_null<Ui::ScrollArea*> scroll,
	Fn<void(QString)> copyLine)
: RpWidget(parent)
, _scroll(scroll)
, _copyLine(std::move(copyLine))
, _font(st::pluginLogConsole.style.font->monospace())
, _lineHeight(std::max(
	st::pluginLogConsole.style.lineHeight,
	st::pluginLogConsole.style.font->monospace()->height)) {
	_scroll->scrolls(
	) | rpl::on_next([=] {
		update();
	}, lifetime());
}

int ConsoleView::gutterWidth() const {
	const auto digits = QString::number(
		_built ? std::max(_built->maxNumber, 1) : 1).size();
	return st::pluginLogGutterSkip * 2 + digits * _font->width(QChar('0'));
}

void ConsoleView::setContent(const Built *built, const QString &empty) {
	_built = built;
	_empty = empty;
	_gutter = gutterWidth();
	auto widest = 0;
	if (_built) {
		for (const auto &row : _built->rows) {
			widest = std::max(widest, row.width);
		}
	}
	_contentWidth = _gutter + st::pluginLogTextSkip * 2 + widest;
	fit();
	update();
}

void ConsoleView::fit() {
	const auto rows = _built ? int(_built->rows.size()) : 0;
	resize(
		std::max(_scroll->width(), _contentWidth),
		std::max(_scroll->height(), std::max(rows, 1) * _lineHeight));
}

void ConsoleView::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto clip = e->rect();
	p.fillRect(clip, st::boxBg);
	p.setFont(_font);

	const auto left = _scroll->scrollLeft();
	const auto visible = _scroll->width();
	const auto textLeft = _gutter + st::pluginLogTextSkip;
	const auto baseline = (_lineHeight - _font->height) / 2 + _font->ascent;
	const auto empty = !_built || _built->rows.empty();
	if (empty) {
		p.setPen(st::windowSubTextFg);
		p.drawText(left + textLeft, baseline, _empty);
	} else {
		const auto &rows = _built->rows;
		const auto from = std::max(clip.top() / _lineHeight, 0);
		const auto till = std::min(
			(clip.top() + clip.height() + _lineHeight - 1) / _lineHeight,
			int(rows.size()));
		for (auto i = from; i < till; ++i) {
			const auto &row = rows[i];
			const auto top = i * _lineHeight;
			const auto bg = (row.number % 2)
				? st::boxBg
				: st::boxDividerBg;
			p.fillRect(0, top, width(), _lineHeight, bg);

			auto x = textLeft;
			for (const auto &part : row.parts) {
				const auto w = _font->width(part.text);
				if (x + w > left + _gutter && x < left + visible) {
					p.setPen(ColorOf(part.color));
					p.drawText(x, top + baseline, part.text);
				}
				x += w;
			}

			p.fillRect(left, top, _gutter, _lineHeight, bg);
			if (row.first) {
				p.setPen(st::windowSubTextFg);
				const auto number = QString::number(row.number);
				p.drawText(
					left + _gutter
						- st::pluginLogGutterSkip
						- _font->width(number),
					top + baseline,
					number);
			}
		}
	}
	p.fillRect(
		left + _gutter - st::lineWidth,
		clip.top(),
		st::lineWidth,
		clip.height(),
		st::shadowFg);
}

void ConsoleView::contextMenuEvent(QContextMenuEvent *e) {
	if (!_built || _built->rows.empty()) {
		return;
	}
	const auto index = e->pos().y() / _lineHeight;
	if (index < 0 || index >= int(_built->rows.size())) {
		return;
	}
	const auto text = _built->texts[_built->rows[index].entry];
	const auto copy = _copyLine;
	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuWithIcons);
	_menu->addAction(tr::fresh_PluginLogCopyLine(tr::now), [=] {
		copy(text);
	});
	_menu->popup(e->globalPos());
}

} // namespace

QString PadRight(const QString &text, int width) {
	return (text.size() > width)
		? (text.left(width - 1) + QChar(0x2026))
		: text.leftJustified(width, ' ');
}

QString PadLeft(const QString &text, int width) {
	return text.rightJustified(width, ' ');
}

void FillConsoleBox(
		not_null<Ui::GenericBox*> box,
		ConsoleDescriptor descriptor) {
	box->setTitle(std::move(descriptor.title));
	box->setWidth(st::pluginLogBoxWidth);
	box->setMinHeight(st::pluginLogBoxHeight);
	box->setMaxHeight(st::pluginLogBoxHeight);

	const auto state = box->lifetime().make_state<State>();
	const auto lines = std::move(descriptor.lines);
	const auto clear = std::move(descriptor.clear);
	const auto empty = descriptor.empty;
	const auto font = st::pluginLogConsole.style.font->monospace();

	const auto top = box->setPinnedToTopContent(
		object_ptr<Ui::VerticalLayout>(box));
	const auto tabs = top->add(
		object_ptr<Ui::SettingsSlider>(top, st::defaultTabsSlider),
		st::pluginLogTabsPadding);
	tabs->setSections({
		tr::fresh_PluginLogTabAll(tr::now),
		descriptor.problemsTab,
	});
	tabs->setActiveSectionFast(0);
	const auto search = top->add(object_ptr<Ui::MultiSelect>(
		top,
		st::defaultMultiSelect,
		tr::fresh_PluginLogSearch()));
	top->add(object_ptr<Ui::PlainShadow>(top));

	// WHY: the console scrolls on both axes itself, so this row fills the
	// box scroll exactly and the box never scrolls.
	const auto wrap = box->addRow(
		object_ptr<Ui::RpWidget>(box),
		style::margins());
	wrap->resize(st::pluginLogBoxWidth, st::pluginLogBoxHeight / 2);
	const auto scroll = Ui::CreateChild<Ui::ScrollArea>(
		wrap,
		st::boxScroll);
	const auto view = scroll->setOwnedWidget(object_ptr<ConsoleView>(
		scroll,
		scroll,
		[=](QString text) {
			QGuiApplication::clipboard()->setText(text);
			box->showToast(tr::lng_text_copied(tr::now));
		})).data();
	const auto bottomLine = Ui::CreateChild<Ui::PlainShadow>(wrap);

	scroll->setCustomWheelProcess([=](not_null<QWheelEvent*> e) {
		const auto delta = Ui::ScrollDeltaF(e);
		const auto shift = (e->modifiers() & Qt::ShiftModifier) != 0;
		const auto horizontal = shift
			? (delta.x() != 0. ? delta.x() : delta.y())
			: (std::abs(delta.x()) > std::abs(delta.y()))
			? delta.x()
			: 0.;
		if (horizontal == 0.) {
			return false;
		}
		scroll->scrollToX(scroll->scrollLeft() - int(std::round(horizontal)));
		return true;
	});

	const auto fitToBox = [=] {
		const auto height = std::max(box->scrollHeight(), 1);
		if (wrap->height() != height) {
			wrap->resize(wrap->width(), height);
		}
	};
	wrap->sizeValue(
	) | rpl::on_next([=](QSize size) {
		scroll->setGeometry(0, 0, size.width(), size.height());
		bottomLine->setGeometry(
			0,
			size.height() - st::lineWidth,
			size.width(),
			st::lineWidth);
		view->fit();
	}, wrap->lifetime());
	box->sizeValue(
	) | rpl::on_next([=](QSize) {
		fitToBox();
	}, wrap->lifetime());

	const auto down = Ui::CreateChild<Ui::JumpDownButton>(
		box.get(),
		st::historyToDown);
	down->hide();
	const auto placeDown = [=] {
		const auto shown = state->downAnimation.value(
			state->downShown ? 1. : 0.);
		const auto shift = anim::interpolate(
			0,
			down->height() + st::historyToDownPosition.y(),
			shown);
		const auto bottom = wrap->mapTo(box.get(), QPoint(0, wrap->height()));
		down->moveToRight(
			st::historyToDownPosition.x(),
			bottom.y() - shift);
		const auto hidden = !state->downShown
			&& !state->downAnimation.animating();
		down->setVisible(!hidden);
		down->raise();
	};
	const auto toggleDown = [=](bool shown) {
		if (state->downShown == shown) {
			return;
		}
		state->downShown = shown;
		state->downAnimation.start(
			placeDown,
			shown ? 0. : 1.,
			shown ? 1. : 0.,
			st::historyToDownDuration);
	};
	const auto atBottom = [=] {
		return scroll->scrollTop()
			>= scroll->scrollTopMax() - st::pluginLogBottomSkip;
	};
	const auto scrollToBottom = [=] {
		crl::on_main(box, [=] {
			scroll->scrollToY(scroll->scrollTopMax());
		});
	};
	const auto updateDown = [=] {
		const auto bottom = atBottom();
		if (bottom && state->unread) {
			state->unread = 0;
			down->setUnreadCount(0);
		}
		toggleDown(!bottom);
	};
	down->setClickedCallback([=] {
		scroll->scrollToY(scroll->scrollTopMax());
	});
	box->sizeValue(
	) | rpl::on_next([=](QSize) {
		placeDown();
	}, down->lifetime());
	wrap->geometryValue(
	) | rpl::on_next([=](QRect) {
		placeDown();
	}, down->lifetime());
	scroll->scrolls(
	) | rpl::on_next(updateDown, down->lifetime());

	const auto refresh = [=](bool reset) {
		auto built = Build(lines(), font, state->problems, state->query);
		if (!reset && built.plain == state->shown.plain) {
			return;
		}
		const auto bottom = reset || atBottom();
		const auto added = int(built.texts.size())
			- int(state->shown.texts.size());
		state->shown = std::move(built);
		view->setContent(
			&state->shown,
			(state->problems || !state->query.isEmpty())
				? tr::fresh_PluginLogNothingFound(tr::now)
				: empty);
		if (bottom) {
			scrollToBottom();
		} else if (added > 0) {
			state->unread += added;
			down->setUnreadCount(state->unread);
		}
	};

	tabs->sectionActivated(
	) | rpl::on_next([=](int index) {
		state->problems = (index == 1);
		refresh(true);
	}, tabs->lifetime());
	search->setQueryChangedCallback([=](const QString &query) {
		state->query = query.trimmed();
		refresh(true);
	});
	box->setFocusCallback([=] {
		search->setInnerFocus();
	});
	box->showFinishes(
	) | rpl::on_next([=] {
		fitToBox();
		scrollToBottom();
	}, box->lifetime());

	const auto timer = box->lifetime().make_state<base::Timer>([=] {
		refresh(false);
	});
	timer->callEach(kRefreshInterval);
	refresh(true);

	box->addLeftButton(tr::fresh_PluginLogClear(), [=] {
		clear();
		refresh(true);
	});
	box->addButton(tr::fresh_PluginLogCopy(), [=] {
		QGuiApplication::clipboard()->setText(state->shown.plain);
		box->showToast(tr::lng_text_copied(tr::now));
	});
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace FreshPlugins
