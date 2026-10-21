#include "check.h"
#include "fakes.h"
#include "manager.h"
#include "package_builder.h"
#include "python_support.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

using namespace fgpl;
using namespace pkg;
namespace fs = std::filesystem;

[[nodiscard]] cbor::Bytes ReadFile(const fs::path &path) {
	auto stream = std::ifstream(path, std::ios::binary);
	return { std::istreambuf_iterator<char>(stream), {} };
}

[[nodiscard]] cbor::Value Event(int64_t chat = 1) {
	return cbor::Value::FromMap({
		{ "type", Text("message.new") },
		{ "chat", Int(chat) },
		{ "text", Text("hi") },
	});
}

[[nodiscard]] cbor::Value PermissionList(std::vector<std::pair<std::string, std::vector<std::string>>> items) {
	auto list = cbor::Array();
	for (const auto &item : items) {
		auto entry = cbor::Map();
		entry.push_back({ "id", Text(item.first) });
		entry.push_back({ "reason", Text("Needed for tests.") });
		if (!item.second.empty()) {
			entry.push_back({ "hosts", Array(item.second) });
		}
		list.push_back(cbor::Value::FromMap(std::move(entry)));
	}
	return cbor::Value::FromArray(std::move(list));
}

[[nodiscard]] cbor::Value Document(
		const std::string &id,
		const std::string &version,
		std::vector<std::pair<std::string, std::vector<std::string>>> permissions = {
			{ "messages.read", {} },
			{ "storage.private", {} },
		}) {
	auto document = ValidDocument();
	Set(document, "id", Text(id));
	Set(document, "version", Text(version));
	Remove(document, "permissions");
	Set(document, "permissions.required", PermissionList(std::move(permissions)));
	return document;
}

[[nodiscard]] cbor::Bytes CounterArchive(
		const std::string &id,
		const std::string &version,
		uint8_t seed = 1,
		std::vector<std::pair<std::string, std::vector<std::string>>> permissions = {
			{ "messages.read", {} },
			{ "storage.private", {} },
		}) {
#ifdef FGPL_COUNTER_WASM
	const auto key = Seed(seed);
	return Build(
		Document(id, version, std::move(permissions)),
		{ { "plugin.wasm", ReadFile(FGPL_COUNTER_WASM) } },
		&key).archive;
#else
	return {};
#endif
}

[[nodiscard]] cbor::Bytes WatArchive(
		const std::string &id,
		const std::string &version,
		const std::string &wat,
		uint8_t seed = 1) {
	const auto key = Seed(seed);
	return Build(
		Document(id, version),
		{ { "plugin.wasm", wat::Wasm(wat) } },
		&key).archive;
}

[[nodiscard]] std::string CrashOnEvent() {
	return wat::Module("", "(unreachable)");
}

[[nodiscard]] std::string CrashOnInit() {
	auto text = wat::Module("", "(i32.const 0)");
	const auto from = text.find("(func (export \"fg_init\") (param i32 i32) (result i32) (i32.const 0))");
	text.replace(from, std::string("(func (export \"fg_init\") (param i32 i32) (result i32) (i32.const 0))").size(),
		"(func (export \"fg_init\") (param i32 i32) (result i32) (unreachable))");
	return text;
}

struct Lab {
	explicit Lab(const char *name) {
		root = fs::temp_directory_path() / name;
		fs::remove_all(root);
		fs::create_directories(root);
		environment.app = *ParseVersion("7.6.1");
		environment.apiLevel = 1;
		environment.platform = "linux";
		environment.platformVersion = *ParseVersion("6.1.0");
		services.data = &data;
		services.clockMs = [this] { return time; };
		cacheSettings.directory = root / "cache";
		cacheSettings.secret.fill(0x33);
		key.fill(0x77);
	}

	~Lab() {
		fs::remove_all(root);
	}

	[[nodiscard]] ManagerConfig config() const {
		auto result = ManagerConfig();
		result.root = root / "plugins";
		result.host.path = FGPL_HOST_BINARY;
		result.host.addressSpaceMb = 1024;
		result.python.wasm = fs::path(FGPL_CPYTHON_DIR) / "python.wasm";
		result.python.library = fs::path(FGPL_CPYTHON_DIR) / "lib";
		result.python.sdk = FGPL_PYTHON_SDK_DIR;
		result.cache = cacheSettings;
		result.environment = environment;
		result.services = services;
		result.stateKey = key;
		return result;
	}

	[[nodiscard]] std::optional<cbor::Value> stored(
			const std::string &id,
			const std::string &account,
			const std::string &name) const {
		auto storage = FileStorage(root / "plugins" / id / "data" / account / "store.cbor");
		const auto bytes = storage.get(name);
		if (!bytes) {
			return std::nullopt;
		}
		auto decoded = cbor::Decode(*bytes);
		return decoded.ok() ? std::optional<cbor::Value>(decoded.value()) : std::nullopt;
	}

	fs::path root;
	HostEnvironment environment;
	fakes::FakeData data;
	ManagerServices services;
	CacheSettings cacheSettings;
	std::array<uint8_t, 32> key = {};
	int64_t time = 1000;

};

[[nodiscard]] GrantDecision Everything() {
	return { { "messages.read", "storage.private", "net.hosts" }, std::nullopt };
}

void Prepare(PluginManager &manager, const std::string &id, const std::string &account = "A") {
	CHECK(manager.setEnabled(id, true));
	CHECK(manager.grant(id, account, Everything()));
}

void TestLifecycleAndPersistence() {
#ifdef FGPL_COUNTER_WASM
	auto lab = Lab("fgpl-manager-life");
	{
		auto manager = PluginManager(lab.config());
		const auto refused = manager.install(CounterArchive("com.example.counter", "1.0.0"), [] {
			return InstallOptions();
		}());
		CHECK(refused.ok());
		auto plain = Build(Document("com.example.plain", "1.0.0"), { { "plugin.wasm", ReadFile(FGPL_COUNTER_WASM) } });
		const auto unsignedResult = manager.install(plain.archive);
		CHECK(!unsignedResult.ok() && unsignedResult.error().error == InstallError::UnsignedRefused);
		InstallOptions allow;
		allow.acceptUnsigned = true;
		CHECK(manager.install(plain.archive, allow).ok());
		CHECK(manager.uninstall("com.example.plain", true));

		auto infos = manager.list();
		CHECK(infos.size() == 1);
		CHECK(infos[0].status == PluginStatus::Disabled);
		CHECK(infos[0].signedPackage);
		CHECK(infos[0].requests.size() == 2);
		CHECK(infos[0].issues.empty());

		CHECK(manager.dispatch("A", Event()).delivered == 0);
		Prepare(manager, "com.example.counter");
		for (auto i = 0; i != 3; ++i) {
			const auto report = manager.dispatch("A", Event());
			CHECK(report.delivered == 1);
		}
		CHECK(manager.runningCount() == 1);
		CHECK(manager.list()[0].status == PluginStatus::Running);
		CHECK(manager.list()[0].stats.events == 3);
		CHECK(manager.dispatch("A", Event(api::kServiceChat)).delivered == 0);
		const auto logs = manager.log("com.example.counter");
		auto startedLine = false;
		for (const auto &line : logs) {
			startedLine = startedLine || line == "started for A";
		}
		CHECK(startedLine);
	}
	CHECK(lab.stored("com.example.counter", "A", "count")->asInt() == 3);
	{
		auto manager = PluginManager(lab.config());
		CHECK(!manager.stateWasTampered());
		const auto infos = manager.list();
		CHECK(infos.size() == 1 && infos[0].enabled);
		CHECK(manager.grants("com.example.counter", "A").has_value());
		CHECK(manager.dispatch("A", Event()).delivered == 1);
		manager.shutdown();
	}
	CHECK(lab.stored("com.example.counter", "A", "count")->asInt() == 4);

	auto text = std::string();
	{
		auto stream = std::ifstream(lab.root / "plugins" / "state.json");
		text = std::string(std::istreambuf_iterator<char>(stream), {});
	}
	const auto at = text.find("\"granted\":[");
	CHECK(at != std::string::npos);
	text.insert(at + 11, "\"clipboard\",");
	{
		auto stream = std::ofstream(lab.root / "plugins" / "state.json", std::ios::trunc);
		stream << text;
	}
	{
		auto manager = PluginManager(lab.config());
		CHECK(manager.stateWasTampered());
		const auto infos = manager.list();
		CHECK(infos.size() == 1 && !infos[0].enabled);
		CHECK(infos[0].disabledReason.find("outside the app") != std::string::npos);
		CHECK(!manager.grants("com.example.counter", "A").has_value());
		CHECK(manager.dispatch("A", Event()).delivered == 0);
		CHECK(manager.setEnabled("com.example.counter", true));
		CHECK(manager.dispatch("A", Event()).delivered == 0);
		CHECK(manager.grant("com.example.counter", "A", Everything()));
		CHECK(manager.dispatch("A", Event()).delivered == 1);
	}
#endif
}

void TestInstallPolicy() {
#ifdef FGPL_COUNTER_WASM
	auto lab = Lab("fgpl-manager-policy");
	auto manager = PluginManager(lab.config());
	CHECK(manager.install(CounterArchive("com.example.policy", "1.1.0", 1)).ok());

	const auto same = manager.install(CounterArchive("com.example.policy", "1.1.0", 1));
	CHECK(!same.ok() && same.error().error == InstallError::SameVersion);
	const auto older = manager.install(CounterArchive("com.example.policy", "1.0.0", 1));
	CHECK(!older.ok() && older.error().error == InstallError::DowngradeRefused);
	InstallOptions downgrade;
	downgrade.allowDowngrade = true;
	CHECK(manager.install(CounterArchive("com.example.policy", "1.0.0", 1), downgrade).ok());
	const auto stranger = manager.install(CounterArchive("com.example.policy", "1.2.0", 2));
	CHECK(!stranger.ok() && stranger.error().error == InstallError::PublisherChanged);
	InstallOptions trust;
	trust.acceptNewPublisher = true;
	CHECK(manager.install(CounterArchive("com.example.policy", "1.2.0", 2), trust).ok());

	auto incompatible = Document("com.example.future", "1.0.0");
	Set(incompatible, "compat.freshgram", Text(">=9.0.0"));
	const auto key = Seed(1);
	const auto future = manager.install(Build(incompatible, { { "plugin.wasm", ReadFile(FGPL_COUNTER_WASM) } }, &key).archive);
	CHECK(!future.ok() && future.error().error == InstallError::Incompatible);

	auto dependent = Document("com.example.child", "1.0.0");
	Set(dependent, "dependencies", cbor::Value::FromArray({
		cbor::Value::FromMap({ { "id", Text("com.example.policy") }, { "version", Text(">=9.0.0") } }),
	}));
	const auto missing = manager.install(Build(dependent, { { "plugin.wasm", ReadFile(FGPL_COUNTER_WASM) } }, &key).archive);
	CHECK(!missing.ok() && missing.error().error == InstallError::MissingDependency);
	Set(dependent, "dependencies", cbor::Value::FromArray({
		cbor::Value::FromMap({ { "id", Text("com.example.policy") }, { "version", Text(">=1.0.0") } }),
	}));
	CHECK(manager.install(Build(dependent, { { "plugin.wasm", ReadFile(FGPL_COUNTER_WASM) } }, &key).archive).ok());

	auto hostile = Document("com.example.hostile", "1.0.0");
	Set(hostile, "conflicts", cbor::Value::FromArray({
		cbor::Value::FromMap({ { "id", Text("com.example.policy") } }),
	}));
	const auto conflict = manager.install(Build(hostile, { { "plugin.wasm", ReadFile(FGPL_COUNTER_WASM) } }, &key).archive);
	CHECK(!conflict.ok() && conflict.error().error == InstallError::Conflict);

	const auto garbage = manager.install(Bytes("not a package"));
	CHECK(!garbage.ok() && garbage.error().error == InstallError::Package);

	manager.onLock();
	const auto locked = manager.install(CounterArchive("com.example.later", "1.0.0"));
	CHECK(!locked.ok() && locked.error().error == InstallError::Locked);
#endif
}

void TestUpdatesAndRollback() {
#ifdef FGPL_COUNTER_WASM
	auto lab = Lab("fgpl-manager-update");
	auto manager = PluginManager(lab.config());
	const auto id = std::string("com.example.updating");
	CHECK(manager.install(CounterArchive(id, "1.0.0")).ok());
	Prepare(manager, id);
	CHECK(manager.dispatch("A", Event()).delivered == 1);

	const auto update = manager.install(CounterArchive(id, "1.1.0", 1, {
		{ "messages.read", {} },
		{ "storage.private", {} },
		{ "net.hosts", { "api.example.com" } },
	}));
	CHECK(update.ok() && update.value().updated);
	CHECK(update.ok() && update.value().newRequests == std::vector<std::string>{ "net.hosts" });
	CHECK(manager.list()[0].pendingUpdate);
	CHECK(manager.list()[0].version == "1.1.0");
	CHECK(manager.grants(id, "A")->granted.size() == 2);
	CHECK(manager.dispatch("A", Event()).delivered == 1);
	CHECK(!manager.list()[0].pendingUpdate);
	CHECK(!fs::exists(lab.root / "plugins" / id / "1.0.0"));
	CHECK(lab.stored(id, "A", "count")->asInt() == 2);

	const auto broken = manager.install(WatArchive(id, "1.2.0", CrashOnInit()));
	CHECK(broken.ok());
	CHECK(manager.list()[0].version == "1.2.0");
	const auto failed = manager.dispatch("A", Event());
	CHECK(failed.delivered == 0);
	CHECK(manager.list()[0].version == "1.1.0");
	CHECK(!manager.list()[0].pendingUpdate);
	auto rolled = false;
	for (const auto &line : manager.log(id)) {
		rolled = rolled || line.find("rolled back to 1.1.0") != std::string::npos;
	}
	CHECK(rolled);
	CHECK(manager.list()[0].enabled);
	CHECK(manager.dispatch("A", Event()).delivered == 1);
	CHECK(lab.stored(id, "A", "count")->asInt() == 3);

	const auto crashing = manager.install(WatArchive(id, "1.3.0", CrashOnEvent()));
	CHECK(crashing.ok());
	CHECK(manager.dispatch("A", Event()).failed == 1);
	CHECK(manager.list()[0].version == "1.1.0");
#endif
}

void TestCrashLoopDisablesThePlugin() {
	auto lab = Lab("fgpl-manager-crash");
	auto manager = PluginManager(lab.config());
	const auto id = std::string("com.example.crashy");
	CHECK(manager.install(WatArchive(id, "1.0.0", CrashOnEvent())).ok());
	Prepare(manager, id);

	CHECK(manager.dispatch("A", Event()).failed == 1);
	CHECK(manager.runningCount() == 0);
	const auto waiting = manager.dispatch("A", Event());
	CHECK(waiting.skipped == 1 && waiting.failed == 0);
	CHECK(manager.list()[0].stats.dropped == 1);

	lab.time += 1500;
	CHECK(manager.dispatch("A", Event()).failed == 1);
	lab.time += 500;
	CHECK(manager.dispatch("A", Event()).skipped == 1);
	lab.time += 3000;
	CHECK(manager.dispatch("A", Event()).failed == 1);

	const auto info = manager.list()[0];
	CHECK(!info.enabled);
	CHECK(info.status == PluginStatus::Disabled);
	CHECK(info.disabledReason.find("repeated failures") != std::string::npos);
	CHECK(info.stats.failures == 3);
	lab.time += 100000;
	CHECK(manager.dispatch("A", Event()).failed == 0);

	CHECK(manager.setEnabled(id, true));
	CHECK(manager.list()[0].disabledReason.empty());
	CHECK(manager.dispatch("A", Event()).failed == 1);
}

void TestLockHiddenAndLogout() {
#ifdef FGPL_COUNTER_WASM
	auto lab = Lab("fgpl-manager-lock");
	auto manager = PluginManager(lab.config());
	const auto id = std::string("com.example.locked");
	CHECK(manager.install(CounterArchive(id, "1.0.0")).ok());
	Prepare(manager, id, "A");
	CHECK(manager.grant(id, "B", Everything()));
	CHECK(manager.dispatch("A", Event()).delivered == 1);
	CHECK(manager.dispatch("B", Event()).delivered == 1);
	CHECK(manager.runningCount() == 2);
	CHECK(lab.stored(id, "A", "count")->asInt() == 1);
	CHECK(lab.stored(id, "B", "count")->asInt() == 1);

	manager.onLock();
	CHECK(manager.runningCount() == 0);
	CHECK(manager.dispatch("A", Event()).delivered == 0);
	CHECK(!manager.startEnabled("A").started);
	manager.onUnlock();
	CHECK(manager.startEnabled("A").started == 1);
	CHECK(manager.dispatch("A", Event()).delivered == 1);
	CHECK(lab.stored(id, "A", "count")->asInt() == 2);

	manager.setHidden(true);
	CHECK(manager.list().empty());
	CHECK(manager.runningCount() == 0);
	CHECK(manager.dispatch("A", Event()).delivered == 0);
	manager.setHidden(false);
	CHECK(manager.list().size() == 1);
	CHECK(manager.dispatch("A", Event()).delivered == 1);

	manager.onLogout("A");
	CHECK(!manager.grants(id, "A").has_value());
	CHECK(manager.grants(id, "B").has_value());
	CHECK(manager.dispatch("A", Event()).delivered == 0);
	CHECK(manager.dispatch("B", Event()).delivered == 1);

	CHECK(manager.grant(id, "B", { { "messages.read" }, std::nullopt }));
	CHECK(manager.dispatch("B", Event()).delivered == 1);
	CHECK(lab.stored(id, "B", "count")->asInt() == 2);
	CHECK(!manager.grant(id, "../evil", Everything()));
	CHECK(!manager.grant("com.example.nothing", "A", Everything()));
#endif
}

void TestUninstallAndDamage() {
#ifdef FGPL_COUNTER_WASM
	auto lab = Lab("fgpl-manager-damage");
	auto manager = PluginManager(lab.config());
	const auto id = std::string("com.example.fragile");
	CHECK(manager.install(CounterArchive(id, "1.0.0")).ok());
	Prepare(manager, id);
	CHECK(manager.dispatch("A", Event()).delivered == 1);

	manager.onLock();
	manager.onUnlock();
	const auto path = lab.root / "plugins" / id / "1.0.0" / "package.fgpl";
	auto bytes = ReadFile(path);
	bytes[bytes.size() / 3] ^= 0x10;
	{
		auto stream = std::ofstream(path, std::ios::binary | std::ios::trunc);
		stream.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
	}
	CHECK(manager.dispatch("A", Event()).delivered == 0);
	auto damaged = false;
	for (const auto &line : manager.log(id)) {
		damaged = damaged || line.find("changed after it was installed") != std::string::npos;
	}
	CHECK(damaged);

	CHECK(manager.uninstall(id, false));
	CHECK(!fs::exists(lab.root / "plugins" / id / "1.0.0"));
	CHECK(fs::exists(lab.root / "plugins" / id / "data"));
	CHECK(manager.list().empty());
	CHECK(!manager.uninstall(id, false));
	CHECK(manager.install(CounterArchive(id, "1.0.0")).ok());
	CHECK(manager.uninstall(id, true));
	CHECK(!fs::exists(lab.root / "plugins" / id));
#endif
}

void TestPythonPlugin() {
	if (!pysupport::Installed()) {
		return;
	}
	auto lab = Lab("fgpl-manager-python");
	auto manager = PluginManager(lab.config());
	auto document = Document("com.example.pycounter", "1.0.0");
	Set(document, "runtime.kind", Text("python"));
	Remove(document, "runtime.module");
	Set(document, "runtime.entry", Text("main.py"));
	const auto key = Seed(4);
	const auto built = Build(document, {
		{ "main.py", ReadFile(fs::path(FGPL_PYTHON_PLUGIN_DIR) / "counter_plugin.py") },
	}, &key);
	const auto installed = manager.install(built.archive);
	CHECK(installed.ok());
	Prepare(manager, "com.example.pycounter");
	CHECK(manager.dispatch("A", Event()).delivered == 1);
	CHECK(manager.dispatch("A", Event()).delivered == 1);
	CHECK(lab.stored("com.example.pycounter", "A", "count")->asInt() == 2);
	auto logged = false;
	for (const auto &line : manager.log("com.example.pycounter")) {
		logged = logged || line == "count=2";
	}
	CHECK(logged);
	manager.shutdown();
	CHECK(fs::exists(lab.root / "cache"));
}

} // namespace

int main() {
	TestLifecycleAndPersistence();
	TestInstallPolicy();
	TestUpdatesAndRollback();
	TestCrashLoopDisablesThePlugin();
	TestLockHiddenAndLogout();
	TestUninstallAndDamage();
	TestPythonPlugin();
	return Failures() ? 1 : 0;
}
