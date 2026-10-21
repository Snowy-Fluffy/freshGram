#include "broker.h"

#include "net_policy.h"

#include <algorithm>
#include <chrono>

namespace fgpl {
namespace api {

cbor::Value Ok(cbor::Value value) {
	return cbor::Value::FromMap({
		{ "ok", cbor::Value::FromBool(true) },
		{ "v", std::move(value) },
	});
}

cbor::Value Fail(std::string_view code, std::string_view message) {
	return cbor::Value::FromMap({
		{ "ok", cbor::Value::FromBool(false) },
		{ "e", cbor::Value::FromText(std::string(code)) },
		{ "m", cbor::Value::FromText(std::string(message)) },
	});
}

} // namespace api

namespace {

constexpr auto kMaxKeyBytes = size_t(128);
constexpr auto kMaxValueBytes = size_t(1) << 20;
constexpr auto kMaxKeys = size_t(4096);
constexpr auto kMaxChats = size_t(500);
constexpr auto kMaxHistory = size_t(100);
constexpr auto kMaxRequestBody = size_t(1) << 20;
constexpr auto kMaxResponseBody = size_t(8) << 20;
constexpr auto kMaxHeaders = size_t(32);
constexpr auto kMaxHeaderValue = size_t(4096);
constexpr auto kMaxToast = size_t(200);
constexpr auto kMaxMessage = size_t(4096);

[[nodiscard]] cbor::Value Text(std::string_view text) {
	return cbor::Value::FromText(std::string(text));
}

[[nodiscard]] cbor::Value Int(int64_t number) {
	return cbor::Value::FromInt(number);
}

[[nodiscard]] const cbor::Value *Arg(const cbor::Value *args, const char *key) {
	return args ? args->find(key) : nullptr;
}

[[nodiscard]] std::optional<std::string> TextArg(
		const cbor::Value *args,
		const char *key,
		size_t maxBytes) {
	const auto value = Arg(args, key);
	const auto text = value ? value->asText() : nullptr;
	if (!text || text->size() > maxBytes) {
		return std::nullopt;
	}
	return *text;
}

[[nodiscard]] std::optional<int64_t> IntArg(
		const cbor::Value *args,
		const char *key) {
	const auto value = Arg(args, key);
	return value ? value->asInt() : std::nullopt;
}

[[nodiscard]] bool ValidKey(const std::string &key) {
	if (key.empty() || key.size() > kMaxKeyBytes) {
		return false;
	}
	for (const auto ch : key) {
		if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] cbor::Bytes Encoded(const cbor::Value &value) {
	auto bytes = cbor::Bytes();
	if (!cbor::Encode(value, bytes, 16)) {
		bytes.clear();
	}
	return bytes;
}

[[nodiscard]] cbor::Value ChatValue(const Chat &chat) {
	return cbor::Value::FromMap({
		{ "id", Int(chat.id) },
		{ "title", Text(chat.title) },
		{ "type", Text(chat.type) },
		{ "unread", Int(chat.unread) },
		{ "pinned", cbor::Value::FromBool(chat.pinned) },
	});
}

[[nodiscard]] cbor::Value MessageValue(const Message &message) {
	return cbor::Value::FromMap({
		{ "id", Int(message.id) },
		{ "chat", Int(message.chat) },
		{ "from", Int(message.from) },
		{ "date", Int(message.date) },
		{ "text", Text(message.text) },
	});
}

} // namespace

std::optional<cbor::Bytes> MemoryStorage::get(const std::string &key) {
	const auto i = _items.find(key);
	if (i == _items.end()) {
		return std::nullopt;
	}
	return i->second;
}

void MemoryStorage::set(const std::string &key, cbor::Bytes value) {
	_items[key] = std::move(value);
}

bool MemoryStorage::remove(const std::string &key) {
	return _items.erase(key) != 0;
}

std::vector<std::string> MemoryStorage::keys(
		const std::string &prefix,
		size_t limit) {
	auto result = std::vector<std::string>();
	for (auto i = _items.lower_bound(prefix);
		i != _items.end() && result.size() < limit;
		++i) {
		if (i->first.rfind(prefix, 0) != 0) {
			break;
		}
		result.push_back(i->first);
	}
	return result;
}

uint64_t MemoryStorage::usedBytes() {
	auto total = uint64_t(0);
	for (const auto &item : _items) {
		total += item.first.size() + item.second.size();
	}
	return total;
}

size_t MemoryStorage::count() {
	return _items.size();
}

Broker::Broker(Grants grants, BrokerServices services)
: _grants(std::move(grants))
, _services(std::move(services)) {
	if (!_services.clockMs) {
		_services.clockMs = [] {
			return int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		};
	}
}

bool Broker::has(const char *permission) const {
	return _grants.permissions.contains(permission);
}

bool Broker::chatInScope(int64_t id) const {
	return !_grants.chats || _grants.chats->contains(id);
}

bool Broker::chatVisible(int64_t id, bool secret) const {
	return !secret && id != api::kServiceChat && chatInScope(id);
}

bool Broker::admit() {
	const auto now = _services.clockMs();
	std::erase_if(_recent, [&](int64_t time) {
		return time <= now - 1000;
	});
	if (int(_recent.size()) >= _grants.callsPerSecond) {
		return false;
	}
	_recent.push_back(now);
	return true;
}

void Broker::log(std::string_view line) {
	if (_services.log) {
		_services.log(line);
	}
}

cbor::Value Broker::call(const cbor::Value &request) {
	const auto operation = request.find("op");
	const auto name = operation ? operation->asText() : nullptr;
	if (!name) {
		return api::Fail(api::error::kInvalid, "no operation");
	}
	const auto args = request.find("a");
	if (args && !args->asMap()) {
		return api::Fail(api::error::kInvalid, "arguments must be a map");
	}
	auto response = cbor::Value();
	if (!admit()) {
		response = api::Fail(api::error::kLimit, "too many calls");
	} else {
		response = dispatch(*name, args);
	}
	if (_services.audit) {
		const auto ok = response.find("ok");
		const auto code = response.find("e");
		_services.audit(
			*name,
			(ok && ok->asBool().value_or(false))
				? std::string_view("ok")
				: (code && code->asText())
				? std::string_view(*code->asText())
				: std::string_view("failed"));
	}
	return response;
}

cbor::Value Broker::dispatch(const std::string &op, const cbor::Value *args) {
	if (op == "meta.info") {
		auto granted = cbor::Array();
		for (const auto &permission : _grants.permissions) {
			granted.push_back(Text(permission));
		}
		return api::Ok(cbor::Value::FromMap({
			{ "api", Int(api::kVersion) },
			{ "plugin", Text(_grants.pluginId) },
			{ "granted", cbor::Value::FromArray(std::move(granted)) },
		}));
	} else if (op == "log") {
		const auto text = TextArg(args, "text", 4096);
		if (!text) {
			return api::Fail(api::error::kInvalid, "text");
		}
		log(*text);
		return api::Ok();
	} else if (op.rfind("storage.", 0) == 0) {
		if (!has("storage.private")) {
			return api::Fail(api::error::kDenied, "storage.private");
		}
		return storageCall(op, args);
	} else if (op == "net.fetch") {
		if (!has("net.hosts")) {
			return api::Fail(api::error::kDenied, "net.hosts");
		}
		return netFetch(args);
	}
	return dataCall(op, args);
}

cbor::Value Broker::storageCall(const std::string &op, const cbor::Value *args) {
	const auto storage = _services.storage;
	if (!storage) {
		return api::Fail(api::error::kUnavailable, "storage");
	}
	if (op == "storage.usage") {
		return api::Ok(cbor::Value::FromMap({
			{ "used", Int(int64_t(storage->usedBytes())) },
			{ "quota", Int(int64_t(_grants.storageQuotaBytes)) },
			{ "keys", Int(int64_t(storage->count())) },
		}));
	} else if (op == "storage.list") {
		const auto prefix = Arg(args, "prefix")
			? TextArg(args, "prefix", kMaxKeyBytes)
			: std::optional<std::string>(std::string());
		auto limit = IntArg(args, "limit").value_or(100);
		if (!prefix || limit < 1) {
			return api::Fail(api::error::kInvalid, "prefix or limit");
		}
		limit = std::min<int64_t>(limit, 1000);
		auto result = cbor::Array();
		for (const auto &key : storage->keys(*prefix, size_t(limit))) {
			result.push_back(Text(key));
		}
		return api::Ok(cbor::Value::FromArray(std::move(result)));
	}
	const auto key = TextArg(args, "key", kMaxKeyBytes);
	if (!key || !ValidKey(*key)) {
		return api::Fail(api::error::kInvalid, "key");
	}
	if (op == "storage.get") {
		const auto stored = storage->get(*key);
		if (!stored) {
			return api::Ok();
		}
		const auto decoded = cbor::Decode(*stored);
		return decoded.ok()
			? api::Ok(decoded.value())
			: api::Fail(api::error::kFailed, "a damaged value");
	} else if (op == "storage.delete") {
		return api::Ok(cbor::Value::FromBool(storage->remove(*key)));
	} else if (op == "storage.set") {
		const auto value = Arg(args, "value");
		if (!value) {
			return api::Fail(api::error::kInvalid, "value");
		}
		auto bytes = Encoded(*value);
		if (bytes.empty() || bytes.size() > kMaxValueBytes) {
			return api::Fail(api::error::kLimit, "the value is too large");
		}
		const auto existing = storage->get(*key);
		const auto freed = existing ? key->size() + existing->size() : 0;
		const auto added = key->size() + bytes.size();
		if (!existing && storage->count() >= kMaxKeys) {
			return api::Fail(api::error::kQuota, "too many keys");
		} else if (storage->usedBytes() - freed + added > _grants.storageQuotaBytes) {
			return api::Fail(api::error::kQuota, "the quota is used up");
		}
		storage->set(*key, std::move(bytes));
		return api::Ok();
	}
	return api::Fail(api::error::kInvalid, "unknown storage operation");
}

cbor::Value Broker::netFetch(const cbor::Value *args) {
	if (!_services.transport) {
		return api::Fail(api::error::kUnavailable, "network");
	}
	const auto method = TextArg(args, "method", 8).value_or("GET");
	const auto address = TextArg(args, "url", 2048);
	if (!address || !net::IsAllowedMethod(method)) {
		return api::Fail(api::error::kInvalid, "method or url");
	}
	const auto parsed = net::ParseUrl(*address);
	if (!parsed.ok()) {
		return api::Fail(api::error::kInvalid, parsed.error());
	} else if (!net::HostAllowed(parsed.value(), _grants.hosts)) {
		return api::Fail(api::error::kDenied, parsed.value().host);
	}
	auto request = FetchRequest();
	request.method = method;
	request.url = parsed.value().text;
	request.host = parsed.value().host;
	request.port = parsed.value().port;
	request.maxResponseBytes = kMaxResponseBody;
	const auto timeout = IntArg(args, "timeout").value_or(10000);
	request.timeoutMs = int(std::clamp<int64_t>(timeout, 100, 30000));
	if (const auto headers = Arg(args, "headers")) {
		const auto map = headers->asMap();
		if (!map || map->size() > kMaxHeaders) {
			return api::Fail(api::error::kInvalid, "headers");
		}
		for (const auto &entry : *map) {
			const auto text = entry.value.asText();
			if (!text
				|| text->size() > kMaxHeaderValue
				|| !net::IsValidHeaderName(entry.key)
				|| net::IsForbiddenRequestHeader(entry.key)
				|| text->find_first_of("\r\n") != std::string::npos) {
				return api::Fail(api::error::kInvalid, "header " + entry.key);
			}
			request.headers.emplace_back(entry.key, *text);
		}
	}
	if (const auto body = Arg(args, "body")) {
		const auto bytes = body->asBytes();
		const auto text = body->asText();
		if (bytes) {
			request.body = *bytes;
		} else if (text) {
			request.body.assign(text->begin(), text->end());
		} else if (!body->isNull()) {
			return api::Fail(api::error::kInvalid, "body");
		}
		if (request.body.size() > kMaxRequestBody) {
			return api::Fail(api::error::kLimit, "the body is too large");
		}
	}
	const auto patterns = _grants.hosts;
	request.allowRedirect = [patterns](const std::string &target) {
		const auto next = net::ParseUrl(target);
		return next.ok() && net::HostAllowed(next.value(), patterns);
	};
	const auto response = _services.transport->fetch(request);
	if (!response.ok()) {
		return api::Fail(response.error().code, response.error().message);
	} else if (response.value().body.size() > kMaxResponseBody) {
		return api::Fail(api::error::kLimit, "the response is too large");
	}
	auto headers = cbor::Map();
	for (const auto &header : response.value().headers) {
		auto name = header.first;
		std::transform(name.begin(), name.end(), name.begin(), [](char ch) {
			return char(std::tolower(static_cast<unsigned char>(ch)));
		});
		if (name == "set-cookie" || name == "set-cookie2") {
			continue;
		}
		headers.push_back({ name, Text(header.second) });
	}
	return api::Ok(cbor::Value::FromMap({
		{ "status", Int(response.value().status) },
		{ "headers", cbor::Value::FromMap(std::move(headers)) },
		{ "body", cbor::Value::FromBytes(response.value().body) },
	}));
}

cbor::Value Broker::dataCall(const std::string &op, const cbor::Value *args) {
	const auto data = _services.data;
	const auto need = [&](const char *permission) -> std::optional<cbor::Value> {
		if (!has(permission)) {
			return api::Fail(api::error::kDenied, permission);
		} else if (!data) {
			return api::Fail(api::error::kUnavailable, "data");
		}
		return std::nullopt;
	};
	if (op == "chats.list") {
		if (const auto refused = need("chats.list")) {
			return *refused;
		}
		const auto limit = size_t(std::clamp<int64_t>(
			IntArg(args, "limit").value_or(100),
			1,
			int64_t(kMaxChats)));
		auto result = cbor::Array();
		for (const auto &chat : data->chats(kMaxChats)) {
			if (chatVisible(chat.id, chat.secret) && result.size() < limit) {
				result.push_back(ChatValue(chat));
			}
		}
		return api::Ok(cbor::Value::FromArray(std::move(result)));
	} else if (op == "chat.info" || op == "messages.history") {
		if (const auto refused = need(
				op == "chat.info" ? "chat.info" : "messages.history")) {
			return *refused;
		}
		const auto id = IntArg(args, "chat");
		if (!id) {
			return api::Fail(api::error::kInvalid, "chat");
		} else if (*id == api::kServiceChat || !chatInScope(*id)) {
			return api::Fail(api::error::kDenied, "this chat");
		}
		const auto chat = data->chat(*id);
		if (!chat || chat->secret) {
			return api::Fail(api::error::kNotFound, "chat");
		} else if (op == "chat.info") {
			return api::Ok(ChatValue(*chat));
		}
		const auto limit = size_t(std::clamp<int64_t>(
			IntArg(args, "limit").value_or(20),
			1,
			int64_t(kMaxHistory)));
		auto result = cbor::Array();
		for (const auto &message : data->history(
				*id,
				limit,
				IntArg(args, "before").value_or(0))) {
			if (message.chat == *id && result.size() < limit) {
				result.push_back(MessageValue(message));
			}
		}
		return api::Ok(cbor::Value::FromArray(std::move(result)));
	} else if (op == "account.info") {
		if (const auto refused = need("account.info")) {
			return *refused;
		}
		const auto account = data->account();
		return api::Ok(cbor::Value::FromMap({
			{ "id", Int(account.id) },
			{ "name", Text(account.name) },
			{ "username", Text(account.username) },
			{ "premium", cbor::Value::FromBool(account.premium) },
		}));
	} else if (op == "stars.balance") {
		if (const auto refused = need("stars.balance")) {
			return *refused;
		}
		return api::Ok(cbor::Value::FromMap({
			{ "balance", Int(data->stars()) },
		}));
	} else if (op == "composer.draft" || op == "composer.set_draft") {
		if (const auto refused = need("composer.draft")) {
			return *refused;
		}
		const auto id = IntArg(args, "chat");
		if (!id || !chatInScope(*id) || *id == api::kServiceChat) {
			return api::Fail(api::error::kDenied, "this chat");
		}
		const auto chat = data->chat(*id);
		if (!chat || chat->secret) {
			return api::Fail(api::error::kNotFound, "chat");
		} else if (op == "composer.draft") {
			return api::Ok(Text(data->draft(*id)));
		}
		const auto text = TextArg(args, "text", kMaxMessage);
		if (!text) {
			return api::Fail(api::error::kInvalid, "text");
		}
		return data->setDraft(*id, *text)
			? api::Ok()
			: api::Fail(api::error::kFailed, "the draft was not set");
	} else if (op == "messages.send") {
		if (const auto refused = need("messages.send")) {
			return *refused;
		}
		const auto id = IntArg(args, "chat");
		const auto text = TextArg(args, "text", kMaxMessage);
		if (!id || !text || text->empty()) {
			return api::Fail(api::error::kInvalid, "chat or text");
		} else if (!chatInScope(*id) || *id == api::kServiceChat) {
			return api::Fail(api::error::kDenied, "this chat");
		}
		const auto chat = data->chat(*id);
		if (!chat || chat->secret) {
			return api::Fail(api::error::kNotFound, "chat");
		}
		return data->send(*id, *text)
			? api::Ok()
			: api::Fail(api::error::kFailed, "the message was not sent");
	} else if (op == "ui.toast") {
		if (const auto refused = need("ui.toast")) {
			return *refused;
		}
		const auto text = TextArg(args, "text", kMaxToast);
		if (!text) {
			return api::Fail(api::error::kInvalid, "text");
		}
		data->toast(_grants.pluginId + ": " + *text);
		return api::Ok();
	}
	return api::Fail(api::error::kInvalid, "unknown operation");
}

std::optional<cbor::Value> Broker::filterEvent(const cbor::Value &event) const {
	const auto type = event.find("type");
	const auto name = type ? type->asText() : nullptr;
	if (!name) {
		return std::nullopt;
	}
	const auto chat = event.find("chat");
	const auto id = chat ? chat->asInt() : std::nullopt;
	const auto secret = event.find("secret");
	if (name->rfind("message.", 0) == 0) {
		if (!has("messages.read") || !id) {
			return std::nullopt;
		}
		const auto isSecret = secret && secret->asBool().value_or(false);
		if (!chatVisible(*id, isSecret)) {
			return std::nullopt;
		}
		if (*name == "message.deleted" && !has("messages.read_deleted")) {
			auto reduced = cbor::Map();
			for (const auto &entry : *event.asMap()) {
				if (entry.key != "text" && entry.key != "old_text") {
					reduced.push_back(entry);
				}
			}
			return cbor::Value::FromMap(std::move(reduced));
		}
		return event;
	}
	return std::nullopt;
}

} // namespace fgpl
