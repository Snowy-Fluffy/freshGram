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
#include "data/data_folder.h"
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
#include "storage/cache/storage_cache_database.h"
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
constexpr auto kWaitServer = std::chrono::milliseconds(10000);
constexpr auto kMaxHistory = size_t(200);

thread_local bool TimedOut = false;

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
		TimedOut = true;
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
			TimedOut = true;
			return fallback;
		}
		call->changed.wait_for(lock, kWaitSlice);
	}
	return call->result;
}

template <typename T>
[[nodiscard]] T OnMainAsync(Fn<void(Fn<void(T)> done)> start, T fallback) {
	auto &gate = GetGate();
	if (gate.closed.load()) {
		TimedOut = true;
		return fallback;
	}
	const auto call = std::make_shared<Call<T>>();
	call->result = fallback;
	crl::on_main([=] {
		{
			auto lock = std::unique_lock(call->mutex);
			if (call->stage != Stage::Pending || GetGate().closed.load()) {
				return;
			}
			call->stage = Stage::Running;
		}
		start([=](T value) {
			{
				auto lock = std::unique_lock(call->mutex);
				if (call->stage != Stage::Running) {
					return;
				}
				call->result = std::move(value);
				call->stage = Stage::Finished;
			}
			call->changed.notify_all();
		});
	});

	auto lock = std::unique_lock(call->mutex);
	const auto until = std::chrono::steady_clock::now() + kWaitServer;
	while (call->stage != Stage::Finished) {
		if (gate.closed.load() || std::chrono::steady_clock::now() >= until) {
			call->stage = Stage::Abandoned;
			TimedOut = true;
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
	if (const auto user = peer->asUser()) {
		return user->isSelf() ? "self" : user->isBot() ? "bot" : "user";
	} else if (peer->isChat()) {
		return "group";
	} else if (peer->isForum()) {
		return "forum";
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
	result.archived = (history->folder() != nullptr);
	return result;
}

[[nodiscard]] QByteArray Jpeg(const QImage &image) {
	auto result = QByteArray();
	auto buffer = QBuffer(&result);
	buffer.open(QIODevice::WriteOnly);
	image.save(&buffer, "JPG", 87);
	return result;
}

[[nodiscard]] std::string MediaKindOf(not_null<const HistoryItem*> item);

[[nodiscard]] fgpl::Message MessageOf(not_null<const HistoryItem*> item) {
	auto result = fgpl::Message();
	result.id = item->id.bare;
	result.chat = ChatId(item->history()->peer);
	result.from = ChatId(item->from());
	result.date = item->date();
	result.text = Utf8(item->originalText().text);
	result.topic = item->topicRootId().bare;
	result.replyTo = item->replyToId().bare;
	result.out = item->out();
	result.deleted = item->isDeleted();
	result.media = MediaKindOf(item);
	return result;
}

[[nodiscard]] std::string MediaKindOf(const MTPMessageMedia &media) {
	return media.match([](const MTPDmessageMediaPhoto &) {
		return std::string("photo");
	}, [](const MTPDmessageMediaDocument &) {
		return std::string("file");
	}, [](const MTPDmessageMediaPoll &) {
		return std::string("poll");
	}, [](const MTPDmessageMediaContact &) {
		return std::string("contact");
	}, [](const MTPDmessageMediaGeo &) {
		return std::string("location");
	}, [](const MTPDmessageMediaWebPage &) {
		return std::string("webpage");
	}, [](const MTPDmessageMediaEmpty &) {
		return std::string();
	}, [](const auto &) {
		return std::string("other");
	});
}

[[nodiscard]] std::vector<fgpl::Message> MessagesOf(
		const MTPmessages_Messages &result,
		int64_t chat) {
	auto list = std::vector<fgpl::Message>();
	const auto parse = [&](const QVector<MTPMessage> &messages) {
		for (const auto &message : messages) {
			message.match([&](const MTPDmessage &data) {
				auto entry = fgpl::Message();
				entry.id = data.vid().v;
				entry.chat = chat;
				entry.from = data.vfrom_id()
					? int64_t(peerFromMTP(*data.vfrom_id()).value)
					: chat;
				entry.date = data.vdate().v;
				entry.text = data.vmessage().v.toStdString();
				entry.out = data.is_out();
				if (const auto reply = data.vreply_to()) {
					reply->match([&](const MTPDmessageReplyHeader &header) {
						entry.replyTo = header.vreply_to_msg_id().value_or_empty();
						entry.topic = header.vreply_to_top_id().value_or_empty();
					}, [](const auto &) {
					});
				}
				if (const auto media = data.vmedia()) {
					entry.media = MediaKindOf(*media);
				}
				list.push_back(std::move(entry));
			}, [](const auto &) {
			});
		}
	};
	result.match([](const MTPDmessages_messagesNotModified &) {
	}, [&](const auto &data) {
		parse(data.vmessages().v);
	});
	return list;
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
			const auto add = [&](not_null<Dialogs::MainList*> list) {
				for (const auto &row : *list->indexed()) {
					if (result.size() >= limit) {
						break;
					}
					if (const auto history = row->history()) {
						result.push_back(ChatOf(history));
					}
				}
			};
			add(session->data().chatsList());
			if (const auto archive = session->data().folderLoaded(Data::Folder::kId)) {
				add(archive->chatsList());
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
			int64_t before,
			int64_t topic) override {
		const auto cap = std::min(limit, kMaxHistory);
		auto result = OnMain<std::vector<fgpl::Message>>([=] {
			auto result = std::vector<fgpl::Message>();
			const auto history = historyOf(chat);
			if (!history || FreshSecret::IsSecretPeerId(history->peer->id)) {
				return result;
			}
			for (auto b = history->blocks.rbegin(); b != history->blocks.rend(); ++b) {
				for (auto m = (*b)->messages.rbegin(); m != (*b)->messages.rend(); ++m) {
					const auto item = (*m)->data();
					if (item->isService()
						|| item->isEmpty()
						|| !IsServerMsgId(item->id)
						|| (topic && item->topicRootId().bare != topic)) {
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
		if (result.size() >= cap || TimedOut) {
			return result;
		}
		const auto offset = result.empty() ? before : result.back().id;
		const auto more = int(cap - result.size());
		auto older = OnMainAsync<std::vector<fgpl::Message>>([=](
				Fn<void(std::vector<fgpl::Message>)> done) {
			const auto session = FindSession(_account);
			const auto history = historyOf(chat);
			if (!session || !history || FreshSecret::IsSecretPeerId(history->peer->id)) {
				done({});
				return;
			}
			const auto finish = [=](const MTPmessages_Messages &answer) {
				done(MessagesOf(answer, chat));
			};
			const auto fail = [=] {
				done({});
			};
			if (topic) {
				session->api().request(MTPmessages_GetReplies(
					history->peer->input(),
					MTP_int(topic),
					MTP_int(offset),
					MTP_int(0),
					MTP_int(0),
					MTP_int(more),
					MTP_int(0),
					MTP_int(0),
					MTP_long(0)
				)).done(finish).fail(fail).send();
			} else {
				session->api().request(MTPmessages_GetHistory(
					history->peer->input(),
					MTP_int(offset),
					MTP_int(0),
					MTP_int(0),
					MTP_int(more),
					MTP_int(0),
					MTP_int(0),
					MTP_long(0)
				)).done(finish).fail(fail).send();
			}
		}, {});
		for (auto &message : older) {
			if (result.size() >= cap) {
				break;
			} else if (offset > 0 && message.id >= offset) {
				continue;
			}
			result.push_back(std::move(message));
		}
		return result;
	}

	bool takeTimedOut() override {
		return std::exchange(TimedOut, false);
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
		return OnMainAsync<int64_t>([=](Fn<void(int64_t)> done) {
			const auto session = FindSession(_account);
			if (!session) {
				done(0);
				return;
			}
			const auto credits = &session->credits();
			if (credits->loaded()) {
				done(credits->balance().whole());
				return;
			}
			const auto lifetime = std::make_shared<rpl::lifetime>();
			credits->loadedValue(
			) | rpl::filter(rpl::mappers::_1) | rpl::take(1) | rpl::on_next([=] {
				done(credits->balance().whole());
				lifetime->destroy();
			}, *lifetime);
			credits->load();
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
		struct Source {
			fgpl::Media media;
			QByteArray bytes;
			QString path;
			std::optional<Storage::Cache::Key> cache;
		};
		const auto source = OnMain<std::optional<Source>>([=]() -> std::optional<Source> {
			const auto session = FindSession(_account);
			const auto peerId = PeerId(uint64(chat));
			if (!session || FreshSecret::IsSecretPeerId(peerId)) {
				return std::nullopt;
			}
			const auto item = session->data().message(peerId, MsgId(message));
			const auto media = item ? item->media() : nullptr;
			if (!media || media->ttlSeconds()) {
				return std::nullopt;
			}
			auto result = Source();
			result.media.deleted = item->isDeleted();
			result.media.kind = thumbnail ? "thumbnail" : MediaKindOf(item);
			if (const auto photo = media->photo()) {
				const auto size = thumbnail
					? Data::PhotoSize::Thumbnail
					: Data::PhotoSize::Large;
				if (const auto view = photo->activeMediaView()) {
					result.bytes = view->imageBytes(size);
					if (result.bytes.isEmpty()) {
						if (const auto image = view->image(size)) {
							result.bytes = Jpeg(image->original());
						}
					}
				}
				if (result.bytes.isEmpty()) {
					result.cache = photo->location(size).file().cacheKey();
				}
				result.media.mime = "image/jpeg";
			} else if (const auto document = media->document()) {
				const auto view = document->activeMediaView();
				if (thumbnail) {
					const auto image = view ? view->thumbnail() : nullptr;
					if (image) {
						result.bytes = Jpeg(image->original());
					}
					result.media.mime = "image/jpeg";
				} else {
					result.bytes = view ? view->bytes() : QByteArray();
					if (result.bytes.isEmpty()) {
						result.path = document->filepath(false);
						result.cache = document->cacheKey();
					}
					result.media.mime = Utf8(document->mimeString());
					result.media.name = Utf8(document->filename());
				}
			} else {
				return std::nullopt;
			}
			return result;
		}, std::nullopt);
		if (!source) {
			return std::nullopt;
		}
		auto result = source->media;
		auto bytes = source->bytes;
		if (bytes.isEmpty() && !source->path.isEmpty()) {
			auto file = QFile(source->path);
			if (file.open(QIODevice::ReadOnly)) {
				bytes = file.read(qint64(maxBytes) + 1);
			}
		}
		if (bytes.isEmpty() && source->cache) {
			const auto key = *source->cache;
			bytes = OnMainAsync<QByteArray>([=](Fn<void(QByteArray)> done) {
				const auto session = FindSession(_account);
				if (!session) {
					done(QByteArray());
					return;
				}
				session->data().cache().get(key, [=](QByteArray &&value) {
					done(std::move(value));
				});
			}, QByteArray());
		}
		if (bytes.isEmpty()) {
			return std::nullopt;
		}
		bytes.truncate(qsizetype(std::min<size_t>(size_t(bytes.size()), maxBytes + 1)));
		result.bytes.assign(bytes.constData(), bytes.constData() + bytes.size());
		return result;
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
