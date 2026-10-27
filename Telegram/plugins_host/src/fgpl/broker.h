#pragma once

#include "fgpl/cbor.h"
#include "fgpl/runtime.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace fgpl {

namespace api {

constexpr auto kVersion = int64_t(4);
constexpr auto kServiceChat = int64_t(777000);

namespace error {

constexpr auto kDenied = "denied";
constexpr auto kInvalid = "invalid";
constexpr auto kNotFound = "not_found";
constexpr auto kQuota = "quota";
constexpr auto kLimit = "limit";
constexpr auto kUnavailable = "unavailable";
constexpr auto kFailed = "failed";
constexpr auto kCancelled = "cancelled";

} // namespace error

[[nodiscard]] cbor::Value Ok(cbor::Value value = cbor::Value());
[[nodiscard]] cbor::Value Fail(
	std::string_view code,
	std::string_view message = {});

} // namespace api

struct Grants {
	std::string pluginId;
	std::string account;
	std::set<std::string> permissions;
	std::optional<std::set<int64_t>> chats;
	uint64_t storageQuotaBytes = uint64_t(10) << 20;
	int callsPerSecond = 200;
	int maxPending = 32;
};

class Storage {
public:
	virtual ~Storage() = default;

	[[nodiscard]] virtual std::optional<cbor::Bytes> get(
		const std::string &key) = 0;
	virtual void set(const std::string &key, cbor::Bytes value) = 0;
	virtual bool remove(const std::string &key) = 0;
	[[nodiscard]] virtual std::vector<std::string> keys(
		const std::string &prefix,
		size_t limit) = 0;
	[[nodiscard]] virtual uint64_t usedBytes() = 0;
	[[nodiscard]] virtual size_t count() = 0;
};

class MemoryStorage final : public Storage {
public:
	std::optional<cbor::Bytes> get(const std::string &key) override;
	void set(const std::string &key, cbor::Bytes value) override;
	bool remove(const std::string &key) override;
	std::vector<std::string> keys(
		const std::string &prefix,
		size_t limit) override;
	uint64_t usedBytes() override;
	size_t count() override;

private:
	std::map<std::string, cbor::Bytes> _items;

};

struct Chat {
	int64_t id = 0;
	std::string title;
	std::string type;
	int64_t unread = 0;
	bool pinned = false;
	bool secret = false;
	bool archived = false;
};

struct Message {
	int64_t id = 0;
	int64_t chat = 0;
	int64_t from = 0;
	int64_t date = 0;
	std::string text;
	int64_t topic = 0;
	int64_t replyTo = 0;
	bool out = false;
	bool deleted = false;
	std::string media;
};

struct Account {
	int64_t id = 0;
	std::string name;
	std::string username;
	bool premium = false;
	std::string phone;
};

struct Contact {
	int64_t id = 0;
	std::string name;
	std::string username;
	std::string phone;
};

struct Folder {
	int64_t id = 0;
	std::string title;
	std::vector<int64_t> chats;
};

struct AppSettings {
	std::string language;
	std::string theme;
	bool streamerMode = false;
};

struct Media {
	std::string kind;
	std::string mime;
	std::string name;
	cbor::Bytes bytes;
	bool deleted = false;
};

struct OutgoingMessage {
	int64_t chat = 0;
	std::string text;
	bool markdown = false;
	int64_t replyTo = 0;
	int64_t topic = 0;
	bool silent = false;
	int64_t schedule = 0;
	bool waitForServerId = false;
};

// What a plugin asked for while handling message.outgoing.
struct OutgoingEdit {
	std::optional<std::string> text;
	bool markdown = false;
	std::optional<bool> silent;
	std::optional<int64_t> schedule;
	std::optional<int64_t> replyTo;
	std::optional<int64_t> topic;
	bool drop = false;
	std::string dropReason;
};

struct ToastAction {
	std::string plugin;
	std::string text;
	std::string id;
	std::string label;
	bool undo = false;
	int seconds = 5;
};

struct SendResult {
	std::string error;
	int64_t localId = 0;
	int64_t serverId = 0;
};

struct OutgoingMedia {
	OutgoingMessage message;
	std::string name;
	std::string kind;
	cbor::Bytes bytes;
	bool spoiler = false;
	int64_t duration = 0;
};

struct OutgoingMediaEdit {
	int64_t chat = 0;
	int64_t id = 0;
	std::string name;
	std::string kind;
	cbor::Bytes bytes;
	std::string caption;
	bool markdown = false;
	bool spoiler = false;
};

struct UserInfo {
	int64_t id = 0;
	std::string name;
	std::string username;
	bool bot = false;
	bool premium = false;
	bool contact = false;
	bool self = false;
	std::string status;
	int64_t lastSeen = 0;
};

struct PickedFile {
	std::string name;
	cbor::Bytes bytes;
};

class DataProvider {
public:
	virtual ~DataProvider() = default;

	[[nodiscard]] virtual std::vector<Chat> chats(size_t limit) = 0;
	[[nodiscard]] virtual std::optional<Chat> chat(int64_t id) = 0;
	[[nodiscard]] virtual std::vector<Message> history(
		int64_t chat,
		size_t limit,
		int64_t before,
		int64_t topic) = 0;
	// True once after a call that could not get an answer from the app in time.
	[[nodiscard]] virtual bool takeTimedOut() {
		return false;
	}
	[[nodiscard]] virtual Account account() = 0;
	[[nodiscard]] virtual int64_t stars() = 0;
	[[nodiscard]] virtual std::string draft(int64_t chat) = 0;
	virtual bool setDraft(int64_t chat, const std::string &text) = 0;
	virtual bool send(int64_t chat, const std::string &text) = 0;
	virtual void toast(const std::string &text) = 0;
	// A toast with a button; the app tells the plugin about the click and the end.
	virtual void toastAction(const ToastAction &toast) {
		this->toast(toast.text);
	}
	[[nodiscard]] virtual std::vector<Contact> contacts(size_t limit) {
		(void)limit;
		return {};
	}
	[[nodiscard]] virtual std::vector<Folder> folders() {
		return {};
	}
	[[nodiscard]] virtual AppSettings settings() {
		return {};
	}
	[[nodiscard]] virtual std::optional<Media> media(
			int64_t chat,
			int64_t message,
			bool thumbnail,
			size_t maxBytes) {
		(void)chat;
		(void)message;
		(void)thumbnail;
		(void)maxBytes;
		return std::nullopt;
	}

	[[nodiscard]] virtual Result<Media, std::string> downloadMedia(
			int64_t chat,
			int64_t message,
			bool thumbnail,
			size_t maxBytes) {
		(void)chat;
		(void)message;
		(void)thumbnail;
		(void)maxBytes;
		return std::string(api::error::kUnavailable);
	}
	[[nodiscard]] virtual std::vector<Message> mediaMessages(
			int64_t chat,
			const std::string &kind,
			size_t limit,
			int64_t before) {
		(void)chat;
		(void)kind;
		(void)limit;
		(void)before;
		return {};
	}
	[[nodiscard]] virtual SendResult sendMedia(const OutgoingMedia &media) {
		(void)media;
		return { .error = api::error::kUnavailable };
	}

	// The sends answer an error code, or an empty one on success.
	// A message id may be a server id or a local one (negative).
	[[nodiscard]] virtual SendResult sendMessage(const OutgoingMessage &message) {
		if (message.markdown || message.replyTo || message.topic
			|| message.silent || message.schedule) {
			return { .error = api::error::kUnavailable };
		}
		return {
			.error = send(message.chat, message.text)
				? std::string()
				: std::string(api::error::kFailed),
		};
	}

	// The calls below answer an error code, or an empty string on success.
	[[nodiscard]] virtual std::string editMedia(const OutgoingMediaEdit &edit) {
		(void)edit;
		return api::error::kUnavailable;
	}
	[[nodiscard]] virtual std::string edit(
			int64_t chat,
			int64_t id,
			const std::string &text,
			bool markdown) {
		(void)chat;
		(void)id;
		(void)text;
		(void)markdown;
		return api::error::kUnavailable;
	}
	[[nodiscard]] virtual std::string remove(
			int64_t chat,
			const std::vector<int64_t> &ids,
			bool everyone) {
		(void)chat;
		(void)ids;
		(void)everyone;
		return api::error::kUnavailable;
	}
	[[nodiscard]] virtual std::string react(
			int64_t chat,
			int64_t id,
			const std::string &emoji) {
		(void)chat;
		(void)id;
		(void)emoji;
		return api::error::kUnavailable;
	}
	[[nodiscard]] virtual std::string forward(
			int64_t from,
			const std::vector<int64_t> &ids,
			int64_t to,
			bool silent) {
		(void)from;
		(void)ids;
		(void)to;
		(void)silent;
		return api::error::kUnavailable;
	}
	[[nodiscard]] virtual std::string markRead(int64_t chat) {
		(void)chat;
		return api::error::kUnavailable;
	}
	[[nodiscard]] virtual std::string chatAction(
			int64_t chat,
			int64_t topic,
			const std::string &kind) {
		(void)chat;
		(void)topic;
		(void)kind;
		return api::error::kUnavailable;
	}
	[[nodiscard]] virtual std::vector<Message> search(
			int64_t chat,
			const std::string &query,
			size_t limit) {
		(void)chat;
		(void)query;
		(void)limit;
		return {};
	}
	[[nodiscard]] virtual std::optional<UserInfo> user(int64_t id) {
		(void)id;
		return std::nullopt;
	}
	[[nodiscard]] virtual std::string notify(
			const std::string &title,
			const std::string &text) {
		(void)title;
		(void)text;
		return api::error::kUnavailable;
	}
	[[nodiscard]] virtual std::string clipboardWrite(const std::string &text) {
		(void)text;
		return api::error::kUnavailable;
	}
	[[nodiscard]] virtual std::optional<std::string> clipboardRead() {
		return std::nullopt;
	}
	[[nodiscard]] virtual Result<PickedFile, std::string> pickFile(
			const std::string &title,
			const std::string &filter,
			size_t maxBytes) {
		(void)title;
		(void)filter;
		(void)maxBytes;
		return std::string(api::error::kUnavailable);
	}
	[[nodiscard]] virtual std::string saveFile(
			const std::string &name,
			const cbor::Bytes &bytes) {
		(void)name;
		(void)bytes;
		return api::error::kUnavailable;
	}
};

struct NetOwner {
	std::string plugin;
	std::string account;
};

struct FetchRequest {
	NetOwner owner;
	std::string method;
	std::string url;
	std::string host;
	int port = 443;
	std::vector<std::pair<std::string, std::string>> headers;
	cbor::Bytes body;
	int timeoutMs = 10000;
	size_t maxResponseBytes = 0;
	std::function<bool(const std::string &url)> allowRedirect;
};

struct FetchResponse {
	int status = 0;
	std::vector<std::pair<std::string, std::string>> headers;
	cbor::Bytes body;
};

struct FetchError {
	std::string code;
	std::string message;
};

enum class SocketKind : uint8_t {
	Tcp,
	Udp,
};

struct SocketRequest {
	SocketKind kind = SocketKind::Tcp;
	std::string host;
	int port = 0;
	bool ip = false;
	bool tls = false;
	int timeoutMs = 10000;
};

struct SocketRead {
	cbor::Bytes data;
	bool closed = false;
};

struct NetTraffic {
	uint64_t received = 0;
	uint64_t sent = 0;
	int open = 0;
};

class Transport {
public:
	virtual ~Transport() = default;

	[[nodiscard]] virtual Result<FetchResponse, FetchError> fetch(
		const FetchRequest &request) = 0;

	[[nodiscard]] virtual Result<int64_t, FetchError> open(
			const NetOwner &owner,
			const SocketRequest &request) {
		(void)owner;
		(void)request;
		return FetchError{ "unavailable", "sockets are not available" };
	}
	[[nodiscard]] virtual Result<size_t, FetchError> write(
			int64_t id,
			std::span<const uint8_t> data,
			int timeoutMs) {
		(void)id;
		(void)data;
		(void)timeoutMs;
		return FetchError{ "unavailable", "sockets are not available" };
	}
	[[nodiscard]] virtual Result<SocketRead, FetchError> read(
			int64_t id,
			size_t maxBytes,
			int timeoutMs) {
		(void)id;
		(void)maxBytes;
		(void)timeoutMs;
		return FetchError{ "unavailable", "sockets are not available" };
	}
	virtual void close(int64_t id) {
		(void)id;
	}
	virtual void abort(const NetOwner &owner) {
		(void)owner;
	}
	[[nodiscard]] virtual NetTraffic traffic(const std::string &plugin) {
		(void)plugin;
		return {};
	}
};

struct BrokerServices {
	Storage *storage = nullptr;
	DataProvider *data = nullptr;
	Transport *transport = nullptr;
	std::function<int64_t()> clockMs;
	std::function<void(std::string_view line)> log;
	std::function<void(std::string_view op, std::string_view outcome)> audit;
	std::function<bool(std::string_view kind, const cbor::Value &payload)> ui;
	std::function<void()> unload;
	std::function<std::string(const std::string &to, cbor::Value data)> sendToPlugin;
	std::function<std::vector<std::string>()> pluginPeers;
	std::function<std::string(const std::string &id, int64_t delayMs, bool repeat)> setTimer;
	std::function<bool(const std::string &id)> clearTimer;
	std::function<std::vector<std::string>()> timers;
	// Remembers an action id that may come back as toast.click.
	std::function<void(const std::string &id)> toastOffered;
	// Answers an empty string, or an error code (not_found, invalid).
	std::function<std::string(
		int64_t ticket,
		std::vector<std::string> items,
		bool markdown,
		std::string error)> translateReply;

	std::function<void(std::function<void()> work)> post;
	std::function<void(int64_t delayMs, std::function<void()> work)> after;
	std::function<void(int64_t ticket, cbor::Value result)> complete;
	std::shared_ptr<void> keepAlive;
};

[[nodiscard]] std::optional<cbor::Value> FilterEvent(
	const Grants &grants,
	const cbor::Value &event);

class SocketWorker;

class Broker final
	: public Bridge
	, public std::enable_shared_from_this<Broker> {
public:
	Broker(Grants grants, BrokerServices services);
	~Broker();

	cbor::Value call(const cbor::Value &request) override;
	void log(std::string_view line) override;

	[[nodiscard]] std::optional<cbor::Value> filterEvent(
		const cbor::Value &event) const;
	[[nodiscard]] cbor::Value filterTargeted(const cbor::Value &event) const;

	[[nodiscard]] const Grants &grants() const {
		return _grants;
	}

	void beginOutgoing();
	[[nodiscard]] OutgoingEdit finishOutgoing();
	void releaseSockets();

	[[nodiscard]] int pending() const {
		return _pending.load();
	}
	void close() {
		_closed = true;
	}
	void noteUserAction(std::string_view type, std::string_view window = {});

private:
	[[nodiscard]] cbor::Value schedule(
		const std::string &op,
		const cbor::Value *args);
	[[nodiscard]] SocketWorker &sockets();
	[[nodiscard]] bool has(const char *permission) const;
	[[nodiscard]] bool chatVisible(int64_t id, bool secret) const;
	[[nodiscard]] bool chatInScope(int64_t id) const;
	[[nodiscard]] cbor::Value dispatch(
		const std::string &op,
		const cbor::Value *args);
	[[nodiscard]] cbor::Value storageCall(
		const std::string &op,
		const cbor::Value *args);
	[[nodiscard]] cbor::Value netFetch(const cbor::Value *args);
	[[nodiscard]] cbor::Value socketCall(
		const std::string &op,
		const cbor::Value *args);
	[[nodiscard]] cbor::Value uiCall(
		const std::string &op,
		const cbor::Value *args);
	[[nodiscard]] cbor::Value dataCall(
		const std::string &op,
		const cbor::Value *args);
	[[nodiscard]] cbor::Value dataAnswer(
		const std::string &op,
		const cbor::Value *args);
	[[nodiscard]] cbor::Value actionAnswer(
		const std::string &op,
		const cbor::Value *args);
	[[nodiscard]] cbor::Value deviceAnswer(
		const std::string &op,
		const cbor::Value *args);
	[[nodiscard]] cbor::Value timerCall(
		const std::string &op,
		const cbor::Value *args);
	[[nodiscard]] bool recentUserAction();
	[[nodiscard]] bool admit();
	[[nodiscard]] std::optional<cbor::Value> throttle(const std::string &op);

	Grants _grants;
	BrokerServices _services;
	std::vector<int64_t> _recent;
	std::map<int64_t, SocketKind> _sockets;
	std::unique_ptr<SocketWorker> _socketWorker;
	std::mutex _socketWorkerMutex;
	std::mutex _storageMutex;
	std::atomic<int> _pending = 0;
	std::atomic<bool> _closed = false;
	std::mutex _limitsMutex;
	double _sendTokens = 5.;
	int64_t _sendRefilled = 0;
	int64_t _lastToast = 0;
	int64_t _lastNotify = 0;
	std::map<int64_t, int64_t> _lastChatAction;
	int64_t _lastDialog = 0;
	int64_t _lastUserAction = 0;
	bool _dialogOpen = false;
	std::set<int64_t> _badgeUsers;
	std::map<std::string, std::string> _windows;
	std::map<std::string, int64_t> _windowUpdated;
	int64_t _nextTicket = 1;
	bool _outgoing = false;
	OutgoingEdit _outgoingEdit;

};

} // namespace fgpl
