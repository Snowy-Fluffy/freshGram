#pragma once

#include <wasmtime.hh>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>

namespace fgpl::guest {

[[nodiscard]] inline bool InRange(
		wasmtime::Span<uint8_t> memory,
		uint32_t pointer,
		uint64_t length) {
	return uint64_t(pointer) + length <= memory.size();
}

inline void Put(
		wasmtime::Span<uint8_t> memory,
		uint32_t pointer,
		uint64_t value,
		int bytes) {
	for (auto i = 0; i != bytes; ++i) {
		memory.data()[pointer + i] = uint8_t(value >> (8 * i));
	}
}

inline void PutU8(wasmtime::Span<uint8_t> memory, uint32_t pointer, uint8_t v) {
	Put(memory, pointer, v, 1);
}

inline void PutU16(wasmtime::Span<uint8_t> memory, uint32_t pointer, uint16_t v) {
	Put(memory, pointer, v, 2);
}

inline void PutU32(wasmtime::Span<uint8_t> memory, uint32_t pointer, uint32_t v) {
	Put(memory, pointer, v, 4);
}

inline void PutU64(wasmtime::Span<uint8_t> memory, uint32_t pointer, uint64_t v) {
	Put(memory, pointer, v, 8);
}

[[nodiscard]] inline uint32_t GetU32(
		wasmtime::Span<uint8_t> memory,
		uint32_t pointer) {
	auto result = uint32_t(0);
	for (auto i = 0; i != 4; ++i) {
		result |= uint32_t(memory.data()[pointer + i]) << (8 * i);
	}
	return result;
}

[[nodiscard]] inline std::optional<wasmtime::Memory> MemoryOf(
		wasmtime::Caller &caller) {
	const auto found = caller.get_export("memory");
	if (!found) {
		return std::nullopt;
	}
	if (const auto memory = std::get_if<wasmtime::Memory>(&*found)) {
		return *memory;
	}
	return std::nullopt;
}

[[nodiscard]] inline std::optional<wasmtime::Span<uint8_t>> ViewOf(
		wasmtime::Caller &caller) {
	const auto memory = MemoryOf(caller);
	if (!memory) {
		return std::nullopt;
	}
	return memory->data(caller.context());
}

template <typename F>
void Define(
		wasmtime::Linker &linker,
		std::unordered_set<std::string> &defined,
		std::string_view module,
		std::string_view name,
		F &&function) {
	if (linker.func_wrap(
			module,
			name,
			std::decay_t<F>(std::forward<F>(function)))) {
		defined.insert(std::string(module) + "::" + std::string(name));
	}
}

} // namespace fgpl::guest
