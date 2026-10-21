#pragma once

#include "cbor.h"

#include <initializer_list>
#include <string>
#include <string_view>

namespace fgpl::proto {

[[nodiscard]] inline cbor::Value Map(
		std::initializer_list<cbor::MapEntry> entries) {
	return cbor::Value::FromMap(cbor::Map(entries));
}

[[nodiscard]] inline cbor::Value Text(std::string_view text) {
	return cbor::Value::FromText(std::string(text));
}

[[nodiscard]] inline cbor::Value Int(int64_t number) {
	return cbor::Value::FromInt(number);
}

[[nodiscard]] inline cbor::Value Bool(bool value) {
	return cbor::Value::FromBool(value);
}

[[nodiscard]] inline std::string TypeOf(const cbor::Value &message) {
	const auto found = message.find("t");
	const auto text = found ? found->asText() : nullptr;
	return text ? *text : std::string();
}

[[nodiscard]] inline int64_t IntField(
		const cbor::Value &message,
		std::string_view key,
		int64_t fallback = 0) {
	const auto found = message.find(key);
	const auto number = found ? found->asInt() : std::nullopt;
	return number ? *number : fallback;
}

[[nodiscard]] inline std::string TextField(
		const cbor::Value &message,
		std::string_view key) {
	const auto found = message.find(key);
	const auto text = found ? found->asText() : nullptr;
	return text ? *text : std::string();
}

} // namespace fgpl::proto
