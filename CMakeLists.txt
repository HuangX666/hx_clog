cmake_minimum_required(VERSION 3.16)

project(hx_clog
    VERSION 1.1.0
    DESCRIPTION "A portable C/C++11 logging framework"
    LANGUAGES C CXX
)

# ---------------------------------------------------------------------------
# Source / execution charset
#
# All source files in this repository are UTF-8 without BOM. Tell MSVC to treat
# both the source and execution character sets as UTF-8 so non-ASCII content
# (and the C4819 codepage warning) is handled correctly regardless of the
# system locale. Applies to every target defined below.
# ---------------------------------------------------------------------------
if(MSVC)
    add_compile_options(/utf-8)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(HX_CLOG_BUILD_SHARED   "Build hx_clog as shared library" OFF)
option(HX_CLOG_BUILD_EXAMPLES "Build examples" ON)
option(HX_CLOG_BUILD_TESTS    "Build tests" ON)
option(HX_CLOG_ENABLE_CPP11   "Enable C++11 wrapper" ON)
option(HX_CLOG_ENABLE_ASYNC   "Enable async logging" ON)
option(HX_CLOG_ENABLE_CRASH   "Enable crash handler" ON)
option(HX_CLOG_ENABLE_COLOR   "Enable colored console output" ON)
option(HX_CLOG_ENABLE_SYSLOG  "Enable syslog sink on Unix/macOS" OFF)
option(HX_CLOG_ENABLE_MINIDUMP "Enable minidump on Windows" ON)
option(HX_CLOG_ENABLE_ZLIB "Enable zlib compression for old rotated backups" ON)

# Per-line size cap. By default a single log line is capped at 512 KB in both
# sync and async modes. Turn this option ON to remove the cap entirely (a line
# may then grow to whatever the process can allocate).
option(HX_CLOG_UNLIMITED_LINE "Remove the per-line size cap (memory-bound)" OFF)
# Optional explicit cap in bytes (ignored when HX_CLOG_UNLIMITED_LINE is ON).
set(HX_CLOG_MAX_LINE_BYTES "" CACHE STRING "Override max log line size in bytes")

# crash diagnostics build options
option(HX_CLOG_ENABLE_STACKTRACE   "Enable crash stacktrace capture" ON)
option(HX_CLOG_ENABLE_SYMBOLIZE    "Enable address symbolization" ON)
option(HX_CLOG_KEEP_FRAME_POINTER  "Keep frame pointer for better stacktrace" ON)

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(HX_CLOG_SOURCES
    src/hx_clog_core.c
    src/hx_clog_format.c
    src/hx_clog_sink.c
    src/hx_clog_file.c
    src/hx_clog_rotate.c
)

if(HX_CLOG_ENABLE_ASYNC)
    list(APPEND HX_CLOG_SOURCES src/hx_clog_async.c)
endif()

if(HX_CLOG_ENABLE_CRASH)
    list(APPEND HX_CLOG_SOURCES src/hx_clog_crash.c)
endif()

# ---------------------------------------------------------------------------
# Library target
# ---------------------------------------------------------------------------
if(HX_CLOG_BUILD_SHARED)
    add_library(hx_clog SHARED ${HX_CLOG_SOURCES})
    target_compile_definitions(hx_clog
        PUBLIC  HX_CLOG_BUILD_SHARED=1
        PRIVATE HX_CLOG_EXPORTS=1)
else()
    add_library(hx_clog STATIC ${HX_CLOG_SOURCES})
endif()

add_library(hx_clog::hx_clog ALIAS hx_clog)

target_include_directories(hx_clog
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(hx_clog PUBLIC c_std_99)

if(HX_CLOG_ENABLE_CPP11)
    target_compile_features(hx_clog PUBLIC cxx_std_11)
    target_compile_definitions(hx_clog PUBLIC HX_CLOG_ENABLE_CPP11=1)
endif()

if(HX_CLOG_ENABLE_ASYNC)
    target_compile_definitions(hx_clog PUBLIC HX_CLOG_ENABLE_ASYNC=1)
endif()

if(HX_CLOG_ENABLE_CRASH)
    target_compile_definitions(hx_clog PUBLIC HX_CLOG_ENABLE_CRASH=1)
endif()

if(HX_CLOG_ENABLE_ZLIB)
    find_package(ZLIB QUIET)
    if(ZLIB_FOUND)
        target_compile_definitions(hx_clog PUBLIC HX_CLOG_ENABLE_ZLIB=1)
        target_link_libraries(hx_clog PRIVATE ZLIB::ZLIB)
    else()
        message(WARNING "HX_CLOG_ENABLE_ZLIB=ON but zlib was not found; old backups will be deleted instead of compressed")
    endif()
endif()

if(HX_CLOG_UNLIMITED_LINE)
    target_compile_definitions(hx_clog PUBLIC HX_CLOG_UNLIMITED_LINE=1)
elseif(HX_CLOG_MAX_LINE_BYTES)
    target_compile_definitions(hx_clog PUBLIC HX_CLOG_MAX_LINE=${HX_CLOG_MAX_LINE_BYTES})
endif()

# These definitions gate real code paths: when COLOR is off the console sink
# ignores enable_color; STACKTRACE/SYMBOLIZE/MINIDUMP compile the matching
# crash-report sections out and set the crash config defaults accordingly.
if(HX_CLOG_ENABLE_COLOR)
    target_compile_definitions(hx_clog PRIVATE HX_CLOG_ENABLE_COLOR=1)
endif()

if(HX_CLOG_ENABLE_STACKTRACE)
    target_compile_definitions(hx_clog PRIVATE HX_CLOG_ENABLE_STACKTRACE=1)
endif()
if(HX_CLOG_ENABLE_SYMBOLIZE)
    target_compile_definitions(hx_clog PRIVATE HX_CLOG_ENABLE_SYMBOLIZE=1)
endif()

# Keep frame pointers for better stack traces on GCC/Clang
if(HX_CLOG_KEEP_FRAME_POINTER AND (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang"))
    target_compile_options(hx_clog PUBLIC -fno-omit-frame-pointer)
endif()

if(MSVC)
    target_compile_options(hx_clog PRIVATE /Zi)
    target_link_options(hx_clog PUBLIC /DEBUG)
endif()

# ---------------------------------------------------------------------------
# Platform definitions and libraries
# ---------------------------------------------------------------------------
# (platform detection itself lives in src/hx_clog_internal.h via _WIN32 /
# __APPLE__ / __ANDROID__ / __linux__; no platform macro is injected here)
if(WIN32)
    target_link_libraries(hx_clog PRIVATE dbghelp advapi32)
    if(HX_CLOG_ENABLE_MINIDUMP)
        target_compile_definitions(hx_clog PRIVATE HX_CLOG_ENABLE_MINIDUMP=1)
    endif()
elseif(ANDROID)
    target_compile_definitions(hx_clog PRIVATE _GNU_SOURCE)
    find_package(Threads REQUIRED)
    target_link_libraries(hx_clog PUBLIC Threads::Threads)
    target_link_libraries(hx_clog PRIVATE log ${CMAKE_DL_LIBS})
elseif(APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(hx_clog PUBLIC Threads::Threads)
    if(HX_CLOG_ENABLE_SYSLOG)
        # the sink code accepts syslog on Apple too; previously only the
        # plain-UNIX branch defined this, so macOS silently lost the sink
        target_compile_definitions(hx_clog PRIVATE HX_CLOG_ENABLE_SYSLOG=1)
    endif()
elseif(UNIX)
    target_compile_definitions(hx_clog PRIVATE _GNU_SOURCE)
    find_package(Threads REQUIRED)
    target_link_libraries(hx_clog PUBLIC Threads::Threads)
    if(HX_CLOG_ENABLE_SYSLOG)
        target_compile_definitions(hx_clog PRIVATE HX_CLOG_ENABLE_SYSLOG=1)
    endif()
    # needed for dladdr / backtrace symbol export on some toolchains
    target_link_libraries(hx_clog PRIVATE ${CMAKE_DL_LIBS})
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(hx_clog PUBLIC -rdynamic)
    endif()
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(HX_CLOG_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(HX_CLOG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS hx_clog
    EXPORT hx_clogTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES include/hx_clog.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(HX_CLOG_ENABLE_CPP11)
    install(FILES include/hx_clog_cpp.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

install(EXPORT hx_clogTargets
    FILE hx_clogTargets.cmake
    NAMESPACE hx_clog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hx_clog
)

# pkg-config file for non-CMake consumers
if(NOT WIN32)
    set(HX_CLOG_PC_LIBS_PRIVATE "-lpthread")
    if(NOT APPLE)
        string(APPEND HX_CLOG_PC_LIBS_PRIVATE " -ldl")
    endif()
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/hx_clog.pc.in
        ${CMAKE_CURRENT_BINARY_DIR}/hx_clog.pc
        @ONLY
    )
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/hx_clog.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    )
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/hx_clogConfig.cmake.in)
    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/hx_clogConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/hx_clogConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hx_clog
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/hx_clogConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/hx_clogConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/hx_clogConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hx_clog
    )
endif()
