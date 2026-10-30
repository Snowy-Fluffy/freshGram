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
constexpr auto kMaxBadgeUsers = size_t(256);
constexpr auto kWindowUpdateEachMs = int64_t(100);
constexpr auto kMaxMessage = size_t(4096);
constexpr auto kMaxSocketsPerKind = 16;
constexpr auto kMaxSocketChunk = size_t(1) << 20;
constexpr auto kMaxDatagram = size_t(65507);
constexpr auto kMaxSocketWaitMs = int64_t(30000);
constexpr auto kMaxWsHandshake = size_t(16) << 10;
constexpr auto kMaxWsProtocols = size_t(8);
constexpr auto kMaxWsReason = size_t(123);
constexpr auto kMaxMedia = size_t(100) << 20;
constexpr auto kMaxContacts = size_t(1000);
constexpr auto kMaxFolders = size_t(100);
constexpr auto kMaxPluginMessage = size_t(64) << 10;
// The event is nested three levels inside the host frame, which the host decodes at depth 16.
constexpr auto kMaxPluginDepth = size_t(12);
constexpr auto kMaxIds = size_t(100);
constexpr auto kMaxQuery = size_t(256);
constexpr auto kMaxEmoji = size_t(64);
constexpr auto kMaxReactionsList = size_t(100);
constexpr auto kMaxStickers = size_t(100);
constexpr auto kMaxTextEmoji = size_t(100);
constexpr auto kMaxNotifyTitle = size_t(64);
constexpr auto kMaxNotifyText = size_t(256);
constexpr auto kNotifyEachMs = int64_t(5000);
constexpr auto kResolveEachMs = int64_t(1000);
constexpr auto kInlineEachMs = int64_t(1000);
constexpr auto kMaxInlineResults = size_t(50);
constexpr auto kMaxReplyChain = int64_t(20);
constexpr auto kMaxMembers = int64_t(200);
constexpr auto kMembersEachMs = int64_t(1000);
constexpr auto kModerateEachMs = int64_t(1000);
constexpr auto kMaxPollQuestion = size_t(1024);
constexpr auto kMaxPollOption = size_t(400);
constexpr auto kMaxPollOptions = size_t(10);
constexpr auto kMaxPollExplanation = size_t(800);
constexpr auto kMaxMuteSeconds = int64_t(366) * 24 * 3600;
constexpr auto kMaxContactName = size_t(64);
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

constexpr auto kMaxTranslateItems = size_t(20);
constexpr auto kMaxTranslatedItem = size_t(32) << 10;
constexpr auto kMaxTranslatedTotal = size_t(96) << 10;

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

[[nodiscard]] cbor::Bytes Encoded(const cbor::Value &value, size_t depth = 16) {
	auto bytes = cbor::Bytes();
	if (!cbor::Encode(value, bytes, depth)) {
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

[[nodiscard]] cbor::Value UserValue(const UserInfo &user) {
	return cbor::Value::FromMap({
		{ "id", Int(user.id) },
		{ "name", Text(user.name) },
		{ "username", Text(user.username) },
		{ "bot", cbor::Value::FromBool(user.bot) },
		{ "premium", cbor::Value::FromBool(user.premium) },
		{ "contact", cbor::Value::FromBool(user.contact) },
		{ "self", cbor::Value::FromBool(user.self) },
		{ "status", Text(user.status) },
		{ "last_seen", Int(user.lastSeen) },
		{ "has_photo", cbor::Value::FromBool(user.hasPhoto) },
	});
}

[[nodiscard]] cbor::Value ChannelValue(const ChannelInfo &channel) {
	auto map = cbor::Map{
		{ "id", Int(channel.id) },
		{ "title", Text(channel.title) },
		{ "username", Text(channel.username) },
		{ "type", Text(channel.type) },
		{ "about", Text(channel.about) },
		{ "verified", cbor::Value::FromBool(channel.verified) },
		{ "scam", cbor::Value::FromBool(channel.scam) },
		{ "fake", cbor::Value::FromBool(channel.fake) },
		{ "has_photo", cbor::Value::FromBool(channel.hasPhoto) },
	};
	if (channel.members >= 0) {
		map.push_back({ "members", Int(channel.members) });
	}
	return cbor::Value::FromMap(std::move(map));
}

[[nodiscard]] std::optional<std::string> NormalizedUsername(
		const std::string &text) {
	auto name = text;
	if (!name.empty() && name.front() == '@') {
		name.erase(0, 1);
	}
	if (name.size() < 4 || name.size() > 32) {
		return std::nullopt;
	}
	for (const auto ch : name) {
		const auto word = (ch >= 'a' && ch <= 'z')
			|| (ch >= 'A' && ch <= 'Z')
			|| (ch >= '0' && ch <= '9')
			|| ch == '_';
		if (!word) {
			return std::nullopt;
		}
	}
	return name;
}

[[nodiscard]] cbor::Value StickerValue(const StickerInfo &sticker) {
	return cbor::Value::FromMap({
		{ "id", Int(sticker.id) },
		{ "emoji", Text(sticker.emoji) },
		{ "set", Text(sticker.set) },
		{ "animated", cbor::Value::FromBool(sticker.animated) },
		{ "video", cbor::Value::FromBool(sticker.video) },
	});
}

void AddReactionKind(cbor::Map &map, const ReactionKind &kind) {
	if (kind.paid) {
		map.push_back({ "paid", cbor::Value::FromBool(true) });
	} else if (kind.customEmoji) {
		map.push_back({ "custom_emoji", Int(kind.customEmoji) });
	} else {
		map.push_back({ "emoji", Text(kind.emoji) });
	}
}

[[nodiscard]] std::optional<ReactionKind> ReactionArgs(
		const cbor::Value *args,
		bool *invalid) {
	const auto emoji = Arg(args, "emoji");
	const auto custom = Arg(args, "custom_emoji");
	*invalid = false;
	if (emoji && custom) {
		*invalid = true;
		return std::nullopt;
	} else if (custom) {
		const auto id = custom->asInt();
		if (!id || *id <= 0) {
			*invalid = true;
			return std::nullopt;
		}
		return ReactionKind{ .customEmoji = *id };
	} else if (emoji) {
		const auto text = TextArg(args, "emoji", kMaxEmoji);
		if (!text) {
			*invalid = true;
			return std::nullopt;
		}
		return ReactionKind{ .emoji = *text };
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<std::vector<TextEmoji>> TextEmojiArg(
		const cbor::Value *args) {
	auto result = std::vector<TextEmoji>();
	const auto value = Arg(args, "custom_emoji");
	if (!value) {
		return result;
	}
	const auto list = value->asArray();
	if (!list || list->size() > kMaxTextEmoji) {
		return std::nullopt;
	}
	for (const auto &item : *list) {
		const auto offset = item.find("offset") ? item.find("offset")->asInt() : std::nullopt;
		const auto length = item.find("length") ? item.find("length")->asInt() : std::nullopt;
		const auto id = item.find("id") ? item.find("id")->asInt() : std::nullopt;
		if (!offset || !length || !id || *offset < 0 || *length <= 0 || *id <= 0
			|| size_t(*offset) > kMaxMessage || size_t(*length) > kMaxMessage) {
			return std::nullopt;
		}
		result.push_back({ *offset, *length, *id });
	}
	return result;
}

[[nodiscard]] cbor::Value MessageValueImpl(const Message &message) {
	auto map = cbor::Map{
		{ "id", Int(message.id) },
		{ "chat", Int(message.chat) },
		{ "from", Int(message.from) },
		{ "date", Int(message.date) },
		{ "text", Text(message.text) },
		{ "out", cbor::Value::FromBool(message.out) },
	};
	if (!message.fromKind.empty()) {
		map.push_back({ "from_kind", Text(message.fromKind) });
	}
	if (message.sticker) {
		map.push_back({ "sticker", StickerValue(*message.sticker) });
	}
	if (message.emojiOnly) {
		map.push_back({ "emoji_only", cbor::Value::FromBool(true) });
	}
	if (!message.entities.empty()) {
		map.push_back({ "entities", TextEntitiesValue(message.entities) });
	}
	if (!message.reactions.empty()) {
		auto reactions = cbor::Array();
		for (const auto &reaction : message.reactions) {
			auto entry = cbor::Map();
			AddReactionKind(entry, reaction.kind);
			entry.push_back({ "count", Int(reaction.count) });
			entry.push_back({ "my", cbor::Value::FromBool(reaction.my) });
			reactions.push_back(cbor::Value::FromMap(std::move(entry)));
		}
		map.push_back({ "reactions", cbor::Value::FromArray(std::move(reactions)) });
	}
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
	if (!message.buttons.empty()) {
		auto rows = cbor::Array();
		for (const auto &row : message.buttons) {
			auto line = cbor::Array();
			for (const auto &button : row) {
				auto entry = cbor::Map{
					{ "text", Text(button.text) },
					{ "type", Text(button.type) },
				};
				if (!button.url.empty()) {
					entry.push_back({ "url", Text(button.url) });
				}
				line.push_back(cbor::Value::FromMap(std::move(entry)));
			}
			rows.push_back(cbor::Value::FromArray(std::move(line)));
		}
		map.push_back({ "buttons", cbor::Value::FromArray(std::move(rows)) });
	}
	return cbor::Value::FromMap(std::move(map));
}

[[nodiscard]] bool IsSocketCall(const std::string &op) {
	return op.rfind("net.tcp.", 0) == 0
		|| op.rfind("net.udp.", 0) == 0
		|| op.rfind("net.ws.", 0) == 0
		|| op.rfind("net.socket.", 0) == 0;
}

} // namespace

cbor::Value MessageValue(const Message &message) {
	return MessageValueImpl(message);
}

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
		_localSockets.clear();
		_ws.clear();
		return cbor::Value();
	});
}

void Broker::beginIncoming() {
	_hook = HookKind::Incoming;
	_outgoingEdit = OutgoingEdit();
}

void Broker::beginOutgoing() {
	_hook = HookKind::Outgoing;
	_outgoingEdit = OutgoingEdit();
}

OutgoingEdit Broker::finishOutgoing() {
	_hook = HookKind::None;
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
	} else if (_hook != HookKind::None
		&& *name != "meta.info"
		&& *name != "log"
		&& *name != "time.now"
		&& *name != "messages.set_outgoing"
		&& *name != "messages.drop_outgoing"
		&& *name != "messages.set_incoming"
		&& *name != "messages.hide_incoming") {
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
	if (_hook != HookKind::None) {
		return api::Fail(api::error::kUnavailable, "not while changing an outgoing message");
	} else if (op == "messages.set_outgoing"
		|| op == "messages.drop_outgoing"
		|| op == "messages.set_incoming"
		|| op == "messages.hide_incoming"
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
	// The app sends closes itself (toast timeout, refused dialog), so they are no gesture.
	if (type != "dialog.close"
		&& type != "window.close"
		&& type != "toast.close") {
		_lastUserAction = _services.clockMs();
	}
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
		|| op == "stickers.send"
		|| op == "messages.forward"
		|| op == "messages.send_media"
		|| op == "messages.edit_media"
		|| op == "messages.send_poll") {
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
	} else if (op == "users.resolve" || op == "channels.resolve") {
		if (_lastResolve && now - _lastResolve < kResolveEachMs) {
			return api::Fail(api::error::kLimit, "usernames are resolved too often");
		}
		_lastResolve = now;
	} else if (op == "chats.ban" || op == "chats.unban") {
		if (_lastModerate && now - _lastModerate < kModerateEachMs) {
			return api::Fail(api::error::kLimit, "members are moderated too often");
		}
		_lastModerate = now;
	} else if (op == "chats.members" || op == "chats.admins") {
		if (_lastMembers && now - _lastMembers < kMembersEachMs) {
			return api::Fail(api::error::kLimit, "members are listed too often");
		}
		_lastMembers = now;
	} else if (op == "bots.inline_query" || op == "bots.press_button") {
		if (_lastInline && now - _lastInline < kInlineEachMs) {
			return api::Fail(api::error::kLimit, "bots are asked too often");
		}
		_lastInline = now;
	} else if (op == "ui.dialog.show") {
		const auto userAction = _lastUserAction
			&& (now - _lastUserAction < kUserActionWindowMs);
		if (!_dialogOpen
			&& !userAction
			&& _lastDialog
			&& now - _lastDialog < kDialogEachMs) {
			return api::Fail(api::error::kLimit, "dialogs open after a user action");
		}
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
		} else if (_hook != HookKind::Outgoing) {
			return api::Fail(api::error::kInvalid, "only while handling message.outgoing");
		}
		const auto text = Arg(args, "text")
			? TextArg(args, "text", kMaxMessage)
			: std::optional<std::string>();
		const auto markdown = MarkdownArg(args);
		const auto entities = Arg(args, "entities")
			? ParseTextEntities(*Arg(args, "entities"))
			: std::optional<std::vector<TextEntity>>();
		const auto silent = Arg(args, "silent")
			? BoolArg(args, "silent", false)
			: std::optional<bool>();
		const auto schedule = IntArg(args, "schedule");
		const auto replyTo = IntArg(args, "reply_to");
		const auto topic = IntArg(args, "topic");
		const auto given = [&](const char *key) {
			return Arg(args, key) != nullptr;
		};
		if (given("entities") && (!entities || (markdown && *markdown))) {
			return api::Fail(api::error::kInvalid, "entities");
		}
		if (!markdown
			|| (given("text") && (!text || text->empty()))
			|| (given("silent") && !silent)
			|| (given("schedule") && (!schedule || *schedule < 0))
			|| (given("reply_to") && (!replyTo || *replyTo < 0))
			|| (given("topic") && (!topic || *topic < 0))) {
			return api::Fail(api::error::kInvalid, "text, format, silent, schedule, reply_to or topic");
		} else if (!given("text") && !given("entities") && !given("silent")
			&& !given("schedule") && !given("reply_to") && !given("topic")) {
			return api::Fail(api::error::kInvalid, "nothing to change");
		}
		if (text) {
			_outgoingEdit.text = *text;
			_outgoingEdit.markdown = *markdown;
		}
		if (entities) {
			_outgoingEdit.entities = *entities;
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
		} else if (_hook != HookKind::Outgoing) {
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
	} else if (op == "messages.set_incoming") {
		if (!has("messages.transform_incoming")) {
			return api::Fail(api::error::kDenied, "messages.transform_incoming");
		} else if (_hook != HookKind::Incoming) {
			return api::Fail(api::error::kInvalid, "only while handling message.incoming");
		}
		const auto text = Arg(args, "text")
			? TextArg(args, "text", kMaxMessage)
			: std::optional<std::string>();
		const auto markdown = MarkdownArg(args);
		const auto entities = Arg(args, "entities")
			? ParseTextEntities(*Arg(args, "entities"))
			: std::optional<std::vector<TextEntity>>();
		const auto given = [&](const char *key) {
			return Arg(args, key) != nullptr;
		};
		if (!markdown
			|| (given("text") && (!text || text->empty()))
			|| (given("entities") && (!entities || *markdown))) {
			return api::Fail(api::error::kInvalid, "text, format or entities");
		} else if (!given("text") && !given("entities")) {
			return api::Fail(api::error::kInvalid, "nothing to change");
		}
		if (text) {
			_outgoingEdit.text = *text;
			_outgoingEdit.markdown = *markdown;
		}
		if (entities) {
			_outgoingEdit.entities = *entities;
		}
		return api::Ok();
	} else if (op == "messages.hide_incoming") {
		if (!has("messages.transform_incoming")) {
			return api::Fail(api::error::kDenied, "messages.transform_incoming");
		} else if (_hook != HookKind::Incoming) {
			return api::Fail(api::error::kInvalid, "only while handling message.incoming");
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
	} else if (op == "translate.reply") {
		if (!has("translate.provide")) {
			return api::Fail(api::error::kDenied, "translate.provide");
		} else if (!_services.translateReply) {
			return api::Fail(api::error::kUnavailable, "translation");
		}
		const auto ticket = IntArg(args, "ticket");
		const auto markdown = MarkdownArg(args);
		const auto error = Arg(args, "error")
			? TextArg(args, "error", 200)
			: std::optional<std::string>(std::string());
		const auto list = Arg(args, "items") ? Arg(args, "items")->asArray() : nullptr;
		if (!ticket || !markdown || !error || (error->empty() && !list)) {
			return api::Fail(api::error::kInvalid, "ticket, items, format or error");
		}
		auto items = std::vector<std::string>();
		auto total = size_t(0);
		for (const auto &entry : list ? *list : cbor::Array()) {
			const auto text = entry.asText();
			if (!text || text->size() > kMaxTranslatedItem) {
				return api::Fail(api::error::kInvalid, "items");
			}
			total += text->size();
			if (items.size() >= kMaxTranslateItems || total > kMaxTranslatedTotal) {
				return api::Fail(api::error::kLimit, "the answer is too large");
			}
			items.push_back(*text);
		}
		const auto refused = _services.translateReply(
			*ticket,
			std::move(items),
			*markdown,
			*error);
		return Done(refused, "no such translation");
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
		const auto encoded = Encoded(data ? *data : cbor::Value(), kMaxPluginDepth);
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
	} else if (op == "time.now") {
		const auto data = _services.data;
		return api::Ok(Int(data
			? data->serverTime()
			: std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch()).count()));
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
	} else if (op == "ui.open_settings") {
		if (!has("ui.settings_page")) {
			return api::Fail(api::error::kDenied, "ui.settings_page");
		}
		{
			const auto lock = std::scoped_lock(_limitsMutex);
			if (!_lastUserAction
				|| _services.clockMs() - _lastUserAction >= kUserActionWindowMs) {
				return api::Fail(api::error::kDenied, "only right after a user action");
			}
			_lastUserAction = 0;
		}
		if (!_services.ui || !_services.ui("open_settings", cbor::Value::FromMap({}))) {
			return api::Fail(api::error::kUnavailable, "settings page");
		}
		return api::Ok();
	} else if (op == "badges.set" || op == "badges.clear") {
		if (!has("ui.badges")) {
			return api::Fail(api::error::kDenied, "ui.badges");
		} else if (!_services.ui) {
			return api::Fail(api::error::kUnavailable, "badges");
		} else if (op == "badges.clear") {
			{
				const auto lock = std::scoped_lock(_limitsMutex);
				_badgeUsers.clear();
			}
			_services.ui("badges.clear", cbor::Value());
			return api::Ok();
		}
		const auto user = IntArg(args, "user");
		const auto items = Arg(args, "items");
		if (!user || *user <= 0) {
			return api::Fail(api::error::kInvalid, "user");
		} else if (*user == api::kServiceChat) {
			return api::Fail(api::error::kDenied, "this user");
		} else if (!items) {
			return api::Fail(api::error::kInvalid, "items");
		}
		const auto checked = ui::ValidateBadges(*items);
		if (!checked.ok()) {
			return api::Fail(api::error::kInvalid, checked.error());
		}
		const auto clear = checked.value().asArray()->empty();
		{
			const auto lock = std::scoped_lock(_limitsMutex);
			if (clear) {
				_badgeUsers.erase(*user);
			} else if (!_badgeUsers.contains(*user)
				&& _badgeUsers.size() >= kMaxBadgeUsers) {
				return api::Fail(api::error::kQuota, "too many users have badges");
			} else {
				_badgeUsers.insert(*user);
			}
		}
		if (!_services.ui("badges", cbor::Value::FromMap({
				{ "user", Int(*user) },
				{ "items", checked.value() },
			}))) {
			return api::Fail(api::error::kUnavailable, "badges");
		}
		return api::Ok();
	} else if (op == "net.fetch") {
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
	auto local = false;
	auto parsed = net::ParseLocalUrl(*address);
	if (parsed.ok()) {
		if (parsed.value().scheme != "http" && parsed.value().scheme != "https") {
			return api::Fail(api::error::kInvalid, "only http and https URLs are fetched");
		} else if (!has("net.local")) {
			return api::Fail(api::error::kDenied, "net.local");
		} else if (!localPortAllowed(parsed.value().port)) {
			return api::Fail(api::error::kDenied, "this port");
		}
		local = true;
	} else if (parsed.error() != "not local") {
		return api::Fail(api::error::kInvalid, parsed.error());
	} else {
		if (!has("net.https")) {
			return api::Fail(api::error::kDenied, "net.https");
		}
		parsed = net::ParseUrl(*address);
		if (!parsed.ok()) {
			return api::Fail(api::error::kInvalid, parsed.error());
		}
	}
	auto request = FetchRequest();
	request.local = local;
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
	if (!local) {
		request.allowRedirect = [](const std::string &target) {
			return net::ParseUrl(target).ok();
		};
	}
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
	if (op == "net.ws.connect") {
		return wsConnect(args);
	}
	if (op == "net.tcp.connect" || op == "net.udp.connect") {
		const auto kind = (op == "net.tcp.connect") ? SocketKind::Tcp : SocketKind::Udp;
		const auto addressed = TextArg(args, "host", 255);
		const auto local = addressed && net::IsLoopbackHost(*addressed);
		const auto permission = local
			? "net.local"
			: (kind == SocketKind::Tcp) ? "net.tcp" : "net.udp";
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
		const auto endpoint = local
			? net::ParseLocalEndpoint(*host, *port)
			: net::ParseEndpoint(*host, *port);
		if (!endpoint.ok()) {
			return api::Fail(api::error::kInvalid, endpoint.error());
		} else if (local && !localPortAllowed(endpoint.value().port)) {
			return api::Fail(api::error::kDenied, "this port");
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
		request.local = local;
		request.timeoutMs = std::max(wait(10000), 100);
		const auto opened = transport->open(
			{ _grants.pluginId, _grants.account },
			request);
		if (!opened.ok()) {
			return api::Fail(opened.error().code, opened.error().message);
		}
		_sockets.emplace(opened.value(), kind);
		if (local) {
			_localSockets.insert(opened.value());
		}
		return api::Ok(cbor::Value::FromMap({ { "id", Int(opened.value()) } }));
	}
	const auto id = IntArg(args, "id");
	const auto found = id ? _sockets.find(*id) : _sockets.end();
	if (found == _sockets.end()) {
		return api::Fail(api::error::kNotFound, "socket");
	} else if (!has(_localSockets.contains(found->first)
			? "net.local"
			: found->second == SocketKind::Tcp
			? "net.tcp"
			: (found->second == SocketKind::Udp ? "net.udp" : "net.ws"))) {
		return api::Fail(api::error::kDenied, "socket");
	}
	if ((found->second == SocketKind::Ws) != (op.rfind("net.ws.", 0) == 0)) {
		return api::Fail(api::error::kInvalid, "this socket does not take that call");
	} else if (found->second == SocketKind::Ws) {
		return wsCall(op, found->first, args);
	}
	if (op == "net.socket.close") {
		transport->close(found->first);
		_localSockets.erase(found->first);
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

bool Broker::localPortAllowed(int port) const {
	if (!has("net.local")) {
		return false;
	}
	return std::any_of(
		_grants.localPorts.begin(),
		_grants.localPorts.end(),
		[&](const std::pair<int, int> &range) {
			return port >= range.first && port <= range.second;
		});
}

std::string Broker::wsWrite(int64_t id, std::span<const uint8_t> data, int timeoutMs) {
	const auto transport = _services.transport;
	while (!data.empty()) {
		const auto written = transport->write(id, data, timeoutMs);
		if (!written.ok()) {
			return written.error().code;
		} else if (!written.value()) {
			return api::error::kFailed;
		}
		data = data.subspan(std::min(written.value(), data.size()));
	}
	return {};
}

cbor::Value Broker::wsConnect(const cbor::Value *args) {
	const auto transport = _services.transport;
	if (!has("net.ws") && !has("net.local")) {
		return api::Fail(api::error::kDenied, "net.ws");
	}
	const auto open = std::count_if(_sockets.begin(), _sockets.end(), [&](const auto &entry) {
		return entry.second == SocketKind::Ws;
	});
	if (open >= kMaxSocketsPerKind) {
		return api::Fail(api::error::kLimit, "too many open sockets");
	}
	const auto text = TextArg(args, "url", 2048);
	if (!text) {
		return api::Fail(api::error::kInvalid, "url");
	}
	auto local = false;
	auto url = net::ParseLocalUrl(*text);
	if (url.ok()) {
		if (url.value().scheme != "ws" && url.value().scheme != "wss") {
			return api::Fail(api::error::kInvalid, "url must start with wss://");
		} else if (!has("net.local")) {
			return api::Fail(api::error::kDenied, "net.local");
		} else if (!localPortAllowed(url.value().port)) {
			return api::Fail(api::error::kDenied, "this port");
		}
		local = true;
	} else if (url.error() != "not local") {
		return api::Fail(api::error::kInvalid, url.error());
	} else {
		if (text->rfind("wss://", 0) != 0) {
			return api::Fail(api::error::kInvalid, "url must start with wss://");
		} else if (!has("net.ws")) {
			return api::Fail(api::error::kDenied, "net.ws");
		}
		url = net::ParseUrl("https://" + text->substr(6));
		if (!url.ok()) {
			return api::Fail(api::error::kInvalid, url.error());
		}
	}
	const auto secure = url.value().tls;
	auto extra = std::string();
	if (const auto headers = Arg(args, "headers")) {
		const auto map = headers->asMap();
		if (!map || map->size() > kMaxHeaders) {
			return api::Fail(api::error::kInvalid, "headers");
		}
		for (const auto &entry : *map) {
			const auto value = entry.value.asText();
			if (!value
				|| value->size() > kMaxHeaderValue
				|| !net::IsValidHeaderName(entry.key)
				|| net::IsForbiddenRequestHeader(entry.key)
				|| value->find_first_of("\r\n") != std::string::npos) {
				return api::Fail(api::error::kInvalid, "header " + entry.key);
			}
			extra += entry.key + ": " + *value + "\r\n";
		}
	}
	auto protocols = std::vector<std::string>();
	if (const auto list = Arg(args, "protocols")) {
		const auto array = list->asArray();
		if (!array || array->size() > kMaxWsProtocols) {
			return api::Fail(api::error::kInvalid, "protocols");
		}
		for (const auto &item : *array) {
			const auto name = item.asText();
			const auto valid = name
				&& !name->empty()
				&& name->size() <= 64
				&& std::all_of(name->begin(), name->end(), [](char ch) {
					return (ch >= 'a' && ch <= 'z')
						|| (ch >= 'A' && ch <= 'Z')
						|| (ch >= '0' && ch <= '9')
						|| ch == '.' || ch == '_' || ch == '-' || ch == '+';
				});
			if (!valid) {
				return api::Fail(api::error::kInvalid, "protocols");
			}
			protocols.push_back(*name);
		}
	}
	const auto timeout = int(std::clamp<int64_t>(
		IntArg(args, "timeout").value_or(10000),
		100,
		kMaxSocketWaitMs));
	auto request = SocketRequest();
	request.kind = SocketKind::Tcp;
	request.host = url.value().host;
	request.port = url.value().port;
	request.ip = url.value().ip;
	request.tls = secure;
	request.local = local;
	request.timeoutMs = timeout;
	const auto opened = transport->open({ _grants.pluginId, _grants.account }, request);
	if (!opened.ok()) {
		return api::Fail(opened.error().code, opened.error().message);
	}
	const auto id = opened.value();
	const auto fail = [&](const char *code, std::string message) {
		transport->close(id);
		return api::Fail(code, message);
	};
	const auto hostText = (url.value().host.find(':') != std::string::npos)
		? "[" + url.value().host + "]"
		: url.value().host;
	const auto key = ws::NewKey();
	auto head = "GET " + url.value().target + " HTTP/1.1\r\nHost: " + hostText
		+ ((url.value().port != (secure ? 443 : 80)) ? ":" + std::to_string(url.value().port) : std::string())
		+ "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
		+ "Sec-WebSocket-Key: " + key + "\r\n";
	if (!protocols.empty()) {
		head += "Sec-WebSocket-Protocol: ";
		for (auto i = size_t(0); i != protocols.size(); ++i) {
			head += (i ? ", " : "") + protocols[i];
		}
		head += "\r\n";
	}
	head += extra + "\r\n";
	if (const auto error = wsWrite(id, { reinterpret_cast<const uint8_t*>(head.data()), head.size() }, timeout);
			!error.empty()) {
		return fail(api::error::kFailed, "the handshake was not sent");
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
	auto response = std::string();
	auto end = std::string::npos;
	while ((end = response.find("\r\n\r\n")) == std::string::npos) {
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline - std::chrono::steady_clock::now()).count();
		if (left <= 0 || response.size() > kMaxWsHandshake) {
			return fail(api::error::kFailed, "the handshake did not finish");
		}
		const auto read = transport->read(id, 4096, int(left));
		if (!read.ok()) {
			return fail(api::error::kFailed, "the handshake failed");
		}
		response.append(read.value().data.begin(), read.value().data.end());
		if (read.value().closed && response.find("\r\n\r\n") == std::string::npos) {
			return fail(api::error::kFailed, "the server closed the connection");
		}
	}
	const auto lower = [](std::string value) {
		for (auto &ch : value) {
			ch = char(std::tolower(static_cast<unsigned char>(ch)));
		}
		return value;
	};
	const auto lines = response.substr(0, end);
	if (lines.rfind("HTTP/1.1 101", 0) != 0) {
		return fail(api::error::kFailed, "the server refused: " + lines.substr(0, lines.find("\r\n")).substr(0, 80));
	}
	auto accepted = false;
	auto upgraded = false;
	auto connection = false;
	auto protocol = std::string();
	auto position = lines.find("\r\n");
	while (position != std::string::npos) {
		const auto from = position + 2;
		const auto to = lines.find("\r\n", from);
		const auto line = lines.substr(from, to == std::string::npos ? std::string::npos : to - from);
		position = to;
		const auto colon = line.find(':');
		if (colon == std::string::npos) {
			continue;
		}
		const auto name = lower(line.substr(0, colon));
		auto value = line.substr(colon + 1);
		value.erase(0, value.find_first_not_of(" \t"));
		value.erase(value.find_last_not_of(" \t") + 1);
		if (name == "sec-websocket-accept") {
			accepted = (value == ws::AcceptFor(key));
		} else if (name == "upgrade") {
			upgraded = (lower(value) == "websocket");
		} else if (name == "connection") {
			connection = lower(value).find("upgrade") != std::string::npos;
		} else if (name == "sec-websocket-protocol") {
			protocol = value;
		} else if (name == "sec-websocket-extensions") {
			return fail(api::error::kFailed, "the server chose an extension");
		}
	}
	if (!accepted || !upgraded || !connection) {
		return fail(api::error::kFailed, "the server did not accept the handshake");
	} else if (!protocol.empty()
		&& std::find(protocols.begin(), protocols.end(), protocol) == protocols.end()) {
		return fail(api::error::kFailed, "the server chose a protocol that was not offered");
	}
	auto state = WsState();
	const auto rest = response.substr(end + 4);
	state.parser.feed({ reinterpret_cast<const uint8_t*>(rest.data()), rest.size() });
	_sockets.emplace(id, SocketKind::Ws);
	if (local) {
		_localSockets.insert(id);
	}
	_ws.emplace(id, std::move(state));
	return api::Ok(cbor::Value::FromMap({
		{ "id", Int(id) },
		{ "protocol", Text(protocol) },
	}));
}

cbor::Value Broker::wsReceive(int64_t id, WsState &state, int timeoutMs) {
	const auto transport = _services.transport;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	const auto closing = [&](int code, const char *error, const char *message) {
		if (!state.closeSent) {
			const auto frame = ws::CloseFrame(code, {});
			(void)wsWrite(id, frame, 1000);
			state.closeSent = true;
		}
		state.closed = true;
		return api::Fail(error, message);
	};
	const auto deliver = [&](uint8_t opcode, cbor::Bytes data) {
		if (opcode == ws::kOpText) {
			if (!ws::IsValidUtf8(data)) {
				return closing(1007, api::error::kFailed, "the text is not valid UTF-8");
			}
			return api::Ok(cbor::Value::FromMap({
				{ "kind", Text("text") },
				{ "data", Text(std::string(data.begin(), data.end())) },
				{ "closed", cbor::Value::FromBool(false) },
			}));
		}
		return api::Ok(cbor::Value::FromMap({
			{ "kind", Text("binary") },
			{ "data", cbor::Value::FromBytes(std::move(data)) },
			{ "closed", cbor::Value::FromBool(false) },
		}));
	};
	while (true) {
		auto frame = ws::Frame();
		const auto status = state.parser.next(frame);
		if (status == ws::Parser::Status::Error) {
			return closing(1002, api::error::kFailed, "the server broke the protocol");
		} else if (status == ws::Parser::Status::Frame) {
			if (frame.opcode == ws::kOpPing) {
				const auto pong = ws::EncodeFrame(ws::kOpPong, frame.payload);
				(void)wsWrite(id, pong, 1000);
			} else if (frame.opcode == ws::kOpClose) {
				auto code = 1005;
				auto reason = std::string();
				if (frame.payload.size() >= 2) {
					code = (int(frame.payload[0]) << 8) | frame.payload[1];
					const auto text = std::span<const uint8_t>(frame.payload).subspan(2);
					if (ws::IsValidUtf8(text)) {
						reason.assign(text.begin(), text.end());
					}
				}
				if (!state.closeSent) {
					const auto echo = ws::CloseFrame((code >= 1000 && code < 5000 && code != 1005 && code != 1006)
						? code
						: 1000, {});
					(void)wsWrite(id, echo, 1000);
					state.closeSent = true;
				}
				state.closed = true;
				return api::Ok(cbor::Value::FromMap({
					{ "kind", Text("close") },
					{ "code", Int(code) },
					{ "reason", Text(reason) },
					{ "closed", cbor::Value::FromBool(true) },
				}));
			} else if (frame.opcode == ws::kOpText || frame.opcode == ws::kOpBinary) {
				if (state.partial) {
					return closing(1002, api::error::kFailed, "the server broke the protocol");
				} else if (frame.fin) {
					return deliver(frame.opcode, std::move(frame.payload));
				}
				state.partial = true;
				state.messageOpcode = frame.opcode;
				state.message = std::move(frame.payload);
			} else if (frame.opcode == ws::kOpContinue) {
				if (!state.partial) {
					return closing(1002, api::error::kFailed, "the server broke the protocol");
				} else if (state.message.size() + frame.payload.size() > ws::kMaxMessage) {
					return closing(1009, api::error::kLimit, "the message is too large");
				}
				state.message.insert(state.message.end(), frame.payload.begin(), frame.payload.end());
				if (frame.fin) {
					state.partial = false;
					return deliver(state.messageOpcode, std::exchange(state.message, {}));
				}
			}
			continue;
		}
		if (state.closed) {
			return api::Ok(cbor::Value::FromMap({
				{ "kind", Text("close") },
				{ "code", Int(1006) },
				{ "reason", Text("") },
				{ "closed", cbor::Value::FromBool(true) },
			}));
		}
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline - std::chrono::steady_clock::now()).count();
		const auto read = transport->read(id, 16384, int(std::max<int64_t>(left, 0)));
		if (!read.ok()) {
			state.closed = true;
			return api::Fail(read.error().code, read.error().message);
		} else if (!read.value().data.empty()) {
			state.parser.feed(read.value().data);
			continue;
		} else if (read.value().closed) {
			state.closed = true;
			continue;
		} else if (left <= 0) {
			return api::Ok(cbor::Value::FromMap({
				{ "kind", Text("none") },
				{ "closed", cbor::Value::FromBool(false) },
			}));
		}
	}
}

cbor::Value Broker::wsCall(const std::string &op, int64_t id, const cbor::Value *args) {
	const auto transport = _services.transport;
	const auto found = _ws.find(id);
	if (found == _ws.end()) {
		return api::Fail(api::error::kNotFound, "socket");
	}
	auto &state = found->second;
	const auto wait = [&](int64_t fallback) {
		return int(std::clamp<int64_t>(
			IntArg(args, "timeout").value_or(fallback),
			0,
			kMaxSocketWaitMs));
	};
	if (op == "net.ws.close") {
		const auto code = IntArg(args, "code").value_or(1000);
		const auto reason = Arg(args, "reason")
			? TextArg(args, "reason", kMaxWsReason)
			: std::optional<std::string>(std::string());
		if (!reason || !(code == 1000 || (code >= 3000 && code < 5000))) {
			return api::Fail(api::error::kInvalid, "code or reason");
		}
		if (!state.closeSent && !state.closed) {
			const auto frame = ws::CloseFrame(int(code), *reason);
			(void)wsWrite(id, frame, 1000);
		}
		transport->close(id);
		_sockets.erase(id);
		_localSockets.erase(id);
		_ws.erase(found);
		return api::Ok();
	} else if (op == "net.ws.send") {
		const auto data = Arg(args, "data");
		auto bytes = cbor::Bytes();
		auto opcode = ws::kOpBinary;
		if (data && data->asBytes()) {
			bytes = *data->asBytes();
		} else if (data && data->asText()) {
			bytes.assign(data->asText()->begin(), data->asText()->end());
			opcode = ws::kOpText;
		} else {
			return api::Fail(api::error::kInvalid, "data");
		}
		if (bytes.size() > ws::kMaxMessage) {
			return api::Fail(api::error::kLimit, "the message is too large");
		} else if (state.closeSent || state.closed) {
			return api::Fail(api::error::kFailed, "the connection is closed");
		}
		const auto frame = ws::EncodeFrame(opcode, bytes);
		const auto error = wsWrite(id, frame, std::max(wait(10000), 100));
		if (!error.empty()) {
			state.closed = true;
			return api::Fail(error, "the message was not sent");
		}
		return api::Ok(cbor::Value::FromMap({ { "sent", Int(int64_t(bytes.size())) } }));
	} else if (op == "net.ws.recv") {
		return wsReceive(id, state, wait(1000));
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
		const auto lock = std::scoped_lock(_limitsMutex);
		_lastDialog = _services.clockMs();
		_dialogOpen = true;
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
			if (chatVisible(chat.id, chat.restricted()) && result.size() < limit) {
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
		if (!chat || chat->restricted()) {
			return api::Fail(api::error::kNotFound, "chat");
		} else if (op == "chat.info") {
			return api::Ok(ChatValue(*chat));
		}
		const auto limit = size_t(std::clamp<int64_t>(
			IntArg(args, "limit").value_or(20),
			1,
			int64_t(kMaxHistory)));
		auto fromUser = int64_t(0);
		if (const auto filter = Arg(args, "from_user")) {
			const auto name = filter->asText();
			fromUser = (name && *name == "me")
				? data->account().id
				: filter->asInt().value_or(0);
			if (fromUser <= 0 || fromUser == api::kServiceChat) {
				return api::Fail(api::error::kInvalid, "from_user");
			}
		}
		auto result = cbor::Array();
		const auto deleted = has("messages.read_deleted");
		for (const auto &message : data->history(
				*id,
				limit,
				IntArg(args, "before").value_or(0),
				IntArg(args, "topic").value_or(0),
				fromUser)) {
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
	} else if (op == "composer.draft"
		|| op == "composer.set_draft"
		|| op == "composer.insert") {
		if (const auto refused = need("composer.draft")) {
			return *refused;
		}
		const auto id = IntArg(args, "chat");
		if (!id || !chatInScope(*id) || *id == api::kServiceChat) {
			return api::Fail(api::error::kDenied, "this chat");
		}
		const auto chat = data->chat(*id);
		if (!chat || chat->restricted()) {
			return api::Fail(api::error::kNotFound, "chat");
		} else if (op == "composer.draft") {
			return api::Ok(Text(data->draft(*id)));
		}
		const auto text = TextArg(args, "text", kMaxMessage);
		if (op == "composer.insert") {
			if (!text || text->empty()) {
				return api::Fail(api::error::kInvalid, "text");
			}
			return Done(data->composerInsert(*id, *text), "the text was not inserted");
		}
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
		const auto customEmoji = TextEmojiArg(args);
		const auto entities = Arg(args, "entities")
			? ParseTextEntities(*Arg(args, "entities"))
			: std::optional<std::vector<TextEntity>>(std::vector<TextEntity>());
		if (!id || !text || text->empty()) {
			return api::Fail(api::error::kInvalid, "chat or text");
		} else if (!customEmoji) {
			return api::Fail(api::error::kInvalid, "custom_emoji");
		} else if (!entities || (markdown && *markdown && !entities->empty())) {
			return api::Fail(api::error::kInvalid, "entities");
		} else if (!markdown || !silent || replyTo < 0 || topic < 0 || when < 0) {
			return api::Fail(api::error::kInvalid, "format, silent, reply_to, topic or schedule");
		} else if (!chatInScope(*id) || *id == api::kServiceChat) {
			return api::Fail(api::error::kDenied, "this chat");
		}
		const auto chat = data->chat(*id);
		if (!chat || chat->restricted()) {
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
			.customEmoji = *customEmoji,
			.entities = *entities,
		}), "the message was not sent", has("messages.ids"));
	} else if (op == "stickers.send") {
		if (const auto refused = need("messages.send")) {
			return *refused;
		}
		const auto id = IntArg(args, "chat");
		const auto sticker = IntArg(args, "id");
		const auto silent = BoolArg(args, "silent", false);
		const auto replyTo = IntArg(args, "reply_to").value_or(0);
		const auto topic = IntArg(args, "topic").value_or(0);
		if (!id || !sticker || *sticker <= 0 || !silent || replyTo < 0 || topic < 0) {
			return api::Fail(api::error::kInvalid, "chat, id, silent, reply_to or topic");
		} else if (!chatInScope(*id) || *id == api::kServiceChat) {
			return api::Fail(api::error::kDenied, "this chat");
		}
		const auto chat = data->chat(*id);
		if (!chat || chat->restricted()) {
			return api::Fail(api::error::kNotFound, "chat");
		}
		return Sent(
			data->sendSticker(*id, *sticker, replyTo, topic, *silent),
			"the sticker was not sent",
			false);
	} else if (op == "stickers.search" || op == "stickers.recent") {
		if (const auto refused = need("messages.send")) {
			return *refused;
		}
		const auto limit = size_t(std::clamp<int64_t>(
			IntArg(args, "limit").value_or(20),
			1,
			int64_t(kMaxStickers)));
		auto emoji = std::string();
		if (op == "stickers.search") {
			const auto text = TextArg(args, "emoji", kMaxEmoji);
			if (!text || text->empty()) {
				return api::Fail(api::error::kInvalid, "emoji");
			}
			emoji = *text;
		}
		auto result = cbor::Array();
		for (const auto &sticker : data->stickers(emoji, limit)) {
			if (result.size() >= limit) {
				break;
			}
			result.push_back(StickerValue(sticker));
		}
		return api::Ok(cbor::Value::FromArray(std::move(result)));
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
		if (!info || info->restricted()) {
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
		if (!info || info->restricted()) {
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
		const auto action = Arg(args, "action");
		const auto style = TextArg(args, "style", 8).value_or("default");
		const auto seconds = Arg(args, "duration") ? IntArg(args, "duration") : std::optional<int64_t>(5);
		if (style != "default" && style != "undo") {
			return api::Fail(api::error::kInvalid, "style");
		} else if (!seconds || *seconds < 1 || *seconds > 8) {
			return api::Fail(api::error::kInvalid, "duration");
		} else if (!action && style == "default" && !Arg(args, "duration")) {
			data->toast(_grants.pluginId + ": " + *text);
			return api::Ok();
		}
		auto toast = ToastAction{
			.plugin = _grants.pluginId,
			.text = _grants.pluginId + ": " + *text,
			.undo = (style == "undo"),
			.seconds = int(*seconds),
		};
		if (action) {
			const auto id = action->asMap() ? TextArg(action, "id", 32) : std::nullopt;
			const auto label = TextArg(action, "label", 40).value_or("");
			if (!action->asMap()
				|| !id
				|| id->empty()
				|| (label.empty() && !toast.undo)) {
				return api::Fail(api::error::kInvalid, "action");
			} else if (!_services.toastOffered) {
				return api::Fail(api::error::kUnavailable, "toast actions");
			}
			toast.id = *id;
			toast.label = label;
			_services.toastOffered(*id);
		} else if (toast.undo) {
			return api::Fail(api::error::kInvalid, "undo needs an action");
		}
		data->toastAction(toast);
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
		if (!chat || chat->restricted()) {
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
		const auto entities = Arg(args, "entities")
			? ParseTextEntities(*Arg(args, "entities"))
			: std::optional<std::vector<TextEntity>>(std::vector<TextEntity>());
		if (!id || *id == 0 || !text || text->empty() || !markdown) {
			return api::Fail(api::error::kInvalid, "id, text or format");
		} else if (!entities || (*markdown && !entities->empty())) {
			return api::Fail(api::error::kInvalid, "entities");
		}
		return Done(
			data->edit(chat.value(), *id, *text, *markdown, *entities),
			"the message was not edited");
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
		auto invalid = false;
		const auto reaction = ReactionArgs(args, &invalid);
		if (!id || *id == 0 || invalid) {
			return api::Fail(api::error::kInvalid, "id, emoji or custom_emoji");
		}
		return Done(
			data->react(chat.value(), *id, reaction.value_or(ReactionKind())),
			"the reaction was not set");
	} else if (op == "messages.reactions") {
		if (const auto refused = need({ "messages.read" })) {
			return *refused;
		}
		const auto chat = chatArg("chat");
		if (!chat.ok()) {
			return chat.error();
		}
		const auto id = IntArg(args, "id");
		auto invalid = false;
		const auto filter = ReactionArgs(args, &invalid);
		const auto limit = size_t(std::clamp<int64_t>(
			IntArg(args, "limit").value_or(50),
			1,
			int64_t(kMaxReactionsList)));
		if (!id || *id == 0 || invalid) {
			return api::Fail(api::error::kInvalid, "id, emoji or custom_emoji");
		}
		const auto list = data->reactionsList(chat.value(), *id, filter, limit);
		if (!list.ok()) {
			return api::Fail(list.error(), "the reactions");
		}
		auto result = cbor::Array();
		for (const auto &entry : list.value()) {
			if (result.size() >= limit) {
				break;
			}
			auto map = cbor::Map{
				{ "user", Int(entry.user) },
				{ "name", Text(entry.name) },
				{ "date", Int(entry.date) },
			};
			AddReactionKind(map, entry.kind);
			result.push_back(cbor::Value::FromMap(std::move(map)));
		}
		return api::Ok(cbor::Value::FromArray(std::move(result)));
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
	} else if (op == "messages.get") {
		if (const auto refused = need({ "messages.history" })) {
			return *refused;
		}
		const auto chat = chatArg("chat");
		const auto id = IntArg(args, "id");
		if (!chat.ok()) {
			return chat.error();
		} else if (!id || *id <= 0) {
			return api::Fail(api::error::kInvalid, "id");
		}
		const auto found = data->message(chat.value(), *id);
		if (!found.ok()) {
			return api::Fail(found.error(), "message");
		} else if (found.value().chat != chat.value()
			|| (found.value().deleted && !has("messages.read_deleted"))) {
			return api::Fail(api::error::kNotFound, "message");
		}
		return api::Ok(MessageValue(found.value()));
	} else if (op == "messages.reply_chain") {
		if (const auto refused = need({ "messages.history" })) {
			return *refused;
		}
		const auto chat = chatArg("chat");
		const auto id = IntArg(args, "id");
		if (!chat.ok()) {
			return chat.error();
		} else if (!id || *id <= 0) {
			return api::Fail(api::error::kInvalid, "id");
		}
		const auto limit = std::clamp<int64_t>(
			IntArg(args, "limit").value_or(5),
			1,
			kMaxReplyChain);
		const auto deleted = has("messages.read_deleted");
		auto result = cbor::Array();
		auto seen = std::set<int64_t>{ *id };
		auto current = *id;
		while (int64_t(result.size()) < limit) {
			const auto child = data->message(chat.value(), current);
			if (!child.ok() || !child.value().replyTo) {
				if (result.empty() && !child.ok()) {
					return api::Fail(child.error(), "message");
				}
				break;
			}
			const auto parentId = child.value().replyTo;
			if (!seen.insert(parentId).second) {
				break;
			}
			const auto parent = data->message(chat.value(), parentId);
			if (!parent.ok()
				|| parent.value().chat != chat.value()
				|| (parent.value().deleted && !deleted)) {
				break;
			}
			result.push_back(MessageValue(parent.value()));
			current = parentId;
		}
		return api::Ok(cbor::Value::FromArray(std::move(result)));
	} else if (op == "messages.pinned" || op == "messages.scheduled") {
		if (const auto refused = need({ "messages.history" })) {
			return *refused;
		}
		const auto chat = chatArg("chat");
		if (!chat.ok()) {
			return chat.error();
		}
		const auto limit = size_t(std::clamp<int64_t>(
			IntArg(args, "limit").value_or(20),
			1,
			int64_t(kMaxHistory)));
		const auto found = (op == "messages.pinned")
			? data->pinnedMessages(chat.value(), limit)
			: data->scheduledMessages(chat.value());
		if (!found.ok()) {
			return api::Fail(found.error(), "messages");
		}
		const auto deleted = has("messages.read_deleted");
		auto result = cbor::Array();
		for (const auto &message : found.value()) {
			if (message.chat == chat.value()
				&& (deleted || !message.deleted)
				&& result.size() < limit) {
				result.push_back(MessageValue(message));
			}
		}
		return api::Ok(cbor::Value::FromArray(std::move(result)));
	} else if (op == "messages.scheduled_delete") {
		if (const auto refused = need({ "messages.delete" })) {
			return *refused;
		}
		const auto chat = chatArg("chat");
		if (!chat.ok()) {
			return chat.error();
		}
		const auto ids = IdsArg(args, "ids");
		if (!ids) {
			return api::Fail(api::error::kInvalid, "ids");
		}
		return Done(
			data->deleteScheduled(chat.value(), *ids),
			"the messages were not deleted");
	} else if (op == "messages.pin" || op == "messages.unpin") {
		if (const auto refused = need({ "chats.manage" })) {
			return *refused;
		}
		const auto chat = chatArg("chat");
		const auto id = IntArg(args, "id");
		const auto silent = BoolArg(args, "silent", false);
		if (!chat.ok()) {
			return chat.error();
		} else if (!id || *id <= 0 || !silent) {
			return api::Fail(api::error::kInvalid, "id or silent");
		}
		return Done(
			data->pinMessage(chat.value(), *id, op == "messages.pin", *silent),
			"the message was not changed");
	} else if (op == "chats.mute") {
		if (const auto refused = need({ "chats.manage" })) {
			return *refused;
		}
		const auto chat = chatArg("chat");
		if (!chat.ok()) {
			return chat.error();
		}
		const auto forever = BoolArg(args, "forever", false);
		const auto seconds = Arg(args, "seconds")
			? IntArg(args, "seconds")
			: std::optional<int64_t>(0);
		if (!forever
			|| !seconds
			|| *seconds < 0
			|| *seconds > kMaxMuteSeconds
			|| (*forever && *seconds)) {
			return api::Fail(api::error::kInvalid, "seconds or forever");
		}
		return Done(
			data->muteChat(chat.value(), *forever ? -1 : *seconds),
			"the chat was not changed");
	} else if (op == "chats.archive" || op == "chats.pin") {
		if (const auto refused = need({ "chats.manage" })) {
			return *refused;
		}
		const auto chat = chatArg("chat");
		const auto on = BoolArg(args, op == "chats.archive" ? "archived" : "pinned", true);
		if (!chat.ok()) {
			return chat.error();
		} else if (!on) {
			return api::Fail(api::error::kInvalid, "archived or pinned");
		}
		return Done(
			(op == "chats.archive")
				? data->archiveChat(chat.value(), *on)
				: data->pinChat(chat.value(), *on),
			"the chat was not changed");
	} else if (op == "users.block" || op == "users.unblock") {
		if (const auto refused = need({ "contacts.manage" })) {
			return *refused;
		}
		const auto id = IntArg(args, "id");
		if (!id || *id <= 0) {
			return api::Fail(api::error::kInvalid, "id");
		} else if (*id == api::kServiceChat) {
			return api::Fail(api::error::kDenied, "this user");
		}
		return Done(
			data->blockUser(*id, op == "users.block"),
			"the user was not changed");
	} else if (op == "contacts.add") {
		if (const auto refused = need({ "contacts.manage" })) {
			return *refused;
		}
		const auto id = IntArg(args, "id");
		const auto first = TextArg(args, "first_name", kMaxContactName);
		const auto last = Arg(args, "last_name")
			? TextArg(args, "last_name", kMaxContactName)
			: std::optional<std::string>(std::string());
		if (!id || *id <= 0 || !first || first->empty() || !last) {
			return api::Fail(api::error::kInvalid, "id, first_name or last_name");
		} else if (*id == api::kServiceChat) {
			return api::Fail(api::error::kDenied, "this user");
		}
		return Done(
			data->addContact(*id, *first, *last),
			"the contact was not added");
	} else if (op == "chats.members" || op == "chats.admins") {
		if (const auto refused = need({ "chat.info", "users.info" })) {
			return *refused;
		}
		const auto chat = chatArg("chat");
		if (!chat.ok()) {
			return chat.error();
		}
		auto kind = std::string(op == "chats.admins" ? "admins" : "recent");
		auto query = std::string();
		if (op == "chats.members" && Arg(args, "query")) {
			const auto text = TextArg(args, "query", kMaxQuery);
			if (!text || text->empty()) {
				return api::Fail(api::error::kInvalid, "query");
			}
			kind = "search";
			query = *text;
		}
		const auto limit = size_t(std::clamp<int64_t>(
			IntArg(args, "limit").value_or(50),
			1,
			kMaxMembers));
		const auto offset = IntArg(args, "offset").value_or(0);
		if (offset < 0 || offset > 1000000) {
			return api::Fail(api::error::kInvalid, "offset");
		}
		const auto found = data->members(chat.value(), kind, query, size_t(offset), limit);
		if (!found.ok()) {
			return api::Fail(found.error(), "members");
		}
		auto result = cbor::Array();
		for (const auto &member : found.value()) {
			if (member.id <= 0
				|| member.id == api::kServiceChat
				|| result.size() >= limit) {
				continue;
			}
			result.push_back(cbor::Value::FromMap({
				{ "id", Int(member.id) },
				{ "name", Text(member.name) },
				{ "username", Text(member.username) },
				{ "role", Text(member.role) },
				{ "bot", cbor::Value::FromBool(member.bot) },
			}));
		}
		return api::Ok(cbor::Value::FromArray(std::move(result)));
	} else if (op == "messages.send_poll") {
		if (const auto refused = need({ "messages.send" })) {
			return *refused;
		}
		const auto chat = chatArg("chat");
		if (!chat.ok()) {
			return chat.error();
		}
		auto poll = OutgoingPoll{ .chat = chat.value() };
		const auto question = TextArg(args, "question", kMaxPollQuestion);
		const auto list = Arg(args, "options") ? Arg(args, "options")->asArray() : nullptr;
		const auto anonymous = BoolArg(args, "anonymous", true);
		const auto multiple = BoolArg(args, "multiple", false);
		const auto quiz = BoolArg(args, "quiz", false);
		const auto silent = BoolArg(args, "silent", false);
		const auto explanation = Arg(args, "explanation")
			? TextArg(args, "explanation", kMaxPollExplanation)
			: std::optional<std::string>(std::string());
		const auto correct = Arg(args, "correct") ? IntArg(args, "correct") : std::optional<int64_t>(-1);
		const auto closePeriod = IntArg(args, "close_period").value_or(0);
		poll.replyTo = IntArg(args, "reply_to").value_or(0);
		poll.topic = IntArg(args, "topic").value_or(0);
		if (!question
			|| question->empty()
			|| !list
			|| list->size() < 2
			|| list->size() > kMaxPollOptions
			|| !anonymous
			|| !multiple
			|| !quiz
			|| !silent
			|| !explanation
			|| !correct
			|| (closePeriod != 0 && (closePeriod < 5 || closePeriod > 600))
			|| poll.replyTo < 0
			|| poll.topic < 0) {
			return api::Fail(api::error::kInvalid, "question, options or flags");
		}
		for (const auto &item : *list) {
			const auto text = item.asText();
			if (!text || text->empty() || text->size() > kMaxPollOption) {
				return api::Fail(api::error::kInvalid, "options");
			}
			poll.options.push_back(*text);
		}
		if (*quiz) {
			if (*multiple || *correct < 0 || *correct >= int64_t(poll.options.size())) {
				return api::Fail(api::error::kInvalid, "correct or multiple");
			}
		} else if (*correct != -1 || !explanation->empty()) {
			return api::Fail(api::error::kInvalid, "correct or explanation");
		}
		poll.question = *question;
		poll.anonymous = *anonymous;
		poll.multiple = *multiple;
		poll.quiz = *quiz;
		poll.correct = *correct;
		poll.explanation = *explanation;
		poll.closePeriod = closePeriod;
		poll.silent = *silent;
		return Sent(data->sendPoll(poll), "the poll was not sent", false);
	} else if (op == "chats.ban" || op == "chats.unban") {
		if (const auto refused = need({ "chats.moderate" })) {
			return *refused;
		}
		const auto chat = chatArg("chat");
		const auto user = IntArg(args, "user");
		if (!chat.ok()) {
			return chat.error();
		} else if (!user || *user <= 0) {
			return api::Fail(api::error::kInvalid, "user");
		} else if (*user == api::kServiceChat || *user == data->account().id) {
			return api::Fail(api::error::kDenied, "this user");
		} else if (!recentUserAction()) {
			return api::Fail(api::error::kDenied, "only right after a user action");
		}
		return Done(
			data->moderate(chat.value(), *user, op == "chats.ban"),
			"the member was not changed");
	} else if (op == "ghost.get" || op == "ghost.set") {
		if (const auto refused = need({ "ghost.control" })) {
			return *refused;
		}
		if (op == "ghost.get") {
			const auto state = data->ghostState();
			if (!state.ok()) {
				return api::Fail(state.error(), "ghost mode");
			}
			const auto &value = state.value();
			return api::Ok(cbor::Value::FromMap({
				{ "active", cbor::Value::FromBool(value.active) },
				{ "send_read_messages", cbor::Value::FromBool(value.readMessages) },
				{ "send_read_stories", cbor::Value::FromBool(value.readStories) },
				{ "send_online", cbor::Value::FromBool(value.onlinePackets) },
				{ "send_typing", cbor::Value::FromBool(value.uploadProgress) },
			}));
		}
		const auto enabled = Arg(args, "enabled") ? Arg(args, "enabled")->asBool() : std::nullopt;
		if (!enabled) {
			return api::Fail(api::error::kInvalid, "enabled");
		} else if (!*enabled && !recentUserAction()) {
			return api::Fail(api::error::kDenied, "only right after a user action");
		}
		return Done(data->setGhost(*enabled), "ghost mode was not changed");
	} else if (op == "bots.press_button") {
		if (const auto refused = need({ "bots.inline" })) {
			return *refused;
		}
		const auto chat = chatArg("chat");
		const auto id = IntArg(args, "id");
		const auto row = IntArg(args, "row");
		const auto column = IntArg(args, "column");
		if (!chat.ok()) {
			return chat.error();
		} else if (!id || *id <= 0 || !row || !column || *row < 0 || *column < 0) {
			return api::Fail(api::error::kInvalid, "id, row or column");
		}
		const auto answer = data->pressButton(chat.value(), *id, *row, *column);
		if (!answer.ok()) {
			return api::Fail(answer.error(), "button");
		}
		return api::Ok(cbor::Value::FromMap({
			{ "text", Text(answer.value().text) },
			{ "alert", cbor::Value::FromBool(answer.value().alert) },
			{ "url", Text(answer.value().url) },
		}));
	} else if (op == "bots.send_inline_result") {
		if (const auto refused = need({ "bots.inline", "messages.send" })) {
			return *refused;
		}
		const auto chat = chatArg("chat");
		const auto queryId = IntArg(args, "query_id");
		const auto resultId = TextArg(args, "id", 128);
		const auto silent = BoolArg(args, "silent", false);
		const auto replyTo = IntArg(args, "reply_to").value_or(0);
		const auto topic = IntArg(args, "topic").value_or(0);
		if (!chat.ok()) {
			return chat.error();
		} else if (!queryId
			|| !resultId
			|| resultId->empty()
			|| !silent
			|| replyTo < 0
			|| topic < 0) {
			return api::Fail(api::error::kInvalid, "query_id, id, silent, reply_to or topic");
		}
		return Sent(data->sendInlineResult({
			.chat = chat.value(),
			.queryId = *queryId,
			.resultId = *resultId,
			.silent = *silent,
			.replyTo = replyTo,
			.topic = topic,
		}), "the result was not sent", false);
	} else if (op == "bots.inline_query") {
		if (const auto refused = need({ "bots.inline" })) {
			return *refused;
		}
		const auto query = TextArg(args, "query", kMaxQuery);
		const auto handle = Arg(args, "bot");
		auto bot = int64_t(0);
		auto username = std::string();
		if (handle && handle->asText()) {
			const auto name = NormalizedUsername(*handle->asText());
			if (name) {
				username = *name;
			}
		} else if (handle && handle->asInt()) {
			bot = *handle->asInt();
		}
		if (!query || (username.empty() && bot <= 0)) {
			return api::Fail(api::error::kInvalid, "bot or query");
		} else if (bot == api::kServiceChat) {
			return api::Fail(api::error::kDenied, "this bot");
		}
		auto context = int64_t(0);
		if (Arg(args, "chat")) {
			const auto chat = chatArg("chat");
			if (!chat.ok()) {
				return chat.error();
			}
			context = chat.value();
		}
		const auto found = data->inlineQuery(bot, username, *query, context);
		if (!found.ok()) {
			return api::Fail(found.error(), "bot");
		}
		auto result = cbor::Array();
		for (const auto &item : found.value()) {
			if (result.size() >= kMaxInlineResults) {
				break;
			}
			result.push_back(cbor::Value::FromMap({
				{ "query_id", Int(item.queryId) },
				{ "id", Text(item.id) },
				{ "title", Text(item.title) },
				{ "description", Text(item.description) },
				{ "text", Text(item.text) },
			}));
		}
		return api::Ok(cbor::Value::FromArray(std::move(result)));
	} else if (op == "users.resolve") {
		if (const auto refused = need({ "users.info" })) {
			return *refused;
		}
		const auto text = TextArg(args, "username", 64);
		const auto name = text ? NormalizedUsername(*text) : std::nullopt;
		if (!name) {
			return api::Fail(api::error::kInvalid, "username");
		}
		const auto user = data->resolveUser(*name);
		if (!user.ok()) {
			return api::Fail(user.error(), "user");
		} else if (user.value().id == api::kServiceChat) {
			return api::Fail(api::error::kDenied, "this user");
		}
		return api::Ok(UserValue(user.value()));
	} else if (op == "users.photo") {
		if (const auto refused = need({ "users.info" })) {
			return *refused;
		}
		const auto id = IntArg(args, "id");
		if (!id || *id <= 0) {
			return api::Fail(api::error::kInvalid, "id");
		} else if (*id == api::kServiceChat) {
			return api::Fail(api::error::kDenied, "this user");
		}
		const auto photo = data->peerPhoto(*id, false, kMaxMedia);
		if (!photo.ok()) {
			return api::Fail(photo.error(), "the photo");
		} else if (photo.value().bytes.size() > kMaxMedia) {
			return api::Fail(api::error::kLimit, "the photo is too large");
		}
		return api::Ok(cbor::Value::FromMap({
			{ "mime", Text(photo.value().mime) },
			{ "data", cbor::Value::FromBytes(photo.value().bytes) },
		}));
	} else if (op == "channels.info" || op == "channels.photo") {
		if (const auto refused = need({ "chat.info" })) {
			return *refused;
		}
		const auto id = IntArg(args, "id");
		if (!id || *id <= 0) {
			return api::Fail(api::error::kInvalid, "id");
		} else if (*id == api::kServiceChat || !chatInScope(*id)) {
			return api::Fail(api::error::kDenied, "this channel");
		}
		if (op == "channels.info") {
			const auto channel = data->channel(*id);
			if (!channel.ok()) {
				return api::Fail(channel.error(), "channel");
			}
			return api::Ok(ChannelValue(channel.value()));
		}
		const auto photo = data->peerPhoto(*id, true, kMaxMedia);
		if (!photo.ok()) {
			return api::Fail(photo.error(), "the photo");
		} else if (photo.value().bytes.size() > kMaxMedia) {
			return api::Fail(api::error::kLimit, "the photo is too large");
		}
		return api::Ok(cbor::Value::FromMap({
			{ "mime", Text(photo.value().mime) },
			{ "data", cbor::Value::FromBytes(photo.value().bytes) },
		}));
	} else if (op == "channels.resolve") {
		if (const auto refused = need({ "chat.info" })) {
			return *refused;
		}
		const auto text = TextArg(args, "username", 64);
		const auto name = text ? NormalizedUsername(*text) : std::nullopt;
		if (!name) {
			return api::Fail(api::error::kInvalid, "username");
		}
		const auto channel = data->resolveChannel(*name);
		if (!channel.ok()) {
			return api::Fail(channel.error(), "channel");
		} else if (channel.value().id == api::kServiceChat
			|| !chatInScope(channel.value().id)) {
			return api::Fail(api::error::kDenied, "this channel");
		}
		return api::Ok(ChannelValue(channel.value()));
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
		return api::Ok(UserValue(*user));
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
	const auto self = event.find("self");
	const auto isSecret = (secret && secret->asBool().value_or(false))
		|| (self && self->asBool().value_or(false));
	const auto visible = id && chatVisible(*id, isSecret);
	const auto reading = has("messages.read");
	auto reduced = cbor::Map();
	for (const auto &entry : *event.asMap()) {
		if (entry.key == "chat" || entry.key == "topic" || entry.key == "reply_to") {
			if (!visible) {
				continue;
			}
		} else if (entry.key == "message" || entry.key == "from" || entry.key == "ids") {
			if (!visible || !reading) {
				continue;
			}
		} else if (entry.key == "text" || entry.key == "old_text") {
			if (!visible || !reading) {
				continue;
			}
		} else if (entry.key == "secret" || entry.key == "self") {
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
	const auto self = event.find("self");
	const auto isSecret = (secret && secret->asBool().value_or(false))
		|| (self && self->asBool().value_or(false));
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
		if (entry.key != "secret" && entry.key != "self") {
			reduced.push_back(entry);
		}
	}
	return cbor::Value::FromMap(std::move(reduced));
	return std::nullopt;
}

} // namespace fgpl
