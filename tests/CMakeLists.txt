#
# AnnPocketDb :: tests/CMakeLists.txt
#
# Minimal, dependency-free regression suite (see tests/test_common.hpp).
# Each test is its own executable with its own main(); CTest just runs the
# binary and checks the exit code (0 == all CHECKs passed).
#
set(ANNPOCKETDB_TEST_SOURCES
  test_basic_kv.cpp
  test_mvcc.cpp
  test_nested_txn.cpp
  test_append.cpp
  test_2pc.cpp
)

if(ANNPOCKETDB_WITH_SUBDATABASE)
  list(APPEND ANNPOCKETDB_TEST_SOURCES test_subdatabase.cpp test_subdb_flags.cpp test_status_codes.cpp)
endif()

# DUPSORT/DUPFIXED live in the kernel (btree/txn), no feature flag needed.
list(APPEND ANNPOCKETDB_TEST_SOURCES test_dupsort.cpp test_subtree_escalation.cpp)

# Cursor count()/replace()/pop()/getMulti()/putMulti()/seekRangeDup() live in
# the kernel (btree/txn), no feature flag needed.
list(APPEND ANNPOCKETDB_TEST_SOURCES test_cursor_extras.cpp)

# util::PageIdList lives in the kernel (util/), no feature flag needed.
list(APPEND ANNPOCKETDB_TEST_SOURCES test_page_id_list.cpp)

# document has no ANNPOCKETDB_WITH_DOCUMENT gate -- always built (see the
# top-level CMakeLists.txt comment: index.cpp and Environment::documentStore()
# both depend on it unconditionally).
list(APPEND ANNPOCKETDB_TEST_SOURCES test_document.cpp)

# Kernel-level smoke tests with no PageManager/Environment involved -- no
# feature flag needed.
list(APPEND ANNPOCKETDB_TEST_SOURCES test_crc32c.cpp test_mapped_file.cpp test_node_codec.cpp
                                     test_btree_rebalance.cpp)

if(ANNPOCKETDB_WITH_BACKUP)
  list(APPEND ANNPOCKETDB_TEST_SOURCES test_incremental_backup.cpp)
endif()

if(ANNPOCKETDB_WITH_ENCRYPTION)
  list(APPEND ANNPOCKETDB_TEST_SOURCES test_crypto_primitives.cpp test_encrypted_db.cpp)
endif()

if(ANNPOCKETDB_WITH_COMPRESSION)
  list(APPEND ANNPOCKETDB_TEST_SOURCES test_compression.cpp)
endif()

if(ANNPOCKETDB_WITH_INDEX)
  list(APPEND ANNPOCKETDB_TEST_SOURCES test_index.cpp)
endif()

if(ANNPOCKETDB_WITH_BLOB)
  list(APPEND ANNPOCKETDB_TEST_SOURCES test_blob.cpp)
endif()

if(ANNPOCKETDB_WITH_QUERY)
  list(APPEND ANNPOCKETDB_TEST_SOURCES test_query.cpp)
endif()

if(ANNPOCKETDB_WITH_CHANGEFEED)
  list(APPEND ANNPOCKETDB_TEST_SOURCES test_changefeed.cpp)
endif()

if(ANNPOCKETDB_WITH_ENCRYPTION AND ANNPOCKETDB_WITH_BACKUP)
  list(APPEND ANNPOCKETDB_TEST_SOURCES test_incremental_backup_encrypted.cpp)
endif()

if(ANNPOCKETDB_WITH_SUBDATABASE AND ANNPOCKETDB_WITH_BACKUP)
  list(APPEND ANNPOCKETDB_TEST_SOURCES test_env_extras.cpp)
endif()

foreach(test_src ${ANNPOCKETDB_TEST_SOURCES})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_executable(${test_name} ${test_src})
  target_link_libraries(${test_name} PRIVATE AnnPocketDb::features AnnPocketDb::kernel)
  target_compile_options(${test_name} PRIVATE ${ANNPOCKETDB_WARN_FLAGS})
  annpocketdb_apply_sanitizers(${test_name})
  add_test(NAME ${test_name} COMMAND ${test_name})
  if(ANNPOCKETDB_SANITIZE STREQUAL "thread")
    set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "TSAN_OPTIONS=${ANNPOCKETDB_TSAN_OPTIONS};TSAN_SUPPRESSIONS=${CMAKE_SOURCE_DIR}/ci/tsan.supp")
  elseif(ANNPOCKETDB_SANITIZE MATCHES "address")
    set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${ANNPOCKETDB_ASAN_OPTIONS};LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/ci/lsan.supp;UBSAN_OPTIONS=${ANNPOCKETDB_UBSAN_OPTIONS}")
  endif()
endforeach()

# ---------------------------------------------------------------------------
# mplay-equivalent multi-process fuzzer (Item 8): a scripted worker
# executable spawned as real, separate OS processes (see process_spawn.hpp)
# by test_mplay_fuzzer, not built via the generic loop above since it needs
# the worker's exact built path threaded in as a compile definition.
# ---------------------------------------------------------------------------
add_executable(mplay_worker mplay_worker.cpp)
target_link_libraries(mplay_worker PRIVATE AnnPocketDb::features AnnPocketDb::kernel)
target_compile_options(mplay_worker PRIVATE ${ANNPOCKETDB_WARN_FLAGS})
annpocketdb_apply_sanitizers(mplay_worker)

add_executable(test_mplay_fuzzer test_mplay_fuzzer.cpp)
target_link_libraries(test_mplay_fuzzer PRIVATE AnnPocketDb::features AnnPocketDb::kernel)
target_compile_options(test_mplay_fuzzer PRIVATE ${ANNPOCKETDB_WARN_FLAGS})
target_compile_definitions(test_mplay_fuzzer PRIVATE
  MPLAY_WORKER_PATH="$<TARGET_FILE:mplay_worker>")
add_dependencies(test_mplay_fuzzer mplay_worker)
annpocketdb_apply_sanitizers(test_mplay_fuzzer)
add_test(NAME test_mplay_fuzzer COMMAND test_mplay_fuzzer)
if(ANNPOCKETDB_SANITIZE STREQUAL "thread")
  set_tests_properties(test_mplay_fuzzer PROPERTIES ENVIRONMENT "TSAN_OPTIONS=${ANNPOCKETDB_TSAN_OPTIONS}")
endif()

# --- soak variant: same worker, higher iteration/process counts, opted
# into a separate "soak" ctest label so default `ctest` runs stay fast and
# CI can run this nightly / on a schedule instead of every PR.
add_executable(test_mplay_soak test_mplay_soak.cpp)
target_link_libraries(test_mplay_soak PRIVATE AnnPocketDb::features AnnPocketDb::kernel)
target_compile_options(test_mplay_soak PRIVATE ${ANNPOCKETDB_WARN_FLAGS})
target_compile_definitions(test_mplay_soak PRIVATE MPLAY_WORKER_PATH="$<TARGET_FILE:mplay_worker>")
add_dependencies(test_mplay_soak mplay_worker)
annpocketdb_apply_sanitizers(test_mplay_soak)
add_test(NAME test_mplay_soak COMMAND test_mplay_soak)
set_tests_properties(test_mplay_soak PROPERTIES LABELS "soak" TIMEOUT 3600)
if(ANNPOCKETDB_SANITIZE STREQUAL "thread")
  set_tests_properties(test_mplay_soak PROPERTIES ENVIRONMENT "TSAN_OPTIONS=${ANNPOCKETDB_TSAN_OPTIONS}")
endif()

add_executable(test_soak_concurrent test_soak_concurrent.cpp)
target_link_libraries(test_soak_concurrent PRIVATE AnnPocketDb::features AnnPocketDb::kernel)
target_compile_options(test_soak_concurrent PRIVATE ${ANNPOCKETDB_WARN_FLAGS})
annpocketdb_apply_sanitizers(test_soak_concurrent)
add_test(NAME test_soak_concurrent COMMAND test_soak_concurrent)
set_tests_properties(test_soak_concurrent PROPERTIES LABELS "soak" TIMEOUT 3600)
if(ANNPOCKETDB_SANITIZE STREQUAL "thread")
  set_tests_properties(test_soak_concurrent PROPERTIES ENVIRONMENT "TSAN_OPTIONS=${ANNPOCKETDB_TSAN_OPTIONS}")
endif()

# ---------------------------------------------------------------------------
# Crash-consistency fuzzing: same real-process pairing pattern as
# mplay_worker/test_mplay_fuzzer above. Needs backup::verify()/repair() and
# real WAL recovery, hence gated on both.
# ---------------------------------------------------------------------------
if(ANNPOCKETDB_WITH_BACKUP AND ANNPOCKETDB_WITH_WAL)
  add_executable(crash_worker crash_worker.cpp)
  target_link_libraries(crash_worker PRIVATE AnnPocketDb::features AnnPocketDb::kernel)
  target_compile_options(crash_worker PRIVATE ${ANNPOCKETDB_WARN_FLAGS})
  annpocketdb_apply_sanitizers(crash_worker)

  add_executable(test_crash_consistency test_crash_consistency.cpp)
  target_link_libraries(test_crash_consistency PRIVATE AnnPocketDb::features AnnPocketDb::kernel)
  target_compile_options(test_crash_consistency PRIVATE ${ANNPOCKETDB_WARN_FLAGS})
  target_compile_definitions(test_crash_consistency PRIVATE
    CRASH_WORKER_PATH="$<TARGET_FILE:crash_worker>")
  add_dependencies(test_crash_consistency crash_worker)
  annpocketdb_apply_sanitizers(test_crash_consistency)
  add_test(NAME test_crash_consistency COMMAND test_crash_consistency)
  if(ANNPOCKETDB_SANITIZE STREQUAL "thread")
    set_tests_properties(test_crash_consistency PROPERTIES ENVIRONMENT "TSAN_OPTIONS=${ANNPOCKETDB_TSAN_OPTIONS};TSAN_SUPPRESSIONS=${CMAKE_SOURCE_DIR}/ci/tsan.supp")
  elseif(ANNPOCKETDB_SANITIZE MATCHES "address")
    set_tests_properties(test_crash_consistency PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${ANNPOCKETDB_ASAN_OPTIONS};LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/ci/lsan.supp;UBSAN_OPTIONS=${ANNPOCKETDB_UBSAN_OPTIONS}")
  endif()
endif()
