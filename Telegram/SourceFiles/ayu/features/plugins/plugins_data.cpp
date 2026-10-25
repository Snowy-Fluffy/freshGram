// This is the source code of freshGram.
#include "ayu/features/plugins/plugins_data.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/plugins/plugins_service.h"
#include "ayu/secret/secret_peer.h"
#include "api/api_common.h"
#include "apiwrap.h"
#include "core/application.h"
#include "crl/crl_on_main.h"
#include "data/data_changes.h"
#include "data/data_chat_filters.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_drafts.h"
#include "data/data_media_types.h"
#include "data/data_chat_participant_status.h"
#include "history/history_item_components.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "lang/lang_instance.h"
#include "ui/image/image.h"
#include "window/themes/window_theme.h"
#include "dialogs/dialogs_entry.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "data/components/credits.h"
#include "ui/toast/toast.h"

#ifdef FRESHGRAM_PLUGINS
#include "fgpl/manager.h"
#endif

#include <QtCore/QBuffer>
#include <QtCore/QFile>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>

namespace FreshPlugins {

#ifdef FRESHGRAM_PLUGINS

namespace {

constexpr auto kWaitSlice = std::chrono::milliseconds(20);
constexpr auto kWaitTotal = std::chrono::milliseconds(1500);
constexpr auto kMaxHistory = size_t(200);

struct Gate {
	std::atomic<bool> closed = false;
};

[[nodiscard]] Gate &GetGate() {
	static auto result = Gate();
	return result;
}

enum class Stage {
	Pending,
	Running,
	Finished,
	Abandoned,
};

template <typename T>
struct Call {
	std::mutex mutex;
	std::condition_variable changed;
	Stage stage = Stage::Pending;
	T result;
};

template <typename T>
[[nodiscard]] T OnMain(Fn<T()> work, T fallback) {
	auto &gate = GetGate();
	if (gate.closed.load()) {
		return fallback;
	}
	const auto call = std::make_shared<Call<T>>();
	call->result = fallback;
	crl::on_main([=] {
		{
			auto lock = std::unique_lock(call->mutex);
			if (call->stage != Stage::Pending || GetGate().closed.load()) {
				call->stage = (call->stage == Stage::Pending)
					? Stage::Finished
					: call->stage;
				call->changed.notify_all();
				return;
			}
			call->stage = Stage::Running;
		}
		auto value = work();
		{
			auto lock = std::unique_lock(call->mutex);
			call->result = std::move(value);
			call->stage = Stage::Finished;
		}
		call->changed.notify_all();
	});

	auto lock = std::unique_lock(call->mutex);
	const auto until = std::chrono::steady_clock::now() + kWaitTotal;
	while (call->stage != Stage::Finished) {
		if (call->stage == Stage::Pending
			&& (gate.closed.load() || std::chrono::steady_clock::now() >= until)) {
			call->stage = Stage::Abandoned;
			return fallback;
		}
		call->changed.wait_for(lock, kWaitSlice);
	}
	return call->result;
}

[[nodiscard]] Main::Session *FindSession(const std::string &account) {
	for (const auto &entry : Core::App().domain().accounts()) {
		if (const auto session = entry.account->maybeSession()) {
			if (AccountId(session).toStdString() == account) {
				return session;
			}
		}
	}
	return nullptr;
}

[[nodiscard]] std::string Utf8(const QString &text) {
	return text.toStdString();
}

[[nodiscard]] int64_t ChatId(not_null<const PeerData*> peer) {
	return int64_t(peer->id.value);
}

[[nodiscard]] const char *KindOf(not_null<PeerData*> peer) {
	if (peer->isUser()) {
		return "user";
	} else if (peer->isChat()) {
		return "group";
	} else if (peer->isMegagroup()) {
		return "supergroup";
	}
	return "channel";
}

[[nodiscard]] fgpl::Chat ChatOf(not_null<History*> history) {
	const auto peer = history->peer;
	auto result = fgpl::Chat();
	result.id = ChatId(peer);
	result.title = Utf8(peer->name());
	result.type = KindOf(peer);
	result.unread = history->unreadCount();
	result.pinned = history->isPinnedDialog(FilterId());
	result.secret = FreshSecret::IsSecretPeerId(peer->id);
	return result;
}

[[nodiscard]] QByteArray Jpeg(const QImage &image) {
	auto result = QByteArray();
	auto buffer = QBuffer(&result);
	buffer.open(QIODevice::WriteOnly);
	image.save(&buffer, "JPG", 87);
	return result;
}

[[nodiscard]] fgpl::Message MessageOf(not_null<const HistoryItem*> item) {
	auto result = fgpl::Message();
	result.id = item->id.bare;
	result.chat = ChatId(item->history()->peer);
	result.from = ChatId(item->from());
	result.date = item->date();
	result.text = Utf8(item->originalText().text);
	return result;
}

int PluginSendDepth = 0;

[[nodiscard]] std::string MediaKindOf(not_null<const HistoryItem*> item) {
	const auto media = item->media();
	if (!media) {
		return std::string();
	} else if (media->photo()) {
		return "photo";
	} else if (const auto document = media->document()) {
		return document->sticker()
			? "sticker"
			: document->isVideoMessage()
			? "round"
			: document->isAnimation()
			? "gif"
			: document->isVideoFile()
			? "video"
			: document->isVoiceMessage()
			? "voice"
			: document->isAudioFile()
			? "audio"
			: "file";
	} else if (media->poll()) {
		return "poll";
	} else if (media->sharedContact()) {
		return "contact";
	} else if (media->location()) {
		return "location";
	} else if (media->webpage()) {
		return "webpage";
	}
	return "other";
}

class AppData final : public fgpl::DataProvider {
public:
	explicit AppData(std::string account) : _account(std::move(account)) {
	}

	std::vector<fgpl::Chat> chats(size_t limit) override {
		return OnMain<std::vector<fgpl::Chat>>([=] {
			auto result = std::vector<fgpl::Chat>();
			const auto session = FindSession(_account);
			if (!session) {
				return result;
			}
			for (const auto &row : *session->data().chatsList()->indexed()) {
				if (result.size() >= limit) {
					break;
				}
				if (const auto history = row->history()) {
					result.push_back(ChatOf(history));
				}
			}
			return result;
		}, {});
	}

	std::optional<fgpl::Chat> chat(int64_t id) override {
		return OnMain<std::optional<fgpl::Chat>>([=]() -> std::optional<fgpl::Chat> {
			const auto history = historyOf(id);
			return history
				? std::optional<fgpl::Chat>(ChatOf(history))
				: std::nullopt;
		}, std::nullopt);
	}

	std::vector<fgpl::Message> history(
			int64_t chat,
			size_t limit,
			int64_t before) override {
		return OnMain<std::vector<fgpl::Message>>([=] {
			auto result = std::vector<fgpl::Message>();
			const auto history = historyOf(chat);
			if (!history || FreshSecret::IsSecretPeerId(history->peer->id)) {
				return result;
			}
			const auto cap = std::min(limit, kMaxHistory);
			for (auto b = history->blocks.rbegin(); b != history->blocks.rend(); ++b) {
				for (auto m = (*b)->messages.rbegin(); m != (*b)->messages.rend(); ++m) {
					const auto item = (*m)->data();
					if (item->isService() || item->isEmpty()) {
						continue;
					} else if (before > 0 && item->id.bare >= before) {
						continue;
					}
					result.push_back(MessageOf(item));
					if (result.size() >= cap) {
						return result;
					}
				}
			}
			return result;
		}, {});
	}

	fgpl::Account account() override {
		return OnMain<fgpl::Account>([=] {
			auto result = fgpl::Account();
			if (const auto session = FindSession(_account)) {
				const auto user = session->user();
				result.id = ChatId(user);
				result.name = Utf8(user->name());
				result.username = Utf8(user->username());
				result.premium = user->isPremium();
				result.phone = Utf8(user->phone());
			}
			return result;
		}, {});
	}

	int64_t stars() override {
		return OnMain<int64_t>([=] {
			const auto session = FindSession(_account);
			return session ? session->credits().balance().whole() : 0;
		}, 0);
	}

	std::string draft(int64_t chat) override {
		return OnMain<std::string>([=] {
			const auto history = historyOf(chat);
			if (!history || FreshSecret::IsSecretPeerId(history->peer->id)) {
				return std::string();
			}
			const auto draft = history->localDraft({}, {});
			return draft ? Utf8(draft->textWithTags.text) : std::string();
		}, std::string());
	}

	bool setDraft(int64_t chat, const std::string &text) override {
		return OnMain<bool>([=] {
			const auto history = historyOf(chat);
			if (!history || FreshSecret::IsSecretPeerId(history->peer->id)) {
				return false;
			}
			const auto textWithTags = TextWithTags{ QString::fromStdString(text) };
			const auto cursor = MessageCursor{
				int(textWithTags.text.size()),
				int(textWithTags.text.size()),
				Ui::kQFixedMax
			};
			const auto previous = history->localDraft({}, {});
			history->setLocalDraft(std::make_unique<Data::Draft>(
				textWithTags,
				previous ? previous->reply : FullReplyTo(),
				previous ? previous->suggest : SuggestOptions(),
				cursor,
				Data::WebPageDraft()));
			history->updateChatListSortPosition();
			history->session().changes().historyUpdated(
				history,
				Data::HistoryUpdate::Flag::CloudDraft);
			return true;
		}, false);
	}

	bool send(int64_t chat, const std::string &text) override {
		return OnMain<bool>([=] {
			const auto session = FindSession(_account);
			const auto history = historyOf(chat);
			if (!session || !history
				|| FreshSecret::IsSecretPeerId(history->peer->id)) {
				return false;
			}
			if (!Data::CanSendTexts(history->peer)
				|| history->peer->slowmodeSecondsLeft() > 0) {
				return false;
			}
			auto action = Api::SendAction(history);
			action.clearDraft = false;
			auto message = Api::MessageToSend(action);
			message.textWithTags = TextWithTags{ QString::fromStdString(text) };
			++PluginSendDepth;
			session->api().sendMessage(std::move(message));
			--PluginSendDepth;
			return true;
		}, false);
	}

	void toast(const std::string &text) override {
		OnMain<bool>([=] {
			Ui::Toast::Show(QString::fromStdString(text));
			return true;
		}, false);
	}

	std::vector<fgpl::Contact> contacts(size_t limit) override {
		return OnMain<std::vector<fgpl::Contact>>([=] {
			auto result = std::vector<fgpl::Contact>();
			const auto session = FindSession(_account);
			if (!session) {
				return result;
			}
			for (const auto &row : *session->data().contactsList()) {
				if (result.size() >= limit) {
					break;
				}
				const auto history = row->history();
				const auto user = history ? history->peer->asUser() : nullptr;
				if (user && !user->isSelf()) {
					result.push_back({
						ChatId(user),
						Utf8(user->name()),
						Utf8(user->username()),
						std::string(),
					});
				}
			}
			return result;
		}, {});
	}

	std::vector<fgpl::Folder> folders() override {
		return OnMain<std::vector<fgpl::Folder>>([=] {
			auto result = std::vector<fgpl::Folder>();
			const auto session = FindSession(_account);
			if (!session) {
				return result;
			}
			auto &filters = session->data().chatsFilters();
			for (const auto &filter : filters.list()) {
				if (!filter.id()) {
					continue;
				}
				auto folder = fgpl::Folder();
				folder.id = filter.id();
				folder.title = Utf8(filter.title().text.text);
				for (const auto &row : *filters.chatsList(filter.id())->indexed()) {
					const auto history = row->history();
					if (history && !FreshSecret::IsSecretPeerId(history->peer->id)) {
						folder.chats.push_back(ChatId(history->peer));
					}
				}
				result.push_back(std::move(folder));
			}
			return result;
		}, {});
	}

	fgpl::AppSettings settings() override {
		return OnMain<fgpl::AppSettings>([=] {
			auto result = fgpl::AppSettings();
			result.language = Utf8(Lang::GetInstance().id());
			result.theme = Window::Theme::IsNightMode() ? "dark" : "light";
			result.streamerMode = AyuSettings::getInstance().streamerMode();
			return result;
		}, {});
	}

	std::optional<fgpl::Media> media(
			int64_t chat,
			int64_t message,
			bool thumbnail,
			size_t maxBytes) override {
		return OnMain<std::optional<fgpl::Media>>([=]() -> std::optional<fgpl::Media> {
			const auto session = FindSession(_account);
			const auto peerId = PeerId(uint64(chat));
			if (!session || FreshSecret::IsSecretPeerId(peerId)) {
				return std::nullopt;
			}
			const auto item = session->data().message(peerId, MsgId(message));
			const auto media = item ? item->media() : nullptr;
			if (!media) {
				return std::nullopt;
			}
			auto result = fgpl::Media();
			auto bytes = QByteArray();
			if (const auto photo = media->photo()) {
				const auto view = photo->activeMediaView();
				if (!view) {
					return std::nullopt;
				}
				const auto size = thumbnail
					? Data::PhotoSize::Thumbnail
					: Data::PhotoSize::Large;
				bytes = view->imageBytes(size);
				if (bytes.isEmpty()) {
					if (const auto image = view->image(size)) {
						bytes = Jpeg(image->original());
					}
				}
				result.kind = thumbnail ? "thumbnail" : "photo";
				result.mime = "image/jpeg";
			} else if (const auto document = media->document()) {
				const auto view = document->activeMediaView();
				if (thumbnail) {
					const auto image = view ? view->thumbnail() : nullptr;
					if (image) {
						bytes = Jpeg(image->original());
					}
					result.kind = "thumbnail";
					result.mime = "image/jpeg";
				} else {
					bytes = view ? view->bytes() : QByteArray();
					const auto path = bytes.isEmpty() ? document->filepath(true) : QString();
					auto file = QFile(path);
					if (!path.isEmpty() && file.open(QIODevice::ReadOnly)) {
						bytes = file.read(qint64(maxBytes) + 1);
					}
					result.kind = "document";
					result.mime = Utf8(document->mimeString());
					result.name = Utf8(document->filename());
				}
			}
			if (bytes.isEmpty()) {
				return std::nullopt;
			}
			bytes.truncate(qsizetype(std::min<size_t>(size_t(bytes.size()), maxBytes + 1)));
			result.bytes.assign(bytes.constData(), bytes.constData() + bytes.size());
			return result;
		}, std::nullopt);
	}

private:
	[[nodiscard]] History *historyOf(int64_t id) const {
		const auto session = FindSession(_account);
		return session
			? session->data().historyLoaded(PeerId(uint64(id)))
			: nullptr;
	}

	std::string _account;

};

struct DataState {
	std::map<std::string, std::unique_ptr<AppData>> providers;
};

[[nodiscard]] DataState &GetData() {
	static auto result = DataState();
	return result;
}

} // namespace

fgpl::DataProvider *DataFor(const std::string &account) {
	auto &state = GetData();
	auto &entry = state.providers[account];
	if (!entry) {
		entry = std::make_unique<AppData>(account);
	}
	return entry.get();
}

void OpenData() {
	GetGate().closed = false;
}

void CloseData() {
	GetGate().closed = true;
}

void ReleaseData() {
	GetData().providers.clear();
}

bool WantsEvents() {
	const auto snapshot = Snapshot();
	return snapshot && snapshot->wantsEvents;
}

bool PluginSending() {
	return PluginSendDepth > 0;
}

void SendMessageEvent(
		const char *type,
		not_null<const HistoryItem*> item,
		const std::string &account,
		MsgId localId,
		bool byPlugin) {
	if (!Running() || item->isService()) {
		return;
	}
	using Value = fgpl::cbor::Value;
	const auto peer = item->history()->peer;
	const auto message = MessageOf(item);
	auto map = fgpl::cbor::Map{
		{ "type", Value::FromText(type) },
		{ "id", Value::FromInt(message.id) },
		{ "chat", Value::FromInt(message.chat) },
		{ "chat_title", Value::FromText(Utf8(peer->name())) },
		{ "chat_type", Value::FromText(KindOf(peer)) },
		{ "from", Value::FromInt(message.from) },
		{ "sender_name", Value::FromText(Utf8(item->from()->name())) },
		{ "date", Value::FromInt(message.date) },
		{ "text", Value::FromText(message.text) },
		{ "out", Value::FromBool(item->out()) },
		{ "forwarded", Value::FromBool(item->Has<HistoryMessageForwarded>()) },
		{ "secret", Value::FromBool(FreshSecret::IsSecretPeerId(peer->id)) },
	};
	const auto optionalInt = [&](const char *key, int64_t value) {
		if (value) {
			map.push_back({ key, Value::FromInt(value) });
		}
	};
	optionalInt("reply_to", item->replyToId().bare);
	optionalInt("topic", item->topicRootId().bare);
	optionalInt("grouped", int64_t(item->groupId().value));
	optionalInt("local_id", localId.bare);
	if (const auto edited = item->Get<HistoryMessageEdited>()) {
		optionalInt("edit_date", edited->date);
	}
	if (const auto media = MediaKindOf(item); !media.empty()) {
		map.push_back({ "media", Value::FromText(media) });
	}
	if (const auto bot = item->viaBot()) {
		map.push_back({ "via_bot", Value::FromText(Utf8(bot->username())) });
	}
	if (byPlugin) {
		map.push_back({ "by_plugin", Value::FromBool(true) });
	}
	auto event = Value::FromMap(std::move(map));
	Post([=, event = std::move(event)](not_null<fgpl::PluginManager*> manager) {
		manager->dispatch(account, event);
	}, false);
}

#else

fgpl::DataProvider *DataFor(const std::string &) {
	return nullptr;
}

void OpenData() {
}

void CloseData() {
}

void ReleaseData() {
}

bool WantsEvents() {
	return false;
}

bool PluginSending() {
	return false;
}

void SendMessageEvent(
	const char *,
	not_null<const HistoryItem*>,
	const std::string &,
	MsgId,
	bool) {
}

#endif

} // namespace FreshPlugins
