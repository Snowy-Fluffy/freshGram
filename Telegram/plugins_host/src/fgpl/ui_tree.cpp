#include "fgpl/ui_tree.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <optional>
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

// A JPEG is accepted by its signature, the app checks its size before decoding.
[[nodiscard]] std::optional<std::pair<uint32_t, uint32_t>> ImageSize(
		const cbor::Bytes &bytes) {
	static const uint8_t png[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
	if (bytes.size() >= 3
		&& bytes[0] == 0xFF
		&& bytes[1] == 0xD8
		&& bytes[2] == 0xFF) {
		return std::make_pair(uint32_t(0), uint32_t(0));
	} else if (bytes.size() < 24
		|| !std::equal(std::begin(png), std::end(png), bytes.begin())) {
		return std::nullopt;
	}
	const auto read = [&](size_t at) {
		return (uint32_t(bytes[at]) << 24)
			| (uint32_t(bytes[at + 1]) << 16)
			| (uint32_t(bytes[at + 2]) << 8)
			| uint32_t(bytes[at + 3]);
	};
	const auto width = read(16);
	const auto height = read(20);
	if (!width || !height || width > kMaxImageDimension || height > kMaxImageDimension) {
		return std::nullopt;
	}
	return std::make_pair(width, height);
}

} // namespace

bool ValidWindowId(const std::string &id) {
	return id.size() <= 32 && ValidId(id);
}

bool ValidIconName(const std::string &name) {
	return !name.empty()
		&& name.size() <= 32
		&& std::all_of(name.begin(), name.end(), [](char ch) {
			return (ch >= 'a' && ch <= 'z')
				|| (ch >= '0' && ch <= '9')
				|| ch == '_' || ch == '-';
		});
}

bool ValidRichIcon(const std::string &name) {
	const auto colon = name.find(':');
	if (colon == std::string::npos) {
		return false;
	}
	const auto kind = name.substr(0, colon);
	const auto digits = name.substr(colon + 1);
	return (kind == "peer" || kind == "emoji")
		&& !digits.empty()
		&& digits.size() <= 20
		&& std::all_of(digits.begin(), digits.end(), [](char ch) {
			return ch >= '0' && ch <= '9';
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

Result<cbor::Value, std::string> ValidateBadges(const cbor::Value &items) {
	const auto list = items.asArray();
	if (!list || list->size() > kMaxBadges) {
		return std::string("badges are a list of up to 4 entries");
	}
	auto check = Checker();
	auto out = cbor::Array();
	auto ids = std::set<std::string>();
	for (const auto &item : *list) {
		if (!item.asMap()
			|| !check.keys(item, { "id", "icon", "image", "tooltip" })) {
			return check.failed() ? check.error() : std::string("badges must be objects");
		}
		const auto idField = item.find("id");
		const auto id = idField ? idField->asText() : nullptr;
		if (!id || id->size() > 32 || !ValidId(*id) || !ids.insert(*id).second) {
			return std::string("a badge needs a unique id of up to 32 characters");
		}
		const auto icon = item.find("icon");
		const auto image = item.find("image");
		if (!icon == !image) {
			return std::string("a badge has an icon or an image");
		}
		auto entry = cbor::Map();
		entry.push_back({ "id", Text(*id) });
		if (icon) {
			const auto name = icon->asText();
			if (!name || !ValidIconName(*name)) {
				return std::string("a badge has a bad icon name");
			}
			entry.push_back({ "icon", Text(*name) });
		} else {
			const auto bytes = image->asBytes();
			if (!bytes || bytes->empty() || bytes->size() > kMaxBadgeImageBytes) {
				return std::string("a badge image is up to 64 KiB");
			}
			const auto size = ImageSize(*bytes);
			if (!size
				|| size->first > kMaxBadgeImageDimension
				|| size->second > kMaxBadgeImageDimension) {
				return std::string("a badge image is a PNG up to 128x128 or a JPEG");
			}
			entry.push_back({ "image", *image });
		}
		const auto tooltip = check.text(item, "tooltip", kMaxBadgeTooltip, false);
		if (check.failed()) {
			return check.error();
		}
		entry.push_back({ "tooltip", Text(tooltip.value_or("")) });
		out.push_back(cbor::Value::FromMap(std::move(entry)));
	}
	return cbor::Value::FromArray(std::move(out));
}

Result<cbor::Value, std::string> ValidatePage(const cbor::Value &page) {
	auto check = Checker();
	if (!page.asMap() || !check.keys(page, { "title", "items", "buttons" })) {
		return check.failed() ? check.error() : std::string("the page must be an object");
	}
	const auto title = check.text(page, "title", kMaxLabel, true);
	if (!title) {
		return check.error();
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
	auto richIcons = size_t(0);
	const auto takeIcon = [&](const cbor::Value &item, cbor::Map &entry) {
		const auto field = item.find("icon");
		if (!field) {
			return true;
		}
		const auto name = field->asText();
		if (name && ValidRichIcon(*name)) {
			if (++richIcons > kMaxRichIcons) {
				check.fail("a page may use at most 32 peer and emoji icons");
				return false;
			}
		} else if (!name || !ValidIconName(*name)) {
			check.fail("icon must be a lowercase name, peer:<id> or emoji:<id>");
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
	auto nodes = size_t(0);
	std::function<Result<cbor::Value, std::string>(const cbor::Value &, size_t)> parseItem;
	const auto parseList = [&](
			const cbor::Value *list,
			size_t depth,
			cbor::Array &result) -> std::optional<std::string> {
		if (!list || !list->asArray() || list->asArray()->size() > kMaxItems) {
			return std::string("items must be a list of at most 64 entries");
		}
		for (const auto &child : *list->asArray()) {
			auto parsed = parseItem(child, depth);
			if (!parsed.ok()) {
				return parsed.error();
			}
			result.push_back(std::move(parsed.value()));
		}
		return std::nullopt;
	};
	parseItem = [&](const cbor::Value &item, size_t depth) -> Result<cbor::Value, std::string> {
		if (depth > kMaxDepth || ++nodes > kMaxNodes) {
			return std::string("the page is too large");
		}
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
			if (*type == "text" || *type == "header" || *type == "code") {
				const auto styled = (*type == "text");
				const auto text = (styled
					? check.keys(item, { "type", "text", "style" })
					: check.keys(item, { "type", "text" }))
					? check.text(item, "text", (*type == "header") ? kMaxLabel : kMaxText, true, *type != "header")
					: std::nullopt;
				if (!text) {
					return check.error();
				}
				entry.push_back({ "text", Text(*text) });
				if (styled) {
					const auto style = check.text(item, "style", 16, false);
					if (check.failed()) {
						return check.error();
					} else if (style) {
						static const auto known = std::set<std::string>{
							"title", "subtitle", "muted", "accent", "danger",
						};
						if (!known.contains(*style)) {
							return std::string("style must be title, subtitle, muted, accent or danger");
						}
						entry.push_back({ "style", Text(*style) });
					}
				}
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
			} else if (*type == "row" || *type == "column") {
				if (!check.keys(item, { "type", "items" })) {
					return check.error();
				}
				auto children = cbor::Array();
				if (const auto error = parseList(item.find("items"), depth + 1, children)) {
					return *error;
				} else if (children.empty()
					|| (*type == "row" && children.size() > kMaxRowItems)) {
					return std::string("a row holds 1 to 4 items, a column at least one");
				}
				entry.push_back({ "items", cbor::Value::FromArray(std::move(children)) });
			} else if (*type == "page") {
				if (!check.keys(item, { "type", "id", "label", "icon", "hint", "disabled", "items" })
					|| !takeId(item, entry)
					|| !takeIcon(item, entry)) {
					return check.error();
				}
				const auto label = check.text(item, "label", kMaxLabel, true);
				if (!label || !takeCommon(item, entry)) {
					return check.error();
				}
				auto children = cbor::Array();
				if (const auto error = parseList(item.find("items"), depth + 1, children)) {
					return *error;
				} else if (children.empty()) {
					return std::string("a page needs items");
				}
				entry.push_back({ "label", Text(*label) });
				entry.push_back({ "items", cbor::Value::FromArray(std::move(children)) });
			} else if (*type == "card") {
				if (!check.keys(item, { "type", "title", "items" })) {
					return check.error();
				}
				const auto cardTitle = check.text(item, "title", kMaxLabel, false);
				auto children = cbor::Array();
				if (check.failed()) {
					return check.error();
				} else if (const auto error = parseList(item.find("items"), depth + 1, children)) {
					return *error;
				} else if (children.empty()) {
					return std::string("a card needs items");
				}
				entry.push_back({ "title", Text(cardTitle.value_or("")) });
				entry.push_back({ "items", cbor::Value::FromArray(std::move(children)) });
			} else if (*type == "tabs") {
				if (!check.keys(item, { "type", "id", "value", "tabs" }) || !takeId(item, entry)) {
					return check.error();
				}
				const auto tabs = item.find("tabs");
				if (!tabs || !tabs->asArray()
					|| tabs->asArray()->empty()
					|| tabs->asArray()->size() > kMaxTabs) {
					return std::string("tabs needs 1 to 8 tabs");
				}
				auto list = cbor::Array();
				auto tabIds = std::set<std::string>();
				for (const auto &tab : *tabs->asArray()) {
					auto tabEntry = cbor::Map();
					if (!tab.asMap()
						|| !check.keys(tab, { "id", "label", "items" })
						|| !takeId(tab, tabEntry)) {
						return check.failed() ? check.error() : std::string("bad tab");
					}
					const auto tabLabel = check.text(tab, "label", kMaxButtonLabel, true);
					auto children = cbor::Array();
					if (!tabLabel) {
						return check.error();
					} else if (const auto error = parseList(tab.find("items"), depth + 1, children)) {
						return *error;
					}
					tabIds.insert(*tab.find("id")->asText());
					tabEntry.push_back({ "label", Text(*tabLabel) });
					tabEntry.push_back({ "items", cbor::Value::FromArray(std::move(children)) });
					list.push_back(cbor::Value::FromMap(std::move(tabEntry)));
				}
				const auto selected = check.text(item, "value", 64, false);
				if (check.failed()) {
					return check.error();
				} else if (selected && !tabIds.contains(*selected)) {
					return std::string("the value is not one of the tabs");
				}
				entry.push_back({ "value", Text(selected
					? *selected
					: *list.front().find("id")->asText()) });
				entry.push_back({ "tabs", cbor::Value::FromArray(std::move(list)) });
			} else if (*type == "image") {
				if (!check.keys(item, { "type", "data", "width", "height", "alt" })) {
					return check.error();
				}
				const auto data = item.find("data");
				const auto bytes = data ? data->asBytes() : nullptr;
				if (!bytes || bytes->empty() || bytes->size() > kMaxImageBytes) {
					return std::string("an image needs data of up to 256 KiB");
				}
				if (!ImageSize(*bytes)) {
					return std::string("an image must be a PNG up to 2048x2048 or a JPEG");
				}
				const auto width = takeInt(item, "width", 0, kMaxImageSide, 0);
				const auto height = width ? takeInt(item, "height", 0, kMaxImageSide, 0) : std::nullopt;
				const auto alt = height ? check.text(item, "alt", kMaxLabel, false) : std::nullopt;
				if (!height || check.failed()) {
					return check.error();
				}
				entry.push_back({ "data", *data });
				entry.push_back({ "width", cbor::Value::FromInt(*width) });
				entry.push_back({ "height", cbor::Value::FromInt(*height) });
				entry.push_back({ "alt", Text(alt.value_or("")) });
			} else if (*type == "list") {
				if (!check.keys(item, { "type", "items" })) {
					return check.error();
				}
				const auto entries = item.find("items");
				if (!entries || !entries->asArray()
					|| entries->asArray()->empty()
					|| entries->asArray()->size() > kMaxItems) {
					return std::string("a list needs 1 to 64 entries");
				}
				auto list = cbor::Array();
				for (const auto &row : *entries->asArray()) {
					auto rowEntry = cbor::Map();
					if (!row.asMap()
						|| !check.keys(row, { "id", "title", "subtitle", "icon" })
						|| !takeId(row, rowEntry)
						|| !takeIcon(row, rowEntry)) {
						return check.failed() ? check.error() : std::string("bad list entry");
					}
					const auto rowTitle = check.text(row, "title", kMaxLabel, true);
					const auto rowSubtitle = rowTitle
						? check.text(row, "subtitle", kMaxLabel, false)
						: std::nullopt;
					if (check.failed()) {
						return check.error();
					}
					rowEntry.push_back({ "title", Text(*rowTitle) });
					rowEntry.push_back({ "subtitle", Text(rowSubtitle.value_or("")) });
					list.push_back(cbor::Value::FromMap(std::move(rowEntry)));
				}
				entry.push_back({ "items", cbor::Value::FromArray(std::move(list)) });
			} else if (*type == "table") {
				if (!check.keys(item, { "type", "columns", "rows" })) {
					return check.error();
				}
				const auto columns = item.find("columns");
				const auto rows = item.find("rows");
				if (!columns || !columns->asArray()
					|| columns->asArray()->empty()
					|| columns->asArray()->size() > kMaxTableColumns
					|| !rows || !rows->asArray()
					|| rows->asArray()->size() > kMaxTableRows) {
					return std::string("a table needs 1 to 6 columns and at most 32 rows");
				}
				auto head = cbor::Array();
				for (const auto &column : *columns->asArray()) {
					if (!column.asText() || !SafeText(*column.asText(), kMaxButtonLabel, false)) {
						return std::string("a column title is not valid text");
					}
					head.push_back(column);
				}
				auto body = cbor::Array();
				for (const auto &row : *rows->asArray()) {
					if (!row.asArray() || row.asArray()->size() != head.size()) {
						return std::string("every row needs a cell for each column");
					}
					for (const auto &cell : *row.asArray()) {
						if (!cell.asText() || !SafeText(*cell.asText(), kMaxCell, false)) {
							return std::string("a table cell is not valid text");
						}
					}
					body.push_back(row);
				}
				entry.push_back({ "columns", cbor::Value::FromArray(std::move(head)) });
				entry.push_back({ "rows", cbor::Value::FromArray(std::move(body)) });
			} else {
				return std::string("unknown item type ") + (type->size() > 20 ? type->substr(0, 20) : *type);
			}
		return cbor::Value::FromMap(std::move(entry));
	};
	if (const auto error = parseList(page.find("items"), 1, out)) {
		return *error;
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
