cmake_minimum_required(VERSION 3.20)

add_executable(test_capi_smoke test_capi_smoke.c)
target_link_libraries(test_capi_smoke PRIVATE AnnPocketDb::c)
target_include_directories(test_capi_smoke PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)
# C99 for the test itself — the library is already C++20.
set_target_properties(test_capi_smoke PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

# Register as a CTest test.
add_test(NAME capi_smoke COMMAND test_capi_smoke)
# Point the dynamic linker at the build tree when running via ctest.
# $<TARGET_FILE_DIR:annpocketdb_c> resolves to the DLL/.so's *actual* output
# directory -- on a multi-config generator (Visual Studio) that's
# ${CMAKE_BINARY_DIR}/Debug (or /Release/...), not CMAKE_BINARY_DIR itself,
# which is what made the plain CMAKE_BINARY_DIR value below fail to find
# annpocketdb_c.dll at runtime (STATUS_DLL_NOT_FOUND / 0xc0000135).
set_tests_properties(capi_smoke PROPERTIES
  ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR};PATH=$<TARGET_FILE_DIR:annpocketdb_c>;${CMAKE_BINARY_DIR}"
)
