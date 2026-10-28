// This is the source code of freshGram.
#include "ayu/features/plugins/plugins_data.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/plugins/plugins_service.h"
#include "ayu/ui/toasts.h"
#include "ayu/secret/secret_peer.h"
#include "api/api_common.h"
#include "api/api_editing.h"
#include "api/api_send_progress.h"
#include "base/call_delayed.h"
#include "base/flat_set.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "core/file_utilities.h"
#include "core/mime_type.h"
#include "storage/file_upload.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/chat/attach/attach_prepare.h"
#include "styles/style_boxes.h"
#include "data/data_forum_topic.h"
#include "data/data_histories.h"
#include "data/data_message_reaction_id.h"
#include "window/notifications_manager.h"
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
#include "lang/lang_keys.h"
#include "core/core_settings.h"
#include "core/version.h"
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
#include "main/session/send_as_peers.h"
#include "data/components/credits.h"
#include "storage/cache/storage_cache_database.h"
#include "ui/toast/toast.h"

#ifdef FRESHGRAM_PLUGINS
#include "fgpl/manager.h"
#endif

#include <QtCore/QBuffer>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>

namespace FreshPlugins {

#ifdef FRESHGRAM_PLUGINS

namespace {

constexpr auto kWaitSlice = std::chrono::milliseconds(20);
constexpr auto kWaitTotal = std::chrono::milliseconds(1500);
constexpr auto kWaitServer = std::chrono::milliseconds(10000);
constexpr auto kWaitUser = std::chrono::minutes(5);
constexpr auto kWaitDownload = std::chrono::minutes(5);
constexpr auto kWaitSend = std::chrono::seconds(100);
constexpr auto kSendTimeout = crl::time(90 * 1000);
constexpr auto kEditMediaTimeout = crl::time(270 * 1000);
constexpr auto kMaxAliases = size_t(512);
constexpr auto kMaxPluginIds = size_t(256);
constexpr auto kOutboxLifetime = std::chrono::hours(1);
constexpr auto kMaxHistory = size_t(200);
constexpr auto kMaxScheduleSeconds = TimeId(365 * 24 * 3600);

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
[[nodiscard]] T OnMainAsync(
		Fn<void(Fn<void(T)> done)> start,
		T fallback,
		std::chrono::milliseconds wait = kWaitServer) {
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
	const auto until = std::chrono::steady_clock::now() + wait;
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

[[nodiscard]] PeerData *ChosenSendAs(not_null<History*> history) {
	const auto peer = history->peer;
	auto &sendAs = peer->session().sendAsPeers();
	return sendAs.shouldChoose({ peer })
		? sendAs.resolveChosen(peer).get()
		: nullptr;
}

struct SendTarget {
	not_null<Data::Thread*> thread;
	MsgId rootId;
	HistoryItem *replied = nullptr;
	bool missingReply = false;
};

[[nodiscard]] SendTarget ResolveTarget(
		not_null<Main::Session*> session,
		not_null<History*> history,
		MsgId topic,
		MsgId replyTo) {
	const auto peer = history->peer;
	const auto replied = replyTo
		? session->data().message(peer->id, replyTo)
		: nullptr;
	if (replyTo && !replied) {
		return {
			.thread = not_null<Data::Thread*>(history),
			.missingReply = true,
		};
	}
	const auto general = MsgId(Data::ForumTopic::kGeneralId);
	const auto rootId = replied
		? ThreadRootOf(peer, replied)
		: (topic && (peer->forum() || topic != general))
		? topic
		: MsgId();
	const auto forumTopic = rootId ? peer->forumTopicFor(rootId) : nullptr;
	return {
		.thread = forumTopic
			? not_null<Data::Thread*>(forumTopic)
			: not_null<Data::Thread*>(history),
		.rootId = rootId,
		.replied = replied,
	};
}

void ApplyTarget(Api::SendAction &action, const SendTarget &target) {
	if (target.rootId) {
		action.replyTo.topicRootId = target.rootId;
		action.replyTo.messageId = FullMsgId(
			action.history->peer->id,
			target.rootId);
	}
	if (target.replied) {
		action.replyTo.messageId = target.replied->fullId();
	}
}

int PluginSendDepth = 0;

struct Tracked {
	std::map<std::pair<const Main::Session*, FullMsgId>, MsgId> aliases;
	std::deque<std::pair<const Main::Session*, FullMsgId>> aliasOrder;
	base::flat_set<std::pair<const Main::Session*, FullMsgId>> pluginIds;
	std::deque<std::pair<const Main::Session*, FullMsgId>> pluginOrder;
};

[[nodiscard]] Tracked &GetTracked() {
	static auto result = Tracked();
	return result;
}

void MarkPluginMessage(not_null<Main::Session*> session, FullMsgId local) {
	auto &tracked = GetTracked();
	const auto key = std::make_pair(session.get(), local);
	if (!tracked.pluginIds.emplace(key).second) {
		return;
	}
	tracked.pluginOrder.push_back(key);
	while (tracked.pluginOrder.size() > kMaxPluginIds) {
		tracked.pluginIds.remove(tracked.pluginOrder.front());
		tracked.pluginOrder.pop_front();
	}
}

[[nodiscard]] MsgId AliasOf(not_null<Main::Session*> session, FullMsgId local) {
	const auto &aliases = GetTracked().aliases;
	const auto i = aliases.find(std::make_pair(session.get(), local));
	return (i != end(aliases)) ? i->second : MsgId();
}

struct SentWait {
	rpl::lifetime lifetime;
	bool finished = false;
};

// Answers the server id of a message sent from here, or nothing when it
// failed (failed == true) or did not get one in time (failed == false).
void WaitSent(
		not_null<Main::Session*> session,
		FullMsgId local,
		crl::time timeout,
		Fn<void(MsgId server, bool failed)> done) {
	const auto state = std::make_shared<SentWait>();
	const auto finish = [=](MsgId server, bool failed) {
		if (std::exchange(state->finished, true)) {
			return;
		}
		state->lifetime.destroy();
		done(server, failed);
	};
	const auto data = &session->data();
	data->itemIdChanged(
	) | rpl::filter([=](const Data::Session::IdChange &change) {
		return (change.newId.peer == local.peer) && (change.oldId == local.msg);
	}) | rpl::on_next([=](const Data::Session::IdChange &change) {
		finish(change.newId.msg, false);
	}, state->lifetime);
	data->itemRemoved(
		local
	) | rpl::on_next([=] {
		finish(MsgId(), true);
	}, state->lifetime);
	data->itemDataChanges(
	) | rpl::filter([=](not_null<HistoryItem*> item) {
		return (item->fullId() == local) && item->hasFailed();
	}) | rpl::on_next([=](not_null<HistoryItem*>) {
		finish(MsgId(), true);
	}, state->lifetime);
	base::call_delayed(timeout, session, [=] {
		finish(MsgId(), false);
	});
}

[[nodiscard]] TextWithEntities Formatted(const std::string &text, bool markdown) {
	auto result = TextWithEntities{ QString::fromStdString(text) };
	if (markdown) {
		TextUtilities::ParseEntities(result, TextParseMarkdown);
	}
	return result;
}

[[nodiscard]] std::optional<Api::SendProgressType> ProgressOf(const std::string &kind) {
	using Type = Api::SendProgressType;
	static const auto kinds = std::map<std::string, Type>{
		{ "typing", Type::Typing },
		{ "upload_photo", Type::UploadPhoto },
		{ "upload_video", Type::UploadVideo },
		{ "upload_file", Type::UploadFile },
		{ "record_voice", Type::RecordVoice },
		{ "upload_voice", Type::UploadVoice },
		{ "record_video", Type::RecordVideo },
		{ "record_round", Type::RecordRound },
		{ "choose_sticker", Type::ChooseSticker },
	};
	const auto i = kinds.find(kind);
	return (i != kinds.end()) ? std::make_optional(i->second) : std::nullopt;
}

[[nodiscard]] QString OutboxPath() {
	return cWorkingDir() + u"tdata/plugins/outbox/"_q;
}

void CleanOutbox() {
	const auto old = QDateTime::currentDateTime().addSecs(
		-std::chrono::duration_cast<std::chrono::seconds>(kOutboxLifetime).count());
	auto entries = QDirIterator(OutboxPath(), QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
	while (entries.hasNext()) {
		const auto path = entries.next();
		if (QFileInfo(path).lastModified() < old) {
			QFile::remove(path);
		}
	}
}

[[nodiscard]] QString WriteOutbox(
		const std::string &fileName,
		const fgpl::cbor::Bytes &bytes) {
	const auto folder = QString(OutboxPath() + QString::number(base::RandomValue<uint64>(), 16) + '/');
	const auto path = QString(folder + QString::fromStdString(fileName));
	CleanOutbox();
	QDir().mkpath(folder);
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly)
		|| file.write(
			reinterpret_cast<const char*>(bytes.data()),
			qint64(bytes.size())) != qint64(bytes.size())) {
		return QString();
	}
	return path;
}

[[nodiscard]] Ui::AlbumType AlbumTypeOf(not_null<HistoryItem*> item) {
	if (item->groupId().empty() || !item->media()) {
		return Ui::AlbumType::None;
	}
	const auto media = item->media();
	if (media->photo()) {
		return Ui::AlbumType::PhotoVideo;
	} else if (const auto document = media->document()) {
		return document->isVideoFile()
			? Ui::AlbumType::PhotoVideo
			: document->isSong()
			? Ui::AlbumType::Music
			: Ui::AlbumType::File;
	}
	return Ui::AlbumType::None;
}

[[nodiscard]] std::optional<ChatRestriction> RestrictionFor(Ui::PreparedFile::Type type) {
	using Type = Ui::PreparedFile::Type;
	switch (type) {
	case Type::Photo: return ChatRestriction::SendPhotos;
	case Type::Video: return ChatRestriction::SendVideos;
	case Type::Music: return ChatRestriction::SendMusic;
	case Type::File: return ChatRestriction::SendFiles;
	case Type::None: break;
	}
	return std::nullopt;
}

[[nodiscard]] MTPMessagesFilter MediaFilter(const std::string &kind) {
	return (kind == "photo")
		? MTP_inputMessagesFilterPhotos()
		: (kind == "video")
		? MTP_inputMessagesFilterVideo()
		: (kind == "file")
		? MTP_inputMessagesFilterDocument()
		: (kind == "voice")
		? MTP_inputMessagesFilterVoice()
		: (kind == "round")
		? MTP_inputMessagesFilterRoundVideo()
		: (kind == "audio")
		? MTP_inputMessagesFilterMusic()
		: (kind == "gif")
		? MTP_inputMessagesFilterGif()
		: MTP_inputMessagesFilterPhotoVideo();
}

[[nodiscard]] std::string StatusOf(not_null<UserData*> user, int64_t &lastSeen) {
	const auto status = user->lastseen();
	const auto now = base::unixtime::now();
	lastSeen = status.onlineTill();
	return user->isBot()
		? "bot"
		: status.isOnline(now)
		? "online"
		: status.isRecently()
		? "recently"
		: status.isWithinWeek()
		? "week"
		: status.isWithinMonth()
		? "month"
		: status.isLongAgo()
		? "long_ago"
		: lastSeen
		? "offline"
		: "hidden";
}

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
			action.options.sendAs = ChosenSendAs(history);
			auto message = Api::MessageToSend(action);
			message.textWithTags = TextWithTags{ QString::fromStdString(text) };
			++PluginSendDepth;
			session->api().sendMessage(std::move(message));
			--PluginSendDepth;
			return true;
		}, false);
	}

	fgpl::SendResult sendMessage(const fgpl::OutgoingMessage &outgoing) override {
		const auto failure = fgpl::SendResult{
			.error = fgpl::api::error::kUnavailable,
		};
		if (!outgoing.waitForServerId) {
			return OnMain<fgpl::SendResult>([=] {
				return startMessage(outgoing);
			}, failure);
		}
		return OnMainAsync<fgpl::SendResult>([=](
				Fn<void(fgpl::SendResult)> done) {
			awaitSent(startMessage(outgoing), outgoing.chat, done);
		}, failure, kWaitSend);
	}

	std::string edit(
			int64_t chat,
			int64_t id,
			const std::string &text,
			bool markdown) override {
		return OnMainAsync<std::string>([=](Fn<void(std::string)> done) {
			resolveMessage(chat, id, [=](HistoryItem *item, std::string error) {
				if (!error.empty()) {
					done(std::move(error));
					return;
				} else if (!item->out() || !item->allowsEdit(base::unixtime::now())) {
					done(fgpl::api::error::kDenied);
					return;
				}
				Api::EditTextMessage(
					item,
					Formatted(text, markdown),
					Data::WebPageDraft(),
					Api::SendOptions(),
					[=](mtpRequestId) { done({}); },
					[=](const QString &, mtpRequestId) {
						done(fgpl::api::error::kFailed);
					},
					false);
			});
		}, fgpl::api::error::kUnavailable, kWaitSend);
	}

	std::string remove(
			int64_t chat,
			const std::vector<int64_t> &ids,
			bool everyone) override {
		return OnMainAsync<std::string>([=](Fn<void(std::string)> done) {
			resolveMessages(chat, ids, [=](
					std::vector<HistoryItem*> items,
					std::string error) {
				const auto session = FindSession(_account);
				if (!error.empty() || !session) {
					done(error.empty() ? std::string(fgpl::api::error::kNotFound) : error);
					return;
				}
				const auto now = base::unixtime::now();
				auto list = MessageIdsList();
				auto revoke = everyone;
				for (const auto item : items) {
					if (!item->out() || !item->canDelete()) {
						done(fgpl::api::error::kDenied);
						return;
					}
					revoke = revoke && item->canDeleteForEveryone(now);
					list.push_back(item->fullId());
				}
				session->data().histories().deleteMessages(list, revoke);
				session->data().sendHistoryChangeNotifications();
				done({});
			});
		}, fgpl::api::error::kUnavailable, kWaitSend);
	}

	std::string react(
			int64_t chat,
			int64_t id,
			const std::string &emoji) override {
		return OnMainAsync<std::string>([=](Fn<void(std::string)> done) {
			resolveMessage(chat, id, [=](HistoryItem *item, std::string error) {
				if (!error.empty()) {
					done(std::move(error));
					return;
				} else if (!item->canReact()) {
					done(fgpl::api::error::kDenied);
					return;
				}
				const auto chosen = item->chosenReactions();
				const auto wanted = Data::ReactionId{ QString::fromStdString(emoji) };
				if (emoji.empty()) {
					for (const auto &reaction : chosen) {
						if (!reaction.paid()) {
							item->toggleReaction(reaction, HistoryReactionSource::Existing);
						}
					}
				} else if (!ranges::contains(chosen, wanted)) {
					item->toggleReaction(wanted, HistoryReactionSource::Selector);
				}
				done({});
			});
		}, fgpl::api::error::kUnavailable, kWaitSend);
	}

	std::string forward(
			int64_t from,
			const std::vector<int64_t> &ids,
			int64_t to,
			bool silent) override {
		return OnMain<std::string>([=]() -> std::string {
			const auto session = FindSession(_account);
			const auto target = historyOf(to);
			if (!session || !target
				|| FreshSecret::IsSecretPeerId(target->peer->id)) {
				return fgpl::api::error::kNotFound;
			} else if (!Data::CanSendTexts(target)) {
				return fgpl::api::error::kDenied;
			}
			auto items = HistoryItemsList();
			for (const auto id : ids) {
				const auto item = messageOf(from, id);
				if (!item) {
					return fgpl::api::error::kNotFound;
				} else if (!item->allowsForward() || item->isDeleted()) {
					return fgpl::api::error::kDenied;
				}
				items.push_back(item);
			}
			auto action = Api::SendAction(target);
			action.clearDraft = false;
			action.options.silent = silent;
			session->api().forwardMessages(
				Data::ResolvedForwardDraft{ .items = std::move(items) },
				action);
			return {};
		}, fgpl::api::error::kUnavailable);
	}

	std::string markRead(int64_t chat) override {
		return OnMain<std::string>([=]() -> std::string {
			const auto history = historyOf(chat);
			if (!history || FreshSecret::IsSecretPeerId(history->peer->id)) {
				return fgpl::api::error::kNotFound;
			}
			history->owner().histories().readInbox(history);
			return {};
		}, fgpl::api::error::kUnavailable);
	}

	std::string chatAction(
			int64_t chat,
			int64_t topic,
			const std::string &kind) override {
		const auto type = ProgressOf(kind);
		if (!type && kind != "cancel") {
			return fgpl::api::error::kInvalid;
		}
		return OnMain<std::string>([=]() -> std::string {
			const auto session = FindSession(_account);
			const auto history = historyOf(chat);
			if (!session || !history
				|| FreshSecret::IsSecretPeerId(history->peer->id)) {
				return fgpl::api::error::kNotFound;
			}
			auto &progress = session->sendProgressManager();
			if (type) {
				progress.update(history, MsgId(topic), *type);
			} else {
				progress.cancel(history, MsgId(topic), Api::SendProgressType::Typing);
			}
			return {};
		}, fgpl::api::error::kUnavailable);
	}

	std::vector<fgpl::Message> search(
			int64_t chat,
			const std::string &query,
			size_t limit) override {
		return OnMainAsync<std::vector<fgpl::Message>>([=](
				Fn<void(std::vector<fgpl::Message>)> done) {
			const auto session = FindSession(_account);
			const auto history = historyOf(chat);
			if (!session || !history
				|| FreshSecret::IsSecretPeerId(history->peer->id)) {
				done({});
				return;
			}
			using Flag = MTPmessages_Search::Flag;
			session->api().request(MTPmessages_Search(
				MTP_flags(Flag()),
				history->peer->input(),
				MTP_string(QString::fromStdString(query)),
				MTP_inputPeerEmpty(),
				MTP_inputPeerEmpty(),
				MTPVector<MTPReaction>(),
				MTP_int(0),
				MTP_inputMessagesFilterEmpty(),
				MTP_int(0),
				MTP_int(0),
				MTP_int(0),
				MTP_int(0),
				MTP_int(int(std::min(limit, kMaxHistory))),
				MTP_int(0),
				MTP_int(0),
				MTP_long(0)
			)).done([=](const MTPmessages_Messages &answer) {
				done(MessagesOf(answer, chat));
			}).fail([=] {
				done({});
			}).send();
		}, {});
	}

	fgpl::Result<fgpl::Media, std::string> downloadMedia(
			int64_t chat,
			int64_t message,
			bool thumbnail,
			size_t maxBytes) override {
		struct Loaded {
			std::string error;
			fgpl::Media media;
			QByteArray bytes;
		};
		const auto loaded = OnMainAsync<Loaded>([=](Fn<void(Loaded)> done) {
			const auto session = FindSession(_account);
			const auto item = messageOf(chat, message);
			const auto media = item ? item->media() : nullptr;
			if (!session || !media || media->ttlSeconds()) {
				done({ .error = fgpl::api::error::kNotFound });
				return;
			}
			const auto base = std::make_shared<fgpl::Media>();
			base->deleted = item->isDeleted();
			base->kind = thumbnail ? "thumbnail" : MediaKindOf(item);
			const auto origin = Data::FileOrigin(item->fullId());
			const auto finish = [=](QByteArray bytes) {
				done({ .media = *base, .bytes = std::move(bytes) });
			};
			const auto fail = [=](const char *error) {
				done({ .error = error });
			};
			auto check = Fn<bool()>();
			if (const auto photo = media->photo()) {
				const auto view = photo->createMediaView();
				const auto size = thumbnail
					? Data::PhotoSize::Thumbnail
					: Data::PhotoSize::Large;
				base->mime = "image/jpeg";
				check = [=] {
					if (const auto image = view->image(size)) {
						auto bytes = view->imageBytes(size);
						finish(bytes.isEmpty() ? Jpeg(image->original()) : bytes);
					} else if (photo->failed(size)) {
						fail(fgpl::api::error::kFailed);
					} else {
						return false;
					}
					return true;
				};
				if (!check()) {
					photo->load(size, origin);
				}
			} else if (const auto document = media->document()) {
				if (!thumbnail && document->size > int64(maxBytes)) {
					fail(fgpl::api::error::kLimit);
					return;
				}
				base->mime = thumbnail ? "image/jpeg" : Utf8(document->mimeString());
				base->name = thumbnail ? std::string() : Utf8(document->filename());
				const auto view = document->createMediaView();
				check = [=] {
					if (thumbnail) {
						if (const auto image = view->thumbnail()) {
							finish(Jpeg(image->original()));
							return true;
						}
					} else if (!view->bytes().isEmpty()) {
						finish(view->bytes());
						return true;
					} else if (!document->loading()
						&& (!document->filepath(true).isEmpty()
							|| document->loadedInMediaCache())) {
						finish(QByteArray());
						return true;
					}
					if (!document->loading()
						&& (document->status == FileDownloadFailed
							|| document->cancelled())) {
						fail(fgpl::api::error::kFailed);
						return true;
					}
					return false;
				};
				if (!check()) {
					if (thumbnail) {
						document->loadThumbnail(origin);
					} else {
						document->save(origin, QString());
					}
				}
			} else {
				fail(fgpl::api::error::kNotFound);
				return;
			}
			const auto lifetime = std::make_shared<rpl::lifetime>();
			session->downloaderTaskFinished(
			) | rpl::on_next([=] {
				if (check()) {
					lifetime->destroy();
				}
			}, *lifetime);
			base::call_delayed(
				std::chrono::duration_cast<std::chrono::milliseconds>(kWaitDownload).count(),
				[=] { lifetime->destroy(); });
		}, Loaded{ .error = fgpl::api::error::kUnavailable }, kWaitDownload);
		if (!loaded.error.empty()) {
			return loaded.error;
		} else if (loaded.bytes.isEmpty()) {
			auto result = media(chat, message, thumbnail, maxBytes);
			if (!result) {
				return std::string(fgpl::api::error::kNotFound);
			}
			return std::move(*result);
		} else if (size_t(loaded.bytes.size()) > maxBytes) {
			return std::string(fgpl::api::error::kLimit);
		}
		auto result = loaded.media;
		result.bytes.assign(loaded.bytes.constData(), loaded.bytes.constData() + loaded.bytes.size());
		return result;
	}

	std::vector<fgpl::Message> mediaMessages(
			int64_t chat,
			const std::string &kind,
			size_t limit,
			int64_t before) override {
		if (kind == "any") {
			auto result = history(chat, kMaxHistory, before, 0);
			std::erase_if(result, [](const fgpl::Message &message) {
				return message.media.empty();
			});
			if (result.size() > limit) {
				result.resize(limit);
			}
			return result;
		}
		return OnMainAsync<std::vector<fgpl::Message>>([=](
				Fn<void(std::vector<fgpl::Message>)> done) {
			const auto session = FindSession(_account);
			const auto history = historyOf(chat);
			if (!session || !history
				|| FreshSecret::IsSecretPeerId(history->peer->id)) {
				done({});
				return;
			}
			const auto peer = history->peer;
			using Flag = MTPmessages_Search::Flag;
			session->api().request(MTPmessages_Search(
				MTP_flags(Flag()),
				peer->input(),
				MTP_string(),
				MTP_inputPeerEmpty(),
				MTP_inputPeerEmpty(),
				MTPVector<MTPReaction>(),
				MTP_int(0),
				MediaFilter(kind),
				MTP_int(0),
				MTP_int(0),
				MTP_int(int(before)),
				MTP_int(0),
				MTP_int(int(std::min(limit, kMaxHistory))),
				MTP_int(0),
				MTP_int(0),
				MTP_long(0)
			)).done([=](const MTPmessages_Messages &answer) {
				session->data().processExistingMessages(peer->asChannel(), answer);
				auto result = std::vector<fgpl::Message>();
				for (const auto &message : MessagesOf(answer, chat)) {
					const auto item = session->data().message(peer->id, MsgId(message.id));
					result.push_back(item ? MessageOf(item) : message);
				}
				done(std::move(result));
			}).fail([=] {
				done({});
			}).send();
		}, {});
	}

	fgpl::SendResult sendMedia(const fgpl::OutgoingMedia &outgoing) override {
		const auto path = WriteOutbox(outgoing.name, outgoing.bytes);
		if (path.isEmpty()) {
			return { .error = fgpl::api::error::kFailed };
		}
		const auto failure = fgpl::SendResult{
			.error = fgpl::api::error::kUnavailable,
		};
		if (!outgoing.message.waitForServerId) {
			return OnMain<fgpl::SendResult>([=] {
				return startMedia(outgoing, path);
			}, failure);
		}
		return OnMainAsync<fgpl::SendResult>([=](
				Fn<void(fgpl::SendResult)> done) {
			awaitSent(startMedia(outgoing, path), outgoing.message.chat, done);
		}, failure, kWaitSend);
	}

	std::string editMedia(const fgpl::OutgoingMediaEdit &edit) override {
		const auto path = WriteOutbox(edit.name, edit.bytes);
		if (path.isEmpty()) {
			return fgpl::api::error::kFailed;
		}
		return OnMainAsync<std::string>([=](Fn<void(std::string)> done) {
			resolveMessage(edit.chat, edit.id, [=](
					HistoryItem *item,
					std::string error) {
				if (!error.empty()) {
					done(std::move(error));
				} else {
					startEditMedia(item, edit, path, done);
				}
			});
		}, fgpl::api::error::kUnavailable, kWaitDownload);
	}

	std::optional<fgpl::UserInfo> user(int64_t id) override {
		return OnMain<std::optional<fgpl::UserInfo>>([=]() -> std::optional<fgpl::UserInfo> {
			const auto session = FindSession(_account);
			const auto peer = session
				? session->data().peerLoaded(PeerId(uint64(id)))
				: nullptr;
			const auto user = peer ? peer->asUser() : nullptr;
			if (!user || FreshSecret::IsSecretPeerId(peer->id)) {
				return std::nullopt;
			}
			auto result = fgpl::UserInfo();
			result.id = ChatId(user);
			result.name = Utf8(user->name());
			result.username = Utf8(user->username());
			result.bot = user->isBot();
			result.premium = user->isPremium();
			result.contact = user->isContact();
			result.self = user->isSelf();
			result.status = StatusOf(user, result.lastSeen);
			return result;
		}, std::nullopt);
	}

	std::string notify(const std::string &title, const std::string &text) override {
		return OnMain<std::string>([=]() -> std::string {
			const auto session = FindSession(_account);
			if (!session) {
				return fgpl::api::error::kNotFound;
			}
			const auto heading = title.empty()
				? AppName.utf16()
				: QString::fromStdString(title);
			const auto body = QString::fromStdString(text);
			auto &manager = Core::App().notifications().manager();
			if (Core::App().settings().desktopNotify()
				&& manager.type() == Window::Notifications::ManagerType::Native) {
				static_cast<Window::Notifications::NativeManager&>(
					manager).showPluginNotification(session->user(), heading, body);
			} else {
				Ui::Toast::Show(heading + u": "_q + body);
			}
			return {};
		}, fgpl::api::error::kUnavailable);
	}

	std::string clipboardWrite(const std::string &text) override {
		return OnMain<std::string>([=]() -> std::string {
			QGuiApplication::clipboard()->setText(QString::fromStdString(text));
			return {};
		}, fgpl::api::error::kUnavailable);
	}

	std::optional<std::string> clipboardRead() override {
		return OnMain<std::optional<std::string>>([] {
			return std::make_optional(Utf8(QGuiApplication::clipboard()->text()));
		}, std::nullopt);
	}

	fgpl::Result<fgpl::PickedFile, std::string> pickFile(
			const std::string &title,
			const std::string &filter,
			size_t maxBytes) override {
		struct Picked {
			std::string error = fgpl::api::error::kUnavailable;
			QString path;
			QByteArray content;
		};
		const auto picked = OnMainAsync<Picked>([=](Fn<void(Picked)> done) {
			FileDialog::GetOpenPath(
				Core::App().getFileDialogParent(),
				title.empty()
					? tr::lng_choose_file(tr::now)
					: QString::fromStdString(title),
				filter.empty()
					? FileDialog::AllFilesFilter()
					: QString::fromStdString(filter),
				[=](FileDialog::OpenResult &&result) {
					done({
						.error = std::string(),
						.path = result.paths.isEmpty() ? QString() : result.paths.front(),
						.content = result.remoteContent,
					});
				},
				[=] { done({ .error = fgpl::api::error::kCancelled }); });
		}, Picked(), kWaitUser);
		if (!picked.error.empty()) {
			return picked.error;
		}
		auto bytes = picked.content;
		if (bytes.isEmpty() && !picked.path.isEmpty()) {
			auto file = QFile(picked.path);
			if (!file.open(QIODevice::ReadOnly)) {
				return std::string(fgpl::api::error::kFailed);
			}
			bytes = file.read(qint64(maxBytes) + 1);
		}
		auto result = fgpl::PickedFile();
		result.name = Utf8(QFileInfo(picked.path).fileName());
		result.bytes.assign(bytes.constData(), bytes.constData() + bytes.size());
		return result;
	}

	std::string saveFile(
			const std::string &name,
			const fgpl::cbor::Bytes &bytes) override {
		const auto path = OnMainAsync<std::optional<QString>>([=](
				Fn<void(std::optional<QString>)> done) {
			const auto folder = QStandardPaths::writableLocation(
				QStandardPaths::DownloadLocation);
			FileDialog::GetWritePath(
				Core::App().getFileDialogParent(),
				tr::lng_save_file(tr::now),
				FileDialog::AllFilesFilter(),
				folder + '/' + QString::fromStdString(name),
				[=](QString &&result) { done(std::move(result)); },
				[=] { done(QString()); });
		}, std::nullopt, kWaitUser);
		if (!path) {
			return fgpl::api::error::kUnavailable;
		} else if (path->isEmpty()) {
			return fgpl::api::error::kCancelled;
		}
		auto file = QFile(*path);
		if (!file.open(QIODevice::WriteOnly)
			|| file.write(
				reinterpret_cast<const char*>(bytes.data()),
				qint64(bytes.size())) != qint64(bytes.size())) {
			return fgpl::api::error::kFailed;
		}
		return {};
	}

	void toast(const std::string &text) override {
		OnMain<bool>([=] {
			Ui::Toast::Show(QString::fromStdString(text));
			return true;
		}, false);
	}

	void toastAction(const fgpl::ToastAction &toast) override {
		OnMain<bool>([=] {
			const auto account = _account;
			const auto clicked = std::make_shared<bool>(false);
			const auto notify = [=](const char *type, const char *reason) {
				using Value = fgpl::cbor::Value;
				auto map = fgpl::cbor::Map{
					{ "type", Value::FromText(type) },
					{ "id", Value::FromText(toast.id) },
				};
				if (reason) {
					map.push_back({ "reason", Value::FromText(reason) });
				}
				Post([=, event = Value::FromMap(std::move(map))](
						not_null<fgpl::PluginManager*> manager) {
					manager->send(toast.plugin, account, event);
				}, false);
			};
			auto config = Ui::Toast::Config{
				.text = TextWithEntities{ QString::fromStdString(toast.text) },
				.adaptive = true,
				.duration = crl::time(toast.seconds) * 1000,
			};
			const auto label = toast.label.empty()
				? tr::lng_translate_undo(tr::now)
				: QString::fromStdString(toast.label);
			Ayu::Ui::ShowToastWithAction(std::move(config), label, [=] {
				*clicked = true;
				notify("toast.click", nullptr);
			}, [=] {
				notify("toast.close", *clicked ? "action" : "timeout");
			});
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
	[[nodiscard]] fgpl::SendResult startMessage(
			const fgpl::OutgoingMessage &outgoing) {
		const auto session = FindSession(_account);
		const auto history = historyOf(outgoing.chat);
		if (!session || !history
			|| FreshSecret::IsSecretPeerId(history->peer->id)) {
			return { .error = fgpl::api::error::kNotFound };
		}
		const auto target = ResolveTarget(
			session,
			history,
			MsgId(outgoing.topic),
			MsgId(outgoing.replyTo));
		if (target.missingReply) {
			return { .error = fgpl::api::error::kNotFound };
		} else if (!Data::CanSendTexts(target.thread)) {
			return { .error = fgpl::api::error::kDenied };
		} else if (history->peer->slowmodeSecondsLeft() > 0) {
			return { .error = fgpl::api::error::kLimit };
		}
		if (outgoing.schedule && !ValidScheduleTime(TimeId(outgoing.schedule))) {
			return { .error = fgpl::api::error::kInvalid };
		}
		auto action = Api::SendAction(target.thread);
		action.clearDraft = false;
		action.options.silent = outgoing.silent;
		action.options.scheduled = TimeId(outgoing.schedule);
		action.options.sendAs = ChosenSendAs(history);
		ApplyTarget(action, target);
		const auto text = Formatted(outgoing.text, outgoing.markdown);
		auto message = Api::MessageToSend(action);
		message.textWithTags = TextWithTags{
			text.text,
			TextUtilities::ConvertEntitiesToTextTags(text.entities),
		};
		const auto tracked = !outgoing.schedule;
		const auto localId = tracked
			? session->data().nextLocalMessageId()
			: MsgId();
		const auto full = FullMsgId(history->peer->id, localId);
		if (tracked) {
			MarkPluginMessage(session, full);
		}
		++PluginSendDepth;
		session->api().sendMessage(
			std::move(message),
			tracked ? std::optional<MsgId>(localId) : std::nullopt);
		--PluginSendDepth;
		auto result = fgpl::SendResult();
		if (tracked
			&& (session->data().message(full) || HasPendingOutgoing(history))) {
			result.localId = localId.bare;
		}
		return result;
	}

	[[nodiscard]] fgpl::SendResult startMedia(
			const fgpl::OutgoingMedia &outgoing,
			const QString &path) {
		const auto &message = outgoing.message;
		const auto session = FindSession(_account);
		const auto history = historyOf(message.chat);
		if (!session || !history
			|| FreshSecret::IsSecretPeerId(history->peer->id)) {
			return { .error = fgpl::api::error::kNotFound };
		} else if (history->peer->starsPerMessageChecked() > 0) {
			return { .error = fgpl::api::error::kDenied };
		}
		const auto target = ResolveTarget(
			session,
			history,
			MsgId(message.topic),
			MsgId(message.replyTo));
		const auto thread = target.thread;
		if (target.missingReply) {
			return { .error = fgpl::api::error::kNotFound };
		} else if (history->peer->slowmodeSecondsLeft() > 0) {
			return { .error = fgpl::api::error::kLimit };
		}
		if (message.schedule && !ValidScheduleTime(TimeId(message.schedule))) {
			return { .error = fgpl::api::error::kInvalid };
		}
		auto action = Api::SendAction(thread);
		action.clearDraft = false;
		action.options.silent = message.silent;
		action.options.scheduled = TimeId(message.schedule);
		action.options.sendAs = ChosenSendAs(history);
		ApplyTarget(action, target);
		const auto track = [&] {
			if (!message.schedule) {
				action.localMessageId = session->data().nextLocalMessageId();
				MarkPluginMessage(
					session,
					FullMsgId(history->peer->id, action.localMessageId));
			}
		};
		if (outgoing.kind == "voice") {
			if (!Data::CanSend(thread, ChatRestriction::SendVoiceMessages)) {
				return { .error = fgpl::api::error::kDenied };
			}
			auto file = QFile(path);
			if (!file.open(QIODevice::ReadOnly)) {
				return { .error = fgpl::api::error::kFailed };
			}
			track();
			session->api().sendVoiceMessage(
				file.readAll(),
				VoiceWaveform(),
				crl::time(outgoing.duration) * 1000,
				false,
				action);
			return { .localId = action.localMessageId.bare };
		}
		auto list = Storage::PrepareMediaList(
			QStringList{ path },
			st::sendMediaPreviewSize,
			session->premium());
		if (list.error != Ui::PreparedList::Error::None || list.files.empty()) {
			return { .error = fgpl::api::error::kInvalid };
		}
		auto &file = list.files.front();
		const auto asFile = (outgoing.kind == "file");
		const auto restriction = asFile
			? std::make_optional(ChatRestriction::SendFiles)
			: RestrictionFor(file.type);
		if (!restriction || !Data::CanSend(thread, *restriction)) {
			return { .error = fgpl::api::error::kDenied };
		}
		const auto caption = Formatted(message.text, message.markdown);
		file.caption = TextWithTags{
			caption.text,
			TextUtilities::ConvertEntitiesToTextTags(caption.entities),
		};
		file.spoiler = outgoing.spoiler;
		const auto type = (asFile || file.type == Ui::PreparedFile::Type::File)
			? SendMediaType::File
			: SendMediaType::Photo;
		track();
		++PluginSendDepth;
		session->api().sendFiles(std::move(list), type, nullptr, action);
		--PluginSendDepth;
		return { .localId = action.localMessageId.bare };

	}

	void startEditMedia(
			not_null<HistoryItem*> item,
			const fgpl::OutgoingMediaEdit &edit,
			const QString &path,
			Fn<void(std::string)> done) {
		const auto session = FindSession(_account);
		if (!session) {
			done(fgpl::api::error::kNotFound);
			return;
		} else if (!item->out()
			|| !item->allowsEdit(base::unixtime::now())
			|| !item->allowsEditMedia()
			|| item->isEditingMedia()) {
			done(fgpl::api::error::kDenied);
			return;
		}
		const auto history = item->history();
		const auto albumType = AlbumTypeOf(item);
		auto list = Storage::PrepareMediaList(
			QStringList{ path },
			st::sendMediaPreviewSize,
			session->premium());
		if (list.error != Ui::PreparedList::Error::None || list.files.size() != 1) {
			done(fgpl::api::error::kInvalid);
			return;
		}
		auto &file = list.files.front();
		const auto asFile = (edit.kind == "file");
		if (file.information && Core::IsMimeSticker(file.information->filemime)) {
			done(fgpl::api::error::kInvalid);
			return;
		} else if (albumType != Ui::AlbumType::None
			&& (!file.canBeInAlbumType(albumType)
				|| (asFile && albumType == Ui::AlbumType::PhotoVideo))) {
			done(fgpl::api::error::kInvalid);
			return;
		}
		const auto restriction = asFile
			? std::make_optional(ChatRestriction::SendFiles)
			: RestrictionFor(file.type);
		if (!restriction || !Data::CanSend(history, *restriction)) {
			done(fgpl::api::error::kDenied);
			return;
		}
		const auto caption = Formatted(edit.caption, edit.markdown);
		file.spoiler = edit.spoiler;
		const auto type = (asFile || file.type == Ui::PreparedFile::Type::File)
			? SendMediaType::File
			: SendMediaType::Photo;

		auto options = Api::SendOptions();
		options.shortcutId = item->shortcutId();
		options.invertCaption = item->invertMedia();
		auto action = Api::SendAction(history, options);
		action.replaceMediaOf = item->fullId().msg;

		const auto itemId = item->fullId();
		const auto state = std::make_shared<SentWait>();
		const auto finish = [=](std::string error) {
			if (std::exchange(state->finished, true)) {
				return;
			}
			state->lifetime.destroy();
			Api::SetEditMediaCompletion(itemId, nullptr);
			done(std::move(error));
		};
		Api::SetEditMediaCompletion(itemId, [=](const QString &error) {
			if (!error.isEmpty()) {
			}
			finish(error.isEmpty() || error == u"MESSAGE_NOT_MODIFIED"_q
				? std::string()
				: (error == u"MEDIA_NEW_INVALID"_q)
				? std::string(fgpl::api::error::kInvalid)
				: ranges::contains(Api::kDefaultEditMessagesErrors, error)
				? std::string(fgpl::api::error::kDenied)
				: std::string(fgpl::api::error::kFailed));
		});
		const auto failed = [=](FullMsgId id) {
			if (id == itemId) {
				finish(fgpl::api::error::kFailed);
			}
		};
		session->uploader().photoFailed() | rpl::on_next(failed, state->lifetime);
		session->uploader().documentFailed() | rpl::on_next(failed, state->lifetime);
		session->data().itemRemoved(
			itemId
		) | rpl::on_next([=] {
			finish(fgpl::api::error::kNotFound);
		}, state->lifetime);
		base::call_delayed(kEditMediaTimeout, session, [=] {
			finish(fgpl::api::error::kFailed);
		});

		session->api().editMedia(
			std::move(list),
			type,
			TextWithTags{
				caption.text,
				TextUtilities::ConvertEntitiesToTextTags(caption.entities),
			},
			action);
	}

	void awaitSent(
			fgpl::SendResult sent,
			int64_t chat,
			Fn<void(fgpl::SendResult)> done) {
		const auto session = FindSession(_account);
		if (!sent.error.empty() || !sent.localId || !session) {
			done(std::move(sent));
			return;
		}
		const auto local = FullMsgId(PeerId(uint64(chat)), MsgId(sent.localId));
		if (const auto server = AliasOf(session, local)) {
			sent.serverId = server.bare;
			done(std::move(sent));
			return;
		}
		WaitSent(session, local, kSendTimeout, [=](MsgId server, bool failed) {
			auto result = sent;
			if (failed) {
				result.error = fgpl::api::error::kFailed;
			} else {
				result.serverId = server.bare;
			}
			done(std::move(result));
		});
	}

	// Answers the message under a server id, or under a local id, waiting
	// until a message that is still being sent gets its server id.
	void resolveMessage(
			int64_t chat,
			int64_t id,
			Fn<void(HistoryItem*, std::string error)> done) {
		const auto session = FindSession(_account);
		const auto peerId = PeerId(uint64(chat));
		if (!session || FreshSecret::IsSecretPeerId(peerId)) {
			done(nullptr, fgpl::api::error::kNotFound);
			return;
		}
		const auto msgId = MsgId(id);
		const auto found = [=](MsgId real) {
			const auto item = session->data().message(peerId, real);
			if (item) {
				done(item, {});
			} else {
				done(nullptr, fgpl::api::error::kNotFound);
			}
		};
		if (!IsClientMsgId(msgId)) {
			if (id < 0) {
				done(nullptr, fgpl::api::error::kInvalid);
			} else {
				found(msgId);
			}
			return;
		}
		const auto local = FullMsgId(peerId, msgId);
		const auto item = session->data().message(local);
		if (!item) {
			if (const auto real = AliasOf(session, local)) {
				found(real);
			} else {
				done(nullptr, fgpl::api::error::kNotFound);
			}
		} else if (item->hasFailed()) {
			done(nullptr, fgpl::api::error::kFailed);
		} else if (!item->out()
			|| !item->isLocal()
			|| item->isDeleted()
			|| item->isEphemeral()) {
			done(nullptr, fgpl::api::error::kNotFound);
		} else {
			WaitSent(session, local, kSendTimeout, [=](MsgId server, bool failed) {
				if (server) {
					found(server);
				} else {
					done(nullptr, failed
						? fgpl::api::error::kFailed
						: fgpl::api::error::kUnavailable);
				}
			});
		}
	}

	void resolveMessages(
			int64_t chat,
			std::vector<int64_t> ids,
			Fn<void(std::vector<HistoryItem*>, std::string error)> done) {
		resolveFrom(
			chat,
			std::make_shared<std::vector<int64_t>>(std::move(ids)),
			0,
			std::make_shared<std::vector<FullMsgId>>(),
			std::move(done));
	}

	void resolveFrom(
			int64_t chat,
			std::shared_ptr<std::vector<int64_t>> ids,
			size_t index,
			std::shared_ptr<std::vector<FullMsgId>> resolved,
			Fn<void(std::vector<HistoryItem*>, std::string error)> done) {
		if (index == ids->size()) {
			const auto session = FindSession(_account);
			auto result = std::vector<HistoryItem*>();
			for (const auto &id : *resolved) {
				const auto item = session ? session->data().message(id) : nullptr;
				if (!item) {
					done({}, fgpl::api::error::kNotFound);
					return;
				} else if (!ranges::contains(result, item)) {
					result.push_back(item);
				}
			}
			done(std::move(result), {});
			return;
		}
		resolveMessage(chat, (*ids)[index], [=](
				HistoryItem *item,
				std::string error) {
			if (!error.empty()) {
				done({}, std::move(error));
				return;
			}
			resolved->push_back(item->fullId());
			resolveFrom(chat, ids, index + 1, resolved, done);
		});
	}

	[[nodiscard]] HistoryItem *messageOf(int64_t chat, int64_t id) const {
		const auto session = FindSession(_account);
		const auto peerId = PeerId(uint64(chat));
		return (session && !FreshSecret::IsSecretPeerId(peerId))
			? session->data().message(peerId, MsgId(id))
			: nullptr;
	}

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

std::string PeerKind(not_null<PeerData*> peer) {
	return KindOf(peer);
}

std::string MessageMediaKind(not_null<const HistoryItem*> item) {
	return MediaKindOf(item);
}

bool ValidScheduleTime(TimeId when) {
	const auto now = base::unixtime::now();
	return (when > now + 10) && (when <= now + kMaxScheduleSeconds);
}

MsgId ThreadRootOf(
		not_null<PeerData*> peer,
		not_null<HistoryItem*> item) {
	if (peer->forum()) {
		return item->topicRootId();
	} else if (const auto top = item->replyToTop()) {
		return top;
	}
	return item->isDiscussionPost() ? item->id : MsgId();
}

std::string UserStatus(not_null<UserData*> user, int64_t &lastSeen) {
	return StatusOf(user, lastSeen);
}

bool WantsEvent(const char *type) {
	const auto snapshot = Snapshot();
	return snapshot && snapshot->wants(type);
}

bool WantsEvents() {
	const auto snapshot = Snapshot();
	return snapshot && snapshot->wantsEvents;
}

bool PluginSending() {
	return PluginSendDepth > 0;
}

bool IsPluginMessage(not_null<Main::Session*> session, FullMsgId id) {
	return GetTracked().pluginIds.contains(std::make_pair(
		static_cast<const Main::Session*>(session.get()),
		id));
}

void NoteMessageIdChange(
		not_null<Main::Session*> session,
		FullMsgId old,
		MsgId now) {
	auto &tracked = GetTracked();
	const auto key = std::make_pair(
		static_cast<const Main::Session*>(session.get()),
		old);
	tracked.pluginIds.remove(key);
	if (!tracked.aliases.emplace(key, now).second) {
		return;
	}
	tracked.aliasOrder.push_back(key);
	while (tracked.aliasOrder.size() > kMaxAliases) {
		tracked.aliases.erase(tracked.aliasOrder.front());
		tracked.aliasOrder.pop_front();
	}
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
	if (!item->reactions().empty()) {
		auto reactions = fgpl::cbor::Array();
		for (const auto &reaction : item->reactions()) {
			const auto emoji = reaction.id.emoji();
			if (emoji.isEmpty()) {
				continue;
			}
			reactions.push_back(Value::FromMap({
				{ "emoji", Value::FromText(Utf8(emoji)) },
				{ "count", Value::FromInt(reaction.count) },
				{ "my", Value::FromBool(reaction.my) },
			}));
		}
		map.push_back({ "reactions", Value::FromArray(std::move(reactions)) });
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

std::string PeerKind(not_null<PeerData*>) {
	return {};
}

std::string MessageMediaKind(not_null<const HistoryItem*>) {
	return {};
}

bool ValidScheduleTime(TimeId) {
	return false;
}

MsgId ThreadRootOf(not_null<PeerData*>, not_null<HistoryItem*>) {
	return {};
}

std::string UserStatus(not_null<UserData*>, int64_t &) {
	return {};
}

bool WantsEvent(const char *) {
	return false;
}

bool WantsEvents() {
	return false;
}

bool PluginSending() {
	return false;
}

bool IsPluginMessage(not_null<Main::Session*>, FullMsgId) {
	return false;
}

void NoteMessageIdChange(not_null<Main::Session*>, FullMsgId, MsgId) {
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
