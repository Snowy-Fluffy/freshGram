cmake_minimum_required(VERSION 3.20)
project(freshgram_plugins_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FGPL_BUILD_TESTS "Build the unit tests" ON)
option(FGPL_SANITIZE "Build with address and undefined behavior sanitizers" OFF)
option(FGPL_FUZZ "Build the libFuzzer targets (needs clang)" OFF)
set(FGPL_WASMTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wasmtime-v50.0.0-x86_64-linux-c-api"
	CACHE PATH "Unpacked Wasmtime C API release")

function(fgpl_options target)
	if (MSVC)
		target_compile_options(${target} PRIVATE /W4 /permissive-)
	else()
		target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
	endif()
	if (FGPL_SANITIZE AND NOT MSVC)
		target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
		target_link_options(${target} PRIVATE -fsanitize=address,undefined)
	endif()
endfunction()

add_library(fgpl_core STATIC src/cbor.cpp src/vfs.cpp)
target_include_directories(fgpl_core PUBLIC src)
fgpl_options(fgpl_core)

add_library(wasmtime STATIC IMPORTED)
set_target_properties(wasmtime PROPERTIES
	IMPORTED_LOCATION "${FGPL_WASMTIME_DIR}/lib/libwasmtime.a"
	INTERFACE_INCLUDE_DIRECTORIES "${FGPL_WASMTIME_DIR}/include")
if (UNIX)
	set_property(TARGET wasmtime PROPERTY INTERFACE_LINK_LIBRARIES pthread dl m)
endif()

add_library(fgpl_runtime STATIC src/runtime.cpp src/wasi_host.cpp)
target_link_libraries(fgpl_runtime PUBLIC fgpl_core wasmtime)
fgpl_options(fgpl_runtime)

if (FGPL_BUILD_TESTS)
	enable_testing()
	find_program(FGPL_CLANG clang)
	set(fgpl_wasm_ready OFF)
	if (FGPL_CLANG)
		execute_process(COMMAND ${FGPL_CLANG} --print-targets
			OUTPUT_VARIABLE fgpl_targets ERROR_QUIET)
		if (fgpl_targets MATCHES "wasm32")
			set(fgpl_wasm_ready ON)
		endif()
	endif()

	foreach (name test_cbor test_vfs test_runtime)
		add_executable(${name} tests/${name}.cpp)
		target_link_libraries(${name} PRIVATE fgpl_core)
		if (name STREQUAL "test_runtime")
			target_link_libraries(${name} PRIVATE fgpl_runtime)
		endif()
		fgpl_options(${name})
		add_test(NAME ${name} COMMAND ${name})
	endforeach()

	if (fgpl_wasm_ready)
		set(probe_wasm ${CMAKE_CURRENT_BINARY_DIR}/wasi_probe.wasm)
		add_custom_command(OUTPUT ${probe_wasm}
			COMMAND ${FGPL_CLANG} --target=wasm32 -O2 -nostdlib -fno-builtin
				-Wl,--no-entry -o ${probe_wasm}
				${CMAKE_CURRENT_SOURCE_DIR}/tests/plugins/wasi_probe.c
			DEPENDS tests/plugins/wasi_probe.c)
		add_custom_target(wasi_probe DEPENDS ${probe_wasm})
		add_executable(test_wasi tests/test_wasi.cpp)
		target_link_libraries(test_wasi PRIVATE fgpl_runtime)
		target_compile_definitions(test_wasi PRIVATE FGPL_PROBE_WASM="${probe_wasm}")
		add_dependencies(test_wasi wasi_probe)
		fgpl_options(test_wasi)
		add_test(NAME test_wasi COMMAND test_wasi)
	endif()
endif()

if (FGPL_FUZZ)
	foreach (name fuzz_cbor fuzz_vfs)
		add_executable(${name} tests/${name}.cpp)
		target_link_libraries(${name} PRIVATE fgpl_core)
		target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
		target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
	endforeach()
endif()
