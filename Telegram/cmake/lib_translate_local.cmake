# This file is part of freshGram.
#
# CTranslate2 (CPU only, int8 on the bundled Ruy) and sentencepiece for the
# local translation with the Opus-MT models.

option(FRESHGRAM_DISABLE_TRANSLATION "Build without the local translation" OFF)

add_library(lib_translate_local INTERFACE)
add_library(tdesktop::lib_translate_local ALIAS lib_translate_local)

if (NOT FRESHGRAM_DISABLE_RECOGNITION AND NOT FRESHGRAM_DISABLE_TRANSLATION)
    function(freshgram_add_translate)
        set(ct2_dir ${third_party_loc}/CTranslate2)
        set(spm_dir ${third_party_loc}/sentencepiece)

        # CTranslate2 picks the kernels by the host processor, but an x86_64
        # build on an arm64 Mac must get the x86_64 ones.
        if (APPLE AND CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
            set(CMAKE_SYSTEM_PROCESSOR x86_64)
        endif()
        # Windows reports ARM64 in capitals, which its match for arm64 misses.
        if (WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64)$")
            set(CMAKE_SYSTEM_PROCESSOR arm64)
        endif()

        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(CMAKE_SKIP_INSTALL_RULES ON)
        set(CMAKE_POSITION_INDEPENDENT_CODE ON)
        # The old minimum versions of the nested projects are rejected.
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

        set(WITH_MKL OFF CACHE BOOL "" FORCE)
        set(WITH_DNNL OFF CACHE BOOL "" FORCE)
        set(WITH_OPENBLAS OFF CACHE BOOL "" FORCE)
        set(WITH_ACCELERATE OFF CACHE BOOL "" FORCE)
        set(WITH_CUDA OFF CACHE BOOL "" FORCE)
        set(WITH_CUDNN OFF CACHE BOOL "" FORCE)
        set(WITH_HIP OFF CACHE BOOL "" FORCE)
        set(WITH_RUY ON CACHE BOOL "" FORCE)
        set(OPENMP_RUNTIME NONE CACHE STRING "" FORCE)
        set(ENABLE_CPU_DISPATCH ON CACHE BOOL "" FORCE)
        set(ENABLE_PROFILING OFF CACHE BOOL "" FORCE)
        set(BUILD_CLI OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTS OFF CACHE BOOL "" FORCE)

        set(SPM_ENABLE_SHARED OFF CACHE BOOL "" FORCE)
        set(SPM_BUILD_TEST OFF CACHE BOOL "" FORCE)
        set(SPM_ENABLE_TCMALLOC OFF CACHE BOOL "" FORCE)
        set(SPM_PROTOBUF_PROVIDER internal CACHE STRING "" FORCE)
        set(SPM_ABSL_PROVIDER internal CACHE STRING "" FORCE)

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
            ${ct2_dir}
            ${CMAKE_CURRENT_BINARY_DIR}/CTranslate2
            EXCLUDE_FROM_ALL)
        add_subdirectory(
            ${spm_dir}
            ${CMAKE_CURRENT_BINARY_DIR}/sentencepiece
            EXCLUDE_FROM_ALL)
        if (COMMAND cmake_diagnostic)
            cmake_diagnostic(POP)
        elseif (was_suppressed STREQUAL "")
            unset(CMAKE_SUPPRESS_DEVELOPER_ERRORS CACHE)
        else()
            set(CMAKE_SUPPRESS_DEVELOPER_ERRORS ${was_suppressed} CACHE BOOL "" FORCE)
        endif()
    endfunction()
    freshgram_add_translate()

    # The headers of sentencepiece bring their own small "absl", so they
    # must be visible only to the one source file that includes them.
    set_source_files_properties(
        ${src_loc}/ayu/features/recognition/text_translation.cpp
        PROPERTIES INCLUDE_DIRECTORIES
        "${third_party_loc}/sentencepiece/src;${third_party_loc}/sentencepiece/third_party")
    # fastText detects the language of a text. Its headers have names like
    # "model.h" and "args.h", so they are visible to one source file only.
    set(ft_dir ${third_party_loc}/fastText/src)
    add_library(freshgram_fasttext STATIC
        ${ft_dir}/args.cc
        ${ft_dir}/autotune.cc
        ${ft_dir}/densematrix.cc
        ${ft_dir}/dictionary.cc
        ${ft_dir}/fasttext.cc
        ${ft_dir}/loss.cc
        ${ft_dir}/matrix.cc
        ${ft_dir}/meter.cc
        ${ft_dir}/model.cc
        ${ft_dir}/productquantizer.cc
        ${ft_dir}/quantmatrix.cc
        ${ft_dir}/utils.cc
        ${ft_dir}/vector.cc)
    set_target_properties(freshgram_fasttext PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
    target_compile_features(freshgram_fasttext PUBLIC cxx_std_17)
    find_package(Threads REQUIRED)
    target_link_libraries(freshgram_fasttext PRIVATE Threads::Threads)
    if (MSVC)
        target_compile_definitions(freshgram_fasttext
        PRIVATE
            _CRT_SECURE_NO_WARNINGS
            _USE_MATH_DEFINES
            NOMINMAX)
        target_compile_options(freshgram_fasttext PRIVATE /w /FIcstdint)
    else()
        target_compile_options(freshgram_fasttext PRIVATE -w -include cstdint)
    endif()
    set_source_files_properties(
        ${src_loc}/ayu/features/recognition/language_id.cpp
        PROPERTIES INCLUDE_DIRECTORIES "${ft_dir}")
    target_link_libraries(lib_translate_local INTERFACE ctranslate2 sentencepiece-static freshgram_fasttext)
    target_compile_definitions(lib_translate_local INTERFACE FRESHGRAM_LOCAL_TRANSLATE)
endif()
