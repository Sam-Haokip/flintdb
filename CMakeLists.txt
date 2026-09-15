cmake_minimum_required(VERSION 3.16)
project(flintdb CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra)

add_library(flintdb_core STATIC
  src/storage/page.cpp
  src/storage/disk_manager.cpp
  src/storage/buffer_pool.cpp
  src/storage/heap_file.cpp
  src/index/btree.cpp
)
target_include_directories(flintdb_core PUBLIC src)

add_executable(flintdb_tests
  tests/test_main.cpp
  tests/test_page.cpp
  tests/test_disk_manager.cpp
  tests/test_buffer_pool.cpp
  tests/test_heap_file.cpp
  tests/test_btree.cpp
)
target_link_libraries(flintdb_tests PRIVATE flintdb_core)
target_include_directories(flintdb_tests PRIVATE tests)

add_executable(bench_index_vs_scan
  bench/bench_index_vs_scan.cpp
)
target_link_libraries(bench_index_vs_scan PRIVATE flintdb_core)

enable_testing()
add_test(NAME flintdb_tests COMMAND flintdb_tests)
