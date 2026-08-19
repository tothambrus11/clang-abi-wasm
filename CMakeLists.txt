# clang-abi-wasm — record layout as structured data.
#
# Builds two things from one source file:
#
#   abi_query_test   a native binary, for developing against a system-speed
#                    build. This is where the C++ gets debugged; the wasm link
#                    is mechanical once it compiles here.
#   abi_query.mjs    the Emscripten ES module the web app loads.
#
# Both link the *frontend only*. LLVM_TARGETS_TO_BUILD is empty in the LLVM we
# link against: -fsyntax-only never reaches a backend, and clang's per-target
# ABI knowledge lives in Basic/Targets, which is compiled in regardless. That
# single fact is what makes a small build possible while keeping every target.

cmake_minimum_required(VERSION 3.20)
project(clang_abi_wasm CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

message(STATUS "LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")
message(STATUS "targets built into LLVM: '${LLVM_TARGETS_TO_BUILD}'")

# The frontend libraries, and nothing else. No CodeGen, no MC, no lld, no
# analyzer — see docs/why-frontend-only.md for the measurements behind this.
set(ABI_CLANG_LIBS
  clangFrontend
  clangSerialization
  clangSema
  clangAnalysis
  clangEdit
  clangParse
  clangDriver
  clangAST
  clangLex
  clangBasic
  clangSupport
)

llvm_map_components_to_libnames(ABI_LLVM_LIBS support targetparser binaryformat frontendopenmp option)

add_library(abi_query_core STATIC src/abi_query.cpp)
target_include_directories(abi_query_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${LLVM_INCLUDE_DIRS}
  ${CLANG_INCLUDE_DIRS})
target_compile_definitions(abi_query_core PRIVATE ${LLVM_DEFINITIONS})

# Header search is pinned, not discovered — see the comment in runQuery. The
# wasm build mounts its pack at /usr; the native build borrows the resource
# headers from the LLVM tree it links, so both behave identically and a native
# test predicts the wasm result.
if(EMSCRIPTEN)
  set(ABI_SYSROOT "/usr" CACHE STRING "sysroot the module reads headers from")
  set(ABI_RESOURCE_DIR "/usr/lib/clang/${LLVM_VERSION_MAJOR}" CACHE STRING "")
else()
  set(ABI_SYSROOT "" CACHE STRING "sysroot the native harness reads headers from")
  set(ABI_RESOURCE_DIR "${LLVM_LIBRARY_DIR}/clang/${LLVM_VERSION_MAJOR}" CACHE STRING "")
endif()
target_compile_definitions(abi_query_core PRIVATE
  ABI_DEFAULT_SYSROOT="${ABI_SYSROOT}"
  ABI_DEFAULT_RESOURCE_DIR="${ABI_RESOURCE_DIR}")
message(STATUS "resource dir: ${ABI_RESOURCE_DIR}")
message(STATUS "sysroot     : ${ABI_SYSROOT}")
# LLVM headers assume the flags LLVM itself was built with.
target_compile_options(abi_query_core PRIVATE -fno-rtti -fno-exceptions)
target_link_libraries(abi_query_core PUBLIC ${ABI_CLANG_LIBS} ${ABI_LLVM_LIBS})

if(EMSCRIPTEN)
  add_executable(abi_query src/wasm_main.cpp)
  target_link_libraries(abi_query PRIVATE abi_query_core)
  set_target_properties(abi_query PROPERTIES OUTPUT_NAME "abi_query" SUFFIX ".mjs")
  target_link_options(abi_query PRIVATE
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sEXPORT_NAME=createAbiModule
    -sENVIRONMENT=web,worker,node
    -sALLOW_MEMORY_GROWTH=1
    # clang's recursive descent parser wants a real stack.
    -sSTACK_SIZE=8MB
    -sEXPORTED_FUNCTIONS=_abi_query,_abi_version,_malloc,_free
    -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToNewUTF8,FS
    # The header pack is mounted at /usr/include; see scripts/package-headers.sh.
    --preload-file ${CMAKE_CURRENT_BINARY_DIR}/sysroot@/usr
    -O3
  )
else()
  # Native harness: same code path, run from a terminal in milliseconds.
  add_executable(abi_query_test test/main.cpp)
  target_link_libraries(abi_query_test PRIVATE abi_query_core)
endif()
