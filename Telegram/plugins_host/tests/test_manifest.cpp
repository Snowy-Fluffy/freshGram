#include "check.h"
#include "fgpl/manifest.h"
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
	CHECK(hook.declared && hook.block && hook.hold);
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
	CHECK(hook.declared && hook.block);
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
	TestIncomingHook();
	TestMenuConditions();
	TestTranslatorManifest();
	TestStorageExport();
	return Failures() ? 1 : 0;
}
