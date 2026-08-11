#
# AnnPocketDb :: cmake/Sanitizers.cmake
#
# Central sanitizer configuration. Wired in as ANNPOCKETDB_SANITIZE, a single
# string cache var so CI can select one mode per build (sanitizers cannot be
# combined arbitrarily -- TSan is mutually exclusive with ASan/MSan).
#
#   -DANNPOCKETDB_SANITIZE=none      (default) no instrumentation
#   -DANNPOCKETDB_SANITIZE=thread    ThreadSanitizer -- the primary tool for
#                                     catching MVCC / free-list / reader-table
#                                     races. Requires Clang or GCC. Not
#                                     available on MSVC (still preview-only
#                                     as of this writing -- see README).
#   -DANNPOCKETDB_SANITIZE=address   AddressSanitizer -- heap/stack OOB,
#                                     use-after-free, use-after-scope. Also
#                                     catches the corrupt-page-triggers-OOB
#                                     class of bug LMDB shipped CVEs for.
#                                     Available on Clang, GCC, and MSVC.
#   -DANNPOCKETDB_SANITIZE=undefined UBSan -- integer overflow, misaligned
#                                     access, signed shift UB, etc. Cheap to
#                                     run alongside ASan.
#   -DANNPOCKETDB_SANITIZE=address+undefined
#                                     ASan + UBSan together (the standard
#                                     "correctness" combo; safe to combine).
#
# TSan and ASan are never combined in one binary -- run them as separate CI
# jobs/matrix legs, not sequentially in the same build.
#
# Usage in CMakeLists.txt (root and tests/CMakeLists.txt):
#
#   include(cmake/Sanitizers.cmake)
#   ...
#   annpocketdb_apply_sanitizers(annpocketdb_kernel)
#   annpocketdb_apply_sanitizers(annpocketdb_features)
#   annpocketdb_apply_sanitizers(${test_name})   # inside the test foreach loop
#

set(ANNPOCKETDB_SANITIZE "none" CACHE STRING
    "Sanitizer mode: none | thread | address | undefined | address+undefined")
set_property(CACHE ANNPOCKETDB_SANITIZE PROPERTY STRINGS
    "none" "thread" "address" "undefined" "address+undefined")

# Recommended flags to run test binaries with under each mode. Exposed as a
# CMake variable so CI/ctest can pick them up (see tests/CMakeLists.txt for
# how these get attached as ENVIRONMENT properties on the relevant tests).
set(ANNPOCKETDB_TSAN_OPTIONS
    "halt_on_error=1:second_deadlock_stack=1:history_size=7:die_after_fork=0")
set(ANNPOCKETDB_ASAN_OPTIONS
    "halt_on_error=1:detect_leaks=1:strict_string_checks=1:check_initialization_order=1:detect_stack_use_after_return=1")
set(ANNPOCKETDB_UBSAN_OPTIONS
    "halt_on_error=1:print_stacktrace=1")

function(annpocketdb_apply_sanitizers target)
  if(ANNPOCKETDB_SANITIZE STREQUAL "none" OR NOT ANNPOCKETDB_SANITIZE)
    return()
  endif()

  if(MSVC)
    if(ANNPOCKETDB_SANITIZE MATCHES "thread")
      message(FATAL_ERROR
        "ANNPOCKETDB_SANITIZE=thread requested but MSVC's ThreadSanitizer "
        "support is preview-only and not wired here. Use a Clang toolset "
        "on Windows (clang-cl) or run the thread job on Linux/macOS in CI "
        "instead. See docs/SANITIZERS.md.")
    endif()
    # MSVC ASan: /fsanitize=address, no separate link flag needed, and it is
    # incompatible with /RTC1 (runtime checks) and edit-and-continue (/ZI).
    if(ANNPOCKETDB_SANITIZE MATCHES "address")
      target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
      target_compile_definitions(${target} PRIVATE _DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)
    endif()
    if(ANNPOCKETDB_SANITIZE MATCHES "undefined")
      message(WARNING "UBSan is not available on MSVC; skipping for target ${target}.")
    endif()
    return()
  endif()

  # Clang / GCC path (Linux, macOS)
  set(_flags "")

  if(ANNPOCKETDB_SANITIZE STREQUAL "thread")
    list(APPEND _flags -fsanitize=thread)
  else()
    if(ANNPOCKETDB_SANITIZE MATCHES "address")
      list(APPEND _flags -fsanitize=address -fsanitize-address-use-after-scope)
    endif()
    if(ANNPOCKETDB_SANITIZE MATCHES "undefined")
      list(APPEND _flags -fsanitize=undefined -fno-sanitize-recover=undefined)
    endif()
  endif()

  if(NOT _flags)
    message(FATAL_ERROR "Unrecognized ANNPOCKETDB_SANITIZE value: '${ANNPOCKETDB_SANITIZE}'")
  endif()

  # -fno-omit-frame-pointer: sanitizers need real stacks to symbolize.
  # -g: debug info for readable reports (harmless in RelWithDebInfo, but
  # sanitizer builds should really be run as Debug or RelWithDebInfo, never
  # a stripped Release -- assert this below).
  target_compile_options(${target} PRIVATE ${_flags} -fno-omit-frame-pointer -g)
  target_link_options(${target} PRIVATE ${_flags})

  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    message(WARNING
      "ANNPOCKETDB_SANITIZE=${ANNPOCKETDB_SANITIZE} with CMAKE_BUILD_TYPE=Release: "
      "assertions and CHECK()s used by the test suite may be compiled out. "
      "Use Debug or RelWithDebInfo for sanitizer builds.")
  endif()
endfunction()

# Convenience: true when any sanitizer is active, so CMakeLists.txt can skip
# things sanitizers don't play well with (e.g. hardware CRC intrinsics are
# fine, but you may want to force ANNPOCKETDB_PARANOID=ON here -- see below).
if(NOT ANNPOCKETDB_SANITIZE STREQUAL "none")
  set(ANNPOCKETDB_SANITIZERS_ACTIVE TRUE)
  # PARANOID mode adds bounds checks on every slot/cell read -- exactly the
  # kind of check that turns a sanitizer-caught OOB into a clean assertion
  # with a useful message instead of a raw ASan stack trace through decode
  # logic. Force it on for sanitizer builds regardless of the user's cache
  # setting, since the whole point of these builds is maximum diagnostic
  # power, not speed.
  set(ANNPOCKETDB_PARANOID ON CACHE BOOL "" FORCE)
else()
  set(ANNPOCKETDB_SANITIZERS_ACTIVE FALSE)
endif()
