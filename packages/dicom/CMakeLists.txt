add_library(nova_dicom STATIC
    dicom.cpp
    dicom_reader.cpp
)

target_include_directories(nova_dicom
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(nova_dicom PUBLIC cxx_std_23)
nova_enable_unity_build(nova_dicom)

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
)

if(NOVA_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(NOVA_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
