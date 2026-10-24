// This is the source code of freshGram.
#include "ayu/ui/boxes/delete_own_messages_box.h"

#include "ayu/ui/boxes/delete_own_topics_box.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "lang/lang_keys.h"
#include "lang_auto.h"
#include "data/data_forum.h"
#include "ui/boxes/choose_date_time.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/number_input.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_ayu_styles.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

namespace AyuUi {
namespace {

enum class Mode {
	All,
	Period,
	Range,
	Count,
};

constexpr auto kHour = TimeId(3600);
constexpr auto kPeriodLimit = 999;
constexpr auto kCountLimit = 9999;

[[nodiscard]] not_null<Ui::NumberInput*> AddNumberInput(
		not_null<Ui::VerticalLayout*> layout,
		const QString &value,
		int limit) {
	const auto wrap = layout->add(
		object_ptr<Ui::FixedHeightWidget>(
			layout,
			st::defaultInputField.heightMin),
		style::margins(
			st::deleteOwnRowPadding.left(),
			0,
			st::deleteOwnRowPadding.right(),
			0));
	const auto input = Ui::CreateChild<Ui::NumberInput>(
		wrap,
		st::defaultInputField,
		tr::fresh_DeleteOwnMessagesNumber(),
		value,
		limit);
	input->setMaximumWidth(st::deleteOwnFieldWidth);
	wrap->widthValue() | rpl::on_next([=](int width) {
		input->resize(width, input->height());
		wrap->resize(width, input->height());
	}, wrap->lifetime());
	return input;
}

[[nodiscard]] TimeId UnitSeconds(int unit) {
	switch (unit) {
	case 0: return kHour;
	case 1: return 24 * kHour;
	case 2: return 7 * 24 * kHour;
	}
	return 30 * 24 * kHour;
}

[[nodiscard]] QString DateText(TimeId value) {
	return langDateTime(base::unixtime::parse(value));
}

} // namespace

void FillDeleteOwnMessagesBox(
		not_null<Ui::GenericBox*> box,
		Data::Forum *forum,
		Fn<void(DeleteOwnRange, Fn<void()>)> chosen) {
	box->setTitle(tr::ayu_DeleteOwnMessages());

	const auto scope = std::make_shared<Ui::RadiobuttonGroup>(0);
	const auto topics = box->lifetime().make_state<std::vector<MsgId>>();
	if (forum) {
		box->addRow(
			object_ptr<Ui::Radiobutton>(
				box,
				scope,
				0,
				tr::fresh_DeleteOwnMessagesScopeAll(tr::now),
				st::defaultCheckbox),
			st::deleteOwnRowPadding);
		box->addRow(
			object_ptr<Ui::Radiobutton>(
				box,
				scope,
				1,
				tr::fresh_DeleteOwnMessagesScopeTopics(tr::now),
				st::defaultCheckbox),
			st::deleteOwnRowPadding);
		const auto topicsWrap = box->addRow(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				box,
				object_ptr<Ui::VerticalLayout>(box)),
			style::margins(st::deleteOwnNestedSkip, 0, 0, 0));
		const auto pickButton = topicsWrap->entity()->add(
			object_ptr<Ui::LinkButton>(
				topicsWrap,
				tr::fresh_DeleteOwnMessagesPickTopics(tr::now)),
			st::deleteOwnRowPadding);
		pickButton->setClickedCallback([=] {
			ShowDeleteOwnTopicsBox(
				forum,
				box->uiShow(),
				*topics,
				[=](std::vector<MsgId> result) {
					*topics = std::move(result);
					pickButton->setText(topics->empty()
						? tr::fresh_DeleteOwnMessagesPickTopics(tr::now)
						: tr::fresh_DeleteOwnMessagesTopicsCount(
							tr::now,
							lt_amount,
							QString::number(topics->size())));
				});
		});
		topicsWrap->toggle(false, anim::type::instant);
		scope->setChangedCallback([=](int value) {
			topicsWrap->toggle(value == 1, anim::type::normal);
		});
	}

	const auto now = base::unixtime::now();
	const auto group = std::make_shared<Ui::RadiobuttonGroup>(
		int(Mode::All));
	const auto from = box->lifetime().make_state<TimeId>(now - 24 * kHour);
	const auto till = box->lifetime().make_state<TimeId>(now);
	const auto unit = std::make_shared<Ui::RadiobuttonGroup>(1);

	const auto addMode = [&](Mode mode, const QString &text) {
		box->addRow(
			object_ptr<Ui::Radiobutton>(
				box,
				group,
				int(mode),
				text,
				st::defaultCheckbox),
			st::deleteOwnRowPadding);
	};

	addMode(Mode::All, tr::fresh_DeleteOwnMessagesAll(tr::now));

	addMode(Mode::Period, tr::fresh_DeleteOwnMessagesPeriod(tr::now));
	const auto periodWrap = box->addRow(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			box,
			object_ptr<Ui::VerticalLayout>(box)),
		style::margins(st::deleteOwnNestedSkip, 0, 0, 0));
	const auto periodInput = AddNumberInput(
		periodWrap->entity(),
		u"24"_q,
		kPeriodLimit);
	const auto units = std::array{
		tr::fresh_DeleteOwnMessagesHours(tr::now),
		tr::fresh_DeleteOwnMessagesDays(tr::now),
		tr::fresh_DeleteOwnMessagesWeeks(tr::now),
		tr::fresh_DeleteOwnMessagesMonths(tr::now),
	};
	auto index = 0;
	for (const auto &text : units) {
		periodWrap->entity()->add(
			object_ptr<Ui::Radiobutton>(
				periodWrap,
				unit,
				index++,
				text,
				st::defaultCheckbox),
			st::deleteOwnRowPadding);
	}
	periodWrap->toggle(false, anim::type::instant);

	addMode(Mode::Range, tr::fresh_DeleteOwnMessagesRange(tr::now));
	const auto rangeWrap = box->addRow(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			box,
			object_ptr<Ui::VerticalLayout>(box)),
		style::margins(st::deleteOwnNestedSkip, 0, 0, 0));
	const auto fromButton = rangeWrap->entity()->add(
		object_ptr<Ui::LinkButton>(rangeWrap, DateText(*from)),
		st::deleteOwnRowPadding);
	const auto tillButton = rangeWrap->entity()->add(
		object_ptr<Ui::LinkButton>(rangeWrap, DateText(*till)),
		st::deleteOwnRowPadding);
	const auto pick = [=](
			rpl::producer<QString> title,
			not_null<TimeId*> value,
			not_null<Ui::LinkButton*> button) {
		box->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> inner) {
			Ui::ChooseDateTimeBox(inner, {
				.title = title,
				.submit = tr::lng_settings_save(),
				.done = [=](TimeId result) {
					*value = result;
					button->setText(DateText(result));
					inner->closeBox();
				},
				.min = [] { return TimeId(1); },
				.time = *value,
				.max = [] { return base::unixtime::now(); },
			});
		}));
	};
	fromButton->setClickedCallback([=] {
		pick(tr::fresh_DeleteOwnMessagesFrom(), from, fromButton);
	});
	tillButton->setClickedCallback([=] {
		pick(tr::fresh_DeleteOwnMessagesTo(), till, tillButton);
	});
	rangeWrap->toggle(false, anim::type::instant);

	addMode(Mode::Count, tr::fresh_DeleteOwnMessagesLastCount(tr::now));
	const auto countWrap = box->addRow(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			box,
			object_ptr<Ui::VerticalLayout>(box)),
		style::margins(st::deleteOwnNestedSkip, 0, 0, 0));
	const auto countInput = AddNumberInput(
		countWrap->entity(),
		u"100"_q,
		kCountLimit);
	countWrap->toggle(false, anim::type::instant);

	group->setChangedCallback([=](int value) {
		periodWrap->toggle(value == int(Mode::Period), anim::type::normal);
		rangeWrap->toggle(value == int(Mode::Range), anim::type::normal);
		countWrap->toggle(value == int(Mode::Count), anim::type::normal);
	});

	box->addButton(tr::lng_box_delete(), [=] {
		auto range = DeleteOwnRange();
		if (forum && scope->current() == 1) {
			if (topics->empty()) {
				box->showToast(tr::fresh_DeleteOwnMessagesInvalid(tr::now));
				return;
			}
			range.topics = *topics;
		}
		switch (Mode(group->current())) {
		case Mode::All:
			break;
		case Mode::Period: {
			const auto count = periodInput->getLastText().toInt();
			if (count < 1) {
				box->showToast(tr::fresh_DeleteOwnMessagesInvalid(tr::now));
				return;
			}
			const auto current = base::unixtime::now();
			range.minDate = current - count * UnitSeconds(unit->current());
			range.maxDate = current;
		} break;
		case Mode::Range:
			if (*from >= *till) {
				box->showToast(tr::fresh_DeleteOwnMessagesInvalid(tr::now));
				return;
			}
			range.minDate = *from;
			range.maxDate = *till;
			break;
		case Mode::Count:
			range.limit = countInput->getLastText().toInt();
			if (range.limit < 1) {
				box->showToast(tr::fresh_DeleteOwnMessagesInvalid(tr::now));
				return;
			}
			break;
		}
		chosen(range, crl::guard(box, [=] { box->closeBox(); }));
	}, st::attentionBoxButton);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

} // namespace AyuUi
