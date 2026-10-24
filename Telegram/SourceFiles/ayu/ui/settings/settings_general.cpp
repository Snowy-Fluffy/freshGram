// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/settings/settings_general.h"

#include "lang_auto.h"
#include "ayu/ayu_settings.h"
#include "ayu/data/ayu_database.h"
#include "ayu/features/recognition/recognition_models.h"
#include "ayu/features/recognition/speech_recognition.h"
#include "ayu/features/recognition/text_recognition.h"
#include "ayu/features/recognition/text_translation.h"
#include "ayu/ui/boxes/recognition_models_box.h"
#include "ayu/ui/settings/ayu_builder.h"
#include "ayu/ui/settings/settings_ayu_utils.h"
#include "ayu/ui/settings/settings_main.h"
#include "base/platform/base_platform_info.h"
#include "core/application.h"
#include "lang/lang_text_entity.h"
#include "platform/platform_translate_provider.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtCore/QRegularExpression>

namespace Settings {

using namespace Builder;
using namespace AyuBuilder;

namespace {

void BuildTranslator(SectionBuilder &builder, AyuSectionBuilder &ayu) {
	builder.addSubsectionTitle(tr::lng_translate_settings_subtitle());

	auto *settings = &AyuSettings::getInstance();

	const auto options = std::vector{
		std::pair(TranslationProvider::Telegram, QString("Telegram")),
		std::pair(TranslationProvider::Google, QString("Google")),
		std::pair(TranslationProvider::Yandex, QString("Yandex")),
	};
	const auto nativeAvailable = Platform::IsTranslateProviderAvailable();
	auto availableOptions = options;
	if (nativeAvailable) {
		availableOptions.push_back(std::pair(
			TranslationProvider::Native,
			[] {
				if constexpr (Platform::IsMac()) {
					return QString("macOS");
				} else if constexpr (Platform::IsWindows()) {
					return QString("Windows");
				} else {
					return QString("Linux");
				}
			}()));
	}
	if (FreshRecognition::TextTranslationAvailable()) {
		availableOptions.push_back(std::pair(
			TranslationProvider::Local,
			tr::fresh_TranslateProviderLocal(tr::now)));
	}
	auto optionLabels = std::vector<QString>();
	optionLabels.reserve(availableOptions.size());
	for (const auto &option : availableOptions) {
		optionLabels.push_back(option.second);
	}

	const auto getIndex = [=](TranslationProvider val) {
		const auto i = ranges::find(
			availableOptions,
			val,
			&std::pair<TranslationProvider, QString>::first);
		return (i != end(availableOptions))
			? int(i - begin(availableOptions))
			: 0;
	};

	auto currentVal = AyuSettings::getInstance().translationProviderValue()
		| rpl::map(getIndex)
		| rpl::map([=](int val) { return availableOptions[val].second; });

	const auto button = builder.addButton({
		.id = u"ayu/translationProvider"_q,
		.title = tr::ayu_TranslationProvider(),
		.st = &st::settingsButtonNoIcon,
		.label = std::move(currentVal),
		.onClick = [=] {
			if (const auto controller = Core::App().activeWindow()->sessionController()) {
				controller->show(Box(
						[=](not_null<Ui::GenericBox*> box) {
							const auto save = [=](int index) {
								const auto option = availableOptions[index].first;
								AyuSettings::getInstance().setTranslationProvider(option);

								if constexpr (Platform::IsMac()) {
									if (option == TranslationProvider::Native) {
										controller->showToast(Ui::Toast::Config{
											.text = tr::lng_translate_settings_use_platform_mac_about(tr::now, tr::rich),
											.duration = 6 * crl::time(1000)
										});
									}
								}
							};
							SingleChoiceBox(box, {
								.title = tr::ayu_TranslationProvider(),
								.options = optionLabels,
								.initialSelection = getIndex(settings->translationProvider()),
								.callback = save,
							});
						}));
			}
		},
	});
	if (button) {
		ayu.addBetaBadge(button);
	}
	if (!FreshRecognition::TextTranslationAvailable()) {
		return;
	}
	const auto controller = builder.controller();
	builder.addButton({
		.id = u"ayu/translateModels"_q,
		.altIds = { u"ayu/localTranslation"_q, u"ayu/offlineTranslation"_q },
		.title = tr::fresh_ManageTranslateModels(),
		.st = &st::settingsButtonNoIcon,
		.label = rpl::single(rpl::empty)
			| rpl::then(rpl::merge(
				FreshRecognition::ModelsChanged(),
				FreshRecognition::ModelDownloadFinished() | rpl::to_empty))
			| rpl::map([] {
				auto codes = QStringList();
				for (const auto &model
					: FreshRecognition::InstalledTranslateModels()) {
					codes.push_back(model.code);
				}
				return codes.isEmpty()
					? tr::fresh_RecognitionOff(tr::now)
					: codes.join(u", "_q);
			}),
		.onClick = [=] {
			controller->show(Box<AyuUi::RecognitionModelsBox>(
				&controller->session(),
				FreshRecognition::ModelKind::Translate));
		},
		.shown = settings->translationProviderValue()
			| rpl::map([](TranslationProvider value) {
				return value == TranslationProvider::Local;
			}),
	});
}

void BuildTranscription(SectionBuilder &builder, AyuSectionBuilder &ayu) {
	const auto controller = builder.controller();
	auto *settings = &AyuSettings::getInstance();
	const auto whisper = FreshRecognition::SpeechRecognitionAvailable();

	auto options = std::vector<QString>{ u"Telegram"_q };
	if (whisper) {
		options.push_back(tr::fresh_TranscriptionWhisper(tr::now));
	}

	builder.addSubsectionTitle(tr::fresh_TranscriptionSubtitle());
	builder.addButton({
		.id = u"ayu/transcriptionProvider"_q,
		.altIds = { u"ayu/whisperProvider"_q, u"ayu/voiceToText"_q },
		.title = tr::fresh_TranscriptionProvider(),
		.st = &st::settingsButtonNoIcon,
		.label = settings->transcriptionProviderValue()
			| rpl::map([=](TranscriptionProvider value) {
				const auto index = std::min(
					int(value),
					int(options.size()) - 1);
				return options[index];
			}),
		.onClick = [=] {
			controller->show(Box([=](not_null<Ui::GenericBox*> box) {
				SingleChoiceBox(box, {
					.title = tr::fresh_TranscriptionProvider(),
					.options = options,
					.initialSelection = std::min(
						int(AyuSettings::getInstance().transcriptionProvider()),
						int(options.size()) - 1),
					.callback = [=](int index) {
						AyuSettings::getInstance().setTranscriptionProvider(
							static_cast<TranscriptionProvider>(index));
					},
				});
			}));
		},
	});
	if (!whisper) {
		return;
	}
	builder.addButton({
		.id = u"ayu/whisperModel"_q,
		.altIds = { u"ayu/whisperModels"_q, u"ayu/speechModel"_q },
		.title = tr::fresh_WhisperModel(),
		.st = &st::settingsButtonNoIcon,
		.label = settings->whisperModelValue()
			| rpl::map([](QString file) {
				if (file.isEmpty()) {
					return tr::fresh_RecognitionOff(tr::now);
				}
				file.remove(QRegularExpression(u"^ggml-|-q\\d+_\\d+\\.bin$"_q));
				return file;
			}),
		.onClick = [=] {
			controller->show(Box<AyuUi::RecognitionModelsBox>(
				&controller->session(),
				FreshRecognition::ModelKind::Whisper));
		},
		.shown = settings->transcriptionProviderValue()
			| rpl::map([](TranscriptionProvider value) {
				return value == TranscriptionProvider::WhisperLocal;
			}),
	});
}

void BuildTextRecognition(SectionBuilder &builder) {
	if (!FreshRecognition::TextRecognitionAvailable()) {
		return;
	}
	const auto controller = builder.controller();
	auto *settings = &AyuSettings::getInstance();

	builder.addSubsectionTitle(tr::fresh_TextRecognitionSubtitle());
	builder.addButton({
		.id = u"ayu/ocrLanguages"_q,
		.altIds = { u"ayu/ocr"_q, u"ayu/tesseract"_q, u"ayu/textRecognition"_q },
		.title = tr::fresh_ManageOcrLanguages(),
		.st = &st::settingsButtonNoIcon,
		.label = settings->ocrLanguagesValue()
			| rpl::map([](QString codes) {
				const auto languages = FreshRecognition::OcrCodes(
					codes).join(u", "_q);
				if (!FreshRecognition::IsAutoOcr(codes)) {
					return languages.isEmpty()
						? tr::fresh_RecognitionOff(tr::now)
						: languages;
				}
				return languages.isEmpty()
					? tr::fresh_OcrAutoShort(tr::now)
					: tr::fresh_OcrAutoWith(
						tr::now,
						lt_languages,
						languages);
			}),
		.onClick = [=] {
			controller->show(Box<AyuUi::RecognitionModelsBox>(
				&controller->session(),
				FreshRecognition::ModelKind::Tesseract));
		},
	});
}

void BuildShowPeerId(SectionBuilder &builder) {
	auto *settings = &AyuSettings::getInstance();

	const auto options = std::vector{
		QString(tr::ayu_SettingsShowID_Hide(tr::now)),
		QString("Telegram API"),
		QString("Bot API")
	};

	auto currentVal = AyuSettings::getInstance().showPeerIdValue()
		| rpl::map([=](PeerIdDisplay val) {
			return options[static_cast<int>(val)];
		});

	const auto controller = builder.controller();
	builder.addButton({
		.id = u"ayu/showPeerId"_q,
		.altIds = { u"ayu/showIdAndDc"_q },
		.title = tr::ayu_SettingsShowID(),
		.st = &st::settingsButtonNoIcon,
		.label = std::move(currentVal),
		.onClick = [=] {
			controller->show(Box(
				[=](not_null<Ui::GenericBox*> box) {
					const auto save = [=](int index) {
						AyuSettings::getInstance().setShowPeerId(
							static_cast<PeerIdDisplay>(index));
					};
					SingleChoiceBox(box, {
						.title = tr::ayu_SettingsShowID(),
						.options = options,
						.initialSelection = static_cast<int>(settings->showPeerId()),
						.callback = save,
					});
				}));
		},
	});
}

void BuildQoLToggles(SectionBuilder &builder, AyuSectionBuilder &ayu) {
	auto *settings = &AyuSettings::getInstance();

	BuildTranslator(builder, ayu);
	ayu.addSectionDivider();

	BuildTranscription(builder, ayu);
	ayu.addSectionDivider();

	if (FreshRecognition::TextRecognitionAvailable()) {
		BuildTextRecognition(builder);
		ayu.addSectionDivider();
	}

	builder.addSubsectionTitle(tr::ayu_CategoryGeneral());

	const auto controller = builder.controller();
	ayu.addSettingToggle({
		.id = u"ayu/localPinnedChats"_q,
		.title = tr::fresh_LocalPinnedChats(),
		.getter = &AyuSettings::localPinnedChats,
		.setter = &AyuSettings::setLocalPinnedChats,
	});
	ayu.addToggle({
		.id = u"ayu/disableStories"_q,
		.altIds = { u"ayu/hideStories"_q },
		.title = tr::ayu_DisableStories(),
		.getter = [=] { return settings->disableStories(); },
		.setter = [=](bool enabled) {
			AyuSettings::getInstance().setDisableStories(enabled);
			ShowRestartPrompt(controller);
		},
	});

	ayu.addSettingToggle({
		.id = u"ayu/disableOpenLinkWarning"_q,
		.title = tr::ayu_DisableOpenLinkWarning(),
		.getter = &AyuSettings::disableOpenLinkWarning,
		.setter = &AyuSettings::setDisableOpenLinkWarning,
	});

	ayu.addCollapsibleToggle({
		.id = u"ayu/similarChannels"_q,
		.title = tr::ayu_DisableSimilarChannels(),
		.checkboxes = {
			NestedEntry{
				tr::ayu_CollapseSimilarChannels(tr::now),
				[] { return AyuSettings::getInstance().collapseSimilarChannels(); },
				[](bool v) { AyuSettings::getInstance().setCollapseSimilarChannels(v); }
			},
			NestedEntry{
				tr::ayu_HideSimilarChannelsTab(tr::now),
				[] { return AyuSettings::getInstance().hideSimilarChannels(); },
				[](bool v) { AyuSettings::getInstance().setHideSimilarChannels(v); }
			}
		},
		.toggledWhenAll = true,
	});

	ayu.addSettingToggle({
		.id = u"ayu/disableNotificationsDelay"_q,
		.title = tr::ayu_DisableNotificationsDelay(),
		.getter = &AyuSettings::disableNotificationsDelay,
		.setter = &AyuSettings::setDisableNotificationsDelay,
	});

	ayu.addSectionDivider();

	const auto zalgoButton = builder.addButton({
		.id = u"ayu/filterZalgo"_q,
		.title = tr::ayu_FilterZalgo(),
		.st = &st::settingsButtonNoIcon,
		.toggled = rpl::single(settings->filterZalgo()),
	});
	if (zalgoButton) {
		zalgoButton->toggledValue(
		) | rpl::filter(
			[=](bool enabled) {
				return (enabled != settings->filterZalgo());
			}
		) | on_next(
			[=](bool enabled) {
				AyuSettings::getInstance().setFilterZalgo(enabled);
				ShowRestartPrompt(controller);
			},
			zalgoButton->lifetime());
		ayu.addBetaBadge(zalgoButton);
	}

	ayu.addSettingToggle({
		.id = u"ayu/improveLinkPreviews"_q,
		.title = tr::ayu_ImproveLinkPreviews(),
		.getter = &AyuSettings::improveLinkPreviews,
		.setter = &AyuSettings::setImproveLinkPreviews,
	});
	ayu.addCollapsibleToggle({
		.id = u"ayu/confirmations"_q,
		.title = tr::ayu_ConfirmationsTitle(),
		.checkboxes = {
			NestedEntry{
				tr::ayu_StickerConfirmation(tr::now),
				[] { return AyuSettings::getInstance().stickerConfirmation(); },
				[](bool v) { AyuSettings::getInstance().setStickerConfirmation(v); }
			},
			NestedEntry{
				tr::ayu_GIFConfirmation(tr::now),
				[] { return AyuSettings::getInstance().gifConfirmation(); },
				[](bool v) { AyuSettings::getInstance().setGifConfirmation(v); }
			},
			NestedEntry{
				tr::ayu_VoiceConfirmation(tr::now),
				[] { return AyuSettings::getInstance().voiceConfirmation(); },
				[](bool v) { AyuSettings::getInstance().setVoiceConfirmation(v); }
			},
			NestedEntry{
				tr::ayu_RoundConfirmation(tr::now),
				[] { return AyuSettings::getInstance().roundConfirmation(); },
				[](bool v) { AyuSettings::getInstance().setRoundConfirmation(v); }
			}
		},
		.toggledWhenAll = false,
	});
	ayu.addSettingToggle({
		.id = u"ayu/showMessageSeconds"_q,
		.altIds = { u"ayu/formatTimeWithSeconds"_q },
		.title = tr::ayu_SettingsShowMessageSeconds(),
		.getter = &AyuSettings::showMessageSeconds,
		.setter = &AyuSettings::setShowMessageSeconds,
	});
	ayu.addSettingToggle({
		.id = u"ayu/showTimeZone"_q,
		.altIds = { u"ayu/timeZoneName"_q },
		.title = tr::fresh_SettingsShowTimeZone(),
		.getter = &AyuSettings::showTimeZone,
		.setter = &AyuSettings::setShowTimeZone,
	});

	BuildShowPeerId(builder);

	ayu.addSectionDivider();

	builder.addSubsectionTitle(rpl::single(QString("Webview")));

	ayu.addSettingToggle({
		.id = u"ayu/spoofWebviewAsAndroid"_q,
		.title = tr::ayu_SettingsSpoofWebviewAsAndroid(),
		.getter = &AyuSettings::spoofWebviewAsAndroid,
		.setter = &AyuSettings::setSpoofWebviewAsAndroid,
	});

	ayu.addCollapsibleToggle({
		.id = u"ayu/biggerWindow"_q,
		.title = tr::ayu_SettingsBiggerWindow(),
		.checkboxes = {
			NestedEntry{
				tr::ayu_SettingsIncreaseWebviewHeight(tr::now),
				[] { return AyuSettings::getInstance().increaseWebviewHeight(); },
				[](bool v) { AyuSettings::getInstance().setIncreaseWebviewHeight(v); }
			},
			NestedEntry{
				tr::ayu_SettingsIncreaseWebviewWidth(tr::now),
				[] { return AyuSettings::getInstance().increaseWebviewWidth(); },
				[](bool v) { AyuSettings::getInstance().setIncreaseWebviewWidth(v); }
			}
		},
		.toggledWhenAll = false,
	});
	ayu.addSectionDivider();

	builder.addSubsectionTitle(tr::lng_settings_privacy_title());

	ayu.addSettingToggle({
		.id = u"ayu/keepDraftsLocal"_q,
		.altIds = { u"ayu/draftsSync"_q },
		.title = tr::fresh_KeepDraftsLocal(),
		.getter = &AyuSettings::keepDraftsLocal,
		.setter = &AyuSettings::setKeepDraftsLocal,
	});
	ayu.addSettingToggle({
		.id = u"ayu/reduceNetworkTracking"_q,
		.altIds = { u"ayu/temporaryKeyLifetime"_q },
		.title = tr::fresh_ReduceNetworkTracking(),
		.getter = &AyuSettings::reduceNetworkTracking,
		.setter = &AyuSettings::setReduceNetworkTracking,
	});

	builder.addSkip();
	builder.addDividerText(tr::fresh_NetworkPrivacyDescription());

	ayu.addSettingToggle({
		.id = u"ayu/enableDebugLogging"_q,
		.title = tr::fresh_EnableDebugLogging(),
		.getter = &AyuSettings::enableDebugLogging,
		.setter = &AyuSettings::setEnableDebugLogging,
	});

	builder.addSkip();
	builder.addDividerText(tr::fresh_DebugLoggingDescription());

	builder.addButton({
		.id = u"ayu/databaseReport"_q,
		.title = tr::fresh_DatabaseReport(),
		.icon = { &st::menuIconStats },
		.onClick = [] {
			Ui::Toast::Show(tr::fresh_DatabaseReportStarted(tr::now));
			AyuDatabase::saveSizeReport([](bool ok) {
				Ui::Toast::Show(ok
					? tr::fresh_DatabaseReportSaved(tr::now)
					: tr::fresh_DatabaseReportFailed(tr::now));
			});
		},
		.shown = AyuSettings::getInstance().enableDebugLoggingValue(),
	});
}

const auto kMeta = BuildHelper({
	.id = AyuGeneral::Id(),
	.parentId = AyuMain::Id(),
	.title = &tr::ayu_CategoryGeneral,
	.icon = &st::menuIconShowAll,
}, [](SectionBuilder &builder) {
	auto ayu = AyuSectionBuilder(builder);

	builder.addSkip();
	BuildQoLToggles(builder, ayu);
	builder.addSkip();
});

} // namespace

rpl::producer<QString> AyuGeneral::title() {
	return tr::ayu_CategoryGeneral();
}

AyuGeneral::AyuGeneral(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

void AyuGeneral::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

Type AyuGeneralId() {
	return AyuGeneral::Id();
}

} // namespace Settings
