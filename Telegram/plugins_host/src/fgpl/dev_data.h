#pragma once

#include "fgpl/broker.h"

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace fgpl::dev {

// Prints one line; may be called from any thread.
using Say = std::function<void(const std::string &line)>;

// The app side of `fgpl_tool run`: a small world in memory, every effect is printed.
class DevData final : public DataProvider {
public:
	explicit DevData(Say say);

	void addChat(Chat chat);
	// Adds a message the plugin can read; returns its id.
	int64_t addMessage(int64_t chat, std::string text, int64_t from, bool out);
	[[nodiscard]] std::vector<Chat> allChats();

	std::vector<Chat> chats(size_t limit) override;
	std::optional<Chat> chat(int64_t id) override;
	std::vector<Message> history(
		int64_t chat,
		size_t limit,
		int64_t before,
		int64_t topic,
		int64_t fromUser) override;
	Account account() override;
	int64_t stars() override;
	std::string draft(int64_t chat) override;
	bool setDraft(int64_t chat, const std::string &text) override;
	bool send(int64_t chat, const std::string &text) override;
	void toast(const std::string &text) override;
	void toastAction(const ToastAction &toast) override;
	SendResult sendMessage(const OutgoingMessage &message) override;
	std::string edit(
		int64_t chat,
		int64_t id,
		const std::string &text,
		const std::vector<TextEntity> &entities) override;
	std::string setDisplay(
		const std::string &plugin,
		int64_t chat,
		int64_t id,
		const std::optional<std::string> &text,
		const std::vector<TextEntity> &entities) override;
	std::string remove(
		int64_t chat,
		const std::vector<int64_t> &ids,
		bool everyone) override;
	std::string react(
		int64_t chat,
		int64_t id,
		const ReactionKind &reaction) override;
	std::string forward(
		int64_t from,
		const std::vector<int64_t> &ids,
		int64_t to,
		bool silent) override;
	std::string markRead(int64_t chat) override;
	std::string chatAction(
		int64_t chat,
		int64_t topic,
		const std::string &kind) override;
	std::vector<Message> search(
		int64_t chat,
		const std::string &query,
		size_t limit) override;
	std::string pinMessage(
		int64_t chat,
		int64_t id,
		bool pinned,
		bool silent) override;
	std::string muteChat(int64_t chat, int64_t seconds) override;
	std::string changeFolder(int64_t folder, int64_t chat, bool add) override;
	std::string changeSettings(const SettingsChange &change) override;
	std::string archiveChat(int64_t chat, bool archived) override;
	std::string pinChat(int64_t chat, bool pinned) override;
	std::string composerInsert(int64_t chat, const std::string &text) override;
	std::string notify(const std::string &title, const std::string &text) override;
	std::string setNotifyRules(const std::string &plugin, std::vector<NotifyRule> rules) override;
	std::string clipboardWrite(const std::string &text) override;
	std::optional<std::string> clipboardRead() override;
	std::string saveFile(const std::string &name, const cbor::Bytes &bytes) override;

private:
	[[nodiscard]] Message *find(int64_t chat, int64_t id);

	Say _say;
	std::mutex _mutex;
	std::vector<Chat> _chats;
	std::map<int64_t, std::vector<Message>> _messages;
	std::map<int64_t, std::string> _drafts;
	std::string _clipboard;
	int64_t _nextId = 1000;
	int64_t _nextLocal = -1;

};

class DevTransport final : public Transport {
public:
	explicit DevTransport(Say say);

	// Answers requests whose URL starts with `prefix`; the longest prefix wins.
	void mock(std::string prefix, int status, cbor::Bytes body);

	Result<FetchResponse, FetchError> fetch(const FetchRequest &request) override;

private:
	struct Mock {
		int status = 0;
		cbor::Bytes body;
	};

	Say _say;
	std::mutex _mutex;
	std::map<std::string, Mock> _mocks;

};

} // namespace fgpl::dev
