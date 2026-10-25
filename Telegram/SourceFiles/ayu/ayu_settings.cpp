// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/data/fresh_vault.h"
#include "ayu/utils/crash_trace.h"
#include "ayu/utils/debug_logging.h"
#include "ayu/utils/fresh_storage_migration.h"
#include "ayu/utils/network_privacy.h"
#include "ayu/ayu_settings.h"

#include "lang_auto.h"
#include "tray.h"
#include "ayu/ayu_ui_settings.h"
#include "ayu/ayu_worker.h"
#include "ayu/data/ayu_database.h"
#include "ayu/data/kept_dialogs.h"
#include "ayu/data/fresh_database_backup.h"
#include "ayu/features/recognition/text_translation.h"
#include "ayu/features/streamer_mode/streamer_mode.h"
#include "ayu/ui/ayu_logo.h"
#include "core/application.h"
#include "features/filters/filters_cache_controller.h"
#include "features/translator/ayu_translator.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "platform/platform_translate_provider.h"
#include "rpl/combine.h"
#include "ui/style/style_core.h"
#include "window/window_controller.h"

#include <filesystem>
#include <fstream>
#include <QApplication>
#include <QFile>

using json = nlohmann::json;

namespace {

constexpr auto kMaxSettingsSize = std::uintmax_t(2) * 1024 * 1024;

bool AccountsAttached = false;

QString getAccountsPath() {
	return cWorkingDir() + u"tdata/fresh_accounts.enc"_q;
}

std::filesystem::path getSettingsPath() {
	const auto path = cWorkingDir() + u"tdata/ayu_settings.json"_q;
#ifdef Q_OS_WIN
	return std::filesystem::path(path.toStdWString());
#else // Q_OS_WIN
	return std::filesystem::path(path.toStdString());
#endif // !Q_OS_WIN
}

void repaintApp() {
	for (QWidget *widget : QApplication::allWidgets()) {
		widget->update();
	}
}

rpl::lifetime lifetime; // idk reactivity dies when placed in `GhostModeAccountSettings` as field

} // namespace

GhostModeAccountSettings::GhostModeAccountSettings() {
	rpl::combine(
		_sendReadMessages.value(),
		_sendReadMessagesLocked.value(),
		_sendReadStories.value(),
		_sendReadStoriesLocked.value(),
		_sendOnlinePackets.value(),
		_sendOnlinePacketsLocked.value(),
		_sendUploadProgress.value(),
		_sendUploadProgressLocked.value(),
		_sendOfflinePacketAfterOnline.value(),
		_sendOfflinePacketAfterOnlineLocked.value(),
		_useScheduledMessages.value(),
		_useScheduledMessagesLocked.value()
	) | rpl::on_next([=](
			bool readMsg, bool readMsgLocked,
			bool readStories, bool readStoriesLocked,
			bool online, bool onlineLocked,
			bool upload, bool uploadLocked,
			bool offline, bool offlineLocked,
			bool scheduled, bool scheduledLocked) {
		_ghostModeActive = (readMsgLocked || !readMsg)
			&& (readStoriesLocked || !readStories)
			&& (onlineLocked || !online)
			&& (uploadLocked || !upload)
			&& (offlineLocked || offline)
			&& (scheduledLocked || scheduled);
	}, lifetime);
}

void GhostModeAccountSettings::setSendReadMessages(bool val) {
	if (_sendReadMessages.current() == val) return;
	_sendReadMessages = val;
	AyuSettings::save();
}

void GhostModeAccountSettings::setSendReadStories(bool val) {
	if (_sendReadStories.current() == val) return;
	_sendReadStories = val;
	AyuSettings::save();
}

void GhostModeAccountSettings::setSendOnlinePackets(bool val) {
	if (_sendOnlinePackets.current() == val) return;
	_sendOnlinePackets = val;
	AyuSettings::save();
}

void GhostModeAccountSettings::setSendUploadProgress(bool val) {
	if (_sendUploadProgress.current() == val) return;
	_sendUploadProgress = val;
	AyuSettings::save();
}

void GhostModeAccountSettings::setSendOfflinePacketAfterOnline(bool val) {
	if (_sendOfflinePacketAfterOnline.current() == val) return;
	_sendOfflinePacketAfterOnline = val;
	AyuSettings::save();
}

void GhostModeAccountSettings::setMarkReadAfterAction(bool val) {
	if (_markReadAfterAction.current() == val) return;
	_markReadAfterAction = val;
	AyuSettings::save();
}

void GhostModeAccountSettings::setUseScheduledMessages(bool val) {
	if (_useScheduledMessages.current() == val) return;
	_useScheduledMessages = val;
	AyuSettings::save();
}

bool GhostModeAccountSettings::shouldSendWithoutSound() const {
	switch (_sendWithoutSound.current()) {
	case SendWithoutSoundOption::Never:
		return false;
	case SendWithoutSoundOption::InGhostMode:
		return isGhostModeActive();
	case SendWithoutSoundOption::Always:
		return true;
	}
	Unexpected("Value in GhostModeAccountSettings::shouldSendWithoutSound.");
}

void GhostModeAccountSettings::setSendWithoutSound(
		SendWithoutSoundOption val) {
	if (_sendWithoutSound.current() == val) return;
	_sendWithoutSound = val;
	AyuSettings::save();
}

void GhostModeAccountSettings::setSuggestGhostModeBeforeViewingStory(bool val) {
	if (_suggestGhostModeBeforeViewingStory.current() == val) return;
	_suggestGhostModeBeforeViewingStory = val;
	AyuSettings::save();
}

void GhostModeAccountSettings::setGhostModeEnabled(bool val) {
	if (!_sendReadMessagesLocked.current()) _sendReadMessages = !val;
	if (!_sendReadStoriesLocked.current()) _sendReadStories = !val;
	if (!_sendOnlinePacketsLocked.current()) _sendOnlinePackets = !val;
	if (!_sendUploadProgressLocked.current()) _sendUploadProgress = !val;
	if (!_sendOfflinePacketAfterOnlineLocked.current()) _sendOfflinePacketAfterOnline = val;
	if (!_useScheduledMessagesLocked.current()) {
		_useScheduledMessages = val;
		if (val) _markReadAfterAction = false;
	}
	if (!_markReadAfterActionLocked.current()) {
		// Read on interact is off in the ghost mode and on otherwise,
		// but never together with the scheduled messages.
		_markReadAfterAction = !val && !_useScheduledMessages.current();
	}
	AyuSettings::save();

	if (val) {
		if (const auto window = Core::App().activeWindow()) {
			if (const auto session = window->maybeSession()) {
				AyuWorker::markAsOnline(session);
			}
		}
	}
}

void GhostModeAccountSettings::setSendReadMessagesLocked(bool val) {
	if (_sendReadMessagesLocked.current() == val) return;
	_sendReadMessagesLocked = val;
	AyuSettings::save();
}

void GhostModeAccountSettings::setSendReadStoriesLocked(bool val) {
	if (_sendReadStoriesLocked.current() == val) return;
	_sendReadStoriesLocked = val;
	AyuSettings::save();
}

void GhostModeAccountSettings::setSendOnlinePacketsLocked(bool val) {
	if (_sendOnlinePacketsLocked.current() == val) return;
	_sendOnlinePacketsLocked = val;
	AyuSettings::save();
}

void GhostModeAccountSettings::setSendUploadProgressLocked(bool val) {
	if (_sendUploadProgressLocked.current() == val) return;
	_sendUploadProgressLocked = val;
	AyuSettings::save();
}

void GhostModeAccountSettings::setSendOfflinePacketAfterOnlineLocked(bool val) {
	if (_sendOfflinePacketAfterOnlineLocked.current() == val) return;
	_sendOfflinePacketAfterOnlineLocked = val;
	AyuSettings::save();
}

void GhostModeAccountSettings::setUseScheduledMessagesLocked(bool val) {
	if (_useScheduledMessagesLocked.current() == val) return;
	_useScheduledMessagesLocked = val;
	AyuSettings::save();
}

void GhostModeAccountSettings::setMarkReadAfterActionLocked(bool val) {
	if (_markReadAfterActionLocked.current() == val) return;
	_markReadAfterActionLocked = val;
	AyuSettings::save();
}

void to_json(nlohmann::json &j, const GhostModeAccountSettings &s) {
	j = nlohmann::json{
		{"sendReadMessages", s._sendReadMessages.current()},
		{"sendReadStories", s._sendReadStories.current()},
		{"sendOnlinePackets", s._sendOnlinePackets.current()},
		{"sendUploadProgress", s._sendUploadProgress.current()},
		{"sendOfflinePacketAfterOnline", s._sendOfflinePacketAfterOnline.current()},
		{"markReadAfterAction", s._markReadAfterAction.current()},
		{"useScheduledMessages", s._useScheduledMessages.current()},
		{"sendWithoutSound", s._sendWithoutSound.current()},
		{"suggestGhostModeBeforeViewingStory", s._suggestGhostModeBeforeViewingStory.current()},
		{"sendReadMessagesLocked", s._sendReadMessagesLocked.current()},
		{"sendReadStoriesLocked", s._sendReadStoriesLocked.current()},
		{"sendOnlinePacketsLocked", s._sendOnlinePacketsLocked.current()},
		{"sendUploadProgressLocked", s._sendUploadProgressLocked.current()},
		{"sendOfflinePacketAfterOnlineLocked", s._sendOfflinePacketAfterOnlineLocked.current()},
		{"useScheduledMessagesLocked", s._useScheduledMessagesLocked.current()},
		{"markReadAfterActionLocked", s._markReadAfterActionLocked.current()}
	};
}

void from_json(const nlohmann::json &j, GhostModeAccountSettings &s) {
	s._sendReadMessages = j.value("sendReadMessages", true);
	s._sendReadStories = j.value("sendReadStories", true);
	s._sendOnlinePackets = j.value("sendOnlinePackets", true);
	s._sendUploadProgress = j.value("sendUploadProgress", true);
	s._sendOfflinePacketAfterOnline = j.value("sendOfflinePacketAfterOnline", false);
	s._markReadAfterAction = j.value("markReadAfterAction", true);
	s._useScheduledMessages = j.value("useScheduledMessages", false);
	const auto sendWithoutSound = j.find("sendWithoutSound");
	s._sendWithoutSound = (sendWithoutSound == j.end())
		? SendWithoutSoundOption::Never
		: sendWithoutSound->is_boolean()
		? (sendWithoutSound->get<bool>()
			? SendWithoutSoundOption::Always
			: SendWithoutSoundOption::Never)
		: sendWithoutSound->get<SendWithoutSoundOption>();
	s._suggestGhostModeBeforeViewingStory = j.value("suggestGhostModeBeforeViewingStory", true);
	s._sendReadMessagesLocked = j.value("sendReadMessagesLocked", false);
	s._sendReadStoriesLocked = j.value("sendReadStoriesLocked", false);
	s._sendOnlinePacketsLocked = j.value("sendOnlinePacketsLocked", false);
	s._sendUploadProgressLocked = j.value("sendUploadProgressLocked", false);
	s._sendOfflinePacketAfterOnlineLocked = j.value("sendOfflinePacketAfterOnlineLocked", false);
	s._useScheduledMessagesLocked = j.value("useScheduledMessagesLocked", true);
	s._markReadAfterActionLocked = j.value("markReadAfterActionLocked", false);
}

void MessageShotSettings::setBackground(Background val) {
	if (_background.current() == int(val)) return;
	_background = int(val);
	AyuSettings::save();
}

void MessageShotSettings::setShowAvatars(bool val) {
	if (_showAvatars.current() == val) return;
	_showAvatars = val;
	AyuSettings::save();
}

void MessageShotSettings::setShowNames(bool val) {
	if (_showNames.current() == val) return;
	_showNames = val;
	AyuSettings::save();
}

void MessageShotSettings::setPadding(int val) {
	if (_padding.current() == val) return;
	_padding = val;
	AyuSettings::save();
}

void MessageShotSettings::setWidth(int val) {
	if (_width.current() == val) return;
	_width = val;
	AyuSettings::save();
}

void MessageShotSettings::setScale(int val) {
	if (_scale.current() == val) return;
	_scale = val;
	AyuSettings::save();
}

void MessageShotSettings::setFormat(Format val) {
	if (_format.current() == int(val)) return;
	_format = int(val);
	AyuSettings::save();
}

void MessageShotSettings::setLastSaveFolder(const QString &val) {
	if (_lastSaveFolder.current() == val) return;
	_lastSaveFolder = val;
	AyuSettings::save();
}

void MessageShotSettings::setShowDate(bool val) {
	if (_showDate.current() == val) return;
	_showDate = val;
	AyuSettings::save();
}

void MessageShotSettings::setShowReactions(bool val) {
	if (_showReactions.current() == val) return;
	_showReactions = val;
	AyuSettings::save();
}

void MessageShotSettings::setShowHeaderDecorations(bool val) {
	if (_showHeaderDecorations.current() == val) return;
	_showHeaderDecorations = val;
	AyuSettings::save();
}

void MessageShotSettings::setShowColorfulReplies(bool val) {
	if (_showColorfulReplies.current() == val) return;
	_showColorfulReplies = val;
	AyuSettings::save();
}

void MessageShotSettings::setRevealSpoilers(bool val) {
	if (_revealSpoilers.current() == val) return;
	_revealSpoilers = val;
	AyuSettings::save();
}

bool MessageShotSettings::isCloudThemeEmpty() const {
	return !_cloudThemeId.current()
		&& !_cloudThemeAccessHash.current()
		&& !_cloudThemeDocumentId.current()
		&& _cloudThemeTitle.current().isEmpty();
}

void MessageShotSettings::clearCloudThemeData() {
	_cloudThemeId = uint64(0);
	_cloudThemeAccessHash = uint64(0);
	_cloudThemeDocumentId = uint64(0);
	_cloudThemeTitle = QString();
	_cloudThemeAccountId = uint64(0);
}

void MessageShotSettings::setEmbeddedTheme(int type, uint32 accentColor) {
	if (_embeddedThemeType.current() == type
		&& _embeddedThemeAccentColor.current() == accentColor
		&& isCloudThemeEmpty()) {
		return;
	}
	_embeddedThemeType = type;
	_embeddedThemeAccentColor = accentColor;
	clearCloudThemeData();
	AyuSettings::save();
}

void MessageShotSettings::setCloudTheme(uint64 accountId, uint64 id, uint64 accessHash, uint64 documentId, const QString &title) {
	if (_embeddedThemeType.current() == -1
		&& _embeddedThemeAccentColor.current() == 0
		&& _cloudThemeAccountId.current() == accountId
		&& _cloudThemeId.current() == id
		&& _cloudThemeAccessHash.current() == accessHash
		&& _cloudThemeDocumentId.current() == documentId
		&& _cloudThemeTitle.current() == title) {
		return;
	}
	_embeddedThemeType = -1;
	_embeddedThemeAccentColor = uint32(0);
	_cloudThemeAccountId = accountId;
	_cloudThemeId = id;
	_cloudThemeAccessHash = accessHash;
	_cloudThemeDocumentId = documentId;
	_cloudThemeTitle = title;
	AyuSettings::save();
}

void MessageShotSettings::clearTheme() {
	if (_embeddedThemeType.current() == -1
		&& _embeddedThemeAccentColor.current() == 0
		&& isCloudThemeEmpty()) {
		return;
	}
	_embeddedThemeType = -1;
	_embeddedThemeAccentColor = uint32(0);
	clearCloudThemeData();
	AyuSettings::save();
}

void to_json(nlohmann::json &j, const MessageShotSettings &s) {
	j = nlohmann::json{
		{"background", s._background.current()},
		{"showAvatars", s._showAvatars.current()},
		{"showNames", s._showNames.current()},
		{"padding", s._padding.current()},
		{"width", s._width.current()},
		{"scale", s._scale.current()},
		{"format", s._format.current()},
		{"lastSaveFolder", s._lastSaveFolder.current()},
		{"showDate", s._showDate.current()},
		{"showReactions", s._showReactions.current()},
		{"showHeaderDecorations", s._showHeaderDecorations.current()},
		{"showColorfulReplies", s._showColorfulReplies.current()},
		{"revealSpoilers", s._revealSpoilers.current()},
		{"embeddedThemeType", s._embeddedThemeType.current()},
		{"embeddedThemeAccentColor", s._embeddedThemeAccentColor.current()},
		{"cloudThemeId", s._cloudThemeId.current()},
		{"cloudThemeAccessHash", s._cloudThemeAccessHash.current()},
		{"cloudThemeDocumentId", s._cloudThemeDocumentId.current()},
		{"cloudThemeTitle", s._cloudThemeTitle.current()},
		{"cloudThemeAccountId", s._cloudThemeAccountId.current()},
	};
}

void from_json(const nlohmann::json &j, MessageShotSettings &s) {
	using Background = MessageShotSettings::Background;
	using Format = MessageShotSettings::Format;
	const auto legacyBackground = j.value("showBackground", true)
		? Background::Color
		: Background::Transparent;
	s._background = std::clamp(
		j.value("background", int(legacyBackground)),
		int(Background::Transparent),
		int(Background::Wallpaper));
	s._showAvatars = j.value("showAvatars", true);
	s._showNames = j.value("showNames", true);
	s._padding = std::clamp(j.value("padding", 1), 0, 3);
	s._width = std::clamp(j.value("width", 2), 0, 2);
	s._scale = std::clamp(j.value("scale", 0), 0, 3);
	s._format = std::clamp(
		j.value("format", int(Format::Png)),
		int(Format::Png),
		int(Format::Jpeg));
	s._lastSaveFolder = j.value("lastSaveFolder", QString());
	s._showDate = j.value("showDate", false);
	s._showReactions = j.value("showReactions", false);
	s._showHeaderDecorations = j.value("showHeaderDecorations", true);
	s._showColorfulReplies = j.value("showColorfulReplies", true);
	s._revealSpoilers = j.value("revealSpoilers", true);
	s._embeddedThemeType = j.value("embeddedThemeType", j.value("themeType", -1));
	s._embeddedThemeAccentColor = j.value("embeddedThemeAccentColor", j.value("themeAccentColor", uint32(0)));
	s._cloudThemeId = j.value("cloudThemeId", uint64(0));
	s._cloudThemeAccessHash = j.value("cloudThemeAccessHash", uint64(0));
	s._cloudThemeDocumentId = j.value("cloudThemeDocumentId", uint64(0));
	s._cloudThemeTitle = j.value("cloudThemeTitle", QString());
	s._cloudThemeAccountId = j.value("cloudThemeAccountId", uint64(0));
}

AyuSettings::AyuSettings()
: _appIcon(AyuAssets::DEFAULT_ICON)
, _editedMark(Core::IsAppLaunched() ? tr::lng_edited(tr::now) : QString("edited")) {
}

AyuSettings &AyuSettings::getInstance() {
	static AyuSettings instance;
	return instance;
}

void AyuSettings::load() {
	FreshStorage::MigrateLegacyNames();
	const auto applied = gsl::finally([] {
		const auto &settings = getInstance();
		FreshDatabaseBackup::configure(
			settings.backupKeepCount(),
			settings.backupIntervalHours());
		FreshDatabaseBackup::setKeepRichest(!settings.deleteSavedFiles());
		AyuDatabase::purgeOlderThan(settings.keepDeletedDays());
		FreshNetworkPrivacy::SetEnabled(settings.reduceNetworkTracking());
		FreshDebugLogging::SetEnabled(settings.enableDebugLogging());
	});
	auto sizeError = std::error_code();
	const auto size = std::filesystem::file_size(getSettingsPath(), sizeError);
	if (!sizeError && size > kMaxSettingsSize) {
		LOG(("freshGram settings: the settings file is too big, ignoring it"));
		return;
	}
	std::ifstream file(getSettingsPath());
	if (!file.good()) {
		return;
	}

	auto &settings = getInstance();

	try {
		json p;
		file >> p;
		file.close();

		if (!p.contains("ghostModeSettings")
			&& !p.value("accountsVaulted", false)) {
			p["ghostModeSettings"] = nlohmann::json::object({
				{"0", {
					{"sendReadMessages", p.value("sendReadMessages", true)},
					{"sendReadStories", p.value("sendReadStories", true)},
					{"sendOnlinePackets", p.value("sendOnlinePackets", true)},
					{"sendUploadProgress", p.value("sendUploadProgress", true)},
					{"sendOfflinePacketAfterOnline", p.value("sendOfflinePacketAfterOnline", false)},
					{"markReadAfterAction", p.value("markReadAfterAction", true)},
					{"useScheduledMessages", p.value("useScheduledMessages", false)},
					{"sendWithoutSound", p.value("sendWithoutSound", false)}
				}}
			});
			p["useGlobalGhostMode"] = true;

			LOG(("freshGram settings: migrated ghost mode settings to per-account format"));
		}

		try {
			from_json(p, settings);
		} catch (...) {
			LOG(("freshGram settings: failed to parse settings file"));
		}
	} catch (...) {
		LOG(("freshGram settings: failed to read settings file (not json-like)"));
	}

	applyForcedGhost();

	AyuFeatures::StreamerMode::refresh();
	settings.validate();
}

void AyuSettings::applyForcedGhost() {
	if (!cGhost()) {
		return;
	}
	auto &ghost = AyuSettings::ghost();
	ghost._sendReadMessages = false;
	ghost._sendReadStories = false;
	ghost._sendOnlinePackets = false;
	ghost._sendUploadProgress = false;
	ghost._sendOfflinePacketAfterOnline = true;
}

void AyuSettings::attachVault() {
	if (!FreshVault::HasKey()) {
		return;
	}
	auto &settings = getInstance();
	AccountsAttached = true;
	const auto path = getAccountsPath();
	if (const auto bytes = FreshVault::ReadFile(path, true)) {
		try {
			const auto p = json::parse(bytes->constBegin(), bytes->constEnd());
			if (p.contains("ghostModeSettings")
				&& p["ghostModeSettings"].is_object()) {
				for (auto &[key, value] : p["ghostModeSettings"].items()) {
					auto &slot = settings._ghostAccounts[std::stoull(key)];
					if (!slot) {
						slot = std::make_unique<GhostModeAccountSettings>();
					}
					value.get_to(*slot);
				}
			}
			if (p.contains("shadowBanIds")) {
				settings._shadowBanIds = p["shadowBanIds"].get<
					std::unordered_set<int64>>();
			}
		} catch (...) {
			LOG(("freshGram settings: failed to parse the sealed accounts data"));
		}
	} else if (QFile::exists(path)) {
		LOG(("freshGram settings: could not read the sealed accounts data"));
	}
	applyForcedGhost();
	save();
}

void AyuSettings::save() {
	auto &settings = getInstance();
	json p = settings;

	auto strip = settings._accountsVaulted;
	if (AccountsAttached) {
		const auto sealed = json{
			{ "ghostModeSettings", p["ghostModeSettings"] },
			{ "shadowBanIds", p["shadowBanIds"] },
		};
		strip = FreshVault::WriteSealedFile(
			getAccountsPath(),
			QByteArray::fromStdString(sealed.dump()));
		if (strip) {
			settings._accountsVaulted = true;
		}
	}
	if (strip) {
		p.erase("ghostModeSettings");
		p.erase("shadowBanIds");
	}
	p["accountsVaulted"] = settings._accountsVaulted;

	std::ofstream file;
	file.open(getSettingsPath());
	file << p.dump(4);
	file.close();
}

void AyuSettings::reset() {
	const auto vaulted = getInstance()._accountsVaulted;
	getInstance() = AyuSettings();
	getInstance()._accountsVaulted = vaulted;
	save();
}

GhostModeAccountSettings &AyuSettings::ghost(not_null<Main::Session*> session) {
	return ghost(session->userId().bare);
}

GhostModeAccountSettings &AyuSettings::ghost(uint64 userId) {
	auto &settings = getInstance();
	auto overriddenId = settings.getOverriddenGhostUserId(userId);

	auto it = settings._ghostAccounts.find(overriddenId);
	if (it == settings._ghostAccounts.end()) {
		auto account = std::make_unique<GhostModeAccountSettings>();
		it = settings._ghostAccounts.emplace(overriddenId, std::move(account)).first;
	}

	return *it->second;
}

GhostModeAccountSettings &AyuSettings::ghost() {
	if (const auto window = Core::App().activeWindow()) {
		if (const auto session = window->maybeSession()) {
			return ghost(session);
		}
	}
	return ghost(0);
}

void AyuSettings::setUseGlobalGhostMode(bool val) {
	if (_useGlobalGhostMode.current() == val) return;
	_useGlobalGhostMode = val;
	save();
}

void AyuSettings::addShadowBan(int64 id) {
	if (_shadowBanIds.insert(id).second) {
		FiltersCacheController::rebuildCache();
		FiltersCacheController::fireUpdate();
		save();
	}
}

void AyuSettings::removeShadowBan(int64 id) {
	if (_shadowBanIds.erase(id) > 0) {
		FiltersCacheController::rebuildCache();
		FiltersCacheController::fireUpdate();
		save();
	}
}

void AyuSettings::validate() {
	AyuSettings defaults;
	auto modified = false;

	auto validateRange = [&](auto &var, auto min, auto max, const auto &defaultVar) {
		if (var.current() < min || var.current() > max) {
			var = defaultVar.current();
			modified = true;
		}
	};

	auto validateEnum = [&](auto &var, const auto &defaultVar, int max = 2) {
		auto intVal = static_cast<int>(var.current());
		if (intVal < 0 || intVal > max) {
			var = defaultVar.current();
			modified = true;
		}
	};

	validateEnum(_showPeerId, defaults._showPeerId);
	validateEnum(_channelBottomButton, defaults._channelBottomButton);
	validateEnum(_showReactionsPanelInContextMenu, defaults._showReactionsPanelInContextMenu);
	validateEnum(_showViewsPanelInContextMenu, defaults._showViewsPanelInContextMenu);
	validateEnum(_showHideMessageInContextMenu, defaults._showHideMessageInContextMenu);
	validateEnum(_showUserMessagesInContextMenu, defaults._showUserMessagesInContextMenu);
	validateEnum(_showMessageDetailsInContextMenu, defaults._showMessageDetailsInContextMenu);
	validateEnum(_showRepeatMessageInContextMenu, defaults._showRepeatMessageInContextMenu);
	validateEnum(_showAddFilterInContextMenu, defaults._showAddFilterInContextMenu);

	validateEnum(_translationProvider, defaults._translationProvider, 4);
	validateEnum(_transcriptionProvider, defaults._transcriptionProvider, 1);
	if (((_translationProvider.current() == TranslationProvider::Native)
			&& !Platform::IsTranslateProviderAvailable())
		|| ((_translationProvider.current() == TranslationProvider::Local)
			&& !FreshRecognition::TextTranslationAvailable())) {
		_translationProvider = defaults._translationProvider.current();
		modified = true;
	}

	if (_translateComposeTo.current().size() != 2) {
		_translateComposeTo = defaults._translateComposeTo.current();
		modified = true;
	}

	validateRange(_messageBubbleRadius, 0, 16, defaults._messageBubbleRadius);
	validateRange(_maxEditRevisions, 0, 100000, defaults._maxEditRevisions);
	validateRange(_deletedRestoreLimit, 0, 1000000, defaults._deletedRestoreLimit);
	validateRange(_keptSnapshotLimit, 0, 100000, defaults._keptSnapshotLimit);
	validateRange(_deletedMediaMaxSizeMb, 0, 4096, defaults._deletedMediaMaxSizeMb);
	validateRange(_keepDeletedDays, 0, 3650, defaults._keepDeletedDays);
	validateRange(_passcodeWipeAttempts, 0, 50, defaults._passcodeWipeAttempts);
	validateRange(_passcodeWipeAction, 0, 1, defaults._passcodeWipeAction);
	validateRange(_lockOnMinimize, -1, 3600, defaults._lockOnMinimize);
	validateRange(_lockScreenStyle, 0, 1, defaults._lockScreenStyle);
	validateRange(_lockScreenBackground, 0, 2, defaults._lockScreenBackground);
	validateRange(_lockScreenDim, 0, 75, defaults._lockScreenDim);
	validateRange(_lockScreenAvatarShape, 0, 2, defaults._lockScreenAvatarShape);
	validateRange(_lockScreenCardOpacity, 40, 100, defaults._lockScreenCardOpacity);
	validateRange(_lockScreenCardRadius, 0, 3, defaults._lockScreenCardRadius);
	validateRange(_lockScreenClockFormat, 0, 2, defaults._lockScreenClockFormat);
	validateRange(_lockScreenClockSize, 0, 2, defaults._lockScreenClockSize);
	if (const auto title = sanitizeLockScreenLine(_lockScreenTitle.current(), 40)
		; title != _lockScreenTitle.current()) {
		_lockScreenTitle = title;
		modified = true;
	}
	if (const auto text = sanitizeLockScreenLine(_lockScreenSubmitText.current(), 24)
		; text != _lockScreenSubmitText.current()) {
		_lockScreenSubmitText = text;
		modified = true;
	}
	if (const auto text = sanitizeLockScreenText(_lockScreenText.current())
		; text != _lockScreenText.current()) {
		_lockScreenText = text;
		modified = true;
	}
	validateRange(_hideFromCapture, 0, 2, defaults._hideFromCapture);
	validateRange(_lockedNotifications, 0, 2, defaults._lockedNotifications);
	validateRange(_backupKeepCount, 1, 100, defaults._backupKeepCount);
	validateRange(_backupIntervalHours, 1, 168, defaults._backupIntervalHours);
	validateRange(_replyBackgroundOpacity, 0, 50, defaults._replyBackgroundOpacity);
	validateRange(_reactionBackgroundOpacity, 0, 50, defaults._reactionBackgroundOpacity);
	validateRange(_wideMultiplier, 0.5, 4.0, defaults._wideMultiplier);
	validateRange(_avatarCorners, 0, AyuUiSettings::kMaxAvatarCorners, defaults._avatarCorners);
	validateRange(_iconSet, 0, AyuUiSettings::kIconSetCount - 1, defaults._iconSet);
	validateRange(_designStyle, 0, AyuUiSettings::kDesignStyleCount - 1, defaults._designStyle);

	const auto embeddedType = _messageShotSettings._embeddedThemeType.current();
	auto embeddedTypeValid = (embeddedType == -1) || (embeddedType >= 0 && embeddedType <= 3); // from Window::Theme::EmbeddedType::DayBlue to Window::Theme::EmbeddedType::NightGreen
	if (!embeddedTypeValid) {
		_messageShotSettings._embeddedThemeType = defaults._messageShotSettings._embeddedThemeType.current();
		_messageShotSettings._embeddedThemeAccentColor = defaults._messageShotSettings._embeddedThemeAccentColor.current();
		modified = true;
	}

	if (modified) {
		save();
	}
}

void AyuSettings::setSaveDeletedMessages(bool val) {
	if (_saveDeletedMessages.current() == val) return;
	_saveDeletedMessages = val;
	save();
}

void AyuSettings::setSaveLocallyDeletedMessages(bool val) {
	if (_saveLocallyDeletedMessages.current() == val) return;
	_saveLocallyDeletedMessages = val;
	save();
}

void AyuSettings::setSecretChatsEnabled(bool val) {
	if (_secretChatsEnabled.current() == val) return;
	_secretChatsEnabled = val;
	save();
}

void AyuSettings::setSaveEditedText(bool val) {
	if (_saveEditedText.current() == val) return;
	_saveEditedText = val;
	save();
}

void AyuSettings::setSaveEditedMedia(bool val) {
	if (_saveEditedMedia.current() == val) return;
	_saveEditedMedia = val;
	save();
}

void AyuSettings::setSaveEditedMarkup(bool val) {
	if (_saveEditedMarkup.current() == val) return;
	_saveEditedMarkup = val;
	save();
}

void AyuSettings::setSaveEditedPrivate(bool val) {
	if (_saveEditedPrivate.current() == val) return;
	_saveEditedPrivate = val;
	save();
}

void AyuSettings::setSaveEditedGroups(bool val) {
	if (_saveEditedGroups.current() == val) return;
	_saveEditedGroups = val;
	save();
}

void AyuSettings::setSaveEditedChannels(bool val) {
	if (_saveEditedChannels.current() == val) return;
	_saveEditedChannels = val;
	save();
}

void AyuSettings::setSaveEditedOwn(bool val) {
	if (_saveEditedOwn.current() == val) return;
	_saveEditedOwn = val;
	save();
}

void AyuSettings::setSaveMessagesHistory(bool val) {
	if (_saveMessagesHistory.current() == val) return;
	_saveMessagesHistory = val;
	save();
}

void AyuSettings::setSaveForBots(bool val) {
	if (_saveForBots.current() == val) return;
	_saveForBots = val;
	save();
}

void AyuSettings::setFiltersEnabled(bool val) {
	if (_filtersEnabled.current() == val) return;
	_filtersEnabled = val;
	save();
}

void AyuSettings::setFiltersEnabledInChats(bool val) {
	if (_filtersEnabledInChats.current() == val) return;
	_filtersEnabledInChats = val;
	save();
}

void AyuSettings::setHideFromBlocked(bool val) {
	if (_hideFromBlocked.current() == val) return;
	_hideFromBlocked = val;
	save();
}

void AyuSettings::setSemiTransparentDeletedMessages(bool val) {
	if (_semiTransparentDeletedMessages.current() == val) return;
	_semiTransparentDeletedMessages = val;
	save();
}

void AyuSettings::setInvertSwipeGestures(bool val) {
	if (_invertSwipeGestures.current() == val) return;
	_invertSwipeGestures = val;
	save();
}

void AyuSettings::setDisableAds(bool val) {
	if (_disableAds.current() == val) return;
	_disableAds = val;
	save();
}

void AyuSettings::setDisableStories(bool val) {
	if (_disableStories.current() == val) return;
	_disableStories = val;
	save();
}

void AyuSettings::setDisableCustomBackgrounds(bool val) {
	if (_disableCustomBackgrounds.current() == val) return;
	_disableCustomBackgrounds = val;
	save();
}

void AyuSettings::setHidePremiumStatuses(bool val) {
	if (_hidePremiumStatuses.current() == val) return;
	_hidePremiumStatuses = val;
	save();
}

void AyuSettings::setShowOnlyAddedEmojisAndStickers(bool val) {
	if (_showOnlyAddedEmojisAndStickers.current() == val) return;
	_showOnlyAddedEmojisAndStickers = val;
	save();
}

void AyuSettings::setCollapseSimilarChannels(bool val) {
	if (_collapseSimilarChannels.current() == val) return;
	_collapseSimilarChannels = val;
	save();
}

void AyuSettings::setHideSimilarChannels(bool val) {
	if (_hideSimilarChannels.current() == val) return;
	_hideSimilarChannels = val;
	save();
}

void AyuSettings::setMessageBubbleRadius(int val) {
	if (_messageBubbleRadius.current() == val) return;
	_messageBubbleRadius = val;
	save();
}

void AyuSettings::setWideMultiplier(double val) {
	if (_wideMultiplier.current() == val) return;
	_wideMultiplier = val;
	// doesn't work because it should be set before style::StartManager()
	// AyuUiSettings::setWideMultiplier(val);
	// repaintApp();
	save();
}

void AyuSettings::setSpoofWebviewAsAndroid(bool val) {
	if (_spoofWebviewAsAndroid.current() == val) return;
	_spoofWebviewAsAndroid = val;
	save();
}

void AyuSettings::setDisableOpenLinkWarning(bool val) {
	if (_disableOpenLinkWarning.current() == val) return;
	_disableOpenLinkWarning = val;
	save();
}

void AyuSettings::setIncreaseWebviewHeight(bool val) {
	if (_increaseWebviewHeight.current() == val) return;
	_increaseWebviewHeight = val;
	save();
}

void AyuSettings::setIncreaseWebviewWidth(bool val) {
	if (_increaseWebviewWidth.current() == val) return;
	_increaseWebviewWidth = val;
	save();
}

void AyuSettings::setMaterialSwitches(bool val) {
	if (_materialSwitches.current() == val) return;
	_materialSwitches = val;
	AyuUiSettings::setMaterialSwitches(val);
	repaintApp();
	save();
}

void AyuSettings::setMaterialBubbles(bool val) {
	if (_materialBubbles.current() == val) return;
	_materialBubbles = val;
	repaintApp();
	save();
}

void AyuSettings::setIconSet(int val) {
	if (_iconSet.current() == val) return;
	_iconSet = val;
	save();
}

void AyuSettings::setWhisperModel(const QString &val) {
	if (_whisperModel.current() == val) return;
	_whisperModel = val;
	save();
}

void AyuSettings::setOcrLanguages(const QString &val) {
	if (_ocrLanguages.current() == val) return;
	_ocrLanguages = val;
	save();
}

void AyuSettings::setDesignStyle(int val) {
	if (_designStyle.current() == val) return;
	_designStyle = val;
	save();
}

void AyuSettings::setMaxEditRevisions(int val) {
	if (_maxEditRevisions.current() == val) return;
	_maxEditRevisions = val;
	save();
}

void AyuSettings::setDeletedRestoreLimit(int val) {
	if (_deletedRestoreLimit.current() == val) return;
	_deletedRestoreLimit = val;
	save();
}

void AyuSettings::setKeptSnapshotLimit(int val) {
	if (_keptSnapshotLimit.current() == val) return;
	_keptSnapshotLimit = val;
	save();
}

void AyuSettings::setDeletedMediaMaxSizeMb(int val) {
	if (_deletedMediaMaxSizeMb.current() == val) return;
	_deletedMediaMaxSizeMb = val;
	save();
}

void AyuSettings::setKeepDeletedDays(int val) {
	if (_keepDeletedDays.current() == val) return;
	_keepDeletedDays = val;
	AyuDatabase::purgeOlderThan(val);
	save();
}

void AyuSettings::setDeleteSavedFiles(bool val) {
	if (_deleteSavedFiles.current() == val) return;
	_deleteSavedFiles = val;
	FreshDatabaseBackup::setKeepRichest(!val);
	save();
}

void AyuSettings::setBackupKeepCount(int val) {
	if (_backupKeepCount.current() == val) return;
	_backupKeepCount = val;
	FreshDatabaseBackup::configure(_backupKeepCount.current(), _backupIntervalHours.current());
	save();
}

void AyuSettings::setBackupIntervalHours(int val) {
	if (_backupIntervalHours.current() == val) return;
	_backupIntervalHours = val;
	FreshDatabaseBackup::configure(_backupKeepCount.current(), _backupIntervalHours.current());
	save();
}

void AyuSettings::setReplyBackgroundOpacity(int val) {
	if (_replyBackgroundOpacity.current() == val) return;
	_replyBackgroundOpacity = val;
	style::NotifyPaletteChanged();
	save();
}

void AyuSettings::setReactionBackgroundOpacity(int val) {
	if (_reactionBackgroundOpacity.current() == val) return;
	_reactionBackgroundOpacity = val;
	repaintApp();
	save();
}

void AyuSettings::setDisableNotificationsDelay(bool val) {
	if (_disableNotificationsDelay.current() == val) return;
	_disableNotificationsDelay = val;
	save();
}

void AyuSettings::setLocalPremium(bool val) {
	if (_localPremium.current() == val) return;
	_localPremium = val;
	save();
}

void AyuSettings::setShowChannelReactions(bool val) {
	if (_showChannelReactions.current() == val) return;
	_showChannelReactions = val;
	save();
}

void AyuSettings::setShowGroupReactions(bool val) {
	if (_showGroupReactions.current() == val) return;
	_showGroupReactions = val;
	save();
}

void AyuSettings::setShowPrivateChatReactions(bool val) {
	if (_showPrivateChatReactions.current() == val) return;
	_showPrivateChatReactions = val;
	save();
}

void AyuSettings::setAppIcon(const QString &val) {
	if (_appIcon.current() == val) return;
	_appIcon = val;
	save();
}

void AyuSettings::setSimpleQuotesAndReplies(bool val) {
	if (_simpleQuotesAndReplies.current() == val) return;
	_simpleQuotesAndReplies = val;
	save();
}

void AyuSettings::setHideFastShare(bool val) {
	if (_hideFastShare.current() == val) return;
	_hideFastShare = val;
	save();
}

void AyuSettings::setReplaceBottomInfoWithIcons(bool val) {
	if (_replaceBottomInfoWithIcons.current() == val) return;
	_replaceBottomInfoWithIcons = val;
	save();
}

void AyuSettings::setDeletedMark(const QString &val) {
	if (_deletedMark.current() == val) return;
	_deletedMark = val;
	save();
}

void AyuSettings::setEditedMark(const QString &val) {
	if (_editedMark.current() == val) return;
	_editedMark = val;
	save();
}

void AyuSettings::setUnlimitedRecentStickers(bool val) {
	if (_unlimitedRecentStickers.current() == val) return;
	_unlimitedRecentStickers = val;
	save();
}

void AyuSettings::setShowReactionsPanelInContextMenu(ContextMenuVisibility val) {
	if (_showReactionsPanelInContextMenu.current() == val) return;
	_showReactionsPanelInContextMenu = val;
	save();
}

void AyuSettings::setShowViewsPanelInContextMenu(ContextMenuVisibility val) {
	if (_showViewsPanelInContextMenu.current() == val) return;
	_showViewsPanelInContextMenu = val;
	save();
}

void AyuSettings::setShowHideMessageInContextMenu(ContextMenuVisibility val) {
	if (_showHideMessageInContextMenu.current() == val) return;
	_showHideMessageInContextMenu = val;
	save();
}

void AyuSettings::setShowUserMessagesInContextMenu(ContextMenuVisibility val) {
	if (_showUserMessagesInContextMenu.current() == val) return;
	_showUserMessagesInContextMenu = val;
	save();
}

void AyuSettings::setShowMessageDetailsInContextMenu(ContextMenuVisibility val) {
	if (_showMessageDetailsInContextMenu.current() == val) return;
	_showMessageDetailsInContextMenu = val;
	save();
}

void AyuSettings::setShowRepeatMessageInContextMenu(ContextMenuVisibility val) {
	if (_showRepeatMessageInContextMenu.current() == val) return;
	_showRepeatMessageInContextMenu = val;
	save();
}

void AyuSettings::setShowAddFilterInContextMenu(ContextMenuVisibility val) {
	if (_showAddFilterInContextMenu.current() == val) return;
	_showAddFilterInContextMenu = val;
	save();
}

void AyuSettings::setShowAttachButtonInMessageField(bool val) {
	if (_showAttachButtonInMessageField.current() == val) return;
	_showAttachButtonInMessageField = val;
	save();
}

void AyuSettings::setShowCommandsButtonInMessageField(bool val) {
	if (_showCommandsButtonInMessageField.current() == val) return;
	_showCommandsButtonInMessageField = val;
	save();
}

void AyuSettings::setShowEmojiButtonInMessageField(bool val) {
	if (_showEmojiButtonInMessageField.current() == val) return;
	_showEmojiButtonInMessageField = val;
	save();
}

void AyuSettings::setShowMicrophoneButtonInMessageField(bool val) {
	if (_showMicrophoneButtonInMessageField.current() == val) return;
	_showMicrophoneButtonInMessageField = val;
	save();
}

void AyuSettings::setShowAutoDeleteButtonInMessageField(bool val) {
	if (_showAutoDeleteButtonInMessageField.current() == val) return;
	_showAutoDeleteButtonInMessageField = val;
	save();
}

void AyuSettings::setShowGiftButtonInMessageField(bool val) {
	if (_showGiftButtonInMessageField.current() == val) return;
	_showGiftButtonInMessageField = val;
	save();
}

void AyuSettings::setShowTranslateButtonInMessageField(bool val) {
	if (_showTranslateButtonInMessageField.current() == val) return;
	_showTranslateButtonInMessageField = val;
	save();
}

void AyuSettings::setTranslateComposeTo(const QString &val) {
	if (_translateComposeTo.current() == val) return;
	_translateComposeTo = val;
	save();
}

void AyuSettings::setAutoTranslateOutgoing(bool val) {
	if (_autoTranslateOutgoing.current() == val) return;
	_autoTranslateOutgoing = val;
	save();
}

void AyuSettings::setShowAiEditorButtonInMessageField(bool val) {
	if (_showAiEditorButtonInMessageField.current() == val) return;
	_showAiEditorButtonInMessageField = val;
	save();
}

void AyuSettings::setShowAttachPopup(bool val) {
	if (_showAttachPopup.current() == val) return;
	_showAttachPopup = val;
	save();
}

void AyuSettings::setShowEmojiPopup(bool val) {
	if (_showEmojiPopup.current() == val) return;
	_showEmojiPopup = val;
	save();
}

void AyuSettings::setShowMyProfileInDrawer(bool val) {
	if (_showMyProfileInDrawer.current() == val) return;
	_showMyProfileInDrawer = val;
	save();
}

void AyuSettings::setShowBotsInDrawer(bool val) {
	if (_showBotsInDrawer.current() == val) return;
	_showBotsInDrawer = val;
	save();
}

void AyuSettings::setShowNewGroupInDrawer(bool val) {
	if (_showNewGroupInDrawer.current() == val) return;
	_showNewGroupInDrawer = val;
	save();
}

void AyuSettings::setShowNewStoryInDrawer(bool val) {
	if (_showNewStoryInDrawer.current() == val) return;
	_showNewStoryInDrawer = val;
	save();
}

void AyuSettings::setPasscodeWipeAttempts(int val) {
	if (_passcodeWipeAttempts.current() == val) return;
	_passcodeWipeAttempts = val;
	save();
}

void AyuSettings::setPasscodeWipeAction(int val) {
	if (_passcodeWipeAction.current() == val) return;
	_passcodeWipeAction = val;
	save();
}

void AyuSettings::setLockScreenShowClock(bool val) {
	if (_lockScreenShowClock.current() == val) return;
	_lockScreenShowClock = val;
	save();
}

void AyuSettings::setLockScreenStyle(int val) {
	if (_lockScreenStyle.current() == val) return;
	_lockScreenStyle = val;
	save();
}

void AyuSettings::setLockScreenBackground(int val) {
	if (_lockScreenBackground.current() == val) return;
	_lockScreenBackground = val;
	save();
}

void AyuSettings::setLockScreenBlur(bool val) {
	if (_lockScreenBlur.current() == val) return;
	_lockScreenBlur = val;
	save();
}

void AyuSettings::setLockScreenDim(int val) {
	if (_lockScreenDim.current() == val) return;
	_lockScreenDim = val;
	save();
}

void AyuSettings::setLockScreenShowDate(bool val) {
	if (_lockScreenShowDate.current() == val) return;
	_lockScreenShowDate = val;
	save();
}

QString AyuSettings::sanitizeLockScreenLine(QString text, int maxLength) {
	text.replace(QChar('\r'), QChar(' '));
	text.replace(QChar('\n'), QChar(' '));
	for (auto &ch : text) {
		if (ch.category() == QChar::Other_Control) {
			ch = QChar(' ');
		}
	}
	return text.trimmed().left(maxLength);
}

QString AyuSettings::sanitizeLockScreenText(QString text) {
	constexpr auto kMaxLength = 120;
	constexpr auto kMaxLines = 3;
	text.replace(u"\r\n"_q, u"\n"_q);
	text.replace(QChar('\r'), QChar('\n'));
	auto lines = text.split(QChar('\n'));
	for (auto &line : lines) {
		line = line.trimmed();
	}
	while (!lines.isEmpty() && lines.back().isEmpty()) {
		lines.pop_back();
	}
	while (!lines.isEmpty() && lines.front().isEmpty()) {
		lines.pop_front();
	}
	if (lines.size() > kMaxLines) {
		lines = lines.mid(0, kMaxLines);
	}
	auto result = lines.join(QChar('\n'));
	for (auto &ch : result) {
		if (ch != QChar('\n') && (ch.category() == QChar::Other_Control)) {
			ch = QChar(' ');
		}
	}
	return result.left(kMaxLength);
}

void AyuSettings::setLockScreenText(QString val) {
	val = sanitizeLockScreenText(std::move(val));
	if (_lockScreenText.current() == val) return;
	_lockScreenText = val;
	save();
}

void AyuSettings::setLockScreenShowLogout(bool val) {
	if (_lockScreenShowLogout.current() == val) return;
	_lockScreenShowLogout = val;
	save();
}

void AyuSettings::setLockScreenShowEye(bool val) {
	if (_lockScreenShowEye.current() == val) return;
	_lockScreenShowEye = val;
	save();
}

void AyuSettings::setLockScreenColor1(uint32 val) {
	if (_lockScreenColor1.current() == val) return;
	_lockScreenColor1 = val;
	save();
}

void AyuSettings::setLockScreenColor2(uint32 val) {
	if (_lockScreenColor2.current() == val) return;
	_lockScreenColor2 = val;
	save();
}

void AyuSettings::setLockScreenAvatar(bool val) {
	if (_lockScreenAvatar.current() == val) return;
	_lockScreenAvatar = val;
	save();
}

void AyuSettings::setLockScreenAvatarShape(int val) {
	if (_lockScreenAvatarShape.current() == val) return;
	_lockScreenAvatarShape = val;
	save();
}

void AyuSettings::setLockScreenCardOpacity(int val) {
	if (_lockScreenCardOpacity.current() == val) return;
	_lockScreenCardOpacity = val;
	save();
}

void AyuSettings::setLockScreenCardRadius(int val) {
	if (_lockScreenCardRadius.current() == val) return;
	_lockScreenCardRadius = val;
	save();
}

void AyuSettings::setLockScreenCardBorder(bool val) {
	if (_lockScreenCardBorder.current() == val) return;
	_lockScreenCardBorder = val;
	save();
}

void AyuSettings::setLockScreenClockFormat(int val) {
	if (_lockScreenClockFormat.current() == val) return;
	_lockScreenClockFormat = val;
	save();
}

void AyuSettings::setLockScreenClockSeconds(bool val) {
	if (_lockScreenClockSeconds.current() == val) return;
	_lockScreenClockSeconds = val;
	save();
}

void AyuSettings::setLockScreenClockSize(int val) {
	if (_lockScreenClockSize.current() == val) return;
	_lockScreenClockSize = val;
	save();
}

void AyuSettings::setLockScreenClockColor(uint32 val) {
	if (_lockScreenClockColor.current() == val) return;
	_lockScreenClockColor = val;
	save();
}

void AyuSettings::setLockScreenTitle(QString val) {
	val = sanitizeLockScreenLine(std::move(val), 40);
	if (_lockScreenTitle.current() == val) return;
	_lockScreenTitle = val;
	save();
}

void AyuSettings::setLockScreenSubmitText(QString val) {
	val = sanitizeLockScreenLine(std::move(val), 24);
	if (_lockScreenSubmitText.current() == val) return;
	_lockScreenSubmitText = val;
	save();
}

void AyuSettings::setSendClearMetadata(bool val) {
	if (_sendClearMetadata.current() == val) return;
	_sendClearMetadata = val;
	save();
}

void AyuSettings::setLockScreenAccent(uint32 val) {
	if (_lockScreenAccent.current() == val) return;
	_lockScreenAccent = val;
	save();
}

void AyuSettings::setLockOnSystemLock(bool val) {
	if (_lockOnSystemLock.current() == val) return;
	_lockOnSystemLock = val;
	save();
}

void AyuSettings::setLockOnMinimize(int val) {
	if (_lockOnMinimize.current() == val) return;
	_lockOnMinimize = val;
	save();
}

void AyuSettings::setHideFromCapture(int val) {
	if (_hideFromCapture.current() == val) return;
	_hideFromCapture = val;
	save();
}

void AyuSettings::setLockedNotifications(int val) {
	if (_lockedNotifications.current() == val) return;
	_lockedNotifications = val;
	save();
}

void AyuSettings::setShowNewChannelInDrawer(bool val) {
	if (_showNewChannelInDrawer.current() == val) return;
	_showNewChannelInDrawer = val;
	save();
}

void AyuSettings::setShowContactsInDrawer(bool val) {
	if (_showContactsInDrawer.current() == val) return;
	_showContactsInDrawer = val;
	save();
}

void AyuSettings::setShowCallsInDrawer(bool val) {
	if (_showCallsInDrawer.current() == val) return;
	_showCallsInDrawer = val;
	save();
}

void AyuSettings::setShowSavedMessagesInDrawer(bool val) {
	if (_showSavedMessagesInDrawer.current() == val) return;
	_showSavedMessagesInDrawer = val;
	save();
}

void AyuSettings::setShowLReadToggleInDrawer(bool val) {
	if (_showLReadToggleInDrawer.current() == val) return;
	_showLReadToggleInDrawer = val;
	save();
}

void AyuSettings::setShowSReadToggleInDrawer(bool val) {
	if (_showSReadToggleInDrawer.current() == val) return;
	_showSReadToggleInDrawer = val;
	save();
}

void AyuSettings::setShowNightModeToggleInDrawer(bool val) {
	if (_showNightModeToggleInDrawer.current() == val) return;
	_showNightModeToggleInDrawer = val;
	save();
}

void AyuSettings::setShowGhostToggleInDrawer(bool val) {
	if (_showGhostToggleInDrawer.current() == val) return;
	_showGhostToggleInDrawer = val;
	save();
}

void AyuSettings::setShowStreamerToggleInDrawer(bool val) {
	if (_showStreamerToggleInDrawer.current() == val) return;
	_showStreamerToggleInDrawer = val;
	save();
}

void AyuSettings::setShowGhostToggleInTray(bool val) {
	if (_showGhostToggleInTray.current() == val) return;
	_showGhostToggleInTray = val;
	save();
}

void AyuSettings::setShowStreamerToggleInTray(bool val) {
	if (_showStreamerToggleInTray.current() == val) return;
	_showStreamerToggleInTray = val;
	save();
}

void AyuSettings::setMonoFont(const QString &val) {
	if (_monoFont.current() == val) return;
	_monoFont = val;
	// doesn't work because `static const auto family = ...`
	// AyuUiSettings::setMonoFont(val);
	// repaintApp();
	save();
}

void AyuSettings::setHideNotificationCounters(bool val) {
	if (_hideNotificationCounters.current() == val) return;
	_hideNotificationCounters = val;
	save();
}

void AyuSettings::setHideNotificationBadge(bool val) {
	if (_hideNotificationBadge.current() == val) return;
	_hideNotificationBadge = val;
	Core::App().refreshApplicationIcon();
	Core::App().tray().updateIconCounters();
	Core::App().domain().notifyUnreadBadgeChanged();
	save();
}

void AyuSettings::setHideBirthdayNotifications(bool val) {
	if (_hideBirthdayNotifications.current() == val) return;
	_hideBirthdayNotifications = val;
	save();
}

void AyuSettings::setHideAllChatsFolder(bool val) {
	if (_hideAllChatsFolder.current() == val) return;
	_hideAllChatsFolder = val;
	save();
}

void AyuSettings::setChannelBottomButton(ChannelBottomButton val) {
	if (_channelBottomButton.current() == val) return;
	_channelBottomButton = val;
	save();
}

void AyuSettings::setQuickAdminShortcuts(bool val) {
	if (_quickAdminShortcuts.current() == val) return;
	_quickAdminShortcuts = val;
	save();
}

void AyuSettings::setDisableGreetingSticker(bool val) {
	if (_disableGreetingSticker.current() == val) return;
	_disableGreetingSticker = val;
	save();
}

void AyuSettings::setShowPeerId(PeerIdDisplay val) {
	if (_showPeerId.current() == val) return;
	_showPeerId = val;
	save();
}

void AyuSettings::setKeepRemovedUserChats(bool val) {
	if (_keepRemovedUserChats.current() == val) return;
	_keepRemovedUserChats = val;
	save();
}

void AyuSettings::setKeepRemovedGroups(bool val) {
	if (_keepRemovedGroups.current() == val) return;
	_keepRemovedGroups = val;
	save();
}

void AyuSettings::setKeepDeletedTopics(bool val) {
	if (_keepDeletedTopics.current() == val) return;
	_keepDeletedTopics = val;
	save();
	if (!val) {
		FreshKept::releaseDeletedTopics();
	}
}

void AyuSettings::setKeepRemovedForums(bool val) {
	if (_keepRemovedForums.current() == val) return;
	_keepRemovedForums = val;
	save();
}

void AyuSettings::setKeepRemovedChannels(bool val) {
	if (_keepRemovedChannels.current() == val) return;
	_keepRemovedChannels = val;
	save();
}

void AyuSettings::setRestoreDeletedInChats(bool val) {
	if (_restoreDeletedInChats.current() == val) return;
	_restoreDeletedInChats = val;
	save();
}

void AyuSettings::setShowDeletedChatIcon(bool val) {
	if (_showDeletedChatIcon.current() == val) return;
	_showDeletedChatIcon = val;
	save();
}

void AyuSettings::setLocalPinnedChats(bool val) {
	if (_localPinnedChats.current() == val) return;
	_localPinnedChats = val;
	save();
}

void AyuSettings::setKeepDraftsLocal(bool val) {
	if (_keepDraftsLocal.current() == val) return;
	_keepDraftsLocal = val;
	save();
}

void AyuSettings::setStashHintShown(int val) {
	if (_stashHintShown.current() == val) return;
	_stashHintShown = val;
	save();
}

void AyuSettings::setStashHintUsed(bool val) {
	if (_stashHintUsed.current() == val) return;
	_stashHintUsed = val;
	save();
}

void AyuSettings::setReduceNetworkTracking(bool val) {
	if (_reduceNetworkTracking.current() == val) return;
	_reduceNetworkTracking = val;
	FreshNetworkPrivacy::SetEnabled(val);
	save();
}

void AyuSettings::setEnableDebugLogging(bool val) {
	if (_enableDebugLogging.current() == val) return;
	_enableDebugLogging = val;
	FreshDebugLogging::SetEnabled(val);
	const auto directory = cWorkingDir() + u"tdata"_q;
	if (val) {
		FreshCrashTrace::Install(directory);
	} else {
		Logs::removeFiles();
		FreshCrashTrace::Remove(directory);
		QFile::remove(directory + u"/fresh_db_report.txt"_q);
	}
	save();
}

void AyuSettings::setMarkOldMessagesDeleted(bool val) {
	if (_markOldMessagesDeleted.current() == val) return;
	_markOldMessagesDeleted = val;
	save();
}

void AyuSettings::setShowTimeZone(bool val) {
	if (_showTimeZone.current() == val) return;
	_showTimeZone = val;
	save();
}

void AyuSettings::setShowMessageSeconds(bool val) {
	if (_showMessageSeconds.current() == val) return;
	_showMessageSeconds = val;
	save();
}

void AyuSettings::setShowMessageShot(bool val) {
	if (_showMessageShot.current() == val) return;
	_showMessageShot = val;
	save();
}

void AyuSettings::setFilterZalgo(bool val) {
	if (_filterZalgo.current() == val) return;
	_filterZalgo = val;
	save();
}

void AyuSettings::setStickerConfirmation(bool val) {
	if (_stickerConfirmation.current() == val) return;
	_stickerConfirmation = val;
	save();
}

void AyuSettings::setGifConfirmation(bool val) {
	if (_gifConfirmation.current() == val) return;
	_gifConfirmation = val;
	save();
}

void AyuSettings::setVoiceConfirmation(bool val) {
	if (_voiceConfirmation.current() == val) return;
	_voiceConfirmation = val;
	save();
}

void AyuSettings::setRoundConfirmation(bool val) {
	if (_roundConfirmation.current() == val) return;
	_roundConfirmation = val;
	save();
}

void AyuSettings::setTranscriptionProvider(TranscriptionProvider val) {
	if (_transcriptionProvider.current() == val) return;
	_transcriptionProvider = val;
	save();
}

void AyuSettings::setTranslationProvider(TranslationProvider val) {
	if (((val == TranslationProvider::Native)
			&& !Platform::IsTranslateProviderAvailable())
		|| ((val == TranslationProvider::Local)
			&& !FreshRecognition::TextTranslationAvailable())) {
		val = TranslationProvider::Telegram;
	}
	if (_translationProvider.current() == val) return;
	_translationProvider = val;
	if (const auto manager = Ayu::Translator::TranslateManager::currentInstance()) {
		manager->resetCache();
	}
	save();
}

void AyuSettings::setAdaptiveCoverColor(bool val) {
	if (_adaptiveCoverColor.current() == val) return;
	_adaptiveCoverColor = val;
	save();
}

void AyuSettings::setImproveLinkPreviews(bool val) {
	if (_improveLinkPreviews.current() == val) return;
	_improveLinkPreviews = val;
	save();
}

void AyuSettings::setCrashReporting(bool val) {
	if (_crashReporting.current() == val) return;
	_crashReporting = val;
	save();
}

void AyuSettings::setPluginsEnabled(bool val) {
	if (_pluginsEnabled.current() == val) return;
	_pluginsEnabled = val;
	save();
}

void AyuSettings::setPluginsRuntimeAutoUpdate(bool val) {
	if (_pluginsRuntimeAutoUpdate.current() == val) return;
	_pluginsRuntimeAutoUpdate = val;
	save();
}

void AyuSettings::setAvatarCorners(int val) {
	if (_avatarCorners.current() == val) return;
	_avatarCorners = val;
	AyuUiSettings::setAvatarCorners(val);
	save();
}

void AyuSettings::setSingleCornerRadius(bool val) {
	if (_singleCornerRadius.current() == val) return;
	_singleCornerRadius = val;
	repaintApp();
	save();
}

void AyuSettings::setStreamerMode(bool val) {
	if (_streamerMode.current() == val) return;
	_streamerMode = val;
	AyuFeatures::StreamerMode::apply(val);
	AyuFeatures::StreamerMode::refresh();
	repaintApp();
	save();
}

void AyuSettings::setStreamerHideWholeWindow(bool val) {
	if (_streamerHideWholeWindow.current() == val) return;
	_streamerHideWholeWindow = val;
	AyuFeatures::StreamerMode::apply(_streamerMode.current());
	save();
}

void AyuSettings::setStreamerSpoilers(bool val) {
	if (_streamerSpoilers.current() == val) return;
	_streamerSpoilers = val;
	AyuFeatures::StreamerMode::refresh();
	repaintApp();
	save();
}

void to_json(nlohmann::json &j, const AyuSettings &s) {
	auto ghostAccounts = nlohmann::json::object();
	for (const auto &[key, value] : s._ghostAccounts) {
		ghostAccounts[std::to_string(key)] = *value;
	}

	j = nlohmann::json{
		{"ghostModeSettings", ghostAccounts},
		{"useGlobalGhostMode", s._useGlobalGhostMode.current()},
		{"saveDeletedMessages", s._saveDeletedMessages.current()},
		{"saveLocallyDeletedMessages", s._saveLocallyDeletedMessages.current()},
		{"secretChatsEnabled", s._secretChatsEnabled.current()},
		{"saveMessagesHistory", s._saveMessagesHistory.current()},
		{"saveEditedText", s._saveEditedText.current()},
		{"saveEditedMedia", s._saveEditedMedia.current()},
		{"saveEditedMarkup", s._saveEditedMarkup.current()},
		{"saveEditedPrivate", s._saveEditedPrivate.current()},
		{"saveEditedGroups", s._saveEditedGroups.current()},
		{"saveEditedChannels", s._saveEditedChannels.current()},
		{"saveEditedOwn", s._saveEditedOwn.current()},
		{"saveForBots", s._saveForBots.current()},
		{"shadowBanIds", s._shadowBanIds},
		{"filtersEnabled", s._filtersEnabled.current()},
		{"filtersEnabledInChats", s._filtersEnabledInChats.current()},
		{"hideFromBlocked", s._hideFromBlocked.current()},
		{"semiTransparentDeletedMessages", s._semiTransparentDeletedMessages.current()},
		{"disableAds", s._disableAds.current()},
		{"invertSwipeGestures", s._invertSwipeGestures.current()},
		{"disableStories", s._disableStories.current()},
		{"disableCustomBackgrounds", s._disableCustomBackgrounds.current()},
		{"hidePremiumStatuses", s._hidePremiumStatuses.current()},
		{"showOnlyAddedEmojisAndStickers", s._showOnlyAddedEmojisAndStickers.current()},
		{"collapseSimilarChannels", s._collapseSimilarChannels.current()},
		{"hideSimilarChannels", s._hideSimilarChannels.current()},
		{"messageBubbleRadius", s._messageBubbleRadius.current()},
		{"disableOpenLinkWarning", s._disableOpenLinkWarning.current()},
		{"wideMultiplier", s._wideMultiplier.current()},
		{"spoofWebviewAsAndroid", s._spoofWebviewAsAndroid.current()},
		{"increaseWebviewHeight", s._increaseWebviewHeight.current()},
		{"increaseWebviewWidth", s._increaseWebviewWidth.current()},
		{"materialSwitches", s._materialSwitches.current()},
		{"materialBubbles", s._materialBubbles.current()},
		{"iconSet", s._iconSet.current()},
		{"designStyle", s._designStyle.current()},
		{"whisperModel", s._whisperModel.current()},
		{"ocrLanguages", s._ocrLanguages.current()},
		{"maxEditRevisions", s._maxEditRevisions.current()},
		{"deletedRestoreLimit", s._deletedRestoreLimit.current()},
		{"keptSnapshotLimit", s._keptSnapshotLimit.current()},
		{"deletedMediaMaxSizeMb", s._deletedMediaMaxSizeMb.current()},
		{"keepDeletedDays", s._keepDeletedDays.current()},
		{"deleteSavedFiles", s._deleteSavedFiles.current()},
		{"backupKeepCount", s._backupKeepCount.current()},
		{"backupIntervalHours", s._backupIntervalHours.current()},
		{"replyBackgroundOpacity", s._replyBackgroundOpacity.current()},
		{"reactionBackgroundOpacity", s._reactionBackgroundOpacity.current()},
		{"disableNotificationsDelay", s._disableNotificationsDelay.current()},
		{"localPremium", s._localPremium.current()},
		{"showChannelReactions", s._showChannelReactions.current()},
		{"showGroupReactions", s._showGroupReactions.current()},
		{"showPrivateChatReactions", s._showPrivateChatReactions.current()},
		{"appIcon", s._appIcon.current()},
		{"simpleQuotesAndReplies", s._simpleQuotesAndReplies.current()},
		{"hideFastShare", s._hideFastShare.current()},
		{"replaceBottomInfoWithIcons", s._replaceBottomInfoWithIcons.current()},
		{"deletedMark", s._deletedMark.current()},
		{"editedMark", s._editedMark.current()},
		{"unlimitedRecentStickers", s._unlimitedRecentStickers.current()},
		{"showReactionsPanelInContextMenu", s._showReactionsPanelInContextMenu.current()},
		{"showViewsPanelInContextMenu", s._showViewsPanelInContextMenu.current()},
		{"showHideMessageInContextMenu", s._showHideMessageInContextMenu.current()},
		{"showUserMessagesInContextMenu", s._showUserMessagesInContextMenu.current()},
		{"showMessageDetailsInContextMenu", s._showMessageDetailsInContextMenu.current()},
		{"showRepeatMessageInContextMenu", s._showRepeatMessageInContextMenu.current()},
		{"showAddFilterInContextMenu", s._showAddFilterInContextMenu.current()},
		{"showAttachButtonInMessageField", s._showAttachButtonInMessageField.current()},
		{"showCommandsButtonInMessageField", s._showCommandsButtonInMessageField.current()},
		{"showEmojiButtonInMessageField", s._showEmojiButtonInMessageField.current()},
		{"showMicrophoneButtonInMessageField", s._showMicrophoneButtonInMessageField.current()},
		{"showAutoDeleteButtonInMessageField", s._showAutoDeleteButtonInMessageField.current()},
		{"showGiftButtonInMessageField", s._showGiftButtonInMessageField.current()},
		{"showTranslateButtonInMessageField", s._showTranslateButtonInMessageField.current()},
		{"translateComposeTo", s._translateComposeTo.current()},
		{"autoTranslateOutgoing", s._autoTranslateOutgoing.current()},
		{"showAiEditorButtonInMessageField", s._showAiEditorButtonInMessageField.current()},
		{"showAttachPopup", s._showAttachPopup.current()},
		{"showEmojiPopup", s._showEmojiPopup.current()},
		{"showMyProfileInDrawer", s._showMyProfileInDrawer.current()},
		{"showBotsInDrawer", s._showBotsInDrawer.current()},
		{"showNewGroupInDrawer", s._showNewGroupInDrawer.current()},
		{"showNewChannelInDrawer", s._showNewChannelInDrawer.current()},
		{"showNewStoryInDrawer", s._showNewStoryInDrawer.current()},
		{"passcodeWipeAttempts", s._passcodeWipeAttempts.current()},
		{"passcodeWipeAction", s._passcodeWipeAction.current()},
		{"lockScreenShowClock", s._lockScreenShowClock.current()},
		{"lockScreenStyle", s._lockScreenStyle.current()},
		{"lockScreenBackground", s._lockScreenBackground.current()},
		{"lockScreenBlur", s._lockScreenBlur.current()},
		{"lockScreenDim", s._lockScreenDim.current()},
		{"lockScreenShowDate", s._lockScreenShowDate.current()},
		{"lockScreenText", s._lockScreenText.current()},
		{"lockScreenShowLogout", s._lockScreenShowLogout.current()},
		{"lockScreenShowEye", s._lockScreenShowEye.current()},
		{"lockScreenColor1", s._lockScreenColor1.current()},
		{"lockScreenColor2", s._lockScreenColor2.current()},
		{"lockScreenAvatar", s._lockScreenAvatar.current()},
		{"lockScreenAvatarShape", s._lockScreenAvatarShape.current()},
		{"lockScreenCardOpacity", s._lockScreenCardOpacity.current()},
		{"lockScreenCardRadius", s._lockScreenCardRadius.current()},
		{"lockScreenCardBorder", s._lockScreenCardBorder.current()},
		{"lockScreenClockFormat", s._lockScreenClockFormat.current()},
		{"lockScreenClockSeconds", s._lockScreenClockSeconds.current()},
		{"lockScreenClockSize", s._lockScreenClockSize.current()},
		{"lockScreenClockColor", s._lockScreenClockColor.current()},
		{"lockScreenTitle", s._lockScreenTitle.current()},
		{"lockScreenSubmitText", s._lockScreenSubmitText.current()},
		{"lockScreenAccent", s._lockScreenAccent.current()},
		{"sendClearMetadata", s._sendClearMetadata.current()},
		{"lockOnSystemLock", s._lockOnSystemLock.current()},
		{"lockOnMinimize", s._lockOnMinimize.current()},
		{"hideFromCapture", s._hideFromCapture.current()},
		{"lockedNotifications", s._lockedNotifications.current()},
		{"showContactsInDrawer", s._showContactsInDrawer.current()},
		{"showCallsInDrawer", s._showCallsInDrawer.current()},
		{"showSavedMessagesInDrawer", s._showSavedMessagesInDrawer.current()},
		{"showLReadToggleInDrawer", s._showLReadToggleInDrawer.current()},
		{"showSReadToggleInDrawer", s._showSReadToggleInDrawer.current()},
		{"showNightModeToggleInDrawer", s._showNightModeToggleInDrawer.current()},
		{"showGhostToggleInDrawer", s._showGhostToggleInDrawer.current()},
		{"showStreamerToggleInDrawer", s._showStreamerToggleInDrawer.current()},
		{"showGhostToggleInTray", s._showGhostToggleInTray.current()},
		{"showStreamerToggleInTray", s._showStreamerToggleInTray.current()},
		{"monoFont", s._monoFont.current()},
		{"hideNotificationCounters", s._hideNotificationCounters.current()},
		{"hideNotificationBadge", s._hideNotificationBadge.current()},
		{"hideAllChatsFolder", s._hideAllChatsFolder.current()},
		{"hideBirthdayNotifications", s._hideBirthdayNotifications.current()},
		{"channelBottomButton", s._channelBottomButton.current()},
		{"quickAdminShortcuts", s._quickAdminShortcuts.current()},
		{"disableGreetingSticker", s._disableGreetingSticker.current()},
		{"showPeerId", s._showPeerId.current()},
		{"showMessageSeconds", s._showMessageSeconds.current()},
		{"keepRemovedUserChats", s._keepRemovedUserChats.current()},
		{"keepRemovedGroups", s._keepRemovedGroups.current()},
		{"keepRemovedChannels", s._keepRemovedChannels.current()},
		{"keepRemovedForums", s._keepRemovedForums.current()},
		{"keepDeletedTopics", s._keepDeletedTopics.current()},
		{"restoreDeletedInChats", s._restoreDeletedInChats.current()},
		{"showDeletedChatIcon", s._showDeletedChatIcon.current()},
		{"localPinnedChats", s._localPinnedChats.current()},
		{"keepDraftsLocal", s._keepDraftsLocal.current()},
		{"stashHintShown", s._stashHintShown.current()},
		{"stashHintUsed", s._stashHintUsed.current()},
		{"reduceNetworkTracking", s._reduceNetworkTracking.current()},
		{"enableDebugLogging", s._enableDebugLogging.current()},
		{"markOldMessagesDeleted", s._markOldMessagesDeleted.current()},
		{"showTimeZone", s._showTimeZone.current()},
		{"showMessageShot", s._showMessageShot.current()},
		{"filterZalgo", s._filterZalgo.current()},
		{"stickerConfirmation", s._stickerConfirmation.current()},
		{"gifConfirmation", s._gifConfirmation.current()},
		{"voiceConfirmation", s._voiceConfirmation.current()},
		{"roundConfirmation", s._roundConfirmation.current()},
		{"translationProvider", s._translationProvider.current()},
		{"transcriptionProvider", s._transcriptionProvider.current()},
		{"adaptiveCoverColor", s._adaptiveCoverColor.current()},
		{"improveLinkPreviews", s._improveLinkPreviews.current()},
		{"crashReporting", s._crashReporting.current()},
		{"pluginsEnabled", s._pluginsEnabled.current()},
		{"pluginsRuntimeAutoUpdate", s._pluginsRuntimeAutoUpdate.current()},
		{"avatarCorners", s._avatarCorners.current()},
		{"singleCornerRadius", s._singleCornerRadius.current()},
		{"streamerMode", s._streamerMode.current()},
		{"streamerHideWholeWindow", s._streamerHideWholeWindow.current()},
		{"streamerSpoilers", s._streamerSpoilers.current()},
		{"messageShotSettings", s._messageShotSettings}
	};
}

void from_json(const nlohmann::json &j, AyuSettings &s) {
	AyuSettings defaults;

	if (j.contains("ghostModeSettings") && j["ghostModeSettings"].is_object()) {
		s._ghostAccounts.clear();
		for (auto &[key, value] : j["ghostModeSettings"].items()) {
			auto account = std::make_unique<GhostModeAccountSettings>();
			value.get_to(*account);
			s._ghostAccounts[std::stoull(key)] = std::move(account);
		}
	}

	s._accountsVaulted = j.value("accountsVaulted", false);
	s._useGlobalGhostMode = j.value("useGlobalGhostMode", defaults._useGlobalGhostMode.current());
	s._saveDeletedMessages = j.value("saveDeletedMessages", defaults._saveDeletedMessages.current());
	s._saveLocallyDeletedMessages = j.value("saveLocallyDeletedMessages", defaults._saveLocallyDeletedMessages.current());
	s._secretChatsEnabled = j.value("secretChatsEnabled", defaults._secretChatsEnabled.current());
	s._saveMessagesHistory = j.value("saveMessagesHistory", defaults._saveMessagesHistory.current());
	s._saveEditedText = j.value("saveEditedText", defaults._saveEditedText.current());
	s._saveEditedMedia = j.value("saveEditedMedia", defaults._saveEditedMedia.current());
	s._saveEditedMarkup = j.value("saveEditedMarkup", defaults._saveEditedMarkup.current());
	s._saveEditedPrivate = j.value("saveEditedPrivate", defaults._saveEditedPrivate.current());
	s._saveEditedGroups = j.value("saveEditedGroups", defaults._saveEditedGroups.current());
	s._saveEditedChannels = j.value("saveEditedChannels", defaults._saveEditedChannels.current());
	s._saveEditedOwn = j.value("saveEditedOwn", defaults._saveEditedOwn.current());
	s._saveForBots = j.value("saveForBots", defaults._saveForBots.current());
	s._shadowBanIds = j.value("shadowBanIds", defaults._shadowBanIds);
	s._filtersEnabled = j.value("filtersEnabled", defaults._filtersEnabled.current());
	s._filtersEnabledInChats = j.value("filtersEnabledInChats", defaults._filtersEnabledInChats.current());
	s._hideFromBlocked = j.value("hideFromBlocked", defaults._hideFromBlocked.current());
	s._semiTransparentDeletedMessages = j.value("semiTransparentDeletedMessages", defaults._semiTransparentDeletedMessages.current());
	s._disableAds = j.value("disableAds", defaults._disableAds.current());
	s._invertSwipeGestures = j.value("invertSwipeGestures", defaults._invertSwipeGestures.current());
	s._disableStories = j.value("disableStories", defaults._disableStories.current());
	s._disableCustomBackgrounds = j.value("disableCustomBackgrounds", defaults._disableCustomBackgrounds.current());
	s._hidePremiumStatuses = j.value("hidePremiumStatuses", defaults._hidePremiumStatuses.current());
	s._showOnlyAddedEmojisAndStickers = j.value("showOnlyAddedEmojisAndStickers", defaults._showOnlyAddedEmojisAndStickers.current());
	s._collapseSimilarChannels = j.value("collapseSimilarChannels", defaults._collapseSimilarChannels.current());
	s._hideSimilarChannels = j.value("hideSimilarChannels", defaults._hideSimilarChannels.current());
	s._messageBubbleRadius = j.value("messageBubbleRadius", defaults._messageBubbleRadius.current());
	s._disableOpenLinkWarning = j.value("disableOpenLinkWarning", defaults._disableOpenLinkWarning.current());
	s._wideMultiplier = j.value("wideMultiplier", defaults._wideMultiplier.current());
	s._spoofWebviewAsAndroid = j.value("spoofWebviewAsAndroid", defaults._spoofWebviewAsAndroid.current());
	s._increaseWebviewHeight = j.value("increaseWebviewHeight", defaults._increaseWebviewHeight.current());
	s._increaseWebviewWidth = j.value("increaseWebviewWidth", defaults._increaseWebviewWidth.current());
	s._materialSwitches = j.value("materialSwitches", defaults._materialSwitches.current());
	s._materialBubbles = j.value("materialBubbles", defaults._materialBubbles.current());
	s._designStyle = j.value("designStyle", defaults._designStyle.current());
	s._whisperModel = j.value("whisperModel", defaults._whisperModel.current());
	s._ocrLanguages = j.value("ocrLanguages", defaults._ocrLanguages.current());
	s._iconSet = j.contains("iconSet")
		? j.value("iconSet", defaults._iconSet.current())
		: j.value("materialIcons", true)
		? int(AyuUiSettings::IconSet::Materialgram)
		: int(AyuUiSettings::IconSet::Telegram);
	s._maxEditRevisions = j.value("maxEditRevisions", defaults._maxEditRevisions.current());
	s._deletedRestoreLimit = j.value("deletedRestoreLimit", defaults._deletedRestoreLimit.current());
	s._keptSnapshotLimit = j.value("keptSnapshotLimit", defaults._keptSnapshotLimit.current());
	s._deletedMediaMaxSizeMb = j.value("deletedMediaMaxSizeMb", defaults._deletedMediaMaxSizeMb.current());
	s._keepDeletedDays = j.value("keepDeletedDays", defaults._keepDeletedDays.current());
	s._deleteSavedFiles = j.value("deleteSavedFiles", defaults._deleteSavedFiles.current());
	s._backupKeepCount = j.value("backupKeepCount", defaults._backupKeepCount.current());
	s._backupIntervalHours = j.value("backupIntervalHours", defaults._backupIntervalHours.current());
	s._replyBackgroundOpacity = j.value("replyBackgroundOpacity", defaults._replyBackgroundOpacity.current());
	s._reactionBackgroundOpacity = j.value("reactionBackgroundOpacity", defaults._reactionBackgroundOpacity.current());
	s._disableNotificationsDelay = j.value("disableNotificationsDelay", defaults._disableNotificationsDelay.current());
	s._localPremium = j.value("localPremium", defaults._localPremium.current());
	s._showChannelReactions = j.value("showChannelReactions", defaults._showChannelReactions.current());
	s._showGroupReactions = j.value("showGroupReactions", defaults._showGroupReactions.current());
	s._showPrivateChatReactions = j.value("showPrivateChatReactions", defaults._showPrivateChatReactions.current());
	s._appIcon = j.value("appIcon", defaults._appIcon.current());
	s._simpleQuotesAndReplies = j.value("simpleQuotesAndReplies", defaults._simpleQuotesAndReplies.current());
	s._hideFastShare = j.value("hideFastShare", defaults._hideFastShare.current());
	s._replaceBottomInfoWithIcons = j.value("replaceBottomInfoWithIcons", defaults._replaceBottomInfoWithIcons.current());
	s._deletedMark = j.value("deletedMark", defaults._deletedMark.current());
	s._editedMark = j.value("editedMark", defaults._editedMark.current());
	s._unlimitedRecentStickers = j.value("unlimitedRecentStickers", defaults._unlimitedRecentStickers.current());
	s._showReactionsPanelInContextMenu = j.value("showReactionsPanelInContextMenu", defaults._showReactionsPanelInContextMenu.current());
	s._showViewsPanelInContextMenu = j.value("showViewsPanelInContextMenu", defaults._showViewsPanelInContextMenu.current());
	s._showHideMessageInContextMenu = j.value("showHideMessageInContextMenu", defaults._showHideMessageInContextMenu.current());
	s._showUserMessagesInContextMenu = j.value("showUserMessagesInContextMenu", defaults._showUserMessagesInContextMenu.current());
	s._showMessageDetailsInContextMenu = j.value("showMessageDetailsInContextMenu", defaults._showMessageDetailsInContextMenu.current());
	s._showRepeatMessageInContextMenu = j.value("showRepeatMessageInContextMenu", defaults._showRepeatMessageInContextMenu.current());
	s._showAddFilterInContextMenu = j.value("showAddFilterInContextMenu", defaults._showAddFilterInContextMenu.current());
	s._showAttachButtonInMessageField = j.value("showAttachButtonInMessageField", defaults._showAttachButtonInMessageField.current());
	s._showCommandsButtonInMessageField = j.value("showCommandsButtonInMessageField", defaults._showCommandsButtonInMessageField.current());
	s._showEmojiButtonInMessageField = j.value("showEmojiButtonInMessageField", defaults._showEmojiButtonInMessageField.current());
	s._showMicrophoneButtonInMessageField = j.value("showMicrophoneButtonInMessageField", defaults._showMicrophoneButtonInMessageField.current());
	s._showAutoDeleteButtonInMessageField = j.value("showAutoDeleteButtonInMessageField", defaults._showAutoDeleteButtonInMessageField.current());
	s._showGiftButtonInMessageField = j.value("showGiftButtonInMessageField", defaults._showGiftButtonInMessageField.current());
	s._showTranslateButtonInMessageField = j.value("showTranslateButtonInMessageField", defaults._showTranslateButtonInMessageField.current());
	s._translateComposeTo = j.value("translateComposeTo", defaults._translateComposeTo.current());
	s._autoTranslateOutgoing = j.value("autoTranslateOutgoing", defaults._autoTranslateOutgoing.current());
	s._showAiEditorButtonInMessageField = j.value("showAiEditorButtonInMessageField", defaults._showAiEditorButtonInMessageField.current());
	s._showAttachPopup = j.value("showAttachPopup", defaults._showAttachPopup.current());
	s._showEmojiPopup = j.value("showEmojiPopup", defaults._showEmojiPopup.current());
	s._showMyProfileInDrawer = j.value("showMyProfileInDrawer", defaults._showMyProfileInDrawer.current());
	s._showBotsInDrawer = j.value("showBotsInDrawer", defaults._showBotsInDrawer.current());
	s._showNewGroupInDrawer = j.value("showNewGroupInDrawer", defaults._showNewGroupInDrawer.current());
	s._showNewChannelInDrawer = j.value("showNewChannelInDrawer", defaults._showNewChannelInDrawer.current());
	s._showNewStoryInDrawer = j.value("showNewStoryInDrawer", defaults._showNewStoryInDrawer.current());
	s._passcodeWipeAttempts = j.value("passcodeWipeAttempts", defaults._passcodeWipeAttempts.current());
	s._passcodeWipeAction = j.value("passcodeWipeAction", defaults._passcodeWipeAction.current());
	s._lockScreenShowClock = j.value("lockScreenShowClock", defaults._lockScreenShowClock.current());
	s._lockScreenStyle = j.value("lockScreenStyle", defaults._lockScreenStyle.current());
	s._lockScreenBackground = j.value("lockScreenBackground", defaults._lockScreenBackground.current());
	s._lockScreenBlur = j.value("lockScreenBlur", defaults._lockScreenBlur.current());
	s._lockScreenDim = j.value("lockScreenDim", defaults._lockScreenDim.current());
	s._lockScreenShowDate = j.value("lockScreenShowDate", defaults._lockScreenShowDate.current());
	s._lockScreenText = j.value("lockScreenText", defaults._lockScreenText.current());
	s._lockScreenShowLogout = j.value("lockScreenShowLogout", defaults._lockScreenShowLogout.current());
	s._lockScreenShowEye = j.value("lockScreenShowEye", defaults._lockScreenShowEye.current());
	s._lockScreenColor1 = j.value("lockScreenColor1", defaults._lockScreenColor1.current());
	s._lockScreenColor2 = j.value("lockScreenColor2", defaults._lockScreenColor2.current());
	s._lockScreenAvatar = j.value("lockScreenAvatar", defaults._lockScreenAvatar.current());
	s._lockScreenAvatarShape = j.value("lockScreenAvatarShape", defaults._lockScreenAvatarShape.current());
	s._lockScreenCardOpacity = j.value("lockScreenCardOpacity", defaults._lockScreenCardOpacity.current());
	s._lockScreenCardRadius = j.value("lockScreenCardRadius", defaults._lockScreenCardRadius.current());
	s._lockScreenCardBorder = j.value("lockScreenCardBorder", defaults._lockScreenCardBorder.current());
	s._lockScreenClockFormat = j.value("lockScreenClockFormat", defaults._lockScreenClockFormat.current());
	s._lockScreenClockSeconds = j.value("lockScreenClockSeconds", defaults._lockScreenClockSeconds.current());
	s._lockScreenClockSize = j.value("lockScreenClockSize", defaults._lockScreenClockSize.current());
	s._lockScreenClockColor = j.value("lockScreenClockColor", defaults._lockScreenClockColor.current());
	s._lockScreenTitle = j.value("lockScreenTitle", defaults._lockScreenTitle.current());
	s._lockScreenSubmitText = j.value("lockScreenSubmitText", defaults._lockScreenSubmitText.current());
	s._lockScreenAccent = j.value("lockScreenAccent", defaults._lockScreenAccent.current());
	s._sendClearMetadata = j.value("sendClearMetadata", defaults._sendClearMetadata.current());
	s._lockOnSystemLock = j.value("lockOnSystemLock", defaults._lockOnSystemLock.current());
	s._lockOnMinimize = j.value("lockOnMinimize", defaults._lockOnMinimize.current());
	s._hideFromCapture = j.value("hideFromCapture", defaults._hideFromCapture.current());
	s._lockedNotifications = j.value("lockedNotifications", defaults._lockedNotifications.current());
	s._showContactsInDrawer = j.value("showContactsInDrawer", defaults._showContactsInDrawer.current());
	s._showCallsInDrawer = j.value("showCallsInDrawer", defaults._showCallsInDrawer.current());
	s._showSavedMessagesInDrawer = j.value("showSavedMessagesInDrawer", defaults._showSavedMessagesInDrawer.current());
	s._showLReadToggleInDrawer = j.value("showLReadToggleInDrawer", defaults._showLReadToggleInDrawer.current());
	s._showSReadToggleInDrawer = j.value("showSReadToggleInDrawer", defaults._showSReadToggleInDrawer.current());
	s._showNightModeToggleInDrawer = j.value("showNightModeToggleInDrawer", defaults._showNightModeToggleInDrawer.current());
	s._showGhostToggleInDrawer = j.value("showGhostToggleInDrawer", defaults._showGhostToggleInDrawer.current());
	s._showStreamerToggleInDrawer = j.value("showStreamerToggleInDrawer", defaults._showStreamerToggleInDrawer.current());
	s._showGhostToggleInTray = j.value("showGhostToggleInTray", defaults._showGhostToggleInTray.current());
	s._showStreamerToggleInTray = j.value("showStreamerToggleInTray", defaults._showStreamerToggleInTray.current());
	s._monoFont = j.value("monoFont", defaults._monoFont.current());
	s._hideNotificationCounters = j.value("hideNotificationCounters", defaults._hideNotificationCounters.current());
	s._hideNotificationBadge = j.value("hideNotificationBadge", defaults._hideNotificationBadge.current());
	s._hideAllChatsFolder = j.value("hideAllChatsFolder", defaults._hideAllChatsFolder.current());
	s._hideBirthdayNotifications = j.value("hideBirthdayNotifications", defaults._hideBirthdayNotifications.current());
	s._channelBottomButton = j.value("channelBottomButton", defaults._channelBottomButton.current());
	s._quickAdminShortcuts = j.value("quickAdminShortcuts", defaults._quickAdminShortcuts.current());
	s._disableGreetingSticker = j.value("disableGreetingSticker", defaults._disableGreetingSticker.current());
	s._showPeerId = j.value("showPeerId", defaults._showPeerId.current());
	s._showMessageSeconds = j.value("showMessageSeconds", defaults._showMessageSeconds.current());
	s._keepRemovedUserChats = j.value("keepRemovedUserChats", defaults._keepRemovedUserChats.current());
	s._keepRemovedGroups = j.value("keepRemovedGroups", defaults._keepRemovedGroups.current());
	s._keepRemovedChannels = j.value("keepRemovedChannels", defaults._keepRemovedChannels.current());
	s._keepRemovedForums = j.value("keepRemovedForums", defaults._keepRemovedForums.current());
	s._keepDeletedTopics = j.value("keepDeletedTopics", defaults._keepDeletedTopics.current());
	s._restoreDeletedInChats = j.value("restoreDeletedInChats", defaults._restoreDeletedInChats.current());
	s._showDeletedChatIcon = j.value("showDeletedChatIcon", defaults._showDeletedChatIcon.current());
	s._localPinnedChats = j.value("localPinnedChats", defaults._localPinnedChats.current());
	s._keepDraftsLocal = j.value("keepDraftsLocal", defaults._keepDraftsLocal.current());
	s._stashHintShown = j.value("stashHintShown", defaults._stashHintShown.current());
	s._stashHintUsed = j.value("stashHintUsed", defaults._stashHintUsed.current());
	s._reduceNetworkTracking = j.value("reduceNetworkTracking", defaults._reduceNetworkTracking.current());
	s._enableDebugLogging = j.value("enableDebugLogging", defaults._enableDebugLogging.current());
	s._markOldMessagesDeleted = j.value("markOldMessagesDeleted", defaults._markOldMessagesDeleted.current());
	s._showTimeZone = j.value("showTimeZone", defaults._showTimeZone.current());
	s._showMessageShot = j.value("showMessageShot", defaults._showMessageShot.current());
	s._filterZalgo = j.value("filterZalgo", defaults._filterZalgo.current());
	s._stickerConfirmation = j.value("stickerConfirmation", defaults._stickerConfirmation.current());
	s._gifConfirmation = j.value("gifConfirmation", defaults._gifConfirmation.current());
	s._voiceConfirmation = j.value("voiceConfirmation", defaults._voiceConfirmation.current());
	s._roundConfirmation = j.value("roundConfirmation", defaults._roundConfirmation.current());
	s._translationProvider = j.value("translationProvider", defaults._translationProvider.current());
	s._transcriptionProvider = j.value("transcriptionProvider", defaults._transcriptionProvider.current());
	s._adaptiveCoverColor = j.value("adaptiveCoverColor", defaults._adaptiveCoverColor.current());
	s._improveLinkPreviews = j.value("improveLinkPreviews", defaults._improveLinkPreviews.current());
	s._crashReporting = j.value("crashReporting", defaults._crashReporting.current());
	s._pluginsEnabled = j.value("pluginsEnabled", defaults._pluginsEnabled.current());
	s._pluginsRuntimeAutoUpdate = j.value("pluginsRuntimeAutoUpdate", defaults._pluginsRuntimeAutoUpdate.current());
	s._avatarCorners = j.value("avatarCorners", defaults._avatarCorners.current());
	s._singleCornerRadius = j.value("singleCornerRadius", defaults._singleCornerRadius.current());
	s._streamerMode = j.value("streamerMode", defaults._streamerMode.current());
	s._streamerHideWholeWindow = j.value("streamerHideWholeWindow", defaults._streamerHideWholeWindow.current());
	s._streamerSpoilers = j.value("streamerSpoilers", defaults._streamerSpoilers.current());

	if (j.contains("messageShotSettings") && j["messageShotSettings"].is_object()) {
		j["messageShotSettings"].get_to(s._messageShotSettings);
	}
}
