#include "check.h"
#include "fgpl/manifest.h"
#include "fgpl/permissions.h"
#include "package_builder.h"

#include <functional>
#include <string>
#include <vector>

namespace {

using namespace fgpl;
using namespace pkg;

[[nodiscard]] cbor::Value WithFiles(cbor::Value document) {
	if (!document.asMap()
		|| (document.find("files")
			&& document.find("files")->asMap()
			&& !document.find("files")->asMap()->empty())) {
		return document;
	}
	Set(document, "files", cbor::Value::FromMap({
		{ "plugin.wasm", Text(HashText(Bytes("x"))) },
		{ "main.py", Text(HashText(Bytes("x"))) },
		{ "icon.png", Text(HashText(Bytes("x"))) },
		{ "CHANGELOG.md", Text(HashText(Bytes("x"))) },
	}));
	return document;
}

[[nodiscard]] Result<Manifest, std::vector<std::string>> Parse(
		const cbor::Value &document) {
	return ParseManifest(WithFiles(document));
}

void ExpectError(
		const char *what,
		const std::function<void(cbor::Value &)> &mutate,
		const std::string &fragment) {
	auto document = ValidDocument();
	mutate(document);
	const auto result = Parse(document);
	if (result.ok()) {
		std::fprintf(stderr, "%s: accepted\n", what);
		CHECK(false);
		return;
	}
	auto found = false;
	for (const auto &line : result.error()) {
		found = found || line.find(fragment) != std::string::npos;
	}
	if (!found) {
		std::fprintf(stderr, "%s: no error mentions '%s':\n", what, fragment.c_str());
		for (const auto &line : result.error()) {
			std::fprintf(stderr, "    %s\n", line.c_str());
		}
		CHECK(false);
	}
}

void TestOutdated() {
	const auto manifest = Parse(ValidDocument()).value();
	auto environment = HostEnvironment();
	for (const auto &[app, outdated] : std::vector<std::pair<const char *, bool>>{
		{ "7.6.1", false }, { "7.6.9", false }, { "7.5.0", false },
		{ "7.7.0", true }, { "8.0.0", true }, { "6.9.0", false },
	}) {
		environment.app = *ParseVersion(app);
		if (Outdated(manifest, environment) != outdated) {
			std::fprintf(stderr, "outdated for %s\n", app);
			CHECK(false);
		}
	}
	auto untested = manifest;
	untested.testedWith = std::nullopt;
	environment.app = *ParseVersion("9.0.0");
	CHECK(!Outdated(untested, environment));
}

void TestValidManifest() {
	const auto result = Parse(ValidDocument());
	if (!result.ok()) {
		for (const auto &line : result.error()) {
			std::fprintf(stderr, "%s\n", line.c_str());
		}
		CHECK(false);
		return;
	}
	const auto &manifest = result.value();
	CHECK(manifest.id == "com.example.demo");
	CHECK(manifest.version.text() == "1.2.0");
	CHECK(manifest.name.get("default") == "Demo");
	CHECK(manifest.name.get("ru") == "Demo");
	CHECK(manifest.permissions.size() == 3);
	CHECK(manifest.permissions[0].required);
	CHECK(manifest.permissions[1].id == "net.https");
	CHECK(manifest.apiMin == 1 && manifest.apiMax == 1000);
	CHECK(manifest.events.size() == 1);
	CHECK(manifest.files.size() == 4);
	CHECK(!manifest.signature.has_value());
}

void TestLocalizedAndOptionalForms() {
	auto document = ValidDocument();
	Set(document, "name", cbor::Value::FromMap({
		{ "default", Text("Quick Translate") },
		{ "ru", Text("Быстрый перевод") },
		{ "pt-BR", Text("Tradução") },
	}));
	Set(document, "x-note", Text("ignored"));
	Set(document, "compat.api", cbor::Value::FromMap({ { "min", Int(1) }, { "max", Int(3) } }));
	Set(document, "activation.on", Array({ "command", "event", "startup" }));
	Set(document, "locales", Array({ "en", "ru", "pt-BR" }));
	Set(document, "categories", Array({ "utility", "translation" }));
	Set(document, "compat.requires", Array({ "ui.menu", "net.http" }));
	Set(document, "compat.accounts.premium", cbor::Value::FromBool(true));
	Set(document, "dependencies", cbor::Value::FromArray({
		cbor::Value::FromMap({ { "id", Text("com.example.helper") }, { "version", Text(">=1.0.0") } }),
	}));
	Set(document, "conflicts", cbor::Value::FromArray({
		cbor::Value::FromMap({ { "id", Text("com.example.old") } }),
	}));
	Set(document, "storage.schema", Int(3));
	Set(document, "updates.feed", Text("https://example.com/feed.json"));
	const auto result = Parse(document);
	if (!result.ok()) {
		for (const auto &line : result.error()) {
			std::fprintf(stderr, "%s\n", line.c_str());
		}
		CHECK(false);
		return;
	}
	const auto &manifest = result.value();
	CHECK(manifest.name.get("ru") == "Быстрый перевод");
	CHECK(manifest.name.get("pt-BR") == "Tradução");
	CHECK(manifest.name.get("pt") == "Quick Translate");
	CHECK(manifest.name.get("de") == "Quick Translate");
	CHECK(manifest.apiMax == 3);
	CHECK(manifest.activation.size() == 3);
	CHECK(manifest.premiumOnly);
	CHECK(manifest.needs.size() == 2);
	CHECK(manifest.dependencies.size() == 1 && manifest.dependencies[0].range.has_value());
	CHECK(manifest.conflicts.size() == 1);
	CHECK(manifest.storageSchema == 3);
}

void TestRejections() {
	ExpectError("not an object", [](auto &d) { d = Int(1); }, "must be an object");
	ExpectError("unknown key", [](auto &d) { Set(d, "surprise", Int(1)); }, "unknown key 'surprise'");
	ExpectError("typo", [](auto &d) { Set(d, "permisions", Int(1)); }, "unknown key");
	ExpectError("app name", [](auto &d) { Set(d, "name", Text("Telegram")); }, "name of the app");
	ExpectError("app name spaced", [](auto &d) { Set(d, "name", Text(" fresh Gram ")); }, "name of the app");
	ExpectError("app name localized", [](auto &d) {
		Set(d, "name", cbor::Value::FromMap({ { "default", Text("Tools") }, { "ru", Text("AyuGram") } }));
	}, "name of the app");
	ExpectError("schema", [](auto &d) { Set(d, "schema", Int(2)); }, "schema");
	ExpectError("no schema", [](auto &d) { Remove(d, "schema"); }, "schema");
	ExpectError("no id", [](auto &d) { Remove(d, "id"); }, "id");
	for (const auto bad : { "Demo", "demo", "com..demo", "com.demo-", "-com.demo", "com.Demo", "com.de mo", "a.b", "com.example.demo.with.a.very.long.name.that.goes.on.and.on.and.on.forever" }) {
		ExpectError("bad id", [&](auto &d) { Set(d, "id", Text(bad)); }, "id");
	}
	ExpectError("no name", [](auto &d) { Remove(d, "name"); }, "name");
	ExpectError("empty name", [](auto &d) { Set(d, "name", Text("")); }, "name");
	ExpectError("name without default", [](auto &d) {
		Set(d, "name", cbor::Value::FromMap({ { "ru", Text("x") } }));
	}, "default");
	ExpectError("bad language", [](auto &d) {
		Set(d, "name", cbor::Value::FromMap({ { "default", Text("x") }, { "R U!", Text("y") } }));
	}, "bad entry");
	ExpectError("version", [](auto &d) { Set(d, "version", Text("1.2")); }, "semantic version");
	ExpectError("range", [](auto &d) { Set(d, "compat.freshgram", Text("^7.6")); }, "version range");
	ExpectError("no range", [](auto &d) { Remove(d, "compat.freshgram"); }, "compat.freshgram");
	ExpectError("tested", [](auto &d) { Set(d, "compat.testedWith", Text("7")); }, "testedWith");
	ExpectError("api", [](auto &d) { Set(d, "compat.api", Int(0)); }, "compat.api");
	ExpectError("api order", [](auto &d) {
		Set(d, "compat.api", cbor::Value::FromMap({ { "min", Int(3) }, { "max", Int(2) } }));
	}, "max is below min");
	ExpectError("platforms", [](auto &d) { Set(d, "compat.platforms", Array({ "linux" })); }, "unknown key");
	ExpectError("platform versions", [](auto &d) { Set(d, "compat.platformVersions.windows", Text(">=10")); }, "unknown key");
	ExpectError("requires", [](auto &d) { Set(d, "compat.requires", Array({ "Bad Feature" })); }, "requires");
	ExpectError("kind", [](auto &d) { Set(d, "runtime.kind", Text("native")); }, "runtime.kind");
	ExpectError("module extension", [](auto &d) { Set(d, "runtime.module", Text("plugin.so")); }, "runtime.module");
	ExpectError("module path", [](auto &d) { Set(d, "runtime.module", Text("../plugin.wasm")); }, "runtime.module");
	ExpectError("absolute path", [](auto &d) { Set(d, "runtime.module", Text("/plugin.wasm")); }, "runtime.module");
	ExpectError("entry on wasm", [](auto &d) { Set(d, "runtime.entry", Text("main.py")); }, "runtime.entry");
	ExpectError("abi", [](auto &d) { Set(d, "runtime.abi", Int(2)); }, "ABI");
	ExpectError("limits", [](auto &d) { Set(d, "runtime.limits.memoryMb", Int(100000)); }, "memoryMb");
	ExpectError("module not listed", [](auto &d) { Set(d, "runtime.module", Text("other.wasm")); }, "does not list the runtime file");
	ExpectError("permission", [](auto &d) {
		Set(d, "permissions.required", cbor::Value::FromArray({
			cbor::Value::FromMap({ { "id", Text("root.access") }, { "reason", Text("x") } }),
		}));
	}, "unknown permission");
	ExpectError("duplicate permission", [](auto &d) {
		Set(d, "permissions.optional", cbor::Value::FromArray({
			cbor::Value::FromMap({ { "id", Text("messages.read") }, { "reason", Text("x") } }),
		}));
	}, "listed twice");
	ExpectError("no reason", [](auto &d) {
		Set(d, "permissions.required", cbor::Value::FromArray({
			cbor::Value::FromMap({ { "id", Text("messages.read") } }),
		}));
	}, "reason");
	for (const auto old : { "net.hosts", "net.any" }) {
		ExpectError("old network permission", [&](auto &d) {
			Set(d, "permissions.optional", cbor::Value::FromArray({
				cbor::Value::FromMap({ { "id", Text(old) }, { "reason", Text("x") } }),
			}));
		}, "unknown permission");
	}
	ExpectError("hosts list", [](auto &d) {
		Set(d, "permissions.optional", cbor::Value::FromArray({
			cbor::Value::FromMap({ { "id", Text("net.https") }, { "reason", Text("x") }, { "hosts", Array({ "a.example.com" }) } }),
		}));
	}, "unknown key");
	ExpectError("messages without permission", [](auto &d) {
		Set(d, "contributes.messages.from", Array({ "com.example.other" }));
	}, "plugins.message");
	ExpectError("bad sender", [](auto &d) {
		Set(d, "permissions.optional", cbor::Value::FromArray({
			cbor::Value::FromMap({ { "id", Text("plugins.message") }, { "reason", Text("x") } }),
		}));
		Set(d, "contributes.messages.from", Array({ "Not An Id" }));
	}, "plugin ids");
	ExpectError("outgoing without permission", [](auto &d) {
		Set(d, "contributes.outgoing.onFailure", Text("block"));
	}, "messages.transform_outgoing");
	ExpectError("outgoing policy", [](auto &d) {
		Set(d, "permissions.optional", cbor::Value::FromArray({
			cbor::Value::FromMap({ { "id", Text("messages.transform_outgoing") }, { "reason", Text("x") } }),
		}));
		Set(d, "contributes.outgoing.onFailure", Text("maybe"));
	}, "onFailure");
	ExpectError("incoming without permission", [](auto &d) {
		Set(d, "contributes.incoming.onFailure", Text("block"));
	}, "messages.transform_incoming");
	ExpectError("incoming policy", [](auto &d) {
		Set(d, "permissions.optional", cbor::Value::FromArray({
			cbor::Value::FromMap({ { "id", Text("messages.transform_incoming") }, { "reason", Text("x") } }),
		}));
		Set(d, "contributes.incoming.onFailure", Text("maybe"));
	}, "onFailure");
	ExpectError("incoming mode", [](auto &d) {
		Set(d, "permissions.optional", cbor::Value::FromArray({
			cbor::Value::FromMap({ { "id", Text("messages.transform_incoming") }, { "reason", Text("x") } }),
		}));
		Set(d, "contributes.incoming.mode", Text("later"));
	}, "mode");
	ExpectError("outgoing has no mode", [](auto &d) {
		Set(d, "permissions.optional", cbor::Value::FromArray({
			cbor::Value::FromMap({ { "id", Text("messages.transform_outgoing") }, { "reason", Text("x") } }),
		}));
		Set(d, "contributes.outgoing.mode", Text("hold"));
	}, "mode");
	ExpectError("unknown event", [](auto &d) {
		Set(d, "contributes.events", Array({ "chat.shouting" }));
	}, "unknown event");
	for (const auto &item : std::vector<std::pair<const char *, const char *>>{
			{ "chat.typing", "chats.activity" },
			{ "chat.read", "chats.activity" },
			{ "chat.member", "chats.activity" },
			{ "chat.title_changed", "chats.activity" },
			{ "story.new", "stories.read" },
			{ "call.incoming", "calls.activity" },
			{ "call.ended", "calls.activity" },
			{ "user.status", "users.presence" },
			{ "app.focus", "app.activity" },
			{ "app.blur", "app.activity" } }) {
		ExpectError(item.first, [event = item.first](auto &d) {
			Set(d, "contributes.events", Array({ event }));
		}, std::string(item.second) + " permission");
	}
	ExpectError("composer without permission", [](auto &d) {
		Set(d, "permissions.optional", cbor::Value::FromArray({
			cbor::Value::FromMap({ { "id", Text("ui.menu") }, { "reason", Text("x") } }),
		}));
		Set(d, "contributes.menus", cbor::Value::FromArray({
			cbor::Value::FromMap({ { "surface", Text("composer") }, { "id", Text("go") }, { "title", Text("Go") } }),
		}));
	}, "ui.composer_button");
	ExpectError("scope", [](auto &d) {
		Set(d, "permissions.required", cbor::Value::FromArray({
			cbor::Value::FromMap({ { "id", Text("messages.read") }, { "reason", Text("x") }, { "scope", Text("everything") } }),
		}));
	}, "scope");
	ExpectError("events without permission", [](auto &d) {
		Set(d, "permissions.required", cbor::Value::FromArray({}));
	}, "needs the messages.read");
	ExpectError("unknown event", [](auto &d) { Set(d, "contributes.events", Array({ "chat.closed" })); }, "unknown event");
	ExpectError("menu without permission", [](auto &d) {
		Set(d, "contributes.menus", cbor::Value::FromArray({
			cbor::Value::FromMap({ { "surface", Text("message") }, { "id", Text("go") }, { "title", Text("Go") } }),
		}));
	}, "ui.menu");
	ExpectError("command without permission", [](auto &d) {
		Set(d, "contributes.commands", cbor::Value::FromArray({
			cbor::Value::FromMap({ { "name", Text("tr") } }),
		}));
	}, "ui.command");
	ExpectError("link without permission", [](auto &d) {
		Set(d, "contributes.links", cbor::Value::FromArray({
			cbor::Value::FromMap({ { "path", Text("stats") } }),
		}));
	}, "ui.link");
	ExpectError("bad link path", [](auto &d) {
		Set(d, "contributes.links", cbor::Value::FromArray({
			cbor::Value::FromMap({ { "path", Text("a/b") } }),
		}));
	}, "bad link path");
	ExpectError("settings page without permission", [](auto &d) {
		Set(d, "contributes.settingsPage", cbor::Value::FromBool(true));
	}, "ui.settings_page");
	ExpectError("background without permission", [](auto &d) {
		Set(d, "activation.background", cbor::Value::FromBool(true));
	}, "background permission");
	ExpectError("self dependency", [](auto &d) {
		Set(d, "dependencies", cbor::Value::FromArray({
			cbor::Value::FromMap({ { "id", Text("com.example.demo") }, { "version", Text("*") } }),
		}));
	}, "itself");
	ExpectError("dependency without range", [](auto &d) {
		Set(d, "dependencies", cbor::Value::FromArray({
			cbor::Value::FromMap({ { "id", Text("com.example.other") } }),
		}));
	}, "version");
	ExpectError("http url", [](auto &d) { Set(d, "homepage", Text("http://example.com")); }, "https");
	ExpectError("ip url", [](auto &d) { Set(d, "support", Text("https://127.0.0.1/")); }, "https");
	ExpectError("feed", [](auto &d) { Set(d, "updates.feed", Text("ftp://example.com/x")); }, "https");
	ExpectError("icon not listed", [](auto &d) { Set(d, "icon", Text("missing.png")); }, "does not list");
	ExpectError("files bad hash", [](auto &d) {
		Set(d, "files", cbor::Value::FromMap({ { "plugin.wasm", Text("md5-abc") } }));
	}, "bad hash");
	ExpectError("files bad path", [](auto &d) {
		Set(d, "files", cbor::Value::FromMap({ { "../x", Text(HashText(Bytes("x"))) } }));
	}, "bad path");
	ExpectError("signature alg", [](auto &d) {
		Set(d, "signature", cbor::Value::FromMap({ { "alg", Text("rsa") }, { "publicKey", Text("AAAA") }, { "value", Text("AAAA") } }));
	}, "ed25519");
	ExpectError("signature key", [](auto &d) {
		Set(d, "signature", cbor::Value::FromMap({ { "alg", Text("ed25519") }, { "publicKey", Text("AAAA") }, { "value", Text("AAAA") } }));
	}, "malformed");
}

void TestPythonRuntime() {
	auto document = ValidDocument();
	Set(document, "runtime.kind", Text("python"));
	Remove(document, "runtime.module");
	Set(document, "runtime.entry", Text("main.py"));
	Set(document, "runtime.pythonVersion", Text("3.14"));
	const auto result = Parse(document);
	CHECK(result.ok() && result.value().entry == "main.py");
	Set(document, "runtime.entry", Text("main.js"));
	CHECK(!Parse(document).ok());
	Set(document, "runtime.entry", Text("main.py"));
	Set(document, "runtime.module", Text("plugin.wasm"));
	CHECK(!Parse(document).ok());
}

void TestCompatibility() {
	auto manifest = Parse(ValidDocument()).value();
	const auto environment = [] {
		auto result = HostEnvironment();
		result.app = *ParseVersion("7.6.1");
		result.apiLevel = 1;
		result.features = { "ui.menu", "net.http" };
		return result;
	};
	CHECK(CheckCompatibility(manifest, environment()).empty());

	auto old = environment();
	old.app = *ParseVersion("7.5.9");
	auto issues = CheckCompatibility(manifest, old);
	CHECK(issues.size() == 1 && issues[0].problem == Problem::AppTooOld);

	auto fresh = environment();
	fresh.app = *ParseVersion("8.0.0");
	issues = CheckCompatibility(manifest, fresh);
	CHECK(issues.size() == 1 && issues[0].problem == Problem::AppTooNew);

	auto api = environment();
	api.apiLevel = 2;
	CHECK(CheckCompatibility(manifest, api).empty());
	auto capped = manifest;
	capped.apiMax = 1;
	issues = CheckCompatibility(capped, api);
	CHECK(issues.size() == 1 && issues[0].problem == Problem::ApiUnsupported);
	auto newer = manifest;
	newer.apiMin = 3;
	issues = CheckCompatibility(newer, api);
	CHECK(issues.size() == 1 && issues[0].problem == Problem::ApiUnsupported);

	manifest.needs = { "ui.menu", "ui.canvas" };
	issues = CheckCompatibility(manifest, environment());
	CHECK(issues.size() == 1 && issues[0].problem == Problem::MissingFeature);
	CHECK(issues[0].detail == "needs ui.canvas");

	manifest.needs = { "net.udp", "menu.tray", "ui.dialog", "plugins.message" };
	CHECK(CheckCompatibility(manifest, HostEnvironment{ *ParseVersion("7.6.1") }).empty());
	auto noTray = HostEnvironment{ *ParseVersion("7.6.1") };
	noTray.features.erase("menu.tray");
	issues = CheckCompatibility(manifest, noTray);
	CHECK(issues.size() == 1 && issues[0].detail == "needs menu.tray");
	manifest.needs.clear();

	manifest.premiumOnly = true;
	issues = CheckCompatibility(manifest, environment());
	CHECK(issues.size() == 1 && issues[0].problem == Problem::PremiumRequired);

	manifest.appRange = *VersionRange::Parse(">=9.0.0");
	manifest.apiMin = 6;
	manifest.apiMax = 6;
	CHECK(CheckCompatibility(manifest, environment()).size() == 3);
}

void TestStorageExport() {
	const auto withExport = [](cbor::Value value) {
		return [=](cbor::Value &d) { Set(d, "storage.export", value); };
	};
	ExpectError("export key", withExport(cbor::Value::FromMap({
		{ "prefixes", Array({ "a" }) }, { "all", Int(1) } })), "all");
	ExpectError("export without prefixes", withExport(cbor::Value::FromMap({})), "prefix");
	ExpectError("export bad prefix", withExport(cbor::Value::FromMap({
		{ "prefixes", Array({ "a\nb" }) } })), "prefix");
	auto document = ValidDocument();
	withExport(cbor::Value::FromMap({
		{ "prefixes", Array({ "settings.", "theme" }) },
		{ "exclude", Array({ "settings.token" }) },
	}))(document);
	const auto parsed = Parse(document);
	CHECK(parsed.ok());
	if (parsed.ok()) {
		const auto &decl = parsed.value().storageExport;
		CHECK(decl.includes("settings.volume") && decl.includes("theme"));
		CHECK(!decl.includes("settings.token") && !decl.includes("other"));
		CHECK(!StorageExport().includes("settings.volume"));
	}
	ExpectError("description control", [](auto &d) {
		Set(d, "description", Text("bad \x01 text"));
	}, "description");
	ExpectError("name bidi", [](auto &d) {
		Set(d, "name", Text("Safe \xE2\x80\xAE" "txet"));
	}, "name");
	ExpectError("author newline", [](auto &d) {
		Set(d, "author.name", Text("a\nb"));
	}, "author.name");
}

void TestTranslatorManifest() {
	const auto withTranslator = [](cbor::Value translator, bool permission = true) {
		return [=](cbor::Value &d) {
			if (permission) {
				Set(d, "permissions.optional", cbor::Value::FromArray({
					cbor::Value::FromMap({ { "id", Text("translate.provide") }, { "reason", Text("x") } }),
				}));
			}
			Set(d, "contributes.translator", translator);
		};
	};
	ExpectError("translator permission", withTranslator(cbor::Value::FromMap({
		{ "name", Text("T") } }), false), "translate.provide");
	ExpectError("translator name", withTranslator(cbor::Value::FromMap({})), "name");
	ExpectError("translator key", withTranslator(cbor::Value::FromMap({
		{ "name", Text("T") }, { "speed", Int(1) } })), "speed");
	ExpectError("translator language", withTranslator(cbor::Value::FromMap({
		{ "name", Text("T") }, { "languages", Array({ "Not A Code!" }) } })), "language");
	ExpectError("translator limit", withTranslator(cbor::Value::FromMap({
		{ "name", Text("T") }, { "maxChars", Int(10) } })), "maxChars");

	auto document = ValidDocument();
	withTranslator(cbor::Value::FromMap({
		{ "name", Text("Shout") },
		{ "languages", Array({ "en", "pt-br" }) },
		{ "maxChars", Int(2000) },
	}))(document);
	const auto parsed = Parse(document);
	CHECK(parsed.ok());
	if (parsed.ok()) {
		const auto &decl = parsed.value().translator;
		CHECK(decl.declared && decl.name == "Shout" && decl.languages.size() == 2);
		CHECK(decl.maxChars == 2000);
		CHECK(parsed.value().activation.contains("translate"));
	}
	CHECK(HostFeatures().contains("translate.provide"));
}

void TestAiProviderManifest() {
	const auto withProvider = [](cbor::Value provider, bool permission = true) {
		return [=](cbor::Value &d) {
			if (permission) {
				Set(d, "permissions.optional", cbor::Value::FromArray({
					cbor::Value::FromMap({ { "id", Text("ai.provide") }, { "reason", Text("x") } }),
				}));
			}
			Set(d, "contributes.aiProvider", provider);
		};
	};
	ExpectError("provider permission", withProvider(cbor::Value::FromMap({
		{ "name", Text("P") } }), false), "ai.provide");
	ExpectError("provider name", withProvider(cbor::Value::FromMap({})), "name");
	ExpectError("provider key", withProvider(cbor::Value::FromMap({
		{ "name", Text("P") }, { "speed", Int(1) } })), "speed");
	ExpectError("provider limit", withProvider(cbor::Value::FromMap({
		{ "name", Text("P") }, { "maxChars", Int(10) } })), "maxChars");

	auto document = ValidDocument();
	withProvider(cbor::Value::FromMap({
		{ "name", Text("My model") },
		{ "maxChars", Int(2000) },
	}))(document);
	const auto parsed = Parse(document);
	CHECK(parsed.ok());
	if (parsed.ok()) {
		const auto &decl = parsed.value().aiProvider;
		CHECK(decl.declared && decl.name == "My model" && decl.maxChars == 2000);
		CHECK(parsed.value().activation.contains("ai"));
	}
	CHECK(HostFeatures().contains("ai.provide"));
	CHECK(PermissionDanger("ai.provide") == Danger::High);
}

void TestTranscriberManifest() {
	const auto withTranscriber = [](cbor::Value decl, bool permission = true) {
		return [=](cbor::Value &d) {
			if (permission) {
				Set(d, "permissions.optional", cbor::Value::FromArray({
					cbor::Value::FromMap({ { "id", Text("transcribe.provide") }, { "reason", Text("x") } }),
					cbor::Value::FromMap({ { "id", Text("media.read") }, { "reason", Text("x") } }),
				}));
			}
			Set(d, "contributes.transcriber", decl);
		};
	};
	ExpectError("transcriber permission", withTranscriber(cbor::Value::FromMap({
		{ "name", Text("P") } }), false), "transcribe.provide");
	ExpectError("transcriber media", [](auto &d) {
		Set(d, "permissions.optional", cbor::Value::FromArray({
			cbor::Value::FromMap({ { "id", Text("transcribe.provide") }, { "reason", Text("x") } }),
		}));
		Set(d, "contributes.transcriber", cbor::Value::FromMap({ { "name", Text("P") } }));
	}, "media.read");
	ExpectError("transcriber name", withTranscriber(cbor::Value::FromMap({})), "name");
	ExpectError("transcriber limit", withTranscriber(cbor::Value::FromMap({
		{ "name", Text("P") }, { "maxSeconds", Int(1) } })), "maxSeconds");
	auto document = ValidDocument();
	withTranscriber(cbor::Value::FromMap({ { "name", Text("Whisper") }, { "maxSeconds", Int(120) } }))(document);
	const auto parsed = Parse(document);
	CHECK(parsed.ok());
	if (parsed.ok()) {
		const auto &decl = parsed.value().transcriber;
		CHECK(decl.declared && decl.name == "Whisper" && decl.maxSeconds == 120);
		CHECK(parsed.value().activation.contains("transcribe"));
	}
	CHECK(HostFeatures().contains("transcribe.provide"));
	CHECK(PermissionDanger("transcribe.provide") == Danger::High);
}

void TestMenuConditions() {
	const auto withMenu = [](const char *surface, cbor::Value when) {
		return [=](cbor::Value &d) {
			Set(d, "permissions.optional", cbor::Value::FromArray({
				cbor::Value::FromMap({ { "id", Text("ui.menu") }, { "reason", Text("x") } }),
			}));
			Set(d, "contributes.menus", cbor::Value::FromArray({
				cbor::Value::FromMap({
					{ "surface", Text(surface) }, { "id", Text("x") },
					{ "title", Text("X") }, { "when", when },
				}),
			}));
		};
	};
	ExpectError("unknown key", withMenu("message", cbor::Value::FromMap({ { "color", Int(1) } })), "color");
	ExpectError("message key on a chat", withMenu("chat", cbor::Value::FromMap({
		{ "out", cbor::Value::FromBool(true) } })), "does not apply");
	ExpectError("chat type on the tray", withMenu("tray", cbor::Value::FromMap({
		{ "chat_type", Array({ "group" }) } })), "does not apply");
	ExpectError("bad media", withMenu("message", cbor::Value::FromMap({
		{ "media", Array({ "hologram" }) } })), "media");
	ExpectError("bad flag", withMenu("selection", cbor::Value::FromMap({ { "forwarded", Int(1) } })), "forwarded");

	auto document = ValidDocument();
	withMenu("shared_media", cbor::Value::FromMap({
		{ "chat_type", Array({ "group", "channel" }) },
		{ "media", Array({ "photo", "video" }) },
		{ "out", cbor::Value::FromBool(true) },
		{ "has_text", cbor::Value::FromBool(false) },
	}))(document);
	const auto parsed = Parse(document);
	CHECK(parsed.ok());
	if (!parsed.ok()) {
		return;
	}
	const auto &when = parsed.value().menus.front().when;
	CHECK(!when.empty() && when.chatTypes.size() == 2 && when.media.size() == 2);
	CHECK(HostFeatures().contains("menu.selection") && HostFeatures().contains("menu.shared_media"));

	auto context = MenuContext();
	context.chatType = "group";
	context.out = true;
	context.media = "photo";
	CHECK(Matches(when, context));
	context.hasText = true;
	CHECK(!Matches(when, context));
	context.hasText = false;
	context.out = false;
	CHECK(!Matches(when, context));
	context.out = true;
	context.media = "file";
	CHECK(!Matches(when, context));
	context.media = "photo";
	context.chatType = "user";
	CHECK(!Matches(when, context));

	auto hasMedia = MenuWhen();
	hasMedia.hasMedia = true;
	auto preview = MenuContext();
	preview.media = "webpage";
	CHECK(!Matches(hasMedia, preview));
	preview.media = "voice";
	CHECK(Matches(hasMedia, preview));
	CHECK(Matches(MenuWhen(), MenuContext()));
}

void TestLocalPorts() {
	const auto withPorts = [](std::optional<cbor::Value> ports, const char *id = "net.local") {
		return [=](cbor::Value &d) {
			auto entry = cbor::Map{ { "id", Text(id) }, { "reason", Text("x") } };
			if (ports) {
				entry.push_back({ "ports", *ports });
			}
			Set(d, "permissions.optional", cbor::Value::FromArray({ cbor::Value::FromMap(std::move(entry)) }));
		};
	};
	ExpectError("local without ports", withPorts(std::nullopt), "ports");
	ExpectError("local with no ports", withPorts(cbor::Value::FromArray({})), "ports");
	ExpectError("local port zero", withPorts(cbor::Value::FromArray({ Int(0) })), "ports");
	ExpectError("local port high", withPorts(cbor::Value::FromArray({ Int(70000) })), "ports");
	ExpectError("local bad range", withPorts(cbor::Value::FromArray({ Text("9000-8000") })), "ports");
	ExpectError("local text range", withPorts(cbor::Value::FromArray({ Text("a-b") })), "ports");
	ExpectError("local entry", withPorts(cbor::Value::FromArray({ cbor::Value::FromBool(true) })), "ports");
	auto many = cbor::Array();
	for (auto i = 1; i != 18; ++i) {
		many.push_back(Int(i));
	}
	ExpectError("local too many", withPorts(cbor::Value::FromArray(std::move(many))), "ports");
	ExpectError("ports elsewhere", withPorts(cbor::Value::FromArray({ Int(80) }), "net.https"), "net.local");

	auto document = ValidDocument();
	withPorts(cbor::Value::FromArray({ Int(8080), Text("9000-9100") }))(document);
	const auto parsed = Parse(document);
	CHECK(parsed.ok());
	if (!parsed.ok()) {
		return;
	}
	const auto &ports = parsed.value().permissions.back().ports;
	CHECK(ports.size() == 2 && ports[0] == std::pair(8080, 8080) && ports[1] == std::pair(9000, 9100));
	const auto grants = MakeGrants(parsed.value(), { { "net.local" }, std::nullopt });
	CHECK(grants.permissions.contains("net.local") && grants.localPorts == ports);
	CHECK(MakeGrants(parsed.value(), { {}, std::nullopt }).localPorts.empty());
	CHECK(HostFeatures().contains("net.local"));

	const auto version = [&](cbor::Value list) {
		auto other = ValidDocument();
		withPorts(std::move(list))(other);
		return Parse(other).value();
	};
	const auto widened = [&](cbor::Value before, cbor::Value after) {
		const auto added = NewRequests(version(std::move(before)), version(std::move(after)));
		return std::ranges::find(added, "net.local:ports") != added.end();
	};
	const auto list = [](std::initializer_list<cbor::Value> items) {
		return cbor::Value::FromArray(cbor::Array(items));
	};
	CHECK(widened(list({ Int(8080) }), list({ Text("1-65535") })));
	CHECK(widened(list({ Int(8080) }), list({ Int(8080), Int(8081) })));
	CHECK(widened(list({ Text("9000-9100") }), list({ Text("8999-9100") })));
	CHECK(!widened(list({ Text("1-65535") }), list({ Int(8080) })));
	CHECK(!widened(list({ Int(8080), Text("9000-9100") }), list({ Text("9010-9020"), Int(8080) })));
	CHECK(!widened(list({ Text("9000-9050"), Text("9051-9100") }), list({ Text("9000-9100") })));
	CHECK(!widened(list({ Int(8080) }), list({ Int(8080) })));
}

void TestFolderPermission() {
	const auto withMode = [](std::optional<const char*> mode, const char *id = "fs.folder") {
		return [=](cbor::Value &d) {
			auto entry = cbor::Map{ { "id", Text(id) }, { "reason", Text("x") } };
			if (mode) {
				entry.push_back({ "mode", Text(*mode) });
			}
			Set(d, "permissions.optional", cbor::Value::FromArray({ cbor::Value::FromMap(std::move(entry)) }));
		};
	};
	ExpectError("folder without mode", withMode(std::nullopt), "mode");
	ExpectError("folder bad mode", withMode("rw"), "mode");
	ExpectError("mode elsewhere", withMode("read", "clipboard"), "mode");

	for (const auto mode : { "read", "write" }) {
		auto document = ValidDocument();
		withMode(mode)(document);
		const auto parsed = Parse(document);
		CHECK(parsed.ok());
		if (!parsed.ok()) {
			return;
		}
		const auto writable = (std::string(mode) == "write");
		const auto chosen = MakeGrants(parsed.value(), { { "fs.folder" }, std::nullopt, "/some/dir" });
		CHECK(chosen.permissions.contains("fs.folder"));
		CHECK(chosen.folder == "/some/dir" && chosen.folderWritable == writable);
		const auto unset = MakeGrants(parsed.value(), { { "fs.folder" }, std::nullopt, "" });
		CHECK(!unset.permissions.contains("fs.folder") && unset.folder.empty());
		const auto declined = MakeGrants(parsed.value(), { {}, std::nullopt, "/some/dir" });
		CHECK(declined.folder.empty());
	}
	CHECK(PermissionDanger("fs.folder") == Danger::High);
	CHECK(HostFeatures().contains("fs.folder"));

	auto read = ValidDocument();
	withMode("read")(read);
	auto write = ValidDocument();
	withMode("write")(write);
	const auto before = Parse(read);
	const auto after = Parse(write);
	CHECK(before.ok() && after.ok());
	if (before.ok() && after.ok()) {
		const auto raised = NewRequests(before.value(), after.value());
		CHECK(raised.size() == 1 && raised[0] == "fs.folder:write");
		CHECK(NewRequests(after.value(), before.value()).empty());
	}
	const auto notes = RiskNotes({ "fs.folder", "net.https" });
	auto folderNotes = 0;
	for (const auto &note : notes) {
		folderNotes += (note.risk == Risk::Folder || note.risk == Risk::FolderExfiltration);
	}
	CHECK(folderNotes == 2);
}

void TestIncomingHook() {
	auto document = ValidDocument();
	Set(document, "permissions.optional", cbor::Value::FromArray({
		cbor::Value::FromMap({ { "id", Text("messages.transform_incoming") }, { "reason", Text("x") } }),
	}));
	Set(document, "contributes.incoming.mode", Text("hold"));
	Set(document, "contributes.incoming.onFailure", Text("block"));
	Set(document, "contributes.incoming.filter", cbor::Value::FromMap({
		{ "chats", cbor::Value::FromArray({ Int(5) }) },
		{ "chat_types", Array({ "group" }) },
	}));
	const auto parsed = Parse(document);
	CHECK(parsed.ok());
	if (!parsed.ok()) {
		return;
	}
	const auto &hook = parsed.value().incoming;
	CHECK(hook.declared && hook.block && hook.hold && !hook.service);
	CHECK(!parsed.value().outgoing.declared);
	auto info = IncomingInfo();
	info.chat = 5;
	info.chatType = "group";
	CHECK(Matches(hook.filter, info));
	info.chatType = "user";
	CHECK(!Matches(hook.filter, info));
	info.chatType = "group";
	info.chat = 6;
	CHECK(!Matches(hook.filter, info));

	auto plain = ValidDocument();
	Set(plain, "permissions.optional", cbor::Value::FromArray({
		cbor::Value::FromMap({ { "id", Text("messages.transform_incoming") }, { "reason", Text("x") } }),
	}));
	Set(plain, "contributes.incoming.onFailure", Text("pass"));
	Set(plain, "contributes.incoming.service", cbor::Value::FromBool(true));
	const auto serviced = Parse(plain);
	CHECK(serviced.ok() && serviced.value().incoming.service);
	Set(plain, "contributes.incoming.service", Text("yes"));
	CHECK(!Parse(plain).ok());
	Remove(plain, "contributes.incoming.service");
	const auto swapped = Parse(plain);
	CHECK(swapped.ok() && swapped.value().incoming.declared && !swapped.value().incoming.hold);
	CHECK(HostFeatures().contains("messages.transform_incoming"));
}

void TestOutgoingFilter() {
	const auto withHook = [](cbor::Value filter) {
		return [filter = std::move(filter)](cbor::Value &d) {
			Set(d, "permissions.optional", cbor::Value::FromArray({
				cbor::Value::FromMap({ { "id", Text("messages.transform_outgoing") }, { "reason", Text("x") } }),
			}));
			Set(d, "contributes.outgoing.filter", filter);
		};
	};
	ExpectError("filter key", withHook(cbor::Value::FromMap({ { "color", Int(1) } })), "color");
	ExpectError("filter chat type", withHook(cbor::Value::FromMap({
		{ "chat_types", Array({ "planet" }) } })), "chat_types");
	ExpectError("filter chats", withHook(cbor::Value::FromMap({
		{ "chats", cbor::Value::FromArray({ Text("x") }) } })), "chats");

	auto document = ValidDocument();
	withHook(cbor::Value::FromMap({
		{ "chats", cbor::Value::FromArray({ Int(5), Int(6) }) },
		{ "chat_types", Array({ "group", "channel" }) },
	}))(document);
	Set(document, "contributes.outgoing.onFailure", Text("block"));
	const auto parsed = Parse(document);
	CHECK(parsed.ok());
	if (!parsed.ok()) {
		return;
	}
	const auto &hook = parsed.value().outgoing;
	CHECK(hook.declared && hook.block && !hook.media);
	CHECK(hook.filter.chats == std::set<int64_t>({ 5, 6 }));
	CHECK(hook.filter.chatTypes.size() == 2);

	auto info = OutgoingInfo();
	info.chat = 5;
	info.chatType = "group";
	CHECK(Matches(hook.filter, info));
	info.chat = 7;
	CHECK(!Matches(hook.filter, info));
	info.chat = 6;
	info.chatType = "user";
	CHECK(!Matches(hook.filter, info));
	info.chatType = "channel";
	CHECK(Matches(hook.filter, info));
	CHECK(Matches(OutgoingFilter(), info));

	Set(document, "contributes.outgoing.media", cbor::Value::FromBool(true));
	const auto withMedia = Parse(document);
	CHECK(withMedia.ok() && withMedia.value().outgoing.media);
	Set(document, "contributes.outgoing.media", Text("yes"));
	CHECK(!Parse(document).ok());
}

void TestPrioritiesAndEventFilters() {
	const auto permit = [](cbor::Value &d, std::initializer_list<const char*> ids) {
		auto list = cbor::Array();
		for (const auto id : ids) {
			list.push_back(cbor::Value::FromMap({ { "id", Text(id) }, { "reason", Text("x") } }));
		}
		Set(d, "permissions.optional", cbor::Value::FromArray(std::move(list)));
	};
	auto document = ValidDocument();
	permit(document, { "messages.transform_outgoing", "messages.transform_incoming" });
	Set(document, "contributes.outgoing.priority", Int(50));
	Set(document, "contributes.outgoing.final", cbor::Value::FromBool(true));
	Set(document, "contributes.incoming.priority", Int(-3));
	const auto parsed = Parse(document);
	CHECK(parsed.ok());
	if (parsed.ok()) {
		CHECK(parsed.value().outgoing.priority == 50 && parsed.value().outgoing.final);
		CHECK(parsed.value().incoming.priority == -3);
	}
	Set(document, "contributes.outgoing.priority", Int(5000));
	CHECK(!Parse(document).ok());
	Set(document, "contributes.outgoing.priority", Int(1));
	Set(document, "contributes.incoming.final", cbor::Value::FromBool(true));
	CHECK(!Parse(document).ok());

	auto menus = ValidDocument();
	Set(menus, "permissions.optional", cbor::Value::FromArray({
		cbor::Value::FromMap({ { "id", Text("ui.menu") }, { "reason", Text("x") } }),
	}));
	Set(menus, "contributes.menus", cbor::Value::FromArray({
		cbor::Value::FromMap({
			{ "surface", Text("message") }, { "id", Text("a") }, { "title", Text("A") }, { "priority", Int(7) },
		}),
	}));
	const auto withMenu = Parse(menus);
	CHECK(withMenu.ok() && withMenu.value().menus.size() == 1 && withMenu.value().menus[0].priority == 7);

	auto filtered = ValidDocument();
	permit(filtered, { "chats.activity" });
	Set(filtered, "contributes.events", Array({ "message.new", "chat.typing" }));
	Set(filtered, "contributes.eventFilters", cbor::Value::FromMap({
		{ "message.new", cbor::Value::FromMap({
			{ "chat_types", Array({ "group" }) },
			{ "out", cbor::Value::FromBool(false) },
			{ "media", Array({ "photo" }) },
		}) },
		{ "chat.typing", cbor::Value::FromMap({ { "chats", cbor::Value::FromArray({ Int(5) }) } }) },
	}));
	const auto withFilters = Parse(filtered);
	CHECK(withFilters.ok());
	if (withFilters.ok()) {
		const auto &filters = withFilters.value().eventFilters;
		CHECK(filters.size() == 2);
		const auto message = [](const char *chatType, bool out, const char *media) {
			return cbor::Value::FromMap({
				{ "type", Text("message.new") },
				{ "chat", Int(1) },
				{ "chat_type", Text(chatType) },
				{ "out", cbor::Value::FromBool(out) },
				{ "media", Text(media) },
			});
		};
		const auto &filter = filters.at("message.new");
		CHECK(MatchesEvent(filter, message("group", false, "photo")));
		CHECK(!MatchesEvent(filter, message("user", false, "photo")));
		CHECK(!MatchesEvent(filter, message("group", true, "photo")));
		CHECK(!MatchesEvent(filter, message("group", false, "video")));
		const auto typing = [](int64_t chat) {
			return cbor::Value::FromMap({ { "type", Text("chat.typing") }, { "chat", Int(chat) } });
		};
		CHECK(MatchesEvent(filters.at("chat.typing"), typing(5)));
		CHECK(!MatchesEvent(filters.at("chat.typing"), typing(6)));
	}
	auto bad = filtered;
	Set(bad, "contributes.eventFilters", cbor::Value::FromMap({
		{ "chat.typing", cbor::Value::FromMap({ { "out", cbor::Value::FromBool(true) } }) },
	}));
	CHECK(!Parse(bad).ok());
	Set(bad, "contributes.eventFilters", cbor::Value::FromMap({
		{ "story.new", cbor::Value::FromMap({ { "chats", cbor::Value::FromArray({ Int(5) }) } }) },
	}));
	CHECK(!Parse(bad).ok());
}

void TestSignedTextIgnoresTheSignature() {
	auto document = ValidDocument();
	const auto before = SignedText(document);
	Set(document, "signature.alg", Text("ed25519"));
	CHECK(SignedText(document) == before);
	Set(document, "name", Text("Other"));
	CHECK(SignedText(document) != before);
}

} // namespace

void TestIcons() {
	auto document = ValidDocument();
	Set(document, "permissions.optional", cbor::Value::FromArray({
		cbor::Value::FromMap({ { "id", Text("ui.menu") }, { "reason", Text("x") } }),
	}));
	Set(document, "contributes.icons", cbor::Value::FromMap({
		{ "star-mark", Text("icon.png") },
	}));
	Set(document, "contributes.menus", cbor::Value::FromArray({
		cbor::Value::FromMap({ { "surface", Text("message") }, { "id", Text("a") }, { "title", Text("A") }, { "icon", Text("star-mark") } }),
		cbor::Value::FromMap({ { "surface", Text("chat") }, { "id", Text("b") }, { "title", Text("B") }, { "icon", Text("translate") } }),
	}));
	const auto parsed = Parse(document);
	CHECK(parsed.ok());
	if (parsed.ok()) {
		CHECK(parsed.value().icons.at("star-mark") == "icon.png");
		CHECK(parsed.value().menus[0].icon == "star-mark");
		CHECK(parsed.value().menus[1].icon == "translate");
	}
	const auto withMenus = [](auto &d, const char *icon) {
		Set(d, "permissions.optional", cbor::Value::FromArray({
			cbor::Value::FromMap({ { "id", Text("ui.menu") }, { "reason", Text("x") } }),
		}));
		Set(d, "contributes.menus", cbor::Value::FromArray({
			cbor::Value::FromMap({ { "surface", Text("message") }, { "id", Text("a") }, { "title", Text("A") }, { "icon", Text(icon) } }),
		}));
	};
	ExpectError("unknown icon", [&](auto &d) { withMenus(d, "nope"); }, "unknown icon");
	ExpectError("bad icon name", [&](auto &d) { withMenus(d, "Bad Name"); }, "bad icon name");
	ExpectError("icon type", [](auto &d) {
		Set(d, "contributes.icons", cbor::Value::FromMap({ { "a", Text("CHANGELOG.md") } }));
	}, ".png or .svg");
	ExpectError("icon file", [](auto &d) {
		Set(d, "contributes.icons", cbor::Value::FromMap({ { "a", Text("missing.svg") } }));
	}, "missing.svg");
}

int main() {
	TestOutdated();
	TestValidManifest();
	TestLocalizedAndOptionalForms();
	TestRejections();
	TestPythonRuntime();
	TestCompatibility();
	TestSignedTextIgnoresTheSignature();
	TestIcons();
	TestOutgoingFilter();
	TestPrioritiesAndEventFilters();
	TestIncomingHook();
	TestLocalPorts();
	TestFolderPermission();
	TestMenuConditions();
	TestTranslatorManifest();
	TestAiProviderManifest();
	TestTranscriberManifest();
	TestStorageExport();
	return Failures() ? 1 : 0;
}
