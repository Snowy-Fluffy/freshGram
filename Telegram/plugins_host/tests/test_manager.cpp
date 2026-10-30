#include "check.h"
#include "fakes.h"
#include "fgpl/base64.h"
#include "fgpl/json.h"
#include "fgpl/manager.h"
#include "fgpl/sha256.h"
#include "package_builder.h"
#include "python_support.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <fstream>
#include <iterator>
#include <chrono>
#include <string>
#include <thread>

#include <dirent.h>
#include <unistd.h>

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
		environment.apiLevel = 2;
		services.data = &data;
		services.clockMs = [this] { return time.load(); };
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
		result.storageFlushMs = 0;
		result.environment = environment;
		result.services = services;
		result.stateKey = key;
		if (std::getenv("FGPL_NO_SANDBOX")) {
			result.host.path = FGPL_UNSANDBOXED_HOST_BINARY;
			result.host.unsandboxedTestHost = true;
		}
		return result;
	}

	[[nodiscard]] std::optional<cbor::Value> stored(
			const std::string &id,
			const std::string &account,
			const std::string &name) const {
		(void)account;
		auto storage = FileStorage(root / "plugins" / id / "data" / "store.cbor");
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
	std::atomic<int64_t> time{ 1000 };

};

[[nodiscard]] GrantDecision Everything() {
	return { { "messages.read", "storage.private", "net.https" }, std::nullopt };
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

		CHECK(manager.dispatchAndWait("A", Event()).delivered == 0);
		Prepare(manager, "com.example.counter");
		for (auto i = 0; i != 3; ++i) {
			const auto report = manager.dispatchAndWait("A", Event());
			CHECK(report.delivered == 1);
		}
		CHECK(manager.runningCount() == 1);
		CHECK(manager.list()[0].status == PluginStatus::Running);
		CHECK(manager.list()[0].stats.events == 3);
		CHECK(manager.dispatchAndWait("A", Event(api::kServiceChat)).delivered == 0);
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
		CHECK(manager.dispatchAndWait("A", Event()).delivered == 1);
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
		CHECK(manager.dispatchAndWait("A", Event()).delivered == 0);
		CHECK(manager.setEnabled("com.example.counter", true));
		CHECK(manager.dispatchAndWait("A", Event()).delivered == 0);
		CHECK(manager.grant("com.example.counter", "A", Everything()));
		CHECK(manager.dispatchAndWait("A", Event()).delivered == 1);
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
	CHECK(manager.dispatchAndWait("A", Event()).delivered == 1);

	const auto update = manager.install(CounterArchive(id, "1.1.0", 1, {
		{ "messages.read", {} },
		{ "storage.private", {} },
		{ "net.https", {} },
	}));
	CHECK(update.ok() && update.value().updated);
	CHECK(update.ok() && update.value().newRequests == std::vector<std::string>{ "net.https" });
	CHECK(manager.list()[0].pendingUpdate);
	CHECK(manager.list()[0].version == "1.1.0");
	CHECK(manager.grants(id, "A")->granted.size() == 2);
	CHECK(manager.dispatchAndWait("A", Event()).delivered == 1);
	CHECK(!manager.list()[0].pendingUpdate);
	CHECK(!fs::exists(lab.root / "plugins" / id / "1.0.0"));
	CHECK(lab.stored(id, "A", "count")->asInt() == 2);

	const auto broken = manager.install(WatArchive(id, "1.2.0", CrashOnInit()));
	CHECK(broken.ok());
	CHECK(manager.list()[0].version == "1.2.0");
	const auto failed = manager.dispatchAndWait("A", Event());
	CHECK(failed.delivered == 0);
	CHECK(manager.list()[0].version == "1.1.0");
	CHECK(!manager.list()[0].pendingUpdate);
	auto rolled = false;
	for (const auto &line : manager.log(id)) {
		rolled = rolled || line.find("rolled back to 1.1.0") != std::string::npos;
	}
	CHECK(rolled);
	CHECK(manager.list()[0].enabled);
	CHECK(manager.dispatchAndWait("A", Event()).delivered == 1);
	CHECK(lab.stored(id, "A", "count")->asInt() == 3);

	const auto crashing = manager.install(WatArchive(id, "1.3.0", CrashOnEvent()));
	CHECK(crashing.ok());
	CHECK(manager.dispatchAndWait("A", Event()).failed == 1);
	CHECK(manager.list()[0].version == "1.1.0");
#endif
}

void TestCrashLoopDisablesThePlugin() {
	auto lab = Lab("fgpl-manager-crash");
	auto manager = PluginManager(lab.config());
	const auto id = std::string("com.example.crashy");
	CHECK(manager.install(WatArchive(id, "1.0.0", CrashOnEvent())).ok());
	Prepare(manager, id);

	CHECK(manager.dispatchAndWait("A", Event()).failed == 1);
	CHECK(manager.runningCount() == 0);
	const auto waiting = manager.dispatchAndWait("A", Event());
	CHECK(waiting.skipped == 1 && waiting.failed == 0);
	CHECK(manager.list()[0].stats.dropped == 1);

	lab.time += 1500;
	CHECK(manager.dispatchAndWait("A", Event()).failed == 1);
	lab.time += 500;
	CHECK(manager.dispatchAndWait("A", Event()).skipped == 1);
	lab.time += 3000;
	CHECK(manager.dispatchAndWait("A", Event()).failed == 1);

	const auto info = manager.list()[0];
	CHECK(!info.enabled);
	CHECK(info.status == PluginStatus::Disabled);
	CHECK(info.disabledReason.find("repeated failures") != std::string::npos);
	CHECK(info.stats.failures == 3);
	lab.time += 100000;
	CHECK(manager.dispatchAndWait("A", Event()).failed == 0);

	CHECK(manager.setEnabled(id, true));
	CHECK(manager.list()[0].disabledReason.empty());
	CHECK(manager.dispatchAndWait("A", Event()).failed == 1);
}

void TestLockHiddenAndLogout() {
#ifdef FGPL_COUNTER_WASM
	auto lab = Lab("fgpl-manager-lock");
	auto manager = PluginManager(lab.config());
	const auto id = std::string("com.example.locked");
	CHECK(manager.install(CounterArchive(id, "1.0.0")).ok());
	Prepare(manager, id, "A");
	CHECK(manager.grant(id, "B", Everything()));
	CHECK(manager.dispatchAndWait("A", Event()).delivered == 1);
	CHECK(manager.dispatchAndWait("B", Event()).delivered == 1);
	CHECK(manager.runningCount() == 2);
	CHECK(manager.grants(id, "B")->granted == manager.grants(id, "A")->granted);
	CHECK(lab.stored(id, "A", "count")->asInt() == 2);

	manager.onLock();
	CHECK(manager.runningCount() == 0);
	CHECK(manager.dispatchAndWait("A", Event()).delivered == 0);
	CHECK(!manager.startEnabled("A").started);
	manager.onUnlock();
	CHECK(manager.startEnabled("A").started == 0);
	CHECK(manager.dispatchAndWait("A", Event()).delivered == 1);
	CHECK(lab.stored(id, "A", "count")->asInt() == 3);

	manager.setHidden(true);
	CHECK(manager.list().empty());
	CHECK(manager.runningCount() == 0);
	CHECK(manager.dispatchAndWait("A", Event()).delivered == 0);
	manager.setHidden(false);
	CHECK(manager.list().size() == 1);
	CHECK(manager.dispatchAndWait("A", Event()).delivered == 1);

	manager.onLogout("A");
	CHECK(manager.runningCount() == 0);
	CHECK(manager.grants(id, "A").has_value());
	CHECK(manager.grants(id, "B").has_value());
	const auto afterLogout = manager.dispatchAndWait("B", Event());
	CHECK(afterLogout.delivered == 1);
	if (afterLogout.delivered != 1) {
		for (const auto &line : manager.log(id)) {
			std::fprintf(stderr, "  log after logout: %s\n", line.c_str());
		}
	}

	CHECK(manager.grant(id, "B", { { "messages.read" }, std::nullopt }));
	const auto restricted = manager.dispatchAndWait("B", Event());
	CHECK(restricted.delivered == 1);
	CHECK(lab.stored(id, "B", "count")->asInt() == 5);
	if (restricted.delivered != 1) {
		for (const auto &line : manager.log(id)) {
			std::fprintf(stderr, "  log: %s\n", line.c_str());
		}
	}
	CHECK(!manager.grant(id, "../evil", Everything()));
	CHECK(!manager.grant("com.example.nothing", "A", Everything()));
#endif
}

void TestStorageRecoveryIsLogged() {
#ifdef FGPL_COUNTER_WASM
	auto lab = Lab("fgpl-manager-recovery");
	const auto id = std::string("com.example.counter");
	{
		auto manager = PluginManager(lab.config());
		CHECK(manager.install(CounterArchive(id, "1.0.0")).ok());
		Prepare(manager, id);
		CHECK(manager.dispatchAndWait("A", Event()).delivered == 1);
		manager.shutdown();
	}
	{
		auto manager = PluginManager(lab.config());
		CHECK(manager.dispatchAndWait("A", Event()).delivered == 1);
		manager.shutdown();
	}
	CHECK(lab.stored(id, "A", "count")->asInt() == 2);
	const auto file = lab.root / "plugins" / id / "data" / "store.cbor";
	{
		auto stream = std::ofstream(file, std::ios::binary | std::ios::trunc);
		stream << "not a cbor document";
	}
	auto manager = PluginManager(lab.config());
	CHECK(manager.dispatchAndWait("A", Event()).delivered == 1);
	auto logged = false;
	for (const auto &line : manager.log(id)) {
		logged = logged || line.find("restored the previous copy") != std::string::npos;
	}
	CHECK(logged);
	manager.shutdown();
	CHECK(lab.stored(id, "A", "count")->asInt() == 2);
#endif
}

void TestUninstallAndDamage() {
#ifdef FGPL_COUNTER_WASM
	auto lab = Lab("fgpl-manager-damage");
	auto manager = PluginManager(lab.config());
	const auto id = std::string("com.example.fragile");
	CHECK(manager.install(CounterArchive(id, "1.0.0")).ok());
	Prepare(manager, id);
	CHECK(manager.dispatchAndWait("A", Event()).delivered == 1);

	manager.onLock();
	manager.onUnlock();
	const auto path = lab.root / "plugins" / id / "1.0.0" / "package.fgpl";
	auto bytes = ReadFile(path);
	bytes[bytes.size() / 3] ^= 0x10;
	{
		auto stream = std::ofstream(path, std::ios::binary | std::ios::trunc);
		stream.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
	}
	CHECK(manager.dispatchAndWait("A", Event()).delivered == 0);
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
	CHECK(manager.dispatchAndWait("A", Event()).delivered == 1);
	CHECK(manager.dispatchAndWait("A", Event()).delivered == 1);
	CHECK(lab.stored("com.example.pycounter", "A", "count")->asInt() == 2);
	auto logged = false;
	for (const auto &line : manager.log("com.example.pycounter")) {
		logged = logged || line == "count=2";
	}
	CHECK(logged);
	manager.shutdown();
	CHECK(fs::exists(lab.root / "cache"));
}

void TestUiContributions() {
	if (!pysupport::Installed()) {
		return;
	}
	auto lab = Lab("fgpl-manager-ui");
	lab.time = 100000;
	auto pages = std::vector<std::pair<std::string, cbor::Value>>();
	auto windows = std::vector<std::string>();
	auto pagesMutex = std::mutex();
	auto config = lab.config();
	config.services.window = [&](
			const std::string &id,
			const std::string &account,
			const std::string &window,
			const std::string &mode,
			const cbor::Value *page) {
		const auto lock = std::scoped_lock(pagesMutex);
		windows.push_back(id + "/" + account + "/" + window + "/" + mode + (page ? "/open" : "/close"));
	};
	config.services.settingsPage = [&](const std::string &id, const std::string &account, const cbor::Value &page) {
		const auto lock = std::scoped_lock(pagesMutex);
		pages.emplace_back(id + "/" + account, page);
	};
	auto opened = std::vector<std::string>();
	config.services.openSettings = [&](const std::string &id, const std::string &account) {
		const auto lock = std::scoped_lock(pagesMutex);
		opened.push_back(id + "/" + account);
	};
	auto manager = PluginManager(config);
	auto document = Document("com.example.uidemo", "1.0.0", {
		{ "ui.menu", {} }, { "ui.command", {} }, { "ui.settings_page", {} }, { "storage.private", {} },
		{ "ui.dialog", {} },
		{ "ui.badges", {} },
	});
	Set(document, "runtime.kind", Text("python"));
	Remove(document, "runtime.module");
	Set(document, "runtime.entry", Text("main.py"));
	Remove(document, "contributes.events");
	Set(document, "contributes.menus", cbor::Value::FromArray({
		cbor::Value::FromMap({
			{ "surface", Text("message") }, { "id", Text("translate") },
			{ "title", cbor::Value::FromMap({ { "default", Text("Translate") }, { "ru", Text("Перевести") } }) },
		}),
	}));
	Set(document, "contributes.commands", cbor::Value::FromArray({
		cbor::Value::FromMap({ { "name", Text("tr") }, { "description", Text("Translate") } }),
	}));
	Set(document, "contributes.settingsPage", cbor::Value::FromBool(true));
	const auto key = Seed(5);
	const auto built = Build(document, {
		{ "main.py", ReadFile(fs::path(FGPL_PYTHON_PLUGIN_DIR) / "ui_demo.py") },
	}, &key);
	const auto installed = manager.install(built.archive);
	CHECK(installed.ok());
	if (!installed.ok()) {
		for (const auto &line : installed.error().details) {
			std::fprintf(stderr, "install: %s\n", line.c_str());
		}
		return;
	}
	const auto id = std::string("com.example.uidemo");
	CHECK(manager.setEnabled(id, true));

	CHECK(manager.contributions("A", "default").empty());
	const auto badgeClick = cbor::Value::FromMap({
		{ "type", Text("badge.click") }, { "id", Text("vip") }, { "user", Int(7) }, { "surface", Text("chat") },
	});
	CHECK(manager.grant(id, "A", { { "ui.menu", "ui.command", "ui.settings_page", "storage.private", "ui.dialog" }, std::nullopt }));
	CHECK(!manager.send(id, "A", badgeClick));
	CHECK(manager.grant(id, "A", { { "ui.menu", "ui.command", "ui.settings_page", "storage.private", "ui.dialog", "ui.badges" }, std::nullopt }));
	CHECK(manager.send(id, "A", badgeClick));
	const auto contributions = manager.contributions("A", "ru");
	CHECK(contributions.size() == 1);
	if (!contributions.empty()) {
		CHECK(contributions[0].menus.size() == 1 && contributions[0].menus[0].title == "Перевести");
		CHECK(contributions[0].commands.size() == 1 && contributions[0].commands[0].name == "tr");
		CHECK(contributions[0].settingsPage);
	}
	CHECK(manager.contributions("B", "ru").size() == 1);

	const auto click = [](const char *surface, const char *menuId) {
		return cbor::Value::FromMap({
			{ "type", Text("menu.click") }, { "surface", Text(surface) }, { "id", Text(menuId) },
			{ "chat", Int(1) }, { "message", Int(5) }, { "text", Text("hello") },
		});
	};
	CHECK(manager.send(id, "A", click("message", "translate")));
	CHECK(!manager.send(id, "A", click("message", "unknown")));
	CHECK(!manager.send(id, "A", click("chat", "translate")));
	CHECK(manager.send(id, "B", click("message", "translate")));
	CHECK(!manager.send("com.example.none", "A", click("message", "translate")));
	CHECK(!manager.send(id, "A", cbor::Value::FromMap({ { "type", Text("message.new") }, { "chat", Int(1) } })));
	CHECK(manager.send(id, "A", cbor::Value::FromMap({
		{ "type", Text("command") }, { "name", Text("tr") }, { "args", Text("bonjour") },
	})));
	CHECK(!manager.send(id, "A", cbor::Value::FromMap({ { "type", Text("command") }, { "name", Text("other") } })));
	CHECK(manager.send(id, "A", cbor::Value::FromMap({
		{ "type", Text("settings.change") }, { "id", Text("loud") }, { "value", cbor::Value::FromBool(true) },
	})));
	CHECK(manager.send(id, "A", cbor::Value::FromMap({
		{ "type", Text("command") }, { "name", Text("tr") }, { "args", Text("window") },
	})));
	CHECK(manager.drain(120000));
	CHECK(manager.send(id, "A", cbor::Value::FromMap({
		{ "type", Text("window.click") }, { "window", Text("board") }, { "id", Text("go") },
	})));
	CHECK(manager.drain(120000));
	const auto window = lab.stored(id, "A", "window");
	CHECK(window && window->asArray()->size() == 3 && *(*window->asArray())[1].asText() == "board");
	{
		const auto lock = std::scoped_lock(pagesMutex);
		CHECK(windows == (std::vector<std::string>{ "com.example.uidemo/A/board/window/open" }));
	}

	const auto menu = lab.stored(id, "A", "menu");
	CHECK(menu && menu->find("chat")->asInt() == 1 && menu->find("message")->isNull());
	CHECK(menu && menu->find("text")->isNull());
	{
		const auto lock = std::scoped_lock(pagesMutex);
		CHECK(std::find(opened.begin(), opened.end(), id + "/A") != opened.end());
	}
	const auto command = lab.stored(id, "A", "command");
	CHECK(command && *command->asText() == "window");
	const auto setting = lab.stored(id, "A", "setting");
	CHECK(setting && setting->asArray()->size() == 3 && *(*setting->asArray())[1].asText() == "loud");
	{
		const auto lock = std::scoped_lock(pagesMutex);
		CHECK(!pages.empty());
		const auto own = std::find_if(pages.begin(), pages.end(), [&](const auto &page) {
			return page.first == id + "/A";
		});
		CHECK(own != pages.end());
		if (own != pages.end()) {
			CHECK(*own->second.find("title")->asText() == "Demo");
			CHECK(own->second.find("items")->asArray()->size() == 2);
		}
	}

	CHECK(manager.setEnabled(id, false));
	CHECK(manager.contributions("A", "ru").empty());
	CHECK(!manager.send(id, "A", click("message", "translate")));
	manager.shutdown();
}

[[nodiscard]] std::string SlowModule(int64_t iterations) {
	return wat::Module("", "(local $i i32) (loop $l (local.set $i (i32.add (local.get $i) (i32.const 1))) (br_if $l (i32.lt_u (local.get $i) (i32.const "
		+ std::to_string(iterations) + ")))) (i32.const 0)");
}

[[nodiscard]] cbor::Bytes SlowArchive(const std::string &id, int64_t iterations) {
	auto document = Document(id, "1.0.0");
	Set(document, "runtime.limits.eventTimeoutMs", Int(30000));
	const auto key = Seed(1);
	return Build(document, { { "plugin.wasm", wat::Wasm(SlowModule(iterations)) } }, &key).archive;
}

[[nodiscard]] int ChildProcesses() {
	auto count = 0;
	const auto me = getpid();
	auto *directory = opendir("/proc");
	if (!directory) {
		return 0;
	}
	while (const auto *entry = readdir(directory)) {
		const auto name = std::string(entry->d_name);
		if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) {
			continue;
		}
		auto stream = std::ifstream("/proc/" + name + "/stat");
		auto line = std::string();
		if (!std::getline(stream, line)) {
			continue;
		}
		const auto close = line.rfind(')');
		if (close == std::string::npos) {
			continue;
		}
		auto state = char(0);
		auto parent = 0;
		if (std::sscanf(line.c_str() + close + 2, "%c %d", &state, &parent) == 2
			&& parent == me
			&& state != 'Z') {
			++count;
		}
	}
	closedir(directory);
	return count;
}

[[nodiscard]] bool NoChildren(int waitMs = 2000) {
	for (auto waited = 0; waited <= waitMs; waited += 50) {
		if (ChildProcesses() == 0) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	return false;
}

[[nodiscard]] double Since(std::chrono::steady_clock::time_point begin) {
	return std::chrono::duration<double>(
		std::chrono::steady_clock::now() - begin).count();
}

void TestDispatchDoesNotBlockAndKeepsOrder() {
#ifdef FGPL_COUNTER_WASM
	auto lab = Lab("fgpl-manager-order");
	auto manager = PluginManager(lab.config());
	const auto id = std::string("com.example.ordered");
	CHECK(manager.install(CounterArchive(id, "1.0.0")).ok());
	Prepare(manager, id);
	const auto begin = std::chrono::steady_clock::now();
	auto queued = 0;
	for (auto chat = 1; chat <= 20; ++chat) {
		queued += manager.dispatch("A", Event(chat)).queued;
	}
	CHECK(queued == 20);
	CHECK(Since(begin) < 1.0);
	CHECK(manager.drain());
	CHECK(lab.stored(id, "A", "count")->asInt() == 20);
	CHECK(lab.stored(id, "A", "last_chat")->asInt() == 20);
	CHECK(manager.list()[0].stats.events == 20);
#endif
}

void TestSlowPluginDoesNotDelayOthers() {
#ifdef FGPL_COUNTER_WASM
	auto lab = Lab("fgpl-manager-fair");
	auto manager = PluginManager(lab.config());
	CHECK(manager.install(SlowArchive("com.example.slow", 800000000)).ok());
	CHECK(manager.install(CounterArchive("com.example.fast", "1.0.0")).ok());
	Prepare(manager, "com.example.slow");
	Prepare(manager, "com.example.fast");
	const auto begin = std::chrono::steady_clock::now();
	for (auto chat = 1; chat <= 4; ++chat) {
		CHECK(manager.dispatch("A", Event(chat)).queued == 2);
	}
	auto fastDone = 0.;
	while (Since(begin) < 20) {
		manager.pump();
		const auto count = lab.stored("com.example.fast", "A", "count");
		if (count && count->asInt() == 4) {
			fastDone = Since(begin);
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	CHECK(fastDone > 0 && fastDone < 5);
	manager.pump();
	auto slowEvents = uint64_t(99);
	for (const auto &info : manager.list()) {
		if (info.id == "com.example.slow") {
			slowEvents = info.stats.events;
		}
	}
	CHECK(slowEvents < 4);
	CHECK(manager.drain(60000));
	manager.shutdown();
#endif
}

void TestQueueOverflowDropsTheOldest() {
	auto lab = Lab("fgpl-manager-overflow");
	auto config = lab.config();
	config.maxQueuedEvents = 4;
	auto manager = PluginManager(config);
	CHECK(manager.install(SlowArchive("com.example.busy", 300000000)).ok());
	Prepare(manager, "com.example.busy");
	CHECK(manager.dispatch("A", Event()).queued == 1);
	CHECK(manager.drain());
	auto dropped = 0;
	auto queued = 0;
	for (auto i = 0; i != 30; ++i) {
		const auto report = manager.dispatch("A", Event(i + 1));
		dropped += report.dropped;
		queued += report.queued;
	}
	CHECK(queued == 30);
	CHECK(dropped >= 20);
	CHECK(manager.drain(60000));
	const auto info = manager.list()[0];
	CHECK(info.stats.dropped >= 20);
	CHECK(info.stats.events <= 12);
	CHECK(info.stats.events >= 5);
}

void TestLockIsPromptWhileBusy() {
	auto lab = Lab("fgpl-manager-busylock");
	auto manager = PluginManager(lab.config());
	CHECK(manager.install(SlowArchive("com.example.long", 4000000000)).ok());
	Prepare(manager, "com.example.long");
	CHECK(manager.dispatch("A", Event()).queued == 1);
	for (auto waited = 0; waited < 5000 && manager.pump() + manager.runningCount() == 0; waited += 20) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	const auto begin = std::chrono::steady_clock::now();
	manager.onLock();
	CHECK(Since(begin) < 2.0);
	CHECK(manager.runningCount() == 0);
	CHECK(NoChildren());
	CHECK(manager.dispatch("A", Event()).queued == 0);
	manager.onUnlock();
	CHECK(manager.dispatch("A", Event()).queued == 1);
	manager.onLock();
	CHECK(NoChildren());
}

void TestManyPluginsAndAccounts() {
#ifdef FGPL_COUNTER_WASM
	auto lab = Lab("fgpl-manager-stress");
	auto config = lab.config();
	config.maxConcurrentStarts = 1;
	auto manager = PluginManager(config);
	const auto ids = std::vector<std::string>{
		"com.example.stress1", "com.example.stress2", "com.example.stress3",
	};
	for (const auto &id : ids) {
		CHECK(manager.install(CounterArchive(id, "1.0.0")).ok());
		CHECK(manager.setEnabled(id, true));
		CHECK(manager.grant(id, "A", Everything()));
		CHECK(manager.grant(id, "B", Everything()));
	}
	for (auto round = 1; round <= 25; ++round) {
		CHECK(manager.dispatch("A", Event(round)).queued == 3);
		CHECK(manager.dispatch("B", Event(round)).queued == 3);
	}
	CHECK(manager.drain(120000));
	CHECK(manager.runningCount() == 6);
	if (manager.runningCount() != 6) {
		for (const auto &id : ids) {
			for (const auto &line : manager.log(id)) {
				std::fprintf(stderr, "  %s: %s\n", id.c_str(), line.c_str());
			}
		}
	}
	for (const auto &id : ids) {
		for (const auto account : { "A", "B" }) {
			const auto count = lab.stored(id, account, "count");
			CHECK(count && count->asInt() == 50);
			CHECK(lab.stored(id, account, "last_chat")->asInt() == 25);
		}
	}
	manager.shutdown();
	CHECK(NoChildren());
#endif
}

void TestStopWhileStartingIsPrompt() {
	if (!pysupport::Installed()) {
		return;
	}
	auto lab = Lab("fgpl-manager-cancel");
	auto manager = PluginManager(lab.config());
	auto document = Document("com.example.pyslow", "1.0.0");
	Set(document, "runtime.kind", Text("python"));
	Remove(document, "runtime.module");
	Set(document, "runtime.entry", Text("main.py"));
	const auto key = Seed(4);
	const auto built = Build(document, {
		{ "main.py", ReadFile(fs::path(FGPL_PYTHON_PLUGIN_DIR) / "counter_plugin.py") },
	}, &key);
	CHECK(manager.install(built.archive).ok());
	Prepare(manager, "com.example.pyslow");
	CHECK(manager.dispatch("A", Event()).queued == 1);
	std::this_thread::sleep_for(std::chrono::milliseconds(800));
	const auto begin = std::chrono::steady_clock::now();
	manager.onLock();
	CHECK(Since(begin) < 3.0);
	CHECK(manager.runningCount() == 0);
	CHECK(NoChildren());
	manager.onUnlock();
	CHECK(manager.dispatchAndWait("A", Event(), 120000).delivered == 1);
}

} // namespace

[[nodiscard]] std::string CallingModule(const std::string &request) {
	auto escaped = std::string();
	for (const auto ch : request) {
		char buffer[4];
		std::snprintf(buffer, sizeof(buffer), "\\%02x", static_cast<unsigned char>(ch));
		escaped += buffer;
	}
	return wat::Module(
		"(data (i32.const 64) \"" + escaped + "\")",
		"(drop (call $call (i32.const 64) (i32.const "
			+ std::to_string(request.size())
			+ ") (i32.const 512) (i32.const 256))) (i32.const 0)");
}

[[nodiscard]] std::string UnloadRequest() {
	return std::string("\xa1\x62op\x6dplugin.unload", 18);
}

[[nodiscard]] std::string OutgoingRequest() {
	return std::string("\xa2\x62op\x75messages.set_outgoing\x61" "a\xa1\x64text\x65HELLO", 40);
}

[[nodiscard]] cbor::Bytes CustomArchive(
		const std::string &id,
		const std::string &wat,
		std::vector<std::pair<std::string, std::vector<std::string>>> permissions,
		std::function<void(cbor::Value &)> edit = nullptr) {
	const auto key = Seed(1);
	auto document = Document(id, "1.0.0", std::move(permissions));
	if (edit) {
		edit(document);
	}
	return Build(document, { { "plugin.wasm", wat::Wasm(wat) } }, &key).archive;
}

void TestSelfUnloadAndLazyStart() {
	auto lab = Lab("fgpl-manager-unload");
	auto manager = PluginManager(lab.config());
	const auto id = std::string("com.example.sleepy");
	CHECK(manager.install(CustomArchive(id, CallingModule(UnloadRequest()), {
		{ "messages.read", {} },
	})).ok());
	CHECK(manager.setEnabled(id, true));
	CHECK(manager.grant(id, "A", { { "messages.read" }, std::nullopt }));
	CHECK(manager.startEnabled("A").started == 0);
	CHECK(manager.runningCount() == 0);
	CHECK(manager.dispatchAndWait("A", Event()).delivered == 1);
	for (auto waited = 0; waited < 5000 && manager.runningCount() != 0; waited += 20) {
		manager.pump();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	CHECK(manager.runningCount() == 0);
	CHECK(manager.list()[0].stats.unloads == 1);
	CHECK(manager.list()[0].stats.failures == 0);
	CHECK(manager.list()[0].enabled);
	CHECK(manager.dispatchAndWait("A", Event()).delivered == 1);
	auto edited = Event();
	edited.asMap()->front().value = Text("message.edited");
	CHECK(manager.dispatchAndWait("A", edited).delivered == 0);
	CHECK(manager.list()[0].stats.cpuMs < 60000);
}

void TestOutgoingTransform() {
	auto lab = Lab("fgpl-manager-outgoing");
	auto manager = PluginManager(lab.config());
	const auto id = std::string("com.example.shout");
	const auto outgoing = [](const char *policy) {
		return [policy](cbor::Value &document) {
			Set(document, "contributes.outgoing.onFailure", Text(policy));
		};
	};
	CHECK(manager.install(CustomArchive(id, CallingModule(OutgoingRequest()), {
		{ "messages.read", {} },
		{ "messages.transform_outgoing", {} },
	}, outgoing("pass"))).ok());
	CHECK(manager.setEnabled(id, true));
	CHECK(manager.grant(id, "A", { { "messages.read", "messages.transform_outgoing" }, std::nullopt }));
	CHECK(manager.startEnabled("A").started == 1);
	CHECK(manager.drain(20000));
	const auto changed = manager.transformOutgoing("A", 1, false, "hi", 5000);
	CHECK(!changed.blocked && changed.text == "HELLO");
	const auto secret = manager.transformOutgoing("A", 3, true, "hi", 5000);
	CHECK(!secret.blocked && secret.text == "hi");
	const auto service = manager.transformOutgoing("A", api::kServiceChat, false, "hi", 5000);
	CHECK(service.text == "hi");
	CHECK(manager.transformOutgoing("B", 1, false, "hi", 5000).text == "hi");

	const auto strict = std::string("com.example.strict");
	CHECK(manager.install(CustomArchive(strict, CrashOnEvent(), {
		{ "messages.read", {} },
		{ "messages.transform_outgoing", {} },
	}, outgoing("block"))).ok());
	CHECK(manager.setEnabled(strict, true));
	CHECK(manager.grant(strict, "A", { { "messages.transform_outgoing" }, std::nullopt }));
	CHECK(manager.startEnabled("A").started == 0);
	CHECK(manager.drain(20000));
	const auto blocked = manager.transformOutgoing("A", 1, false, "hi", 5000);
	CHECK(blocked.blocked && blocked.plugin == strict);
}

[[nodiscard]] std::string EncodedRequest(const char *op, cbor::Map args = {}) {
	auto map = cbor::Map{ { "op", Text(op) } };
	if (!args.empty()) {
		map.push_back({ "a", cbor::Value::FromMap(std::move(args)) });
	}
	auto bytes = cbor::Bytes();
	CHECK(cbor::Encode(cbor::Value::FromMap(std::move(map)), bytes, 8));
	return std::string(bytes.begin(), bytes.end());
}

void TestOutgoingFilterDropAndOptions() {
	auto lab = Lab("fgpl-manager-outgoing-options");
	auto manager = PluginManager(lab.config());
	const auto permissions = std::vector<std::pair<std::string, std::vector<std::string>>>{
		{ "messages.read", {} },
		{ "messages.transform_outgoing", {} },
	};
	const auto filtered = [](const char *key, cbor::Value value) {
		return [=](cbor::Value &document) {
			Set(document, "contributes.outgoing.onFailure", Text("pass"));
			Set(document, std::string("contributes.outgoing.filter.") + key, value);
		};
	};
	const auto dropper = std::string("com.example.dropper");
	const auto options = std::string("com.example.options");
	CHECK(manager.install(CustomArchive(dropper, CallingModule(EncodedRequest(
		"messages.drop_outgoing",
		{ { "reason", Text("no spam") } })), permissions,
		filtered("chats", cbor::Value::FromArray({ Int(9) })))).ok());
	CHECK(manager.install(CustomArchive(options, CallingModule(EncodedRequest(
		"messages.set_outgoing",
		{
			{ "silent", cbor::Value::FromBool(true) },
			{ "reply_to", Int(5) },
		})), permissions,
		filtered("chat_types", Array({ "group" })))).ok());
	for (const auto &id : { dropper, options }) {
		CHECK(manager.setEnabled(id, true));
		CHECK(manager.grant(id, "A", { { "messages.read", "messages.transform_outgoing" }, std::nullopt }));
	}
	CHECK(manager.startEnabled("A").started == 2);
	CHECK(manager.drain(20000));

	const auto run = [&](int64_t chat, const char *type) {
		auto info = OutgoingInfo();
		info.chat = chat;
		info.chatType = type;
		info.text = "hi";
		auto result = std::optional<OutgoingResult>();
		manager.transformOutgoingAsync("A", info, 5000, [&](OutgoingResult value) {
			result = std::move(value);
		});
		while (!result) {
			manager.pump();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return std::move(*result);
	};
	const auto snapshot = manager.snapshot();
	auto probe = OutgoingInfo();
	probe.chat = 1;
	probe.chatType = "user";
	CHECK(!snapshot.transformsOutgoing(probe));
	probe.chatType = "group";
	CHECK(snapshot.transformsOutgoing(probe));
	probe.chat = 9;
	probe.chatType = "user";
	CHECK(snapshot.transformsOutgoing(probe));

	const auto untouched = run(1, "user");
	CHECK(!untouched.dropped && !untouched.silent && !untouched.replyTo && untouched.text == "hi");
	const auto changed = run(2, "group");
	CHECK(!changed.dropped && changed.silent == std::optional<bool>(true));
	CHECK(changed.replyTo == std::optional<int64_t>(5) && !changed.schedule);
	const auto dropped = run(9, "group");
	CHECK(dropped.dropped && dropped.plugin == dropper && dropped.reason == "no spam");
	CHECK(!dropped.silent);
}

void TestOutgoingEntities() {
	auto lab = Lab("fgpl-manager-outgoing-entities");
	auto manager = PluginManager(lab.config());
	const auto permissions = std::vector<std::pair<std::string, std::vector<std::string>>>{
		{ "messages.read", {} },
		{ "messages.transform_outgoing", {} },
	};
	const auto filtered = [](int64_t chat) {
		return [=](cbor::Value &document) {
			Set(document, "contributes.outgoing.onFailure", Text("pass"));
			Set(document, "contributes.outgoing.filter.chats", cbor::Value::FromArray({ Int(chat) }));
		};
	};
	const auto formatter = std::string("com.example.formatter");
	const auto rewriter = std::string("com.example.rewriter");
	CHECK(manager.install(CustomArchive(formatter, CallingModule(EncodedRequest(
		"messages.set_outgoing",
		{ { "entities", cbor::Value::FromArray({
			cbor::Value::FromMap({
				{ "type", Text("italic") },
				{ "offset", Int(1) },
				{ "length", Int(1) },
			}),
		}) } })), permissions, filtered(11))).ok());
	CHECK(manager.install(CustomArchive(rewriter, CallingModule(EncodedRequest(
		"messages.set_outgoing",
		{ { "text", Text("bye") } })), permissions, filtered(12))).ok());
	for (const auto &id : { formatter, rewriter }) {
		CHECK(manager.setEnabled(id, true));
		CHECK(manager.grant(id, "A", { { "messages.read", "messages.transform_outgoing" }, std::nullopt }));
	}
	CHECK(manager.startEnabled("A").started == 2);
	CHECK(manager.drain(20000));

	const auto run = [&](int64_t chat) {
		auto info = OutgoingInfo();
		info.chat = chat;
		info.chatType = "group";
		info.text = "hi";
		info.entities.push_back({ "bold", 0, 2 });
		auto result = std::optional<OutgoingResult>();
		manager.transformOutgoingAsync("A", info, 5000, [&](OutgoingResult value) {
			result = std::move(value);
		});
		while (!result) {
			manager.pump();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return std::move(*result);
	};
	const auto untouched = run(1);
	CHECK(untouched.text == "hi" && !untouched.entities);
	const auto formatted = run(11);
	CHECK(formatted.text == "hi");
	CHECK(formatted.entities && formatted.entities->size() == 1);
	if (formatted.entities && formatted.entities->size() == 1) {
		CHECK((*formatted.entities)[0].type == "italic" && (*formatted.entities)[0].offset == 1);
	}
	const auto rewritten = run(12);
	CHECK(rewritten.text == "bye" && rewritten.entities && rewritten.entities->empty());
}

void TestSettingsExport() {
#ifdef FGPL_COUNTER_WASM
	auto lab = Lab("fgpl-manager-export");
	auto manager = PluginManager(lab.config());
	const auto id = std::string("com.example.counter");
	const auto key = Seed(1);
	const auto build = [&](const char *version, int schema, bool exported) {
		auto document = Document(id, version);
		Set(document, "storage.schema", Int(schema));
		if (exported) {
			Set(document, "storage.export", cbor::Value::FromMap({
				{ "prefixes", Array({ "count" }) },
				{ "exclude", Array({ "count.secret" }) },
			}));
		}
		return Build(document, { { "plugin.wasm", ReadFile(FGPL_COUNTER_WASM) } }, &key).archive;
	};
	CHECK(manager.install(build("1.0.0", 2, true)).ok());
	Prepare(manager, id);
	for (auto i = 0; i != 2; ++i) {
		CHECK(manager.dispatchAndWait("A", Event()).delivered == 1);
	}
	CHECK(manager.setEnabled(id, false));
	CHECK(manager.setEnabled(id, true));
	CHECK(lab.stored(id, "A", "count")->asInt() == 2);

	const auto exported = manager.exportSettings(id);
	CHECK(exported.ok());
	if (!exported.ok()) {
		return;
	}
	const auto document = cbor::Decode(exported.value());
	CHECK(document.ok() && *document.value().find("plugin")->asText() == id);
	CHECK(document.value().find("items")->find("count")->asInt() == 2);
	CHECK(document.value().find("storageSchema")->asInt() == 2);

	CHECK(manager.dispatchAndWait("A", Event()).delivered == 1);
	CHECK(manager.setEnabled(id, false));
	CHECK(manager.setEnabled(id, true));
	CHECK(lab.stored(id, "A", "count")->asInt() == 3);
	const auto restored = manager.importSettings(id, exported.value());
	CHECK(restored.ok() && restored.value() == 1);
	CHECK(lab.stored(id, "A", "count")->asInt() == 2);
	CHECK(manager.runningCount() == 0);

	const auto code = [&](std::span<const uint8_t> data) {
		const auto result = manager.importSettings(id, data);
		return result.ok() ? std::string() : result.error();
	};
	CHECK(code(cbor::Bytes{ 1, 2, 3 }) == "invalid");
	const auto edited = [&](const char *path, cbor::Value value) {
		auto copy = document.value();
		Set(copy, path, std::move(value));
		auto bytes = cbor::Bytes();
		CHECK(cbor::Encode(copy, bytes, 20));
		return bytes;
	};
	CHECK(code(edited("plugin", Text("com.example.other"))) == "wrong_plugin");
	CHECK(code(edited("storageSchema", Int(3))) == "newer");
	CHECK(code(edited("format", Text("something.else"))) == "invalid");
	CHECK(code(edited("items", cbor::Value::FromMap({ { "elsewhere", Int(1) } }))) == "invalid");
	CHECK(code(edited("items", cbor::Value::FromMap({ { "count.secret", Int(1) } }))) == "invalid");
	CHECK(code(edited("items", cbor::Value::FromMap({ { "count", Text(std::string(size_t(11) << 20, 'x')) } }))) == "invalid");
	CHECK(code(edited("items", cbor::Value::FromMap({ { "count", Int(9) } }))) == "");
	CHECK(lab.stored(id, "A", "count")->asInt() == 9);
	CHECK(code(edited("storageSchema", Int(1))) == "");

	CHECK(manager.install(build("1.1.0", 2, false)).ok());
	CHECK(!manager.exportSettings(id).ok());
	CHECK(code(exported.value()) == "unavailable");
	CHECK(!manager.exportSettings("com.example.none").ok());
#endif
}

void TestToastEvents() {
	auto lab = Lab("fgpl-manager-toast");
	auto manager = PluginManager(lab.config());
	const auto id = std::string("com.example.toaster");
	CHECK(manager.install(CustomArchive(id, CallingModule(EncodedRequest(
		"ui.toast",
		{
			{ "text", Text("Deleted") },
			{ "action", cbor::Value::FromMap({ { "id", Text("undo") }, { "label", Text("Undo") } }) },
		})), {
		{ "messages.read", {} },
		{ "ui.toast", {} },
	})).ok());
	CHECK(manager.setEnabled(id, true));
	CHECK(manager.grant(id, "A", { { "messages.read", "ui.toast" }, std::nullopt }));
	const auto click = [&](const char *type, const char *action) {
		return manager.send(id, "A", cbor::Value::FromMap({
			{ "type", Text(type) }, { "id", Text(action) } }));
	};
	CHECK(!click("toast.click", "undo"));
	CHECK(manager.dispatchAndWait("A", Event()).delivered == 1);
	CHECK(!click("toast.click", "other"));
	CHECK(click("toast.click", "undo"));
	CHECK(click("toast.close", "undo"));
	CHECK(!click("toast.click", "undo"));
	CHECK(!click("toast.close", "undo"));
}

void TestTranslator() {
	auto lab = Lab("fgpl-manager-translator");
	auto manager = PluginManager(lab.config());
	const auto permissions = std::vector<std::pair<std::string, std::vector<std::string>>>{
		{ "translate.provide", {} },
	};
	const auto translator = [](cbor::Value &document) {
		Remove(document, "contributes.events");
		Set(document, "contributes.translator", cbor::Value::FromMap({
			{ "name", Text("Shout") },
			{ "languages", Array({ "en", "ru" }) },
		}));
	};
	const auto good = std::string("com.example.shout");
	const auto crashing = std::string("com.example.crasher");
	const auto bystander = std::string("com.example.bystander");
	CHECK(manager.install(CustomArchive(good, CallingModule(EncodedRequest(
		"translate.reply",
		{ { "ticket", Int(1) }, { "items", Array({ "X" }) } })), permissions, translator)).ok());
	CHECK(manager.install(CustomArchive(crashing, CrashOnEvent(), permissions, translator)).ok());
	CHECK(manager.install(CustomArchive(bystander, CallingModule(EncodedRequest(
		"translate.reply",
		{ { "ticket", Int(2) }, { "items", Array({ "STOLEN" }) } })), permissions, translator)).ok());
	for (const auto &id : { good, crashing, bystander }) {
		CHECK(manager.setEnabled(id, true));
	}
	CHECK(manager.grant(good, "A", { { "translate.provide" }, std::nullopt }));
	CHECK(manager.grant(crashing, "A", { { "translate.provide" }, std::nullopt }));

	const auto listed = manager.snapshot().contributions("A", "en");
	auto names = std::vector<std::string>();
	for (const auto &entry : listed) {
		if (entry.translator.declared) {
			names.push_back(entry.translator.name + ":" + std::to_string(entry.translator.languages.size()));
		}
	}
	CHECK(names.size() == 2 && names[0] == "Shout:2");

	const auto ask = [&](const std::string &plugin, const char *account = "A") {
		auto result = std::optional<TranslateResult>();
		auto request = TranslateRequest();
		request.to = "ru";
		request.items = { "hi" };
		manager.translateAsync(account, plugin, request, 5000, [&](TranslateResult value) {
			result = std::move(value);
		});
		for (auto waited = 0; !result && waited < 20000; waited += 5) {
			manager.pump();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return result;
	};
	const auto answered = ask(good);
	CHECK(answered && answered->error.empty() && answered->items == std::vector<std::string>{ "X" });
	const auto refused = ask(bystander);
	CHECK(refused && !refused->error.empty());
	const auto failed = ask(crashing);
	CHECK(failed && !failed->error.empty());
	const auto missing = ask("com.example.none");
	CHECK(missing && missing->error == "unavailable");
	const auto otherAccount = ask(good, "bad account!");
	CHECK(otherAccount && otherAccount->error == "unavailable");

	auto empty = std::optional<TranslateResult>();
	manager.translateAsync("A", good, TranslateRequest{ "ru", {} }, 1000, [&](TranslateResult value) {
		empty = std::move(value);
	});
	CHECK(empty && empty->error == "invalid");
}

void TestPluginsTalkThroughTheApp() {
#ifdef FGPL_COUNTER_WASM
	auto lab = Lab("fgpl-manager-messages");
	auto manager = PluginManager(lab.config());
	const auto sender = std::string("com.example.sender");
	const auto receiver = std::string("com.example.receiver");
	const auto request = std::string(
		"\xa2\x62op\x6cplugins.send\x61" "a\xa2\x62to\x74" "com.example.receiver"
		"\x64" "data\x62hi", 52);
	CHECK(manager.install(CustomArchive(sender, CallingModule(request), {
		{ "messages.read", {} },
		{ "plugins.message", {} },
	})).ok());
	const auto key = Seed(1);
	auto document = Document(receiver, "1.0.0", {
		{ "plugins.message", {} },
		{ "storage.private", {} },
	});
	Remove(document, "contributes.events");
	Set(document, "contributes.messages.from", Array({ sender }));
	CHECK(manager.install(Build(document, { { "plugin.wasm", ReadFile(FGPL_COUNTER_WASM) } }, &key).archive).ok());
	for (const auto &id : { sender, receiver }) {
		CHECK(manager.setEnabled(id, true));
	}
	CHECK(manager.grant(sender, "A", { { "messages.read", "plugins.message" }, std::nullopt }));
	CHECK(manager.grant(receiver, "A", { { "plugins.message", "storage.private" }, std::nullopt }));
	CHECK(manager.dispatchAndWait("A", Event()).delivered == 2);
	CHECK(manager.drain(20000));
	CHECK(lab.stored(receiver, "A", "count") && lab.stored(receiver, "A", "count")->asInt() == 1);

	CHECK(manager.grant(receiver, "A", { { "storage.private" }, std::nullopt }));
	CHECK(manager.dispatchAndWait("A", Event()).delivered == 1);
	CHECK(manager.drain(20000));
	CHECK(lab.stored(receiver, "A", "count")->asInt() == 1);
	CHECK(manager.dispatchAndWait("B", Event()).delivered == 1);
#endif
}

void TestOldPerAccountStateIsMerged() {
#ifdef FGPL_COUNTER_WASM
	auto lab = Lab("fgpl-manager-merge");
	const auto id = std::string("com.example.merged");
	{
		auto manager = PluginManager(lab.config());
		CHECK(manager.install(CounterArchive(id, "1.0.0")).ok());
		Prepare(manager, id, "A");
		manager.shutdown();
	}
	const auto path = lab.root / "plugins" / "state.json";
	auto text = std::string();
	{
		auto stream = std::ifstream(path);
		text.assign(std::istreambuf_iterator<char>(stream), {});
	}
	auto document = json::Parse(text).value();
	std::erase_if(*document.asMap(), [](const cbor::MapEntry &entry) {
		return entry.key == "mac";
	});
	for (auto &plugin : *document.asMap()->front().value.asMap()) {
		for (auto &field : *plugin.value.asMap()) {
			if (field.key == "grants") {
				field.value = cbor::Value::FromMap({
					{ "A", cbor::Value::FromMap({ { "granted", Array({ "messages.read", "storage.private" }) } }) },
					{ "B", cbor::Value::FromMap({ { "granted", Array({ "messages.read" }) } }) },
				});
			}
		}
	}
	const auto canonical = json::Canonical(document);
	const auto mac = HmacSha256(lab.key, { reinterpret_cast<const uint8_t *>(canonical.data()), canonical.size() });
	document.asMap()->push_back({ "mac", Text(Base64Encode(mac)) });
	{
		auto stream = std::ofstream(path, std::ios::trunc);
		stream << json::Canonical(document);
	}
	{
		auto old = FileStorage(lab.root / "plugins" / id / "data" / "A" / "store.cbor");
		auto bytes = cbor::Bytes();
		CHECK(cbor::Encode(Int(7), bytes, 4));
		old.set("count", bytes);
		CHECK(old.flush());
	}
	auto manager = PluginManager(lab.config());
	CHECK(!manager.stateWasTampered());
	const auto merged = manager.grants(id, "C");
	CHECK(merged && merged->granted == (std::set<std::string>{ "messages.read" }));
	CHECK(manager.grant(id, "C", Everything()));
	CHECK(manager.dispatchAndWait("C", Event()).delivered == 1);
	CHECK(lab.stored(id, "C", "count") && lab.stored(id, "C", "count")->asInt() == 8);
#endif
}

void TestConflictsBothWays() {
	auto lab = Lab("fgpl-manager-conflicts");
	auto manager = PluginManager(lab.config());
	CHECK(manager.install(CustomArchive("com.example.first", CrashOnEvent(), {
		{ "messages.read", {} },
	}, [](cbor::Value &document) {
		Set(document, "conflicts", cbor::Value::FromArray({
			cbor::Value::FromMap({ { "id", Text("com.example.second") } }),
		}));
	})).ok());
	const auto second = manager.install(CustomArchive("com.example.second", CrashOnEvent(), {
		{ "messages.read", {} },
	}));
	CHECK(!second.ok() && second.error().error == InstallError::Conflict);
}


[[nodiscard]] cbor::Value Said(const char *text) {
	return cbor::Value::FromMap({
		{ "type", Text("message.new") },
		{ "chat", Int(1) },
		{ "text", Text(text) },
	});
}

[[nodiscard]] cbor::Bytes AsyncPythonArchive(const std::string &id) {
	auto document = Document(id, "1.0.0", {
		{ "messages.read", {} },
		{ "storage.private", {} },
		{ "net.https", {} },
	});
	Set(document, "runtime.kind", Text("python"));
	Remove(document, "runtime.module");
	Set(document, "runtime.entry", Text("main.py"));
	const auto key = Seed(4);
	return Build(document, {
		{ "main.py", ReadFile(fs::path(FGPL_PYTHON_PLUGIN_DIR) / "async_plugin.py") },
	}, &key).archive;
}

void TestAwaitedCallsDoNotBlockThePlugin() {
	if (!pysupport::Installed()) {
		return;
	}
	auto lab = Lab("fgpl-manager-async");
	auto transport = fakes::FakeTransport();
	transport.delayMs = 800;
	lab.services.transport = &transport;
	auto manager = PluginManager(lab.config());
	const auto id = std::string("com.example.waiter");
	CHECK(manager.install(AsyncPythonArchive(id)).ok());
	Prepare(manager, id);
	CHECK(manager.dispatch("A", Said("slow")).queued == 1);
	CHECK(manager.dispatch("A", Said("fast")).queued == 1);
	CHECK(manager.drain(60000));
	CHECK(lab.stored(id, "A", "slow") && lab.stored(id, "A", "slow")->asInt() == 200);
	CHECK(lab.stored(id, "A", "fast") && lab.stored(id, "A", "fast")->asInt() == 1);
	auto order = std::vector<std::string>();
	for (const auto &line : manager.log(id)) {
		if (line == "fast" || line == "slow done") {
			order.push_back(line);
		}
	}
	CHECK(order == (std::vector<std::string>{ "fast", "slow done" }));
	CHECK(manager.list()[0].stats.failures == 0);
	manager.shutdown();
}

void TestWaitingForTheAppIsNotTheEventsTime() {
	if (!pysupport::Installed()) {
		return;
	}
	auto lab = Lab("fgpl-manager-hosttime");
	auto transport = fakes::FakeTransport();
	transport.delayMs = 4500;
	lab.services.transport = &transport;
	auto manager = PluginManager(lab.config());
	const auto id = std::string("com.example.patient");
	CHECK(manager.install(AsyncPythonArchive(id)).ok());
	Prepare(manager, id);
	const auto report = manager.dispatchAndWait("A", Said("sync"));
	CHECK(report.delivered == 1 && report.failed == 0);
	CHECK(lab.stored(id, "A", "sync") && lab.stored(id, "A", "sync")->asInt() == 200);
	manager.shutdown();
}

void TestStopDoesNotWaitForTheNetwork() {
	if (!pysupport::Installed()) {
		return;
	}
	auto lab = Lab("fgpl-manager-abort");
	auto transport = fakes::FakeTransport();
	transport.holdUntilAbort = true;
	lab.services.transport = &transport;
	auto manager = PluginManager(lab.config());
	const auto id = std::string("com.example.stuck");
	CHECK(manager.install(AsyncPythonArchive(id)).ok());
	Prepare(manager, id);
	CHECK(manager.dispatch("A", Said("sync")).queued == 1);
	for (auto waited = 0; waited < 60000 && transport.fetching.load() == 0; waited += 20) {
		manager.pump();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	CHECK(transport.fetching.load() == 1);
	const auto started = std::chrono::steady_clock::now();
	CHECK(manager.setEnabled(id, false));
	CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
	CHECK(manager.runningCount() == 0);
	{
		const auto lock = std::scoped_lock(transport.mutex);
		CHECK(transport.aborted.size() == 1 && transport.aborted[0].plugin == id);
	}
}

void TestOutgoingAnswersWithoutWaiting() {
	auto lab = Lab("fgpl-manager-outgoing-async");
	auto manager = PluginManager(lab.config());
	const auto id = std::string("com.example.shout");
	CHECK(manager.install(CustomArchive(id, CallingModule(OutgoingRequest()), {
		{ "messages.read", {} },
		{ "messages.transform_outgoing", {} },
	}, [](cbor::Value &document) {
		Set(document, "contributes.outgoing.onFailure", Text("block"));
	})).ok());
	CHECK(manager.setEnabled(id, true));
	CHECK(manager.grant(id, "A", { { "messages.read", "messages.transform_outgoing" }, std::nullopt }));
	const auto early = manager.transformOutgoing("A", 1, false, "hi", 5000);
	CHECK(early.blocked && early.plugin == id);
	CHECK(manager.drain(20000));
	CHECK(manager.snapshot().transformsOutgoing(1, false));
	CHECK(!manager.snapshot().transformsOutgoing(1, true));

	auto answers = std::vector<OutgoingResult>();
	manager.transformOutgoingAsync("A", 1, false, "a", 5000, [&](OutgoingResult result) {
		answers.push_back(std::move(result));
	});
	manager.transformOutgoingAsync("A", 3, true, "b", 5000, [&](OutgoingResult result) {
		answers.push_back(std::move(result));
	});
	CHECK(answers.size() == 1 && answers[0].text == "b");
	for (auto waited = 0; waited < 10000 && answers.size() < 2; waited += 10) {
		manager.pump();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	CHECK(answers.size() == 2 && !answers[1].blocked && answers[1].text == "HELLO");
	manager.shutdown();
}

void TestSnapshotMatchesTheManager() {
#ifdef FGPL_COUNTER_WASM
	auto lab = Lab("fgpl-manager-snapshot");
	auto manager = PluginManager(lab.config());
	const auto id = std::string("com.example.counter");
	CHECK(manager.install(CounterArchive(id, "1.0.0")).ok());
	Prepare(manager, id);
	CHECK(manager.dispatchAndWait("A", Event()).delivered == 1);
	const auto snapshot = manager.snapshot();
	CHECK(snapshot.plugins.size() == 1 && snapshot.plugin(id) && snapshot.plugin(id)->enabled);
	CHECK(snapshot.manifest(id) && snapshot.manifest(id)->id == id);
	CHECK(snapshot.grantsOf(id) && snapshot.grantsOf(id)->granted == manager.grants(id, "A")->granted);
	CHECK(snapshot.log(id) == manager.log(id));
	CHECK(snapshot.wantsEvents == manager.wantsEvents());
	const auto path = lab.root / "plugins" / id / "1.0.0" / "package.fgpl";
	fs::remove(path);
	CHECK(manager.list().size() == 1 && !manager.list()[0].requests.empty());
	manager.shutdown();
#endif
}


void TestPermissionsReachEveryAccount() {
	auto lab = Lab("fgpl-manager-everywhere");
	auto manager = PluginManager(lab.config());
	const auto id = std::string("com.example.resident");
	CHECK(manager.install(CustomArchive(id, CallingModule(OutgoingRequest()), {
		{ "messages.read", {} },
		{ "messages.transform_outgoing", {} },
	}, [](cbor::Value &document) {
		Set(document, "contributes.outgoing.onFailure", Text("pass"));
	})).ok());
	CHECK(manager.setEnabled(id, true));
	CHECK(manager.grant(id, "A", { { "messages.read", "messages.transform_outgoing" }, std::nullopt }));
	CHECK(manager.startEnabled("A").started == 1);
	CHECK(manager.startEnabled("B").started == 1);
	CHECK(manager.drain(20000));
	CHECK(manager.grant(id, "A", { { "messages.transform_outgoing" }, std::nullopt }));
	CHECK(manager.runningCount() == 2);
	manager.onLock();
	CHECK(manager.runningCount() == 0);
	manager.onUnlock();
	CHECK(manager.runningCount() == 2);
	manager.onLogout("B");
	CHECK(manager.runningCount() == 1);
	CHECK(manager.setEnabled(id, false));
	CHECK(manager.runningCount() == 0);
	CHECK(manager.setEnabled(id, true));
	CHECK(manager.runningCount() == 1);
	manager.shutdown();
}

void TestEventsAreFilteredBeforeStart() {
#ifdef FGPL_COUNTER_WASM
	auto lab = Lab("fgpl-manager-prefilter");
	auto manager = PluginManager(lab.config());
	const auto id = std::string("com.example.counter");
	CHECK(manager.install(CounterArchive(id, "1.0.0")).ok());
	CHECK(manager.setEnabled(id, true));
	CHECK(manager.grant(id, "A", { { "messages.read", "storage.private" }, std::set<int64_t>{ 1 } }));
	auto secret = Event(1);
	secret.asMap()->push_back({ "secret", cbor::Value::FromBool(true) });
	CHECK(manager.dispatch("A", secret).queued == 0);
	auto saved = Event(1);
	saved.asMap()->push_back({ "self", cbor::Value::FromBool(true) });
	CHECK(manager.dispatch("A", saved).queued == 0);
	CHECK(manager.dispatch("A", Event(2)).queued == 0);
	CHECK(manager.dispatch("A", Event(api::kServiceChat)).queued == 0);
	CHECK(manager.runningCount() == 0);
	CHECK(manager.dispatch("A", Event(1)).queued == 1);
	CHECK(manager.runningCount() == 1);
	manager.shutdown();
#endif
}

void TestFailedHandlerBlocksOutgoing() {
	auto lab = Lab("fgpl-manager-status");
	auto manager = PluginManager(lab.config());
	const auto id = std::string("com.example.grumpy");
	CHECK(manager.install(CustomArchive(id, wat::Module("", "(i32.const 7)"), {
		{ "messages.read", {} },
		{ "messages.transform_outgoing", {} },
	}, [](cbor::Value &document) {
		Set(document, "contributes.outgoing.onFailure", Text("block"));
	})).ok());
	CHECK(manager.setEnabled(id, true));
	CHECK(manager.grant(id, "A", { { "messages.transform_outgoing" }, std::nullopt }));
	CHECK(manager.startEnabled("A").started == 1);
	CHECK(manager.drain(20000));
	const auto result = manager.transformOutgoing("A", 1, false, "hi", 5000);
	CHECK(result.blocked && result.reason == "the plugin failed");
	manager.shutdown();
}

[[nodiscard]] std::string TimerRequest() {
	return std::string("\xa2\x62op\x6atimers.set\x61" "a\xa2\x62id\x61t\x65" "delay\x19\x03\xe8", 32);
}

void TestTimersWakeThePlugin() {
	auto lab = Lab("fgpl-manager-timers");
	auto manager = PluginManager(lab.config());
	const auto id = std::string("com.example.ticker");
	CHECK(manager.install(CustomArchive(id, CallingModule(TimerRequest()), {
		{ "messages.read", {} },
	})).ok());
	CHECK(manager.setEnabled(id, true));
	CHECK(manager.grant(id, "A", { { "messages.read" }, std::nullopt }));
	CHECK(manager.startEnabled("A").started == 0);
	CHECK(manager.dispatchAndWait("A", Event()).delivered == 1);
	manager.onLock();
	manager.onUnlock();
	CHECK(manager.runningCount() == 0);
	lab.time += 1000;
	manager.pump();
	CHECK(manager.runningCount() == 1);
	CHECK(manager.drain(20000));
	CHECK(manager.list()[0].stats.events == 2);
	CHECK(manager.setEnabled(id, false));
	CHECK(manager.setEnabled(id, true));
	lab.time += 5000;
	manager.pump();
	CHECK(manager.runningCount() == 0);
	manager.shutdown();
}

void TestManualStartOfLazyPlugin() {
	auto lab = Lab("fgpl-manager-manual");
	auto manager = PluginManager(lab.config());
	const auto id = std::string("com.example.lazy");
	CHECK(manager.install(CustomArchive(id, wat::Module("", "(i32.const 0)"), {
		{ "messages.read", {} },
	})).ok());
	CHECK(manager.setEnabled(id, true));
	CHECK(manager.grant(id, "A", { { "messages.read" }, std::nullopt }));
	CHECK(manager.startEnabled("A").started == 0);
	CHECK(manager.list()[0].onDemand);
	CHECK(manager.list()[0].status == PluginStatus::Stopped);
	CHECK(manager.start(id));
	CHECK(manager.runningCount() == 1);
	CHECK(manager.list()[0].status == PluginStatus::Running);
	CHECK(manager.setEnabled(id, false));
	CHECK(!manager.start(id));
	manager.shutdown();
}

void TestCustomIconsReachTheSnapshot() {
	auto lab = Lab("fgpl-manager-icons");
	auto manager = PluginManager(lab.config());
	const auto id = std::string("com.example.icons");
	const auto key = Seed(1);
	auto document = Document(id, "1.0.0", { { "messages.read", {} }, { "ui.menu", {} } });
	Set(document, "contributes.icons", cbor::Value::FromMap({ { "mark", Text("mark.svg") } }));
	Set(document, "contributes.menus", cbor::Value::FromArray({
		cbor::Value::FromMap({ { "surface", Text("message") }, { "id", Text("go") }, { "title", Text("Go") }, { "icon", Text("mark") } }),
	}));
	const auto svg = std::string("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\"/>");
	const auto installed = manager.install(Build(document, {
		{ "plugin.wasm", wat::Wasm(wat::Module("", "(i32.const 0)")) },
		{ "mark.svg", cbor::Bytes(svg.begin(), svg.end()) },
	}, &key).archive);
	CHECK(installed.ok());
	if (!installed.ok()) {
		for (const auto &line : installed.error().details) {
			std::fprintf(stderr, "    %s\n", line.c_str());
		}
	}
	CHECK(manager.setEnabled(id, true));
	CHECK(manager.grant(id, "A", { { "messages.read", "ui.menu" }, std::nullopt }));
	const auto snapshot = manager.snapshot();
	const auto icon = snapshot.customIcon(id, "mark");
	CHECK(icon && icon->size() == svg.size());
	CHECK(!snapshot.customIcon(id, "other"));
	const auto contributions = snapshot.contributions("A", "en");
	CHECK(contributions.size() == 1
		&& contributions[0].menus.size() == 1
		&& contributions[0].menus[0].icon == "mark");
	manager.shutdown();
}

int main(int argc, char **argv) {
	const auto only = (argc > 1) ? std::string(argv[1]) : std::string();
	const auto run = [&](const char *name, void (*test)()) {
		if (only.empty() || only == name) {
			test();
		}
	};
	run("life", TestLifecycleAndPersistence);
	run("policy", TestInstallPolicy);
	run("update", TestUpdatesAndRollback);
	run("crash", TestCrashLoopDisablesThePlugin);
	run("lock", TestLockHiddenAndLogout);
	run("recovery", TestStorageRecoveryIsLogged);
	run("damage", TestUninstallAndDamage);
	run("python", TestPythonPlugin);
	run("ui", TestUiContributions);
	run("order", TestDispatchDoesNotBlockAndKeepsOrder);
	run("fair", TestSlowPluginDoesNotDelayOthers);
	run("overflow", TestQueueOverflowDropsTheOldest);
	run("busylock", TestLockIsPromptWhileBusy);
	run("timers", TestTimersWakeThePlugin);
	run("manual", TestManualStartOfLazyPlugin);
	run("icons", TestCustomIconsReachTheSnapshot);
	run("stress", TestManyPluginsAndAccounts);
	run("cancel", TestStopWhileStartingIsPrompt);
	run("unload", TestSelfUnloadAndLazyStart);
	run("outgoing", TestOutgoingTransform);
	run("outgoing-options", TestOutgoingFilterDropAndOptions);
	run("outgoing-entities", TestOutgoingEntities);
	run("translator", TestTranslator);
	run("toast-events", TestToastEvents);
	run("settings-export", TestSettingsExport);
	run("conflicts", TestConflictsBothWays);
	run("messages", TestPluginsTalkThroughTheApp);
	run("merge", TestOldPerAccountStateIsMerged);
	run("async", TestAwaitedCallsDoNotBlockThePlugin);
	run("hosttime", TestWaitingForTheAppIsNotTheEventsTime);
	run("abort", TestStopDoesNotWaitForTheNetwork);
	run("outgoingasync", TestOutgoingAnswersWithoutWaiting);
	run("snapshot", TestSnapshotMatchesTheManager);
	run("everywhere", TestPermissionsReachEveryAccount);
	run("prefilter", TestEventsAreFilteredBeforeStart);
	run("status", TestFailedHandlerBlocksOutgoing);
	return Failures() ? 1 : 0;
}
