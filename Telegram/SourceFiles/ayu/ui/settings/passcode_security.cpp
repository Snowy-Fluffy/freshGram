// This is the source code of freshGram.
#include "ayu/ui/settings/passcode_security.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/passcode_guard/lock_background.h"
#include "ayu/features/passcode_guard/passcode_guard.h"
#include "ayu/utils/passcode_strength.h"
#include "base/platform/base_platform_info.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "crl/crl_async.h"
#include "crl/crl_on_main.h"
#include "lang/lang_keys.h"
#include "lang_auto.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "settings/cloud_password/settings_cloud_password_common.h"
#include "settings/settings_builder.h"
#include "storage/details/storage_passcode_kdf.h"
#include "storage/storage_domain.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/color_editor.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_lock_widgets.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_window_lock_widgets.h"

namespace AyuUi {
namespace {

using namespace Settings::Builder;
using Storage::PasscodeKdfLevel;

constexpr auto kStrengthSegments = 4;

struct Choice {
	int value = 0;
	QString text;
};

[[nodiscard]] QString OffText() {
	return tr::lng_settings_auto_night_mode_off(tr::now);
}

void ChooseLockBackground(not_null<Window::SessionController*> controller) {
	const auto filter = u"Image files (*.jpg *.jpeg *.png *.webp)"_q;
	const auto weak = base::make_weak(controller);
	FileDialog::GetOpenPath(
		controller->widget().get(),
		tr::lng_choose_image(tr::now),
		filter,
		[=](FileDialog::OpenResult &&result) {
			if (result.paths.isEmpty()) {
				return;
			}
			FreshLockBackground::Import(result.paths.front(), [=](bool ok) {
				if (ok) {
					AyuSettings::getInstance().setLockScreenBackground(2);
				} else if (const auto strong = weak.get()) {
					strong->showToast(tr::fresh_PasscodeLockImageFailed(tr::now));
				}
			});
		});
}

void ChooseLockAvatar(not_null<Window::SessionController*> controller) {
	const auto filter = u"Image files (*.jpg *.jpeg *.png *.webp)"_q;
	const auto weak = base::make_weak(controller);
	FileDialog::GetOpenPath(
		controller->widget().get(),
		tr::lng_choose_image(tr::now),
		filter,
		[=](FileDialog::OpenResult &&result) {
			if (result.paths.isEmpty()) {
				return;
			}
			FreshLockBackground::ImportAvatar(result.paths.front(), [=](bool ok) {
				if (ok) {
					AyuSettings::getInstance().setLockScreenAvatar(true);
				} else if (const auto strong = weak.get()) {
					strong->showToast(tr::fresh_PasscodeLockImageFailed(tr::now));
				}
			});
		});
}

void LockLineBox(
		not_null<Ui::GenericBox*> box,
		tr::phrase<> title,
		tr::phrase<> placeholder,
		const QString &current,
		int maxLength,
		Fn<void(QString)> set) {
	box->setTitle(title());
	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::NoNewlines,
		placeholder(),
		current));
	field->setMaxLength(maxLength);
	box->setFocusCallback([=] {
		field->setFocusFast();
	});
	const auto save = [=] {
		set(field->getLastText());
		box->closeBox();
	};
	field->submits() | rpl::on_next(save, field->lifetime());
	box->addButton(tr::lng_settings_save(), save);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void LockTextBox(not_null<Ui::GenericBox*> box) {
	constexpr auto kMaxLength = 120;
	box->setTitle(tr::fresh_PasscodeLockText());
	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::newGroupDescription,
		Ui::InputField::Mode::MultiLine,
		tr::fresh_PasscodeLockTextPlaceholder(),
		AyuSettings::getInstance().lockScreenText()));
	field->setMaxLength(kMaxLength);
	box->setFocusCallback([=] {
		field->setFocusFast();
	});
	const auto save = [=] {
		AyuSettings::getInstance().setLockScreenText(field->getLastText());
		box->closeBox();
	};
	field->submits() | rpl::on_next(save, field->lifetime());
	box->addButton(tr::lng_settings_save(), save);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void LockColorBox(
		not_null<Ui::GenericBox*> box,
		tr::phrase<> title,
		uint32 current,
		Fn<void(uint32)> set) {
	box->setTitle(title());
	const auto editor = box->addRow(object_ptr<ColorEditor>(
		box,
		ColorEditor::Mode::HSL,
		current ? QColor::fromRgba(current) : st::activeButtonBg->c));
	const auto save = [=] {
		set(editor->color().rgba());
		box->closeBox();
	};
	editor->submitRequests() | rpl::on_next(save, editor->lifetime());
	box->setFocusCallback([=] {
		editor->setInnerFocus();
	});
	box->addButton(tr::lng_settings_save(), save);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	box->addLeftButton(tr::fresh_PasscodeLockColorDefault(), [=] {
		set(0);
		box->closeBox();
	});
	box->setWidth(editor->width());
}

void LockPreviewBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setTitle(tr::fresh_PasscodeLockPreview());
	box->setWidth(st::boxWideWidth);
	box->setNoContentMargin(true);
	const auto lock = box->addRow(
		object_ptr<Window::PasscodeLockWidget>(
			box,
			&controller->window(),
			true),
		style::margins());
	lock->resize(st::boxWideWidth, st::passcodePreviewHeight);
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

[[nodiscard]] QString KdfLevelName(PasscodeKdfLevel level) {
	switch (level) {
	case PasscodeKdfLevel::Standard: return tr::fresh_PasscodeKdfStandard(tr::now);
	case PasscodeKdfLevel::Strong: return tr::fresh_PasscodeKdfStrong(tr::now);
	case PasscodeKdfLevel::Maximum: return tr::fresh_PasscodeKdfMaximum(tr::now);
	}
	return QString();
}

[[nodiscard]] QString KdfLevelAbout(PasscodeKdfLevel level) {
	switch (level) {
	case PasscodeKdfLevel::Standard:
		return tr::fresh_PasscodeKdfStandardAbout(tr::now);
	case PasscodeKdfLevel::Strong:
		return tr::fresh_PasscodeKdfStrongAbout(tr::now);
	case PasscodeKdfLevel::Maximum:
		return tr::fresh_PasscodeKdfMaximumAbout(tr::now);
	}
	return QString();
}

void PasscodeKdfBox(
		not_null<Ui::GenericBox*> box,
		Fn<void()> changed) {
	box->setTitle(tr::fresh_PasscodeKdf());
	const auto content = box->verticalLayout();
	auto &local = Core::App().domain().local();
	const auto current = Storage::PasscodeKdfLevelOf(local.passcodeKdf());
	const auto supported = Storage::PasscodeKdfArgon2Supported();
	const auto group = std::make_shared<Ui::RadiobuttonGroup>(int(current));

	Ui::AddSkip(content);
	for (const auto level : {
			PasscodeKdfLevel::Standard,
			PasscodeKdfLevel::Strong,
			PasscodeKdfLevel::Maximum }) {
		const auto radio = content->add(
			object_ptr<Ui::Radiobutton>(
				content,
				group,
				int(level),
				KdfLevelName(level),
				st::defaultBoxCheckbox),
			st::boxRowPadding + style::margins(0, 0, 0, st::boxOptionListSkip));
		const auto enabled = supported
			|| (level == PasscodeKdfLevel::Standard);
		radio->setDisabled(!enabled);
		content->add(
			object_ptr<Ui::FlatLabel>(
				content,
				KdfLevelAbout(level),
				st::boxDividerLabel),
			st::boxRowPadding
				+ style::margins(
					st::defaultBoxCheckbox.textPosition.x(),
					0,
					0,
					st::boxOptionListSkip));
	}
	if (!supported) {
		content->add(
			object_ptr<Ui::FlatLabel>(
				content,
				tr::fresh_PasscodeKdfUnsupported(),
				st::boxDividerLabel),
			st::boxRowPadding);
	}
	Ui::AddSkip(content);
	Ui::AddDividerText(content, tr::fresh_PasscodeKdfAbout());
	Ui::AddSkip(content);

	const auto field = Settings::CloudPassword::AddPasswordField(
		content,
		tr::lng_passcode_enter_old(),
		QString());
	const auto error = Settings::CloudPassword::AddError(content, field);

	const auto weak = QPointer<Ui::GenericBox>(box.get());
	const auto busy = std::make_shared<bool>(false);
	const auto setBusy = [=](bool value) {
		*busy = value;
		// WHY: the box looks blocked while the keys are made on a thread.
		box->setCloseByEscape(!value);
		box->setCloseByOutsideClick(!value);
		field->setDisabled(value);
		content->setAttribute(Qt::WA_TransparentForMouseEvents, value);
	};
	const auto fail = [=](const QString &text) {
		setBusy(false);
		error->setText(text);
		error->show();
		field->selectAll();
		field->setFocus();
		field->showError();
	};
	const auto save = [=] {
		const auto level = PasscodeKdfLevel(group->current());
		if (*busy) {
			return;
		} else if (level == current) {
			box->closeBox();
			return;
		} else if (!passcodeCanTry()) {
			fail(tr::lng_flood_error(tr::now));
			return;
		}
		const auto passcode = field->text().toUtf8();
		if (passcode.isEmpty()) {
			field->showError();
			return;
		}
		setBusy(true);
		error->setText(tr::fresh_PasscodeKdfApplying(tr::now));
		error->show();
		const auto apply = [=] {
			Core::App().domain().local().setPasscodeAsync(
				passcode,
				Storage::PasscodeKdfFor(level),
				[=](bool ok) {
					if (ok) {
						Core::App().localPasscodeChanged();
					}
					if (!weak) {
						return;
					} else if (!ok) {
						fail(tr::fresh_PasscodeKdfFailed(tr::now));
						return;
					}
					box->showToast(tr::fresh_PasscodeKdfChanged(tr::now));
					if (changed) {
						changed();
					}
					box->closeBox();
				});
		};
		CheckPasscodeAsync(passcode, crl::guard(box, [=](
				Storage::PasscodeCheck result) {
			switch (result) {
			case Storage::PasscodeCheck::Failed:
				fail(tr::fresh_PasscodeKeyUnavailable(tr::now));
				return;
			case Storage::PasscodeCheck::Wrong:
				cSetPasscodeBadTries(cPasscodeBadTries() + 1);
				cSetPasscodeLastTry(crl::now());
				fail(tr::lng_passcode_wrong(tr::now));
				return;
			case Storage::PasscodeCheck::Correct:
				break;
			}
			FreshPasscodeGuard::RegisterSuccess();
			apply();
		}));
	};
	QObject::connect(field, &Ui::MaskedInputField::submitted, save);
	box->setFocusCallback([=] { field->setFocusFast(); });
	box->addButton(tr::lng_settings_save(), save);
	box->addButton(tr::lng_cancel(), [=] {
		if (!*busy) {
			box->closeBox();
		}
	});
}

} // namespace

void CheckPasscodeAsync(
		const QByteArray &passcode,
		Fn<void(Storage::PasscodeCheck)> done) {
	const auto input = Core::App().domain().local().passcodeKdfInput();
	if (!input) {
		done(Storage::PasscodeCheck::Failed);
		return;
	}
	crl::async([=] {
		auto key = Storage::PrecomputedPasscodeKey{
			.salt = input->salt,
			.kdf = input->kdf,
			.key = Storage::DerivePasscodeKey(
				passcode,
				input->salt,
				input->kdf),
		};
		crl::on_main([=, key = std::move(key)] {
			auto &domain = Core::App().domain();
			done(domain.started()
				? domain.local().checkPasscodeKey(key)
				: Storage::PasscodeCheck::Failed);
		});
	});
}

void SetPasscodeAsync(const QByteArray &passcode, Fn<void(bool)> done) {
	auto &local = Core::App().domain().local();
	local.setPasscodeAsync(passcode, local.passcodeKdf(), [=](bool ok) {
		if (ok) {
			FreshPasscodeGuard::RegisterSuccess();
			Core::App().localPasscodeChanged();
		}
		done(ok);
	});
}

void AddPasscodeStrength(
		not_null<Ui::VerticalLayout*> content,
		not_null<Ui::PasswordInput*> field) {
	const auto &font = st::boxDividerLabel.style.font;
	const auto barHeight = st::lineWidth * 4;
	const auto skip = st::lineWidth * 6;
	const auto widget = content->add(object_ptr<Ui::FixedHeightWidget>(
		content,
		skip + barHeight + skip + font->height));
	const auto text = std::make_shared<QString>();
	const auto level = std::make_shared<FreshPasscode::Strength>();
	const auto refresh = [=] {
		const auto value = field->text();
		*text = FreshPasscode::StrengthText(value);
		*level = FreshPasscode::StrengthForBits(
			FreshPasscode::EstimateBits(value));
		widget->update();
	};
	QObject::connect(field, &Ui::MaskedInputField::changed, refresh);
	widget->paintRequest(
	) | rpl::on_next([=] {
		if (text->isEmpty()) {
			return;
		}
		auto p = QPainter(widget);
		auto hq = PainterHighQualityEnabler(p);
		const auto width = field->width();
		const auto left = (widget->width() - width) / 2;
		const auto gap = st::lineWidth * 4;
		const auto segment = (width - gap * (kStrengthSegments - 1))
			/ kStrengthSegments;
		const auto filled = int(*level);
		const auto &active = (*level == FreshPasscode::Strength::Weak)
			? st::boxTextFgError
			: st::activeButtonBg;
		p.setPen(Qt::NoPen);
		for (auto i = 0; i != kStrengthSegments; ++i) {
			p.setBrush((i < filled) ? active : st::windowBgOver);
			p.drawRoundedRect(
				QRect(left + i * (segment + gap), skip, segment, barHeight),
				barHeight / 2.,
				barHeight / 2.);
		}
		p.setFont(font);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(left, skip + barHeight + skip, width, font->height),
			*text,
			style::al_left);
	}, widget->lifetime());
}

void AddPasscodeSecuritySettings(SectionBuilder &builder) {
	const auto controller = builder.controller();
	if (!controller) {
		return;
	}
	auto &settings = AyuSettings::getInstance();

	const auto addChoice = [&](
			QString id,
			tr::phrase<> title,
			const style::icon *icon,
			std::vector<Choice> choices,
			rpl::producer<int> value,
			Fn<int()> get,
			Fn<void(int)> set,
			rpl::producer<bool> shown = nullptr) {
		auto label = std::move(value) | rpl::map([=](int current) {
			for (const auto &choice : choices) {
				if (choice.value == current) {
					return choice.text;
				}
			}
			return QString();
		});
		builder.addButton({
			.id = std::move(id),
			.title = title(),
			.icon = { icon },
			.label = std::move(label),
			.onClick = [=] {
				auto texts = std::vector<QString>();
				auto selected = 0;
				for (const auto &choice : choices) {
					if (choice.value == get()) {
						selected = int(texts.size());
					}
					texts.push_back(choice.text);
				}
				controller->show(Box(SingleChoiceBox, SingleChoiceBoxArgs{
					.title = title(),
					.options = texts,
					.initialSelection = selected,
					.callback = [=](int index) {
						set(choices[index].value);
					},
				}));
			},
			.shown = shown ? std::move(shown) : rpl::single(true),
		});
	};
	const auto addToggle = [&](
			QString id,
			tr::phrase<> title,
			const style::icon *icon,
			bool value,
			Fn<void(bool)> set,
			rpl::producer<bool> shown = nullptr) {
		const auto button = builder.addButton({
			.id = std::move(id),
			.title = title(),
			.icon = { icon },
			.toggled = rpl::single(value),
			.shown = shown ? std::move(shown) : rpl::single(true),
		});
		if (button) {
			button->toggledChanges(
			) | rpl::on_next(set, button->lifetime());
		}
	};

	const auto kdfChanged = std::make_shared<rpl::event_stream<>>();
	builder.addSkip();
	builder.addSubsectionTitle(tr::fresh_PasscodeEncryption());
	builder.addButton({
		.id = u"passcode/kdf"_q,
		.title = tr::fresh_PasscodeKdf(),
		.icon = { &st::menuIconPermissions },
		.label = kdfChanged->events_starting_with({}) | rpl::map([] {
			return KdfLevelName(Storage::PasscodeKdfLevelOf(
				Core::App().domain().local().passcodeKdf()));
		}),
		.onClick = [=] {
			controller->show(Box(PasscodeKdfBox, [=] {
				kdfChanged->fire({});
			}));
		},
		.keywords = { u"argon2"_q, u"kdf"_q, u"encryption"_q, u"entropy"_q },
	});
	builder.addSkip();
	builder.addDividerText(tr::fresh_PasscodeKdfAbout());

	builder.addSkip();
	builder.addSubsectionTitle(tr::fresh_PasscodeWrongInput());
	auto attempts = std::vector<Choice>{ { 0, OffText() } };
	for (const auto count : { 5, 10, 15, 20 }) {
		attempts.push_back({ count, QString::number(count) });
	}
	addChoice(
		u"passcode/wipe"_q,
		tr::fresh_PasscodeWipeAfter,
		&st::menuIconDelete,
		std::move(attempts),
		settings.passcodeWipeAttemptsValue(),
		[] { return AyuSettings::getInstance().passcodeWipeAttempts(); },
		[](int value) {
			AyuSettings::getInstance().setPasscodeWipeAttempts(value);
		});
	addChoice(
		u"passcode/wipe-action"_q,
		tr::fresh_PasscodeWipeAction,
		&st::menuIconLeave,
		{
			{ 0, tr::fresh_PasscodeWipeLogout(tr::now) },
			{ 1, tr::fresh_PasscodeWipeDeleteAll(tr::now) },
		},
		settings.passcodeWipeActionValue(),
		[] { return AyuSettings::getInstance().passcodeWipeAction(); },
		[](int value) {
			AyuSettings::getInstance().setPasscodeWipeAction(value);
		},
		settings.passcodeWipeAttemptsValue() | rpl::map(rpl::mappers::_1 > 0));
	builder.addSkip();
	builder.addDividerText(tr::fresh_PasscodeWipeAbout());

	builder.addSkip();
	builder.addSubsectionTitle(tr::fresh_PasscodeLockScreen());
	const auto cardStyle = [&] {
		return settings.lockScreenStyleValue(
		) | rpl::map(rpl::mappers::_1 == 0);
	};
	const auto gradientShown = [&] {
		return rpl::combine(
			settings.lockScreenStyleValue(),
			settings.lockScreenBackgroundValue(),
			[](int style, int background) {
				return (style == 0) && (background == 0);
			});
	};
	const auto colorShown = [&] {
		return rpl::combine(
			settings.lockScreenStyleValue(),
			settings.lockScreenBackgroundValue(),
			[](int style, int background) {
				return (style == 0) && (background <= 1);
			});
	};
	const auto imageShown = [&] {
		return rpl::combine(
			settings.lockScreenStyleValue(),
			settings.lockScreenBackgroundValue(),
			[](int style, int background) {
				return (style == 0) && (background == 2);
			});
	};
	addChoice(
		u"passcode/lock-style"_q,
		tr::fresh_PasscodeLockStyle,
		&st::menuIconCustomize,
		{
			{ 0, tr::fresh_PasscodeLockStyleFresh(tr::now) },
			{ 1, tr::fresh_PasscodeLockStyleTelegram(tr::now) },
		},
		settings.lockScreenStyleValue(),
		[] { return AyuSettings::getInstance().lockScreenStyle(); },
		[](int value) {
			AyuSettings::getInstance().setLockScreenStyle(value);
		});
	const auto chooseImage = [=] {
		ChooseLockBackground(controller);
	};
	addChoice(
		u"passcode/lock-background"_q,
		tr::fresh_PasscodeLockBackground,
		&st::menuIconPalette,
		{
			{ 0, tr::fresh_PasscodeLockBackgroundGradient(tr::now) },
			{ 1, tr::fresh_PasscodeLockBackgroundSolid(tr::now) },
			{ 2, tr::fresh_PasscodeLockBackgroundImage(tr::now) },
		},
		settings.lockScreenBackgroundValue(),
		[] { return AyuSettings::getInstance().lockScreenBackground(); },
		[=](int value) {
			if (value == 2) {
				chooseImage();
			} else {
				AyuSettings::getInstance().setLockScreenBackground(value);
			}
		},
		cardStyle());
	const auto colorLabel = [](uint32 color) {
		return color
			? QColor::fromRgba(color).name().toUpper()
			: tr::fresh_PasscodeLockColorDefault(tr::now);
	};
	builder.addButton({
		.id = u"passcode/lock-color1"_q,
		.title = tr::fresh_PasscodeLockColor(),
		.icon = { &st::menuIconPalette },
		.label = settings.lockScreenColor1Value() | rpl::map(colorLabel),
		.onClick = [=] {
			controller->show(Box(
				LockColorBox,
				tr::fresh_PasscodeLockColor,
				AyuSettings::getInstance().lockScreenColor1(),
				[](uint32 color) {
					AyuSettings::getInstance().setLockScreenColor1(color);
				}));
		},
		.shown = colorShown(),
	});
	builder.addButton({
		.id = u"passcode/lock-color2"_q,
		.title = tr::fresh_PasscodeLockColorSecond(),
		.icon = { &st::menuIconPalette },
		.label = settings.lockScreenColor2Value() | rpl::map(colorLabel),
		.onClick = [=] {
			controller->show(Box(
				LockColorBox,
				tr::fresh_PasscodeLockColorSecond,
				AyuSettings::getInstance().lockScreenColor2(),
				[](uint32 color) {
					AyuSettings::getInstance().setLockScreenColor2(color);
				}));
		},
		.shown = gradientShown(),
	});
	builder.addButton({
		.id = u"passcode/lock-image"_q,
		.title = tr::fresh_PasscodeLockChangeImage(),
		.icon = { &st::menuIconPhoto },
		.onClick = chooseImage,
		.shown = imageShown(),
	});
	addToggle(
		u"passcode/lock-blur"_q,
		tr::fresh_PasscodeLockBlur,
		&st::menuIconSpoiler,
		settings.lockScreenBlur(),
		[](bool value) {
			AyuSettings::getInstance().setLockScreenBlur(value);
		},
		imageShown());
	auto dims = std::vector<Choice>{ { 0, OffText() } };
	for (const auto dim : { 25, 50, 75 }) {
		dims.push_back({ dim, QString::number(dim) + '%' });
	}
	addChoice(
		u"passcode/lock-dim"_q,
		tr::fresh_PasscodeLockDim,
		&st::menuIconNightMode,
		std::move(dims),
		settings.lockScreenDimValue(),
		[] { return AyuSettings::getInstance().lockScreenDim(); },
		[](int value) {
			AyuSettings::getInstance().setLockScreenDim(value);
		},
		imageShown());
	addToggle(
		u"passcode/clock"_q,
		tr::fresh_PasscodeShowClock,
		&st::menuIconSchedule,
		settings.lockScreenShowClock(),
		[](bool value) {
			AyuSettings::getInstance().setLockScreenShowClock(value);
		},
		cardStyle());
	addToggle(
		u"passcode/date"_q,
		tr::fresh_PasscodeShowDate,
		&st::menuIconOrderDate,
		settings.lockScreenShowDate(),
		[](bool value) {
			AyuSettings::getInstance().setLockScreenShowDate(value);
		},
		cardStyle());
	builder.addButton({
		.id = u"passcode/lock-text"_q,
		.title = tr::fresh_PasscodeLockText(),
		.icon = { &st::menuIconArticle },
		.label = settings.lockScreenTextValue(
		) | rpl::map([](const QString &text) {
			return text.isEmpty()
				? OffText()
				: text.split(QChar('\n')).front();
		}),
		.onClick = [=] {
			controller->show(Box(LockTextBox));
		},
	});
	addToggle(
		u"passcode/lock-logout"_q,
		tr::fresh_PasscodeLockShowLogout,
		&st::menuIconLeave,
		settings.lockScreenShowLogout(),
		[](bool value) {
			AyuSettings::getInstance().setLockScreenShowLogout(value);
		});
	addToggle(
		u"passcode/lock-eye"_q,
		tr::fresh_PasscodeLockShowEye,
		&st::menuIconShowInChat,
		settings.lockScreenShowEye(),
		[](bool value) {
			AyuSettings::getInstance().setLockScreenShowEye(value);
		});
	const auto avatarSet = [&] {
		return rpl::combine(
			settings.lockScreenStyleValue(),
			settings.lockScreenAvatarValue(),
			[](int style, bool avatar) {
				return (style == 0) && avatar;
			});
	};
	const auto clockShown = [&] {
		return rpl::combine(
			settings.lockScreenStyleValue(),
			settings.lockScreenShowClockValue(),
			[](int style, bool clock) {
				return (style == 0) && clock;
			});
	};
	builder.addButton({
		.id = u"passcode/lock-avatar"_q,
		.title = tr::fresh_PasscodeLockAvatar(),
		.icon = { &st::menuIconProfile },
		.label = settings.lockScreenAvatarValue(
		) | rpl::map([](bool avatar) {
			return avatar
				? tr::fresh_PasscodeLockAvatarImage(tr::now)
				: tr::fresh_PasscodeLockAvatarLock(tr::now);
		}),
		.onClick = [=] {
			ChooseLockAvatar(controller);
		},
		.shown = cardStyle(),
	});
	builder.addButton({
		.id = u"passcode/lock-avatar-remove"_q,
		.title = tr::fresh_PasscodeLockAvatarRemove(),
		.icon = { &st::menuIconDelete },
		.onClick = [] {
			FreshLockBackground::RemoveAvatar();
		},
		.shown = avatarSet(),
	});
	addChoice(
		u"passcode/lock-avatar-shape"_q,
		tr::fresh_PasscodeLockAvatarShape,
		&st::menuIconCustomize,
		{
			{ 0, tr::fresh_PasscodeLockShapeCircle(tr::now) },
			{ 1, tr::fresh_PasscodeLockShapeRounded(tr::now) },
			{ 2, tr::fresh_PasscodeLockShapeSquare(tr::now) },
		},
		settings.lockScreenAvatarShapeValue(),
		[] { return AyuSettings::getInstance().lockScreenAvatarShape(); },
		[](int value) {
			AyuSettings::getInstance().setLockScreenAvatarShape(value);
		},
		cardStyle());
	auto opacities = std::vector<Choice>();
	for (const auto opacity : { 100, 80, 60, 40 }) {
		opacities.push_back({ opacity, QString::number(opacity) + '%' });
	}
	addChoice(
		u"passcode/lock-card-opacity"_q,
		tr::fresh_PasscodeLockCardOpacity,
		&st::menuIconSpoiler,
		std::move(opacities),
		settings.lockScreenCardOpacityValue(),
		[] { return AyuSettings::getInstance().lockScreenCardOpacity(); },
		[](int value) {
			AyuSettings::getInstance().setLockScreenCardOpacity(value);
		},
		cardStyle());
	addChoice(
		u"passcode/lock-card-radius"_q,
		tr::fresh_PasscodeLockCardRadius,
		&st::menuIconCustomize,
		{
			{ 0, tr::fresh_PasscodeLockColorDefault(tr::now) },
			{ 1, tr::fresh_PasscodeLockRadiusNone(tr::now) },
			{ 2, tr::fresh_PasscodeLockRadiusSmall(tr::now) },
			{ 3, tr::fresh_PasscodeLockRadiusLarge(tr::now) },
		},
		settings.lockScreenCardRadiusValue(),
		[] { return AyuSettings::getInstance().lockScreenCardRadius(); },
		[](int value) {
			AyuSettings::getInstance().setLockScreenCardRadius(value);
		},
		cardStyle());
	addToggle(
		u"passcode/lock-card-border"_q,
		tr::fresh_PasscodeLockCardBorder,
		&st::menuIconCollapse,
		settings.lockScreenCardBorder(),
		[](bool value) {
			AyuSettings::getInstance().setLockScreenCardBorder(value);
		},
		cardStyle());
	addChoice(
		u"passcode/lock-clock-format"_q,
		tr::fresh_PasscodeLockClockFormat,
		&st::menuIconSchedule,
		{
			{ 0, tr::fresh_PasscodeLockClockSystem(tr::now) },
			{ 1, tr::fresh_PasscodeLockClock24(tr::now) },
			{ 2, tr::fresh_PasscodeLockClock12(tr::now) },
		},
		settings.lockScreenClockFormatValue(),
		[] { return AyuSettings::getInstance().lockScreenClockFormat(); },
		[](int value) {
			AyuSettings::getInstance().setLockScreenClockFormat(value);
		},
		clockShown());
	addToggle(
		u"passcode/lock-clock-seconds"_q,
		tr::fresh_PasscodeLockClockSeconds,
		&st::menuIconSchedule,
		settings.lockScreenClockSeconds(),
		[](bool value) {
			AyuSettings::getInstance().setLockScreenClockSeconds(value);
		},
		clockShown());
	addChoice(
		u"passcode/lock-clock-size"_q,
		tr::fresh_PasscodeLockClockSize,
		&st::menuIconFont,
		{
			{ 0, tr::fresh_PasscodeLockSizeSmall(tr::now) },
			{ 1, tr::fresh_PasscodeLockSizeNormal(tr::now) },
			{ 2, tr::fresh_PasscodeLockSizeLarge(tr::now) },
		},
		settings.lockScreenClockSizeValue(),
		[] { return AyuSettings::getInstance().lockScreenClockSize(); },
		[](int value) {
			AyuSettings::getInstance().setLockScreenClockSize(value);
		},
		clockShown());
	builder.addButton({
		.id = u"passcode/lock-clock-color"_q,
		.title = tr::fresh_PasscodeLockClockColor(),
		.icon = { &st::menuIconPalette },
		.label = settings.lockScreenClockColorValue() | rpl::map(colorLabel),
		.onClick = [=] {
			controller->show(Box(
				LockColorBox,
				tr::fresh_PasscodeLockClockColor,
				AyuSettings::getInstance().lockScreenClockColor(),
				[](uint32 color) {
					AyuSettings::getInstance().setLockScreenClockColor(color);
				}));
		},
		.shown = clockShown(),
	});
	const auto lineLabel = [](const QString &text) {
		return text.isEmpty()
			? tr::fresh_PasscodeLockColorDefault(tr::now)
			: text;
	};
	builder.addButton({
		.id = u"passcode/lock-title"_q,
		.title = tr::fresh_PasscodeLockTitle(),
		.icon = { &st::menuIconEdit },
		.label = settings.lockScreenTitleValue() | rpl::map(lineLabel),
		.onClick = [=] {
			controller->show(Box(
				LockLineBox,
				tr::fresh_PasscodeLockTitle,
				tr::fresh_PasscodeLockTitlePlaceholder,
				AyuSettings::getInstance().lockScreenTitle(),
				40,
				[](QString text) {
					AyuSettings::getInstance().setLockScreenTitle(
						std::move(text));
				}));
		},
		.shown = cardStyle(),
	});
	builder.addButton({
		.id = u"passcode/lock-submit-text"_q,
		.title = tr::fresh_PasscodeLockSubmitText(),
		.icon = { &st::menuIconEdit },
		.label = settings.lockScreenSubmitTextValue() | rpl::map(lineLabel),
		.onClick = [=] {
			controller->show(Box(
				LockLineBox,
				tr::fresh_PasscodeLockSubmitText,
				tr::fresh_PasscodeLockSubmitPlaceholder,
				AyuSettings::getInstance().lockScreenSubmitText(),
				24,
				[](QString text) {
					AyuSettings::getInstance().setLockScreenSubmitText(
						std::move(text));
				}));
		},
		.shown = cardStyle(),
	});
	builder.addButton({
		.id = u"passcode/lock-accent"_q,
		.title = tr::fresh_PasscodeLockAccent(),
		.icon = { &st::menuIconPalette },
		.label = settings.lockScreenAccentValue() | rpl::map(colorLabel),
		.onClick = [=] {
			controller->show(Box(
				LockColorBox,
				tr::fresh_PasscodeLockAccent,
				AyuSettings::getInstance().lockScreenAccent(),
				[](uint32 color) {
					AyuSettings::getInstance().setLockScreenAccent(color);
				}));
		},
		.shown = cardStyle(),
	});
	builder.addButton({
		.id = u"passcode/lock-preview"_q,
		.title = tr::fresh_PasscodeLockPreview(),
		.icon = { &st::menuIconShowAll },
		.onClick = [=] {
			controller->show(Box(LockPreviewBox, controller));
		},
	});
	builder.addSkip();
	builder.addDividerText(tr::fresh_PasscodeLockAppearanceAbout());

	builder.addSkip();
	builder.addSubsectionTitle(tr::fresh_PasscodeAutoLock());
	addToggle(
		u"passcode/system-lock"_q,
		tr::fresh_PasscodeLockOnSystemLock,
		&st::menuIconLock,
		settings.lockOnSystemLock(),
		[](bool value) {
			AyuSettings::getInstance().setLockOnSystemLock(value);
		});
	addChoice(
		u"passcode/minimize"_q,
		tr::fresh_PasscodeLockOnMinimize,
		&st::menuIconCollapse,
		{
			{ -1, OffText() },
			{ 0, tr::fresh_PasscodeImmediately(tr::now) },
			{ 10, tr::lng_seconds(tr::now, lt_count, 10) },
			{ 60, tr::lng_minutes(tr::now, lt_count, 1) },
			{ 300, tr::lng_minutes(tr::now, lt_count, 5) },
		},
		settings.lockOnMinimizeValue(),
		[] { return AyuSettings::getInstance().lockOnMinimize(); },
		[](int value) {
			AyuSettings::getInstance().setLockOnMinimize(value);
		});
	if (!Platform::IsLinux()) {
		addChoice(
			u"passcode/capture"_q,
			tr::fresh_PasscodeHideFromCapture,
			&st::menuIconStealth,
			{
				{ 0, OffText() },
				{ 1, tr::fresh_PasscodeCaptureLocked(tr::now) },
				{ 2, tr::fresh_PasscodeCaptureAlways(tr::now) },
			},
			settings.hideFromCaptureValue(),
			[] { return AyuSettings::getInstance().hideFromCapture(); },
			[](int value) {
				AyuSettings::getInstance().setHideFromCapture(value);
			});
	}
	addChoice(
		u"passcode/notifications"_q,
		tr::fresh_PasscodeLockedNotifications,
		&st::menuIconNotifications,
		{
			{ 0, tr::fresh_PasscodeNotifyHidden(tr::now) },
			{ 1, tr::fresh_PasscodeNotifySilent(tr::now) },
			{ 2, tr::fresh_PasscodeNotifyNone(tr::now) },
		},
		settings.lockedNotificationsValue(),
		[] { return AyuSettings::getInstance().lockedNotifications(); },
		[](int value) {
			AyuSettings::getInstance().setLockedNotifications(value);
		});
	builder.addSkip();
}

} // namespace AyuUi
