// This is the source code of freshGram.
#include "ayu/features/plugins/plugins_transcribe.h"

#include "ayu/features/plugins/plugins_data.h"
#include "ayu/features/plugins/plugins_service.h"
#include "base/weak_ptr.h"
#include "crl/crl_on_main.h"
#include "data/data_document.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"

#ifdef FRESHGRAM_PLUGINS
#include "fgpl/manager.h"
#endif

namespace FreshPlugins {

#ifdef FRESHGRAM_PLUGINS

namespace {

constexpr auto kTranscribeTimeoutMs = 120000;

[[nodiscard]] bool Provides(
		const fgpl::ManagerSnapshot &snapshot,
		const std::string &id) {
	const auto info = snapshot.plugin(id);
	const auto parsed = snapshot.manifest(id);
	const auto grant = snapshot.grantsOf(id, std::string());
	return info
		&& info->enabled
		&& parsed
		&& parsed->transcriber.declared
		&& grant
		&& grant->granted.contains("transcribe.provide");
}

[[nodiscard]] FreshRecognition::LocalTranscript Failure(
		const std::string &code) {
	auto result = FreshRecognition::LocalTranscript();
	result.status = FreshRecognition::LocalTranscribeStatus::Failed;
	result.details = QString::fromStdString(code);
	result.error = (code == "too_long")
		? FreshRecognition::RecognitionError::TooLarge
		: FreshRecognition::RecognitionError::Failed;
	return result;
}

} // namespace

std::vector<TranscriberEntry> Transcribers() {
	auto result = std::vector<TranscriberEntry>();
	const auto snapshot = Snapshot();
	if (!snapshot) {
		return result;
	}
	for (const auto &info : snapshot->plugins) {
		if (Provides(*snapshot, info.id)) {
			const auto &declared = snapshot->manifest(info.id)->transcriber.name;
			result.push_back({
				QString::fromStdString(info.id),
				QString::fromStdString(declared.empty() ? info.id : declared),
			});
		}
	}
	return result;
}

bool TranscriberAvailable(const QString &plugin) {
	const auto snapshot = Snapshot();
	return snapshot && Provides(*snapshot, plugin.toStdString());
}

QString TranscriberName(const QString &plugin) {
	const auto snapshot = Snapshot();
	const auto parsed = snapshot
		? snapshot->manifest(plugin.toStdString())
		: nullptr;
	return (parsed && !parsed->transcriber.name.empty())
		? QString::fromStdString(parsed->transcriber.name)
		: plugin;
}

void TranscribeByPlugin(
		not_null<HistoryItem*> item,
		const QString &plugin,
		Fn<void(FreshRecognition::LocalTranscript)> done) {
	const auto session = &item->history()->session();
	const auto weak = base::make_weak(session);
	const auto finish = [=](fgpl::TranscribeResult result) {
		crl::on_main(weak, [=, result = std::move(result)]() mutable {
			if (!result.error.empty()) {
				done(Failure(result.error));
				return;
			}
			auto transcript = FreshRecognition::LocalTranscript();
			transcript.status = FreshRecognition::LocalTranscribeStatus::Done;
			transcript.text = QString::fromStdString(result.text).trimmed();
			done(std::move(transcript));
		});
	};
	auto request = fgpl::TranscribeRequest();
	request.chat = int64_t(item->history()->peer->id.value);
	request.message = int64_t(item->id.bare);
	if (const auto media = item->media()) {
		if (const auto document = media->document()) {
			request.video = document->isVideoMessage();
			request.duration = std::max<int64_t>(
				0,
				document->duration() / 1000);
		}
	}
	const auto account = AccountId(session).toStdString();
	const auto id = plugin.toStdString();
	const auto posted = Post([=, request = std::move(request)](
			not_null<fgpl::PluginManager*> manager) mutable {
		manager->transcribeAsync(
			account,
			id,
			std::move(request),
			kTranscribeTimeoutMs,
			finish);
	}, false);
	if (!posted) {
		auto result = fgpl::TranscribeResult();
		result.error = "unavailable";
		finish(std::move(result));
	}
}

#else

std::vector<TranscriberEntry> Transcribers() {
	return {};
}

bool TranscriberAvailable(const QString &) {
	return false;
}

QString TranscriberName(const QString &plugin) {
	return plugin;
}

void TranscribeByPlugin(
		not_null<HistoryItem*>,
		const QString &,
		Fn<void(FreshRecognition::LocalTranscript)>) {
}

#endif

} // namespace FreshPlugins
