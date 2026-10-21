#include "manager.h"

#include "base64.h"
#include "json.h"
#include "sha256.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <system_error>

namespace fgpl {
namespace {

namespace fs = std::filesystem;

[[nodiscard]] bool ValidAccount(const std::string &account) {
	if (account.empty() || account.size() > 32) {
		return false;
	}
	for (const auto ch : account) {
		if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
			|| (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] cbor::Bytes ReadFile(const fs::path &path) {
	auto stream = std::ifstream(path, std::ios::binary);
	return { std::istreambuf_iterator<char>(stream), {} };
}

[[nodiscard]] bool WriteFileAtomic(const fs::path &path, std::span<const uint8_t> data) {
	auto error = std::error_code();
	fs::create_directories(path.parent_path(), error);
	if (error) {
		return false;
	}
	const auto temporary = path.string() + ".tmp";
	{
		auto stream = std::ofstream(temporary, std::ios::binary | std::ios::trunc);
		if (!stream.write(
				reinterpret_cast<const char *>(data.data()),
				std::streamsize(data.size()))) {
			return false;
		}
	}
	fs::rename(temporary, path, error);
	if (error) {
		fs::remove(temporary, error);
		return false;
	}
	return true;
}

[[nodiscard]] cbor::Value Text(std::string_view text) {
	return cbor::Value::FromText(std::string(text));
}

[[nodiscard]] cbor::Value Strings(const std::set<std::string> &items) {
	auto result = cbor::Array();
	for (const auto &item : items) {
		result.push_back(Text(item));
	}
	return cbor::Value::FromArray(std::move(result));
}

} // namespace

PluginManager::PluginManager(ManagerConfig config)
: _config(std::move(config)) {
	if (!_config.services.clockMs) {
		_config.services.clockMs = [] {
			return int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		};
	}
	loadState();
}

PluginManager::~PluginManager() {
	shutdown();
}

int64_t PluginManager::now() const {
	return _config.services.clockMs();
}

fs::path PluginManager::versionDirectory(
		const std::string &id,
		const std::string &version) const {
	return _config.root / id / version;
}

fs::path PluginManager::archivePath(
		const std::string &id,
		const std::string &version) const {
	return versionDirectory(id, version) / "package.fgpl";
}

Result<Package, std::string> PluginManager::openInstalled(
		const Record &record,
		const std::string &version,
		const std::string &hash) const {
	const auto bytes = ReadFile(archivePath(record.id, version));
	if (bytes.empty()) {
		return std::string("the package file is missing");
	} else if (!hash.empty() && HashText(bytes) != hash) {
		return std::string("the package file was changed after it was installed");
	}
	auto opened = OpenPackage(bytes);
	if (!opened.ok()) {
		return opened.error().details.empty()
			? std::string("the package is damaged")
			: opened.error().details.front();
	}
	return std::move(opened.value());
}

cbor::Value PluginManager::stateDocument() const {
	auto plugins = cbor::Map();
	for (const auto &item : _records) {
		const auto &record = item.second;
		auto grants = cbor::Map();
		for (const auto &grant : record.grants) {
			auto entry = cbor::Map();
			entry.push_back({ "granted", Strings(grant.second.granted) });
			if (grant.second.chats) {
				auto chats = cbor::Array();
				for (const auto chat : *grant.second.chats) {
					chats.push_back(cbor::Value::FromInt(chat));
				}
				entry.push_back({ "chats", cbor::Value::FromArray(std::move(chats)) });
			}
			grants.push_back({ grant.first, cbor::Value::FromMap(std::move(entry)) });
		}
		plugins.push_back({ record.id, cbor::Value::FromMap({
			{ "name", Text(record.name) },
			{ "enabled", cbor::Value::FromBool(record.enabled) },
			{ "current", Text(record.current) },
			{ "previous", Text(record.previous) },
			{ "pending", cbor::Value::FromBool(record.pendingUpdate) },
			{ "publisher", Text(record.publisher) },
			{ "archive", Text(record.archiveHash) },
			{ "previousArchive", Text(record.previousArchiveHash) },
			{ "reason", Text(record.disabledReason) },
			{ "events", cbor::Value::FromInt(int64_t(record.stats.events)) },
			{ "failures", cbor::Value::FromInt(int64_t(record.stats.failures)) },
			{ "dropped", cbor::Value::FromInt(int64_t(record.stats.dropped)) },
			{ "grants", cbor::Value::FromMap(std::move(grants)) },
		}) });
	}
	return cbor::Value::FromMap({
		{ "schema", cbor::Value::FromInt(1) },
		{ "plugins", cbor::Value::FromMap(std::move(plugins)) },
	});
}

void PluginManager::saveState() {
	auto document = stateDocument();
	const auto text = json::Canonical(document);
	const auto mac = HmacSha256(
		_config.stateKey,
		{ reinterpret_cast<const uint8_t *>(text.data()), text.size() });
	document.asMap()->push_back({ "mac", Text(Base64Encode(mac)) });
	const auto out = json::Canonical(document);
	_saveFailed = !WriteFileAtomic(
		_config.root / "state.json",
		{ reinterpret_cast<const uint8_t *>(out.data()), out.size() });
}

void PluginManager::loadState() {
	const auto bytes = ReadFile(_config.root / "state.json");
	if (bytes.empty()) {
		return;
	}
	const auto parsed = json::Parse(
		std::string(bytes.begin(), bytes.end()),
		[] {
			auto limits = json::Limits();
			limits.maxBytes = size_t(16) << 20;
			limits.maxItems = 400000;
			return limits;
		}());
	auto trusted = parsed.ok() && parsed.value().asMap();
	if (trusted) {
		auto stripped = cbor::Map();
		auto mac = std::string();
		for (const auto &entry : *parsed.value().asMap()) {
			if (entry.key == "mac" && entry.value.asText()) {
				mac = *entry.value.asText();
			} else {
				stripped.push_back(entry);
			}
		}
		const auto text = json::Canonical(cbor::Value::FromMap(std::move(stripped)));
		const auto expected = HmacSha256(
			_config.stateKey,
			{ reinterpret_cast<const uint8_t *>(text.data()), text.size() });
		const auto given = Base64Decode(mac);
		trusted = given && ConstantTimeEqual(expected, *given);
	}
	_tampered = !trusted;
	const auto plugins = parsed.ok() ? parsed.value().find("plugins") : nullptr;
	if (!plugins || !plugins->asMap()) {
		return;
	}
	const auto text = [](const cbor::Value &map, const char *key) {
		const auto found = map.find(key);
		return (found && found->asText()) ? *found->asText() : std::string();
	};
	for (const auto &entry : *plugins->asMap()) {
		if (!entry.value.asMap()) {
			continue;
		}
		auto record = Record();
		record.id = entry.key;
		record.name = text(entry.value, "name");
		record.current = text(entry.value, "current");
		record.previous = text(entry.value, "previous");
		record.publisher = text(entry.value, "publisher");
		record.archiveHash = text(entry.value, "archive");
		record.previousArchiveHash = text(entry.value, "previousArchive");
		record.disabledReason = text(entry.value, "reason");
		const auto integer = [&](const char *key) {
			const auto found = entry.value.find(key);
			return (found && found->asInt()) ? uint64_t(*found->asInt()) : uint64_t(0);
		};
		record.stats = { integer("events"), integer("failures"), integer("dropped") };
		if (_tampered) {
			record.enabled = false;
			record.archiveHash.clear();
			record.previousArchiveHash.clear();
			record.pendingUpdate = false;
			record.disabledReason = "the plugin list was changed outside the app";
		} else {
			const auto enabled = entry.value.find("enabled");
			record.enabled = enabled && enabled->asBool().value_or(false);
			const auto pending = entry.value.find("pending");
			record.pendingUpdate = pending && pending->asBool().value_or(false);
			const auto grants = entry.value.find("grants");
			for (const auto &grant : (grants && grants->asMap()) ? *grants->asMap() : cbor::Map()) {
				if (!ValidAccount(grant.key) || !grant.value.asMap()) {
					continue;
				}
				auto stored = StoredGrant();
				if (const auto list = grant.value.find("granted")) {
					for (const auto &item : list->asArray() ? *list->asArray() : cbor::Array()) {
						if (const auto id = item.asText()) {
							stored.granted.insert(*id);
						}
					}
				}
				if (const auto chats = grant.value.find("chats")) {
					stored.chats = std::set<int64_t>();
					for (const auto &item : chats->asArray() ? *chats->asArray() : cbor::Array()) {
						if (const auto id = item.asInt()) {
							stored.chats->insert(*id);
						}
					}
				}
				record.grants.emplace(grant.key, std::move(stored));
			}
		}
		_records.emplace(record.id, std::move(record));
	}
}

bool PluginManager::mayRun() const {
	return !_locked && !_hidden;
}

void PluginManager::addLog(Record &record, std::string_view line) {
	record.log.emplace_back(line.substr(0, 500));
	while (record.log.size() > _config.logLines) {
		record.log.pop_front();
	}
}

Result<InstallReport, InstallFailure> PluginManager::install(
		std::span<const uint8_t> archive,
		const InstallOptions &options) {
	const auto fail = [](InstallError error, std::vector<std::string> details) {
		return InstallFailure{ error, std::move(details) };
	};
	if (_locked || _hidden) {
		return fail(InstallError::Locked, { "plugins cannot be installed now" });
	}
	auto opened = OpenPackage(archive);
	if (!opened.ok()) {
		return fail(InstallError::Package, opened.error().details);
	}
	auto &package = opened.value();
	const auto &manifest = package.manifest;

	const auto issues = CheckCompatibility(manifest, _config.environment);
	if (!issues.empty()) {
		auto details = std::vector<std::string>();
		for (const auto &issue : issues) {
			details.push_back(issue.detail);
		}
		return fail(InstallError::Incompatible, std::move(details));
	} else if (package.signature == SignatureState::Unsigned
		&& !options.acceptUnsigned) {
		return fail(InstallError::UnsignedRefused, { "the package is not signed" });
	}
	for (const auto &dependency : manifest.dependencies) {
		const auto found = _records.find(dependency.id);
		const auto version = (found != _records.end())
			? ParseVersion(found->second.current)
			: std::nullopt;
		if (!version || !dependency.range || !dependency.range->matches(*version)) {
			return fail(InstallError::MissingDependency, { dependency.id });
		}
	}
	for (const auto &conflict : manifest.conflicts) {
		const auto found = _records.find(conflict.id);
		if (found == _records.end()) {
			continue;
		}
		const auto version = ParseVersion(found->second.current);
		if (!conflict.range || (version && conflict.range->matches(*version))) {
			return fail(InstallError::Conflict, { conflict.id });
		}
	}

	auto report = InstallReport();
	report.id = manifest.id;
	report.version = manifest.version.text();
	report.signedPackage = package.signature == SignatureState::Valid;
	report.publisher = package.publisher;

	const auto existing = _records.find(manifest.id);
	auto previousManifest = std::optional<Manifest>();
	if (existing != _records.end()) {
		const auto current = ParseVersion(existing->second.current);
		if (current) {
			const auto order = Compare(manifest.version, *current);
			if (order == 0) {
				return fail(InstallError::SameVersion, { existing->second.current });
			} else if (order < 0 && !options.allowDowngrade) {
				return fail(InstallError::DowngradeRefused, { existing->second.current });
			}
		}
		if (!existing->second.publisher.empty()
			&& existing->second.publisher != package.publisher
			&& !options.acceptNewPublisher) {
			return fail(InstallError::PublisherChanged, {
				existing->second.publisher,
				package.publisher,
			});
		}
		const auto old = openInstalled(
			existing->second,
			existing->second.current,
			existing->second.archiveHash);
		if (old.ok()) {
			previousManifest = old.value().manifest;
		}
	}

	if (!WriteFileAtomic(archivePath(manifest.id, report.version), archive)) {
		return fail(InstallError::Io, { "cannot store the package" });
	}
	if (existing == _records.end()) {
		auto record = Record();
		record.id = manifest.id;
		record.name = manifest.name.get("default");
		record.current = report.version;
		record.publisher = package.publisher;
		record.archiveHash = HashText(archive);
		_records.emplace(record.id, std::move(record));
	} else {
		auto &record = existing->second;
		stopPlugin(record.id);
		if (!record.previous.empty() && record.previous != record.current) {
			auto error = std::error_code();
			fs::remove_all(versionDirectory(record.id, record.previous), error);
		}
		record.previous = record.current;
		record.previousArchiveHash = record.archiveHash;
		record.current = report.version;
		record.archiveHash = HashText(archive);
		record.pendingUpdate = true;
		record.publisher = package.publisher;
		record.name = manifest.name.get("default");
		record.disabledReason.clear();
		record.failures.clear();
		auto requested = std::set<std::string>();
		for (const auto &request : manifest.permissions) {
			requested.insert(request.id);
		}
		for (auto &grant : record.grants) {
			std::erase_if(grant.second.granted, [&](const std::string &id) {
				return !requested.contains(id);
			});
		}
		report.updated = true;
		if (previousManifest) {
			report.newRequests = NewRequests(*previousManifest, manifest);
		}
	}
	saveState();
	return report;
}

void PluginManager::stop(const Key &key) {
	const auto i = _instances.find(key);
	if (i == _instances.end()) {
		return;
	}
	if (i->second->storage) {
		i->second->storage->flush();
	}
	if (i->second->client) {
		i->second->client->kill();
	}
	_instances.erase(i);
}

void PluginManager::stopPlugin(const std::string &id) {
	auto keys = std::vector<Key>();
	for (const auto &item : _instances) {
		if (item.first.first == id) {
			keys.push_back(item.first);
		}
	}
	for (const auto &key : keys) {
		stop(key);
	}
}

bool PluginManager::uninstall(const std::string &id, bool removeData) {
	const auto found = _records.find(id);
	if (found == _records.end()) {
		return false;
	}
	stopPlugin(id);
	auto error = std::error_code();
	if (removeData) {
		fs::remove_all(_config.root / id, error);
	} else {
		for (const auto &version : { found->second.current, found->second.previous }) {
			if (!version.empty()) {
				fs::remove_all(versionDirectory(id, version), error);
			}
		}
	}
	_records.erase(found);
	saveState();
	return true;
}

bool PluginManager::setEnabled(const std::string &id, bool enabled) {
	const auto found = _records.find(id);
	if (found == _records.end()) {
		return false;
	}
	auto &record = found->second;
	if (!enabled) {
		record.enabled = false;
		stopPlugin(id);
		saveState();
		return true;
	}
	const auto opened = openInstalled(record, record.current, record.archiveHash);
	if (!opened.ok()) {
		record.disabledReason = opened.error();
		saveState();
		return false;
	}
	if (record.archiveHash.empty()) {
		const auto bytes = ReadFile(archivePath(id, record.current));
		record.archiveHash = HashText(bytes);
	}
	record.enabled = true;
	record.disabledReason.clear();
	record.failures.clear();
	for (auto i = _notBefore.begin(); i != _notBefore.end();) {
		i = (i->first.first == id) ? _notBefore.erase(i) : std::next(i);
	}
	saveState();
	return true;
}

bool PluginManager::grant(
		const std::string &id,
		const std::string &account,
		const GrantDecision &decision) {
	const auto found = _records.find(id);
	if (found == _records.end() || !ValidAccount(account)) {
		return false;
	}
	auto &record = found->second;
	const auto opened = openInstalled(record, record.current, record.archiveHash);
	if (!opened.ok()) {
		return false;
	}
	auto stored = StoredGrant();
	for (const auto &request : opened.value().manifest.permissions) {
		if (decision.granted.contains(request.id)) {
			stored.granted.insert(request.id);
		}
	}
	stored.chats = decision.chats;
	record.grants[account] = std::move(stored);
	stop({ id, account });
	saveState();
	return true;
}

std::optional<GrantDecision> PluginManager::grants(
		const std::string &id,
		const std::string &account) const {
	const auto found = _records.find(id);
	if (found == _records.end()) {
		return std::nullopt;
	}
	const auto grant = found->second.grants.find(account);
	if (grant == found->second.grants.end()) {
		return std::nullopt;
	}
	return GrantDecision{ grant->second.granted, grant->second.chats };
}

std::vector<PluginInfo> PluginManager::list() const {
	auto result = std::vector<PluginInfo>();
	if (_hidden) {
		return result;
	}
	for (const auto &item : _records) {
		const auto &record = item.second;
		auto info = PluginInfo();
		info.id = record.id;
		info.name = record.name;
		info.version = record.current;
		info.previousVersion = record.previous;
		info.enabled = record.enabled;
		info.pendingUpdate = record.pendingUpdate;
		info.publisher = record.publisher;
		info.signedPackage = !record.publisher.empty();
		info.disabledReason = record.disabledReason;
		info.stats = record.stats;
		const auto opened = openInstalled(record, record.current, record.archiveHash);
		if (opened.ok()) {
			info.requests = opened.value().manifest.permissions;
			info.issues = CheckCompatibility(
				opened.value().manifest,
				_config.environment);
		}
		auto running = false;
		for (const auto &instance : _instances) {
			running = running || instance.first.first == record.id;
		}
		info.status = running
			? PluginStatus::Running
			: (!record.enabled ? PluginStatus::Disabled : PluginStatus::Stopped);
		result.push_back(std::move(info));
	}
	return result;
}

std::vector<std::string> PluginManager::log(const std::string &id) const {
	const auto found = _records.find(id);
	if (found == _records.end()) {
		return {};
	}
	return { found->second.log.begin(), found->second.log.end() };
}

bool PluginManager::loadPython() {
	if (_pythonLoaded) {
		return _pythonAvailable;
	}
	_pythonLoaded = true;
	const auto &python = _config.python;
	if (python.wasm.empty() || !fs::exists(python.wasm)) {
		return false;
	}
	_pythonWasm = ReadFile(python.wasm);
	if (!python.expectedHash.empty() && HashText(_pythonWasm) != python.expectedHash) {
		_pythonWasm.clear();
		return false;
	}
	auto error = std::error_code();
	for (const auto &entry : fs::recursive_directory_iterator(python.library, error)) {
		if (entry.is_regular_file()) {
			_pythonFiles.emplace_back(
				"runtime/lib/" + fs::relative(entry.path(), python.library).generic_string(),
				ReadFile(entry.path()));
		}
	}
	for (const auto &entry : fs::recursive_directory_iterator(python.sdk, error)) {
		if (entry.is_regular_file() && entry.path().extension() == ".py") {
			_pythonFiles.emplace_back(
				"runtime/sdk/" + fs::relative(entry.path(), python.sdk).generic_string(),
				ReadFile(entry.path()));
		}
	}
	_pythonAvailable = !_pythonWasm.empty() && !_pythonFiles.empty();
	return _pythonAvailable;
}

Result<LoadRequest, std::string> PluginManager::buildRequest(
		const Package &package) {
	const auto &manifest = package.manifest;
	auto request = LoadRequest();
	request.limits.memoryBytes = uint64_t(manifest.memoryMb) << 20;
	request.limits.callTimeoutMs = uint32_t(manifest.eventTimeoutMs);
	request.files = PluginFiles(package);
	if (manifest.kind == "wasm") {
		request.wasm = package.files.at(manifest.module);
		return request;
	}
	if (!loadPython()) {
		return std::string("the Python runtime is not installed");
	}
	request.wasm = _pythonWasm;
	request.files.insert(request.files.end(), _pythonFiles.begin(), _pythonFiles.end());
	request.args = { "python", "-S", "-B", "/plugin/" + manifest.entry };
	request.env = {
		"PYTHONHOME=/runtime",
		"PYTHONPATH=/runtime/sdk:/plugin",
		"PYTHONDONTWRITEBYTECODE=1",
		"PYTHONNOUSERSITE=1",
		"PYTHONUTF8=1",
		"PYTHONUNBUFFERED=1",
		"PYTHONHASHSEED=0",
	};
	request.limits.memoryBytes = std::max<uint64_t>(request.limits.memoryBytes, uint64_t(128) << 20);
	return request;
}

std::string PluginManager::start(
		Record &record,
		const std::string &account,
		std::unique_ptr<Instance> &out) {
	const auto opened = openInstalled(record, record.current, record.archiveHash);
	if (!opened.ok()) {
		return "the plugin files are not usable: " + opened.error();
	}
	const auto &package = opened.value();
	if (!CheckCompatibility(package.manifest, _config.environment).empty()) {
		return "the plugin is not compatible with this version";
	}
	const auto grant = record.grants.find(account);
	if (grant == record.grants.end()) {
		return "no permissions were granted for this account";
	}
	auto request = buildRequest(package);
	if (!request.ok()) {
		return request.error();
	}

	auto instance = std::make_unique<Instance>();
	instance->id = record.id;
	instance->account = account;
	instance->storage = std::make_unique<FileStorage>(
		_config.root / record.id / "data" / account / "store.cbor");
	auto services = BrokerServices();
	services.storage = instance->storage.get();
	services.data = _config.services.data;
	services.transport = _config.services.transport;
	services.clockMs = _config.services.clockMs;
	const auto raw = &record;
	services.log = [this, raw](std::string_view line) {
		addLog(*raw, line);
	};
	instance->broker = std::make_unique<Broker>(
		MakeGrants(package.manifest, { grant->second.granted, grant->second.chats }),
		std::move(services));

	auto hostConfig = _config.host;
	if (package.manifest.kind == "python") {
		hostConfig.addressSpaceMb = std::max(hostConfig.addressSpaceMb, 2048);
	}
	auto started = HostClient::Start(hostConfig);
	if (!started.ok()) {
		return "the host did not start: " + started.error().detail;
	}
	instance->client = std::move(started.value());
	const auto loaded = LoadWithCache(
		*instance->client,
		hostConfig,
		std::move(request.value()),
		_config.cache);
	if (!loaded.ok()) {
		return "the module did not load: " + loaded.error().detail;
	}
	const auto init = instance->client->init(
		cbor::Value::FromMap({
			{ "account", Text(account) },
			{ "plugin", Text(record.id) },
			{ "version", Text(record.current) },
			{ "api", cbor::Value::FromInt(api::kVersion) },
		}),
		*instance->broker);
	if (!init.ok()) {
		return "the plugin failed to start: " + init.error().detail;
	}
	instance->storage->flush();
	out = std::move(instance);
	return {};
}

PluginManager::Instance *PluginManager::ensure(
		Record &record,
		const std::string &account) {
	const auto key = Key{ record.id, account };
	if (const auto i = _instances.find(key); i != _instances.end()) {
		if (i->second->client->alive()) {
			return i->second.get();
		}
		stop(key);
	}
	if (!mayRun() || !record.enabled || !record.grants.contains(account)) {
		return nullptr;
	} else if (const auto gate = _notBefore.find(key);
		gate != _notBefore.end() && now() < gate->second) {
		return nullptr;
	}
	auto instance = std::unique_ptr<Instance>();
	const auto problem = start(record, account, instance);
	if (!problem.empty()) {
		noteFailure(record, account, problem);
		return nullptr;
	}
	addLog(record, "started for " + account);
	return _instances.emplace(key, std::move(instance)).first->second.get();
}

void PluginManager::noteFailure(
		Record &record,
		const std::string &account,
		const std::string &why) {
	addLog(record, "failure: " + why);
	++record.stats.failures;
	const auto time = now();
	record.failures.push_back(time);
	while (!record.failures.empty()
		&& record.failures.front() < time - _config.failureWindowMs) {
		record.failures.pop_front();
	}
	const auto key = Key{ record.id, account };
	const auto streak = std::min(++_streak[key], 6);
	_notBefore[key] = time + (int64_t(1000) << (streak - 1));

	if (record.pendingUpdate && !record.previous.empty()) {
		stopPlugin(record.id);
		auto error = std::error_code();
		fs::remove_all(versionDirectory(record.id, record.current), error);
		record.current = record.previous;
		record.archiveHash = record.previousArchiveHash;
		record.previous.clear();
		record.previousArchiveHash.clear();
		record.pendingUpdate = false;
		record.failures.clear();
		_streak.erase(key);
		_notBefore.erase(key);
		addLog(record, "rolled back to " + record.current);
	} else if (int(record.failures.size()) >= _config.maxFailures) {
		record.enabled = false;
		record.disabledReason = "stopped after repeated failures: " + why;
		stopPlugin(record.id);
	}
	saveState();
}

void PluginManager::commitUpdate(Record &record) {
	if (!record.pendingUpdate) {
		return;
	}
	record.pendingUpdate = false;
	if (!record.previous.empty()) {
		auto error = std::error_code();
		fs::remove_all(versionDirectory(record.id, record.previous), error);
	}
	record.previous.clear();
	record.previousArchiveHash.clear();
	saveState();
}

DispatchReport PluginManager::dispatch(
		const std::string &account,
		const cbor::Value &event) {
	auto report = DispatchReport();
	if (!ValidAccount(account) || !mayRun()) {
		return report;
	}
	auto ids = std::vector<std::string>();
	for (const auto &item : _records) {
		ids.push_back(item.first);
	}
	for (const auto &id : ids) {
		const auto found = _records.find(id);
		if (found == _records.end()) {
			continue;
		}
		auto &record = found->second;
		if (!record.enabled || !record.grants.contains(account)) {
			continue;
		}
		const auto key = Key{ id, account };
		const auto before = _instances.size();
		auto *instance = ensure(record, account);
		if (!instance) {
			++record.stats.dropped;
			++report.skipped;
			continue;
		}
		report.started += int(_instances.size() > before);
		const auto filtered = instance->broker->filterEvent(event);
		if (!filtered) {
			++report.skipped;
			continue;
		}
		const auto result = instance->client->deliver(*filtered, *instance->broker);
		instance->storage->flush();
		if (result.ok()) {
			++report.delivered;
			++record.stats.events;
			_streak.erase(key);
			if (!instance->committed) {
				instance->committed = true;
				commitUpdate(record);
			}
			continue;
		}
		++report.failed;
		stop(key);
		noteFailure(record, account, result.error().detail.empty()
			? std::string("the plugin stopped answering")
			: result.error().detail);
	}
	return report;
}

DispatchReport PluginManager::startEnabled(const std::string &account) {
	auto report = DispatchReport();
	if (!ValidAccount(account) || !mayRun()) {
		return report;
	}
	auto ids = std::vector<std::string>();
	for (const auto &item : _records) {
		ids.push_back(item.first);
	}
	for (const auto &id : ids) {
		const auto found = _records.find(id);
		if (found == _records.end()) {
			continue;
		}
		const auto before = _instances.size();
		if (ensure(found->second, account)) {
			report.started += int(_instances.size() > before);
		}
	}
	return report;
}

void PluginManager::onLock() {
	_locked = true;
	auto keys = std::vector<Key>();
	for (const auto &item : _instances) {
		keys.push_back(item.first);
	}
	for (const auto &key : keys) {
		stop(key);
	}
}

void PluginManager::onUnlock() {
	_locked = false;
}

void PluginManager::setHidden(bool hidden) {
	_hidden = hidden;
	if (hidden) {
		onLock();
		_locked = false;
	}
}

void PluginManager::onLogout(const std::string &account) {
	auto keys = std::vector<Key>();
	for (const auto &item : _instances) {
		if (item.first.second == account) {
			keys.push_back(item.first);
		}
	}
	for (const auto &key : keys) {
		stop(key);
	}
	for (auto &item : _records) {
		item.second.grants.erase(account);
	}
	saveState();
}

void PluginManager::shutdown() {
	auto keys = std::vector<Key>();
	for (const auto &item : _instances) {
		keys.push_back(item.first);
	}
	for (const auto &key : keys) {
		stop(key);
	}
	if (!_records.empty()) {
		saveState();
	}
}

} // namespace fgpl
