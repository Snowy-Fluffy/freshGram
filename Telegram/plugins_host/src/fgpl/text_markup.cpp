#include "fgpl/text_markup.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <map>

namespace fgpl {
namespace {

constexpr auto kMaxDepth = 16;
constexpr auto kMaxOpenTags = 32;
constexpr auto kMaxLanguage = size_t(32);

struct Out {
	std::string text;
	std::vector<TextEntity> entities;
	int64_t units = 0;

	void append(std::string_view part) {
		text.append(part);
		units += Utf16Length(part);
	}

	[[nodiscard]] bool add(TextEntity entity, int64_t from) {
		if (entity.type.empty() || units <= from) {
			return true;
		} else if (entities.size() >= kMaxTextEntities) {
			return false;
		}
		entity.offset = from;
		entity.length = units - from;
		entities.push_back(std::move(entity));
		return true;
	}
};

[[nodiscard]] TextEntity Typed(const char *type) {
	auto result = TextEntity();
	result.type = type;
	return result;
}

[[nodiscard]] bool AsciiPunct(char c) {
	return (c >= '!' && c <= '/') || (c >= ':' && c <= '@')
		|| (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

[[nodiscard]] bool Digits(std::string_view text) {
	return !text.empty()
		&& text.size() <= 18
		&& std::all_of(text.begin(), text.end(), [](char c) {
			return c >= '0' && c <= '9';
		});
}

[[nodiscard]] int64_t ToNumber(std::string_view text) {
	auto result = int64_t(0);
	std::from_chars(text.data(), text.data() + text.size(), result);
	return result;
}

[[nodiscard]] bool Prefixed(std::string_view text, std::string_view prefix) {
	return text.substr(0, prefix.size()) == prefix;
}

// The id of tg://user?id=N or tg://emoji?id=N, or zero.
[[nodiscard]] int64_t LinkedId(std::string_view url, std::string_view prefix) {
	if (!Prefixed(url, prefix)) {
		return 0;
	}
	const auto digits = url.substr(prefix.size());
	return Digits(digits) ? ToNumber(digits) : 0;
}

[[nodiscard]] bool ValidLanguage(std::string_view language) {
	return !language.empty()
		&& language.size() <= kMaxLanguage
		&& std::all_of(language.begin(), language.end(), [](char c) {
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
				|| (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '#' || c == '.';
		});
}

const char kTooMany[] = "the text has too many formatted parts";

class MarkdownParser {
public:
	MarkdownParser(Out &out, std::string_view source)
	: _out(out)
	, _src(source) {
	}

	[[nodiscard]] std::string run() {
		return range(0, _src.size(), 0);
	}

private:
	struct Open {
		std::string_view mark;
		const char *type = nullptr;
		int64_t start = 0;
	};

	[[nodiscard]] bool starts(size_t at, std::string_view mark) const {
		return _src.compare(at, mark.size(), mark) == 0;
	}

	[[nodiscard]] std::string range(size_t begin, size_t end, int depth) {
		if (depth > kMaxDepth) {
			return "the markup is nested too deep";
		}
		static const auto marks = std::vector<std::pair<std::string_view, const char*>>{
			{ "**", "bold" }, { "__", "italic" }, { "~~", "strike" }, { "||", "spoiler" },
		};
		auto stack = std::vector<Open>();
		auto i = begin;
		while (i < end) {
			const auto c = _src[i];
			if (c == '\\' && i + 1 < end && AsciiPunct(_src[i + 1])) {
				_out.append(_src.substr(i + 1, 1));
				i += 2;
				continue;
			} else if (starts(i, "```")) {
				const auto error = fence(i, end);
				if (!error.empty()) {
					return error;
				}
				continue;
			} else if (c == '`') {
				const auto close = _src.find('`', i + 1);
				if (close != std::string_view::npos && close < end && close > i + 1) {
					const auto from = _out.units;
					_out.append(_src.substr(i + 1, close - i - 1));
					if (!_out.add(Typed("code"), from)) {
						return kTooMany;
					}
					i = close + 1;
					continue;
				}
			} else if (c == '!' && i + 1 < end && _src[i + 1] == '[' && emoji(i, end)) {
				if (!_error.empty()) {
					return _error;
				}
				continue;
			} else if (c == '[') {
				auto error = std::string();
				if (link(i, end, depth, error)) {
					if (!error.empty()) {
						return error;
					}
					continue;
				}
			}
			auto matched = false;
			for (const auto &[mark, type] : marks) {
				if (!starts(i, mark) || i + mark.size() > end) {
					continue;
				}
				matched = true;
				if (!stack.empty() && stack.back().mark == mark) {
					if (!_out.add(Typed(stack.back().type), stack.back().start)) {
						return kTooMany;
					}
					stack.pop_back();
				} else if (std::any_of(stack.begin(), stack.end(), [&](const Open &open) {
					return open.mark == mark;
				})) {
					return std::string(mark) + " closes after formatting that was opened inside it";
				} else {
					const auto closer = _src.find(mark, i + mark.size());
					if (closer != std::string_view::npos && closer < end) {
						stack.push_back({ mark, type, _out.units });
					} else {
						_out.append(mark);
					}
				}
				i += mark.size();
				break;
			}
			if (!matched) {
				_out.append(_src.substr(i, 1));
				++i;
			}
		}
		if (!stack.empty()) {
			return "unclosed " + std::string(stack.back().mark) + " (escape it with a backslash)";
		}
		return {};
	}

	[[nodiscard]] std::string fence(size_t &i, size_t end) {
		const auto close = _src.find("```", i + 3);
		if (close == std::string_view::npos || close >= end) {
			_out.append("```");
			i += 3;
			return {};
		}
		auto body = _src.substr(i + 3, close - i - 3);
		auto language = std::string_view();
		const auto newline = body.find('\n');
		if (newline != std::string_view::npos
			&& (newline == 0 || ValidLanguage(body.substr(0, newline)))) {
			language = body.substr(0, newline);
			body.remove_prefix(newline + 1);
		}
		if (!body.empty() && body.back() == '\n') {
			body.remove_suffix(1);
		}
		const auto from = _out.units;
		_out.append(body);
		auto entity = Typed("pre");
		entity.language = std::string(language);
		i = close + 3;
		return _out.add(std::move(entity), from) ? std::string() : std::string(kTooMany);
	}

	// Handles ![alt](tg://emoji?id=N); leaves the text alone when it is not one.
	[[nodiscard]] bool emoji(size_t &i, size_t end) {
		const auto close = _src.find(']', i + 2);
		if (close == std::string_view::npos
			|| close >= end
			|| close == i + 2
			|| close + 1 >= end
			|| _src[close + 1] != '(') {
			return false;
		}
		const auto paren = _src.find(')', close + 2);
		if (paren == std::string_view::npos || paren >= end) {
			return false;
		}
		const auto id = LinkedId(_src.substr(close + 2, paren - close - 2), "tg://emoji?id=");
		if (!id) {
			return false;
		}
		const auto from = _out.units;
		_out.append(_src.substr(i + 2, close - i - 2));
		auto entity = Typed("custom_emoji");
		entity.id = id;
		if (!_out.add(std::move(entity), from)) {
			_error = kTooMany;
		}
		i = paren + 1;
		return true;
	}

	// Handles [text](url); false when it is not a link, error is set on a failure.
	[[nodiscard]] bool link(size_t &i, size_t end, int depth, std::string &error) {
		auto level = 1;
		auto close = i + 1;
		for (; close < end && level; ++close) {
			if (_src[close] == '\\') {
				++close;
			} else if (_src[close] == '[') {
				++level;
			} else if (_src[close] == ']') {
				--level;
			}
		}
		if (level) {
			return false;
		}
		--close;
		if (close + 1 >= end || _src[close + 1] != '(') {
			return false;
		}
		const auto paren = _src.find(')', close + 2);
		if (paren == std::string_view::npos || paren >= end) {
			return false;
		}
		const auto url = _src.substr(close + 2, paren - close - 2);
		auto entity = Typed("text_url");
		if (const auto id = LinkedId(url, "tg://user?id=")) {
			entity = Typed("mention_name");
			entity.id = id;
		} else if (SafeEntityUrl(url)) {
			entity.url = std::string(url);
		} else {
			return false;
		}
		const auto from = _out.units;
		error = range(i + 1, close, depth + 1);
		if (error.empty() && !_out.add(std::move(entity), from)) {
			error = kTooMany;
		}
		i = paren + 1;
		return true;
	}

	Out &_out;
	std::string_view _src;
	std::string _error;

};

[[nodiscard]] bool FenceToggles(std::string_view line) {
	auto count = 0;
	for (auto at = line.find("```"); at != std::string_view::npos; at = line.find("```", at + 3)) {
		++count;
	}
	return (count % 2) != 0;
}

[[nodiscard]] std::string ParseMarkdown(Out &out, std::string_view source) {
	struct Line {
		std::string_view text;
		bool quote = false;
		bool expandable = false;
	};
	auto lines = std::vector<Line>();
	auto inFence = false;
	auto anyQuote = false;
	for (auto start = size_t(0); start <= source.size();) {
		auto stop = source.find('\n', start);
		if (stop == std::string_view::npos) {
			stop = source.size();
		}
		auto line = Line{ source.substr(start, stop - start) };
		if (!inFence) {
			if (Prefixed(line.text, "**>")) {
				line.quote = line.expandable = true;
				line.text.remove_prefix(3);
			} else if (Prefixed(line.text, ">")) {
				line.quote = true;
				line.text.remove_prefix(1);
			}
			if (line.quote && Prefixed(line.text, " ")) {
				line.text.remove_prefix(1);
			}
		}
		anyQuote = anyQuote || line.quote;
		if (!line.quote && FenceToggles(line.text)) {
			inFence = !inFence;
		}
		lines.push_back(line);
		start = stop + 1;
	}
	if (!anyQuote) {
		return MarkdownParser(out, source).run();
	}
	auto group = size_t(0);
	while (group < lines.size()) {
		auto last = group + 1;
		while (last < lines.size()
			&& lines[last].quote == lines[group].quote
			&& !lines[last].expandable) {
			++last;
		}
		auto body = std::string();
		for (auto i = group; i != last; ++i) {
			if (i != group) {
				body += '\n';
			}
			body += lines[i].text;
		}
		const auto from = out.units;
		if (auto error = MarkdownParser(out, body).run(); !error.empty()) {
			return error;
		}
		if (lines[group].quote) {
			auto entity = Typed("blockquote");
			entity.collapsed = lines[group].expandable;
			if (!out.add(std::move(entity), from)) {
				return kTooMany;
			}
		}
		if (last < lines.size()) {
			out.append("\n");
		}
		group = last;
	}
	return {};
}

void AppendCodePoint(std::string &text, uint32_t code) {
	if (code < 0x80) {
		text += char(code);
	} else if (code < 0x800) {
		text += char(0xC0 | (code >> 6));
		text += char(0x80 | (code & 0x3F));
	} else if (code < 0x10000) {
		text += char(0xE0 | (code >> 12));
		text += char(0x80 | ((code >> 6) & 0x3F));
		text += char(0x80 | (code & 0x3F));
	} else {
		text += char(0xF0 | (code >> 18));
		text += char(0x80 | ((code >> 12) & 0x3F));
		text += char(0x80 | ((code >> 6) & 0x3F));
		text += char(0x80 | (code & 0x3F));
	}
}

// Decodes &name; or &#N; at the start of the text; the length used is 0 when it is no entity.
[[nodiscard]] size_t DecodeReference(std::string_view text, std::string &into) {
	const auto semicolon = text.find(';');
	if (text.empty() || text[0] != '&' || semicolon == std::string_view::npos || semicolon > 10) {
		return 0;
	}
	const auto name = text.substr(1, semicolon - 1);
	static const auto named = std::map<std::string_view, uint32_t>{
		{ "lt", '<' }, { "gt", '>' }, { "amp", '&' }, { "quot", '"' },
		{ "apos", '\'' }, { "nbsp", 0xA0 },
	};
	if (const auto i = named.find(name); i != named.end()) {
		AppendCodePoint(into, i->second);
		return semicolon + 1;
	} else if (name.size() >= 2 && name[0] == '#') {
		const auto hex = (name[1] == 'x' || name[1] == 'X');
		const auto digits = name.substr(hex ? 2 : 1);
		auto code = uint32_t(0);
		const auto parsed = std::from_chars(
			digits.data(),
			digits.data() + digits.size(),
			code,
			hex ? 16 : 10);
		if (digits.empty()
			|| parsed.ec != std::errc()
			|| parsed.ptr != digits.data() + digits.size()
			|| code == 0
			|| code > 0x10FFFF
			|| (code >= 0xD800 && code <= 0xDFFF)) {
			return 0;
		}
		AppendCodePoint(into, code);
		return semicolon + 1;
	}
	return 0;
}

[[nodiscard]] std::string Lower(std::string_view text) {
	auto result = std::string(text);
	for (auto &ch : result) {
		if (ch >= 'A' && ch <= 'Z') {
			ch = char(ch - 'A' + 'a');
		}
	}
	return result;
}

class HtmlParser {
public:
	HtmlParser(Out &out, std::string_view source)
	: _out(out)
	, _src(source) {
	}

	[[nodiscard]] std::string run() {
		auto i = size_t(0);
		while (i < _src.size()) {
			if (_src[i] == '<') {
				if (auto error = tag(i); !error.empty()) {
					return error;
				}
			} else if (_src[i] == '&') {
				auto decoded = std::string();
				const auto used = DecodeReference(_src.substr(i), decoded);
				_out.append(used ? std::string_view(decoded) : _src.substr(i, 1));
				i += used ? used : 1;
			} else {
				const auto stop = _src.find_first_of("<&", i);
				const auto count = (stop == std::string_view::npos ? _src.size() : stop) - i;
				_out.append(_src.substr(i, count));
				i += count;
			}
		}
		if (!_stack.empty()) {
			return "<" + _stack.back().tag + "> is not closed";
		}
		return {};
	}

private:
	struct Element {
		std::string tag;
		int64_t start = 0;
		TextEntity entity;
		bool merged = false;
		bool pre = false;
	};

	using Attributes = std::vector<std::pair<std::string, std::string>>;

	[[nodiscard]] static const std::string *Find(const Attributes &list, std::string_view name) {
		for (const auto &item : list) {
			if (item.first == name) {
				return &item.second;
			}
		}
		return nullptr;
	}

	[[nodiscard]] std::string tag(size_t &i) {
		auto at = i + 1;
		const auto closing = (at < _src.size() && _src[at] == '/');
		if (closing) {
			++at;
		}
		const auto nameStart = at;
		while (at < _src.size()
			&& (std::isalnum(static_cast<unsigned char>(_src[at])) || _src[at] == '-')) {
			++at;
		}
		if (at == nameStart) {
			return "a '<' that starts no tag (write &lt;)";
		}
		const auto name = Lower(_src.substr(nameStart, at - nameStart));
		auto attributes = Attributes();
		auto selfClosing = false;
		while (true) {
			while (at < _src.size() && std::isspace(static_cast<unsigned char>(_src[at]))) {
				++at;
			}
			if (at >= _src.size()) {
				return "the tag <" + name + " is not finished";
			} else if (_src[at] == '>') {
				++at;
				break;
			} else if (_src[at] == '/' && at + 1 < _src.size() && _src[at + 1] == '>') {
				selfClosing = true;
				at += 2;
				break;
			}
			const auto attributeStart = at;
			while (at < _src.size()
				&& (std::isalnum(static_cast<unsigned char>(_src[at])) || _src[at] == '-' || _src[at] == '_')) {
				++at;
			}
			if (at == attributeStart) {
				return "bad attributes in <" + name + ">";
			}
			auto attribute = std::make_pair(Lower(_src.substr(attributeStart, at - attributeStart)), std::string());
			if (at < _src.size() && _src[at] == '=') {
				++at;
				auto raw = std::string_view();
				if (at < _src.size() && (_src[at] == '"' || _src[at] == '\'')) {
					const auto quote = _src[at];
					const auto stop = _src.find(quote, at + 1);
					if (stop == std::string_view::npos) {
						return "an unclosed quote in <" + name + ">";
					}
					raw = _src.substr(at + 1, stop - at - 1);
					at = stop + 1;
				} else {
					const auto stop = _src.find_first_of(" \t\r\n>", at);
					const auto last = (stop == std::string_view::npos) ? _src.size() : stop;
					raw = _src.substr(at, last - at);
					at = last;
				}
				for (auto j = size_t(0); j < raw.size();) {
					const auto used = DecodeReference(raw.substr(j), attribute.second);
					if (used) {
						j += used;
					} else {
						attribute.second += raw[j++];
					}
				}
			}
			if (!Find(attributes, attribute.first)) {
				attributes.push_back(std::move(attribute));
			}
		}
		i = at;
		return closing ? close(name) : open(name, attributes, selfClosing);
	}

	[[nodiscard]] std::string open(
			const std::string &name,
			const Attributes &attributes,
			bool selfClosing) {
		if (name == "br") {
			_out.append("\n");
			return {};
		} else if (_stack.size() >= kMaxOpenTags) {
			return "the tags are nested too deep";
		}
		auto element = Element{ name, _out.units };
		static const auto simple = std::map<std::string, const char*>{
			{ "b", "bold" }, { "strong", "bold" },
			{ "i", "italic" }, { "em", "italic" },
			{ "u", "underline" }, { "ins", "underline" },
			{ "s", "strike" }, { "strike", "strike" }, { "del", "strike" },
			{ "code", "code" },
			{ "tg-spoiler", "spoiler" }, { "spoiler", "spoiler" },
		};
		if (const auto i = simple.find(name); i != simple.end()) {
			element.entity = Typed(i->second);
			if (name == "code" && !_stack.empty() && _stack.back().pre && _stack.back().start == _out.units) {
				element.merged = true;
				if (const auto cls = Find(attributes, "class"); cls && Prefixed(*cls, "language-")) {
					const auto language = std::string_view(*cls).substr(9);
					if (!ValidLanguage(language)) {
						return "a bad code language";
					}
					_stack.back().entity.language = std::string(language);
				}
			}
		} else if (name == "span") {
			const auto cls = Find(attributes, "class");
			if (!cls || *cls != "tg-spoiler") {
				return "<span> is only supported as <span class=\"tg-spoiler\">";
			}
			element.entity = Typed("spoiler");
		} else if (name == "pre") {
			element.entity = Typed("pre");
			element.pre = true;
		} else if (name == "blockquote") {
			element.entity = Typed("blockquote");
			element.entity.collapsed = (Find(attributes, "expandable") != nullptr);
		} else if (name == "a") {
			const auto href = Find(attributes, "href");
			if (!href) {
				return "<a> needs href";
			} else if (const auto id = LinkedId(*href, "tg://user?id=")) {
				element.entity = Typed("mention_name");
				element.entity.id = id;
			} else if (SafeEntityUrl(*href)) {
				element.entity = Typed("text_url");
				element.entity.url = *href;
			} else {
				return "<a> has an unsafe or too long href";
			}
		} else if (name == "emoji" || name == "tg-emoji") {
			const auto id = Find(attributes, (name == "emoji") ? "id" : "emoji-id");
			if (!id || !Digits(*id) || !ToNumber(*id)) {
				return "<" + name + "> needs a numeric emoji id";
			}
			element.entity = Typed("custom_emoji");
			element.entity.id = ToNumber(*id);
		} else {
			return "the tag <" + name + "> is not supported";
		}
		if (selfClosing) {
			return {};
		}
		_stack.push_back(std::move(element));
		return {};
	}

	[[nodiscard]] std::string close(const std::string &name) {
		if (name == "br") {
			return {};
		} else if (_stack.empty() || _stack.back().tag != name) {
			return "</" + name + "> does not match "
				+ (_stack.empty() ? std::string("an open tag") : "<" + _stack.back().tag + ">");
		}
		auto element = std::move(_stack.back());
		_stack.pop_back();
		if (element.merged) {
			return {};
		} else if (element.entity.type == "custom_emoji" && _out.units == element.start) {
			return "an emoji tag needs the emoji inside";
		}
		return _out.add(std::move(element.entity), element.start)
			? std::string()
			: std::string(kTooMany);
	}

	Out &_out;
	std::string_view _src;
	std::vector<Element> _stack;

};

} // namespace

int64_t Utf16Length(std::string_view text) {
	auto result = int64_t(0);
	for (const auto ch : text) {
		const auto byte = static_cast<unsigned char>(ch);
		if ((byte & 0xC0) != 0x80) {
			result += ((byte & 0xF8) == 0xF0) ? 2 : 1;
		}
	}
	return result;
}

std::optional<TextFormat> TextFormatNamed(std::string_view name) {
	if (name == "plain") {
		return TextFormat::Plain;
	} else if (name == "markdown") {
		return TextFormat::Markdown;
	} else if (name == "html") {
		return TextFormat::Html;
	}
	return std::nullopt;
}

Result<MarkedText, std::string> ParseMarkup(TextFormat format, std::string_view source) {
	if (format == TextFormat::Plain) {
		return MarkedText{ std::string(source), {} };
	}
	auto out = Out();
	const auto error = (format == TextFormat::Markdown)
		? ParseMarkdown(out, source)
		: HtmlParser(out, source).run();
	if (!error.empty()) {
		return error;
	}
	std::stable_sort(out.entities.begin(), out.entities.end(), [](const auto &a, const auto &b) {
		return (a.offset != b.offset) ? (a.offset < b.offset) : (a.length > b.length);
	});
	return MarkedText{ std::move(out.text), std::move(out.entities) };
}

} // namespace fgpl
