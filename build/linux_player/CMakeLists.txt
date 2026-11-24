cmake_minimum_required(VERSION 3.18)
project(LinuxGameAssembly LANGUAGES C CXX ASM)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
set(root "${CMAKE_CURRENT_SOURCE_DIR}/../..")
file(STRINGS "${GENERATED}/objects.txt" objects)
add_library(GameAssembly SHARED api.S asset_bundles.cpp engine_bridge.cpp firebase_linux.cpp game_assembly.cpp game_traces.S movie_player.cpp native_gc.cpp octo_download.cpp
    "${root}/runtime/elfload.c" "${root}/runtime/runtime.cpp" "${root}/runtime/cooperative_poll.cpp"
    "${root}/runtime/format.cpp" "${root}/host/libc.cpp" "${root}/host/logging.cpp"
    "${root}/host/signal_host.cpp" "${root}/host/semaphore_host.cpp" "${root}/host/nonlocal_jmp.cpp"
    "${root}/host/signal_delivery.cpp" "${root}/runtime/signal_frame.cpp"
    "${root}/host/module_registry.cpp" "${root}/host/module_registry_elf.cpp" "${root}/host/firebase_app.cpp" ${objects})
target_compile_features(GameAssembly PRIVATE cxx_std_17)
target_compile_options(GameAssembly PRIVATE -O2 -fvisibility=hidden)
target_compile_definitions(GameAssembly PRIVATE ADDRESS_SIZE_BITS=64)
target_include_directories(GameAssembly PRIVATE "${GENERATED}" "${root}/runtime" "${root}/host" "${root}/third_party/remill-include")
target_link_libraries(GameAssembly PRIVATE pthread dl atomic OpenSSL::Crypto)
target_link_options(GameAssembly PRIVATE -Wl,-z,defs -Wl,--wrap=sub_825aad50 "-Wl,--version-script=${GENERATED}/exports.map")
set_target_properties(GameAssembly PROPERTIES PREFIX "" LINK_DEPENDS "${GENERATED}/exports.map")
