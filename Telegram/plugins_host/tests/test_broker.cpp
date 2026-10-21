#include "broker.h"
#include "check.h"
#include "fakes.h"
#include "net_policy.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace fgpl;
using fakes::FakeData;
using fakes::FakeTransport;

[[nodiscard]] cbor::Value Text(std::string_view text) {
	return cbor::Value::FromText(std::string(text));
}

[[nodiscard]] cbor::Value Int(int64_t number) {
	return cbor::Value::FromInt(number);
}

[[nodiscard]] cbor::Value Request(
		std::string_view op,
		cbor::Map args = {}) {
	return cbor::Value::FromMap({
		{ "op", Text(op) },
		{ "a", cbor::Value::FromMap(std::move(args)) },
	});
}

[[nodiscard]] bool IsOk(const cbor::Value &response) {
	const auto ok = response.find("ok");
	return ok && ok->asBool().value_or(false);
}

[[nodiscard]] std::string Code(const cbor::Value &response) {
	const auto code = response.find("e");
	return (code && code->asText()) ? *code->asText() : std::string();
}

[[nodiscard]] const cbor::Value &Payload(const cbor::Value &response) {
	static const auto empty = cbor::Value();
	const auto value = response.find("v");
	return value ? *value : empty;
}

struct Fixture {
	explicit Fixture(std::set<std::string> permissions) {
		grants.pluginId = "com.example.test";
		grants.permissions = std::move(permissions);
		grants.hosts = { "api.example.com", "*.cdn.example.com:8443" };
		time = 0;
		services.storage = &storage;
		services.data = &data;
		services.transport = &transport;
		services.clockMs = [this] { return time; };
		services.log = [this](std::string_view line) {
			logs.emplace_back(line);
		};
		services.audit = [this](std::string_view op, std::string_view outcome) {
			audit.emplace_back(std::string(op) + ":" + std::string(outcome));
		};
	}

	[[nodiscard]] Broker make() const {
		return Broker(grants, services);
	}

	Grants grants;
	BrokerServices services;
	MemoryStorage storage;
	FakeData data;
	FakeTransport transport;
	int64_t time = 0;
	std::vector<std::string> logs;
	std::vector<std::string> audit;

};

void TestUrlParsing() {
	const auto good = net::ParseUrl("HTTPS://Api.Example.com:8443/a/b?q=1#frag");
	CHECK(good.ok());
	if (good.ok()) {
		CHECK(good.value().host == "api.example.com");
		CHECK(good.value().port == 8443);
		CHECK(good.value().target == "/a/b?q=1");
		CHECK(good.value().text == "https://api.example.com:8443/a/b?q=1");
	}
	CHECK(net::ParseUrl("https://example.com").value().target == "/");
	CHECK(net::ParseUrl("https://example.com?x=1").value().target == "/?x=1");
	CHECK(net::ParseUrl("https://example.com./").value().host == "example.com");
	for (const auto bad : {
		"http://example.com/", "ftp://example.com/", "example.com",
		"https://", "https:///x", "https://user:pw@example.com/",
		"https://example.com:0/", "https://example.com:70000/",
		"https://example.com:abc/", "https://127.0.0.1/", "https://8.8.8.8/",
		"https://[::1]/", "https://localhost/", "https://a.localhost/",
		"https://printer.local/", "https://x.internal/",
		"https://exa mple.com/", "https://exa\tmple.com/",
		"https://-.com/..", "https://a..b/", "https://.a/",
		"https://\xC3\xA9xample.com/",
	}) {
		if (net::ParseUrl(bad).ok()) {
			std::fprintf(stderr, "accepted: %s\n", bad);
			CHECK(false);
		}
	}
	CHECK(!net::ParseUrl(std::string("https://example.com/") + std::string(3000, 'a')).ok());
}

void TestHostMatching() {
	const auto parse = [](const char *text) {
		return net::ParseUrl(text).value();
	};
	const auto patterns = std::vector<std::string>{
		"api.example.com", "*.cdn.example.com:8443", "other.org:444",
	};
	CHECK(net::HostAllowed(parse("https://api.example.com/x"), patterns));
	CHECK(net::HostAllowed(parse("https://API.example.com/x"), patterns));
	CHECK(!net::HostAllowed(parse("https://api.example.com:444/x"), patterns));
	CHECK(!net::HostAllowed(parse("https://sub.api.example.com/x"), patterns));
	CHECK(!net::HostAllowed(parse("https://example.com/x"), patterns));
	CHECK(net::HostAllowed(parse("https://a.cdn.example.com:8443/x"), patterns));
	CHECK(net::HostAllowed(parse("https://a.b.cdn.example.com:8443/x"), patterns));
	CHECK(!net::HostAllowed(parse("https://cdn.example.com:8443/x"), patterns));
	CHECK(!net::HostAllowed(parse("https://a.cdn.example.com/x"), patterns));
	CHECK(!net::HostAllowed(parse("https://evilcdn.example.com:8443/x"), patterns));
	CHECK(net::HostAllowed(parse("https://other.org:444/x"), patterns));
	CHECK(!net::HostAllowed(parse("https://other.org/x"), patterns));
	CHECK(!net::HostAllowed(parse("https://api.example.com/x"), {}));
}

void TestPublicAddresses() {
	for (const auto good : {
		"8.8.8.8", "1.1.1.1", "93.184.216.34", "172.15.0.1", "172.32.0.1",
		"100.63.0.1", "100.128.0.1", "2606:4700:4700::1111",
		"::ffff:8.8.8.8", "2a00:1450:4001::1",
	}) {
		if (!net::IsPublicAddress(good)) {
			std::fprintf(stderr, "should be public: %s\n", good);
			CHECK(false);
		}
	}
	for (const auto bad : {
		"0.0.0.0", "10.0.0.1", "10.255.255.255", "127.0.0.1", "127.1.2.3",
		"169.254.169.254", "172.16.0.1", "172.31.255.255", "192.168.0.1",
		"100.64.0.1", "100.127.255.255", "224.0.0.1", "255.255.255.255",
		"198.18.0.1", "192.0.2.1", "198.51.100.1", "203.0.113.1",
		"::", "::1", "fe80::1", "fc00::1", "fd12:3456::1", "ff02::1",
		"::ffff:10.0.0.1", "::ffff:127.0.0.1", "2001:db8::1",
		"not an address", "", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1::2::3",
		"12345::1", "1:2:3:4:5:6:7:8:9", "::ffff:1.2.3",
	}) {
		if (net::IsPublicAddress(bad)) {
			std::fprintf(stderr, "should not be public: %s\n", bad);
			CHECK(false);
		}
	}
}

void TestHeaderRules() {
	for (const auto name : {
		"Host", "content-length", "Transfer-Encoding", "Cookie", "Connection",
		"Upgrade", "Proxy-Authorization", "Proxy-Foo", "Sec-Fetch-Mode",
		"X-Forwarded-For", "TE", "Expect",
	}) {
		CHECK(net::IsForbiddenRequestHeader(name));
	}
	for (const auto name : { "Authorization", "Accept", "X-Api-Key", "User-Agent" }) {
		CHECK(!net::IsForbiddenRequestHeader(name));
	}
	CHECK(net::IsValidHeaderName("X-Api-Key"));
	CHECK(!net::IsValidHeaderName("Bad Name"));
	CHECK(!net::IsValidHeaderName("Bad:Name"));
	CHECK(!net::IsValidHeaderName(""));
	CHECK(net::IsAllowedMethod("GET"));
	CHECK(!net::IsAllowedMethod("get"));
	CHECK(!net::IsAllowedMethod("CONNECT"));
	CHECK(!net::IsAllowedMethod("TRACE"));
}

void TestMetaAndLog() {
	auto fixture = Fixture({ "storage.private" });
	auto broker = fixture.make();
	const auto info = broker.call(Request("meta.info"));
	CHECK(IsOk(info));
	CHECK(Payload(info).find("api")->asInt() == 1);
	CHECK(*Payload(info).find("plugin")->asText() == "com.example.test");
	CHECK(Payload(info).find("granted")->asArray()->size() == 1);
	CHECK(IsOk(broker.call(Request("log", { { "text", Text("hello") } }))));
	CHECK(fixture.logs.size() == 1 && fixture.logs[0] == "hello");
	CHECK(Code(broker.call(Request("log"))) == "invalid");
}

void TestEveryOperationNeedsItsPermission() {
	const auto calls = std::vector<std::pair<std::string, cbor::Value>>{
		{ "storage.get", Request("storage.get", { { "key", Text("k") } }) },
		{ "storage.set", Request("storage.set", { { "key", Text("k") }, { "value", Int(1) } }) },
		{ "net.hosts", Request("net.fetch", { { "url", Text("https://api.example.com/") } }) },
		{ "chats.list", Request("chats.list") },
		{ "chat.info", Request("chat.info", { { "chat", Int(1) } }) },
		{ "messages.history", Request("messages.history", { { "chat", Int(1) } }) },
		{ "account.info", Request("account.info") },
		{ "stars.balance", Request("stars.balance") },
		{ "composer.draft", Request("composer.draft", { { "chat", Int(1) } }) },
		{ "composer.draft", Request("composer.set_draft", { { "chat", Int(1) }, { "text", Text("x") } }) },
		{ "messages.send", Request("messages.send", { { "chat", Int(1) }, { "text", Text("x") } }) },
		{ "ui.toast", Request("ui.toast", { { "text", Text("x") } }) },
	};
	for (const auto &item : calls) {
		auto fixture = Fixture({});
		auto broker = fixture.make();
		const auto refused = broker.call(item.second);
		if (Code(refused) != "denied") {
			std::fprintf(stderr, "%s was not denied\n", item.first.c_str());
			CHECK(false);
		}
		auto allowed = Fixture({ item.first.rfind("storage.", 0) == 0
			? std::string("storage.private")
			: item.first });
		auto grantedBroker = allowed.make();
		const auto answer = grantedBroker.call(item.second);
		if (Code(answer) == "denied") {
			std::fprintf(stderr, "%s was denied despite the grant\n", item.first.c_str());
			CHECK(false);
		}
	}
	CHECK(Fixture({}).make().call(Request("no.such.op")).find("e") != nullptr);
}

void TestStorage() {
	auto fixture = Fixture({ "storage.private" });
	fixture.grants.storageQuotaBytes = 300;
	auto broker = fixture.make();
	const auto set = [&](const std::string &key, cbor::Value value) {
		return broker.call(Request("storage.set", { { "key", Text(key) }, { "value", std::move(value) } }));
	};
	CHECK(IsOk(set("count", Int(7))));
	CHECK(IsOk(set("nested", cbor::Value::FromMap({
		{ "list", cbor::Value::FromArray({ Int(1), Text("two"), cbor::Value() }) },
		{ "flag", cbor::Value::FromBool(true) },
	}))));
	const auto got = broker.call(Request("storage.get", { { "key", Text("count") } }));
	CHECK(IsOk(got) && Payload(got).asInt() == 7);
	const auto nested = broker.call(Request("storage.get", { { "key", Text("nested") } }));
	CHECK(IsOk(nested) && Payload(nested).find("list")->asArray()->size() == 3);
	const auto missing = broker.call(Request("storage.get", { { "key", Text("nope") } }));
	CHECK(IsOk(missing) && Payload(missing).isNull());

	CHECK(Code(set("", Int(1))) == "invalid");
	CHECK(Code(set(std::string(129, 'k'), Int(1))) == "invalid");
	CHECK(Code(set(std::string("bad\x01key"), Int(1))) == "invalid");
	CHECK(Code(broker.call(Request("storage.set", { { "key", Text("k") } }))) == "invalid");
	CHECK(Code(broker.call(Request("storage.get", { { "key", Int(1) } }))) == "invalid");

	CHECK(Code(set("big", Text(std::string(400, 'x')))) == "quota");
	CHECK(IsOk(set("count", Text(std::string(100, 'y')))));
	const auto usage = broker.call(Request("storage.usage"));
	CHECK(IsOk(usage));
	CHECK(Payload(usage).find("quota")->asInt() == 300);
	CHECK(Payload(usage).find("keys")->asInt() == 2);
	CHECK(IsOk(set("count", Int(1))));
	CHECK(IsOk(broker.call(Request("storage.delete", { { "key", Text("nested") } }))));
	CHECK(IsOk(set("a1", Int(1))) && IsOk(set("a2", Int(2))) && IsOk(set("b1", Int(3))));
	const auto list = broker.call(Request("storage.list", { { "prefix", Text("a") } }));
	CHECK(IsOk(list) && Payload(list).asArray()->size() == 2);
	CHECK(Code(broker.call(Request("storage.list", { { "limit", Int(0) } }))) == "invalid");
	CHECK(Code(set("huge", cbor::Value::FromBytes(cbor::Bytes(2 << 20, 1)))) == "limit");
}

void TestKeyCountLimit() {
	auto fixture = Fixture({ "storage.private" });
	fixture.grants.storageQuotaBytes = uint64_t(100) << 20;
	auto broker = fixture.make();
	fixture.grants.callsPerSecond = 1000000;
	auto limited = Fixture({ "storage.private" });
	limited.grants.callsPerSecond = 1000000;
	limited.grants.storageQuotaBytes = uint64_t(100) << 20;
	auto big = limited.make();
	auto accepted = 0;
	for (auto i = 0; i != 4200; ++i) {
		if (IsOk(big.call(Request("storage.set", {
				{ "key", Text("k" + std::to_string(i)) },
				{ "value", Int(i) },
			})))) {
			++accepted;
		}
	}
	CHECK(accepted == 4096);
}

void TestNetwork() {
	auto fixture = Fixture({ "net.hosts" });
	auto broker = fixture.make();
	const auto fetch = [&](cbor::Map args) {
		return broker.call(Request("net.fetch", std::move(args)));
	};
	const auto ok = fetch({ { "url", Text("https://api.example.com/v1?q=1") } });
	CHECK(IsOk(ok));
	CHECK(Payload(ok).find("status")->asInt() == 200);
	CHECK(Payload(ok).find("body")->asBytes()->size() == 5);
	CHECK(Payload(ok).find("headers")->find("content-type") != nullptr);
	CHECK(Payload(ok).find("headers")->find("set-cookie") == nullptr);
	CHECK(fixture.transport.requests.back().url == "https://api.example.com/v1?q=1");
	CHECK(!fixture.transport.redirectAllowed);
	CHECK(fixture.transport.redirectToAllowed);

	CHECK(Code(fetch({ { "url", Text("https://evil.example.org/") } })) == "denied");
	CHECK(Code(fetch({ { "url", Text("http://api.example.com/") } })) == "invalid");
	CHECK(Code(fetch({ { "url", Text("https://127.0.0.1/") } })) == "invalid");
	CHECK(Code(fetch({ { "url", Text("https://user@api.example.com/") } })) == "invalid");
	CHECK(Code(fetch({ { "url", Text("https://api.example.com:444/") } })) == "denied");
	CHECK(Code(fetch({ { "url", Text("https://a.cdn.example.com:8443/") } })) == "");
	CHECK(Code(fetch({})) == "invalid");
	CHECK(Code(fetch({ { "url", Text("https://api.example.com/") }, { "method", Text("CONNECT") } })) == "invalid");

	const auto withHeaders = [&](cbor::Map headers) {
		return fetch({
			{ "url", Text("https://api.example.com/") },
			{ "headers", cbor::Value::FromMap(std::move(headers)) },
		});
	};
	CHECK(IsOk(withHeaders({ { "Authorization", Text("Bearer x") } })));
	CHECK(Code(withHeaders({ { "Host", Text("evil") } })) == "invalid");
	CHECK(Code(withHeaders({ { "Cookie", Text("a=b") } })) == "invalid");
	CHECK(Code(withHeaders({ { "X-A", Text("a\r\nInjected: 1") } })) == "invalid");
	CHECK(Code(withHeaders({ { "Bad Name", Text("a") } })) == "invalid");
	CHECK(Code(withHeaders({ { "X-A", Int(1) } })) == "invalid");
	CHECK(Code(withHeaders({ { "X-Big", Text(std::string(5000, 'a')) } })) == "invalid");
	auto many = cbor::Map();
	for (auto i = 0; i != 40; ++i) {
		many.push_back({ "X-H" + std::to_string(i), Text("v") });
	}
	CHECK(Code(withHeaders(std::move(many))) == "invalid");

	CHECK(Code(fetch({
		{ "url", Text("https://api.example.com/") },
		{ "body", cbor::Value::FromBytes(cbor::Bytes(2 << 20, 1)) },
	})) == "limit");
	CHECK(IsOk(fetch({
		{ "url", Text("https://api.example.com/") },
		{ "method", Text("POST") },
		{ "body", Text("payload") },
	})));
	CHECK(fixture.transport.requests.back().body.size() == 7);
	CHECK(Code(fetch({ { "url", Text("https://api.example.com/") }, { "body", Int(1) } })) == "invalid");

	fixture.transport.bodySize = 9 << 20;
	CHECK(Code(fetch({ { "url", Text("https://api.example.com/") } })) == "limit");
	fixture.transport.bodySize = 5;
	fixture.transport.fail = true;
	const auto failed = fetch({ { "url", Text("https://api.example.com/") } });
	CHECK(Code(failed) == "failed");

	fetch({ { "url", Text("https://api.example.com/") }, { "timeout", Int(1) } });
	fixture.transport.fail = false;
	fetch({ { "url", Text("https://api.example.com/") }, { "timeout", Int(1) } });
	CHECK(fixture.transport.requests.back().timeoutMs == 100);
	fetch({ { "url", Text("https://api.example.com/") }, { "timeout", Int(1000000) } });
	CHECK(fixture.transport.requests.back().timeoutMs == 30000);
}

void TestDataIsFilteredByTheBroker() {
	auto fixture = Fixture({
		"chats.list", "chat.info", "messages.history", "account.info",
		"stars.balance", "composer.draft", "messages.send", "ui.toast",
	});
	auto broker = fixture.make();

	const auto chats = broker.call(Request("chats.list"));
	CHECK(IsOk(chats));
	auto ids = std::vector<int64_t>();
	for (const auto &chat : *Payload(chats).asArray()) {
		ids.push_back(*chat.find("id")->asInt());
	}
	CHECK((ids == std::vector<int64_t>{ 1, 2, 5 }));
	CHECK(Payload(broker.call(Request("chats.list", { { "limit", Int(2) } }))).asArray()->size() == 2);

	CHECK(Code(broker.call(Request("chat.info", { { "chat", Int(api::kServiceChat) } }))) == "denied");
	CHECK(Code(broker.call(Request("chat.info", { { "chat", Int(3) } }))) == "not_found");
	CHECK(Code(broker.call(Request("chat.info", { { "chat", Int(99) } }))) == "not_found");
	CHECK(IsOk(broker.call(Request("chat.info", { { "chat", Int(1) } }))));

	const auto history = broker.call(Request("messages.history", { { "chat", Int(1) }, { "limit", Int(3) } }));
	CHECK(IsOk(history) && Payload(history).asArray()->size() == 3);
	CHECK(Code(broker.call(Request("messages.history", { { "chat", Int(api::kServiceChat) } }))) == "denied");
	CHECK(Code(broker.call(Request("messages.history", { { "chat", Int(3) } }))) == "not_found");
	CHECK(Payload(broker.call(Request("messages.history", { { "chat", Int(1) }, { "limit", Int(100000) } }))).asArray()->size() == 100);

	const auto account = broker.call(Request("account.info"));
	CHECK(IsOk(account));
	CHECK(Payload(account).find("phone") == nullptr);
	CHECK(Payload(account).find("name") != nullptr);
	CHECK(Payload(broker.call(Request("stars.balance"))).find("balance")->asInt() == 1234);

	CHECK(IsOk(broker.call(Request("composer.set_draft", { { "chat", Int(1) }, { "text", Text("hi") } }))));
	CHECK(*Payload(broker.call(Request("composer.draft", { { "chat", Int(1) } }))).asText() == "hi");
	CHECK(Code(broker.call(Request("composer.set_draft", { { "chat", Int(3) }, { "text", Text("x") } }))) == "not_found");
	CHECK(Code(broker.call(Request("composer.draft", { { "chat", Int(api::kServiceChat) } }))) == "denied");

	CHECK(IsOk(broker.call(Request("messages.send", { { "chat", Int(2) }, { "text", Text("yo") } }))));
	CHECK(fixture.data.sent.size() == 1 && fixture.data.sent[0].first == 2);
	CHECK(Code(broker.call(Request("messages.send", { { "chat", Int(api::kServiceChat) }, { "text", Text("x") } }))) == "denied");
	CHECK(Code(broker.call(Request("messages.send", { { "chat", Int(3) }, { "text", Text("x") } }))) == "not_found");
	CHECK(Code(broker.call(Request("messages.send", { { "chat", Int(1) }, { "text", Text("") } }))) == "invalid");
	CHECK(fixture.data.sent.size() == 1);

	CHECK(IsOk(broker.call(Request("ui.toast", { { "text", Text("hello") } }))));
	CHECK(fixture.data.toasts.size() == 1 && fixture.data.toasts[0] == "com.example.test: hello");
	CHECK(Code(broker.call(Request("ui.toast", { { "text", Text(std::string(500, 'x')) } }))) == "invalid");
}

void TestChatScope() {
	auto fixture = Fixture({ "chats.list", "messages.history", "messages.send", "composer.draft" });
	fixture.grants.chats = std::set<int64_t>{ 1, api::kServiceChat, 3 };
	auto broker = fixture.make();
	const auto chats = broker.call(Request("chats.list"));
	CHECK(Payload(chats).asArray()->size() == 1);
	CHECK(IsOk(broker.call(Request("messages.history", { { "chat", Int(1) } }))));
	CHECK(Code(broker.call(Request("messages.history", { { "chat", Int(2) } }))) == "denied");
	CHECK(Code(broker.call(Request("messages.history", { { "chat", Int(api::kServiceChat) } }))) == "denied");
	CHECK(Code(broker.call(Request("messages.history", { { "chat", Int(3) } }))) == "not_found");
	CHECK(Code(broker.call(Request("messages.send", { { "chat", Int(2) }, { "text", Text("x") } }))) == "denied");
	CHECK(Code(broker.call(Request("composer.draft", { { "chat", Int(5) } }))) == "denied");
}

void TestRateLimit() {
	auto fixture = Fixture({});
	fixture.grants.callsPerSecond = 5;
	auto broker = fixture.make();
	for (auto i = 0; i != 5; ++i) {
		CHECK(IsOk(broker.call(Request("meta.info"))));
	}
	CHECK(Code(broker.call(Request("meta.info"))) == "limit");
	fixture.time = 999;
	CHECK(Code(broker.call(Request("meta.info"))) == "limit");
	fixture.time = 1001;
	CHECK(IsOk(broker.call(Request("meta.info"))));
}

void TestMalformedRequests() {
	auto fixture = Fixture({ "storage.private" });
	auto broker = fixture.make();
	CHECK(Code(broker.call(cbor::Value())) == "invalid");
	CHECK(Code(broker.call(Int(1))) == "invalid");
	CHECK(Code(broker.call(cbor::Value::FromMap({ { "op", Int(1) } }))) == "invalid");
	CHECK(Code(broker.call(cbor::Value::FromMap({
		{ "op", Text("storage.get") },
		{ "a", Int(5) },
	}))) == "invalid");
	CHECK(Code(broker.call(cbor::Value::FromMap({ { "op", Text("storage.get") } }))) == "invalid");
	CHECK(Code(broker.call(Request("storage.frobnicate", { { "key", Text("k") } }))) == "invalid");
	CHECK(Code(broker.call(Request("../../etc/passwd"))) == "invalid");
}

void TestAuditTrail() {
	auto fixture = Fixture({ "storage.private" });
	auto broker = fixture.make();
	CHECK(IsOk(broker.call(Request("storage.get", { { "key", Text("k") } }))));
	CHECK(!IsOk(broker.call(Request("account.info"))));
	CHECK(fixture.audit.size() == 2);
	CHECK(fixture.audit.size() == 2 && fixture.audit[0] == "storage.get:ok");
	CHECK(fixture.audit.size() == 2 && fixture.audit[1] == "account.info:denied");
}

void TestEvents() {
	const auto event = [](const char *type, int64_t chat, bool secret = false) {
		return cbor::Value::FromMap({
			{ "type", Text(type) },
			{ "chat", Int(chat) },
			{ "secret", cbor::Value::FromBool(secret) },
			{ "text", Text("hello") },
			{ "old_text", Text("old") },
		});
	};
	{
		auto fixture = Fixture({});
		CHECK(!fixture.make().filterEvent(event("message.new", 1)));
	}
	auto fixture = Fixture({ "messages.read" });
	auto broker = fixture.make();
	CHECK(broker.filterEvent(event("message.new", 1)).has_value());
	CHECK(!broker.filterEvent(event("message.new", api::kServiceChat)));
	CHECK(!broker.filterEvent(event("message.new", 3, true)));
	CHECK(!broker.filterEvent(cbor::Value::FromMap({ { "type", Text("message.new") } })));
	CHECK(!broker.filterEvent(event("account.changed", 1)));
	CHECK(!broker.filterEvent(cbor::Value()));
	const auto deleted = broker.filterEvent(event("message.deleted", 1));
	CHECK(deleted && deleted->find("text") == nullptr);
	CHECK(deleted && deleted->find("old_text") == nullptr);

	fixture.grants.permissions.insert("messages.read_deleted");
	auto privileged = fixture.make();
	const auto kept = privileged.filterEvent(event("message.deleted", 1));
	CHECK(kept && kept->find("old_text") != nullptr);

	fixture.grants.chats = std::set<int64_t>{ 2 };
	auto scoped = fixture.make();
	CHECK(!scoped.filterEvent(event("message.new", 1)));
	CHECK(scoped.filterEvent(event("message.new", 2)).has_value());
}

} // namespace

int main() {
	TestUrlParsing();
	TestHostMatching();
	TestPublicAddresses();
	TestHeaderRules();
	TestMetaAndLog();
	TestEveryOperationNeedsItsPermission();
	TestStorage();
	TestKeyCountLimit();
	TestNetwork();
	TestDataIsFilteredByTheBroker();
	TestChatScope();
	TestRateLimit();
	TestMalformedRequests();
	TestAuditTrail();
	TestEvents();
	return Failures() ? 1 : 0;
}
