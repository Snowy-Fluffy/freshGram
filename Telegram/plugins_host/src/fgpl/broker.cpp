#include "fgpl/broker.h"
#include "fgpl/manifest.h"
#include "fgpl/ui_tree.h"

#include "fgpl/net_policy.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>

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
constexpr auto kMaxRequestBody = size_t(100) << 20;
constexpr auto kMaxResponseBody = size_t(100) << 20;
constexpr auto kMaxHeaders = size_t(32);
constexpr auto kMaxHeaderValue = size_t(4096);
constexpr auto kMaxToast = size_t(200);
constexpr auto kMaxSleepMs = int64_t(60000);
constexpr auto kMaxChatsScanned = size_t(5000);
constexpr auto kSendBurst = 5.;
constexpr auto kSendEachMs = 1000.;
constexpr auto kToastEachMs = int64_t(2000);
constexpr auto kDialogEachMs = int64_t(60000);
constexpr auto kUserActionWindowMs = int64_t(10000);
constexpr auto kMaxWindows = size_t(3);
constexpr auto kWindowUpdateEachMs = int64_t(100);
constexpr auto kMaxMessage = size_t(4096);
constexpr auto kMaxSocketsPerKind = 16;
constexpr auto kMaxSocketChunk = size_t(1) << 20;
constexpr auto kMaxDatagram = size_t(65507);
constexpr auto kMaxSocketWaitMs = int64_t(30000);
constexpr auto kMaxMedia = size_t(100) << 20;
constexpr auto kMaxContacts = size_t(1000);
constexpr auto kMaxFolders = size_t(100);
constexpr auto kMaxPluginMessage = size_t(64) << 10;
constexpr auto kMaxIds = size_t(100);
constexpr auto kMaxQuery = size_t(256);
constexpr auto kMaxEmoji = size_t(64);
constexpr auto kMaxNotifyTitle = size_t(64);
constexpr auto kMaxNotifyText = size_t(256);
constexpr auto kNotifyEachMs = int64_t(5000);
constexpr auto kChatActionEachMs = int64_t(3000);
constexpr auto kMaxClipboard = size_t(64) << 10;
constexpr auto kMaxFile = size_t(100) << 20;
constexpr auto kMaxFileName = size_t(128);
constexpr auto kMaxSentMedia = size_t(100) << 20;
constexpr auto kMaxCaption = size_t(4096);
constexpr auto kMinTimerMs = int64_t(1000);
constexpr auto kMaxTimerMs = int64_t(7) * 24 * 3600 * 1000;

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

[[nodiscard]] std::optional<bool> BoolArg(
		const cbor::Value *args,
		const char *key,
		bool fallback) {
	const auto value = Arg(args, key);
	if (!value) {
		return fallback;
	}
	return value->asBool();
}

[[nodiscard]] std::optional<std::vector<int64_t>> IdsArg(
		const cbor::Value *args,
		const char *key,
		bool allowLocal = false) {
	const auto value = Arg(args, key);
	const auto list = value ? value->asArray() : nullptr;
	if (!list || list->empty() || list->size() > kMaxIds) {
		return std::nullopt;
	}
	auto result = std::vector<int64_t>();
	for (const auto &item : *list) {
		const auto id = item.asInt();
		if (!id || *id == 0 || (*id < 0 && !allowLocal)) {
			return std::nullopt;
		}
		result.push_back(*id);
	}
	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}

[[nodiscard]] std::optional<bool> MarkdownArg(const cbor::Value *args) {
	const auto value = Arg(args, "format");
	if (!value) {
		return false;
	}
	const auto text = value->asText();
	if (!text || (*text != "plain" && *text != "markdown")) {
		return std::nullopt;
	}
	return (*text == "markdown");
}

[[nodiscard]] cbor::Value Done(const std::string &error, std::string_view what) {
	return error.empty() ? api::Ok() : api::Fail(error, what);
}

[[nodiscard]] cbor::Value Sent(
		const SendResult &result,
		std::string_view what,
		bool withServerId) {
	if (!result.error.empty()) {
		return api::Fail(result.error, what);
	}
	auto answer = cbor::Map();
	if (withServerId) {
		answer.push_back({
			"id",
			result.serverId ? Int(result.serverId) : cbor::Value(),
		});
	}
	answer.push_back({
		"local_id",
		result.localId ? Int(result.localId) : cbor::Value(),
	});
	return api::Ok(cbor::Value::FromMap(std::move(answer)));
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
		{ "archived", cbor::Value::FromBool(chat.archived) },
	});
}

[[nodiscard]] cbor::Value MessageValue(const Message &message) {
	auto map = cbor::Map{
		{ "id", Int(message.id) },
		{ "chat", Int(message.chat) },
		{ "from", Int(message.from) },
		{ "date", Int(message.date) },
		{ "text", Text(message.text) },
		{ "out", cbor::Value::FromBool(message.out) },
	};
	if (message.topic) {
		map.push_back({ "topic", Int(message.topic) });
	}
	if (message.replyTo) {
		map.push_back({ "reply_to", Int(message.replyTo) });
	}
	if (!message.media.empty()) {
		map.push_back({ "media", Text(message.media) });
	}
	if (message.deleted) {
		map.push_back({ "deleted", cbor::Value::FromBool(true) });
	}
	return cbor::Value::FromMap(std::move(map));
}

[[nodiscard]] bool IsSocketCall(const std::string &op) {
	return op.rfind("net.tcp.", 0) == 0
		|| op.rfind("net.udp.", 0) == 0
		|| op.rfind("net.socket.", 0) == 0;
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

// WHY: a Qt socket works only on the thread that created it, so every socket
// call of a plugin, waited for or not, runs on one thread of its own.
class SocketWorker final {
public:
	SocketWorker()
	: _state(std::make_shared<State>())
	, _thread([state = _state] { Loop(*state); })
	, _id(_thread.get_id()) {
	}

	~SocketWorker() {
		{
			const auto lock = std::scoped_lock(_state->mutex);
			_state->stop = true;
		}
		_state->wake.notify_all();
		if (std::this_thread::get_id() == _id) {
			_thread.detach();
		} else {
			_thread.join();
		}
	}

	void post(std::function<void()> work) {
		{
			const auto lock = std::scoped_lock(_state->mutex);
			_state->queue.push_back(std::move(work));
		}
		_state->wake.notify_one();
	}

	[[nodiscard]] cbor::Value run(const std::function<cbor::Value()> &work) {
		if (std::this_thread::get_id() == _id) {
			return work();
		}
		struct Waiting {
			std::mutex mutex;
			std::condition_variable done;
			std::optional<cbor::Value> result;
		} waiting;
		post([&] {
			auto value = work();
			const auto lock = std::scoped_lock(waiting.mutex);
			waiting.result = std::move(value);
			waiting.done.notify_all();
		});
		auto lock = std::unique_lock(waiting.mutex);
		waiting.done.wait(lock, [&] { return waiting.result.has_value(); });
		return std::move(*waiting.result);
	}

private:
	struct State {
		std::mutex mutex;
		std::condition_variable wake;
		std::deque<std::function<void()>> queue;
		bool stop = false;
	};

	static void Loop(State &state) {
		while (true) {
			auto work = std::function<void()>();
			{
				auto lock = std::unique_lock(state.mutex);
				state.wake.wait(lock, [&] {
					return state.stop || !state.queue.empty();
				});
				if (state.queue.empty()) {
					return;
				}
				work = std::move(state.queue.front());
				state.queue.pop_front();
			}
			work();
		}
	}

	std::shared_ptr<State> _state;
	std::thread _thread;
	std::thread::id _id;

};

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
	_socketWorker = nullptr;
}

SocketWorker &Broker::sockets() {
	const auto lock = std::scoped_lock(_socketWorkerMutex);
	if (!_socketWorker) {
		_socketWorker = std::make_unique<SocketWorker>();
	}
	return *_socketWorker;
}

void Broker::releaseSockets() {
	const auto worker = [&] {
		const auto lock = std::scoped_lock(_socketWorkerMutex);
		return _socketWorker.get();
	}();
	if (!worker) {
		return;
	}
	(void)worker->run([&] {
		if (_services.transport) {
			for (const auto &[id, kind] : _sockets) {
				_services.transport->close(id);
			}
		}
		_sockets.clear();
		return cbor::Value();
	});
}

void Broker::beginOutgoing() {
	_outgoing = true;
	_outgoingEdit = OutgoingEdit();
}

OutgoingEdit Broker::finishOutgoing() {
	_outgoing = false;
	return std::exchange(_outgoingEdit, OutgoingEdit());
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
	const auto awaited = request.find("async");
	auto response = cbor::Value();
	if (!admit()) {
		response = api::Fail(api::error::kLimit, "too many calls");
	} else if (awaited && awaited->asBool().value_or(false)) {
		response = schedule(*name, args);
	} else if (_outgoing
		&& *name != "meta.info"
		&& *name != "log"
		&& *name != "messages.set_outgoing"
		&& *name != "messages.drop_outgoing") {
		response = api::Fail(api::error::kUnavailable, "not while changing an outgoing message");
	} else if (*name == "time.sleep" || *name == "files.pick" || *name == "files.save") {
		response = api::Fail(api::error::kInvalid, *name + " must be awaited");
	} else if (IsSocketCall(*name)) {
		response = sockets().run([&] { return socketCall(*name, args); });
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

cbor::Value Broker::schedule(const std::string &op, const cbor::Value *args) {
	if (_outgoing) {
		return api::Fail(api::error::kUnavailable, "not while changing an outgoing message");
	} else if (op == "messages.set_outgoing"
		|| op == "messages.drop_outgoing"
		|| op == "plugin.unload") {
		return api::Fail(api::error::kInvalid, "this call cannot be awaited");
	} else if (!_services.post || !_services.after || !_services.complete) {
		return api::Fail(api::error::kUnavailable, "awaitable calls");
	}
	const auto self = weak_from_this().lock();
	if (!self) {
		return api::Fail(api::error::kUnavailable, "awaitable calls");
	} else if (_pending.load() >= _grants.maxPending) {
		return api::Fail(api::error::kLimit, "too many calls are pending");
	}
	auto delay = int64_t(-1);
	if (op == "time.sleep") {
		const auto ms = IntArg(args, "ms");
		if (!ms || *ms < 0 || *ms > kMaxSleepMs) {
			return api::Fail(api::error::kInvalid, "ms");
		}
		delay = *ms;
	}
	const auto ticket = _nextTicket++;
	++_pending;
	const auto copy = args ? std::make_optional(*args) : std::nullopt;
	const auto finish = [self, ticket](cbor::Value result) {
		if (self->_closed) {
			result = api::Fail(api::error::kUnavailable, "the plugin stopped");
		}
		self->_services.complete(ticket, std::move(result));
		--self->_pending;
	};
	if (delay >= 0) {
		_services.after(delay, [finish] {
			finish(api::Ok());
		});
	} else if (IsSocketCall(op)) {
		sockets().post([self, op, copy, finish] {
			finish(self->_closed
				? cbor::Value()
				: self->socketCall(op, copy ? &*copy : nullptr));
		});
	} else {
		_services.post([self, op, copy, finish] {
			finish(self->_closed
				? cbor::Value()
				: self->dispatch(op, copy ? &*copy : nullptr));
		});
	}
	return api::Ok(cbor::Value::FromMap({ { "ticket", Int(ticket) } }));
}

void Broker::noteUserAction(std::string_view type, std::string_view window) {
	const auto lock = std::scoped_lock(_limitsMutex);
	_lastUserAction = _services.clockMs();
	if (type == "dialog.close") {
		_dialogOpen = false;
	} else if (type == "window.close") {
		_windows.erase(std::string(window));
		_windowUpdated.erase(std::string(window));
	}
}

std::optional<cbor::Value> Broker::throttle(const std::string &op) {
	const auto lock = std::scoped_lock(_limitsMutex);
	const auto now = _services.clockMs();
	if (op == "messages.send"
		|| op == "messages.forward"
		|| op == "messages.send_media"
		|| op == "messages.edit_media") {
		_sendTokens = std::min(
			kSendBurst,
			_sendTokens + double(now - _sendRefilled) / kSendEachMs);
		_sendRefilled = now;
		if (_sendTokens < 1.) {
			return api::Fail(api::error::kLimit, "messages are sent too often");
		}
		_sendTokens -= 1.;
	} else if (op == "ui.toast") {
		if (_lastToast && now - _lastToast < kToastEachMs) {
			return api::Fail(api::error::kLimit, "toasts are shown too often");
		}
		_lastToast = now;
	} else if (op == "notify.show") {
		if (_lastNotify && now - _lastNotify < kNotifyEachMs) {
			return api::Fail(api::error::kLimit, "notifications are shown too often");
		}
		_lastNotify = now;
	} else if (op == "ui.dialog.show") {
		const auto userAction = _lastUserAction
			&& (now - _lastUserAction < kUserActionWindowMs);
		if (!_dialogOpen
			&& !userAction
			&& _lastDialog
			&& now - _lastDialog < kDialogEachMs) {
			return api::Fail(api::error::kLimit, "dialogs open after a user action");
		}
		_lastDialog = now;
		_dialogOpen = true;
	} else if (op == "ui.dialog.close") {
		_dialogOpen = false;
	}
	return std::nullopt;
}

cbor::Value Broker::dispatch(const std::string &op, const cbor::Value *args) {
	if (auto refused = throttle(op)) {
		return std::move(*refused);
	}
	if (op == "messages.set_outgoing") {
		if (!has("messages.transform_outgoing")) {
			return api::Fail(api::error::kDenied, "messages.transform_outgoing");
		} else if (!_outgoing) {
			return api::Fail(api::error::kInvalid, "only while handling message.outgoing");
		}
		const auto text = Arg(args, "text")
			? TextArg(args, "text", kMaxMessage)
			: std::optional<std::string>();
		const auto markdown = MarkdownArg(args);
		const auto silent = Arg(args, "silent")
			? BoolArg(args, "silent", false)
			: std::optional<bool>();
		const auto schedule = IntArg(args, "schedule");
		const auto replyTo = IntArg(args, "reply_to");
		const auto topic = IntArg(args, "topic");
		const auto given = [&](const char *key) {
			return Arg(args, key) != nullptr;
		};
		if (!markdown
			|| (given("text") && (!text || text->empty()))
			|| (given("silent") && !silent)
			|| (given("schedule") && (!schedule || *schedule < 0))
			|| (given("reply_to") && (!replyTo || *replyTo < 0))
			|| (given("topic") && (!topic || *topic < 0))) {
			return api::Fail(api::error::kInvalid, "text, format, silent, schedule, reply_to or topic");
		} else if (!given("text") && !given("silent") && !given("schedule")
			&& !given("reply_to") && !given("topic")) {
			return api::Fail(api::error::kInvalid, "nothing to change");
		}
		if (text) {
			_outgoingEdit.text = *text;
			_outgoingEdit.markdown = *markdown;
		}
		if (silent) {
			_outgoingEdit.silent = *silent;
		}
		if (schedule) {
			_outgoingEdit.schedule = *schedule;
		}
		if (replyTo) {
			_outgoingEdit.replyTo = *replyTo;
		}
		if (topic) {
			_outgoingEdit.topic = *topic;
		}
		return api::Ok();
	} else if (op == "messages.drop_outgoing") {
		if (!has("messages.transform_outgoing")) {
			return api::Fail(api::error::kDenied, "messages.transform_outgoing");
		} else if (!_outgoing) {
			return api::Fail(api::error::kInvalid, "only while handling message.outgoing");
		}
		const auto reason = Arg(args, "reason")
			? TextArg(args, "reason", 200)
			: std::optional<std::string>(std::string());
		if (!reason) {
			return api::Fail(api::error::kInvalid, "reason");
		}
		_outgoingEdit.drop = true;
		_outgoingEdit.dropReason = *reason;
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
		auto chats = cbor::Value::FromText("all");
		if (_grants.chats) {
			auto list = cbor::Array();
			for (const auto id : *_grants.chats) {
				list.push_back(Int(id));
			}
			chats = cbor::Value::FromArray(std::move(list));
		}
		return api::Ok(cbor::Value::FromMap({
			{ "api", Int(api::kVersion) },
			{ "plugin", Text(_grants.pluginId) },
			{ "granted", cbor::Value::FromArray(std::move(granted)) },
			{ "chats", std::move(chats) },
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
		const auto lock = std::scoped_lock(_storageMutex);
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
	} else if (IsSocketCall(op)) {
		return socketCall(op, args);
	} else if (op.rfind("ui.dialog.", 0) == 0 || op.rfind("ui.window.", 0) == 0) {
		return uiCall(op, args);
	} else if (op.rfind("timers.", 0) == 0) {
		return timerCall(op, args);
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
	request.timeoutMs = int(std::clamp<int64_t>(timeout, 100, 300000));
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
	auto list = cbor::Array();
	for (const auto &header : response.value().headers) {
		auto name = header.first;
		std::transform(name.begin(), name.end(), name.begin(), [](char ch) {
			return char(std::tolower(static_cast<unsigned char>(ch)));
		});
		if (name == "set-cookie" || name == "set-cookie2") {
			continue;
		}
		list.push_back(cbor::Value::FromArray({ Text(name), Text(header.second) }));
		const auto existing = std::find_if(headers.begin(), headers.end(), [&](const auto &entry) {
			return entry.key == name;
		});
		if (existing != headers.end()) {
			existing->value = Text(*existing->value.asText() + ", " + header.second);
		} else {
			headers.push_back({ name, Text(header.second) });
		}
	}
	return api::Ok(cbor::Value::FromMap({
		{ "status", Int(response.value().status) },
		{ "headers", cbor::Value::FromMap(std::move(headers)) },
		{ "header_list", cbor::Value::FromArray(std::move(list)) },
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
	const auto idField = Arg(args, "id");
	const auto id = idField ? idField->asText() : nullptr;
	if (!id || !ui::ValidWindowId(*id)) {
		return api::Fail(api::error::kInvalid, "id");
	}
	if (op == "ui.window.close") {
		{
			const auto lock = std::scoped_lock(_limitsMutex);
			_windows.erase(*id);
			_windowUpdated.erase(*id);
		}
		_services.ui("window.close", cbor::Value::FromMap({ { "id", Text(*id) } }));
		return api::Ok();
	} else if (op != "ui.window.show" && op != "ui.window.update") {
		return api::Fail(api::error::kInvalid, "unknown window operation");
	}
	const auto page = Arg(args, "page");
	if (!page) {
		return api::Fail(api::error::kInvalid, "page");
	}
	const auto checked = ui::ValidatePage(*page);
	if (!checked.ok()) {
		return api::Fail(api::error::kInvalid, checked.error());
	}
	auto mode = std::string("layer");
	if (const auto field = Arg(args, "mode")) {
		if (!field->asText() || (*field->asText() != "layer" && *field->asText() != "window")) {
			return api::Fail(api::error::kInvalid, "mode");
		}
		mode = *field->asText();
	}
	auto opened = false;
	{
		const auto lock = std::scoped_lock(_limitsMutex);
		const auto now = _services.clockMs();
		const auto i = _windows.find(*id);
		if (i != _windows.end()) {
			mode = i->second;
			auto &last = _windowUpdated[*id];
			if (last && now - last < kWindowUpdateEachMs) {
				return api::Fail(api::error::kLimit, "windows are updated too often");
			}
			last = now;
		} else if (op == "ui.window.update") {
			return api::Fail(api::error::kNotFound, "the window is not open");
		} else {
			const auto userAction = _lastUserAction
				&& (now - _lastUserAction < kUserActionWindowMs);
			if (_windows.size() >= kMaxWindows) {
				return api::Fail(api::error::kLimit, "too many windows are open");
			} else if (!userAction && _lastDialog && now - _lastDialog < kDialogEachMs) {
				return api::Fail(api::error::kLimit, "windows open after a user action");
			}
			_lastDialog = now;
			_windows[*id] = mode;
			_windowUpdated[*id] = now;
			opened = true;
		}
	}
	auto payload = cbor::Value::FromMap({
		{ "id", Text(*id) },
		{ "mode", Text(mode) },
		{ "page", checked.value() },
	});
	if (!_services.ui("window", payload)) {
		if (opened) {
			const auto lock = std::scoped_lock(_limitsMutex);
			_windows.erase(*id);
		}
		return api::Fail(api::error::kUnavailable, "windows");
	}
	return api::Ok();
}

cbor::Value Broker::dataCall(const std::string &op, const cbor::Value *args) {
	const auto data = _services.data;
	if (data) {
		(void)data->takeTimedOut();
	}
	auto result = dataAnswer(op, args);
	if (data && data->takeTimedOut()) {
		return api::Fail(api::error::kUnavailable, "the app did not answer in time");
	}
	return result;
}

cbor::Value Broker::dataAnswer(const std::string &op, const cbor::Value *args) {
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
		for (const auto &chat : data->chats(kMaxChatsScanned)) {
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
		const auto deleted = has("messages.read_deleted");
		for (const auto &message : data->history(
				*id,
				limit,
				IntArg(args, "before").value_or(0),
				IntArg(args, "topic").value_or(0))) {
			if (message.chat == *id
				&& (deleted || !message.deleted)
				&& result.size() < limit) {
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
		const auto replace = Arg(args, "replace");
		if (!text || (replace && !replace->asBool())) {
			return api::Fail(api::error::kInvalid, "text or replace");
		} else if (!(replace && *replace->asBool()) && !data->draft(*id).empty()) {
			return api::Fail(api::error::kInvalid, "the draft is not empty, pass replace");
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
		const auto markdown = MarkdownArg(args);
		const auto silent = BoolArg(args, "silent", false);
		const auto replyTo = IntArg(args, "reply_to").value_or(0);
		const auto topic = IntArg(args, "topic").value_or(0);
		const auto when = IntArg(args, "schedule").value_or(0);
		if (!id || !text || text->empty()) {
			return api::Fail(api::error::kInvalid, "chat or text");
		} else if (!markdown || !silent || replyTo < 0 || topic < 0 || when < 0) {
			return api::Fail(api::error::kInvalid, "format, silent, reply_to, topic or schedule");
		} else if (!chatInScope(*id) || *id == api::kServiceChat) {
			return api::Fail(api::error::kDenied, "this chat");
		}
		const auto chat = data->chat(*id);
		if (!chat || chat->secret) {
			return api::Fail(api::error::kNotFound, "chat");
		}
		return Sent(data->sendMessage({
			.chat = *id,
			.text = *text,
			.markdown = *markdown,
			.replyTo = replyTo,
			.topic = topic,
			.silent = *silent,
			.schedule = when,
			.waitForServerId = has("messages.ids"),
		}), "the message was not sent", has("messages.ids"));
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
			if (result.size() >= limit
				|| contact.id == api::kServiceChat
				|| !chatInScope(contact.id)) {
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
		const auto thumbnail = BoolArg(args, "thumbnail", false);
		const auto download = BoolArg(args, "download", false);
		if (!chat || !message || !thumbnail || !download) {
			return api::Fail(api::error::kInvalid, "chat, message, thumbnail or download");
		} else if (*chat == api::kServiceChat || !chatInScope(*chat)) {
			return api::Fail(api::error::kDenied, "this chat");
		}
		const auto info = data->chat(*chat);
		if (!info || info->secret) {
			return api::Fail(api::error::kNotFound, "chat");
		}
		auto media = data->media(*chat, *message, *thumbnail, kMaxMedia);
		if (!media && *download) {
			auto loaded = data->downloadMedia(*chat, *message, *thumbnail, kMaxMedia);
			if (!loaded.ok()) {
				return api::Fail(loaded.error(), "the media was not downloaded");
			}
			media = std::move(loaded.value());
		}
		if (media && media->deleted && !has("messages.read_deleted")) {
			return api::Fail(api::error::kDenied, "messages.read_deleted");
		} else if (!media) {
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
	} else if (op == "media.list") {
		if (const auto refused = need("media.read")) {
			return *refused;
		}
		static const auto kinds = std::set<std::string>{
			"any", "photo", "video", "photo_video", "file", "voice",
			"round", "audio", "gif",
		};
		const auto chat = IntArg(args, "chat");
		const auto kind = TextArg(args, "kind", 16).value_or("any");
		const auto before = IntArg(args, "before").value_or(0);
		if (!chat || !kinds.contains(kind) || before < 0) {
			return api::Fail(api::error::kInvalid, "chat, kind or before");
		} else if (*chat == api::kServiceChat || !chatInScope(*chat)) {
			return api::Fail(api::error::kDenied, "this chat");
		}
		const auto info = data->chat(*chat);
		if (!info || info->secret) {
			return api::Fail(api::error::kNotFound, "chat");
		}
		const auto limit = size_t(std::clamp<int64_t>(
			IntArg(args, "limit").value_or(20),
			1,
			int64_t(kMaxHistory)));
		const auto deleted = has("messages.read_deleted");
		auto result = cbor::Array();
		for (const auto &message : data->mediaMessages(*chat, kind, limit, before)) {
			if (message.chat == *chat
				&& !message.media.empty()
				&& (deleted || !message.deleted)
				&& result.size() < limit) {
				result.push_back(MessageValue(message));
			}
		}
		return api::Ok(cbor::Value::FromArray(std::move(result)));
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
	return actionAnswer(op, args);
}

cbor::Value Broker::actionAnswer(const std::string &op, const cbor::Value *args) {
	const auto data = _services.data;
	const auto need = [&](std::initializer_list<const char*> permissions) -> std::optional<cbor::Value> {
		for (const auto permission : permissions) {
			if (!has(permission)) {
				return api::Fail(api::error::kDenied, permission);
			}
		}
		if (!data) {
			return api::Fail(api::error::kUnavailable, "data");
		}
		return std::nullopt;
	};
	const auto chatArg = [&](const char *key) -> Result<int64_t, cbor::Value> {
		const auto id = IntArg(args, key);
		if (!id) {
			return api::Fail(api::error::kInvalid, key);
		} else if (*id == api::kServiceChat || !chatInScope(*id)) {
			return api::Fail(api::error::kDenied, "this chat");
		}
		const auto chat = data->chat(*id);
		if (!chat || chat->secret) {
			return api::Fail(api::error::kNotFound, "chat");
		}
		return *id;
	};
	if (op == "messages.send_media") {
		if (const auto refused = need({ "messages.send" })) {
			return *refused;
		}
		const auto chat = chatArg("chat");
		if (!chat.ok()) {
			return chat.error();
		}
		static const auto kinds = std::set<std::string>{ "auto", "file", "voice" };
		const auto bytes = Arg(args, "data");
		const auto raw = bytes ? bytes->asBytes() : nullptr;
		const auto name = TextArg(args, "name", kMaxFileName);
		const auto kind = TextArg(args, "kind", 16).value_or("auto");
		const auto caption = Arg(args, "caption")
			? TextArg(args, "caption", kMaxCaption)
			: std::optional<std::string>(std::string());
		const auto markdown = MarkdownArg(args);
		const auto silent = BoolArg(args, "silent", false);
		const auto spoiler = BoolArg(args, "spoiler", false);
		const auto replyTo = IntArg(args, "reply_to").value_or(0);
		const auto topic = IntArg(args, "topic").value_or(0);
		const auto when = IntArg(args, "schedule").value_or(0);
		const auto duration = IntArg(args, "duration").value_or(0);
		if (!raw || raw->empty()) {
			return api::Fail(api::error::kInvalid, "data");
		} else if (raw->size() > kMaxSentMedia) {
			return api::Fail(api::error::kLimit, "the file is too large");
		} else if (!name
			|| name->empty()
			|| name->find_first_of("/\\") != std::string::npos
			|| !kinds.contains(kind)
			|| !caption
			|| !markdown
			|| !silent
			|| !spoiler
			|| replyTo < 0 || topic < 0 || when < 0
			|| duration < 0 || duration > 3600
			|| (kind == "voice" && !duration)) {
			return api::Fail(api::error::kInvalid, "name, kind, caption or options");
		}
		return Sent(data->sendMedia({
			.message = {
				.chat = chat.value(),
				.text = *caption,
				.markdown = *markdown,
				.replyTo = replyTo,
				.topic = topic,
				.silent = *silent,
				.schedule = when,
				.waitForServerId = has("messages.ids"),
			},
			.name = *name,
			.kind = kind,
			.bytes = *raw,
			.spoiler = *spoiler,
			.duration = duration,
		}), "the media was not sent", has("messages.ids"));
	} else if (op == "messages.edit") {
		if (const auto refused = need({ "messages.edit" })) {
			return *refused;
		}
		const auto chat = chatArg("chat");
		if (!chat.ok()) {
			return chat.error();
		}
		const auto id = IntArg(args, "id");
		const auto text = TextArg(args, "text", kMaxMessage);
		const auto markdown = MarkdownArg(args);
		if (!id || *id == 0 || !text || text->empty() || !markdown) {
			return api::Fail(api::error::kInvalid, "id, text or format");
		}
		return Done(data->edit(chat.value(), *id, *text, *markdown), "the message was not edited");
	} else if (op == "messages.edit_media") {
		if (const auto refused = need({ "messages.edit" })) {
			return *refused;
		}
		const auto chat = chatArg("chat");
		if (!chat.ok()) {
			return chat.error();
		}
		static const auto kinds = std::set<std::string>{ "auto", "file" };
		const auto id = IntArg(args, "id");
		const auto bytes = Arg(args, "data");
		const auto raw = bytes ? bytes->asBytes() : nullptr;
		const auto name = TextArg(args, "name", kMaxFileName);
		const auto kind = TextArg(args, "kind", 16).value_or("auto");
		const auto caption = Arg(args, "caption")
			? TextArg(args, "caption", kMaxCaption)
			: std::optional<std::string>(std::string());
		const auto markdown = MarkdownArg(args);
		const auto spoiler = BoolArg(args, "spoiler", false);
		if (!raw || raw->empty()) {
			return api::Fail(api::error::kInvalid, "data");
		} else if (raw->size() > kMaxSentMedia) {
			return api::Fail(api::error::kLimit, "the file is too large");
		} else if (!id
			|| *id == 0
			|| !name
			|| name->empty()
			|| name->find_first_of("/\\") != std::string::npos
			|| !kinds.contains(kind)
			|| !caption
			|| !markdown
			|| !spoiler) {
			return api::Fail(api::error::kInvalid, "id, name, kind, caption or options");
		}
		return Done(data->editMedia({
			.chat = chat.value(),
			.id = *id,
			.name = *name,
			.kind = kind,
			.bytes = *raw,
			.caption = *caption,
			.markdown = *markdown,
			.spoiler = *spoiler,
		}), "the media was not replaced");
	} else if (op == "messages.delete") {
		if (const auto refused = need({ "messages.delete" })) {
			return *refused;
		}
		const auto chat = chatArg("chat");
		if (!chat.ok()) {
			return chat.error();
		}
		const auto ids = IdsArg(args, "ids", true);
		const auto everyone = BoolArg(args, "everyone", true);
		if (!ids || !everyone) {
			return api::Fail(api::error::kInvalid, "ids or everyone");
		}
		return Done(data->remove(chat.value(), *ids, *everyone), "the messages were not deleted");
	} else if (op == "messages.react") {
		if (const auto refused = need({ "messages.react" })) {
			return *refused;
		}
		const auto chat = chatArg("chat");
		if (!chat.ok()) {
			return chat.error();
		}
		const auto id = IntArg(args, "id");
		const auto emoji = Arg(args, "emoji")
			? TextArg(args, "emoji", kMaxEmoji)
			: std::optional<std::string>(std::string());
		if (!id || *id == 0 || !emoji) {
			return api::Fail(api::error::kInvalid, "id or emoji");
		}
		return Done(data->react(chat.value(), *id, *emoji), "the reaction was not set");
	} else if (op == "messages.forward") {
		if (const auto refused = need({ "messages.send", "messages.read" })) {
			return *refused;
		}
		const auto from = chatArg("from_chat");
		if (!from.ok()) {
			return from.error();
		}
		const auto to = chatArg("to_chat");
		if (!to.ok()) {
			return to.error();
		}
		const auto ids = IdsArg(args, "ids");
		const auto silent = BoolArg(args, "silent", false);
		if (!ids || !silent) {
			return api::Fail(api::error::kInvalid, "ids or silent");
		}
		return Done(
			data->forward(from.value(), *ids, to.value(), *silent),
			"the messages were not forwarded");
	} else if (op == "chats.mark_read" || op == "chats.action") {
		if (const auto refused = need({ "messages.send" })) {
			return *refused;
		}
		const auto chat = chatArg("chat");
		if (!chat.ok()) {
			return chat.error();
		} else if (op == "chats.mark_read") {
			return Done(data->markRead(chat.value()), "the chat was not marked as read");
		}
		const auto kind = TextArg(args, "kind", 32).value_or("typing");
		const auto topic = IntArg(args, "topic").value_or(0);
		if (topic < 0) {
			return api::Fail(api::error::kInvalid, "topic");
		}
		{
			const auto lock = std::scoped_lock(_limitsMutex);
			const auto now = _services.clockMs();
			auto &last = _lastChatAction[chat.value()];
			if (kind != "cancel" && last && now - last < kChatActionEachMs) {
				return api::Fail(api::error::kLimit, "chat actions are sent too often");
			}
			last = now;
		}
		return Done(data->chatAction(chat.value(), topic, kind), "the action was not sent");
	} else if (op == "messages.search") {
		if (const auto refused = need({ "messages.history" })) {
			return *refused;
		}
		const auto chat = chatArg("chat");
		if (!chat.ok()) {
			return chat.error();
		}
		const auto query = TextArg(args, "query", kMaxQuery);
		if (!query || query->empty()) {
			return api::Fail(api::error::kInvalid, "query");
		}
		const auto limit = size_t(std::clamp<int64_t>(
			IntArg(args, "limit").value_or(20),
			1,
			int64_t(kMaxHistory)));
		const auto deleted = has("messages.read_deleted");
		auto result = cbor::Array();
		for (const auto &message : data->search(chat.value(), *query, limit)) {
			if (message.chat == chat.value()
				&& (deleted || !message.deleted)
				&& result.size() < limit) {
				result.push_back(MessageValue(message));
			}
		}
		return api::Ok(cbor::Value::FromArray(std::move(result)));
	} else if (op == "users.info") {
		if (const auto refused = need({ "users.info" })) {
			return *refused;
		}
		const auto id = IntArg(args, "id");
		if (!id || *id <= 0) {
			return api::Fail(api::error::kInvalid, "id");
		} else if (*id == api::kServiceChat) {
			return api::Fail(api::error::kDenied, "this user");
		}
		const auto user = data->user(*id);
		if (!user) {
			return api::Fail(api::error::kNotFound, "user");
		}
		return api::Ok(cbor::Value::FromMap({
			{ "id", Int(user->id) },
			{ "name", Text(user->name) },
			{ "username", Text(user->username) },
			{ "bot", cbor::Value::FromBool(user->bot) },
			{ "premium", cbor::Value::FromBool(user->premium) },
			{ "contact", cbor::Value::FromBool(user->contact) },
			{ "self", cbor::Value::FromBool(user->self) },
			{ "status", Text(user->status) },
			{ "last_seen", Int(user->lastSeen) },
		}));
	}
	return deviceAnswer(op, args);
}

bool Broker::recentUserAction() {
	const auto lock = std::scoped_lock(_limitsMutex);
	return _lastUserAction
		&& (_services.clockMs() - _lastUserAction < kUserActionWindowMs);
}

cbor::Value Broker::deviceAnswer(const std::string &op, const cbor::Value *args) {
	const auto data = _services.data;
	const auto need = [&](const char *permission, bool userAction) -> std::optional<cbor::Value> {
		if (!has(permission)) {
			return api::Fail(api::error::kDenied, permission);
		} else if (!data) {
			return api::Fail(api::error::kUnavailable, "data");
		} else if (userAction && !recentUserAction()) {
			return api::Fail(api::error::kDenied, "only right after a user action");
		}
		return std::nullopt;
	};
	if (op == "notify.show") {
		if (const auto refused = need("notifications", false)) {
			return *refused;
		}
		const auto title = Arg(args, "title")
			? TextArg(args, "title", kMaxNotifyTitle)
			: std::optional<std::string>(std::string());
		const auto text = TextArg(args, "text", kMaxNotifyText);
		if (!title || !text || text->empty()) {
			return api::Fail(api::error::kInvalid, "title or text");
		}
		return Done(data->notify(*title, *text), "the notification was not shown");
	} else if (op == "clipboard.write") {
		if (const auto refused = need("clipboard", true)) {
			return *refused;
		}
		const auto text = TextArg(args, "text", kMaxClipboard);
		if (!text) {
			return api::Fail(api::error::kInvalid, "text");
		}
		return Done(data->clipboardWrite(*text), "the clipboard was not changed");
	} else if (op == "clipboard.read") {
		if (const auto refused = need("clipboard", true)) {
			return *refused;
		}
		const auto text = data->clipboardRead();
		if (!text) {
			return api::Fail(api::error::kUnavailable, "the clipboard");
		} else if (text->size() > kMaxClipboard) {
			return api::Fail(api::error::kLimit, "the clipboard text is too large");
		}
		return api::Ok(Text(*text));
	} else if (op == "files.pick") {
		if (const auto refused = need("fs.user_selected", true)) {
			return *refused;
		}
		const auto title = Arg(args, "title")
			? TextArg(args, "title", kMaxNotifyTitle)
			: std::optional<std::string>(std::string());
		const auto filter = Arg(args, "filter")
			? TextArg(args, "filter", 256)
			: std::optional<std::string>(std::string());
		if (!title || !filter) {
			return api::Fail(api::error::kInvalid, "title or filter");
		}
		const auto picked = data->pickFile(*title, *filter, kMaxFile);
		if (!picked.ok()) {
			return (picked.error() == api::error::kCancelled)
				? api::Ok()
				: api::Fail(picked.error(), "the file was not read");
		} else if (picked.value().bytes.size() > kMaxFile) {
			return api::Fail(api::error::kLimit, "the file is too large");
		}
		return api::Ok(cbor::Value::FromMap({
			{ "name", Text(picked.value().name) },
			{ "data", cbor::Value::FromBytes(picked.value().bytes) },
		}));
	} else if (op == "files.save") {
		if (const auto refused = need("fs.user_selected", true)) {
			return *refused;
		}
		const auto name = TextArg(args, "name", kMaxFileName);
		const auto bytes = Arg(args, "data");
		const auto raw = bytes ? bytes->asBytes() : nullptr;
		const auto text = bytes ? bytes->asText() : nullptr;
		if (!name
			|| name->empty()
			|| name->find_first_of("/\\") != std::string::npos
			|| (!raw && !text)) {
			return api::Fail(api::error::kInvalid, "name or data");
		}
		const auto content = raw ? *raw : cbor::Bytes(text->begin(), text->end());
		if (content.size() > kMaxFile) {
			return api::Fail(api::error::kLimit, "the file is too large");
		}
		const auto error = data->saveFile(*name, content);
		return (error == api::error::kCancelled)
			? api::Ok(cbor::Value::FromBool(false))
			: error.empty()
			? api::Ok(cbor::Value::FromBool(true))
			: api::Fail(error, "the file was not saved");
	}
	return api::Fail(api::error::kInvalid, "unknown operation");
}

cbor::Value Broker::timerCall(const std::string &op, const cbor::Value *args) {
	if (!_services.setTimer || !_services.clearTimer || !_services.timers) {
		return api::Fail(api::error::kUnavailable, "timers");
	} else if (op == "timers.list") {
		auto list = cbor::Array();
		for (const auto &id : _services.timers()) {
			list.push_back(Text(id));
		}
		return api::Ok(cbor::Value::FromArray(std::move(list)));
	}
	const auto id = TextArg(args, "id", 64);
	if (!id || !ValidKey(*id)) {
		return api::Fail(api::error::kInvalid, "id");
	} else if (op == "timers.clear") {
		return api::Ok(cbor::Value::FromBool(_services.clearTimer(*id)));
	} else if (op != "timers.set") {
		return api::Fail(api::error::kInvalid, "unknown timer operation");
	}
	const auto delay = IntArg(args, "delay");
	const auto repeat = BoolArg(args, "repeat", false);
	if (!delay || *delay < kMinTimerMs || *delay > kMaxTimerMs || !repeat) {
		return api::Fail(api::error::kInvalid, "delay must be from 1000 ms to 7 days");
	}
	return Done(_services.setTimer(*id, *delay, *repeat), "the timer was not set");
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
		if (entry.key == "chat" || entry.key == "topic" || entry.key == "reply_to") {
			if (!visible) {
				continue;
			}
		} else if (entry.key == "message" || entry.key == "from") {
			if (!visible || !reading) {
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
	return FilterEvent(_grants, event);
}

std::optional<cbor::Value> FilterEvent(const Grants &grants, const cbor::Value &event) {
	const auto type = event.find("type");
	const auto name = type ? type->asText() : nullptr;
	if (!name) {
		return std::nullopt;
	}
	const auto has = [&](const char *permission) {
		return grants.permissions.contains(permission);
	};
	const auto permission = EventPermission(*name);
	if (!permission || !has(permission)) {
		return std::nullopt;
	} else if (!EventIsChatScoped(*name)) {
		return event;
	}
	const auto chat = event.find("chat");
	const auto id = chat ? chat->asInt() : std::nullopt;
	const auto secret = event.find("secret");
	const auto isSecret = secret && secret->asBool().value_or(false);
	if (!id
		|| isSecret
		|| *id == api::kServiceChat
		|| (grants.chats && !grants.chats->contains(*id))) {
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
	} else if (name->rfind("message.", 0) == 0) {
		return event;
	}
	auto reduced = cbor::Map();
	for (const auto &entry : *event.asMap()) {
		if (entry.key != "secret") {
			reduced.push_back(entry);
		}
	}
	return cbor::Value::FromMap(std::move(reduced));
	return std::nullopt;
}

} // namespace fgpl
