#pragma once

#include "fgpl/broker.h"
#include "fgpl/cached_load.h"
#include "fgpl/file_storage.h"
#include "fgpl/host_client.h"
#include "fgpl/manifest.h"
#include "fgpl/package.h"
#include "fgpl/permissions.h"

#include <atomic>
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

struct LogRing {
	std::mutex mutex;
	std::deque<std::string> lines;
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
	size_t maxQueuedBytes = size_t(8) << 20;
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
	std::string text;
	std::string plugin;
	std::string reason;
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
	PluginStatus status = PluginStatus::Stopped;
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
};

struct DispatchReport {
	int queued = 0;
	int skipped = 0;
	int dropped = 0;
	int started = 0;
	int delivered = 0;
	int failed = 0;
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

	int pump();
	bool drain(int timeoutMs = 60000);

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
	[[nodiscard]] bool wantsEvents() const;

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
		PluginStats stats;
		std::deque<int64_t> failures;
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
		std::optional<std::string> text;
	};

	struct StartPlan {
		HostConfig host;
		LoadRequest request;
		CacheSettings cache;
		cbor::Value init;
	};

	struct Instance {
		std::string id;
		std::string account;
		uint64_t generation = 0;
		std::unique_ptr<FileStorage> storage;
		std::unique_ptr<Broker> broker;
		std::unique_ptr<HostClient> client;
		StartPlan plan;
		Declared declared;
		std::thread thread;
		std::mutex mutex;
		std::condition_variable wake;
		std::deque<std::pair<cbor::Value, size_t>> queue;
		std::deque<std::shared_ptr<Outgoing>> outgoing;
		size_t queuedBytes = 0;
		bool closing = false;
		bool busy = false;
		bool ready = false;
		bool finished = false;
		std::atomic<bool> cancel{ false };
		std::atomic<bool> unload{ false };
		std::atomic<HostClient*> live{ nullptr };
		std::atomic<int> pid{ -1 };
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
		} kind = Kind::Started;
		std::string detail;
	};

	using Key = std::pair<std::string, std::string>;

	[[nodiscard]] int64_t now() const;
	[[nodiscard]] std::filesystem::path versionDirectory(
		const std::string &id,
		const std::string &version) const;
	[[nodiscard]] std::filesystem::path archivePath(
		const std::string &id,
		const std::string &version) const;
	[[nodiscard]] Result<Package, std::string> openInstalled(
		const Record &record,
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
	[[nodiscard]] bool activatedBy(const Record &record, const char *trigger) const;
	void sampleUsage(Record &record, const Instance &instance);
	[[nodiscard]] Result<std::unique_ptr<Instance>, std::string> prepare(
		Record &record,
		const std::string &account);
	void run(Instance &instance);
	void post(Completion completion);
	void stop(const Key &key);
	void stopMany(const std::vector<Key> &keys);
	void stopPlugin(const std::string &id);
	void stopAll();
	void noteFailure(Record &record, const std::string &account, const std::string &why);
	void commitUpdate(Record &record);
	void addLog(Record &record, std::string_view line);
	[[nodiscard]] bool mayRun() const;

	[[nodiscard]] Result<LoadRequest, std::string> buildRequest(
		const Package &package);
	[[nodiscard]] bool loadPython();

	ManagerConfig _config;
	mutable std::map<std::string, std::pair<std::string, std::shared_ptr<const Manifest>>> _manifests;
	std::map<std::string, Record> _records;
	std::map<Key, std::unique_ptr<Instance>> _instances;
	std::map<Key, int64_t> _notBefore;
	std::map<std::string, uint64_t> _finishedCpuMs;
	std::map<Key, int> _streak;
	bool _locked = false;
	bool _hidden = false;
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
	bool _pythonLoaded = false;
	bool _pythonAvailable = false;
	cbor::Bytes _pythonWasm;
	std::vector<std::pair<std::string, cbor::Bytes>> _pythonFiles;

};

} // namespace fgpl
