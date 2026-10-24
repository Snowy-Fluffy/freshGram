// This is the source code of freshGram.
#include "ayu/features/recognition/speech_recognition.h"

#include "ayu/features/recognition/recognition_audio.h"
#include "ayu/features/recognition/recognition_models.h"
#include "base/call_delayed.h"

#ifdef FRESHGRAM_LOCAL_RECOGNITION
#include <whisper.h>
#endif // FRESHGRAM_LOCAL_RECOGNITION

#include <QtCore/QThread>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_AMD64))
#include <intrin.h>
#endif

#include <algorithm>
#include <cmath>
#include <mutex>
#include <numeric>

namespace FreshRecognition {
namespace {

#ifdef FRESHGRAM_LOCAL_RECOGNITION

constexpr auto kKeepModel = 60 * crl::time(1000);

// The last loaded model, its loading takes seconds for the large ones.
std::mutex ContextMutex;
whisper_context *Context = nullptr;
QString ContextKey;
std::atomic<int> ContextUses = 0;

void FreeContext() {
	if (Context) {
		whisper_free(Context);
		Context = nullptr;
	}
	ContextKey = QString();
}

void ScheduleContextRelease() {
	const auto uses = ContextUses.load();
	base::call_delayed(kKeepModel, [=] {
		crl::async([=] {
			const auto lock = std::unique_lock(
				ContextMutex,
				std::try_to_lock);
			if (lock.owns_lock() && ContextUses.load() == uses) {
				FreeContext();
			}
		});
	});
}

// The engine is built with AVX2 on x86, so check before running it.
[[nodiscard]] bool ProcessorIsSupported() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_AMD64))
	int info[4] = { 0 };
	__cpuid(info, 0);
	if (info[0] < 7) {
		return false;
	}
	__cpuid(info, 1);
	const auto fma = (info[2] & (1 << 12)) != 0;
	const auto f16c = (info[2] & (1 << 29)) != 0;
	const auto osxsave = (info[2] & (1 << 27)) != 0;
	if (!fma || !f16c || !osxsave
		|| (_xgetbv(0) & 0x6) != 0x6) {
		return false;
	}
	__cpuidex(info, 7, 0);
	return (info[1] & (1 << 5)) != 0;
#elif defined(__x86_64__) || defined(__i386__)
	return __builtin_cpu_supports("avx2")
		&& __builtin_cpu_supports("fma")
		&& __builtin_cpu_supports("f16c");
#else
	return true;
#endif
}

struct Progress {
	Fn<void(int)> callback;
};

void ProgressCallback(
		whisper_context*,
		whisper_state*,
		int progress,
		void *userData) {
	const auto callback = static_cast<Progress*>(userData)->callback;
	if (callback) {
		crl::on_main([=] {
			callback(progress);
		});
	}
}

bool AbortCallback(void *userData) {
	return static_cast<const std::atomic<bool>*>(userData)->load();
}

[[nodiscard]] SpeechResult Transcribe(
		const SpeechRequest &request,
		const std::atomic<bool> &cancelled,
		Fn<void(int)> progress) {
	auto result = SpeechResult();
	if (!ProcessorIsSupported()) {
		result.error = RecognitionError::UnsupportedCpu;
		return result;
	}
	if (!ModelFileIntact(request.modelPath, request.modelSha256)) {
		result.error = RecognitionError::Integrity;
		return result;
	}
	const auto audio = DecodeAudioFile(request.audioPath, cancelled);
	if (cancelled.load()) {
		result.cancelled = true;
		return result;
	} else if (audio.samples.empty()) {
		result.error = RecognitionError::Decode;
		result.details = audio.error;
		return result;
	}
	const auto peak = std::accumulate(
		audio.samples.begin(),
		audio.samples.end(),
		0.f,
		[](float was, float sample) {
			return std::max(was, std::abs(sample));
		});
	if (peak < 0.001f) {
		result.ok = true; // Silence.
		return result;
	}

	const auto lock = std::lock_guard(ContextMutex);
	++ContextUses;
	if (cancelled.load()) {
		result.cancelled = true;
		return result;
	}
	const auto key = request.modelPath + '#' + request.modelSha256;
	if (!Context || ContextKey != key) {
		FreeContext();
		auto contextParams = whisper_context_default_params();
		contextParams.use_gpu = false;
		const auto model = request.modelPath.toUtf8();
		Context = whisper_init_from_file_with_params(
			model.constData(),
			contextParams);
		if (!Context) {
			result.error = RecognitionError::LoadFailed;
			return result;
		}
		ContextKey = key;
	}
	const auto context = Context;

	auto params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
	const auto language = request.language.toUtf8();
	params.n_threads = std::clamp(QThread::idealThreadCount() / 2, 1, 8);
	params.print_progress = false;
	params.print_realtime = false;
	params.print_timestamps = false;
	params.print_special = false;
	params.translate = false;
	params.no_context = true;
	params.suppress_blank = true;
	params.suppress_nst = true;
	params.language = language.isEmpty() ? "auto" : language.constData();
	params.detect_language = false;

	auto progressData = Progress{ std::move(progress) };
	params.progress_callback = &ProgressCallback;
	params.progress_callback_user_data = &progressData;
	params.abort_callback = &AbortCallback;
	params.abort_callback_user_data = const_cast<std::atomic<bool>*>(
		&cancelled);

	const auto code = whisper_full(
		context,
		params,
		audio.samples.data(),
		int(audio.samples.size()));
	if (cancelled.load()) {
		result.cancelled = true;
	} else if (code != 0) {
		result.error = RecognitionError::Failed;
	} else {
		const auto count = whisper_full_n_segments(context);
		auto text = QString();
		for (auto i = 0; i != count; ++i) {
			text += QString::fromUtf8(
				whisper_full_get_segment_text(context, i));
		}
		result.text = text.trimmed();
		const auto id = whisper_full_lang_id(context);
		if (const auto name = whisper_lang_str(id)) {
			result.language = QString::fromLatin1(name);
		}
		result.ok = true;
	}
	if (code != 0) {
		// WHY: the state after a failed or aborted run is not trusted.
		FreeContext();
	}
	return result;
}

#endif // FRESHGRAM_LOCAL_RECOGNITION

} // namespace

bool SpeechRecognitionAvailable() {
#ifdef FRESHGRAM_LOCAL_RECOGNITION
	return ProcessorIsSupported();
#else // FRESHGRAM_LOCAL_RECOGNITION
	return false;
#endif // !FRESHGRAM_LOCAL_RECOGNITION
}

std::shared_ptr<std::atomic<bool>> TranscribeAsync(
		SpeechRequest request,
		Fn<void(int percent)> progress,
		Fn<void(SpeechResult)> done) {
	auto cancelled = std::make_shared<std::atomic<bool>>(false);
#ifdef FRESHGRAM_LOCAL_RECOGNITION
	crl::async([=] {
		const auto result = Transcribe(request, *cancelled, progress);
		crl::on_main([=] {
			ScheduleContextRelease();
			done(result);
		});
	});
#else // FRESHGRAM_LOCAL_RECOGNITION
	crl::on_main([=] {
		auto result = SpeechResult();
		result.error = RecognitionError::Unavailable;
		done(std::move(result));
	});
#endif // !FRESHGRAM_LOCAL_RECOGNITION
	return cancelled;
}

} // namespace FreshRecognition
