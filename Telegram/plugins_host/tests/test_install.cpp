#include "check.h"
#include "fakes.h"
#include "fgpl/host_client.h"
#include "package_builder.h"
#include "fgpl/permissions.h"
#include "python_support.h"

#include <fstream>
#include <iterator>

namespace {

using namespace fgpl;
using namespace pkg;

[[nodiscard]] cbor::Bytes ReadFile(const std::string &path) {
	auto stream = std::ifstream(path, std::ios::binary);
	return { std::istreambuf_iterator<char>(stream), {} };
}

[[nodiscard]] cbor::Value Event() {
	return cbor::Value::FromMap({
		{ "type", Text("message.new") },
		{ "chat", Int(1) },
		{ "text", Text("hi") },
	});
}

void TestPermissions() {
	const auto manifest = ParseManifest(Build(ValidDocument(), {
		{ "plugin.wasm", ReactorWasm() },
	}).document).ok()
		? ParseManifest(Build(ValidDocument(), { { "plugin.wasm", ReactorWasm() } }).document).value()
		: Manifest();
	CHECK(manifest.id == "com.example.demo");

	auto decision = GrantDecision();
	decision.granted = { "messages.read", "net.https", "calls.nothing" };
	const auto grants = MakeGrants(manifest, decision);
	CHECK(grants.permissions == (std::set<std::string>{ "messages.read", "net.https" }));
	CHECK(grants.storageQuotaBytes == (uint64_t(10) << 20));
	CHECK(MakeGrants(manifest, {}).permissions.empty());

	const auto notes = RiskNotes(grants.permissions);
	CHECK(notes.size() == 2 && notes[0].risk == Risk::ExfiltrationPossible);
	CHECK(RiskNotes({ "net.https" }).size() == 1);
	CHECK(RiskNotes({ "net.udp" }).size() == 2);
	CHECK(RiskNotes({ "messages.read" }).empty());
	CHECK(RiskNotes({ "messages.read_deleted", "net.tcp", "messages.send", "background" }).size() == 6);

	auto next = manifest;
	next.permissions.push_back({ "net.tcp", {}, "", false });
	next.permissions[0].scope = "all";
	const auto added = NewRequests(manifest, next);
	CHECK(added.size() == 2);
	CHECK(NewRequests(manifest, manifest).empty());

	CHECK(PermissionDanger("ui.toast") == Danger::Low);
	CHECK(PermissionDanger("storage.private") == Danger::Low);
	CHECK(PermissionDanger("net.https") == Danger::Medium);
	CHECK(PermissionDanger("messages.read") == Danger::Medium);
	CHECK(PermissionDanger("messages.read", "all") == Danger::High);
	CHECK(PermissionDanger("net.udp") == Danger::High);
	CHECK(PermissionDanger("messages.send") == Danger::High);
	for (const auto &id : KnownPermissions()) {
		CHECK(PermissionDanger(id) <= Danger::High);
	}
}

void TestInstalledPackageRunsInTheSandbox() {
#ifdef FGPL_COUNTER_WASM
	auto document = ValidDocument();
	Remove(document, "permissions");
	Set(document, "permissions.required", cbor::Value::FromArray({
		cbor::Value::FromMap({ { "id", Text("messages.read") }, { "reason", Text("Counts messages.") } }),
		cbor::Value::FromMap({ { "id", Text("storage.private") }, { "reason", Text("Keeps the count.") } }),
	}));
	const auto seed = Seed(3);
	const auto built = Build(document, { { "plugin.wasm", ReadFile(FGPL_COUNTER_WASM) } }, &seed);
	const auto opened = OpenPackage(built.archive);
	CHECK(opened.ok());
	if (!opened.ok()) {
		return;
	}
	const auto &package = opened.value();
	auto decision = GrantDecision();
	decision.granted = { "messages.read", "storage.private" };
	auto storage = MemoryStorage();
	auto data = fakes::FakeData();
	auto services = BrokerServices();
	services.storage = &storage;
	services.data = &data;
	auto broker = Broker(MakeGrants(package.manifest, decision), services);

	auto config = HostConfig();
	config.path = FGPL_HOST_BINARY;
	auto started = HostClient::Start(config);
	CHECK(started.ok());
	if (!started.ok()) {
		return;
	}
	auto request = LoadRequest();
	request.wasm = package.files.at(package.manifest.module);
	request.files = PluginFiles(package);
	request.limits.memoryBytes = uint64_t(package.manifest.memoryMb) << 20;
	request.limits.callTimeoutMs = uint32_t(package.manifest.eventTimeoutMs);
	CHECK(started.value()->load(request).ok());
	CHECK(started.value()->init(cbor::Value::FromMap({}), broker).ok());
	for (auto i = 1; i <= 3; ++i) {
		const auto event = broker.filterEvent(Event());
		CHECK(event.has_value());
		const auto result = started.value()->deliver(*event, broker);
		CHECK(result.ok() && result.value() == i);
	}
	const auto count = storage.get("count");
	CHECK(count.has_value());

	auto denied = Broker(MakeGrants(package.manifest, {}), services);
	CHECK(!denied.filterEvent(Event()).has_value());
#endif
}

} // namespace

int main() {
	TestPermissions();
	TestInstalledPackageRunsInTheSandbox();
	return Failures() ? 1 : 0;
}
