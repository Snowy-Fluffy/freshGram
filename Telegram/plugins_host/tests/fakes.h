#pragma once

#include "fgpl/broker.h"

#include <map>
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
	std::vector<Message> history(int64_t chat, size_t limit, int64_t) override {
		auto result = std::vector<Message>();
		for (auto i = size_t(0); i != limit; ++i) {
			result.push_back({ int64_t(100 + i), chat, 1, 1000, "hello" });
		}
		result.push_back({ 999, chat + 1, 1, 1000, "from another chat" });
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

	std::map<int64_t, std::string> drafts;
	std::vector<std::pair<int64_t, std::string>> sent;
	std::vector<std::string> toasts;

};

class FakeTransport final : public Transport {
public:
	Result<FetchResponse, FetchError> fetch(const FetchRequest &request) override {
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
		auto response = FetchResponse();
		response.status = 200;
		response.headers = {
			{ "Content-Type", "text/plain" },
			{ "Set-Cookie", "a=b" },
		};
		response.body = cbor::Bytes(bodySize, 'x');
		return response;
	}

	std::vector<FetchRequest> requests;
	bool fail = false;
	size_t bodySize = 5;
	bool redirectAllowed = true;
	bool redirectToAllowed = false;

};

} // namespace fakes
