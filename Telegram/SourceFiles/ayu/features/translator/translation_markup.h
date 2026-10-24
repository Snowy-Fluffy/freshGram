// This is the source code of freshGram.
#pragma once

#include "ayu/features/recognition/text_translation.h"

#include <QtCore/QString>

#include <vector>

namespace Ayu::Translator {

struct MarkupSpan {
	int offset = 0;
	int length = 0;
	int type = 0;
	QString data;
};

struct MarkupText {
	QString text;
	std::vector<MarkupSpan> spans;
};

enum class MarkupRole {
	Drop,
	// Left as is: the text of the span is not translated.
	Keep,
	// Carried over to the translation by the word alignment.
	Style,
};

struct MarkupMasked {
	QString text;
	QString style;
	std::vector<FreshRecognition::TextSpan> spans;
	std::vector<MarkupSpan> styles;
	std::vector<MarkupSpan> kept;
};

struct MarkupRestored {
	MarkupText text;
	// Indices in MarkupMasked::styles that could not be placed.
	std::vector<int> lost;
};

[[nodiscard]] MarkupMasked MaskMarkup(
	const MarkupText &source,
	const Fn<MarkupRole(int)> &role);

[[nodiscard]] MarkupRestored RestoreMarkup(
	const MarkupMasked &masked,
	const QString &original,
	const QString &translated,
	const std::vector<FreshRecognition::TextSpan> &mapped);

} // namespace Ayu::Translator
