#include "fgpl/broker.h"
#include "check.h"
#include "fakes.h"
#include "fgpl/net_policy.h"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
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
		grants.account = "acc";
		grants.permissions = std::move(permissions);
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
	const auto ip = net::ParseUrl("https://8.8.8.8:8443/dns");
	CHECK(ip.ok() && ip.value().ip && ip.value().text == "https://8.8.8.8:8443/dns");
	const auto v6 = net::ParseUrl("https://[2606:4700:4700::1111]/");
	CHECK(v6.ok() && v6.value().host == "2606:4700:4700::1111"
		&& v6.value().text == "https://[2606:4700:4700::1111]/");
	for (const auto bad : {
		"http://example.com/", "ftp://example.com/", "example.com",
		"https://", "https:///x", "https://user:pw@example.com/",
		"https://example.com:0/", "https://example.com:70000/",
		"https://example.com:abc/", "https://127.0.0.1/", "https://10.0.0.1/",
		"https://[::1]/", "https://[fe80::1]/", "https://2606:4700::1/",
		"https://[2606:4700::1/", "https://127.1/", "https://0x7f.1/",
		"https://2130706433/", "https://localhost/", "https://a.localhost/",
		"https://printer.local/", "https://x.internal/",
		"https://exa mple.com/", "https://exa\tmple.com/",
		"https://-.com/..", "https://a..b/", "https://.a/",
		"https://\xC3\xA9xample.com/",
		"https://012.0.0.1/", "https://172.032.0.1:22/", "https://8.08.8.8/",
	}) {
		if (net::ParseUrl(bad).ok()) {
			std::fprintf(stderr, "accepted: %s\n", bad);
			CHECK(false);
		}
	}
	CHECK(!net::ParseUrl(std::string("https://example.com/") + std::string(3000, 'a')).ok());
}

void TestEndpoints() {
	const auto good = net::ParseEndpoint("Example.COM.", 5222);
	CHECK(good.ok() && good.value().host == "example.com" && !good.value().ip);
	const auto v4 = net::ParseEndpoint("8.8.8.8", 53);
	CHECK(v4.ok() && v4.value().ip && v4.value().port == 53);
	const auto v6 = net::ParseEndpoint("[2606:4700:4700::1111]", 443);
	CHECK(v6.ok() && v6.value().host == "2606:4700:4700::1111" && v6.value().ip);
	CHECK(net::ParseEndpoint("2606:4700:4700::1111", 443).ok());
	for (const auto bad : {
		"127.0.0.1", "10.1.2.3", "192.168.1.1", "[::1]", "::1", "fe80::1",
		"localhost", "printer.local", "nas.home.arpa", "127.1", "0x7f.1",
		"2130706433", "a..b", "", "exa mple.com", "[2001:db8::1]", "224.0.0.1",
	}) {
		if (net::ParseEndpoint(bad, 80).ok()) {
			std::fprintf(stderr, "endpoint accepted: %s\n", bad);
			CHECK(false);
		}
	}
	CHECK(!net::ParseEndpoint("example.com", 0).ok());
	CHECK(!net::ParseEndpoint("example.com", 65536).ok());
}

void TestPublicAddresses() {
	for (const auto good : {
		"8.8.8.8", "1.1.1.1", "93.184.216.34", "172.15.0.1", "172.32.0.1",
		"100.63.0.1", "100.128.0.1", "2606:4700:4700::1111",
		"::ffff:8.8.8.8", "2a00:1450:4001::1", "2002:0808:0808::1",
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
		"::ffff:10.0.0.1", "::ffff:127.0.0.1", "2001:db8::1", "fec0::1",
		"100::1", "64:ff9b:1::1", "2002:0a00:0001::1", "::10.0.0.1", "::5",
		"2001:0000:4136:e378:8000:63bf:f5ff:fffe",
		"not an address", "", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1::2::3",
		"12345::1", "1:2:3:4:5:6:7:8:9", "::ffff:1.2.3",
		"012.0.0.1", "172.032.0.1", "8.08.8.8", "00.0.0.0", "::ffff:010.0.0.1",
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
	CHECK(Payload(info).find("api")->asInt() == 5);
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
		{ "net.https", Request("net.fetch", { { "url", Text("https://api.example.com/") } }) },
		{ "net.tcp", Request("net.tcp.connect", { { "host", Text("example.com") }, { "port", Int(80) } }) },
		{ "net.udp", Request("net.udp.connect", { { "host", Text("8.8.8.8") }, { "port", Int(53) } }) },
		{ "contacts.list", Request("contacts.list") },
		{ "folders.list", Request("folders.list") },
		{ "settings.read", Request("settings.read") },
		{ "media.read", Request("media.get", { { "chat", Int(1) }, { "message", Int(100) } }) },
		{ "ui.dialog", Request("ui.dialog.close") },
		{ "ui.dialog", Request("ui.window.close", { { "id", Text("w") } }) },
		{ "messages.transform_outgoing", Request("messages.set_outgoing", { { "text", Text("x") } }) },
		{ "chats.list", Request("chats.list") },
		{ "chat.info", Request("chat.info", { { "chat", Int(1) } }) },
		{ "messages.history", Request("messages.history", { { "chat", Int(1) } }) },
		{ "account.info", Request("account.info") },
		{ "stars.balance", Request("stars.balance") },
		{ "composer.draft", Request("composer.draft", { { "chat", Int(1) } }) },
		{ "composer.draft", Request("composer.set_draft", { { "chat", Int(1) }, { "text", Text("x") } }) },
		{ "messages.send", Request("messages.send", { { "chat", Int(1) }, { "text", Text("x") } }) },
		{ "ui.toast", Request("ui.toast", { { "text", Text("x") } }) },
		{ "messages.edit", Request("messages.edit", { { "chat", Int(1) }, { "id", Int(100) }, { "text", Text("x") } }) },
		{ "messages.edit", Request("messages.edit_media", { { "chat", Int(1) }, { "id", Int(100) }, { "data", cbor::Value::FromBytes(cbor::Bytes(4, 1)) }, { "name", Text("a.png") } }) },
		{ "messages.delete", Request("messages.delete", { { "chat", Int(1) }, { "ids", cbor::Value::FromArray({ Int(100) }) } }) },
		{ "messages.react", Request("messages.react", { { "chat", Int(1) }, { "id", Int(100) }, { "emoji", Text("x") } }) },
		{ "messages.send", Request("chats.mark_read", { { "chat", Int(1) } }) },
		{ "messages.history", Request("messages.search", { { "chat", Int(1) }, { "query", Text("x") } }) },
		{ "users.info", Request("users.info", { { "id", Int(7) } }) },
		{ "users.info", Request("users.resolve", { { "username", Text("bobby") } }) },
		{ "users.info", Request("users.photo", { { "id", Int(7) } }) },
		{ "chat.info", Request("channels.info", { { "id", Int(2) } }) },
		{ "notifications", Request("notify.show", { { "text", Text("x") } }) },
		{ "bots.inline", Request("bots.inline_query", { { "bot", Text("inlinebot") }, { "query", Text("q") } }) },
		{ "net.ws", Request("net.ws.connect", { { "url", Text("wss://api.example.com/socket") } }) },
		{ "bots.inline", Request("bots.press_button", { { "chat", Int(1) }, { "id", Int(100) }, { "row", Int(0) }, { "column", Int(0) } }) },
		{ "messages.history", Request("messages.get", { { "chat", Int(1) }, { "id", Int(100) } }) },
		{ "messages.history", Request("messages.reply_chain", { { "chat", Int(1) }, { "id", Int(103) } }) },
		{ "messages.history", Request("messages.pinned", { { "chat", Int(1) } }) },
		{ "messages.history", Request("messages.scheduled", { { "chat", Int(1) } }) },
		{ "messages.delete", Request("messages.scheduled_delete", { { "chat", Int(1) }, { "ids", cbor::Value::FromArray({ Int(7) }) } }) },
		{ "chats.manage", Request("messages.pin", { { "chat", Int(1) }, { "id", Int(100) } }) },
		{ "chats.manage", Request("messages.unpin", { { "chat", Int(1) }, { "id", Int(100) } }) },
		{ "chats.manage", Request("chats.mute", { { "chat", Int(1) }, { "seconds", Int(60) } }) },
		{ "chats.manage", Request("chats.archive", { { "chat", Int(1) } }) },
		{ "chats.manage", Request("chats.pin", { { "chat", Int(1) } }) },
		{ "contacts.manage", Request("users.block", { { "id", Int(7) } }) },
		{ "contacts.manage", Request("users.unblock", { { "id", Int(7) } }) },
		{ "contacts.manage", Request("contacts.add", { { "id", Int(7) }, { "first_name", Text("Bob") } }) },
		{ "composer.draft", Request("composer.insert", { { "chat", Int(1) }, { "text", Text("x") } }) },
		{ "messages.send", Request("messages.send_poll", { { "chat", Int(1) }, { "question", Text("q?") }, { "options", cbor::Value::FromArray({ Text("a"), Text("b") }) } }) },
		{ "ghost.control", Request("ghost.get") },
		{ "ghost.control", Request("ghost.set", { { "enabled", cbor::Value::FromBool(true) } }) },
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
	auto fixture = Fixture({ "net.https" });
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
	CHECK(fixture.transport.requests.back().owner.plugin == "com.example.test");
	CHECK(fixture.transport.requests.back().owner.account == "acc");
	CHECK(fixture.transport.redirectAllowed);
	CHECK(fixture.transport.redirectToAllowed);
	CHECK(!fixture.transport.redirectToHttp);
	CHECK(!fixture.transport.redirectToPrivate);

	CHECK(IsOk(fetch({ { "url", Text("https://evil.example.org/") } })));
	CHECK(IsOk(fetch({ { "url", Text("https://8.8.8.8/") } })));
	CHECK(IsOk(fetch({ { "url", Text("https://api.example.com:444/") } })));
	CHECK(Code(fetch({ { "url", Text("http://api.example.com/") } })) == "invalid");
	CHECK(Code(fetch({ { "url", Text("https://127.0.0.1/") } })) == "denied");
	CHECK(Code(fetch({ { "url", Text("https://192.168.1.1/") } })) == "invalid");
	CHECK(Code(fetch({ { "url", Text("https://user@api.example.com/") } })) == "invalid");
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
		{ "body", cbor::Value::FromBytes(cbor::Bytes((size_t(100) << 20) + 1, 1)) },
	})) == "limit");
	CHECK(IsOk(fetch({
		{ "url", Text("https://api.example.com/") },
		{ "method", Text("POST") },
		{ "body", cbor::Value::FromBytes(cbor::Bytes(size_t(100) << 20, 1)) },
	})));
	CHECK(fixture.transport.requests.back().body.size() == (size_t(100) << 20));
	CHECK(IsOk(fetch({
		{ "url", Text("https://api.example.com/") },
		{ "method", Text("POST") },
		{ "body", Text("payload") },
	})));
	CHECK(fixture.transport.requests.back().body.size() == 7);
	CHECK(Code(fetch({ { "url", Text("https://api.example.com/") }, { "body", Int(1) } })) == "invalid");

	fixture.transport.bodySize = size_t(100) << 20;
	CHECK(IsOk(fetch({ { "url", Text("https://api.example.com/") } })));
	fixture.transport.bodySize = (size_t(100) << 20) + 1;
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
	CHECK(fixture.transport.requests.back().timeoutMs == 300000);
}

void TestSockets() {
	auto fixture = Fixture({ "net.tcp" });
	auto broker = fixture.make();
	const auto call = [&](std::string_view op, cbor::Map args) {
		return broker.call(Request(op, std::move(args)));
	};
	const auto opened = call("net.tcp.connect", {
		{ "host", Text("Example.com") },
		{ "port", Int(5222) },
		{ "tls", cbor::Value::FromBool(true) },
		{ "timeout", Int(1) },
	});
	CHECK(IsOk(opened));
	const auto id = Payload(opened).find("id")->asInt().value_or(0);
	CHECK(fixture.transport.opened.back().host == "example.com");
	CHECK(fixture.transport.opened.back().tls);
	CHECK(fixture.transport.opened.back().timeoutMs == 100);
	CHECK(fixture.transport.owners.back().plugin == "com.example.test");
	CHECK(Code(call("net.udp.connect", { { "host", Text("8.8.8.8") }, { "port", Int(53) } })) == "denied");
	CHECK(Code(call("net.tcp.connect", { { "host", Text("127.0.0.1") }, { "port", Int(22) } })) == "denied");
	CHECK(Code(call("net.tcp.connect", { { "host", Text("10.0.0.5") }, { "port", Int(22) } })) == "invalid");
	CHECK(Code(call("net.tcp.connect", { { "host", Text("localhost") }, { "port", Int(22) } })) == "denied");
	CHECK(Code(call("net.tcp.connect", { { "host", Text("example.com") }, { "port", Int(0) } })) == "invalid");

	const auto sent = call("net.socket.send", { { "id", Int(id) }, { "data", Text("hello") } });
	CHECK(IsOk(sent) && Payload(sent).find("sent")->asInt() == 5);
	CHECK(fixture.transport.written.size() == 5);
	CHECK(Code(call("net.socket.send", { { "id", Int(id) }, { "data", cbor::Value::FromBytes(cbor::Bytes(2 << 20, 1)) } })) == "limit");
	CHECK(Code(call("net.socket.send", { { "id", Int(id) } })) == "invalid");
	const auto read = call("net.socket.recv", { { "id", Int(id) }, { "max", Int(2) } });
	CHECK(IsOk(read) && Payload(read).find("data")->asBytes()->size() == 2);
	CHECK(Code(call("net.socket.send", { { "id", Int(id + 100) }, { "data", Text("x") } })) == "not_found");
	CHECK(IsOk(call("net.socket.close", { { "id", Int(id) } })));
	CHECK(Code(call("net.socket.recv", { { "id", Int(id) } })) == "not_found");

	for (auto i = 0; i != 16; ++i) {
		CHECK(IsOk(call("net.tcp.connect", { { "host", Text("8.8.8.8") }, { "port", Int(443) } })));
	}
	CHECK(Code(call("net.tcp.connect", { { "host", Text("8.8.8.8") }, { "port", Int(443) } })) == "limit");
	{
		auto other = Broker(fixture.grants, fixture.services);
		CHECK(Code(other.call(Request("net.socket.recv", { { "id", Int(2) } }))) == "not_found");
	}
	broker.releaseSockets();
	CHECK(fixture.transport.live.empty());

	auto udp = Fixture({ "net.udp" });
	auto udpBroker = udp.make();
	const auto datagram = udpBroker.call(Request("net.udp.connect", {
		{ "host", Text("[2606:4700:4700::1111]") },
		{ "port", Int(53) },
	}));
	CHECK(IsOk(datagram));
	CHECK(udp.transport.opened.back().kind == SocketKind::Udp && udp.transport.opened.back().ip);
	CHECK(Code(udpBroker.call(Request("net.udp.connect", {
		{ "host", Text("8.8.8.8") },
		{ "port", Int(53) },
		{ "tls", cbor::Value::FromBool(true) },
	}))) == "invalid");
	const auto udpId = Payload(datagram).find("id")->asInt().value_or(0);
	CHECK(Code(udpBroker.call(Request("net.socket.send", {
		{ "id", Int(udpId) },
		{ "data", cbor::Value::FromBytes(cbor::Bytes(70000, 1)) },
	}))) == "limit");
	udp.transport.fail = true;
	CHECK(Code(udpBroker.call(Request("net.udp.connect", { { "host", Text("8.8.4.4") }, { "port", Int(53) } }))) == "connect");
}

void TestNewDataCalls() {
	auto fixture = Fixture({ "contacts.list", "folders.list", "settings.read", "media.read" });
	auto broker = fixture.make();
	const auto contacts = broker.call(Request("contacts.list"));
	CHECK(IsOk(contacts) && Payload(contacts).asArray()->size() == 2);
	for (const auto &contact : *Payload(contacts).asArray()) {
		CHECK(contact.find("phone") == nullptr);
		CHECK(contact.find("id")->asInt() != api::kServiceChat);
	}
	const auto folders = broker.call(Request("folders.list"));
	CHECK(IsOk(folders) && Payload(folders).asArray()->size() == 1);
	const auto &chats = *(*Payload(folders).asArray())[0].find("chats")->asArray();
	CHECK(chats.size() == 3);
	for (const auto &chat : chats) {
		CHECK(chat.asInt() != api::kServiceChat);
	}
	const auto settings = broker.call(Request("settings.read"));
	CHECK(IsOk(settings) && *Payload(settings).find("theme")->asText() == "dark");
	const auto media = broker.call(Request("media.get", { { "chat", Int(1) }, { "message", Int(100) } }));
	CHECK(IsOk(media) && Payload(media).find("data")->asBytes()->size() == 10);
	CHECK(Code(broker.call(Request("media.get", { { "chat", Int(1) }, { "message", Int(7) } }))) == "not_found");
	CHECK(Code(broker.call(Request("media.get", { { "chat", Int(3) }, { "message", Int(100) } }))) == "not_found");
	CHECK(Code(broker.call(Request("media.get", { { "chat", Int(api::kServiceChat) }, { "message", Int(1) } }))) == "denied");
	const auto thumbnail = broker.call(Request("media.get", {
		{ "chat", Int(1) },
		{ "message", Int(100) },
		{ "thumbnail", cbor::Value::FromBool(true) },
	}));
	CHECK(IsOk(thumbnail) && *Payload(thumbnail).find("kind")->asText() == "thumbnail");
}

void TestOutgoingAndUnload() {
	auto unloads = 0;
	auto dialogs = std::vector<std::string>();
	auto fixture = Fixture({ "messages.transform_outgoing", "chats.list", "ui.dialog" });
	fixture.services.unload = [&] { ++unloads; };
	fixture.services.ui = [&](std::string_view kind, const cbor::Value &) {
		dialogs.emplace_back(kind);
		return true;
	};
	auto broker = fixture.make();
	CHECK(Code(broker.call(Request("messages.set_outgoing", { { "text", Text("x") } }))) == "invalid");
	broker.beginOutgoing();
	CHECK(Code(broker.call(Request("chats.list"))) == "unavailable");
	CHECK(IsOk(broker.call(Request("log", { { "text", Text("still fine") } }))));
	CHECK(Code(broker.call(Request("messages.set_outgoing", { { "text", Text("") } }))) == "invalid");
	CHECK(IsOk(broker.call(Request("messages.set_outgoing", { { "text", Text("changed") } }))));
	CHECK(broker.finishOutgoing().text == std::optional<std::string>("changed"));
	CHECK(IsOk(broker.call(Request("chats.list"))));
	broker.beginOutgoing();
	CHECK(!broker.finishOutgoing().text);

	broker.beginOutgoing();
	CHECK(Code(broker.call(Request("messages.set_outgoing"))) == "invalid");
	CHECK(Code(broker.call(Request("messages.set_outgoing", { { "schedule", Int(-1) } }))) == "invalid");
	CHECK(Code(broker.call(Request("messages.set_outgoing", { { "silent", Text("yes") } }))) == "invalid");
	CHECK(IsOk(broker.call(Request("messages.set_outgoing", {
		{ "silent", cbor::Value::FromBool(true) },
		{ "schedule", Int(2000000000) },
		{ "reply_to", Int(5) },
		{ "topic", Int(7) },
	}))));
	const auto edit = broker.finishOutgoing();
	CHECK(!edit.text && edit.silent == std::optional<bool>(true));
	CHECK(edit.schedule == std::optional<int64_t>(2000000000));
	CHECK(edit.replyTo == std::optional<int64_t>(5) && edit.topic == std::optional<int64_t>(7));
	CHECK(!edit.drop);

	CHECK(Code(broker.call(Request("messages.drop_outgoing"))) == "invalid");
	broker.beginOutgoing();
	CHECK(IsOk(broker.call(Request("messages.drop_outgoing", { { "reason", Text("spam") } }))));
	const auto dropped = broker.finishOutgoing();
	CHECK(dropped.drop && dropped.dropReason == "spam");

	CHECK(IsOk(broker.call(Request("plugin.unload"))));
	CHECK(unloads == 1);
	const auto page = cbor::Value::FromMap({
		{ "title", Text("Hello") },
		{ "items", cbor::Value::FromArray({
			cbor::Value::FromMap({ { "type", Text("text") }, { "text", Text("hi") } }),
		}) },
	});
	CHECK(IsOk(broker.call(Request("ui.dialog.show", { { "page", page } }))));
	CHECK(Code(broker.call(Request("ui.dialog.show", { { "page", Text("x") } }))) == "invalid");
	CHECK(IsOk(broker.call(Request("ui.dialog.close"))));
	CHECK(dialogs == (std::vector<std::string>{ "dialog", "dialog.close" }));
}

void TestServerTime() {
	auto fixture = Fixture({});
	auto broker = fixture.make();
	const auto now = broker.call(Request("time.now"));
	CHECK(IsOk(now) && Payload(now).asInt() == std::optional<int64_t>(1700000000));
	broker.beginOutgoing();
	CHECK(IsOk(broker.call(Request("time.now"))));
	(void)broker.finishOutgoing();
}

void TestHistoryFromUser() {
	auto fixture = Fixture({ "messages.history" });
	auto broker = fixture.make();
	CHECK(IsOk(broker.call(Request("messages.history", { { "chat", Int(1) } }))));
	CHECK(fixture.data.historyFromUser == 0);
	const auto mine = broker.call(Request("messages.history", {
		{ "chat", Int(1) }, { "limit", Int(3) }, { "from_user", Text("me") },
	}));
	CHECK(IsOk(mine) && fixture.data.historyFromUser == 42);
	CHECK(Payload(mine).asArray() && Payload(mine).asArray()->size() == 3);
	for (const auto &message : *Payload(mine).asArray()) {
		CHECK(message.find("out")->asBool() == std::optional<bool>(true));
	}
	CHECK(IsOk(broker.call(Request("messages.history", {
		{ "chat", Int(1) }, { "from_user", Int(7) },
	}))));
	CHECK(fixture.data.historyFromUser == 7);
	for (const auto &bad : { Text("you"), Int(0), Int(-5), Int(777000), cbor::Value::FromBool(true) }) {
		CHECK(Code(broker.call(Request("messages.history", {
			{ "chat", Int(1) }, { "from_user", bad },
		}))) == "invalid");
	}
}

void TestInlineQuery() {
	auto fixture = Fixture({ "bots.inline" });
	auto broker = fixture.make();
	const auto found = broker.call(Request("bots.inline_query", {
		{ "bot", Text("@inlinebot") }, { "query", Text("cats") },
	}));
	CHECK(IsOk(found));
	CHECK(fixture.data.inlineQueries == (std::vector<std::string>{ "0/inlinebot/cats" }));
	const auto list = Payload(found).asArray();
	CHECK(list && list->size() == 2);
	if (list && list->size() == 2) {
		CHECK(*(*list)[0].find("title")->asText() == "Title cats");
		CHECK(*(*list)[0].find("text")->asText() == "Text one");
		CHECK(*(*list)[1].find("id")->asText() == "r2");
	}
	fixture.time += 1500;
	CHECK(IsOk(broker.call(Request("bots.inline_query", { { "bot", Int(9) }, { "query", Text("") } }))));
	CHECK(fixture.data.inlineQueries.back() == "9//");
	CHECK(Code(broker.call(Request("bots.inline_query", { { "bot", Int(9) }, { "query", Text("x") } }))) == "limit");
	fixture.time += 1500;
	CHECK(Code(broker.call(Request("bots.inline_query", { { "bot", Text("nobodybot") }, { "query", Text("x") } }))) == "not_found");
	fixture.time += 1500;
	CHECK(Code(broker.call(Request("bots.inline_query", { { "bot", Int(777000) }, { "query", Text("x") } }))) == "denied");
	for (const auto &bad : {
		Request("bots.inline_query", { { "bot", Int(0) }, { "query", Text("x") } }),
		Request("bots.inline_query", { { "bot", Text("a b") }, { "query", Text("x") } }),
		Request("bots.inline_query", { { "bot", Int(9) } }),
		Request("bots.inline_query", { { "bot", Int(9) }, { "query", Text(std::string(300, 'a')) } }),
	}) {
		fixture.time += 1500;
		CHECK(Code(broker.call(bad)) == "invalid");
	}
}

void TestOpenSettings() {
	auto kinds = std::vector<std::string>();
	auto fixture = Fixture({ "ui.settings_page" });
	fixture.services.ui = [&](std::string_view kind, const cbor::Value &) {
		kinds.emplace_back(kind);
		return true;
	};
	fixture.time = 100000;
	auto broker = fixture.make();
	CHECK(Code(broker.call(Request("ui.open_settings"))) == "denied");
	broker.noteUserAction("menu.click");
	CHECK(IsOk(broker.call(Request("ui.open_settings"))));
	CHECK(kinds == (std::vector<std::string>{ "open_settings" }));
	CHECK(Code(broker.call(Request("ui.open_settings"))) == "denied");
	broker.noteUserAction("menu.click");
	fixture.time += 11000;
	CHECK(Code(broker.call(Request("ui.open_settings"))) == "denied");
	CHECK(kinds.size() == 1);

	auto plain = Fixture({});
	plain.time = 100000;
	auto other = plain.make();
	other.noteUserAction("menu.click");
	CHECK(Code(other.call(Request("ui.open_settings"))) == "denied");

	auto unwired = Fixture({ "ui.settings_page" });
	unwired.time = 100000;
	auto idle = unwired.make();
	idle.noteUserAction("menu.click");
	CHECK(Code(idle.call(Request("ui.open_settings"))) == "unavailable");
}

void TestOutgoingEntities() {
	auto fixture = Fixture({ "messages.transform_outgoing" });
	auto broker = fixture.make();
	const auto entity = [](std::string_view type, int64_t offset, int64_t length, cbor::Map extra = {}) {
		auto map = cbor::Map{
			{ "type", Text(type) },
			{ "offset", Int(offset) },
			{ "length", Int(length) },
		};
		for (auto &item : extra) {
			map.push_back(std::move(item));
		}
		return cbor::Value::FromMap(std::move(map));
	};
	const auto set = [&](cbor::Value entities, cbor::Map extra = {}) {
		extra.push_back({ "entities", std::move(entities) });
		return broker.call(Request("messages.set_outgoing", std::move(extra)));
	};
	broker.beginOutgoing();
	CHECK(IsOk(set(cbor::Value::FromArray({
		entity("bold", 0, 3),
		entity("italic", 4, 2),
		entity("text_url", 0, 1, { { "url", Text("https://example.com/") } }),
		entity("pre", 0, 2, { { "language", Text("cpp") } }),
		entity("custom_emoji", 6, 2, { { "id", Int(55) } }),
	}), { { "text", Text("abc de fg") } })));
	auto edit = broker.finishOutgoing();
	CHECK(edit.text == std::optional<std::string>("abc de fg"));
	CHECK(edit.entities && edit.entities->size() == 5);
	if (edit.entities && edit.entities->size() == 5) {
		CHECK((*edit.entities)[0].type == "bold" && (*edit.entities)[1].offset == 4);
		CHECK((*edit.entities)[2].url == "https://example.com/");
		CHECK((*edit.entities)[3].language == "cpp");
		CHECK((*edit.entities)[4].id == 55);
	}

	broker.beginOutgoing();
	CHECK(IsOk(set(cbor::Value::FromArray(cbor::Array()))));
	edit = broker.finishOutgoing();
	CHECK(!edit.text && edit.entities && edit.entities->empty());

	const auto refused = [&](cbor::Value entities, cbor::Map extra = {}) {
		broker.beginOutgoing();
		const auto code = Code(set(std::move(entities), std::move(extra)));
		const auto result = broker.finishOutgoing();
		return code == "invalid" && !result.entities;
	};
	CHECK(refused(Text("bold")));
	CHECK(refused(cbor::Value::FromArray({ Text("bold") })));
	CHECK(refused(cbor::Value::FromArray({ entity("mystery", 0, 1) })));
	CHECK(refused(cbor::Value::FromArray({ entity("bold", -1, 1) })));
	CHECK(refused(cbor::Value::FromArray({ entity("bold", 0, 0) })));
	CHECK(refused(cbor::Value::FromArray({ entity("text_url", 0, 1) })));
	CHECK(refused(cbor::Value::FromArray({ entity("text_url", 0, 1, { { "url", Text("javascript:alert(1)") } }) })));
	CHECK(refused(cbor::Value::FromArray({ entity("custom_emoji", 0, 1) })));
	CHECK(refused(cbor::Value::FromArray({ entity("custom_emoji", 0, 1, { { "id", Int(0) } }) })));
	CHECK(refused(cbor::Value::FromArray({ entity("bold", 0, 1) }), { { "text", Text("x") }, { "format", Text("markdown") } }));
	auto many = cbor::Array();
	for (auto i = 0; i != 300; ++i) {
		many.push_back(entity("bold", i, 1));
	}
	CHECK(refused(cbor::Value::FromArray(std::move(many))));
}

void TestIncomingHook() {
	auto fixture = Fixture({ "messages.transform_incoming", "messages.transform_outgoing", "chats.list" });
	auto broker = fixture.make();
	CHECK(Code(broker.call(Request("messages.set_incoming", { { "text", Text("x") } }))) == "invalid");
	CHECK(Code(broker.call(Request("messages.hide_incoming"))) == "invalid");

	broker.beginIncoming();
	CHECK(Code(broker.call(Request("chats.list"))) == "unavailable");
	CHECK(IsOk(broker.call(Request("log", { { "text", Text("fine") } }))));
	CHECK(Code(broker.call(Request("messages.set_outgoing", { { "text", Text("x") } }))) == "invalid");
	CHECK(Code(broker.call(Request("messages.drop_outgoing"))) == "invalid");
	CHECK(Code(broker.call(Request("messages.set_incoming"))) == "invalid");
	CHECK(Code(broker.call(Request("messages.set_incoming", { { "text", Text("") } }))) == "invalid");
	CHECK(Code(broker.call(Request("messages.set_incoming", { { "silent", cbor::Value::FromBool(true) } }))) == "invalid");
	CHECK(IsOk(broker.call(Request("messages.set_incoming", { { "text", Text("seen") } }))));
	const auto bold = cbor::Value::FromMap({ { "type", Text("bold") }, { "offset", Int(0) }, { "length", Int(2) } });
	CHECK(IsOk(broker.call(Request("messages.set_incoming", { { "entities", cbor::Value::FromArray({ bold }) } }))));
	CHECK(Code(broker.call(Request("messages.set_incoming", {
		{ "text", Text("x") }, { "format", Text("markdown") },
		{ "entities", cbor::Value::FromArray({ bold }) },
	}))) == "invalid");
	CHECK(Code(broker.call(Request("messages.set_incoming", { { "entities", Text("bold") } }))) == "invalid");
	auto edit = broker.finishOutgoing();
	CHECK(edit.text == std::optional<std::string>("seen") && edit.entities && edit.entities->size() == 1);
	CHECK(!edit.drop);

	broker.beginIncoming();
	CHECK(IsOk(broker.call(Request("messages.hide_incoming", { { "reason", Text("spam") } }))));
	CHECK(Code(broker.call(Request("messages.hide_incoming", { { "reason", Text(std::string(300, 'a')) } }))) == "invalid");
	edit = broker.finishOutgoing();
	CHECK(edit.drop && edit.dropReason == "spam");

	broker.beginOutgoing();
	CHECK(Code(broker.call(Request("messages.set_incoming", { { "text", Text("x") } }))) == "invalid");
	CHECK(Code(broker.call(Request("messages.hide_incoming"))) == "invalid");
	CHECK(IsOk(broker.call(Request("messages.set_outgoing", { { "text", Text("x") } }))));
	(void)broker.finishOutgoing();

	auto outgoingOnly = Fixture({ "messages.transform_outgoing" });
	auto outgoingBroker = outgoingOnly.make();
	outgoingBroker.beginIncoming();
	CHECK(Code(outgoingBroker.call(Request("messages.set_incoming", { { "text", Text("x") } }))) == "denied");
	CHECK(Code(outgoingBroker.call(Request("messages.hide_incoming"))) == "denied");
	(void)outgoingBroker.finishOutgoing();
	auto incomingOnly = Fixture({ "messages.transform_incoming" });
	auto incomingBroker = incomingOnly.make();
	incomingBroker.beginOutgoing();
	CHECK(Code(incomingBroker.call(Request("messages.set_outgoing", { { "text", Text("x") } }))) == "denied");
	(void)incomingBroker.finishOutgoing();
}

void TestMessageLookups() {
	auto fixture = Fixture({ "messages.history" });
	auto broker = fixture.make();
	const auto got = broker.call(Request("messages.get", { { "chat", Int(1) }, { "id", Int(100) } }));
	CHECK(IsOk(got));
	const auto entities = Payload(got).find("entities");
	CHECK(entities && entities->asArray() && entities->asArray()->size() == 1);
	CHECK(entities && *(*entities->asArray())[0].find("type")->asText() == "bold");
	const auto rows = Payload(got).find("buttons");
	CHECK(rows && rows->asArray() && rows->asArray()->size() == 1);
	if (rows && rows->asArray() && rows->asArray()->size() == 1) {
		const auto line = (*rows->asArray())[0].asArray();
		CHECK(line && line->size() == 2);
		CHECK(line && *(*line)[0].find("type")->asText() == "callback" && (*line)[0].find("url") == nullptr);
		CHECK(line && *(*line)[1].find("url")->asText() == "https://example.com/");
	}
	CHECK(Payload(broker.call(Request("messages.get", { { "chat", Int(1) }, { "id", Int(101) } }))).find("entities") == nullptr);
	CHECK(Code(broker.call(Request("messages.get", { { "chat", Int(1) }, { "id", Int(5000) } }))) == "not_found");
	CHECK(Code(broker.call(Request("messages.get", { { "chat", Int(1) }, { "id", Int(998) } }))) == "not_found");
	CHECK(Code(broker.call(Request("messages.get", { { "chat", Int(1) }, { "id", Int(0) } }))) == "invalid");
	CHECK(Code(broker.call(Request("messages.get", { { "chat", Int(1) } }))) == "invalid");
	CHECK(Code(broker.call(Request("messages.get", { { "chat", Int(777000) }, { "id", Int(100) } }))) == "denied");
	CHECK(Code(broker.call(Request("messages.get", { { "chat", Int(3) }, { "id", Int(100) } }))) == "not_found");

	auto reader = Fixture({ "messages.history", "messages.read_deleted" });
	auto readerBroker = reader.make();
	CHECK(IsOk(readerBroker.call(Request("messages.get", { { "chat", Int(1) }, { "id", Int(998) } }))));

	const auto chain = broker.call(Request("messages.reply_chain", { { "chat", Int(1) }, { "id", Int(105) }, { "limit", Int(3) } }));
	CHECK(IsOk(chain) && Payload(chain).asArray() && Payload(chain).asArray()->size() == 3);
	if (Payload(chain).asArray() && Payload(chain).asArray()->size() == 3) {
		CHECK((*Payload(chain).asArray())[0].find("id")->asInt() == 104);
		CHECK((*Payload(chain).asArray())[2].find("id")->asInt() == 102);
	}
	const auto whole = broker.call(Request("messages.reply_chain", { { "chat", Int(1) }, { "id", Int(105) }, { "limit", Int(500) } }));
	CHECK(IsOk(whole) && Payload(whole).asArray()->size() == 4);
	const auto none = broker.call(Request("messages.reply_chain", { { "chat", Int(1) }, { "id", Int(100) } }));
	CHECK(IsOk(none) && Payload(none).asArray()->empty());
	CHECK(Code(broker.call(Request("messages.reply_chain", { { "chat", Int(1) }, { "id", Int(5000) } }))) == "not_found");

	const auto pinned = broker.call(Request("messages.pinned", { { "chat", Int(1) } }));
	CHECK(IsOk(pinned) && Payload(pinned).asArray()->size() == 2);
	const auto one = broker.call(Request("messages.pinned", { { "chat", Int(1) }, { "limit", Int(1) } }));
	CHECK(IsOk(one) && Payload(one).asArray()->size() == 1);
	const auto scheduled = broker.call(Request("messages.scheduled", { { "chat", Int(1) } }));
	CHECK(IsOk(scheduled) && Payload(scheduled).asArray()->size() == 1);
	CHECK(Code(broker.call(Request("messages.scheduled", { { "chat", Int(777000) } }))) == "denied");

	auto deleter = Fixture({ "messages.delete" });
	auto deleterBroker = deleter.make();
	CHECK(IsOk(deleterBroker.call(Request("messages.scheduled_delete", {
		{ "chat", Int(1) }, { "ids", cbor::Value::FromArray({ Int(7), Int(8) }) },
	}))));
	CHECK(deleter.data.actions == (std::vector<std::string>{ "unschedule 1 2" }));
	CHECK(Code(deleterBroker.call(Request("messages.scheduled_delete", { { "chat", Int(1) }, { "ids", cbor::Value::FromArray({}) } }))) == "invalid");
	CHECK(Code(deleterBroker.call(Request("messages.scheduled_delete", { { "chat", Int(1) }, { "ids", cbor::Value::FromArray({ Int(-1) }) } }))) == "invalid");
}

void TestChatManagement() {
	auto fixture = Fixture({ "chats.manage", "contacts.manage" });
	auto broker = fixture.make();
	CHECK(IsOk(broker.call(Request("messages.pin", { { "chat", Int(1) }, { "id", Int(100) }, { "silent", cbor::Value::FromBool(true) } }))));
	CHECK(IsOk(broker.call(Request("messages.unpin", { { "chat", Int(1) }, { "id", Int(100) } }))));
	CHECK(Code(broker.call(Request("messages.pin", { { "chat", Int(1) }, { "id", Int(55) } }))) == "not_found");
	CHECK(Code(broker.call(Request("messages.pin", { { "chat", Int(1) }, { "id", Int(0) } }))) == "invalid");
	CHECK(Code(broker.call(Request("messages.pin", { { "chat", Int(1) }, { "id", Int(5) }, { "silent", Text("x") } }))) == "invalid");
	CHECK(Code(broker.call(Request("messages.pin", { { "chat", Int(777000) }, { "id", Int(5) } }))) == "denied");
	CHECK(IsOk(broker.call(Request("chats.mute", { { "chat", Int(1) }, { "seconds", Int(3600) } }))));
	CHECK(IsOk(broker.call(Request("chats.mute", { { "chat", Int(1) }, { "forever", cbor::Value::FromBool(true) } }))));
	CHECK(IsOk(broker.call(Request("chats.mute", { { "chat", Int(1) } }))));
	CHECK(Code(broker.call(Request("chats.mute", { { "chat", Int(1) }, { "seconds", Int(-5) } }))) == "invalid");
	CHECK(Code(broker.call(Request("chats.mute", { { "chat", Int(1) }, { "seconds", Int(99999999999) } }))) == "invalid");
	CHECK(Code(broker.call(Request("chats.mute", { { "chat", Int(1) }, { "seconds", Int(5) }, { "forever", cbor::Value::FromBool(true) } }))) == "invalid");
	CHECK(IsOk(broker.call(Request("chats.archive", { { "chat", Int(1) } }))));
	CHECK(IsOk(broker.call(Request("chats.archive", { { "chat", Int(1) }, { "archived", cbor::Value::FromBool(false) } }))));
	CHECK(IsOk(broker.call(Request("chats.pin", { { "chat", Int(1) } }))));
	CHECK(IsOk(broker.call(Request("chats.pin", { { "chat", Int(1) }, { "pinned", cbor::Value::FromBool(false) } }))));
	CHECK(Code(broker.call(Request("chats.pin", { { "chat", Int(1) }, { "pinned", Int(2) } }))) == "invalid");
	CHECK(Code(broker.call(Request("chats.archive", { { "chat", Int(3) } }))) == "not_found");
	CHECK(IsOk(broker.call(Request("users.block", { { "id", Int(7) } }))));
	CHECK(IsOk(broker.call(Request("users.unblock", { { "id", Int(7) } }))));
	CHECK(Code(broker.call(Request("users.block", { { "id", Int(0) } }))) == "invalid");
	CHECK(Code(broker.call(Request("users.block", { { "id", Int(777000) } }))) == "denied");
	CHECK(IsOk(broker.call(Request("contacts.add", { { "id", Int(7) }, { "first_name", Text("Bob") }, { "last_name", Text("Ross") } }))));
	CHECK(Code(broker.call(Request("contacts.add", { { "id", Int(7) }, { "first_name", Text("") } }))) == "invalid");
	CHECK(Code(broker.call(Request("contacts.add", { { "id", Int(7) } }))) == "invalid");
	CHECK(Code(broker.call(Request("contacts.add", { { "id", Int(777000) }, { "first_name", Text("x") } }))) == "denied");
	CHECK(fixture.data.actions == (std::vector<std::string>{
		"pin 1 100 silent", "unpin 1 100", "pin 1 55",
		"mute 1 3600", "mute 1 -1", "mute 1 0",
		"archive 1 on", "archive 1 off", "pinchat 1 on", "pinchat 1 off",
		"block 7", "unblock 7", "contact 7 Bob|Ross",
	}));

	auto limited = Fixture({ "chats.manage" });
	auto limitedBroker = limited.make();
	CHECK(Code(limitedBroker.call(Request("users.block", { { "id", Int(7) } }))) == "denied");
	auto contacts = Fixture({ "contacts.manage" });
	auto contactsBroker = contacts.make();
	CHECK(Code(contactsBroker.call(Request("chats.pin", { { "chat", Int(1) } }))) == "denied");

	auto composer = Fixture({ "composer.draft" });
	auto composerBroker = composer.make();
	CHECK(IsOk(composerBroker.call(Request("composer.insert", { { "chat", Int(1) }, { "text", Text("hi") } }))));
	CHECK(Code(composerBroker.call(Request("composer.insert", { { "chat", Int(1) }, { "text", Text("") } }))) == "invalid");
	CHECK(Code(composerBroker.call(Request("composer.insert", { { "chat", Int(777000) }, { "text", Text("x") } }))) == "denied");
	CHECK(composer.data.actions == (std::vector<std::string>{ "insert 1 hi" }));
}

void TestMembersPollsAndGhost() {
	auto fixture = Fixture({ "chat.info", "users.info", "messages.send", "ghost.control" });
	auto broker = fixture.make();
	fixture.time = 5000;
	const auto members = broker.call(Request("chats.members", { { "chat", Int(1) }, { "limit", Int(10) } }));
	CHECK(IsOk(members) && Payload(members).asArray() && Payload(members).asArray()->size() == 3);
	if (Payload(members).asArray() && Payload(members).asArray()->size() == 3) {
		const auto &first = (*Payload(members).asArray())[0];
		CHECK(first.find("id")->asInt() == 7 && *first.find("role")->asText() == "member");
		CHECK(*(*Payload(members).asArray())[2].find("role")->asText() == "creator");
	}
	CHECK(Code(broker.call(Request("chats.members", { { "chat", Int(1) } }))) == "limit");
	fixture.time += 1500;
	const auto small = broker.call(Request("chats.members", { { "chat", Int(1) }, { "limit", Int(2) }, { "offset", Int(4) } }));
	CHECK(IsOk(small) && Payload(small).asArray()->size() == 2);
	fixture.time += 1500;
	CHECK(IsOk(broker.call(Request("chats.admins", { { "chat", Int(1) } }))));
	fixture.time += 1500;
	CHECK(IsOk(broker.call(Request("chats.members", { { "chat", Int(1) }, { "query", Text("bo") } }))));
	fixture.time += 1500;
	CHECK(Code(broker.call(Request("chats.members", { { "chat", Int(1) }, { "query", Text("") } }))) == "invalid");
	fixture.time += 1500;
	CHECK(Code(broker.call(Request("chats.members", { { "chat", Int(1) }, { "offset", Int(-1) } }))) == "invalid");
	fixture.time += 1500;
	CHECK(Code(broker.call(Request("chats.members", { { "chat", Int(777000) } }))) == "denied");
	CHECK(fixture.data.actions == (std::vector<std::string>{
		"members 1 recent  0 10", "members 1 recent  4 2", "members 1 admins  0 50", "members 1 search bo 0 50",
	}));
	auto onlyChat = Fixture({ "chat.info" });
	auto onlyChatBroker = onlyChat.make();
	CHECK(Code(onlyChatBroker.call(Request("chats.members", { { "chat", Int(1) } }))) == "denied");

	fixture.data.actions.clear();
	const auto options = [](std::initializer_list<const char*> list) {
		auto result = cbor::Array();
		for (const auto item : list) {
			result.push_back(Text(item));
		}
		return cbor::Value::FromArray(std::move(result));
	};
	const auto yes = cbor::Value::FromBool(true);
	CHECK(IsOk(broker.call(Request("messages.send_poll", {
		{ "chat", Int(1) }, { "question", Text("Lunch?") }, { "options", options({ "pizza", "soup" }) },
		{ "anonymous", cbor::Value::FromBool(false) }, { "multiple", yes }, { "close_period", Int(60) },
	}))));
	CHECK(IsOk(broker.call(Request("messages.send_poll", {
		{ "chat", Int(1) }, { "question", Text("2+2") }, { "options", options({ "3", "4" }) },
		{ "quiz", yes }, { "correct", Int(1) }, { "explanation", Text("math") },
	}))));
	CHECK(fixture.data.actions == (std::vector<std::string>{
		"poll 1 Lunch? [pizza,soup,] public multiple close60",
		"poll 1 2+2 [3,4,] quiz1 why:math",
	}));
	const auto invalid = [&](cbor::Map args) {
		args.push_back({ "chat", Int(1) });
		fixture.time += 1500;
		return Code(broker.call(Request("messages.send_poll", std::move(args)))) == "invalid";
	};
	CHECK(invalid({ { "question", Text("") }, { "options", options({ "a", "b" }) } }));
	CHECK(invalid({ { "question", Text("q") }, { "options", options({ "a" }) } }));
	CHECK(invalid({ { "question", Text("q") }, { "options", options({ "a", "" }) } }));
	CHECK(invalid({ { "question", Text("q") }, { "options", options({ "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k" }) } }));
	CHECK(invalid({ { "question", Text("q") }, { "options", Text("a") } }));
	CHECK(invalid({ { "question", Text("q") }, { "options", options({ "a", "b" }) }, { "quiz", yes } }));
	CHECK(invalid({ { "question", Text("q") }, { "options", options({ "a", "b" }) }, { "quiz", yes }, { "correct", Int(2) } }));
	CHECK(invalid({ { "question", Text("q") }, { "options", options({ "a", "b" }) }, { "quiz", yes }, { "correct", Int(0) }, { "multiple", yes } }));
	CHECK(invalid({ { "question", Text("q") }, { "options", options({ "a", "b" }) }, { "correct", Int(0) } }));
	CHECK(invalid({ { "question", Text("q") }, { "options", options({ "a", "b" }) }, { "explanation", Text("why") } }));
	CHECK(invalid({ { "question", Text("q") }, { "options", options({ "a", "b" }) }, { "close_period", Int(2) } }));
	CHECK(invalid({ { "question", Text("q") }, { "options", options({ "a", "b" }) }, { "close_period", Int(601) } }));
	fixture.time += 1500;
	CHECK(Code(broker.call(Request("messages.send_poll", { { "chat", Int(777000) }, { "question", Text("q") }, { "options", options({ "a", "b" }) } }))) == "denied");

	const auto state = broker.call(Request("ghost.get"));
	CHECK(IsOk(state) && Payload(state).find("active")->asBool() == false);
	CHECK(IsOk(broker.call(Request("ghost.set", { { "enabled", yes } }))));
	CHECK(Payload(broker.call(Request("ghost.get"))).find("active")->asBool() == true);
	CHECK(Payload(broker.call(Request("ghost.get"))).find("send_read_messages")->asBool() == false);
	CHECK(Code(broker.call(Request("ghost.set", { { "enabled", cbor::Value::FromBool(false) } }))) == "denied");
	CHECK(Code(broker.call(Request("ghost.set", { { "enabled", Int(1) } }))) == "invalid");
	CHECK(Code(broker.call(Request("ghost.set"))) == "invalid");
	broker.noteUserAction("menu.click");
	CHECK(IsOk(broker.call(Request("ghost.set", { { "enabled", cbor::Value::FromBool(false) } }))));
	CHECK(Payload(broker.call(Request("ghost.get"))).find("active")->asBool() == false);
	fixture.time += 11000;
	CHECK(IsOk(broker.call(Request("ghost.set", { { "enabled", yes } }))));
	CHECK(Code(broker.call(Request("ghost.set", { { "enabled", cbor::Value::FromBool(false) } }))) == "denied");
}

void TestLocalNetwork() {
	CHECK(net::IsLoopbackHost("localhost") && net::IsLoopbackHost("LOCALHOST.") && net::IsLoopbackHost("127.0.0.1"));
	CHECK(net::IsLoopbackHost("127.9.9.9") && net::IsLoopbackHost("::1") && net::IsLoopbackHost("[::1]"));
	for (const auto host : { "example.com", "localhost.example.com", "10.0.0.1", "192.168.1.1", "0.0.0.0", "::", "fe80::1", "8.8.8.8", "128.0.0.1" }) {
		CHECK(!net::IsLoopbackHost(host));
	}
	const auto parsed = net::ParseLocalUrl("HTTP://LocalHost:8080/a?b=1#frag");
	CHECK(parsed.ok() && parsed.value().scheme == "http" && !parsed.value().tls);
	CHECK(parsed.ok() && parsed.value().host == "localhost" && parsed.value().port == 8080);
	CHECK(parsed.ok() && parsed.value().target == "/a?b=1" && parsed.value().text == "http://localhost:8080/a?b=1");
	CHECK(net::ParseLocalUrl("http://127.0.0.1/").value().port == 80);
	CHECK(net::ParseLocalUrl("https://[::1]/x").value().port == 443);
	CHECK(net::ParseLocalUrl("wss://localhost:1/").value().tls);
	CHECK(net::ParseLocalUrl("ws://localhost/").value().scheme == "ws");
	CHECK(net::ParseLocalUrl("https://example.com/").error() == "not local");
	CHECK(net::ParseLocalUrl("http://10.0.0.1/").error() == "not local");
	CHECK(net::ParseLocalUrl("ftp://localhost/").error() == "not local");
	CHECK(!net::ParseLocalUrl("http://user@localhost/").ok());
	CHECK(!net::ParseLocalUrl("http://localhost:0/").ok() && !net::ParseLocalUrl("http://localhost:99999/").ok());
	CHECK(net::ParseLocalEndpoint("localhost", 80).ok() && net::ParseLocalEndpoint("127.0.0.1", 80).value().ip);
	CHECK(!net::ParseLocalEndpoint("example.com", 80).ok() && !net::ParseLocalEndpoint("localhost", 0).ok());
	CHECK(!net::ParseUrl("https://localhost/").ok() && !net::ParseUrl("http://localhost/").ok());

	auto fixture = Fixture({ "net.local" });
	fixture.grants.localPorts = { { 8080, 8080 }, { 9000, 9100 } };
	fixture.transport.webSocket = true;
	auto broker = fixture.make();
	const auto fetch = [&](Broker &target, const char *url) {
		return Code(target.call(Request("net.fetch", { { "url", Text(url) } })));
	};
	CHECK(fetch(broker, "http://localhost:8080/api").empty());
	CHECK(fixture.transport.requests.back().local && fixture.transport.requests.back().url == "http://localhost:8080/api");
	CHECK(fixture.transport.requests.back().host == "localhost" && !fixture.transport.requests.back().allowRedirect);
	CHECK(fetch(broker, "http://127.0.0.1:9050/").empty());
	CHECK(fetch(broker, "https://[::1]:9100/x").empty());
	CHECK(fetch(broker, "http://localhost:8081/") == "denied");
	CHECK(fetch(broker, "http://localhost/") == "denied");
	CHECK(fetch(broker, "ws://localhost:8080/") == "invalid");
	CHECK(fetch(broker, "https://example.com/") == "denied");
	CHECK(fetch(broker, "http://10.0.0.1:8080/") == "denied");
	CHECK(fetch(broker, "http://localhost:8080/\x01") == "invalid");

	auto publicOnly = Fixture({ "net.https", "net.tcp", "net.udp", "net.ws" });
	auto publicBroker = publicOnly.make();
	CHECK(fetch(publicBroker, "http://localhost:8080/") == "denied");
	CHECK(fetch(publicBroker, "https://127.0.0.1/") == "denied");
	CHECK(fetch(publicBroker, "http://example.com/") == "invalid");
	CHECK(fetch(publicBroker, "https://192.168.1.1/") == "invalid");
	CHECK(fetch(publicBroker, "https://api.example.com/").empty());
	CHECK(Code(publicBroker.call(Request("net.tcp.connect", { { "host", Text("127.0.0.1") }, { "port", Int(8080) } }))) == "denied");
	CHECK(Code(publicBroker.call(Request("net.udp.connect", { { "host", Text("localhost") }, { "port", Int(9000) } }))) == "denied");
	CHECK(Code(publicBroker.call(Request("net.ws.connect", { { "url", Text("ws://localhost:8080/") } }))) == "denied");
	CHECK(publicOnly.transport.opened.empty());

	const auto tcp = broker.call(Request("net.tcp.connect", { { "host", Text("localhost") }, { "port", Int(8080) } }));
	CHECK(IsOk(tcp));
	CHECK(fixture.transport.opened.size() == 1 && fixture.transport.opened[0].local && !fixture.transport.opened[0].tls);
	CHECK(fixture.transport.opened[0].host == "localhost" && fixture.transport.opened[0].port == 8080);
	const auto tcpId = IsOk(tcp) ? Payload(tcp).find("id")->asInt().value_or(0) : 0;
	CHECK(IsOk(broker.call(Request("net.socket.send", { { "id", Int(tcpId) }, { "data", Text("ping") } }))));
	CHECK(IsOk(broker.call(Request("net.socket.close", { { "id", Int(tcpId) } }))));
	CHECK(Code(broker.call(Request("net.tcp.connect", { { "host", Text("127.0.0.1") }, { "port", Int(8081) } }))) == "denied");
	CHECK(Code(broker.call(Request("net.tcp.connect", { { "host", Text("example.com") }, { "port", Int(8080) } }))) == "denied");
	CHECK(IsOk(broker.call(Request("net.udp.connect", { { "host", Text("127.0.0.1") }, { "port", Int(9001) } }))));
	CHECK(fixture.transport.opened.back().local);
	CHECK(Code(broker.call(Request("net.udp.connect", { { "host", Text("127.0.0.1") }, { "port", Int(9000) }, { "tls", cbor::Value::FromBool(true) } }))) == "invalid");

	const auto plain = broker.call(Request("net.ws.connect", { { "url", Text("ws://localhost:9001/chat") } }));
	CHECK(IsOk(plain));
	CHECK(fixture.transport.opened.back().local && !fixture.transport.opened.back().tls);
	const auto request = std::string(fixture.transport.written.begin(), fixture.transport.written.end());
	CHECK(request.find("GET /chat HTTP/1.1\r\nHost: localhost:9001\r\n") != std::string::npos);
	fixture.transport.handshakeAnswered = false;
	const auto secure = broker.call(Request("net.ws.connect", { { "url", Text("wss://[::1]:9002/") } }));
	CHECK(IsOk(secure) && fixture.transport.opened.back().tls);
	CHECK(Code(broker.call(Request("net.ws.connect", { { "url", Text("ws://localhost:7000/") } }))) == "denied");
	CHECK(Code(broker.call(Request("net.ws.connect", { { "url", Text("ws://example.com/") } }))) == "invalid");
	CHECK(Code(broker.call(Request("net.ws.connect", { { "url", Text("https://localhost:9001/") } }))) == "invalid");
	const auto wsId = IsOk(plain) ? Payload(plain).find("id")->asInt().value_or(0) : 0;
	CHECK(IsOk(broker.call(Request("net.ws.send", { { "id", Int(wsId) }, { "data", Text("hi") } }))));

	auto noPorts = Fixture({ "net.local" });
	auto noPortsBroker = noPorts.make();
	CHECK(fetch(noPortsBroker, "http://localhost:8080/") == "denied");
	CHECK(Code(noPortsBroker.call(Request("net.tcp.connect", { { "host", Text("localhost") }, { "port", Int(8080) } }))) == "denied");
	auto empty = Fixture({});
	auto emptyBroker = empty.make();
	CHECK(fetch(emptyBroker, "http://localhost:8080/") == "denied");
	CHECK(Code(emptyBroker.call(Request("net.tcp.connect", { { "host", Text("localhost") }, { "port", Int(8080) } }))) == "denied");
}

void TestWebSocketPrimitives() {
	CHECK(ws::AcceptFor("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
	const auto hash = ws::Sha1("abc");
	CHECK(hash[0] == 0xA9 && hash[1] == 0x99 && hash[2] == 0x3E);
	CHECK(hash[19] == 0x9D);
	CHECK(ws::NewKey().size() == 24 && ws::NewKey() != ws::NewKey());

	auto frame = ws::Frame();
	auto parser = ws::Parser();
	const auto feed = [&](std::initializer_list<uint8_t> bytes) {
		const auto list = std::vector<uint8_t>(bytes);
		parser.feed(list);
	};
	feed({ 0x81, 0x02, 'h' });
	CHECK(parser.next(frame) == ws::Parser::Status::More);
	feed({ 'i', 0x89, 0x00 });
	CHECK(parser.next(frame) == ws::Parser::Status::Frame);
	CHECK(frame.fin && frame.opcode == ws::kOpText && frame.payload == (cbor::Bytes{ 'h', 'i' }));
	CHECK(parser.next(frame) == ws::Parser::Status::Frame && frame.opcode == ws::kOpPing);
	CHECK(parser.next(frame) == ws::Parser::Status::More);

	const auto broken = [&](std::initializer_list<uint8_t> bytes) {
		auto single = ws::Parser();
		const auto list = std::vector<uint8_t>(bytes);
		single.feed(list);
		auto out = ws::Frame();
		return single.next(out) == ws::Parser::Status::Error;
	};
	CHECK(broken({ 0x81, 0x82, 1, 2, 3, 4, 5, 6 }));
	CHECK(broken({ 0xC1, 0x00 }));
	CHECK(broken({ 0x09, 0x00 }));
	CHECK(broken({ 0x89, 126, 0, 200 }));
	CHECK(broken({ 0x83, 0x00 }));
	CHECK(broken({ 0x82, 127, 0, 0, 0, 0, 0, 0x20, 0, 0 }));
	CHECK(broken({ 0x82, 127, 0x80, 0, 0, 0, 0, 0, 0, 0 }));

	const auto valid = [](std::string_view text) {
		return ws::IsValidUtf8({ reinterpret_cast<const uint8_t*>(text.data()), text.size() });
	};
	CHECK(valid("plain") && valid("\xD0\x9F\xD1\x80") && valid("\xF0\x9F\x98\x80"));
	CHECK(!valid("\xC0\xAF") && !valid("\xED\xA0\x80") && !valid("\xE2\x82") && !valid("\xFF"));

	const auto encoded = ws::EncodeFrame(ws::kOpText, cbor::Bytes{ 'a', 'b' });
	CHECK(encoded.size() == 8 && encoded[0] == 0x81 && encoded[1] == (0x80 | 2));
	CHECK((encoded[6] ^ encoded[2]) == 'a' && (encoded[7] ^ encoded[3]) == 'b');
	const auto large = ws::EncodeFrame(ws::kOpBinary, cbor::Bytes(300, 1));
	CHECK(large[1] == (0x80 | 126) && large[2] == 1 && large[3] == 44);
}

void TestWebSocket() {
	const auto server = [](uint8_t opcode, cbor::Bytes payload, bool fin = true) {
		auto frame = cbor::Bytes{ uint8_t((fin ? 0x80 : 0) | opcode), uint8_t(payload.size()) };
		frame.insert(frame.end(), payload.begin(), payload.end());
		return frame;
	};
	const auto text = [](std::string_view value) {
		return cbor::Bytes(value.begin(), value.end());
	};
	const auto connect = [](Broker &broker, const char *url = "wss://example.com/chat", cbor::Map more = {}) {
		more.push_back({ "url", Text(url) });
		return broker.call(Request("net.ws.connect", std::move(more)));
	};

	auto denied = Fixture({});
	auto deniedBroker = denied.make();
	CHECK(Code(connect(deniedBroker)) == "denied");

	auto fixture = Fixture({ "net.ws" });
	fixture.transport.webSocket = true;
	auto broker = fixture.make();
	for (const auto bad : { "https://example.com/", "ws://example.com/", "wss://", "wss://192.168.1.1/", "wss://user@example.com/", "example.com" }) {
		CHECK(Code(connect(broker, bad)) == "invalid");
	}
	CHECK(Code(connect(broker, "wss://127.0.0.1/")) == "denied");
	CHECK(Code(connect(broker, "ws://localhost:8080/")) == "denied");
	CHECK(Code(connect(broker, "wss://example.com/", { { "headers", cbor::Value::FromMap({ { "Cookie", Text("a=b") } }) } })) == "invalid");
	CHECK(Code(connect(broker, "wss://example.com/", { { "protocols", cbor::Value::FromArray({ Text("bad name") }) } })) == "invalid");
	CHECK(fixture.transport.opened.empty());

	const auto opened = connect(broker, "wss://example.com/chat", {
		{ "headers", cbor::Value::FromMap({ { "X-Token", Text("abc") } }) },
	});
	CHECK(IsOk(opened));
	const auto id = IsOk(opened) ? Payload(opened).find("id")->asInt().value_or(0) : 0;
	CHECK(fixture.transport.opened.size() == 1 && fixture.transport.opened[0].tls);
	CHECK(fixture.transport.opened[0].host == "example.com" && fixture.transport.opened[0].port == 443);
	const auto request = std::string(fixture.transport.written.begin(), fixture.transport.written.end());
	CHECK(request.rfind("GET /chat HTTP/1.1\r\nHost: example.com\r\n", 0) == 0);
	CHECK(request.find("Upgrade: websocket\r\n") != std::string::npos);
	CHECK(request.find("Sec-WebSocket-Version: 13\r\n") != std::string::npos);
	CHECK(request.find("X-Token: abc\r\n") != std::string::npos);
	CHECK(request.find("Sec-WebSocket-Protocol") == std::string::npos);

	CHECK(Code(broker.call(Request("net.socket.send", { { "id", Int(id) }, { "data", Text("x") } }))) == "invalid");
	CHECK(Code(broker.call(Request("net.socket.recv", { { "id", Int(id) } }))) == "invalid");
	CHECK(Code(broker.call(Request("net.ws.send", { { "id", Int(999) }, { "data", Text("x") } }))) == "not_found");

	const auto before = fixture.transport.written.size();
	CHECK(IsOk(broker.call(Request("net.ws.send", { { "id", Int(id) }, { "data", Text("hello") } }))));
	CHECK(fixture.transport.written.size() == before + 11);
	CHECK(fixture.transport.written[before] == 0x81 && fixture.transport.written[before + 1] == (0x80 | 5));
	auto plain = std::string();
	for (auto i = 0; i != 5; ++i) {
		plain += char(fixture.transport.written[before + 6 + i] ^ fixture.transport.written[before + 2 + (i % 4)]);
	}
	CHECK(plain == "hello");
	CHECK(IsOk(broker.call(Request("net.ws.send", { { "id", Int(id) }, { "data", cbor::Value::FromBytes(cbor::Bytes{ 1, 2 }) } }))));
	CHECK(fixture.transport.written[before + 11] == 0x82);
	CHECK(Code(broker.call(Request("net.ws.send", { { "id", Int(id) }, { "data", Int(1) } }))) == "invalid");
	CHECK(Code(broker.call(Request("net.ws.send", { { "id", Int(id) }, { "data", cbor::Value::FromBytes(cbor::Bytes((1 << 20) + 1, 0)) } }))) == "limit");

	const auto recv = [&] {
		return broker.call(Request("net.ws.recv", { { "id", Int(id) }, { "timeout", Int(0) } }));
	};
	CHECK(*Payload(recv()).find("kind")->asText() == "none");
	fixture.transport.inbound.push_back(server(ws::kOpText, text("hey")));
	const auto first = recv();
	CHECK(IsOk(first) && *Payload(first).find("kind")->asText() == "text" && *Payload(first).find("data")->asText() == "hey");
	fixture.transport.inbound.push_back(server(ws::kOpBinary, { 1, 2 }));
	const auto second = recv();
	CHECK(*Payload(second).find("kind")->asText() == "binary" && *Payload(second).find("data")->asBytes() == (cbor::Bytes{ 1, 2 }));

	const auto written = fixture.transport.written.size();
	fixture.transport.inbound.push_back(server(ws::kOpPing, text("p")));
	fixture.transport.inbound.push_back(server(ws::kOpPong, {}));
	fixture.transport.inbound.push_back(server(ws::kOpText, text("a"), false));
	fixture.transport.inbound.push_back(server(ws::kOpContinue, text("b"), true));
	const auto joined = recv();
	CHECK(IsOk(joined) && *Payload(joined).find("data")->asText() == "ab");
	CHECK(fixture.transport.written.size() == written + 7 && fixture.transport.written[written] == 0x8A);

	fixture.transport.inbound.push_back(server(ws::kOpClose, { 0x03, 0xE9, 'b', 'y', 'e' }));
	const auto closing = recv();
	CHECK(IsOk(closing) && *Payload(closing).find("kind")->asText() == "close");
	CHECK(Payload(closing).find("code")->asInt() == 1001 && *Payload(closing).find("reason")->asText() == "bye");
	CHECK(Payload(closing).find("closed")->asBool() == true);
	CHECK(fixture.transport.written[fixture.transport.written.size() - 8] == 0x88);
	const auto again = recv();
	CHECK(*Payload(again).find("kind")->asText() == "close" && Payload(again).find("code")->asInt() == 1006);
	CHECK(Code(broker.call(Request("net.ws.send", { { "id", Int(id) }, { "data", Text("late") } }))) == "failed");
	CHECK(IsOk(broker.call(Request("net.ws.close", { { "id", Int(id) } }))));
	CHECK(fixture.transport.closed == (std::vector<int64_t>{ id }));
	CHECK(Code(broker.call(Request("net.ws.recv", { { "id", Int(id) } }))) == "not_found");

	const auto failing = [&](auto setup, cbor::Map more = {}) {
		auto bad = Fixture({ "net.ws" });
		bad.transport.webSocket = true;
		setup(bad.transport);
		auto badBroker = bad.make();
		const auto code = Code(connect(badBroker, "wss://example.com/", std::move(more)));
		return code == "failed" && bad.transport.closed.size() == 1;
	};
	CHECK(failing([](FakeTransport &t) { t.handshakeStatus = "HTTP/1.1 403 Forbidden"; }));
	CHECK(failing([](FakeTransport &t) { t.badAccept = true; }));
	CHECK(failing([](FakeTransport &t) { t.handshakeExtra = "Sec-WebSocket-Extensions: permessage-deflate\r\n"; }));
	CHECK(failing([](FakeTransport &t) { t.handshakeExtra = "Sec-WebSocket-Protocol: other\r\n"; },
		{ { "protocols", cbor::Value::FromArray({ Text("chat") }) } }));
	CHECK(failing([](FakeTransport &t) { t.fail = false; t.peerClosed = true; t.handshakeAnswered = true; }));

	auto chosen = Fixture({ "net.ws" });
	chosen.transport.webSocket = true;
	chosen.transport.handshakeExtra = "Sec-WebSocket-Protocol: chat\r\n";
	auto chosenBroker = chosen.make();
	const auto negotiated = connect(chosenBroker, "wss://example.com:8443/x?y=1", {
		{ "protocols", cbor::Value::FromArray({ Text("chat"), Text("super") }) },
	});
	CHECK(IsOk(negotiated) && *Payload(negotiated).find("protocol")->asText() == "chat");
	const auto head = std::string(chosen.transport.written.begin(), chosen.transport.written.end());
	CHECK(head.find("Host: example.com:8443\r\n") != std::string::npos);
	CHECK(head.find("GET /x?y=1 HTTP/1.1") == 0);
	CHECK(head.find("Sec-WebSocket-Protocol: chat, super\r\n") != std::string::npos);

	auto bad = Fixture({ "net.ws" });
	bad.transport.webSocket = true;
	auto badBroker = bad.make();
	const auto badId = Payload(connect(badBroker)).find("id")->asInt().value_or(0);
	bad.transport.inbound.push_back(server(ws::kOpText, { 0xC0, 0xAF }));
	const auto broken = badBroker.call(Request("net.ws.recv", { { "id", Int(badId) }, { "timeout", Int(0) } }));
	CHECK(Code(broken) == "failed");
	bad.transport.inbound.push_back(server(ws::kOpText, text("x")));
	CHECK(Code(badBroker.call(Request("net.ws.send", { { "id", Int(badId) }, { "data", Text("x") } }))) == "failed");

	auto many = Fixture({ "net.ws" });
	many.transport.webSocket = true;
	auto manyBroker = many.make();
	for (auto i = 0; i != 16; ++i) {
		many.transport.handshakeAnswered = false;
		CHECK(IsOk(connect(manyBroker)));
	}
	CHECK(Code(connect(manyBroker)) == "limit");
}

void TestModeration() {
	auto fixture = Fixture({ "chats.moderate" });
	fixture.time = 5000;
	auto broker = fixture.make();
	const auto ban = [&](int64_t chat, int64_t user, const char *op = "chats.ban") {
		fixture.time += 1500;
		return Code(broker.call(Request(op, { { "chat", Int(chat) }, { "user", Int(user) } })));
	};
	CHECK(ban(1, 7) == "denied");
	broker.noteUserAction("menu.click");
	CHECK(ban(1, 7).empty());
	CHECK(ban(1, 7, "chats.unban").empty());
	CHECK(ban(1, 0) == "invalid");
	CHECK(ban(1, -3) == "invalid");
	CHECK(ban(1, 42) == "denied");
	CHECK(ban(1, 777000) == "denied");
	CHECK(ban(777000, 7) == "denied");
	CHECK(ban(3, 7) == "not_found");
	CHECK(Code(broker.call(Request("chats.ban", { { "chat", Int(1) }, { "user", Int(7) } }))) == "limit");
	fixture.time += 1500;
	CHECK(Code(broker.call(Request("chats.ban", { { "chat", Int(1) } }))) == "invalid");
	CHECK(fixture.data.actions == (std::vector<std::string>{ "ban 1 7", "unban 1 7" }));
	fixture.time += 11000;
	CHECK(ban(1, 7) == "denied");
	auto other = Fixture({ "chats.manage" });
	other.time = 5000;
	auto otherBroker = other.make();
	otherBroker.noteUserAction("menu.click");
	CHECK(Code(otherBroker.call(Request("chats.ban", { { "chat", Int(1) }, { "user", Int(7) } }))) == "denied");
}

void TestBotButtons() {
	auto fixture = Fixture({ "bots.inline", "messages.send" });
	auto broker = fixture.make();
	fixture.time = 5000;
	const auto query = broker.call(Request("bots.inline_query", {
		{ "bot", Text("inlinebot") }, { "query", Text("cats") }, { "chat", Int(1) },
	}));
	CHECK(IsOk(query));
	CHECK(fixture.data.inlineQueries == (std::vector<std::string>{ "0/inlinebot/cats/1" }));
	CHECK((*Payload(query).asArray())[0].find("query_id")->asInt() == 77);
	CHECK(IsOk(broker.call(Request("bots.send_inline_result", {
		{ "chat", Int(1) }, { "query_id", Int(77) }, { "id", Text("r1") },
		{ "silent", cbor::Value::FromBool(true) }, { "reply_to", Int(100) },
	}))));
	CHECK(fixture.data.actions == (std::vector<std::string>{ "inline 1 77 r1 silent reply100" }));
	for (const auto &bad : {
		Request("bots.send_inline_result", { { "chat", Int(1) }, { "id", Text("r1") } }),
		Request("bots.send_inline_result", { { "chat", Int(1) }, { "query_id", Int(77) } }),
		Request("bots.send_inline_result", { { "chat", Int(1) }, { "query_id", Int(77) }, { "id", Text("") } }),
		Request("bots.send_inline_result", { { "chat", Int(1) }, { "query_id", Int(77) }, { "id", Text("r") }, { "reply_to", Int(-1) } }),
	}) {
		CHECK(Code(broker.call(bad)) == "invalid");
	}
	CHECK(Code(broker.call(Request("bots.send_inline_result", { { "chat", Int(777000) }, { "query_id", Int(77) }, { "id", Text("r") } }))) == "denied");
	fixture.time += 1500;
	CHECK(Code(broker.call(Request("bots.inline_query", { { "bot", Text("inlinebot") }, { "query", Text("x") }, { "chat", Int(777000) } }))) == "denied");

	fixture.time += 1500;
	const auto pressed = broker.call(Request("bots.press_button", {
		{ "chat", Int(1) }, { "id", Int(100) }, { "row", Int(0) }, { "column", Int(0) },
	}));
	CHECK(IsOk(pressed));
	CHECK(*Payload(pressed).find("text")->asText() == "Pressed" && Payload(pressed).find("alert")->asBool() == true);
	fixture.time += 1500;
	const auto link = broker.call(Request("bots.press_button", {
		{ "chat", Int(1) }, { "id", Int(100) }, { "row", Int(0) }, { "column", Int(1) },
	}));
	CHECK(IsOk(link) && *Payload(link).find("url")->asText() == "https://example.com/");
	fixture.time += 1500;
	CHECK(Code(broker.call(Request("bots.press_button", { { "chat", Int(1) }, { "id", Int(100) }, { "row", Int(3) }, { "column", Int(0) } }))) == "not_found");
	fixture.time += 1500;
	CHECK(Code(broker.call(Request("bots.press_button", { { "chat", Int(1) }, { "id", Int(100) }, { "row", Int(-1) }, { "column", Int(0) } }))) == "invalid");
	fixture.time += 1500;
	CHECK(Code(broker.call(Request("bots.press_button", { { "chat", Int(1) }, { "id", Int(0) }, { "row", Int(0) }, { "column", Int(0) } }))) == "invalid");
	CHECK(Code(broker.call(Request("bots.press_button", { { "chat", Int(1) }, { "id", Int(100) }, { "row", Int(0) }, { "column", Int(0) } }))) == "limit");

	auto onlyInline = Fixture({ "bots.inline" });
	auto onlyInlineBroker = onlyInline.make();
	CHECK(Code(onlyInlineBroker.call(Request("bots.send_inline_result", { { "chat", Int(1) }, { "query_id", Int(77) }, { "id", Text("r1") } }))) == "denied");
	auto onlySend = Fixture({ "messages.send" });
	auto onlySendBroker = onlySend.make();
	CHECK(Code(onlySendBroker.call(Request("bots.send_inline_result", { { "chat", Int(1) }, { "query_id", Int(77) }, { "id", Text("r1") } }))) == "denied");
}

void TestEntitiesInMessages() {
	auto fixture = Fixture({ "messages.send", "messages.edit" });
	auto broker = fixture.make();
	const auto bold = cbor::Value::FromMap({ { "type", Text("bold") }, { "offset", Int(0) }, { "length", Int(2) } });
	const auto mention = cbor::Value::FromMap({
		{ "type", Text("mention_name") }, { "offset", Int(3) }, { "length", Int(3) }, { "id", Int(7) },
	});
	CHECK(IsOk(broker.call(Request("messages.send", {
		{ "chat", Int(1) }, { "text", Text("hi bob") },
		{ "entities", cbor::Value::FromArray({ bold, mention }) },
	}))));
	CHECK(fixture.data.outgoing.size() == 1 && fixture.data.outgoing[0].entities.size() == 2);
	if (fixture.data.outgoing.size() == 1 && fixture.data.outgoing[0].entities.size() == 2) {
		CHECK(fixture.data.outgoing[0].entities[1].type == "mention_name" && fixture.data.outgoing[0].entities[1].id == 7);
	}
	CHECK(Code(broker.call(Request("messages.send", {
		{ "chat", Int(1) }, { "text", Text("x") }, { "format", Text("markdown") },
		{ "entities", cbor::Value::FromArray({ bold }) },
	}))) == "invalid");
	CHECK(Code(broker.call(Request("messages.send", {
		{ "chat", Int(1) }, { "text", Text("x") },
		{ "entities", cbor::Value::FromArray({ cbor::Value::FromMap({ { "type", Text("mention_name") }, { "offset", Int(0) }, { "length", Int(1) } }) }) },
	}))) == "invalid");
	CHECK(IsOk(broker.call(Request("messages.send", {
		{ "chat", Int(1) }, { "text", Text("see #tag") },
		{ "entities", cbor::Value::FromArray({ cbor::Value::FromMap({ { "type", Text("hashtag") }, { "offset", Int(4) }, { "length", Int(4) } }) }) },
	}))));
	CHECK(IsOk(broker.call(Request("messages.edit", {
		{ "chat", Int(1) }, { "id", Int(100) }, { "text", Text("new") },
		{ "entities", cbor::Value::FromArray({ bold }) },
	}))));
	CHECK(fixture.data.actions.size() == 1 && fixture.data.actions[0] == "edit 1 100 new e1");
	CHECK(Code(broker.call(Request("messages.edit", {
		{ "chat", Int(1) }, { "id", Int(100) }, { "text", Text("new") }, { "format", Text("markdown") },
		{ "entities", cbor::Value::FromArray({ bold }) },
	}))) == "invalid");
	CHECK(Code(broker.call(Request("messages.edit", {
		{ "chat", Int(1) }, { "id", Int(100) }, { "text", Text("new") }, { "entities", Text("bold") },
	}))) == "invalid");
}

void TestPluginMessages() {
	auto sent = std::vector<std::pair<std::string, cbor::Value>>();
	auto fixture = Fixture({ "plugins.message" });
	fixture.services.sendToPlugin = [&](const std::string &to, cbor::Value data) {
		sent.emplace_back(to, std::move(data));
		return (to == "com.example.missing") ? std::string("not_found") : std::string();
	};
	fixture.services.pluginPeers = [] {
		return std::vector<std::string>{ "com.example.friend" };
	};
	auto broker = fixture.make();
	const auto ok = broker.call(Request("plugins.send", {
		{ "to", Text("com.example.friend") },
		{ "data", cbor::Value::FromMap({ { "hello", Int(1) } }) },
	}));
	CHECK(IsOk(ok));
	CHECK(sent.size() == 1 && sent[0].first == "com.example.friend");
	CHECK(sent[0].second.find("hello")->asInt() == 1);
	CHECK(Code(broker.call(Request("plugins.send", { { "to", Text("com.example.missing") } }))) == "not_found");
	CHECK(Code(broker.call(Request("plugins.send", { { "to", Text("com.example.test") } }))) == "invalid");
	CHECK(Code(broker.call(Request("plugins.send", {}))) == "invalid");
	CHECK(Code(broker.call(Request("plugins.send", {
		{ "to", Text("com.example.friend") },
		{ "data", cbor::Value::FromBytes(cbor::Bytes(70000, 1)) },
	}))) == "limit");
	const auto peers = broker.call(Request("plugins.peers"));
	CHECK(IsOk(peers) && Payload(peers).asArray()->size() == 1);

	auto denied = Fixture({});
	denied.services.sendToPlugin = fixture.services.sendToPlugin;
	denied.services.pluginPeers = fixture.services.pluginPeers;
	auto deniedBroker = denied.make();
	CHECK(Code(deniedBroker.call(Request("plugins.send", { { "to", Text("com.example.friend") } }))) == "denied");
	CHECK(Code(deniedBroker.call(Request("plugins.peers"))) == "denied");
	CHECK(sent.size() == 2);
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
	CHECK(Payload(history).asArray()->front().find("id")->asInt() == 100);
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

[[nodiscard]] cbor::Value Item(std::vector<std::pair<std::string, cbor::Value>> fields) {
	auto map = cbor::Map();
	for (auto &field : fields) {
		map.push_back({ field.first, std::move(field.second) });
	}
	return cbor::Value::FromMap(std::move(map));
}

[[nodiscard]] cbor::Value Page(std::vector<cbor::Value> items, const std::string &title = "Demo") {
	return Item({
		{ "title", Text(title) },
		{ "items", cbor::Value::FromArray(std::move(items)) },
	});
}

void TestRichIconsAndSubPages() {
	auto fixture = Fixture({ "ui.settings_page" });
	auto received = std::vector<cbor::Value>();
	fixture.services.ui = [&](std::string_view, const cbor::Value &page) {
		received.push_back(page);
		return true;
	};
	auto broker = fixture.make();
	const auto call = [&](cbor::Value page) {
		return Code(broker.call(Request("ui.settings.set", { { "page", std::move(page) } })));
	};
	const auto button = [](const char *id, const char *icon) {
		return Item({ { "type", Text("button") }, { "id", Text(id) }, { "label", Text("x") }, { "icon", Text(icon) } });
	};
	CHECK(call(Page({ button("a", "peer:777"), button("b", "emoji:5368324170671202286") })) == "");
	CHECK(call(Page({ button("a", "peer:") })) == "invalid");
	CHECK(call(Page({ button("a", "peer:-5") })) == "invalid");
	CHECK(call(Page({ button("a", "peer:12x") })) == "invalid");
	CHECK(call(Page({ button("a", "sticker:1") })) == "invalid");
	CHECK(call(Page({ button("a", "emoji:123456789012345678901") })) == "invalid");
	auto many = std::vector<cbor::Value>();
	for (auto i = 0; i != 33; ++i) {
		many.push_back(button(("b" + std::to_string(i)).c_str(), "emoji:5"));
	}
	CHECK(call(Page(std::move(many))) == "invalid");
	auto allowed = std::vector<cbor::Value>();
	for (auto i = 0; i != 32; ++i) {
		allowed.push_back(button(("b" + std::to_string(i)).c_str(), "emoji:5"));
	}
	CHECK(call(Page(std::move(allowed))) == "");

	const auto subpage = [&](const char *id, std::vector<cbor::Value> items) {
		return Item({
			{ "type", Text("page") }, { "id", Text(id) }, { "label", Text("More") },
			{ "icon", Text("settings") }, { "hint", Text("Extra options") },
			{ "items", cbor::Value::FromArray(std::move(items)) },
		});
	};
	received.clear();
	const auto toggle = Item({ { "type", Text("toggle") }, { "id", Text("deep") }, { "label", Text("Deep") } });
	CHECK(call(Page({ subpage("more", { toggle }) })) == "");
	const auto &first = received.back().find("items")->asArray()->front();
	CHECK(*first.find("label")->asText() == "More" && first.find("items")->asArray()->size() == 1);
	CHECK(call(Page({ subpage("more", {}) })) == "invalid");
	CHECK(call(Page({ subpage("more", { Item({ { "type", Text("divider") } }) }), toggle })) == "");
	CHECK(call(Page({ subpage("more", { toggle }), toggle })) == "invalid");
	CHECK(call(Page({ Item({ { "type", Text("page") }, { "id", Text("p") },
		{ "items", cbor::Value::FromArray({ toggle }) } }) })) == "invalid");
	CHECK(call(Page({ subpage("one", { subpage("two", { subpage("three", { subpage("four", { subpage("five", { toggle }) }) }) }) }) })) == "invalid");
	CHECK(call(Page({ subpage("one", { subpage("two", { toggle }) }) })) == "");
}

void TestSettingsPage() {
	auto fixture = Fixture({ "ui.settings_page" });
	auto received = std::vector<cbor::Value>();
	fixture.services.ui = [&](std::string_view kind, const cbor::Value &page) {
		received.push_back(page);
		return kind == "settings";
	};
	auto broker = fixture.make();
	const auto call = [&](cbor::Value page) {
		return broker.call(Request("ui.settings.set", { { "page", std::move(page) } }));
	};
	const auto good = Page({
		Item({ { "type", Text("text") }, { "text", Text("Line one\nline two") } }),
		Item({ { "type", Text("toggle") }, { "id", Text("loud") }, { "label", Text("Loud") }, { "value", cbor::Value::FromBool(true) } }),
		Item({ { "type", Text("input") }, { "id", Text("name") }, { "label", Text("Name") } }),
		Item({ { "type", Text("select") }, { "id", Text("mode") }, { "label", Text("Mode") },
			{ "options", cbor::Value::FromArray({
				Item({ { "value", Text("a") }, { "label", Text("A") } }),
				Item({ { "value", Text("b") }, { "label", Text("B") } }),
			}) } }),
		Item({ { "type", Text("button") }, { "id", Text("go") }, { "label", Text("Go") } }),
		Item({ { "type", Text("divider") } }),
	});
	CHECK(IsOk(call(good)));
	CHECK(received.size() == 1);
	const auto &items = *received[0].find("items")->asArray();
	CHECK(items.size() == 6);
	CHECK(*items[2].find("value")->asText() == "");
	CHECK(*items[3].find("value")->asText() == "a");

	const auto bad = [&](cbor::Value page) {
		return Code(call(std::move(page))) == "invalid";
	};
	CHECK(bad(Page({ Item({ { "type", Text("select") }, { "id", Text("mode") }, { "label", Text("Mode") },
		{ "value", Text("nope") },
		{ "options", cbor::Value::FromArray({ Item({ { "value", Text("a") }, { "label", Text("A") } }) }) } }) })));
	CHECK(bad(Page({ Item({ { "type", Text("text") }, { "text", Text("bad \xe2\x80\xa8 line") } }) })));
	CHECK(bad(Page({ Item({ { "type", Text("text") }, { "text", Text("bad \xff byte") } }) })));
	CHECK(bad(Page({ Item({ { "type", Text("webview") } }) })));
	CHECK(bad(Page({ Item({ { "type", Text("button") }, { "id", Text("Go") }, { "label", Text("x") } }) })));
	CHECK(bad(Page({
		Item({ { "type", Text("button") }, { "id", Text("go") }, { "label", Text("x") } }),
		Item({ { "type", Text("button") }, { "id", Text("go") }, { "label", Text("y") } }),
	})));
	CHECK(bad(Page({ Item({ { "type", Text("button") }, { "id", Text("go") }, { "label", Text("x") }, { "script", Text("1") } }) })));
	CHECK(bad(Page({ Item({ { "type", Text("text") }, { "text", Text(std::string(2000, 'x')) } }) })));
	CHECK(bad(Page({ Item({ { "type", Text("text") }, { "text", Text("a\x01") } }) })));
	CHECK(bad(Page({ Item({ { "type", Text("text") }, { "text", Text("evil \xE2\x80\xAE text") } }) })));
	CHECK(bad(Page({}, "bad \xE2\x81\xA6")));
	CHECK(bad(Page({ Item({ { "type", Text("select") }, { "id", Text("m") }, { "label", Text("m") }, { "options", cbor::Value::FromArray({}) } }) })));
	auto many = std::vector<cbor::Value>();
	for (auto i = 0; i != 65; ++i) {
		many.push_back(Item({ { "type", Text("divider") } }));
	}
	CHECK(bad(Page(std::move(many))));
	CHECK(bad(Text("not a page")));
	CHECK(received.size() == 1);

	auto rich = Page({
		Item({ { "type", Text("header") }, { "text", Text("Section") } }),
		Item({ { "type", Text("link") }, { "label", Text("Docs") }, { "url", Text("https://example.com/docs") } }),
		Item({ { "type", Text("slider") }, { "id", Text("volume") }, { "label", Text("Volume") },
			{ "min", Int(0) }, { "max", Int(100) }, { "step", Int(10) }, { "value", Int(57) },
			{ "hint", Text("How loud") } }),
		Item({ { "type", Text("input") }, { "id", Text("notes") }, { "label", Text("Notes") },
			{ "multiline", cbor::Value::FromBool(true) }, { "max", Int(3000) },
			{ "value", Text("one\ntwo") }, { "disabled", cbor::Value::FromBool(true) } }),
		Item({ { "type", Text("input") }, { "id", Text("token") }, { "label", Text("Token") },
			{ "password", cbor::Value::FromBool(true) } }),
		Item({ { "type", Text("chat") }, { "id", Text("target") }, { "label", Text("Chat") }, { "value", Int(1) } }),
		Item({ { "type", Text("progress") }, { "label", Text("Done") }, { "value", Int(40) } }),
	});
	auto withButtons = cbor::Map(*rich.asMap());
	withButtons.push_back({ "buttons", cbor::Value::FromArray({
		Item({ { "id", Text("ok") }, { "label", Text("OK") }, { "style", Text("primary") } }),
		Item({ { "id", Text("cancel") }, { "label", Text("Cancel") } }),
	}) });
	CHECK(IsOk(call(cbor::Value::FromMap(std::move(withButtons)))));
	CHECK(received.size() == 2);
	const auto &richItems = *received[1].find("items")->asArray();
	CHECK(*richItems[2].find("value")->asInt() == 50);
	CHECK(*richItems[2].find("hint")->asText() == "How loud");
	CHECK(*richItems[3].find("multiline")->asBool() && *richItems[3].find("disabled")->asBool());
	CHECK(*richItems[3].find("max")->asInt() == 3000);
	CHECK(*richItems[4].find("password")->asBool());
	CHECK(received[1].find("buttons")->asArray()->size() == 2);
	CHECK(bad(Page({ Item({ { "type", Text("link") }, { "label", Text("x") }, { "url", Text("http://example.com") } }) })));
	CHECK(bad(Page({ Item({ { "type", Text("slider") }, { "id", Text("s") }, { "label", Text("s") },
		{ "min", Int(5) }, { "max", Int(5) } }) })));
	CHECK(bad(Page({ Item({ { "type", Text("slider") }, { "id", Text("s") }, { "label", Text("s") },
		{ "min", Int(0) }, { "max", Int(100000) } }) })));
	CHECK(bad(Page({ Item({ { "type", Text("slider") }, { "id", Text("s") }, { "label", Text("s") },
		{ "min", Int(0) }, { "max", Int(5) }, { "step", Int(10) } }) })));
	CHECK(bad(Page({ Item({ { "type", Text("input") }, { "id", Text("i") }, { "label", Text("i") },
		{ "value", Text("a\nb") } }) })));
	CHECK(bad(Page({ Item({ { "type", Text("progress") }, { "value", Int(101) } }) })));
	CHECK(bad(Page({ Item({ { "type", Text("button") }, { "id", Text("b") }, { "label", Text("b") },
		{ "icon", Text("Not Valid") } }) })));
	CHECK(IsOk(call(Page({ Item({ { "type", Text("button") }, { "id", Text("b") }, { "label", Text("b") },
		{ "icon", Text("star") } }) }))));
	CHECK(*received.back().find("items")->asArray()->front().find("icon")->asText() == "star");
	received.pop_back();
	CHECK(bad(Page({ Item({ { "type", Text("button") }, { "id", Text("b") }, { "label", Text("b") },
		{ "style", Text("rainbow") } }) })));
	auto clash = cbor::Map(*Page({ Item({ { "type", Text("button") }, { "id", Text("ok") }, { "label", Text("b") } }) }).asMap());
	clash.push_back({ "buttons", cbor::Value::FromArray({ Item({ { "id", Text("ok") }, { "label", Text("OK") } }) }) });
	CHECK(bad(cbor::Value::FromMap(std::move(clash))));
	CHECK(received.size() == 2);

	auto denied = Fixture({ "ui.toast" });
	denied.services.ui = fixture.services.ui;
	auto refused = denied.make();
	CHECK(Code(refused.call(Request("ui.settings.set", { { "page", good } }))) == "denied");
	auto none = Fixture({ "ui.settings_page" });
	auto unavailable = none.make();
	CHECK(Code(unavailable.call(Request("ui.settings.set", { { "page", good } }))) == "unavailable");
}

[[nodiscard]] cbor::Value Png(uint32_t width, uint32_t height) {
	auto bytes = cbor::Bytes{
		0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
		0, 0, 0, 13, 'I', 'H', 'D', 'R',
	};
	for (const auto number : { width, height }) {
		for (auto shift = 24; shift >= 0; shift -= 8) {
			bytes.push_back(uint8_t(number >> shift));
		}
	}
	return cbor::Value::FromBytes(std::move(bytes));
}

[[nodiscard]] cbor::Value Label(const char *id, const char *text) {
	return Item({ { "type", Text("button") }, { "id", Text(id) }, { "label", Text(text) } });
}

void TestBadges() {
	auto fixture = Fixture({ "ui.badges" });
	auto received = std::vector<std::pair<std::string, cbor::Value>>();
	fixture.services.ui = [&](std::string_view kind, const cbor::Value &payload) {
		received.emplace_back(std::string(kind), payload);
		return kind == "badges" || kind == "badges.clear";
	};
	auto broker = fixture.make();
	const auto badge = [](std::string id, std::vector<std::pair<std::string, cbor::Value>> more) {
		auto map = cbor::Map();
		map.push_back({ "id", Text(id) });
		for (auto &entry : more) {
			map.push_back({ std::move(entry.first), std::move(entry.second) });
		}
		return cbor::Value::FromMap(std::move(map));
	};
	const auto set = [&](int64_t user, std::vector<cbor::Value> items) {
		return broker.call(Request("badges.set", {
			{ "user", cbor::Value::FromInt(user) },
			{ "items", cbor::Value::FromArray(std::move(items)) },
		}));
	};
	const auto star = badge("star", { { "icon", Text("star") }, { "tooltip", Text("A star") } });
	CHECK(IsOk(set(5, { star })));
	CHECK(received.size() == 1 && received.back().first == "badges");
	CHECK(*received.back().second.find("user")->asInt() == 5);
	CHECK(*received.back().second.find("items")->asArray()->front().find("tooltip")->asText() == "A star");
	CHECK(IsOk(set(5, { badge("pic", { { "image", Png(64, 64) } }) })));
	CHECK(IsOk(set(5, {})));
	CHECK(Code(set(5, { badge("pic", { { "image", Png(512, 512) } }) })) == "invalid");
	CHECK(Code(set(5, { badge("pic", { { "icon", Text("star") }, { "image", Png(8, 8) } }) })) == "invalid");
	CHECK(Code(set(5, { badge("none", {}) })) == "invalid");
	CHECK(Code(set(5, { badge("Bad Id", { { "icon", Text("star") } }) })) == "invalid");
	CHECK(Code(set(5, { star, star })) == "invalid");
	CHECK(Code(set(5, { badge("tip", { { "icon", Text("star") }, { "tooltip", Text(std::string(100, 'x')) } }) })) == "invalid");
	CHECK(Code(set(5, { badge("x", { { "icon", Text("star") }, { "extra", Text("y") } }) })) == "invalid");
	auto many = std::vector<cbor::Value>();
	for (auto i = 0; i != 5; ++i) {
		many.push_back(badge("b" + std::to_string(i), { { "icon", Text("star") } }));
	}
	CHECK(Code(set(5, std::move(many))) == "invalid");
	CHECK(Code(set(0, { star })) == "invalid");
	CHECK(Code(set(777000, { star })) == "denied");

	fixture.time += 1500;
	for (auto user = int64_t(100); user != 100 + 256; ++user) {
		if (user % 100 == 0) {
			fixture.time += 1500;
		}
		CHECK(IsOk(set(user, { star })));
	}
	fixture.time += 1500;
	CHECK(Code(set(1000, { star })) == "quota");
	CHECK(IsOk(set(100, { star })));
	CHECK(IsOk(set(100, {})));
	CHECK(IsOk(set(1000, { star })));

	received.clear();
	CHECK(IsOk(broker.call(Request("badges.clear"))));
	CHECK(received.size() == 1 && received.back().first == "badges.clear");

	auto denied = Fixture({ "ui.toast" });
	denied.services.ui = fixture.services.ui;
	auto refused = denied.make();
	CHECK(Code(refused.call(Request("badges.set", {
		{ "user", cbor::Value::FromInt(5) },
		{ "items", cbor::Value::FromArray({ star }) },
	}))) == "denied");
	auto none = Fixture({ "ui.badges" });
	auto unavailable = none.make();
	CHECK(Code(unavailable.call(Request("badges.set", {
		{ "user", cbor::Value::FromInt(5) },
		{ "items", cbor::Value::FromArray({ star }) },
	}))) == "unavailable");
}

void TestTemplateTree() {
	auto fixture = Fixture({ "ui.dialog" });
	auto received = std::vector<cbor::Value>();
	fixture.services.ui = [&](std::string_view kind, const cbor::Value &payload) {
		received.push_back(payload);
		return kind == "dialog";
	};
	auto broker = fixture.make();
	const auto call = [&](cbor::Value page) {
		return broker.call(Request("ui.dialog.show", { { "page", std::move(page) } }));
	};
	const auto array = [](std::vector<cbor::Value> list) {
		return cbor::Value::FromArray(std::move(list));
	};
	const auto good = Page({
		Item({ { "type", Text("text") }, { "text", Text("Big") }, { "style", Text("title") } }),
		Item({ { "type", Text("row") }, { "items", array({
			Label("a", "A"),
			Label("b", "B"),
		}) } }),
		Item({ { "type", Text("card") }, { "title", Text("Card") }, { "items", array({
			Item({ { "type", Text("code") }, { "text", Text("x = 1\ny = 2") } }),
			Item({ { "type", Text("column") }, { "items", array({ Label("c", "C") }) } }),
		}) } }),
		Item({ { "type", Text("tabs") }, { "id", Text("tabs") }, { "tabs", array({
			Item({ { "id", Text("one") }, { "label", Text("One") }, { "items", array({
				Item({ { "type", Text("divider") } }),
			}) } }),
			Item({ { "id", Text("two") }, { "label", Text("Two") }, { "items", array({ Label("d", "D") }) } }),
		}) }, { "value", Text("two") } }),
		Item({ { "type", Text("image") }, { "data", Png(64, 32) }, { "width", Int(64) }, { "alt", Text("logo") } }),
		Item({ { "type", Text("list") }, { "items", array({
			Item({ { "id", Text("first") }, { "title", Text("First") }, { "subtitle", Text("sub") } }),
			Item({ { "id", Text("second") }, { "title", Text("Second") }, { "icon", Text("star") } }),
		}) } }),
		Item({ { "type", Text("table") },
			{ "columns", array({ Text("Name"), Text("Qty") }) },
			{ "rows", array({ array({ Text("a"), Text("1") }), array({ Text("b"), Text("2") }) }) } }),
	});
	CHECK(IsOk(call(good)));
	CHECK(received.size() == 1);
	const auto &items = *received[0].find("items")->asArray();
	CHECK(items.size() == 7);
	CHECK(*items[0].find("style")->asText() == "title");
	CHECK(items[1].find("items")->asArray()->size() == 2);
	CHECK(*items[3].find("value")->asText() == "two");
	CHECK(items[4].find("height")->asInt() == 0);
	CHECK(*items[5].find("items")->asArray()->at(0).find("subtitle")->asText() == "sub");

	const auto bad = [&](cbor::Value page) {
		return Code(call(std::move(page))) == "invalid";
	};
	const auto wrap = [&](cbor::Value inner) { return Page({ std::move(inner) }); };
	auto deep = Label("deep", "x");
	for (auto i = 0; i != 3; ++i) {
		deep = Item({ { "type", Text("column") }, { "items", array({ std::move(deep) }) } });
	}
	CHECK(IsOk(call(wrap(deep))));
	CHECK(bad(wrap(Item({ { "type", Text("column") }, { "items", array({ deep }) } }))));
	CHECK(bad(Page({ Label("a", "A"), Item({ { "type", Text("row") }, { "items", array({ Label("a", "A2") }) } }) })));
	CHECK(bad(wrap(Item({ { "type", Text("row") }, { "items", array({}) } }))));
	CHECK(bad(wrap(Item({ { "type", Text("row") }, { "items", array({
		Label("a", "A"), Label("b", "B"), Label("c", "C"), Label("d", "D"), Label("e", "E"),
	}) } }))));
	CHECK(bad(wrap(Item({ { "type", Text("row") }, { "items", array({ Label("a", "A") }) }, { "gap", Int(4) } }))));
	CHECK(bad(wrap(Item({ { "type", Text("tabs") }, { "id", Text("t") }, { "value", Text("nope") }, { "tabs", array({
		Item({ { "id", Text("one") }, { "label", Text("One") }, { "items", array({}) } }),
	}) } }))));
	CHECK(bad(wrap(Item({ { "type", Text("text") }, { "text", Text("x") }, { "style", Text("blink") } }))));
	CHECK(bad(wrap(Item({ { "type", Text("image") }, { "data", Png(4096, 4) } }))));
	CHECK(bad(wrap(Item({ { "type", Text("image") }, { "data", Png(0, 4) } }))));
	CHECK(bad(wrap(Item({ { "type", Text("image") }, { "data", cbor::Value::FromBytes({ '<', 's', 'v', 'g', '>' }) } }))));
	CHECK(bad(wrap(Item({ { "type", Text("image") }, { "data", Png(8, 8) }, { "width", Int(5000) } }))));
	CHECK(bad(wrap(Item({ { "type", Text("image") }, { "data", Text("http://example.org/a.png") } }))));
	auto huge = cbor::Bytes{ 0xFF, 0xD8, 0xFF };
	huge.resize(300 * 1024);
	CHECK(bad(wrap(Item({ { "type", Text("image") }, { "data", cbor::Value::FromBytes(std::move(huge)) } }))));
	CHECK(IsOk(call(wrap(Item({ { "type", Text("image") }, { "data", cbor::Value::FromBytes({ 0xFF, 0xD8, 0xFF, 0xE0 }) } })))));
	CHECK(bad(wrap(Item({ { "type", Text("table") },
		{ "columns", array({ Text("A"), Text("B") }) },
		{ "rows", array({ array({ Text("only one") }) }) } }))));
	CHECK(bad(wrap(Item({ { "type", Text("table") }, { "columns", array({}) }, { "rows", array({}) } }))));
	CHECK(bad(wrap(Item({ { "type", Text("list") }, { "items", array({}) } }))));
	CHECK(bad(wrap(Item({ { "type", Text("list") }, { "items", array({
		Item({ { "id", Text("x") }, { "title", Text("X") }, { "icon", Text("Not Valid") } }),
	}) } }))));
	auto many = std::vector<cbor::Value>();
	for (auto i = 0; i != 5; ++i) {
		auto cells = std::vector<cbor::Value>();
		for (auto k = 0; k != 4; ++k) {
			auto column = std::vector<cbor::Value>();
			for (auto j = 0; j != 14; ++j) {
				column.push_back(Item({ { "type", Text("divider") } }));
			}
			cells.push_back(Item({ { "type", Text("column") }, { "items", array(std::move(column)) } }));
		}
		many.push_back(Item({ { "type", Text("row") }, { "items", array(std::move(cells)) } }));
	}
	CHECK(bad(Page(std::move(many))));
}

void TestWindows() {
	auto fixture = Fixture({ "ui.dialog" });
	fixture.time = 100000;
	auto calls = std::vector<std::pair<std::string, cbor::Value>>();
	fixture.services.ui = [&](std::string_view kind, const cbor::Value &payload) {
		calls.emplace_back(std::string(kind), payload);
		return true;
	};
	auto broker = fixture.make();
	const auto page = Page({ Label("go", "Go") });
	const auto show = [&](const char *id, const char *mode = nullptr, const char *op = "ui.window.show") {
		auto args = cbor::Map{
			{ "id", Text(id) },
			{ "page", page },
		};
		if (mode) {
			args.push_back({ "mode", Text(mode) });
		}
		return broker.call(Request(op, std::move(args)));
	};
	const auto close = [&](const char *id) {
		return broker.call(Request("ui.window.close", { { "id", Text(id) } }));
	};
	CHECK(Code(show("Bad Id")) == "invalid");
	CHECK(Code(show("w", "fullscreen")) == "invalid");
	CHECK(Code(show("w", nullptr, "ui.window.update")) == "not_found");
	CHECK(calls.empty());

	CHECK(IsOk(show("w", "window")));
	CHECK(calls.size() == 1 && calls[0].first == "window");
	CHECK(*calls[0].second.find("id")->asText() == "w");
	CHECK(*calls[0].second.find("mode")->asText() == "window");
	CHECK(Code(show("w", "layer", "ui.window.update")) == "limit");
	fixture.time += 100;
	CHECK(IsOk(show("w", "layer", "ui.window.update")));
	CHECK(*calls.back().second.find("mode")->asText() == "window");

	CHECK(Code(show("x")) == "limit");
	broker.noteUserAction("menu.click");
	CHECK(IsOk(show("x")));
	CHECK(IsOk(show("y")));
	CHECK(Code(show("z")) == "limit");
	CHECK(IsOk(close("y")));
	CHECK(calls.back().first == "window.close");
	CHECK(IsOk(show("z")));
	CHECK(Code(show("y")) == "limit");
	broker.noteUserAction("window.close", "z");
	CHECK(IsOk(show("y")));
	CHECK(Code(close("Bad Id")) == "invalid");

	auto denied = Fixture({ "ui.toast" });
	denied.services.ui = fixture.services.ui;
	auto refused = denied.make();
	CHECK(Code(refused.call(Request("ui.window.show", { { "id", Text("w") }, { "page", page } }))) == "denied");
	auto none = Fixture({ "ui.dialog" });
	auto unavailable = none.make();
	CHECK(Code(unavailable.call(Request("ui.window.show", { { "id", Text("w") }, { "page", page } }))) == "unavailable");
}

void TestTargetedEvents() {
	auto fixture = Fixture({ "ui.menu" });
	fixture.grants.chats = std::set<int64_t>{ 1 };
	auto broker = fixture.make();
	const auto click = [&](int64_t chat, bool secret) {
		return Item({
			{ "type", Text("menu.click") }, { "surface", Text("message") }, { "id", Text("x") },
			{ "chat", Int(chat) }, { "message", Int(5) }, { "from", Int(9) }, { "text", Text("hello") },
			{ "secret", cbor::Value::FromBool(secret) },
		});
	};
	const auto inScope = broker.filterTargeted(click(1, false));
	CHECK(inScope.find("chat") && !inScope.find("message") && !inScope.find("from"));
	CHECK(!inScope.find("text") && !inScope.find("secret"));
	auto readingFixture = Fixture({ "ui.menu", "messages.read" });
	readingFixture.grants.chats = std::set<int64_t>{ 1 };
	const auto full = readingFixture.make().filterTargeted(click(1, false));
	CHECK(full.find("message") && full.find("from") && full.find("text"));
	CHECK(inScope.find("id") && inScope.find("surface"));
	const auto outside = broker.filterTargeted(click(2, false));
	CHECK(!outside.find("chat") && !outside.find("message") && !outside.find("text"));
	CHECK(outside.find("id") != nullptr);
	CHECK(!broker.filterTargeted(click(1, true)).find("chat"));
	CHECK(!broker.filterTargeted(click(api::kServiceChat, false)).find("chat"));

	auto reading = Fixture({ "ui.menu", "messages.read" });
	auto reader = reading.make();
	CHECK(reader.filterTargeted(click(1, false)).find("text") != nullptr);
	CHECK(reader.filterTargeted(click(1, true)).find("text") == nullptr);

	const auto selection = [&](int64_t chat, bool secret) {
		return Item({
			{ "type", Text("menu.click") }, { "surface", Text("selection") }, { "id", Text("x") },
			{ "chat", Int(chat) }, { "ids", cbor::Value::FromArray({ Int(5), Int(6) }) },
			{ "secret", cbor::Value::FromBool(secret) },
		});
	};
	CHECK(!broker.filterTargeted(selection(1, false)).find("ids"));
	CHECK(broker.filterTargeted(selection(1, false)).find("chat") != nullptr);
	const auto listed = readingFixture.make().filterTargeted(selection(1, false));
	CHECK(listed.find("ids") && listed.find("ids")->asArray()->size() == 2);
	CHECK(!readingFixture.make().filterTargeted(selection(2, false)).find("ids"));
	CHECK(!readingFixture.make().filterTargeted(selection(1, true)).find("ids"));
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

[[nodiscard]] cbor::Value Texts(std::vector<std::string> items) {
	auto list = cbor::Array();
	for (auto &item : items) {
		list.push_back(Text(std::move(item)));
	}
	return cbor::Value::FromArray(std::move(list));
}

void TestToastActions() {
	auto offered = std::vector<std::string>();
	const auto make = [&](std::set<std::string> permissions) {
		auto fixture = std::make_unique<Fixture>(std::move(permissions));
		fixture->services.toastOffered = [&](const std::string &id) { offered.push_back(id); };
		return fixture;
	};
	const auto action = [](const char *id, const char *label) {
		return cbor::Value::FromMap({ { "id", Text(id) }, { "label", Text(label) } });
	};
	auto fixture = make({ "ui.toast" });
	auto broker = fixture->make();
	CHECK(IsOk(broker.call(Request("ui.toast", {
		{ "text", Text("Deleted") }, { "action", action("undo", "Undo") }, { "duration", Int(7) } }))));
	CHECK(fixture->data.toastActions.size() == 1 && offered == std::vector<std::string>{ "undo" });
	const auto &shown = fixture->data.toastActions.back();
	CHECK(shown.id == "undo" && shown.label == "Undo" && !shown.undo && shown.seconds == 7);
	CHECK(shown.plugin == "com.example.test" && shown.text == "com.example.test: Deleted");

	const auto call = [&](cbor::Map args) {
		auto each = make({ "ui.toast" });
		auto isolated = each->make();
		args.push_back({ "text", Text("x") });
		return Code(isolated.call(Request("ui.toast", std::move(args))));
	};
	CHECK(call({ { "action", action("undo", "") }, { "style", Text("undo") } }) == "");
	CHECK(call({ { "action", action("", "Undo") } }) == "invalid");
	CHECK(call({ { "action", action("go", "") } }) == "invalid");
	CHECK(call({ { "action", Text("go") } }) == "invalid");
	CHECK(call({ { "style", Text("undo") } }) == "invalid");
	CHECK(call({ { "style", Text("neon") } }) == "invalid");
	CHECK(call({ { "action", action("a", "A") }, { "duration", Int(9) } }) == "invalid");
	CHECK(call({ { "action", action("a", "A") }, { "duration", Int(0) } }) == "invalid");
	CHECK(call({ { "action", action("a", "A") } }) == "");

	auto plain = Fixture({ "ui.toast" });
	auto plainBroker = plain.make();
	CHECK(IsOk(plainBroker.call(Request("ui.toast", { { "text", Text("hi") } }))));
	CHECK(plain.data.toasts.size() == 1 && plain.data.toastActions.empty());
}

void TestTranslateReply() {
	auto fixture = Fixture({ "translate.provide" });
	auto seen = std::vector<std::tuple<int64_t, size_t, bool, std::string>>();
	fixture.services.translateReply = [&](
			int64_t ticket,
			std::vector<std::string> items,
			bool markdown,
			std::string error) -> std::string {
		seen.emplace_back(ticket, items.size(), markdown, error);
		return (ticket == 7) ? std::string() : std::string(api::error::kNotFound);
	};
	auto broker = fixture.make();
	const auto reply = [&](cbor::Map args) {
		return Code(broker.call(Request("translate.reply", std::move(args))));
	};
	CHECK(reply({ { "ticket", Int(7) }, { "items", Texts({ "a", "b" }) } }) == "");
	CHECK(IsOk(broker.call(Request("translate.reply", {
		{ "ticket", Int(7) }, { "items", Texts({ "**a**" }) }, { "format", Text("markdown") } }))));
	CHECK(IsOk(broker.call(Request("translate.reply", {
		{ "ticket", Int(7) }, { "error", Text("no key") } }))));
	CHECK(seen.size() == 3 && std::get<0>(seen[0]) == 7 && std::get<1>(seen[0]) == 2);
	CHECK(std::get<2>(seen[1]) && std::get<3>(seen[2]) == "no key");
	CHECK(reply({ { "ticket", Int(8) }, { "items", Texts({ "a" }) } }) == "not_found");
	CHECK(reply({ { "items", Texts({ "a" }) } }) == "invalid");
	CHECK(reply({ { "ticket", Int(7) } }) == "invalid");
	CHECK(reply({ { "ticket", Int(7) }, { "items", cbor::Value::FromArray({ Int(1) }) } }) == "invalid");
	CHECK(reply({ { "ticket", Int(7) }, { "items", Texts({ "a" }) }, { "format", Text("html") } }) == "invalid");
	CHECK(reply({
		{ "ticket", Int(7) },
		{ "items", cbor::Value::FromArray({ Text(std::string((size_t(32) << 10) + 1, 'x')) }) } }) == "invalid");
	auto many = cbor::Array();
	for (auto i = 0; i != 21; ++i) {
		many.push_back(Text("x"));
	}
	CHECK(reply({ { "ticket", Int(7) }, { "items", cbor::Value::FromArray(std::move(many)) } }) == "limit");
	auto big = cbor::Array();
	for (auto i = 0; i != 4; ++i) {
		big.push_back(Text(std::string(size_t(30) << 10, 'x')));
	}
	CHECK(reply({ { "ticket", Int(7) }, { "items", cbor::Value::FromArray(std::move(big)) } }) == "limit");
	CHECK(Code(Fixture({}).make().call(Request("translate.reply", {
		{ "ticket", Int(7) }, { "items", Texts({ "a" }) } }))) == "denied");
}

void TestActivityEvents() {
	const auto chatEvent = [](const char *type, int64_t chat, bool secret = false) {
		return cbor::Value::FromMap({
			{ "type", Text(type) },
			{ "chat", Int(chat) },
			{ "secret", cbor::Value::FromBool(secret) },
			{ "user", Int(7) },
		});
	};
	const auto plain = [](const char *type) {
		return cbor::Value::FromMap({ { "type", Text(type) }, { "user", Int(7) } });
	};
	for (const auto type : { "chat.typing", "chat.read", "chat.member" }) {
		CHECK(!Fixture({}).make().filterEvent(chatEvent(type, 1)));
		CHECK(!Fixture({ "messages.read" }).make().filterEvent(chatEvent(type, 1)));
		auto fixture = Fixture({ "chats.activity" });
		auto broker = fixture.make();
		const auto kept = broker.filterEvent(chatEvent(type, 1));
		CHECK(kept && kept->find("secret") == nullptr && kept->find("user"));
		CHECK(!broker.filterEvent(chatEvent(type, api::kServiceChat)));
		CHECK(!broker.filterEvent(chatEvent(type, 3, true)));
		fixture.grants.chats = std::set<int64_t>{ 2 };
		auto scoped = fixture.make();
		CHECK(!scoped.filterEvent(chatEvent(type, 1)));
		CHECK(scoped.filterEvent(chatEvent(type, 2)).has_value());
	}
	CHECK(!Fixture({ "chats.activity" }).make().filterEvent(plain("user.status")));
	CHECK(Fixture({ "users.presence" }).make().filterEvent(plain("user.status")).has_value());
	{
		auto fixture = Fixture({ "users.presence" });
		fixture.grants.chats = std::set<int64_t>{ 2 };
		CHECK(fixture.make().filterEvent(plain("user.status")).has_value());
	}
	for (const auto type : { "app.focus", "app.blur" }) {
		CHECK(!Fixture({ "users.presence" }).make().filterEvent(plain(type)));
		CHECK(Fixture({ "app.activity" }).make().filterEvent(plain(type)).has_value());
	}
	CHECK(!Fixture({ "chats.activity" }).make().filterEvent(chatEvent("chat.unknown", 1)));
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

[[nodiscard]] cbor::Value Awaited(std::string_view op, cbor::Map args = {}) {
	auto request = Request(op, std::move(args));
	request.asMap()->push_back({ "async", cbor::Value::FromBool(true) });
	return request;
}

void TestAwaitableCalls() {
	auto fixture = Fixture({ "storage.private", "net.https", "net.tcp" });
	auto queued = std::vector<std::function<void()>>();
	auto delays = std::vector<int64_t>();
	auto results = std::map<int64_t, cbor::Value>();
	fixture.services.post = [&](std::function<void()> work) {
		queued.push_back(std::move(work));
	};
	fixture.services.after = [&](int64_t delayMs, std::function<void()> work) {
		delays.push_back(delayMs);
		queued.push_back(std::move(work));
	};
	fixture.services.complete = [&](int64_t ticket, cbor::Value result) {
		results.emplace(ticket, std::move(result));
	};
	const auto broker = std::make_shared<Broker>(fixture.grants, fixture.services);
	const auto ticketOf = [](const cbor::Value &response) {
		const auto ticket = Payload(response).find("ticket");
		return (ticket && ticket->asInt()) ? *ticket->asInt() : int64_t(-1);
	};

	const auto set = broker->call(Awaited("storage.set", {
		{ "key", Text("k") },
		{ "value", Int(5) },
	}));
	CHECK(IsOk(set) && ticketOf(set) == 1);
	CHECK(broker->pending() == 1 && results.empty());
	const auto fetch = broker->call(Awaited("net.fetch", {
		{ "url", Text("https://api.example.com/x") },
	}));
	CHECK(IsOk(fetch) && ticketOf(fetch) == 2);
	for (auto &work : queued) {
		work();
	}
	queued.clear();
	CHECK(broker->pending() == 0);
	CHECK(results.size() == 2 && IsOk(results[1]) && IsOk(results[2]));
	CHECK(Payload(results[2]).find("status")->asInt() == 200);

	const auto denied = broker->call(Awaited("chats.list"));
	CHECK(IsOk(denied));
	queued.front()();
	queued.clear();
	CHECK(Code(results[ticketOf(denied)]) == "denied");

	CHECK(Code(broker->call(Request("time.sleep", { { "ms", Int(10) } }))) == "invalid");
	const auto sleep = broker->call(Awaited("time.sleep", { { "ms", Int(250) } }));
	CHECK(IsOk(sleep) && delays == std::vector<int64_t>{ 250 });
	CHECK(Code(broker->call(Awaited("time.sleep", { { "ms", Int(600000) } }))) == "invalid");
	queued.front()();
	queued.clear();
	CHECK(IsOk(results[ticketOf(sleep)]));

	CHECK(Code(broker->call(Awaited("messages.set_outgoing", { { "text", Text("x") } }))) == "invalid");
	CHECK(Code(broker->call(Awaited("plugin.unload"))) == "invalid");
	broker->beginOutgoing();
	CHECK(Code(broker->call(Awaited("storage.get", { { "key", Text("k") } }))) == "unavailable");
	(void)broker->finishOutgoing();

	for (auto i = 0; i != fixture.grants.maxPending; ++i) {
		CHECK(IsOk(broker->call(Awaited("storage.get", { { "key", Text("k") } }))));
		fixture.time += 10;
	}
	CHECK(Code(broker->call(Awaited("storage.get", { { "key", Text("k") } }))) == "limit");
	for (auto &work : queued) {
		work();
	}
	queued.clear();
	CHECK(broker->pending() == 0);

	const auto socket = broker->call(Awaited("net.tcp.connect", {
		{ "host", Text("example.org") },
		{ "port", Int(443) },
	}));
	CHECK(IsOk(socket));
	for (auto waited = 0; waited < 5000 && broker->pending() != 0; ++waited) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	CHECK(broker->pending() == 0);
	const auto opened = Payload(results[ticketOf(socket)]).find("id");
	CHECK(opened && opened->asInt());
	const auto sent = broker->call(Request("net.socket.send", {
		{ "id", Int(opened ? opened->asInt().value_or(0) : 0) },
		{ "data", Text("ping") },
	}));
	CHECK(IsOk(sent));
	broker->releaseSockets();
	CHECK(fixture.transport.closed.size() == 1);

	auto plain = fixture.make();
	CHECK(Code(plain.call(Awaited("storage.get", { { "key", Text("k") } }))) == "unavailable");
}

} // namespace

void TestRateLimits() {
	auto fixture = Fixture({ "messages.send", "ui.toast", "ui.dialog" });
	fixture.time = 100000;
	auto broker = fixture.make();
	const auto send = [&] {
		return broker.call(Request("messages.send", { { "chat", Int(1) }, { "text", Text("hi") } }));
	};
	for (auto i = 0; i != 5; ++i) {
		CHECK(IsOk(send()));
	}
	CHECK(Code(send()) == "limit");
	fixture.time += 1000;
	CHECK(IsOk(send()));
	CHECK(Code(send()) == "limit");

	const auto toast = [&] {
		return broker.call(Request("ui.toast", { { "text", Text("hello") } }));
	};
	CHECK(IsOk(toast()));
	CHECK(Code(toast()) == "limit");
	fixture.time += 2000;
	CHECK(IsOk(toast()));

	const auto dialog = [&] {
		return broker.call(Request("ui.dialog.show", {
			{ "page", cbor::Value::FromMap({
				{ "title", Text("Hello") },
				{ "items", cbor::Value::FromArray({}) },
			}) },
		}));
	};
	CHECK(Code(dialog()) != "limit");
	CHECK(Code(broker.call(Request("ui.dialog.close"))) != "limit");
	CHECK(Code(dialog()) == "limit");
	broker.noteUserAction("toast.close");
	broker.noteUserAction("dialog.close");
	broker.noteUserAction("window.close", "w");
	CHECK(Code(dialog()) == "limit");
	broker.noteUserAction("menu.click");
	CHECK(Code(dialog()) != "limit");
	CHECK(Code(dialog()) != "limit");
}

[[nodiscard]] cbor::Value Ids(std::initializer_list<int64_t> ids) {
	auto list = cbor::Array();
	for (const auto id : ids) {
		list.push_back(Int(id));
	}
	return cbor::Value::FromArray(std::move(list));
}

void TestLevelTwoCalls() {
	auto fixture = Fixture({
		"messages.send", "messages.read", "messages.edit", "messages.delete",
		"messages.react", "messages.history", "users.info", "chat.info", "notifications",
		"clipboard", "fs.user_selected",
	});
	fixture.time = 100000;
	fixture.grants.chats = std::set<int64_t>{ 1, 2, 3 };
	auto broker = fixture.make();
	auto &data = fixture.data;

	const auto first = broker.call(Request("messages.send", {
		{ "chat", Int(1) },
		{ "text", Text("**hi**") },
		{ "format", Text("markdown") },
		{ "reply_to", Int(100) },
		{ "topic", Int(5) },
		{ "silent", cbor::Value::FromBool(true) },
		{ "schedule", Int(2000000000) },
	}));
	CHECK(IsOk(first));
	CHECK(data.outgoing.size() == 1);
	const auto &sent = data.outgoing.back();
	CHECK(sent.markdown && sent.replyTo == 100 && sent.topic == 5 && sent.silent && sent.schedule == 2000000000);
	CHECK(!sent.waitForServerId);
	CHECK(Payload(first).find("local_id")->asInt() == -1);
	CHECK(Payload(first).find("id") == nullptr);
	CHECK(Code(broker.call(Request("messages.send", {
		{ "chat", Int(1) }, { "text", Text("x") }, { "format", Text("html") } }))) == "invalid");
	CHECK(Code(broker.call(Request("messages.send", {
		{ "chat", Int(5) }, { "text", Text("x") } }))) == "denied");

	CHECK(IsOk(broker.call(Request("messages.edit", {
		{ "chat", Int(1) }, { "id", Int(100) }, { "text", Text("new") } }))));
	CHECK(Code(broker.call(Request("messages.edit", {
		{ "chat", Int(1) }, { "id", Int(101) }, { "text", Text("new") } }))) == "denied");
	CHECK(Code(broker.call(Request("messages.edit", {
		{ "chat", Int(3) }, { "id", Int(100) }, { "text", Text("new") } }))) == "not_found");
	CHECK(IsOk(broker.call(Request("messages.edit", {
		{ "chat", Int(1) }, { "id", Int(-5) }, { "text", Text("new") } }))));
	CHECK(data.actions.back() == "edit 1 -5 new");
	CHECK(Code(broker.call(Request("messages.edit", {
		{ "chat", Int(1) }, { "id", Int(0) }, { "text", Text("new") } }))) == "invalid");
	CHECK(IsOk(broker.call(Request("messages.react", {
		{ "chat", Int(1) }, { "id", Int(-5) }, { "emoji", Text("x") } }))));
	CHECK(IsOk(broker.call(Request("messages.delete", {
		{ "chat", Int(1) }, { "ids", Ids({ 100, 101, 100 }) } }))));
	CHECK(data.actions.back() == "delete 1 2 everyone");
	CHECK(Code(broker.call(Request("messages.delete", {
		{ "chat", Int(1) }, { "ids", Ids({}) } }))) == "invalid");
	CHECK(IsOk(broker.call(Request("messages.delete", {
		{ "chat", Int(1) }, { "ids", Ids({ -1, 100 }) } }))));
	CHECK(data.actions.back() == "delete 1 2 everyone");
	CHECK(Code(broker.call(Request("messages.delete", {
		{ "chat", Int(1) }, { "ids", Ids({ 0 }) } }))) == "invalid");

	CHECK(IsOk(broker.call(Request("messages.react", {
		{ "chat", Int(2) }, { "id", Int(100) }, { "emoji", Text("👍") } }))));
	CHECK(IsOk(broker.call(Request("messages.react", { { "chat", Int(2) }, { "id", Int(100) } }))));
	CHECK(data.actions.back() == "react 2 100 ");

	CHECK(IsOk(broker.call(Request("messages.forward", {
		{ "from_chat", Int(1) }, { "ids", Ids({ 100 }) }, { "to_chat", Int(2) } }))));
	CHECK(data.actions.back() == "forward 1 1 2");
	CHECK(Code(broker.call(Request("messages.forward", {
		{ "from_chat", Int(1) }, { "ids", Ids({ 100 }) }, { "to_chat", Int(5) } }))) == "denied");

	CHECK(IsOk(broker.call(Request("chats.mark_read", { { "chat", Int(1) } }))));
	CHECK(IsOk(broker.call(Request("chats.action", { { "chat", Int(1) } }))));
	CHECK(data.actions.back() == "action 1 typing");
	CHECK(Code(broker.call(Request("chats.action", { { "chat", Int(1) } }))) == "limit");
	CHECK(IsOk(broker.call(Request("chats.action", { { "chat", Int(2) } }))));
	CHECK(IsOk(broker.call(Request("chats.action", { { "chat", Int(1) }, { "kind", Text("cancel") } }))));

	const auto found = broker.call(Request("messages.search", {
		{ "chat", Int(1) }, { "query", Text("o") }, { "limit", Int(5) } }));
	CHECK(IsOk(found));
	const auto &messages = *Payload(found).asArray();
	CHECK(messages.size() == 5);
	for (const auto &message : messages) {
		CHECK(message.find("deleted") == nullptr && *message.find("chat")->asInt() == 1);
	}

	const auto user = broker.call(Request("users.info", { { "id", Int(7) } }));
	CHECK(IsOk(user) && *Payload(user).find("username")->asText() == "bob");
	CHECK(*Payload(user).find("status")->asText() == "online");
	CHECK(Code(broker.call(Request("users.info", { { "id", Int(9) } }))) == "not_found");
	CHECK(Code(broker.call(Request("users.info", { { "id", Int(api::kServiceChat) } }))) == "denied");

	const auto resolved = broker.call(Request("users.resolve", { { "username", Text("@Bobby") } }));
	CHECK(Code(resolved) == "not_found");
	fixture.time += 1000;
	const auto resolvedOk = broker.call(Request("users.resolve", { { "username", Text("@bobby") } }));
	CHECK(IsOk(resolvedOk) && *Payload(resolvedOk).find("id")->asInt() == 7);
	CHECK(Code(broker.call(Request("users.resolve", { { "username", Text("bobby") } }))) == "limit");
	fixture.time += 1000;
	CHECK(Code(broker.call(Request("users.resolve", { { "username", Text("a b") } }))) == "invalid");
	const auto photo = broker.call(Request("users.photo", { { "id", Int(7) } }));
	CHECK(IsOk(photo) && *Payload(photo).find("mime")->asText() == "image/jpeg");
	CHECK(Code(broker.call(Request("users.photo", { { "id", Int(9) } }))) == "not_found");
	CHECK(Code(broker.call(Request("users.photo", { { "id", Int(api::kServiceChat) } }))) == "denied");

	fixture.time += 1000;
	const auto channelInfo = broker.call(Request("channels.info", { { "id", Int(2) } }));
	CHECK(IsOk(channelInfo) && *Payload(channelInfo).find("type")->asText() == "channel");
	CHECK(*Payload(channelInfo).find("members")->asInt() == 1200);
	CHECK(Code(broker.call(Request("channels.info", { { "id", Int(3) } }))) == "not_found");
	CHECK(IsOk(broker.call(Request("channels.resolve", { { "username", Text("@newsy") } }))));
	CHECK(Code(broker.call(Request("channels.resolve", { { "username", Text("newsy") } }))) == "limit");
	CHECK(IsOk(broker.call(Request("channels.photo", { { "id", Int(2) } }))));
	CHECK(Code(broker.call(Request("channels.photo", { { "id", Int(3) } }))) == "not_found");
	fixture.time += 1000;

	const auto reactors = broker.call(Request("messages.reactions", { { "chat", Int(1) }, { "id", Int(100) } }));
	CHECK(IsOk(reactors) && Payload(reactors).asArray()->size() == 2);
	CHECK(Payload(reactors).asArray()->at(1).find("custom_emoji") != nullptr);
	const auto filtered = broker.call(Request("messages.reactions", {
		{ "chat", Int(1) }, { "id", Int(100) }, { "emoji", Text("x") } }));
	CHECK(IsOk(filtered) && Payload(filtered).asArray()->size() == 1);
	CHECK(Code(broker.call(Request("messages.reactions", { { "chat", Int(1) }, { "id", Int(5) } }))) == "not_found");
	CHECK(Code(broker.call(Request("messages.reactions", {
		{ "chat", Int(1) }, { "id", Int(100) }, { "emoji", Text("x") }, { "custom_emoji", Int(5) } }))) == "invalid");
	CHECK(IsOk(broker.call(Request("messages.react", { { "chat", Int(1) }, { "id", Int(100) }, { "custom_emoji", Int(55) } }))));
	CHECK(data.actions.back().find("custom55") != std::string::npos);
	CHECK(Code(broker.call(Request("messages.react", { { "chat", Int(1) }, { "id", Int(100) }, { "custom_emoji", Int(-1) } }))) == "invalid");

	const auto searched = broker.call(Request("stickers.search", { { "emoji", Text("x") } }));
	CHECK(IsOk(searched) && *Payload(searched).asArray()->at(0).find("id")->asInt() == 321);
	CHECK(IsOk(broker.call(Request("stickers.recent"))));
	CHECK(Code(broker.call(Request("stickers.search"))) == "invalid");
	fixture.time += 5000;
	CHECK(IsOk(broker.call(Request("stickers.send", { { "chat", Int(1) }, { "id", Int(321) } }))));
	CHECK(data.actions.back() == "sticker 1 321");
	CHECK(Code(broker.call(Request("stickers.send", { { "chat", Int(1) }, { "id", Int(5) } }))) == "not_found");
	CHECK(Code(broker.call(Request("stickers.send", { { "chat", Int(9) }, { "id", Int(321) } }))) == "denied");
	fixture.time += 5000;
	CHECK(Code(broker.call(Request("messages.send", {
		{ "chat", Int(1) }, { "text", Text("a") },
		{ "custom_emoji", cbor::Value::FromArray({ cbor::Value::FromMap({
			{ "offset", Int(0) }, { "length", Int(0) }, { "id", Int(5) } }) }) } }))) == "invalid");
	CHECK(IsOk(broker.call(Request("messages.send", {
		{ "chat", Int(1) }, { "text", Text("a") },
		{ "custom_emoji", cbor::Value::FromArray({ cbor::Value::FromMap({
			{ "offset", Int(0) }, { "length", Int(1) }, { "id", Int(5) } }) }) } }))));
	fixture.time += 5000;

	CHECK(IsOk(broker.call(Request("notify.show", { { "title", Text("T") }, { "text", Text("x") } }))));
	CHECK(Code(broker.call(Request("notify.show", { { "text", Text("x") } }))) == "limit");
	fixture.time += 5000;
	CHECK(IsOk(broker.call(Request("notify.show", { { "text", Text("y") } }))));

	CHECK(Code(broker.call(Request("clipboard.write", { { "text", Text("x") } }))) == "denied");
	CHECK(Code(broker.call(Request("clipboard.read"))) == "denied");
	broker.noteUserAction("menu.click");
	CHECK(IsOk(broker.call(Request("clipboard.write", { { "text", Text("copied") } }))));
	const auto pasted = broker.call(Request("clipboard.read"));
	CHECK(IsOk(pasted) && *Payload(pasted).asText() == "copied");
	fixture.time += 10000;
	CHECK(Code(broker.call(Request("clipboard.read"))) == "denied");

	CHECK(Code(broker.call(Request("files.pick"))) == "invalid");
	CHECK(Code(broker.call(Request("files.save", { { "name", Text("a") }, { "data", Text("b") } }))) == "invalid");

	const auto opened = [](int64_t chat, bool secret) {
		return cbor::Value::FromMap({
			{ "type", Text("chat.opened") },
			{ "chat", Int(chat) },
			{ "secret", cbor::Value::FromBool(secret) },
		});
	};
	auto active = fixture.grants;
	CHECK(!FilterEvent(active, opened(1, false)));
	active.permissions.insert("chats.active");
	const auto passed = FilterEvent(active, opened(1, false));
	CHECK(passed && passed->find("secret") == nullptr && *passed->find("chat")->asInt() == 1);
	CHECK(!FilterEvent(active, opened(5, false)));
	CHECK(!FilterEvent(active, opened(2, true)));

	const auto meta = broker.call(Request("meta.info"));
	CHECK(Payload(meta).find("chats")->asArray()->size() == 3);
	auto all = Fixture({});
	CHECK(*Payload(all.make().call(Request("meta.info"))).find("chats")->asText() == "all");
}

void TestMediaCalls() {
	auto fixture = Fixture({ "media.read", "messages.send" });
	fixture.time = 100000;
	fixture.grants.chats = std::set<int64_t>{ 1, 2 };
	auto broker = fixture.make();
	auto &data = fixture.data;

	CHECK(Code(broker.call(Request("media.get", { { "chat", Int(1) }, { "message", Int(101) } }))) == "not_found");
	const auto loaded = broker.call(Request("media.get", {
		{ "chat", Int(1) }, { "message", Int(101) }, { "download", cbor::Value::FromBool(true) } }));
	CHECK(IsOk(loaded) && *Payload(loaded).find("name")->asText() == "a.pdf");
	CHECK(Payload(loaded).find("data")->asBytes()->size() == 20);
	CHECK(Code(broker.call(Request("media.get", {
		{ "chat", Int(1) }, { "message", Int(102) }, { "download", cbor::Value::FromBool(true) } }))) == "limit");
	CHECK(Code(broker.call(Request("media.get", {
		{ "chat", Int(1) }, { "message", Int(101) }, { "download", Int(1) } }))) == "invalid");

	const auto listed = broker.call(Request("media.list", {
		{ "chat", Int(1) }, { "kind", Text("photo") }, { "limit", Int(5) } }));
	CHECK(IsOk(listed));
	for (const auto &message : *Payload(listed).asArray()) {
		CHECK(*message.find("media")->asText() == "photo");
		CHECK(message.find("deleted") == nullptr);
	}
	CHECK(Code(broker.call(Request("media.list", { { "chat", Int(1) }, { "kind", Text("sticker") } }))) == "invalid");
	CHECK(Code(broker.call(Request("media.list", { { "chat", Int(5) } }))) == "denied");

	const auto file = cbor::Value::FromBytes(cbor::Bytes(64, 7));
	CHECK(IsOk(broker.call(Request("messages.send_media", {
		{ "chat", Int(1) },
		{ "data", file },
		{ "name", Text("cat.png") },
		{ "caption", Text("**cat**") },
		{ "format", Text("markdown") },
		{ "spoiler", cbor::Value::FromBool(true) },
		{ "reply_to", Int(100) },
	}))));
	CHECK(data.sentMedia.size() == 1);
	const auto &sent = data.sentMedia.back();
	CHECK(sent.name == "cat.png" && sent.kind == "auto" && sent.bytes.size() == 64);
	CHECK(sent.spoiler && sent.message.markdown && sent.message.replyTo == 100);
	CHECK(sent.message.text == "**cat**");
	CHECK(Code(broker.call(Request("messages.send_media", {
		{ "chat", Int(1) }, { "data", file }, { "name", Text("a/b.png") } }))) == "invalid");
	CHECK(Code(broker.call(Request("messages.send_media", {
		{ "chat", Int(1) }, { "data", file }, { "name", Text("v.ogg") }, { "kind", Text("voice") } }))) == "invalid");
	CHECK(Code(broker.call(Request("messages.send_media", {
		{ "chat", Int(1) }, { "data", Text("x") }, { "name", Text("a.txt") } }))) == "invalid");
	CHECK(Code(broker.call(Request("messages.send_media", {
		{ "chat", Int(5) }, { "data", file }, { "name", Text("a.txt") } }))) == "denied");
	CHECK(Code(broker.call(Request("messages.send_media", {
		{ "chat", Int(1) },
		{ "data", cbor::Value::FromBytes(cbor::Bytes((size_t(100) << 20) + 1, 1)) },
		{ "name", Text("big.bin") } }))) == "limit");
	auto large = Fixture({ "messages.send" });
	CHECK(IsOk(large.make().call(Request("messages.send_media", {
		{ "chat", Int(1) },
		{ "data", cbor::Value::FromBytes(cbor::Bytes(size_t(100) << 20, 1)) },
		{ "name", Text("big.bin") } }))));
	CHECK(large.data.sentMedia.size() == 1
		&& large.data.sentMedia.back().bytes.size() == (size_t(100) << 20));

	auto denied = Fixture({ "media.read" });
	CHECK(Code(denied.make().call(Request("messages.send_media", {
		{ "chat", Int(1) }, { "data", file }, { "name", Text("a.txt") } }))) == "denied");

	auto editing = Fixture({ "messages.edit" });
	editing.grants.chats = std::set<int64_t>{ 1, 2 };
	auto editor = editing.make();
	const auto editMedia = [&](cbor::Map args) {
		args.push_back({ "chat", Int(1) });
		return editor.call(Request("messages.edit_media", std::move(args)));
	};
	CHECK(IsOk(editMedia({
		{ "id", Int(100) },
		{ "data", file },
		{ "name", Text("cat.png") },
		{ "caption", Text("**cat**") },
		{ "format", Text("markdown") },
		{ "spoiler", cbor::Value::FromBool(true) },
	})));
	CHECK(editing.data.editedMedia.size() == 1);
	const auto &edited = editing.data.editedMedia.back();
	CHECK(edited.id == 100 && edited.name == "cat.png" && edited.kind == "auto");
	CHECK(edited.bytes.size() == 64 && edited.markdown && edited.spoiler);
	CHECK(edited.caption == "**cat**");
	CHECK(IsOk(editMedia({ { "id", Int(-7) }, { "data", file }, { "name", Text("a.bin") }, { "kind", Text("file") } })));
	CHECK(editing.data.editedMedia.back().id == -7);
	const auto refused = [&](cbor::Map args, int64_t chat = 1) {
		auto fresh = Fixture({ "messages.edit" });
		fresh.grants.chats = std::set<int64_t>{ 1, 2 };
		auto broker = fresh.make();
		args.push_back({ "chat", Int(chat) });
		return Code(broker.call(Request("messages.edit_media", std::move(args))));
	};
	CHECK(refused({ { "id", Int(101) }, { "data", file }, { "name", Text("a.bin") } }) == "not_found");
	CHECK(refused({ { "id", Int(0) }, { "data", file }, { "name", Text("a.bin") } }) == "invalid");
	CHECK(refused({ { "data", file }, { "name", Text("a.bin") } }) == "invalid");
	CHECK(refused({ { "id", Int(100) }, { "data", file }, { "name", Text("a.bin") }, { "kind", Text("voice") } }) == "invalid");
	CHECK(refused({ { "id", Int(100) }, { "data", file }, { "name", Text("a/b.bin") } }) == "invalid");
	CHECK(refused({ { "id", Int(100) }, { "data", Text("x") }, { "name", Text("a.bin") } }) == "invalid");
	CHECK(refused({
		{ "id", Int(100) },
		{ "data", cbor::Value::FromBytes(cbor::Bytes((size_t(100) << 20) + 1, 1)) },
		{ "name", Text("big.bin") } }) == "limit");
	CHECK(refused({ { "id", Int(100) }, { "data", file }, { "name", Text("a.bin") } }, 5) == "denied");
}

void TestSentIds() {
	const auto file = cbor::Value::FromBytes(cbor::Bytes(8, 7));
	const auto send = [&](bool ids, bool media) {
		auto permissions = std::set<std::string>{ "messages.send" };
		if (ids) {
			permissions.insert("messages.ids");
		}
		auto fixture = Fixture(std::move(permissions));
		auto broker = fixture.make();
		const auto answer = media
			? broker.call(Request("messages.send_media", {
				{ "chat", Int(1) }, { "data", file }, { "name", Text("a.png") } }))
			: broker.call(Request("messages.send", {
				{ "chat", Int(1) }, { "text", Text("x") } }));
		CHECK(IsOk(answer));
		const auto waited = media
			? fixture.data.sentMedia.back().message.waitForServerId
			: fixture.data.outgoing.back().waitForServerId;
		CHECK(waited == ids);
		const auto &payload = Payload(answer);
		const auto local = payload.find("local_id");
		const auto server = payload.find("id");
		CHECK(local && local->asInt() == -1);
		CHECK(ids ? (server && server->asInt() == 500) : !server);
	};
	auto forwarding = Fixture({ "messages.send", "messages.read" });
	auto forwarder = forwarding.make();
	CHECK(Code(forwarder.call(Request("messages.forward", {
		{ "from_chat", Int(1) }, { "ids", Ids({ -1 }) }, { "to_chat", Int(2) } }))) == "invalid");
	send(false, false);
	send(true, false);
	send(false, true);
	send(true, true);
}

void TestFilesAndTimers() {
	auto fixture = Fixture({ "fs.user_selected" });
	fixture.time = 100000;
	auto queued = std::vector<std::function<void()>>();
	auto results = std::map<int64_t, cbor::Value>();
	fixture.services.post = [&](std::function<void()> work) {
		queued.push_back(std::move(work));
	};
	fixture.services.after = [&](int64_t, std::function<void()> work) {
		queued.push_back(std::move(work));
	};
	fixture.services.complete = [&](int64_t ticket, cbor::Value result) {
		results.emplace(ticket, std::move(result));
	};
	auto timers = std::map<std::string, std::pair<int64_t, bool>>();
	fixture.services.setTimer = [&](const std::string &id, int64_t delay, bool repeat) {
		timers[id] = { delay, repeat };
		return std::string();
	};
	fixture.services.clearTimer = [&](const std::string &id) {
		return timers.erase(id) > 0;
	};
	fixture.services.timers = [&] {
		auto result = std::vector<std::string>();
		for (const auto &[id, timer] : timers) {
			result.push_back(id);
		}
		return result;
	};
	const auto broker = std::make_shared<Broker>(fixture.grants, fixture.services);
	const auto run = [&](cbor::Value request) {
		const auto response = broker->call(request);
		CHECK(IsOk(response));
		for (auto &work : queued) {
			work();
		}
		queued.clear();
		return results[*Payload(response).find("ticket")->asInt()];
	};

	CHECK(Code(run(Awaited("files.pick"))) == "denied");
	broker->noteUserAction("command");
	const auto picked = run(Awaited("files.pick", { { "filter", Text("Text (*.txt)") } }));
	CHECK(IsOk(picked) && *Payload(picked).find("name")->asText() == "a.txt");
	CHECK(Payload(picked).find("data")->asBytes()->size() == 2);
	fixture.data.cancelPick = true;
	const auto cancelled = run(Awaited("files.pick"));
	CHECK(IsOk(cancelled) && Payload(cancelled).isNull());
	const auto saved = run(Awaited("files.save", { { "name", Text("out.txt") }, { "data", Text("abc") } }));
	CHECK(IsOk(saved) && *Payload(saved).asBool());
	CHECK(fixture.data.actions.back() == "save out.txt 3");
	CHECK(Code(run(Awaited("files.save", { { "name", Text("../x") }, { "data", Text("a") } }))) == "invalid");

	CHECK(IsOk(broker->call(Request("timers.set", {
		{ "id", Text("tick") }, { "delay", Int(60000) }, { "repeat", cbor::Value::FromBool(true) } }))));
	CHECK(timers["tick"] == std::make_pair(int64_t(60000), true));
	CHECK(Code(broker->call(Request("timers.set", { { "id", Text("t") }, { "delay", Int(10) } }))) == "invalid");
	CHECK(Code(broker->call(Request("timers.set", { { "delay", Int(5000) } }))) == "invalid");
	const auto list = broker->call(Request("timers.list"));
	CHECK(IsOk(list) && Payload(list).asArray()->size() == 1);
	CHECK(*Payload(broker->call(Request("timers.clear", { { "id", Text("tick") } }))).asBool());
	CHECK(!*Payload(broker->call(Request("timers.clear", { { "id", Text("tick") } }))).asBool());
}

void TestClosedBrokerDropsAwaitedWork() {
	auto fixture = Fixture({ "storage.private" });
	auto queued = std::vector<std::function<void()>>();
	auto results = std::map<int64_t, cbor::Value>();
	fixture.services.post = [&](std::function<void()> work) {
		queued.push_back(std::move(work));
	};
	fixture.services.after = [&](int64_t, std::function<void()> work) {
		queued.push_back(std::move(work));
	};
	fixture.services.complete = [&](int64_t ticket, cbor::Value result) {
		results.emplace(ticket, std::move(result));
	};
	const auto broker = std::make_shared<Broker>(fixture.grants, fixture.services);
	auto request = Request("storage.set", { { "key", Text("k") }, { "value", Int(1) } });
	request.asMap()->push_back({ "async", cbor::Value::FromBool(true) });
	CHECK(IsOk(broker->call(request)));
	broker->close();
	for (auto &work : queued) {
		work();
	}
	CHECK(results.size() == 1 && Code(results.begin()->second) == "unavailable");
	CHECK(!fixture.storage.get("k"));
}

void TestSavedMessagesAreHidden() {
	auto fixture = Fixture({
		"chats.list", "chat.info", "messages.history", "composer.draft",
		"messages.send", "messages.read", "messages.delete", "chats.activity",
	});
	fixture.data.withSaved = true;
	auto broker = fixture.make();
	const auto saved = FakeData::kSavedChat;

	const auto chats = broker.call(Request("chats.list"));
	auto ids = std::vector<int64_t>();
	for (const auto &chat : *Payload(chats).asArray()) {
		ids.push_back(*chat.find("id")->asInt());
	}
	CHECK((ids == std::vector<int64_t>{ 1, 2, 5 }));

	CHECK(Code(broker.call(Request("chat.info", { { "chat", Int(saved) } }))) == "not_found");
	CHECK(Code(broker.call(Request("messages.history", { { "chat", Int(saved) } }))) == "not_found");
	CHECK(Code(broker.call(Request("composer.draft", { { "chat", Int(saved) } }))) == "not_found");
	CHECK(Code(broker.call(Request("composer.set_draft", { { "chat", Int(saved) }, { "text", Text("x") } }))) == "not_found");
	CHECK(Code(broker.call(Request("messages.send", { { "chat", Int(saved) }, { "text", Text("x") } }))) == "not_found");
	CHECK(Code(broker.call(Request("messages.delete", { { "chat", Int(saved) }, { "ids", cbor::Value::FromArray({ Int(1) }) } }))) == "not_found");
	CHECK(Code(broker.call(Request("messages.forward", { { "from_chat", Int(saved) }, { "to_chat", Int(1) }, { "ids", cbor::Value::FromArray({ Int(1) }) } }))) == "not_found");
	CHECK(Code(broker.call(Request("messages.forward", { { "from_chat", Int(1) }, { "to_chat", Int(saved) }, { "ids", cbor::Value::FromArray({ Int(1) }) } }))) == "not_found");
	CHECK(Code(broker.call(Request("chats.mark_read", { { "chat", Int(saved) } }))) == "not_found");
	CHECK(fixture.data.sent.empty());
	CHECK(IsOk(broker.call(Request("chat.info", { { "chat", Int(1) } }))));

	const auto event = [&](const char *type, bool self) {
		return cbor::Value::FromMap({
			{ "type", Text(type) },
			{ "chat", Int(self ? saved : 1) },
			{ "secret", cbor::Value::FromBool(false) },
			{ "self", cbor::Value::FromBool(self) },
			{ "user", Int(7) },
		});
	};
	for (const auto type : { "message.new", "chat.opened", "chat.typing" }) {
		CHECK(!broker.filterEvent(event(type, true)));
	}
	const auto kept = broker.filterEvent(event("chat.typing", false));
	CHECK(kept && kept->find("self") == nullptr && kept->find("secret") == nullptr);

	const auto click = cbor::Value::FromMap({
		{ "type", Text("menu.click") },
		{ "chat", Int(saved) },
		{ "self", cbor::Value::FromBool(true) },
		{ "text", Text("hidden") },
	});
	const auto reduced = broker.filterTargeted(click);
	CHECK(!reduced.find("chat") && !reduced.find("text") && !reduced.find("self"));
}

int main() {
	TestUrlParsing();
	TestEndpoints();
	TestPublicAddresses();
	TestHeaderRules();
	TestMetaAndLog();
	TestEveryOperationNeedsItsPermission();
	TestStorage();
	TestKeyCountLimit();
	TestNetwork();
	TestSockets();
	TestNewDataCalls();
	TestOutgoingAndUnload();
	TestServerTime();
	TestHistoryFromUser();
	TestInlineQuery();
	TestOpenSettings();
	TestIncomingHook();
	TestMessageLookups();
	TestChatManagement();
	TestBotButtons();
	TestModeration();
	TestLocalNetwork();
	TestWebSocketPrimitives();
	TestWebSocket();
	TestMembersPollsAndGhost();
	TestEntitiesInMessages();
	TestOutgoingEntities();
	TestPluginMessages();
	TestDataIsFilteredByTheBroker();
	TestRichIconsAndSubPages();
	TestSettingsPage();
	TestBadges();
	TestTemplateTree();
	TestWindows();
	TestTargetedEvents();
	TestChatScope();
	TestSavedMessagesAreHidden();
	TestRateLimit();
	TestMalformedRequests();
	TestAuditTrail();
	TestEvents();
	TestActivityEvents();
	TestTranslateReply();
	TestToastActions();
	TestAwaitableCalls();
	TestRateLimits();
	TestClosedBrokerDropsAwaitedWork();
	TestLevelTwoCalls();
	TestMediaCalls();
	TestSentIds();
	TestFilesAndTimers();
	return Failures() ? 1 : 0;
}
