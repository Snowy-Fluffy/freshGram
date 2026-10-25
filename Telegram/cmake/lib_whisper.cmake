# This file is part of freshGram.
#
# whisper.cpp (CPU only) for the local speech recognition.

option(FRESHGRAM_DISABLE_RECOGNITION "Build without the local recognition" OFF)

add_library(lib_whisper INTERFACE)
add_library(tdesktop::lib_whisper ALIAS lib_whisper)

if (NOT FRESHGRAM_DISABLE_RECOGNITION)
    function(freshgram_add_whisper)
        # The options are forced in the cache, because the old minimum
        # version of whisper.cpp ignores ordinary variables for them.
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(CMAKE_SKIP_INSTALL_RULES ON)
        set(CMAKE_POSITION_INDEPENDENT_CODE ON)
        set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
        set(WHISPER_CURL OFF CACHE BOOL "" FORCE)
        set(WHISPER_SDL2 OFF CACHE BOOL "" FORCE)
        set(WHISPER_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
        set(GGML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
        set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
        set(GGML_METAL OFF CACHE BOOL "" FORCE)
        set(GGML_CUDA OFF CACHE BOOL "" FORCE)
        set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
        set(GGML_CCACHE OFF CACHE BOOL "" FORCE)
        # ggml reads these variables even when it never sets them.
        set(ARCH_FLAGS "")
        set(ARCH_DEFINITIONS "")
        # GGML_NATIVE is off, so the instruction sets are not enabled
        # by default, and the engine needs AVX2 to be fast.
        if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x64)$"
            OR (APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "x86_64"))
            set(GGML_SSE42 ON CACHE BOOL "" FORCE)
            set(GGML_AVX ON CACHE BOOL "" FORCE)
            set(GGML_AVX2 ON CACHE BOOL "" FORCE)
            set(GGML_BMI2 ON CACHE BOOL "" FORCE)
            set(GGML_FMA ON CACHE BOOL "" FORCE)
            set(GGML_F16C ON CACHE BOOL "" FORCE)
        elseif (MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
            # WHY: ggml for MSVC on ARM only probes the build machine, so the answers are
            # fixed to the ARMv8-A baseline and the binary runs on every ARM64 device.
            set(GGML_NATIVE ON CACHE BOOL "" FORCE)
            foreach (feature dotprod i8mm sve sme)
                set(GGML_MACHINE_SUPPORTS_${feature} OFF CACHE BOOL "" FORCE)
                set(GGML_MACHINE_SUPPORTS_no${feature} OFF CACHE BOOL "" FORCE)
            endforeach()
            set(GGML_MACHINE_SUPPORTS_fma ON CACHE BOOL "" FORCE)
        endif()
        # The warnings of the library are errors in this project.
        if (COMMAND cmake_diagnostic)
            cmake_diagnostic(PUSH)
            foreach (category AUTHOR DEPRECATED POLICY UNINITIALIZED)
                cmake_diagnostic(DEMOTE CMD_${category} WARN)
            endforeach()
        else()
            set(was_suppressed "$CACHE{CMAKE_SUPPRESS_DEVELOPER_ERRORS}")
            set(CMAKE_SUPPRESS_DEVELOPER_ERRORS ON CACHE BOOL "" FORCE)
        endif()
        add_subdirectory(
            ${third_party_loc}/whisper.cpp
            ${CMAKE_CURRENT_BINARY_DIR}/whisper.cpp
            EXCLUDE_FROM_ALL)
        if (COMMAND cmake_diagnostic)
            cmake_diagnostic(POP)
        elseif (was_suppressed STREQUAL "")
            unset(CMAKE_SUPPRESS_DEVELOPER_ERRORS CACHE)
        else()
            set(CMAKE_SUPPRESS_DEVELOPER_ERRORS ${was_suppressed} CACHE BOOL "" FORCE)
        endif()
    endfunction()
    freshgram_add_whisper()

    target_link_libraries(lib_whisper INTERFACE whisper)
    target_compile_definitions(lib_whisper INTERFACE FRESHGRAM_LOCAL_RECOGNITION)
endif()
