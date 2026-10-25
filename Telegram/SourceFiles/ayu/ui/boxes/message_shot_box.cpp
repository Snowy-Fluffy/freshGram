// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/boxes/message_shot_box.h"

#include "lang_auto.h"
#include "ayu/ayu_settings.h"
#include "ayu/ui/boxes/theme_selector_box.h"
#include "ayu/ui/components/image_view.h"
#include "ayu/utils/telegram_helpers.h"
#include "boxes/abstract_box.h"
#include "core/file_utilities.h"
#include "data/data_chat.h"
#include "data/data_channel.h"
#include "data/data_todo_list.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item_components.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "mainwidget.h"
#include "settings/settings_common.h"
#include "styles/style_ayu_styles.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"

#include <memory>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QRegularExpression>

namespace {

constexpr auto kJpegQuality = 95;
constexpr auto kFileNameLimit = 32;

[[nodiscard]] QString DefaultFileNamePrefix(
		const AyuFeatures::MessageShot::ShotConfig &config) {
	auto name = config.messages.empty()
		? QString()
		: config.messages.front()->history()->peer->name();
	static const auto kForbidden = QRegularExpression(
		u"[\\\\/:*?\"<>|\\x00-\\x1F]"_q);
	name.replace(kForbidden, QString());
	name = name.simplified();
	name.replace(QChar(' '), QChar('_'));
	if (name.size() > kFileNameLimit) {
		name = name.left(kFileNameLimit);
	}
	return name.isEmpty() ? u"shot"_q : (u"shot_"_q + name);
}

[[nodiscard]] QImage FlattenForJpeg(const QImage &image) {
	auto result = QImage(image.size(), QImage::Format_RGB32);
	result.setDevicePixelRatio(image.devicePixelRatio());
	result.fill(AyuFeatures::MessageShot::makeDefaultBackgroundColor());
	auto p = QPainter(&result);
	p.drawImage(0, 0, image);
	p.end();
	return result;
}

} // namespace

MessageShotBox::MessageShotBox(
	QWidget *parent,
	AyuFeatures::MessageShot::ShotConfig config)
	: _config(std::move(config)) {
}

void MessageShotBox::prepare() {
	setupContent();
}

void MessageShotBox::setupContent() {
	_selectedPalette = AyuFeatures::MessageShot::getPersistedPalette();
	if (!_selectedPalette) {
		_selectedPalette = std::make_shared<style::palette>();
	}
	AyuFeatures::MessageShot::setPersistedPalette(_selectedPalette);

	AyuFeatures::MessageShot::ensureChatThemesRefreshed();

	auto &settings = AyuSettings::getInstance();
	auto &shotSettings = settings.messageShotSettings();
	const auto savedSimpleQuotesAndReplies = settings.simpleQuotesAndReplies();
	settings.setSimpleQuotesAndReplies(!shotSettings.showColorfulReplies());

	using namespace Settings;

	auto savedThemeApplyResult = AyuFeatures::MessageShot::SavedThemeApplyResult::Failed;
	const auto hasSavedTheme = shotSettings.embeddedThemeType() != -1
		|| shotSettings.cloudThemeId() != 0;
	if (hasSavedTheme) {
		savedThemeApplyResult = AyuFeatures::MessageShot::applySavedThemePalette(
			_selectedPalette,
			nullptr);
		if (savedThemeApplyResult != AyuFeatures::MessageShot::SavedThemeApplyResult::Failed) {
			_config.st = std::make_shared<Ui::ChatStyle>(_selectedPalette.get());
		} else {
			shotSettings.clearTheme();
			_config.st = std::make_shared<Ui::ChatStyle>(_config.controller->chatStyle());
		}
	}

	AyuFeatures::MessageShot::setShotConfig(_config);

	setTitle(rpl::single(tr::ayu_MessageShotTopBarText(tr::now)));

	auto wrap = object_ptr<Ui::VerticalLayout>(this);
	const auto content = wrap.data();
	setInnerWidget(object_ptr<Ui::OverrideMargins>(this, std::move(wrap)));

	AddSubsectionTitle(content, tr::ayu_MessageShotPreview());

	const auto imageView = content->add(object_ptr<ImageView>(content), st::imageViewPadding);

	AddSkip(content);
	AddDivider(content);
	AddSkip(content);
	AddSubsectionTitle(content, tr::ayu_MessageShotPreferences());

	auto hasReactions = false;
	auto hasReplies = false;
	auto hasHeaderDecorations = false;
	auto hasSpoilers = false;
	for (const auto &item : _config.messages) {
		if (!hasReactions && !item->reactions().empty()) {
			hasReactions = true;
		}
		if (!hasReplies) {
			if (item->replyTo().replying()
				|| (item->media() && item->media()->webpage())) {
				hasReplies = true;
			}
		}
		if (!hasSpoilers) {
			for (const auto &entity : item->originalText().entities) {
				if (entity.type() == EntityType::Spoiler) {
					hasSpoilers = true;
					break;
				}
			}
			if (!hasSpoilers && item->media()) {
				if (item->media()->hasSpoiler()) {
					hasSpoilers = true;
				} else if (const auto todoList = item->media()->todolist()) {
					for (const auto &entity : todoList->title.entities) {
						if (entity.type() == EntityType::Spoiler) {
							hasSpoilers = true;
							break;
						}
					}
					if (!hasSpoilers) {
						for (const auto &task : todoList->items) {
							for (const auto &entity : task.text.entities) {
								if (entity.type() == EntityType::Spoiler) {
									hasSpoilers = true;
									break;
								}
							}
							if (hasSpoilers) break;
						}
					}
				}
			}
		}
		if (!hasHeaderDecorations) {
			const auto drawChannelBadge = [&] {
				if (item->isDiscussionPost()) {
					return true;
				} else if (item->author()->isMegagroup()) {
					if (const auto signedInfo = item->Get<HistoryMessageSigned>()) {
						if (!signedInfo->viaBusinessBot) {
							return false;
						}
					}
				}
				return item->history()->peer->isMegagroup()
					&& item->author()->isChannel()
					&& !item->out();
			}();

			auto badgeText = QString();
			if (item->isDiscussionPost()) {
				badgeText = tr::lng_channel_badge(tr::now);
			} else if (item->author()->isMegagroup()) {
				if (const auto signedInfo = item->Get<HistoryMessageSigned>()) {
					if (!signedInfo->viaBusinessBot) {
						badgeText = signedInfo->author;
					}
				}
			} else if (drawChannelBadge) {
				badgeText = tr::lng_channel_badge(tr::now);
			} else if (const auto chat = item->history()->peer->asChat()) {
				if (const auto user = item->author()->asUser()) {
					const auto rank = chat->memberRanks.find(peerToUser(user->id));
					if (rank != chat->memberRanks.end()) {
						badgeText = rank->second;
					}
				}
			} else if (const auto channel = item->history()->peer->asMegagroup()) {
				if (const auto user = item->author()->asUser()) {
					const auto info = channel->mgInfo.get();
					const auto userId = peerToUser(user->id);
					const auto isCreator = info && (info->creator == user);
					const auto isAdmin = info && info->admins.contains(userId);
					if (isCreator || isAdmin) {
						const auto rank = info->memberRanks.find(userId);
						if (rank != info->memberRanks.end()
							&& !rank->second.isEmpty()) {
							badgeText = rank->second;
						} else if (isCreator) {
							badgeText = tr::lng_owner_badge(tr::now);
						} else {
							badgeText = tr::lng_admin_badge(tr::now);
						}
					} else {
						badgeText = item->fromRank();
					}
				}
			}

			hasHeaderDecorations = drawChannelBadge
				|| !badgeText.isEmpty()
				|| (item->boostsApplied() > 0);
		}
		if (hasReactions
			&& hasReplies
			&& hasHeaderDecorations
			&& hasSpoilers) {
			break;
		}
	}

	const auto firstPreviewLatch = std::make_shared<TimedCountDownLatch>(1);
	const auto generation = content->lifetime().make_state<int>(0);
	const auto weak = base::make_weak(this);
	const auto boxWidth = content->lifetime().make_state<rpl::variable<int>>(
		st::boxWidth);
	const auto boxWidthFor = [=](const QImage &image)
	{
		return image.width() / style::DevicePixelRatio()
			+ (st::boxPadding.left() + st::boxPadding.right()) * 4;
	};

	const auto updatePreview = [=]
	{
		const auto currentGeneration = ++(*generation);
		AyuFeatures::MessageShot::Make(this, _config, [=](const QImage &image, bool final)
		{
			if (!weak || currentGeneration != *generation) {
				return;
			}

			if (final || imageView->getImage().isNull()) {
				imageView->setImage(image);
			}
			firstPreviewLatch->countDown();
			if (const auto width = boxWidthFor(image); width > boxWidth->current()) {
				*boxWidth = width;
			}
		});
	};

	if (savedThemeApplyResult == AyuFeatures::MessageShot::SavedThemeApplyResult::AwaitingAsync) {
		const auto weakBox = base::make_weak(this);
		AyuFeatures::MessageShot::subscribeToCloudThemeLoad(
			_config.controller,
			_selectedPalette,
			[=] {
				if (!weakBox) {
					return;
				}
				_config.st = std::make_shared<Ui::ChatStyle>(_selectedPalette.get());
				updatePreview();
			});
	}

	auto selectedTheme =
		content->lifetime().make_state<rpl::variable<QString>>(
			AyuFeatures::MessageShot::resolveThemeName());

	AddButtonWithLabel(
		content,
		tr::ayu_MessageShotTheme(),
		selectedTheme->value(),
		st::settingsButtonNoIcon
	)->addClickHandler(
		[=]
		{
			AyuFeatures::MessageShot::setChoosingTheme(true);

			auto box = Box<ThemeSelectorBox>(_config.controller);
			box->paletteSelected() | rpl::on_next(
				[=](const style::palette &palette) mutable
				{
					_selectedPalette->reset();
					_selectedPalette->load(palette.save());

					_config.st = std::make_shared<Ui::ChatStyle>(_selectedPalette.get());

					auto &shot = AyuSettings::getInstance().messageShotSettings();
					const auto embedded = AyuFeatures::MessageShot::getSelectedFromDefault();
					const auto cloud = AyuFeatures::MessageShot::getSelectedFromCustom();
					if (cloud.has_value()) {
						const auto accountId = _config.controller->session().userId().bare;
						shot.setCloudTheme(accountId, cloud->id, cloud->accessHash, cloud->documentId, cloud->title);
					} else if (embedded != Window::Theme::EmbeddedType(-1)) {
						const auto color = AyuFeatures::MessageShot::getSelectedColorFromDefault();
						shot.setEmbeddedTheme(static_cast<int>(embedded), color ? color->rgb() : 0);
					} else {
						shot.clearTheme();
					}

					updatePreview();
				},
				content->lifetime());

			box->themeNameChanged() | rpl::on_next(
				[=](const QString &name)
				{
					selectedTheme->force_assign(name);
				},
				content->lifetime());

			box->boxClosing() | rpl::on_next(
				[=]
				{
					AyuFeatures::MessageShot::setChoosingTheme(false);
				},
				content->lifetime());

			Ui::show(std::move(box), Ui::LayerOption::KeepOther);
		});
	const auto addChoice = [=](
			tr::phrase<> title,
			std::vector<QString> options,
			int current,
			Fn<void(int)> apply) {
		const auto selected = content->lifetime().make_state<int>(
			std::clamp(current, 0, int(options.size()) - 1));
		const auto label = content->lifetime().make_state<
			rpl::variable<QString>>(options[*selected]);
		const auto titleText = title(tr::now);
		const auto button = AddButtonWithLabel(
			content,
			title(),
			label->value(),
			st::settingsButtonNoIcon);
		button->addClickHandler([=]
		{
			Ui::show(Box([=](not_null<Ui::GenericBox*> box)
			{
				SingleChoiceBox(box, {
					.title = rpl::single(titleText),
					.options = options,
					.initialSelection = *selected,
					.callback = [=](int index)
					{
						*selected = index;
						*label = options[index];
						apply(index);
					},
				});
			}), Ui::LayerOption::KeepOther);
		});
		return button;
	};

	using Background = MessageShotSettings::Background;
	addChoice(
		tr::fresh_MessageShotBackground,
		{
			tr::fresh_MessageShotBackgroundNone(tr::now),
			tr::fresh_MessageShotBackgroundColor(tr::now),
			tr::fresh_MessageShotBackgroundWallpaper(tr::now),
		},
		int(shotSettings.background()),
		[=](int index)
		{
			AyuSettings::getInstance().messageShotSettings().setBackground(
				Background(index));
			updatePreview();
		});
	addChoice(
		tr::fresh_MessageShotPadding,
		{
			tr::fresh_MessageShotPaddingNone(tr::now),
			tr::fresh_MessageShotPaddingSmall(tr::now),
			tr::fresh_MessageShotPaddingMedium(tr::now),
			tr::fresh_MessageShotPaddingLarge(tr::now),
		},
		shotSettings.padding(),
		[=](int index)
		{
			AyuSettings::getInstance().messageShotSettings().setPadding(index);
			updatePreview();
		});
	addChoice(
		tr::fresh_MessageShotWidth,
		{
			tr::fresh_MessageShotWidthNarrow(tr::now),
			tr::fresh_MessageShotWidthMedium(tr::now),
			tr::fresh_MessageShotWidthStandard(tr::now),
		},
		shotSettings.width(),
		[=](int index)
		{
			AyuSettings::getInstance().messageShotSettings().setWidth(index);
			updatePreview();
		});

	AddButtonWithIcon(
		content,
		tr::fresh_MessageShotShowAvatars(),
		st::settingsButtonNoIcon
	)->toggleOn(rpl::single(shotSettings.showAvatars())
	)->toggledValue(
	) | rpl::skip(1) | on_next(
		[=](bool enabled)
		{
			AyuSettings::getInstance().messageShotSettings().setShowAvatars(enabled);
			updatePreview();
		},
		content->lifetime());

	AddButtonWithIcon(
		content,
		tr::fresh_MessageShotShowNames(),
		st::settingsButtonNoIcon
	)->toggleOn(rpl::single(shotSettings.showNames())
	)->toggledValue(
	) | rpl::skip(1) | on_next(
		[=](bool enabled)
		{
			AyuSettings::getInstance().messageShotSettings().setShowNames(enabled);
			updatePreview();
		},
		content->lifetime());

	auto latestToggle = AddButtonWithIcon(
		content,
		tr::ayu_MessageShotShowDate(),
		st::settingsButtonNoIcon
	);
	latestToggle->toggleOn(rpl::single(shotSettings.showDate())
	)->toggledValue(
	) | rpl::skip(1) | on_next(
		[=](bool enabled)
		{
			AyuSettings::getInstance().messageShotSettings().setShowDate(enabled);
			updatePreview();
		},
		content->lifetime());

	if (hasReactions) {
		latestToggle = AddButtonWithIcon(
			content,
			tr::ayu_MessageShotShowReactions(),
			st::settingsButtonNoIcon
		);
		latestToggle->toggleOn(rpl::single(shotSettings.showReactions())
		)->toggledValue(
		) | rpl::skip(1) | on_next(
			[=](bool enabled)
			{
				AyuSettings::getInstance().messageShotSettings().setShowReactions(enabled);
				updatePreview();
			},
			content->lifetime());
	}

	if (hasHeaderDecorations) {
		latestToggle = AddButtonWithIcon(
			content,
			tr::ayu_MessageShotShowHeaderDecorations(),
			st::settingsButtonNoIcon
		);
		latestToggle->toggleOn(rpl::single(shotSettings.showHeaderDecorations())
		)->toggledValue(
		) | rpl::skip(1) | on_next(
			[=](bool enabled)
			{
				AyuSettings::getInstance().messageShotSettings().setShowHeaderDecorations(enabled);
				updatePreview();
			},
			content->lifetime());
	}

	if (hasReplies) {
		latestToggle = AddButtonWithIcon(
			content,
			tr::ayu_MessageShotShowColorfulReplies(),
			st::settingsButtonNoIcon
		);
		latestToggle->toggleOn(rpl::single(shotSettings.showColorfulReplies())
		)->toggledValue(
		) | rpl::skip(1) | on_next(
			[=](bool enabled)
			{
				auto &currentSettings = AyuSettings::getInstance();
				currentSettings.messageShotSettings().setShowColorfulReplies(enabled);
				currentSettings.setSimpleQuotesAndReplies(!enabled);

				_config.st = std::make_shared<Ui::ChatStyle>(_config.st.get());
				updatePreview();
			},
			content->lifetime());
	}

	if (hasSpoilers) {
		latestToggle = AddButtonWithIcon(
			content,
			tr::ayu_MessageShotRevealSpoilers(),
			st::settingsButtonNoIcon
		);
		latestToggle->toggleOn(rpl::single(shotSettings.revealSpoilers())
		)->toggledValue(
		) | rpl::skip(1) | on_next(
			[=](bool enabled)
			{
				AyuSettings::getInstance().messageShotSettings().setRevealSpoilers(enabled);
				updatePreview();
			},
			content->lifetime());
	}

	AddSkip(content);
	AddDivider(content);
	AddSkip(content);
	AddSubsectionTitle(content, tr::fresh_MessageShotExport());

	using Format = MessageShotSettings::Format;
	addChoice(
		tr::fresh_MessageShotFormat,
		{ u"PNG"_q, u"JPEG"_q },
		int(shotSettings.format()),
		[=](int index)
		{
			AyuSettings::getInstance().messageShotSettings().setFormat(
				Format(index));
		});
	latestToggle = addChoice(
		tr::fresh_MessageShotScale,
		{
			tr::fresh_MessageShotScaleScreen(tr::now),
			u"1x"_q,
			u"2x"_q,
			u"3x"_q,
		},
		shotSettings.scale(),
		[=](int index)
		{
			AyuSettings::getInstance().messageShotSettings().setScale(index);
		});

	AddSkip(content);

	const auto exporting = content->lifetime().make_state<bool>(false);
	const auto exportImage = [=](Fn<void(QImage)> done)
	{
		if (*exporting) {
			return;
		}
		const auto scale = AyuSettings::getInstance().messageShotSettings().scale();
		if (scale <= 0 || scale == style::DevicePixelRatio()) {
			done(imageView->getImage());
			return;
		}
		*exporting = true;
		AyuFeatures::MessageShot::Make(this, _config, [=](const QImage &image, bool final)
		{
			if (!weak || !final) {
				return;
			}
			*exporting = false;
			done(image);
		}, scale);
	};

	const auto saveImage = [=](const QImage &image, const QString &chosen)
	{
		const auto jpegByDefault = AyuSettings::getInstance()
			.messageShotSettings().format() == Format::Jpeg;
		auto path = chosen;
		auto jpeg = jpegByDefault;
		const auto suffix = QFileInfo(path).suffix().toLower();
		if (suffix == u"jpg"_q || suffix == u"jpeg"_q) {
			jpeg = true;
		} else if (suffix == u"png"_q) {
			jpeg = false;
		} else {
			path += jpeg ? u".jpg"_q : u".png"_q;
		}

		const auto saved = jpeg
			? FlattenForJpeg(image).save(path, "JPG", kJpegQuality)
			: image.save(path, "PNG");
		if (!saved) {
			showToast(tr::fresh_MessageShotSaveFailed(tr::now));
			return;
		}
		AyuSettings::getInstance().messageShotSettings().setLastSaveFolder(
			QFileInfo(path).absolutePath());
		_config.controller->showToast(tr::fresh_MessageShotSaved(tr::now));

		_tookShot = true;
		closeBox();
	};

	addButton(tr::ayu_MessageShotSave(),
			  [=]
			  {
				  exportImage([=](QImage image)
				  {
					  if (image.isNull()) {
						  return;
					  }
					  const auto &shot = AyuSettings::getInstance().messageShotSettings();
					  const auto jpeg = (shot.format() == Format::Jpeg);
					  const auto png = u"PNG Image (*.png)"_q;
					  const auto jpg = u"JPEG Image (*.jpg *.jpeg)"_q;
					  const auto filter = (jpeg ? (jpg + u";;"_q + png) : (png + u";;"_q + jpg))
						  + u";;"_q
						  + FileDialog::AllFilesFilter();
					  auto folder = shot.lastSaveFolder();
					  if (!folder.isEmpty() && !QDir(folder).exists()) {
						  folder = QString();
					  }
					  FileDialog::GetWritePath(
						  this,
						  tr::lng_save_file(tr::now),
						  filter,
						  filedialogDefaultName(
							  DefaultFileNamePrefix(_config),
							  jpeg ? u".jpg"_q : u".png"_q,
							  folder),
						  crl::guard(this, [=](QString &&result)
						  {
							  if (!result.isEmpty()) {
								  saveImage(image, result);
							  }
						  }));
				  });
			  });
	addButton(tr::ayu_MessageShotCopy(),
			  [=]
			  {
				  exportImage([=](QImage image)
				  {
					  if (image.isNull()) {
						  return;
					  }
					  QGuiApplication::clipboard()->setImage(image);
					  _config.controller->showToast(tr::fresh_MessageShotCopied(tr::now));

					  _tookShot = true;
					  closeBox();
				  });
			  });
	addLeftButton(tr::fresh_MessageShotSend(),
			  [=]
			  {
				  exportImage([=](QImage image)
				  {
					  if (image.isNull()) {
						  return;
					  }
					  const auto controller = _config.controller;
					  auto chosen = [=](not_null<Data::Thread*> thread)
					  {
						  const auto weakThread = base::make_weak(thread);
						  const auto weakController = base::make_weak(controller);
						  crl::on_main([=]
						  {
							  const auto strongController = weakController.get();
							  const auto strong = weakThread.get();
							  if (!strongController || !strong) {
								  return;
							  }
							  // open the chat and its send files box with the shot
							  auto data = std::make_unique<QMimeData>();
							  data->setImageData(image);
							  strongController->content()->filesOrForwardDrop(
								  strong,
								  data.get(),
								  true);
						  });
						  if (weak) {
							  _tookShot = true;
							  closeBox();
						  }
						  return true;
					  };
					  controller->show(
						  Window::PrepareChooseRecipientBox(
							  &controller->session(),
							  std::move(chosen),
							  tr::fresh_MessageShotSend()),
						  Ui::LayerOption::KeepOther);
				  });
			  });

	updatePreview();
	firstPreviewLatch->await(std::chrono::seconds(1));

	boxClosing() | rpl::on_next(
		[=]
		{
			AyuFeatures::MessageShot::resetCustomSelected();
			AyuFeatures::MessageShot::resetDefaultSelected();
			AyuFeatures::MessageShot::resetShotConfig();

			AyuSettings::getInstance().setSimpleQuotesAndReplies(savedSimpleQuotesAndReplies);
		},
		content->lifetime());

	boxWidth->value(
	) | rpl::on_next([=](int width)
	{
		content->resizeToWidth(width);
	}, content->lifetime());
	rpl::combine(
		boxWidth->value(),
		content->heightValue()
	) | rpl::on_next([=](int width, int height)
	{
		setDimensions(width, height);
	}, content->lifetime());

	scrollToWidget(latestToggle);
}
