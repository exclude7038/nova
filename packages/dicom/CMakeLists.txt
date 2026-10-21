add_library(nova_dicom STATIC
    dicom.cpp
    dicom_reader.cpp
    dicom_directory_reader.cpp
)

target_include_directories(nova_dicom
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(nova_dicom PUBLIC cxx_std_26)
nova_enable_unity_build(nova_dicom)

# Module imports in an ordinary .cpp must not be combined into a unity TU.
set_source_files_properties(dicom_directory_reader.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

find_package(quill REQUIRED)
find_package(libassert REQUIRED)
find_package(DCMTK REQUIRED)
find_package(magic_enum REQUIRED)

target_link_libraries(nova_dicom
    PUBLIC
    dcmtk::dcmtk
    libassert::assert
    nova_logging
    quill::quill
    nova_core
    PRIVATE
    nova_project_options
    magic_enum::magic_enum
    nova_concurrency
    nova_di
)

if(NOVA_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(NOVA_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(NOVA_BUILD_FUZZ_TESTS)
    add_subdirectory(fuzz)
endif()
