cmake_minimum_required(VERSION 3.20)
# C is only needed for the vendored Monocypher source
# (thirdparty/monocypher/monocypher.c) when ANNPOCKETDB_WITH_ENCRYPTION is on.
project(AnnPocketDb VERSION 0.1.1 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -Wall/-Wextra are GCC/Clang-only; cl.exe mangles -Wextra into /Wextra and
# fails with D8021 (invalid numeric argument to /W). Use /W4 on MSVC instead.
if(MSVC)
  set(ANNPOCKETDB_WARN_FLAGS /W4)
else()
  set(ANNPOCKETDB_WARN_FLAGS -Wall -Wextra)
endif()

# Must come before the ANNPOCKETDB_PARANOID option() below: Sanitizers.cmake
# force-sets that cache variable to ON when a sanitizer is active, and a
# CACHE variable that already exists wins over a later plain option() call.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Sanitizers.cmake)

option(ANNPOCKETDB_BUILD_TESTS "Build AnnPocketDb kernel test suite" ON)
option(ANNPOCKETDB_ENABLE_HARDWARE_CRC "Enable SSE4.2/ARMv8-CRC hardware CRC32C" ON)
option(ANNPOCKETDB_PARANOID
  "Bounds-check every slot/cell on getPage when CRC is enabled (slower; off by default for LMDB-parity)"
  OFF)

# ---------------------------------------------------------------------------
# Feature toggles (zero external dependency modules)
# ---------------------------------------------------------------------------
option(ANNPOCKETDB_WITH_WAL "Build the write-ahead log module" ON)
option(ANNPOCKETDB_WITH_BLOB "Build the large-object blob module" ON)
option(ANNPOCKETDB_WITH_INDEX "Build the secondary index module" ON)
option(ANNPOCKETDB_WITH_BACKUP "Build the online backup/restore module" ON)
option(ANNPOCKETDB_WITH_QUERY "Build the query engine module" ON)
option(ANNPOCKETDB_WITH_CHANGEFEED "Build the changefeed/CDC module" ON)
option(ANNPOCKETDB_WITH_SUBDATABASE "Build named sub-database support" ON)

# ---------------------------------------------------------------------------
# Feature toggles (require vendored third-party libraries)
# ---------------------------------------------------------------------------
option(ANNPOCKETDB_WITH_COMPRESSION "Build the LZ4 value/overflow-level compression module (vendored LZ4, no network fetch)" OFF)
option(ANNPOCKETDB_WITH_ENCRYPTION
  "Build the page-level XChaCha20-Poly1305 encryption + optional SHA-256 checksum module (vendored Monocypher + sha256.hpp, no network fetch)"
  OFF)

if(CMAKE_BUILD_TYPE STREQUAL "")
  set(CMAKE_BUILD_TYPE "RelWithDebInfo")
endif()

# ---------------------------------------------------------------------------
# Kernel library: platform + util + core (page manager) + btree + txn + api.
#
# Deliberately zero third-party dependencies -- this is the part of the
# engine every feature module builds on.
# ---------------------------------------------------------------------------
set(KERNEL_SOURCES
  src/util/crc32c.cpp
  src/platform/mapped_file.cpp
  src/platform/interprocess_lock.cpp
  src/core/free_list.cpp
  src/core/page_manager.cpp
  src/btree/node_codec.cpp
  src/btree/btree.cpp
  src/btree/cursor.cpp
  src/txn/transaction.cpp
  src/api/environment.cpp
  # ChangeSink (not the rest of the changefeed module) is called directly
  # from WriteTransaction::put/remove/commit above whenever a sink happens
  # to be attached -- always compiled regardless of ANNPOCKETDB_WITH_
  # CHANGEFEED, same reasoning as the WAL/changefeed block below. See
  # change_sink.cpp's own header comment.
  src/changefeed/change_sink.cpp
)

set(KERNEL_HEADERS
  include/annpocketdb/util/crc32c.hpp
  include/annpocketdb/util/status.hpp
  include/annpocketdb/util/types.hpp
  include/annpocketdb/platform/mapped_file.hpp
  include/annpocketdb/core/free_list.hpp
  include/annpocketdb/core/meta_page.hpp
  include/annpocketdb/core/page.hpp
  include/annpocketdb/core/page_manager.hpp
  include/annpocketdb/core/reader_table.hpp
  include/annpocketdb/btree/node_codec.hpp
  include/annpocketdb/btree/btree.hpp
  include/annpocketdb/btree/cursor.hpp
  include/annpocketdb/txn/transaction.hpp
  include/annpocketdb/api/environment.hpp
)

# Encryption lives at the PageManager layer (see core/encryption_sidecar.hpp),
# so unlike every other optional module it must be part of the kernel tier
# itself, not the features tier above it -- the one deliberate exception to
# "kernel has zero third-party deps", gated entirely behind this option.
if(ANNPOCKETDB_WITH_ENCRYPTION)
  list(APPEND KERNEL_SOURCES
    src/encryption/page_cipher.cpp
    src/encryption/chacha8.cpp
    src/core/encryption_sidecar.cpp
    src/core/checksum_sidecar.cpp
  )
  list(APPEND KERNEL_HEADERS
    include/annpocketdb/encryption/page_cipher.hpp
    include/annpocketdb/encryption/chacha8.hpp
    include/annpocketdb/core/encryption_sidecar.hpp
    include/annpocketdb/core/checksum_sidecar.hpp
  )
endif()

# Compression's hook point is src/btree/btree.cpp (kernel tier), so -- like
# encryption above -- it must be part of the kernel tier itself rather than
# the features tier below, the second deliberate exception to "kernel has
# zero third-party deps".
if(ANNPOCKETDB_WITH_COMPRESSION)
  list(APPEND KERNEL_SOURCES src/compression/lz4_codec.cpp)
  list(APPEND KERNEL_HEADERS include/annpocketdb/compression/lz4_codec.hpp)
endif()

# WAL is called directly from PageManager::commit() (src/core/page_manager.cpp,
# kernel tier) on its core commit path, not from an optional accessor -- so
# like encryption/compression above, it must be part of the kernel tier
# itself rather than the features tier below (a static archive must never
# need a symbol from an archive that depends on it). Third deliberate
# exception to "kernel has zero third-party deps" (WAL itself vendors
# nothing third-party, so the *dependency-free* part of that claim still
# holds -- only the kernel/features placement changes).
if(ANNPOCKETDB_WITH_WAL)
  list(APPEND KERNEL_SOURCES src/wal/record.cpp src/wal/wal.cpp)
  list(APPEND KERNEL_HEADERS include/annpocketdb/wal/record.hpp include/annpocketdb/wal/wal.hpp)
endif()

# The rest of the changefeed module (delivery backends, DurableChangeFeed,
# factories) -- ChangeSink itself (needed unconditionally, see above) is not
# here, it's in change_sink.cpp. This part genuinely is optional: nothing
# outside Environment::open() ever references it, and open() is guarded by
# #if defined(ANNPOCKETDB_WITH_CHANGEFEED) (src/api/environment.cpp).
if(ANNPOCKETDB_WITH_CHANGEFEED)
  list(APPEND KERNEL_SOURCES src/changefeed/changefeed.cpp)
  list(APPEND KERNEL_HEADERS include/annpocketdb/changefeed/changefeed.hpp)
endif()

add_library(annpocketdb_kernel STATIC ${KERNEL_SOURCES} ${KERNEL_HEADERS})
target_include_directories(annpocketdb_kernel PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_options(annpocketdb_kernel PRIVATE ${ANNPOCKETDB_WARN_FLAGS})
annpocketdb_apply_sanitizers(annpocketdb_kernel)

if(ANNPOCKETDB_PARANOID)
  target_compile_definitions(annpocketdb_kernel PUBLIC ANNPOCKETDB_PARANOID=1)
endif()

if(ANNPOCKETDB_ENABLE_HARDWARE_CRC AND (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i386|i686"))
  if(NOT MSVC)
    # MSVC exposes SSE4.2 intrinsics (<nmmintrin.h>) without a dedicated
    # /arch: switch; -msse4.2 is a GCC/Clang-only flag cl.exe doesn't understand.
    target_compile_options(annpocketdb_kernel PRIVATE -msse4.2)
  endif()
endif()

if(WIN32)
  # No extra system libraries needed: CreateFileMapping/MapViewOfFile live in
  # kernel32, linked by default on all supported toolchains.
else()
  find_package(Threads REQUIRED)
  target_link_libraries(annpocketdb_kernel PUBLIC Threads::Threads)
endif()

if(ANNPOCKETDB_WITH_ENCRYPTION)
  # Vendored, no network fetch: thirdparty/monocypher/{monocypher.c,.h}
  # (XChaCha20-Poly1305 AEAD + Argon2id), copied from the sibling AnnMeta
  # project's own vendoring of the same upstream release.
  add_library(annpocketdb_monocypher STATIC thirdparty/monocypher/monocypher.c)
  target_include_directories(annpocketdb_monocypher PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/monocypher
  )
  if(MSVC)
    target_compile_options(annpocketdb_monocypher PRIVATE /W0)
  else()
    target_compile_options(annpocketdb_monocypher PRIVATE -w)
  endif()
  # Monocypher is written in constant-time style with data-dependent-looking
  # branches that are actually constant-time -- ASan won't misfire on it, but
  # TSan needs to see it instrumented too, or calls into it show up as
  # unhelpful "call into uninstrumented code" noise instead of a real report.
  annpocketdb_apply_sanitizers(annpocketdb_monocypher)
  target_link_libraries(annpocketdb_kernel PUBLIC annpocketdb_monocypher)

  # thirdparty/sha256/sha256.hpp is header-only (optional checksum bolt-on).
  target_include_directories(annpocketdb_kernel PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/sha256
  )
  target_compile_definitions(annpocketdb_kernel PUBLIC ANNPOCKETDB_WITH_ENCRYPTION=1)
endif()

if(ANNPOCKETDB_WITH_COMPRESSION)
  # Vendored, no network fetch: thirdparty/lz4/{lz4.c,.h}, LZ4's official
  # two-file "basic API" distribution (BSD 2-Clause), pinned to v1.9.4 --
  # same vendoring pattern as annpocketdb_monocypher above.
  add_library(annpocketdb_lz4 STATIC thirdparty/lz4/lz4.c)
  target_include_directories(annpocketdb_lz4 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/lz4
  )
  if(MSVC)
    target_compile_options(annpocketdb_lz4 PRIVATE /W0)
  else()
    target_compile_options(annpocketdb_lz4 PRIVATE -w)
  endif()
  target_link_libraries(annpocketdb_kernel PUBLIC annpocketdb_lz4)
  target_compile_definitions(annpocketdb_kernel PUBLIC ANNPOCKETDB_WITH_COMPRESSION=1)
endif()

if(ANNPOCKETDB_WITH_WAL)
  target_compile_definitions(annpocketdb_kernel PUBLIC ANNPOCKETDB_WITH_WAL=1)
endif()

if(ANNPOCKETDB_WITH_CHANGEFEED)
  target_compile_definitions(annpocketdb_kernel PUBLIC ANNPOCKETDB_WITH_CHANGEFEED=1)
endif()

add_library(AnnPocketDb::kernel ALIAS annpocketdb_kernel)

# ---------------------------------------------------------------------------
# Feature modules (zero external deps)
# ---------------------------------------------------------------------------
set(FEATURE_SOURCES)
set(FEATURE_LIBS annpocketdb_kernel)

# Self-contained (no third-party dependency despite the name -- MessagePack
# codec is hand-rolled, see document.hpp). Always built: index.cpp and
# Environment::documentStore() both depend on it unconditionally.
list(APPEND FEATURE_SOURCES src/document/document.cpp)

# environment_blob.cpp/environment_subdatabase.cpp are always compiled (like
# document.cpp above), *not* gated behind ANNPOCKETDB_WITH_BLOB/_SUBDATABASE
# -- Environment::putBlob/getBlob/removeBlob and the five sub-database
# methods are declared unconditionally in environment.hpp, so something must
# always define them. Each method internally #ifdefs on its own flag,
# returning Status::NotSupported when that flag is off, matching the WAL
# pattern in Environment::open(). blob_store.cpp/sub_database.cpp themselves
# stay genuinely gated below -- nothing calls into them when their flag is
# off.
list(APPEND FEATURE_SOURCES src/api/environment_blob.cpp src/api/environment_subdatabase.cpp)

if(ANNPOCKETDB_WITH_BLOB)
  list(APPEND FEATURE_SOURCES src/blob/blob_store.cpp)
  target_compile_definitions(annpocketdb_kernel PUBLIC ANNPOCKETDB_WITH_BLOB=1)
endif()

if(ANNPOCKETDB_WITH_INDEX)
  list(APPEND FEATURE_SOURCES src/index/index.cpp)
  target_compile_definitions(annpocketdb_kernel PUBLIC ANNPOCKETDB_WITH_INDEX=1)
endif()

if(ANNPOCKETDB_WITH_BACKUP)
  list(APPEND FEATURE_SOURCES src/backup/backup.cpp)
  target_compile_definitions(annpocketdb_kernel PUBLIC ANNPOCKETDB_WITH_BACKUP=1)
endif()

if(ANNPOCKETDB_WITH_QUERY)
  list(APPEND FEATURE_SOURCES src/query/query.cpp)
  target_compile_definitions(annpocketdb_kernel PUBLIC ANNPOCKETDB_WITH_QUERY=1)
endif()

if(ANNPOCKETDB_WITH_SUBDATABASE)
  list(APPEND FEATURE_SOURCES src/api/sub_database.cpp)
  target_compile_definitions(annpocketdb_kernel PUBLIC ANNPOCKETDB_WITH_SUBDATABASE=1)
endif()

if(FEATURE_SOURCES)
  add_library(annpocketdb_features STATIC ${FEATURE_SOURCES})
  target_include_directories(annpocketdb_features PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  )
  target_link_libraries(annpocketdb_features PUBLIC annpocketdb_kernel)
  target_compile_options(annpocketdb_features PRIVATE ${ANNPOCKETDB_WARN_FLAGS})
  annpocketdb_apply_sanitizers(annpocketdb_features)
  add_library(AnnPocketDb::features ALIAS annpocketdb_features)
  list(APPEND FEATURE_LIBS annpocketdb_features)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(ANNPOCKETDB_BUILD_TESTS)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
  else()
    message(WARNING "ANNPOCKETDB_BUILD_TESTS is ON but tests/CMakeLists.txt does not exist yet -- skipping. "
                     "The README describes an extensive test suite that is not actually present in this checkout.")
  endif()
endif()



# ---------------------------------------------------------------------------
# Offline maintenance CLI (dump / load / verify / repair)
# ---------------------------------------------------------------------------
if(ANNPOCKETDB_WITH_BACKUP)
  add_executable(apdb tools/apdb.cpp)
  target_link_libraries(apdb PRIVATE AnnPocketDb::features AnnPocketDb::kernel)
  target_compile_options(apdb PRIVATE ${ANNPOCKETDB_WARN_FLAGS})
  if(WIN32)
    # CommandLineToArgvW, for Unicode (UTF-16) command-line paths
    target_link_libraries(apdb PRIVATE shell32)
  endif()
  install(TARGETS apdb RUNTIME DESTINATION bin)
endif()

# ---------------------------------------------------------------------------
# Shared C API library  (libannpocketdb_c.so / annpocketdb_c.dll)
#
# This is the stable FFI layer consumed by Go (cgo), Java (JNI), Node.js
# (N-API), and Python (ctypes).  It is a SHARED library because:
#   - cgo, JNI, and N-API all dlopen/link against a shared object at runtime.
#   - A shared library lets all four bindings share one copy of the engine
#     in-process rather than duplicating it in four static archives.
# ---------------------------------------------------------------------------
option(ANNPOCKETDB_BUILD_CAPI "Build the shared C API library (libannpocketdb_c)" ON)

if(ANNPOCKETDB_BUILD_CAPI)
  add_library(annpocketdb_c SHARED src/capi/annpocketdb_c.cpp)

  # annpocketdb_c.cpp's extern "C" functions have no __declspec(dllexport)
  # annotations, so MSVC (unlike GCC/Clang, which export everything public
  # by default) exports nothing from the DLL by default -- and critically,
  # generates no import .lib for it either, which is exactly what makes
  # downstream consumers (e.g. tests/capi) fail to link with
  # "cannot open file ...annpocketdb_c.lib". This makes MSVC auto-export
  # every public symbol, matching GCC/Clang's default. A real dllexport/
  # dllimport macro in the public header would be the more idiomatic fix if
  # finer-grained control over the exported surface is ever needed.
  set_target_properties(annpocketdb_c PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

  target_include_directories(annpocketdb_c
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>
  )
  target_compile_options(annpocketdb_c PRIVATE ${ANNPOCKETDB_WARN_FLAGS})

  # Link against the full feature+kernel stack.  PRIVATE because consumers of
  # libannpocketdb_c depend only on the C header, not on C++ headers.
  target_link_libraries(annpocketdb_c PRIVATE ${FEATURE_LIBS})

  # On POSIX set an SONAME so the dynamic linker can find the library by its
  # versioned name (libannpocketdb_c.so.0).
  set_target_properties(annpocketdb_c PROPERTIES
    VERSION   "${PROJECT_VERSION}"
    SOVERSION "${APDB_VERSION_MAJOR}"
    OUTPUT_NAME "annpocketdb_c"
  )
  # On macOS ensure the install_name is relocatable.
  if(APPLE)
    set_target_properties(annpocketdb_c PROPERTIES
      INSTALL_NAME_DIR "@rpath"
      BUILD_WITH_INSTALL_RPATH TRUE
    )
  endif()

  add_library(AnnPocketDb::c ALIAS annpocketdb_c)

  install(TARGETS annpocketdb_c
    LIBRARY  DESTINATION lib
    RUNTIME  DESTINATION bin   # DLL on Windows
    ARCHIVE  DESTINATION lib   # import lib on Windows
  )
  install(DIRECTORY include/annpocketdb_c DESTINATION include)

  # C smoke test — plain C99, no C++ framework dependency.
  if(ANNPOCKETDB_BUILD_TESTS)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/capi/CMakeLists.txt")
      add_subdirectory(tests/capi)
    endif()
  endif()
endif()

# Version constant used by the version_check ABI symbol.
set(APDB_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
