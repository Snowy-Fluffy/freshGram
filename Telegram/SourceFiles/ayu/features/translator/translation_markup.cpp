// This is the source code of freshGram.
#include "ayu/features/translator/translation_markup.h"

#include <QtCore/QRegularExpression>

#include <algorithm>

namespace Ayu::Translator {
namespace {

using FreshRecognition::TextSpan;

[[nodiscard]] QRegularExpression PatternFor(const QString &style) {
	return (style == u"["_q)
		? QRegularExpression(u"\\[\\s*(\\d+)\\s*\\]"_q)
		: QRegularExpression(u"~\\s*(\\d+)\\s*~"_q);
}

[[nodiscard]] QString Placeholder(const QString &style, int index) {
	return (style == u"["_q)
		? u"[%1]"_q.arg(index + 1)
		: u"~%1~"_q.arg(index + 1);
}

[[nodiscard]] QString ChooseStyle(const QString &text) {
	for (const auto &style : { u"~"_q, u"["_q }) {
		if (!PatternFor(style).match(text).hasMatch()) {
			return style;
		}
	}
	return QString();
}

struct Mapping {
	int from = 0;
	int till = 0;
	int toFrom = 0;
	int toTill = 0;
};

// A position inside a replaced range goes to its start or to its end.
[[nodiscard]] int Transform(
		const std::vector<Mapping> &list,
		int position,
		bool end) {
	auto shift = 0;
	for (const auto &item : list) {
		if (position <= item.from) {
			break;
		} else if (position >= item.till) {
			shift += (item.toTill - item.toFrom) - (item.till - item.from);
			continue;
		}
		return end ? item.toTill : item.toFrom;
	}
	return position + shift;
}

} // namespace

MarkupMasked MaskMarkup(
		const MarkupText &source,
		const Fn<MarkupRole(int)> &role) {
	auto result = MarkupMasked();
	auto kept = std::vector<MarkupSpan>();
	auto styles = std::vector<MarkupSpan>();
	for (const auto &span : source.spans) {
		if (span.length <= 0
			|| span.offset < 0
			|| span.offset + span.length > source.text.size()) {
			continue;
		}
		switch (role(span.type)) {
		case MarkupRole::Keep: kept.push_back(span); break;
		case MarkupRole::Style: styles.push_back(span); break;
		case MarkupRole::Drop: break;
		}
	}
	std::stable_sort(begin(kept), end(kept), [](const auto &a, const auto &b) {
		return a.offset < b.offset;
	});
	result.style = kept.empty() ? QString() : ChooseStyle(source.text);
	auto mapping = std::vector<Mapping>();
	if (!result.style.isEmpty()) {
		auto from = 0;
		for (const auto &span : kept) {
			if (span.offset < from) {
				continue;
			}
			result.text += source.text.mid(from, span.offset - from);
			const auto placeholder = Placeholder(
				result.style,
				int(result.kept.size()));
			mapping.push_back({
				.from = span.offset,
				.till = span.offset + span.length,
				.toFrom = int(result.text.size()),
				.toTill = int(result.text.size() + placeholder.size()),
			});
			result.text += placeholder;
			result.kept.push_back(span);
			from = span.offset + span.length;
		}
		result.text += source.text.mid(from);
	} else {
		result.text = source.text;
	}
	for (const auto &span : styles) {
		const auto from = Transform(mapping, span.offset, false);
		const auto till = Transform(mapping, span.offset + span.length, true);
		if (till > from) {
			result.spans.push_back({ from, till - from });
			result.styles.push_back(span);
		}
	}
	return result;
}

MarkupRestored RestoreMarkup(
		const MarkupMasked &masked,
		const QString &original,
		const QString &translated,
		const std::vector<TextSpan> &mapped) {
	auto result = MarkupRestored();
	auto &text = result.text.text;
	auto used = std::vector<bool>(masked.kept.size(), false);
	auto mapping = std::vector<Mapping>();
	const auto append = [&](int index) {
		const auto &span = masked.kept[index];
		result.text.spans.push_back({
			.offset = int(text.size()),
			.length = span.length,
			.type = span.type,
			.data = span.data,
		});
		text += original.mid(span.offset, span.length);
		used[index] = true;
	};
	auto from = 0;
	if (!masked.style.isEmpty()) {
		auto i = PatternFor(masked.style).globalMatch(translated);
		while (i.hasNext()) {
			const auto match = i.next();
			const auto index = match.captured(1).toInt() - 1;
			if (index < 0 || index >= int(masked.kept.size()) || used[index]) {
				continue;
			}
			text += translated.mid(from, match.capturedStart() - from);
			const auto before = int(text.size());
			append(index);
			mapping.push_back({
				.from = int(match.capturedStart()),
				.till = int(match.capturedEnd()),
				.toFrom = before,
				.toTill = int(text.size()),
			});
			from = match.capturedEnd();
		}
	}
	text += translated.mid(from);
	for (auto index = 0; index != int(masked.kept.size()); ++index) {
		if (!used[index]) {
			text += ' ';
			append(index);
		}
	}
	for (auto index = 0; index != int(masked.styles.size()); ++index) {
		const auto lost = [&] {
			result.lost.push_back(index);
		};
		if (index >= int(mapped.size()) || mapped[index].length <= 0) {
			lost();
			continue;
		}
		auto start = Transform(mapping, mapped[index].offset, false);
		auto finish = Transform(
			mapping,
			mapped[index].offset + mapped[index].length,
			true);
		while (start < finish && text[start].isSpace()) {
			++start;
		}
		while (finish > start && text[finish - 1].isSpace()) {
			--finish;
		}
		if (finish <= start || finish > text.size()) {
			lost();
			continue;
		}
		const auto &style = masked.styles[index];
		result.text.spans.push_back({
			.offset = start,
			.length = finish - start,
			.type = style.type,
			.data = style.data,
		});
	}
	return result;
}

} // namespace Ayu::Translator
