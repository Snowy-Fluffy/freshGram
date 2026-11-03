#include "fgpl/dev_data.h"

#include "fgpl/json.h"

#include <algorithm>
#include <chrono>

namespace fgpl::dev {
namespace {

[[nodiscard]] std::string Quoted(const std::string &text) {
	return json::Canonical(cbor::Value::FromText(text));
}

[[nodiscard]] std::string Preview(const cbor::Bytes &bytes) {
	return std::to_string(bytes.size()) + " bytes";
}

} // namespace

DevData::DevData(Say say)
: _say(std::move(say)) {
}

void DevData::addChat(Chat chat) {
	const auto lock = std::scoped_lock(_mutex);
	_chats.push_back(std::move(chat));
}

int64_t DevData::addMessage(int64_t chat, std::string text, int64_t from, bool out) {
	const auto lock = std::scoped_lock(_mutex);
	auto message = Message();
	message.id = _nextId++;
	message.chat = chat;
	message.from = from;
	message.out = out;
	message.date = int64_t(std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count());
	message.text = std::move(text);
	_messages[chat].push_back(message);
	return message.id;
}

std::vector<Chat> DevData::allChats() {
	const auto lock = std::scoped_lock(_mutex);
	return _chats;
}

std::vector<Chat> DevData::chats(size_t limit) {
	const auto lock = std::scoped_lock(_mutex);
	auto result = _chats;
	if (result.size() > limit) {
		result.resize(limit);
	}
	return result;
}

std::optional<Chat> DevData::chat(int64_t id) {
	const auto lock = std::scoped_lock(_mutex);
	for (const auto &chat : _chats) {
		if (chat.id == id) {
			return chat;
		}
	}
	return std::nullopt;
}

std::vector<Message> DevData::history(
		int64_t chat,
		size_t limit,
		int64_t before,
		int64_t topic,
		int64_t fromUser) {
	const auto lock = std::scoped_lock(_mutex);
	auto result = std::vector<Message>();
	const auto found = _messages.find(chat);
	if (found == _messages.end()) {
		return result;
	}
	for (auto i = found->second.rbegin(); i != found->second.rend(); ++i) {
		if ((before > 0 && i->id >= before)
			|| (topic && i->topic != topic)
			|| (fromUser && i->from != fromUser)) {
			continue;
		}
		result.push_back(*i);
		if (result.size() >= limit) {
			break;
		}
	}
	return result;
}

Account DevData::account() {
	auto result = Account();
	result.id = 1;
	result.name = "Dev";
	result.username = "dev";
	return result;
}

int64_t DevData::stars() {
	return 0;
}

std::string DevData::draft(int64_t chat) {
	const auto lock = std::scoped_lock(_mutex);
	const auto found = _drafts.find(chat);
	return (found != _drafts.end()) ? found->second : std::string();
}

bool DevData::setDraft(int64_t chat, const std::string &text) {
	{
		const auto lock = std::scoped_lock(_mutex);
		_drafts[chat] = text;
	}
	_say("[app] draft chat=" + std::to_string(chat) + " " + Quoted(text));
	return true;
}

bool DevData::send(int64_t chat, const std::string &text) {
	auto message = OutgoingMessage();
	message.chat = chat;
	message.text = text;
	return sendMessage(message).error.empty();
}

void DevData::toast(const std::string &text) {
	_say("[app] toast " + Quoted(text));
}

void DevData::toastAction(const ToastAction &toast) {
	_say("[app] toast " + (toast.title.empty() ? std::string() : Quoted(toast.title) + " ")
		+ Quoted(toast.text)
		+ (toast.id.empty() ? std::string() : " button=" + Quoted(toast.label) + " id=" + toast.id)
		+ (toast.undo ? " undo" : ""));
}

SendResult DevData::sendMessage(const OutgoingMessage &message) {
	if (!chat(message.chat)) {
		return { .error = api::error::kNotFound };
	}
	auto line = "[app] send chat=" + std::to_string(message.chat);
	if (message.replyTo) {
		line += " reply_to=" + std::to_string(message.replyTo);
	}
	if (message.topic) {
		line += " topic=" + std::to_string(message.topic);
	}
	if (message.silent) {
		line += " silent";
	}
	if (message.schedule) {
		line += " schedule=" + std::to_string(message.schedule);
	}
	if (!message.entities.empty()) {
		line += " entities=" + std::to_string(message.entities.size());
	}
	line += " " + Quoted(message.text);
	_say(line);
	if (message.schedule) {
		return {};
	}
	auto result = SendResult();
	{
		const auto lock = std::scoped_lock(_mutex);
		result.localId = _nextLocal--;
	}
	const auto id = addMessage(message.chat, message.text, 1, true);
	if (message.waitForServerId) {
		result.serverId = id;
	}
	return result;
}

Message *DevData::find(int64_t chat, int64_t id) {
	const auto messages = _messages.find(chat);
	if (messages == _messages.end()) {
		return nullptr;
	}
	for (auto &message : messages->second) {
		if (message.id == id) {
			return &message;
		}
	}
	return nullptr;
}

std::string DevData::edit(
		int64_t chat,
		int64_t id,
		const std::string &text,
		const std::vector<TextEntity> &entities) {
	{
		const auto lock = std::scoped_lock(_mutex);
		const auto message = find(chat, id);
		if (!message) {
			return api::error::kNotFound;
		} else if (!message->out) {
			return api::error::kDenied;
		}
		message->text = text;
	}
	_say("[app] edit chat=" + std::to_string(chat) + " id=" + std::to_string(id)
		+ (entities.empty() ? "" : " entities=" + std::to_string(entities.size()))
		+ " " + Quoted(text));
	return {};
}

std::string DevData::setDisplay(
		const std::string &,
		int64_t chat,
		int64_t id,
		const std::optional<std::string> &text,
		const std::vector<TextEntity> &entities) {
	{
		const auto lock = std::scoped_lock(_mutex);
		const auto message = find(chat, id);
		if (!message) {
			return api::error::kNotFound;
		} else if (message->out) {
			return api::error::kDenied;
		}
	}
	_say("[app] set_display chat=" + std::to_string(chat) + " id=" + std::to_string(id)
		+ (entities.empty() ? "" : " entities=" + std::to_string(entities.size()))
		+ (text ? " " + Quoted(*text) : std::string(" reset")));
	return {};
}

std::string DevData::remove(
		int64_t chat,
		const std::vector<int64_t> &ids,
		bool everyone) {
	{
		const auto lock = std::scoped_lock(_mutex);
		for (const auto id : ids) {
			const auto message = find(chat, id);
			if (!message || !message->out) {
				return message ? api::error::kDenied : api::error::kNotFound;
			}
		}
		auto &messages = _messages[chat];
		messages.erase(std::remove_if(messages.begin(), messages.end(), [&](const Message &message) {
			return std::find(ids.begin(), ids.end(), message.id) != ids.end();
		}), messages.end());
	}
	_say("[app] delete chat=" + std::to_string(chat) + " ids=" + std::to_string(ids.size())
		+ (everyone ? " everyone" : ""));
	return {};
}

std::string DevData::react(int64_t chat, int64_t id, const ReactionKind &reaction) {
	_say("[app] react chat=" + std::to_string(chat) + " id=" + std::to_string(id) + " "
		+ (reaction.customEmoji
			? "custom_emoji=" + std::to_string(reaction.customEmoji)
			: reaction.emoji.empty() ? std::string("none") : Quoted(reaction.emoji)));
	return {};
}

std::string DevData::forward(
		int64_t from,
		const std::vector<int64_t> &ids,
		int64_t to,
		bool silent) {
	_say("[app] forward " + std::to_string(ids.size()) + " from=" + std::to_string(from)
		+ " to=" + std::to_string(to) + (silent ? " silent" : ""));
	return {};
}

std::string DevData::markRead(int64_t chat) {
	_say("[app] mark_read chat=" + std::to_string(chat));
	return {};
}

std::string DevData::chatAction(int64_t chat, int64_t topic, const std::string &kind) {
	_say("[app] action chat=" + std::to_string(chat) + (topic ? " topic=" + std::to_string(topic) : "")
		+ " " + kind);
	return {};
}

std::vector<Message> DevData::search(
		int64_t chat,
		const std::string &query,
		size_t limit) {
	const auto lock = std::scoped_lock(_mutex);
	auto result = std::vector<Message>();
	const auto found = _messages.find(chat);
	if (found == _messages.end()) {
		return result;
	}
	for (auto i = found->second.rbegin(); i != found->second.rend() && result.size() < limit; ++i) {
		if (i->text.find(query) != std::string::npos) {
			result.push_back(*i);
		}
	}
	return result;
}

std::string DevData::pinMessage(int64_t chat, int64_t id, bool pinned, bool silent) {
	_say(std::string("[app] ") + (pinned ? "pin" : "unpin") + " chat=" + std::to_string(chat)
		+ " id=" + std::to_string(id) + (silent ? " silent" : ""));
	return {};
}

std::string DevData::muteChat(int64_t chat, int64_t seconds) {
	_say("[app] mute chat=" + std::to_string(chat) + " seconds=" + std::to_string(seconds));
	return {};
}

std::string DevData::changeFolder(int64_t folder, int64_t chat, bool add) {
	_say(std::string("[app] folder ") + (add ? "add" : "remove") + " folder=" + std::to_string(folder) + " chat=" + std::to_string(chat));
	return {};
}

std::string DevData::changeSettings(const SettingsChange &change) {
	_say("[app] settings" + (change.theme ? " theme=" + *change.theme : std::string())
		+ (change.streamerMode ? std::string(" streamer=") + (*change.streamerMode ? "on" : "off") : std::string()));
	return {};
}

std::string DevData::archiveChat(int64_t chat, bool archived) {
	_say(std::string("[app] ") + (archived ? "archive" : "unarchive") + " chat=" + std::to_string(chat));
	return {};
}

std::string DevData::pinChat(int64_t chat, bool pinned) {
	_say(std::string("[app] ") + (pinned ? "pin_chat" : "unpin_chat") + " chat=" + std::to_string(chat));
	return {};
}

std::string DevData::composerInsert(int64_t chat, const std::string &text) {
	_say("[app] composer chat=" + std::to_string(chat) + " " + Quoted(text));
	return {};
}

std::string DevData::setNotifyRules(const std::string &, std::vector<NotifyRule> rules) {
	_say("[app] notify rules " + std::to_string(rules.size()));
	return {};
}

std::string DevData::notify(const std::string &title, const std::string &text) {
	_say("[app] notify " + Quoted(title) + " " + Quoted(text));
	return {};
}

std::string DevData::clipboardWrite(const std::string &text) {
	{
		const auto lock = std::scoped_lock(_mutex);
		_clipboard = text;
	}
	_say("[app] clipboard " + Quoted(text));
	return {};
}

std::optional<std::string> DevData::clipboardRead() {
	const auto lock = std::scoped_lock(_mutex);
	return _clipboard;
}

std::string DevData::saveFile(const std::string &name, const cbor::Bytes &bytes) {
	_say("[app] save_file " + Quoted(name) + " " + Preview(bytes));
	return {};
}

DevTransport::DevTransport(Say say)
: _say(std::move(say)) {
}

void DevTransport::mock(std::string prefix, int status, cbor::Bytes body) {
	const auto lock = std::scoped_lock(_mutex);
	_mocks[std::move(prefix)] = { status, std::move(body) };
}

Result<FetchResponse, FetchError> DevTransport::fetch(const FetchRequest &request) {
	auto answer = Mock{ 404, {} };
	auto matched = size_t(0);
	{
		const auto lock = std::scoped_lock(_mutex);
		for (const auto &[prefix, mock] : _mocks) {
			if (prefix.size() >= matched && request.url.compare(0, prefix.size(), prefix) == 0) {
				matched = prefix.size();
				answer = mock;
			}
		}
	}
	_say("[net] " + request.method + " " + request.url + " body=" + Preview(request.body)
		+ " -> " + std::to_string(answer.status) + (matched ? "" : " (no mock)"));
	return FetchResponse{ answer.status, {}, std::move(answer.body) };
}

} // namespace fgpl::dev
