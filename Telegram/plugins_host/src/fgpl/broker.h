#pragma once

#include "fgpl/cbor.h"
#include "fgpl/runtime.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fgpl {

namespace api {

constexpr auto kVersion = int64_t(1);
constexpr auto kServiceChat = int64_t(777000);

namespace error {

constexpr auto kDenied = "denied";
constexpr auto kInvalid = "invalid";
constexpr auto kNotFound = "not_found";
constexpr auto kQuota = "quota";
constexpr auto kLimit = "limit";
constexpr auto kUnavailable = "unavailable";
constexpr auto kFailed = "failed";

} // namespace error

[[nodiscard]] cbor::Value Ok(cbor::Value value = cbor::Value());
[[nodiscard]] cbor::Value Fail(
	std::string_view code,
	std::string_view message = {});

} // namespace api

struct Grants {
	std::string pluginId;
	std::set<std::string> permissions;
	std::vector<std::string> hosts;
	std::optional<std::set<int64_t>> chats;
	uint64_t storageQuotaBytes = uint64_t(10) << 20;
	int callsPerSecond = 200;
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
};

struct Message {
	int64_t id = 0;
	int64_t chat = 0;
	int64_t from = 0;
	int64_t date = 0;
	std::string text;
};

struct Account {
	int64_t id = 0;
	std::string name;
	std::string username;
	bool premium = false;
	std::string phone;
};

class DataProvider {
public:
	virtual ~DataProvider() = default;

	[[nodiscard]] virtual std::vector<Chat> chats(size_t limit) = 0;
	[[nodiscard]] virtual std::optional<Chat> chat(int64_t id) = 0;
	[[nodiscard]] virtual std::vector<Message> history(
		int64_t chat,
		size_t limit,
		int64_t before) = 0;
	[[nodiscard]] virtual Account account() = 0;
	[[nodiscard]] virtual int64_t stars() = 0;
	[[nodiscard]] virtual std::string draft(int64_t chat) = 0;
	virtual bool setDraft(int64_t chat, const std::string &text) = 0;
	virtual bool send(int64_t chat, const std::string &text) = 0;
	virtual void toast(const std::string &text) = 0;
};

struct FetchRequest {
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

class Transport {
public:
	virtual ~Transport() = default;

	[[nodiscard]] virtual Result<FetchResponse, FetchError> fetch(
		const FetchRequest &request) = 0;
};

struct BrokerServices {
	Storage *storage = nullptr;
	DataProvider *data = nullptr;
	Transport *transport = nullptr;
	std::function<int64_t()> clockMs;
	std::function<void(std::string_view line)> log;
	std::function<void(std::string_view op, std::string_view outcome)> audit;
	std::function<bool(std::string_view kind, const cbor::Value &payload)> ui;
};

class Broker final : public Bridge {
public:
	Broker(Grants grants, BrokerServices services);

	cbor::Value call(const cbor::Value &request) override;
	void log(std::string_view line) override;

	[[nodiscard]] std::optional<cbor::Value> filterEvent(
		const cbor::Value &event) const;
	[[nodiscard]] cbor::Value filterTargeted(const cbor::Value &event) const;

	[[nodiscard]] const Grants &grants() const {
		return _grants;
	}

private:
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
	[[nodiscard]] cbor::Value dataCall(
		const std::string &op,
		const cbor::Value *args);
	[[nodiscard]] bool admit();

	Grants _grants;
	BrokerServices _services;
	std::vector<int64_t> _recent;

};

} // namespace fgpl
