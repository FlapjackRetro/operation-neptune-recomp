cmake_minimum_required(VERSION 3.16)
project(OperationNeptuneRecomp C)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT MSVC)
  add_compile_options(-w -fno-strict-aliasing)   # generated code is warning-noisy
endif()

# gen/ is written by tools/lift_full.py (git-ignored)
file(GLOB GENERATED gen/recomp_*.c)
if(NOT GENERATED)
  message(FATAL_ERROR "gen/ is empty: run tools/lift_full.py first")
endif()

add_executable(nep src/main.c src/recomp/cpu.c src/recomp/runtime16.c src/recomp/overrides.c src/recomp/ega.c ${GENERATED})
target_compile_definitions(nep PRIVATE RECOMP_IRQ RECOMP_MEM_HOOK RECOMP_TRACE)
target_include_directories(nep PRIVATE src/recomp gen)
find_package(Threads REQUIRED)
target_link_libraries(nep PRIVATE Threads::Threads m)
set_target_properties(nep PROPERTIES ENABLE_EXPORTS ON)   # symbol names in watchdog backtraces

enable_testing()
add_executable(test_x87 test/test_x87.c)
target_link_libraries(test_x87 PRIVATE m)
add_test(NAME x87 COMMAND test_x87)
