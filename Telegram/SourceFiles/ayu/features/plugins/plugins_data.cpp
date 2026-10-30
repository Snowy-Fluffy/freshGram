// This is the source code of freshGram.
#include "ayu/features/plugins/plugins_data.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/plugins/plugins_service.h"
#include "ayu/ui/toasts.h"
#include "ayu/secret/secret_peer.h"
#include "api/api_common.h"
#include "api/api_editing.h"
#include "api/api_send_progress.h"
#include "api/api_sending.h"
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
#include "history/history_item_reply_markup.h"
#include "data/data_channel.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/data_groups.h"
#include "ayu/features/filters/filters_cache_controller.h"
#include "data/notify/data_notify_settings.h"
#include "api/api_blocked_peers.h"
#include "api/api_chat_participants.h"
#include "api/api_polls.h"
#include "data/data_poll.h"
#include "data/stickers/data_stickers.h"
#include "data/stickers/data_custom_emoji.h"
#include "ui/emoji_config.h"
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
constexpr auto kWaitPhoto = std::chrono::milliseconds(30000);
constexpr auto kWaitUser = std::chrono::minutes(5);
constexpr auto kWaitDownload = std::chrono::minutes(5);
constexpr auto kWaitSend = std::chrono::seconds(100);
constexpr auto kSendTimeout = crl::time(90 * 1000);
constexpr auto kEditMediaTimeout = crl::time(270 * 1000);
constexpr auto kMaxAliases = size_t(512);
constexpr auto kMaxPluginIds = size_t(256);
constexpr auto kOutboxLifetime = std::chrono::hours(1);
constexpr auto kPollQuestionLimit = 255;
constexpr auto kPollOptionLimit = 100;
constexpr auto kPollExplanationLimit = 200;
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

[[nodiscard]] bool HiddenPeer(not_null<PeerData*> peer) {
	return FreshSecret::IsSecretPeerId(peer->id) || peer->isSelf();
}

[[nodiscard]] bool HiddenPeerId(
		not_null<Main::Session*> session,
		PeerId peerId) {
	return FreshSecret::IsSecretPeerId(peerId)
		|| peerId == session->userPeerId();
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

[[nodiscard]] fgpl::ReactionKind ReactionKindOf(const Data::ReactionId &id) {
	if (id.paid()) {
		return { .paid = true };
	} else if (const auto custom = std::get_if<DocumentId>(&id.data)) {
		return { .customEmoji = int64_t(*custom) };
	}
	return { .emoji = Utf8(id.emoji()) };
}

[[nodiscard]] fgpl::StickerInfo StickerInfoOf(not_null<DocumentData*> document) {
	auto result = fgpl::StickerInfo();
	result.id = int64_t(document->id);
	if (const auto sticker = document->sticker()) {
		result.emoji = Utf8(sticker->alt);
		result.animated = sticker->isLottie() || sticker->isAnimated();
		result.video = sticker->isWebm();
		auto set = sticker->set.shortName;
		if (set.isEmpty() && sticker->set.id) {
			const auto &sets = document->owner().stickers().sets();
			const auto i = sets.find(sticker->set.id);
			if (i != sets.end()) {
				set = i->second->shortName;
			}
		}
		result.set = Utf8(set);
	}
	return result;
}

[[nodiscard]] const std::map<EntityType, const char*> &EntityNames() {
	static const auto names = std::map<EntityType, const char*>{
		{ EntityType::Bold, "bold" },
		{ EntityType::Italic, "italic" },
		{ EntityType::Underline, "underline" },
		{ EntityType::StrikeOut, "strike" },
		{ EntityType::Code, "code" },
		{ EntityType::Pre, "pre" },
		{ EntityType::Spoiler, "spoiler" },
		{ EntityType::Blockquote, "blockquote" },
		{ EntityType::CustomUrl, "text_url" },
		{ EntityType::MentionName, "mention_name" },
		{ EntityType::CustomEmoji, "custom_emoji" },
		{ EntityType::Url, "url" },
		{ EntityType::Mention, "mention" },
		{ EntityType::Hashtag, "hashtag" },
		{ EntityType::Cashtag, "cashtag" },
		{ EntityType::BotCommand, "bot_command" },
		{ EntityType::Email, "email" },
		{ EntityType::Phone, "phone" },
	};
	return names;
}

[[nodiscard]] std::vector<fgpl::TextEntity> EntitiesOfList(
		const EntitiesInText &list) {
	auto result = std::vector<fgpl::TextEntity>();
	for (const auto &entity : list) {
		const auto i = EntityNames().find(entity.type());
		if (i == EntityNames().end() || result.size() >= fgpl::kMaxTextEntities) {
			continue;
		}
		auto item = fgpl::TextEntity{
			.type = i->second,
			.offset = entity.offset(),
			.length = entity.length(),
		};
		if (entity.type() == EntityType::CustomUrl) {
			item.url = Utf8(entity.data());
		} else if (entity.type() == EntityType::Pre) {
			item.language = Utf8(entity.data());
		} else if (entity.type() == EntityType::CustomEmoji) {
			item.id = int64_t(Data::ParseCustomEmojiData(entity.data()));
		} else if (entity.type() == EntityType::MentionName) {
			item.id = int64_t(TextUtilities::MentionNameDataToFields(entity.data()).userId);
		}
		result.push_back(std::move(item));
	}
	return result;
}

// The entities Telegram finds itself are skipped, a mention needs a known user.
[[nodiscard]] EntitiesInText EntitiesToApp(
		Main::Session *session,
		const QString &text,
		const std::vector<fgpl::TextEntity> &entities) {
	static const auto types = [] {
		auto result = std::map<std::string, EntityType>();
		for (const auto &[type, name] : EntityNames()) {
			if (!fgpl::IsDetectedEntity(name)) {
				result.emplace(name, type);
			}
		}
		return result;
	}();
	auto converted = EntitiesInText();
	for (const auto &item : entities) {
		const auto i = types.find(item.type);
		if (i == types.end()
			|| item.offset > text.size()
			|| item.length > text.size() - item.offset) {
			continue;
		}
		auto data = QString();
		if (i->second == EntityType::CustomUrl) {
			data = QString::fromStdString(item.url);
		} else if (i->second == EntityType::Pre) {
			data = QString::fromStdString(item.language);
		} else if (i->second == EntityType::CustomEmoji) {
			data = Data::SerializeCustomEmojiId(DocumentId(item.id));
		} else if (i->second == EntityType::MentionName) {
			const auto user = session
				? session->data().userLoaded(UserId(item.id))
				: nullptr;
			if (!user) {
				continue;
			}
			data = TextUtilities::MentionNameDataFromFields({
				.selfId = session->userId().bare,
				.userId = peerToUser(user->id).bare,
				.accessHash = user->accessHash(),
			});
		}
		converted.push_back(EntityInText(
			i->second,
			int(item.offset),
			int(item.length),
			data));
	}
	return converted;
}

// Only emoji and custom emoji (and spaces), like the big emoji in a chat.
[[nodiscard]] bool IsEmojiOnly(const TextWithEntities &text) {
	const auto length = int(text.text.size());
	auto found = false;
	for (auto i = 0; i != length;) {
		if (text.text[i].isSpace()) {
			++i;
			continue;
		}
		const auto custom = ranges::find_if(text.entities, [&](
				const EntityInText &entity) {
			return entity.type() == EntityType::CustomEmoji
				&& entity.offset() == i
				&& entity.length() > 0;
		});
		if (custom != text.entities.end()) {
			i += custom->length();
		} else {
			auto emojiLength = 0;
			if (!Ui::Emoji::Find(
					QStringView(text.text).mid(i),
					&emojiLength)) {
				return false;
			}
			i += emojiLength;
		}
		found = true;
	}
	return found;
}

[[nodiscard]] fgpl::Message MessageOf(not_null<const HistoryItem*> item) {
	auto result = fgpl::Message();
	result.id = item->id.bare;
	result.chat = ChatId(item->history()->peer);
	result.from = ChatId(item->from());
	result.fromKind = KindOf(item->from());
	result.date = item->date();
	const auto &original = item->originalText();
	result.text = Utf8(original.text);
	result.emojiOnly = IsEmojiOnly(original);
	result.entities = EntitiesOfList(original.entities);
	if (const auto media = item->media()) {
		if (const auto document = media->document()) {
			if (document->sticker()) {
				result.sticker = StickerInfoOf(document);
			}
		}
	}
	for (const auto &reaction : item->reactions()) {
		result.reactions.push_back({
			.kind = ReactionKindOf(reaction.id),
			.count = reaction.count,
			.my = reaction.my,
		});
	}
	result.topic = item->topicRootId().bare;
	result.replyTo = item->replyToId().bare;
	result.out = item->out();
	result.deleted = item->isDeleted();
	result.media = MediaKindOf(item);
	if (const auto markup = item->inlineReplyMarkup()) {
		using Type = HistoryMessageMarkupButton::Type;
		for (const auto &row : markup->data.rows) {
			auto line = std::vector<fgpl::MessageButton>();
			for (const auto &button : row) {
				auto entry = fgpl::MessageButton{ .text = Utf8(button.text) };
				if (button.type == Type::Callback) {
					entry.type = "callback";
				} else if (button.type == Type::Url) {
					entry.type = "url";
					entry.url = button.data.toStdString();
				} else {
					entry.type = "other";
				}
				line.push_back(std::move(entry));
			}
			result.buttons.push_back(std::move(line));
		}
	}
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

[[nodiscard]] fgpl::InlineResult InlineResultOf(const MTPBotInlineResult &entry) {
	auto result = fgpl::InlineResult();
	const auto fill = [&](const auto &data) {
		result.id = data.vid().v.toStdString();
		if (const auto title = data.vtitle()) {
			result.title = title->v.toStdString();
		}
		if (const auto description = data.vdescription()) {
			result.description = description->v.toStdString();
		}
		data.vsend_message().match([&](const MTPDbotInlineMessageText &text) {
			result.text = text.vmessage().v.toStdString();
		}, [](const auto &) {
		});
	};
	entry.match([&](const MTPDbotInlineResult &data) {
		fill(data);
	}, [&](const MTPDbotInlineMediaResult &data) {
		fill(data);
	});
	return result;
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

[[nodiscard]] std::optional<fgpl::ChannelInfo> ChannelInfoOf(
		not_null<PeerData*> peer) {
	const auto channel = peer->asChannel();
	if (!channel || FreshSecret::IsSecretPeerId(peer->id)) {
		return std::nullopt;
	}
	auto result = fgpl::ChannelInfo();
	result.id = ChatId(channel);
	result.title = Utf8(channel->name());
	result.username = Utf8(channel->username());
	result.type = KindOf(channel);
	result.about = Utf8(channel->about());
	result.members = channel->membersCountKnown()
		? channel->membersCount()
		: -1;
	result.verified = channel->isVerified();
	result.scam = channel->isScam();
	result.fake = channel->isFake();
	result.hasPhoto = channel->hasUserpic();
	return result;
}

[[nodiscard]] std::optional<fgpl::UserInfo> UserInfoOf(
		not_null<PeerData*> peer) {
	const auto user = peer->asUser();
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
	result.hasPhoto = user->hasUserpic();
	return result;
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
			int64_t topic,
			int64_t fromUser) override {
		const auto cap = std::min(limit, kMaxHistory);
		auto result = OnMain<std::vector<fgpl::Message>>([=] {
			auto result = std::vector<fgpl::Message>();
			const auto history = historyOf(chat);
			if (!history || HiddenPeer(history->peer)) {
				return result;
			}
			for (auto b = history->blocks.rbegin(); b != history->blocks.rend(); ++b) {
				for (auto m = (*b)->messages.rbegin(); m != (*b)->messages.rend(); ++m) {
					const auto item = (*m)->data();
					if (item->isService()
						|| item->isEmpty()
						|| !IsServerMsgId(item->id)
						|| (topic && item->topicRootId().bare != topic)
						|| (fromUser && ChatId(item->from()) != fromUser)) {
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
			if (!session || !history || HiddenPeer(history->peer)) {
				done({});
				return;
			}
			const auto finish = [=](const MTPmessages_Messages &answer) {
				done(MessagesOf(answer, chat));
			};
			const auto fail = [=] {
				done({});
			};
			if (fromUser) {
				const auto sender = session->data().peerLoaded(
					PeerId(uint64(fromUser)));
				if (!sender) {
					done({});
					return;
				}
				using Flag = MTPmessages_Search::Flag;
				session->api().request(MTPmessages_Search(
					MTP_flags(Flag::f_from_id
						| (topic ? Flag::f_top_msg_id : Flag())),
					history->peer->input(),
					MTP_string(),
					sender->input(),
					MTP_inputPeerEmpty(),
					MTPVector<MTPReaction>(),
					MTP_int(topic),
					MTP_inputMessagesFilterEmpty(),
					MTP_int(0),
					MTP_int(0),
					MTP_int(offset),
					MTP_int(0),
					MTP_int(more),
					MTP_int(0),
					MTP_int(0),
					MTP_long(0)
				)).done([=](const MTPmessages_Messages &answer) {
					auto list = MessagesOf(answer, chat);
					for (auto &message : list) {
						message.from = fromUser;
					}
					done(std::move(list));
				}).fail(fail).send();
			} else if (topic) {
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

	int64_t serverTime() override {
		return base::unixtime::now();
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
			if (!history || HiddenPeer(history->peer)) {
				return std::string();
			}
			const auto draft = history->localDraft({}, {});
			return draft ? Utf8(draft->textWithTags.text) : std::string();
		}, std::string());
	}

	bool setDraft(int64_t chat, const std::string &text) override {
		return OnMain<bool>([=] {
			const auto history = historyOf(chat);
			if (!history || HiddenPeer(history->peer)) {
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
				|| HiddenPeer(history->peer)) {
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
			bool markdown,
			const std::vector<fgpl::TextEntity> &entities) override {
		return OnMainAsync<std::string>([=](Fn<void(std::string)> done) {
			resolveMessage(chat, id, [=](HistoryItem *item, std::string error) {
				if (!error.empty()) {
					done(std::move(error));
					return;
				} else if (!item->out() || !item->allowsEdit(base::unixtime::now())) {
					done(fgpl::api::error::kDenied);
					return;
				}
				auto formatted = Formatted(text, markdown);
				for (auto &entity : EntitiesToApp(
						&item->history()->session(),
						formatted.text,
						entities)) {
					formatted.entities.push_back(std::move(entity));
				}
				Api::EditTextMessage(
					item,
					std::move(formatted),
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

	fgpl::Result<fgpl::Message, std::string> message(
			int64_t chat,
			int64_t id) override {
		struct Found {
			std::string error;
			fgpl::Message message;
		};
		const auto found = OnMainAsync<Found>([=](Fn<void(Found)> done) {
			const auto session = FindSession(_account);
			const auto peerId = PeerId(uint64(chat));
			if (!session || id <= 0 || HiddenPeerId(session, peerId)) {
				done({ .error = fgpl::api::error::kNotFound });
				return;
			}
			const auto finish = [=] {
				const auto item = session->data().message(peerId, MsgId(id));
				if (!item
					|| item->isService()
					|| item->isEmpty()
					|| !IsServerMsgId(item->id)) {
					done({ .error = fgpl::api::error::kNotFound });
				} else {
					done({ .message = MessageOf(item) });
				}
			};
			const auto peer = session->data().peerLoaded(peerId);
			if (session->data().message(peerId, MsgId(id)) || !peer) {
				finish();
			} else {
				session->api().requestMessageData(peer, MsgId(id), finish);
			}
		}, Found{ .error = fgpl::api::error::kUnavailable });
		if (!found.error.empty()) {
			return found.error;
		}
		return found.message;
	}

	fgpl::Result<std::vector<fgpl::Message>, std::string> pinnedMessages(
			int64_t chat,
			size_t limit) override {
		const auto count = int(std::min(limit, kMaxHistory));
		return listFromServer(chat, [=](
				not_null<Main::Session*> session,
				not_null<PeerData*> peer,
				Fn<void(const MTPmessages_Messages&)> done,
				Fn<void()> fail) {
			session->api().request(MTPmessages_Search(
				MTP_flags(MTPmessages_Search::Flag()),
				peer->input(),
				MTP_string(),
				MTP_inputPeerEmpty(),
				MTP_inputPeerEmpty(),
				MTPVector<MTPReaction>(),
				MTP_int(0),
				MTP_inputMessagesFilterPinned(),
				MTP_int(0),
				MTP_int(0),
				MTP_int(0),
				MTP_int(0),
				MTP_int(count),
				MTP_int(0),
				MTP_int(0),
				MTP_long(0)
			)).done(done).fail(fail).send();
		});
	}

	fgpl::Result<std::vector<fgpl::Message>, std::string> scheduledMessages(
			int64_t chat) override {
		return listFromServer(chat, [=](
				not_null<Main::Session*> session,
				not_null<PeerData*> peer,
				Fn<void(const MTPmessages_Messages&)> done,
				Fn<void()> fail) {
			session->api().request(MTPmessages_GetScheduledHistory(
				peer->input(),
				MTP_long(0)
			)).done(done).fail(fail).send();
		});
	}

	std::string deleteScheduled(
			int64_t chat,
			const std::vector<int64_t> &ids) override {
		return OnMainAsync<std::string>([=](Fn<void(std::string)> done) {
			const auto session = FindSession(_account);
			const auto peer = session
				? session->data().peerLoaded(PeerId(uint64(chat)))
				: nullptr;
			if (!peer || HiddenPeer(peer)) {
				done(fgpl::api::error::kNotFound);
				return;
			}
			auto list = QVector<MTPint>();
			for (const auto id : ids) {
				list.push_back(MTP_int(int(id)));
			}
			session->api().request(MTPmessages_DeleteScheduledMessages(
				peer->input(),
				MTP_vector<MTPint>(std::move(list))
			)).done([=](const MTPUpdates &result) {
				session->api().applyUpdates(result);
				done({});
			}).fail([=] {
				done(fgpl::api::error::kFailed);
			}).send();
		}, fgpl::api::error::kUnavailable, kWaitSend);
	}

	std::string pinMessage(
			int64_t chat,
			int64_t id,
			bool pinned,
			bool silent) override {
		return OnMainAsync<std::string>([=](Fn<void(std::string)> done) {
			resolveMessage(chat, id, [=](HistoryItem *item, std::string error) {
				if (!error.empty()) {
					done(std::move(error));
					return;
				}
				const auto peer = item->history()->peer;
				if (!peer->canPinMessages()) {
					done(fgpl::api::error::kDenied);
					return;
				}
				using Flag = MTPmessages_UpdatePinnedMessage::Flag;
				const auto session = &peer->session();
				session->api().request(MTPmessages_UpdatePinnedMessage(
					MTP_flags((pinned ? Flag() : Flag::f_unpin)
						| ((pinned && silent) ? Flag::f_silent : Flag())),
					peer->input(),
					MTP_int(item->id.bare)
				)).done([=](const MTPUpdates &result) {
					session->api().applyUpdates(result);
					done({});
				}).fail([=] {
					done(fgpl::api::error::kFailed);
				}).send();
			});
		}, fgpl::api::error::kUnavailable, kWaitSend);
	}

	std::string muteChat(int64_t chat, int64_t seconds) override {
		return OnMain<std::string>([=] {
			const auto session = FindSession(_account);
			const auto peer = session
				? session->data().peerLoaded(PeerId(uint64(chat)))
				: nullptr;
			if (!peer || HiddenPeer(peer)) {
				return std::string(fgpl::api::error::kNotFound);
			}
			auto value = Data::MuteValue();
			if (!seconds) {
				value.unmute = true;
			} else if (seconds < 0) {
				value.forever = true;
			} else {
				value.period = int(seconds);
			}
			session->data().notifySettings().update(peer, value);
			return std::string();
		}, std::string(fgpl::api::error::kUnavailable));
	}

	std::string archiveChat(int64_t chat, bool archived) override {
		return OnMain<std::string>([=] {
			const auto history = historyOf(chat);
			if (!history || HiddenPeer(history->peer)) {
				return std::string(fgpl::api::error::kNotFound);
			}
			history->session().api().toggleHistoryArchived(history, archived, nullptr);
			return std::string();
		}, std::string(fgpl::api::error::kUnavailable));
	}

	std::string pinChat(int64_t chat, bool pinned) override {
		return OnMain<std::string>([=] {
			const auto history = historyOf(chat);
			if (!history || HiddenPeer(history->peer)) {
				return std::string(fgpl::api::error::kNotFound);
			} else if (history->isPinnedDialog(FilterId()) == pinned) {
				return std::string();
			}
			const auto owner = &history->owner();
			if (pinned && !owner->pinnedCanPin(history)) {
				return std::string(fgpl::api::error::kLimit);
			}
			owner->setChatPinned(history, FilterId(), pinned);
			const auto flags = pinned
				? MTPmessages_ToggleDialogPin::Flag::f_pinned
				: MTPmessages_ToggleDialogPin::Flag(0);
			owner->session().api().request(MTPmessages_ToggleDialogPin(
				MTP_flags(flags),
				MTP_inputDialogPeer(history->peer->input())
			)).done([=] {
				owner->notifyPinnedDialogsOrderUpdated();
			}).send();
			return std::string();
		}, std::string(fgpl::api::error::kUnavailable));
	}

	std::string moderate(int64_t chat, int64_t user, bool ban) override {
		return OnMain<std::string>([=] {
			const auto session = FindSession(_account);
			const auto peer = session
				? session->data().peerLoaded(PeerId(uint64(chat)))
				: nullptr;
			const auto member = session
				? session->data().userLoaded(UserId(user))
				: nullptr;
			if (!peer || HiddenPeer(peer) || !member || HiddenPeer(member)) {
				return std::string(fgpl::api::error::kNotFound);
			} else if (const auto channel = peer->asChannel()) {
				if (!channel->canBanMembers()) {
					return std::string(fgpl::api::error::kDenied);
				} else if (ban) {
					session->api().chatParticipants().kick(
						channel,
						member,
						ChatRestrictionsInfo());
				} else {
					session->api().chatParticipants().unblock(channel, member);
				}
				return std::string();
			} else if (const auto basic = peer->asChat()) {
				if (!ban) {
					return std::string(fgpl::api::error::kInvalid);
				} else if (!basic->canBanMembers()) {
					return std::string(fgpl::api::error::kDenied);
				}
				session->api().chatParticipants().kick(basic, member);
				return std::string();
			}
			return std::string(fgpl::api::error::kNotFound);
		}, std::string(fgpl::api::error::kUnavailable));
	}

	std::string blockUser(int64_t id, bool blocked) override {
		return OnMain<std::string>([=] {
			const auto session = FindSession(_account);
			const auto user = session
				? session->data().userLoaded(UserId(id))
				: nullptr;
			if (!user || HiddenPeer(user) || user->isSelf()) {
				return std::string(fgpl::api::error::kNotFound);
			} else if (blocked) {
				session->api().blockedPeers().block(user);
			} else {
				session->api().blockedPeers().unblock(user);
			}
			return std::string();
		}, std::string(fgpl::api::error::kUnavailable));
	}

	std::string addContact(
			int64_t id,
			const std::string &firstName,
			const std::string &lastName) override {
		return OnMainAsync<std::string>([=](Fn<void(std::string)> done) {
			const auto session = FindSession(_account);
			const auto user = session
				? session->data().userLoaded(UserId(id))
				: nullptr;
			if (!user || HiddenPeer(user) || user->isBot()) {
				done(fgpl::api::error::kNotFound);
				return;
			}
			const auto first = QString::fromStdString(firstName);
			const auto last = QString::fromStdString(lastName);
			session->api().request(MTPcontacts_AddContact(
				MTP_flags(0),
				user->inputUser(),
				MTP_string(first),
				MTP_string(last),
				MTP_string(user->phone()),
				MTPTextWithEntities()
			)).done([=](const MTPUpdates &result) {
				user->setName(first, last, user->nameOrPhone, user->username());
				session->api().applyUpdates(result);
				done({});
			}).fail([=] {
				done(fgpl::api::error::kFailed);
			}).send();
		}, fgpl::api::error::kUnavailable, kWaitSend);
	}

	fgpl::Result<std::vector<fgpl::Member>, std::string> members(
			int64_t chat,
			const std::string &kind,
			const std::string &query,
			size_t offset,
			size_t limit) override {
		struct Listed {
			std::string error;
			std::vector<fgpl::Member> list;
		};
		const auto listed = OnMainAsync<Listed>([=](Fn<void(Listed)> done) {
			const auto session = FindSession(_account);
			const auto peer = session
				? session->data().peerLoaded(PeerId(uint64(chat)))
				: nullptr;
			if (!peer || HiddenPeer(peer)) {
				done({ .error = fgpl::api::error::kNotFound });
				return;
			}
			const auto memberOf = [=](
					PeerId id,
					const char *role) -> std::optional<fgpl::Member> {
				const auto user = peerIsUser(id)
					? session->data().userLoaded(peerToUser(id))
					: nullptr;
				if (!user || HiddenPeer(user)) {
					return std::nullopt;
				}
				return fgpl::Member{
					.id = ChatId(user),
					.name = Utf8(user->name()),
					.username = Utf8(user->username()),
					.role = role,
					.bot = user->isBot(),
				};
			};
			const auto fail = [=] {
				done({ .error = fgpl::api::error::kFailed });
			};
			if (const auto channel = peer->asChannel()) {
				if (!channel->canViewMembers()) {
					done({ .error = fgpl::api::error::kDenied });
					return;
				}
				auto filter = MTPChannelParticipantsFilter(
					MTP_channelParticipantsRecent());
				if (kind == "admins") {
					filter = MTP_channelParticipantsAdmins();
				} else if (kind == "search") {
					filter = MTP_channelParticipantsSearch(
						MTP_string(QString::fromStdString(query)));
				}
				session->api().request(MTPchannels_GetParticipants(
					channel->inputChannel(),
					filter,
					MTP_int(int(offset)),
					MTP_int(int(limit)),
					MTP_long(0)
				)).done([=](const MTPchannels_ChannelParticipants &result) {
					auto answer = Listed();
					result.match([&](const MTPDchannels_channelParticipants &data) {
						session->data().processUsers(data.vusers());
						session->data().processChats(data.vchats());
						for (const auto &participant : data.vparticipants().v) {
							const auto [id, role] = participant.match([](
									const MTPDchannelParticipant &d) {
								return std::pair(peerFromUser(UserId(d.vuser_id().v)), "member");
							}, [](const MTPDchannelParticipantSelf &d) {
								return std::pair(peerFromUser(UserId(d.vuser_id().v)), "member");
							}, [](const MTPDchannelParticipantCreator &d) {
								return std::pair(peerFromUser(UserId(d.vuser_id().v)), "creator");
							}, [](const MTPDchannelParticipantAdmin &d) {
								return std::pair(peerFromUser(UserId(d.vuser_id().v)), "admin");
							}, [](const MTPDchannelParticipantBanned &d) {
								return std::pair(peerFromMTP(d.vpeer()), "banned");
							}, [](const MTPDchannelParticipantLeft &d) {
								return std::pair(peerFromMTP(d.vpeer()), "left");
							});
							if (const auto member = memberOf(id, role)) {
								answer.list.push_back(*member);
							}
						}
					}, [](const MTPDchannels_channelParticipantsNotModified &) {
					});
					done(std::move(answer));
				}).fail(fail).send();
			} else if (const auto basic = peer->asChat()) {
				session->api().request(MTPmessages_GetFullChat(
					basic->inputChat()
				)).done([=](const MTPmessages_ChatFull &result) {
					const auto &data = result.data();
					session->data().processUsers(data.vusers());
					session->data().processChats(data.vchats());
					auto answer = Listed();
					data.vfull_chat().match([&](const MTPDchatFull &full) {
						full.vparticipants().match([&](const MTPDchatParticipants &list) {
							const auto needle = QString::fromStdString(query).toLower();
							auto skipped = size_t(0);
							for (const auto &participant : list.vparticipants().v) {
								const auto [id, role] = participant.match([](
										const MTPDchatParticipant &d) {
									return std::pair(peerFromUser(UserId(d.vuser_id().v)), "member");
								}, [](const MTPDchatParticipantCreator &d) {
									return std::pair(peerFromUser(UserId(d.vuser_id().v)), "creator");
								}, [](const MTPDchatParticipantAdmin &d) {
									return std::pair(peerFromUser(UserId(d.vuser_id().v)), "admin");
								});
								auto member = memberOf(id, role);
								if (!member
									|| (kind == "admins" && std::string(role) == "member")
									|| (kind == "search"
										&& !QString::fromStdString(member->name).toLower().contains(needle)
										&& !QString::fromStdString(member->username).toLower().contains(needle))) {
									continue;
								} else if (skipped < offset) {
									++skipped;
									continue;
								} else if (answer.list.size() >= limit) {
									break;
								}
								answer.list.push_back(std::move(*member));
							}
						}, [](const MTPDchatParticipantsForbidden &) {
						});
					}, [](const auto &) {
					});
					done(std::move(answer));
				}).fail(fail).send();
			} else {
				done({ .error = fgpl::api::error::kNotFound });
			}
		}, Listed{ .error = fgpl::api::error::kUnavailable });
		if (!listed.error.empty()) {
			return listed.error;
		}
		return listed.list;
	}

	fgpl::SendResult sendPoll(const fgpl::OutgoingPoll &poll) override {
		return OnMain<fgpl::SendResult>([=] {
			const auto session = FindSession(_account);
			const auto history = historyOf(poll.chat);
			if (!session || !history || HiddenPeer(history->peer)) {
				return fgpl::SendResult{ .error = fgpl::api::error::kNotFound };
			}
			const auto target = ResolveTarget(
				session,
				history,
				MsgId(poll.topic),
				MsgId(poll.replyTo));
			if (target.missingReply) {
				return fgpl::SendResult{ .error = fgpl::api::error::kNotFound };
			} else if (!Data::CanSend(target.thread, ChatRestriction::SendPolls)) {
				return fgpl::SendResult{ .error = fgpl::api::error::kDenied };
			} else if (history->peer->slowmodeSecondsLeft() > 0) {
				return fgpl::SendResult{ .error = fgpl::api::error::kLimit };
			}
			const auto question = QString::fromStdString(poll.question).trimmed();
			const auto explanation = QString::fromStdString(poll.explanation).trimmed();
			if (question.isEmpty() || question.size() > kPollQuestionLimit
				|| explanation.size() > kPollExplanationLimit) {
				return fgpl::SendResult{ .error = fgpl::api::error::kInvalid };
			}
			using Flag = PollData::Flag;
			auto data = PollData(&session->data(), base::RandomValue<uint64>());
			data.question = TextWithEntities{ question };
			data.solution = TextWithEntities{ explanation };
			auto index = 0;
			for (const auto &option : poll.options) {
				const auto text = QString::fromStdString(option).trimmed();
				if (text.isEmpty() || text.size() > kPollOptionLimit) {
					return fgpl::SendResult{ .error = fgpl::api::error::kInvalid };
				}
				auto answer = PollAnswer{
					TextWithEntities{ text },
					QByteArray(1, char('0' + index)),
				};
				answer.correct = (index == poll.correct);
				data.answers.push_back(std::move(answer));
				++index;
			}
			if (poll.closePeriod) {
				data.closePeriod = TimeId(poll.closePeriod);
				data.closeDate = base::unixtime::now() + data.closePeriod;
			}
			data.setFlags(Flag(0)
				| (poll.anonymous ? Flag(0) : Flag::PublicVotes)
				| (poll.multiple ? Flag::MultiChoice : Flag(0))
				| (poll.quiz ? Flag::Quiz : Flag(0)));
			auto action = Api::SendAction(target.thread);
			action.clearDraft = false;
			action.options.silent = poll.silent;
			action.options.sendAs = ChosenSendAs(history);
			ApplyTarget(action, target);
			session->api().polls().create(
				data,
				TextWithEntities(),
				action,
				[] {},
				[](bool) {});
			return fgpl::SendResult();
		}, fgpl::SendResult{ .error = fgpl::api::error::kUnavailable });
	}

	fgpl::Result<fgpl::GhostState, std::string> ghostState() override {
		struct Read {
			std::string error;
			fgpl::GhostState state;
		};
		const auto read = OnMain<Read>([=] {
			const auto session = FindSession(_account);
			if (!session) {
				return Read{ .error = fgpl::api::error::kNotFound };
			}
			const auto &ghost = AyuSettings::ghost(session);
			return Read{ .state = {
				.active = ghost.isGhostModeActive(),
				.readMessages = ghost.sendReadMessages(),
				.readStories = ghost.sendReadStories(),
				.onlinePackets = ghost.sendOnlinePackets(),
				.uploadProgress = ghost.sendUploadProgress(),
			} };
		}, Read{ .error = fgpl::api::error::kUnavailable });
		if (!read.error.empty()) {
			return read.error;
		}
		return read.state;
	}

	std::string setGhost(bool enabled) override {
		return OnMain<std::string>([=] {
			const auto session = FindSession(_account);
			if (!session) {
				return std::string(fgpl::api::error::kNotFound);
			}
			AyuSettings::ghost(session).setGhostModeEnabled(enabled);
			return std::string();
		}, std::string(fgpl::api::error::kUnavailable));
	}

	fgpl::Result<fgpl::ButtonAnswer, std::string> pressButton(
			int64_t chat,
			int64_t id,
			int64_t row,
			int64_t column) override {
		struct Pressed {
			std::string error;
			fgpl::ButtonAnswer answer;
		};
		const auto pressed = OnMainAsync<Pressed>([=](Fn<void(Pressed)> done) {
			resolveMessage(chat, id, [=](HistoryItem *item, std::string error) {
				if (!error.empty()) {
					done({ .error = std::move(error) });
					return;
				}
				const auto markup = item->inlineReplyMarkup();
				if (!markup
					|| row >= int64_t(markup->data.rows.size())
					|| column >= int64_t(markup->data.rows[row].size())) {
					done({ .error = fgpl::api::error::kNotFound });
					return;
				}
				using Type = HistoryMessageMarkupButton::Type;
				const auto &button = markup->data.rows[row][column];
				if (button.type == Type::Url) {
					done({ .answer = { .url = button.data.toStdString() } });
					return;
				} else if (button.type != Type::Callback) {
					done({ .error = fgpl::api::error::kInvalid });
					return;
				}
				const auto peer = item->history()->peer;
				const auto session = &peer->session();
				session->api().request(MTPmessages_GetBotCallbackAnswer(
					MTP_flags(MTPmessages_GetBotCallbackAnswer::Flag::f_data),
					peer->input(),
					MTP_int(item->id.bare),
					MTP_bytes(button.data),
					MTPInputCheckPasswordSRP()
				)).done([=](const MTPmessages_BotCallbackAnswer &result) {
					const auto &data = result.data();
					auto answer = fgpl::ButtonAnswer();
					if (const auto message = data.vmessage()) {
						answer.text = message->v.toStdString();
					}
					if (const auto url = data.vurl()) {
						answer.url = url->v.toStdString();
					}
					answer.alert = data.is_alert();
					done({ .answer = std::move(answer) });
				}).fail([=] {
					done({ .error = fgpl::api::error::kFailed });
				}).send();
			});
		}, Pressed{ .error = fgpl::api::error::kUnavailable }, kWaitSend);
		if (!pressed.error.empty()) {
			return pressed.error;
		}
		return pressed.answer;
	}

	fgpl::SendResult sendInlineResult(const fgpl::OutgoingInline &result) override {
		return OnMain<fgpl::SendResult>([=] {
			const auto session = FindSession(_account);
			const auto history = historyOf(result.chat);
			if (!session || !history || HiddenPeer(history->peer)) {
				return fgpl::SendResult{ .error = fgpl::api::error::kNotFound };
			}
			const auto target = ResolveTarget(
				session,
				history,
				MsgId(result.topic),
				MsgId(result.replyTo));
			if (target.missingReply) {
				return fgpl::SendResult{ .error = fgpl::api::error::kNotFound };
			} else if (!Data::CanSendTexts(target.thread)) {
				return fgpl::SendResult{ .error = fgpl::api::error::kDenied };
			} else if (history->peer->slowmodeSecondsLeft() > 0) {
				return fgpl::SendResult{ .error = fgpl::api::error::kLimit };
			}
			auto action = Api::SendAction(target.thread);
			ApplyTarget(action, target);
			const auto sendAs = ChosenSendAs(history);
			using Flag = MTPmessages_SendInlineBotResult::Flag;
			const auto replyTo = action.mtpReplyTo();
			session->api().request(MTPmessages_SendInlineBotResult(
				MTP_flags(Flag()
					| (result.silent ? Flag::f_silent : Flag())
					| (action.replyTo ? Flag::f_reply_to : Flag())
					| (sendAs ? Flag::f_send_as : Flag())),
				history->peer->input(),
				replyTo,
				MTP_long(base::RandomValue<uint64>()),
				MTP_long(result.queryId),
				MTP_string(result.resultId),
				MTP_int(0),
				sendAs ? sendAs->input() : MTP_inputPeerEmpty(),
				MTPInputQuickReplyShortcut(),
				MTP_long(0)
			)).done([=](const MTPUpdates &updates) {
				session->api().applyUpdates(updates);
			}).send();
			return fgpl::SendResult();
		}, fgpl::SendResult{ .error = fgpl::api::error::kUnavailable });
	}

	std::string composerInsert(int64_t chat, const std::string &text) override {
		return OnMain<std::string>([=] {
			const auto history = historyOf(chat);
			if (!history || HiddenPeer(history->peer)) {
				return std::string(fgpl::api::error::kNotFound);
			}
			const auto previous = history->localDraft({}, {});
			auto current = previous ? previous->textWithTags : TextWithTags();
			const auto inserted = QString::fromStdString(text);
			const auto at = previous
				? std::clamp(int(previous->cursor.position), 0, int(current.text.size()))
				: int(current.text.size());
			current.text.insert(at, inserted);
			for (auto &tag : current.tags) {
				if (tag.offset >= at) {
					tag.offset += int(inserted.size());
				} else if (tag.offset + tag.length > at) {
					tag.length += int(inserted.size());
				}
			}
			const auto position = at + int(inserted.size());
			const auto cursor = MessageCursor{ position, position, Ui::kQFixedMax };
			history->setLocalDraft(std::make_unique<Data::Draft>(
				current,
				previous ? previous->reply : FullReplyTo(),
				previous ? previous->suggest : SuggestOptions(),
				cursor,
				Data::WebPageDraft()));
			history->updateChatListSortPosition();
			history->session().changes().historyUpdated(
				history,
				Data::HistoryUpdate::Flag::CloudDraft);
			return std::string();
		}, std::string(fgpl::api::error::kUnavailable));
	}

	std::string react(
			int64_t chat,
			int64_t id,
			const fgpl::ReactionKind &reaction) override {
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
				const auto remove = reaction.emoji.empty()
					&& !reaction.customEmoji
					&& !reaction.paid;
				if (reaction.paid) {
					done(fgpl::api::error::kInvalid);
					return;
				} else if (remove) {
					for (const auto &existing : chosen) {
						if (!existing.paid()) {
							item->toggleReaction(existing, HistoryReactionSource::Existing);
						}
					}
					done({});
					return;
				}
				const auto wanted = reaction.customEmoji
					? Data::ReactionId{ DocumentId(reaction.customEmoji) }
					: Data::ReactionId{ QString::fromStdString(reaction.emoji) };
				if (!ranges::contains(chosen, wanted)) {
					item->toggleReaction(wanted, HistoryReactionSource::Selector);
				}
				done({});
			});
		}, fgpl::api::error::kUnavailable, kWaitSend);
	}

	fgpl::Result<std::vector<fgpl::ReactionEntry>, std::string> reactionsList(
			int64_t chat,
			int64_t id,
			const std::optional<fgpl::ReactionKind> &filter,
			size_t limit) override {
		struct Loaded {
			std::string error;
			std::vector<fgpl::ReactionEntry> list;
		};
		const auto loaded = OnMainAsync<Loaded>([=](Fn<void(Loaded)> done) {
			resolveMessage(chat, id, [=](HistoryItem *item, std::string error) {
				const auto session = FindSession(_account);
				if (!error.empty() || !session) {
					done({ .error = error.empty()
						? std::string(fgpl::api::error::kNotFound)
						: error });
					return;
				} else if (item->isLocal() || item->isService()) {
					done({ .error = fgpl::api::error::kNotFound });
					return;
				}
				using Flag = MTPmessages_GetMessageReactionsList::Flag;
				const auto wanted = filter
					? (filter->customEmoji
						? Data::ReactionId{ DocumentId(filter->customEmoji) }
						: Data::ReactionId{ QString::fromStdString(filter->emoji) })
					: Data::ReactionId();
				session->api().request(MTPmessages_GetMessageReactionsList(
					MTP_flags(filter ? Flag::f_reaction : Flag(0)),
					item->history()->peer->input(),
					MTP_int(item->id),
					filter ? Data::ReactionToMTP(wanted) : MTPReaction(),
					MTPstring(),
					MTP_int(int(limit))
				)).done([=](const MTPmessages_MessageReactionsList &result) {
					const auto &data = result.data();
					session->data().processUsers(data.vusers());
					session->data().processChats(data.vchats());
					auto list = std::vector<fgpl::ReactionEntry>();
					for (const auto &entry : data.vreactions().v) {
						const auto &reaction = entry.data();
						const auto peerId = peerFromMTP(reaction.vpeer_id());
						auto out = fgpl::ReactionEntry();
						out.user = int64_t(peerId.value);
						if (const auto peer = session->data().peerLoaded(peerId)) {
							out.name = Utf8(peer->name());
						}
						out.kind = ReactionKindOf(
							Data::ReactionFromMTP(reaction.vreaction()));
						out.date = reaction.vdate().v;
						list.push_back(std::move(out));
					}
					done({ .list = std::move(list) });
				}).fail([=] {
					done({ .error = fgpl::api::error::kFailed });
				}).send();
			});
		}, Loaded{ .error = fgpl::api::error::kUnavailable });
		if (!loaded.error.empty()) {
			return loaded.error;
		}
		return loaded.list;
	}

	fgpl::SendResult sendSticker(
			int64_t chat,
			int64_t id,
			int64_t replyTo,
			int64_t topic,
			bool silent) override {
		return OnMain<fgpl::SendResult>([=]() -> fgpl::SendResult {
			const auto session = FindSession(_account);
			const auto history = historyOf(chat);
			const auto document = findSticker(session, DocumentId(id));
			if (!session || !history || !document
				|| HiddenPeer(history->peer)) {
				return { .error = fgpl::api::error::kNotFound };
			}
			const auto target = ResolveTarget(
				session,
				history,
				MsgId(topic),
				MsgId(replyTo));
			if (target.missingReply) {
				return { .error = fgpl::api::error::kNotFound };
			} else if (!Data::CanSend(target.thread, ChatRestriction::SendStickers)
				|| history->peer->starsPerMessageChecked() > 0) {
				return { .error = fgpl::api::error::kDenied };
			} else if (history->peer->slowmodeSecondsLeft() > 0) {
				return { .error = fgpl::api::error::kLimit };
			}
			auto action = Api::SendAction(target.thread);
			action.clearDraft = false;
			action.options.silent = silent;
			action.options.sendAs = ChosenSendAs(history);
			ApplyTarget(action, target);
			const auto localId = session->data().nextLocalMessageId();
			const auto full = FullMsgId(history->peer->id, localId);
			MarkPluginMessage(session, full);
			++PluginSendDepth;
			Api::SendExistingDocument(
				Api::MessageToSend(action),
				not_null<DocumentData*>(document),
				localId);
			--PluginSendDepth;
			auto result = fgpl::SendResult();
			if (session->data().message(full) || HasPendingOutgoing(history)) {
				result.localId = localId.bare;
			}
			return result;
		}, fgpl::SendResult{ .error = fgpl::api::error::kUnavailable });
	}

	std::vector<fgpl::StickerInfo> stickers(
			const std::string &emoji,
			size_t limit) override {
		return OnMain<std::vector<fgpl::StickerInfo>>([=] {
			auto result = std::vector<fgpl::StickerInfo>();
			const auto session = FindSession(_account);
			if (!session) {
				return result;
			}
			auto seen = base::flat_set<DocumentId>();
			const auto add = [&](DocumentData *document) {
				if (document
					&& document->sticker()
					&& result.size() < limit
					&& !seen.contains(document->id)) {
					seen.insert(document->id);
					result.push_back(StickerInfoOf(document));
				}
			};
			const auto &stickers = session->data().stickers();
			if (emoji.empty()) {
				for (const auto &recent : stickers.getRecentPack()) {
					add(recent.first);
				}
				return result;
			}
			const auto wanted = Ui::Emoji::Find(
				QString::fromStdString(emoji));
			if (!wanted) {
				return result;
			}
			for (const auto setId : stickers.setsOrder()) {
				const auto &sets = stickers.sets();
				const auto i = sets.find(setId);
				if (i == sets.end()) {
					continue;
				}
				const auto j = i->second->emoji.find(wanted->original());
				if (j != i->second->emoji.end()) {
					for (const auto document : j->second) {
						add(document);
					}
				}
			}
			return result;
		}, {});
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
				|| HiddenPeer(target->peer)) {
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
			if (!history || HiddenPeer(history->peer)) {
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
				|| HiddenPeer(history->peer)) {
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
				|| HiddenPeer(history->peer)) {
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
			auto result = history(chat, kMaxHistory, before, 0, 0);
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
				|| HiddenPeer(history->peer)) {
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
			return peer ? UserInfoOf(peer) : std::nullopt;
		}, std::nullopt);
	}

	fgpl::Result<std::vector<fgpl::InlineResult>, std::string> inlineQuery(
			int64_t bot,
			const std::string &username,
			const std::string &query,
			int64_t chat) override {
		struct Answered {
			std::string error;
			std::vector<fgpl::InlineResult> results;
		};
		const auto answered = OnMainAsync<Answered>([=](Fn<void(Answered)> done) {
			const auto session = FindSession(_account);
			if (!session) {
				done({ .error = fgpl::api::error::kNotFound });
				return;
			}
			const auto ask = [=](PeerData *peer) {
				const auto user = peer ? peer->asUser() : nullptr;
				if (!user || HiddenPeer(user) || !user->isBot() || user->isBlocked()) {
					done({ .error = fgpl::api::error::kNotFound });
					return;
				} else if (user->botInfo && user->botInfo->inlinePlaceholder.isEmpty()) {
					done({ .error = fgpl::api::error::kFailed });
					return;
				}
				const auto context = chat
					? session->data().peerLoaded(PeerId(uint64(chat)))
					: nullptr;
				session->api().request(MTPmessages_GetInlineBotResults(
					MTP_flags(0),
					user->inputUser(),
					(context && !HiddenPeer(context))
						? context->input()
						: MTP_inputPeerEmpty(),
					MTPInputGeoPoint(),
					MTP_string(QString::fromStdString(query)),
					MTP_string()
				)).done([=](const MTPmessages_BotResults &result) {
					const auto &data = result.data();
					session->data().processUsers(data.vusers());
					auto answer = Answered();
					for (const auto &entry : data.vresults().v) {
						auto item = InlineResultOf(entry);
						item.queryId = data.vquery_id().v;
						answer.results.push_back(std::move(item));
					}
					done(std::move(answer));
				}).fail([=] {
					done({ .error = fgpl::api::error::kFailed });
				}).send();
			};
			if (bot > 0) {
				ask(session->data().userLoaded(UserId(bot)));
				return;
			}
			const auto name = QString::fromStdString(username);
			if (const auto known = session->data().peerByUsername(name)) {
				ask(known);
				return;
			}
			session->api().request(MTPcontacts_ResolveUsername(
				MTP_flags(0),
				MTP_string(name),
				MTP_string()
			)).done([=](const MTPcontacts_ResolvedPeer &result) {
				const auto &data = result.data();
				session->data().processUsers(data.vusers());
				session->data().processChats(data.vchats());
				ask(session->data().peerLoaded(peerFromMTP(data.vpeer())));
			}).fail([=] {
				done({ .error = fgpl::api::error::kNotFound });
			}).send();
		}, Answered{ .error = fgpl::api::error::kUnavailable });
		if (!answered.error.empty()) {
			return answered.error;
		}
		return answered.results;
	}

	fgpl::Result<fgpl::UserInfo, std::string> resolveUser(
			const std::string &username) override {
		struct Resolved {
			std::string error;
			fgpl::UserInfo user;
		};
		const auto resolved = OnMainAsync<Resolved>([=](Fn<void(Resolved)> done) {
			const auto session = FindSession(_account);
			if (!session) {
				done({ .error = fgpl::api::error::kNotFound });
				return;
			}
			const auto name = QString::fromStdString(username);
			const auto finish = [=](PeerData *peer) {
				const auto info = peer ? UserInfoOf(peer) : std::nullopt;
				if (info) {
					done({ .user = *info });
				} else {
					done({ .error = fgpl::api::error::kNotFound });
				}
			};
			if (const auto known = session->data().peerByUsername(name)) {
				finish(known);
				return;
			}
			session->api().request(MTPcontacts_ResolveUsername(
				MTP_flags(0),
				MTP_string(name),
				MTP_string()
			)).done([=](const MTPcontacts_ResolvedPeer &result) {
				const auto &data = result.data();
				session->data().processUsers(data.vusers());
				session->data().processChats(data.vchats());
				finish(session->data().peerLoaded(peerFromMTP(data.vpeer())));
			}).fail([=] {
				done({ .error = fgpl::api::error::kNotFound });
			}).send();
		}, Resolved{ .error = fgpl::api::error::kUnavailable });
		if (!resolved.error.empty()) {
			return resolved.error;
		}
		return resolved.user;
	}

	fgpl::Result<fgpl::Media, std::string> peerPhoto(
			int64_t id,
			bool channel,
			size_t maxBytes) override {
		struct Loaded {
			std::string error;
			QByteArray bytes;
			bool video = false;
		};
		const auto loaded = OnMainAsync<Loaded>([=](Fn<void(Loaded)> done) {
			const auto session = FindSession(_account);
			const auto peer = session
				? session->data().peerLoaded(PeerId(uint64(id)))
				: nullptr;
			const auto user = (peer && !channel) ? peer->asUser() : nullptr;
			const auto chan = (peer && channel) ? peer->asChannel() : nullptr;
			if (!(user || chan)
				|| FreshSecret::IsSecretPeerId(peer->id)
				|| !peer->hasUserpic()) {
				done({ .error = fgpl::api::error::kNotFound });
				return;
			}
			const auto view = std::make_shared<Ui::PeerUserpicView>(
				peer->createUserpicView());
			const auto lifetime = std::make_shared<rpl::lifetime>();
			const auto finished = std::make_shared<bool>(false);
			const auto finish = [=](Loaded value) {
				if (!*finished) {
					*finished = true;
					lifetime->destroy();
					done(std::move(value));
				}
			};
			const auto watch = [=](Fn<bool()> check) {
				if (check()) {
					return;
				}
				session->downloaderTaskFinished(
				) | rpl::on_next([=] {
					check();
				}, *lifetime);
			};
			const auto small = [=] {
				if (const auto image = peer->userpicCloudImage(*view)) {
					finish({ .bytes = Jpeg(*image) });
					return true;
				}
				return false;
			};
			const auto large = [=](not_null<PhotoData*> photo) {
				const auto media = photo->createMediaView();
				const auto origin = user
					? Data::FileOrigin(Data::FileOriginUserPhoto(
						peerToUser(user->id),
						photo->id))
					: Data::FileOrigin(Data::FileOriginPeerPhoto(peer->id));
				const auto size = Data::PhotoSize::Large;
				const auto video = photo->hasVideo();
				if (video) {
					media->videoWanted(size, origin);
				} else {
					media->wanted(size, origin);
				}
				watch([=] {
					if (*finished) {
						return true;
					} else if (video) {
						const auto content = media->videoContent(size);
						if (!content.isEmpty()) {
							finish({ .bytes = content, .video = true });
							return true;
						} else if (photo->videoFailed(size)) {
							return small();
						}
					} else if (const auto image = media->image(size)) {
						const auto bytes = media->imageBytes(size);
						finish({ .bytes = bytes.isEmpty()
							? Jpeg(image->original())
							: bytes });
						return true;
					} else if (photo->failed(size)) {
						return small();
					}
					return false;
				});
			};
			peer->loadUserpic();
			if (chan) {
				const auto fetch = [=] {
					const auto photo = session->data().photo(
						chan->userpicPhotoId());
					if (photo->isNull()) {
						watch(small);
					} else {
						large(photo);
					}
				};
				if (chan->wasFullUpdated()) {
					fetch();
				} else {
					session->changes().peerUpdates(
						chan,
						Data::PeerUpdate::Flag::FullInfo
					) | rpl::take(1) | rpl::on_next(fetch, *lifetime);
					session->api().requestFullPeer(chan);
				}
			} else {
				session->api().request(MTPphotos_GetUserPhotos(
					user->inputUser(),
					MTP_int(0),
					MTP_long(0),
					MTP_int(1)
				)).done([=](const MTPphotos_Photos &result) {
					const auto photo = result.match([&](const auto &data) -> PhotoData* {
						session->data().processUsers(data.vusers());
						if (data.vphotos().v.isEmpty()) {
							return nullptr;
						}
						const auto processed = session->data().processPhoto(
							data.vphotos().v.front());
						return processed->isNull() ? nullptr : processed.get();
					});
					if (photo) {
						large(photo);
					} else {
						watch(small);
					}
				}).fail([=] {
					watch(small);
				}).send();
			}
			base::call_delayed(
				std::chrono::duration_cast<std::chrono::milliseconds>(
					kWaitPhoto).count(),
				[=] {
					if (!small()) {
						finish({ .error = fgpl::api::error::kFailed });
					}
				});
		}, Loaded{ .error = fgpl::api::error::kUnavailable }, kWaitPhoto + kWaitServer);
		if (!loaded.error.empty()) {
			return loaded.error;
		} else if (loaded.bytes.isEmpty()) {
			return std::string(fgpl::api::error::kFailed);
		} else if (size_t(loaded.bytes.size()) > maxBytes) {
			return std::string(fgpl::api::error::kLimit);
		}
		auto result = fgpl::Media();
		result.kind = loaded.video ? "video" : "photo";
		result.mime = loaded.video ? "video/mp4" : "image/jpeg";
		result.bytes.assign(
			loaded.bytes.constData(),
			loaded.bytes.constData() + loaded.bytes.size());
		return result;
	}

	fgpl::Result<fgpl::ChannelInfo, std::string> channel(int64_t id) override {
		return channelOf([=](not_null<Main::Session*> session, Fn<void(PeerData*)> found) {
			found(session->data().peerLoaded(PeerId(uint64(id))));
		});
	}

	fgpl::Result<fgpl::ChannelInfo, std::string> resolveChannel(
			const std::string &username) override {
		return channelOf([=](not_null<Main::Session*> session, Fn<void(PeerData*)> found) {
			const auto name = QString::fromStdString(username);
			if (const auto known = session->data().peerByUsername(name)) {
				found(known);
				return;
			}
			session->api().request(MTPcontacts_ResolveUsername(
				MTP_flags(0),
				MTP_string(name),
				MTP_string()
			)).done([=](const MTPcontacts_ResolvedPeer &result) {
				const auto &data = result.data();
				session->data().processUsers(data.vusers());
				session->data().processChats(data.vchats());
				found(session->data().peerLoaded(peerFromMTP(data.vpeer())));
			}).fail([=] {
				found(nullptr);
			}).send();
		});
	}

	template <typename Find>
	fgpl::Result<fgpl::ChannelInfo, std::string> channelOf(Find find) {
		struct Resolved {
			std::string error;
			fgpl::ChannelInfo channel;
		};
		const auto resolved = OnMainAsync<Resolved>([=](Fn<void(Resolved)> done) {
			const auto session = FindSession(_account);
			if (!session) {
				done({ .error = fgpl::api::error::kNotFound });
				return;
			}
			find(not_null<Main::Session*>(session), [=](PeerData *peer) {
				const auto channel = (peer && peer->asChannel())
					? peer->asChannel()
					: nullptr;
				const auto once = std::make_shared<bool>(false);
				const auto finish = [=] {
					if (*once) {
						return;
					}
					*once = true;
					const auto info = ChannelInfoOf(channel);
					if (info) {
						done({ .channel = *info });
					} else {
						done({ .error = fgpl::api::error::kNotFound });
					}
				};
				if (!channel) {
					done({ .error = fgpl::api::error::kNotFound });
				} else if (channel->wasFullUpdated()) {
					finish();
				} else {
					const auto lifetime = std::make_shared<rpl::lifetime>();
					session->changes().peerUpdates(
						channel,
						Data::PeerUpdate::Flag::FullInfo
					) | rpl::take(1) | rpl::on_next([=] {
						lifetime->destroy();
						finish();
					}, *lifetime);
					session->api().requestFullPeer(channel);
					base::call_delayed(3000, [=] {
						lifetime->destroy();
						finish();
					});
				}
			});
		}, Resolved{ .error = fgpl::api::error::kUnavailable });
		if (!resolved.error.empty()) {
			return resolved.error;
		}
		return resolved.channel;
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
					if (history && !HiddenPeer(history->peer)) {
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
			if (!session || HiddenPeerId(session, peerId)) {
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
	[[nodiscard]] DocumentData *findSticker(
			Main::Session *session,
			DocumentId id) const {
		if (!session) {
			return nullptr;
		}
		const auto &stickers = session->data().stickers();
		for (const auto &recent : stickers.getRecentPack()) {
			if (recent.first->id == id) {
				return recent.first;
			}
		}
		for (const auto setId : stickers.setsOrder()) {
			const auto i = stickers.sets().find(setId);
			if (i == stickers.sets().end()) {
				continue;
			}
			for (const auto document : i->second->stickers) {
				if (document->id == id) {
					return document;
				}
			}
		}
		return nullptr;
	}

	[[nodiscard]] fgpl::SendResult startMessage(
			const fgpl::OutgoingMessage &outgoing) {
		const auto session = FindSession(_account);
		const auto history = historyOf(outgoing.chat);
		if (!session || !history
			|| HiddenPeer(history->peer)) {
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
		auto text = Formatted(outgoing.text, outgoing.markdown);
		const auto size = int(text.text.size());
		for (const auto &emoji : outgoing.customEmoji) {
			if (emoji.offset + emoji.length > size) {
				return { .error = fgpl::api::error::kInvalid };
			}
			text.entities.push_back(EntityInText(
				EntityType::CustomEmoji,
				int(emoji.offset),
				int(emoji.length),
				Data::SerializeCustomEmojiId(DocumentId(emoji.id))));
		}
		for (auto &entity : EntitiesToApp(session, text.text, outgoing.entities)) {
			text.entities.push_back(std::move(entity));
		}
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
			|| HiddenPeer(history->peer)) {
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
		if (!session || HiddenPeerId(session, peerId)) {
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

	template <typename Request>
	[[nodiscard]] fgpl::Result<std::vector<fgpl::Message>, std::string> listFromServer(
			int64_t chat,
			Request request) {
		struct Listed {
			std::string error;
			std::vector<fgpl::Message> list;
		};
		const auto listed = OnMainAsync<Listed>([=](Fn<void(Listed)> done) {
			const auto session = FindSession(_account);
			const auto peer = session
				? session->data().peerLoaded(PeerId(uint64(chat)))
				: nullptr;
			if (!peer || HiddenPeer(peer)) {
				done({ .error = fgpl::api::error::kNotFound });
				return;
			}
			request(session, peer, [=](const MTPmessages_Messages &answer) {
				done({ .list = MessagesOf(answer, chat) });
			}, [=] {
				done({ .error = fgpl::api::error::kFailed });
			});
		}, Listed{ .error = fgpl::api::error::kUnavailable });
		if (!listed.error.empty()) {
			return listed.error;
		}
		return listed.list;
	}

	[[nodiscard]] HistoryItem *messageOf(int64_t chat, int64_t id) const {
		const auto session = FindSession(_account);
		const auto peerId = PeerId(uint64(chat));
		return (session && !HiddenPeerId(session, peerId))
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

std::vector<fgpl::TextEntity> EntitiesOf(const TextWithTags &text) {
	return EntitiesOfList(TextUtilities::ConvertTextTagsToEntities(text.tags));
}

TextWithTags TextWithEntitiesOf(
		Main::Session *session,
		const QString &text,
		const std::vector<fgpl::TextEntity> &entities) {
	return TextWithTags{
		text,
		TextUtilities::ConvertEntitiesToTextTags(
			EntitiesToApp(session, text, entities)),
	};
}

namespace {

struct PluginHidden {
	base::flat_set<FullMsgId> hidden;
	base::flat_set<FullMsgId> pending;
};

[[nodiscard]] PluginHidden &PluginHiddenState() {
	static auto result = PluginHidden();
	return result;
}

} // namespace

bool IsHiddenByPlugin(not_null<const HistoryItem*> item) {
	const auto &state = PluginHiddenState();
	const auto id = item->fullId();
	return state.hidden.contains(id) || state.pending.contains(id);
}

void SetHiddenByPlugin(not_null<HistoryItem*> item, bool hidden, bool pending) {
	auto &state = PluginHiddenState();
	auto items = std::vector<not_null<HistoryItem*>>{ item };
	if (const auto group = item->history()->owner().groups().find(item)) {
		for (const auto other : group->items) {
			if (other != item) {
				items.push_back(other);
			}
		}
	}
	for (const auto &one : items) {
		auto &set = pending ? state.pending : state.hidden;
		if (hidden) {
			set.emplace(one->fullId());
		} else {
			set.remove(one->fullId());
			if (!pending) {
				state.pending.remove(one->fullId());
			}
		}
		one->invalidateChatListEntry();
	}
	FiltersCacheController::fireUpdate();
}

void ForgetHiddenByPlugin(FullMsgId id) {
	auto &state = PluginHiddenState();
	state.hidden.remove(id);
	state.pending.remove(id);
}

void ReleasePendingHidden() {
	auto &state = PluginHiddenState();
	if (state.pending.empty()) {
		return;
	}
	state.pending.clear();
	FiltersCacheController::fireUpdate();
}

fgpl::IncomingInfo IncomingInfoOf(not_null<HistoryItem*> item, bool edited) {
	auto result = fgpl::IncomingInfo();
	const auto peer = item->history()->peer;
	auto message = MessageOf(item);
	result.chat = ChatId(peer);
	result.secret = FreshSecret::IsSecretPeerId(peer->id);
	result.chatType = PeerKind(peer);
	result.edited = edited;
	result.text = message.text;
	result.entities = message.entities;
	const auto value = fgpl::MessageValue(message);
	if (const auto map = value.asMap()) {
		result.message = *map;
	}
	return result;
}

TextWithEntities DisplayTextOf(
		Main::Session *session,
		const QString &text,
		const std::vector<fgpl::TextEntity> &entities) {
	return TextWithEntities{ text, EntitiesToApp(session, text, entities) };
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
		{ "from_kind", Value::FromText(KindOf(item->from())) },
		{ "sender_name", Value::FromText(Utf8(item->from()->name())) },
		{ "date", Value::FromInt(message.date) },
		{ "text", Value::FromText(message.text) },
		{ "out", Value::FromBool(item->out()) },
		{ "forwarded", Value::FromBool(item->Has<HistoryMessageForwarded>()) },
		{ "secret", Value::FromBool(FreshSecret::IsSecretPeerId(peer->id)) },
		{ "self", Value::FromBool(peer->isSelf()) },
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
	if (message.sticker) {
		map.push_back({ "sticker", Value::FromMap({
			{ "id", Value::FromInt(message.sticker->id) },
			{ "emoji", Value::FromText(message.sticker->emoji) },
			{ "set", Value::FromText(message.sticker->set) },
			{ "animated", Value::FromBool(message.sticker->animated) },
			{ "video", Value::FromBool(message.sticker->video) },
		}) });
	}
	if (message.emojiOnly) {
		map.push_back({ "emoji_only", Value::FromBool(true) });
	}
	if (!message.entities.empty()) {
		map.push_back({ "entities", fgpl::TextEntitiesValue(message.entities) });
	}
	if (!message.reactions.empty()) {
		auto reactions = fgpl::cbor::Array();
		for (const auto &reaction : message.reactions) {
			auto entry = fgpl::cbor::Map();
			if (reaction.kind.paid) {
				entry.push_back({ "paid", Value::FromBool(true) });
			} else if (reaction.kind.customEmoji) {
				entry.push_back({ "custom_emoji", Value::FromInt(reaction.kind.customEmoji) });
			} else {
				entry.push_back({ "emoji", Value::FromText(reaction.kind.emoji) });
			}
			entry.push_back({ "count", Value::FromInt(reaction.count) });
			entry.push_back({ "my", Value::FromBool(reaction.my) });
			reactions.push_back(Value::FromMap(std::move(entry)));
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

bool IsHiddenByPlugin(not_null<const HistoryItem*>) {
	return false;
}

void SetHiddenByPlugin(not_null<HistoryItem*>, bool, bool) {
}

void ForgetHiddenByPlugin(FullMsgId) {
}

void ReleasePendingHidden() {
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
