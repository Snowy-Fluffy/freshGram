// This is the source code of freshGram.
#pragma once

#include <QtCore/QString>

#include <atomic>
#include <vector>

namespace FreshRecognition {

struct DecodedAudio {
	std::vector<float> samples; // 16 kHz, mono.
	QString error;
};

// Decodes the first audio stream of a voice message, a round video or
// a music file. Call it from a background thread only.
[[nodiscard]] DecodedAudio DecodeAudioFile(
	const QString &path,
	const std::atomic<bool> &cancelled);

} // namespace FreshRecognition
