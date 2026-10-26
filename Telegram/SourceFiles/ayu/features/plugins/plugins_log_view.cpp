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
#include "ui/controls/jump_down_button.h"
#include "ui/effects/animations.h"
#include "ui/layers/generic_box.h"
#include "ui/ui_utility.h"
#include "ui/text/text_entity.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/multi_select.h"
#include "ui/wrap/vertical_layout.h"

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace FreshPlugins {
namespace {

constexpr auto kRefreshInterval = crl::time(1000);
constexpr auto kColorsCount = 4;

struct Built {
	TextWithEntities text;
	int lines = 0;
};

struct State {
	Built shown;
	bool problems = false;
	QString query;
	int unread = 0;
	bool downShown = false;
	Ui::Animations::Simple downAnimation;
};

[[nodiscard]] Built Build(
		const std::vector<ConsoleLine> &lines,
		bool problems,
		const QString &query) {
	auto result = Built();
	auto &text = result.text.text;
	for (const auto &line : lines) {
		if (problems && !line.problem) {
			continue;
		}
		if (!query.isEmpty()) {
			auto plain = QString();
			for (const auto &part : line.parts) {
				plain += part.text;
			}
			if (!plain.contains(query, Qt::CaseInsensitive)) {
				continue;
			}
		}
		if (result.lines++) {
			text.push_back('\n');
		}
		for (const auto &part : line.parts) {
			if (part.color != ConsoleColor::Normal && !part.text.isEmpty()) {
				result.text.entities.push_back(EntityInText(
					EntityType::Colorized,
					text.size(),
					part.text.size(),
					QString(QChar(ushort(part.color)))));
			}
			text += part.text;
		}
	}
	return result;
}

[[nodiscard]] TextWithEntities Dim(const QString &text) {
	return {
		.text = text,
		.entities = { EntityInText(
			EntityType::Colorized,
			0,
			text.size(),
			QString(QChar(ushort(ConsoleColor::Dim)))) },
	};
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

	const auto console = box->lifetime().make_state<style::FlatLabel>(
		st::pluginLogConsole);
	console->style.font = console->style.font->monospace();
	const auto colors = box->lifetime().make_state<
		std::array<Ui::Text::SpecialColor, kColorsCount>>();
	const auto pens = std::array<const style::color*, kColorsCount>{
		&st::windowSubTextFg,
		&st::boxTextFgGood,
		&st::historyPeer8NameFg,
		&st::attentionButtonFg,
	};
	for (auto i = 0; i != kColorsCount; ++i) {
		(*colors)[i].pen = &(*pens[i])->p;
		(*colors)[i].penSelected = &(*pens[i])->p;
	}
	const auto label = box->addRow(
		object_ptr<Ui::FlatLabel>(box, *console),
		st::pluginLogPadding);
	label->setSelectable(true);
	label->setColors(*colors);

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
		down->moveToRight(
			st::historyToDownPosition.x(),
			box->height() - shift);
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
		return box->scrollTop() + box->scrollHeight()
			>= box->verticalLayout()->height() - st::pluginLogBottomSkip;
	};
	const auto scrollToBottom = [=] {
		crl::on_main(box, [=] {
			box->scrollToY(QWIDGETSIZE_MAX);
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
		box->scrollToY(QWIDGETSIZE_MAX);
	});
	box->sizeValue(
	) | rpl::on_next([=](QSize) {
		placeDown();
	}, down->lifetime());
	box->scrolls(
	) | rpl::on_next(updateDown, down->lifetime());

	const auto refresh = [=](bool reset) {
		auto built = Build(lines(), state->problems, state->query);
		const auto &was = state->shown.text.text;
		if (!reset && built.text.text == was) {
			return;
		}
		const auto appended = state->shown.lines
			&& built.text.text.startsWith(was);
		if (!reset && !appended && label->hasSelection()) {
			return;
		}
		const auto bottom = reset || atBottom();
		const auto added = built.lines - state->shown.lines;
		state->shown = std::move(built);
		label->setMarkedText(state->shown.lines
			? state->shown.text
			: Dim((state->problems || !state->query.isEmpty())
				? tr::fresh_PluginLogNothingFound(tr::now)
				: empty));
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
	) | rpl::on_next(scrollToBottom, box->lifetime());

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
		QGuiApplication::clipboard()->setText(state->shown.lines
			? state->shown.text.text
			: QString());
		box->showToast(tr::lng_text_copied(tr::now));
	});
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace FreshPlugins
