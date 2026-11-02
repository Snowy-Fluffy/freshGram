// This is the source code of freshGram.
#include "ayu/features/plugins/plugins_service.h"

#include "ayu/ayu_settings.h"
#include "ayu/data/fresh_vault.h"
#include "ayu/features/filters/filters_controller.h"
#include "ayu/features/plugins/plugins_control.h"
#include "ayu/features/plugins/plugins_data.h"
#include "ayu/features/plugins/plugins_runtime.h"
#include "ayu/features/plugins/plugins_transport.h"
#include "ayu/features/plugins/plugins_ui.h"
#include "ayu/secret/secret_peer.h"
#include "api/api_common.h"
#include "apiwrap.h"
#include "crl/crl_on_main.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_send_action.h"
#include "data/data_user.h"
#include "data/data_drafts.h"
#include "data/data_forum_topic.h"
#include "dialogs/dialogs_key.h"
#include "window/window_session_controller.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "storage/localimageloader.h"
#include "ui/chat/attach/attach_prepare.h"
#include "base/unixtime.h"
#include "calls/calls_call.h"
#include "calls/calls_instance.h"
#include "data/data_stories.h"
#include "lang/lang_keys.h"
#include "ui/toast/toast.h"
#include "base/flat_set.h"
#include "base/timer.h"
#include "rpl/combine.h"
#include "core/application.h"
#include "core/version.h"
#include "platform/platform_specific.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "settings.h"
#include "storage/storage_account.h"
#include "storage/storage_domain.h"

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>

#ifdef FRESHGRAM_PLUGINS
#include "fgpl/manager.h"
#include "fgpl/runtime_bundle.h"
#include "fgpl/os.h"
#include "fgpl/sandbox.h"
#include "fgpl/semver.h"
#endif

#include <cctype>
#include <deque>
#include <map>

namespace FreshPlugins {
namespace {

constexpr auto kOutgoingTimeoutMs = 1000;
constexpr auto kRememberedMessages = size_t(20000);
constexpr auto kFreshEditSeconds = TimeId(300);
constexpr auto kTypingEachMs = crl::time(3000);
constexpr auto kMaxRememberedActivity = size_t(512);

#ifdef FRESHGRAM_PLUGINS

struct Seen {
	TimeId editDate = 0;
	size_t textHash = 0;
};

struct Events {
	base::flat_set<FullMsgId> sending;
	base::flat_set<FullMsgId> byPlugin;
	std::map<FullMsgId, Seen> seen;
	std::deque<FullMsgId> seenOrder;
	std::map<FullMsgId, Seen> incoming;
	std::deque<FullMsgId> incomingOrder;
	base::flat_set<FullMsgId> deleted;
	std::deque<FullMsgId> deletedOrder;
	std::map<std::tuple<PeerId, PeerId, QString, bool>, crl::time> typing;
	std::map<std::pair<PeerId, bool>, MsgId> readTill;
	std::map<PeerId, std::pair<std::string, int64_t>> statuses;
	std::map<PeerId, QString> titles;
};

struct Watched {
	std::string id;
	rpl::lifetime lifetime;
	rpl::lifetime events;
};

// A send of files waits in the same queue as the texts, with its caption in the message.
struct OutgoingFiles {
	Ui::PreparedList list;
	SendMediaType type = SendMediaType();
	std::shared_ptr<SendingAlbum> album;
};

struct Outgoing {
	Api::MessageToSend message;
	std::optional<MsgId> localMessageId;
	bool byPlugin = false;
	std::unique_ptr<OutgoingFiles> files;
};

struct Service {
	std::unique_ptr<Control> control;
	std::map<Main::Account*, Watched> watched;
	std::map<not_null<History*>, std::deque<Outgoing>> outgoing;
	bool sending = false;
	rpl::variable<State> state = State::Disabled;
	rpl::event_stream<> list;
	rpl::lifetime lifetime;
	fgpl::os::Handle hostFd = fgpl::os::kNoHandle;
	int revision = 0;
	bool started = false;
};

[[nodiscard]] Service &GetService() {
	static auto result = Service();
	return result;
}

[[nodiscard]] QString HostBinary() {
	return RuntimePath() + u"/"_q + QString::fromLatin1(fgpl::os::kHostFileName);
}

[[nodiscard]] std::optional<fgpl::InstalledRuntime> Installed() {
	return TrustedRuntime();
}

[[nodiscard]] bool DeriveSecret(const char *label, std::array<uint8_t, 32> &out) {
	return FreshVault::DeriveKey(label, out.data(), out.size());
}

[[nodiscard]] std::optional<fgpl::ManagerConfig> MakeConfig(
		Service &service) {
	auto config = fgpl::ManagerConfig();
	config.root = PathOf(StorePath());
	config.host.path = HostBinary().toStdString();
	config.services.dataFor = &DataFor;
	config.services.transport = NetworkTransport();
	config.services.settingsPage = [](
			const std::string &id,
			const std::string &account,
			const fgpl::cbor::Value &page) {
		crl::on_main([=] {
			StoreSettingsPage(id, account, page);
		});
	};
	config.services.openSettings = [](
			const std::string &id,
			const std::string &account) {
		crl::on_main([=] {
			OpenPluginSettings(id, account);
		});
	};
	config.services.badges = [](
			const std::string &id,
			const std::string &account,
			int64_t user,
			const fgpl::cbor::Value *items) {
		auto copy = items ? std::make_optional(*items) : std::nullopt;
		crl::on_main([=] {
			StoreBadges(id, account, user, copy ? &*copy : nullptr);
		});
	};
	config.services.dialog = [](
			const std::string &id,
			const std::string &account,
			const fgpl::cbor::Value *page) {
		auto copy = page ? std::make_optional(*page) : std::nullopt;
		crl::on_main([=] {
			ShowPluginDialog(id, account, copy ? &*copy : nullptr);
		});
	};
	config.services.window = [](
			const std::string &id,
			const std::string &account,
			const std::string &window,
			const std::string &mode,
			const fgpl::cbor::Value *page) {
		auto copy = page ? std::make_optional(*page) : std::nullopt;
		crl::on_main([=] {
			ShowPluginWindow(id, account, window, mode, copy ? &*copy : nullptr);
		});
	};

	const auto installed = Installed();
	if (!installed) {
		return std::nullopt;
	}
	auto sealed = fgpl::SealExecutable(
		config.host.path,
		installed->hostHash);
	if (!sealed.ok()) {
		LOG(("Plugins Error: %1").arg(QString::fromStdString(sealed.error())));
		return std::nullopt;
	}
	service.hostFd = sealed.value();
	service.revision = installed->revision;
	config.host.executableFd = service.hostFd;
	const auto runtime = PathOf(RuntimePath());
	config.python.expectedHash = installed->pythonHash;
	config.python.wasm = runtime / "python" / "python.wasm";
	config.python.library = runtime / "python" / "lib";
	config.python.sdk = runtime / "sdk";

	config.cache.directory = PathOf(cWorkingDir() + u"tdata/plugins/cache"_q);
	if (!DeriveSecret("freshgram.plugins.cache", config.cache.secret)
		|| !DeriveSecret("freshgram.plugins.state", config.stateKey)) {
		return std::nullopt;
	}

	config.environment.app = *fgpl::ParseVersion(AppVersionStr);
	config.environment.apiLevel = 8;
	if (!Platform::TrayIconSupported()) {
		config.environment.features.erase("menu.tray");
	}
	return config;
}

[[nodiscard]] State Compute() {
	if (!AyuSettings::getInstance().pluginsEnabled()) {
		return State::Disabled;
	} else if (!SandboxSupported()) {
		return State::NoSandbox;
	} else if (!Installed()) {
		return State::NoRuntime;
	} else if (!FreshVault::HasKey()) {
		return State::NoKey;
	}
	return State::Ready;
}

void StartAccounts(Service &service) {
	if (!service.control || !Core::App().domain().started()) {
		return;
	}
	for (const auto &[account, watched] : service.watched) {
		if (!watched.id.empty()) {
			Post([id = watched.id](not_null<fgpl::PluginManager*> manager) {
				manager->startEnabled(id);
			});
		}
	}
}

void DropManager(Service &service) {
	if (service.control) {
		CloseData();
		service.control = nullptr;
		ReleaseData();
	}
	if (service.hostFd != fgpl::os::kNoHandle) {
		fgpl::os::CloseExecutable(service.hostFd);
		service.hostFd = fgpl::os::kNoHandle;
	}
	service.revision = 0;
}

void Refresh() {
	auto &service = GetService();
	const auto wanted = Compute();
	if (wanted != State::Ready) {
		DropManager(service);
	} else if (service.control) {
		const auto installed = Installed();
		if (!installed || installed->revision != service.revision) {
			DropManager(service);
		}
	}
	if (wanted == State::Ready && !service.control) {
		if (auto config = MakeConfig(service)) {
			OpenData();
			service.control = std::make_unique<Control>(std::move(*config), [] {
				GetService().list.fire({});
			});
			StartAccounts(service);
		} else {
			DropManager(service);
			service.state = FreshVault::HasKey() ? State::NoRuntime : State::NoKey;
			return;
		}
	}
	service.state = wanted;
	service.list.fire({});
}

void ForgetAccount(Service &service, Main::Account *account) {
	const auto i = service.watched.find(account);
	if (i == end(service.watched)) {
		return;
	}
	const auto id = i->second.id;
	service.watched.erase(i);
	if (!id.empty()) {
		Post([=](not_null<fgpl::PluginManager*> manager) {
			manager->onLogout(id);
		});
	}
}

[[nodiscard]] bool Reportable(not_null<const HistoryItem*> item) {
	return item->isRegular()
		&& IsServerMsgId(item->id)
		&& !item->isService()
		&& !item->history()->peer->isSelf();
}

[[nodiscard]] bool ReportableService(not_null<const HistoryItem*> item) {
	return item->isRegular()
		&& IsServerMsgId(item->id)
		&& item->isService()
		&& !item->history()->peer->isSelf();
}

[[nodiscard]] Seen SeenOf(not_null<const HistoryItem*> item) {
	const auto edited = item->Get<HistoryMessageEdited>();
	return {
		.editDate = edited ? edited->date : TimeId(0),
		.textHash = size_t(qHash(item->originalText().text)),
	};
}

template <typename Set>
void Remember(
		Set &set,
		std::deque<FullMsgId> &order,
		FullMsgId id) {
	order.push_back(id);
	while (order.size() > kRememberedMessages) {
		set.erase(order.front());
		order.pop_front();
	}
}

void Note(Events &events, not_null<const HistoryItem*> item) {
	const auto id = item->fullId();
	if (!events.seen.contains(id)) {
		Remember(events.seen, events.seenOrder, id);
	}
	events.seen[id] = SeenOf(item);
}

void DispatchActivity(
		const std::string &account,
		const char *type,
		fgpl::cbor::Map fields) {
	if (!Running()) {
		return;
	}
	using Value = fgpl::cbor::Value;
	fields.insert(fields.begin(), { "type", Value::FromText(type) });
	Post([=, event = Value::FromMap(std::move(fields))](
			not_null<fgpl::PluginManager*> manager) {
		manager->dispatch(account, event);
	}, false);
}

[[nodiscard]] bool Hidden(not_null<PeerData*> peer) {
	return FreshSecret::IsSecretPeerId(peer->id)
		|| peer->isSelf()
		|| peer->id.value == fgpl::api::kServiceChat
		|| FiltersController::isBlocked(peer);
}

template <typename Map>
void Trim(Map &map) {
	while (map.size() > kMaxRememberedActivity) {
		map.erase(map.begin());
	}
}

void ApplyIncoming(
		not_null<Main::Session*> session,
		FullMsgId id,
		const fgpl::IncomingResult &result) {
	const auto item = session->data().message(id);
	if (!item) {
		return;
	}
	SetHiddenByPlugin(item, result.hidden, true);
	SetHiddenByPlugin(item, result.hidden, false);
	if (result.hidden) {
		return;
	}
	if (!result.text && !result.entities) {
		item->setPluginDisplay(std::nullopt);
		return;
	}
	item->setPluginDisplay(DisplayTextOf(
		session,
		result.text
			? QString::fromStdString(*result.text)
			: item->originalText().text,
		result.entities.value_or(std::vector<fgpl::TextEntity>())));
}

void RunIncoming(
		not_null<Main::Session*> session,
		const std::string &account,
		not_null<HistoryItem*> item,
		not_null<Events*> events) {
	const auto service = item->isService();
	if (service
		? !ReportableService(item)
		: (item->out() || !Reportable(item))) {
		return;
	} else if (Hidden(item->history()->peer)) {
		return;
	}
	const auto snapshot = Snapshot();
	const auto fullId = item->fullId();
	const auto now = SeenOf(item);
	const auto known = events->incoming.find(fullId);
	const auto edited = (known != end(events->incoming));
	if (edited && known->second.editDate == now.editDate
		&& known->second.textHash == now.textHash) {
		return;
	} else if (!edited && now.editDate && now.editDate
			<= base::unixtime::now() - kFreshEditSeconds) {
		return;
	} else if (!snapshot) {
		return;
	}
	auto info = IncomingInfoOf(item, edited);
	const auto mode = snapshot->transformsIncoming(info);
	if (mode == fgpl::IncomingMode::None) {
		return;
	}
	if (!edited) {
		Remember(events->incoming, events->incomingOrder, fullId);
	}
	events->incoming[fullId] = now;
	const auto hold = (mode == fgpl::IncomingMode::Hold);
	if (hold) {
		SetHiddenByPlugin(item, true, true);
	}
	const auto weak = base::make_weak(session);
	const auto posted = Post([=](not_null<fgpl::PluginManager*> manager) {
		manager->transformIncomingAsync(
			account,
			info,
			kOutgoingTimeoutMs,
			[=](fgpl::IncomingResult result) {
				crl::on_main([=] {
					if (const auto strong = weak.get()) {
						ApplyIncoming(strong, fullId, result);
					}
				});
			});
	}, false);
	if (!posted && hold) {
		SetHiddenByPlugin(item, false, true);
	}
}

void WatchActivity(
		not_null<Main::Session*> session,
		const std::string &id,
		not_null<Events*> events,
		rpl::lifetime &lifetime) {
	using Value = fgpl::cbor::Value;
	const auto ghost = [=] {
		return AyuSettings::ghost(session).isGhostModeActive();
	};

	session->data().sendActionManager().activity(
	) | rpl::on_next([=](const Data::SendActionManager::Activity &activity) {
		if (!WantsEvent("chat.typing")) {
			return;
		}
		const auto peer = activity.history->peer;
		if (Hidden(peer) || FiltersController::isBlocked(activity.user)) {
			return;
		}
		const auto key = std::make_tuple(
			peer->id,
			activity.user->id,
			activity.kind,
			activity.cancelled);
		const auto now = crl::now();
		auto &last = events->typing[key];
		if (last && now - last < kTypingEachMs) {
			return;
		}
		last = now;
		Trim(events->typing);
		auto fields = fgpl::cbor::Map{
			{ "chat", Value::FromInt(int64_t(peer->id.value)) },
			{ "user", Value::FromInt(int64_t(activity.user->id.value)) },
			{ "action", Value::FromText(activity.kind.toStdString()) },
			{ "cancelled", Value::FromBool(activity.cancelled) },
		};
		if (activity.rootId) {
			fields.push_back({ "topic", Value::FromInt(activity.rootId.bare) });
		}
		DispatchActivity(id, "chat.typing", std::move(fields));
	}, lifetime);

	session->changes().historyUpdates(
		Data::HistoryUpdate::Flag::OutboxRead | Data::HistoryUpdate::Flag::InboxRead
	) | rpl::on_next([=](const Data::HistoryUpdate &update) {
		if (!WantsEvent("chat.read")) {
			return;
		}
		const auto history = update.history;
		if (Hidden(history->peer)) {
			return;
		}
		const auto notify = [&](bool outbox) {
			if (!outbox && ghost()) {
				return;
			}
			const auto till = outbox
				? history->outboxReadTillId()
				: history->inboxReadTillId();
			auto &last = events->readTill[{ history->peer->id, outbox }];
			if (till <= last) {
				return;
			}
			last = till;
			Trim(events->readTill);
			DispatchActivity(id, "chat.read", {
				{ "chat", Value::FromInt(int64_t(history->peer->id.value)) },
				{ "read_till", Value::FromInt(till.bare) },
				{ "outbox", Value::FromBool(outbox) },
			});
		};
		if (update.flags & Data::HistoryUpdate::Flag::OutboxRead) {
			notify(true);
		}
		if (update.flags & Data::HistoryUpdate::Flag::InboxRead) {
			notify(false);
		}
	}, lifetime);

	const auto member = [=](
			not_null<PeerData*> peer,
			not_null<UserData*> user,
			bool joined) {
		if (!WantsEvent("chat.member")
			|| Hidden(peer)
			|| FiltersController::isBlocked(user)) {
			return;
		}
		DispatchActivity(id, "chat.member", {
			{ "chat", Value::FromInt(int64_t(peer->id.value)) },
			{ "user", Value::FromInt(int64_t(user->id.value)) },
			{ "joined", Value::FromBool(joined) },
		});
	};
	session->data().chatMemberChanged(
	) | rpl::on_next([=](const Data::Session::ChatMember &change) {
		member(change.chat, change.user, change.joined);
	}, lifetime);
	session->data().megagroupParticipantAdded(
	) | rpl::on_next([=](const Data::Session::MegagroupParticipant &change) {
		member(std::get<0>(change), std::get<1>(change), true);
	}, lifetime);
	session->data().megagroupParticipantRemoved(
	) | rpl::on_next([=](const Data::Session::MegagroupParticipant &change) {
		member(std::get<0>(change), std::get<1>(change), false);
	}, lifetime);

	session->changes().peerUpdates(
		Data::PeerUpdate::Flag::OnlineStatus
	) | rpl::on_next([=](const Data::PeerUpdate &update) {
		if (!WantsEvent("user.status")) {
			return;
		}
		const auto user = update.peer->asUser();
		if (!user || user->isSelf() || user->isBot() || FiltersController::isBlocked(user)) {
			return;
		}
		auto lastSeen = int64_t(0);
		auto status = UserStatus(user, lastSeen);
		if (status == "hidden" || status == "bot") {
			return;
		}
		const auto now = std::make_pair(status, lastSeen);
		auto &known = events->statuses[user->id];
		if (known == now) {
			return;
		}
		known = now;
		Trim(events->statuses);
		auto fields = fgpl::cbor::Map{
			{ "user", Value::FromInt(int64_t(user->id.value)) },
			{ "status", Value::FromText(status) },
		};
		if (lastSeen) {
			fields.push_back({ "last_seen", Value::FromInt(lastSeen) });
		}
		DispatchActivity(id, "user.status", std::move(fields));
	}, lifetime);

	session->changes().peerUpdates(
		Data::PeerUpdate::Flag::Name
	) | rpl::on_next([=](const Data::PeerUpdate &update) {
		const auto peer = update.peer;
		if (!WantsEvent("chat.title_changed")
			|| peer->isUser()
			|| Hidden(peer)) {
			return;
		}
		const auto title = peer->name();
		const auto i = events->titles.find(peer->id);
		const auto known = (i != end(events->titles));
		if (known && i->second == title) {
			return;
		}
		const auto old = known ? i->second : QString();
		events->titles[peer->id] = title;
		Trim(events->titles);
		auto fields = fgpl::cbor::Map{
			{ "chat", Value::FromInt(int64_t(peer->id.value)) },
			{ "title", Value::FromText(title.toStdString()) },
		};
		if (known) {
			fields.push_back({ "old_title", Value::FromText(old.toStdString()) });
		}
		DispatchActivity(id, "chat.title_changed", std::move(fields));
	}, lifetime);

	session->data().stories().newStories(
	) | rpl::on_next([=](FullStoryId story) {
		if (!WantsEvent("story.new")) {
			return;
		}
		const auto peer = session->data().peerLoaded(story.peer);
		if (!peer || peer->isSelf() || Hidden(peer)) {
			return;
		}
		DispatchActivity(id, "story.new", {
			{ "chat", Value::FromInt(int64_t(peer->id.value)) },
			{ "story_id", Value::FromInt(int64_t(story.story)) },
			{ "date", Value::FromInt(int64_t(base::unixtime::now())) },
		});
	}, lifetime);

	const auto callLifetime = lifetime.make_state<rpl::lifetime>();
	Core::App().calls().currentCallValue(
	) | rpl::on_next([=](Calls::Call *call) {
		callLifetime->destroy();
		if (!call || &call->user()->session() != session.get()) {
			return;
		}
		const auto user = call->user();
		if (Hidden(user)) {
			return;
		}
		const auto fields = [=](fgpl::cbor::Map more = {}) {
			auto result = fgpl::cbor::Map{
				{ "chat", Value::FromInt(int64_t(user->id.value)) },
				{ "user", Value::FromInt(int64_t(user->id.value)) },
				{ "incoming", Value::FromBool(
					call->type() == Calls::CallType::Incoming) },
			};
			for (auto &field : more) {
				result.push_back(std::move(field));
			}
			return result;
		};
		if (call->type() == Calls::CallType::Incoming
			&& WantsEvent("call.incoming")) {
			DispatchActivity(id, "call.incoming", fields());
		}
		const auto started = std::make_shared<crl::time>(0);
		const auto finished = std::make_shared<bool>(false);
		call->stateValue(
		) | rpl::on_next([=](Calls::Call::State state) {
			using State = Calls::Call::State;
			if (state == State::Established && !*started) {
				*started = crl::now();
				return;
			}
			const auto reason = [&]() -> const char* {
				switch (state) {
				case State::Ended: return "ended";
				case State::EndedByOtherDevice: return "other_device";
				case State::Busy: return "busy";
				case State::Failed:
				case State::FailedHangingUp: return "failed";
				default: return nullptr;
				}
			}();
			if (!reason || *finished || !WantsEvent("call.ended")) {
				return;
			}
			*finished = true;
			DispatchActivity(id, "call.ended", fields({
				{ "reason", Value::FromText(reason) },
				{ "duration", Value::FromInt(*started
					? (crl::now() - *started) / 1000
					: 0) },
			}));
		}, *callLifetime);
	}, lifetime);

	Core::App().appDeactivatedValue(
	) | rpl::skip(1) | rpl::on_next([=](bool deactivated) {
		const auto type = deactivated ? "app.blur" : "app.focus";
		if (WantsEvent(type) && !ghost()) {
			DispatchActivity(id, type, {});
		}
	}, lifetime);
}

void WatchEvents(
		not_null<Main::Session*> session,
		const std::string &id,
		rpl::lifetime &lifetime) {
	const auto events = lifetime.make_state<Events>();
	const auto data = &session->data();
	WatchActivity(session, id, events, lifetime);
	data->newItemAdded(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		if (!IsServerMsgId(item->id)) {
			if (item->out() && item->isLocal()) {
				events->sending.emplace(item->fullId());
				if (PluginSending() || IsPluginMessage(session, item->fullId())) {
					events->byPlugin.emplace(item->fullId());
				}
			}
			return;
		}
		RunIncoming(session, id, item, events);
		if (!WantsEvents() || !Reportable(item)) {
			return;
		}
		Note(*events, item);
		SendMessageEvent("message.new", item, id);
	}, lifetime);
	data->itemIdChanged(
	) | rpl::on_next([=](const Data::Session::IdChange &change) {
		const auto old = FullMsgId(change.newId.peer, change.oldId);
		NoteMessageIdChange(session, old, change.newId.msg);
		const auto sent = events->sending.remove(old);
		const auto byPlugin = events->byPlugin.remove(old);
		const auto item = data->message(change.newId);
		if (!sent || !item || !WantsEvents() || !Reportable(item)) {
			return;
		}
		Note(*events, item);
		SendMessageEvent("message.new", item, id, change.oldId, byPlugin);
	}, lifetime);
	session->changes().messageUpdates(
		Data::MessageUpdate::Flag::Edited
			| Data::MessageUpdate::Flag::Deleted
			| Data::MessageUpdate::Flag::NewUnreadReaction
	) | rpl::on_next([=](const Data::MessageUpdate &update) {
		const auto item = update.item;
		if (update.flags & Data::MessageUpdate::Flag::Edited) {
			RunIncoming(session, id, item, events);
		}
		if (!WantsEvents() || !Reportable(item)) {
			return;
		} else if (update.flags & Data::MessageUpdate::Flag::NewUnreadReaction) {
			SendMessageEvent("message.reaction", item, id);
			return;
		} else if (update.flags & Data::MessageUpdate::Flag::Deleted) {
			if (events->deleted.emplace(item->fullId()).second) {
				Remember(events->deleted, events->deletedOrder, item->fullId());
				SendMessageEvent("message.deleted", item, id);
			}
			return;
		}
		const auto now = SeenOf(item);
		const auto known = events->seen.find(item->fullId());
		const auto changed = (known != end(events->seen))
			? (known->second.editDate != now.editDate
				|| known->second.textHash != now.textHash)
			: (now.editDate > base::unixtime::now() - kFreshEditSeconds);
		if (changed) {
			Note(*events, item);
			SendMessageEvent("message.edited", item, id);
		}
	}, lifetime);
	data->itemPinnedChanges(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		if (WantsEvent("message.pinned") && Reportable(item)) {
			SendMessageEvent("message.pinned", item, id);
		}
	}, lifetime);
	data->itemRemoved(
	) | rpl::on_next([=](not_null<const HistoryItem*> item) {
		const auto fullId = item->fullId();
		events->seen.erase(fullId);
		events->incoming.erase(fullId);
		ForgetHiddenByPlugin(fullId);
		events->sending.remove(fullId);
		events->byPlugin.remove(fullId);
		if (!WantsEvents() || !Reportable(item) || item->isDeleted()) {
			return;
		} else if (events->deleted.emplace(fullId).second) {
			Remember(events->deleted, events->deletedOrder, fullId);
			SendMessageEvent("message.deleted", item, id);
		}
	}, lifetime);
}

void WatchAccount(Service &service, not_null<Main::Account*> account) {
	if (service.watched.contains(account)) {
		return;
	}
	auto &entry = service.watched[account];
	account->sessionValue(
	) | rpl::on_next([=](Main::Session *session) {
		auto &service = GetService();
		const auto i = service.watched.find(account);
		if (i == end(service.watched)) {
			return;
		}
		i->second.events.destroy();
		if (!session) {
			const auto id = std::exchange(i->second.id, std::string());
			if (!id.empty()) {
				Post([=](not_null<fgpl::PluginManager*> manager) {
					manager->onLogout(id);
				});
			}
			return;
		}
		const auto id = AccountId(session).toStdString();
		i->second.id = id;
		Post([=](not_null<fgpl::PluginManager*> manager) {
			manager->startEnabled(id);
		});
		WatchEvents(session, id, i->second.events);
	}, entry.lifetime);
}

void SyncAccounts() {
	auto &service = GetService();
	if (!Core::App().domain().started()) {
		return;
	}
	auto present = base::flat_set<Main::Account*>();
	for (const auto &entry : Core::App().domain().accounts()) {
		present.emplace(entry.account.get());
		WatchAccount(service, entry.account.get());
	}
	auto gone = std::vector<Main::Account*>();
	for (const auto &[account, watched] : service.watched) {
		if (!present.contains(account)) {
			gone.push_back(account);
		}
	}
	for (const auto account : gone) {
		ForgetAccount(service, account);
	}
}

#endif

} // namespace

void Start() {
#ifdef FRESHGRAM_PLUGINS
	auto &service = GetService();
	if (std::exchange(service.started, true)) {
		return;
	}
	AyuSettings::getInstance().pluginsEnabledValue(
	) | rpl::on_next([] {
		Refresh();
	}, service.lifetime);

	auto &settings = AyuSettings::getInstance();
	rpl::combine(
		settings.pluginsBlockHttpsValue(),
		settings.pluginsBlockTcpValue(),
		settings.pluginsBlockUdpValue(),
		settings.pluginsProxyValue()
	) | rpl::on_next([](
			bool blockHttps,
			bool blockTcp,
			bool blockUdp,
			const PluginsProxy &proxy) {
		auto policy = NetworkPolicy{
			.blockHttps = blockHttps,
			.blockTcp = blockTcp,
			.blockUdp = blockUdp,
		};
		if (proxy.enabled && proxy.valid()) {
			policy.proxy = NetworkProxy{
				.type = (proxy.type == PluginsProxy::Type::Http)
					? NetworkProxy::Type::Http
					: NetworkProxy::Type::Socks5,
				.host = proxy.host,
				.port = proxy.port,
				.user = proxy.user,
				.password = proxy.password,
			};
		}
		SetNetworkPolicy(std::move(policy));
	}, service.lifetime);

	Core::App().domain().accountsChanges(
	) | rpl::on_next([] {
		SyncAccounts();
		Refresh();
	}, service.lifetime);

	Core::App().passcodeLockValue(
	) | rpl::on_next([](bool locked) {
		Post([=](not_null<fgpl::PluginManager*> manager) {
			if (locked) {
				manager->onLock();
			} else {
				manager->onUnlock();
			}
		});
		if (!locked) {
			// The domain starts only after the passcode is entered, and no
			// account change is reported for that.
			SyncAccounts();
		}
		Refresh();
	}, service.lifetime);

	SyncAccounts();
	Refresh();
	StartRuntimeUpdates();
#endif
}

#ifdef FRESHGRAM_PLUGINS
namespace {
void RestoreDraft(const Api::MessageToSend &message);
} // namespace
#endif

void Stop() {
#ifdef FRESHGRAM_PLUGINS
	auto &service = GetService();
	service.lifetime.destroy();
	service.watched.clear();
	ReleasePendingHidden();
	for (auto &[history, queue] : base::take(service.outgoing)) {
		for (const auto &outgoing : queue) {
			if (!outgoing.byPlugin && !outgoing.files) {
				RestoreDraft(outgoing.message);
			}
		}
	}
	DropManager(service);
	service.state = State::Disabled;
	service.started = false;
#endif
}

#ifdef FRESHGRAM_PLUGINS

namespace {

[[nodiscard]] std::string OutgoingKindOf(
		SendMediaType type,
		const Ui::PreparedFile &file) {
	using Type = Ui::PreparedFile::Type;
	return (type == SendMediaType::File)
		? "file"
		: (file.type == Type::Photo)
		? "photo"
		: (file.type == Type::Video)
		? "video"
		: (file.type == Type::Music)
		? "audio"
		: "file";
}

[[nodiscard]] std::string OutgoingNameOf(const Ui::PreparedFile &file) {
	return (file.displayName.isEmpty()
		? QFileInfo(file.path).fileName()
		: file.displayName).toStdString();
}

[[nodiscard]] fgpl::OutgoingInfo InfoOf(
		const Api::MessageToSend &message,
		const OutgoingFiles *files = nullptr) {
	const auto &action = message.action;
	const auto peer = action.history->peer;
	auto result = fgpl::OutgoingInfo();
	result.chat = int64_t(peer->id.value);
	result.secret = FreshSecret::IsSecretPeerId(peer->id);
	result.chatType = PeerKind(peer);
	result.topic = action.replyTo.topicRootId.bare;
	result.replyTo = (action.replyTo.messageId.msg == action.replyTo.topicRootId)
		? 0
		: action.replyTo.messageId.msg.bare;
	result.silent = action.options.silent;
	result.scheduled = action.options.scheduled;
	result.text = message.textWithTags.text.toStdString();
	result.entities = EntitiesOf(message.textWithTags);
	if (files) {
		for (const auto &file : files->list.files) {
			result.files.push_back({
				.kind = OutgoingKindOf(files->type, file),
				.name = OutgoingNameOf(file),
				.size = file.size,
			});
		}
	}
	return result;
}

[[nodiscard]] bool Transforms(
		const Api::MessageToSend &message,
		const OutgoingFiles *files = nullptr) {
	const auto snapshot = Snapshot();
	const auto &options = message.action.options;
	return snapshot
		&& (files ? !files->list.files.empty() : !message.textWithTags.text.isEmpty())
		&& !options.shortcutId
		&& !options.welcomeTemplate
		&& !options.suggest.exists
		&& (!snapshot->loaded
			|| snapshot->transformsOutgoing(InfoOf(message, files)));
}

void ApplyChanges(
		Api::MessageToSend &message,
		const fgpl::OutgoingResult &result) {
	auto &action = message.action;
	const auto history = action.history;
	const auto peer = history->peer;
	if (result.silent) {
		action.options.silent = *result.silent;
	}
	if (result.schedule) {
		if (!*result.schedule) {
			action.options.scheduled = 0;
		} else if (ValidScheduleTime(TimeId(*result.schedule))) {
			action.options.scheduled = TimeId(*result.schedule);
		}
	}
	auto &replyTo = action.replyTo;
	if (result.replyTo) {
		if (!*result.replyTo) {
			replyTo.messageId = FullMsgId();
		} else if (const auto replied = peer->owner().message(
				peer->id,
				MsgId(*result.replyTo))) {
			replyTo.messageId = replied->fullId();
			replyTo.topicRootId = ThreadRootOf(peer, replied);
		}
	}
	if (result.topic) {
		const auto marker = (replyTo.messageId.msg == replyTo.topicRootId);
		const auto wanted = MsgId(*result.topic);
		const auto topic = wanted ? peer->forumTopicFor(wanted) : nullptr;
		const auto allowed = topic
			? Data::CanSendTexts(topic)
			: (!peer->forum()
				&& wanted != MsgId(Data::ForumTopic::kGeneralId));
		if (!wanted) {
			replyTo.topicRootId = MsgId();
		} else if (allowed) {
			replyTo.topicRootId = wanted;
		} else {
			return;
		}
		if (marker) {
			replyTo.messageId = FullMsgId();
		}
	}
	if (replyTo.topicRootId && !replyTo.messageId) {
		replyTo.messageId = FullMsgId(peer->id, replyTo.topicRootId);
	}
}

void SendNow(Outgoing &&outgoing) {
	auto &service = GetService();
	const auto history = outgoing.message.action.history;
	service.sending = true;
	if (const auto files = outgoing.files.get()) {
		if (!files->list.files.empty()) {
			files->list.files.front().caption = outgoing.message.textWithTags;
		}
		history->session().api().sendFiles(
			std::move(files->list),
			files->type,
			std::move(files->album),
			outgoing.message.action);
	} else {
		history->session().api().sendMessage(
			std::move(outgoing.message),
			outgoing.localMessageId);
	}
	service.sending = false;
}

void RestoreDraft(const Api::MessageToSend &message) {
	const auto history = message.action.history;
	const auto &replyTo = message.action.replyTo;
	const auto current = history->localDraft(
		replyTo.topicRootId,
		replyTo.monoforumPeerId);
	if (!current || current->textWithTags.text.isEmpty()) {
		const auto text = message.textWithTags.text;
		const auto cursor = MessageCursor{
			int(text.size()),
			int(text.size()),
			Ui::kQFixedMax
		};
		history->setLocalDraft(std::make_unique<Data::Draft>(
			message.textWithTags,
			replyTo,
			SuggestOptions(),
			cursor,
			Data::WebPageDraft()));
		history->updateChatListSortPosition();
		history->session().changes().historyUpdated(
			history,
			Data::HistoryUpdate::Flag::CloudDraft);
		history->session().local().writeDrafts(history);
	}
}

void Refuse(const Outgoing &outgoing, const fgpl::OutgoingResult &result) {
	const auto snapshot = Snapshot();
	const auto parsed = snapshot ? snapshot->manifest(result.plugin) : nullptr;
	const auto name = parsed
		? QString::fromStdString(parsed->name.get(Lang::Id().toStdString()))
		: QString::fromStdString(result.plugin);
	if (!outgoing.byPlugin && !outgoing.files) {
		RestoreDraft(outgoing.message);
	}
	Ui::Toast::Show(tr::fresh_PluginOutgoingBlocked(
		tr::now,
		lt_plugin,
		name,
		lt_reason,
		QString::fromStdString(result.reason)));
}

void Drop(const fgpl::OutgoingResult &result) {
	if (result.reason.empty()) {
		return;
	}
	const auto snapshot = Snapshot();
	const auto parsed = snapshot ? snapshot->manifest(result.plugin) : nullptr;
	const auto name = parsed
		? QString::fromStdString(parsed->name.get(Lang::Id().toStdString()))
		: QString::fromStdString(result.plugin);
	Ui::Toast::Show(tr::fresh_PluginOutgoingBlocked(
		tr::now,
		lt_plugin,
		name,
		lt_reason,
		QString::fromStdString(result.reason)));
}

void Next(not_null<History*> history);

void Finish(
		not_null<History*> history,
		base::weak_ptr<Main::Session> weak,
		fgpl::OutgoingResult result) {
	auto &service = GetService();
	const auto i = service.outgoing.find(history);
	if (i == end(service.outgoing)) {
		return;
	} else if (!weak || i->second.empty()) {
		service.outgoing.erase(i);
		return;
	}
	auto outgoing = std::move(i->second.front());
	i->second.pop_front();
	if (result.blocked) {
		Refuse(outgoing, result);
	} else if (result.dropped) {
		Drop(result);
	} else {
		ApplyChanges(outgoing.message, result);
		const auto changed = QString::fromStdString(result.text);
		if (result.entities) {
			outgoing.message.textWithTags = TextWithEntitiesOf(
				&history->session(),
				changed,
				*result.entities);
		} else if (changed != outgoing.message.textWithTags.text) {
			outgoing.message.textWithTags = TextWithTags{ changed };
		}
		SendNow(std::move(outgoing));
	}
	Next(history);
}

void Next(not_null<History*> history) {
	auto &service = GetService();
	auto i = service.outgoing.find(history);
	while (i != end(service.outgoing) && !i->second.empty()) {
		if (!Transforms(i->second.front().message, i->second.front().files.get())) {
			auto outgoing = std::move(i->second.front());
			i->second.pop_front();
			SendNow(std::move(outgoing));
			i = service.outgoing.find(history);
			continue;
		}
		const auto &message = i->second.front().message;
		const auto account = AccountId(&history->session()).toStdString();
		const auto info = InfoOf(message, i->second.front().files.get());
		const auto weak = base::make_weak(&history->session());
		const auto posted = Post([=](not_null<fgpl::PluginManager*> manager) {
			manager->transformOutgoingAsync(
				account,
				info,
				kOutgoingTimeoutMs,
				[=](fgpl::OutgoingResult result) {
					crl::on_main([=, result = std::move(result)] {
						Finish(history, weak, result);
					});
				});
		}, false);
		if (posted) {
			return;
		}
		auto outgoing = std::move(i->second.front());
		i->second.pop_front();
		SendNow(std::move(outgoing));
		i = service.outgoing.find(history);
	}
	if (i != end(service.outgoing)) {
		service.outgoing.erase(i);
	}
}

} // namespace

#endif

bool ApplyOutgoing(
		Api::MessageToSend &message,
		std::optional<MsgId> localMessageId) {
#ifdef FRESHGRAM_PLUGINS
	auto &service = GetService();
	const auto history = message.action.history;
	if (service.sending || !service.control) {
		return false;
	}
	const auto queued = service.outgoing.find(history);
	const auto waiting = (queued != end(service.outgoing))
		&& !queued->second.empty();
	if (!waiting && !Transforms(message)) {
		return false;
	}
	auto &queue = service.outgoing[history];
	const auto byPlugin = PluginSending()
		|| (localMessageId
			&& IsPluginMessage(
				&history->session(),
				FullMsgId(history->peer->id, *localMessageId)));
	queue.push_back({ std::move(message), localMessageId, byPlugin });
	if (!waiting) {
		Next(history);
	}
	return true;
#else
	return false;
#endif
}

bool ApplyOutgoingFiles(
		Ui::PreparedList &list,
		SendMediaType type,
		std::shared_ptr<SendingAlbum> &album,
		const Api::SendAction &action) {
#ifdef FRESHGRAM_PLUGINS
	auto &service = GetService();
	if (service.sending
		|| !service.control
		|| PluginSending()
		|| list.files.empty()
		|| !action.history) {
		return false;
	}
	const auto history = action.history;
	auto message = Api::MessageToSend(action);
	message.textWithTags = list.files.front().caption;
	auto files = std::make_unique<OutgoingFiles>(OutgoingFiles{
		std::move(list),
		type,
		std::move(album),
	});
	const auto queued = service.outgoing.find(history);
	const auto waiting = (queued != end(service.outgoing))
		&& !queued->second.empty();
	if (!waiting && !Transforms(message, files.get())) {
		list = std::move(files->list);
		album = std::move(files->album);
		return false;
	}
	auto &queue = service.outgoing[history];
	queue.push_back({ std::move(message), std::nullopt, false, std::move(files) });
	if (!waiting) {
		Next(history);
	}
	return true;
#else
	return false;
#endif
}

std::optional<OutgoingFileSource> PendingOutgoingFile(
		not_null<History*> history,
		size_t index,
		bool thumbnail) {
#ifdef FRESHGRAM_PLUGINS
	const auto &outgoing = GetService().outgoing;
	const auto i = outgoing.find(history);
	if (i == end(outgoing) || i->second.empty()) {
		return std::nullopt;
	}
	const auto &files = i->second.front().files;
	if (!files || index >= files->list.files.size()) {
		return std::nullopt;
	}
	const auto &file = files->list.files[index];
	auto result = OutgoingFileSource{
		.kind = OutgoingKindOf(files->type, file),
		.name = OutgoingNameOf(file),
	};
	if (thumbnail) {
		if (file.preview.isNull()) {
			return std::nullopt;
		}
		auto buffer = QBuffer(&result.bytes);
		buffer.open(QIODevice::WriteOnly);
		file.preview.save(&buffer, "JPEG", 87);
		result.kind = "thumbnail";
		result.mime = "image/jpeg";
	} else if (!file.content.isEmpty()) {
		result.bytes = file.content;
	} else if (!file.path.isEmpty()) {
		result.path = file.path;
	} else {
		return std::nullopt;
	}
	return result;
#else
	return std::nullopt;
#endif
}

bool HasPendingOutgoing(not_null<History*> history) {
#ifdef FRESHGRAM_PLUGINS
	const auto &outgoing = GetService().outgoing;
	const auto i = outgoing.find(history);
	return (i != end(outgoing)) && !i->second.empty();
#else
	return false;
#endif
}

bool SandboxSupported() {
#ifdef FRESHGRAM_PLUGINS
	static const auto result = fgpl::sandbox::Supported();
	return result;
#else
	return false;
#endif
}

State CurrentState() {
#ifdef FRESHGRAM_PLUGINS
	return GetService().state.current();
#else
	return State::Unavailable;
#endif
}

rpl::producer<State> StateValue() {
#ifdef FRESHGRAM_PLUGINS
	return GetService().state.value();
#else
	return rpl::single(State::Unavailable);
#endif
}

rpl::producer<> ListChanges() {
#ifdef FRESHGRAM_PLUGINS
	return GetService().list.events();
#else
	return rpl::never<>();
#endif
}

void NotifyListChanged() {
#ifdef FRESHGRAM_PLUGINS
	GetService().list.fire({});
#endif
}

bool Running() {
#ifdef FRESHGRAM_PLUGINS
	return GetService().control != nullptr;
#else
	return false;
#endif
}

std::shared_ptr<const fgpl::ManagerSnapshot> Snapshot() {
#ifdef FRESHGRAM_PLUGINS
	const auto &control = GetService().control;
	return control ? control->snapshot() : nullptr;
#else
	return nullptr;
#endif
}

bool Post(
		Fn<void(not_null<fgpl::PluginManager*>)> work,
		bool changesState) {
#ifdef FRESHGRAM_PLUGINS
	if (const auto &control = GetService().control) {
		return control->post(std::move(work), changesState);
	}
#endif
	return false;
}

QString StorePath() {
	return cWorkingDir() + u"tdata/plugins/store"_q;
}

QString RuntimeRootPath() {
	return cWorkingDir() + u"tdata/plugins/runtime"_q;
}

QString RuntimePath() {
#ifdef FRESHGRAM_PLUGINS
	// On Windows "current" is a pointer file, not a link.
	return QString::fromStdString(fgpl::os::PathToUtf8(
		fgpl::os::ResolveCurrent(PathOf(RuntimeRootPath()))));
#else // FRESHGRAM_PLUGINS
	return RuntimeRootPath() + u"/current"_q;
#endif // FRESHGRAM_PLUGINS
}

std::filesystem::path PathOf(const QString &text) {
#ifdef Q_OS_WIN
	// std::filesystem would read a narrow string as ANSI here.
	return std::filesystem::path(text.toStdWString());
#else // Q_OS_WIN
	return std::filesystem::path(text.toStdString());
#endif // Q_OS_WIN
}

void NotifyRuntimeChanged() {
#ifdef FRESHGRAM_PLUGINS
	if (GetService().started) {
		Refresh();
	}
#endif
}

void WatchActiveChat(not_null<Window::SessionController*> controller) {
#ifdef FRESHGRAM_PLUGINS
	const auto session = &controller->session();
	controller->activeChatChanges(
	) | rpl::on_next([=](Dialogs::Key key) {
		const auto history = key.history();
		if (!history || !WantsEvents()) {
			return;
		}
		using Value = fgpl::cbor::Value;
		const auto peer = history->peer;
		auto map = fgpl::cbor::Map{
			{ "type", Value::FromText("chat.opened") },
			{ "chat", Value::FromInt(int64_t(peer->id.value)) },
			{ "chat_title", Value::FromText(peer->name().toStdString()) },
			{ "secret", Value::FromBool(FreshSecret::IsSecretPeerId(peer->id)) },
			{ "self", Value::FromBool(peer->isSelf()) },
		};
		if (const auto topic = key.topic()) {
			map.push_back({ "topic", Value::FromInt(topic->rootId().bare) });
		}
		const auto account = AccountId(session).toStdString();
		Post([=, event = Value::FromMap(std::move(map))](
				not_null<fgpl::PluginManager*> manager) {
			manager->dispatch(account, event);
		}, false);
	}, controller->lifetime());
#else
	(void)controller;
#endif
}

QString AccountId(not_null<Main::Session*> session) {
	return QString::number(session->uniqueId());
}

} // namespace FreshPlugins
