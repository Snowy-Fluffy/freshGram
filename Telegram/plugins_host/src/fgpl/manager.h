#pragma once

#include "fgpl/async_pool.h"
#include "fgpl/broker.h"
#include "fgpl/cached_load.h"
#include "fgpl/file_storage.h"
#include "fgpl/host_client.h"
#include "fgpl/manifest.h"
#include "fgpl/package.h"
#include "fgpl/permissions.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fgpl {

struct LogLine {
	int64_t unixMs = 0;
	std::string text;
	bool fromApp = false;
};

struct LogRing {
	std::mutex mutex;
	std::deque<LogLine> lines;
};

class SharedStorage final : public Storage {
public:
	explicit SharedStorage(std::filesystem::path file);

	std::optional<cbor::Bytes> get(const std::string &key) override;
	void set(const std::string &key, cbor::Bytes value) override;
	bool remove(const std::string &key) override;
	std::vector<std::string> keys(
		const std::string &prefix,
		size_t limit) override;
	uint64_t usedBytes() override;
	size_t count() override;
	bool flush();
	void discard();

private:
	std::mutex _mutex;
	std::unique_ptr<FileStorage> _storage;

};

struct PythonRuntime {
	std::filesystem::path wasm;
	std::filesystem::path library;
	std::filesystem::path sdk;
	std::string expectedHash;
	std::string version = "3.14";
};

struct ManagerServices {
	std::function<void(
		const std::string &id,
		const std::string &account,
		const cbor::Value &page)> settingsPage;
	std::function<void(
		const std::string &id,
		const std::string &account,
		const cbor::Value *page)> dialog;
	// A window of the plugin: `mode` is "layer" or "window"; no page closes it.
	std::function<void(
		const std::string &id,
		const std::string &account,
		const std::string &window,
		const std::string &mode,
		const cbor::Value *page)> window;
	DataProvider *data = nullptr;
	std::function<DataProvider*(const std::string &account)> dataFor;
	Transport *transport = nullptr;
	std::function<int64_t()> clockMs;
};

struct ManagerConfig {
	std::filesystem::path root;
	HostConfig host;
	PythonRuntime python;
	CacheSettings cache;
	HostEnvironment environment;
	ManagerServices services;
	std::array<uint8_t, 32> stateKey = {};
	int maxFailures = 3;
	int64_t failureWindowMs = 600000;
	size_t logLines = 200;
	size_t maxQueuedEvents = 256;
	size_t maxQueuedBytes = size_t(256) << 20;
	int maxConcurrentStarts = 2;
};

struct InstallOptions {
	bool allowDowngrade = false;
	bool acceptUnsigned = false;
	bool acceptNewPublisher = false;
};

enum class InstallError : uint8_t {
	Package,
	Incompatible,
	UnsignedRefused,
	PublisherChanged,
	SameVersion,
	DowngradeRefused,
	MissingDependency,
	Conflict,
	Locked,
	Io,
};

struct InstallFailure {
	InstallError error = InstallError::Io;
	std::vector<std::string> details;
};

struct InstallReport {
	std::string id;
	std::string version;
	bool updated = false;
	bool signedPackage = false;
	std::string publisher;
	std::vector<std::string> newRequests;
};

enum class PluginStatus : uint8_t {
	Stopped,
	Running,
	Disabled,
};

struct PluginStats {
	uint64_t events = 0;
	uint64_t failures = 0;
	uint64_t dropped = 0;
	uint64_t unloads = 0;
	uint64_t cpuMs = 0;
	uint64_t memoryBytes = 0;
	uint64_t netReceived = 0;
	uint64_t netSent = 0;
	int openSockets = 0;
	int running = 0;
};

struct OutgoingResult {
	bool blocked = false;
	bool dropped = false;
	std::string text;
	bool markdown = false;
	std::optional<bool> silent;
	std::optional<int64_t> schedule;
	std::optional<int64_t> replyTo;
	std::optional<int64_t> topic;
	std::string plugin;
	std::string reason;
};

struct TranslateRequest {
	std::string to;
	std::vector<std::string> items;
};

struct TranslateResult {
	std::string error;
	std::vector<std::string> items;
	bool markdown = false;
};

struct PluginInfo {
	std::string id;
	std::string name;
	std::string version;
	std::string previousVersion;
	bool enabled = false;
	bool pendingUpdate = false;
	bool signedPackage = false;
	std::string publisher;
	std::string disabledReason;
	std::string problem;
	PluginStatus status = PluginStatus::Stopped;
	bool onDemand = false;
	int64_t retryInMs = 0;
	std::string lastFailure;
	std::vector<PermissionRequest> requests;
	std::vector<CompatIssue> issues;
	std::string testedWith;
	bool outdated = false;
	PluginStats stats;
};

struct ContributedMenu {
	std::string surface;
	std::string id;
	std::string title;
	std::string icon;
	MenuWhen when;
};

struct ContributedTranslator {
	bool declared = false;
	std::string name;
	std::vector<std::string> languages;
	int maxChars = 0;
};

struct ContributedCommand {
	std::string name;
	std::string description;
};

struct Contribution {
	std::string id;
	std::string name;
	std::vector<ContributedMenu> menus;
	std::vector<ContributedCommand> commands;
	bool settingsPage = false;
	ContributedTranslator translator;
};

struct DispatchReport {
	int queued = 0;
	int skipped = 0;
	int dropped = 0;
	int started = 0;
	int delivered = 0;
	int failed = 0;
};

struct ManagerSnapshot {
	std::vector<PluginInfo> plugins;
	std::map<std::string, std::shared_ptr<const Manifest>> manifests;
	std::map<std::string, GrantDecision> grants;
	std::map<std::string, std::shared_ptr<LogRing>> logs;
	std::map<std::string, std::shared_ptr<const cbor::Bytes>> icons;
	std::map<std::pair<std::string, std::string>, std::shared_ptr<const cbor::Bytes>> customIcons;
	HostEnvironment environment;
	bool loaded = false;
	bool wantsEvents = false;
	std::set<std::string> wantedEvents;
	bool locked = false;
	bool hidden = false;

	[[nodiscard]] const PluginInfo *plugin(const std::string &id) const;
	[[nodiscard]] std::shared_ptr<const Manifest> manifest(
		const std::string &id) const;
	[[nodiscard]] std::shared_ptr<const cbor::Bytes> icon(
		const std::string &id) const;
	[[nodiscard]] std::shared_ptr<const cbor::Bytes> customIcon(
		const std::string &id,
		const std::string &name) const;
	[[nodiscard]] std::optional<GrantDecision> grantsOf(
		const std::string &id) const;
	[[nodiscard]] std::vector<std::string> log(const std::string &id) const;
	[[nodiscard]] std::vector<LogLine> logLines(const std::string &id) const;
	[[nodiscard]] std::vector<Contribution> contributions(
		const std::string &account,
		const std::string &language) const;
	[[nodiscard]] bool wants(const char *event) const {
		return wantedEvents.contains(event);
	}
	[[nodiscard]] bool transformsOutgoing(const OutgoingInfo &info) const;
	[[nodiscard]] bool transformsOutgoing(int64_t chat, bool secret) const;
};

class PluginManager {
public:
	explicit PluginManager(ManagerConfig config);
	~PluginManager();

	[[nodiscard]] Result<InstallReport, InstallFailure> install(
		std::span<const uint8_t> archive,
		const InstallOptions &options = {});
	bool uninstall(const std::string &id, bool removeData);

	bool setEnabled(const std::string &id, bool enabled);
	bool start(const std::string &id);
	bool grant(
		const std::string &id,
		const std::string &account,
		const GrantDecision &decision);
	[[nodiscard]] std::optional<GrantDecision> grants(
		const std::string &id,
		const std::string &account) const;

	[[nodiscard]] std::vector<PluginInfo> list() const;
	[[nodiscard]] std::shared_ptr<const Manifest> manifest(
		const std::string &id) const;
	[[nodiscard]] std::vector<Contribution> contributions(
		const std::string &account,
		const std::string &language) const;
	bool send(
		const std::string &id,
		const std::string &account,
		const cbor::Value &event);
	[[nodiscard]] std::vector<std::string> log(const std::string &id) const;
	void clearLog(const std::string &id);

	void onLock();
	void onUnlock();
	void onLogout(const std::string &account);
	void setHidden(bool hidden);

	DispatchReport dispatch(const std::string &account, const cbor::Value &event);
	DispatchReport dispatchAndWait(
		const std::string &account,
		const cbor::Value &event,
		int timeoutMs = 60000);
	DispatchReport startEnabled(const std::string &account);
	[[nodiscard]] OutgoingResult transformOutgoing(
		const std::string &account,
		int64_t chat,
		bool secret,
		const std::string &text,
		int timeoutMs = 1000);
	void transformOutgoingAsync(
		const std::string &account,
		int64_t chat,
		bool secret,
		const std::string &text,
		int timeoutMs,
		std::function<void(OutgoingResult)> done);
	void transformOutgoingAsync(
		const std::string &account,
		OutgoingInfo info,
		int timeoutMs,
		std::function<void(OutgoingResult)> done);

	// Asks a plugin with contributes.translator; done is called exactly once,
	// possibly from another thread.
	void translateAsync(
		const std::string &account,
		const std::string &plugin,
		TranslateRequest request,
		int timeoutMs,
		std::function<void(TranslateResult)> done);

	[[nodiscard]] ManagerSnapshot snapshot() const;

	int pump();
	bool drain(int timeoutMs = 60000);
	void waitForWork(int timeoutMs, const std::function<bool()> &ready);
	void wake();

	void shutdown();

	[[nodiscard]] bool stateWasTampered() const {
		return _tampered;
	}
	[[nodiscard]] bool saveFailed() const {
		return _saveFailed;
	}
	[[nodiscard]] size_t runningCount() const {
		return _instances.size();
	}
	[[nodiscard]] bool wantsEvents(const char *event = nullptr) const;

private:
	struct StoredGrant {
		std::set<std::string> granted;
		std::optional<std::set<int64_t>> chats;
	};

	struct Record {
		std::string id;
		std::string name;
		bool enabled = false;
		std::string current;
		std::string previous;
		bool pendingUpdate = false;
		std::string publisher;
		std::string archiveHash;
		std::string previousArchiveHash;
		std::string disabledReason;
		std::map<std::string, StoredGrant> grants;
		std::shared_ptr<SharedStorage> store;
		std::shared_ptr<std::mutex> serial = std::make_shared<std::mutex>();
		PluginStats stats;
		std::deque<int64_t> failures;
		std::string lastFailure;
		std::shared_ptr<LogRing> log = std::make_shared<LogRing>();
	};

	struct Declared {
		std::set<std::string> menus;
		std::set<std::string> commands;
		bool settingsPage = false;
	};

	struct Outgoing {
		cbor::Value event;
		std::mutex mutex;
		std::condition_variable done;
		bool finished = false;
		bool ok = false;
		OutgoingEdit edit;
	};

	struct StartPlan {
		std::string version;
		std::string archiveHash;
		HostConfig host;
		LoadRequest request;
		CacheSettings cache;
		cbor::Value init;
	};

	struct Instance {
		std::string id;
		std::string account;
		uint64_t generation = 0;
		std::shared_ptr<SharedStorage> storage;
		std::shared_ptr<std::mutex> serial;
		std::shared_ptr<Broker> broker;
		std::unique_ptr<HostClient> client;
		StartPlan plan;
		Declared declared;
		std::thread thread;
		std::mutex mutex;
		std::condition_variable wake;
		std::deque<std::pair<cbor::Value, size_t>> queue;
		std::deque<std::shared_ptr<Outgoing>> outgoing;
		std::deque<cbor::Value> results;
		size_t queuedBytes = 0;
		bool closing = false;
		bool busy = false;
		bool ready = false;
		bool finished = false;
		std::atomic<bool> cancel{ false };
		std::atomic<bool> unload{ false };
		std::atomic<HostClient*> live{ nullptr };
		std::atomic<int> pid{ -1 };
		std::function<void(std::string_view)> appLog;
		bool committed = false;
	};

	struct Completion {
		std::string id;
		std::string account;
		uint64_t generation = 0;
		enum class Kind : uint8_t {
			Started,
			Delivered,
			Failed,
			Unloaded,
			Outgoing,
		} kind = Kind::Started;
		std::string detail;
	};

	using Key = std::pair<std::string, std::string>;

	struct OutgoingChain {
		std::string account;
		OutgoingInfo info;
		int timeoutMs = 0;
		std::vector<std::string> plugins;
		size_t next = 0;
		std::shared_ptr<Outgoing> current;
		bool blocking = false;
		std::chrono::steady_clock::time_point deadline;
		OutgoingResult result;
		std::function<void(OutgoingResult)> done;
	};

	struct Route {
		bool acceptsAny = false;
		std::set<std::string> from;
	};

	struct Timer {
		int64_t due = 0;
		int64_t period = 0;
	};

	struct Mail {
		std::string from;
		std::string to;
		std::string account;
		cbor::Value data;
	};

	[[nodiscard]] int64_t now() const;
	[[nodiscard]] std::filesystem::path versionDirectory(
		const std::string &id,
		const std::string &version) const;
	[[nodiscard]] std::filesystem::path archivePath(
		const std::string &id,
		const std::string &version) const;
	[[nodiscard]] Result<Package, std::string> openInstalled(
		const std::string &id,
		const std::string &version,
		const std::string &hash) const;

	void loadState();
	void saveState();
	[[nodiscard]] cbor::Value stateDocument() const;

	[[nodiscard]] Instance *ensure(
		Record &record,
		const std::string &account,
		const char *trigger = nullptr);
	[[nodiscard]] Instance *running(const std::string &id, const std::string &account);
	[[nodiscard]] std::shared_ptr<SharedStorage> storeOf(Record &record);
	[[nodiscard]] bool activatedBy(const Record &record, const char *trigger) const;
	void sampleUsage(Record &record, const Instance &instance);
	[[nodiscard]] Result<std::shared_ptr<Instance>, std::string> prepare(
		Record &record,
		const std::string &account);
	void run(Instance &instance);
	void post(Completion completion);
	void stop(const Key &key);
	void stopMany(const std::vector<Key> &keys);
	void stopPlugin(const std::string &id);
	void stopAll();
	void startEverywhere(Record &record);
	void noteFailure(Record &record, const std::string &account, const std::string &why);
	void commitUpdate(Record &record);
	void addLog(Record &record, std::string_view line);
	void rebuildRoutes();
	[[nodiscard]] std::string route(
		const std::string &from,
		const std::string &account,
		const std::string &to,
		cbor::Value data);
	[[nodiscard]] std::vector<std::string> peers(
		const std::string &from,
		const std::string &account) const;
	void deliverMail();
	void fireTimers();
	void clearTimers(const std::function<bool(const Key &key)> &matches);
	[[nodiscard]] std::string setTimer(
		const Key &key,
		const std::string &id,
		int64_t delayMs,
		bool repeat);
	void advanceOutgoing();
	[[nodiscard]] std::string acceptTranslation(
		const std::string &plugin,
		const std::string &account,
		int64_t ticket,
		std::vector<std::string> items,
		bool markdown,
		std::string error);
	void expireTranslations();
	void failTranslations(
		const std::string &plugin,
		const std::string &account,
		const std::string &reason);
	[[nodiscard]] bool advance(
		OutgoingChain &chain,
		std::chrono::steady_clock::time_point now);
	bool enqueue(Instance &instance, Record &record, const cbor::Value &event);
	[[nodiscard]] bool mayRun() const;

	[[nodiscard]] Result<LoadRequest, std::string> buildRequest(
		const Package &package);
	[[nodiscard]] Result<LoadRequest, std::string> loadRequest(
		const std::string &id,
		const StartPlan &plan);
	[[nodiscard]] bool pythonAvailable() const;
	// Reads the Python runtime into a Python request, wasm requests stay as they are.
	[[nodiscard]] std::string fillPython(
		LoadRequest &request,
		const std::atomic<bool> *cancel);

	ManagerConfig _config;
	mutable std::map<std::string, std::pair<std::string, std::shared_ptr<const Manifest>>> _manifests;
	mutable std::map<std::string, std::shared_ptr<const cbor::Bytes>> _icons;
	mutable std::map<std::pair<std::string, std::string>, std::shared_ptr<const cbor::Bytes>> _customIcons;
	std::map<std::string, Record> _records;
	std::map<Key, std::shared_ptr<Instance>> _instances;
	std::vector<std::shared_ptr<OutgoingChain>> _chains;
	std::map<Key, int64_t> _notBefore;
	std::map<std::string, uint64_t> _finishedCpuMs;
	mutable std::mutex _routesMutex;
	std::map<std::string, std::map<std::string, Route>> _routes;
	std::mutex _mailMutex;
	std::deque<Mail> _mail;
	std::map<Key, int> _streak;
	std::mutex _timersMutex;
	struct PendingTranslation {
		std::string plugin;
		std::string account;
		size_t count = 0;
		std::chrono::steady_clock::time_point deadline;
		std::function<void(TranslateResult)> done;
	};
	std::mutex _toastMutex;
	std::map<Key, std::deque<std::string>> _toastActions;
	std::mutex _translationsMutex;
	std::map<int64_t, PendingTranslation> _translations;
	int64_t _nextTranslation = 1;
	std::map<Key, std::map<std::string, Timer>> _timers;
	std::set<std::string> _accounts;
	bool _locked = false;
	bool _hidden = false;
	bool _closed = false;
	bool _tampered = false;
	bool _saveFailed = false;
	uint64_t _nextGeneration = 1;
	std::mutex _outboxMutex;
	std::condition_variable _outboxWake;
	std::deque<Completion> _outbox;
	std::mutex _slotMutex;
	std::condition_variable _slotWake;
	int _freeSlots = 0;
	uint64_t _deliveredTotal = 0;
	uint64_t _failedTotal = 0;
	std::unique_ptr<AsyncPool> _pool;

};

} // namespace fgpl
