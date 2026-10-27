#include "fgpl/manager.h"

#include "fgpl/base64.h"
#include "fgpl/json.h"
#include "fgpl/sha256.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

#include <unistd.h>

#ifdef __GLIBC__
#include <malloc.h>
#endif // __GLIBC__

namespace fgpl {
namespace {

namespace fs = std::filesystem;

constexpr auto kMaxMail = size_t(1024);
constexpr auto kAllAccounts = "*";
constexpr auto kAsyncThreads = 8;
constexpr auto kResultOverhead = size_t(1024);
constexpr auto kMaxTimers = size_t(32);
constexpr auto kMaxIconBytes = size_t(64) << 10;

[[nodiscard]] size_t EncodedSize(const cbor::Value &event);

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
	auto error = std::error_code();
	const auto size = fs::file_size(path, error);
	if (error) {
		return {};
	}
	auto stream = std::ifstream(path, std::ios::binary);
	auto result = cbor::Bytes(size_t(size));
	if (!stream.read(
			reinterpret_cast<char *>(result.data()),
			std::streamsize(result.size()))) {
		return {};
	}
	return result;
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
: _config(std::move(config))
, _pool(std::make_unique<AsyncPool>(kAsyncThreads)) {
	if (!_config.services.clockMs) {
		_config.services.clockMs = [] {
			return int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		};
	}
	_freeSlots = std::max(1, _config.maxConcurrentStarts);
	loadState();
	rebuildRoutes();
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
		const std::string &id,
		const std::string &version,
		const std::string &hash) const {
	const auto bytes = ReadFile(archivePath(id, version));
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
	rebuildRoutes();
}

void PluginManager::rebuildRoutes() {
	auto routes = std::map<std::string, std::map<std::string, Route>>();
	for (const auto &[id, record] : _records) {
		const auto parsed = record.enabled ? manifest(id) : nullptr;
		if (!parsed) {
			continue;
		}
		for (const auto &[key, grant] : record.grants) {
			if (!grant.granted.contains("plugins.message")) {
				continue;
			}
			auto entry = Route();
			for (const auto &sender : parsed->messagesFrom) {
				if (sender == "*") {
					entry.acceptsAny = true;
				} else {
					entry.from.insert(sender);
				}
			}
			routes[key][id] = std::move(entry);
		}
	}
	const auto lock = std::scoped_lock(_routesMutex);
	_routes = std::move(routes);
}

std::string PluginManager::route(
		const std::string &from,
		const std::string &account,
		const std::string &to,
		cbor::Value data) {
	{
		const auto lock = std::scoped_lock(_routesMutex);
		const auto plugins = _routes.find(kAllAccounts);
		if (plugins == _routes.end() || !plugins->second.contains(from)) {
			return api::error::kDenied;
		}
		const auto target = plugins->second.find(to);
		if (target == plugins->second.end()) {
			return api::error::kNotFound;
		} else if (!target->second.acceptsAny && !target->second.from.contains(from)) {
			return api::error::kDenied;
		}
	}
	{
		const auto lock = std::scoped_lock(_mailMutex);
		if (_mail.size() >= kMaxMail) {
			return api::error::kLimit;
		}
		_mail.push_back({ from, to, account, std::move(data) });
	}
	_outboxWake.notify_all();
	return std::string();
}

std::vector<std::string> PluginManager::peers(
		const std::string &from,
		const std::string &account) const {
	auto result = std::vector<std::string>();
	if (!ValidAccount(account)) {
		return result;
	}
	const auto lock = std::scoped_lock(_routesMutex);
	const auto plugins = _routes.find(kAllAccounts);
	if (plugins == _routes.end() || !plugins->second.contains(from)) {
		return result;
	}
	for (const auto &[id, entry] : plugins->second) {
		if (id != from && (entry.acceptsAny || entry.from.contains(from))) {
			result.push_back(id);
		}
	}
	return result;
}

bool PluginManager::enqueue(Instance &instance, Record &record, const cbor::Value &event) {
	const auto size = EncodedSize(event);
	{
		const auto lock = std::scoped_lock(instance.mutex);
		if (instance.closing || instance.finished) {
			return false;
		}
		while (!instance.queue.empty()
			&& (instance.queue.size() >= _config.maxQueuedEvents
				|| instance.queuedBytes + size > _config.maxQueuedBytes)) {
			instance.queuedBytes -= instance.queue.front().second;
			instance.queue.pop_front();
			++record.stats.dropped;
		}
		instance.queue.emplace_back(event, size);
		instance.queuedBytes += size;
	}
	instance.wake.notify_one();
	return true;
}

void PluginManager::deliverMail() {
	auto batch = std::deque<Mail>();
	{
		const auto lock = std::scoped_lock(_mailMutex);
		batch.swap(_mail);
	}
	for (auto &mail : batch) {
		const auto found = _records.find(mail.to);
		if (found == _records.end() || !mayRun()) {
			continue;
		}
		auto &record = found->second;
		auto *instance = ensure(record, mail.account, "message");
		if (!instance) {
			++record.stats.dropped;
			continue;
		}
		enqueue(*instance, record, cbor::Value::FromMap({
			{ "type", Text("plugin.message") },
			{ "from", Text(mail.from) },
			{ "data", std::move(mail.data) },
		}));
	}
}

std::string PluginManager::setTimer(
		const Key &key,
		const std::string &id,
		int64_t delayMs,
		bool repeat) {
	const auto lock = std::scoped_lock(_timersMutex);
	auto &timers = _timers[key];
	if (!timers.contains(id) && timers.size() >= kMaxTimers) {
		return api::error::kLimit;
	}
	timers[id] = { now() + delayMs, repeat ? delayMs : 0 };
	return {};
}

void PluginManager::clearTimers(const std::function<bool(const Key &key)> &matches) {
	const auto lock = std::scoped_lock(_timersMutex);
	std::erase_if(_timers, [&](const auto &entry) {
		return matches(entry.first);
	});
}

void PluginManager::fireTimers() {
	if (!mayRun()) {
		return;
	}
	const auto time = now();
	auto due = std::vector<std::pair<Key, std::string>>();
	{
		const auto lock = std::scoped_lock(_timersMutex);
		for (auto i = _timers.begin(); i != _timers.end();) {
			auto &timers = i->second;
			for (auto j = timers.begin(); j != timers.end();) {
				if (j->second.due > time) {
					++j;
					continue;
				}
				due.emplace_back(i->first, j->first);
				if (j->second.period) {
					j->second.due = time + j->second.period;
					++j;
				} else {
					j = timers.erase(j);
				}
			}
			i = timers.empty() ? _timers.erase(i) : std::next(i);
		}
	}
	for (const auto &[key, id] : due) {
		const auto found = _records.find(key.first);
		if (found == _records.end() || !_accounts.contains(key.second)) {
			continue;
		}
		auto &record = found->second;
		auto *instance = ensure(record, key.second, "timer");
		if (!instance) {
			++record.stats.dropped;
			continue;
		}
		enqueue(*instance, record, cbor::Value::FromMap({
			{ "type", Text("timer") },
			{ "id", Text(id) },
		}));
	}
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
			auto merged = std::optional<StoredGrant>();
			for (const auto &grant : (grants && grants->asMap()) ? *grants->asMap() : cbor::Map()) {
				if ((grant.key != kAllAccounts && !ValidAccount(grant.key))
					|| !grant.value.asMap()) {
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
				if (grant.key == kAllAccounts || !merged) {
					merged = std::move(stored);
					if (grant.key == kAllAccounts) {
						break;
					}
					continue;
				}
				std::erase_if(merged->granted, [&](const std::string &id) {
					return !stored.granted.contains(id);
				});
				if (stored.chats) {
					if (!merged->chats) {
						merged->chats = stored.chats;
					} else {
						std::erase_if(*merged->chats, [&](int64_t chat) {
							return !stored.chats->contains(chat);
						});
					}
				}
			}
			if (merged) {
				record.grants.emplace(kAllAccounts, std::move(*merged));
			}
		}
		_records.emplace(record.id, std::move(record));
	}
}

bool PluginManager::mayRun() const {
	return !_locked && !_hidden && !_closed;
}

namespace {

void AppendLog(LogRing &ring, size_t maxLines, std::string_view line, bool fromApp) {
	const auto time = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	const auto lock = std::scoped_lock(ring.mutex);
	ring.lines.push_back({ int64_t(time), std::string(line.substr(0, 500)), fromApp });
	while (ring.lines.size() > maxLines) {
		ring.lines.pop_front();
	}
}

[[nodiscard]] size_t EncodedSize(const cbor::Value &event) {
	auto bytes = cbor::Bytes();
	return cbor::Encode(event, bytes, 16) ? bytes.size() : 0;
}

struct Usage {
	uint64_t cpuMs = 0;
	uint64_t rssBytes = 0;
};

[[nodiscard]] Usage ProcessUsage(int pid) {
	if (pid <= 0) {
		return {};
	}
	auto stream = std::ifstream("/proc/" + std::to_string(pid) + "/stat");
	auto line = std::string();
	if (!std::getline(stream, line)) {
		return {};
	}
	const auto close = line.rfind(')');
	if (close == std::string::npos) {
		return {};
	}
	auto fields = std::istringstream(line.substr(close + 2));
	auto values = std::vector<std::string>();
	for (auto value = std::string(); fields >> value;) {
		values.push_back(value);
	}
	if (values.size() < 22) {
		return {};
	}
	const auto ticks = sysconf(_SC_CLK_TCK);
	const auto page = sysconf(_SC_PAGESIZE);
	const auto cpu = std::stoull(values[11]) + std::stoull(values[12]);
	return {
		ticks > 0 ? cpu * 1000 / uint64_t(ticks) : 0,
		std::stoull(values[21]) * uint64_t(page > 0 ? page : 4096),
	};
}

} // namespace

void PluginManager::addLog(Record &record, std::string_view line) {
	AppendLog(*record.log, _config.logLines, line, true);
}

std::vector<std::string> PluginManager::log(const std::string &id) const {
	const auto found = _records.find(id);
	if (found == _records.end()) {
		return {};
	}
	auto result = std::vector<std::string>();
	const auto lock = std::scoped_lock(found->second.log->mutex);
	for (const auto &line : found->second.log->lines) {
		result.push_back(line.text);
	}
	return result;
}

void PluginManager::clearLog(const std::string &id) {
	const auto found = _records.find(id);
	if (found != _records.end()) {
		const auto lock = std::scoped_lock(found->second.log->mutex);
		found->second.log->lines.clear();
	}
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
		_finishedCpuMs[key.first] += ProcessUsage(instance.pid.load()).cpuMs;
		{
			const auto lock = std::scoped_lock(instance.mutex);
			instance.closing = true;
			if (const auto record = _records.find(key.first); record != _records.end()) {
				record->second.stats.dropped += instance.queue.size();
			}
			instance.queue.clear();
			instance.queuedBytes = 0;
			for (const auto &pending : instance.outgoing) {
				const auto done = std::scoped_lock(pending->mutex);
				pending->finished = true;
				pending->done.notify_all();
			}
			instance.outgoing.clear();
		}
		instance.cancel.store(true);
		instance.broker->close();
		instance.wake.notify_all();
		if (const auto client = instance.live.load()) {
			client->abort();
		}
		if (_config.services.transport) {
			_config.services.transport->abort({ instance.id, instance.account });
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
	for (const auto &item : _records) {
		if (item.first == manifest.id) {
			continue;
		}
		const auto other = this->manifest(item.first);
		for (const auto &conflict : other ? other->conflicts : std::vector<Dependency>()) {
			if (conflict.id == manifest.id
				&& (!conflict.range || conflict.range->matches(manifest.version))) {
				return fail(InstallError::Conflict, { item.first });
			}
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
			existing->second.id,
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
	clearTimers([&](const Key &key) { return key.first == id; });
	if (found->second.store) {
		found->second.store->discard();
	}
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
		clearTimers([&](const Key &key) { return key.first == id; });
		saveState();
		return true;
	}
	const auto opened = openInstalled(record.id, record.current, record.archiveHash);
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
	record.lastFailure.clear();
	record.failures.clear();
	for (auto i = _notBefore.begin(); i != _notBefore.end();) {
		i = (i->first.first == id) ? _notBefore.erase(i) : std::next(i);
	}
	saveState();
	startEverywhere(record);
	return true;
}

bool PluginManager::start(const std::string &id) {
	const auto found = _records.find(id);
	if (found == _records.end() || !found->second.enabled || !mayRun()) {
		return false;
	}
	auto &record = found->second;
	std::erase_if(_notBefore, [&](const auto &entry) {
		return entry.first.first == id;
	});
	auto started = false;
	for (const auto &account : _accounts) {
		started = (ensure(record, account) != nullptr) || started;
	}
	return started;
}

void PluginManager::startEverywhere(Record &record) {
	for (const auto &account : _accounts) {
		(void)ensure(record, account, "startup");
	}
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
	const auto opened = openInstalled(record.id, record.current, record.archiveHash);
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
	record.grants.clear();
	record.grants[kAllAccounts] = std::move(stored);
	stopPlugin(id);
	saveState();
	startEverywhere(record);
	return true;
}

std::optional<GrantDecision> PluginManager::grants(
		const std::string &id,
		const std::string &account) const {
	const auto found = _records.find(id);
	if (found == _records.end() || !ValidAccount(account)) {
		return std::nullopt;
	}
	const auto grant = found->second.grants.find(kAllAccounts);
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
		if (const auto finished = _finishedCpuMs.find(record.id); finished != _finishedCpuMs.end()) {
			info.stats.cpuMs = finished->second;
		}
		for (const auto &instance : _instances) {
			if (instance.first.first == record.id) {
				const auto usage = ProcessUsage(instance.second->pid.load());
				info.stats.cpuMs += usage.cpuMs;
				info.stats.memoryBytes += usage.rssBytes;
				++info.stats.running;
			}
		}
		if (_config.services.transport) {
			const auto traffic = _config.services.transport->traffic(record.id);
			info.stats.netReceived = traffic.received;
			info.stats.netSent = traffic.sent;
			info.stats.openSockets = traffic.open;
		}
		if (const auto parsed = manifest(record.id)) {
			info.requests = parsed->permissions;
			info.issues = CheckCompatibility(*parsed, _config.environment);
			info.testedWith = parsed->testedWith
				? parsed->testedWith->text()
				: std::string();
			info.outdated = Outdated(*parsed, _config.environment);
		} else {
			const auto opened = openInstalled(record.id, record.current, record.archiveHash);
			info.problem = opened.ok()
				? std::string("the manifest cannot be read")
				: opened.error();
		}
		auto running = false;
		for (const auto &instance : _instances) {
			running = running || instance.first.first == record.id;
		}
		info.status = running
			? PluginStatus::Running
			: (!record.enabled ? PluginStatus::Disabled : PluginStatus::Stopped);
		info.onDemand = !activatedBy(record, "startup");
		info.lastFailure = record.lastFailure;
		for (const auto &[key, until] : _notBefore) {
			if (key.first == record.id) {
				info.retryInMs = std::max(info.retryInMs, until - now());
			}
		}
		result.push_back(std::move(info));
	}
	return result;
}

std::shared_ptr<const Manifest> PluginManager::manifest(const std::string &id) const {
	const auto found = _records.find(id);
	if (found == _records.end()) {
		return nullptr;
	}
	const auto &record = found->second;
	const auto key = record.current + "/" + record.archiveHash;
	if (const auto cached = _manifests.find(id);
		cached != _manifests.end() && cached->second.first == key) {
		return cached->second.second;
	}
	const auto opened = openInstalled(record.id, record.current, record.archiveHash);
	std::erase_if(_customIcons, [&](const auto &entry) {
		return entry.first.first == id;
	});
	if (!opened.ok()) {
		_manifests.erase(id);
		_icons.erase(id);
		return nullptr;
	}
	const auto &package = opened.value();
	auto result = std::make_shared<const Manifest>(package.manifest);
	_manifests[id] = { key, result };
	const auto icon = package.files.find(package.manifest.icon);
	if (icon != package.files.end()) {
		_icons[id] = std::make_shared<const cbor::Bytes>(icon->second);
	} else {
		_icons.erase(id);
	}
	for (const auto &[name, path] : package.manifest.icons) {
		const auto file = package.files.find(path);
		if (file != package.files.end() && file->second.size() <= kMaxIconBytes) {
			_customIcons[{ id, name }] = std::make_shared<const cbor::Bytes>(file->second);
		}
	}
	return result;
}

std::vector<Contribution> PluginManager::contributions(
		const std::string &account,
		const std::string &language) const {
	return snapshot().contributions(account, language);
}

ManagerSnapshot PluginManager::snapshot() const {
	auto result = ManagerSnapshot();
	result.loaded = true;
	result.plugins = list();
	result.environment = _config.environment;
	result.wantsEvents = wantsEvents();
	result.locked = _locked;
	result.hidden = _hidden;
	for (const auto &[id, record] : _records) {
		if (const auto parsed = manifest(id)) {
			result.manifests.emplace(id, parsed);
			if (const auto icon = _icons.find(id); icon != _icons.end()) {
				result.icons.emplace(id, icon->second);
			}
			for (const auto &[key, bytes] : _customIcons) {
				if (key.first == id) {
					result.customIcons.emplace(key, bytes);
				}
			}
		}
		if (const auto grant = record.grants.find(kAllAccounts); grant != record.grants.end()) {
			result.grants.emplace(id, GrantDecision{ grant->second.granted, grant->second.chats });
		}
		result.logs.emplace(id, record.log);
	}
	return result;
}

const PluginInfo *ManagerSnapshot::plugin(const std::string &id) const {
	const auto i = std::find_if(plugins.begin(), plugins.end(), [&](const PluginInfo &info) {
		return info.id == id;
	});
	return (i == plugins.end()) ? nullptr : &*i;
}

std::shared_ptr<const Manifest> ManagerSnapshot::manifest(const std::string &id) const {
	const auto i = manifests.find(id);
	return (i == manifests.end()) ? nullptr : i->second;
}

std::shared_ptr<const cbor::Bytes> ManagerSnapshot::customIcon(
		const std::string &id,
		const std::string &name) const {
	const auto i = customIcons.find({ id, name });
	return (i == customIcons.end()) ? nullptr : i->second;
}

std::shared_ptr<const cbor::Bytes> ManagerSnapshot::icon(const std::string &id) const {
	const auto i = icons.find(id);
	return (i == icons.end()) ? nullptr : i->second;
}

std::optional<GrantDecision> ManagerSnapshot::grantsOf(const std::string &id) const {
	const auto i = grants.find(id);
	return (i == grants.end()) ? std::nullopt : std::make_optional(i->second);
}

std::vector<std::string> ManagerSnapshot::log(const std::string &id) const {
	auto result = std::vector<std::string>();
	for (auto &line : logLines(id)) {
		result.push_back(std::move(line.text));
	}
	return result;
}

std::vector<LogLine> ManagerSnapshot::logLines(const std::string &id) const {
	const auto i = logs.find(id);
	if (i == logs.end() || !i->second) {
		return {};
	}
	const auto lock = std::scoped_lock(i->second->mutex);
	return { i->second->lines.begin(), i->second->lines.end() };
}

std::vector<Contribution> ManagerSnapshot::contributions(
		const std::string &account,
		const std::string &language) const {
	auto result = std::vector<Contribution>();
	if (hidden || locked || !ValidAccount(account)) {
		return result;
	}
	for (const auto &info : plugins) {
		const auto grant = grants.find(info.id);
		if (!info.enabled || grant == grants.end()) {
			continue;
		}
		const auto parsed = manifest(info.id);
		if (!parsed || !CheckCompatibility(*parsed, environment).empty()) {
			continue;
		}
		const auto has = [&](const char *name) {
			return grant->second.granted.contains(name);
		};
		auto entry = Contribution();
		entry.id = info.id;
		entry.name = parsed->name.get(language);
		if (has("ui.menu")) {
			for (const auto &menu : parsed->menus) {
				if (menu.surface == "composer" && !has("ui.composer_button")) {
					continue;
				}
				entry.menus.push_back({ menu.surface, menu.id, menu.title.get(language), menu.icon });
			}
		}
		if (has("ui.command")) {
			for (const auto &command : parsed->commands) {
				entry.commands.push_back({ command.name, command.description.get(language) });
			}
		}
		entry.settingsPage = parsed->settingsPage && has("ui.settings_page");
		if (!entry.menus.empty() || !entry.commands.empty() || entry.settingsPage) {
			result.push_back(std::move(entry));
		}
	}
	return result;
}

bool ManagerSnapshot::transformsOutgoing(int64_t chat, bool secret) const {
	auto info = OutgoingInfo();
	info.chat = chat;
	info.secret = secret;
	return transformsOutgoing(info);
}

bool ManagerSnapshot::transformsOutgoing(const OutgoingInfo &info) const {
	const auto chat = info.chat;
	if (hidden || locked || info.secret || chat == api::kServiceChat) {
		return false;
	}
	for (const auto &plugin : plugins) {
		const auto grant = grants.find(plugin.id);
		const auto parsed = manifest(plugin.id);
		if (plugin.enabled
			&& grant != grants.end()
			&& grant->second.granted.contains("messages.transform_outgoing")
			&& parsed
			&& parsed->outgoing.declared
			&& Matches(parsed->outgoing.filter, info)
			&& (!grant->second.chats || grant->second.chats->contains(chat))) {
			return true;
		}
	}
	return false;
}

bool PluginManager::send(
		const std::string &id,
		const std::string &account,
		const cbor::Value &event) {
	if (!ValidAccount(account) || !mayRun() || !event.asMap()) {
		return false;
	}
	const auto found = _records.find(id);
	if (found == _records.end()) {
		return false;
	}
	auto &record = found->second;
	const auto grant = record.grants.find(kAllAccounts);
	const auto typeField = event.find("type");
	const auto type = typeField ? typeField->asText() : nullptr;
	if (!record.enabled || grant == record.grants.end() || !type) {
		return false;
	}
	const auto parsed = manifest(id);
	if (!parsed) {
		return false;
	}
	const auto has = [&](const char *name) {
		return grant->second.granted.contains(name);
	};
	const auto text = [&](const char *name) -> std::string {
		const auto field = event.find(name);
		return field && field->asText() ? *field->asText() : std::string();
	};
	if (*type == "menu.click") {
		const auto surface = text("surface");
		const auto declared = std::any_of(parsed->menus.begin(), parsed->menus.end(), [&](const auto &menu) {
			return menu.surface == surface && menu.id == text("id");
		});
		if (!has("ui.menu")
			|| !declared
			|| (surface == "composer" && !has("ui.composer_button"))) {
			return false;
		}
	} else if (*type == "command") {
		const auto declared = std::any_of(parsed->commands.begin(), parsed->commands.end(), [&](const auto &command) {
			return command.name == text("name");
		});
		if (!has("ui.command") || !declared) {
			return false;
		}
	} else if (*type == "settings.change"
		|| *type == "settings.click"
		|| *type == "settings.open") {
		if (!has("ui.settings_page") || !parsed->settingsPage) {
			return false;
		}
	} else if (*type == "dialog.click"
		|| *type == "dialog.change"
		|| *type == "dialog.close"
		|| *type == "window.click"
		|| *type == "window.change"
		|| *type == "window.close") {
		if (!has("ui.dialog")) {
			return false;
		}
	} else {
		return false;
	}
	pump();
	const auto trigger = (*type == "menu.click")
		? "menu"
		: (*type == "command")
		? "command"
		: (type->rfind("settings.", 0) == 0)
		? "settings"
		: nullptr;
	auto *instance = trigger
		? ensure(record, account, trigger)
		: running(id, account);
	if (!instance) {
		++record.stats.dropped;
		return false;
	}
	instance->broker->noteUserAction(*type, text("window"));
	const auto filtered = instance->broker->filterTargeted(event);
	const auto size = EncodedSize(filtered);
	{
		const auto lock = std::scoped_lock(instance->mutex);
		if (instance->closing || instance->finished) {
			return false;
		}
		while (!instance->queue.empty()
			&& (instance->queue.size() >= _config.maxQueuedEvents
				|| instance->queuedBytes + size > _config.maxQueuedBytes)) {
			instance->queuedBytes -= instance->queue.front().second;
			instance->queue.pop_front();
			++record.stats.dropped;
		}
		instance->queue.emplace_back(filtered, size);
		instance->queuedBytes += size;
	}
	instance->wake.notify_one();
	return true;
}

bool PluginManager::pythonAvailable() const {
	const auto &python = _config.python;
	auto error = std::error_code();
	return !python.wasm.empty()
		&& fs::exists(python.wasm, error)
		&& fs::exists(python.library, error)
		&& fs::exists(python.sdk, error);
}

std::string PluginManager::fillPython(
		LoadRequest &request,
		const std::atomic<bool> *cancel) {
	// Only Python requests carry arguments, a wasm module is already in place.
	if (request.args.empty()) {
		return {};
	}
	const auto &python = _config.python;
	const auto failed = [&] {
		return (cancel && cancel->load())
			? std::string("cancelled")
			: std::string("the Python runtime is not installed");
	};
	request.wasm = ReadFile(python.wasm);
	if (request.wasm.empty()
		|| (!python.expectedHash.empty()
			&& HashText(request.wasm) != python.expectedHash)) {
		request.wasm.clear();
		return failed();
	}
	const auto before = request.files.size();
	auto error = std::error_code();
	for (const auto &entry : fs::recursive_directory_iterator(python.library, error)) {
		if (cancel && cancel->load()) {
			return failed();
		} else if (entry.is_regular_file()) {
			request.files.emplace_back(
				"runtime/lib/" + fs::relative(entry.path(), python.library).generic_string(),
				ReadFile(entry.path()));
		}
	}
	for (const auto &entry : fs::recursive_directory_iterator(python.sdk, error)) {
		if (entry.is_regular_file() && entry.path().extension() == ".py") {
			request.files.emplace_back(
				"runtime/sdk/" + fs::relative(entry.path(), python.sdk).generic_string(),
				ReadFile(entry.path()));
		}
	}
	if ((cancel && cancel->load()) || request.files.size() == before) {
		return failed();
	}
	return {};
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
	if (!pythonAvailable()) {
		return std::string("the Python runtime is not installed");
	}
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

Result<LoadRequest, std::string> PluginManager::loadRequest(
		const std::string &id,
		const StartPlan &plan) {
	const auto opened = openInstalled(id, plan.version, plan.archiveHash);
	if (!opened.ok()) {
		return "the plugin files are not usable: " + opened.error();
	}
	return buildRequest(opened.value());
}

Result<std::shared_ptr<PluginManager::Instance>, std::string> PluginManager::prepare(
		Record &record,
		const std::string &account) {
	const auto parsed = manifest(record.id);
	if (!parsed) {
		const auto opened = openInstalled(record.id, record.current, record.archiveHash);
		return "the plugin files are not usable: "
			+ (opened.ok() ? std::string("the manifest changed") : opened.error());
	}
	const auto &manifest = *parsed;
	if (!CheckCompatibility(manifest, _config.environment).empty()) {
		return std::string("the plugin is not compatible with this version");
	}
	const auto grant = record.grants.find(kAllAccounts);
	if (grant == record.grants.end()) {
		return std::string("no permissions were granted");
	}

	auto instance = std::make_shared<Instance>();
	instance->id = record.id;
	instance->account = account;
	instance->generation = _nextGeneration++;
	instance->storage = storeOf(record);
	instance->serial = record.serial;
	auto services = BrokerServices();
	services.storage = instance->storage.get();
	services.keepAlive = instance->storage;
	services.data = _config.services.dataFor
		? _config.services.dataFor(account)
		: _config.services.data;
	services.transport = _config.services.transport;
	services.clockMs = _config.services.clockMs;
	const auto ring = record.log;
	const auto maxLines = _config.logLines;
	services.log = [ring, maxLines](std::string_view line) {
		AppendLog(*ring, maxLines, line, false);
	};
	instance->appLog = [ring, maxLines](std::string_view line) {
		AppendLog(*ring, maxLines, line, true);
	};
	for (const auto &menu : manifest.menus) {
		instance->declared.menus.insert(menu.surface + "/" + menu.id);
	}
	for (const auto &command : manifest.commands) {
		instance->declared.commands.insert(command.name);
	}
	instance->declared.settingsPage = manifest.settingsPage;
	const auto settings = manifest.settingsPage
		? _config.services.settingsPage
		: nullptr;
	const auto dialog = _config.services.dialog;
	const auto window = _config.services.window;
	const auto pluginId = record.id;
	services.ui = [settings, dialog, window, pluginId, account](std::string_view kind, const cbor::Value &payload) {
		if (kind == "settings" && settings) {
			settings(pluginId, account, payload);
			return true;
		} else if (kind == "dialog" && dialog) {
			dialog(pluginId, account, &payload);
			return true;
		} else if (kind == "dialog.close" && dialog) {
			dialog(pluginId, account, nullptr);
			return true;
		} else if (kind == "window" && window) {
			const auto id = payload.find("id");
			const auto mode = payload.find("mode");
			const auto page = payload.find("page");
			if (!id || !id->asText() || !mode || !mode->asText() || !page) {
				return false;
			}
			window(pluginId, account, *id->asText(), *mode->asText(), page);
			return true;
		} else if (kind == "window.close" && window) {
			const auto id = payload.find("id");
			if (!id || !id->asText()) {
				return false;
			}
			window(pluginId, account, *id->asText(), std::string(), nullptr);
			return true;
		}
		return false;
	};
	const auto raw = instance.get();
	services.unload = [raw] {
		raw->unload.store(true);
	};
	services.sendToPlugin = [this, pluginId, account](const std::string &to, cbor::Value data) {
		return route(pluginId, account, to, std::move(data));
	};
	services.pluginPeers = [this, pluginId, account] {
		return peers(pluginId, account);
	};
	const auto key = Key{ pluginId, account };
	services.setTimer = [this, key](const std::string &id, int64_t delayMs, bool repeat) {
		return setTimer(key, id, delayMs, repeat);
	};
	services.clearTimer = [this, key](const std::string &id) {
		const auto lock = std::scoped_lock(_timersMutex);
		const auto i = _timers.find(key);
		return (i != _timers.end()) && i->second.erase(id) > 0;
	};
	services.timers = [this, key] {
		const auto lock = std::scoped_lock(_timersMutex);
		auto result = std::vector<std::string>();
		if (const auto i = _timers.find(key); i != _timers.end()) {
			for (const auto &[id, timer] : i->second) {
				result.push_back(id);
			}
		}
		return result;
	};
	services.post = [this](std::function<void()> work) {
		_pool->post(std::move(work));
	};
	services.after = [this](int64_t delayMs, std::function<void()> work) {
		_pool->postAfter(delayMs, std::move(work));
	};
	services.complete = [weak = std::weak_ptr<Instance>(instance)](
			int64_t ticket,
			cbor::Value result) {
		const auto strong = weak.lock();
		if (!strong) {
			return;
		}
		if (EncodedSize(result) + kResultOverhead > RuntimeLimits().maxMessageBytes) {
			result = api::Fail(api::error::kLimit, "the answer is too large");
		}
		auto event = cbor::Map{
			{ "type", Text("call.result") },
			{ "ticket", cbor::Value::FromInt(ticket) },
		};
		for (const auto &entry : result.asMap() ? *result.asMap() : cbor::Map()) {
			event.push_back(entry);
		}
		{
			const auto lock = std::scoped_lock(strong->mutex);
			if (strong->closing || strong->finished) {
				return;
			}
			strong->results.push_back(cbor::Value::FromMap(std::move(event)));
		}
		strong->wake.notify_one();
	};
	auto grants = MakeGrants(manifest, { grant->second.granted, grant->second.chats });
	grants.account = account;
	instance->broker = std::make_shared<Broker>(std::move(grants), std::move(services));

	instance->plan.version = record.current;
	instance->plan.archiveHash = record.archiveHash;
	instance->plan.host = _config.host;
	if (manifest.kind == "python") {
		instance->plan.host.addressSpaceMb = std::max(
			instance->plan.host.addressSpaceMb,
			2048);
	}
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
	struct Release {
		Broker *broker = nullptr;
		~Release() {
			broker->releaseSockets();
		}
	} release{ instance.broker.get() };
	const auto unloaded = [&] {
		if (!instance.unload.load()) {
			return false;
		}
		post({
			instance.id,
			instance.account,
			instance.generation,
			Completion::Kind::Unloaded,
			{},
		});
		const auto lock = std::scoped_lock(instance.mutex);
		instance.finished = true;
		return true;
	};
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
		auto request = loadRequest(instance.id, instance.plan);
		if (!request.ok()) {
			fail(request.error());
			return;
		}
		instance.plan.request = std::move(request.value());
	}
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
		instance.pid.store(instance.client->pid());
		if (instance.cancel.load()) {
			problem = "cancelled";
		} else if (auto filled = fillPython(instance.plan.request, &instance.cancel);
				!filled.empty()) {
			problem = std::move(filled);
		} else {
			const auto loaded = LoadWithCache(
				*instance.client,
				instance.plan.host,
				std::move(instance.plan.request),
				instance.plan.cache,
				&instance.cancel);
			if (!loaded.ok()) {
				problem = "the module did not load: " + loaded.error().detail;
			} else {
				const auto serial = std::scoped_lock(*instance.serial);
				const auto init = instance.client->init(
					instance.plan.init,
					*instance.broker);
				if (!init.ok()) {
					problem = "the plugin failed to start: " + init.error().detail;
				} else if (init.value() != 0 && instance.appLog) {
					instance.appLog("failure: the start handler returned "
						+ std::to_string(init.value()));
				}
			}
		}
	}
	releaseSlot();
	instance.plan.request = LoadRequest();
#ifdef __GLIBC__
	// The request buffers are gone by now, give their heap back.
	malloc_trim(0);
#endif // __GLIBC__
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
	if (unloaded()) {
		return;
	}

	while (true) {
		auto event = cbor::Value();
		auto outgoing = std::shared_ptr<Outgoing>();
		{
			auto lock = std::unique_lock(instance.mutex);
			instance.wake.wait(lock, [&] {
				return instance.closing
					|| !instance.queue.empty()
					|| !instance.outgoing.empty()
					|| !instance.results.empty();
			});
			if (instance.closing) {
				return;
			}
			if (!instance.outgoing.empty()) {
				outgoing = std::move(instance.outgoing.front());
				instance.outgoing.pop_front();
				event = outgoing->event;
			} else if (!instance.results.empty()) {
				event = std::move(instance.results.front());
				instance.results.pop_front();
			} else {
				event = std::move(instance.queue.front().first);
				instance.queuedBytes -= instance.queue.front().second;
				instance.queue.pop_front();
			}
			instance.busy = true;
		}
		auto serial = std::unique_lock(*instance.serial);
		if (outgoing) {
			instance.broker->beginOutgoing();
		}
		const auto result = instance.client->deliver(event, *instance.broker);
		serial.unlock();
		if (outgoing) {
			auto edit = instance.broker->finishOutgoing();
			const auto lock = std::scoped_lock(outgoing->mutex);
			outgoing->ok = result.ok() && result.value() == 0;
			outgoing->edit = std::move(edit);
			outgoing->finished = true;
			outgoing->done.notify_all();
		}
		if (outgoing) {
			post({
				instance.id,
				instance.account,
				instance.generation,
				Completion::Kind::Outgoing,
				{},
			});
		}
		instance.storage->flush();
		if (result.ok()) {
			post({
				instance.id,
				instance.account,
				instance.generation,
				Completion::Kind::Delivered,
				{},
			});
			{
				const auto lock = std::scoped_lock(instance.mutex);
				instance.busy = false;
			}
			if (unloaded()) {
				return;
			}
			continue;
		}
		fail(result.error().detail.empty()
			? std::string("the plugin stopped answering")
			: result.error().detail);
		return;
	}
}

SharedStorage::SharedStorage(fs::path file)
: _storage(std::make_unique<FileStorage>(std::move(file))) {
}

std::optional<cbor::Bytes> SharedStorage::get(const std::string &key) {
	const auto lock = std::scoped_lock(_mutex);
	return _storage ? _storage->get(key) : std::nullopt;
}

void SharedStorage::set(const std::string &key, cbor::Bytes value) {
	const auto lock = std::scoped_lock(_mutex);
	if (_storage) {
		_storage->set(key, std::move(value));
	}
}

bool SharedStorage::remove(const std::string &key) {
	const auto lock = std::scoped_lock(_mutex);
	return _storage && _storage->remove(key);
}

std::vector<std::string> SharedStorage::keys(const std::string &prefix, size_t limit) {
	const auto lock = std::scoped_lock(_mutex);
	return _storage ? _storage->keys(prefix, limit) : std::vector<std::string>();
}

uint64_t SharedStorage::usedBytes() {
	const auto lock = std::scoped_lock(_mutex);
	return _storage ? _storage->usedBytes() : 0;
}

size_t SharedStorage::count() {
	const auto lock = std::scoped_lock(_mutex);
	return _storage ? _storage->count() : 0;
}

bool SharedStorage::flush() {
	const auto lock = std::scoped_lock(_mutex);
	return !_storage || _storage->flush();
}

void SharedStorage::discard() {
	const auto lock = std::scoped_lock(_mutex);
	_storage = nullptr;
}

std::shared_ptr<SharedStorage> PluginManager::storeOf(Record &record) {
	if (record.store) {
		return record.store;
	}
	const auto data = _config.root / record.id / "data";
	const auto file = data / "store.cbor";
	auto error = std::error_code();
	if (!fs::exists(file, error)) {
		auto newest = fs::path();
		auto newestTime = fs::file_time_type::min();
		for (const auto &entry : fs::directory_iterator(data, error)) {
			const auto old = entry.path() / "store.cbor";
			if (entry.is_directory(error) && fs::exists(old, error)) {
				const auto time = fs::last_write_time(old, error);
				if (!error && (newest.empty() || time > newestTime)) {
					newest = old;
					newestTime = time;
				}
			}
		}
		if (!newest.empty()) {
			fs::copy_file(newest, file, error);
		}
	}
	record.store = std::make_shared<SharedStorage>(file);
	return record.store;
}

bool PluginManager::wantsEvents() const {
	if (!mayRun()) {
		return false;
	}
	for (const auto &[id, record] : _records) {
		if (!record.enabled || record.grants.empty()) {
			continue;
		}
		const auto parsed = manifest(id);
		if (!parsed || parsed->events.empty()) {
			continue;
		}
		const auto active = std::any_of(_instances.begin(), _instances.end(), [&](const auto &entry) {
			return entry.first.first == id;
		});
		if (active || parsed->activation.contains("event")) {
			return true;
		}
	}
	return false;
}

bool PluginManager::activatedBy(const Record &record, const char *trigger) const {
	const auto parsed = manifest(record.id);
	if (!parsed) {
		return false;
	} else if (std::string_view(trigger) == "startup") {
		const auto transforms = parsed->outgoing.declared
			&& std::any_of(record.grants.begin(), record.grants.end(), [](const auto &entry) {
				return entry.second.granted.contains("messages.transform_outgoing");
			});
		const auto background = parsed->background
			&& std::any_of(record.grants.begin(), record.grants.end(), [](const auto &entry) {
				return entry.second.granted.contains("background");
			});
		return parsed->activation.contains("startup") || background || transforms;
	}
	return parsed->activation.contains(trigger);
}

PluginManager::Instance *PluginManager::running(
		const std::string &id,
		const std::string &account) {
	const auto i = _instances.find({ id, account });
	return (i == _instances.end()) ? nullptr : i->second.get();
}

PluginManager::Instance *PluginManager::ensure(
		Record &record,
		const std::string &account,
		const char *trigger) {
	const auto key = Key{ record.id, account };
	if (const auto i = _instances.find(key); i != _instances.end()) {
		return i->second.get();
	}
	if (trigger && !activatedBy(record, trigger)) {
		return nullptr;
	} else if (!mayRun() || !record.enabled || !record.grants.contains(kAllAccounts)) {
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
	record.lastFailure = why;
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
	deliverMail();
	fireTimers();
	auto batch = std::deque<Completion>();
	{
		const auto lock = std::scoped_lock(_outboxMutex);
		batch.swap(_outbox);
	}
	auto handled = 0;
	for (auto &completion : batch) {
		if (completion.kind == Completion::Kind::Outgoing) {
			continue;
		}
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
			record->second.lastFailure.clear();
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
		case Completion::Kind::Unloaded:
			++record->second.stats.unloads;
			_streak.erase(key);
			addLog(record->second, "unloaded itself for " + completion.account);
			stop(key);
			break;
		case Completion::Kind::Outgoing:
			break;
		}
	}
	advanceOutgoing();
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
				|| (instance.ready
					&& instance.queue.empty()
					&& instance.results.empty()
					&& !instance.busy
					&& !instance.broker->pending()));
		}
		{
			const auto lock = std::scoped_lock(_outboxMutex);
			idle = idle && _outbox.empty();
		}
		{
			const auto lock = std::scoped_lock(_mailMutex);
			idle = idle && _mail.empty();
		}
		idle = idle && _chains.empty();
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

void PluginManager::waitForWork(int timeoutMs, const std::function<bool()> &ready) {
	auto lock = std::unique_lock(_outboxMutex);
	_outboxWake.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
		if (!_outbox.empty() || ready()) {
			return true;
		}
		const auto mail = std::scoped_lock(_mailMutex);
		return !_mail.empty();
	});
}

void PluginManager::wake() {
	{
		const auto lock = std::scoped_lock(_outboxMutex);
	}
	_outboxWake.notify_all();
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
		if (!record.enabled || !record.grants.contains(kAllAccounts)) {
			continue;
		}
		const auto parsed = manifest(id);
		const auto typeField = event.find("type");
		const auto type = typeField ? typeField->asText() : nullptr;
		if (!parsed || !type
			|| std::find(parsed->events.begin(), parsed->events.end(), *type)
				== parsed->events.end()) {
			++report.skipped;
			continue;
		}
		const auto grant = record.grants.find(kAllAccounts);
		if (!FilterEvent(MakeGrants(*parsed, { grant->second.granted, grant->second.chats }), event)) {
			++report.skipped;
			continue;
		}
		const auto before = _instances.size();
		auto *instance = ensure(record, account, "event");
		if (!instance) {
			if (activatedBy(record, "event")) {
				++record.stats.dropped;
			}
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
	if (!ValidAccount(account)) {
		return report;
	}
	_accounts.insert(account);
	if (!mayRun()) {
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
		if (ensure(found->second, account, "startup")) {
			report.started += int(_instances.size() > before);
		}
	}
	return report;
}

OutgoingResult PluginManager::transformOutgoing(
		const std::string &account,
		int64_t chat,
		bool secret,
		const std::string &text,
		int timeoutMs) {
	auto result = std::optional<OutgoingResult>();
	transformOutgoingAsync(account, chat, secret, text, timeoutMs, [&](OutgoingResult value) {
		result = std::move(value);
	});
	while (!result) {
		pump();
		if (result) {
			break;
		}
		auto lock = std::unique_lock(_outboxMutex);
		_outboxWake.wait_for(lock, std::chrono::milliseconds(20), [&] {
			return !_outbox.empty();
		});
	}
	return std::move(*result);
}

void PluginManager::transformOutgoingAsync(
		const std::string &account,
		int64_t chat,
		bool secret,
		const std::string &text,
		int timeoutMs,
		std::function<void(OutgoingResult)> done) {
	auto info = OutgoingInfo();
	info.chat = chat;
	info.secret = secret;
	info.text = text;
	transformOutgoingAsync(account, std::move(info), timeoutMs, std::move(done));
}

void PluginManager::transformOutgoingAsync(
		const std::string &account,
		OutgoingInfo info,
		int timeoutMs,
		std::function<void(OutgoingResult)> done) {
	auto chain = std::make_shared<OutgoingChain>();
	chain->account = account;
	chain->timeoutMs = timeoutMs;
	chain->result.text = info.text;
	const auto allowed = !info.secret && info.chat != api::kServiceChat;
	chain->info = std::move(info);
	chain->done = std::move(done);
	if (ValidAccount(account) && mayRun() && allowed) {
		for (const auto &item : _records) {
			chain->plugins.push_back(item.first);
		}
	}
	_chains.push_back(std::move(chain));
	advanceOutgoing();
}

bool PluginManager::advance(
		OutgoingChain &chain,
		std::chrono::steady_clock::time_point now) {
	const auto refuse = [&](const std::string &plugin, std::string reason) {
		chain.result.blocked = true;
		chain.result.plugin = plugin;
		chain.result.reason = std::move(reason);
		return true;
	};
	while (true) {
		if (const auto pending = chain.current) {
			auto finished = false;
			auto ok = false;
			auto edit = OutgoingEdit();
			{
				const auto lock = std::scoped_lock(pending->mutex);
				finished = pending->finished;
				ok = pending->ok;
				edit = pending->edit;
			}
			if (!finished && now < chain.deadline) {
				return false;
			}
			chain.current = nullptr;
			if (finished && ok) {
				auto &result = chain.result;
				auto &info = chain.info;
				if (edit.drop) {
					result.dropped = true;
					result.plugin = chain.plugins[chain.next - 1];
					result.reason = std::move(edit.dropReason);
					return true;
				}
				if (edit.text) {
					result.text = *edit.text;
					result.markdown = edit.markdown;
					info.text = *edit.text;
				}
				if (edit.silent) {
					result.silent = info.silent = *edit.silent;
				}
				if (edit.schedule) {
					result.schedule = info.scheduled = *edit.schedule;
				}
				if (edit.replyTo) {
					result.replyTo = info.replyTo = *edit.replyTo;
				}
				if (edit.topic) {
					result.topic = info.topic = *edit.topic;
				}
			} else if (chain.blocking) {
				return refuse(
					chain.plugins[chain.next - 1],
					finished ? "the plugin failed" : "the plugin did not answer in time");
			}
			continue;
		}
		if (chain.next >= chain.plugins.size() || !mayRun()) {
			return true;
		}
		const auto &id = chain.plugins[chain.next++];
		const auto found = _records.find(id);
		if (found == _records.end()) {
			continue;
		}
		auto &record = found->second;
		const auto grant = record.grants.find(kAllAccounts);
		const auto parsed = manifest(record.id);
		if (!record.enabled
			|| grant == record.grants.end()
			|| !grant->second.granted.contains("messages.transform_outgoing")
			|| !parsed
			|| !parsed->outgoing.declared
			|| !Matches(parsed->outgoing.filter, chain.info)
			|| (grant->second.chats && !grant->second.chats->contains(chain.info.chat))) {
			continue;
		}
		const auto blocking = parsed->outgoing.block;
		auto *instance = ensure(record, chain.account, nullptr);
		auto ready = false;
		if (instance) {
			const auto lock = std::scoped_lock(instance->mutex);
			ready = instance->ready && !instance->closing && !instance->finished;
		}
		if (!ready) {
			if (blocking) {
				return refuse(record.id, "the plugin is not ready yet");
			}
			continue;
		}
		auto pending = std::make_shared<Outgoing>();
		const auto &info = chain.info;
		auto event = cbor::Map{
			{ "type", Text("message.outgoing") },
			{ "chat", cbor::Value::FromInt(info.chat) },
			{ "text", Text(chain.result.text) },
			{ "chat_type", Text(info.chatType) },
			{ "silent", cbor::Value::FromBool(info.silent) },
		};
		const auto optional = [&](const char *key, int64_t value) {
			if (value) {
				event.push_back({ key, cbor::Value::FromInt(value) });
			}
		};
		optional("topic", info.topic);
		optional("reply_to", info.replyTo);
		optional("scheduled", info.scheduled);
		pending->event = cbor::Value::FromMap(std::move(event));
		{
			const auto lock = std::scoped_lock(instance->mutex);
			instance->outgoing.push_back(pending);
		}
		instance->wake.notify_one();
		chain.current = std::move(pending);
		chain.blocking = blocking;
		chain.deadline = now + std::chrono::milliseconds(chain.timeoutMs);
		return false;
	}
}

void PluginManager::advanceOutgoing() {
	const auto now = std::chrono::steady_clock::now();
	auto finished = std::vector<std::shared_ptr<OutgoingChain>>();
	for (const auto &chain : _chains) {
		if (advance(*chain, now)) {
			finished.push_back(chain);
		}
	}
	if (finished.empty()) {
		return;
	}
	std::erase_if(_chains, [&](const std::shared_ptr<OutgoingChain> &chain) {
		return std::find(finished.begin(), finished.end(), chain) != finished.end();
	});
	for (const auto &chain : finished) {
		chain->done(std::move(chain->result));
	}
}

void PluginManager::onLock() {
	_locked = true;
	stopAll();
}

void PluginManager::onUnlock() {
	_locked = false;
	for (const auto &account : std::set<std::string>(_accounts)) {
		startEnabled(account);
	}
}

void PluginManager::setHidden(bool hidden) {
	_hidden = hidden;
	if (hidden) {
		stopAll();
	}
}

void PluginManager::onLogout(const std::string &account) {
	_accounts.erase(account);
	clearTimers([&](const Key &key) { return key.second == account; });
	auto keys = std::vector<Key>();
	for (const auto &item : _instances) {
		if (item.first.second == account) {
			keys.push_back(item.first);
		}
	}
	stopMany(keys);
}

void PluginManager::shutdown() {
	if (std::exchange(_closed, true)) {
		return;
	}
	stopAll();
	advanceOutgoing();
	_pool->stop();
	if (!_records.empty()) {
		saveState();
	}
}

} // namespace fgpl
