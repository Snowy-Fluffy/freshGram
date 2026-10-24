// This is the source code of freshGram.
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <atomic>
#include <memory>
#include <vector>

namespace FreshRecognition {

struct TextPart {
	QString text;
	bool translate = false;
};

// Sentences to translate and the whitespace between them, in order.
[[nodiscard]] std::vector<TextPart> SplitForTranslation(const QString &text);

// A range of a text in UTF-16 units.
struct TextSpan {
	int offset = 0;
	int length = 0;
};

struct TranslationModel {
	QString path;
	QString sha256;
};

struct TranslationInput {
	QString text;
	// Ranges that are carried over to the translation by the word alignment.
	std::vector<TextSpan> spans;
};

struct TranslationOutput {
	QString text;
	// One for each input span, with a zero length if it could not be placed.
	std::vector<TextSpan> spans;
};

struct TranslationJob {
	// One model, or two when the translation goes through English.
	std::vector<TranslationModel> chain;
	std::vector<TranslationInput> inputs;
};

struct TranslationResult {
	bool ok = false;
	bool cancelled = false;
	std::vector<TranslationOutput> outputs;
	QString error;
};

// The engine is compiled in.
[[nodiscard]] bool TextTranslationAvailable();

// The callback is called in the main thread, set the flag to cancel.
std::shared_ptr<std::atomic<bool>> TranslateTextsAsync(
	TranslationJob job,
	Fn<void(TranslationResult)> done);

} // namespace FreshRecognition
