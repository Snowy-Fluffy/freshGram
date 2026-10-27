// This is the source code of freshGram.
#pragma once

#include "base/basic_types.h"

#include <translate_provider.h>

#include <memory>
#include <vector>

namespace Main {
class Session;
} // namespace Main

namespace FreshPlugins {

struct TranslatorEntry {
	QString plugin;
	QString name;
};

// Enabled plugins that declare contributes.translator and may provide it.
[[nodiscard]] std::vector<TranslatorEntry> Translators();
[[nodiscard]] bool TranslatorAvailable(const QString &plugin);
[[nodiscard]] QString TranslatorName(const QString &plugin);

// Null when the plugin cannot translate now.
[[nodiscard]] std::unique_ptr<Ui::TranslateProvider> CreatePluginTranslateProvider(
	not_null<Main::Session*> session,
	const QString &plugin);

} // namespace FreshPlugins
