cmake_minimum_required(VERSION 3.20)
project(reincomp_browser_tools LANGUAGES C CXX)
if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "Configure with emcmake and a WebAssembly build of LLVM 18")
endif()
find_package(LLVM 18.1.8 EXACT REQUIRED CONFIG)
find_package(LLD REQUIRED CONFIG HINTS "${LLVM_DIR}/../lld")
find_package(gflags REQUIRED)
find_package(glog REQUIRED)
include("${GENERATED}/sources.cmake")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-fexceptions)
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

function(browser_tool name)
  target_sources(${name} PRIVATE browser-process.cpp)
  set_target_properties(${name} PROPERTIES SUFFIX ".mjs")
  target_link_options(${name} PRIVATE -fexceptions -sMODULARIZE=1 -sEXPORT_ES6=1
    -sENVIRONMENT=worker -sINVOKE_RUN=0 -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1
    -sMAXIMUM_MEMORY=4294967296 -sSTACK_SIZE=8388608 -sFORCE_FILESYSTEM=1
    "-sEXPORTED_RUNTIME_METHODS=['FS','callMain','WORKERFS']" -lworkerfs.js)
endfunction()

llvm_map_components_to_libnames(lift_libs core irreader bitreader bitwriter linker passes transformutils support)
find_program(HOST_PYTHON python3 REQUIRED NO_CMAKE_FIND_ROOT_PATH)
set(lift_source "${REMILL_ROOT}/bin/lift/Lift.cpp")
set(browser_lift "${CMAKE_CURRENT_BINARY_DIR}/BrowserLift.cpp")
add_custom_command(OUTPUT "${browser_lift}"
  COMMAND "${HOST_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_web_io.py" "${lift_source}" "${browser_lift}"
  DEPENDS "${lift_source}" prepare_web_io.py VERBATIM)
list(REMOVE_ITEM LIFTER_SOURCES "${lift_source}")
list(APPEND LIFTER_SOURCES "${browser_lift}")
set_source_files_properties("${browser_lift}" PROPERTIES INCLUDE_DIRECTORIES "${REMILL_ROOT}/bin/lift")
add_executable(lift ${LIFTER_SOURCES})
target_include_directories(lift PRIVATE "${REMILL_ROOT}/include" "${REMILL_ROOT}" "${GENERATED}/include" "${CMAKE_CURRENT_SOURCE_DIR}")
# Remill's CLI uses these as defaults when a caller omits --arch and --os.
# The browser tool always selects the guest explicitly.
target_compile_definitions(lift PRIVATE REMILL_INSTALL_SEMANTICS_DIR="" REMILL_ARCH="aarch64" REMILL_OS="linux")
foreach(arch X86 AARCH32 AARCH64 SPARC32 SPARC64 PPC64_32ADDR)
  target_compile_definitions(lift PRIVATE REMILL_BUILD_SEMANTICS_DIR_${arch}="")
endforeach()
target_link_libraries(lift PRIVATE glog::glog ${GFLAGS_LIBRARIES} ${lift_libs})
browser_tool(lift)

add_executable(poll ../poll_instrument.cpp)
target_link_libraries(poll PRIVATE LLVMIRReader LLVMCore)
browser_tool(poll)

add_executable(coff coff.cpp)
target_link_libraries(coff PRIVATE LLVMX86CodeGen LLVMX86AsmParser LLVMIRReader LLVMPasses)
browser_tool(coff)

add_executable(link link.cpp)
target_include_directories(link PRIVATE ${LLD_INCLUDE_DIRS})
target_link_libraries(link PRIVATE lldMinGW lldCOFF)
browser_tool(link)
