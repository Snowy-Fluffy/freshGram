// This is the source code of freshGram.
#include "ayu/features/plugins/plugins_service.h"

#include "ayu/ayu_settings.h"
#include "ayu/data/fresh_vault.h"
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
#include "data/data_drafts.h"
#include "data/data_forum_topic.h"
#include "dialogs/dialogs_key.h"
#include "window/window_session_controller.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "base/unixtime.h"
#include "lang/lang_keys.h"
#include "ui/toast/toast.h"
#include "base/flat_set.h"
#include "base/timer.h"
#include "core/application.h"
#include "core/version.h"
#include "platform/platform_specific.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "settings.h"
#include "storage/storage_account.h"
#include "storage/storage_domain.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>

#ifdef FRESHGRAM_PLUGINS
#include "fgpl/manager.h"
#include "fgpl/runtime_bundle.h"
#include "fgpl/sandbox_linux.h"
#include "fgpl/semver.h"

#include <unistd.h>
#endif

#include <cctype>
#include <deque>
#include <map>

namespace FreshPlugins {
namespace {

constexpr auto kOutgoingTimeoutMs = 1000;
constexpr auto kRememberedMessages = size_t(20000);
constexpr auto kFreshEditSeconds = TimeId(300);

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
	base::flat_set<FullMsgId> deleted;
	std::deque<FullMsgId> deletedOrder;
};

struct Watched {
	std::string id;
	rpl::lifetime lifetime;
	rpl::lifetime events;
};

struct Outgoing {
	Api::MessageToSend message;
	std::optional<MsgId> localMessageId;
	bool byPlugin = false;
};

struct Service {
	std::unique_ptr<Control> control;
	std::map<Main::Account*, Watched> watched;
	std::map<not_null<History*>, std::deque<Outgoing>> outgoing;
	bool sending = false;
	rpl::variable<State> state = State::Disabled;
	rpl::event_stream<> list;
	rpl::lifetime lifetime;
	int hostFd = -1;
	int revision = 0;
	bool started = false;
};

[[nodiscard]] Service &GetService() {
	static auto result = Service();
	return result;
}

[[nodiscard]] QString HostBinary() {
	return RuntimePath() + u"/fgpl_plugin_host"_q;
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
	config.root = StorePath().toStdString();
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
	const auto runtime = RuntimePath().toStdString();
	config.python.expectedHash = installed->pythonHash;
	config.python.wasm = runtime + "/python/python.wasm";
	config.python.library = runtime + "/python/lib";
	config.python.sdk = runtime + "/sdk";

	config.cache.directory = (cWorkingDir() + u"tdata/plugins/cache"_q).toStdString();
	if (!DeriveSecret("freshgram.plugins.cache", config.cache.secret)
		|| !DeriveSecret("freshgram.plugins.state", config.stateKey)) {
		return std::nullopt;
	}

	config.environment.app = *fgpl::ParseVersion(AppVersionStr);
	config.environment.apiLevel = 3;
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
	if (service.hostFd >= 0) {
		close(base::take(service.hostFd));
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
	return item->isRegular() && IsServerMsgId(item->id) && !item->isService();
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

void WatchEvents(
		not_null<Main::Session*> session,
		const std::string &id,
		rpl::lifetime &lifetime) {
	const auto events = lifetime.make_state<Events>();
	const auto data = &session->data();
	data->newItemAdded(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		if (!IsServerMsgId(item->id)) {
			if (item->out() && item->isLocal()) {
				events->sending.emplace(item->fullId());
				if (PluginSending()) {
					events->byPlugin.emplace(item->fullId());
				}
			}
			return;
		} else if (!WantsEvents() || !Reportable(item)) {
			return;
		}
		Note(*events, item);
		SendMessageEvent("message.new", item, id);
	}, lifetime);
	data->itemIdChanged(
	) | rpl::on_next([=](const Data::Session::IdChange &change) {
		const auto old = FullMsgId(change.newId.peer, change.oldId);
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
	data->itemRemoved(
	) | rpl::on_next([=](not_null<const HistoryItem*> item) {
		const auto fullId = item->fullId();
		events->seen.erase(fullId);
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
	for (auto &[history, queue] : base::take(service.outgoing)) {
		for (const auto &outgoing : queue) {
			if (!outgoing.byPlugin) {
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

[[nodiscard]] bool Transforms(const Api::MessageToSend &message) {
	const auto snapshot = Snapshot();
	const auto peer = message.action.history->peer;
	return snapshot
		&& !message.textWithTags.text.isEmpty()
		&& (!snapshot->loaded || snapshot->transformsOutgoing(
			int64_t(peer->id.value),
			FreshSecret::IsSecretPeerId(peer->id)));
}

void SendNow(Outgoing &&outgoing) {
	auto &service = GetService();
	const auto history = outgoing.message.action.history;
	service.sending = true;
	history->session().api().sendMessage(
		std::move(outgoing.message),
		outgoing.localMessageId);
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
	if (!outgoing.byPlugin) {
		RestoreDraft(outgoing.message);
	}
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
	} else {
		const auto changed = QString::fromStdString(result.text);
		if (result.markdown) {
			auto parsed = TextWithEntities{ changed };
			TextUtilities::ParseEntities(parsed, TextParseMarkdown);
			outgoing.message.textWithTags = TextWithTags{
				parsed.text,
				TextUtilities::ConvertEntitiesToTextTags(parsed.entities),
			};
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
		if (!Transforms(i->second.front().message)) {
			auto outgoing = std::move(i->second.front());
			i->second.pop_front();
			SendNow(std::move(outgoing));
			i = service.outgoing.find(history);
			continue;
		}
		const auto &message = i->second.front().message;
		const auto peer = history->peer;
		const auto account = AccountId(&history->session()).toStdString();
		const auto chat = int64_t(peer->id.value);
		const auto secret = FreshSecret::IsSecretPeerId(peer->id);
		const auto text = message.textWithTags.text.toStdString();
		const auto weak = base::make_weak(&history->session());
		const auto posted = Post([=](not_null<fgpl::PluginManager*> manager) {
			manager->transformOutgoingAsync(
				account,
				chat,
				secret,
				text,
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
	queue.push_back({ std::move(message), localMessageId, PluginSending() });
	if (!waiting) {
		Next(history);
	}
	return true;
#else
	return false;
#endif
}

bool SandboxSupported() {
#ifdef FRESHGRAM_PLUGINS
	static const auto result = (fgpl::sandbox::LandlockAbi() > 0);
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
	return RuntimeRootPath() + u"/current"_q;
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
