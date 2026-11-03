// This is the source code of freshGram.
#pragma once

#include "ayu/features/recognition/local_transcribe.h"

#include <vector>

class HistoryItem;

namespace FreshPlugins {

struct TranscriberEntry {
	QString plugin;
	QString name;
};

// Enabled plugins that declare contributes.transcriber and may provide it.
[[nodiscard]] std::vector<TranscriberEntry> Transcribers();
[[nodiscard]] bool TranscriberAvailable(const QString &plugin);
[[nodiscard]] QString TranscriberName(const QString &plugin);

// The callback is called in the main thread, unless the session is gone.
void TranscribeByPlugin(
	not_null<HistoryItem*> item,
	const QString &plugin,
	Fn<void(FreshRecognition::LocalTranscript)> done);

} // namespace FreshPlugins
