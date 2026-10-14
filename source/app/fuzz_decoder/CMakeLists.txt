# Decoder fuzzer — opt-in, NOT included in the root add_subdirectory chain.
#
# To build standalone (no libFuzzer required):
#   cmake -S source/app/fuzz_decoder -B build/fuzz_decoder
#   cmake --build build/fuzz_decoder
#
# Or use build.sh in this directory for direct clang invocations.

cmake_minimum_required(VERSION 3.13)
project(fuzz_decoder C)

set(CMAKE_C_STANDARD 99)

# Locate the in-tree static libs the harness depends on.
# Assumes a prior in-tree build under <repo>/build/.
set(REPO_ROOT      "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set(LIB_ROOT       "${REPO_ROOT}/source/lib")
set(GPR_BUILD "${REPO_ROOT}/build" CACHE PATH "Existing GPR native build")
set(BUILD_LIB_ROOT "${GPR_BUILD}/source/lib")
find_package(Threads REQUIRED)

include_directories(
    "${LIB_ROOT}/vc5_common"
    "${LIB_ROOT}/vc5_encoder"
    "${LIB_ROOT}/vc5_decoder"
    "${LIB_ROOT}/common/public"
    "${LIB_ROOT}/common/private"
)

# Standalone driver: harness + signal-handling main.
add_executable(fuzz_decoder_standalone
    main.c
    standalone.c
)

target_link_directories(fuzz_decoder_standalone PRIVATE
    "${BUILD_LIB_ROOT}/vc5_decoder"
    "${BUILD_LIB_ROOT}/vc5_encoder"
    "${BUILD_LIB_ROOT}/vc5_common"
    "${BUILD_LIB_ROOT}/common"
)

target_link_libraries(fuzz_decoder_standalone
    vc5_decoder
    vc5_encoder
    vc5_common
    common
    Threads::Threads
)
if(NOT WIN32)
  target_link_libraries(fuzz_decoder_standalone m)
endif()

set_target_properties(fuzz_decoder_standalone PROPERTIES FOLDER app)
