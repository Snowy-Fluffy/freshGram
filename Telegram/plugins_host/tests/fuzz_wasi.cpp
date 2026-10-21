#include "fgpl/runtime.h"

#include <wasmtime.hh>

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace fgpl;

struct Function {
	const char *name;
	const char *params;
	bool returnsNothing;
};

const Function kFunctions[] = {
	{ "args_sizes_get", "ii", false },
	{ "args_get", "ii", false },
	{ "environ_sizes_get", "ii", false },
	{ "environ_get", "ii", false },
	{ "clock_time_get", "ili", false },
	{ "clock_res_get", "ii", false },
	{ "random_get", "ii", false },
	{ "sched_yield", "", false },
	{ "fd_write", "iiii", false },
	{ "fd_prestat_get", "ii", false },
	{ "fd_prestat_dir_name", "iii", false },
	{ "fd_close", "i", false },
	{ "fd_fdstat_get", "ii", false },
	{ "fd_read", "iiii", false },
	{ "fd_pread", "iiili", false },
	{ "fd_seek", "ilii", false },
	{ "fd_tell", "ii", false },
	{ "fd_filestat_get", "ii", false },
	{ "path_open", "iiiiillii", false },
	{ "path_filestat_get", "iiiii", false },
	{ "path_readlink", "iiiiii", false },
	{ "fd_readdir", "iiili", false },
	{ "path_create_directory", "iii", false },
	{ "path_remove_directory", "iii", false },
	{ "path_unlink_file", "iii", false },
	{ "path_rename", "iiiiii", false },
	{ "path_link", "iiiiiii", false },
	{ "path_symlink", "iiiii", false },
	{ "fd_allocate", "ill", false },
	{ "fd_filestat_set_size", "il", false },
	{ "fd_filestat_set_times", "illi", false },
	{ "path_filestat_set_times", "iiiilli", false },
	{ "fd_pwrite", "iiili", false },
	{ "fd_renumber", "ii", false },
	{ "fd_fdstat_set_rights", "ill", false },
	{ "poll_oneoff", "iiii", false },
	{ "sock_accept", "iii", false },
	{ "sock_recv", "iiiiii", false },
	{ "sock_send", "iiiii", false },
	{ "sock_shutdown", "ii", false },
	{ "fd_sync", "i", false },
	{ "fd_datasync", "i", false },
	{ "fd_advise", "illi", false },
	{ "fd_fdstat_set_flags", "ii", false },
	{ "proc_exit", "i", true },
};

constexpr auto kCount = int(sizeof(kFunctions) / sizeof(kFunctions[0]));
constexpr auto kRecord = 96;

[[nodiscard]] std::string BuildModule() {
	auto text = std::string("(module\n");
	for (auto i = 0; i != kCount; ++i) {
		text += "(import \"wasi_snapshot_preview1\" \"";
		text += kFunctions[i].name;
		text += "\" (func $f" + std::to_string(i);
		for (auto p = kFunctions[i].params; *p; ++p) {
			text += (*p == 'l') ? " (param i64)" : " (param i32)";
		}
		text += kFunctions[i].returnsNothing ? "))\n" : " (result i32)))\n";
	}
	text += R"(
(import "freshgram" "call" (func $call (param i32 i32 i32 i32) (result i32)))
(memory (export "memory") 2)
(data (i32.const 0) "hello.txt")
(data (i32.const 64) "sub")
(data (i32.const 128) "sub/inner.txt")
(data (i32.const 192) "..")
(data (i32.const 256) "../runtime")
(data (i32.const 320) "/plugin")
(data (i32.const 384) "data")
(data (i32.const 448) "sub/../hello.txt")
(data (i32.const 512) ".")
(data (i32.const 576) "\00\04\00\00\08\00\00\00")
(data (i32.const 640) "\00\04\00\00\04\00\00\00\10\04\00\00\04\00\00\00")
(data (i32.const 704) "\00\00\00\00\ff\ff\ff\ff")
(data (i32.const 768) "\c0\03\00\00\08\00\00\00")
(global $bump (mut i32) (i32.const 4096))
(func (export "fg_alloc") (param $n i32) (result i32)
	(local $p i32)
	(local.set $p (global.get $bump))
	(global.set $bump (i32.add (global.get $bump) (local.get $n)))
	(local.get $p))
(func (export "fg_free") (param i32 i32) (global.set $bump (i32.const 4096)))
(func (export "fg_init") (param i32 i32) (result i32) (i32.const 0))
(func $arg32 (param $p i32) (result i32)
	(local $v i32)
	(local.set $v (i32.load (local.get $p)))
	(if (i32.eq (i32.load8_u offset=8 (local.get $p)) (i32.const 1))
		(then (return (i32.rem_u (local.get $v) (i32.const 70000)))))
	(if (i32.eq (i32.load8_u offset=8 (local.get $p)) (i32.const 2))
		(then (return (i32.rem_u (local.get $v) (i32.const 12)))))
	(if (i32.eq (i32.load8_u offset=8 (local.get $p)) (i32.const 3))
		(then (return (i32.mul (i32.rem_u (local.get $v) (i32.const 14)) (i32.const 64)))))
	(local.get $v))
(func $arg64 (param $p i32) (result i64)
	(local $v i64)
	(local.set $v (i64.load (local.get $p)))
	(if (i32.eq (i32.load8_u offset=8 (local.get $p)) (i32.const 1))
		(then (return (i64.rem_u (local.get $v) (i64.const 70000)))))
	(if (i32.eq (i32.load8_u offset=8 (local.get $p)) (i32.const 2))
		(then (return (i64.rem_u (local.get $v) (i64.const 40)))))
	(local.get $v))
(func (export "fg_on_event") (param $ptr i32) (param $len i32) (result i32)
	(local $p i32) (local $end i32) (local $f i32)
	(local.set $p (local.get $ptr))
	(local.set $end (i32.add (local.get $ptr) (local.get $len)))
	(block $done
	(loop $next
		(br_if $done (i32.gt_u (i32.add (local.get $p) (i32.const )" + std::to_string(kRecord) + R"()) (local.get $end)))
		(local.set $f (i32.rem_u (i32.load8_u (local.get $p)) (i32.const )" + std::to_string(kCount) + R"()))
		(block $adv
)";
	for (auto k = kCount - 1; k >= 0; --k) {
		text += "(block $c" + std::to_string(k) + "\n";
	}
	text += "(br_table";
	for (auto k = 0; k != kCount; ++k) {
		text += " $c" + std::to_string(k);
	}
	text += " $c" + std::to_string(kCount - 1) + " (local.get $f))\n";
	for (auto k = 0; k != kCount; ++k) {
		text += ")\n";
		auto call = "(call $f" + std::to_string(k);
		auto offset = 1;
		for (auto p = kFunctions[k].params; *p; ++p, offset += 9) {
			const auto at = "(i32.add (local.get $p) (i32.const "
				+ std::to_string(offset) + "))";
			call += (*p == 'l')
				? " (call $arg64 " + at + ")"
				: " (call $arg32 " + at + ")";
		}
		call += ")";
		text += kFunctions[k].returnsNothing ? call : "(drop " + call + ")";
		text += "\n(br $adv)\n";
	}
	text += R"()
		(local.set $p (i32.add (local.get $p) (i32.const )" + std::to_string(kRecord) + R"()))
		(br $next)))
	(i32.const 0)))
)";
	return text;
}

class QuietBridge final : public Bridge {
public:
	cbor::Value call(const cbor::Value &) override {
		return {};
	}
	void log(std::string_view) override {
	}
};

struct Harness {
	Harness() {
		auto wasm = wasmtime::wat2wasm(BuildModule());
		if (!wasm) {
			std::fprintf(stderr, "wat: %s\n", wasm.err().message().c_str());
			std::abort();
		}
		module = wasm.unwrap();
		auto builder = VfsBuilder();
		const auto add = [&](const char *path, std::string_view text) {
			if (builder.addFile(path, cbor::Bytes(text.begin(), text.end()))
				!= Errc::Ok) {
				std::abort();
			}
		};
		add("plugin/hello.txt", "hello from the vfs\n");
		add("plugin/sub/inner.txt", "inner");
		add("plugin/data/a.txt", "a");
		add("plugin/data/b.txt", "bb");
		add("runtime/lib/os.py", "import sys");
		files = builder.finish();
	}

	void ensure() {
		if (plugin && !plugin->dead()) {
			return;
		}
		auto limits = RuntimeLimits();
		limits.callTimeoutMs = 200;
		limits.memoryBytes = uint64_t(4) << 20;
		limits.maxMessageBytes = 1 << 16;
		auto loaded = runtime.load(module, bridge, limits, files);
		if (!loaded.ok()) {
			std::fprintf(stderr, "load: %s\n", loaded.error().detail.c_str());
			std::abort();
		}
		plugin = std::move(loaded.value());
	}

	Runtime runtime;
	QuietBridge bridge;
	std::vector<uint8_t> module;
	std::shared_ptr<const Vfs> files;
	std::unique_ptr<Plugin> plugin;
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	static auto harness = Harness();
	if (size > 2048) {
		return 0;
	}
	harness.ensure();
	const auto result = harness.plugin->deliver(
		cbor::Value::FromBytes(cbor::Bytes(data, data + size)));
	return result.ok() ? 0 : 0;
}
