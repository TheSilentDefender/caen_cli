cmake_minimum_required(VERSION 3.14)

project(caen_cli
    VERSION 0.1
    DESCRIPTION "CAEN digitiser oscilloscope frontend"
    LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not supported.\n"
        "Please configure with:  cmake -S . -B build")
endif()


set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)


if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "No build type specified — defaulting to Release")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()


set(_ARCH_FLAGS "-march=x86-64 -mtune=generic")
set(_OPT_FLAGS  "-O3 -fomit-frame-pointer -ffast-math -funroll-loops")
set(_DBG_FLAGS  "-O0 -g3 -fno-omit-frame-pointer")

set(CMAKE_C_FLAGS_RELEASE          "${_ARCH_FLAGS} ${_OPT_FLAGS}"         CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE        "${_ARCH_FLAGS} ${_OPT_FLAGS}"         CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_RELWITHDEBINFO   "${_ARCH_FLAGS} ${_OPT_FLAGS} -g"      CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${_ARCH_FLAGS} ${_OPT_FLAGS} -g"      CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_DEBUG            "${_ARCH_FLAGS} ${_DBG_FLAGS}"         CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_DEBUG          "${_ARCH_FLAGS} ${_DBG_FLAGS}"         CACHE STRING "" FORCE)


option(CAEN_SCOPE_FULLY_STATIC      "Statically link libstdc++ / libgcc"  OFF)
option(CAEN_SCOPE_ADD_DISTCLEAN     "Add a distclean convenience target"   ON)
option(CAEN_SCOPE_STRIP_ISA_NOTES   "Strip .note.gnu.property after build" ON)
option(CAEN_SCOPE_ENABLE_SANITIZERS "Enable ASan + UBSan in Debug builds"  OFF)


set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build all libraries as static" FORCE)


if(CAEN_SCOPE_ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "Sanitisers enabled: AddressSanitizer + UBSan")
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all)
    add_link_options   (-fsanitize=address,undefined)
endif()

include(ExternalProject)

ExternalProject_Add(caen_felib_ext
    GIT_REPOSITORY https://github.com/caenspa/caen-felib.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE

    PATCH_COMMAND
        ${CMAKE_COMMAND} -E touch
            <SOURCE_DIR>/aclocal.m4
            <SOURCE_DIR>/configure
            <SOURCE_DIR>/Makefile.in
            <SOURCE_DIR>/src/Makefile.in
            <SOURCE_DIR>/config.h.in
    CONFIGURE_COMMAND
        <SOURCE_DIR>/configure
            --prefix=<INSTALL_DIR>
            --enable-shared
            --disable-static
            "CFLAGS=${_ARCH_FLAGS} ${_OPT_FLAGS}"
    BUILD_COMMAND   make -j
    INSTALL_COMMAND make install
    BUILD_IN_SOURCE FALSE
)

ExternalProject_Get_Property(caen_felib_ext INSTALL_DIR)
set(CAEN_FELIB_INCLUDE_DIR "${INSTALL_DIR}/include")
set(CAEN_FELIB_LIBRARY     "${INSTALL_DIR}/lib/libCAEN_FELib.so")


file(MAKE_DIRECTORY "${CAEN_FELIB_INCLUDE_DIR}")

add_library(CAEN::FELib SHARED IMPORTED)
set_target_properties(CAEN::FELib PROPERTIES
    IMPORTED_LOCATION             "${CAEN_FELIB_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${CAEN_FELIB_INCLUDE_DIR}"
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${CAEN_FELIB_INCLUDE_DIR}")
add_dependencies(CAEN::FELib caen_felib_ext)

include(FetchContent)

find_package(CLI11 QUIET)
if(NOT CLI11_FOUND)
    FetchContent_Declare(CLI11
        GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
        GIT_TAG        v2.4.2
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(CLI11)
endif()

find_package(ftxui QUIET)
if(NOT ftxui_FOUND)
    FetchContent_Declare(ftxui
        GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
        GIT_TAG        v5.0.0
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(ftxui)
endif()

set(FMT_DOC     OFF CACHE BOOL "" FORCE)
set(FMT_TEST    OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        10.2.1
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(fmt)

set(SPDLOG_BUILD_SHARED  OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL  ON  CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH   OFF CACHE BOOL "" FORCE)
set(SPDLOG_COMPILED_LIB  ON  CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.14.1
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(spdlog)

if(NOT TARGET spdlog::spdlog)
    message(FATAL_ERROR "spdlog::spdlog target not found after FetchContent_MakeAvailable")
endif()

find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(tomlplusplus)

add_executable(caen_cli
    main.cpp
    CaenScope.cpp
    Acquisition.cpp
    AcquisitionSetup.cpp
    AcquisitionLoop.cpp
    SavingLoop.cpp
)

target_include_directories(caen_cli PRIVATE
    ${ftxui_SOURCE_DIR}/include
    ${ftxui_BINARY_DIR}/include
    ${CAEN_FELIB_INCLUDE_DIR}
)

target_compile_options(caen_cli PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wno-error=pedantic>
)

target_link_libraries(caen_cli PRIVATE
    CAEN::FELib
    CLI11::CLI11
    ftxui::screen
    ftxui::dom
    spdlog::spdlog
    fmt::fmt
    nlohmann_json::nlohmann_json
    tomlplusplus::tomlplusplus
)

if(CAEN_SCOPE_FULLY_STATIC)
    target_link_options(caen_cli PRIVATE -static-libstdc++ -static-libgcc)
endif()


if(CAEN_SCOPE_STRIP_ISA_NOTES)
    add_custom_command(TARGET caen_cli POST_BUILD
        COMMAND objcopy --remove-section .note.gnu.property $<TARGET_FILE:caen_cli>
        COMMENT "Stripping .note.gnu.property for portability"
    )
endif()


if(CAEN_SCOPE_ADD_DISTCLEAN)
    add_custom_target(distclean
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "${CMAKE_BINARY_DIR}/CMakeFiles"
            "${CMAKE_BINARY_DIR}/CMakeCache.txt"
            "${CMAKE_BINARY_DIR}/cmake_install.cmake"
            "${CMAKE_BINARY_DIR}/Makefile"
            "${CMAKE_BINARY_DIR}/build.ninja"
            "${CMAKE_BINARY_DIR}/.ninja_deps"
            "${CMAKE_BINARY_DIR}/.ninja_log"
            "${CMAKE_BINARY_DIR}/caen_cli"
        COMMENT "distclean: removing CMake-generated build files"
    )
endif()