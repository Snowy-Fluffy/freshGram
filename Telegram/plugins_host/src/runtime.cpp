#include "runtime.h"

#include "guest.h"
#include "wasi_host.h"

#include <wasmtime.h>

#include <chrono>
#include <deque>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fgpl {

struct RuntimeState {
	explicit RuntimeState(wasmtime::Config config)
	: engine(std::move(config))
	, ticker([this] { tick(); }) {
	}

	~RuntimeState() {
		{
			const auto lock = std::scoped_lock(mutex);
			stop = true;
		}
		wake.notify_all();
		ticker.join();
	}

	void tick() {
		auto lock = std::unique_lock(mutex);
		while (!stop) {
			wake.wait_for(
				lock,
				std::chrono::milliseconds(10),
				[&] { return stop; });
			engine.increment_epoch();
		}
	}

	wasmtime::Engine engine;
	std::mutex mutex;
	std::condition_variable wake;
	bool stop = false;
	std::thread ticker;

};

namespace {

constexpr auto kTickMs = uint32_t(10);
constexpr auto kHostModule = std::string_view("freshgram");
constexpr auto kLogLineLimit = size_t(4096);

using AllocFn = wasmtime::TypedFunc<std::tuple<int32_t>, int32_t>;
using EntryFn = wasmtime::TypedFunc<std::tuple<int32_t, int32_t>, int32_t>;
using ReleaseFn = wasmtime::TypedFunc<
	std::tuple<int32_t, int32_t>,
	std::tuple<>>;
using StartFn = wasmtime::TypedFunc<std::tuple<>, std::tuple<>>;

} // namespace

struct PluginState {
	PluginState(
		std::shared_ptr<RuntimeState> runtime,
		Bridge &bridge,
		const RuntimeLimits &limits)
	: runtime(std::move(runtime))
	, bridge(&bridge)
	, limits(limits)
	, store(this->runtime->engine) {
	}

	std::shared_ptr<RuntimeState> runtime;
	Bridge *bridge = nullptr;
	RuntimeLimits limits;
	wasmtime::Store store;
	std::optional<wasmtime::Memory> memory;
	std::optional<AllocFn> alloc;
	std::optional<EntryFn> onInit;
	std::optional<EntryFn> onEvent;
	std::optional<ReleaseFn> release;
	std::optional<StartFn> start;
	std::unique_ptr<Wasi> wasi;
	Dispatcher *dispatcher = nullptr;
	std::vector<uint8_t> frameBuffer;
	std::deque<std::vector<uint8_t>> replies;
	int64_t currentId = -1;
	std::string logLines[3];
	std::string lastError;
	size_t callsLeft = 0;
	bool dead = false;

};

namespace {

using namespace guest;

[[nodiscard]] std::optional<AllocFn> AllocOf(wasmtime::Caller &caller) {
	const auto found = caller.get_export("fg_alloc");
	if (!found) {
		return std::nullopt;
	}
	const auto function = std::get_if<wasmtime::Func>(&*found);
	if (!function) {
		return std::nullopt;
	}
	auto typed = function->typed<std::tuple<int32_t>, int32_t>(
		caller.context());
	if (!typed) {
		return std::nullopt;
	}
	return typed.unwrap();
}

void SetDeadline(PluginState &state) {
	const auto ticks = state.limits.callTimeoutMs / kTickMs + 1;
	state.store.context().set_epoch_deadline(ticks);
}

void FlushLog(PluginState &state, size_t index) {
	auto &line = state.logLines[index];
	if (!line.empty()) {
		state.bridge->log(line);
		line.clear();
	}
}

void FlushLogs(PluginState &state) {
	FlushLog(state, 1);
	FlushLog(state, 2);
}

void AppendLog(
		PluginState &state,
		size_t index,
		const uint8_t *data,
		size_t size) {
	auto &line = state.logLines[index];
	for (auto i = size_t(0); i != size; ++i) {
		if (data[i] == '\n') {
			state.bridge->log(line);
			line.clear();
		} else {
			line.push_back(char(data[i]));
			if (line.size() >= kLogLineLimit) {
				FlushLog(state, index);
			}
		}
	}
}

[[nodiscard]] CallError Fail(
		PluginState &state,
		const wasmtime::TrapError &error) {
	state.dead = true;
	state.lastError = error.message();
	if (const auto trap = std::get_if<wasmtime::Trap>(&error.data)) {
		if (trap->code() == WASMTIME_TRAP_CODE_INTERRUPT) {
			return CallError::Timeout;
		}
	}
	return CallError::Trap;
}

[[nodiscard]] wasmtime::Trap ToTrap(wasmtime::TrapError &&error) {
	if (auto trap = std::get_if<wasmtime::Trap>(&error.data)) {
		return std::move(*trap);
	}
	return wasmtime::Trap(error.message());
}

struct Placed {
	uint32_t pointer = 0;
	std::optional<wasmtime::TrapError> trap;
	bool outOfMemory = false;
	bool badPointer = false;
};

[[nodiscard]] Placed Place(
		wasmtime::Store::Context context,
		wasmtime::Memory memory,
		const AllocFn &alloc,
		std::span<const uint8_t> bytes) {
	auto result = Placed();
	auto called = alloc.call(
		context,
		std::make_tuple(int32_t(bytes.size())));
	if (!called) {
		result.trap = std::move(called.err());
		return result;
	}
	result.pointer = uint32_t(called.unwrap());
	if (result.pointer == 0) {
		result.outOfMemory = true;
		return result;
	}
	const auto data = memory.data(context);
	if (!InRange(data, result.pointer, bytes.size())) {
		result.badPointer = true;
		return result;
	}
	std::memcpy(data.data() + result.pointer, bytes.data(), bytes.size());
	return result;
}

[[nodiscard]] wasmtime::Result<int32_t, wasmtime::Trap> HostCall(
		PluginState &state,
		wasmtime::Caller &caller,
		int32_t requestPointer,
		int32_t requestLength,
		int32_t outPointer,
		int32_t outLength) {
	const auto memory = MemoryOf(caller);
	const auto alloc = AllocOf(caller);
	if (!memory || !alloc) {
		return guest_status::kBadArguments;
	} else if (state.callsLeft == 0) {
		return guest_status::kTooManyCalls;
	}
	--state.callsLeft;
	const auto context = caller.context();
	const auto request = uint32_t(requestPointer);
	const auto length = uint32_t(requestLength);
	const auto slotPointer = uint32_t(outPointer);
	const auto slotLength = uint32_t(outLength);
	auto data = memory->data(context);
	if (requestLength < 0
		|| length > state.limits.maxMessageBytes
		|| !InRange(data, request, length)
		|| !InRange(data, slotPointer, 4)
		|| !InRange(data, slotLength, 4)) {
		return guest_status::kBadArguments;
	}
	const auto decoded = cbor::Decode(
		std::span<const uint8_t>(data.data() + request, length),
		state.limits.cbor);
	if (!decoded.ok()) {
		return guest_status::kBadRequest;
	}
	const auto response = state.bridge->call(decoded.value());
	auto bytes = cbor::Bytes();
	if (!cbor::Encode(response, bytes, state.limits.cbor.maxDepth)
		|| bytes.size() > state.limits.maxMessageBytes) {
		return guest_status::kBadResponse;
	}
	auto placed = Place(context, *memory, *alloc, bytes);
	if (placed.trap) {
		return ToTrap(std::move(*placed.trap));
	} else if (placed.outOfMemory) {
		return guest_status::kNoMemory;
	} else if (placed.badPointer) {
		return wasmtime::Trap("fg_alloc returned an invalid pointer");
	}
	data = memory->data(context);
	PutU32(data, slotPointer, placed.pointer);
	PutU32(data, slotLength, uint32_t(bytes.size()));
	return guest_status::kOk;
}

void LinkHost(
		wasmtime::Linker &linker,
		PluginState &state,
		std::unordered_set<std::string> &defined) {
	Define(
		linker,
		defined,
		kHostModule,
		"call",
		[&state](wasmtime::Caller caller, int32_t a, int32_t b, int32_t c, int32_t d)
		-> wasmtime::Result<int32_t, wasmtime::Trap> {
			return HostCall(state, caller, a, b, c, d);
		});
	state.wasi->link(linker, defined);
}

[[nodiscard]] Result<int32_t, CallError> Invoke(
		PluginState &state,
		const EntryFn &entry,
		const cbor::Value &message) {
	if (state.dead || state.start) {
		return CallError::Dead;
	}
	auto bytes = cbor::Bytes();
	if (!cbor::Encode(message, bytes, state.limits.cbor.maxDepth)
		|| bytes.size() > state.limits.maxMessageBytes) {
		return CallError::BadMessage;
	}
	const auto context = state.store.context();
	state.callsLeft = state.limits.maxCallsPerDispatch;
	SetDeadline(state);
	const auto placed = Place(context, *state.memory, *state.alloc, bytes);
	if (placed.trap) {
		return Fail(state, *placed.trap);
	} else if (placed.badPointer) {
		state.dead = true;
		state.lastError = "fg_alloc returned an invalid pointer";
		return CallError::GuestFailed;
	} else if (placed.outOfMemory) {
		return CallError::GuestFailed;
	}
	SetDeadline(state);
	const auto arguments = std::make_tuple(
		int32_t(placed.pointer),
		int32_t(bytes.size()));
	auto status = entry.call(context, arguments);
	if (!status) {
		FlushLogs(state);
		return Fail(state, status.err());
	}
	if (state.release) {
		SetDeadline(state);
		auto released = state.release->call(context, arguments);
		if (!released) {
			FlushLogs(state);
			return Fail(state, released.err());
		}
	}
	FlushLogs(state);
	return status.unwrap();
}

[[nodiscard]] std::vector<uint8_t> Framed(const cbor::Value &message) {
	auto body = cbor::Bytes();
	if (!cbor::Encode(message, body, 24)) {
		return {};
	}
	const auto size = uint32_t(body.size());
	auto result = std::vector<uint8_t>{
		uint8_t(size >> 24),
		uint8_t(size >> 16),
		uint8_t(size >> 8),
		uint8_t(size),
	};
	result.insert(result.end(), body.begin(), body.end());
	return result;
}

[[nodiscard]] cbor::Value Pair(std::string key, cbor::Value value) {
	return cbor::Value::FromMap({ { std::move(key), std::move(value) } });
}

[[nodiscard]] int32_t CommandOutput(
		PluginState &state,
		int fd,
		std::span<const uint8_t> bytes) {
	if (fd != 1 || !state.dispatcher) {
		AppendLog(state, size_t(fd), bytes.data(), bytes.size());
		return 0;
	}
	constexpr auto kIo = int32_t(29);
	auto &buffer = state.frameBuffer;
	buffer.insert(buffer.end(), bytes.begin(), bytes.end());
	while (buffer.size() >= 4) {
		const auto size = (size_t(buffer[0]) << 24)
			| (size_t(buffer[1]) << 16)
			| (size_t(buffer[2]) << 8)
			| size_t(buffer[3]);
		if (size > state.limits.maxMessageBytes) {
			state.dead = true;
			state.lastError = "a protocol frame is too large";
			return kIo;
		} else if (buffer.size() < 4 + size) {
			break;
		}
		const auto decoded = cbor::Decode(
			std::span<const uint8_t>(buffer.data() + 4, size),
			state.limits.cbor);
		buffer.erase(buffer.begin(), buffer.begin() + 4 + ptrdiff_t(size));
		if (!decoded.ok()) {
			state.dead = true;
			state.lastError = "a malformed protocol frame";
			return kIo;
		}
		const auto &message = decoded.value();
		const auto type = message.find("t");
		const auto name = (type && type->asText()) ? *type->asText() : std::string();
		if (name == "call") {
			const auto request = message.find("v");
			auto reply = cbor::Value();
			if (state.callsLeft == 0) {
				reply = Pair("error", cbor::Value::FromText("too_many_calls"));
			} else {
				--state.callsLeft;
				reply = state.bridge->call(request ? *request : cbor::Value());
			}
			auto frame = Framed(cbor::Value::FromMap({
				{ "t", cbor::Value::FromText("reply") },
				{ "v", std::move(reply) },
			}));
			if (frame.empty() || frame.size() > state.limits.maxMessageBytes + 64) {
				state.dead = true;
				state.lastError = "a reply does not fit the limits";
				return kIo;
			}
			state.replies.push_back(std::move(frame));
		} else if (name == "done") {
			const auto status = message.find("status");
			const auto number = status ? status->asInt() : std::nullopt;
			state.dispatcher->finished(
				state.currentId,
				int32_t(number.value_or(0)));
			state.currentId = -1;
		} else {
			state.dead = true;
			state.lastError = "an unknown protocol frame: " + name;
			return kIo;
		}
	}
	return 0;
}

[[nodiscard]] std::optional<std::vector<uint8_t>> CommandInput(
		PluginState &state) {
	if (!state.replies.empty()) {
		auto frame = std::move(state.replies.front());
		state.replies.pop_front();
		return frame;
	} else if (!state.dispatcher || state.dead) {
		return std::nullopt;
	}
	if (state.currentId >= 0) {
		state.dispatcher->finished(state.currentId, 0);
		state.currentId = -1;
	}
	auto item = state.dispatcher->next();
	if (item.kind == Dispatch::Kind::Stop) {
		return std::nullopt;
	}
	state.currentId = item.id;
	state.callsLeft = state.limits.maxCallsPerDispatch;
	auto frame = Framed(cbor::Value::FromMap({
		{ "t", cbor::Value::FromText(
			item.kind == Dispatch::Kind::Init ? "init" : "event") },
		{ "id", cbor::Value::FromInt(item.id) },
		{ "v", std::move(item.value) },
	}));
	if (frame.empty()) {
		state.dead = true;
		state.lastError = "a message does not fit the protocol";
		return std::nullopt;
	}
	SetDeadline(state);
	return frame;
}

[[nodiscard]] wasmtime::Config MakeConfig(const RuntimeOptions &options) {
	auto config = wasmtime::Config();
	config.epoch_interruption(true);
	config.consume_fuel(false);
	config.max_wasm_stack(options.stackBytes);
	config.wasm_threads(false);
	config.shared_memory(false);
	config.wasm_tail_call(false);
	config.wasm_simd(false);
	config.wasm_relaxed_simd(false);
	config.wasm_multi_memory(false);
	config.wasm_memory64(false);
	config.wasm_gc(false);
	config.wasm_function_references(false);
	config.wasm_wide_arithmetic(false);
	config.wasm_exceptions(false);
	config.wasm_custom_page_sizes(false);
	config.wasm_component_model(false);
	config.wasm_bulk_memory(true);
	config.wasm_multi_value(true);
	config.wasm_reference_types(true);
	config.parallel_compilation(false);
	config.debug_info(false);
	config.memory_init_cow(false);
	if (options.baselineCompiler) {
		config.strategy(wasmtime::Strategy::Winch);
	}
	if (options.memoryReservation != 0) {
		config.memory_reservation(options.memoryReservation);
		config.memory_guard_size(options.memoryGuard);
		config.memory_reservation_for_growth(0);
	}
	return config;
}

} // namespace

Plugin::Plugin(std::unique_ptr<PluginState> state)
: _state(std::move(state)) {
}

Plugin::~Plugin() = default;

Result<int32_t, CallError> Plugin::init(const cbor::Value &info) {
	return Invoke(*_state, *_state->onInit, info);
}

Result<int32_t, CallError> Plugin::deliver(const cbor::Value &event) {
	return Invoke(*_state, *_state->onEvent, event);
}

bool Plugin::isCommand() const {
	return _state->start.has_value();
}

Result<int32_t, CallError> Plugin::runCommand(Dispatcher &dispatcher) {
	auto &state = *_state;
	if (state.dead || !state.start) {
		return CallError::Dead;
	}
	state.dispatcher = &dispatcher;
	state.callsLeft = state.limits.maxCallsPerDispatch;
	SetDeadline(state);
	auto result = state.start->call(state.store.context(), std::tuple<>());
	FlushLogs(state);
	state.dispatcher = nullptr;
	const auto code = state.wasi->exitCode();
	if (!result) {
		if (code && *code == 0) {
			state.dead = true;
			return 0;
		}
		return Fail(state, result.err());
	}
	state.dead = true;
	return 0;
}

bool Plugin::dead() const {
	return _state->dead;
}

const std::string &Plugin::lastError() const {
	return _state->lastError;
}

uint64_t Plugin::memoryBytes() const {
	return _state->memory->data(_state->store.context()).size();
}

Runtime::Runtime(const RuntimeOptions &options)
: _state(std::make_shared<RuntimeState>(MakeConfig(options))) {
}

Runtime::~Runtime() = default;

const char *Runtime::EngineVersion() {
	return WASMTIME_VERSION;
}

Result<std::unique_ptr<Plugin>, LoadFailure> Runtime::load(
		std::span<const uint8_t> wasm,
		Bridge &bridge,
		const RuntimeLimits &limits,
		std::shared_ptr<const Vfs> files,
		const ProcessSpec &process) {
	return loadFrom(wasm, false, bridge, limits, std::move(files), process);
}

Result<std::unique_ptr<Plugin>, LoadFailure> Runtime::loadCompiled(
		std::span<const uint8_t> compiled,
		Bridge &bridge,
		const RuntimeLimits &limits,
		std::shared_ptr<const Vfs> files,
		const ProcessSpec &process) {
	return loadFrom(compiled, true, bridge, limits, std::move(files), process);
}

Result<std::vector<uint8_t>, LoadFailure> Runtime::compile(
		std::span<const uint8_t> wasm,
		const RuntimeLimits &limits) {
	if (wasm.size() > limits.maxModuleBytes) {
		return LoadFailure{ LoadError::TooLarge, "the module is too large" };
	}
	auto bytes = std::vector<uint8_t>(wasm.begin(), wasm.end());
	auto module = wasmtime::Module::compile(_state->engine, bytes);
	if (!module) {
		return LoadFailure{ LoadError::InvalidModule, module.err().message() };
	}
	auto serialized = module.unwrap().serialize();
	if (!serialized) {
		return LoadFailure{ LoadError::InvalidModule, serialized.err().message() };
	}
	return serialized.unwrap();
}

Result<std::unique_ptr<Plugin>, LoadFailure> Runtime::loadFrom(
		std::span<const uint8_t> input,
		bool precompiled,
		Bridge &bridge,
		const RuntimeLimits &limits,
		std::shared_ptr<const Vfs> files,
		const ProcessSpec &process) {
	const auto fail = [](LoadError error, std::string detail) {
		return LoadFailure{ error, std::move(detail) };
	};
	const auto allowed = precompiled
		? limits.maxModuleBytes * 4
		: limits.maxModuleBytes;
	if (input.size() > allowed) {
		return fail(LoadError::TooLarge, "the module is too large");
	}
	auto bytes = std::vector<uint8_t>(input.begin(), input.end());
	auto compiled = precompiled
		? wasmtime::Module::deserialize(_state->engine, bytes)
		: wasmtime::Module::compile(_state->engine, bytes);
	if (!compiled) {
		return fail(LoadError::InvalidModule, compiled.err().message());
	}
	const auto module = compiled.unwrap();
	auto state = std::make_unique<PluginState>(_state, bridge, limits);
	state->store.limiter(int64_t(limits.memoryBytes), 10000, 1, 100, 1);
	const auto raw = state.get();
	state->wasi = std::make_unique<Wasi>(WasiConfig{
		.files = std::move(files),
		.mounts = { "plugin", "runtime" },
		.maxIoBytes = limits.maxMessageBytes,
		.args = process.args,
		.env = process.env,
		.output = [raw](int fd, std::span<const uint8_t> bytes) {
			return CommandOutput(*raw, fd, bytes);
		},
		.input = [raw] {
			return CommandInput(*raw);
		},
	});

	auto linker = wasmtime::Linker(_state->engine);
	auto defined = std::unordered_set<std::string>();
	LinkHost(linker, *state, defined);
	for (const auto &import : module.imports()) {
		const auto key = std::string(import.module())
			+ "::"
			+ std::string(import.name());
		if (!defined.contains(key)) {
			return fail(LoadError::ForbiddenImport, key);
		}
	}

	const auto context = state->store.context();
	SetDeadline(*state);
	state->callsLeft = limits.maxCallsPerDispatch;
	auto created = linker.instantiate(context, module);
	FlushLogs(*state);
	if (!created) {
		return fail(LoadError::Instantiate, created.err().message());
	}
	auto instance = created.unwrap();

	const auto function = [&](const char *name) -> std::optional<wasmtime::Func> {
		const auto found = instance.get(context, name);
		if (found) {
			if (const auto value = std::get_if<wasmtime::Func>(&*found)) {
				return *value;
			}
		}
		return std::nullopt;
	};
	const auto memory = instance.get(context, "memory");
	if (!memory) {
		return fail(LoadError::MissingExport, "memory");
	} else if (!std::get_if<wasmtime::Memory>(&*memory)) {
		return fail(LoadError::BadExport, "memory");
	}
	state->memory = std::get<wasmtime::Memory>(*memory);

	const auto alloc = function("fg_alloc");
	const auto init = function("fg_init");
	const auto event = function("fg_on_event");
	if (!event) {
		if (const auto start = function("_start")) {
			auto typed = start->typed<std::tuple<>, std::tuple<>>(context);
			if (!typed) {
				return fail(LoadError::BadExport, "_start");
			}
			state->start = typed.unwrap();
			return std::unique_ptr<Plugin>(new Plugin(std::move(state)));
		}
	}
	if (!alloc) {
		return fail(LoadError::MissingExport, "fg_alloc");
	} else if (!init) {
		return fail(LoadError::MissingExport, "fg_init");
	} else if (!event) {
		return fail(LoadError::MissingExport, "fg_on_event");
	}
	auto typedAlloc = alloc->typed<std::tuple<int32_t>, int32_t>(context);
	auto typedInit = init->typed<std::tuple<int32_t, int32_t>, int32_t>(context);
	auto typedEvent = event->typed<std::tuple<int32_t, int32_t>, int32_t>(context);
	if (!typedAlloc) {
		return fail(LoadError::BadExport, "fg_alloc");
	} else if (!typedInit) {
		return fail(LoadError::BadExport, "fg_init");
	} else if (!typedEvent) {
		return fail(LoadError::BadExport, "fg_on_event");
	}
	state->alloc = typedAlloc.unwrap();
	state->onInit = typedInit.unwrap();
	state->onEvent = typedEvent.unwrap();
	if (const auto release = function("fg_free")) {
		auto typed = release->typed<std::tuple<int32_t, int32_t>, std::tuple<>>(
			context);
		if (!typed) {
			return fail(LoadError::BadExport, "fg_free");
		}
		state->release = typed.unwrap();
	}
	return std::unique_ptr<Plugin>(new Plugin(std::move(state)));
}

} // namespace fgpl
