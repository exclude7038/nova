include_guard(GLOBAL)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "Nova fuzzing requires Linux and Clang")
endif()

if(NOT NOVA_SANITIZER STREQUAL "asan-ubsan")
    message(FATAL_ERROR "Use -DNOVA_SANITIZER=asan-ubsan for the dedicated fuzz build")
endif()

include(FetchContent)

set(FUZZTEST_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(FUZZTEST_BUILD_FLATBUFFERS OFF CACHE BOOL "" FORCE)
set(FUZZTEST_FUZZING_MODE ON CACHE BOOL "" FORCE)
set(FUZZTEST_COMPATIBILITY_MODE "" CACHE STRING "" FORCE)

FetchContent_Declare(fuzztest
    GIT_REPOSITORY https://github.com/google/fuzztest.git
    GIT_TAG 2026-06-29
    EXCLUDE_FROM_ALL
    SYSTEM
)

FetchContent_MakeAvailable(fuzztest)

if(NOT TARGET fuzztest::fuzztest)
    message(FATAL_ERROR "FuzzTest CMake targets were not created")
endif()

function(nova_fuzz_require_cxx26 directory)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)

    foreach(target IN LISTS targets)
        get_target_property(target_type "${target}" TYPE)

        if(target_type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|EXECUTABLE)$")
            set_target_properties("${target}" PROPERTIES
                CXX_STANDARD 26
                CXX_STANDARD_REQUIRED ON
                CXX_EXTENSIONS OFF
            )
        endif()
    endforeach()

    get_property(subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)

    foreach(subdirectory IN LISTS subdirectories)
        nova_fuzz_require_cxx26("${subdirectory}")
    endforeach()
endfunction()

nova_fuzz_require_cxx26("${CMAKE_SOURCE_DIR}")

fuzztest_setup_fuzzing_flags()
add_compile_options(-UNDEBUG)
