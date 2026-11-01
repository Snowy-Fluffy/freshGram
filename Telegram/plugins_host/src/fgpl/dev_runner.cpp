#include "fgpl/dev_runner.h"

#include "fgpl/dev_data.h"
#include "fgpl/directory_package.h"
#include "fgpl/file_storage.h"
#include "fgpl/json.h"
#include "fgpl/manager.h"
#include "fgpl/package.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fgpl::dev {
namespace {

namespace fs = std::filesystem;

constexpr auto kDrainMs = 10000;
constexpr auto kTickMs = 50;

class Console {
public:
	void say(const std::string &line) {
		const auto lock = std::scoped_lock(_mutex);
		std::fputs(line.c_str(), stdout);
		std::fputc('\n', stdout);
		std::fflush(stdout);
	}

private:
	std::mutex _mutex;

};

struct Lines {
	std::mutex mutex;
	std::condition_variable wake;
	std::deque<std::string> queue;
	bool eof = false;
};

struct Token {
	std::string text;
	bool quoted = false;
};

[[nodiscard]] std::vector<Token> Tokenize(const std::string &line, bool &ok) {
	auto result = std::vector<Token>();
	auto current = Token();
	auto inside = false;
	auto open = false;
	ok = true;
	for (auto i = size_t(0); i != line.size(); ++i) {
		const auto ch = line[i];
		if (inside) {
			if (ch == '\\' && i + 1 != line.size()) {
				const auto next = line[++i];
				current.text += (next == 'n') ? '\n' : next;
			} else if (ch == '"') {
				inside = false;
			} else {
				current.text += ch;
			}
		} else if (ch == '"') {
			inside = true;
			open = true;
			if (current.text.empty()) {
				current.quoted = true;
			}
		} else if (ch == ' ' || ch == '\t' || ch == '\r') {
			if (open || !current.text.empty()) {
				result.push_back(std::move(current));
				current = Token();
				open = false;
			}
		} else {
			current.text += ch;
			open = true;
		}
	}
	ok = !inside;
	if (open || !current.text.empty()) {
		result.push_back(std::move(current));
	}
	return result;
}

[[nodiscard]] bool IsInteger(const std::string &text) {
	if (text.empty() || text.size() > 18) {
		return false;
	}
	auto start = (text[0] == '-') ? size_t(1) : size_t(0);
	if (start == text.size()) {
		return false;
	}
	for (auto i = start; i != text.size(); ++i) {
		if (text[i] < '0' || text[i] > '9') {
			return false;
		}
	}
	return true;
}

[[nodiscard]] std::optional<cbor::Value> ParseValue(const std::string &text) {
	if (!text.empty() && text[0] == '@') {
		auto bytes = ReadWholeFile(text.substr(1));
		return bytes
			? std::optional<cbor::Value>(cbor::Value::FromBytes(std::move(*bytes)))
			: std::nullopt;
	} else if (text == "true" || text == "false") {
		return cbor::Value::FromBool(text == "true");
	} else if (text == "null") {
		return cbor::Value();
	} else if (IsInteger(text)) {
		return cbor::Value::FromInt(std::stoll(text));
	}
	return cbor::Value::FromText(text);
}

struct Fields {
	cbor::Map map;
	std::vector<std::string> positional;
	std::string error;
};

[[nodiscard]] Fields ParseFields(const std::vector<Token> &tokens, size_t from) {
	auto result = Fields();
	for (auto i = from; i != tokens.size(); ++i) {
		const auto &token = tokens[i];
		const auto equals = token.text.find('=');
		if (token.quoted || equals == std::string::npos || equals == 0) {
			result.positional.push_back(token.text);
			continue;
		}
		auto key = token.text.substr(0, equals);
		const auto rest = token.text.substr(equals + 1);
		auto json = false;
		if (key.back() == ':') {
			key.pop_back();
			json = !rest.empty() && rest[0] == '=';
		}
		auto value = std::optional<cbor::Value>();
		if (json) {
			auto parsed = json::Parse(rest.substr(1));
			if (!parsed.ok()) {
				result.error = key + ": " + parsed.error();
				return result;
			}
			value = std::move(parsed.value());
		} else {
			value = ParseValue(rest);
			if (!value) {
				result.error = "cannot read " + rest.substr(1);
				return result;
			}
		}
		result.map.push_back({ std::move(key), std::move(*value) });
	}
	return result;
}

[[nodiscard]] cbor::Value Event(std::string type, cbor::Map fields) {
	auto map = cbor::Map();
	map.push_back({ "type", cbor::Value::FromText(std::move(type)) });
	for (auto &field : fields) {
		map.push_back(std::move(field));
	}
	return cbor::Value::FromMap(std::move(map));
}

[[nodiscard]] std::optional<int64_t> Number(const std::string &text) {
	return IsInteger(text) ? std::optional<int64_t>(std::stoll(text)) : std::nullopt;
}

[[nodiscard]] std::string Render(const cbor::Value &value) {
	return json::Canonical(value);
}

[[nodiscard]] std::string Joined(const std::vector<std::string> &items, size_t from) {
	auto result = std::string();
	for (auto i = from; i < items.size(); ++i) {
		if (!result.empty()) {
			result += ' ';
		}
		result += items[i];
	}
	return result;
}

const char *kHelp =
	"commands (one per line, `#` starts a comment):\n"
	"  event <type> [key=value ...]          deliver an event (message.new, chat.opened, ...)\n"
	"  msg new <chat> <text> [from=N]        add a message to the chat and deliver message.new\n"
	"  msg add <chat> <text> [from=N]        add a message the plugin can read\n"
	"  chat add <id> <title> [type]          add a chat (user, group, channel, ...)\n"
	"  command <name> [args] [chat=N]        run a /command of the plugin\n"
	"  menu <surface> <id> [key=value ...]   click a menu item\n"
	"  settings open|click|change <id> [value]\n"
	"  dialog click|change|close <id> [value]\n"
	"  window <window> click|change|close <id> [value]\n"
	"  toast click|close <id> [reason]\n"
	"  outgoing <chat> <text>                run the message.outgoing hooks on a text\n"
	"  timer advance <ms>                    move the clock so that timers fire\n"
	"  mock <url-prefix> <status> <body|@file>   answer net.fetch calls\n"
	"  grant|revoke <permission>             change the granted permissions\n"
	"  storage                               print the plugin's storage\n"
	"  info                                  print the plugin's status and permissions\n"
	"  wait <ms>                             let the plugin run\n"
	"  quit\n"
	"values: 12, true, null, text, \"quoted text\", @file (bytes), key:=<json>";

class Session {
public:
	Session(
		const RunOptions &options,
		Console &console,
		DevData &data,
		DevTransport &transport,
		PluginManager &manager,
		std::string id,
		fs::path root,
		std::set<std::string> granted,
		std::atomic<int64_t> &offset)
	: _options(options)
	, _console(console)
	, _data(data)
	, _transport(transport)
	, _manager(manager)
	, _id(std::move(id))
	, _root(std::move(root))
	, _granted(std::move(granted))
	, _offset(offset) {
	}

	enum class Result : uint8_t {
		Ok,
		Failed,
		Quit,
	};

	[[nodiscard]] Result execute(const std::string &line) {
		auto ok = true;
		const auto tokens = Tokenize(line, ok);
		if (!ok) {
			return fail("an unclosed quote");
		} else if (tokens.empty() || tokens[0].text[0] == '#') {
			return Result::Ok;
		}
		const auto &name = tokens[0].text;
		if (name == "quit" || name == "exit") {
			return Result::Quit;
		} else if (name == "help") {
			_console.say(kHelp);
			return Result::Ok;
		} else if (name == "event") {
			return event(tokens);
		} else if (name == "msg") {
			return message(tokens);
		} else if (name == "chat") {
			return chat(tokens);
		} else if (name == "command") {
			return command(tokens);
		} else if (name == "menu") {
			return menu(tokens);
		} else if (name == "settings" || name == "dialog" || name == "window" || name == "toast") {
			return ui(tokens);
		} else if (name == "outgoing") {
			return outgoing(tokens);
		} else if (name == "timer") {
			return timer(tokens);
		} else if (name == "mock") {
			return mock(tokens);
		} else if (name == "grant" || name == "revoke") {
			return grant(tokens);
		} else if (name == "storage") {
			return storage();
		} else if (name == "info") {
			return info();
		} else if (name == "wait") {
			return wait(tokens);
		}
		return fail("unknown command " + name + " (try help)");
	}

private:
	[[nodiscard]] Result fail(const std::string &text) {
		_console.say("[run] error: " + text);
		return Result::Failed;
	}

	[[nodiscard]] Result report(const DispatchReport &report) {
		_console.say("[run] queued=" + std::to_string(report.queued)
			+ " delivered=" + std::to_string(report.delivered)
			+ " skipped=" + std::to_string(report.skipped)
			+ " failed=" + std::to_string(report.failed));
		if (report.queued == 0 && report.delivered == 0) {
			_console.say("[run] no plugin took it: check contributes.events and the permissions");
		}
		return Result::Ok;
	}

	[[nodiscard]] Result event(const std::vector<Token> &tokens) {
		if (tokens.size() < 2) {
			return fail("usage: event <type> [key=value ...]");
		}
		auto fields = ParseFields(tokens, 2);
		if (!fields.error.empty()) {
			return fail(fields.error);
		}
		return report(_manager.dispatchAndWait(
			_options.account,
			Event(tokens[1].text, std::move(fields.map)),
			kDrainMs));
	}

	[[nodiscard]] Result message(const std::vector<Token> &tokens) {
		if (tokens.size() < 4 || (tokens[1].text != "new" && tokens[1].text != "add")) {
			return fail("usage: msg new|add <chat> <text> [from=N]");
		}
		const auto chatId = Number(tokens[2].text);
		const auto info = chatId ? _data.chat(*chatId) : std::nullopt;
		if (!info) {
			return fail("no such chat, add it with `chat add`");
		}
		auto fields = ParseFields(tokens, 4);
		if (!fields.error.empty()) {
			return fail(fields.error);
		}
		auto from = (info->type == "user") ? info->id : int64_t(100);
		auto out = false;
		for (const auto &field : fields.map) {
			if (field.key == "from" && field.value.asInt()) {
				from = *field.value.asInt();
			} else if (field.key == "out") {
				out = true;
			}
		}
		const auto &text = tokens[3].text;
		const auto id = _data.addMessage(info->id, text, from, out);
		if (tokens[1].text == "add") {
			_console.say("[run] message " + std::to_string(id));
			return Result::Ok;
		}
		return report(_manager.dispatchAndWait(_options.account, Event("message.new", {
			{ "id", cbor::Value::FromInt(id) },
			{ "chat", cbor::Value::FromInt(info->id) },
			{ "chat_title", cbor::Value::FromText(info->title) },
			{ "chat_type", cbor::Value::FromText(info->type) },
			{ "from", cbor::Value::FromInt(from) },
			{ "from_kind", cbor::Value::FromText(out ? "self" : "user") },
			{ "sender_name", cbor::Value::FromText("User " + std::to_string(from)) },
			{ "date", cbor::Value::FromInt(std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch()).count()) },
			{ "text", cbor::Value::FromText(text) },
			{ "out", cbor::Value::FromBool(out) },
			{ "forwarded", cbor::Value::FromBool(false) },
		}), kDrainMs));
	}

	[[nodiscard]] Result chat(const std::vector<Token> &tokens) {
		const auto id = (tokens.size() >= 4 && tokens[1].text == "add")
			? Number(tokens[2].text)
			: std::nullopt;
		if (!id) {
			return fail("usage: chat add <id> <title> [type]");
		}
		_data.addChat({
			.id = *id,
			.title = tokens[3].text,
			.type = (tokens.size() > 4) ? tokens[4].text : std::string("user"),
		});
		return Result::Ok;
	}

	[[nodiscard]] Result command(const std::vector<Token> &tokens) {
		if (tokens.size() < 2) {
			return fail("usage: command <name> [args] [chat=N]");
		}
		auto fields = ParseFields(tokens, 2);
		if (!fields.error.empty()) {
			return fail(fields.error);
		}
		auto map = cbor::Map();
		map.push_back({ "name", cbor::Value::FromText(tokens[1].text) });
		map.push_back({ "args", cbor::Value::FromText(Joined(fields.positional, 0)) });
		for (auto &field : fields.map) {
			map.push_back(std::move(field));
		}
		return send(Event("command", std::move(map)));
	}

	[[nodiscard]] Result menu(const std::vector<Token> &tokens) {
		if (tokens.size() < 3) {
			return fail("usage: menu <surface> <id> [key=value ...]");
		}
		auto fields = ParseFields(tokens, 3);
		if (!fields.error.empty()) {
			return fail(fields.error);
		}
		auto map = cbor::Map();
		map.push_back({ "surface", cbor::Value::FromText(tokens[1].text) });
		map.push_back({ "id", cbor::Value::FromText(tokens[2].text) });
		for (auto &field : fields.map) {
			map.push_back(std::move(field));
		}
		return send(Event("menu.click", std::move(map)));
	}

	[[nodiscard]] Result ui(const std::vector<Token> &tokens) {
		const auto &group = tokens[0].text;
		auto window = std::string();
		auto at = size_t(1);
		if (group == "window") {
			if (tokens.size() < 3) {
				return fail("usage: window <window> click|change|close <id> [value]");
			}
			window = tokens[1].text;
			at = 2;
		}
		if (tokens.size() <= at) {
			return fail("usage: " + group + " click|change|close <id> [value]");
		}
		const auto &action = tokens[at].text;
		auto map = cbor::Map();
		if (!window.empty()) {
			map.push_back({ "window", cbor::Value::FromText(window) });
		}
		auto rest = at + 1;
		if (action != "open" && action != "close" && tokens.size() > rest) {
			map.push_back({ "id", cbor::Value::FromText(tokens[rest].text) });
			++rest;
		}
		if (tokens.size() > rest) {
			const auto value = ParseValue(tokens[rest].text);
			if (!value) {
				return fail("cannot read " + tokens[rest].text);
			}
			map.push_back({ (group == "toast") ? "reason" : "value", *value });
		}
		return send(Event(group + "." + action, std::move(map)));
	}

	[[nodiscard]] Result send(const cbor::Value &event) {
		if (!_manager.send(_id, _options.account, event)) {
			return fail("the plugin does not accept this event (check its manifest and permissions)");
		}
		settle();
		return Result::Ok;
	}

	[[nodiscard]] Result outgoing(const std::vector<Token> &tokens) {
		const auto chatId = (tokens.size() >= 3) ? Number(tokens[1].text) : std::nullopt;
		if (!chatId) {
			return fail("usage: outgoing <chat> <text>");
		}
		const auto result = _manager.transformOutgoing(
			_options.account,
			*chatId,
			false,
			tokens[2].text,
			kDrainMs);
		if (result.blocked) {
			_console.say("[run] outgoing: blocked");
		} else if (result.dropped) {
			_console.say("[run] outgoing: dropped by " + result.plugin
				+ (result.reason.empty() ? "" : ": " + result.reason));
		} else {
			_console.say("[run] outgoing: " + json::Canonical(cbor::Value::FromText(result.text)));
		}
		return Result::Ok;
	}

	[[nodiscard]] Result timer(const std::vector<Token> &tokens) {
		const auto ms = (tokens.size() == 3 && tokens[1].text == "advance")
			? Number(tokens[2].text)
			: std::nullopt;
		if (!ms || *ms < 0) {
			return fail("usage: timer advance <ms>");
		}
		_offset += *ms;
		settle();
		return Result::Ok;
	}

	[[nodiscard]] Result mock(const std::vector<Token> &tokens) {
		const auto status = (tokens.size() == 4) ? Number(tokens[2].text) : std::nullopt;
		if (!status) {
			return fail("usage: mock <url-prefix> <status> <body|@file>");
		}
		auto body = cbor::Bytes(tokens[3].text.begin(), tokens[3].text.end());
		if (tokens[3].text[0] == '@' && !tokens[3].quoted) {
			auto bytes = ReadWholeFile(tokens[3].text.substr(1));
			if (!bytes) {
				return fail("cannot read " + tokens[3].text.substr(1));
			}
			body = std::move(*bytes);
		}
		_transport.mock(tokens[1].text, int(*status), std::move(body));
		return Result::Ok;
	}

	[[nodiscard]] Result grant(const std::vector<Token> &tokens) {
		if (tokens.size() != 2) {
			return fail("usage: grant|revoke <permission>");
		}
		auto found = false;
		for (const auto &info : _manager.list()) {
			for (const auto &request : info.requests) {
				found = found || (info.id == _id && request.id == tokens[1].text);
			}
		}
		if (!found) {
			return fail("the manifest does not request " + tokens[1].text);
		}
		if (tokens[0].text == "grant") {
			_granted.insert(tokens[1].text);
		} else {
			_granted.erase(tokens[1].text);
		}
		if (!_manager.grant(_id, _options.account, { _granted, std::nullopt, _options.folder })) {
			return fail("the grant was refused");
		}
		settle();
		return Result::Ok;
	}

	[[nodiscard]] Result storage() {
		settle();
		auto store = FileStorage(_root / _id / "data" / "store.cbor");
		const auto keys = store.keys("", 1000);
		for (const auto &key : keys) {
			const auto bytes = store.get(key);
			auto text = std::string("?");
			if (bytes) {
				const auto decoded = cbor::Decode(*bytes);
				text = decoded.ok() ? Render(decoded.value()) : std::to_string(bytes->size()) + " bytes";
			}
			_console.say("[run] " + key + " = " + text);
		}
		if (keys.empty()) {
			_console.say("[run] the storage is empty");
		}
		return Result::Ok;
	}

	[[nodiscard]] Result info() {
		for (const auto &info : _manager.list()) {
			_console.say("[run] " + info.id + " " + info.version + " enabled=" + (info.enabled ? "yes" : "no")
				+ " events=" + std::to_string(info.stats.events)
				+ " failures=" + std::to_string(info.stats.failures)
				+ (info.lastFailure.empty() ? "" : " last failure: " + info.lastFailure));
			for (const auto &request : info.requests) {
				_console.say(std::string("[run]   ") + (_granted.contains(request.id) ? "granted " : "not granted ")
					+ request.id + (request.required ? " (required)" : ""));
			}
		}
		return Result::Ok;
	}

	[[nodiscard]] Result wait(const std::vector<Token> &tokens) {
		const auto ms = (tokens.size() == 2) ? Number(tokens[1].text) : std::nullopt;
		if (!ms || *ms < 0 || *ms > 600000) {
			return fail("usage: wait <ms>");
		}
		const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(*ms);
		while (std::chrono::steady_clock::now() < until) {
			_manager.pump();
			std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
		}
		settle();
		return Result::Ok;
	}

	void settle() {
		if (!_manager.drain(kDrainMs)) {
			_console.say("[run] the plugin is still busy after 10 seconds");
		}
	}

	const RunOptions &_options;
	Console &_console;
	DevData &_data;
	DevTransport &_transport;
	PluginManager &_manager;
	std::string _id;
	fs::path _root;
	std::set<std::string> _granted;
	std::atomic<int64_t> &_offset;

};

[[nodiscard]] std::string ExecutableDirectory(const RunOptions &options) {
	return options.hostDirectory.empty() ? std::string(".") : options.hostDirectory;
}

[[nodiscard]] fs::path HostPath(const RunOptions &options) {
	if (!options.host.empty()) {
		return options.host;
	} else if (const auto env = std::getenv("FGPL_HOST"); env && *env) {
		return env;
	}
#ifdef _WIN32
	const auto suffix = std::string(".exe");
#else
	const auto suffix = std::string();
#endif
	return fs::path(ExecutableDirectory(options)) / (
		(options.noSandbox ? "fgpl_plugin_host_unsandboxed" : "fgpl_plugin_host") + suffix);
}

[[nodiscard]] PythonRuntime PythonPaths(const RunOptions &options) {
	auto result = PythonRuntime();
	auto runtime = options.runtime;
	if (runtime.empty()) {
		if (const auto env = std::getenv("FGPL_RUNTIME_DIR"); env && *env) {
			runtime = env;
		}
	}
	if (!runtime.empty()) {
		result.wasm = fs::path(runtime) / "python" / "python.wasm";
		result.library = fs::path(runtime) / "python" / "lib";
		result.sdk = fs::path(runtime) / "sdk";
	} else {
#if defined(FGPL_DEV_CPYTHON_DIR) && defined(FGPL_DEV_SDK_DIR)
		result.wasm = fs::path(FGPL_DEV_CPYTHON_DIR) / "python.wasm";
		result.library = fs::path(FGPL_DEV_CPYTHON_DIR) / "lib";
		result.sdk = FGPL_DEV_SDK_DIR;
#endif
	}
	return result;
}

[[nodiscard]] int ProcessId() {
#ifdef _WIN32
	return _getpid();
#else
	return int(::getpid());
#endif
}

} // namespace

int Run(const RunOptions &options) {
	auto console = Console();
	const auto say = [&](const std::string &line) {
		console.say(line);
	};

	auto archive = cbor::Bytes();
	if (fs::is_directory(options.target)) {
		auto read = ReadDirectoryPackage(options.target);
		if (!read.ok()) {
			std::fprintf(stderr, "%s\n", read.error().c_str());
			return 1;
		}
		archive = WriteArchive(read.value().document, std::move(read.value().files));
	} else if (auto bytes = ReadWholeFile(options.target)) {
		archive = std::move(*bytes);
	} else {
		std::fprintf(stderr, "cannot read %s\n", options.target.c_str());
		return 1;
	}
	const auto opened = OpenPackage(archive);
	if (!opened.ok()) {
		std::fprintf(stderr, "the package is not valid:\n");
		for (const auto &line : opened.error().details) {
			std::fprintf(stderr, "  %s\n", line.c_str());
		}
		return 2;
	}
	const auto &manifest = opened.value().manifest;
	const auto version = ParseVersion(options.app);
	if (!version) {
		std::fprintf(stderr, "--app: not a version: %s\n", options.app.c_str());
		return 1;
	}

	auto config = ManagerConfig();
	const auto temporary = options.state.empty();
	const auto root = temporary
		? fs::temp_directory_path() / ("fgpl-run-" + std::to_string(ProcessId()))
		: fs::path(options.state);
	auto error = std::error_code();
	fs::create_directories(root, error);
	if (error) {
		std::fprintf(stderr, "cannot create %s\n", root.string().c_str());
		return 1;
	}
	const auto cleanup = [&] {
		if (temporary) {
			auto ignored = std::error_code();
			fs::remove_all(root, ignored);
		}
	};
	const auto pluginsRoot = root / "plugins";
	config.root = pluginsRoot;
	config.host.path = HostPath(options).string();
	config.host.unsandboxedTestHost = options.noSandbox;
	config.host.addressSpaceMb = 1024;
	config.python = PythonPaths(options);
	config.cache.directory = root / "cache";
	config.cache.secret.fill(0x44);
	config.storageFlushMs = 0;
	config.environment.app = *version;
	config.environment.apiLevel = int(api::kVersion);
	if (!fs::exists(config.host.path)) {
		std::fprintf(stderr, "no plugin host at %s (use --host or --no-sandbox)\n", config.host.path.c_str());
		cleanup();
		return 1;
	} else if (manifest.kind == "python" && !fs::exists(config.python.wasm)) {
		std::fprintf(stderr,
			"no Python runtime: pass --runtime <dir with python/ and sdk/> or set FGPL_RUNTIME_DIR\n");
		cleanup();
		return 1;
	}

	auto failures = std::atomic<int>(0);
	auto data = DevData(say);
	auto transport = DevTransport(say);
	data.addChat({ .id = 1, .title = "Alice", .type = "user" });
	data.addChat({ .id = 2, .title = "Group", .type = "group" });
	data.addChat({ .id = 3, .title = "News", .type = "channel" });
	auto offset = std::atomic<int64_t>(0);
	const auto started = std::chrono::steady_clock::now();
	config.services.data = &data;
	config.services.transport = &transport;
	config.services.clockMs = [&] {
		return int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - started).count()) + offset.load() + 1000;
	};
	config.services.logSink = [&](const std::string &plugin, const LogLine &line) {
		say((line.fromApp ? "[host] " : "[" + plugin + "] ") + line.text);
		if (line.fromApp && line.text.rfind("failure", 0) == 0) {
			++failures;
		}
	};
	config.services.settingsPage = [&](const std::string &, const std::string &, const cbor::Value &page) {
		say("[app] settings page " + Render(page));
	};
	config.services.openSettings = [&](const std::string &, const std::string &) {
		say("[app] open settings");
	};
	config.services.dialog = [&](const std::string &, const std::string &, const cbor::Value *page) {
		say(page ? "[app] dialog " + Render(*page) : std::string("[app] dialog closed"));
	};
	config.services.window = [&](
			const std::string &,
			const std::string &,
			const std::string &window,
			const std::string &mode,
			const cbor::Value *page) {
		say(page
			? "[app] window " + window + " (" + mode + ") " + Render(*page)
			: "[app] window " + window + " closed");
	};
	config.services.badges = [&](
			const std::string &,
			const std::string &,
			int64_t user,
			const cbor::Value *items) {
		say("[app] badges user=" + std::to_string(user) + " "
			+ (items ? Render(*items) : std::string("cleared")));
	};

	auto manager = PluginManager(std::move(config));
	if (!temporary) {
		manager.uninstall(manifest.id, false);
	}
	InstallOptions install;
	install.acceptUnsigned = true;
	install.acceptNewPublisher = true;
	install.allowDowngrade = true;
	const auto installed = manager.install(archive, install);
	if (!installed.ok()) {
		std::fprintf(stderr, "cannot install:\n");
		for (const auto &line : installed.error().details) {
			std::fprintf(stderr, "  %s\n", line.c_str());
		}
		manager.shutdown();
		cleanup();
		return 2;
	}
	const auto id = installed.value().id;

	auto granted = std::set<std::string>();
	for (const auto &info : manager.list()) {
		if (info.id != id) {
			continue;
		}
		for (const auto &request : info.requests) {
			if ((options.grant && !options.grant->contains(request.id))
				|| options.deny.contains(request.id)) {
				continue;
			} else if (request.id == "fs.folder" && options.folder.empty()) {
				say("[run] fs.folder is not granted: pass --folder <directory>");
				continue;
			}
			granted.insert(request.id);
		}
	}
	say("[run] " + id + " " + installed.value().version + " ("
		+ manifest.kind + "), host " + (options.noSandbox ? "without a sandbox" : "sandboxed")
		+ ", account " + options.account);
	say("[run] granted: " + (granted.empty() ? std::string("nothing") : Joined(
		std::vector<std::string>(granted.begin(), granted.end()), 0)));
	if (!manager.setEnabled(id, true)
		|| !manager.grant(id, options.account, { granted, std::nullopt, options.folder })) {
		std::fprintf(stderr, "cannot enable the plugin\n");
		manager.shutdown();
		cleanup();
		return 2;
	}
	say("[run] type `help` for the commands");
	manager.startEnabled(options.account);
	if (!manager.drain(kDrainMs)) {
		say("[run] the plugin is still starting after 10 seconds");
	}

	auto lines = std::make_shared<Lines>();
	auto input = std::shared_ptr<std::istream>();
	if (options.script.empty()) {
		input = std::shared_ptr<std::istream>(&std::cin, [](std::istream *) {});
	} else {
		input = std::make_shared<std::ifstream>(options.script);
		if (!*input) {
			std::fprintf(stderr, "cannot read %s\n", options.script.c_str());
			manager.shutdown();
			cleanup();
			return 1;
		}
	}
	std::thread([lines, input] {
		auto line = std::string();
		while (std::getline(*input, line)) {
			const auto lock = std::scoped_lock(lines->mutex);
			lines->queue.push_back(line);
			lines->wake.notify_all();
		}
		const auto lock = std::scoped_lock(lines->mutex);
		lines->eof = true;
		lines->wake.notify_all();
	}).detach();

	auto session = Session(
		options,
		console,
		data,
		transport,
		manager,
		id,
		pluginsRoot,
		granted,
		offset);
	auto errors = 0;
	while (true) {
		manager.pump();
		auto line = std::optional<std::string>();
		auto finished = false;
		{
			auto lock = std::unique_lock(lines->mutex);
			lines->wake.wait_for(lock, std::chrono::milliseconds(kTickMs), [&] {
				return !lines->queue.empty() || lines->eof;
			});
			if (!lines->queue.empty()) {
				line = std::move(lines->queue.front());
				lines->queue.pop_front();
			} else {
				finished = lines->eof;
			}
		}
		if (line) {
			const auto result = session.execute(*line);
			if (result == Session::Result::Quit) {
				break;
			} else if (result == Session::Result::Failed) {
				++errors;
			}
		} else if (finished) {
			break;
		}
	}
	if (!manager.drain(kDrainMs)) {
		say("[run] the plugin is still busy after 10 seconds");
	}
	manager.shutdown();
	cleanup();
	if (errors || failures.load()) {
		say("[run] finished with " + std::to_string(errors) + " command errors and "
			+ std::to_string(failures.load()) + " plugin failures");
		return 3;
	}
	return 0;
}

} // namespace fgpl::dev
