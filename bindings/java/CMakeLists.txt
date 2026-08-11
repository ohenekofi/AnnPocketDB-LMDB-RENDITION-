cmake_minimum_required(VERSION 3.20)

find_package(JNI REQUIRED)

add_library(annpocketdb_jni SHARED src/main/cpp/annpocketdb_jni.cpp)

target_include_directories(annpocketdb_jni PRIVATE ${JNI_INCLUDE_DIRS})
target_link_libraries(annpocketdb_jni PRIVATE AnnPocketDb::c)

# Remove the standard "lib" prefix for JNI on Windows, keep it on POSIX.
if(WIN32)
    set_target_properties(annpocketdb_jni PROPERTIES PREFIX "")
endif()
