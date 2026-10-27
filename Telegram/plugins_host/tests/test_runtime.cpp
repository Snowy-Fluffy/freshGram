#include "check.h"
#include "fgpl/runtime.h"
#include "wat_module.h"

#include <wasmtime.hh>

#include <chrono>
#include <string>
#include <vector>

namespace {

using namespace fgpl;
using wat::Module;
using wat::Wasm;

class MockBridge final : public Bridge {
public:
	cbor::Value call(const cbor::Value &request) override {
		requests.push_back(request);
		return reply;
	}
	void log(std::string_view line) override {
		logs.emplace_back(line);
	}

	std::vector<cbor::Value> requests;
	std::vector<std::string> logs;
	cbor::Value reply = cbor::Value::FromMap({
		{ "ok", cbor::Value::FromBool(true) },
	});

};

[[nodiscard]] Result<std::unique_ptr<Plugin>, LoadFailure> Load(
		Runtime &runtime,
		MockBridge &bridge,
		const std::string &wat,
		const RuntimeLimits &limits = {}) {
	return runtime.load(Wasm(wat), bridge, limits);
}

const auto kEvent = [] {
	return cbor::Value::FromMap({ { "type", cbor::Value::FromInt(1) } });
};

void TestRoundTripThroughBridge() {
	auto runtime = Runtime();
	auto bridge = MockBridge();
	auto loaded = Load(runtime, bridge, Module("", R"(
		(drop (call $call (local.get $ptr) (local.get $len)
			(i32.const 16) (i32.const 20)))
		(i32.or
			(i32.shl (i32.load (i32.const 20)) (i32.const 8))
			(i32.load8_u (i32.load (i32.const 16)))))"));
	CHECK(loaded.ok());
	if (!loaded.ok()) {
		return;
	}
	auto &plugin = *loaded.value();
	CHECK(plugin.init(kEvent()).ok());
	const auto status = plugin.deliver(kEvent());
	CHECK(status.ok());
	if (status.ok()) {
		CHECK(status.value() == ((5 << 8) | 0xA1));
	}
	CHECK(bridge.requests.size() == 1);
	CHECK(bridge.requests.size() == 1 && bridge.requests[0] == kEvent());
	CHECK(!plugin.dead());
	CHECK(plugin.deliver(kEvent()).ok());
	CHECK(bridge.requests.size() == 2);
}

void TestTimeout() {
	auto runtime = Runtime();
	auto bridge = MockBridge();
	auto limits = RuntimeLimits();
	limits.callTimeoutMs = 100;
	auto loaded = Load(runtime, bridge, Module("", R"(
		(loop $l (br $l)) (i32.const 0))"), limits);
	CHECK(loaded.ok());
	if (!loaded.ok()) {
		return;
	}
	auto &plugin = *loaded.value();
	const auto start = std::chrono::steady_clock::now();
	const auto result = plugin.deliver(kEvent());
	const auto elapsed = std::chrono::steady_clock::now() - start;
	CHECK(!result.ok() && result.error() == CallError::Timeout);
	CHECK(elapsed < std::chrono::milliseconds(1500));
	CHECK(plugin.dead());
	CHECK(plugin.deliver(kEvent()).error() == CallError::Dead);
}

void TestTraps() {
	auto runtime = Runtime();
	auto bridge = MockBridge();
	auto loaded = Load(runtime, bridge, Module("", "(unreachable)"));
	CHECK(loaded.ok());
	if (loaded.ok()) {
		const auto result = loaded.value()->deliver(kEvent());
		CHECK(!result.ok() && result.error() == CallError::Trap);
		CHECK(loaded.value()->dead());
	}

	loaded = Load(runtime, bridge, Module(
		"(func $r (result i32) (call $r))",
		"(call $r)"));
	CHECK(loaded.ok());
	if (loaded.ok()) {
		const auto result = loaded.value()->deliver(kEvent());
		CHECK(!result.ok() && result.error() == CallError::Trap);
		CHECK(loaded.value()->lastError().find("stack") != std::string::npos);
	}

	loaded = Load(runtime, bridge, Module(
		R"((import "wasi_snapshot_preview1" "proc_exit"
			(func $exit (param i32))))",
		"(call $exit (i32.const 7)) (i32.const 0)"));
	CHECK(loaded.ok());
	if (loaded.ok()) {
		const auto result = loaded.value()->deliver(kEvent());
		CHECK(!result.ok() && result.error() == CallError::Trap);
		CHECK(loaded.value()->lastError().find("proc_exit(7)")
			!= std::string::npos);
	}
}

void TestMemoryLimit() {
	auto runtime = Runtime();
	auto bridge = MockBridge();
	const auto grow = Module("", "(memory.grow (i32.const 2000))");
	auto loaded = Load(runtime, bridge, grow);
	CHECK(loaded.ok());
	if (loaded.ok()) {
		const auto result = loaded.value()->deliver(kEvent());
		CHECK(result.ok() && result.value() == -1);
		CHECK(loaded.value()->memoryBytes() == 65536);
	}

	auto limits = RuntimeLimits();
	limits.memoryBytes = uint64_t(1) << 30;
	loaded = Load(runtime, bridge, grow, limits);
	CHECK(loaded.ok());
	if (loaded.ok()) {
		const auto result = loaded.value()->deliver(kEvent());
		CHECK(result.ok() && result.value() == 1);
		CHECK(loaded.value()->memoryBytes() == 2001 * 65536ULL);
	}

	auto big = Module("", "(i32.const 0)");
	big.replace(big.find("(memory (export \"memory\") 1)"),
		std::string("(memory (export \"memory\") 1)").size(),
		"(memory (export \"memory\") 2000)");
	const auto refused = Load(runtime, bridge, big);
	CHECK(!refused.ok() && refused.error().error == LoadError::Instantiate);
}

void TestStartFunctionIsBounded() {
	auto runtime = Runtime();
	auto bridge = MockBridge();
	auto limits = RuntimeLimits();
	limits.callTimeoutMs = 100;
	const auto start = std::chrono::steady_clock::now();
	const auto loaded = Load(runtime, bridge, Module(
		"(func $s (loop $l (br $l))) (start $s)",
		"(i32.const 0)"), limits);
	CHECK(!loaded.ok() && loaded.error().error == LoadError::Instantiate);
	CHECK(std::chrono::steady_clock::now() - start
		< std::chrono::milliseconds(1500));
}

void TestImportsAreAllowlisted() {
	auto runtime = Runtime();
	auto bridge = MockBridge();
	const auto withImport = [&](const std::string &import) {
		return Load(runtime, bridge, Module(import, "(i32.const 0)"));
	};

	auto result = withImport(
		R"((import "env" "foo" (func)))");
	CHECK(!result.ok() && result.error().error == LoadError::ForbiddenImport);
	CHECK(!result.ok() && result.error().detail == "env::foo");

	result = withImport(
		R"((import "wasi_snapshot_preview1" "sock_connect"
			(func (param i32 i32 i32) (result i32))))");
	CHECK(!result.ok() && result.error().error == LoadError::ForbiddenImport);

	result = withImport(
		R"((import "wasi_unstable" "fd_write"
			(func (param i32 i32 i32 i32) (result i32))))");
	CHECK(!result.ok() && result.error().error == LoadError::ForbiddenImport);

	result = withImport(
		R"((import "env" "memory" (memory 1)))");
	CHECK(!result.ok());

	result = withImport(
		R"((import "freshgram" "other" (func)))");
	CHECK(!result.ok() && result.error().error == LoadError::ForbiddenImport);

	result = withImport(
		R"((import "wasi_snapshot_preview1" "sched_yield"
			(func (result i32))))");
	CHECK(result.ok());

	result = withImport(
		R"((import "wasi_snapshot_preview1" "sched_yield"
			(func (param i32) (result i32))))");
	CHECK(!result.ok() && result.error().error == LoadError::Instantiate);
}

void TestExportsAreChecked() {
	auto runtime = Runtime();
	auto bridge = MockBridge();
	const auto noEvent = Wasm(R"((module
		(memory (export "memory") 1)
		(func (export "fg_alloc") (param i32) (result i32) (i32.const 8))
		(func (export "fg_init") (param i32 i32) (result i32) (i32.const 0))))");
	auto result = runtime.load(noEvent, bridge);
	CHECK(!result.ok() && result.error().error == LoadError::MissingExport);
	CHECK(!result.ok() && result.error().detail == "fg_on_event");

	const auto noMemory = Wasm(R"((module
		(func (export "fg_alloc") (param i32) (result i32) (i32.const 8))
		(func (export "fg_init") (param i32 i32) (result i32) (i32.const 0))
		(func (export "fg_on_event") (param i32 i32) (result i32) (i32.const 0))))");
	result = runtime.load(noMemory, bridge);
	CHECK(!result.ok() && result.error().error == LoadError::MissingExport);

	const auto wrongType = Wasm(R"((module
		(memory (export "memory") 1)
		(func (export "fg_alloc") (result i32) (i32.const 8))
		(func (export "fg_init") (param i32 i32) (result i32) (i32.const 0))
		(func (export "fg_on_event") (param i32 i32) (result i32) (i32.const 0))))");
	result = runtime.load(wrongType, bridge);
	CHECK(!result.ok() && result.error().error == LoadError::BadExport);
	CHECK(!result.ok() && result.error().detail == "fg_alloc");

	const auto garbage = std::vector<uint8_t>{ 1, 2, 3, 4 };
	result = runtime.load(garbage, bridge);
	CHECK(!result.ok() && result.error().error == LoadError::InvalidModule);

	auto limits = RuntimeLimits();
	limits.maxModuleBytes = 8;
	result = runtime.load(noEvent, bridge, limits);
	CHECK(!result.ok() && result.error().error == LoadError::TooLarge);
}

void TestDisabledProposals() {
	auto runtime = Runtime();
	auto bridge = MockBridge();
	const auto simd = Wasm(Module("", R"(
		(drop (v128.const i32x4 0 0 0 0)) (i32.const 0))"));
	CHECK(!runtime.load(simd, bridge).ok());

	const auto atomics = Wasm(Module("", R"(
		(drop (i32.atomic.load (i32.const 0))) (i32.const 0))"));
	CHECK(!runtime.load(atomics, bridge).ok());

	const auto shared = Wasm(R"((module
		(memory (export "memory") 1 2 shared)
		(func (export "fg_alloc") (param i32) (result i32) (i32.const 8))
		(func (export "fg_init") (param i32 i32) (result i32) (i32.const 0))
		(func (export "fg_on_event") (param i32 i32) (result i32) (i32.const 0))))");
	CHECK(!runtime.load(shared, bridge).ok());
}

void TestCallValidation() {
	auto runtime = Runtime();
	auto bridge = MockBridge();
	const auto bad = [&](const std::string &arguments) {
		auto loaded = Load(runtime, bridge, Module(
			R"((data (i32.const 64) "\1c"))",
			"(call $call " + arguments + ")"));
		CHECK(loaded.ok());
		if (!loaded.ok()) {
			return int32_t(0);
		}
		const auto result = loaded.value()->deliver(kEvent());
		CHECK(result.ok());
		return result.ok() ? result.value() : int32_t(0);
	};
	CHECK(bad("(i32.const 64) (i32.const 1) (i32.const 16) (i32.const 20)")
		== guest_status::kBadRequest);
	CHECK(bad("(i32.const -256) (i32.const 16) (i32.const 16) (i32.const 20)")
		== guest_status::kBadArguments);
	CHECK(bad("(i32.const 64) (i32.const -1) (i32.const 16) (i32.const 20)")
		== guest_status::kBadArguments);
	CHECK(bad("(local.get $ptr) (local.get $len) (i32.const 65534) (i32.const 20)")
		== guest_status::kBadArguments);
	CHECK(bad("(local.get $ptr) (local.get $len) (i32.const 16) (i32.const 65535)")
		== guest_status::kBadArguments);
	CHECK(bad("(local.get $ptr) (i32.const 70000) (i32.const 16) (i32.const 20)")
		== guest_status::kBadArguments);
	CHECK(bridge.requests.empty());
}

void TestCallBudget() {
	auto runtime = Runtime();
	auto bridge = MockBridge();
	auto loaded = Load(runtime, bridge, Module("", R"(
		(local $i i32) (local $s i32)
		(loop $l
			(local.set $s (call $call (local.get $ptr) (local.get $len)
				(i32.const 16) (i32.const 20)))
			(local.set $i (i32.add (local.get $i) (i32.const 1)))
			(br_if $l (i32.lt_u (local.get $i) (i32.const 1000))))
		(local.get $s))"));
	CHECK(loaded.ok());
	if (!loaded.ok()) {
		return;
	}
	const auto result = loaded.value()->deliver(kEvent());
	CHECK(result.ok() && result.value() == guest_status::kTooManyCalls);
	CHECK(bridge.requests.size() == 256);
	const auto again = loaded.value()->deliver(kEvent());
	CHECK(again.ok() && bridge.requests.size() == 512);
}

void TestResponseLimits() {
	auto runtime = Runtime();
	auto bridge = MockBridge();
	bridge.reply = cbor::Value::FromText(std::string(100, 'x'));
	auto limits = RuntimeLimits();
	limits.maxMessageBytes = 64;
	auto loaded = Load(runtime, bridge, Module("", R"(
		(call $call (local.get $ptr) (local.get $len)
			(i32.const 16) (i32.const 20)))"), limits);
	CHECK(loaded.ok());
	if (!loaded.ok()) {
		return;
	}
	const auto result = loaded.value()->deliver(kEvent());
	CHECK(result.ok() && result.value() == guest_status::kOk);
	CHECK(!loaded.value()->dead());

	const auto huge = cbor::Value::FromText(std::string(100, 'y'));
	const auto refused = loaded.value()->deliver(huge);
	CHECK(!refused.ok() && refused.error() == CallError::BadMessage);
	CHECK(!loaded.value()->dead());
}

void TestLargeRequests() {
	auto runtime = Runtime();
	auto bridge = MockBridge();
	const auto module = R"((module
		(import "freshgram" "call" (func $call (param i32 i32 i32 i32) (result i32)))
		(memory (export "memory") 64)
		(data (i32.const 64) "\a2\62op\61x\61d\5a\00\30\00\00")
		(global $bump (mut i32) (i32.const 3200000))
		(func (export "fg_alloc") (param $n i32) (result i32) (local $p i32)
			(local.set $p (global.get $bump))
			(global.set $bump (i32.add (global.get $bump) (local.get $n)))
			(local.get $p))
		(func (export "fg_free") (param i32 i32) (global.set $bump (i32.const 3200000)))
		(func (export "fg_init") (param i32 i32) (result i32) (i32.const 0))
		(func (export "fg_on_event") (param i32 i32) (result i32)
			(call $call (i32.const 64) (i32.const 3145741) (i32.const 16) (i32.const 20)))))";
	auto loaded = Load(runtime, bridge, module);
	CHECK(loaded.ok());
	if (loaded.ok()) {
		const auto result = loaded.value()->deliver(kEvent());
		CHECK(result.ok() && result.value() == guest_status::kOk);
		CHECK(bridge.requests.size() == 1);
		const auto data = bridge.requests.empty() ? nullptr : bridge.requests[0].find("d");
		CHECK(data && data->asBytes() && data->asBytes()->size() == (size_t(3) << 20));
	}

	auto strict = RuntimeLimits();
	strict.cbor.maxBytes = size_t(1) << 20;
	auto refused = Load(runtime, bridge, module, strict);
	CHECK(refused.ok());
	if (refused.ok()) {
		const auto result = refused.value()->deliver(kEvent());
		CHECK(result.ok() && result.value() == guest_status::kBadRequest);
	}
}

void TestAllocatorMisbehavior() {
	auto runtime = Runtime();
	auto bridge = MockBridge();
	auto loaded = Load(runtime, bridge, Module("", "(i32.const 0)", "(i32.const 0)"));
	CHECK(loaded.ok());
	if (loaded.ok()) {
		const auto result = loaded.value()->deliver(kEvent());
		CHECK(!result.ok() && result.error() == CallError::GuestFailed);
		CHECK(!loaded.value()->dead());
	}

	loaded = Load(runtime, bridge, Module("", "(i32.const 0)", "(i32.const -4096)"));
	CHECK(loaded.ok());
	if (loaded.ok()) {
		const auto result = loaded.value()->deliver(kEvent());
		CHECK(!result.ok() && result.error() == CallError::GuestFailed);
		CHECK(loaded.value()->dead());
	}

	loaded = Load(runtime, bridge, Module(
		"",
		R"((call $call (local.get $ptr) (local.get $len)
			(i32.const 16) (i32.const 20)))",
		"(i32.const -4096)"));
	CHECK(loaded.ok());
	if (loaded.ok()) {
		const auto result = loaded.value()->deliver(kEvent());
		CHECK(!result.ok() && result.error() == CallError::GuestFailed);
		CHECK(loaded.value()->dead());
	}
}

void TestStdoutIsLogged() {
	auto runtime = Runtime();
	auto bridge = MockBridge();
	auto loaded = Load(runtime, bridge, Module(
		R"((import "wasi_snapshot_preview1" "fd_write"
			(func $fd_write (param i32 i32 i32 i32) (result i32)))
			(data (i32.const 64) "hello\nworld")
			(data (i32.const 32) "\40\00\00\00\0b\00\00\00"))",
		R"((call $fd_write (i32.const 1) (i32.const 32) (i32.const 1)
			(i32.const 40)))"));
	CHECK(loaded.ok());
	if (!loaded.ok()) {
		return;
	}
	const auto result = loaded.value()->deliver(kEvent());
	CHECK(result.ok() && result.value() == 0);
	CHECK(bridge.logs.size() == 2);
	CHECK(bridge.logs.size() == 2 && bridge.logs[0] == "hello");
	CHECK(bridge.logs.size() == 2 && bridge.logs[1] == "world");

	loaded = Load(runtime, bridge, Module(
		R"((import "wasi_snapshot_preview1" "fd_write"
			(func $fd_write (param i32 i32 i32 i32) (result i32)))
			(data (i32.const 32) "\40\00\00\00\0b\00\00\00"))",
		R"((call $fd_write (i32.const 3) (i32.const 32) (i32.const 1)
			(i32.const 40)))"));
	CHECK(loaded.ok());
	if (loaded.ok()) {
		const auto badFd = loaded.value()->deliver(kEvent());
		CHECK(badFd.ok() && badFd.value() == 8);
	}

	loaded = Load(runtime, bridge, Module(
		R"((import "wasi_snapshot_preview1" "fd_write"
			(func $fd_write (param i32 i32 i32 i32) (result i32)))
			(data (i32.const 32) "\f0\ff\00\00\ff\ff\00\00"))",
		R"((call $fd_write (i32.const 1) (i32.const 32) (i32.const 1)
			(i32.const 40)))"));
	CHECK(loaded.ok());
	if (loaded.ok()) {
		const auto outside = loaded.value()->deliver(kEvent());
		CHECK(outside.ok() && outside.value() == 21);
	}
}

void TestClockAndRandom() {
	auto runtime = Runtime();
	auto bridge = MockBridge();
	auto loaded = Load(runtime, bridge, Module(
		R"((import "wasi_snapshot_preview1" "clock_time_get"
			(func $clock (param i32 i64 i32) (result i32))))",
		R"((drop (call $clock (i32.const 1) (i64.const 1) (i32.const 48)))
		(i32.wrap_i64 (i64.rem_u (i64.load (i32.const 48))
			(i64.const 1000000))))"));
	CHECK(loaded.ok());
	if (loaded.ok()) {
		const auto result = loaded.value()->deliver(kEvent());
		CHECK(result.ok() && result.value() == 0);
	}

	loaded = Load(runtime, bridge, Module(
		R"((import "wasi_snapshot_preview1" "clock_time_get"
			(func $clock (param i32 i64 i32) (result i32))))",
		"(call $clock (i32.const 7) (i64.const 1) (i32.const 48))"));
	CHECK(loaded.ok());
	if (loaded.ok()) {
		const auto result = loaded.value()->deliver(kEvent());
		CHECK(result.ok() && result.value() == 28);
	}

	loaded = Load(runtime, bridge, Module(
		R"((import "wasi_snapshot_preview1" "random_get"
			(func $random (param i32 i32) (result i32))))",
		R"((drop (call $random (i32.const 96) (i32.const 32)))
		(i32.or (i32.load (i32.const 96)) (i32.load (i32.const 100))))"));
	CHECK(loaded.ok());
	if (loaded.ok()) {
		const auto result = loaded.value()->deliver(kEvent());
		CHECK(result.ok() && result.value() != 0);
	}
}

} // namespace

int main() {
	TestRoundTripThroughBridge();
	TestTimeout();
	TestTraps();
	TestMemoryLimit();
	TestStartFunctionIsBounded();
	TestImportsAreAllowlisted();
	TestExportsAreChecked();
	TestDisabledProposals();
	TestCallValidation();
	TestCallBudget();
	TestResponseLimits();
	TestLargeRequests();
	TestAllocatorMisbehavior();
	TestStdoutIsLogged();
	TestClockAndRandom();
	return Failures() ? 1 : 0;
}
