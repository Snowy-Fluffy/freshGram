// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/settings/settings_ayu.h"

#include "lang_auto.h"
#include "ayu/ayu_settings.h"
#include "ayu/data/messages_storage.h"
#include "ayu/secret/secret_policy.h"
#include "ayu/ui/ayu_userpic.h"
#include "ayu/ui/settings/ayu_builder.h"
#include "ayu/ui/settings/settings_ayu_utils.h"
#include "ayu/ui/settings/settings_main.h"
#include "boxes/peer_list_box.h"
#include "core/application.h"
#include "data/data_user.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "styles/style_ayu_icons.h"
#include "styles/style_ayu_styles.h"
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_window.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/boxes/confirm_box.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/text/text.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/menu/menu_item_base.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include <limits>

namespace Settings {

using namespace Builder;
using namespace AyuBuilder;

namespace {

struct GhostPickerState {
	rpl::variable<uint64> selectedUserId;
	base::unique_qptr<Ui::PopupMenu> menu;
	Ui::LinkButton *pickerButton = nullptr;
	Fn<void()> refreshCheckboxes;
};

struct AccountUserpicGeometry {
	QRect outer;
	QRect inner;
};

[[nodiscard]] AccountUserpicGeometry AccountUserpic(int height) {
	const auto line = st::mainMenuAccountLine;
	const auto skip = 2 * line + st::lineWidth;
	const auto full = st::mainMenuAccountSize + 2 * skip;
	const auto outer = QRect(
		st::defaultWhoRead.photoLeft
			+ (st::defaultWhoRead.photoSize - full) / 2,
		(height - full) / 2,
		full,
		full);
	return {
		.outer = outer,
		.inner = QRect(
			outer.x() + skip,
			outer.y() + skip,
			st::mainMenuAccountSize,
			st::mainMenuAccountSize),
	};
}

void PaintAccountOutline(Painter &p, QRect outer) {
	const auto line = st::mainMenuAccountLine;
	const auto shift = st::lineWidth + (line * 0.5);
	const auto rect = QRectF(outer).marginsRemoved(QMarginsF(
		shift,
		shift,
		shift,
		shift));
	auto hq = PainterHighQualityEnabler(p);
	auto pen = st::windowBgActive->p;
	pen.setWidthF(line);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	AyuUserpic::PaintShape(p, rect);
}

class AccountAction final : public Ui::Menu::ItemBase {
public:
	AccountAction(
		not_null<Ui::Menu::Menu*> parent,
		const style::Menu &st,
		UserData *user,
		bool active,
		Fn<void()> callback)
	: ItemBase(parent, st)
	, _dummyAction(Ui::CreateChild<QAction>(parent.get()))
	, _user(user)
	, _active(active)
	, _st(st)
	, _height(st::defaultWhoRead.photoSkip * 2 + st::defaultWhoRead.photoSize) {
		setAcceptBoth(true);
		fitToMenuWidth();

		_text.setText(_st.itemStyle, user->name());
		const auto goodWidth = st::defaultWhoRead.nameLeft
			+ _text.maxWidth()
			+ _st.itemPadding.right();
		setMinWidth(std::clamp(goodWidth, _st.widthMin, _st.widthMax));

		setActionTriggered(std::move(callback));

		paintRequest(
		) | rpl::on_next([=] {
			paint(Painter(this));
		}, lifetime());

		enableMouseSelecting();
	}

	not_null<QAction*> action() const override { return _dummyAction; }
	bool isEnabled() const override { return true; }

protected:
	int contentHeight() const override { return _height; }

private:
	void paint(Painter &&p) {
		const auto selected = isSelected();
		if (selected && _st.itemBgOver->c.alpha() < 255) {
			p.fillRect(0, 0, width(), _height, _st.itemBg);
		}
		const auto bg = selected ? _st.itemBgOver : _st.itemBg;
		p.fillRect(0, 0, width(), _height, bg);
		if (isEnabled()) {
			paintRipple(p, 0, 0);
		}

		const auto userpic = AccountUserpic(_height);
		_user->paintUserpicLeft(
			p,
			_userpicView,
			userpic.inner.x(),
			userpic.inner.y(),
			width(),
			userpic.inner.width());
		if (_active) {
			PaintAccountOutline(p, userpic.outer);
		}

		p.setPen(selected ? _st.itemFgOver : _st.itemFg);
		_text.drawLeftElided(
			p,
			st::defaultWhoRead.nameLeft,
			(_height - _st.itemStyle.font->height) / 2,
			width() - st::defaultWhoRead.nameLeft - _st.itemPadding.right(),
			width());
	}

	const not_null<QAction*> _dummyAction;
	UserData *_user = nullptr;
	mutable Ui::PeerUserpicView _userpicView;
	const bool _active = false;
	const style::Menu &_st;
	Ui::Text::String _text;
	const int _height;
};

class GlobalAction final : public Ui::Menu::ItemBase {
public:
	GlobalAction(
		not_null<Ui::Menu::Menu*> parent,
		const style::Menu &st,
		const QString &text,
		bool active,
		Fn<void()> callback)
	: ItemBase(parent, st)
	, _dummyAction(Ui::CreateChild<QAction>(parent.get()))
	, _active(active)
	, _st(st)
	, _height(st::defaultWhoRead.photoSkip * 2 + st::defaultWhoRead.photoSize) {
		setAcceptBoth(true);
		fitToMenuWidth();

		_text.setText(_st.itemStyle, text);
		const auto goodWidth = st::defaultWhoRead.nameLeft
			+ _text.maxWidth()
			+ _st.itemPadding.right();
		setMinWidth(std::clamp(goodWidth, _st.widthMin, _st.widthMax));

		setActionTriggered(std::move(callback));

		paintRequest(
		) | rpl::on_next([=] {
			paint(Painter(this));
		}, lifetime());

		enableMouseSelecting();
	}

	not_null<QAction*> action() const override { return _dummyAction; }
	bool isEnabled() const override { return true; }

protected:
	int contentHeight() const override { return _height; }

private:
	void paint(Painter &&p) {
		const auto selected = isSelected();
		if (selected && _st.itemBgOver->c.alpha() < 255) {
			p.fillRect(0, 0, width(), _height, _st.itemBg);
		}
		const auto bg = selected ? _st.itemBgOver : _st.itemBg;
		p.fillRect(0, 0, width(), _height, bg);
		if (isEnabled()) {
			paintRipple(p, 0, 0);
		}

		const auto userpic = AccountUserpic(_height);
		{
			auto hq = PainterHighQualityEnabler(p);
			auto rect = QRectF(userpic.inner);
			auto gradient = QLinearGradient(rect.topLeft(), rect.bottomLeft());
			gradient.setStops({
				{ 0., st::historyPeer5UserpicBg->c },
				{ 1., st::historyPeer5UserpicBg2->c },
			});
			p.setPen(Qt::NoPen);
			p.setBrush(gradient);
			AyuUserpic::PaintShape(p, rect);
		}
		{
			auto hq = PainterHighQualityEnabler(p);
			p.drawImage(
				userpic.inner,
				st::ayuGhostModeGlobalIcon.instance(st::historyPeerUserpicFg->c));
		}
		if (_active) {
			PaintAccountOutline(p, userpic.outer);
		}

		p.setPen(selected ? _st.itemFgOver : _st.itemFg);
		_text.drawLeftElided(
			p,
			st::defaultWhoRead.nameLeft,
			(_height - _st.itemStyle.font->height) / 2,
			width() - st::defaultWhoRead.nameLeft - _st.itemPadding.right(),
			width());
	}

	const not_null<QAction*> _dummyAction;
	const bool _active = false;
	const style::Menu &_st;
	Ui::Text::String _text;
	const int _height;
};

QString GetAccountName(uint64 userId) {
	for (const auto &account : Core::App().domain().orderedAccounts()) {
		if (account->sessionExists()
			&& account->session().userId().bare == userId) {
			return account->session().user()->name();
		}
	}
	return QString("Unknown");
}

QString PickerLabel(uint64 userId) {
	return (userId == 0)
		? tr::ayu_GhostModeGlobalSettings(tr::now)
		: GetAccountName(userId);
}

void selectGhostProfile(GhostPickerState *state, uint64 userId) {
	if (state->selectedUserId.current() == userId) {
		return;
	}

	auto wasGlobal = (state->selectedUserId.current() == 0);
	auto nowGlobal = (userId == 0);

	AyuSettings::getInstance().setUseGlobalGhostMode(nowGlobal);

	state->selectedUserId = userId;

	state->pickerButton->setText(PickerLabel(userId));

	state->refreshCheckboxes();

	if (wasGlobal != nowGlobal) {
		Ui::Toast::Show(nowGlobal
			? tr::ayu_GhostModeSwitchedToGlobalSettings(tr::now)
			: tr::ayu_GhostModeSwitchedToIndividualSettings(tr::now));
	}
}

void BuildGhostEssentials(SectionBuilder &builder) {
	builder.add([](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			const auto container = wctx.container;
			const auto controller = wctx.controller;

			auto activeCount = 0;
			for (const auto &account : Core::App().domain().orderedAccounts()) {
				if (account->sessionExists()) {
					++activeCount;
				}
			}

			if (activeCount <= 1 && !AyuSettings::getInstance().useGlobalGhostMode()) {
				auto userId = controller->session().userId().bare;
				auto &src = AyuSettings::ghost(userId);
				auto &dst = AyuSettings::ghost(0);
				dst.setSendReadMessages(src.sendReadMessages());
				dst.setSendReadStories(src.sendReadStories());
				dst.setSendOnlinePackets(src.sendOnlinePackets());
				dst.setSendUploadProgress(src.sendUploadProgress());
				dst.setSendOfflinePacketAfterOnline(src.sendOfflinePacketAfterOnline());
				dst.setMarkReadAfterAction(src.markReadAfterAction());
				dst.setUseScheduledMessages(src.useScheduledMessages());
				dst.setSendWithoutSound(src.sendWithoutSound());
				dst.setSuggestGhostModeBeforeViewingStory(src.suggestGhostModeBeforeViewingStory());
				dst.setSendReadMessagesLocked(src.sendReadMessagesLocked());
				dst.setSendReadStoriesLocked(src.sendReadStoriesLocked());
				dst.setSendOnlinePacketsLocked(src.sendOnlinePacketsLocked());
				dst.setSendUploadProgressLocked(src.sendUploadProgressLocked());
				dst.setSendOfflinePacketAfterOnlineLocked(src.sendOfflinePacketAfterOnlineLocked());
				dst.setUseScheduledMessagesLocked(src.useScheduledMessagesLocked());
				AyuSettings::getInstance().setUseGlobalGhostMode(true);
			}

			const auto isGlobal = AyuSettings::getInstance().useGlobalGhostMode();
			auto initialUserId = isGlobal
				? uint64(0)
				: controller->session().userId().bare;

			const auto state = container->lifetime().make_state<GhostPickerState>();
			state->selectedUserId = initialUserId;

			const auto title = AddSubsectionTitle(container, tr::ayu_GhostEssentialsHeader());

			const auto pickerButton = Ui::CreateChild<Ui::LinkButton>(
				container.get(),
				PickerLabel(initialUserId),
				st::ghostPickerButton);
			state->pickerButton = pickerButton;

			const auto arrow = Ui::CreateChild<Ui::AbstractButton>(container.get());
			{
				const auto &icon = st::ghostPickerArrow;
				arrow->resize(icon.size());
				arrow->paintRequest(
				) | rpl::on_next([=, &icon] {
					auto p = QPainter(arrow);
					icon.paint(p, 0, 0, arrow->width());
				}, arrow->lifetime());
			}
			arrow->setCursor(style::cur_pointer);

			const auto showPicker = activeCount > 1;
			pickerButton->setVisible(showPicker);
			arrow->setVisible(showPicker);

			rpl::combine(
				title->geometryValue(),
				container->widthValue(),
				pickerButton->naturalWidthValue()
			) | rpl::on_next([=](QRect r, int width, int natural) {
				pickerButton->resizeToNaturalWidth(width / 2);
				pickerButton->moveToRight(
					st::defaultSubsectionTitlePadding.right() + arrow->width() + st::normalFont->spacew / 2,
					r.y() + (r.height() - pickerButton->height()) / 2,
					width);
				arrow->moveToLeft(
					pickerButton->x() + pickerButton->width() + st::normalFont->spacew / 2,
					r.y() + (r.height() - arrow->height()) / 2);
			}, pickerButton->lifetime());

			std::vector checkboxes{
				NestedEntry{
					tr::ayu_DontReadMessages(tr::now),
					[state] { return !AyuSettings::ghost(state->selectedUserId.current()).sendReadMessages(); },
					[state](bool v) { AyuSettings::ghost(state->selectedUserId.current()).setSendReadMessages(!v); },
					[state] { return AyuSettings::ghost(state->selectedUserId.current()).sendReadMessagesLocked(); },
					[state](bool v) { AyuSettings::ghost(state->selectedUserId.current()).setSendReadMessagesLocked(v); }
				},
				NestedEntry{
					tr::ayu_DontReadStories(tr::now),
					[state] { return !AyuSettings::ghost(state->selectedUserId.current()).sendReadStories(); },
					[state](bool v) { AyuSettings::ghost(state->selectedUserId.current()).setSendReadStories(!v); },
					[state] { return AyuSettings::ghost(state->selectedUserId.current()).sendReadStoriesLocked(); },
					[state](bool v) { AyuSettings::ghost(state->selectedUserId.current()).setSendReadStoriesLocked(v); }
				},
				NestedEntry{
					tr::ayu_DontSendOnlinePackets(tr::now),
					[state] { return !AyuSettings::ghost(state->selectedUserId.current()).sendOnlinePackets(); },
					[state](bool v) { AyuSettings::ghost(state->selectedUserId.current()).setSendOnlinePackets(!v); },
					[state] { return AyuSettings::ghost(state->selectedUserId.current()).sendOnlinePacketsLocked(); },
					[state](bool v) { AyuSettings::ghost(state->selectedUserId.current()).setSendOnlinePacketsLocked(v); }
				},
				NestedEntry{
					tr::ayu_DontSendUploadProgress(tr::now),
					[state] { return !AyuSettings::ghost(state->selectedUserId.current()).sendUploadProgress(); },
					[state](bool v) { AyuSettings::ghost(state->selectedUserId.current()).setSendUploadProgress(!v); },
					[state] { return AyuSettings::ghost(state->selectedUserId.current()).sendUploadProgressLocked(); },
					[state](bool v) { AyuSettings::ghost(state->selectedUserId.current()).setSendUploadProgressLocked(v); }
				},
				NestedEntry{
					tr::ayu_SendOfflinePacketAfterOnline(tr::now),
					[state] { return AyuSettings::ghost(state->selectedUserId.current()).sendOfflinePacketAfterOnline(); },
					[state](bool v) { AyuSettings::ghost(state->selectedUserId.current()).setSendOfflinePacketAfterOnline(v); },
					[state] { return AyuSettings::ghost(state->selectedUserId.current()).sendOfflinePacketAfterOnlineLocked(); },
					[state](bool v) { AyuSettings::ghost(state->selectedUserId.current()).setSendOfflinePacketAfterOnlineLocked(v); }
				},
				NestedEntry{
					tr::ayu_UseScheduledMessages(tr::now),
					[state] { return AyuSettings::ghost(state->selectedUserId.current()).useScheduledMessages(); },
					[state](bool v) {
						auto &ghost = AyuSettings::ghost(state->selectedUserId.current());
						ghost.setUseScheduledMessages(v);
						if (v) {
							ghost.setMarkReadAfterAction(false);
						}
					},
					[state] { return AyuSettings::ghost(state->selectedUserId.current()).useScheduledMessagesLocked(); },
					[state](bool v) { AyuSettings::ghost(state->selectedUserId.current()).setUseScheduledMessagesLocked(v); }
				},
			};

			auto collapsible = AddCollapsibleToggle(
				container,
				tr::ayu_GhostModeToggle(),
				std::move(checkboxes),
				true,
				tr::ayu_GhostModeOptionShiftDescription(tr::now));
			state->refreshCheckboxes = std::move(collapsible.refresh);
			if (wctx.highlights && collapsible.widget) {
				wctx.highlights->push_back(std::make_pair(
					u"ayu/ghostModeToggle"_q,
					HighlightEntry{ collapsible.widget, {} }));
				wctx.highlights->push_back(std::make_pair(
					u"ayu/useScheduledMessages"_q,
					HighlightEntry{ collapsible.widget, {} }));
			}

			const auto markReadButton = AddButtonWithIcon(
				container,
				tr::ayu_MarkReadAfterAction(),
				st::settingsButtonNoIcon
			);
			if (wctx.highlights) {
				wctx.highlights->push_back(std::make_pair(
					u"ayu/markReadAfterAction"_q,
					HighlightEntry{ markReadButton.get(), {} }));
			}
			markReadButton->toggleOn(
				state->selectedUserId.value()
				| rpl::map([](uint64 id) {
					return AyuSettings::ghost(id).markReadAfterActionValue();
				}) | rpl::flatten_latest()
			)->toggledValue(
			) | rpl::filter(
				[=](bool enabled) {
					return enabled != AyuSettings::ghost(state->selectedUserId.current()).markReadAfterAction();
				}
			) | on_next(
				[=](bool enabled) {
					auto &ghost = AyuSettings::ghost(state->selectedUserId.current());
					ghost.setMarkReadAfterAction(enabled);
					if (enabled) {
						ghost.setUseScheduledMessages(false);
					}
				},
				container->lifetime());
			AddSkip(container);
			AddDividerText(container, tr::ayu_MarkReadAfterActionDescription());

			AddSkip(container);
			const auto silentOptions = std::vector<QString>{
				tr::ayu_SendWithoutSoundByDefaultNever(tr::now),
				tr::ayu_SendWithoutSoundByDefaultInGhostMode(tr::now),
				tr::ayu_SendWithoutSoundByDefaultAlways(tr::now),
			};
			const auto silentOptionText = state->selectedUserId.value(
			) | rpl::map([=](uint64 id) {
				return AyuSettings::ghost(id).sendWithoutSoundValue(
				) | rpl::map([=](SendWithoutSoundOption value) {
					return silentOptions[static_cast<int>(value)];
				});
			}) | rpl::flatten_latest();
			const auto silentButton = AddButtonWithLabel(
				container,
				tr::ayu_SendWithoutSoundByDefault(),
				std::move(silentOptionText),
				st::settingsButtonNoIcon);
			if (wctx.highlights) {
				wctx.highlights->push_back(std::make_pair(
					u"ayu/sendWithoutSound"_q,
					HighlightEntry{ silentButton.get(), {} }));
			}
			silentButton->addClickHandler([=] {
				controller->show(Box([=](not_null<Ui::GenericBox*> box) {
					const auto save = [=](int index) {
						AyuSettings::ghost(state->selectedUserId.current()
						).setSendWithoutSound(
							static_cast<SendWithoutSoundOption>(index));
					};
					SingleChoiceBox(box, {
						.title = tr::ayu_SendWithoutSoundByDefault(),
						.options = silentOptions,
						.initialSelection = static_cast<int>(
							AyuSettings::ghost(state->selectedUserId.current()
							).sendWithoutSound()),
						.callback = save,
					});
				}));
			});
			AddSkip(container);
			AddDividerText(container, tr::ayu_SendWithoutSoundByDefaultDescription());

			AddSkip(container);
			const auto suggestGhostModeButton = AddButtonWithIcon(
				container,
				tr::ayu_SuggestGhostModeBeforeViewingStory(),
				st::settingsButtonNoIcon);
			if (wctx.highlights) {
				wctx.highlights->push_back(std::make_pair(
					u"ayu/suggestGhostModeBeforeViewingStory"_q,
					HighlightEntry{ suggestGhostModeButton.get(), {} }));
			}
			suggestGhostModeButton->toggleOn(
				state->selectedUserId.value()
				| rpl::map([](uint64 id) {
					return AyuSettings::ghost(id).suggestGhostModeBeforeViewingStoryValue();
				}) | rpl::flatten_latest()
			)->toggledValue(
			) | rpl::filter(
				[=](bool enabled) {
					return enabled != AyuSettings::ghost(state->selectedUserId.current()).suggestGhostModeBeforeViewingStory();
				}
			) | on_next(
				[=](bool enabled) {
					AyuSettings::ghost(state->selectedUserId.current()).setSuggestGhostModeBeforeViewingStory(enabled);
				},
				container->lifetime());
			AddSkip(container);
			AddDividerText(container, tr::ayu_SuggestGhostModeBeforeViewingStoryDescription());

			auto showMenu = [=] {
				state->menu = base::make_unique_q<Ui::PopupMenu>(
					pickerButton,
					st::defaultPopupMenu);

				state->menu->addAction(
					base::make_unique_q<GlobalAction>(
						state->menu->menu(),
						st::defaultPopupMenu.menu,
						tr::ayu_GhostModeGlobalSettings(tr::now),
						state->selectedUserId.current() == 0,
						[=] { selectGhostProfile(state, 0); }));

				for (const auto &account : Core::App().domain().orderedAccounts()) {
					if (!account->sessionExists()) {
						continue;
					}
					auto user = account->session().user();
					auto id = account->session().userId().bare;
					state->menu->addAction(
						base::make_unique_q<AccountAction>(
							state->menu->menu(),
							st::defaultPopupMenu.menu,
							user,
							state->selectedUserId.current() == id,
							[=] { selectGhostProfile(state, id); }));
				}

				state->menu->popup(
					pickerButton->mapToGlobal(
						QPoint(pickerButton->width(), pickerButton->height())));
			};
			pickerButton->setClickedCallback(showMenu);
			arrow->setClickedCallback(showMenu);
		}, [&](const SearchContext &sctx) {
			sctx.entries->push_back({
				.id = u"ayu/ghostModeToggle"_q,
				.title = tr::ayu_GhostModeToggle(tr::now),
				.section = sctx.sectionId,
			});
			sctx.entries->push_back({
				.id = u"ayu/markReadAfterAction"_q,
				.title = tr::ayu_MarkReadAfterAction(tr::now),
				.section = sctx.sectionId,
			});
			sctx.entries->push_back({
				.id = u"ayu/useScheduledMessages"_q,
				.title = tr::ayu_UseScheduledMessages(tr::now),
				.section = sctx.sectionId,
			});
			sctx.entries->push_back({
				.id = u"ayu/sendWithoutSound"_q,
				.title = tr::ayu_SendWithoutSoundByDefault(tr::now),
				.section = sctx.sectionId,
			});
			sctx.entries->push_back({
				.id = u"ayu/suggestGhostModeBeforeViewingStory"_q,
				.title = tr::ayu_SuggestGhostModeBeforeViewingStory(tr::now),
				.section = sctx.sectionId,
			});
		});
	});
}

void BuildSpyEssentials(SectionBuilder &builder, AyuSectionBuilder &ayu) {
	builder.addSubsectionTitle(tr::ayu_SpyEssentialsHeader());

	ayu.addSettingToggle({
		.id = u"ayu/saveDeletedMessages"_q,
		.title = tr::ayu_SaveDeletedMessages(),
		.getter = &AyuSettings::saveDeletedMessages,
		.setter = &AyuSettings::setSaveDeletedMessages,
	});
	ayu.addSettingToggle({
		.id = u"ayu/saveMessagesHistory"_q,
		.title = tr::ayu_SaveMessagesHistory(),
		.getter = &AyuSettings::saveMessagesHistory,
		.setter = &AyuSettings::setSaveMessagesHistory,
	});

	ayu.addSectionDivider();

	ayu.addSettingToggle({
		.id = u"ayu/saveForBots"_q,
		.title = tr::ayu_MessageSavingSaveForBots(),
		.getter = &AyuSettings::saveForBots,
		.setter = &AyuSettings::setSaveForBots,
	});
}

void BuildDeletedChats(SectionBuilder &builder, AyuSectionBuilder &ayu) {
	builder.addSubsectionTitle(tr::ayu_DeletedChatsHeader());

	ayu.addCollapsibleToggle({
		.id = u"ayu/keepRemovedDialogs"_q,
		.title = tr::ayu_KeepRemovedDialogs(),
		.checkboxes = {
			NestedEntry{
				tr::ayu_KeepRemovedUserChats(tr::now),
				[] { return AyuSettings::getInstance().keepRemovedUserChats(); },
				[](bool v) { AyuSettings::getInstance().setKeepRemovedUserChats(v); }
			},
			NestedEntry{
				tr::ayu_KeepRemovedGroups(tr::now),
				[] { return AyuSettings::getInstance().keepRemovedGroups(); },
				[](bool v) { AyuSettings::getInstance().setKeepRemovedGroups(v); }
			},
			NestedEntry{
				tr::ayu_KeepRemovedForums(tr::now),
				[] { return AyuSettings::getInstance().keepRemovedForums(); },
				[](bool v) { AyuSettings::getInstance().setKeepRemovedForums(v); }
			},
			NestedEntry{
				tr::ayu_KeepRemovedChannels(tr::now),
				[] { return AyuSettings::getInstance().keepRemovedChannels(); },
				[](bool v) { AyuSettings::getInstance().setKeepRemovedChannels(v); }
			}
		},
		.toggledWhenAll = false,
	});
	ayu.addSettingToggle({
		.id = u"ayu/keepDeletedTopics"_q,
		.title = tr::ayu_KeepDeletedTopics(),
		.getter = &AyuSettings::keepDeletedTopics,
		.setter = &AyuSettings::setKeepDeletedTopics,
	});
	ayu.addSettingToggle({
		.id = u"ayu/restoreDeletedInChats"_q,
		.title = tr::ayu_RestoreDeletedInChats(),
		.getter = &AyuSettings::restoreDeletedInChats,
		.setter = &AyuSettings::setRestoreDeletedInChats,
	});
	ayu.addSettingToggle({
		.id = u"ayu/showDeletedChatIcon"_q,
		.title = tr::ayu_ShowDeletedChatIcon(),
		.getter = &AyuSettings::showDeletedChatIcon,
		.setter = &AyuSettings::setShowDeletedChatIcon,
	});
	ayu.addSettingToggle({
		.id = u"ayu/markOldMessagesDeleted"_q,
		.title = tr::ayu_MarkOldMessagesDeleted(),
		.getter = &AyuSettings::markOldMessagesDeleted,
		.setter = &AyuSettings::setMarkOldMessagesDeleted,
	});

	builder.addSkip();
	builder.addDividerText(tr::ayu_DeletedChatsDescription());
}

constexpr auto kNever = std::numeric_limits<int>::max();

void AddStorageSlider(
		AyuSectionBuilder &ayu,
		const QString &id,
		rpl::producer<QString> title,
		std::vector<int> values,
		const QString &unit,
		int current,
		Fn<void(int)> apply) {
	const auto wasZero = (current == 0) && (values.back() == kNever);
	const auto steps = int(values.size());
	ayu.addSlider({
		.id = id,
		.title = std::move(title),
		.steps = steps,
		.current = wasZero ? kNever : current,
		.indexToValue = [=](int index) { return values[index]; },
		.onFinalChanged = [=](int value) {
			apply((value == kNever) ? 0 : value);
		},
		.formatLabel = [=](int value) {
			return (value == kNever)
				? tr::ayu_StorageUnlimited(tr::now)
				: (value == 0)
				? tr::ayu_StorageOff(tr::now)
				: unit.isEmpty()
				? QString::number(value)
				: QString::number(value) + ' ' + unit;
		},
	});
}

void ShowClearAllDeleted(not_null<Window::SessionController*> controller) {
	const auto step = [=](
			QString text,
			QString action,
			bool attention,
			Fn<void()> next) {
		controller->show(Ui::MakeConfirmBox({
			.text = text,
			.confirmed = [=](Fn<void()> &&close) {
				close();
				next();
			},
			.confirmText = action,
			.cancelText = tr::lng_cancel(),
			.confirmStyle = attention ? &st::attentionBoxButton : nullptr,
		}));
	};
	const auto proceed = tr::ayu_ClearAllDeletedContinue(tr::now);
	step(tr::ayu_ClearAllDeletedFirst(tr::now), proceed, false, [=] {
		step(tr::ayu_ClearAllDeletedSecond(tr::now), proceed, true, [=] {
			step(tr::ayu_ClearAllDeletedThird(tr::now), tr::ayu_ClearAllDeletedAction(tr::now), true, [] {
				AyuMessages::clearAllDeleted();
				Core::Restart();
			});
		});
	});
}

void BuildStorage(SectionBuilder &builder, AyuSectionBuilder &ayu) {
	const auto &settings = AyuSettings::getInstance();
	builder.addSubsectionTitle(tr::ayu_StorageHeader());

	AddStorageSlider(
		ayu,
		u"ayu/maxEditRevisions"_q,
		tr::ayu_MaxEditRevisions(),
		{ 10, 25, 50, 100, 250, 500, kNever },
		QString(),
		settings.maxEditRevisions(),
		[](int value) {
			AyuSettings::getInstance().setMaxEditRevisions(value);
		});
	AddStorageSlider(
		ayu,
		u"ayu/deletedRestoreLimit"_q,
		tr::ayu_DeletedRestoreLimit(),
		{ 100, 500, 1000, 3000, 10000, kNever },
		QString(),
		settings.deletedRestoreLimit(),
		[](int value) {
			AyuSettings::getInstance().setDeletedRestoreLimit(value);
		});
	AddStorageSlider(
		ayu,
		u"ayu/keptSnapshotLimit"_q,
		tr::ayu_KeptSnapshotLimit(),
		{ 25, 50, 100, 250, 500, 1000, kNever },
		QString(),
		settings.keptSnapshotLimit(),
		[](int value) {
			AyuSettings::getInstance().setKeptSnapshotLimit(value);
		});
	AddStorageSlider(
		ayu,
		u"ayu/deletedMediaMaxSizeMb"_q,
		tr::ayu_DeletedMediaMaxSize(),
		{ 0, 8, 16, 32, 64, 128, 256 },
		tr::ayu_StorageUnitMb(tr::now),
		settings.deletedMediaMaxSizeMb(),
		[](int value) {
			AyuSettings::getInstance().setDeletedMediaMaxSizeMb(value);
		});
	AddStorageSlider(
		ayu,
		u"ayu/keepDeletedDays"_q,
		tr::ayu_KeepDeletedDays(),
		{ 30, 90, 180, 365, kNever },
		tr::ayu_StorageUnitDays(tr::now),
		settings.keepDeletedDays(),
		[](int value) {
			AyuSettings::getInstance().setKeepDeletedDays(value);
		});
	AddStorageSlider(
		ayu,
		u"ayu/backupKeepCount"_q,
		tr::ayu_BackupKeepCount(),
		{ 2, 5, 10, 20 },
		QString(),
		settings.backupKeepCount(),
		[](int value) {
			AyuSettings::getInstance().setBackupKeepCount(value);
		});
	AddStorageSlider(
		ayu,
		u"ayu/backupIntervalHours"_q,
		tr::ayu_BackupInterval(),
		{ 1, 3, 6, 12, 24 },
		tr::ayu_StorageUnitHours(tr::now),
		settings.backupIntervalHours(),
		[](int value) {
			AyuSettings::getInstance().setBackupIntervalHours(value);
		});

	builder.addSkip();
	const auto controller = builder.controller();
	builder.addButton({
		.id = u"ayu/clearAllDeleted"_q,
		.title = tr::ayu_ClearAllDeleted(),
		.st = &st::settingsAttentionButton,
		.onClick = [=] {
			if (controller) {
				ShowClearAllDeleted(controller);
			}
		},
		.keywords = { u"delete"_q, u"clear"_q, u"storage"_q },
	});
	builder.addSkip();
	builder.addDividerText(tr::ayu_StorageDescription());
}

void BuildOther(SectionBuilder &builder, AyuSectionBuilder &ayu) {
	builder.addSubsectionTitle(tr::ayu_MessageSavingOtherHeader());

	ayu.addSettingToggle({
		.id = u"ayu/localPremium"_q,
		.title = tr::ayu_LocalPremium(),
		.getter = &AyuSettings::localPremium,
		.setter = &AyuSettings::setLocalPremium,
	});
	ayu.addSettingToggle({
		.id = u"ayu/disableAds"_q,
		.title = tr::ayu_DisableAds(),
		.getter = &AyuSettings::disableAds,
		.setter = &AyuSettings::setDisableAds,
	});
}

void BuildSecretChats(SectionBuilder &builder) {
	builder.addSubsectionTitle(tr::ayu_SecretChatsHeader());
	builder.add([](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			const auto container = wctx.container;
			AyuSecret::SyncPolicy();
			const auto button = AddButtonWithIcon(
				container,
				tr::ayu_SecretChatsSwitch(),
				st::settingsButtonNoIcon);
			if (wctx.highlights) {
				wctx.highlights->push_back(std::make_pair(
					u"ayu/secretChats"_q,
					HighlightEntry{ button.get(), {} }));
			}
			button->toggleOn(
				AyuSettings::getInstance().secretChatsEnabledValue(),
				true);
			button->addClickHandler([=] {
				auto &settings = AyuSettings::getInstance();
				if (settings.secretChatsEnabled()) {
					settings.setSecretChatsEnabled(false);
				} else if (!AyuSecret::PasscodeSet()) {
					Ui::Toast::Show(tr::ayu_SecretChatsNeedPasscode(tr::now));
				} else {
					settings.setSecretChatsEnabled(true);
				}
			});
			AddSkip(container);
			AddDividerText(container, tr::ayu_SecretChatsSwitchDescription());
		}, [&](const SearchContext &sctx) {
			sctx.entries->push_back({
				.id = u"ayu/secretChats"_q,
				.title = tr::ayu_SecretChatsSwitch(tr::now),
				.section = sctx.sectionId,
			});
		});
	});
}

const auto kMeta = BuildHelper({
	.id = AyuGhost::Id(),
	.parentId = AyuMain::Id(),
	.title = u"freshGram"_q,
	.icon = &st::menuIconGroupReactions,
}, [](SectionBuilder &builder) {
	auto ayu = AyuSectionBuilder(builder);

	builder.addSkip();
	BuildGhostEssentials(builder);

	builder.addSkip();
	BuildSpyEssentials(builder, ayu);

	ayu.addSectionDivider();
	BuildDeletedChats(builder, ayu);

	ayu.addSectionDivider();
	BuildStorage(builder, ayu);
	BuildOther(builder, ayu);
	ayu.addSectionDivider();
	BuildSecretChats(builder);
	builder.addSkip();
});

} // namespace

rpl::producer<QString> AyuGhost::title() {
	return tr::ayu_ProductName();
}

AyuGhost::AyuGhost(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller)
, _controller(controller) {
	setupContent();
}

void AyuGhost::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

Type AyuGhostId() {
	return AyuGhost::Id();
}

} // namespace Settings
