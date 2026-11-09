// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ayu_infra.h"

#include "ayu/ayu_settings.h"
#include "ayu/ayu_ui_settings.h"
#include "ayu/ayu_worker.h"
#include "ayu/data/ayu_database.h"
#include "ayu/ui/ayu_logo.h"
#include "features/translator/ayu_translator.h"
#include "lang/lang_instance.h"
#include "ui/chat/chat_style_radius.h"

#ifdef Q_OS_WIN
#include "ayu/utils/windows_utils.h"
#endif

#ifdef Q_OS_LINUX
#include "platform/platform_specific.h"
#endif

namespace AyuInfra {

void initUiSettings() {
	const auto &settings = AyuSettings::getInstance();

	AyuUiSettings::setMonoFont(settings.monoFont());
	AyuUiSettings::setWideMultiplier(settings.wideMultiplier());
	AyuUiSettings::setMaterialSwitches(settings.materialSwitches()
		&& settings.designStyle() != int(AyuUiSettings::DesignStyle::Telegram));
	AyuUiSettings::setIconSet(settings.iconSet());
	AyuUiSettings::setDesignStyle(settings.designStyle());
	AyuUiSettings::setAvatarCorners(settings.avatarCorners());
	Ui::SetAppliedBubbleRadius(settings.messageBubbleRadius());
}

void initWorker() {
	AyuWorker::initialize();
}

void initTranslator() {
	Ayu::Translator::TranslateManager::init();
}

void initIcon() {
#ifdef Q_OS_WIN
	AyuAssets::loadAppIco();
	reloadAppIconFromTaskBar();
#endif

#ifdef Q_OS_LINUX
	// InstallLauncher() has just put the default icon back.
	if (AyuSettings::getInstance().appIcon() != AyuAssets::DEFAULT_ICON) {
		Platform::UpdateLauncherIcon();
	}
#endif
}

void init() {
	initUiSettings();
	initIcon();
	initWorker();
	initTranslator();
}

}
