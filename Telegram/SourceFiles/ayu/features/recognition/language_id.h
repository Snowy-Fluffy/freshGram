// This is the source code of freshGram.
#pragma once

#include <QtCore/QString>

#include <vector>

namespace FreshRecognition {

struct LanguageGuess {
	// A two-letter code, like "ru".
	QString code;
	float probability = 0.f;
};

// The detector is compiled in.
[[nodiscard]] bool LanguageDetectionAvailable();

// The best guess is first, and the list is empty if the text is too short.
[[nodiscard]] std::vector<LanguageGuess> GuessLanguages(
	const QString &text,
	int count = 5);

} // namespace FreshRecognition
