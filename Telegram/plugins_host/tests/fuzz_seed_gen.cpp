#include "fgpl/cbor.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

// Writes the starting corpus: fuzz_seed_gen <broker directory> <ui directory>.

namespace {

using namespace fgpl;
namespace fs = std::filesystem;

using Fields = std::vector<std::pair<std::string, cbor::Value>>;

[[nodiscard]] cbor::Value Text(std::string text) {
	return cbor::Value::FromText(std::move(text));
}

[[nodiscard]] cbor::Value Int(int64_t number) {
	return cbor::Value::FromInt(number);
}

[[nodiscard]] cbor::Value Map(Fields fields) {
	auto map = cbor::Map();
	for (auto &field : fields) {
		map.push_back({ field.first, std::move(field.second) });
	}
	return cbor::Value::FromMap(std::move(map));
}

[[nodiscard]] cbor::Value Button(const char *id) {
	return Map({ { "type", Text("button") }, { "id", Text(id) }, { "label", Text("Go") } });
}

[[nodiscard]] cbor::Value Page(std::vector<cbor::Value> items) {
	return Map({ { "title", Text("Demo") }, { "items", cbor::Value::FromArray(std::move(items)) } });
}

void Write(const fs::path &path, const std::string &prefix, const cbor::Value &value) {
	auto bytes = cbor::Bytes();
	if (!cbor::Encode(value, bytes)) {
		std::fprintf(stderr, "cannot encode %s\n", path.c_str());
		return;
	}
	auto out = std::ofstream(path, std::ios::binary);
	out.write(prefix.data(), std::streamsize(prefix.size()));
	out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
}

} // namespace

int main(int argc, char **argv) {
	if (argc != 3) {
		std::fprintf(stderr, "usage: fuzz_seed_gen <broker directory> <ui directory>\n");
		return 1;
	}
	const auto broker = fs::path(argv[1]);
	const auto ui = fs::path(argv[2]);
	fs::create_directories(broker);
	fs::create_directories(ui);

	const auto chat = Int(1);
	const auto call = [&](const char *op, Fields fields = {}) {
		return Map({ { "op", Text(op) }, { "a", Map(std::move(fields)) } });
	};
	const auto requests = std::vector<cbor::Value>{
		call("meta.info"),
		call("log", { { "message", Text("hi") } }),
		call("account.info"),
		call("stars.balance"),
		call("settings.read"),
		call("folders.list"),
		call("contacts.list", { { "limit", Int(10) } }),
		call("chats.list", { { "limit", Int(10) } }),
		call("chat.info", { { "chat", chat } }),
		call("chats.mark_read", { { "chat", chat } }),
		call("chats.action", { { "chat", chat }, { "kind", Text("typing") } }),
		call("messages.history", { { "chat", chat }, { "limit", Int(5) } }),
		call("messages.search", { { "chat", chat }, { "query", Text("hello") } }),
		call("messages.send", { { "chat", chat }, { "text", Text("yo") } }),
		call("messages.send_media", { { "chat", chat }, { "name", Text("a.jpg") }, { "kind", Text("photo") },
			{ "data", cbor::Value::FromBytes({ 1, 2, 3 }) } }),
		call("messages.edit", { { "chat", chat }, { "id", Int(100) }, { "text", Text("edited") } }),
		call("messages.edit_media", { { "chat", chat }, { "id", Int(100) }, { "name", Text("a.jpg") },
			{ "kind", Text("photo") }, { "data", cbor::Value::FromBytes({ 1, 2, 3 }) } }),
		call("messages.delete", { { "chat", chat }, { "ids", cbor::Value::FromArray({ Int(100) }) } }),
		call("messages.forward", { { "chat", chat }, { "ids", cbor::Value::FromArray({ Int(100) }) }, { "to", Int(2) } }),
		call("messages.react", { { "chat", chat }, { "id", Int(100) }, { "emoji", Text("x") } }),
		call("messages.reactions", { { "chat", chat }, { "id", Int(100) } }),
		call("messages.set_outgoing", { { "text", Text("changed") } }),
		call("messages.drop_outgoing"),
		call("composer.draft", { { "chat", chat } }),
		call("composer.set_draft", { { "chat", chat }, { "text", Text("draft") } }),
		call("media.get", { { "chat", chat }, { "id", Int(100) } }),
		call("media.list", { { "chat", chat }, { "kind", Text("photo") } }),
		call("users.info", { { "user", Int(7) } }),
		call("users.resolve", { { "username", Text("bobby") } }),
		call("bots.inline_query", { { "bot", Text("inlinebot") }, { "query", Text("cats") } }),
		call("time.now"),
		call("chats.members", { { "chat", chat } }),
		call("chats.admins", { { "chat", chat } }),
		call("messages.send_poll", { { "chat", chat }, { "question", Text("q?") }, { "options", cbor::Value::FromArray({ Text("a"), Text("b") }) } }),
		call("ghost.get"),
		call("ghost.set", { { "enabled", cbor::Value::FromBool(true) } }),
		call("messages.get", { { "chat", chat }, { "id", Int(100) } }),
		call("messages.reply_chain", { { "chat", chat }, { "id", Int(103) } }),
		call("messages.pinned", { { "chat", chat } }),
		call("messages.scheduled", { { "chat", chat } }),
		call("messages.pin", { { "chat", chat }, { "id", Int(100) } }),
		call("chats.mute", { { "chat", chat }, { "seconds", Int(60) } }),
		call("chats.archive", { { "chat", chat } }),
		call("users.block", { { "id", Int(7) } }),
		call("contacts.add", { { "id", Int(7) }, { "first_name", Text("Bob") } }),
		call("bots.press_button", { { "chat", chat }, { "id", Int(100) }, { "row", Int(0) }, { "column", Int(0) } }),
		call("bots.send_inline_result", { { "chat", chat }, { "query_id", Int(77) }, { "id", Text("r1") } }),
		call("composer.insert", { { "chat", chat }, { "text", Text("hi") } }),
		call("ui.open_settings"),
		call("messages.history", { { "chat", chat }, { "from_user", Text("me") } }),
		call("messages.set_outgoing", { { "entities", cbor::Value::FromArray({ Map({ { "type", Text("bold") }, { "offset", Int(0) }, { "length", Int(2) } }) }) } }),
		call("users.photo", { { "user", Int(7) } }),
		call("channels.info", { { "id", Int(2) } }),
		call("channels.resolve", { { "username", Text("newsy") } }),
		call("stickers.search", { { "emoji", Text("x") } }),
		call("stickers.recent"),
		call("stickers.send", { { "chat", chat }, { "id", Int(321) } }),
		call("clipboard.write", { { "text", Text("copy") } }),
		call("clipboard.read"),
		call("notify.show", { { "title", Text("t") }, { "text", Text("x") } }),
		call("ui.toast", { { "text", Text("toast") } }),
		call("ui.settings.set", { { "page", Page({ Button("go") }) } }),
		call("ui.dialog.show", { { "id", Text("d") }, { "page", Page({ Button("go") }) } }),
		call("ui.dialog.close", { { "id", Text("d") } }),
		call("ui.window.update", { { "id", Text("w") }, { "page", Page({ Button("go") }) } }),
		call("ui.window.close", { { "id", Text("w") } }),
		call("badges.set", { { "user", Int(7) }, { "items", cbor::Value::FromArray({}) } }),
		call("badges.clear"),
		call("storage.set", { { "key", Text("k") }, { "value", Text("v") } }),
		call("storage.get", { { "key", Text("k") } }),
		call("storage.list", { { "prefix", Text("") } }),
		call("storage.delete", { { "key", Text("k") } }),
		call("storage.usage"),
		call("timers.set", { { "id", Text("t") }, { "delay", Int(1000) } }),
		call("timers.list"),
		call("timers.clear", { { "id", Text("t") } }),
		call("plugins.peers"),
		call("plugins.send", { { "to", Text("com.example.peer") }, { "data", Text("x") } }),
		call("net.fetch", { { "url", Text("https://api.example.com/x") }, { "method", Text("POST") },
			{ "body", cbor::Value::FromBytes({ 1 }) } }),
		call("net.tcp.connect", { { "host", Text("example.com") }, { "port", Int(443) } }),
		call("net.udp.connect", { { "host", Text("example.com") }, { "port", Int(53) } }),
		call("net.socket.send", { { "id", Int(1) }, { "data", cbor::Value::FromBytes({ 1 }) } }),
		call("net.socket.recv", { { "id", Int(1) }, { "max", Int(16) } }),
		call("net.socket.close", { { "id", Int(1) } }),
		call("plugin.unload"),
	};
	auto index = 0;
	for (const auto &request : requests) {
		const auto name = std::to_string(index++);
		Write(broker / ("all_" + name), std::string(1, '\0'), request);
		Write(broker / ("sel_" + name), std::string(1, '\1'), request);
		Write(broker / ("none_" + name), std::string(1, '\2'), request);
	}

	const auto pages = std::vector<cbor::Value>{
		Page({}),
		Page({ Button("go") }),
		Page({ Map({ { "type", Text("toggle") }, { "id", Text("t") }, { "label", Text("L") },
			{ "value", cbor::Value::FromBool(true) } }) }),
		Page({ Map({ { "type", Text("row") }, { "items", cbor::Value::FromArray({ Button("a"), Button("b") }) } }) }),
		Page({ Map({ { "type", Text("text") }, { "text", Text("hello") } }),
			Map({ { "type", Text("link") }, { "label", Text("x") }, { "url", Text("https://example.com") } }) }),
		Page({ Map({ { "type", Text("slider") }, { "id", Text("s") }, { "label", Text("L") },
			{ "min", Int(0) }, { "max", Int(10) }, { "value", Int(5) } }) }),
		cbor::Value::FromArray({ Map({ { "text", Text("badge") } }) }),
	};
	index = 0;
	for (const auto &page : pages) {
		Write(ui / ("page_" + std::to_string(index++)), std::string(), page);
	}
	return 0;
}
