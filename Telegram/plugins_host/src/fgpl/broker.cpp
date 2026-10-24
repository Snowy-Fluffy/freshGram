#include "fgpl/broker.h"
#include "fgpl/ui_tree.h"

#include "fgpl/net_policy.h"

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
constexpr auto kMaxSocketsPerKind = 16;
constexpr auto kMaxSocketChunk = size_t(1) << 20;
constexpr auto kMaxDatagram = size_t(65507);
constexpr auto kMaxSocketWaitMs = int64_t(30000);
constexpr auto kMaxMedia = size_t(8) << 20;
constexpr auto kMaxContacts = size_t(1000);
constexpr auto kMaxFolders = size_t(100);
constexpr auto kMaxPluginMessage = size_t(64) << 10;

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

Broker::~Broker() {
	releaseSockets();
}

void Broker::releaseSockets() {
	if (_services.transport) {
		for (const auto &[id, kind] : _sockets) {
			_services.transport->close(id);
		}
	}
	_sockets.clear();
}

void Broker::beginOutgoing() {
	_outgoing = true;
	_outgoingText = std::nullopt;
}

std::optional<std::string> Broker::finishOutgoing() {
	_outgoing = false;
	return std::exchange(_outgoingText, std::nullopt);
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
	if (_outgoing
		&& op != "meta.info"
		&& op != "log"
		&& op != "messages.set_outgoing") {
		return api::Fail(api::error::kUnavailable, "not while changing an outgoing message");
	}
	if (op == "messages.set_outgoing") {
		if (!has("messages.transform_outgoing")) {
			return api::Fail(api::error::kDenied, "messages.transform_outgoing");
		} else if (!_outgoing) {
			return api::Fail(api::error::kInvalid, "only while handling message.outgoing");
		}
		const auto text = TextArg(args, "text", kMaxMessage);
		if (!text || text->empty()) {
			return api::Fail(api::error::kInvalid, "text");
		}
		_outgoingText = *text;
		return api::Ok();
	} else if (op == "plugins.send" || op == "plugins.peers") {
		if (!has("plugins.message")) {
			return api::Fail(api::error::kDenied, "plugins.message");
		} else if (!_services.sendToPlugin || !_services.pluginPeers) {
			return api::Fail(api::error::kUnavailable, "plugin messages");
		} else if (op == "plugins.peers") {
			auto peers = cbor::Array();
			for (const auto &peer : _services.pluginPeers()) {
				peers.push_back(Text(peer));
			}
			return api::Ok(cbor::Value::FromArray(std::move(peers)));
		}
		const auto to = TextArg(args, "to", 64);
		const auto data = Arg(args, "data");
		if (!to || to->empty() || *to == _grants.pluginId) {
			return api::Fail(api::error::kInvalid, "to");
		}
		const auto encoded = Encoded(data ? *data : cbor::Value());
		if (encoded.empty()) {
			return api::Fail(api::error::kInvalid, "data");
		} else if (encoded.size() > kMaxPluginMessage) {
			return api::Fail(api::error::kLimit, "the message is too large");
		}
		const auto refused = _services.sendToPlugin(*to, data ? *data : cbor::Value());
		return refused.empty() ? api::Ok() : api::Fail(refused, *to);
	} else if (op == "plugin.unload") {
		if (_services.unload) {
			_services.unload();
		}
		return api::Ok();
	} else if (op == "meta.info") {
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
	} else if (op == "ui.settings.set") {
		if (!has("ui.settings_page")) {
			return api::Fail(api::error::kDenied, "ui.settings_page");
		}
		const auto page = Arg(args, "page");
		if (!page) {
			return api::Fail(api::error::kInvalid, "page");
		}
		const auto checked = ui::ValidatePage(*page);
		if (!checked.ok()) {
			return api::Fail(api::error::kInvalid, checked.error());
		} else if (!_services.ui || !_services.ui("settings", checked.value())) {
			return api::Fail(api::error::kUnavailable, "settings page");
		}
		return api::Ok();
	} else if (op == "net.fetch") {
		if (!has("net.https")) {
			return api::Fail(api::error::kDenied, "net.https");
		}
		return netFetch(args);
	} else if (op.rfind("net.tcp.", 0) == 0
		|| op.rfind("net.udp.", 0) == 0
		|| op.rfind("net.socket.", 0) == 0) {
		return socketCall(op, args);
	} else if (op.rfind("ui.dialog.", 0) == 0) {
		return uiCall(op, args);
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
	}
	auto request = FetchRequest();
	request.owner = { _grants.pluginId, _grants.account };
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
	request.allowRedirect = [](const std::string &target) {
		return net::ParseUrl(target).ok();
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

cbor::Value Broker::socketCall(const std::string &op, const cbor::Value *args) {
	const auto transport = _services.transport;
	if (!transport) {
		return api::Fail(api::error::kUnavailable, "network");
	}
	const auto wait = [&](int64_t fallback) {
		return int(std::clamp<int64_t>(
			IntArg(args, "timeout").value_or(fallback),
			0,
			kMaxSocketWaitMs));
	};
	if (op == "net.tcp.connect" || op == "net.udp.connect") {
		const auto kind = (op == "net.tcp.connect") ? SocketKind::Tcp : SocketKind::Udp;
		const auto permission = (kind == SocketKind::Tcp) ? "net.tcp" : "net.udp";
		if (!has(permission)) {
			return api::Fail(api::error::kDenied, permission);
		}
		const auto open = std::count_if(_sockets.begin(), _sockets.end(), [&](const auto &entry) {
			return entry.second == kind;
		});
		if (open >= kMaxSocketsPerKind) {
			return api::Fail(api::error::kLimit, "too many open sockets");
		}
		const auto host = TextArg(args, "host", 255);
		const auto port = IntArg(args, "port");
		if (!host || !port) {
			return api::Fail(api::error::kInvalid, "host or port");
		}
		const auto endpoint = net::ParseEndpoint(*host, *port);
		if (!endpoint.ok()) {
			return api::Fail(api::error::kInvalid, endpoint.error());
		}
		const auto tls = Arg(args, "tls");
		if (tls && (!tls->asBool() || (*tls->asBool() && kind == SocketKind::Udp))) {
			return api::Fail(api::error::kInvalid, "tls");
		}
		auto request = SocketRequest();
		request.kind = kind;
		request.host = endpoint.value().host;
		request.port = endpoint.value().port;
		request.ip = endpoint.value().ip;
		request.tls = tls && tls->asBool().value_or(false);
		request.timeoutMs = std::max(wait(10000), 100);
		const auto opened = transport->open(
			{ _grants.pluginId, _grants.account },
			request);
		if (!opened.ok()) {
			return api::Fail(opened.error().code, opened.error().message);
		}
		_sockets.emplace(opened.value(), kind);
		return api::Ok(cbor::Value::FromMap({ { "id", Int(opened.value()) } }));
	}
	const auto id = IntArg(args, "id");
	const auto found = id ? _sockets.find(*id) : _sockets.end();
	if (found == _sockets.end()) {
		return api::Fail(api::error::kNotFound, "socket");
	} else if (!has(found->second == SocketKind::Tcp ? "net.tcp" : "net.udp")) {
		return api::Fail(api::error::kDenied, "socket");
	}
	if (op == "net.socket.close") {
		transport->close(found->first);
		_sockets.erase(found);
		return api::Ok();
	} else if (op == "net.socket.send") {
		const auto data = Arg(args, "data");
		auto bytes = cbor::Bytes();
		if (data && data->asBytes()) {
			bytes = *data->asBytes();
		} else if (data && data->asText()) {
			bytes.assign(data->asText()->begin(), data->asText()->end());
		} else {
			return api::Fail(api::error::kInvalid, "data");
		}
		const auto limit = (found->second == SocketKind::Udp) ? kMaxDatagram : kMaxSocketChunk;
		if (bytes.empty() || bytes.size() > limit) {
			return api::Fail(api::error::kLimit, "the data is too large or empty");
		}
		const auto written = transport->write(found->first, bytes, std::max(wait(10000), 100));
		if (!written.ok()) {
			return api::Fail(written.error().code, written.error().message);
		}
		return api::Ok(cbor::Value::FromMap({ { "sent", Int(int64_t(written.value())) } }));
	} else if (op == "net.socket.recv") {
		const auto most = size_t(std::clamp<int64_t>(
			IntArg(args, "max").value_or(int64_t(kMaxDatagram)),
			1,
			int64_t(kMaxSocketChunk)));
		const auto read = transport->read(found->first, most, wait(1000));
		if (!read.ok()) {
			return api::Fail(read.error().code, read.error().message);
		}
		return api::Ok(cbor::Value::FromMap({
			{ "data", cbor::Value::FromBytes(read.value().data) },
			{ "closed", cbor::Value::FromBool(read.value().closed) },
		}));
	}
	return api::Fail(api::error::kInvalid, "unknown socket operation");
}

cbor::Value Broker::uiCall(const std::string &op, const cbor::Value *args) {
	if (!has("ui.dialog")) {
		return api::Fail(api::error::kDenied, "ui.dialog");
	} else if (!_services.ui) {
		return api::Fail(api::error::kUnavailable, "dialogs");
	}
	if (op == "ui.dialog.show") {
		const auto page = Arg(args, "page");
		if (!page) {
			return api::Fail(api::error::kInvalid, "page");
		}
		const auto checked = ui::ValidatePage(*page);
		if (!checked.ok()) {
			return api::Fail(api::error::kInvalid, checked.error());
		} else if (!_services.ui("dialog", checked.value())) {
			return api::Fail(api::error::kUnavailable, "dialogs");
		}
		return api::Ok();
	} else if (op == "ui.dialog.close") {
		_services.ui("dialog.close", cbor::Value());
		return api::Ok();
	}
	return api::Fail(api::error::kInvalid, "unknown dialog operation");
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
	} else if (op == "contacts.list") {
		if (const auto refused = need("contacts.list")) {
			return *refused;
		}
		const auto limit = size_t(std::clamp<int64_t>(
			IntArg(args, "limit").value_or(200),
			1,
			int64_t(kMaxContacts)));
		auto result = cbor::Array();
		for (const auto &contact : data->contacts(limit)) {
			if (result.size() >= limit || contact.id == api::kServiceChat) {
				continue;
			}
			result.push_back(cbor::Value::FromMap({
				{ "id", Int(contact.id) },
				{ "name", Text(contact.name) },
				{ "username", Text(contact.username) },
			}));
		}
		return api::Ok(cbor::Value::FromArray(std::move(result)));
	} else if (op == "folders.list") {
		if (const auto refused = need("folders.list")) {
			return *refused;
		}
		auto result = cbor::Array();
		for (const auto &folder : data->folders()) {
			if (result.size() >= kMaxFolders) {
				break;
			}
			auto chats = cbor::Array();
			for (const auto chat : folder.chats) {
				if (chat != api::kServiceChat && chatInScope(chat) && chats.size() < kMaxChats) {
					chats.push_back(Int(chat));
				}
			}
			result.push_back(cbor::Value::FromMap({
				{ "id", Int(folder.id) },
				{ "title", Text(folder.title) },
				{ "chats", cbor::Value::FromArray(std::move(chats)) },
			}));
		}
		return api::Ok(cbor::Value::FromArray(std::move(result)));
	} else if (op == "settings.read") {
		if (const auto refused = need("settings.read")) {
			return *refused;
		}
		const auto settings = data->settings();
		return api::Ok(cbor::Value::FromMap({
			{ "language", Text(settings.language) },
			{ "theme", Text(settings.theme) },
			{ "streamerMode", cbor::Value::FromBool(settings.streamerMode) },
		}));
	} else if (op == "media.get") {
		if (const auto refused = need("media.read")) {
			return *refused;
		}
		const auto chat = IntArg(args, "chat");
		const auto message = IntArg(args, "message");
		const auto thumbnail = Arg(args, "thumbnail");
		if (!chat || !message || (thumbnail && !thumbnail->asBool())) {
			return api::Fail(api::error::kInvalid, "chat, message or thumbnail");
		} else if (*chat == api::kServiceChat || !chatInScope(*chat)) {
			return api::Fail(api::error::kDenied, "this chat");
		}
		const auto info = data->chat(*chat);
		if (!info || info->secret) {
			return api::Fail(api::error::kNotFound, "chat");
		}
		const auto media = data->media(
			*chat,
			*message,
			thumbnail && thumbnail->asBool().value_or(false),
			kMaxMedia);
		if (!media) {
			return api::Fail(api::error::kNotFound, "the media is not downloaded");
		} else if (media->bytes.size() > kMaxMedia) {
			return api::Fail(api::error::kLimit, "the media is too large");
		}
		return api::Ok(cbor::Value::FromMap({
			{ "kind", Text(media->kind) },
			{ "mime", Text(media->mime) },
			{ "name", Text(media->name) },
			{ "data", cbor::Value::FromBytes(media->bytes) },
		}));
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

cbor::Value Broker::filterTargeted(const cbor::Value &event) const {
	const auto chat = event.find("chat");
	const auto id = chat ? chat->asInt() : std::nullopt;
	const auto secret = event.find("secret");
	const auto isSecret = secret && secret->asBool().value_or(false);
	const auto visible = id && chatVisible(*id, isSecret);
	const auto reading = has("messages.read");
	auto reduced = cbor::Map();
	for (const auto &entry : *event.asMap()) {
		if (entry.key == "chat" || entry.key == "message" || entry.key == "from") {
			if (!visible) {
				continue;
			}
		} else if (entry.key == "text" || entry.key == "old_text") {
			if (!visible || !reading) {
				continue;
			}
		} else if (entry.key == "secret") {
			continue;
		}
		reduced.push_back(entry);
	}
	return cbor::Value::FromMap(std::move(reduced));
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
