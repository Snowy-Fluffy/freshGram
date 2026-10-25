#pragma once

#include "fgpl/broker.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace fakes {

using namespace fgpl;

class FakeData final : public DataProvider {
public:
	std::vector<Chat> chats(size_t) override {
		return {
			{ 1, "Alice", "private", 2, true, false },
			{ 2, "Group", "group", 0, false, false },
			{ 3, "Secret", "private", 0, false, true },
			{ api::kServiceChat, "Telegram", "private", 1, false, false },
			{ 5, "Channel", "channel", 9, false, false },
		};
	}
	std::optional<Chat> chat(int64_t id) override {
		for (const auto &item : chats(10)) {
			if (item.id == id) {
				return item;
			}
		}
		return std::nullopt;
	}
	std::vector<Message> history(int64_t chat, size_t limit, int64_t, int64_t) override {
		auto result = std::vector<Message>();
		const auto message = [](int64_t id, int64_t chat, int64_t date, std::string text) {
			auto result = Message();
			result.id = id;
			result.chat = chat;
			result.from = 1;
			result.date = date;
			result.text = std::move(text);
			return result;
		};
		auto removed = message(998, chat, 900, "removed");
		removed.deleted = true;
		result.push_back(std::move(removed));
		for (auto i = size_t(0); i != limit; ++i) {
			result.push_back(message(int64_t(100 + i), chat, 1000, "hello"));
		}
		result.push_back(message(999, chat + 1, 1000, "from another chat"));
		return result;
	}
	Account account() override {
		return { 42, "Me", "me", true, "+10000000000" };
	}
	int64_t stars() override {
		return 1234;
	}
	std::string draft(int64_t chat) override {
		return drafts[chat];
	}
	bool setDraft(int64_t chat, const std::string &text) override {
		drafts[chat] = text;
		return true;
	}
	bool send(int64_t chat, const std::string &text) override {
		sent.emplace_back(chat, text);
		return true;
	}
	void toast(const std::string &text) override {
		toasts.push_back(text);
	}
	std::vector<Contact> contacts(size_t) override {
		return {
			{ 7, "Bob", "bob", "+10000000001" },
			{ api::kServiceChat, "Telegram", "", "" },
			{ 8, "Carol", "", "+10000000002" },
		};
	}
	std::vector<Folder> folders() override {
		return { { 1, "Work", { 1, 3, api::kServiceChat, 5 } } };
	}
	AppSettings settings() override {
		return { "ru", "dark", false };
	}
	std::optional<Media> media(int64_t chat, int64_t message, bool thumbnail, size_t) override {
		if (chat == 1 && message == 100) {
			return Media{ thumbnail ? "thumbnail" : "photo", "image/jpeg", "a.jpg", cbor::Bytes(10, 1) };
		}
		return std::nullopt;
	}

	std::map<int64_t, std::string> drafts;
	std::vector<std::pair<int64_t, std::string>> sent;
	std::vector<std::string> toasts;

};

class FakeTransport final : public Transport {
public:
	Result<FetchResponse, FetchError> fetch(const FetchRequest &request) override {
		if (delayMs > 0 || holdUntilAbort) {
			++fetching;
			auto lock = std::unique_lock(mutex);
			const auto wait = std::chrono::milliseconds(holdUntilAbort ? 30000 : delayMs);
			if (wake.wait_for(lock, wait, [&] { return !aborted.empty(); })) {
				return FetchError{ "closed", "aborted" };
			}
		}
		const auto lock = std::scoped_lock(mutex);
		requests.push_back(request);
		if (fail) {
			return FetchError{ "failed", "boom" };
		}
		redirectAllowed = request.allowRedirect
			? request.allowRedirect("https://evil.example.org/x")
			: false;
		redirectToAllowed = request.allowRedirect
			? request.allowRedirect("https://api.example.com/next")
			: false;
		redirectToHttp = request.allowRedirect
			? request.allowRedirect("http://api.example.com/next")
			: true;
		redirectToPrivate = request.allowRedirect
			? request.allowRedirect("https://10.0.0.1/next")
			: true;
		auto response = FetchResponse();
		response.status = 200;
		response.headers = {
			{ "Content-Type", "text/plain" },
			{ "Set-Cookie", "a=b" },
		};
		response.body = cbor::Bytes(bodySize, 'x');
		return response;
	}

	Result<int64_t, FetchError> open(const NetOwner &owner, const SocketRequest &request) override {
		opened.push_back(request);
		owners.push_back(owner);
		if (fail) {
			return FetchError{ "connect", "refused" };
		}
		const auto id = nextId++;
		live.insert(id);
		return id;
	}
	Result<size_t, FetchError> write(int64_t id, std::span<const uint8_t> data, int) override {
		if (!live.contains(id)) {
			return FetchError{ "closed", "closed" };
		}
		written.insert(written.end(), data.begin(), data.end());
		return data.size();
	}
	Result<SocketRead, FetchError> read(int64_t id, size_t maxBytes, int) override {
		if (!live.contains(id)) {
			return FetchError{ "closed", "closed" };
		}
		auto result = SocketRead();
		result.data = cbor::Bytes(std::min<size_t>(maxBytes, 3), 'r');
		return result;
	}
	void close(int64_t id) override {
		if (live.erase(id)) {
			closed.push_back(id);
		}
	}
	void abort(const NetOwner &owner) override {
		{
			const auto lock = std::scoped_lock(mutex);
			aborted.push_back(owner);
		}
		wake.notify_all();
	}

	std::mutex mutex;
	std::condition_variable wake;
	int delayMs = 0;
	bool holdUntilAbort = false;
	std::atomic<int> fetching = 0;
	std::vector<NetOwner> aborted;
	std::vector<FetchRequest> requests;
	bool fail = false;
	size_t bodySize = 5;
	bool redirectAllowed = true;
	bool redirectToAllowed = false;
	bool redirectToHttp = true;
	bool redirectToPrivate = true;
	std::vector<SocketRequest> opened;
	std::vector<NetOwner> owners;
	std::set<int64_t> live;
	std::vector<int64_t> closed;
	cbor::Bytes written;
	int64_t nextId = 1;

};

} // namespace fakes
