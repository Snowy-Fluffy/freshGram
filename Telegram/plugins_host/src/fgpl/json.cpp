#include "fgpl/json.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <unordered_set>

namespace fgpl::json {
namespace {

class Parser {
public:
	Parser(std::string_view text, const Limits &limits)
	: _text(text)
	, _limits(limits) {
	}

	[[nodiscard]] Result<cbor::Value, std::string> run() {
		auto value = parseValue(0);
		if (!value.ok()) {
			return value;
		}
		skipSpace();
		if (_position != _text.size()) {
			return error("unexpected data after the document");
		}
		return value;
	}

private:
	[[nodiscard]] std::string error(const std::string &what) const {
		return what + " at offset " + std::to_string(_position);
	}

	[[nodiscard]] std::nullopt_t fail(const std::string &what) {
		_failure = error(what);
		return std::nullopt;
	}

	void skipSpace() {
		while (_position < _text.size()
			&& (_text[_position] == ' '
				|| _text[_position] == '\t'
				|| _text[_position] == '\n'
				|| _text[_position] == '\r')) {
			++_position;
		}
	}

	[[nodiscard]] Result<cbor::Value, std::string> parseValue(size_t depth) {
		if (depth > _limits.maxDepth) {
			return error("the document is nested too deeply");
		} else if (++_items > _limits.maxItems) {
			return error("the document has too many values");
		}
		skipSpace();
		if (_position >= _text.size()) {
			return error("unexpected end");
		}
		const auto ch = _text[_position];
		if (ch == '{') {
			return parseObject(depth);
		} else if (ch == '[') {
			return parseArray(depth);
		} else if (ch == '"') {
			auto text = parseString();
			if (!text) {
				return _failure;
			}
			return cbor::Value::FromText(std::move(*text));
		} else if (ch == '-' || (ch >= '0' && ch <= '9')) {
			return parseNumber();
		} else if (_text.substr(_position, 4) == "true") {
			_position += 4;
			return cbor::Value::FromBool(true);
		} else if (_text.substr(_position, 5) == "false") {
			_position += 5;
			return cbor::Value::FromBool(false);
		} else if (_text.substr(_position, 4) == "null") {
			_position += 4;
			return cbor::Value();
		}
		return error("unexpected character");
	}

	[[nodiscard]] Result<cbor::Value, std::string> parseNumber() {
		const auto start = _position;
		if (_text[_position] == '-') {
			++_position;
		}
		if (_position >= _text.size()
			|| _text[_position] < '0'
			|| _text[_position] > '9') {
			return error("a bad number");
		} else if (_text[_position] == '0'
			&& _position + 1 < _text.size()
			&& _text[_position + 1] >= '0'
			&& _text[_position + 1] <= '9') {
			return error("a number with a leading zero");
		}
		auto value = int64_t(0);
		while (_position < _text.size()
			&& _text[_position] >= '0'
			&& _text[_position] <= '9') {
			value = value * 10 + (_text[_position] - '0');
			if (value > kMaxSafeInteger) {
				return error("a number out of range");
			}
			++_position;
		}
		if (_position < _text.size()
			&& (_text[_position] == '.'
				|| _text[_position] == 'e'
				|| _text[_position] == 'E')) {
			return error("only integers are allowed");
		}
		return cbor::Value::FromInt(_text[start] == '-' ? -value : value);
	}

	[[nodiscard]] std::optional<std::string> parseString() {
		++_position;
		auto result = std::string();
		while (true) {
			if (_position >= _text.size()) {
				return fail("an unterminated string");
			}
			const auto ch = static_cast<unsigned char>(_text[_position]);
			if (ch == '"') {
				++_position;
				return result;
			} else if (ch < 0x20) {
				return fail("a control character in a string");
			} else if (ch != '\\') {
				result.push_back(char(ch));
				++_position;
				continue;
			}
			++_position;
			if (_position >= _text.size()) {
				return fail("a bad escape");
			}
			const auto escape = _text[_position++];
			switch (escape) {
			case '"': result.push_back('"'); break;
			case '\\': result.push_back('\\'); break;
			case '/': result.push_back('/'); break;
			case 'b': result.push_back('\b'); break;
			case 'f': result.push_back('\f'); break;
			case 'n': result.push_back('\n'); break;
			case 'r': result.push_back('\r'); break;
			case 't': result.push_back('\t'); break;
			case 'u': {
				auto point = parseHex();
				if (!point.ok()) {
					_failure = point.error();
					return std::nullopt;
				}
				auto code = point.value();
				if (code >= 0xD800 && code <= 0xDBFF) {
					if (_text.substr(_position, 2) != "\\u") {
						return fail("a lone surrogate");
					}
					_position += 2;
					auto low = parseHex();
					if (!low.ok()) {
						_failure = low.error();
						return std::nullopt;
					} else if (low.value() < 0xDC00 || low.value() > 0xDFFF) {
						return fail("a bad surrogate pair");
					}
					code = 0x10000 + ((code - 0xD800) << 10) + (low.value() - 0xDC00);
				} else if (code >= 0xDC00 && code <= 0xDFFF) {
					return fail("a lone surrogate");
				}
				append(result, code);
			} break;
			default:
				return fail("a bad escape");
			}
		}
	}

	[[nodiscard]] Result<uint32_t, std::string> parseHex() {
		if (_position + 4 > _text.size()) {
			return error("a bad unicode escape");
		}
		auto value = uint32_t(0);
		for (auto i = 0; i != 4; ++i) {
			const auto ch = _text[_position++];
			value <<= 4;
			if (ch >= '0' && ch <= '9') {
				value |= uint32_t(ch - '0');
			} else if (ch >= 'a' && ch <= 'f') {
				value |= uint32_t(ch - 'a' + 10);
			} else if (ch >= 'A' && ch <= 'F') {
				value |= uint32_t(ch - 'A' + 10);
			} else {
				return error("a bad unicode escape");
			}
		}
		return value;
	}

	static void append(std::string &out, uint32_t code) {
		if (code < 0x80) {
			out.push_back(char(code));
		} else if (code < 0x800) {
			out.push_back(char(0xC0 | (code >> 6)));
			out.push_back(char(0x80 | (code & 0x3F)));
		} else if (code < 0x10000) {
			out.push_back(char(0xE0 | (code >> 12)));
			out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
			out.push_back(char(0x80 | (code & 0x3F)));
		} else {
			out.push_back(char(0xF0 | (code >> 18)));
			out.push_back(char(0x80 | ((code >> 12) & 0x3F)));
			out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
			out.push_back(char(0x80 | (code & 0x3F)));
		}
	}

	[[nodiscard]] Result<cbor::Value, std::string> parseArray(size_t depth) {
		++_position;
		auto result = cbor::Array();
		skipSpace();
		if (_position < _text.size() && _text[_position] == ']') {
			++_position;
			return cbor::Value::FromArray(std::move(result));
		}
		while (true) {
			auto item = parseValue(depth + 1);
			if (!item.ok()) {
				return item;
			}
			result.push_back(std::move(item.value()));
			skipSpace();
			if (_position >= _text.size()) {
				return error("an unterminated array");
			} else if (_text[_position] == ',') {
				++_position;
			} else if (_text[_position] == ']') {
				++_position;
				return cbor::Value::FromArray(std::move(result));
			} else {
				return error("expected , or ]");
			}
		}
	}

	[[nodiscard]] Result<cbor::Value, std::string> parseObject(size_t depth) {
		++_position;
		auto result = cbor::Map();
		auto seen = std::unordered_set<std::string>();
		skipSpace();
		if (_position < _text.size() && _text[_position] == '}') {
			++_position;
			return cbor::Value::FromMap(std::move(result));
		}
		while (true) {
			skipSpace();
			if (_position >= _text.size() || _text[_position] != '"') {
				return error("expected a key");
			}
			auto key = parseString();
			if (!key) {
				return _failure;
			} else if (!seen.insert(*key).second) {
				return error("a duplicate key '" + *key + "'");
			}
			skipSpace();
			if (_position >= _text.size() || _text[_position] != ':') {
				return error("expected :");
			}
			++_position;
			auto value = parseValue(depth + 1);
			if (!value.ok()) {
				return value;
			}
			result.push_back({ std::move(*key), std::move(value.value()) });
			skipSpace();
			if (_position >= _text.size()) {
				return error("an unterminated object");
			} else if (_text[_position] == ',') {
				++_position;
			} else if (_text[_position] == '}') {
				++_position;
				return cbor::Value::FromMap(std::move(result));
			} else {
				return error("expected , or }");
			}
		}
	}

	std::string_view _text;
	const Limits &_limits;
	size_t _position = 0;
	size_t _items = 0;
	std::string _failure;

};

[[nodiscard]] std::u16string ToUtf16(const std::string &text) {
	auto result = std::u16string();
	for (auto i = size_t(0); i < text.size();) {
		const auto lead = static_cast<unsigned char>(text[i]);
		auto extra = 0;
		auto code = uint32_t(lead);
		if (lead >= 0xF0) {
			extra = 3;
			code = lead & 0x07;
		} else if (lead >= 0xE0) {
			extra = 2;
			code = lead & 0x0F;
		} else if (lead >= 0xC0) {
			extra = 1;
			code = lead & 0x1F;
		}
		for (auto k = 1; k <= extra && i + size_t(k) < text.size(); ++k) {
			code = (code << 6) | (static_cast<unsigned char>(text[i + size_t(k)]) & 0x3F);
		}
		i += size_t(extra) + 1;
		if (code >= 0x10000) {
			code -= 0x10000;
			result.push_back(char16_t(0xD800 + (code >> 10)));
			result.push_back(char16_t(0xDC00 + (code & 0x3FF)));
		} else {
			result.push_back(char16_t(code));
		}
	}
	return result;
}

void WriteString(std::string &out, const std::string &text) {
	out.push_back('"');
	for (const auto raw : text) {
		const auto ch = static_cast<unsigned char>(raw);
		switch (ch) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (ch < 0x20) {
				char buffer[8];
				std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
				out += buffer;
			} else {
				out.push_back(raw);
			}
		}
	}
	out.push_back('"');
}

void WriteValue(std::string &out, const cbor::Value &value) {
	switch (value.type()) {
	case cbor::Value::Type::Null: out += "null"; break;
	case cbor::Value::Type::Bool:
		out += *value.asBool() ? "true" : "false";
		break;
	case cbor::Value::Type::Int:
		out += std::to_string(*value.asInt());
		break;
	case cbor::Value::Type::Text:
		WriteString(out, *value.asText());
		break;
	case cbor::Value::Type::Array: {
		out.push_back('[');
		auto first = true;
		for (const auto &item : *value.asArray()) {
			if (!first) {
				out.push_back(',');
			}
			first = false;
			WriteValue(out, item);
		}
		out.push_back(']');
	} break;
	case cbor::Value::Type::Map: {
		auto entries = std::vector<const cbor::MapEntry *>();
		for (const auto &entry : *value.asMap()) {
			entries.push_back(&entry);
		}
		std::sort(entries.begin(), entries.end(), [](auto a, auto b) {
			return ToUtf16(a->key) < ToUtf16(b->key);
		});
		out.push_back('{');
		auto first = true;
		for (const auto entry : entries) {
			if (!first) {
				out.push_back(',');
			}
			first = false;
			WriteString(out, entry->key);
			out.push_back(':');
			WriteValue(out, entry->value);
		}
		out.push_back('}');
	} break;
	default:
		out += "null";
	}
}

} // namespace

Result<cbor::Value, std::string> Parse(
		std::string_view text,
		const Limits &limits) {
	if (text.size() > limits.maxBytes) {
		return std::string("the document is too large");
	} else if (text.size() >= 3
		&& static_cast<unsigned char>(text[0]) == 0xEF
		&& static_cast<unsigned char>(text[1]) == 0xBB
		&& static_cast<unsigned char>(text[2]) == 0xBF) {
		return std::string("a byte order mark is not allowed");
	} else if (!cbor::IsValidUtf8(text)) {
		return std::string("the document is not valid UTF-8");
	}
	return Parser(text, limits).run();
}

std::string Canonical(const cbor::Value &value) {
	auto result = std::string();
	WriteValue(result, value);
	return result;
}

} // namespace fgpl::json
