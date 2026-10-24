// This is the source code of freshGram.
#include "ayu/ui/boxes/story_compose_box.h"

#include "ayu/features/stories/story_poster.h"
#include "ayu/ui/boxes/story_privacy_box.h"
#include "base/event_filter.h"
#include "base/random.h"
#include "boxes/premium_preview_box.h"
#include "boxes/send_gif_with_caption_box.h"
#include "chat_helpers/message_field.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "data/data_channel.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_stories.h"
#include "data/data_story.h"
#include "data/data_user.h"
#include "editor/photo_editor_layer_widget.h"
#include "editor/video/video_editor_layer.h"
#include "lang/lang_keys.h"
#include "lang_auto.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "menu/menu_checked_action.h"
#include "settings/settings_common.h"
#include "storage/storage_media_prepare.h"
#include "ui/chat/attach/attach_controls.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/controls/userpic_button.h"
#include "ui/image/image_prepare.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_ayu_styles.h"
#include "styles/style_boxes.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtGui/QPainter>
#include <QtGui/QPainterPath>

#include <array>

namespace AyuUi {
namespace {

using FreshStories::Audience;

constexpr auto kPeriods = std::array<TimeId, 4>{
	6 * 3600,
	12 * 3600,
	24 * 3600,
	48 * 3600,
};
constexpr auto kDefaultPeriod = 24 * 3600;

// The choice made in the window, kept by the account and the author.
struct SavedChoice {
	TextWithTags caption;
	Audience audience = Audience::Contacts;
	std::vector<UserId> excepted;
	std::vector<UserId> selected;
	TimeId period = kDefaultPeriod;
	bool pinned = false;
	bool protect = false;
	std::vector<int> albums;
};

// WHY: the choice survives closing the window, until the app is closed.
[[nodiscard]] base::flat_map<std::pair<uint64, PeerId>, SavedChoice> &Saved() {
	static auto result = base::flat_map<
		std::pair<uint64, PeerId>,
		SavedChoice>();
	return result;
}

[[nodiscard]] std::pair<uint64, PeerId> SavedKey(not_null<PeerData*> peer) {
	return { peer->session().uniqueId(), peer->id };
}

[[nodiscard]] std::vector<UserId> UserIds(
		const std::vector<not_null<UserData*>> &users) {
	auto result = std::vector<UserId>();
	result.reserve(users.size());
	for (const auto &user : users) {
		result.push_back(peerToUser(user->id));
	}
	return result;
}

[[nodiscard]] std::vector<not_null<UserData*>> Users(
		not_null<Main::Session*> session,
		const std::vector<UserId> &ids) {
	auto result = std::vector<not_null<UserData*>>();
	for (const auto &id : ids) {
		if (const auto user = session->data().userLoaded(id)) {
			result.push_back(user);
		}
	}
	return result;
}

[[nodiscard]] std::vector<not_null<PeerData*>> Authors(
		not_null<PeerData*> initial) {
	const auto session = &initial->session();
	auto result = std::vector<not_null<PeerData*>>();
	const auto add = [&](not_null<PeerData*> peer) {
		if (peer->canPostStories() && !ranges::contains(result, peer)) {
			result.push_back(peer);
		}
	};
	result.push_back(initial);
	add(session->user());
	session->data().enumerateBroadcasts([&](not_null<ChannelData*> channel) {
		add(channel);
	});
	session->data().enumerateGroups([&](not_null<PeerData*> group) {
		add(group);
	});
	return result;
}

[[nodiscard]] QString AuthorName(not_null<PeerData*> peer) {
	return peer->isSelf()
		? tr::lng_stories_my_name(tr::now)
		: peer->name();
}

[[nodiscard]] QString PeriodText(TimeId period) {
	switch (period) {
	case 6 * 3600: return tr::fresh_Story6h(tr::now);
	case 12 * 3600: return tr::fresh_Story12h(tr::now);
	case 48 * 3600: return tr::fresh_Story48h(tr::now);
	}
	return tr::fresh_Story24h(tr::now);
}

[[nodiscard]] Ui::PreparedFile CloneFile(const Ui::PreparedFile &file) {
	auto result = Ui::PreparedFile(file.path);
	result.displayName = file.displayName;
	result.content = file.content;
	result.size = file.size;
	if (file.information) {
		result.information = std::make_unique<Ui::PreparedFileInformation>(
			*file.information);
	}
	result.preview = file.preview;
	result.shownDimensions = file.shownDimensions;
	result.originalDimensions = file.originalDimensions;
	result.type = file.type;
	result.sendLargePhotos = file.sendLargePhotos;
	result.animationJob = file.animationJob;
	return result;
}

[[nodiscard]] bool CanEdit(const Ui::PreparedFile &file) {
	using Type = Ui::PreparedFile::Type;
	return (file.type == Type::Photo)
		|| (file.type == Type::Video
			&& file.information
			&& file.canEditVideo());
}

// WHY: like in the stories viewer, the media is fitted in 9:16
// over its own blurred copy, which fills the rest.
[[nodiscard]] QImage PrepareCard(const QImage &image, QSize size) {
	const auto ratio = style::DevicePixelRatio();
	auto result = QImage(size * ratio, QImage::Format_ARGB32_Premultiplied);
	result.setDevicePixelRatio(ratio);
	result.fill(st::boxDividerBg->c);
	if (image.isNull() || size.isEmpty()) {
		return result;
	}
	auto p = QPainter(&result);
	auto hq = PainterHighQualityEnabler(p);
	const auto small = image.scaled(
		QSize(size.width() / 4, size.height() / 4).expandedTo({ 1, 1 }),
		Qt::KeepAspectRatioByExpanding,
		Qt::SmoothTransformation);
	auto blurred = Images::BlurLargeImage(
		small.convertToFormat(QImage::Format_ARGB32_Premultiplied),
		8);
	const auto cover = blurred.size().scaled(
		size,
		Qt::KeepAspectRatioByExpanding);
	p.drawImage(
		QRect(
			(size.width() - cover.width()) / 2,
			(size.height() - cover.height()) / 2,
			cover.width(),
			cover.height()),
		blurred);
	const auto shade = QColor(0, 0, 0, st::storyPreviewShade);
	p.fillRect(QRect(QPoint(), size), shade);
	const auto fitted = image.size().scaled(size, Qt::KeepAspectRatio);
	p.drawImage(
		QRect(
			(size.width() - fitted.width()) / 2,
			(size.height() - fitted.height()) / 2,
			fitted.width(),
			fitted.height()),
		image);
	return result;
}

void ShowAlbumsBox(
		not_null<Window::SessionController*> controller,
		const std::vector<Data::StoryAlbum> &list,
		std::vector<int> chosen,
		Fn<void(std::vector<int>)> done) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::fresh_StoryAlbums());
		const auto state = box->lifetime().make_state<std::vector<int>>(
			chosen);
		Ui::AddSkip(box->verticalLayout());
		for (const auto &album : list) {
			const auto id = album.id;
			const auto checkbox = box->addRow(
				object_ptr<Ui::Checkbox>(
					box,
					album.title,
					ranges::contains(*state, id),
					st::defaultBoxCheckbox),
				st::boxRowPadding + QMargins(0, 0, 0, st::boxLittleSkip));
			checkbox->checkedChanges(
			) | rpl::on_next([=](bool checked) {
				state->erase(ranges::remove(*state, id), end(*state));
				if (checked) {
					state->push_back(id);
				}
			}, checkbox->lifetime());
		}
		box->addButton(tr::lng_box_done(), [=] {
			const auto callback = done;
			auto result = *state;
			box->closeBox();
			callback(std::move(result));
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void FillStoryComposeBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		std::shared_ptr<Ui::PreparedFile> file) {
	box->setTitle(tr::fresh_StoryNew());
	box->setWidth(st::boxWideWidth);

	struct State {
		explicit State(not_null<PeerData*> peer)
		: poster(&peer->session())
		, author(peer) {
		}

		FreshStories::Poster poster;
		Ui::PreparedList list;
		uint64 fileKey = base::RandomValue<uint64>();
		rpl::variable<not_null<PeerData*>> author;
		rpl::variable<StoryAudienceChoice> audience;
		rpl::variable<TimeId> period = kDefaultPeriod;
		rpl::variable<bool> pinned = false;
		rpl::variable<bool> protect = false;
		rpl::variable<std::vector<int>> albums;
		std::vector<Data::StoryAlbum> albumsList;
		rpl::event_stream<> previewChanged;
		std::optional<float64> progress;
		QImage card;
		base::unique_qptr<Ui::PopupMenu> menu;
		base::unique_qptr<Ui::UserpicButton> authorUserpic;
		bool busy = false;
		bool posted = false;
	};
	const auto session = &peer->session();
	const auto container = box->verticalLayout();
	const auto state = box->lifetime().make_state<State>(peer);
	state->list.files.push_back(std::move(*file));
	const auto current = [=]() -> Ui::PreparedFile& {
		return state->list.files.front();
	};
	const auto premium = session->premium();

	const auto i = Saved().find(SavedKey(peer));
	const auto saved = (i != end(Saved())) ? i->second : SavedChoice();
	state->audience = StoryAudienceChoice{
		.audience = saved.audience,
		.excepted = Users(session, saved.excepted),
		.selected = Users(session, saved.selected),
	};
	state->period = (premium && ranges::contains(kPeriods, saved.period))
		? saved.period
		: kDefaultPeriod;
	state->pinned = saved.pinned;
	state->protect = saved.protect;
	state->albums = saved.albums;

	// Author.
	const auto authors = Authors(peer);
	if (authors.size() > 1) {
		const auto button = Settings::AddButtonWithLabel(
			container,
			tr::fresh_StoryPostAs(),
			state->author.value() | rpl::map(AuthorName),
			st::settingsButton);
		state->author.value(
		) | rpl::on_next([=](not_null<PeerData*> author) {
			state->authorUserpic = base::make_unique_q<Ui::UserpicButton>(
				button,
				author,
				st::storyAuthorUserpic);
			const auto userpic = state->authorUserpic.get();
			userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
			userpic->show();
			button->sizeValue(
			) | rpl::on_next([=](QSize size) {
				userpic->move(
					st::storyAuthorUserpicLeft,
					(size.height() - userpic->height()) / 2);
			}, userpic->lifetime());
		}, button->lifetime());
		button->setClickedCallback([=] {
			state->menu = base::make_unique_q<Ui::PopupMenu>(
				button,
				st::popupMenuWithIcons);
			for (const auto &item : authors) {
				const auto chosen = (state->author.current() == item);
				const auto choose = [=] {
					if (state->author.current() == item) {
						return;
					}
					state->albums = std::vector<int>();
					state->author = item;
					FreshStories::CheckCanPost(
						session,
						item,
						[] {},
						crl::guard(box, [=](QString type) {
							controller->showToast(
								FreshStories::ErrorText(type));
						}));
				};
				Menu::AddCheckedAction(
					state->menu.get(),
					AuthorName(item),
					choose,
					nullptr,
					chosen);
			}
			state->menu->popup(QCursor::pos());
		});
		Ui::AddDivider(container);
	}

	// Preview.
	const auto previewHeight = st::storyPreviewHeight;
	const auto previewWidth = previewHeight * 9 / 16;
	const auto preview = container->add(
		object_ptr<Ui::FixedHeightWidget>(container, previewHeight),
		st::storyPreviewPadding);
	const auto cardRect = [=] {
		return QRect(
			(preview->width() - previewWidth) / 2,
			0,
			previewWidth,
			previewHeight);
	};
	const auto controls = Ui::CreateChild<Ui::AttachControlsWidget>(
		preview,
		Ui::AttachControls::Type::EditOnly);
	preview->sizeValue(
	) | rpl::on_next([=](QSize size) {
		const auto card = cardRect();
		controls->moveToLeft(
			card.x() + card.width() - controls->width()
				- st::sendBoxAlbumGroupSkipRight,
			card.y() + st::sendBoxAlbumGroupSkipTop,
			size.width());
	}, controls->lifetime());
	const auto refreshControls = [=] {
		controls->setVisible(CanEdit(current()));
	};
	refreshControls();
	state->previewChanged.events(
	) | rpl::on_next([=] {
		state->card = QImage();
		refreshControls();
		preview->update();
	}, preview->lifetime());

	preview->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(preview);
		auto hq = PainterHighQualityEnabler(p);
		const auto card = cardRect();
		if (state->card.isNull()) {
			state->card = PrepareCard(current().preview, card.size());
		}
		auto path = QPainterPath();
		const auto radius = st::storyPreviewRadius;
		path.addRoundedRect(card, radius, radius);
		p.setClipPath(path);
		p.drawImage(card.topLeft(), state->card);
		if (!state->progress) {
			return;
		}
		p.fillRect(card, QColor(0, 0, 0, 110));
		const auto size = st::storyProgressSize;
		const auto line = st::storyProgressLine;
		const auto arc = QRect(
			card.x() + (card.width() - size) / 2,
			card.y() + (card.height() - size) / 2,
			size,
			size).marginsRemoved({ line, line, line, line });
		auto pen = QPen(QColor(255, 255, 255, 70));
		pen.setWidth(line);
		pen.setCapStyle(Qt::RoundCap);
		p.setPen(pen);
		p.drawEllipse(arc);
		pen.setColor(QColor(255, 255, 255));
		p.setPen(pen);
		const auto value = std::clamp(*state->progress, 0., 1.);
		p.drawArc(arc, 90 * 16, -int(std::max(value, 0.02) * 360 * 16));
		p.setFont(st::semiboldFont);
		const auto textTop = arc.y() + arc.height() + line * 3;
		p.drawText(
			QRect(
				card.x(),
				textTop,
				card.width(),
				st::semiboldFont->height),
			QString::number(int(std::round(value * 100))) + '%',
			style::al_top);
	}, preview->lifetime());

	const auto openEditor = [=] {
		if (state->busy || !CanEdit(current())) {
			return;
		}
		const auto done = crl::guard(box, [=](bool ok) {
			if (ok) {
				// The edited file is uploaded again.
				state->fileKey = base::RandomValue<uint64>();
				state->previewChanged.fire({});
			}
		});
		if (current().type == Ui::PreparedFile::Type::Photo) {
			Editor::OpenWithPreparedFile(
				box,
				controller->uiShow(),
				&current(),
				st::sendMediaPreviewSize,
				done,
				0,
				QSize(720, 1280),
				false);
		} else {
			Editor::OpenWithPreparedVideoFile(
				box,
				controller->uiShow(),
				&state->list,
				0,
				st::sendMediaPreviewSize,
				done);
		}
	};
	const auto replaceFile = [=] {
		if (state->busy) {
			return;
		}
		const auto callback = crl::guard(box, [=](
				FileDialog::OpenResult &&result) {
			const auto check = [](const Ui::PreparedList &list) {
				if (list.files.size() != 1) {
					return false;
				}
				const auto type = list.files.front().type;
				return (type == Ui::PreparedFile::Type::Photo)
					|| (type == Ui::PreparedFile::Type::Video);
			};
			auto list = Storage::PreparedFileFromFilesDialog(
				std::move(result),
				check,
				[=](tr::phrase<> text) {
					controller->showToast(text(tr::now));
				},
				st::sendMediaPreviewSize,
				session->premium());
			if (!list || state->busy) {
				return;
			}
			state->list = std::move(*list);
			state->fileKey = base::RandomValue<uint64>();
			state->previewChanged.fire({});
		});
		FileDialog::GetOpenPath(
			box.get(),
			tr::fresh_StoryChoose(tr::now),
			FileDialog::PhotoVideoFilesFilter(),
			callback);
	};
	controls->editRequests() | rpl::on_next(openEditor, controls->lifetime());
	preview->setCursor(style::cur_pointer);
	base::install_event_filter(preview, [=](not_null<QEvent*> e) {
		const auto type = e->type();
		if (type == QEvent::ContextMenu) {
			state->menu = base::make_unique_q<Ui::PopupMenu>(
				preview,
				st::popupMenuWithIcons);
			if (CanEdit(current())) {
				state->menu->addAction(
					tr::fresh_StoryEdit(tr::now),
					openEditor,
					&st::menuIconDraw);
			}
			state->menu->addAction(
				tr::fresh_StoryReplace(tr::now),
				replaceFile,
				&st::menuIconReplace);
			state->menu->popup(QCursor::pos());
			return base::EventFilterResult::Cancel;
		} else if (type == QEvent::MouseButtonRelease) {
			const auto mouse = static_cast<QMouseEvent*>(e.get());
			if (mouse->button() == Qt::LeftButton
				&& cardRect().contains(mouse->pos())) {
				openEditor();
				return base::EventFilterResult::Cancel;
			}
		}
		return base::EventFilterResult::Continue;
	});

	Ui::AddDivider(container);
	Ui::AddSkip(container);

	// Settings.
	const auto audienceWrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	const auto audienceButton = Settings::AddButtonWithLabel(
		audienceWrap->entity(),
		tr::fresh_StoryAudience(),
		state->audience.value() | rpl::map(StoryAudienceText),
		st::settingsButton,
		{ &st::menuIconGroups });
	audienceButton->setClickedCallback([=] {
		ShowStoryAudienceBox(
			controller,
			state->audience.current(),
			crl::guard(box, [=](StoryAudienceChoice choice) {
				state->audience = std::move(choice);
			}));
	});
	audienceWrap->toggleOn(state->author.value(
	) | rpl::map([](not_null<PeerData*> author) {
		return author->isSelf();
	}));
	audienceWrap->finishAnimating();

	const auto periodButton = Settings::AddButtonWithLabel(
		container,
		tr::fresh_StoryDuration(),
		state->period.value() | rpl::map(PeriodText),
		st::settingsButton,
		{ &st::menuIconTimer });
	periodButton->setClickedCallback([=] {
		// Only Premium may choose how long a story is visible.
		if (!premium) {
			ShowPremiumPreviewBox(controller, PremiumFeature::Stories);
			return;
		}
		state->menu = base::make_unique_q<Ui::PopupMenu>(
			periodButton,
			st::popupMenuWithIcons);
		for (const auto period : kPeriods) {
			const auto chosen = (state->period.current() == period);
			Menu::AddCheckedAction(
				state->menu.get(),
				PeriodText(period),
				[=] { state->period = period; },
				nullptr,
				chosen);
		}
		state->menu->popup(QCursor::pos());
	});

	const auto albumsWrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	const auto albumsButton = Settings::AddButtonWithLabel(
		albumsWrap->entity(),
		tr::fresh_StoryAlbums(),
		state->albums.value(
		) | rpl::map([](const std::vector<int> &albums) {
			return albums.empty()
				? tr::fresh_StoryAlbumsNone(tr::now)
				: QString::number(albums.size());
		}),
		st::settingsButton,
		{ &st::menuIconStoriesSavedSection });
	albumsWrap->hide(anim::type::instant);
	albumsButton->setClickedCallback([=] {
		ShowAlbumsBox(
			controller,
			state->albumsList,
			state->albums.current(),
			crl::guard(box, [=](std::vector<int> albums) {
				if (!albums.empty()) {
					// WHY: the stories of the albums are on the profile.
					state->pinned = true;
				}
				state->albums = std::move(albums);
			}));
	});
	state->author.value(
	) | rpl::map([=](not_null<PeerData*> author) {
		return session->data().stories().albumsListValue(author->id);
	}) | rpl::flatten_latest(
	) | rpl::on_next([=](const std::vector<Data::StoryAlbum> &list) {
		state->albumsList = list;
		auto albums = state->albums.current();
		albums.erase(
			ranges::remove_if(albums, [&](int id) {
				return !ranges::contains(list, id, &Data::StoryAlbum::id);
			}),
			end(albums));
		state->albums = std::move(albums);
		albumsWrap->toggle(!list.empty(), anim::type::instant);
	}, albumsWrap->lifetime());

	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);

	const auto addToggle = [&](
			rpl::producer<QString> text,
			const style::icon *icon,
			not_null<rpl::variable<bool>*> value) {
		const auto button = Settings::AddButtonWithIcon(
			container,
			std::move(text),
			st::settingsButton,
			{ icon });
		button->toggleOn(value->value());
		button->toggledChanges(
		) | rpl::on_next([=](bool toggled) {
			*value = toggled;
		}, button->lifetime());
	};
	addToggle(tr::fresh_StoryKeep(), &st::menuIconPin, &state->pinned);
	addToggle(tr::fresh_StoryProtect(), &st::menuIconLock, &state->protect);
	Ui::AddSkip(container);

	// Caption.
	const auto limit = session->appConfig().get<int>(
		premium
			? u"story_caption_length_limit_premium"_q
			: u"story_caption_length_limit_default"_q,
		premium ? 2048 : 200);
	const auto bottom = box->setPinnedToBottomContent(
		object_ptr<Ui::VerticalLayout>(box));
	const auto captionWrap = bottom->add(
		object_ptr<Ui::RpWidget>(box),
		st::boxRowPadding);
	const auto caption = Ui::CreateChild<Ui::InputField>(
		captionWrap,
		st::defaultComposeFiles.caption,
		Ui::InputField::Mode::MultiLine,
		tr::fresh_StoryCaption());
	Ui::ResizeFitChild(captionWrap, caption);
	caption->setMaxLength(limit);
	caption->setSubmitSettings(Core::App().settings().sendSubmitWay());
	const auto allow = [](not_null<DocumentData*>) { return false; };
	Ui::SetupCaptionFieldInBox(
		box,
		controller,
		caption,
		session->user(),
		allow,
		PremiumFeature::AnimatedEmoji);
	InitMessageFieldHandlers(
		controller,
		caption,
		Window::GifPauseReason::Layer,
		allow);
	caption->setTextWithTags(saved.caption);
	box->setFocusCallback([=] {
		caption->setFocusFast();
	});

	const auto setProgress = [=](std::optional<float64> progress) {
		state->progress = progress;
		preview->update();
	};

	const auto save = [=] {
		const auto &audience = state->audience.current();
		Saved()[SavedKey(state->author.current())] = SavedChoice{
			.caption = state->posted
				? TextWithTags()
				: caption->getTextWithTags(),
			.audience = audience.audience,
			.excepted = UserIds(audience.excepted),
			.selected = UserIds(audience.selected),
			.period = state->period.current(),
			.pinned = state->pinned.current(),
			.protect = state->protect.current(),
			.albums = state->albums.current(),
		};
	};
	box->boxClosing() | rpl::on_next(save, box->lifetime());

	const auto post = [=] {
		if (state->busy) {
			return;
		}
		const auto author = state->author.current();
		const auto &audience = state->audience.current();
		const auto chosen = author->isSelf()
			? audience.audience
			: Audience::Everyone;
		if (chosen == Audience::Selected && audience.selected.empty()) {
			controller->showToast(tr::fresh_StoryNoUsers(tr::now));
			return;
		}
		state->busy = true;
		setProgress(0.);
		auto list = Ui::PreparedList();
		list.files.push_back(CloneFile(current()));
		Storage::ApplyModifications(list);
		auto users = std::vector<not_null<UserData*>>();
		if (chosen == Audience::Selected) {
			users = audience.selected;
		} else if (chosen != Audience::CloseFriends && author->isSelf()) {
			users = audience.excepted;
		}
		auto draft = FreshStories::Draft{
			.peer = author,
			.file = std::move(list.files.front()),
			.fileKey = state->fileKey,
			.caption = caption->getTextWithTags(),
			.audience = chosen,
			.users = std::move(users),
			.albums = state->albums.current(),
			.period = premium ? state->period.current() : kDefaultPeriod,
			.pinned = state->pinned.current(),
			.protect = state->protect.current(),
		};
		state->poster.post(std::move(draft), {
			.progress = crl::guard(box, [=](float64 value) {
				setProgress(value);
			}),
			.done = crl::guard(box, [=] {
				state->posted = true;
				controller->showToast(tr::fresh_StoryPosted(tr::now));
				box->closeBox();
			}),
			.failed = crl::guard(box, [=](QString type) {
				// The window stays, the next try sends the same media.
				state->busy = false;
				setProgress(std::nullopt);
				controller->showToast(FreshStories::ErrorText(type));
			}),
		});
	};
	caption->submits() | rpl::on_next(post, caption->lifetime());
	box->addButton(tr::fresh_StoryPost(), post);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

} // namespace

void ShowStoryComposeBox(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		Ui::PreparedFile &&file) {
	controller->show(Box(
		FillStoryComposeBox,
		controller,
		peer,
		std::make_shared<Ui::PreparedFile>(std::move(file))));
}

} // namespace AyuUi
