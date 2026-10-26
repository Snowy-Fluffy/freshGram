#include "fgpl/ui_tree.h"

#include <algorithm>
#include <limits>
#include <set>
#include <string_view>

namespace fgpl::ui {
namespace {

[[nodiscard]] bool ValidId(const std::string &id) {
	if (id.empty() || id.size() > 64) {
		return false;
	}
	return std::all_of(id.begin(), id.end(), [](char ch) {
		return (ch >= 'a' && ch <= 'z')
			|| (ch >= '0' && ch <= '9')
			|| ch == '_' || ch == '-' || ch == '.';
	});
}

[[nodiscard]] cbor::Value Text(std::string text) {
	return cbor::Value::FromText(std::move(text));
}

class Checker {
public:
	[[nodiscard]] bool keys(
			const cbor::Value &item,
			std::initializer_list<std::string_view> allowed) {
		for (const auto &entry : *item.asMap()) {
			if (std::find(allowed.begin(), allowed.end(), entry.key) == allowed.end()) {
				_error = "unknown field " + entry.key;
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] std::optional<std::string> text(
			const cbor::Value &item,
			const char *name,
			size_t maxBytes,
			bool required,
			bool newlines = false) {
		const auto field = item.find(name);
		if (!field) {
			if (required) {
				_error = std::string(name) + " is required";
			}
			return std::nullopt;
		}
		const auto value = field->asText();
		if (!value || !SafeText(*value, maxBytes, newlines)) {
			_error = std::string(name) + " is not valid text";
			return std::nullopt;
		}
		return *value;
	}

	void fail(std::string error) {
		_error = std::move(error);
	}

	[[nodiscard]] const std::string &error() const {
		return _error;
	}

	[[nodiscard]] bool failed() const {
		return !_error.empty();
	}

private:
	std::string _error;

};

[[nodiscard]] bool ValidUtf8(const std::string &text) {
	for (auto i = size_t(0); i < text.size();) {
		const auto lead = uint8_t(text[i]);
		const auto length = (lead < 0x80)
			? 1
			: ((lead & 0xE0) == 0xC0 && lead >= 0xC2)
			? 2
			: ((lead & 0xF0) == 0xE0)
			? 3
			: ((lead & 0xF8) == 0xF0 && lead <= 0xF4)
			? 4
			: 0;
		if (!length || i + length > text.size()) {
			return false;
		}
		for (auto k = 1; k != length; ++k) {
			if ((uint8_t(text[i + k]) & 0xC0) != 0x80) {
				return false;
			}
		}
		i += length;
	}
	return true;
}

} // namespace

bool ValidIconName(const std::string &name) {
	return !name.empty()
		&& name.size() <= 32
		&& std::all_of(name.begin(), name.end(), [](char ch) {
			return (ch >= 'a' && ch <= 'z')
				|| (ch >= '0' && ch <= '9')
				|| ch == '_' || ch == '-';
		});
}

const std::set<std::string> &BuiltinIcons() {
	static const auto result = std::set<std::string>{
		"add", "archive", "block", "bot", "cancel", "chat", "chats", "clear",
		"collapse", "commands", "copy", "customize", "delete", "devices",
		"download", "edit", "emoji", "expand", "export", "file", "folder",
		"font", "forward", "gif", "hide", "hourglass", "info", "like", "link",
		"lock", "mark_read", "mute", "network", "night", "notifications",
		"palette", "permissions", "phone", "photo", "pin", "premium", "profile",
		"qr", "reactions", "remove", "reply", "report", "restore", "schedule",
		"search", "select", "send", "settings", "share", "show", "silent",
		"sound_off", "sound_on", "spoiler", "star", "stats", "stickers",
		"storage", "timer", "translate", "unmute", "unpin", "users",
		"zoom_in", "zoom_out",
	};
	return result;
}

bool SafeText(const std::string &text, size_t maxBytes, bool allowNewlines) {
	if (text.size() > maxBytes || !ValidUtf8(text)) {
		return false;
	}
	for (auto i = size_t(0); i != text.size(); ++i) {
		const auto code = uint8_t(text[i]);
		if (code < 0x20) {
			if (!(allowNewlines && code == '\n')) {
				return false;
			}
		} else if (code == 0x7F) {
			return false;
		} else if (code == 0xD8 && i + 1 < text.size() && uint8_t(text[i + 1]) == 0x9C) {
			return false;
		} else if (code == 0xE2 && i + 2 < text.size()) {
			const auto second = uint8_t(text[i + 1]);
			const auto third = uint8_t(text[i + 2]);
			if ((second == 0x80 && third >= 0xA8 && third <= 0xAE)
				|| (second == 0x81 && third >= 0xA6 && third <= 0xA9)
				|| (second == 0x80 && (third == 0x8E || third == 0x8F))) {
				return false;
			}
		}
	}
	return true;
}

Result<cbor::Value, std::string> ValidatePage(const cbor::Value &page) {
	auto check = Checker();
	if (!page.asMap() || !check.keys(page, { "title", "items", "buttons" })) {
		return check.failed() ? check.error() : std::string("the page must be an object");
	}
	const auto title = check.text(page, "title", kMaxLabel, true);
	const auto items = page.find("items");
	if (!title) {
		return check.error();
	} else if (!items || !items->asArray() || items->asArray()->size() > kMaxItems) {
		return std::string("items must be a list of at most 64 entries");
	}
	auto out = cbor::Array();
	auto ids = std::set<std::string>();
	const auto takeId = [&](const cbor::Value &item, cbor::Map &entry) -> bool {
		const auto idField = item.find("id");
		const auto id = idField ? idField->asText() : nullptr;
		if (!id || !ValidId(*id) || !ids.insert(*id).second) {
			check.fail("an item has a bad or repeated id");
			return false;
		}
		entry.push_back({ "id", Text(*id) });
		return true;
	};
	const auto takeBool = [&](const cbor::Value &item, const char *name, cbor::Map &entry) {
		const auto field = item.find(name);
		if (field && !field->asBool()) {
			check.fail(std::string(name) + " must be true or false");
			return false;
		}
		entry.push_back({ name, cbor::Value::FromBool(field && *field->asBool()) });
		return true;
	};
	const auto takeInt = [&](
			const cbor::Value &item,
			const char *name,
			int64_t min,
			int64_t max,
			int64_t fallback) -> std::optional<int64_t> {
		const auto field = item.find(name);
		if (!field) {
			return fallback;
		}
		const auto value = field->asInt();
		if (!value || *value < min || *value > max) {
			check.fail(std::string(name) + " is out of range");
			return std::nullopt;
		}
		return *value;
	};
	const auto takeIcon = [&](const cbor::Value &item, cbor::Map &entry) {
		const auto field = item.find("icon");
		if (!field) {
			return true;
		}
		const auto name = field->asText();
		if (!name || !ValidIconName(*name)) {
			check.fail("icon must be a lowercase name");
			return false;
		}
		entry.push_back({ "icon", Text(*name) });
		return true;
	};
	const auto takeCommon = [&](const cbor::Value &item, cbor::Map &entry) {
		const auto hint = check.text(item, "hint", kMaxHint, false, true);
		if (check.failed()) {
			return false;
		} else if (hint) {
			entry.push_back({ "hint", Text(*hint) });
		}
		return takeBool(item, "disabled", entry);
	};
	for (const auto &item : *items->asArray()) {
		if (!item.asMap()) {
			return std::string("every item must be an object");
		}
		const auto typeField = item.find("type");
		const auto type = typeField ? typeField->asText() : nullptr;
		if (!type) {
			return std::string("an item has no type");
		}
		auto entry = cbor::Map();
		entry.push_back({ "type", Text(*type) });
		if (*type == "text" || *type == "header") {
			const auto text = check.keys(item, { "type", "text" })
				? check.text(item, "text", (*type == "text") ? kMaxText : kMaxLabel, true, *type == "text")
				: std::nullopt;
			if (!text) {
				return check.error();
			}
			entry.push_back({ "text", Text(*text) });
		} else if (*type == "divider") {
			if (!check.keys(item, { "type" })) {
				return check.error();
			}
		} else if (*type == "link") {
			if (!check.keys(item, { "type", "label", "url", "icon" }) || !takeIcon(item, entry)) {
				return check.error();
			}
			const auto label = check.text(item, "label", kMaxLabel, true);
			const auto url = label ? check.text(item, "url", kMaxUrl, true) : std::nullopt;
			if (!url) {
				return check.error();
			} else if (url->rfind("https://", 0) != 0 || url->size() <= 8
				|| url->find_first_of(" \"<>\\") != std::string::npos) {
				return std::string("a link needs an https:// url");
			}
			entry.push_back({ "label", Text(*label) });
			entry.push_back({ "url", Text(*url) });
		} else if (*type == "progress") {
			if (!check.keys(item, { "type", "label", "value" })) {
				return check.error();
			}
			const auto label = check.text(item, "label", kMaxLabel, false);
			const auto value = check.failed()
				? std::nullopt
				: takeInt(item, "value", -1, 100, 0);
			if (!value) {
				return check.error();
			}
			entry.push_back({ "label", Text(label.value_or("")) });
			entry.push_back({ "value", cbor::Value::FromInt(*value) });
		} else if (*type == "toggle") {
			if (!check.keys(item, { "type", "id", "label", "value", "hint", "disabled", "icon" })
				|| !takeId(item, entry)
				|| !takeIcon(item, entry)) {
				return check.error();
			}
			const auto label = check.text(item, "label", kMaxLabel, true);
			if (!label || !takeBool(item, "value", entry) || !takeCommon(item, entry)) {
				return check.error();
			}
			entry.push_back({ "label", Text(*label) });
		} else if (*type == "button") {
			if (!check.keys(item, { "type", "id", "label", "hint", "disabled", "style", "icon" })
				|| !takeId(item, entry)
				|| !takeIcon(item, entry)) {
				return check.error();
			}
			const auto label = check.text(item, "label", kMaxLabel, true);
			const auto style = label ? check.text(item, "style", 16, false) : std::nullopt;
			if (!label || !takeCommon(item, entry)) {
				return check.error();
			} else if (style && *style != "default" && *style != "primary" && *style != "danger") {
				return std::string("style must be default, primary or danger");
			}
			entry.push_back({ "label", Text(*label) });
			entry.push_back({ "style", Text(style.value_or("default")) });
		} else if (*type == "input") {
			if (!check.keys(item, {
					"type", "id", "label", "value", "placeholder",
					"multiline", "password", "max", "hint", "disabled" })
				|| !takeId(item, entry)) {
				return check.error();
			}
			const auto max = takeInt(item, "max", 1, int64_t(kMaxLongValue), int64_t(kMaxValue));
			const auto label = max ? check.text(item, "label", kMaxLabel, true) : std::nullopt;
			const auto multiline = item.find("multiline");
			const auto lines = multiline && multiline->asBool().value_or(false);
			const auto value = label
				? check.text(item, "value", size_t(*max), false, lines)
				: std::nullopt;
			const auto placeholder = check.failed()
				? std::nullopt
				: check.text(item, "placeholder", kMaxLabel, false);
			if (check.failed()
				|| !takeBool(item, "multiline", entry)
				|| !takeBool(item, "password", entry)
				|| !takeCommon(item, entry)) {
				return check.error();
			}
			entry.push_back({ "label", Text(*label) });
			entry.push_back({ "value", Text(value.value_or("")) });
			entry.push_back({ "placeholder", Text(placeholder.value_or("")) });
			entry.push_back({ "max", cbor::Value::FromInt(*max) });
		} else if (*type == "slider") {
			if (!check.keys(item, {
					"type", "id", "label", "min", "max", "step", "value", "hint", "disabled" })
				|| !takeId(item, entry)) {
				return check.error();
			}
			const auto label = check.text(item, "label", kMaxLabel, true);
			const auto min = label ? takeInt(item, "min", -kMaxSliderValue, kMaxSliderValue, 0) : std::nullopt;
			const auto max = min ? takeInt(item, "max", -kMaxSliderValue, kMaxSliderValue, 100) : std::nullopt;
			const auto step = max ? takeInt(item, "step", 1, kMaxSliderValue, 1) : std::nullopt;
			if (!step) {
				return check.error();
			} else if (*max - *min < *step || (*max - *min) / *step > kMaxSliderSteps) {
				return std::string("a slider needs at least two positions and at most 1000 steps");
			}
			const auto value = takeInt(item, "value", *min, *max, *min);
			if (!value || !takeCommon(item, entry)) {
				return check.error();
			}
			entry.push_back({ "label", Text(*label) });
			entry.push_back({ "min", cbor::Value::FromInt(*min) });
			entry.push_back({ "max", cbor::Value::FromInt(*max) });
			entry.push_back({ "step", cbor::Value::FromInt(*step) });
			entry.push_back({ "value", cbor::Value::FromInt(*min + (*value - *min) / *step * *step) });
		} else if (*type == "chat") {
			if (!check.keys(item, { "type", "id", "label", "value", "hint", "disabled", "icon" })
				|| !takeId(item, entry)
				|| !takeIcon(item, entry)) {
				return check.error();
			}
			const auto label = check.text(item, "label", kMaxLabel, true);
			const auto value = label
				? takeInt(item, "value", 0, std::numeric_limits<int64_t>::max(), 0)
				: std::nullopt;
			if (!value || !takeCommon(item, entry)) {
				return check.error();
			}
			entry.push_back({ "label", Text(*label) });
			entry.push_back({ "value", cbor::Value::FromInt(*value) });
		} else if (*type == "select") {
			if (!check.keys(item, { "type", "id", "label", "value", "options", "hint", "disabled", "icon" })
				|| !takeId(item, entry)
				|| !takeIcon(item, entry)) {
				return check.error();
			}
			const auto label = check.text(item, "label", kMaxLabel, true);
			const auto options = item.find("options");
			if (!label || !options || !options->asArray()
				|| options->asArray()->empty()
				|| options->asArray()->size() > kMaxOptions) {
				return check.failed() ? check.error() : std::string("a select needs 1 to 16 options");
			}
			auto list = cbor::Array();
			auto values = std::set<std::string>();
			for (const auto &option : *options->asArray()) {
				if (!option.asMap() || !check.keys(option, { "value", "label" })) {
					return check.failed() ? check.error() : std::string("bad option");
				}
				const auto optionValue = check.text(option, "value", 64, true);
				const auto optionLabel = optionValue ? check.text(option, "label", kMaxLabel, true) : std::nullopt;
				if (!optionLabel || !values.insert(*optionValue).second) {
					return check.failed() ? check.error() : std::string("repeated option");
				}
				list.push_back(cbor::Value::FromMap({
					{ "value", Text(*optionValue) },
					{ "label", Text(*optionLabel) },
				}));
			}
			const auto selected = check.text(item, "value", 64, false);
			if (check.failed() || !takeCommon(item, entry)) {
				return check.error();
			}
			if (selected && !values.contains(*selected)) {
				return std::string("the value is not one of the options");
			}
			const auto chosen = selected
				? *selected
				: *list.front().find("value")->asText();
			entry.push_back({ "label", Text(*label) });
			entry.push_back({ "value", Text(chosen) });
			entry.push_back({ "options", cbor::Value::FromArray(std::move(list)) });
		} else {
			return std::string("unknown item type ") + (type->size() > 20 ? type->substr(0, 20) : *type);
		}
		out.push_back(cbor::Value::FromMap(std::move(entry)));
	}
	auto buttons = cbor::Array();
	if (const auto list = page.find("buttons")) {
		if (!list->asArray() || list->asArray()->size() > kMaxButtons) {
			return std::string("buttons must be a list of at most 3 entries");
		}
		for (const auto &button : *list->asArray()) {
			auto entry = cbor::Map();
			if (!button.asMap()
				|| !check.keys(button, { "id", "label", "style" })
				|| !takeId(button, entry)) {
				return check.failed() ? check.error() : std::string("bad button");
			}
			const auto label = check.text(button, "label", kMaxButtonLabel, true);
			const auto style = label ? check.text(button, "style", 16, false) : std::nullopt;
			if (check.failed()) {
				return check.error();
			} else if (style && *style != "default" && *style != "primary" && *style != "danger") {
				return std::string("style must be default, primary or danger");
			}
			entry.push_back({ "label", Text(*label) });
			entry.push_back({ "style", Text(style.value_or("default")) });
			buttons.push_back(cbor::Value::FromMap(std::move(entry)));
		}
	}
	auto result = cbor::Map{
		{ "title", Text(*title) },
		{ "items", cbor::Value::FromArray(std::move(out)) },
	};
	if (!buttons.empty()) {
		result.push_back({ "buttons", cbor::Value::FromArray(std::move(buttons)) });
	}
	return cbor::Value::FromMap(std::move(result));
}

} // namespace fgpl::ui
