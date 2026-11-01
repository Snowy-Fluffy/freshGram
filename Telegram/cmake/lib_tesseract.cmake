# This file is part of freshGram.
#
# Tesseract and Leptonica for the local text recognition. They are built
# from the sources here, without the CMake projects of the libraries,
# so no image libraries, archives, curl or ScrollView are involved.

add_library(lib_tesseract INTERFACE)
add_library(tdesktop::lib_tesseract ALIAS lib_tesseract)

if (NOT FRESHGRAM_DISABLE_RECOGNITION)
    set(lept_dir ${third_party_loc}/leptonica)
    set(tess_dir ${third_party_loc}/tesseract)
    set(ocr_gen ${CMAKE_CURRENT_BINARY_DIR}/ocr_generated)

    #
    # Leptonica.
    #

    file(GLOB lept_sources ${lept_dir}/src/*.c)
    file(GLOB lept_headers ${lept_dir}/src/*.h)

    # Tesseract includes <leptonica/allheaders.h>.
    foreach (header ${lept_headers})
        get_filename_component(header_name ${header} NAME)
        configure_file(
            ${header}
            ${ocr_gen}/leptonica/${header_name}
            COPYONLY)
    endforeach()
    file(WRITE ${ocr_gen}/leptonica/endianness.h
"#if !defined (L_BIG_ENDIAN) && !defined (L_LITTLE_ENDIAN)
# if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) \\
    && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#  define L_BIG_ENDIAN
# else
#  define L_LITTLE_ENDIAN
# endif
#endif
")

    add_library(freshgram_leptonica STATIC ${lept_sources})
    set_target_properties(freshgram_leptonica PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        C_STANDARD 11)
    target_include_directories(freshgram_leptonica
    PRIVATE
        ${lept_dir}/src
        ${ocr_gen}/leptonica
    PUBLIC
        ${ocr_gen})
    target_compile_definitions(freshgram_leptonica
    PRIVATE
        HAVE_LIBJPEG=0
        HAVE_LIBTIFF=0
        HAVE_LIBPNG=0
        HAVE_LIBZ=0
        HAVE_LIBGIF=0
        HAVE_LIBUNGIF=0
        HAVE_LIBWEBP=0
        HAVE_LIBWEBP_ANIM=0
        HAVE_LIBJP2K=0)
    if (MSVC)
        target_compile_definitions(freshgram_leptonica PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(freshgram_leptonica PRIVATE /w)
    else()
        target_compile_options(freshgram_leptonica PRIVATE -w)
        target_link_libraries(freshgram_leptonica PRIVATE m)
    endif()

    #
    # Tesseract.
    #

    set(TESSERACT_VERSION_FULL 5.5.3)
    set(GENERIC_MAJOR_VERSION 5)
    set(GENERIC_MINOR_VERSION 5)
    set(GENERIC_MICRO_VERSION 3)
    set(PACKAGE_VERSION ${TESSERACT_VERSION_FULL})
    configure_file(
        ${tess_dir}/include/tesseract/version.h.in
        ${ocr_gen}/tesseract/version.h
        @ONLY)
    file(WRITE ${ocr_gen}/config_auto.h
"#define PACKAGE_VERSION \"${TESSERACT_VERSION_FULL}\"
#define GRAPHICS_DISABLED 1
#define FAST_FLOAT 1
")

    include(${tess_dir}/cmake/SourceLists.cmake)
    set(tess_sources "")
    foreach (source ${TESSERACT_SRC_CORE})
        if (source MATCHES "\\.cpp$")
            list(APPEND tess_sources ${tess_dir}/${source})
        endif()
    endforeach()
    foreach (source ${TESSERACT_SRC_ARCH})
        list(APPEND tess_sources ${tess_dir}/${source})
    endforeach()

    set(tess_arch_defines "")
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x64)$"
        OR (APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "x86_64"))
        if (MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            set(tess_avx_flags /arch:AVX)
            set(tess_avx2_flags /arch:AVX2)
            set(tess_fma_flags /arch:AVX2 -D__FMA__)
            set(tess_sse_flags -D__SSE4_1__)
        else()
            set(tess_avx_flags -mavx)
            set(tess_avx2_flags -mavx2)
            set(tess_fma_flags -mfma)
            set(tess_sse_flags -msse4.1)
        endif()
        set(tess_arch ${tess_dir}/src/arch)
        list(APPEND tess_sources
            ${tess_arch}/dotproductavx.cpp
            ${tess_arch}/intsimdmatrixavx2.cpp
            ${tess_arch}/dotproductfma.cpp
            ${tess_arch}/dotproductsse.cpp
            ${tess_arch}/intsimdmatrixsse.cpp)
        set_source_files_properties(${tess_arch}/dotproductavx.cpp
            PROPERTIES COMPILE_OPTIONS "${tess_avx_flags}")
        set_source_files_properties(${tess_arch}/intsimdmatrixavx2.cpp
            PROPERTIES COMPILE_OPTIONS "${tess_avx2_flags}")
        set_source_files_properties(${tess_arch}/dotproductfma.cpp
            PROPERTIES COMPILE_OPTIONS "${tess_fma_flags}")
        if (tess_sse_flags)
            set_source_files_properties(
                ${tess_arch}/dotproductsse.cpp
                ${tess_arch}/intsimdmatrixsse.cpp
                PROPERTIES COMPILE_OPTIONS "${tess_sse_flags}")
        endif()
        list(APPEND tess_arch_defines
            HAVE_AVX
            HAVE_AVX2
            HAVE_FMA
            HAVE_SSE4_1)
    elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$"
        OR (APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "arm64"))
        # WHY: MSVC does not define __ARM_NEON, so the NEON files compile empty while simddetect still references them.
        if (NOT (MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
            list(APPEND tess_sources
                ${tess_dir}/src/arch/dotproductneon.cpp
                ${tess_dir}/src/arch/intsimdmatrixneon.cpp)
            list(APPEND tess_arch_defines HAVE_NEON)
        endif()
    endif()

    add_library(freshgram_tesseract STATIC ${tess_sources})
    set_target_properties(freshgram_tesseract PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
    target_compile_features(freshgram_tesseract PUBLIC cxx_std_20)
    target_include_directories(freshgram_tesseract
    PRIVATE
        ${tess_dir}/src
        ${tess_dir}/src/arch
        ${tess_dir}/src/ccmain
        ${tess_dir}/src/ccstruct
        ${tess_dir}/src/ccutil
        ${tess_dir}/src/classify
        ${tess_dir}/src/cutil
        ${tess_dir}/src/dict
        ${tess_dir}/src/lstm
        ${tess_dir}/src/textord
        ${tess_dir}/src/viewer
        ${tess_dir}/src/wordrec
        ${tess_dir}/src/training
        ${ocr_gen}
        ${ocr_gen}/leptonica
    PUBLIC
        ${tess_dir}/include
        ${ocr_gen})
    target_compile_definitions(freshgram_tesseract
    PRIVATE
        HAVE_CONFIG_H
        ${tess_arch_defines})
    if (MSVC)
        target_compile_definitions(freshgram_tesseract
        PRIVATE
            _CRT_SECURE_NO_WARNINGS
            _CRT_NONSTDC_NO_DEPRECATE
            _USE_MATH_DEFINES
            NOMINMAX
            _SILENCE_STDEXT_HASH_DEPRECATION_WARNINGS=1
            _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
        target_compile_options(freshgram_tesseract PRIVATE /w /FIcstdint)
    else()
        target_compile_options(freshgram_tesseract PRIVATE -w)
    endif()
    find_package(Threads REQUIRED)
    target_link_libraries(freshgram_tesseract
    PUBLIC
        freshgram_leptonica
    PRIVATE
        Threads::Threads)
    if (WIN32)
        target_link_libraries(freshgram_tesseract PRIVATE ws2_32)
    endif()

    target_link_libraries(lib_tesseract INTERFACE freshgram_tesseract)
    target_compile_definitions(lib_tesseract INTERFACE FRESHGRAM_LOCAL_OCR)
endif()
