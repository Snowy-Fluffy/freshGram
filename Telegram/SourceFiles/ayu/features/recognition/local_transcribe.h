// This is the source code of freshGram.
//
// The local transcription of voice messages and round videos, used by
// the usual transcribe button when the Whisper (local) provider is chosen.
#pragma once

#include "ayu/features/recognition/recognition_models.h"

class HistoryItem;

namespace FreshRecognition {

enum class LocalTranscribeStatus {
	Done,
	NoModel,
	Failed,
	Cancelled,
};

struct LocalTranscript {
	LocalTranscribeStatus status = LocalTranscribeStatus::Failed;
	QString text; // Empty if there was no speech.
	RecognitionError error = RecognitionError::None;
	QString details; // For the log.
};

// The Whisper (local) provider is chosen and the engine is available.
[[nodiscard]] bool LocalTranscriptionEnabled();

// Downloads the media if needed and recognizes the speech in a background
// thread, one message at a time. The callback is called in the main thread unless the session
// is gone, then it is not called at all.
void TranscribeLocally(
	not_null<HistoryItem*> item,
	Fn<void(LocalTranscript)> done);

} // namespace FreshRecognition
