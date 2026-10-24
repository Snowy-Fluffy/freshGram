// This is the source code of freshGram.
#include "ayu/features/plugins/plugins_service.h"

#include "ayu/ayu_settings.h"
#include "ayu/data/ayu_vault.h"
#include "ayu/features/plugins/plugins_data.h"
#include "ayu/features/plugins/plugins_runtime.h"
#include "ayu/features/plugins/plugins_transport.h"
#include "ayu/features/plugins/plugins_ui.h"
#include "crl/crl_on_main.h"
#include "data/data_changes.h"
#include "data/data_session.h"
#include "history/history_item.h"
#include "base/flat_set.h"
#include "base/timer.h"
#include "core/application.h"
#include "core/version.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "settings.h"
#include "storage/storage_domain.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSysInfo>

#include <unistd.h>

#ifdef FRESHGRAM_PLUGINS
#include "fgpl/manager.h"
#include "fgpl/runtime_bundle.h"
#include "fgpl/sandbox_linux.h"
#include "fgpl/semver.h"
#endif

#include <cctype>
#include <map>

namespace AyuPlugins {
namespace {

constexpr auto kPumpEach = crl::time(50);

#ifdef FRESHGRAM_PLUGINS

struct Watched {
	std::string id;
	rpl::lifetime lifetime;
	rpl::lifetime events;
};

struct Service {
	std::unique_ptr<fgpl::PluginManager> manager;
	base::Timer pump;
	std::map<Main::Account*, Watched> watched;
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

[[nodiscard]] fgpl::Version KernelVersion() {
	const auto text = QSysInfo::kernelVersion().toStdString();
	auto length = size_t(0);
	while (length < text.size() && (std::isdigit(uchar(text[length])) || text[length] == '.')) {
		++length;
	}
	if (const auto parsed = fgpl::ParseVersion(text.substr(0, length))) {
		return *parsed;
	}
	return {};
}

[[nodiscard]] bool DeriveSecret(const char *label, std::array<uint8_t, 32> &out) {
	return AyuVault::DeriveKey(label, out.data(), out.size());
}

[[nodiscard]] std::unique_ptr<fgpl::PluginManager> MakeManager(
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
	RefreshProxy();

	const auto installed = Installed();
	if (!installed) {
		return nullptr;
	}
	auto sealed = fgpl::SealExecutable(
		config.host.path,
		installed->hostHash);
	if (!sealed.ok()) {
		LOG(("Plugins Error: %1").arg(QString::fromStdString(sealed.error())));
		return nullptr;
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
		return nullptr;
	}

	config.environment.app = *fgpl::ParseVersion(AppVersionStr);
	config.environment.apiLevel = 1;
	config.environment.platform = "linux";
	config.environment.platformVersion = KernelVersion();
	return std::make_unique<fgpl::PluginManager>(std::move(config));
}

[[nodiscard]] State Compute() {
	if (!AyuSettings::getInstance().pluginsEnabled()) {
		return State::Disabled;
	} else if (!SandboxSupported()) {
		return State::NoSandbox;
	} else if (!Installed()) {
		return State::NoRuntime;
	} else if (!AyuVault::HasKey()) {
		return State::NoKey;
	}
	return State::Ready;
}

void StartAccounts(Service &service) {
	if (!service.manager || !Core::App().domain().started()) {
		return;
	}
	for (const auto &[account, watched] : service.watched) {
		if (!watched.id.empty()) {
			service.manager->startEnabled(watched.id);
		}
	}
}

void DropManager(Service &service) {
	if (service.manager) {
		CloseData();
		service.manager->shutdown();
		service.manager = nullptr;
		ReleaseData();
	}
	service.pump.cancel();
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
	} else if (service.manager) {
		const auto installed = Installed();
		if (!installed || installed->revision != service.revision) {
			DropManager(service);
		}
	}
	if (wanted == State::Ready && !service.manager) {
		service.manager = MakeManager(service);
		if (service.manager) {
			service.pump.callEach(kPumpEach);
			StartAccounts(service);
		} else {
			DropManager(service);
			service.state = AyuVault::HasKey() ? State::NoRuntime : State::NoKey;
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
	if (service.manager && !id.empty()) {
		service.manager->onLogout(id);
	}
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
			if (service.manager && !id.empty()) {
				service.manager->onLogout(id);
			}
			return;
		}
		const auto id = AccountId(session).toStdString();
		i->second.id = id;
		if (service.manager) {
			service.manager->startEnabled(id);
		}
		session->data().newItemAdded(
		) | rpl::on_next([=](not_null<HistoryItem*> item) {
			if (WantsEvents()) {
				SendMessageEvent("message.new", item, id);
			}
		}, i->second.events);
		session->changes().messageUpdates(
			Data::MessageUpdate::Flag::Edited
		) | rpl::on_next([=](const Data::MessageUpdate &update) {
			if (WantsEvents()) {
				SendMessageEvent("message.edited", update.item, id);
			}
		}, i->second.events);
		session->data().itemRemoved(
		) | rpl::on_next([=](not_null<const HistoryItem*> item) {
			if (WantsEvents()) {
				SendMessageEvent("message.deleted", item, id);
			}
		}, i->second.events);
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
	service.pump.setCallback([] {
		auto &service = GetService();
		if (service.manager && service.manager->pump() > 0) {
			service.list.fire({});
		}
	});

	AyuSettings::getInstance().pluginsEnabledValue(
	) | rpl::on_next([] {
		Refresh();
	}, service.lifetime);

	Core::App().domain().accountsChanges(
	) | rpl::on_next([] {
		SyncAccounts();
		Refresh();
	}, service.lifetime);

	Core::App().proxyChanges(
	) | rpl::on_next([] {
		RefreshProxy();
	}, service.lifetime);

	Core::App().passcodeLockValue(
	) | rpl::on_next([](bool locked) {
		if (const auto manager = GetService().manager.get()) {
			if (locked) {
				manager->onLock();
			} else {
				manager->onUnlock();
			}
		}
		Refresh();
	}, service.lifetime);

	SyncAccounts();
	Refresh();
	StartRuntimeUpdates();
#endif
}

void Stop() {
#ifdef FRESHGRAM_PLUGINS
	auto &service = GetService();
	service.lifetime.destroy();
	service.pump.cancel();
	service.watched.clear();
	DropManager(service);
	service.state = State::Disabled;
	service.started = false;
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

fgpl::PluginManager *Manager() {
#ifdef FRESHGRAM_PLUGINS
	return GetService().manager.get();
#else
	return nullptr;
#endif
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

QString AccountId(not_null<Main::Session*> session) {
	return QString::number(session->uniqueId());
}

} // namespace AyuPlugins
