#pragma once

#include "broker.h"
#include "cached_load.h"
#include "file_storage.h"
#include "host_client.h"
#include "manifest.h"
#include "package.h"
#include "permissions.h"

#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace fgpl {

struct PythonRuntime {
	std::filesystem::path wasm;
	std::filesystem::path library;
	std::filesystem::path sdk;
	std::string expectedHash;
	std::string version = "3.14";
};

struct ManagerServices {
	DataProvider *data = nullptr;
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
	PluginStats stats;
};

struct DispatchReport {
	int delivered = 0;
	int skipped = 0;
	int failed = 0;
	int started = 0;
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
	[[nodiscard]] std::vector<std::string> log(const std::string &id) const;

	void onLock();
	void onUnlock();
	void onLogout(const std::string &account);
	void setHidden(bool hidden);

	DispatchReport dispatch(const std::string &account, const cbor::Value &event);
	DispatchReport startEnabled(const std::string &account);

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
		std::deque<std::string> log;
	};

	struct Instance {
		std::string id;
		std::string account;
		std::unique_ptr<FileStorage> storage;
		std::unique_ptr<Broker> broker;
		std::unique_ptr<HostClient> client;
		bool committed = false;
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

	[[nodiscard]] Instance *ensure(Record &record, const std::string &account);
	[[nodiscard]] std::string start(
		Record &record,
		const std::string &account,
		std::unique_ptr<Instance> &out);
	void stop(const Key &key);
	void stopPlugin(const std::string &id);
	void noteFailure(Record &record, const std::string &account, const std::string &why);
	void commitUpdate(Record &record);
	void addLog(Record &record, std::string_view line);
	[[nodiscard]] bool mayRun() const;

	[[nodiscard]] Result<LoadRequest, std::string> buildRequest(
		const Package &package);
	[[nodiscard]] bool loadPython();

	ManagerConfig _config;
	std::map<std::string, Record> _records;
	std::map<Key, std::unique_ptr<Instance>> _instances;
	std::map<Key, int64_t> _notBefore;
	std::map<Key, int> _streak;
	bool _locked = false;
	bool _hidden = false;
	bool _tampered = false;
	bool _saveFailed = false;
	bool _pythonLoaded = false;
	bool _pythonAvailable = false;
	cbor::Bytes _pythonWasm;
	std::vector<std::pair<std::string, cbor::Bytes>> _pythonFiles;

};

} // namespace fgpl
