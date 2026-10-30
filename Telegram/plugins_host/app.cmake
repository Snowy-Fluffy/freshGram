# The app side of the plugins: no Wasmtime here, the host process is downloaded.
# WHY: the Mac App Store and Microsoft Store builds cannot start a downloaded executable.
if (build_macstore OR build_winstore)
	return()
endif()

set(fgpl_src ${CMAKE_CURRENT_LIST_DIR}/src/fgpl)

if (WIN32)
	set(fgpl_platform_sources
		${fgpl_src}/os_win.cpp
		${fgpl_src}/ipc_win.cpp
		${fgpl_src}/process_win.cpp
		${fgpl_src}/sealed_win.cpp
		${fgpl_src}/sandbox_win.cpp)
elseif (APPLE)
	set(fgpl_platform_sources
		${fgpl_src}/os_posix.cpp
		${fgpl_src}/ipc_posix.cpp
		${fgpl_src}/process_posix.cpp
		${fgpl_src}/sealed_mac.cpp
		${fgpl_src}/sandbox_mac.cpp)
else()
	set(fgpl_platform_sources
		${fgpl_src}/os_posix.cpp
		${fgpl_src}/ipc_posix.cpp
		${fgpl_src}/process_posix.cpp
		${fgpl_src}/sealed_linux.cpp
		${fgpl_src}/sandbox_linux.cpp)
endif()

add_library(fgpl_app STATIC
	${fgpl_src}/cbor.cpp
	${fgpl_src}/vfs.cpp
	${fgpl_src}/sha256.cpp
	${fgpl_src}/module_cache.cpp
	${fgpl_src}/base64.cpp
	${fgpl_src}/json.cpp
	${fgpl_src}/semver.cpp
	${fgpl_src}/zip_reader.cpp
	${fgpl_src}/zip_writer.cpp
	${fgpl_src}/signature.cpp
	${fgpl_src}/net_policy.cpp
	${fgpl_src}/manifest.cpp
	${fgpl_src}/wasm_scan.cpp
	${fgpl_src}/package.cpp
	${fgpl_src}/broker.cpp
	${fgpl_src}/permissions.cpp
	${fgpl_src}/file_storage.cpp
	${fgpl_src}/runtime_bundle.cpp
	${fgpl_platform_sources}
	${fgpl_src}/ui_tree.cpp
	${fgpl_src}/ipc.cpp
	${fgpl_src}/host_client.cpp
	${fgpl_src}/cached_load.cpp
	${fgpl_src}/async_pool.cpp
	${fgpl_src}/manager.cpp)
add_library(tdesktop::fgpl_app ALIAS fgpl_app)
init_target(fgpl_app)
target_include_directories(fgpl_app PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_link_libraries(fgpl_app
PUBLIC
	desktop-app::external_zlib
	desktop-app::external_openssl)
if (WIN32)
	target_link_libraries(fgpl_app PUBLIC bcrypt advapi32 userenv)
endif()
