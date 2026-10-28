#include "ayu/secret/secret_policy.h"

#include "ayu/ayu_settings.h"
#include "ayu/data/fresh_database_backup.h"
#include "ayu/secret/secret_manager.h"
#include "core/application.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "main/main_domain.h"
#include "storage/storage_domain.h"
#include "ui/toast/toast.h"

namespace FreshSecret {

bool PasscodeSet() {
	return Core::App().domain().local().hasLocalPasscode();
}

bool Enabled() {
	return AyuSettings::getInstance().secretChatsEnabled() && PasscodeSet();
}

void SyncPolicy() {
	auto &settings = AyuSettings::getInstance();
	if (settings.secretChatsEnabled() && !PasscodeSet()) {
		settings.setSecretChatsEnabled(false);
	}
}

void HandlePasscodeChange() {
	SyncPolicy();
	if (PasscodeSet()) {
		return;
	}
	FreshDatabaseBackup::purgeSecrets();
	auto purged = false;
	for (const auto &entry : Core::App().domain().accounts()) {
		if (const auto session = entry.account->maybeSession()) {
			purged = purged || !Get(session).chats().empty();
			PurgeSession(session);
		}
	}
	if (purged) {
		Ui::Toast::Show(tr::fresh_SecretToastPasscodeRemoved(tr::now));
	}
}

} // namespace FreshSecret
