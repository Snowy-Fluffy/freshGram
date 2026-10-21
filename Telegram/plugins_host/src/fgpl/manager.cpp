#include "fgpl/manager.h"

#include "fgpl/base64.h"
#include "fgpl/json.h"
#include "fgpl/sha256.h"

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
	_freeSlots = std::max(1, _config.maxConcurrentStarts);
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

namespace {

void AppendLog(LogRing &ring, size_t maxLines, std::string_view line) {
	const auto lock = std::scoped_lock(ring.mutex);
	ring.lines.emplace_back(line.substr(0, 500));
	while (ring.lines.size() > maxLines) {
		ring.lines.pop_front();
	}
}

[[nodiscard]] size_t EncodedSize(const cbor::Value &event) {
	auto bytes = cbor::Bytes();
	return cbor::Encode(event, bytes, 16) ? bytes.size() : 0;
}

} // namespace

void PluginManager::addLog(Record &record, std::string_view line) {
	AppendLog(*record.log, _config.logLines, line);
}

std::vector<std::string> PluginManager::log(const std::string &id) const {
	const auto found = _records.find(id);
	if (found == _records.end()) {
		return {};
	}
	const auto lock = std::scoped_lock(found->second.log->mutex);
	return { found->second.log->lines.begin(), found->second.log->lines.end() };
}

void PluginManager::post(Completion completion) {
	{
		const auto lock = std::scoped_lock(_outboxMutex);
		_outbox.push_back(std::move(completion));
	}
	_outboxWake.notify_all();
}

void PluginManager::stopMany(const std::vector<Key> &keys) {
	auto victims = std::vector<Instance *>();
	for (const auto &key : keys) {
		const auto i = _instances.find(key);
		if (i == _instances.end()) {
			continue;
		}
		auto &instance = *i->second;
		{
			const auto lock = std::scoped_lock(instance.mutex);
			instance.closing = true;
			instance.queue.clear();
			instance.queuedBytes = 0;
		}
		instance.cancel.store(true);
		instance.wake.notify_all();
		if (const auto client = instance.live.load()) {
			client->abort();
		}
		victims.push_back(&instance);
	}
	{
		const auto lock = std::scoped_lock(_slotMutex);
	}
	_slotWake.notify_all();
	for (const auto instance : victims) {
		if (instance->thread.joinable()) {
			instance->thread.join();
		}
		if (instance->client) {
			instance->client->kill();
		}
		if (instance->storage) {
			instance->storage->flush();
		}
	}
	for (const auto &key : keys) {
		_instances.erase(key);
	}
}

void PluginManager::stop(const Key &key) {
	stopMany({ key });
}

void PluginManager::stopPlugin(const std::string &id) {
	auto keys = std::vector<Key>();
	for (const auto &item : _instances) {
		if (item.first.first == id) {
			keys.push_back(item.first);
		}
	}
	stopMany(keys);
}

void PluginManager::stopAll() {
	auto keys = std::vector<Key>();
	for (const auto &item : _instances) {
		keys.push_back(item.first);
	}
	stopMany(keys);
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

Result<std::unique_ptr<PluginManager::Instance>, std::string> PluginManager::prepare(
		Record &record,
		const std::string &account) {
	const auto opened = openInstalled(record, record.current, record.archiveHash);
	if (!opened.ok()) {
		return "the plugin files are not usable: " + opened.error();
	}
	const auto &package = opened.value();
	if (!CheckCompatibility(package.manifest, _config.environment).empty()) {
		return std::string("the plugin is not compatible with this version");
	}
	const auto grant = record.grants.find(account);
	if (grant == record.grants.end()) {
		return std::string("no permissions were granted for this account");
	}
	auto request = buildRequest(package);
	if (!request.ok()) {
		return request.error();
	}

	auto instance = std::make_unique<Instance>();
	instance->id = record.id;
	instance->account = account;
	instance->generation = _nextGeneration++;
	instance->storage = std::make_unique<FileStorage>(
		_config.root / record.id / "data" / account / "store.cbor");
	auto services = BrokerServices();
	services.storage = instance->storage.get();
	services.data = _config.services.dataFor
		? _config.services.dataFor(account)
		: _config.services.data;
	services.transport = _config.services.transport;
	services.clockMs = _config.services.clockMs;
	const auto ring = record.log;
	const auto maxLines = _config.logLines;
	services.log = [ring, maxLines](std::string_view line) {
		AppendLog(*ring, maxLines, line);
	};
	instance->broker = std::make_unique<Broker>(
		MakeGrants(package.manifest, { grant->second.granted, grant->second.chats }),
		std::move(services));

	instance->plan.host = _config.host;
	if (package.manifest.kind == "python") {
		instance->plan.host.addressSpaceMb = std::max(
			instance->plan.host.addressSpaceMb,
			2048);
	}
	instance->plan.request = std::move(request.value());
	instance->plan.cache = _config.cache;
	instance->plan.init = cbor::Value::FromMap({
		{ "account", Text(account) },
		{ "plugin", Text(record.id) },
		{ "version", Text(record.current) },
		{ "api", cbor::Value::FromInt(api::kVersion) },
	});
	return instance;
}

void PluginManager::run(Instance &instance) {
	const auto fail = [&](std::string detail) {
		post({
			instance.id,
			instance.account,
			instance.generation,
			Completion::Kind::Failed,
			std::move(detail),
		});
		const auto lock = std::scoped_lock(instance.mutex);
		instance.finished = true;
	};
	{
		auto lock = std::unique_lock(_slotMutex);
		_slotWake.wait(lock, [&] {
			return _freeSlots > 0 || instance.cancel.load();
		});
		if (instance.cancel.load()) {
			lock.unlock();
			fail("cancelled");
			return;
		}
		--_freeSlots;
	}
	const auto releaseSlot = [&] {
		{
			const auto lock = std::scoped_lock(_slotMutex);
			++_freeSlots;
		}
		_slotWake.notify_one();
	};

	auto problem = std::string();
	auto started = HostClient::Start(instance.plan.host);
	if (!started.ok()) {
		problem = "the host did not start: " + started.error().detail;
	} else {
		instance.client = std::move(started.value());
		instance.live.store(instance.client.get());
		if (instance.cancel.load()) {
			problem = "cancelled";
		} else {
			const auto loaded = LoadWithCache(
				*instance.client,
				instance.plan.host,
				std::move(instance.plan.request),
				instance.plan.cache,
				&instance.cancel);
			if (!loaded.ok()) {
				problem = "the module did not load: " + loaded.error().detail;
			} else if (const auto init = instance.client->init(
					instance.plan.init,
					*instance.broker); !init.ok()) {
				problem = "the plugin failed to start: " + init.error().detail;
			}
		}
	}
	releaseSlot();
	instance.plan.request = LoadRequest();
	if (!problem.empty()) {
		fail(std::move(problem));
		return;
	}
	instance.storage->flush();
	post({
		instance.id,
		instance.account,
		instance.generation,
		Completion::Kind::Started,
		{},
	});
	{
		const auto lock = std::scoped_lock(instance.mutex);
		instance.ready = true;
	}

	while (true) {
		auto event = cbor::Value();
		{
			auto lock = std::unique_lock(instance.mutex);
			instance.wake.wait(lock, [&] {
				return instance.closing || !instance.queue.empty();
			});
			if (instance.closing) {
				return;
			}
			event = std::move(instance.queue.front().first);
			instance.queuedBytes -= instance.queue.front().second;
			instance.queue.pop_front();
			instance.busy = true;
		}
		const auto result = instance.client->deliver(event, *instance.broker);
		instance.storage->flush();
		if (result.ok()) {
			post({
				instance.id,
				instance.account,
				instance.generation,
				Completion::Kind::Delivered,
				{},
			});
			const auto lock = std::scoped_lock(instance.mutex);
			instance.busy = false;
			continue;
		}
		fail(result.error().detail.empty()
			? std::string("the plugin stopped answering")
			: result.error().detail);
		return;
	}
}

PluginManager::Instance *PluginManager::ensure(
		Record &record,
		const std::string &account) {
	const auto key = Key{ record.id, account };
	if (const auto i = _instances.find(key); i != _instances.end()) {
		return i->second.get();
	}
	if (!mayRun() || !record.enabled || !record.grants.contains(account)) {
		return nullptr;
	} else if (const auto gate = _notBefore.find(key);
		gate != _notBefore.end() && now() < gate->second) {
		return nullptr;
	}
	auto prepared = prepare(record, account);
	if (!prepared.ok()) {
		noteFailure(record, account, prepared.error());
		return nullptr;
	}
	const auto instance = prepared.value().get();
	_instances.emplace(key, std::move(prepared.value()));
	instance->thread = std::thread([this, instance] {
		run(*instance);
	});
	return instance;
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

int PluginManager::pump() {
	auto batch = std::deque<Completion>();
	{
		const auto lock = std::scoped_lock(_outboxMutex);
		batch.swap(_outbox);
	}
	auto handled = 0;
	for (auto &completion : batch) {
		const auto key = Key{ completion.id, completion.account };
		const auto found = _instances.find(key);
		const auto record = _records.find(completion.id);
		if (found == _instances.end()
			|| found->second->generation != completion.generation
			|| record == _records.end()) {
			continue;
		}
		++handled;
		auto &instance = *found->second;
		switch (completion.kind) {
		case Completion::Kind::Started:
			addLog(record->second, "started for " + completion.account);
			break;
		case Completion::Kind::Delivered:
			++record->second.stats.events;
			++_deliveredTotal;
			_streak.erase(key);
			if (!instance.committed) {
				instance.committed = true;
				commitUpdate(record->second);
			}
			break;
		case Completion::Kind::Failed:
			++_failedTotal;
			stop(key);
			noteFailure(record->second, completion.account, completion.detail);
			break;
		}
	}
	return handled;
}

bool PluginManager::drain(int timeoutMs) {
	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(timeoutMs);
	while (true) {
		pump();
		auto idle = true;
		for (const auto &item : _instances) {
			auto &instance = *item.second;
			const auto lock = std::scoped_lock(instance.mutex);
			idle = idle && (instance.finished
				|| (instance.ready && instance.queue.empty() && !instance.busy));
		}
		{
			const auto lock = std::scoped_lock(_outboxMutex);
			idle = idle && _outbox.empty();
		}
		if (idle) {
			return true;
		} else if (std::chrono::steady_clock::now() >= deadline) {
			return false;
		}
		auto lock = std::unique_lock(_outboxMutex);
		_outboxWake.wait_for(lock, std::chrono::milliseconds(20), [&] {
			return !_outbox.empty();
		});
	}
}

DispatchReport PluginManager::dispatch(
		const std::string &account,
		const cbor::Value &event) {
	auto report = DispatchReport();
	if (!ValidAccount(account) || !mayRun()) {
		return report;
	}
	pump();
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
		const auto size = EncodedSize(*filtered);
		{
			const auto lock = std::scoped_lock(instance->mutex);
			if (instance->closing || instance->finished) {
				++report.skipped;
				continue;
			}
			while (!instance->queue.empty()
				&& (instance->queue.size() >= _config.maxQueuedEvents
					|| instance->queuedBytes + size > _config.maxQueuedBytes)) {
				instance->queuedBytes -= instance->queue.front().second;
				instance->queue.pop_front();
				++record.stats.dropped;
				++report.dropped;
			}
			instance->queue.emplace_back(*filtered, size);
			instance->queuedBytes += size;
		}
		instance->wake.notify_one();
		++report.queued;
	}
	return report;
}

DispatchReport PluginManager::dispatchAndWait(
		const std::string &account,
		const cbor::Value &event,
		int timeoutMs) {
	const auto delivered = _deliveredTotal;
	const auto failed = _failedTotal;
	auto report = dispatch(account, event);
	drain(timeoutMs);
	report.delivered = int(_deliveredTotal - delivered);
	report.failed = int(_failedTotal - failed);
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
	stopAll();
}

void PluginManager::onUnlock() {
	_locked = false;
}

void PluginManager::setHidden(bool hidden) {
	_hidden = hidden;
	if (hidden) {
		stopAll();
	}
}

void PluginManager::onLogout(const std::string &account) {
	auto keys = std::vector<Key>();
	for (const auto &item : _instances) {
		if (item.first.second == account) {
			keys.push_back(item.first);
		}
	}
	stopMany(keys);
	for (auto &item : _records) {
		item.second.grants.erase(account);
	}
	saveState();
}

void PluginManager::shutdown() {
	stopAll();
	if (!_records.empty()) {
		saveState();
	}
}

} // namespace fgpl
