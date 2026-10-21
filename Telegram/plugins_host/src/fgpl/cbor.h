#pragma once

#include "fgpl/result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace fgpl::cbor {

class Value;
struct MapEntry;

using Bytes = std::vector<uint8_t>;
using Array = std::vector<Value>;
using Map = std::vector<MapEntry>;

class Value {
public:
	enum class Type : uint8_t {
		Null,
		Bool,
		Int,
		Float,
		Bytes,
		Text,
		Array,
		Map,
	};

	Value() = default;

	[[nodiscard]] static Value FromBool(bool value);
	[[nodiscard]] static Value FromInt(int64_t value);
	[[nodiscard]] static Value FromFloat(double value);
	[[nodiscard]] static Value FromBytes(Bytes value);
	[[nodiscard]] static Value FromText(std::string value);
	[[nodiscard]] static Value FromArray(Array value);
	[[nodiscard]] static Value FromMap(Map value);

	[[nodiscard]] Type type() const {
		return static_cast<Type>(_data.index());
	}
	[[nodiscard]] bool isNull() const {
		return type() == Type::Null;
	}

	[[nodiscard]] std::optional<bool> asBool() const;
	[[nodiscard]] std::optional<int64_t> asInt() const;
	[[nodiscard]] std::optional<double> asFloat() const;
	[[nodiscard]] const Bytes *asBytes() const;
	[[nodiscard]] const std::string *asText() const;
	[[nodiscard]] const Array *asArray() const;
	[[nodiscard]] const Map *asMap() const;
	[[nodiscard]] std::string *asText();
	[[nodiscard]] Array *asArray();
	[[nodiscard]] Map *asMap();

	[[nodiscard]] const Value *find(std::string_view key) const;

	[[nodiscard]] bool operator==(const Value &other) const;

private:
	std::variant<
		std::monostate,
		bool,
		int64_t,
		double,
		Bytes,
		std::string,
		Array,
		Map> _data;

};

struct MapEntry {
	std::string key;
	Value value;
};

[[nodiscard]] bool operator==(const MapEntry &a, const MapEntry &b);

struct Limits {
	size_t maxDepth = 32;
	size_t maxItems = 65536;
	size_t maxContainer = 16384;
	size_t maxString = 1 << 20;
};

enum class Error : uint8_t {
	Truncated,
	TooLarge,
	TooDeep,
	TooManyItems,
	Unsupported,
	Malformed,
	BadUtf8,
	DuplicateKey,
	KeyNotText,
	IntRange,
	TrailingData,
};

struct Decoded {
	Value value;
	size_t used = 0;
};

[[nodiscard]] Result<Decoded, Error> DecodePrefix(
	std::span<const uint8_t> data,
	const Limits &limits = {});

[[nodiscard]] Result<Value, Error> Decode(
	std::span<const uint8_t> data,
	const Limits &limits = {});

[[nodiscard]] bool Encode(
	const Value &value,
	Bytes &out,
	size_t maxDepth = 32);

[[nodiscard]] bool IsValidUtf8(std::string_view text);

} // namespace fgpl::cbor
