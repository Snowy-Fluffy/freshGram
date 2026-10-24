// This file is part of freshGram.
#include "ayu/ui/boxes/fake_passcode_box.h"

#include "ayu/features/fake_passcode/fake_passcode.h"
#include "core/application.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "lang_auto.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "settings/cloud_password/settings_cloud_password_common.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

namespace AyuUi {
namespace {

using namespace FreshFakePasscode;

constexpr auto kMaxNameLength = 64;
constexpr auto kMaxCommandLength = 256;

[[nodiscard]] QString AccountTitle(
		not_null<Main::Account*> account,
		int index) {
	const auto session = account->maybeSession();
	return session
		? session->user()->name()
		: (u"#"_q + QString::number(index + 1));
}

void EditBox(
		not_null<Ui::GenericBox*> box,
		int id,
		Fn<void()> changed) {
	const auto &list = Current().list;
	const auto found = ranges::find(list, id, &Entry::id);
	const auto editing = (id != 0) && (found != end(list));
	const auto entry = editing ? *found : Entry();

	box->setTitle(editing
		? tr::fresh_FakePasscodeEdit()
		: tr::fresh_FakePasscodeAdd());
	const auto content = box->verticalLayout();

	const auto name = Settings::CloudPassword::AddWrappedField(
		content,
		tr::fresh_FakePasscodeName(),
		entry.name);
	name->setMaxLength(kMaxNameLength);

	auto passcode = (Ui::PasswordInput*)nullptr;
	auto real = (Ui::PasswordInput*)nullptr;
	auto error = (Ui::FlatLabel*)nullptr;
	if (!editing) {
		passcode = Settings::CloudPassword::AddPasswordField(
			content,
			tr::fresh_FakePasscodeField(),
			QString());
		real = Settings::CloudPassword::AddPasswordField(
			content,
			tr::fresh_FakePasscodeCurrent(),
			QString());
		error = Settings::CloudPassword::AddError(content, real);
		QObject::connect(passcode, &Ui::MaskedInputField::changed, [=] {
			error->hide();
		});
	}

	auto unlockToggle = (Ui::SettingsButton*)nullptr;
	if (!editing) {
		Ui::AddSkip(content);
		unlockToggle = content->add(object_ptr<Ui::SettingsButton>(
			content,
			tr::fresh_FakePasscodeUnlocks(),
			st::settingsButtonNoIcon));
		unlockToggle->toggleOn(rpl::single(true));
		Ui::AddDividerText(content, tr::fresh_FakePasscodeUnlocksAbout());
	}
	const auto unlocks = [=] {
		return unlockToggle ? unlockToggle->toggled() : entry.unlocks;
	};
	const auto unlocksValue = [=]() -> rpl::producer<bool> {
		return unlockToggle
			? unlockToggle->toggledValue()
			: rpl::single(entry.unlocks);
	};

	Ui::AddSkip(content);
	Ui::AddSubsectionTitle(content, tr::fresh_FakePasscodeActions());
	const auto addToggle = [&](
			not_null<Ui::VerticalLayout*> container,
			rpl::producer<QString> text,
			bool value) {
		const auto button = container->add(object_ptr<Ui::SettingsButton>(
			container,
			std::move(text),
			st::settingsButtonNoIcon));
		button->toggleOn(rpl::single(value));
		return button;
	};
	const auto clearCache = addToggle(
		content,
		tr::fresh_FakePasscodeClearCache(),
		entry.actions.clearCache);
	const auto cacheExtras = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)));
	cacheExtras->toggleOn(clearCache->toggledValue());
	const auto clearCacheDb = addToggle(
		cacheExtras->entity(),
		tr::fresh_FakePasscodeClearDb(),
		entry.actions.clearCacheDb);
	const auto clearCacheMedia = addToggle(
		cacheExtras->entity(),
		tr::fresh_FakePasscodeClearMedia(),
		entry.actions.clearCacheMedia);
	const auto clearProxies = addToggle(
		content,
		tr::fresh_FakePasscodeClearProxies(),
		entry.actions.clearProxies);

	const auto accountsWrap = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)));
	accountsWrap->toggleOn(unlocksValue());
	auto logouts = std::vector<std::pair<int, Ui::SettingsButton*>>();
	for (const auto &item : Core::App().domain().accounts()) {
		if (!item.account->sessionExists()) {
			continue;
		}
		logouts.emplace_back(item.index, addToggle(
			accountsWrap->entity(),
			rpl::single(tr::fresh_FakePasscodeLogout(
				tr::now,
				lt_name,
				AccountTitle(item.account.get(), item.index))),
			ranges::contains(entry.actions.logoutAccounts, item.index)));
	}
	const auto anyLogout = content->lifetime().make_state<
		rpl::variable<bool>
	>(false);
	const auto refreshLogout = [=] {
		*anyLogout = ranges::any_of(logouts, [](const auto &pair) {
			return pair.second->toggled();
		});
	};
	for (const auto &pair : logouts) {
		pair.second->toggledChanges(
		) | rpl::on_next(refreshLogout, pair.second->lifetime());
	}
	refreshLogout();
	const auto logoutExtras = accountsWrap->entity()->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			accountsWrap->entity(),
			object_ptr<Ui::VerticalLayout>(accountsWrap->entity())));
	logoutExtras->toggleOn(anyLogout->value());
	const auto logoutDb = addToggle(
		logoutExtras->entity(),
		tr::fresh_FakePasscodeLogoutDb(),
		entry.actions.logoutDb);
	const auto logoutMedia = addToggle(
		logoutExtras->entity(),
		tr::fresh_FakePasscodeLogoutMedia(),
		entry.actions.logoutMedia);
	const auto deleteWrap = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)));
	deleteWrap->toggleOn(unlocksValue() | rpl::map([](bool value) {
		return !value;
	}));
	const auto deleteAll = addToggle(
		deleteWrap->entity(),
		tr::fresh_FakePasscodeDeleteAll(),
		entry.actions.deleteAll);
	Ui::AddDividerText(
		deleteWrap->entity(),
		tr::fresh_FakePasscodeDeleteAllAbout());
	Ui::AddDividerText(content, tr::fresh_FakePasscodeActionsAbout());

	Ui::AddSkip(content);
	const auto command = Settings::CloudPassword::AddWrappedField(
		content,
		tr::fresh_FakePasscodeCommand(),
		entry.actions.command);
	command->setMaxLength(kMaxCommandLength);
	Ui::AddDividerText(content, tr::fresh_FakePasscodeCommandAbout());

	const auto collect = [=] {
		auto result = Actions();
		result.clearCache = clearCache->toggled();
		result.clearCacheDb = result.clearCache && clearCacheDb->toggled();
		result.clearCacheMedia = result.clearCache
			&& clearCacheMedia->toggled();
		result.clearProxies = clearProxies->toggled();
		result.command = command->getLastText().trimmed();
		if (unlocks()) {
			for (const auto &[index, button] : logouts) {
				if (button->toggled()) {
					result.logoutAccounts.push_back(index);
				}
			}
			const auto any = !result.logoutAccounts.empty();
			result.logoutDb = any && logoutDb->toggled();
			result.logoutMedia = any && logoutMedia->toggled();
		} else {
			result.deleteAll = deleteAll->toggled();
		}
		return result;
	};
	const auto title = [=] {
		const auto text = name->getLastText().trimmed();
		return !text.isEmpty()
			? text
			: editing
			? entry.name
			: tr::fresh_FakePasscodeDefaultName(
				tr::now,
				lt_number,
				QString::number(Current().list.size() + 1));
	};
	const auto fail = [=](const QString &text, Ui::PasswordInput *field) {
		error->setText(text);
		error->show();
		field->selectAll();
		field->showError();
	};
	const auto save = [=] {
		if (editing) {
			Update(id, title(), collect());
			changed();
			box->closeBox();
			return;
		}
		switch (Add(title(), passcode->text(), real->text(), unlocks(), collect())) {
		case AddResult::Ok:
			changed();
			box->closeBox();
			return;
		case AddResult::Empty:
			passcode->showError();
			return;
		case AddResult::TooShort:
			fail(tr::fresh_FakePasscodeTooShort(tr::now), passcode);
			return;
		case AddResult::TooMany:
			fail(tr::fresh_FakePasscodeLimit(
				tr::now,
				lt_limit,
				QString::number(kMaxEntries)), passcode);
			return;
		case AddResult::WrongPasscode:
			fail(tr::fresh_FakePasscodeWrong(tr::now), real);
			return;
		case AddResult::SameAsReal:
			fail(tr::fresh_FakePasscodeSameAsReal(tr::now), passcode);
			return;
		case AddResult::Duplicate:
			fail(tr::fresh_FakePasscodeDuplicate(tr::now), passcode);
			return;
		case AddResult::Failed:
			fail(tr::fresh_FakePasscodeFailed(tr::now), passcode);
			return;
		}
	};
	box->addButton(tr::lng_settings_save(), [=] {
		if ((!editing && !unlocks()) || collect().deleteAll) {
			box->uiShow()->showBox(Ui::MakeConfirmBox({
				.text = tr::fresh_FakePasscodeConfirm(),
				.confirmed = [=](Fn<void()> &&close) {
					close();
					save();
				},
				.confirmText = tr::lng_settings_save(),
			}));
		} else {
			save();
		}
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	if (editing) {
		box->addLeftButton(tr::lng_box_delete(), [=] {
			box->uiShow()->showBox(Ui::MakeConfirmBox({
				.text = tr::fresh_FakePasscodeDeleteSure(),
				.confirmed = [=](Fn<void()> &&close) {
					Remove(id);
					changed();
					close();
					box->closeBox();
				},
				.confirmText = tr::lng_box_delete(),
				.confirmStyle = &st::attentionBoxButton,
			}));
		});
	}
	box->setFocusCallback([=] {
		if (passcode) {
			passcode->setFocusFast();
		} else {
			name->setFocusFast();
		}
	});
}

} // namespace

void FakePasscodesBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(tr::fresh_FakePasscodes());
	const auto content = box->verticalLayout();

	Ui::AddSkip(content);
	const auto onLock = content->add(object_ptr<Ui::SettingsButton>(
		content,
		tr::fresh_FakePasscodeClearOnLock(),
		st::settingsButtonNoIcon));
	onLock->toggleOn(rpl::single(Current().clearCacheOnLock));
	onLock->toggledChanges(
	) | rpl::on_next([](bool value) {
		SetClearCacheOnLock(value);
	}, onLock->lifetime());
	Ui::AddDividerText(content, tr::fresh_FakePasscodeClearOnLockAbout());

	Ui::AddSkip(content);
	Ui::AddSubsectionTitle(content, tr::fresh_FakePasscodes());
	const auto list = content->add(object_ptr<Ui::VerticalLayout>(content));
	const auto refresh = box->lifetime().make_state<Fn<void()>>();
	*refresh = [=] {
		while (list->count()) {
			delete list->widgetAt(0);
		}
		for (const auto &entry : Current().list) {
			const auto id = entry.id;
			const auto button = list->add(object_ptr<Ui::SettingsButton>(
				list,
				rpl::single(entry.name),
				st::settingsButtonNoIcon));
			button->setClickedCallback([=] {
				box->uiShow()->showBox(Box([=](
						not_null<Ui::GenericBox*> edit) {
					EditBox(edit, id, *refresh);
				}));
			});
		}
	};
	(*refresh)();

	const auto add = content->add(object_ptr<Ui::SettingsButton>(
		content,
		tr::fresh_FakePasscodeAdd(),
		st::settingsButtonLightNoIcon));
	add->setClickedCallback([=] {
		box->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> edit) {
			EditBox(edit, 0, *refresh);
		}));
	});
	Ui::AddDividerText(content, tr::fresh_FakePasscodesAbout());

	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

} // namespace AyuUi
