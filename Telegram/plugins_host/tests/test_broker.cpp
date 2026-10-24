#include "fgpl/broker.h"
#include "check.h"
#include "fakes.h"
#include "fgpl/net_policy.h"

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
		{ "net.https", Request("net.fetch", { { "url", Text("https://api.example.com/") } }) },
		{ "net.tcp", Request("net.tcp.connect", { { "host", Text("example.com") }, { "port", Int(80) } }) },
		{ "net.udp", Request("net.udp.connect", { { "host", Text("8.8.8.8") }, { "port", Int(53) } }) },
		{ "contacts.list", Request("contacts.list") },
		{ "folders.list", Request("folders.list") },
		{ "settings.read", Request("settings.read") },
		{ "media.read", Request("media.get", { { "chat", Int(1) }, { "message", Int(100) } }) },
		{ "ui.dialog", Request("ui.dialog.close") },
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
	CHECK(Code(fetch({ { "url", Text("https://127.0.0.1/") } })) == "invalid");
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
	CHECK(Code(call("net.tcp.connect", { { "host", Text("127.0.0.1") }, { "port", Int(22) } })) == "invalid");
	CHECK(Code(call("net.tcp.connect", { { "host", Text("10.0.0.5") }, { "port", Int(22) } })) == "invalid");
	CHECK(Code(call("net.tcp.connect", { { "host", Text("localhost") }, { "port", Int(22) } })) == "invalid");
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
	CHECK(broker.finishOutgoing() == std::optional<std::string>("changed"));
	CHECK(IsOk(broker.call(Request("chats.list"))));
	broker.beginOutgoing();
	CHECK(!broker.finishOutgoing());

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
		Item({ { "type", Text("select") }, { "id", Text("mode") }, { "label", Text("Mode") }, { "value", Text("nope") },
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

	auto denied = Fixture({ "ui.toast" });
	denied.services.ui = fixture.services.ui;
	auto refused = denied.make();
	CHECK(Code(refused.call(Request("ui.settings.set", { { "page", good } }))) == "denied");
	auto none = Fixture({ "ui.settings_page" });
	auto unavailable = none.make();
	CHECK(Code(unavailable.call(Request("ui.settings.set", { { "page", good } }))) == "unavailable");
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
	CHECK(inScope.find("chat") && inScope.find("message") && inScope.find("from"));
	CHECK(!inScope.find("text") && !inScope.find("secret"));
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
	TestPluginMessages();
	TestDataIsFilteredByTheBroker();
	TestSettingsPage();
	TestTargetedEvents();
	TestChatScope();
	TestRateLimit();
	TestMalformedRequests();
	TestAuditTrail();
	TestEvents();
	return Failures() ? 1 : 0;
}
