#include "ayu/secret/secret_manager.h"
#include "ayu/secret/secret_bridge.h"
#include "ayu/secret/secret_vault.h"

#include "apiwrap.h"
#include "core/application.h"
#include "window/window_controller.h"
#include "ayu/ayu_settings.h"
#include "ayu/data/ayu_database.h"
#include "ayu/secret/secret_crypto.h"
#include "ayu/secret/secret_files.h"
#include "ayu/secret/secret_protocol.h"

#include "ayu/secret/secret_rekey.h"
#include "ayu/secret/secret_tl.h"
#include "base/call_delayed.h"
#include "base/openssl_help.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "lang/lang_keys.h"
#include <crl/crl_async.h>
#include <crl/crl_on_main.h>
#include "crl/crl_time.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "main/main_session.h"
#include "mtproto/facade.h"
#include "mtproto/mtproto_dh_utils.h"
#include "ui/toast/toast.h"

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeDatabase>
#include <QtGui/QImage>
#include <QtGui/QImageReader>

#include <array>
#include <cstring>
#include <map>
#include <algorithm>
#include <deque>
#include <limits>
#include <set>

namespace AyuSecret {
namespace {

constexpr auto kExtrasMagic = 0x31435941U;
constexpr auto kChatRowMagic = 0x32524359U;
constexpr auto kPayloadMagic = 0x32595041U;
constexpr auto kPartSize = 512 * 1024;
constexpr auto kBigFileSize = 10 * 1024 * 1024;
constexpr auto kMaxFileSize = int64(100) * 1024 * 1024;
constexpr auto kDownloadChunk = 128 * 1024;
constexpr auto kTypingTimeout = crl::time(6000);
constexpr auto kTypingSendEvery = crl::time(5000);
constexpr auto kAutoDownloadLimit = 10 * 1024 * 1024;
constexpr auto kMaxPending = 2000;
constexpr auto kMaxEarly = 500;
// Perfect forward secrecy rotation trigger, see
// https://core.telegram.org/api/end-to-end/pfs : replace the key once it
// has been used to encrypt and decrypt more than 100 messages, or once it
// is older than a week provided it encrypted at least one message.
constexpr auto kRekeyAfterUses = 100;
constexpr auto kRekeyAfterTime = 7 * 86400;
constexpr auto kSpecialNote = 1;
constexpr auto kSpecialHidden = 2;
constexpr auto kKindHidden = 100;
constexpr auto kKindNote = 101;
constexpr auto kKindEnded = 102;
constexpr auto kKindRequest = 103;

[[nodiscard]] Bytes FromArray(const QByteArray &data) {
	return Bytes(
		reinterpret_cast<const uint8_t*>(data.constData()),
		reinterpret_cast<const uint8_t*>(data.constData()) + data.size());
}

[[nodiscard]] QByteArray ToArray(const Bytes &data) {
	return QByteArray(
		reinterpret_cast<const char*>(data.data()),
		int(data.size()));
}

[[nodiscard]] Bytes FromChars(const std::vector<char> &data) {
	return Bytes(data.begin(), data.end());
}

[[nodiscard]] std::vector<char> ToChars(const Bytes &data) {
	return std::vector<char>(data.begin(), data.end());
}

[[nodiscard]] bytes::const_span Span(const Bytes &data) {
	return bytes::const_span(
		reinterpret_cast<const gsl::byte*>(data.data()),
		data.size());
}

[[nodiscard]] Bytes FromBytesVector(const bytes::vector &data) {
	return Bytes(
		reinterpret_cast<const uint8_t*>(data.data()),
		reinterpret_cast<const uint8_t*>(data.data()) + data.size());
}

[[nodiscard]] int64_t RandomId() {
	auto result = int64_t(0);
	while (!result) {
		RandomBytes(reinterpret_cast<uint8_t*>(&result), sizeof(result));
	}
	return result;
}

[[nodiscard]] QString Qs(const std::string &value) {
	return QString::fromStdString(value);
}

[[nodiscard]] int ValidDc(int dcId) {
	return (dcId >= 1 && dcId <= 5) ? dcId : 0;
}

struct DhConfig {
	int g = 0;
	Bytes p;
	Bytes random;
};

struct Transfer {
	double progress = 0.;
};

enum class DeleteOrigin {
	Remote,
	User,
	Expired,
};

struct PendingEntry {
	Inbound inbound;
	int date = 0;
	std::optional<MTPEncryptedFile> file;
};

struct EarlyEntry {
	int date = 0;
	QByteArray bytes;
	std::optional<MTPEncryptedFile> file;
};

struct ReadyPacket {
	int date = 0;
	QByteArray bytes;
	bool decrypted = false;
	bool parsed = false;
	Inbound inbound;
	std::optional<MTPEncryptedFile> file;
};

[[nodiscard]] ReadyPacket PreparePacket(
		const std::array<Bytes, 3> &keys,
		bool creator,
		ReadyPacket packet) {
	const auto raw = FromArray(packet.bytes);
	int64_t fingerprint = 0;
	if (raw.size() >= 8) {
		std::memcpy(&fingerprint, raw.data(), 8);
	}
	auto object = Bytes();
	packet.decrypted = false;
	packet.parsed = false;
	for (const auto &key : keys) {
		if (!key.empty() && KeyFingerprint(key) == fingerprint) {
			packet.decrypted = DecryptPacket(key, creator, raw, object);
			break;
		}
	}
	if (packet.decrypted) {
		packet.parsed = ParseLayerObject(object, packet.inbound);
	}
	return packet;
}

struct Chat {
	SecretChatRow row;
	Bytes key;
	Bytes otherKey;
	int ttl = 0;
	int64_t pfsExchange = 0;
	Bytes pfsPending;
	// Our last answer to the inbound exchange (g_b sent in AcceptKey),
	// to retransmit it when the peer's RequestKey is duplicated.
	Bytes pfsAnswerGB;
	// Exchange initiated by us: id, our secret power, prime in use.
	// Kept in memory only: after a restart an uncompleted outgoing
	// exchange is treated as aborted (the spec allows aborting any
	// uncompleted instance by a party that sent neither AcceptKey nor
	// CommitKey for it).
	int64_t pfsOurExchange = 0;
	Bytes pfsOurSecret;
	Bytes pfsOurP;
	// Key usage accounting for rotation triggers.
	int keyInstalledAt = 0;
	int keyUsesOut = 0;
	int keyUsesIn = 0;
	bool loaded = false;
	bool locked = false;
	bool working = false;
	int resendRequestedTill = -1;
	int revision = 0;
	int lastReadSent = 0;
	crl::time typingUntil = 0;
	crl::time typingSent = 0;
	std::vector<MessageData> messages;
	std::map<int, PendingEntry> pending;
	std::vector<EarlyEntry> early;
	std::map<int64_t, Transfer> transfers;
	int nextTicket = 0;
	int nextApply = 0;
	std::map<int, ReadyPacket> ready;
};

[[nodiscard]] std::vector<char> EncodeExtras(const Chat &chat) {
	auto writer = Writer();
	writer.writeUInt(kExtrasMagic);
	writer.writeBytes(chat.key.data(), chat.key.size());
	writer.writeBytes(chat.otherKey.data(), chat.otherKey.size());
	writer.writeInt(chat.ttl);
	writer.writeLong(chat.pfsExchange);
	writer.writeBytes(chat.pfsPending.data(), chat.pfsPending.size());
	writer.writeInt(chat.keyInstalledAt);
	writer.writeInt(chat.keyUsesOut);
	writer.writeInt(chat.keyUsesIn);
	return ToChars(writer.data());
}

[[nodiscard]] std::string ChatContext(ID userId, int chatId) {
	return "chat:" + std::to_string(userId) + ":" + std::to_string(chatId);
}

[[nodiscard]] std::string MessageContext(
		ID userId,
		int chatId,
		int64_t randomId) {
	return "msg:" + std::to_string(userId)
		+ ":" + std::to_string(chatId)
		+ ":" + std::to_string(randomId);
}

[[nodiscard]] std::string FileContext(
		ID userId,
		int chatId,
		int64_t randomId) {
	return "file:" + std::to_string(userId)
		+ ":" + std::to_string(chatId)
		+ ":" + std::to_string(randomId);
}

enum class RowState {
	Failed,
	Legacy,
	Current,
};

[[nodiscard]] bool SealChatRow(SecretChatRow &row) {
	auto writer = Writer();
	writer.writeUInt(kChatRowMagic);
	writer.writeLong(row.accessHash);
	writer.writeLong(row.peerUserId);
	writer.writeInt(row.creator);
	writer.writeInt(row.state);
	writer.writeLong(row.fingerprint);
	writer.writeInt(row.myIn);
	writer.writeInt(row.myOut);
	writer.writeInt(row.hisIn);
	writer.writeInt(row.hisLayer);
	writer.writeInt(row.date);
	writer.writeInt(row.lastDate);
	writer.writeInt(row.unread);
	writer.writeBytes(
		reinterpret_cast<const uint8_t*>(row.keyData.data()),
		row.keyData.size());
	const auto sealed = Vault::Seal(
		writer.data(),
		ChatContext(row.userId, row.chatId));
	if (sealed.empty()) {
		return false;
	}
	row.accessHash = 0;
	row.peerUserId = 0;
	row.creator = 0;
	row.state = 0;
	row.fingerprint = 0;
	row.myIn = 0;
	row.myOut = 0;
	row.hisIn = 0;
	row.hisLayer = 0;
	row.date = 0;
	row.lastDate = 0;
	row.unread = 0;
	row.keyData = ToChars(sealed);
	return true;
}

[[nodiscard]] RowState OpenChatRow(SecretChatRow &row) {
	const auto raw = FromChars(row.keyData);
	auto plain = Bytes();
	if (!Vault::IsBound(raw)) {
		if (Vault::IsSealed(raw)) {
			if (!Vault::Open(raw, plain, std::string())) {
				return RowState::Failed;
			}
			row.keyData = ToChars(plain);
		}
		return RowState::Legacy;
	} else if (!Vault::Open(
			raw,
			plain,
			ChatContext(row.userId, row.chatId))) {
		return RowState::Failed;
	}
	auto reader = Reader(plain.data(), plain.size());
	if (reader.readUInt() != kChatRowMagic) {
		return RowState::Failed;
	}
	row.accessHash = reader.readLong();
	row.peerUserId = reader.readLong();
	row.creator = reader.readInt();
	row.state = reader.readInt();
	row.fingerprint = reader.readLong();
	row.myIn = reader.readInt();
	row.myOut = reader.readInt();
	row.hisIn = reader.readInt();
	row.hisLayer = reader.readInt();
	row.date = reader.readInt();
	row.lastDate = reader.readInt();
	row.unread = reader.readInt();
	const auto inner = reader.readBytes();
	if (reader.failed()) {
		return RowState::Failed;
	}
	row.keyData = ToChars(Bytes(inner.begin(), inner.end()));
	return RowState::Current;
}

[[nodiscard]] std::vector<char> PackPayload(
		ID userId,
		int chatId,
		const MessageData &data,
		int kind) {
	auto writer = Writer();
	writer.writeUInt(kPayloadMagic);
	writer.writeInt(data.outgoing ? 1 : 0);
	writer.writeInt(data.date);
	writer.writeInt(kind);
	writer.writeInt(data.seqIn);
	writer.writeInt(data.seqOut);
	const auto meta = SerializeMeta(data);
	writer.writeBytes(meta.data(), meta.size());
	writer.writeString(data.text);
	return ToChars(Vault::Seal(
		writer.data(),
		MessageContext(userId, chatId, data.randomId)));
}

[[nodiscard]] RowState UnpackRow(
		ID userId,
		const SecretMessageRow &row,
		MessageData &data,
		int &kind) {
	const auto raw = FromChars(row.payload);
	data.randomId = row.randomId;
	auto plain = Bytes();
	if (Vault::IsBound(raw)) {
		if (!Vault::Open(
				raw,
				plain,
				MessageContext(userId, row.chatId, row.randomId))) {
			return RowState::Failed;
		}
		auto reader = Reader(plain.data(), plain.size());
		if (reader.readUInt() != kPayloadMagic) {
			return RowState::Failed;
		}
		data.outgoing = (reader.readInt() != 0);
		data.date = reader.readInt();
		kind = reader.readInt();
		data.seqIn = reader.readInt();
		data.seqOut = reader.readInt();
		const auto meta = reader.readBytes();
		const auto text = reader.readString();
		if (reader.failed()) {
			return RowState::Failed;
		}
		ParseMeta(meta, data);
		data.text = text;
		return RowState::Current;
	}
	data.outgoing = (row.outgoing != 0);
	data.date = row.date;
	kind = row.kind;
	data.seqIn = row.seqIn;
	data.seqOut = row.seqOut;
	if (!Vault::IsSealed(raw)) {
		ParseMeta(raw, data);
		data.text = row.text;
		return RowState::Legacy;
	} else if (!Vault::Open(raw, plain, std::string())) {
		return RowState::Failed;
	}
	auto reader = Reader(plain.data(), plain.size());
	const auto meta = reader.readBytes();
	const auto text = reader.readString();
	if (reader.failed()) {
		return RowState::Failed;
	}
	ParseMeta(meta, data);
	data.text = text;
	return RowState::Legacy;
}

void DecodeExtras(Chat &chat) {
	const auto raw = FromChars(chat.row.keyData);
	if (raw.size() == kKeySize) {
		chat.key = raw;
		return;
	}
	auto reader = Reader(raw.data(), raw.size());
	if (reader.readUInt() != kExtrasMagic) {
		return;
	}
	const auto key = reader.readBytes();
	const auto other = reader.readBytes();
	const auto ttl = reader.readInt();
	const auto exchange = reader.readLong();
	const auto pending = reader.readBytes();
	if (reader.failed()) {
		// Truncated rows must never leave a half-set exchange behind:
		// pfsExchange without pfsPending would block rotation forever.
		chat.pfsExchange = 0;
		chat.pfsPending.clear();
		return;
	}
	chat.ttl = ttl;
	chat.pfsExchange = exchange;
	chat.key = Bytes(key.begin(), key.end());
	chat.otherKey = Bytes(other.begin(), other.end());
	chat.pfsPending = Bytes(pending.begin(), pending.end());
	// Rotation accounting, absent in rows written by older versions.
	// Assume a legacy key is both used and ancient, so that it gets
	// replaced once shortly after the upgrade.
	chat.keyInstalledAt = 0;
	chat.keyUsesOut = 1;
	chat.keyUsesIn = 0;
	const auto installedAt = reader.readInt();
	const auto usesOut = reader.readInt();
	const auto usesIn = reader.readInt();
	if (!reader.failed()) {
		chat.keyInstalledAt = installedAt;
		chat.keyUsesOut = usesOut;
		chat.keyUsesIn = usesIn;
	}
}

[[nodiscard]] int EntityTypeFromText(::EntityType type) {
	switch (type) {
	case ::EntityType::Bold:
	case ::EntityType::Semibold: return int(AyuSecret::EntityType::Bold);
	case ::EntityType::Italic: return int(AyuSecret::EntityType::Italic);
	case ::EntityType::Code: return int(AyuSecret::EntityType::Code);
	case ::EntityType::Pre: return int(AyuSecret::EntityType::Pre);
	case ::EntityType::CustomUrl: return int(AyuSecret::EntityType::TextUrl);
	case ::EntityType::Underline: return int(AyuSecret::EntityType::Underline);
	case ::EntityType::StrikeOut: return int(AyuSecret::EntityType::Strike);
	case ::EntityType::Blockquote: return int(AyuSecret::EntityType::Blockquote);
	case ::EntityType::Spoiler: return int(AyuSecret::EntityType::Spoiler);
	default: return -1;
	}
}

[[nodiscard]] QString MediaPreview(const MessageData &data) {
	if (data.special) {
		return Qs(data.text);
	}
	switch (data.media.type) {
	case MediaType::Photo: return tr::ayu_SecretMediaPhoto(tr::now);
	case MediaType::Video: return tr::ayu_SecretMediaVideo(tr::now);
	case MediaType::Voice: return tr::ayu_SecretMediaVoice(tr::now);
	case MediaType::Audio: return tr::ayu_SecretMediaAudio(tr::now);
	case MediaType::Sticker:
		return Qs(data.media.emoji)
			+ QChar(u' ')
			+ tr::ayu_SecretMediaSticker(tr::now);
	case MediaType::Animation: return "GIF";
	case MediaType::Location:
	case MediaType::Venue: return tr::ayu_SecretMediaLocation(tr::now);
	case MediaType::Contact: return tr::ayu_SecretMediaContact(tr::now);
	case MediaType::Document:
	case MediaType::External:
		return data.media.fileName.empty()
			? tr::ayu_SecretMediaFile(tr::now)
			: Qs(data.media.fileName);
	default: break;
	}
	return Qs(data.text);
}

[[nodiscard]] QString TtlText(int seconds) {
	if (seconds <= 0) {
		return tr::ayu_SecretTtlOff(tr::now);
	} else if (seconds < 60) {
		return tr::ayu_SecretTtlSeconds(tr::now, lt_count, seconds);
	} else if (seconds < 3600) {
		return tr::ayu_SecretTtlMinutes(tr::now, lt_count, seconds / 60);
	} else if (seconds < 86400) {
		return tr::ayu_SecretTtlHours(tr::now, lt_count, seconds / 3600);
	} else if (seconds < 7 * 86400) {
		return tr::ayu_SecretTtlDays(tr::now, lt_count, seconds / 86400);
	}
	return tr::ayu_SecretTtlWeeks(
		tr::now,
		lt_count,
		seconds / (7 * 86400));
}

[[nodiscard]] int SentDate(const MTPmessages_SentEncryptedMessage &result) {
	return result.match([](const MTPDmessages_sentEncryptedMessage &data) {
		return data.vdate().v;
	}, [](const MTPDmessages_sentEncryptedFile &data) {
		return data.vdate().v;
	});
}

} // namespace

struct Manager::Impl {
	explicit Impl(not_null<Main::Session*> session)
	: session(session)
	, userId(ID(session->userId().bare & PeerId::kChatTypeMask)) {
		auto resave = std::vector<int>();
		for (auto &&row : AyuDatabase::getSecretChats(userId)) {
			auto chat = Chat();
			chat.row = std::move(row);
			const auto result = OpenChatRow(chat.row);
			if (result == RowState::Failed) {
				chat.locked = true;
				chat.row.keyData.clear();
				chat.row.state = int(ChatState::Discarded);
			} else {
				if (chat.row.state == int(ChatState::Ready)) {
					DecodeExtras(chat);
				}
				if (result == RowState::Legacy) {
					resave.push_back(chat.row.chatId);
				}
			}
			chats.emplace(chat.row.chatId, std::move(chat));
		}
		for (const auto chatId : resave) {
			saveChat(chats.at(chatId));
		}
		QDir(tempDir()).removeRecursively();
		expireTimer.setCallback([=] { expire(); });
		crl::on_main(session, [=] {
			resumePending();
			scheduleExpire();
		});
	}

	~Impl() {
		QDir(tempDir()).removeRecursively();
	}

	const not_null<Main::Session*> session;
	const ID userId;
	std::map<int, Chat> chats;
	int openChat = 0;
	std::set<int> removed;
	std::set<int> pendingChats;
	bool flushQueued = false;
	std::map<std::pair<int, int64_t>, QByteArray> plainCache;
	std::deque<std::pair<int, int64_t>> plainOrder;
	int64_t plainBytes = 0;
	Fn<void(int)> started;
	base::Timer expireTimer;
	rpl::event_stream<> changes;
	rpl::event_stream<int> messageChanges;

	[[nodiscard]] bool chatInView(int chatId) const {
		if (openChat != chatId) {
			return false;
		}
		const auto window = Core::App().activeWindow();
		return window && window->widget()->isActiveWindow();
	}
	void remember(int chatId, int64_t randomId, const QByteArray &bytes) {
		constexpr auto kLimit = int64_t(96) * 1024 * 1024;
		const auto key = std::make_pair(chatId, randomId);
		if (int64_t(bytes.size()) > kLimit
			|| plainCache.contains(key)) {
			return;
		}
		while (!plainOrder.empty()
			&& plainBytes + bytes.size() > kLimit) {
			const auto oldest = plainOrder.front();
			plainOrder.pop_front();
			if (const auto i = plainCache.find(oldest)
				; i != plainCache.end()) {
				plainBytes -= i->second.size();
				plainCache.erase(i);
			}
		}
		plainCache.emplace(key, bytes);
		plainOrder.push_back(key);
		plainBytes += bytes.size();
	}
	void forget(int chatId, int64_t randomId) {
		if (const auto i = plainCache.find({ chatId, randomId })
			; i != plainCache.end()) {
			plainBytes -= i->second.size();
			plainCache.erase(i);
		}
	}
	void forgetChat(int chatId) {
		for (auto i = plainCache.begin(); i != plainCache.end();) {
			if (i->first.first == chatId) {
				plainBytes -= i->second.size();
				i = plainCache.erase(i);
			} else {
				++i;
			}
		}
		plainOrder.erase(
			std::remove_if(
				plainOrder.begin(),
				plainOrder.end(),
				[&](const auto &key) { return key.first == chatId; }),
			plainOrder.end());
	}
	[[nodiscard]] Chat *find(int chatId) {
		const auto i = chats.find(chatId);
		return (i != chats.end()) ? &i->second : nullptr;
	}
	[[nodiscard]] bool creator(const Chat &chat) const {
		return chat.row.creator != 0;
	}
	[[nodiscard]] int x(const Chat &chat) const {
		return creator(chat) ? 0 : 1;
	}
	[[nodiscard]] QString title(const Chat &chat) const {
		if (const auto user = session->data().userLoaded(
				UserId(uint64(chat.row.peerUserId)))) {
			return user->name();
		}
		return tr::ayu_SecretUserFallback(
			tr::now,
			lt_id,
			QString::number(chat.row.peerUserId));
	}
	void saveChat(Chat &chat) {
		if (chat.locked) {
			return;
		}
		if (chat.row.state == int(ChatState::Ready)) {
			chat.row.keyData = EncodeExtras(chat);
		}
		auto row = chat.row;
		if (SealChatRow(row)) {
			AyuDatabase::saveSecretChat(row);
		}
	}
	void saveMessage(const Chat &chat, const MessageData &data) {
		const auto row = rowFor(chat, data);
		if (!row.payload.empty()) {
			AyuDatabase::updateSecretMessage(row);
		}
	}
	void notify(int chatId = 0) {
		if (!chatId) {
			changes.fire({});
			return;
		}
		pendingChats.insert(chatId);
		if (flushQueued) {
			return;
		}
		flushQueued = true;
		crl::on_main(session, [=] {
			flushQueued = false;
			auto ids = base::take(pendingChats);
			for (const auto id : ids) {
				messageChanges.fire_copy(id);
			}
		});
	}
	void toast(const QString &text) {
		Ui::Toast::Show(text);
	}
	[[nodiscard]] MTPInputEncryptedChat input(const Chat &chat) const {
		return MTP_inputEncryptedChat(
			MTP_int(chat.row.chatId),
			MTP_long(chat.row.accessHash));
	}

	void requestDh(
		Fn<void(const DhConfig &)> done,
		Fn<void()> fail);

	void applyChat(const MTPEncryptedChat &chat);
	void onRequested(const MTPDencryptedChatRequested &data);
	void onChat(const MTPDencryptedChat &data);
	void onPrepared(int chatId, int ticket, ReadyPacket &&packet);
	void applyPacket(Chat &chat, ReadyPacket &&packet);
	void onDiscarded(int chatId, bool historyDeleted);
	[[nodiscard]] ChatInfo makeInfo(const Chat &chat, bool withKey) const;
	void finishCreator(int chatId, const QByteArray &gB, int64 fingerprint);
	void becomeReady(Chat &chat, const Bytes &key);

	void accept(int chatId);
	void discard(int chatId);
	void start(not_null<UserData*> user);

	void handleEncrypted(const MTPEncryptedMessage &message);
	void onEncrypted(
		int chatId,
		int date,
		const QByteArray &bytes,
		const MTPEncryptedFile *file);
	void process(Chat &chat, const Inbound &inbound, int date, const MTPEncryptedFile *file);
	void processService(Chat &chat, const Inbound &inbound, int date);
	void drainPending(Chat &chat);
	void requestResend(Chat &chat, int fromIndex, int tillIndex);
	void answerResend(Chat &chat, int start, int end);

	void loadMessages(Chat &chat);
	[[nodiscard]] SecretMessageRow rowFor(
		const Chat &chat,
		const MessageData &data) const;
	void addMessage(Chat &chat, MessageData data);
	void updateMessage(Chat &chat, const MessageData &data);
	void removeMessage(Chat &chat, int64_t randomId);
	void discardMessage(
		Chat &chat,
		int64_t randomId,
		DeleteOrigin origin);
	[[nodiscard]] MessageData *findMessage(Chat &chat, int64_t randomId);
	void addNote(
		Chat &chat,
		const QString &text,
		int date,
		int special = kSpecialNote);

	struct SendFile {
		bool big = false;
		int parts = 0;
		int64_t fileId = 0;
		int32_t fingerprint = 0;
	};
	void dispatch(
		Chat &chat,
		MessageData data,
		std::optional<SendFile> file);
	void sendEncrypted(
		Chat &chat,
		const Bytes &layerObject,
		int64_t randomId,
		int mode,
		std::optional<SendFile> file,
		const MTPInputEncryptedFile *existing,
		Fn<void(const MTPEncryptedFile*, int)> done,
		Fn<void()> failed);
	void sendService(Chat &chat, const Bytes &message);
	void sendNotifyLayer(Chat &chat);
	void resumePending();

	void sendText(int chatId, TextWithEntities text, int64 replyTo);
	void sendFile(int chatId, OutgoingFile outgoing);
	void uploadParts(
		int chatId,
		int64_t randomId,
		std::shared_ptr<Bytes> encrypted,
		SendFile file,
		int index);
	void downloadMedia(int chatId, int64 randomId);
	void downloadChunk(
		int chatId,
		int64_t randomId,
		std::shared_ptr<Bytes> buffer);
	void finishDownload(int chatId, int64_t randomId, const Bytes &buffer);
	void openMessage(int chatId, int64 randomId);
	void startTimer(Chat &chat, MessageData &data, int from);
	void setTtl(int chatId, int seconds);
	void deleteMessages(int chatId, const std::vector<int64_t> &ids);
	void clearHistory(int chatId);
	void setTyping(int chatId);
	void markRead(int chatId);

	void handleRequestKey(Chat &chat, const Inbound &inbound);
	void handleAcceptKey(Chat &chat, const Inbound &inbound);
	void handleAbortKey(Chat &chat, const Inbound &inbound);
	void handleCommitKey(Chat &chat, const Inbound &inbound);
	void startRekey(Chat &chat);
	void maybeStartRekey(Chat &chat);
	void clearOurExchange(Chat &chat);
	void resetKeyUsage(Chat &chat);

	void expire();
	void scheduleExpire();
	void ackQts(int qts);
	[[nodiscard]] QString mediaDir(int chatId) const;
	[[nodiscard]] QString tempDir() const;
	[[nodiscard]] QString storeFile(
		int chatId,
		int64_t randomId,
		const Bytes &plain) const;
};

void Manager::Impl::requestDh(
		Fn<void(const DhConfig &)> done,
		Fn<void()> fail) {
	session->api().request(MTPmessages_GetDhConfig(
		MTP_int(0),
		MTP_int(256)
	)).done([=](const MTPmessages_DhConfig &result) {
		result.match([&](const MTPDmessages_dhConfig &data) {
			auto config = DhConfig();
			config.g = data.vg().v;
			config.p = FromArray(data.vp().v);
			config.random = FromArray(data.vrandom().v);
			if (config.random.size() != 256
				|| !MTP::IsPrimeAndGood(Span(config.p), config.g)) {
				fail();
				return;
			}
			done(config);
		}, [&](const MTPDmessages_dhConfigNotModified &) {
			fail();
		});
	}).fail([=](const MTP::Error &) {
		fail();
	}).send();
}

void Manager::Impl::applyChat(const MTPEncryptedChat &chat) {
	chat.match([&](const MTPDencryptedChatEmpty &) {
	}, [&](const MTPDencryptedChatWaiting &data) {
		if (const auto existing = find(data.vid().v)) {
			existing->row.accessHash = data.vaccess_hash().v;
			saveChat(*existing);
		}
	}, [&](const MTPDencryptedChatRequested &data) {
		onRequested(data);
	}, [&](const MTPDencryptedChat &data) {
		onChat(data);
	}, [&](const MTPDencryptedChatDiscarded &data) {
		onDiscarded(data.vid().v, data.is_history_deleted());
	});
}

void Manager::Impl::onRequested(const MTPDencryptedChatRequested &data) {
	const auto id = data.vid().v;
	if (find(id)
		|| removed.contains(id)
		|| uint64(data.vadmin_id().v) == session->userId().bare) {
		return;
	}
	auto chat = Chat();
	chat.row.fakeId = 0;
	chat.row.userId = userId;
	chat.row.chatId = id;
	chat.row.accessHash = data.vaccess_hash().v;
	chat.row.peerUserId = data.vadmin_id().v;
	chat.row.creator = 0;
	chat.row.state = int(ChatState::Requested);
	chat.row.keyData = ToChars(FromArray(data.vg_a().v));
	chat.row.fingerprint = 0;
	chat.row.myIn = 0;
	chat.row.myOut = 0;
	chat.row.hisIn = 0;
	chat.row.hisLayer = 0;
	chat.row.date = data.vdate().v;
	chat.row.lastDate = data.vdate().v;
	chat.row.unread = 0;
	chat.loaded = true;
	saveChat(chat);
	const auto date = chat.row.date;
	const auto inserted = chats.emplace(id, std::move(chat)).first;
	addNote(
		inserted->second,
		tr::ayu_SecretNoteRequested(tr::now),
		date,
		kSpecialRequest);
	notify();
}

void Manager::Impl::onChat(const MTPDencryptedChat &data) {
	const auto chat = find(data.vid().v);
	if (!chat) {
		return;
	}
	if (chat->row.state == int(ChatState::Waiting) && creator(*chat)) {
		chat->row.accessHash = data.vaccess_hash().v;
		finishCreator(
			chat->row.chatId,
			data.vg_a_or_b().v,
			data.vkey_fingerprint().v);
	} else if (chat->row.state == int(ChatState::Requested)
		&& !chat->working) {
		chat->row.state = int(ChatState::Discarded);
		chat->row.keyData.clear();
		saveChat(*chat);
		toast(tr::ayu_SecretToastAcceptedElsewhere(tr::now));
		notify();
	}
}

void Manager::Impl::onDiscarded(int chatId, bool historyDeleted) {
	const auto chat = find(chatId);
	if (!chat || chat->row.state == int(ChatState::Discarded)) {
		return;
	}
	chat->row.state = int(ChatState::Discarded);
	chat->row.keyData.clear();
	chat->key.clear();
	chat->otherKey.clear();
	chat->pfsExchange = 0;
	chat->pfsPending.clear();
	chat->pfsAnswerGB.clear();
	clearOurExchange(*chat);
	if (historyDeleted) {
		if (AyuSettings::getInstance().saveDeletedMessages()) {
			loadMessages(*chat);
			auto ids = std::vector<int64_t>();
			for (const auto &message : chat->messages) {
				ids.push_back(message.randomId);
			}
			for (const auto id : ids) {
				discardMessage(*chat, id, DeleteOrigin::Remote);
			}
		} else {
			++chat->revision;
			AyuDatabase::clearSecretMessages(userId, chatId);
			forgetChat(chatId);
			chat->messages.clear();
		}
		chat->row.unread = 0;
	}
	saveChat(*chat);
	addNote(
		*chat,
		tr::ayu_SecretNoteEnded(tr::now),
		base::unixtime::now(),
		kSpecialEnded);
	notify(chatId);
}

void Manager::Impl::becomeReady(Chat &chat, const Bytes &key) {
	chat.key = key;
	chat.row.fingerprint = KeyFingerprint(key);
	chat.row.state = int(ChatState::Ready);
	chat.row.myIn = 0;
	chat.row.myOut = 0;
	chat.row.hisIn = 0;
	chat.working = false;
	resetKeyUsage(chat);
	saveChat(chat);
	sendNotifyLayer(chat);
	notify(chat.row.chatId);
	auto early = std::move(chat.early);
	chat.early.clear();
	for (const auto &entry : early) {
		onEncrypted(
			chat.row.chatId,
			entry.date,
			entry.bytes,
			entry.file ? &*entry.file : nullptr);
	}
}

void Manager::Impl::finishCreator(
		int chatId,
		const QByteArray &gB,
		int64 fingerprint) {
	const auto chat = find(chatId);
	if (!chat || chat->working) {
		return;
	}
	chat->working = true;
	const auto secret = FromChars(chat->row.keyData);
	const auto theirs = FromArray(gB);
	requestDh([=](const DhConfig &config) {
		const auto chat = find(chatId);
		if (!chat) {
			return;
		}
		chat->working = false;
		const auto raw = MTP::CreateAuthKey(
			Span(theirs),
			Span(secret),
			Span(config.p));
		if (raw.empty()) {
			toast(tr::ayu_SecretToastKeyFailed(tr::now));
			return;
		}
		const auto key = PadKey(FromBytesVector(raw));
		if (KeyFingerprint(key) != fingerprint) {
			toast(tr::ayu_SecretToastFingerprint(tr::now));
			discard(chatId);
			return;
		}
		becomeReady(*chat, key);
	}, [=] {
		if (const auto chat = find(chatId)) {
			chat->working = false;
		}
	});
}

void Manager::Impl::accept(int chatId) {
	const auto chat = find(chatId);
	if (!chat
		|| chat->row.state != int(ChatState::Requested)
		|| chat->working) {
		return;
	}
	chat->working = true;
	const auto gA = FromChars(chat->row.keyData);
	requestDh([=](const DhConfig &config) {
		const auto chat = find(chatId);
		if (!chat) {
			return;
		}
		const auto prime = openssl::BigNum(Span(config.p));
		if (!MTP::IsGoodModExpFirst(openssl::BigNum(Span(gA)), prime)) {
			chat->working = false;
			toast(tr::ayu_SecretToastInvalidRequest(tr::now));
			discard(chatId);
			return;
		}
		const auto first = MTP::CreateModExp(
			config.g,
			Span(config.p),
			Span(config.random));
		const auto raw = MTP::CreateAuthKey(
			Span(gA),
			first.randomPower,
			Span(config.p));
		if (raw.empty()) {
			chat->working = false;
			toast(tr::ayu_SecretToastKeyFailed(tr::now));
			return;
		}
		const auto key = PadKey(FromBytesVector(raw));
		const auto fingerprint = KeyFingerprint(key);
		session->api().request(MTPmessages_AcceptEncryption(
			input(*chat),
			MTP_bytes(ToArray(FromBytesVector(first.modexp))),
			MTP_long(fingerprint)
		)).done([=](const MTPEncryptedChat &result) {
			const auto chat = find(chatId);
			if (!chat) {
				return;
			}
			chat->working = false;
			result.match([&](const MTPDencryptedChat &data) {
				if (data.vkey_fingerprint().v != fingerprint) {
					toast(tr::ayu_SecretToastFingerprint(tr::now));
					return;
				}
				chat->row.accessHash = data.vaccess_hash().v;
				becomeReady(*chat, key);
			}, [&](const auto &) {
				applyChat(result);
			});
		}).fail([=](const MTP::Error &error) {
			if (const auto chat = find(chatId)) {
				chat->working = false;
			}
			const auto type = error.type();
			if (type == u"ENCRYPTION_ALREADY_DECLINED"_q
				|| type == u"ENCRYPTION_DECLINED"_q
				|| type == u"ENCRYPTION_ALREADY_ACCEPTED"_q
				|| type == u"CHAT_ID_INVALID"_q) {
				onDiscarded(chatId, false);
			}
			toast(tr::ayu_SecretToastAcceptFailed(
				tr::now,
				lt_reason,
				type));
		}).send();
	}, [=] {
		if (const auto chat = find(chatId)) {
			chat->working = false;
		}
		toast(tr::ayu_SecretToastNoDhConfig(tr::now));
	});
}

void Manager::Impl::discard(int chatId) {
	const auto chat = find(chatId);
	if (!chat) {
		return;
	}
	if (chat->row.state != int(ChatState::Discarded)) {
		session->api().request(MTPmessages_DiscardEncryption(
			MTP_flags(0),
			MTP_int(chatId)
		)).send();
	}
	chat->row.state = int(ChatState::Discarded);
	chat->row.keyData.clear();
	chat->key.clear();
	chat->otherKey.clear();
	chat->pfsExchange = 0;
	chat->pfsPending.clear();
	chat->pfsAnswerGB.clear();
	clearOurExchange(*chat);
	chat->working = false;
	saveChat(*chat);
	notify(chatId);
}

void Manager::Impl::start(not_null<UserData*> user) {
	const auto peerId = user->id.value & PeerId::kChatTypeMask;
	for (const auto &[id, chat] : chats) {
		if (uint64(chat.row.peerUserId) == peerId
			&& creator(chat)
			&& chat.row.state == int(ChatState::Waiting)) {
			toast(tr::ayu_SecretToastWaitingAccept(tr::now));
			return;
		}
	}
	requestDh([=](const DhConfig &config) {
		const auto first = MTP::CreateModExp(
			config.g,
			Span(config.p),
			Span(config.random));
		const auto secret = FromBytesVector(first.randomPower);
		auto randomId = int32(0);
		while (!randomId) {
			RandomBytes(reinterpret_cast<uint8_t*>(&randomId), 4);
			randomId &= 0x7FFFFFFF;
		}
		session->api().request(MTPmessages_RequestEncryption(
			user->inputUser(),
			MTP_int(randomId),
			MTP_bytes(ToArray(FromBytesVector(first.modexp)))
		)).done([=](const MTPEncryptedChat &result) {
			result.match([&](const MTPDencryptedChatWaiting &data) {
				auto chat = Chat();
				chat.row.fakeId = 0;
				chat.row.userId = userId;
				chat.row.chatId = data.vid().v;
				chat.row.accessHash = data.vaccess_hash().v;
				chat.row.peerUserId = ID(peerId);
				chat.row.creator = 1;
				chat.row.state = int(ChatState::Waiting);
				chat.row.keyData = ToChars(secret);
				chat.row.fingerprint = 0;
				chat.row.myIn = 0;
				chat.row.myOut = 0;
				chat.row.hisIn = 0;
				chat.row.hisLayer = 0;
				chat.row.date = data.vdate().v;
				chat.row.lastDate = data.vdate().v;
				chat.row.unread = 0;
				chat.loaded = true;
				saveChat(chat);
				chats[chat.row.chatId] = std::move(chat);
				toast(tr::ayu_SecretToastRequestSent(tr::now));
				notify();
				if (started) {
					started(data.vid().v);
				}
			}, [&](const auto &) {
			});
		}).fail([=](const MTP::Error &error) {
			toast(tr::ayu_SecretToastStartFailed(
				tr::now,
				lt_reason,
				error.type()));
		}).send();
	}, [=] {
		toast(tr::ayu_SecretToastNoDhConfig(tr::now));
	});
}

void Manager::Impl::handleEncrypted(const MTPEncryptedMessage &message) {
	message.match([&](const MTPDencryptedMessage &data) {
		onEncrypted(
			data.vchat_id().v,
			data.vdate().v,
			data.vbytes().v,
			&data.vfile());
	}, [&](const MTPDencryptedMessageService &data) {
		onEncrypted(data.vchat_id().v, data.vdate().v, data.vbytes().v, nullptr);
	});
}

void Manager::Impl::onEncrypted(
		int chatId,
		int date,
		const QByteArray &bytes,
		const MTPEncryptedFile *file) {
	const auto chat = find(chatId);
	if (!chat) {
		return;
	} else if (chat->row.state == int(ChatState::Waiting)) {
		if (int(chat->early.size()) >= kMaxEarly) {
			return;
		}
		chat->early.push_back({
			date,
			bytes,
			file ? std::make_optional(*file) : std::nullopt,
		});
		return;
	} else if (chat->row.state != int(ChatState::Ready)) {
		return;
	}
	const auto ticket = chat->nextTicket++;
	const auto owner = session.get();
	const auto isCreator = creator(*chat);
	auto keys = std::array<Bytes, 3>{
		chat->key,
		chat->otherKey,
		chat->pfsPending,
	};
	auto packet = ReadyPacket();
	packet.date = date;
	packet.bytes = bytes;
	packet.file = file ? std::make_optional(*file) : std::nullopt;
	crl::async([=, keys = std::move(keys), packet = std::move(packet)]() mutable {
		auto prepared = PreparePacket(keys, isCreator, std::move(packet));
		crl::on_main(owner, [=, prepared = std::move(prepared)]() mutable {
			onPrepared(chatId, ticket, std::move(prepared));
		});
	});
}

void Manager::Impl::onPrepared(
		int chatId,
		int ticket,
		ReadyPacket &&packet) {
	auto chat = find(chatId);
	if (!chat) {
		return;
	}
	chat->ready.emplace(ticket, std::move(packet));
	while (chat) {
		const auto i = chat->ready.find(chat->nextApply);
		if (i == chat->ready.end()) {
			break;
		}
		auto next = std::move(i->second);
		chat->ready.erase(i);
		++chat->nextApply;
		applyPacket(*chat, std::move(next));
		chat = find(chatId);
	}
}

void Manager::Impl::applyPacket(Chat &chat, ReadyPacket &&packet) {
	if (chat.row.state != int(ChatState::Ready)) {
		return;
	}
	if (!packet.decrypted) {
		packet = PreparePacket(
			{ chat.key, chat.otherKey, chat.pfsPending },
			creator(chat),
			std::move(packet));
	}
	if (!packet.decrypted) {
		return;
	}
	++chat.keyUsesIn;
	maybeStartRekey(chat);
	if (!chat.otherKey.empty() && packet.bytes.size() >= 8) {
		auto used = int64_t(0);
		std::memcpy(&used, packet.bytes.constData(), sizeof(used));
		if (used == chat.row.fingerprint) {
			chat.otherKey.clear();
			saveChat(chat);
		}
	}
	if (!packet.parsed) {
		sendNotifyLayer(chat);
		return;
	}
	const auto &inbound = packet.inbound;
	const auto file = packet.file ? &*packet.file : nullptr;
	if (inbound.inSeqNo < 0 || inbound.outSeqNo < 0) {
		return;
	}
	const auto mine = x(chat);
	if ((inbound.inSeqNo % 2) != (1 - mine)
		|| (inbound.outSeqNo % 2) != mine) {
		return;
	}
	const auto outIndex = inbound.outSeqNo / 2;
	if (outIndex < chat.row.myIn) {
		return;
	}
	if (outIndex > chat.row.myIn) {
		if (outIndex - chat.row.myIn > kMaxPending
			|| int(chat.pending.size()) >= kMaxPending) {
			return;
		}
		chat.pending.emplace(outIndex, PendingEntry{
			inbound,
			packet.date,
			packet.file,
		});
		requestResend(chat, chat.row.myIn, chat.pending.begin()->first - 1);
		return;
	}
	process(chat, inbound, packet.date, file);
	drainPending(chat);
}

void Manager::Impl::drainPending(Chat &chat) {
	while (true) {
		const auto i = chat.pending.find(chat.row.myIn);
		if (i == chat.pending.end()) {
			break;
		}
		const auto entry = std::move(i->second);
		chat.pending.erase(i);
		process(
			chat,
			entry.inbound,
			entry.date,
			entry.file ? &*entry.file : nullptr);
	}
}

void Manager::Impl::requestResend(Chat &chat, int fromIndex, int tillIndex) {
	if (tillIndex < fromIndex || tillIndex <= chat.resendRequestedTill) {
		return;
	}
	chat.resendRequestedTill = tillIndex;
	const auto mine = x(chat);
	sendService(
		chat,
		BuildResend(RandomId(), fromIndex * 2 + mine, tillIndex * 2 + mine));
}

void Manager::Impl::answerResend(Chat &chat, int start, int end) {
	const auto from = start / 2;
	const auto till = std::min(end / 2, from + 100);
	loadMessages(chat);
	auto resend = std::vector<MessageData>();
	for (const auto &message : chat.messages) {
		if (!message.outgoing || message.seqOut <= 0) {
			continue;
		}
		const auto index = message.seqOut / 2;
		if (index >= from && index <= till && !message.object.empty()) {
			resend.push_back(message);
		}
	}
	for (const auto &data : resend) {
		const auto layerObject = BuildLayerObject(
			data.object,
			data.seqIn,
			data.seqOut);
		const auto media = data.media.type != MediaType::None
			&& data.media.type != MediaType::Location
			&& data.media.type != MediaType::Contact
			&& data.media.type != MediaType::WebPage
			&& !data.special;
		if (media && data.media.fileId) {
			const auto existing = MTPInputEncryptedFile(MTP_inputEncryptedFile(
				MTP_long(data.media.fileId),
				MTP_long(data.media.accessHash)));
			sendEncrypted(chat, layerObject, data.randomId, 2, std::nullopt, &existing, nullptr, nullptr);
		} else {
			sendEncrypted(
				chat,
				layerObject,
				data.randomId,
				data.special == kSpecialHidden ? 1 : 0,
				std::nullopt,
				nullptr,
				nullptr,
				nullptr);
		}
	}
}

void Manager::Impl::process(
		Chat &chat,
		const Inbound &inbound,
		int date,
		const MTPEncryptedFile *file) {
	chat.row.myIn = inbound.outSeqNo / 2 + 1;
	chat.row.hisIn = std::max(chat.row.hisIn, inbound.inSeqNo / 2);
	chat.row.hisLayer = std::max(chat.row.hisLayer, inbound.layer);
	const auto chatId = chat.row.chatId;
	if (inbound.service) {
		processService(chat, inbound, date);
		saveChat(chat);
		notify(chatId);
		return;
	}
	auto data = MessageData();
	data.randomId = inbound.randomId;
	data.outgoing = false;
	data.date = date;
	data.ttl = inbound.ttl;
	data.text = inbound.text;
	data.entities = inbound.entities;
	data.replyTo = inbound.replyTo;
	data.media = inbound.media;
	if (data.media.type == MediaType::Unsupported) {
		data.media = Media();
		data.text += data.text.empty()
			? "[unsupported attachment]"
			: "\n[unsupported attachment]";
	}
	if (file && data.media.type != MediaType::None) {
		file->match([&](const MTPDencryptedFile &fileData) {
			data.media.fileId = fileData.vid().v;
			data.media.accessHash = fileData.vaccess_hash().v;
			data.media.dcId = ValidDc(fileData.vdc_id().v);
			if (data.media.size <= 0) {
				data.media.size = fileData.vsize().v;
			}
		}, [&](const auto &) {
		});
	}
	if (data.media.type == MediaType::External) {
		data.media.dcId = ValidDc(data.media.dcId);
	}
	const auto id = data.randomId;
	const auto autoLoad = (data.media.type == MediaType::Photo
		|| data.media.type == MediaType::Sticker
		|| data.media.type == MediaType::Animation)
		&& data.media.size <= kAutoDownloadLimit
		&& (data.media.fileId != 0);
	addMessage(chat, std::move(data));
	if (autoLoad) {
		downloadMedia(chatId, id);
	}
	if (chatInView(chatId)) {
		markRead(chatId);
	}
}

void Manager::Impl::processService(
		Chat &chat,
		const Inbound &inbound,
		int date) {
	const auto chatId = chat.row.chatId;
	switch (inbound.action) {
	case ActionKind::NotifyLayer:
		chat.row.hisLayer = std::max(chat.row.hisLayer, inbound.actionValue);
		break;
	case ActionKind::Resend:
		answerResend(chat, inbound.resendStart, inbound.resendEnd);
		break;
	case ActionKind::DeleteMessages:
		for (const auto id : inbound.ids) {
			discardMessage(chat, id, DeleteOrigin::Remote);
		}
		break;
	case ActionKind::FlushHistory:
		if (AyuSettings::getInstance().saveDeletedMessages()) {
			loadMessages(chat);
			auto ids = std::vector<int64_t>();
			for (const auto &message : chat.messages) {
				ids.push_back(message.randomId);
			}
			for (const auto id : ids) {
				discardMessage(chat, id, DeleteOrigin::Remote);
			}
		} else {
			++chat.revision;
			AyuDatabase::clearSecretMessages(userId, chatId);
			forgetChat(chatId);
			QDir(mediaDir(chatId)).removeRecursively();
			chat.messages.clear();
		}
		chat.row.unread = 0;
		break;
	case ActionKind::ReadMessages:
		for (const auto id : inbound.ids) {
			if (const auto message = findMessage(chat, id)) {
				if (message->outgoing) {
					message->opened = true;
					if (message->state != DeliveryState::Failed) {
						message->state = DeliveryState::Read;
					}
					startTimer(chat, *message, base::unixtime::now());
					updateMessage(chat, *message);
				}
			}
		}
		break;
	case ActionKind::SetTtl:
		if (chat.ttl == std::max(0, inbound.actionValue)) {
			break;
		}
		chat.ttl = std::max(0, inbound.actionValue);
		addNote(
			chat,
			tr::ayu_SecretNoteTtlSet(
				tr::now,
				lt_name,
				title(chat),
				lt_time,
				TtlText(chat.ttl)),
			date);
		break;
	case ActionKind::ScreenshotMessages:
		addNote(
			chat,
			tr::ayu_SecretNoteScreenshot(tr::now, lt_name, title(chat)),
			date);
		break;
	case ActionKind::RequestKey:
		handleRequestKey(chat, inbound);
		break;
	case ActionKind::AcceptKey:
		handleAcceptKey(chat, inbound);
		break;
	case ActionKind::AbortKey:
		handleAbortKey(chat, inbound);
		break;
	case ActionKind::CommitKey:
		handleCommitKey(chat, inbound);
		break;
	case ActionKind::Typing:
		chat.typingUntil = crl::now() + kTypingTimeout;
		base::call_delayed(kTypingTimeout + 100, session, [=] {
			notify();
		});
		break;
	default:
		break;
	}
}

void Manager::Impl::clearOurExchange(Chat &chat) {
	chat.pfsOurExchange = 0;
	chat.pfsOurSecret.clear();
	chat.pfsOurP.clear();
}

void Manager::Impl::resetKeyUsage(Chat &chat) {
	chat.keyInstalledAt = base::unixtime::now();
	chat.keyUsesOut = 0;
	chat.keyUsesIn = 0;
}

void Manager::Impl::handleRequestKey(Chat &chat, const Inbound &inbound) {
	const auto chatId = chat.row.chatId;
	const auto exchangeId = inbound.exchangeId;
	if (!exchangeId) {
		return;
	}
	if (chat.pfsOurExchange != 0) {
		// Concurrent re-keying, see DecideConcurrent: our instance wins
		// (the other side applies the same rule), otherwise abandon ours
		// and answer the incoming one. On exact match abort both.
		switch (DecideConcurrent(chat.pfsOurExchange, exchangeId)) {
		case ConcurrentDecision::AnswerIncoming:
			clearOurExchange(chat);
			break;
		case ConcurrentDecision::AbortBoth:
			clearOurExchange(chat);
			return;
		case ConcurrentDecision::IgnoreIncoming:
			return;
		}
	}
	if (chat.pfsExchange != 0) {
		// Already answering another instance: keep the first one.
		// A duplicate of the answered exchange gets our answer again,
		// so a lost AcceptKey still completes without waiting for the
		// seqno resend machinery.
		if (chat.pfsExchange == exchangeId
			&& !chat.pfsPending.empty()
			&& !chat.pfsAnswerGB.empty()) {
			sendService(
				chat,
				BuildAcceptKey(
					RandomId(),
					exchangeId,
					chat.pfsAnswerGB,
					KeyFingerprint(chat.pfsPending)));
		}
		return;
	}
	// Reserve the exchange synchronously: duplicates arriving while the
	// DH config is in flight are answered or ignored above instead of
	// spawning parallel exchanges.
	chat.pfsExchange = exchangeId;
	const auto gA = inbound.value;
	requestDh([=](const DhConfig &config) {
		const auto chat = find(chatId);
		if (!chat) {
			return;
		}
		if (chat->row.state != int(ChatState::Ready)
			|| chat->key.empty()
			|| chat->pfsExchange != exchangeId) {
			if (chat->pfsExchange == exchangeId) {
				chat->pfsExchange = 0;
			}
			return;
		}
		if (chat->pfsOurExchange != 0) {
			switch (DecideConcurrent(chat->pfsOurExchange, exchangeId)) {
			case ConcurrentDecision::AnswerIncoming:
				clearOurExchange(*chat);
				break;
			case ConcurrentDecision::AbortBoth:
				clearOurExchange(*chat);
				chat->pfsExchange = 0;
				return;
			case ConcurrentDecision::IgnoreIncoming:
				chat->pfsExchange = 0;
				return;
			}
		}
		const auto prime = openssl::BigNum(Span(config.p));
		if (gA.empty()
			|| gA.size() > kKeySize
			|| !MTP::IsGoodModExpFirst(
				openssl::BigNum(Span(gA)),
				prime)) {
			chat->pfsExchange = 0;
			sendService(*chat, BuildAbortKey(RandomId(), exchangeId));
			return;
		}
		const auto first = MTP::CreateModExp(
			config.g,
			Span(config.p),
			Span(config.random));
		const auto raw = MTP::CreateAuthKey(
			Span(gA),
			first.randomPower,
			Span(config.p));
		if (raw.empty()) {
			chat->pfsExchange = 0;
			sendService(*chat, BuildAbortKey(RandomId(), exchangeId));
			return;
		}
		const auto key = PadKey(FromBytesVector(raw));
		chat->pfsExchange = exchangeId;
		chat->pfsPending = key;
		chat->pfsAnswerGB = FromBytesVector(first.modexp);
		saveChat(*chat);
		sendService(
			*chat,
			BuildAcceptKey(
				RandomId(),
				exchangeId,
				FromBytesVector(first.modexp),
				KeyFingerprint(key)));
	}, [] {
	});
}

void Manager::Impl::handleCommitKey(Chat &chat, const Inbound &inbound) {
	if (chat.row.state != int(ChatState::Ready)
		|| chat.key.empty()
		|| chat.pfsPending.empty()
		|| chat.pfsExchange != inbound.exchangeId
		|| KeyFingerprint(chat.pfsPending) != inbound.fingerprint) {
		return;
	}
	chat.otherKey = chat.key;
	chat.key = chat.pfsPending;
	chat.row.fingerprint = KeyFingerprint(chat.key);
	chat.pfsPending.clear();
	chat.pfsExchange = 0;
	chat.pfsAnswerGB.clear();
	resetKeyUsage(chat);
	saveChat(chat);
	// Let the initiator discard its previous key: everything from now on
	// is encrypted with the new key, and if nothing else is scheduled a
	// no-op carries the new fingerprint to the other side.
	sendService(chat, BuildNoop(RandomId()));
}

void Manager::Impl::handleAcceptKey(Chat &chat, const Inbound &inbound) {
	if (chat.row.state != int(ChatState::Ready) || chat.key.empty()) {
		return;
	}
	const auto exchangeId = inbound.exchangeId;
	if (chat.pfsOurExchange == 0 || chat.pfsOurExchange != exchangeId) {
		// An answer to an exchange we did not initiate (stale after a
		// restart, duplicate, or forged): never commit to it.
		return;
	}
	const auto gB = inbound.value;
	const auto secret = chat.pfsOurSecret;
	const auto p = chat.pfsOurP;
	const auto prime = openssl::BigNum(Span(p));
	if (gB.empty()
		|| gB.size() > kKeySize
		|| !MTP::IsGoodModExpFirst(
			openssl::BigNum(Span(gB)),
			prime)) {
		clearOurExchange(chat);
		sendService(chat, BuildAbortKey(RandomId(), exchangeId));
		return;
	}
	const auto raw = MTP::CreateAuthKey(
		Span(gB),
		Span(secret),
		Span(p));
	if (raw.empty()) {
		clearOurExchange(chat);
		sendService(chat, BuildAbortKey(RandomId(), exchangeId));
		return;
	}
	const auto key = PadKey(FromBytesVector(raw));
	const auto fingerprint = KeyFingerprint(key);
	if (fingerprint != inbound.fingerprint) {
		clearOurExchange(chat);
		sendService(chat, BuildAbortKey(RandomId(), exchangeId));
		return;
	}
	chat.otherKey = chat.key;
	chat.key = key;
	chat.row.fingerprint = fingerprint;
	clearOurExchange(chat);
	resetKeyUsage(chat);
	saveChat(chat);
	// From now on we encrypt only with the new key, including this
	// CommitKey, so the other side observes the new fingerprint at once.
	sendService(chat, BuildCommitKey(RandomId(), exchangeId, fingerprint));
}

void Manager::Impl::handleAbortKey(Chat &chat, const Inbound &inbound) {
	if (chat.row.state != int(ChatState::Ready) || chat.key.empty()) {
		return;
	}
	const auto exchangeId = inbound.exchangeId;
	if (!exchangeId) {
		return;
	}
	if (chat.pfsOurExchange == exchangeId) {
		clearOurExchange(chat);
	} else if (chat.pfsExchange == exchangeId) {
		chat.pfsExchange = 0;
		chat.pfsPending.clear();
		chat.pfsAnswerGB.clear();
		saveChat(chat);
	}
}

void Manager::Impl::maybeStartRekey(Chat &chat) {
	if (chat.row.state != int(ChatState::Ready) || chat.key.empty()) {
		return;
	}
	if (chat.pfsExchange != 0
		|| !chat.pfsPending.empty()
		|| chat.pfsOurExchange != 0) {
		// Never start a new instance while one is uncompleted.
		return;
	}
	const auto due = ShouldRekey(
		chat.keyUsesOut,
		chat.keyUsesIn,
		chat.keyInstalledAt,
		base::unixtime::now(),
		kRekeyAfterUses,
		kRekeyAfterTime);
	if (!due) {
		return;
	}
	startRekey(chat);
}

void Manager::Impl::startRekey(Chat &chat) {
	if (chat.row.state != int(ChatState::Ready) || chat.key.empty()) {
		return;
	}
	if (chat.pfsExchange != 0
		|| !chat.pfsPending.empty()
		|| chat.pfsOurExchange != 0) {
		return;
	}
	const auto chatId = chat.row.chatId;
	const auto exchangeId = RandomId();
	requestDh([=](const DhConfig &config) {
		const auto chat = find(chatId);
		if (!chat
			|| chat->row.state != int(ChatState::Ready)
			|| chat->key.empty()) {
			return;
		}
		if (chat->pfsExchange != 0
			|| !chat->pfsPending.empty()
			|| chat->pfsOurExchange != 0) {
			return;
		}
		const auto first = MTP::CreateModExp(
			config.g,
			Span(config.p),
			Span(config.random));
		if (first.modexp.empty() || first.randomPower.empty()) {
			return;
		}
		chat->pfsOurExchange = exchangeId;
		chat->pfsOurSecret = FromBytesVector(first.randomPower);
		chat->pfsOurP = config.p;
		sendService(
			*chat,
			BuildRequestKey(
				RandomId(),
				exchangeId,
				FromBytesVector(first.modexp)));
	}, [] {
	});
}

void Manager::Impl::loadMessages(Chat &chat) {
	if (chat.loaded) {
		return;
	}
	chat.loaded = true;
	chat.messages.clear();
	auto legacy = std::vector<size_t>();
	for (const auto &row : AyuDatabase::getSecretMessages(
			userId,
			chat.row.chatId)) {
		auto data = MessageData();
		auto kind = 0;
		const auto result = UnpackRow(userId, row, data, kind);
		if (result == RowState::Failed) {
			continue;
		}
		if (kind == kKindHidden) {
			data.special = kSpecialHidden;
		} else if (kind == kKindNote) {
			data.special = kSpecialNote;
		} else if (kind == kKindEnded) {
			data.special = kSpecialEnded;
		} else if (kind == kKindRequest) {
			data.special = kSpecialRequest;
		} else if (data.media.type == MediaType::None && kind > 0) {
			data.media.type = MediaType(kind);
		}
		if (result == RowState::Legacy) {
			legacy.push_back(chat.messages.size());
		}
		chat.messages.push_back(std::move(data));
	}
	for (const auto index : legacy) {
		saveMessage(chat, chat.messages[index]);
	}
}

SecretMessageRow Manager::Impl::rowFor(
		const Chat &chat,
		const MessageData &data) const {
	auto row = SecretMessageRow();
	row.fakeId = 0;
	row.userId = userId;
	row.chatId = chat.row.chatId;
	row.randomId = data.randomId;
	row.outgoing = 0;
	row.date = 0;
	row.kind = 0;
	row.seqIn = 0;
	row.seqOut = 0;
	row.text = std::string();
	const auto kind = (data.special == kSpecialHidden)
		? kKindHidden
		: (data.special == kSpecialNote)
		? kKindNote
		: (data.special == kSpecialEnded)
		? kKindEnded
		: (data.special == kSpecialRequest)
		? kKindRequest
		: int(data.media.type);
	row.payload = PackPayload(userId, chat.row.chatId, data, kind);
	return row;
}

MessageData *Manager::Impl::findMessage(Chat &chat, int64_t randomId) {
	loadMessages(chat);
	for (auto &message : chat.messages) {
		if (message.randomId == randomId) {
			return &message;
		}
	}
	return nullptr;
}

void Manager::Impl::addMessage(Chat &chat, MessageData data) {
	loadMessages(chat);
	const auto outgoing = data.outgoing;
	const auto special = data.special;
	const auto date = data.date;
	++chat.revision;
	const auto row = rowFor(chat, data);
	const auto added = !row.payload.empty()
		&& AyuDatabase::addSecretMessage(row);
	if (added) {
		chat.messages.push_back(std::move(data));
		if (special != kSpecialHidden) {
			chat.row.lastDate = std::max(chat.row.lastDate, date);
		}
		if (!outgoing && !special) {
			if (chatInView(chat.row.chatId)) {
				chat.row.unread = 0;
			} else {
				++chat.row.unread;
			}
		}
	}
	saveChat(chat);
	scheduleExpire();
	notify(chat.row.chatId);
}

void Manager::Impl::updateMessage(Chat &chat, const MessageData &data) {
	++chat.revision;
	saveMessage(chat, data);
	if (const auto message = findMessage(chat, data.randomId)) {
		if (message != &data) {
			*message = data;
		}
	}
	notify(chat.row.chatId);
}

void Manager::Impl::discardMessage(
		Chat &chat,
		int64_t randomId,
		DeleteOrigin origin) {
	const auto message = findMessage(chat, randomId);
	if (!message) {
		return;
	}
	const auto keep = !message->special
		&& AyuSettings::getInstance().saveDeletedMessages();
	if (!keep) {
		removeMessage(chat, randomId);
		return;
	} else if (message->deleted) {
		if (origin == DeleteOrigin::User) {
			removeMessage(chat, randomId);
		}
		return;
	}
	message->deleted = true;
	message->expiresAt = 0;
	updateMessage(chat, *message);
}

void Manager::Impl::removeMessage(Chat &chat, int64_t randomId) {
	loadMessages(chat);
	++chat.revision;
	AyuDatabase::removeSecretMessage(userId, chat.row.chatId, randomId);
	forget(chat.row.chatId, randomId);
	if (const auto message = findMessage(chat, randomId)) {
		if (!message->media.path.empty()) {
			const auto path = QFileInfo(Qs(message->media.path)).absoluteFilePath();
			const auto root = QDir(mediaDir(chat.row.chatId)).absolutePath();
			if (path.startsWith(root + '/')) {
				QFile::remove(path);
			}
		}
	}
	chat.messages.erase(
		std::remove_if(
			chat.messages.begin(),
			chat.messages.end(),
			[&](const MessageData &message) {
				return message.randomId == randomId;
			}),
		chat.messages.end());
}

void Manager::Impl::addNote(
		Chat &chat,
		const QString &text,
		int date,
		int special) {
	auto note = MessageData();
	note.randomId = RandomId();
	note.date = date;
	note.special = special;
	note.text = text.toStdString();
	addMessage(chat, std::move(note));
}

void Manager::Impl::dispatch(
		Chat &chat,
		MessageData data,
		std::optional<SendFile> file) {
	if (chat.row.state != int(ChatState::Ready) || chat.key.empty()) {
		return;
	}
	const auto mine = x(chat);
	++chat.row.myOut;
	data.seqIn = chat.row.myIn * 2 + mine;
	data.seqOut = chat.row.myOut * 2 - 1 - mine;
	data.object = BuildMessage(data);
	data.state = DeliveryState::Pending;
	const auto chatId = chat.row.chatId;
	const auto randomId = data.randomId;
	const auto layerObject = BuildLayerObject(
		data.object,
		data.seqIn,
		data.seqOut);
	const auto special = data.special;
	if (findMessage(chat, randomId)) {
		updateMessage(chat, data);
	} else {
		addMessage(chat, data);
	}
	saveChat(chat);
	sendEncrypted(
		chat,
		layerObject,
		randomId,
		(special == kSpecialHidden) ? 1 : file ? 2 : 0,
		file,
		nullptr,
		[=](const MTPEncryptedFile *sent, int serverDate) {
			const auto chat = find(chatId);
			if (!chat) {
				return;
			}
			if (const auto message = findMessage(*chat, randomId)) {
				message->state = (message->state == DeliveryState::Read)
					? DeliveryState::Read
					: DeliveryState::Sent;
				if (serverDate) {
					message->date = serverDate;
				}
				if (sent) {
					sent->match([&](const MTPDencryptedFile &fileData) {
						message->media.fileId = fileData.vid().v;
						message->media.accessHash = fileData.vaccess_hash().v;
					}, [&](const auto &) {
					});
				}
				updateMessage(*chat, *message);
			}
		},
		[=] {
			const auto chat = find(chatId);
			if (!chat) {
				return;
			}
			if (const auto message = findMessage(*chat, randomId)) {
				message->state = DeliveryState::Failed;
				updateMessage(*chat, *message);
			}
		});
}

void Manager::Impl::sendEncrypted(
		Chat &chat,
		const Bytes &layerObject,
		int64_t randomId,
		int mode,
		std::optional<SendFile> file,
		const MTPInputEncryptedFile *existing,
		Fn<void(const MTPEncryptedFile*, int)> done,
		Fn<void()> failed) {
	++chat.keyUsesOut;
	maybeStartRekey(chat);
	const auto data = ToArray(EncryptPacket(
		chat.key,
		creator(chat),
		layerObject));
	const auto chatId = chat.row.chatId;
	const auto onFail = [=](const MTP::Error &error) {
		const auto type = error.type();
		if (type.startsWith("ENCRYPTION_")) {
			if (const auto chat = find(chatId)) {
				chat->row.state = int(ChatState::Discarded);
				chat->key.clear();
				chat->otherKey.clear();
				chat->pfsExchange = 0;
				chat->pfsPending.clear();
				chat->pfsAnswerGB.clear();
				clearOurExchange(*chat);
				chat->row.keyData.clear();
				saveChat(*chat);
				notify(chatId);
			}
		}
		if (failed) {
			failed();
		}
	};
	if (mode == 1) {
		session->api().request(MTPmessages_SendEncryptedService(
			input(chat),
			MTP_long(randomId),
			MTP_bytes(data)
		)).done([=](const MTPmessages_SentEncryptedMessage &result) {
			if (done) {
				done(nullptr, SentDate(result));
			}
		}).fail(onFail).send();
	} else if (mode == 2) {
		auto inputFile = MTPInputEncryptedFile();
		if (existing) {
			inputFile = *existing;
		} else if (file) {
			inputFile = file->big
				? MTP_inputEncryptedFileBigUploaded(
					MTP_long(file->fileId),
					MTP_int(file->parts),
					MTP_int(file->fingerprint))
				: MTP_inputEncryptedFileUploaded(
					MTP_long(file->fileId),
					MTP_int(file->parts),
					MTP_bytes(QByteArray()),
					MTP_int(file->fingerprint));
		} else {
			inputFile = MTP_inputEncryptedFileEmpty();
		}
		session->api().request(MTPmessages_SendEncryptedFile(
			MTP_flags(0),
			input(chat),
			MTP_long(randomId),
			MTP_bytes(data),
			inputFile
		)).done([=](const MTPmessages_SentEncryptedMessage &result) {
			if (!done) {
				return;
			}
			result.match([&](const MTPDmessages_sentEncryptedFile &sent) {
				done(&sent.vfile(), sent.vdate().v);
			}, [&](const MTPDmessages_sentEncryptedMessage &sent) {
				done(nullptr, sent.vdate().v);
			});
		}).fail(onFail).send();
	} else {
		session->api().request(MTPmessages_SendEncrypted(
			MTP_flags(0),
			input(chat),
			MTP_long(randomId),
			MTP_bytes(data)
		)).done([=](const MTPmessages_SentEncryptedMessage &result) {
			if (done) {
				done(nullptr, SentDate(result));
			}
		}).fail(onFail).send();
	}
}

void Manager::Impl::sendService(Chat &chat, const Bytes &message) {
	auto data = MessageData();
	data.randomId = RandomId();
	data.outgoing = true;
	data.date = base::unixtime::now();
	data.special = kSpecialHidden;
	data.object = message;
	if (chat.row.state != int(ChatState::Ready) || chat.key.empty()) {
		return;
	}
	const auto mine = x(chat);
	++chat.row.myOut;
	data.seqIn = chat.row.myIn * 2 + mine;
	data.seqOut = chat.row.myOut * 2 - 1 - mine;
	const auto layerObject = BuildLayerObject(
		message,
		data.seqIn,
		data.seqOut);
	addMessage(chat, data);
	saveChat(chat);
	auto outerId = RandomId();
	sendEncrypted(chat, layerObject, outerId, 1, std::nullopt, nullptr, nullptr, nullptr);
}

void Manager::Impl::sendNotifyLayer(Chat &chat) {
	sendService(chat, BuildNotifyLayer(RandomId(), kLayer));
}

void Manager::Impl::resumePending() {
	for (auto &[id, chat] : chats) {
		if (chat.row.state != int(ChatState::Ready)) {
			continue;
		}
		loadMessages(chat);
		for (auto &message : chat.messages) {
			if (!message.outgoing
				|| message.state != DeliveryState::Pending
				|| message.special == kSpecialHidden) {
				continue;
			}
			if (message.seqOut <= 0 || message.object.empty()) {
				message.state = DeliveryState::Failed;
				updateMessage(chat, message);
				continue;
			}
			const auto media = (message.media.type != MediaType::None
				&& message.media.type != MediaType::Location
				&& message.media.type != MediaType::Contact
				&& message.media.type != MediaType::WebPage);
			const auto chatId = chat.row.chatId;
			const auto randomId = message.randomId;
			const auto layerObject = BuildLayerObject(
				message.object,
				message.seqIn,
				message.seqOut);
			const auto done = [=](const MTPEncryptedFile *, int serverDate) {
				if (const auto chat = find(chatId)) {
					if (const auto message = findMessage(*chat, randomId)) {
						if (message->state != DeliveryState::Read) {
							message->state = DeliveryState::Sent;
						}
						if (serverDate) {
							message->date = serverDate;
						}
						updateMessage(*chat, *message);
					}
				}
			};
			if (media) {
				if (!message.media.fileId) {
					message.state = DeliveryState::Failed;
					updateMessage(chat, message);
					continue;
				}
				const auto existing = MTPInputEncryptedFile(MTP_inputEncryptedFile(
					MTP_long(message.media.fileId),
					MTP_long(message.media.accessHash)));
				sendEncrypted(chat, layerObject, randomId, 2, std::nullopt, &existing, done, nullptr);
			} else {
				sendEncrypted(chat, layerObject, randomId, 0, std::nullopt, nullptr, done, nullptr);
			}
		}
	}
}

void Manager::Impl::sendText(
		int chatId,
		TextWithEntities text,
		int64 replyTo) {
	const auto chat = find(chatId);
	if (!chat) {
		return;
	}
	TextUtilities::Trim(text);
	if (text.text.isEmpty()) {
		return;
	}
	auto data = MessageData();
	data.randomId = RandomId();
	data.outgoing = true;
	data.date = base::unixtime::now();
	data.ttl = chat->ttl;
	data.replyTo = replyTo;
	data.text = text.text.toStdString();
	for (const auto &entity : text.entities) {
		const auto type = EntityTypeFromText(entity.type());
		if (type < 0) {
			continue;
		}
		auto item = Entity();
		item.type = type;
		item.offset = entity.offset();
		item.length = entity.length();
		item.extra = entity.data().toStdString();
		data.entities.push_back(std::move(item));
	}
	dispatch(*chat, std::move(data), std::nullopt);
}

QString Manager::Impl::mediaDir(int chatId) const {
	return QString("./tdata/ayu_secret/%1").arg(chatId);
}

QString Manager::Impl::storeFile(
		int chatId,
		int64_t randomId,
		const Bytes &plain) const {
	const auto path = QString("%1/%2.fge").arg(mediaDir(chatId)).arg(randomId);
	if (!Vault::SealToFile(path, plain, FileContext(userId, chatId, randomId))) {
		return QString();
	}
	return QDir(path).absolutePath();
}

QString Manager::Impl::tempDir() const {
	return QString("./tdata/ayu_secret_tmp");
}

void Manager::Impl::sendFile(int chatId, OutgoingFile outgoing) {
	const auto chat = find(chatId);
	if (!chat || chat->row.state != int(ChatState::Ready)) {
		return;
	}
	auto plain = Bytes();
	auto name = outgoing.name;
	if (!outgoing.path.isEmpty()) {
		auto file = QFile(outgoing.path);
		if (!file.open(QIODevice::ReadOnly)) {
			toast(tr::ayu_SecretToastReadFailed(tr::now));
			return;
		}
		if (file.size() <= 0) {
			toast(tr::ayu_SecretToastReadFailed(tr::now));
			return;
		} else if (file.size() > kMaxFileSize) {
			toast(tr::ayu_SecretToastTooBig(tr::now));
			return;
		}
		plain = FromArray(file.readAll());
		file.close();
		if (name.isEmpty()) {
			name = QFileInfo(outgoing.path).fileName();
		}
	} else {
		if (outgoing.bytes.isEmpty()) {
			toast(tr::ayu_SecretToastReadFailed(tr::now));
			return;
		} else if (outgoing.bytes.size() > kMaxFileSize) {
			toast(tr::ayu_SecretToastTooBig(tr::now));
			return;
		}
		plain = FromArray(outgoing.bytes);
	}
	auto mime = outgoing.mime;
	if (mime.isEmpty()) {
		mime = outgoing.path.isEmpty()
			? QMimeDatabase().mimeTypeForData(outgoing.bytes).name()
			: QMimeDatabase().mimeTypeForFile(QFileInfo(outgoing.path)).name();
	}
	auto media = Media();
	media.size = int64(plain.size());
	media.mime = mime.toStdString();
	media.fileName = name.toStdString();
	media.caption = outgoing.caption.toStdString();
	auto image = QImage();
	if (outgoing.kind == MediaType::None && mime.startsWith("image/")) {
		image.loadFromData(ToArray(plain));
	}
	if (outgoing.kind == MediaType::Voice) {
		media.type = MediaType::Voice;
		media.duration = outgoing.duration;
		media.waveform = FromArray(outgoing.waveform);
	} else if (outgoing.kind == MediaType::Video) {
		media.type = MediaType::Video;
		media.duration = outgoing.duration;
		media.width = outgoing.width;
		media.height = outgoing.height;
		media.round = outgoing.round;
	} else if (outgoing.kind == MediaType::Sticker) {
		media.type = MediaType::Sticker;
		media.emoji = outgoing.emoji.toStdString();
		media.width = outgoing.width;
		media.height = outgoing.height;
	} else if (outgoing.kind == MediaType::Animation) {
		media.type = MediaType::Animation;
		media.animated = true;
		media.duration = outgoing.duration;
		media.width = outgoing.width;
		media.height = outgoing.height;
	} else if (!image.isNull() && mime != "image/gif") {
		media.type = MediaType::Photo;
		media.width = image.width();
		media.height = image.height();
		const auto thumb = image.scaled(
			90,
			90,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
		auto bytes = QByteArray();
		auto buffer = QBuffer(&bytes);
		buffer.open(QIODevice::WriteOnly);
		thumb.save(&buffer, "JPEG", 80);
		media.thumb = FromArray(bytes);
		media.thumbWidth = thumb.width();
		media.thumbHeight = thumb.height();
	} else if (mime.startsWith("video/")) {
		media.type = MediaType::Video;
		media.duration = outgoing.duration;
		media.width = outgoing.width;
		media.height = outgoing.height;
	} else if (mime.startsWith("audio/")) {
		media.type = MediaType::Audio;
		media.duration = outgoing.duration;
	} else if (!image.isNull() && mime == "image/gif") {
		media.type = MediaType::Animation;
		media.width = image.width();
		media.height = image.height();
	} else {
		media.type = MediaType::Document;
	}
	const auto caption = outgoing.caption;
	const auto key = GenerateFileKey();
	media.key = key.key;
	media.iv = key.iv;

	auto data = MessageData();
	data.randomId = RandomId();
	data.outgoing = true;
	data.date = base::unixtime::now();
	data.ttl = chat->ttl;
	data.text = caption.toStdString();
	data.replyTo = outgoing.replyTo;
	data.media = media;
	const auto stored = storeFile(chatId, data.randomId, plain);
	if (stored.isEmpty()) {
		toast(tr::ayu_SecretToastStoreFailed(tr::now));
		return;
	}
	data.media.path = stored.toStdString();
	data.state = DeliveryState::Pending;
	const auto randomId = data.randomId;
	chat->transfers[randomId] = Transfer();
	addMessage(*chat, data);

	auto encrypted = std::make_shared<Bytes>(
		EncryptFile(plain, key.key, key.iv));
	auto send = SendFile();
	send.big = (encrypted->size() > size_t(kBigFileSize));
	send.parts = int((encrypted->size() + kPartSize - 1) / kPartSize);
	send.fileId = RandomId();
	send.fingerprint = FileFingerprint(key.key, key.iv);
	uploadParts(chatId, randomId, encrypted, send, 0);
}

void Manager::Impl::uploadParts(
		int chatId,
		int64_t randomId,
		std::shared_ptr<Bytes> encrypted,
		SendFile file,
		int index) {
	const auto chat = find(chatId);
	if (!chat) {
		return;
	}
	if (index >= file.parts) {
		chat->transfers.erase(randomId);
		const auto message = findMessage(*chat, randomId);
		if (!message) {
			return;
		}
		auto data = *message;
		data.media.fileId = 0;
		dispatch(*chat, std::move(data), file);
		return;
	}
	const auto offset = size_t(index) * kPartSize;
	const auto size = std::min<size_t>(kPartSize, encrypted->size() - offset);
	const auto chunk = QByteArray(
		reinterpret_cast<const char*>(encrypted->data() + offset),
		int(size));
	const auto next = [=] {
		if (const auto chat = find(chatId)) {
			chat->transfers[randomId].progress = double(index + 1) / file.parts;
			notify(chatId);
		}
		uploadParts(chatId, randomId, encrypted, file, index + 1);
	};
	const auto fail = [=](const MTP::Error &) {
		const auto chat = find(chatId);
		if (!chat) {
			return;
		}
		chat->transfers.erase(randomId);
		if (const auto message = findMessage(*chat, randomId)) {
			message->state = DeliveryState::Failed;
			updateMessage(*chat, *message);
		}
		toast(tr::ayu_SecretToastUploadFailed(tr::now));
	};
	if (file.big) {
		session->api().request(MTPupload_SaveBigFilePart(
			MTP_long(file.fileId),
			MTP_int(index),
			MTP_int(file.parts),
			MTP_bytes(chunk)
		)).done([=] { next(); }).fail(fail).send();
	} else {
		session->api().request(MTPupload_SaveFilePart(
			MTP_long(file.fileId),
			MTP_int(index),
			MTP_bytes(chunk)
		)).done([=] { next(); }).fail(fail).send();
	}
}

void Manager::Impl::downloadMedia(int chatId, int64 randomId) {
	const auto chat = find(chatId);
	if (!chat || chat->transfers.contains(randomId)) {
		return;
	}
	const auto message = findMessage(*chat, randomId);
	if (!message
		|| message->media.type == MediaType::None
		|| !message->media.fileId) {
		return;
	}
	if (!message->media.path.empty()
		&& QFile::exists(Qs(message->media.path))) {
		return;
	}
	if (message->media.size > kMaxFileSize) {
		++chat->revision;
		notify(chatId);
		toast(tr::ayu_SecretToastTooBig(tr::now));
		return;
	}
	chat->transfers[randomId] = Transfer();
	notify(chatId);
	downloadChunk(chatId, randomId, std::make_shared<Bytes>());
}

void Manager::Impl::downloadChunk(
		int chatId,
		int64_t randomId,
		std::shared_ptr<Bytes> buffer) {
	const auto chat = find(chatId);
	if (!chat) {
		return;
	}
	const auto message = findMessage(*chat, randomId);
	if (!message) {
		chat->transfers.erase(randomId);
		return;
	}
	const auto &media = message->media;
	const auto cloud = media.key.empty();
	const auto total = !media.size
		? int64_t(0)
		: cloud
		? int64_t(media.size)
		: int64_t((media.size + 15) & ~int64_t(15));
	const auto location = cloud
		? MTPInputFileLocation(MTP_inputDocumentFileLocation(
			MTP_long(media.fileId),
			MTP_long(media.accessHash),
			MTP_bytes(QByteArray()),
			MTP_string()))
		: MTPInputFileLocation(MTP_inputEncryptedFileLocation(
			MTP_long(media.fileId),
			MTP_long(media.accessHash)));
	const auto offset = int64_t(buffer->size());
	const auto failed = [=](const QString &reason) {
		if (const auto chat = find(chatId)) {
			chat->transfers.erase(randomId);
			notify(chatId);
		}
		toast(reason.isEmpty()
			? tr::ayu_SecretToastDownloadFailed(tr::now)
			: tr::ayu_SecretToastDownloadFailedReason(
				tr::now,
				lt_reason,
				reason));
	};
	const auto fail = [=](const MTP::Error &error) {
		failed(error.type());
	};
	session->api().request(MTPupload_GetFile(
		MTP_flags(0),
		location,
		MTP_long(offset),
		MTP_int(kDownloadChunk)
	)).done([=](const MTPupload_File &result) {
		result.match([&](const MTPDupload_file &data) {
			const auto bytes = FromArray(data.vbytes().v);
			buffer->insert(buffer->end(), bytes.begin(), bytes.end());
			if (int64_t(buffer->size()) > kMaxFileSize + 16) {
				failed("too big");
				return;
			}
			if (const auto chat = find(chatId)) {
				if (total > 0) {
					chat->transfers[randomId].progress
						= std::min(1., double(buffer->size()) / total);
					notify(chatId);
				}
			}
			const auto done = bytes.empty()
				|| (total > 0 && int64_t(buffer->size()) >= total)
				|| int(bytes.size()) < kDownloadChunk;
			if (done) {
				finishDownload(chatId, randomId, *buffer);
			} else {
				downloadChunk(chatId, randomId, buffer);
			}
		}, [&](const auto &) {
			failed(QString());
		});
	}).fail(fail).toDC(MTP::downloadDcId(
		media.dcId ? media.dcId : session->mtp().mainDcId(),
		0)).send();
}

void Manager::Impl::finishDownload(
		int chatId,
		int64_t randomId,
		const Bytes &buffer) {
	const auto chat = find(chatId);
	if (!chat) {
		return;
	}
	chat->transfers.erase(randomId);
	const auto message = findMessage(*chat, randomId);
	if (!message) {
		return;
	}
	auto plain = Bytes();
	if (message->media.key.empty()) {
		plain = buffer;
		if (message->media.size > 0
			&& int64_t(plain.size()) > message->media.size) {
			plain.resize(size_t(message->media.size));
		}
	} else if (!DecryptFile(
			buffer,
			message->media.key,
			message->media.iv,
			message->media.size,
			plain)) {
		toast(tr::ayu_SecretToastDecryptFailed(tr::now));
		notify(chatId);
		return;
	}
	if (message->media.fileName.empty()) {
		auto suffix = QString();
		switch (message->media.type) {
		case MediaType::Photo: suffix = "jpg"; break;
		case MediaType::Voice: suffix = "ogg"; break;
		case MediaType::Sticker: suffix = "webp"; break;
		case MediaType::Video: suffix = "mp4"; break;
		case MediaType::Animation:
			suffix = Qs(message->media.mime).endsWith("gif") ? "gif" : "mp4";
			break;
		default: suffix = "bin"; break;
		}
		message->media.fileName = QString("file.%1").arg(suffix).toStdString();
	}
	const auto path = storeFile(chatId, randomId, plain);
	if (path.isEmpty()) {
		toast(tr::ayu_SecretToastSaveFailed(tr::now));
		notify(chatId);
		return;
	}
	message->media.path = path.toStdString();
	updateMessage(*chat, *message);
}

void Manager::Impl::startTimer(Chat &chat, MessageData &data, int from) {
	if (data.ttl <= 0 || data.expiresAt > 0 || data.special || data.deleted) {
		return;
	}
	data.expiresAt = int(std::min<int64_t>(
		int64_t(from) + data.ttl,
		std::numeric_limits<int>::max()));
	scheduleExpire();
}

void Manager::Impl::openMessage(int chatId, int64 randomId) {
	const auto chat = find(chatId);
	if (!chat) {
		return;
	}
	const auto message = findMessage(*chat, randomId);
	if (!message || message->outgoing || message->opened) {
		return;
	}
	message->opened = true;
	if (message->ttl > 0) {
		startTimer(*chat, *message, base::unixtime::now());
		sendService(*chat, BuildReadMessages(RandomId(), { randomId }));
	}
	updateMessage(*chat, *message);
}

void Manager::Impl::setTtl(int chatId, int seconds) {
	const auto chat = find(chatId);
	if (!chat || chat->row.state != int(ChatState::Ready)) {
		return;
	}
	chat->ttl = std::max(0, seconds);
	sendService(*chat, BuildSetTtl(RandomId(), chat->ttl));
	addNote(
		*chat,
		tr::ayu_SecretNoteTtlSetByYou(
			tr::now,
			lt_time,
			TtlText(chat->ttl)),
		base::unixtime::now());
	saveChat(*chat);
	notify(chatId);
}

void Manager::Impl::deleteMessages(int chatId, const std::vector<int64_t> &ids) {
	const auto chat = find(chatId);
	if (!chat || ids.empty()) {
		return;
	}
	auto announce = std::vector<int64_t>();
	for (const auto id : ids) {
		const auto message = findMessage(*chat, id);
		if (!message || !message->deleted) {
			announce.push_back(id);
		}
		discardMessage(*chat, id, DeleteOrigin::User);
	}
	if (chat->row.state == int(ChatState::Ready) && !announce.empty()) {
		sendService(*chat, BuildDeleteMessages(RandomId(), announce));
	}
	notify(chatId);
}

void Manager::Impl::clearHistory(int chatId) {
	const auto chat = find(chatId);
	if (!chat) {
		return;
	}
	++chat->revision;
	AyuDatabase::clearSecretMessages(userId, chatId);
	forgetChat(chatId);
	QDir(mediaDir(chatId)).removeRecursively();
	chat->messages.clear();
	chat->loaded = true;
	chat->row.unread = 0;
	if (chat->row.state == int(ChatState::Ready)) {
		sendService(*chat, BuildFlushHistory(RandomId()));
	}
	saveChat(*chat);
	notify(chatId);
}

void Manager::Impl::setTyping(int chatId) {
	const auto chat = find(chatId);
	if (!chat
		|| chat->row.state != int(ChatState::Ready)
		|| !AyuSettings::ghost(session).sendUploadProgress()) {
		return;
	}
	const auto now = crl::now();
	if (chat->typingSent && now - chat->typingSent < kTypingSendEvery) {
		return;
	}
	chat->typingSent = now;
	session->api().request(MTPmessages_SetEncryptedTyping(
		input(*chat),
		MTP_bool(true)
	)).send();
}

void Manager::Impl::markRead(int chatId) {
	const auto chat = find(chatId);
	if (!chat) {
		return;
	}
	loadMessages(*chat);
	auto maxDate = 0;
	auto changed = false;
	const auto now = base::unixtime::now();
	for (auto &message : chat->messages) {
		if (message.outgoing || message.special) {
			continue;
		}
		maxDate = std::max(maxDate, message.date);
		if (message.ttl > 0 && !message.opened
			&& message.media.type == MediaType::None) {
			message.opened = true;
			startTimer(*chat, message, now);
			if (chat->row.state == int(ChatState::Ready)) {
				sendService(*chat, BuildReadMessages(RandomId(), { message.randomId }));
			}
			++chat->revision;
			saveMessage(*chat, message);
			changed = true;
		}
	}
	if (chat->row.unread != 0) {
		chat->row.unread = 0;
		saveChat(*chat);
		changed = true;
	}
	if (maxDate > chat->lastReadSent
		&& chat->row.state == int(ChatState::Ready)
		&& AyuSettings::ghost(session).sendReadMessages()) {
		chat->lastReadSent = maxDate;
		session->api().request(MTPmessages_ReadEncryptedHistory(
			input(*chat),
			MTP_int(maxDate)
		)).send();
	}
	if (changed) {
		notify();
	}
}

void Manager::Impl::expire() {
	const auto now = base::unixtime::now();
	for (auto &[id, chat] : chats) {
		loadMessages(chat);
		auto expired = std::vector<int64_t>();
		for (const auto &message : chat.messages) {
			if (message.expiresAt > 0 && message.expiresAt <= now) {
				expired.push_back(message.randomId);
			}
		}
		for (const auto randomId : expired) {
			discardMessage(chat, randomId, DeleteOrigin::Expired);
		}
		if (!expired.empty()) {
			notify(id);
		}
	}
	scheduleExpire();
}

void Manager::Impl::scheduleExpire() {
	auto next = 0;
	for (auto &[id, chat] : chats) {
		if (!chat.loaded) {
			continue;
		}
		for (const auto &message : chat.messages) {
			if (message.expiresAt > 0
				&& (!next || message.expiresAt < next)) {
				next = message.expiresAt;
			}
		}
	}
	if (!next) {
		expireTimer.cancel();
		return;
	}
	const auto delay = std::max(
		crl::time(next - base::unixtime::now()) * 1000,
		crl::time(500));
	expireTimer.callOnce(delay);
}

void Manager::Impl::ackQts(int qts) {
	if (qts > 0) {
		session->api().request(MTPmessages_ReceivedQueue(
			MTP_int(qts)
		)).send();
	}
}

Manager::Manager(not_null<Main::Session*> session)
: _impl(std::make_unique<Impl>(session)) {
	_bridge = std::make_unique<Bridge>(session, this);
	_impl->started = [=](int chatId) {
		_bridge->openChat(chatId);
	};
}

Manager::~Manager() = default;

void Manager::handleUpdate(const MTPUpdate &update) {
	switch (update.type()) {
	case mtpc_updateEncryption:
		_impl->applyChat(update.c_updateEncryption().vchat());
		break;
	case mtpc_updateNewEncryptedMessage: {
		const auto &data = update.c_updateNewEncryptedMessage();
		_impl->handleEncrypted(data.vmessage());
		_impl->ackQts(data.vqts().v);
	} break;
	case mtpc_updateEncryptedChatTyping: {
		const auto &data = update.c_updateEncryptedChatTyping();
		if (const auto chat = _impl->find(data.vchat_id().v)) {
			chat->typingUntil = crl::now() + kTypingTimeout;
			_impl->notify(chat->row.chatId);
			const auto id = chat->row.chatId;
			base::call_delayed(kTypingTimeout + 100, _impl->session, [=] {
				_impl->notify(id);
			});
		}
	} break;
	case mtpc_updateEncryptedMessagesRead: {
		const auto &data = update.c_updateEncryptedMessagesRead();
		if (const auto chat = _impl->find(data.vchat_id().v)) {
			_impl->loadMessages(*chat);
			const auto maxDate = data.vmax_date().v;
			const auto readAt = data.vdate().v;
			for (auto &message : chat->messages) {
				if (!message.outgoing
					|| message.special
					|| message.date > maxDate
					|| message.state == DeliveryState::Read
					|| message.state == DeliveryState::Failed) {
					continue;
				}
				message.state = DeliveryState::Read;
				if (message.media.type == MediaType::None) {
					_impl->startTimer(*chat, message, readAt);
				}
				++chat->revision;
				_impl->saveMessage(*chat, message);
			}
			_impl->notify(chat->row.chatId);
		}
	} break;
	default:
		break;
	}
}

void Manager::handleDifference(
		const QVector<MTPEncryptedMessage> &messages,
		int qts) {
	for (const auto &message : messages) {
		_impl->handleEncrypted(message);
	}
	if (!messages.isEmpty()) {
		_impl->ackQts(qts);
	}
}

ChatInfo Manager::Impl::makeInfo(const Chat &chat, bool withKey) const {
	auto info = ChatInfo();
	info.id = chat.row.chatId;
	info.peerUserId = uint64(chat.row.peerUserId);
	info.creator = (chat.row.creator != 0);
	info.state = ChatState(chat.row.state);
	info.date = chat.row.date;
	info.lastDate = chat.row.lastDate;
	info.unread = chat.row.unread;
	info.ttl = chat.ttl;
	info.layer = chat.row.hisLayer;
	info.typing = (chat.typingUntil > crl::now());
	info.fingerprint = chat.row.fingerprint;
	if (withKey && !chat.key.empty()) {
		info.keyHash = KeyVisualHash(chat.key);
	}
	info.title = title(chat);
	return info;
}

std::vector<ChatInfo> Manager::chats() const {
	auto result = std::vector<ChatInfo>();
	result.reserve(_impl->chats.size());
	for (const auto &[id, chat] : _impl->chats) {
		result.push_back(_impl->makeInfo(chat, false));
	}
	std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
		return a.lastDate > b.lastDate;
	});
	return result;
}

std::optional<ChatInfo> Manager::chat(int chatId, bool withKey) const {
	if (const auto chat = _impl->find(chatId)) {
		return _impl->makeInfo(*chat, withKey);
	}
	return std::nullopt;
}

std::vector<MessageData> Manager::messages(int chatId, int limit) {
	auto result = std::vector<MessageData>();
	if (const auto chat = _impl->find(chatId)) {
		_impl->loadMessages(*chat);
		for (auto i = chat->messages.rbegin();
			i != chat->messages.rend();
			++i) {
			if (i->special == kSpecialHidden) {
				continue;
			}
			result.push_back(*i);
			result.back().object.clear();
			if (limit > 0 && int(result.size()) >= limit) {
				break;
			}
		}
		std::reverse(result.begin(), result.end());
	}
	return result;
}

int Manager::revision(int chatId) const {
	if (const auto chat = _impl->find(chatId)) {
		return chat->revision;
	}
	return -1;
}

int Manager::pendingRequests() const {
	auto result = 0;
	for (const auto &[id, chat] : _impl->chats) {
		if (chat.row.state == int(ChatState::Requested)) {
			++result;
		}
	}
	return result;
}

int Manager::unreadTotal() const {
	auto result = 0;
	for (const auto &[id, chat] : _impl->chats) {
		result += chat.row.unread;
	}
	return result;
}

double Manager::progress(int chatId, int64 randomId) const {
	if (const auto chat = _impl->find(chatId)) {
		const auto i = chat->transfers.find(randomId);
		if (i != chat->transfers.end()) {
			return i->second.progress;
		}
	}
	return -1.;
}

void Manager::accept(int chatId) {
	_impl->accept(chatId);
}

void Manager::decline(int chatId) {
	_impl->discard(chatId);
}

void Manager::discard(int chatId) {
	_impl->discard(chatId);
}

void Manager::start(not_null<UserData*> user) {
	_impl->start(user);
}

void Manager::end(int chatId) {
	const auto chat = _impl->find(chatId);
	if (!chat) {
		return;
	}
	if (chat->row.state == int(ChatState::Ready)) {
		_impl->discard(chatId);
		_impl->addNote(
			*chat,
			tr::ayu_SecretNoteEndedByYou(tr::now),
			base::unixtime::now());
		_impl->notify(chatId);
		return;
	}
	remove(chatId);
}

void Manager::purge() {
	auto ids = std::vector<int>();
	for (const auto &[id, chat] : _impl->chats) {
		ids.push_back(id);
	}
	for (const auto id : ids) {
		remove(id);
	}
}

void Manager::remove(int chatId) {
	const auto chat = _impl->find(chatId);
	if (!chat) {
		return;
	}
	if (chat->row.state != int(ChatState::Discarded)) {
		_impl->discard(chatId);
	}
	AyuDatabase::removeSecretChat(_impl->userId, chatId);
	_impl->forgetChat(chatId);
	QDir(_impl->mediaDir(chatId)).removeRecursively();
	_impl->removed.insert(chatId);
	_impl->chats.erase(chatId);
	_impl->notify(chatId);
}

void Manager::sendText(int chatId, TextWithEntities text, int64 replyTo) {
	_impl->sendText(chatId, std::move(text), replyTo);
}

void Manager::sendFile(int chatId, OutgoingFile outgoing) {
	_impl->sendFile(chatId, std::move(outgoing));
}

void Manager::downloadMedia(int chatId, int64 randomId) {
	_impl->downloadMedia(chatId, randomId);
}

std::optional<MessageData> Manager::message(int chatId, int64 randomId) {
	if (const auto chat = _impl->find(chatId)) {
		_impl->loadMessages(*chat);
		if (const auto found = _impl->findMessage(*chat, randomId)) {
			return *found;
		}
	}
	return std::nullopt;
}

QByteArray Manager::readFile(int chatId, int64 randomId) {
	const auto data = message(chatId, randomId);
	if (!data || data->media.path.empty()) {
		return QByteArray();
	}
	if (const auto i = _impl->plainCache.find({ chatId, randomId })
		; i != _impl->plainCache.end()) {
		return i->second;
	}
	auto plain = Bytes();
	auto legacy = false;
	const auto context = FileContext(_impl->userId, chatId, randomId);
	if (!Vault::OpenFromFile(Qs(data->media.path), plain, context, &legacy)) {
		return QByteArray();
	} else if (legacy) {
		[[maybe_unused]] const auto resealed = Vault::SealToFile(
			Qs(data->media.path),
			plain,
			context);
	}
	auto result = ToArray(plain);
	_impl->remember(chatId, randomId, result);
	return result;
}

void Manager::openMessage(int chatId, int64 randomId) {
	_impl->openMessage(chatId, randomId);
}

void Manager::setTtl(int chatId, int seconds) {
	_impl->setTtl(chatId, seconds);
}

void Manager::deleteMessages(int chatId, const std::vector<int64_t> &randomIds) {
	_impl->deleteMessages(chatId, randomIds);
}

void Manager::clearHistory(int chatId) {
	_impl->clearHistory(chatId);
}

void Manager::setTyping(int chatId) {
	_impl->setTyping(chatId);
}

void Manager::markRead(int chatId) {
	_impl->markRead(chatId);
}

void Manager::setOpenChat(int chatId) {
	_impl->openChat = chatId;
}

rpl::producer<> Manager::changes() const {
	return _impl->changes.events();
}

rpl::producer<int> Manager::messageChanges() const {
	return _impl->messageChanges.events();
}

StoredState LoadState(not_null<Main::Session*> session) {
	const auto rows = AyuDatabase::getSecretState(
		ID(session->userId().bare & PeerId::kChatTypeMask));
	return rows.empty()
		? StoredState()
		: StoredState{ .qts = rows.front().qts, .date = rows.front().date };
}

void SaveState(not_null<Main::Session*> session, int qts, int date) {
	const auto userId = ID(session->userId().bare & PeerId::kChatTypeMask);
	const auto was = LoadState(session);
	if (qts < was.qts || (qts == was.qts && date <= was.date)) {
		return;
	}
	auto row = SecretStateRow();
	row.fakeId = 0;
	row.userId = userId;
	row.qts = qts;
	row.date = date;
	AyuDatabase::saveSecretState(row);
}

namespace {

constexpr auto kDateSaveEvery = crl::time(60 * 1000);

struct DateState {
	bool hold = true;
	int pending = 0;
	base::Timer timer;
};

std::map<Main::Session*, std::unique_ptr<DateState>> DateStates;

[[nodiscard]] DateState &DateStateFor(not_null<Main::Session*> session) {
	const auto i = DateStates.find(session.get());
	if (i != DateStates.end()) {
		return *i->second;
	}
	const auto raw = session.get();
	auto &state = *DateStates.emplace(
		raw,
		std::make_unique<DateState>()).first->second;
	state.timer.setCallback([=] {
		const auto j = DateStates.find(raw);
		if (j == DateStates.end() || j->second->hold || !j->second->pending) {
			return;
		}
		const auto date = base::take(j->second->pending);
		SaveState(raw, LoadState(raw).qts, date);
	});
	raw->lifetime().add([=] {
		DateStates.erase(raw);
	});
	return state;
}

} // namespace

CatchUp CatchUpRange(
		not_null<Main::Session*> session,
		int serverQts,
		int serverDate) {
	auto &state = DateStateFor(session);
	const auto stored = LoadState(session);
	if (!stored.qts) {
		SaveState(session, serverQts, serverDate);
		state.hold = false;
		state.pending = 0;
		state.timer.callEach(kDateSaveEvery);
		return {};
	}
	if (stored.qts >= serverQts && stored.date >= serverDate) {
		state.hold = false;
		state.pending = 0;
		state.timer.callEach(kDateSaveEvery);
		return {};
	}
	state.hold = true;
	return { .missed = true, .qts = stored.qts, .date = stored.date };
}

void CatchUpFinished(not_null<Main::Session*> session) {
	auto &state = DateStateFor(session);
	state.hold = false;
	state.pending = 0;
	state.timer.callEach(kDateSaveEvery);
}

void NoteDate(not_null<Main::Session*> session, int date) {
	auto &state = DateStateFor(session);
	if (!state.hold && date > state.pending) {
		state.pending = date;
	}
}

void PurgeSession(not_null<Main::Session*> session) {
	Get(session).purge();
}

Manager &Get(not_null<Main::Session*> session) {
	static auto managers = std::map<
		Main::Session*,
		std::unique_ptr<Manager>>();
	const auto i = managers.find(session.get());
	if (i != managers.end()) {
		return *i->second;
	}
	const auto raw = session.get();
	auto &result = managers.emplace(
		raw,
		std::make_unique<Manager>(session)).first->second;
	session->lifetime().add([raw] {
		managers.erase(raw);
	});
	return *result;
}

} // namespace AyuSecret
