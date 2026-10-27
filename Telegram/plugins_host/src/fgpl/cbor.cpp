#include "fgpl/cbor.h"

#include <bit>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace fgpl::cbor {
namespace {

constexpr auto kInt64Max = uint64_t(std::numeric_limits<int64_t>::max());

struct Head {
	uint8_t major = 0;
	uint8_t info = 0;
	uint64_t argument = 0;
};

[[nodiscard]] double HalfToDouble(uint16_t bits) {
	const auto sign = (bits & 0x8000) ? -1. : 1.;
	const auto exponent = int((bits >> 10) & 0x1F);
	const auto fraction = int(bits & 0x3FF);
	if (exponent == 0) {
		return sign * std::ldexp(double(fraction), -24);
	} else if (exponent == 31) {
		return fraction
			? std::numeric_limits<double>::quiet_NaN()
			: sign * std::numeric_limits<double>::infinity();
	}
	return sign * std::ldexp(double(fraction | 0x400), exponent - 25);
}

class Reader {
public:
	Reader(std::span<const uint8_t> data, const Limits &limits)
	: _data(data)
	, _limits(limits) {
	}

	[[nodiscard]] Result<Value, Error> item(size_t depth) {
		if (depth > _limits.maxDepth) {
			return Error::TooDeep;
		} else if (++_items > _limits.maxItems) {
			return Error::TooManyItems;
		}
		const auto parsed = head();
		if (!parsed.ok()) {
			return parsed.error();
		}
		const auto [major, info, argument] = parsed.value();
		switch (major) {
		case 0:
			if (info == 31) {
				return Error::Malformed;
			} else if (argument > kInt64Max) {
				return Error::IntRange;
			}
			return Value::FromInt(int64_t(argument));
		case 1:
			if (info == 31) {
				return Error::Malformed;
			} else if (argument > kInt64Max) {
				return Error::IntRange;
			}
			return Value::FromInt(-1 - int64_t(argument));
		case 2:
		case 3: return string(major == 3, info, argument);
		case 4: return array(depth, info, argument);
		case 5: return map(depth, info, argument);
		case 6: return info == 31 ? Error::Malformed : Error::Unsupported;
		}
		return simple(info, argument);
	}

	[[nodiscard]] size_t used() const {
		return _position;
	}

private:
	[[nodiscard]] size_t remaining() const {
		return _data.size() - _position;
	}

	[[nodiscard]] Result<Head, Error> head() {
		if (_position >= _data.size()) {
			return Error::Truncated;
		}
		const auto initial = _data[_position++];
		auto result = Head{
			.major = uint8_t(initial >> 5),
			.info = uint8_t(initial & 31),
		};
		if (result.info < 24) {
			result.argument = result.info;
			return result;
		} else if (result.info == 31) {
			return result;
		} else if (result.info > 27) {
			return Error::Malformed;
		}
		const auto width = size_t(1) << (result.info - 24);
		if (remaining() < width) {
			return Error::Truncated;
		}
		for (auto i = size_t(0); i != width; ++i) {
			result.argument = (result.argument << 8) | _data[_position++];
		}
		return result;
	}

	[[nodiscard]] Result<Value, Error> string(
			bool text,
			uint8_t info,
			uint64_t length) {
		if (info == 31) {
			return Error::Unsupported;
		} else if (length > (text ? _limits.maxString : _limits.maxBytes)) {
			return Error::TooLarge;
		} else if (length > remaining()) {
			return Error::Truncated;
		}
		const auto begin = _data.data() + _position;
		const auto size = size_t(length);
		_position += size;
		if (!text) {
			return Value::FromBytes(Bytes(begin, begin + size));
		}
		auto result = std::string(
			reinterpret_cast<const char *>(begin),
			size);
		if (!IsValidUtf8(result)) {
			return Error::BadUtf8;
		}
		return Value::FromText(std::move(result));
	}

	[[nodiscard]] Result<Value, Error> array(
			size_t depth,
			uint8_t info,
			uint64_t count) {
		if (info == 31) {
			return Error::Unsupported;
		} else if (count > _limits.maxContainer) {
			return Error::TooLarge;
		} else if (count > remaining()) {
			return Error::Truncated;
		}
		auto result = Array();
		result.reserve(size_t(count));
		for (auto i = uint64_t(0); i != count; ++i) {
			auto element = item(depth + 1);
			if (!element.ok()) {
				return element.error();
			}
			result.push_back(std::move(element.value()));
		}
		return Value::FromArray(std::move(result));
	}

	[[nodiscard]] Result<Value, Error> map(
			size_t depth,
			uint8_t info,
			uint64_t count) {
		if (info == 31) {
			return Error::Unsupported;
		} else if (count > _limits.maxContainer) {
			return Error::TooLarge;
		} else if (count > remaining() / 2) {
			return Error::Truncated;
		}
		auto result = Map();
		result.reserve(size_t(count));
		auto seen = std::unordered_set<std::string_view>();
		seen.reserve(size_t(count));
		for (auto i = uint64_t(0); i != count; ++i) {
			if (_position >= _data.size()) {
				return Error::Truncated;
			} else if ((_data[_position] >> 5) != 3) {
				return Error::KeyNotText;
			}
			auto key = item(depth + 1);
			if (!key.ok()) {
				return key.error();
			}
			auto value = item(depth + 1);
			if (!value.ok()) {
				return value.error();
			}
			auto &entry = result.emplace_back(MapEntry{
				.key = std::move(*key.value().asText()),
				.value = std::move(value.value()),
			});
			if (!seen.insert(entry.key).second) {
				return Error::DuplicateKey;
			}
		}
		return Value::FromMap(std::move(result));
	}

	[[nodiscard]] Result<Value, Error> simple(
			uint8_t info,
			uint64_t argument) const {
		switch (info) {
		case 20: return Value::FromBool(false);
		case 21: return Value::FromBool(true);
		case 22: return Value();
		case 25: return Value::FromFloat(
			HalfToDouble(uint16_t(argument)));
		case 26: return Value::FromFloat(
			std::bit_cast<float>(uint32_t(argument)));
		case 27: return Value::FromFloat(std::bit_cast<double>(argument));
		case 31: return Error::Malformed;
		}
		return Error::Unsupported;
	}

	std::span<const uint8_t> _data;
	const Limits &_limits;
	size_t _position = 0;
	size_t _items = 0;

};

void PutHead(Bytes &out, uint8_t major, uint64_t argument) {
	const auto lead = uint8_t(major << 5);
	const auto put = [&](size_t width, uint8_t info) {
		out.push_back(lead | info);
		for (auto shift = int(width) * 8; shift != 0;) {
			shift -= 8;
			out.push_back(uint8_t(argument >> shift));
		}
	};
	if (argument < 24) {
		out.push_back(lead | uint8_t(argument));
	} else if (argument <= 0xFF) {
		put(1, 24);
	} else if (argument <= 0xFFFF) {
		put(2, 25);
	} else if (argument <= 0xFFFFFFFFULL) {
		put(4, 26);
	} else {
		put(8, 27);
	}
}

[[nodiscard]] bool Write(
		const Value &value,
		Bytes &out,
		size_t depth,
		size_t maxDepth) {
	if (depth > maxDepth) {
		return false;
	}
	switch (value.type()) {
	case Value::Type::Null:
		out.push_back(0xF6);
		return true;
	case Value::Type::Bool:
		out.push_back(*value.asBool() ? 0xF5 : 0xF4);
		return true;
	case Value::Type::Int: {
		const auto number = *value.asInt();
		if (number >= 0) {
			PutHead(out, 0, uint64_t(number));
		} else {
			PutHead(out, 1, ~uint64_t(number));
		}
		return true;
	}
	case Value::Type::Float: {
		const auto bits = std::bit_cast<uint64_t>(*value.asFloat());
		out.push_back(0xFB);
		for (auto shift = 64; shift != 0;) {
			shift -= 8;
			out.push_back(uint8_t(bits >> shift));
		}
		return true;
	}
	case Value::Type::Bytes: {
		const auto &bytes = *value.asBytes();
		PutHead(out, 2, bytes.size());
		out.insert(out.end(), bytes.begin(), bytes.end());
		return true;
	}
	case Value::Type::Text: {
		const auto &text = *value.asText();
		if (!IsValidUtf8(text)) {
			return false;
		}
		PutHead(out, 3, text.size());
		out.insert(out.end(), text.begin(), text.end());
		return true;
	}
	case Value::Type::Array: {
		const auto &array = *value.asArray();
		PutHead(out, 4, array.size());
		for (const auto &element : array) {
			if (!Write(element, out, depth + 1, maxDepth)) {
				return false;
			}
		}
		return true;
	}
	case Value::Type::Map: {
		const auto &map = *value.asMap();
		PutHead(out, 5, map.size());
		for (const auto &entry : map) {
			if (!IsValidUtf8(entry.key)) {
				return false;
			}
			PutHead(out, 3, entry.key.size());
			out.insert(out.end(), entry.key.begin(), entry.key.end());
			if (!Write(entry.value, out, depth + 1, maxDepth)) {
				return false;
			}
		}
		return true;
	}
	}
	return false;
}

} // namespace

Value Value::FromBool(bool value) {
	auto result = Value();
	result._data.emplace<bool>(value);
	return result;
}

Value Value::FromInt(int64_t value) {
	auto result = Value();
	result._data.emplace<int64_t>(value);
	return result;
}

Value Value::FromFloat(double value) {
	auto result = Value();
	result._data.emplace<double>(value);
	return result;
}

Value Value::FromBytes(Bytes value) {
	auto result = Value();
	result._data.emplace<Bytes>(std::move(value));
	return result;
}

Value Value::FromText(std::string value) {
	auto result = Value();
	result._data.emplace<std::string>(std::move(value));
	return result;
}

Value Value::FromArray(Array value) {
	auto result = Value();
	result._data.emplace<Array>(std::move(value));
	return result;
}

Value Value::FromMap(Map value) {
	auto result = Value();
	result._data.emplace<Map>(std::move(value));
	return result;
}

std::optional<bool> Value::asBool() const {
	if (const auto value = std::get_if<bool>(&_data)) {
		return *value;
	}
	return std::nullopt;
}

std::optional<int64_t> Value::asInt() const {
	if (const auto value = std::get_if<int64_t>(&_data)) {
		return *value;
	}
	return std::nullopt;
}

std::optional<double> Value::asFloat() const {
	if (const auto value = std::get_if<double>(&_data)) {
		return *value;
	}
	return std::nullopt;
}

const Bytes *Value::asBytes() const {
	return std::get_if<Bytes>(&_data);
}

const std::string *Value::asText() const {
	return std::get_if<std::string>(&_data);
}

const Array *Value::asArray() const {
	return std::get_if<Array>(&_data);
}

const Map *Value::asMap() const {
	return std::get_if<Map>(&_data);
}

std::string *Value::asText() {
	return std::get_if<std::string>(&_data);
}

Array *Value::asArray() {
	return std::get_if<Array>(&_data);
}

Map *Value::asMap() {
	return std::get_if<Map>(&_data);
}

const Value *Value::find(std::string_view key) const {
	if (const auto map = asMap()) {
		for (const auto &entry : *map) {
			if (entry.key == key) {
				return &entry.value;
			}
		}
	}
	return nullptr;
}

bool Value::operator==(const Value &other) const {
	return _data == other._data;
}

bool operator==(const MapEntry &a, const MapEntry &b) {
	return a.key == b.key && a.value == b.value;
}

Result<Decoded, Error> DecodePrefix(
		std::span<const uint8_t> data,
		const Limits &limits) {
	auto reader = Reader(data, limits);
	auto parsed = reader.item(0);
	if (!parsed.ok()) {
		return parsed.error();
	}
	return Decoded{ std::move(parsed.value()), reader.used() };
}

Result<Value, Error> Decode(
		std::span<const uint8_t> data,
		const Limits &limits) {
	auto decoded = DecodePrefix(data, limits);
	if (!decoded.ok()) {
		return decoded.error();
	} else if (decoded.value().used != data.size()) {
		return Error::TrailingData;
	}
	return std::move(decoded.value().value);
}

bool Encode(const Value &value, Bytes &out, size_t maxDepth) {
	const auto size = out.size();
	if (Write(value, out, 0, maxDepth)) {
		return true;
	}
	out.resize(size);
	return false;
}

bool IsValidUtf8(std::string_view text) {
	const auto data = reinterpret_cast<const uint8_t *>(text.data());
	const auto size = text.size();
	auto i = size_t(0);
	while (i != size) {
		const auto lead = data[i];
		if (lead < 0x80) {
			++i;
			continue;
		}
		auto extra = size_t(0);
		auto minimum = uint32_t(0);
		auto point = uint32_t(0);
		if ((lead & 0xE0) == 0xC0) {
			extra = 1;
			minimum = 0x80;
			point = lead & 0x1F;
		} else if ((lead & 0xF0) == 0xE0) {
			extra = 2;
			minimum = 0x800;
			point = lead & 0x0F;
		} else if ((lead & 0xF8) == 0xF0) {
			extra = 3;
			minimum = 0x10000;
			point = lead & 0x07;
		} else {
			return false;
		}
		if (size - i <= extra) {
			return false;
		}
		for (auto k = size_t(1); k <= extra; ++k) {
			if ((data[i + k] & 0xC0) != 0x80) {
				return false;
			}
			point = (point << 6) | (data[i + k] & 0x3F);
		}
		if (point < minimum
			|| point > 0x10FFFF
			|| (point >= 0xD800 && point <= 0xDFFF)) {
			return false;
		}
		i += extra + 1;
	}
	return true;
}

} // namespace fgpl::cbor
