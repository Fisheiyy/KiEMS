cmake_minimum_required(VERSION 3.25)

project(KiEMS LANGUAGES CXX)

option(KIEMS_BUILD_TESTS "Build the libkiems test bundle" ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

if(APPLE AND EXISTS "/opt/homebrew")
    list(PREPEND CMAKE_PREFIX_PATH "/opt/homebrew")
endif()

if(MSVC AND MSVC_VERSION LESS 1936)
    message(FATAL_ERROR "KiEMS needs Visual Studio 2022 17.6 (MSVC 19.36) or newer for its C++23 library features.")
endif()

# Windows toolchain settings, applied before the submodules are added so they inherit them. They
# match what KiCad's own Windows build uses, so code compiled against KiCad's (and CSXCAD's)
# headers sees the same environment. Nothing here applies on macOS or Linux.
if(MSVC)
    add_compile_options(/utf-8 /bigobj /Zc:__cplusplus)
    add_compile_definitions(NOMINMAX _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
elseif(MINGW)
    add_compile_options(-Wa,-mbig-obj)
    add_compile_definitions(_USE_MATH_DEFINES)
endif()

# Windows has no RPATH: an executable finds its DLLs next to itself. Put every binary in one
# directory so the CLI, its sibling workers and their DLLs run straight from the build tree.
if(WIN32)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

find_package(nlohmann_json REQUIRED)
find_package(GEOS REQUIRED)
find_package(Matplot++ QUIET CONFIG)
if(NOT Matplot++_FOUND)
    message(FATAL_ERROR
        "KiEMS requires Matplot++, but CMake could not find it.\n"
        "\n"
        "On Windows, configure with a windows-msvc-* preset: vcpkg then installs Matplot++ from the "
        "root vcpkg.json.\n"
        "\n"
        "Ubuntu does not package Matplot++ consistently. Install it from source, then rerun CMake:\n"
        "  git clone --depth 1 https://github.com/alandefreitas/matplotplusplus.git\n"
        "  cmake -S matplotplusplus -B matplotplusplus/build -DCMAKE_BUILD_TYPE=Release "
        "-DMATPLOTPP_BUILD_EXAMPLES=OFF -DMATPLOTPP_BUILD_TESTS=OFF\n"
        "  cmake --build matplotplusplus/build --parallel\n"
        "  sudo cmake --install matplotplusplus/build\n"
        "\n"
        "If you install it outside /usr/local, configure KiEMS with:\n"
        "  cmake --preset linux-debug -DMatplot++_DIR=/path/to/lib/cmake/Matplot++\n")
endif()

# These two subprojects provide the same public libraries that the Xcode build consumes.  Keep
# their build trees separate from the root build so each can also be built independently.
add_subdirectory(submodules/Copper)
add_subdirectory(submodules/libkicad)

file(GLOB LIBKIEMS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/libkiems/kiems/*.cpp")
add_library(libkiems STATIC ${LIBKIEMS_SOURCES})
add_library(kiems::libkiems ALIAS libkiems)
target_include_directories(libkiems
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libkiems>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/submodules/Copper/submodules/CSXCAD/src>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/submodules"
        "${CMAKE_CURRENT_SOURCE_DIR}/submodules/libkicad"
        "${CMAKE_CURRENT_SOURCE_DIR}/submodules/Copper/Copper"
        "${CMAKE_CURRENT_SOURCE_DIR}/submodules/Copper/CopperUtils")
target_link_libraries(libkiems
    PUBLIC
        libkicad
        CSXCAD
        CopperUtils
        nlohmann_json::nlohmann_json
        GEOS::geos_c
        Matplot++::matplot)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC emits an ABI-transition advisory for ordinary std::pair use; it does not indicate an
    # issue in this single-toolchain build and otherwise obscures actionable diagnostics.
    target_compile_options(libkiems PRIVATE -Wno-psabi)
endif()

if(APPLE)
    find_library(ACCELERATE_FRAMEWORK Accelerate REQUIRED)
    target_link_libraries(libkiems PRIVATE ${ACCELERATE_FRAMEWORK})
endif()

function(add_kiems_tool target source)
    add_executable(${target} "${source}")
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/libkiems"
        "${CMAKE_CURRENT_SOURCE_DIR}/submodules/Copper/Copper")
    target_link_libraries(${target} PRIVATE libkiems Copper libkicad)
    if(APPLE)
        set_target_properties(${target} PROPERTIES INSTALL_RPATH "@loader_path")
    elseif(WIN32)
        if(MSVC)
            # UTF-8 process code page and long-path awareness; see the manifest's own comment.
            target_sources(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows/kiems.manifest")
        endif()
        # Copy the DLLs of imported dependencies that vcpkg's own app-local deployment doesn't
        # cover (KiCad's kicommon, kigal, kiapi and kicad_3dsg) next to the executable.
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E $<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:${target}>>,copy_if_different,true>
                    $<TARGET_RUNTIME_DLLS:${target}> "$<TARGET_FILE_DIR:${target}>"
            COMMAND_EXPAND_LISTS)
    else()
        set_target_properties(${target} PROPERTIES INSTALL_RPATH "$ORIGIN")
    endif()
endfunction()

add_kiems_tool(kiems-cli "${CMAKE_CURRENT_SOURCE_DIR}/kiems-cli/main.cpp")
set_target_properties(kiems-cli PROPERTIES OUTPUT_NAME kiems)
if(WIN32)
    target_sources(kiems-cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/kiems-cli/platform_win32.cpp")
endif()
add_kiems_tool(kiems_fdtd_worker "${CMAKE_CURRENT_SOURCE_DIR}/kiems_fdtd_worker/main.cpp")
add_kiems_tool(copper_fdtd_worker "${CMAKE_CURRENT_SOURCE_DIR}/copper_fdtd_worker/main.cpp")

if(KIEMS_BUILD_TESTS)
    if(APPLE)
        enable_language(OBJCXX)
        enable_testing()
        find_library(XCTEST_FRAMEWORK XCTest REQUIRED)
        add_library(libkiems_tests MODULE "${CMAKE_CURRENT_SOURCE_DIR}/libkiems_tests/libkiems_tests.mm")
        set_target_properties(libkiems_tests PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION xctest)
        target_include_directories(libkiems_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/libkiems"
            "${CMAKE_CURRENT_SOURCE_DIR}/submodules/Copper/CopperUtils")
        target_link_libraries(libkiems_tests PRIVATE libkiems libkicad CopperUtils ${XCTEST_FRAMEWORK})
        add_test(NAME libkiems_tests COMMAND xcrun xctest "$<TARGET_BUNDLE_DIR:libkiems_tests>")
    else()
        add_custom_target(libkiems_tests
            COMMAND "${CMAKE_COMMAND}" -E echo "libkiems_tests uses XCTest and is available on macOS only")
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS libkiems kiems-cli kiems_fdtd_worker copper_fdtd_worker
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(WIN32)
    # The executables load their DLLs from their own directory. vcpkg installs its packages' DLLs
    # (X_VCPKG_APPLOCAL_DEPS_INSTALL in the Windows presets); install KiCad's alongside them.
    get_property(KIEMS_LIBKICAD_RUNTIME_DLLS GLOBAL PROPERTY LIBKICAD_RUNTIME_DLLS)
    if(KIEMS_LIBKICAD_RUNTIME_DLLS)
        install(FILES ${KIEMS_LIBKICAD_RUNTIME_DLLS} DESTINATION "${CMAKE_INSTALL_BINDIR}")
    endif()
endif()
