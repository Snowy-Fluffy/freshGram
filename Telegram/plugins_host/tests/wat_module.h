#pragma once

#include "check.h"

#include <wasmtime.hh>

#include <cstdio>
#include <string>
#include <vector>

namespace wat {

[[nodiscard]] inline std::string Module(
		const std::string &extra,
		const std::string &onEvent,
		const std::string &alloc = "") {
	const auto allocation = alloc.empty()
		? "(local $p i32) (local.set $p (global.get $bump)) "
			"(global.set $bump (i32.add (global.get $bump) (local.get $n))) "
			"(local.get $p)"
		: alloc;
	return R"((module
		(import "freshgram" "call"
			(func $call (param i32 i32 i32 i32) (result i32)))
		)" + extra + R"(
		(memory (export "memory") 1)
		(global $bump (mut i32) (i32.const 1024))
		(func (export "fg_alloc") (param $n i32) (result i32) )" + allocation + R"()
		(func (export "fg_free") (param i32 i32) (global.set $bump (i32.const 1024)))
		(func (export "fg_init") (param i32 i32) (result i32) (i32.const 0))
		(func (export "fg_on_event") (param $ptr i32) (param $len i32) (result i32)
		)" + onEvent + R"()))";
}

[[nodiscard]] inline std::vector<uint8_t> Wasm(const std::string &wat) {
	auto result = wasmtime::wat2wasm(wat);
	if (!result) {
		std::fprintf(stderr, "wat: %s\n", result.err().message().c_str());
		CHECK(false);
		return {};
	}
	return result.unwrap();
}

} // namespace wat
