// This is the source code of freshGram.
#include "ayu/features/recognition/local_transcribe.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/recognition/recognition_models.h"
#include "ayu/features/recognition/speech_recognition.h"
#include "base/weak_ptr.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#include <deque>

namespace FreshRecognition {
namespace {

constexpr auto kMaxDocumentSize = int64(200) * 1024 * 1024;

struct Job {
	base::weak_ptr<Main::Session> session;
	FullMsgId id;
	DocumentId documentId = 0;
	QString model;
	QString modelSha256;
	QString temporary;
	std::shared_ptr<Data::DocumentMedia> media;
	std::shared_ptr<std::atomic<bool>> cancel;
	Fn<void(LocalTranscript)> done;
	bool running = false;
	bool finished = false;
	rpl::lifetime lifetime;
};

base::flat_map<FullMsgId, std::shared_ptr<Job>> Jobs;

// WHY: each run takes the model in memory and half of the cores, so
// the downloaded messages wait here for the running one.
std::deque<std::shared_ptr<Job>> Waiting;
bool Busy = false;
bool TemporaryCleared = false;

[[nodiscard]] QString TemporaryFolder() {
	return ModelsPath() + u"/.tmp"_q;
}

void RunNext();

void Finish(
		const std::shared_ptr<Job> &job,
		LocalTranscript result) {
	if (job->finished) {
		return;
	}
	job->finished = true;
	if (!job->temporary.isEmpty()) {
		QFile::remove(job->temporary);
		job->temporary = QString();
	}
	if (job->running) {
		job->running = false;
		Busy = false;
		crl::on_main([] {
			RunNext();
		});
	}
	Waiting.erase(
		ranges::remove(Waiting, job),
		end(Waiting));
	const auto done = std::move(job->done);
	const auto alive = (job->session.get() != nullptr);
	job->lifetime.destroy();
	if (const auto i = Jobs.find(job->id)
		; i != end(Jobs) && i->second == job) {
		Jobs.erase(i);
	}
	if (alive && done) {
		done(std::move(result));
	}
}

[[nodiscard]] LocalTranscript Failure(
		RecognitionError error,
		const QString &details = QString()) {
	auto result = LocalTranscript();
	result.status = LocalTranscribeStatus::Failed;
	result.error = error;
	result.details = details;
	return result;
}

void Recognize(const std::shared_ptr<Job> &job, const QString &path) {
	job->running = true;
	Busy = true;
	job->cancel = TranscribeAsync({
		.audioPath = path,
		.modelPath = job->model,
		.modelSha256 = job->modelSha256,
	}, nullptr, [=](SpeechResult result) {
		auto transcript = LocalTranscript();
		if (result.cancelled) {
			transcript.status = LocalTranscribeStatus::Cancelled;
		} else if (!result.ok) {
			transcript = Failure(result.error, result.details);
		} else {
			transcript.status = LocalTranscribeStatus::Done;
			transcript.text = result.text;
		}
		Finish(job, std::move(transcript));
	});
}

void Start(const std::shared_ptr<Job> &job) {
	const auto session = job->session.get();
	if (!session) {
		Finish(job, Failure(RecognitionError::NoData));
		return;
	}
	const auto document = session->data().document(job->documentId);
	const auto media = job->media;
	if (!media) {
		Finish(job, Failure(RecognitionError::NoData));
		return;
	}
	auto path = document->filepath(true);
	if (path.isEmpty() || !QFileInfo::exists(path)) {
		const auto bytes = media->bytes();
		if (bytes.isEmpty()) {
			Finish(job, Failure(RecognitionError::NoData));
			return;
		}
		const auto folder = TemporaryFolder();
		QDir().mkpath(folder);
		// WHY: the same document can be in several messages at once.
		path = folder
			+ '/'
			+ QString::number(job->id.peer.value)
			+ '_'
			+ QString::number(job->id.msg.bare)
			+ u".bin"_q;
		auto file = QFile(path);
		if (!file.open(QIODevice::WriteOnly)
			|| file.write(bytes) != bytes.size()) {
			Finish(job, Failure(
				RecognitionError::NoData,
				u"Could not save the file."_q));
			return;
		}
		job->temporary = path;
	}
	Recognize(job, path);
}

void RunNext() {
	while (!Busy && !Waiting.empty()) {
		const auto job = Waiting.front();
		Waiting.pop_front();
		Start(job);
	}
}

void Enqueue(const std::shared_ptr<Job> &job) {
	if (!job->finished) {
		Waiting.push_back(job);
		RunNext();
	}
}

void Transcribe(
		not_null<HistoryItem*> item,
		Fn<void(LocalTranscript)> done,
		bool mayLoadList) {
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	if (!document
		|| !(document->isVoiceMessage() || document->isVideoMessage())) {
		done(Failure(RecognitionError::NoData, u"Not a voice message."_q));
		return;
	}
	const auto session = &item->history()->session();
	const auto model = SelectedWhisperModel();
	if (!model) {
		const auto chosen = !AyuSettings::getInstance().whisperModel().isEmpty();
		if (mayLoadList && chosen && !ModelsKnown()) {
			// WHY: without the list of models no downloaded file is found.
			const auto weak = base::make_weak(session);
			const auto id = item->fullId();
			LoadModelsList(session, [=] {
				const auto strong = weak.get();
				if (!strong) {
					return;
				} else if (const auto item = strong->data().message(id)) {
					Transcribe(item, done, false);
				} else {
					done(Failure(RecognitionError::NoData));
				}
			});
			return;
		}
		auto result = LocalTranscript();
		result.status = LocalTranscribeStatus::NoModel;
		done(std::move(result));
		return;
	} else if (document->size > kMaxDocumentSize) {
		done(Failure(RecognitionError::TooLarge));
		return;
	}

	const auto id = item->fullId();
	if (Jobs.contains(id)) {
		return;
	}
	if (!std::exchange(TemporaryCleared, true)) {
		// What is left after a crash.
		QDir(TemporaryFolder()).removeRecursively();
	}
	const auto job = std::make_shared<Job>();
	job->session = base::make_weak(session);
	job->id = id;
	job->documentId = document->id;
	job->model = ModelPath(*model);
	job->modelSha256 = model->sha256;
	job->done = std::move(done);
	job->media = document->createMediaView();
	Jobs.emplace(id, job);

	const auto weakJob = std::weak_ptr<Job>(job);
	session->lifetime().add([=] {
		if (const auto job = weakJob.lock()) {
			if (job->cancel) {
				job->cancel->store(true);
			}
			if (!job->running) {
				auto result = LocalTranscript();
				result.status = LocalTranscribeStatus::Cancelled;
				Finish(job, std::move(result));
			}
		}
	});

	if (job->media->loaded(true)) {
		Enqueue(job);
		return;
	}
	document->save(::Data::FileOrigin(id), QString());
	session->downloaderTaskFinished(
	) | rpl::filter([=] {
		const auto session = job->session.get();
		return !session
			|| (job->media->loaded()
				|| !session->data().document(job->documentId)->loading());
	}) | rpl::take(1) | rpl::on_next([=] {
		// WHY: a failed start destroys this subscription.
		crl::on_main([=] {
			Enqueue(job);
		});
	}, job->lifetime);
}

} // namespace

bool LocalTranscriptionEnabled() {
	return (AyuSettings::getInstance().transcriptionProvider()
			== TranscriptionProvider::WhisperLocal)
		&& SpeechRecognitionAvailable();
}

void TranscribeLocally(
		not_null<HistoryItem*> item,
		Fn<void(LocalTranscript)> done) {
	Transcribe(item, std::move(done), true);
}

} // namespace FreshRecognition
