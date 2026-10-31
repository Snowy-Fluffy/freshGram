// This is the source code of freshGram.
#pragma once

#include "base/basic_types.h"
#include "ui/text/text_entity.h"

#include <vector>

namespace Main {
class Session;
} // namespace Main

namespace FreshPlugins {

struct AiProviderEntry {
	QString plugin;
	QString name;
};

struct AiRequestData {
	TextWithEntities text;
	QString translateTo;
	QString tone;
	QString prompt;
	bool proofread = false;
	bool emojify = false;
};

// Enabled plugins that declare contributes.aiProvider and may provide it.
[[nodiscard]] std::vector<AiProviderEntry> AiProviders();
[[nodiscard]] bool AiAvailable(const QString &plugin);
[[nodiscard]] QString AiProviderName(const QString &plugin);

// Returns an id for CancelAi; done or fail comes later, never from inside the call.
[[nodiscard]] int RequestAi(
	not_null<Main::Session*> session,
	const QString &plugin,
	AiRequestData data,
	Fn<void(TextWithEntities &&)> done,
	Fn<void(const QString &)> fail);
void CancelAi(int id);

} // namespace FreshPlugins
