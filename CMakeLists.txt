cmake_minimum_required(VERSION 3.16)
project(flintdb CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_library(flintdb_core STATIC
  src/storage/page.cpp
  src/storage/disk_manager.cpp
  src/storage/buffer_pool.cpp
  src/storage/heap_file.cpp
  src/index/btree.cpp
  src/wal/log_manager.cpp
  src/wal/recovery.cpp
  src/txn/transaction_manager.cpp
  src/txn/lock_manager.cpp
  src/txn/transaction.cpp
  src/catalog/catalog.cpp
  src/db/database.cpp
  src/sql/token.cpp
  src/sql/lexer.cpp
  src/sql/parser.cpp
  src/sql/sql_semantics.cpp
  src/sql/planner.cpp
  src/sql/row_codec.cpp
  src/sql/executor.cpp
  src/sql/session.cpp
)
target_include_directories(flintdb_core PUBLIC src)
target_link_libraries(flintdb_core PUBLIC Threads::Threads)

add_executable(flintdb_tests
  tests/test_main.cpp
  tests/test_page.cpp
  tests/test_disk_manager.cpp
  tests/test_buffer_pool.cpp
  tests/test_heap_file.cpp
  tests/test_btree.cpp
  tests/test_log_manager.cpp
  tests/test_recovery.cpp
  tests/test_transaction.cpp
  tests/test_crash_recovery.cpp
  tests/test_lock_manager.cpp
  tests/test_transaction_context.cpp
  tests/test_isolation.cpp
  tests/test_catalog.cpp
  tests/test_database.cpp
  tests/test_lexer.cpp
  tests/test_parser.cpp
  tests/test_planner.cpp
  tests/test_executor.cpp
  tests/test_session.cpp
  tests/test_end_to_end.cpp
  tests/test_storage_corruption.cpp
)
target_link_libraries(flintdb_tests PRIVATE flintdb_core)
target_include_directories(flintdb_tests PRIVATE tests)

add_executable(bench_index_vs_scan
  bench/bench_index_vs_scan.cpp
)
target_link_libraries(bench_index_vs_scan PRIVATE flintdb_core)

add_executable(flintdb_cli
  tools/flintdb_cli.cpp
)
target_link_libraries(flintdb_cli PRIVATE flintdb_core)

enable_testing()
add_test(NAME flintdb_tests COMMAND flintdb_tests)
