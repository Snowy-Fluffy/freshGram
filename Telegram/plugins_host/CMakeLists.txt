cmake_minimum_required(VERSION 3.20)
project(freshgram_plugins_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FGPL_BUILD_TESTS "Build the unit tests" ON)
option(FGPL_SANITIZE "Build with address and undefined behavior sanitizers" OFF)
option(FGPL_TSAN "Build with the thread sanitizer (run the host without its sandbox)" OFF)
option(FGPL_FUZZ "Build the libFuzzer targets (needs clang)" OFF)
option(FGPL_HOST_ONLY "Build only the plugin host, without OpenSSL and zlib (cross builds)" OFF)
set(FGPL_WASMTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wasmtime-v50.0.0-x86_64-linux-c-api"
	CACHE PATH "Unpacked Wasmtime C API release")

if (NOT FGPL_HOST_ONLY)
	find_package(ZLIB REQUIRED)
	find_package(OpenSSL REQUIRED)
endif()
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

function(fgpl_warnings target)
	if (MSVC)
		target_compile_options(${target} PRIVATE /W4 /permissive-)
	else()
		target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
	endif()
endfunction()

function(fgpl_options target)
	fgpl_warnings(${target})
	if (FGPL_TSAN AND NOT MSVC)
		target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
		target_link_options(${target} PRIVATE -fsanitize=thread)
	endif()
	if (FGPL_SANITIZE AND NOT MSVC)
		target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
		target_link_options(${target} PRIVATE -fsanitize=address,undefined)
	endif()
endfunction()

add_library(fgpl_base STATIC src/fgpl/cbor.cpp src/fgpl/vfs.cpp)
target_include_directories(fgpl_base PUBLIC src)
target_link_libraries(fgpl_base PUBLIC Threads::Threads)
fgpl_options(fgpl_base)

if (FGPL_HOST_ONLY)
	set(fgpl_common fgpl_base)
else()
	add_library(fgpl_core STATIC src/fgpl/sha256.cpp src/fgpl/module_cache.cpp
		src/fgpl/base64.cpp src/fgpl/json.cpp src/fgpl/semver.cpp src/fgpl/zip_reader.cpp src/fgpl/signature.cpp src/fgpl/net_policy.cpp src/fgpl/manifest.cpp
		src/fgpl/wasm_scan.cpp src/fgpl/package.cpp src/fgpl/zip_writer.cpp src/fgpl/broker.cpp
		src/fgpl/permissions.cpp src/fgpl/file_storage.cpp src/fgpl/runtime_bundle.cpp src/fgpl/ui_tree.cpp)
	target_link_libraries(fgpl_core PUBLIC fgpl_base ZLIB::ZLIB OpenSSL::Crypto)
	fgpl_options(fgpl_core)
	set(fgpl_common fgpl_core)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
	add_library(fgpl_sandbox STATIC src/fgpl/sandbox_linux.cpp)
	target_include_directories(fgpl_sandbox PUBLIC src)
	target_link_libraries(fgpl_sandbox PUBLIC Threads::Threads)
	fgpl_warnings(fgpl_sandbox)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
	add_library(fgpl_ipc STATIC src/fgpl/ipc.cpp)
	target_link_libraries(fgpl_ipc PUBLIC ${fgpl_common})
	fgpl_options(fgpl_ipc)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT FGPL_HOST_ONLY)
	add_library(fgpl_client STATIC src/fgpl/host_client.cpp src/fgpl/cached_load.cpp
		src/fgpl/manager.cpp)
	target_link_libraries(fgpl_client PUBLIC fgpl_ipc)
	fgpl_options(fgpl_client)
endif()

add_library(wasmtime STATIC IMPORTED)
set_target_properties(wasmtime PROPERTIES
	IMPORTED_LOCATION "${FGPL_WASMTIME_DIR}/lib/libwasmtime.a"
	INTERFACE_INCLUDE_DIRECTORIES "${FGPL_WASMTIME_DIR}/include")
if (UNIX)
	set_property(TARGET wasmtime PROPERTY INTERFACE_LINK_LIBRARIES pthread dl m)
endif()

add_library(fgpl_runtime STATIC src/fgpl/runtime.cpp src/fgpl/wasi_host.cpp)
target_link_libraries(fgpl_runtime PUBLIC ${fgpl_common} wasmtime)
fgpl_options(fgpl_runtime)

if (NOT FGPL_HOST_ONLY)
	add_executable(fgpl_tool src/fgpl/tool_main.cpp)
	target_link_libraries(fgpl_tool PRIVATE fgpl_runtime)
	fgpl_options(fgpl_tool)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT FGPL_SANITIZE)
	add_executable(fgpl_plugin_host src/fgpl/host_main.cpp)
	target_link_libraries(fgpl_plugin_host PRIVATE fgpl_runtime fgpl_sandbox fgpl_ipc)
	target_link_options(fgpl_plugin_host PRIVATE -static-libstdc++ -static-libgcc -Wl,--as-needed)
	fgpl_options(fgpl_plugin_host)
	if (FGPL_BUILD_TESTS AND NOT FGPL_HOST_ONLY)
		add_executable(fgpl_plugin_host_unsandboxed src/fgpl/host_main.cpp)
		target_link_libraries(fgpl_plugin_host_unsandboxed PRIVATE fgpl_runtime fgpl_sandbox fgpl_ipc)
		target_compile_definitions(fgpl_plugin_host_unsandboxed PRIVATE FGPL_UNSANDBOXED_TEST_HOST)
		fgpl_options(fgpl_plugin_host_unsandboxed)
	endif()
endif()

if (FGPL_BUILD_TESTS AND NOT FGPL_HOST_ONLY)
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

	foreach (name test_cbor test_vfs test_cache test_formats test_zip test_manifest test_package test_runtime test_broker test_runtime_bundle)
		add_executable(${name} tests/${name}.cpp)
		target_link_libraries(${name} PRIVATE fgpl_core)
		if (name STREQUAL "test_runtime" OR name STREQUAL "test_broker"
			OR name STREQUAL "test_manifest" OR name STREQUAL "test_package")
			target_link_libraries(${name} PRIVATE fgpl_runtime)
		endif()
		fgpl_options(${name})
		add_test(NAME ${name} COMMAND ${name})
	endforeach()

	if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
		add_executable(sandbox_probe tests/sandbox_probe.cpp)
		target_link_libraries(sandbox_probe PRIVATE fgpl_sandbox)
		fgpl_warnings(sandbox_probe)
		add_executable(test_sandbox tests/test_sandbox.cpp)
		target_link_libraries(test_sandbox PRIVATE fgpl_sandbox)
		target_compile_definitions(test_sandbox PRIVATE
			FGPL_SANDBOX_PROBE="$<TARGET_FILE:sandbox_probe>")
		add_dependencies(test_sandbox sandbox_probe)
		fgpl_warnings(test_sandbox)
		add_test(NAME test_sandbox COMMAND test_sandbox)
	endif()

	if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT FGPL_SANITIZE)
		add_executable(test_host tests/test_host.cpp)
		target_link_libraries(test_host PRIVATE fgpl_client wasmtime)
		target_compile_definitions(test_host PRIVATE
			FGPL_HOST_BINARY="$<TARGET_FILE:fgpl_plugin_host>"
			FGPL_UNSANDBOXED_HOST_BINARY="$<TARGET_FILE:fgpl_plugin_host_unsandboxed>")
		add_dependencies(test_host fgpl_plugin_host fgpl_plugin_host_unsandboxed)
		fgpl_warnings(test_host)
		add_test(NAME test_host COMMAND test_host)
	endif()

	if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT FGPL_SANITIZE)
		add_executable(test_abuse tests/test_abuse.cpp)
		target_link_libraries(test_abuse PRIVATE fgpl_client wasmtime)
		target_compile_definitions(test_abuse PRIVATE
			FGPL_HOST_BINARY="$<TARGET_FILE:fgpl_plugin_host>"
			FGPL_CPYTHON_DIR="${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpython-wasi"
			FGPL_PYTHON_PLUGIN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/plugins/python"
			FGPL_PYTHON_SDK_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../plugins_sdk/python")
		add_dependencies(test_abuse fgpl_plugin_host)
		fgpl_warnings(test_abuse)
		add_test(NAME test_abuse COMMAND test_abuse)

		add_executable(test_sdk tests/test_sdk.cpp)
		target_link_libraries(test_sdk PRIVATE fgpl_client fgpl_runtime)
		target_compile_definitions(test_sdk PRIVATE
			FGPL_HOST_BINARY="$<TARGET_FILE:fgpl_plugin_host>"
			FGPL_CPYTHON_DIR="${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpython-wasi"
			FGPL_PYTHON_PLUGIN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/plugins/python"
			FGPL_PYTHON_SDK_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../plugins_sdk/python")
		if (fgpl_wasm_ready)
			set(counter_wasm ${CMAKE_CURRENT_BINARY_DIR}/counter.wasm)
			add_custom_command(OUTPUT ${counter_wasm}
				COMMAND ${FGPL_CLANG} --target=wasm32 -O2 -nostdlib -fno-builtin
					-Wall -Wextra -Wl,--no-entry -o ${counter_wasm}
					${CMAKE_CURRENT_SOURCE_DIR}/../plugins_sdk/c/examples/counter.c
				DEPENDS ../plugins_sdk/c/examples/counter.c ../plugins_sdk/c/freshgram.h)
			add_custom_target(counter_wasm_target DEPENDS ${counter_wasm})
			add_dependencies(test_sdk counter_wasm_target)
			target_compile_definitions(test_sdk PRIVATE FGPL_COUNTER_WASM="${counter_wasm}")
		endif()
		add_dependencies(test_sdk fgpl_plugin_host)
		fgpl_warnings(test_sdk)
		add_test(NAME test_sdk COMMAND test_sdk)

		add_executable(test_manager tests/test_manager.cpp)
		target_link_libraries(test_manager PRIVATE fgpl_client fgpl_runtime)
		target_compile_definitions(test_manager PRIVATE
			FGPL_HOST_BINARY="$<TARGET_FILE:fgpl_plugin_host>"
			FGPL_UNSANDBOXED_HOST_BINARY="$<TARGET_FILE:fgpl_plugin_host_unsandboxed>"
			FGPL_CPYTHON_DIR="${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpython-wasi"
			FGPL_PYTHON_PLUGIN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/plugins/python"
			FGPL_PYTHON_SDK_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../plugins_sdk/python")
		if (fgpl_wasm_ready)
			add_dependencies(test_manager counter_wasm_target)
			target_compile_definitions(test_manager PRIVATE FGPL_COUNTER_WASM="${counter_wasm}")
		endif()
		add_dependencies(test_manager fgpl_plugin_host fgpl_plugin_host_unsandboxed)
		fgpl_options(test_manager)
		add_test(NAME test_manager COMMAND test_manager)

		add_executable(test_tool tests/test_tool.cpp)
		target_link_libraries(test_tool PRIVATE fgpl_runtime)
		target_compile_definitions(test_tool PRIVATE FGPL_TOOL="$<TARGET_FILE:fgpl_tool>")
		add_dependencies(test_tool fgpl_tool)
		fgpl_warnings(test_tool)
		add_test(NAME test_tool COMMAND test_tool)

		add_executable(test_install tests/test_install.cpp)
		target_link_libraries(test_install PRIVATE fgpl_client fgpl_runtime)
		target_compile_definitions(test_install PRIVATE
			FGPL_HOST_BINARY="$<TARGET_FILE:fgpl_plugin_host>"
			FGPL_CPYTHON_DIR="${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpython-wasi"
			FGPL_PYTHON_PLUGIN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/plugins/python"
			FGPL_PYTHON_SDK_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../plugins_sdk/python")
		if (fgpl_wasm_ready)
			add_dependencies(test_install counter_wasm_target)
			target_compile_definitions(test_install PRIVATE FGPL_COUNTER_WASM="${counter_wasm}")
		endif()
		add_dependencies(test_install fgpl_plugin_host)
		fgpl_warnings(test_install)
		add_test(NAME test_install COMMAND test_install)

		add_executable(test_compile tests/test_compile.cpp)
		target_link_libraries(test_compile PRIVATE fgpl_client wasmtime)
		target_compile_definitions(test_compile PRIVATE
			FGPL_HOST_BINARY="$<TARGET_FILE:fgpl_plugin_host>"
			FGPL_CPYTHON_DIR="${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpython-wasi"
			FGPL_PYTHON_PLUGIN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/plugins/python"
			FGPL_PYTHON_SDK_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../plugins_sdk/python")
		add_dependencies(test_compile fgpl_plugin_host)
		fgpl_warnings(test_compile)
		add_test(NAME test_compile COMMAND test_compile)

		add_executable(test_python tests/test_python.cpp)
		target_link_libraries(test_python PRIVATE fgpl_client)
		target_compile_definitions(test_python PRIVATE
			FGPL_HOST_BINARY="$<TARGET_FILE:fgpl_plugin_host>"
			FGPL_CPYTHON_DIR="${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpython-wasi"
			FGPL_PYTHON_PLUGIN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/plugins/python"
			FGPL_PYTHON_SDK_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../plugins_sdk/python")
		add_dependencies(test_python fgpl_plugin_host)
		fgpl_warnings(test_python)
		add_test(NAME test_python COMMAND test_python)
	endif()

	if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
		add_executable(evil_host tests/evil_host.cpp)
		target_link_libraries(evil_host PRIVATE fgpl_ipc)
		fgpl_options(evil_host)
		add_executable(test_attacks tests/test_attacks.cpp)
		target_link_libraries(test_attacks PRIVATE fgpl_client)
		target_compile_definitions(test_attacks PRIVATE
			FGPL_EVIL_HOST="$<TARGET_FILE:evil_host>")
		if (FGPL_SANITIZE)
			target_compile_definitions(test_attacks PRIVATE FGPL_SANITIZED)
		endif()
		add_dependencies(test_attacks evil_host)
		fgpl_options(test_attacks)
		add_test(NAME test_attacks COMMAND test_attacks)
	endif()

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
	foreach (name fuzz_cbor fuzz_vfs fuzz_frame fuzz_wasi fuzz_package)
		add_executable(${name} tests/${name}.cpp)
		target_link_libraries(${name} PRIVATE fgpl_core)
		if (name STREQUAL "fuzz_frame")
			target_link_libraries(${name} PRIVATE fgpl_ipc)
		elseif (name STREQUAL "fuzz_package")
			target_link_libraries(${name} PRIVATE fgpl_runtime)
		elseif (name STREQUAL "fuzz_wasi")
			target_link_libraries(${name} PRIVATE fgpl_runtime)
		endif()
		target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
		target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
	endforeach()
endif()
