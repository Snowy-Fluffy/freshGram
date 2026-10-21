#include "fgpl/ui_tree.h"

#include <algorithm>
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

} // namespace

bool SafeText(const std::string &text, size_t maxBytes, bool allowNewlines) {
	if (text.size() > maxBytes) {
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
		} else if (code == 0xE2 && i + 2 < text.size()) {
			const auto second = uint8_t(text[i + 1]);
			const auto third = uint8_t(text[i + 2]);
			if ((second == 0x80 && third >= 0xAA && third <= 0xAE)
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
	if (!page.asMap() || !check.keys(page, { "title", "items" })) {
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
		const auto takeId = [&]() -> bool {
			const auto idField = item.find("id");
			const auto id = idField ? idField->asText() : nullptr;
			if (!id || !ValidId(*id) || !ids.insert(*id).second) {
				check.fail("an item has a bad or repeated id");
				return false;
			}
			entry.push_back({ "id", Text(*id) });
			return true;
		};
		if (*type == "text") {
			const auto text = check.keys(item, { "type", "text" })
				? check.text(item, "text", kMaxText, true, true)
				: std::nullopt;
			if (!text) {
				return check.error();
			}
			entry.push_back({ "text", Text(*text) });
		} else if (*type == "divider") {
			if (!check.keys(item, { "type" })) {
				return check.error();
			}
		} else if (*type == "toggle") {
			if (!check.keys(item, { "type", "id", "label", "value" }) || !takeId()) {
				return check.error();
			}
			const auto label = check.text(item, "label", kMaxLabel, true);
			const auto value = item.find("value");
			if (!label || (value && !value->asBool())) {
				return check.failed() ? check.error() : std::string("bad toggle value");
			}
			entry.push_back({ "label", Text(*label) });
			entry.push_back({ "value", cbor::Value::FromBool(value && *value->asBool()) });
		} else if (*type == "button") {
			if (!check.keys(item, { "type", "id", "label" }) || !takeId()) {
				return check.error();
			}
			const auto label = check.text(item, "label", kMaxLabel, true);
			if (!label) {
				return check.error();
			}
			entry.push_back({ "label", Text(*label) });
		} else if (*type == "input") {
			if (!check.keys(item, { "type", "id", "label", "value", "placeholder" }) || !takeId()) {
				return check.error();
			}
			const auto label = check.text(item, "label", kMaxLabel, true);
			const auto value = label ? check.text(item, "value", kMaxValue, false) : std::nullopt;
			const auto placeholder = check.failed() ? std::nullopt : check.text(item, "placeholder", kMaxLabel, false);
			if (check.failed()) {
				return check.error();
			}
			entry.push_back({ "label", Text(*label) });
			entry.push_back({ "value", Text(value.value_or("")) });
			entry.push_back({ "placeholder", Text(placeholder.value_or("")) });
		} else if (*type == "select") {
			if (!check.keys(item, { "type", "id", "label", "value", "options" }) || !takeId()) {
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
			if (check.failed()) {
				return check.error();
			}
			const auto chosen = selected && values.contains(*selected)
				? *selected
				: *values.begin();
			entry.push_back({ "label", Text(*label) });
			entry.push_back({ "value", Text(chosen) });
			entry.push_back({ "options", cbor::Value::FromArray(std::move(list)) });
		} else {
			return std::string("unknown item type ") + (type->size() > 20 ? type->substr(0, 20) : *type);
		}
		out.push_back(cbor::Value::FromMap(std::move(entry)));
	}
	return cbor::Value::FromMap({
		{ "title", Text(*title) },
		{ "items", cbor::Value::FromArray(std::move(out)) },
	});
}

} // namespace fgpl::ui
