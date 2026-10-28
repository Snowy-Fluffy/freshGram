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
	void toastAction(const ToastAction &toast) override {
		toastActions.push_back(toast);
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

	Result<Media, std::string> downloadMedia(int64_t chat, int64_t message, bool, size_t) override {
		if (chat == 1 && message == 101) {
			return Media{ "file", "application/pdf", "a.pdf", cbor::Bytes(20, 2) };
		} else if (message == 102) {
			return std::string(api::error::kLimit);
		}
		return std::string(api::error::kNotFound);
	}
	std::vector<Message> mediaMessages(int64_t chat, const std::string &kind, size_t limit, int64_t) override {
		auto result = history(chat, limit, 0, 0);
		for (auto &message : result) {
			message.media = (message.id == 100) ? std::string() : kind;
		}
		return result;
	}
	SendResult sendMedia(const OutgoingMedia &media) override {
		sentMedia.push_back(media);
		return { .error = {}, .localId = nextLocalId--, .serverId = 500 };
	}
	SendResult sendMessage(const OutgoingMessage &message) override {
		outgoing.push_back(message);
		sent.emplace_back(message.chat, message.text);
		return { .error = {}, .localId = nextLocalId--, .serverId = 500 };
	}
	std::string edit(int64_t chat, int64_t id, const std::string &text, bool markdown) override {
		actions.push_back("edit " + std::to_string(chat) + " " + std::to_string(id)
			+ " " + text + (markdown ? " md" : ""));
		return (id == 100 || id < 0) ? std::string() : std::string(api::error::kDenied);
	}
	std::string editMedia(const OutgoingMediaEdit &edit) override {
		editedMedia.push_back(edit);
		return (edit.id == 100 || edit.id < 0)
			? std::string()
			: std::string(api::error::kNotFound);
	}
	std::string remove(int64_t chat, const std::vector<int64_t> &ids, bool everyone) override {
		actions.push_back("delete " + std::to_string(chat) + " " + std::to_string(ids.size())
			+ (everyone ? " everyone" : ""));
		return {};
	}
	std::string react(int64_t chat, int64_t id, const ReactionKind &reaction) override {
		actions.push_back("react " + std::to_string(chat) + " " + std::to_string(id) + " " + reaction.emoji
			+ (reaction.customEmoji ? "custom" + std::to_string(reaction.customEmoji) : std::string()));
		return {};
	}
	Result<std::vector<ReactionEntry>, std::string> reactionsList(
			int64_t chat,
			int64_t id,
			const std::optional<ReactionKind> &filter,
			size_t limit) override {
		if (chat != 1 || id != 100) {
			return std::string(api::error::kNotFound);
		}
		auto result = std::vector<ReactionEntry>{
			{ 7, "Bob", { "x", 0, false }, 1000 },
			{ 8, "Eve", { "", 55, false }, 1001 },
		};
		if (filter) {
			std::erase_if(result, [&](const ReactionEntry &entry) {
				return entry.kind.emoji != filter->emoji
					|| entry.kind.customEmoji != filter->customEmoji;
			});
		}
		if (result.size() > limit) {
			result.resize(limit);
		}
		return result;
	}
	SendResult sendSticker(int64_t chat, int64_t id, int64_t, int64_t, bool) override {
		if (id != 321) {
			return { .error = api::error::kNotFound };
		}
		actions.push_back("sticker " + std::to_string(chat) + " " + std::to_string(id));
		return { .localId = -1 };
	}
	std::vector<StickerInfo> stickers(const std::string &emoji, size_t limit) override {
		auto result = std::vector<StickerInfo>{ { 321, emoji.empty() ? "r" : emoji, "cats", false, true } };
		if (result.size() > limit) {
			result.resize(limit);
		}
		return result;
	}
	std::string forward(int64_t from, const std::vector<int64_t> &ids, int64_t to, bool) override {
		actions.push_back("forward " + std::to_string(from) + " " + std::to_string(ids.size())
			+ " " + std::to_string(to));
		return {};
	}
	std::string markRead(int64_t chat) override {
		actions.push_back("read " + std::to_string(chat));
		return {};
	}
	std::string chatAction(int64_t chat, int64_t, const std::string &kind) override {
		actions.push_back("action " + std::to_string(chat) + " " + kind);
		return {};
	}
	std::vector<Message> search(int64_t chat, const std::string &query, size_t limit) override {
		auto result = history(chat, limit, 0, 0);
		std::erase_if(result, [&](const Message &message) {
			return message.text.find(query) == std::string::npos;
		});
		return result;
	}
	std::optional<UserInfo> user(int64_t id) override {
		if (id != 7) {
			return std::nullopt;
		}
		return UserInfo{ 7, "Bob", "bob", false, true, true, false, "online", 0 };
	}
	Result<UserInfo, std::string> resolveUser(const std::string &username) override {
		if (username != "bobby") {
			return std::string(api::error::kNotFound);
		}
		return UserInfo{ 7, "Bob", "bob", false, true, true, false, "online", 0, true };
	}
	Result<ChannelInfo, std::string> channel(int64_t id) override {
		if (id != 2) {
			return std::string(api::error::kNotFound);
		}
		return ChannelInfo{ 2, "News", "newsy", "channel", "about", 1200, true, false, false, true };
	}
	Result<ChannelInfo, std::string> resolveChannel(const std::string &username) override {
		return (username == "newsy") ? channel(2) : Result<ChannelInfo, std::string>(std::string(api::error::kNotFound));
	}
	Result<Media, std::string> peerPhoto(int64_t id, bool channel, size_t) override {
		if (id != (channel ? 2 : 7)) {
			return std::string(api::error::kNotFound);
		}
		return Media{ "photo", "image/jpeg", "", cbor::Bytes{ 1, 2, 3 }, false };
	}
	std::string notify(const std::string &title, const std::string &text) override {
		actions.push_back("notify " + title + " " + text);
		return {};
	}
	std::string clipboardWrite(const std::string &text) override {
		clipboard = text;
		return {};
	}
	std::optional<std::string> clipboardRead() override {
		return clipboard;
	}
	Result<PickedFile, std::string> pickFile(const std::string &, const std::string &, size_t) override {
		if (cancelPick) {
			return std::string(api::error::kCancelled);
		}
		return PickedFile{ "a.txt", cbor::Bytes{ 'h', 'i' } };
	}
	std::string saveFile(const std::string &name, const cbor::Bytes &bytes) override {
		actions.push_back("save " + name + " " + std::to_string(bytes.size()));
		return {};
	}

	std::map<int64_t, std::string> drafts;
	std::vector<std::pair<int64_t, std::string>> sent;
	int64_t nextLocalId = -1;
	std::vector<OutgoingMessage> outgoing;
	std::vector<OutgoingMedia> sentMedia;
	std::vector<OutgoingMediaEdit> editedMedia;
	std::vector<std::string> toasts;
	std::vector<ToastAction> toastActions;
	std::vector<std::string> actions;
	std::string clipboard;
	bool cancelPick = false;

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
