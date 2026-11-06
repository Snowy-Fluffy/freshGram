#include "fgpl/manifest.h"

#include "fgpl/base64.h"
#include "fgpl/json.h"
#include "fgpl/net_policy.h"
#include "fgpl/ui_tree.h"
#include "fgpl/vfs.h"

#include <algorithm>
#include <cctype>
#include <string_view>

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

[[nodiscard]] bool IsAppName(std::string_view name) {
	auto letters = std::string();
	for (const auto ch : name) {
		if (std::isalnum(static_cast<unsigned char>(ch))) {
			letters.push_back(char(std::tolower(static_cast<unsigned char>(ch))));
		}
	}
	for (const auto reserved : { "telegram", "telegramdesktop", "freshgram", "freshgramdesktop", "ayugram", "ayugramdesktop" }) {
		if (letters == reserved) {
			return true;
		}
	}
	return false;
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
		} else if (!ui::SafeText(*text, maxSize, false)) {
			error(name, "has control or bidirectional characters");
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
			size_t maxSize,
			bool multiline = false) {
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
			} else if (!ui::SafeText(*plain, maxSize, multiline)) {
				error(name, "has control or bidirectional characters");
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
				|| !ui::SafeText(*line, maxSize, multiline)) {
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

bool StorageExport::includes(const std::string &key) const {
	const auto starts = [&](const std::string &prefix) {
		return key.compare(0, prefix.size(), prefix) == 0;
	};
	return declared
		&& std::any_of(prefixes.begin(), prefixes.end(), starts)
		&& !std::any_of(exclude.begin(), exclude.end(), starts);
}

const std::set<std::string> &MenuSurfaces() {
	static const auto result = std::set<std::string>{
		"message", "chat", "profile", "composer", "main_menu", "tray",
		"selection", "shared_media",
	};
	return result;
}

bool MenuSurfaceHasMessages(const std::string &surface) {
	return surface == "message"
		|| surface == "selection"
		|| surface == "shared_media";
}

const std::set<std::string> &MenuMediaKinds() {
	static const auto result = std::set<std::string>{
		"photo", "video", "gif", "round", "voice", "audio", "file",
		"sticker", "poll", "contact", "location", "webpage", "other",
	};
	return result;
}

bool Matches(const MenuWhen &when, const MenuContext &context) {
	const auto hasMedia = !context.media.empty() && context.media != "webpage";
	if (!when.chatTypes.empty() && !when.chatTypes.contains(context.chatType)) {
		return false;
	} else if (when.out && *when.out != context.out) {
		return false;
	} else if (when.hasMedia && *when.hasMedia != hasMedia) {
		return false;
	} else if (when.hasText && *when.hasText != context.hasText) {
		return false;
	} else if (when.forwarded && *when.forwarded != context.forwarded) {
		return false;
	} else if (!when.media.empty() && !when.media.contains(context.media)) {
		return false;
	}
	return true;
}

bool MatchesEvent(const EventFilter &filter, const cbor::Value &event) {
	const auto text = [&](const char *key) {
		const auto value = event.find(key);
		const auto result = value ? value->asText() : nullptr;
		return result ? *result : std::string();
	};
	const auto flag = [&](const char *key) {
		const auto value = event.find(key);
		return value && value->asBool().value_or(false);
	};
	if (!filter.where.chats.empty()) {
		const auto chat = event.find("chat");
		const auto id = chat ? chat->asInt() : std::nullopt;
		if (!id || !filter.where.chats.contains(*id)) {
			return false;
		}
	}
	if (!filter.where.chatTypes.empty()
		&& !filter.where.chatTypes.contains(text("chat_type"))) {
		return false;
	}
	return Matches(filter.when, MenuContext{
		.chatType = text("chat_type"),
		.out = flag("out"),
		.media = text("media"),
		.hasText = !text("text").empty(),
		.forwarded = flag("forwarded"),
	});
}

namespace {

const std::map<std::string, const char*> &EventPermissions() {
	static const auto permissions = std::map<std::string, const char*>{
		{ "message.new", "messages.read" },
		{ "message.edited", "messages.read" },
		{ "message.deleted", "messages.read" },
		{ "message.reaction", "messages.read" },
		{ "message.pinned", "messages.read" },
		{ "chat.opened", "chats.active" },
		{ "chat.typing", "chats.activity" },
		{ "chat.read", "chats.activity" },
		{ "chat.member", "chats.activity" },
		{ "chat.title_changed", "chats.activity" },
		{ "user.status", "users.presence" },
		{ "story.new", "stories.read" },
		{ "call.incoming", "calls.activity" },
		{ "call.ended", "calls.activity" },
		{ "app.focus", "app.activity" },
		{ "app.blur", "app.activity" },
	};
	return permissions;
}

} // namespace

const char *EventPermission(const std::string &event) {
	const auto &permissions = EventPermissions();
	const auto i = permissions.find(event);
	return (i != permissions.end()) ? i->second : nullptr;
}

const std::vector<std::string> &KnownEvents() {
	static const auto events = [] {
		auto result = std::vector<std::string>();
		for (const auto &[name, permission] : EventPermissions()) {
			result.push_back(name);
		}
		return result;
	}();
	return events;
}

bool EventIsChatScoped(const std::string &event) {
	return event.rfind("message.", 0) == 0
		|| event.rfind("call.", 0) == 0
		|| event == "chat.opened"
		|| event == "chat.typing"
		|| event == "chat.read"
		|| event == "chat.member"
		|| event == "chat.title_changed"
		|| event == "story.new";
}

const std::set<std::string> &OutgoingChatTypes() {
	static const auto result = std::set<std::string>{
		"user", "bot", "self", "group", "supergroup", "forum", "channel",
	};
	return result;
}

namespace {

[[nodiscard]] bool FilterMatches(
		const OutgoingFilter &filter,
		int64_t chat,
		const std::string &chatType) {
	if (!filter.chats.empty() && !filter.chats.contains(chat)) {
		return false;
	} else if (!filter.chatTypes.empty()
		&& !filter.chatTypes.contains(chatType)) {
		return false;
	}
	return true;
}

} // namespace

bool Matches(const OutgoingFilter &filter, const OutgoingInfo &info) {
	return FilterMatches(filter, info.chat, info.chatType);
}

bool Matches(const OutgoingFilter &filter, const IncomingInfo &info) {
	return FilterMatches(filter, info.chat, info.chatType);
}

const std::set<std::string> &HostFeatures() {
	static const auto features = [] {
		auto result = KnownPermissions();
		for (const auto &surface : MenuSurfaces()) {
			result.insert("menu." + surface);
		}
		result.insert("format.html");
		return result;
	}();
	return features;
}

const std::set<std::string> &KnownPermissions() {
	static const auto known = std::set<std::string>{
		"messages.read", "messages.read_deleted", "messages.send",
		"messages.transform_outgoing", "media.read", "media.read_outgoing", "media.download", "chats.list", "chat.info",
		"messages.history", "account.info", "stars.balance", "composer.draft",
		"contacts.list", "settings.read", "folders.list", "ui.menu",
		"ui.command", "ui.link", "ui.settings_page", "ui.composer_button", "ui.dialog",
		"ui.toast", "ui.badges", "notifications", "notifications.suppress", "net.https", "net.tcp", "net.udp", "net.ws", "net.local", "storage.private", "storage.sql",
		"plugins.message",
		"fs.user_selected", "fs.folder", "clipboard", "background",
		"messages.edit", "messages.delete", "messages.react", "users.info",
		"chats.active", "messages.ids", "chats.activity", "users.presence",
		"app.activity", "translate.provide", "ai.provide", "transcribe.provide", "bots.inline", "chats.manage",
		"stories.read", "calls.activity", "image.process",
		"messages.transform_incoming",
		"contacts.manage", "ghost.control", "chats.moderate", "folders.manage", "settings.write",
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
		for (const auto &[language, text] : name->text) {
			if (IsAppName(text)) {
				reader.error("name", "must not be the name of the app");
				break;
			}
		}
		manifest.name = *name;
	}
	if (const auto version = reader.text(document, "version", "", true, 64)) {
		if (const auto parsed = ParseVersion(*version)) {
			manifest.version = *parsed;
		} else {
			reader.error("version", "must be a semantic version");
		}
	}
	if (const auto description = reader.localized(document, "description", "", false, 1000, true)) {
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
			"freshgram", "testedWith", "api", "requires", "accounts" })) {
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
				manifest.apiMin = int(std::clamp<int64_t>(*single, 0, 100000));
				manifest.apiMax = std::max(manifest.apiMin, 1000);
				if (*single < 1 || *single > 1000) {
					reader.error("compat.api", "must be from 1 to 1000");
				}
			} else if (api->asMap()) {
				reader.checkKeys(*api, "compat.api", { "min", "max" });
				manifest.apiMin = int(reader.integer(*api, "min", "compat.api", 1, 1000).value_or(1));
				manifest.apiMax = int(reader.integer(*api, "max", "compat.api", 1, 1000).value_or(1000));
				if (manifest.apiMax < manifest.apiMin) {
					reader.error("compat.api", "max is below min");
				}
			} else {
				reader.error("compat.api", "must be a number or {min, max}");
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
		if (const auto list = reader.array(*activation, "on", "activation", 16)) {
			manifest.activation.clear();
			for (const auto &item : *list) {
				const auto name = item.asText();
				if (!name
					|| (*name != "startup" && *name != "command" && *name != "event"
						&& *name != "menu" && *name != "settings" && *name != "message"
						&& *name != "timer" && *name != "translate" && *name != "ai"
						&& *name != "toast" && *name != "link" && *name != "transcribe")) {
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
				reader.checkKeys(item, where, { "id", "reason", "scope", "ports", "mode" });
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
				request.mode = reader.text(item, "mode", where, false, 8).value_or("");
				if (request.id == "fs.folder") {
					if (request.mode != "read" && request.mode != "write") {
						reader.error(where, "fs.folder needs 'mode': 'read' or 'write'");
					}
				} else if (item.find("mode")) {
					reader.error(where, "'mode' is only for fs.folder");
				}
				const auto ports = item.find("ports");
				if (request.id == "net.local") {
					const auto list = ports ? ports->asArray() : nullptr;
					if (!list || list->empty() || list->size() > 16) {
						reader.error(where, "net.local needs 'ports': 1 to 16 ports or ranges");
					}
					for (const auto &entry : list ? *list : cbor::Array()) {
						auto low = int64_t(0);
						auto high = int64_t(0);
						if (const auto number = entry.asInt()) {
							low = high = *number;
						} else if (const auto range = entry.asText()) {
							const auto dash = range->find('-');
							const auto digits = [](const std::string &text) {
								return !text.empty()
									&& text.size() <= 5
									&& text.find_first_not_of("0123456789") == std::string::npos;
							};
							if (dash == std::string::npos
								|| !digits(range->substr(0, dash))
								|| !digits(range->substr(dash + 1))) {
								low = high = 0;
							} else {
								low = std::stoi(range->substr(0, dash));
								high = std::stoi(range->substr(dash + 1));
							}
						}
						if (low < 1 || high > 65535 || low > high) {
							reader.error(where, "'ports' has a bad entry");
							break;
						}
						request.ports.emplace_back(int(low), int(high));
					}
				} else if (ports) {
					reader.error(where, "'ports' is only for net.local");
				}
				manifest.permissions.push_back(std::move(request));
			}
		}
	}

	if (const auto contributes = reader.object(document, "contributes", "", false, {
			"events", "eventFilters", "menus", "commands", "links", "settingsPage", "outgoing", "incoming", "messages", "icons",
			"translator", "aiProvider", "transcriber" })) {
		if (const auto icons = contributes->find("icons"); icons && !icons->asMap()) {
			reader.error("contributes.icons", "must be an object");
		} else if (icons) {
			for (const auto &entry : *icons->asMap()) {
				const auto path = entry.value.asText();
				if (entry.key.rfind("x-", 0) == 0) {
					continue;
				} else if (!ui::ValidIconName(entry.key)) {
					reader.error("contributes.icons", "has a bad icon name");
				} else if (!path || (!ValidPath(*path, ".png") && !ValidPath(*path, ".svg"))) {
					reader.error("contributes.icons", "needs .png or .svg paths");
				} else if (manifest.icons.size() >= 32) {
					reader.error("contributes.icons", "may list at most 32 icons");
				} else {
					manifest.icons.emplace(entry.key, *path);
				}
			}
		}
		const auto events = reader.array(*contributes, "events", "contributes", 32);
		for (const auto &item : events ? *events : cbor::Array()) {
			const auto name = item.asText();
			if (!name || !EventPermission(*name)) {
				reader.error("contributes.events", "has an unknown event");
			} else {
				manifest.events.push_back(*name);
			}
		}
		if (const auto filters = contributes->find("eventFilters")) {
			if (!filters->asMap()) {
				reader.error("contributes.eventFilters", "must be an object");
			} else {
				for (const auto &entry : *filters->asMap()) {
					const auto where = std::string("contributes.eventFilters.") + entry.key;
					if (std::find(manifest.events.begin(), manifest.events.end(), entry.key)
						== manifest.events.end()) {
						reader.error("contributes.eventFilters", "filters an event that is not listed in events");
						continue;
					} else if (!entry.value.asMap()) {
						reader.error(where, "must be an object");
						continue;
					}
					const auto messages = (entry.key.rfind("message.", 0) == 0);
					auto allowed = std::set<std::string>{ "chats", "chat_types" };
					if (messages) {
						allowed.insert({ "out", "has_media", "has_text", "media", "forwarded" });
					}
					reader.checkKeys(entry.value, where, allowed);
					if (!EventIsChatScoped(entry.key)) {
						for (const auto &key : *entry.value.asMap()) {
							reader.error(where, key.key + " does not apply to " + entry.key);
						}
						continue;
					}
					auto parsed = EventFilter();
					const auto chats = reader.array(entry.value, "chats", where, 256);
					for (const auto &item : chats ? *chats : cbor::Array()) {
						const auto chat = item.asInt();
						if (!chat || !*chat) {
							reader.error(where + ".chats", "has a bad entry");
						} else {
							parsed.where.chats.insert(*chat);
						}
					}
					const auto readKinds = [&](const char *key, const std::set<std::string> &known, std::set<std::string> &into) {
						const auto list = reader.array(entry.value, key, where, 16);
						for (const auto &kind : list ? *list : cbor::Array()) {
							const auto text = kind.asText();
							if (!text || !known.contains(*text)) {
								reader.error(where + "." + key, "has an unknown entry");
							} else {
								into.insert(*text);
							}
						}
					};
					readKinds("chat_types", OutgoingChatTypes(), parsed.where.chatTypes);
					if (messages) {
						readKinds("media", MenuMediaKinds(), parsed.when.media);
						parsed.when.out = reader.boolean(entry.value, "out", where);
						parsed.when.hasMedia = reader.boolean(entry.value, "has_media", where);
						parsed.when.hasText = reader.boolean(entry.value, "has_text", where);
						parsed.when.forwarded = reader.boolean(entry.value, "forwarded", where);
					}
					manifest.eventFilters.emplace(entry.key, std::move(parsed));
				}
			}
		}
		const auto menus = reader.array(*contributes, "menus", "contributes", 32);
		for (const auto &item : menus ? *menus : cbor::Array()) {
			if (!item.asMap()) {
				reader.error("contributes.menus", "entries must be objects");
				continue;
			}
			reader.checkKeys(item, "contributes.menus", { "surface", "id", "title", "icon", "when", "priority" });
			auto menu = MenuItem();
			if (const auto icon = item.find("icon")) {
				const auto name = icon->asText();
				if (!name || !ui::ValidIconName(*name)) {
					reader.error("contributes.menus", "has a bad icon name");
				} else {
					menu.icon = *name;
				}
			}
			menu.surface = reader.text(item, "surface", "contributes.menus", true, 32).value_or("");
			menu.id = reader.text(item, "id", "contributes.menus", true, 64).value_or("");
			menu.priority = int(reader.integer(item, "priority", "contributes.menus", -1000, 1000).value_or(0));
			if (const auto title = reader.localized(item, "title", "contributes.menus", true, 80)) {
				menu.title = *title;
			}
			if (!MenuSurfaces().contains(menu.surface) || !LowerToken(menu.id, 64)) {
				reader.error("contributes.menus", "has a bad surface or id");
			}
			if (const auto when = reader.object(item, "when", "contributes.menus", false, {
					"chat_type", "out", "has_media", "has_text", "media", "forwarded" })) {
				const auto messages = MenuSurfaceHasMessages(menu.surface);
				const auto peers = messages
					|| menu.surface == "chat"
					|| menu.surface == "profile"
					|| menu.surface == "composer";
				for (const auto &entry : *when->asMap()) {
					const auto message = (entry.key != "chat_type");
					if (message ? !messages : !peers) {
						reader.error(
							"contributes.menus.when",
							entry.key + " does not apply to the " + menu.surface + " surface");
					}
				}
				const auto where = std::string("contributes.menus.when");
				const auto kinds = [&](const char *key, const std::set<std::string> &allowed, std::set<std::string> &into) {
					const auto list = reader.array(*when, key, where, 16);
					for (const auto &kind : list ? *list : cbor::Array()) {
						const auto text = kind.asText();
						if (!text || !allowed.contains(*text)) {
							reader.error(where + "." + key, "has an unknown entry");
						} else {
							into.insert(*text);
						}
					}
				};
				kinds("chat_type", OutgoingChatTypes(), menu.when.chatTypes);
				kinds("media", MenuMediaKinds(), menu.when.media);
				menu.when.out = reader.boolean(*when, "out", where);
				menu.when.hasMedia = reader.boolean(*when, "has_media", where);
				menu.when.hasText = reader.boolean(*when, "has_text", where);
				menu.when.forwarded = reader.boolean(*when, "forwarded", where);
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
		const auto links = reader.array(*contributes, "links", "contributes", 16);
		for (const auto &item : links ? *links : cbor::Array()) {
			if (!item.asMap()) {
				reader.error("contributes.links", "entries must be objects");
				continue;
			}
			reader.checkKeys(item, "contributes.links", { "path", "description" });
			auto link = DeepLink();
			link.path = reader.text(item, "path", "contributes.links", true, 64).value_or("");
			for (const auto ch : link.path) {
				if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) {
					reader.error("contributes.links", "bad link path");
					break;
				}
			}
			if (const auto description = reader.localized(item, "description", "contributes.links", false, 200)) {
				link.description = *description;
			}
			manifest.links.push_back(std::move(link));
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
		if (const auto translator = reader.object(*contributes, "translator", "contributes", false, {
				"name", "languages", "maxChars" })) {
			auto &decl = manifest.translator;
			decl.declared = true;
			decl.name = reader.text(*translator, "name", "contributes.translator", true, 40).value_or("");
			const auto languages = reader.array(*translator, "languages", "contributes.translator", 200);
			for (const auto &item : languages ? *languages : cbor::Array()) {
				const auto code = item.asText();
				if (!code || !ValidLanguage(*code)) {
					reader.error("contributes.translator.languages", "has a bad language code");
				} else {
					decl.languages.push_back(*code);
				}
			}
			decl.maxChars = int(reader.integer(*translator, "maxChars", "contributes.translator", 64, 8192).value_or(4096));
		}
		if (const auto provider = reader.object(*contributes, "aiProvider", "contributes", false, {
				"name", "maxChars" })) {
			auto &decl = manifest.aiProvider;
			decl.declared = true;
			decl.name = reader.text(*provider, "name", "contributes.aiProvider", true, 40).value_or("");
			decl.maxChars = int(reader.integer(*provider, "maxChars", "contributes.aiProvider", 64, 8192).value_or(4096));
		}
		if (const auto provider = reader.object(*contributes, "transcriber", "contributes", false, {
				"name", "maxSeconds" })) {
			auto &decl = manifest.transcriber;
			decl.declared = true;
			decl.name = reader.text(*provider, "name", "contributes.transcriber", true, 40).value_or("");
			decl.maxSeconds = int(reader.integer(*provider, "maxSeconds", "contributes.transcriber", 5, 3600).value_or(300));
		}
		const auto readHook = [&](
				const char *key,
				bool &declared,
				bool &block,
				bool *hold,
				OutgoingFilter &parsed,
				int &priority,
				bool *service = nullptr,
				bool *media = nullptr,
				bool *final = nullptr) {
			const auto where = std::string("contributes.") + key;
			auto allowed = std::set<std::string>{ "onFailure", "filter", "priority" };
			if (final) {
				allowed.insert("final");
			}
			if (hold) {
				allowed.insert("mode");
			}
			if (service) {
				allowed.insert("service");
			}
			if (media) {
				allowed.insert("media");
			}
			const auto hook = reader.object(*contributes, key, "contributes", false, allowed);
			if (!hook) {
				return;
			}
			declared = true;
			const auto policy = reader.text(*hook, "onFailure", where, false, 8).value_or("pass");
			if (policy != "pass" && policy != "block") {
				reader.error(where, "onFailure must be 'pass' or 'block'");
			}
			block = (policy == "block");
			priority = int(reader.integer(*hook, "priority", where, -1000, 1000).value_or(0));
			if (final) {
				*final = reader.boolean(*hook, "final", where).value_or(false);
			}
			if (hold) {
				const auto mode = reader.text(*hook, "mode", where, false, 8).value_or("swap");
				if (mode != "swap" && mode != "hold") {
					reader.error(where, "mode must be 'swap' or 'hold'");
				}
				*hold = (mode == "hold");
			}
			if (service) {
				*service = reader.boolean(*hook, "service", where).value_or(false);
			}
			if (media) {
				*media = reader.boolean(*hook, "media", where).value_or(false);
			}
			if (const auto filter = reader.object(*hook, "filter", where, false, {
					"chats", "chat_types" })) {
				const auto filterWhere = where + ".filter";
				const auto chats = reader.array(*filter, "chats", filterWhere, 256);
				for (const auto &item : chats ? *chats : cbor::Array()) {
					const auto chat = item.asInt();
					if (!chat || !*chat) {
						reader.error(filterWhere + ".chats", "has a bad entry");
					} else {
						parsed.chats.insert(*chat);
					}
				}
				const auto list = reader.array(*filter, "chat_types", filterWhere, 32);
				for (const auto &item : list ? *list : cbor::Array()) {
					const auto text = item.asText();
					if (!text || !OutgoingChatTypes().contains(*text)) {
						reader.error(filterWhere + ".chat_types", "has an unknown entry");
					} else {
						parsed.chatTypes.insert(*text);
					}
				}
			}
		};
		readHook("outgoing", manifest.outgoing.declared, manifest.outgoing.block, nullptr, manifest.outgoing.filter, manifest.outgoing.priority, nullptr, &manifest.outgoing.media, &manifest.outgoing.final);
		readHook("incoming", manifest.incoming.declared, manifest.incoming.block, &manifest.incoming.hold, manifest.incoming.filter, manifest.incoming.priority, &manifest.incoming.service);
	}

	ReadDependencies(reader, document, "dependencies", true, manifest.id, manifest.dependencies);
	ReadDependencies(reader, document, "conflicts", false, manifest.id, manifest.conflicts);
	if (const auto storage = reader.object(document, "storage", "", false, { "schema", "export" })) {
		manifest.storageSchema = int(reader.integer(*storage, "schema", "storage", 1, 100000).value_or(1));
		if (const auto exported = reader.object(*storage, "export", "storage", false, { "prefixes", "exclude" })) {
			auto &decl = manifest.storageExport;
			decl.declared = true;
			const auto keys = [&](const char *name, std::vector<std::string> &into, size_t most) {
				const auto list = reader.array(*exported, name, "storage.export", most);
				for (const auto &item : list ? *list : cbor::Array()) {
					const auto key = item.asText();
					if (!key || key->size() > 128 || !ui::SafeText(*key, 128, false)) {
						reader.error(std::string("storage.export.") + name, "has a bad key prefix");
					} else {
						into.push_back(*key);
					}
				}
			};
			keys("prefixes", decl.prefixes, 32);
			keys("exclude", decl.exclude, 32);
			if (decl.prefixes.empty()) {
				reader.error("storage.export", "needs at least one prefix");
			}
		}
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
	for (const auto &event : manifest.events) {
		const auto permission = EventPermission(event);
		if (permission && !hasPermission(permission)) {
			reader.error(
				"contributes.events",
				event + " needs the " + permission + " permission");
		}
	}
	if (!manifest.menus.empty() && !hasPermission("ui.menu")) {
		reader.error("contributes.menus", "needs the ui.menu permission");
	}
	if (!manifest.commands.empty() && !hasPermission("ui.command")) {
		reader.error("contributes.commands", "needs the ui.command permission");
	}
	if (!manifest.links.empty() && !hasPermission("ui.link")) {
		reader.error("contributes.links", "needs the ui.link permission");
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
	if (manifest.translator.declared && !hasPermission("translate.provide")) {
		reader.error("contributes.translator", "needs the translate.provide permission");
	}
	if (manifest.transcriber.declared && !hasPermission("transcribe.provide")) {
		reader.error("contributes.transcriber", "needs the transcribe.provide permission");
	} else if (manifest.transcriber.declared && !hasPermission("media.read")) {
		reader.error("contributes.transcriber", "needs the media.read permission");
	}
	if (manifest.aiProvider.declared && !hasPermission("ai.provide")) {
		reader.error("contributes.aiProvider", "needs the ai.provide permission");
	}
	if (manifest.outgoing.declared && !hasPermission("messages.transform_outgoing")) {
		reader.error("contributes.outgoing", "needs the messages.transform_outgoing permission");
	}
	if (manifest.incoming.declared && !hasPermission("messages.transform_incoming")) {
		reader.error("contributes.incoming", "needs the messages.transform_incoming permission");
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
	for (const auto &[name, path] : manifest.icons) {
		if (!manifest.files.contains(path)) {
			reader.error("files", "does not list '" + path + "'");
		}
	}
	for (const auto &menu : manifest.menus) {
		if (!menu.icon.empty()
			&& !manifest.icons.contains(menu.icon)
			&& !ui::BuiltinIcons().contains(menu.icon)) {
			reader.error("contributes.menus", "uses the unknown icon '" + menu.icon + "'");
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
	for (const auto &feature : manifest.needs) {
		if (!environment.features.contains(feature)) {
			issues.push_back({ Problem::MissingFeature, "needs " + feature });
		}
	}
	if (manifest.premiumOnly && !environment.premium) {
		issues.push_back({ Problem::PremiumRequired, "needs a Premium account" });
	}
	return issues;
}

namespace {

// Lowercase letters and digits only, with Cyrillic lookalikes mapped to Latin.
[[nodiscard]] std::string LookalikeFold(std::string_view text) {
	auto result = std::string();
	for (auto i = size_t(0); i != text.size(); ++i) {
		const auto byte = uint8_t(text[i]);
		if (byte < 0x80) {
			if (std::isalnum(byte)) {
				result.push_back(char(std::tolower(byte)));
			}
		} else if (byte >= 0xC0 && byte < 0xE0 && i + 1 < text.size()) {
			const auto code = ((byte & 0x1F) << 6) | (uint8_t(text[++i]) & 0x3F);
			switch (code) {
			case 0x0430: case 0x0410: result.push_back('a'); break;
			case 0x0435: case 0x0415: result.push_back('e'); break;
			case 0x043E: case 0x041E: result.push_back('o'); break;
			case 0x0440: case 0x0420: result.push_back('p'); break;
			case 0x0441: case 0x0421: result.push_back('c'); break;
			case 0x0445: case 0x0425: result.push_back('x'); break;
			case 0x0443: case 0x0423: result.push_back('y'); break;
			case 0x0456: case 0x0406: result.push_back('i'); break;
			case 0x0455: case 0x0405: result.push_back('s'); break;
			case 0x04BB: case 0x04BA: result.push_back('h'); break;
			}
		}
	}
	return result;
}

[[nodiscard]] bool IsReservedWord(const std::string &folded) {
	return folded.find("freshgram") != std::string::npos
		|| folded.find("snowyfluffy") != std::string::npos;
}

} // namespace

ReservedName ClaimedReservedName(const Manifest &manifest) {
	auto label = std::string();
	for (const auto ch : manifest.id + '.') {
		if (ch != '.') {
			label.push_back(ch);
			continue;
		}
		const auto folded = LookalikeFold(label);
		if (folded == "freshgram" || folded == "snowyfluffy") {
			return ReservedName::Id;
		}
		label.clear();
	}
	return IsReservedWord(LookalikeFold(manifest.authorName))
		? ReservedName::Author
		: ReservedName::None;
}

} // namespace fgpl
