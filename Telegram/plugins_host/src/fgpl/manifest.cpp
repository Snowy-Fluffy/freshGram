#include "fgpl/manifest.h"

#include "fgpl/base64.h"
#include "fgpl/json.h"
#include "fgpl/net_policy.h"
#include "fgpl/vfs.h"

#include <algorithm>
#include <cctype>

namespace fgpl {
namespace {

const auto kTopKeys = std::set<std::string>{
	"schema", "id", "name", "version", "description", "author", "license",
	"homepage", "repository", "support", "changelog", "icon", "categories",
	"keywords", "locales", "compat", "runtime", "activation", "permissions",
	"contributes", "dependencies", "conflicts", "storage", "updates", "files",
	"signature",
};

[[nodiscard]] bool LowerToken(const std::string &text, size_t maxSize) {
	if (text.empty() || text.size() > maxSize) {
		return false;
	}
	for (const auto ch : text) {
		if (!((ch >= 'a' && ch <= 'z')
			|| (ch >= '0' && ch <= '9')
			|| ch == '-')) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool ValidId(const std::string &id) {
	if (id.size() < 5 || id.size() > 64 || id.find('.') == std::string::npos) {
		return false;
	}
	auto previous = '.';
	for (const auto ch : id) {
		const auto separator = (ch == '.' || ch == '-');
		const auto good = separator
			|| (ch >= 'a' && ch <= 'z')
			|| (ch >= '0' && ch <= '9');
		if (!good || (separator && (previous == '.' || previous == '-'))) {
			return false;
		}
		previous = ch;
	}
	return previous != '.' && previous != '-';
}

[[nodiscard]] bool ValidLanguage(const std::string &code) {
	if (code.size() < 2 || code.size() > 12) {
		return false;
	}
	for (auto i = size_t(0); i != code.size(); ++i) {
		const auto ch = code[i];
		const auto letter = ch >= 'a' && ch <= 'z';
		const auto other = (ch >= 'A' && ch <= 'Z')
			|| (ch >= '0' && ch <= '9')
			|| ch == '-';
		if (!(letter || (i >= 2 && other))) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool ValidPath(const std::string &path, const char *extension) {
	if (path.empty() || path.size() > 200) {
		return false;
	}
	auto start = size_t(0);
	while (true) {
		const auto slash = path.find('/', start);
		const auto part = path.substr(
			start,
			slash == std::string::npos ? std::string::npos : slash - start);
		if (!IsValidName(part, 128)) {
			return false;
		} else if (slash == std::string::npos) {
			break;
		}
		start = slash + 1;
	}
	return !extension
		|| (path.size() > std::string(extension).size()
			&& path.ends_with(extension));
}

class Reader {
public:
	[[nodiscard]] std::vector<std::string> &errors() {
		return _errors;
	}

	void error(const std::string &where, const std::string &what) {
		if (_errors.size() < 50) {
			_errors.push_back(where + ": " + what);
		}
	}

	[[nodiscard]] const cbor::Value *object(
			const cbor::Value &parent,
			const char *key,
			const std::string &where,
			bool required,
			const std::set<std::string> &allowed) {
		const auto value = parent.find(key);
		const auto name = where.empty() ? std::string(key) : where + "." + key;
		if (!value) {
			if (required) {
				error(name, "is required");
			}
			return nullptr;
		} else if (!value->asMap()) {
			error(name, "must be an object");
			return nullptr;
		}
		checkKeys(*value, name, allowed);
		return value;
	}

	void checkKeys(
			const cbor::Value &value,
			const std::string &where,
			const std::set<std::string> &allowed) {
		for (const auto &entry : *value.asMap()) {
			if (!allowed.contains(entry.key)
				&& entry.key.rfind("x-", 0) != 0) {
				error(where, "unknown key '" + entry.key + "'");
			}
		}
	}

	[[nodiscard]] std::optional<std::string> text(
			const cbor::Value &parent,
			const char *key,
			const std::string &where,
			bool required,
			size_t maxSize,
			bool allowEmpty = false) {
		const auto value = parent.find(key);
		const auto name = where.empty() ? std::string(key) : where + "." + key;
		if (!value) {
			if (required) {
				error(name, "is required");
			}
			return std::nullopt;
		}
		const auto text = value->asText();
		if (!text) {
			error(name, "must be a string");
			return std::nullopt;
		} else if (text->size() > maxSize || (text->empty() && !allowEmpty)) {
			error(name, "has a bad length");
			return std::nullopt;
		}
		return *text;
	}

	[[nodiscard]] std::optional<int64_t> integer(
			const cbor::Value &parent,
			const char *key,
			const std::string &where,
			int64_t low,
			int64_t high) {
		const auto value = parent.find(key);
		if (!value) {
			return std::nullopt;
		}
		const auto name = where.empty() ? std::string(key) : where + "." + key;
		const auto number = value->asInt();
		if (!number || *number < low || *number > high) {
			error(name, "must be an integer from "
				+ std::to_string(low) + " to " + std::to_string(high));
			return std::nullopt;
		}
		return number;
	}

	[[nodiscard]] std::optional<bool> boolean(
			const cbor::Value &parent,
			const char *key,
			const std::string &where) {
		const auto value = parent.find(key);
		if (!value) {
			return std::nullopt;
		}
		const auto flag = value->asBool();
		if (!flag) {
			error(where.empty() ? std::string(key) : where + "." + key, "must be true or false");
		}
		return flag;
	}

	[[nodiscard]] const cbor::Array *array(
			const cbor::Value &parent,
			const char *key,
			const std::string &where,
			size_t maxCount) {
		const auto value = parent.find(key);
		if (!value) {
			return nullptr;
		}
		const auto name = where.empty() ? std::string(key) : where + "." + key;
		const auto list = value->asArray();
		if (!list || list->size() > maxCount) {
			error(name, "must be a list of at most " + std::to_string(maxCount));
			return nullptr;
		}
		return list;
	}

	[[nodiscard]] std::optional<Localized> localized(
			const cbor::Value &parent,
			const char *key,
			const std::string &where,
			bool required,
			size_t maxSize) {
		const auto value = parent.find(key);
		const auto name = where.empty() ? std::string(key) : where + "." + key;
		if (!value) {
			if (required) {
				error(name, "is required");
			}
			return std::nullopt;
		}
		auto result = Localized();
		if (const auto plain = value->asText()) {
			if (plain->empty() || plain->size() > maxSize) {
				error(name, "has a bad length");
				return std::nullopt;
			}
			result.text["default"] = *plain;
			return result;
		}
		const auto map = value->asMap();
		if (!map || map->size() > 32) {
			error(name, "must be a string or a map of languages");
			return std::nullopt;
		}
		for (const auto &entry : *map) {
			const auto line = entry.value.asText();
			if ((entry.key != "default" && !ValidLanguage(entry.key))
				|| !line
				|| line->empty()
				|| line->size() > maxSize) {
				error(name, "has a bad entry '" + entry.key + "'");
				return std::nullopt;
			}
			result.text[entry.key] = *line;
		}
		if (!result.text.contains("default")) {
			error(name, "needs a 'default' text");
			return std::nullopt;
		}
		return result;
	}

	[[nodiscard]] std::optional<std::string> url(
			const cbor::Value &parent,
			const char *key,
			const std::string &where) {
		const auto text = this->text(parent, key, where, false, 500);
		if (text && !net::ParseUrl(*text).ok()) {
			error(where.empty() ? std::string(key) : where + "." + key, "must be a valid https URL");
			return std::nullopt;
		}
		return text;
	}

private:
	std::vector<std::string> _errors;

};

[[nodiscard]] bool FeatureToken(const std::string &text) {
	if (text.empty() || text.size() > 64) {
		return false;
	}
	for (const auto ch : text) {
		if (!((ch >= 'a' && ch <= 'z')
			|| (ch >= '0' && ch <= '9')
			|| ch == '-' || ch == '.' || ch == '_')) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] std::vector<std::string> Tokens(
		Reader &reader,
		const cbor::Value &parent,
		const char *key,
		const std::string &where,
		size_t maxCount,
		size_t maxSize,
		bool feature = false) {
	auto result = std::vector<std::string>();
	const auto list = reader.array(parent, key, where, maxCount);
	for (const auto &item : list ? *list : cbor::Array()) {
		const auto text = item.asText();
		if (!text || !(feature ? FeatureToken(*text) : LowerToken(*text, maxSize))) {
			reader.error(where.empty() ? key : where + "." + key, "has a bad entry");
		} else {
			result.push_back(*text);
		}
	}
	return result;
}

void ReadDependencies(
		Reader &reader,
		const cbor::Value &document,
		const char *key,
		bool needRange,
		const std::string &ownId,
		std::vector<Dependency> &out) {
	const auto list = reader.array(document, key, "", 16);
	for (const auto &item : list ? *list : cbor::Array()) {
		if (!item.asMap()) {
			reader.error(key, "entries must be objects");
			continue;
		}
		reader.checkKeys(item, key, { "id", "version" });
		auto dependency = Dependency();
		const auto id = reader.text(item, "id", key, true, 64);
		if (!id || !ValidId(*id)) {
			reader.error(key, "has a bad id");
			continue;
		} else if (*id == ownId) {
			reader.error(key, "refers to the plugin itself");
			continue;
		}
		dependency.id = *id;
		const auto range = reader.text(item, "version", key, needRange, 128);
		if (range) {
			dependency.range = VersionRange::Parse(*range);
			if (!dependency.range) {
				reader.error(key, "has a bad version range");
				continue;
			}
		}
		out.push_back(std::move(dependency));
	}
}

} // namespace

std::string Localized::get(const std::string &language) const {
	const auto i = text.find(language);
	if (i != text.end()) {
		return i->second;
	}
	const auto dash = language.find('-');
	if (dash != std::string::npos) {
		const auto base = text.find(language.substr(0, dash));
		if (base != text.end()) {
			return base->second;
		}
	}
	const auto fallback = text.find("default");
	return fallback != text.end() ? fallback->second : std::string();
}

bool Outdated(const Manifest &manifest, const HostEnvironment &environment) {
	if (!manifest.testedWith) {
		return false;
	}
	const auto &tested = *manifest.testedWith;
	const auto &app = environment.app;
	return (app.major != tested.major)
		? (app.major > tested.major)
		: (app.minor > tested.minor);
}

const std::set<std::string> &KnownPermissions() {
	static const auto known = std::set<std::string>{
		"messages.read", "messages.read_deleted", "messages.send",
		"messages.transform_outgoing", "media.read", "chats.list", "chat.info",
		"messages.history", "account.info", "stars.balance", "composer.draft",
		"contacts.list", "settings.read", "folders.list", "ui.menu",
		"ui.command", "ui.settings_page", "ui.composer_button", "ui.dialog",
		"ui.toast", "notifications", "net.https", "net.tcp", "net.udp", "storage.private",
		"plugins.message",
		"fs.user_selected", "clipboard", "background",
	};
	return known;
}

std::string SignedText(const cbor::Value &document) {
	auto stripped = cbor::Map();
	for (const auto &entry : *document.asMap()) {
		if (entry.key != "signature") {
			stripped.push_back(entry);
		}
	}
	return json::Canonical(cbor::Value::FromMap(std::move(stripped)));
}

Result<Manifest, std::vector<std::string>> ParseManifest(
		const cbor::Value &document) {
	auto reader = Reader();
	auto manifest = Manifest();
	if (!document.asMap()) {
		return std::vector<std::string>{ "the manifest must be an object" };
	}
	reader.checkKeys(document, "manifest", kTopKeys);

	if (reader.integer(document, "schema", "", 1, 1).value_or(0) != 1) {
		reader.error("schema", "must be 1");
	}
	if (const auto id = reader.text(document, "id", "", true, 64)) {
		if (!ValidId(*id)) {
			reader.error("id", "must be a reverse-DNS name in lowercase");
		}
		manifest.id = *id;
	}
	if (const auto name = reader.localized(document, "name", "", true, 80)) {
		manifest.name = *name;
	}
	if (const auto version = reader.text(document, "version", "", true, 64)) {
		if (const auto parsed = ParseVersion(*version)) {
			manifest.version = *parsed;
		} else {
			reader.error("version", "must be a semantic version");
		}
	}
	if (const auto description = reader.localized(document, "description", "", false, 1000)) {
		manifest.description = *description;
	}
	if (const auto author = reader.object(document, "author", "", false, { "name", "url", "contact" })) {
		manifest.authorName = reader.text(*author, "name", "author", false, 100).value_or("");
		manifest.authorUrl = reader.url(*author, "url", "author").value_or("");
		manifest.authorContact = reader.text(*author, "contact", "author", false, 200).value_or("");
	}
	manifest.license = reader.text(document, "license", "", false, 64).value_or("");
	manifest.homepage = reader.url(document, "homepage", "").value_or("");
	manifest.repository = reader.url(document, "repository", "").value_or("");
	manifest.support = reader.url(document, "support", "").value_or("");
	for (const auto &item : { std::pair{ "changelog", &manifest.changelog }, std::pair{ "icon", &manifest.icon } }) {
		if (const auto path = reader.text(document, item.first, "", false, 200)) {
			if (!ValidPath(*path, nullptr)) {
				reader.error(item.first, "must be a safe relative path");
			}
			*item.second = *path;
		}
	}
	manifest.categories = Tokens(reader, document, "categories", "", 8, 32);
	manifest.keywords = Tokens(reader, document, "keywords", "", 16, 32);
	const auto locales = reader.array(document, "locales", "", 64);
	for (const auto &item : locales ? *locales : cbor::Array()) {
		const auto code = item.asText();
		if (!code || !ValidLanguage(*code)) {
			reader.error("locales", "has a bad language code");
		} else {
			manifest.locales.push_back(*code);
		}
	}

	if (const auto compat = reader.object(document, "compat", "", true, {
			"freshgram", "testedWith", "api", "platforms", "platformVersions",
			"requires", "accounts" })) {
		if (const auto range = reader.text(*compat, "freshgram", "compat", true, 128)) {
			if (const auto parsed = VersionRange::Parse(*range)) {
				manifest.appRange = *parsed;
			} else {
				reader.error("compat.freshgram", "must be a version range such as '>=7.6.0 <8.0.0'");
			}
		}
		if (const auto tested = reader.text(*compat, "testedWith", "compat", false, 64)) {
			manifest.testedWith = ParseVersion(*tested);
			if (!manifest.testedWith) {
				reader.error("compat.testedWith", "must be a semantic version");
			}
		}
		if (const auto api = compat->find("api")) {
			if (const auto single = api->asInt()) {
				manifest.apiMin = manifest.apiMax = int(std::clamp<int64_t>(*single, 0, 100000));
				if (*single < 1 || *single > 1000) {
					reader.error("compat.api", "must be from 1 to 1000");
				}
			} else if (api->asMap()) {
				reader.checkKeys(*api, "compat.api", { "min", "max" });
				manifest.apiMin = int(reader.integer(*api, "min", "compat.api", 1, 1000).value_or(1));
				manifest.apiMax = int(reader.integer(*api, "max", "compat.api", 1, 1000).value_or(manifest.apiMin));
				if (manifest.apiMax < manifest.apiMin) {
					reader.error("compat.api", "max is below min");
				}
			} else {
				reader.error("compat.api", "must be a number or {min, max}");
			}
		}
		if (const auto list = reader.array(*compat, "platforms", "compat", 3)) {
			manifest.platforms.clear();
			for (const auto &item : *list) {
				const auto name = item.asText();
				if (!name || (*name != "linux" && *name != "windows" && *name != "macos")
					|| !manifest.platforms.insert(*name).second) {
					reader.error("compat.platforms", "must list linux, windows and macos once each");
				}
			}
			if (manifest.platforms.empty()) {
				reader.error("compat.platforms", "must not be empty");
			}
		}
		if (const auto versions = reader.object(*compat, "platformVersions", "compat", false, { "linux", "windows", "macos" })) {
			for (const auto &entry : *versions->asMap()) {
				const auto range = entry.value.asText();
				const auto parsed = range ? VersionRange::Parse(*range) : std::nullopt;
				if (!parsed || entry.key.rfind("x-", 0) == 0) {
					reader.error("compat.platformVersions." + entry.key, "must be a version range");
				} else {
					manifest.platformVersions.emplace(entry.key, *parsed);
				}
			}
		}
		manifest.needs = Tokens(reader, *compat, "requires", "compat", 32, 64, true);
		if (const auto accounts = reader.object(*compat, "accounts", "compat", false, { "premium" })) {
			manifest.premiumOnly = reader.boolean(*accounts, "premium", "compat.accounts").value_or(false);
		}
	}

	if (const auto runtime = reader.object(document, "runtime", "", true, {
			"kind", "module", "entry", "language", "abi", "pythonVersion", "limits" })) {
		manifest.kind = reader.text(*runtime, "kind", "runtime", true, 16).value_or("");
		if (manifest.kind == "wasm") {
			manifest.module = reader.text(*runtime, "module", "runtime", true, 200).value_or("");
			if (!manifest.module.empty() && !ValidPath(manifest.module, ".wasm")) {
				reader.error("runtime.module", "must be a safe path to a .wasm file");
			}
			if (runtime->find("entry")) {
				reader.error("runtime.entry", "is only for python plugins");
			}
		} else if (manifest.kind == "python") {
			manifest.entry = reader.text(*runtime, "entry", "runtime", true, 200).value_or("");
			if (!manifest.entry.empty() && !ValidPath(manifest.entry, ".py")) {
				reader.error("runtime.entry", "must be a safe path to a .py file");
			}
			manifest.pythonVersion = reader.text(*runtime, "pythonVersion", "runtime", false, 32).value_or("");
			if (runtime->find("module")) {
				reader.error("runtime.module", "is only for wasm plugins");
			}
		} else if (!manifest.kind.empty()) {
			reader.error("runtime.kind", "must be 'wasm' or 'python'");
		}
		manifest.language = reader.text(*runtime, "language", "runtime", false, 32).value_or("");
		if (reader.integer(*runtime, "abi", "runtime", 1, 1000).value_or(1) != 1) {
			reader.error("runtime.abi", "unsupported ABI");
		}
		if (const auto limits = reader.object(*runtime, "limits", "runtime", false, {
				"memoryMb", "eventTimeoutMs", "storageMb" })) {
			manifest.memoryMb = int(reader.integer(*limits, "memoryMb", "runtime.limits", 1, 1024).value_or(64));
			manifest.eventTimeoutMs = int(reader.integer(*limits, "eventTimeoutMs", "runtime.limits", 10, 60000).value_or(500));
			manifest.storageMb = int(reader.integer(*limits, "storageMb", "runtime.limits", 0, 1024).value_or(10));
		}
	}

	if (const auto activation = reader.object(document, "activation", "", false, { "on", "background" })) {
		if (const auto list = reader.array(*activation, "on", "activation", 8)) {
			manifest.activation.clear();
			for (const auto &item : *list) {
				const auto name = item.asText();
				if (!name
					|| (*name != "startup" && *name != "command" && *name != "event"
						&& *name != "menu" && *name != "settings" && *name != "message")) {
					reader.error("activation.on", "has an unknown trigger");
				} else {
					manifest.activation.insert(*name);
				}
			}
		}
		manifest.background = reader.boolean(*activation, "background", "activation").value_or(false);
	}

	if (const auto permissions = reader.object(document, "permissions", "", false, { "required", "optional" })) {
		auto seen = std::set<std::string>();
		for (const auto required : { true, false }) {
			const auto key = required ? "required" : "optional";
			const auto list = reader.array(*permissions, key, "permissions", 64);
			for (const auto &item : list ? *list : cbor::Array()) {
				const auto where = std::string("permissions.") + key;
				if (!item.asMap()) {
					reader.error(where, "entries must be objects");
					continue;
				}
				reader.checkKeys(item, where, { "id", "reason", "scope" });
				auto request = PermissionRequest();
				request.required = required;
				request.id = reader.text(item, "id", where, true, 64).value_or("");
				if (!KnownPermissions().contains(request.id)) {
					reader.error(where, "unknown permission '" + request.id + "'");
					continue;
				} else if (!seen.insert(request.id).second) {
					reader.error(where, "'" + request.id + "' is listed twice");
					continue;
				}
				if (const auto reason = reader.localized(item, "reason", where, true, 500)) {
					request.reason = *reason;
				}
				request.scope = reader.text(item, "scope", where, false, 16).value_or("");
				if (!request.scope.empty()
					&& (request.id != "messages.read"
						|| (request.scope != "selected" && request.scope != "all"))) {
					reader.error(where, "'scope' is only for messages.read ('selected' or 'all')");
				}
				manifest.permissions.push_back(std::move(request));
			}
		}
	}

	if (const auto contributes = reader.object(document, "contributes", "", false, {
			"events", "menus", "commands", "settingsPage", "outgoing", "messages" })) {
		const auto events = reader.array(*contributes, "events", "contributes", 16);
		for (const auto &item : events ? *events : cbor::Array()) {
			const auto name = item.asText();
			if (!name || (*name != "message.new" && *name != "message.edited" && *name != "message.deleted")) {
				reader.error("contributes.events", "has an unknown event");
			} else {
				manifest.events.push_back(*name);
			}
		}
		const auto menus = reader.array(*contributes, "menus", "contributes", 32);
		for (const auto &item : menus ? *menus : cbor::Array()) {
			if (!item.asMap()) {
				reader.error("contributes.menus", "entries must be objects");
				continue;
			}
			reader.checkKeys(item, "contributes.menus", { "surface", "id", "title" });
			auto menu = MenuItem();
			menu.surface = reader.text(item, "surface", "contributes.menus", true, 32).value_or("");
			menu.id = reader.text(item, "id", "contributes.menus", true, 64).value_or("");
			if (const auto title = reader.localized(item, "title", "contributes.menus", true, 80)) {
				menu.title = *title;
			}
			static const auto surfaces = std::set<std::string>{
				"message", "chat", "profile", "composer", "main_menu", "tray",
			};
			if (!surfaces.contains(menu.surface) || !LowerToken(menu.id, 64)) {
				reader.error("contributes.menus", "has a bad surface or id");
			}
			manifest.menus.push_back(std::move(menu));
		}
		const auto commands = reader.array(*contributes, "commands", "contributes", 32);
		for (const auto &item : commands ? *commands : cbor::Array()) {
			if (!item.asMap()) {
				reader.error("contributes.commands", "entries must be objects");
				continue;
			}
			reader.checkKeys(item, "contributes.commands", { "name", "description" });
			auto command = Command();
			command.name = reader.text(item, "name", "contributes.commands", true, 32).value_or("");
			for (const auto ch : command.name) {
				if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) {
					reader.error("contributes.commands", "bad command name");
					break;
				}
			}
			if (const auto description = reader.localized(item, "description", "contributes.commands", false, 200)) {
				command.description = *description;
			}
			manifest.commands.push_back(std::move(command));
		}
		manifest.settingsPage = reader.boolean(*contributes, "settingsPage", "contributes").value_or(false);
		if (const auto messages = reader.object(*contributes, "messages", "contributes", false, { "from" })) {
			const auto from = reader.array(*messages, "from", "contributes.messages", 64);
			for (const auto &item : from ? *from : cbor::Array()) {
				const auto sender = item.asText();
				if (!sender || (*sender != "*" && !ValidId(*sender))) {
					reader.error("contributes.messages.from", "must list plugin ids or '*'");
				} else {
					manifest.messagesFrom.push_back(*sender);
				}
			}
		}
		if (const auto outgoing = reader.object(*contributes, "outgoing", "contributes", false, { "onFailure" })) {
			manifest.outgoing = reader.text(*outgoing, "onFailure", "contributes.outgoing", false, 8).value_or("pass");
			if (manifest.outgoing != "pass" && manifest.outgoing != "block") {
				reader.error("contributes.outgoing", "onFailure must be 'pass' or 'block'");
			}
		}
	}

	ReadDependencies(reader, document, "dependencies", true, manifest.id, manifest.dependencies);
	ReadDependencies(reader, document, "conflicts", false, manifest.id, manifest.conflicts);
	if (const auto storage = reader.object(document, "storage", "", false, { "schema" })) {
		manifest.storageSchema = int(reader.integer(*storage, "schema", "storage", 1, 100000).value_or(1));
	}
	if (const auto updates = reader.object(document, "updates", "", false, { "feed" })) {
		manifest.updatesFeed = reader.url(*updates, "feed", "updates").value_or("");
	}

	const auto files = document.find("files");
	if (!files) {
		reader.error("files", "is required");
	} else if (!files->asMap()) {
		reader.error("files", "must be an object");
	} else {
		if (files->asMap()->size() > 4095) {
			reader.error("files", "lists too many files");
		}
		for (const auto &entry : *files->asMap()) {
			const auto hash = entry.value.asText();
			const auto decoded = (hash && hash->rfind("sha256-", 0) == 0)
				? Base64Decode(hash->substr(7))
				: std::nullopt;
			if (entry.key.rfind("x-", 0) == 0 || !ValidPath(entry.key, nullptr)) {
				reader.error("files", "bad path '" + entry.key + "'");
			} else if (!decoded || decoded->size() != 32) {
				reader.error("files", "bad hash for '" + entry.key + "'");
			} else {
				manifest.files.emplace(entry.key, *hash);
			}
		}
	}

	if (const auto signature = reader.object(document, "signature", "", false, { "alg", "publicKey", "value" })) {
		const auto alg = reader.text(*signature, "alg", "signature", true, 16);
		const auto key = reader.text(*signature, "publicKey", "signature", true, 100);
		const auto value = reader.text(*signature, "value", "signature", true, 200);
		const auto keyBytes = key ? Base64Decode(*key) : std::nullopt;
		const auto valueBytes = value ? Base64Decode(*value) : std::nullopt;
		if (alg && *alg != "ed25519") {
			reader.error("signature.alg", "must be ed25519");
		} else if (key && value && (!keyBytes || keyBytes->size() != 32 || !valueBytes || valueBytes->size() != 64)) {
			reader.error("signature", "has a malformed key or value");
		} else if (keyBytes && valueBytes) {
			auto parsed = ManifestSignature();
			std::copy(keyBytes->begin(), keyBytes->end(), parsed.key.begin());
			std::copy(valueBytes->begin(), valueBytes->end(), parsed.value.begin());
			manifest.signature = parsed;
		}
	}

	const auto hasPermission = [&](const char *id) {
		return std::any_of(
			manifest.permissions.begin(),
			manifest.permissions.end(),
			[&](const auto &request) { return request.id == id; });
	};
	if (!manifest.events.empty() && !hasPermission("messages.read")) {
		reader.error("contributes.events", "needs the messages.read permission");
	}
	if (!manifest.menus.empty() && !hasPermission("ui.menu")) {
		reader.error("contributes.menus", "needs the ui.menu permission");
	}
	if (!manifest.commands.empty() && !hasPermission("ui.command")) {
		reader.error("contributes.commands", "needs the ui.command permission");
	}
	if (manifest.settingsPage && !hasPermission("ui.settings_page")) {
		reader.error("contributes.settingsPage", "needs the ui.settings_page permission");
	}
	if (manifest.background && !hasPermission("background")) {
		reader.error("activation.background", "needs the background permission");
	}
	if (!manifest.messagesFrom.empty() && !hasPermission("plugins.message")) {
		reader.error("contributes.messages", "needs the plugins.message permission");
	}
	if (!manifest.outgoing.empty() && !hasPermission("messages.transform_outgoing")) {
		reader.error("contributes.outgoing", "needs the messages.transform_outgoing permission");
	}
	const auto composer = std::any_of(
		manifest.menus.begin(),
		manifest.menus.end(),
		[](const MenuItem &menu) { return menu.surface == "composer"; });
	if (composer && !hasPermission("ui.composer_button")) {
		reader.error("contributes.menus", "the composer surface needs the ui.composer_button permission");
	}
	const auto &runtimeFile = manifest.kind == "python" ? manifest.entry : manifest.module;
	if (!runtimeFile.empty() && !manifest.files.contains(runtimeFile)) {
		reader.error("files", "does not list the runtime file '" + runtimeFile + "'");
	}
	for (const auto &path : { manifest.icon, manifest.changelog }) {
		if (!path.empty() && !manifest.files.contains(path)) {
			reader.error("files", "does not list '" + path + "'");
		}
	}

	if (!reader.errors().empty()) {
		return std::move(reader.errors());
	}
	return manifest;
}

std::vector<CompatIssue> CheckCompatibility(
		const Manifest &manifest,
		const HostEnvironment &environment) {
	auto issues = std::vector<CompatIssue>();
	switch (manifest.appRange.fit(environment.app)) {
	case VersionRange::Fit::Match: break;
	case VersionRange::Fit::TooOld:
		issues.push_back({
			Problem::AppTooOld,
			"needs freshGram " + manifest.appRange.text(),
		});
		break;
	case VersionRange::Fit::TooNew:
		issues.push_back({
			Problem::AppTooNew,
			"was made for freshGram " + manifest.appRange.text(),
		});
		break;
	}
	if (environment.apiLevel < manifest.apiMin
		|| environment.apiLevel > manifest.apiMax) {
		issues.push_back({
			Problem::ApiUnsupported,
			"needs API level " + std::to_string(manifest.apiMin)
				+ " to " + std::to_string(manifest.apiMax),
		});
	}
	if (!manifest.platforms.contains(environment.platform)) {
		issues.push_back({
			Problem::PlatformUnsupported,
			"does not support " + environment.platform,
		});
	} else if (const auto i = manifest.platformVersions.find(environment.platform)
		; i != manifest.platformVersions.end()
		&& !i->second.matches(environment.platformVersion)) {
		issues.push_back({
			Problem::PlatformVersion,
			"needs " + environment.platform + " " + i->second.text(),
		});
	}
	for (const auto &feature : manifest.needs) {
		if (!environment.features.contains(feature)) {
			issues.push_back({ Problem::MissingFeature, feature });
		}
	}
	if (manifest.premiumOnly && !environment.premium) {
		issues.push_back({ Problem::PremiumRequired, "needs a Premium account" });
	}
	return issues;
}

} // namespace fgpl
