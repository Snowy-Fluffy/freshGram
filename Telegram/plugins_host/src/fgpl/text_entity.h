#pragma once

#include "fgpl/cbor.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fgpl {

// A formatted part of a message text, offset and length in UTF-16 units.
struct TextEntity {
	std::string type;
	int64_t offset = 0;
	int64_t length = 0;
	std::string url;
	std::string language;
	int64_t id = 0;
};

constexpr auto kMaxTextEntities = size_t(256);
constexpr auto kMaxEntityUrl = size_t(2048);
constexpr auto kMaxEntityLanguage = size_t(64);

// Telegram finds these in the text itself; they are read, never sent.
[[nodiscard]] inline bool IsDetectedEntity(std::string_view type) {
	return type == "url"
		|| type == "mention"
		|| type == "hashtag"
		|| type == "cashtag"
		|| type == "bot_command"
		|| type == "email"
		|| type == "phone";
}

[[nodiscard]] inline bool KnownEntityType(std::string_view type) {
	return type == "bold"
		|| type == "italic"
		|| type == "underline"
		|| type == "strike"
		|| type == "code"
		|| type == "pre"
		|| type == "spoiler"
		|| type == "blockquote"
		|| type == "text_url"
		|| type == "mention_name"
		|| type == "custom_emoji"
		|| IsDetectedEntity(type);
}

[[nodiscard]] inline bool SafeEntityUrl(std::string_view url) {
	const auto starts = [&](std::string_view prefix) {
		return url.size() > prefix.size() && url.substr(0, prefix.size()) == prefix;
	};
	return url.size() <= kMaxEntityUrl
		&& (starts("https://") || starts("http://") || starts("tg://"));
}

[[nodiscard]] inline std::optional<std::vector<TextEntity>> ParseTextEntities(
		const cbor::Value &value) {
	const auto list = value.asArray();
	if (!list || list->size() > kMaxTextEntities) {
		return std::nullopt;
	}
	auto result = std::vector<TextEntity>();
	for (const auto &item : *list) {
		const auto type = item.find("type") ? item.find("type")->asText() : nullptr;
		const auto offset = item.find("offset") ? item.find("offset")->asInt() : std::nullopt;
		const auto length = item.find("length") ? item.find("length")->asInt() : std::nullopt;
		if (!type || !KnownEntityType(*type) || !offset || !length
			|| *offset < 0 || *length <= 0) {
			return std::nullopt;
		}
		auto entity = TextEntity{ *type, *offset, *length };
		if (const auto url = item.find("url")) {
			if (!url->asText() || !SafeEntityUrl(*url->asText())) {
				return std::nullopt;
			}
			entity.url = *url->asText();
		}
		if (const auto language = item.find("language")) {
			if (!language->asText() || language->asText()->size() > kMaxEntityLanguage) {
				return std::nullopt;
			}
			entity.language = *language->asText();
		}
		if (const auto id = item.find("id")) {
			if (!id->asInt() || *id->asInt() <= 0) {
				return std::nullopt;
			}
			entity.id = *id->asInt();
		}
		if ((entity.type == "text_url" && entity.url.empty())
			|| ((entity.type == "custom_emoji" || entity.type == "mention_name") && !entity.id)) {
			return std::nullopt;
		}
		result.push_back(std::move(entity));
	}
	return result;
}

[[nodiscard]] inline cbor::Value TextEntitiesValue(
		const std::vector<TextEntity> &entities) {
	auto list = cbor::Array();
	for (const auto &entity : entities) {
		auto map = cbor::Map{
			{ "type", cbor::Value::FromText(entity.type) },
			{ "offset", cbor::Value::FromInt(entity.offset) },
			{ "length", cbor::Value::FromInt(entity.length) },
		};
		if (!entity.url.empty()) {
			map.push_back({ "url", cbor::Value::FromText(entity.url) });
		}
		if (!entity.language.empty()) {
			map.push_back({ "language", cbor::Value::FromText(entity.language) });
		}
		if (entity.id) {
			map.push_back({ "id", cbor::Value::FromInt(entity.id) });
		}
		list.push_back(cbor::Value::FromMap(std::move(map)));
	}
	return cbor::Value::FromArray(std::move(list));
}

} // namespace fgpl
