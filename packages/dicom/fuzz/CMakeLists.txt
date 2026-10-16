add_custom_target(nova_dicom_fuzz)

add_executable(nova_dicom_fuzz_reader EXCLUDE_FROM_ALL
    dicom_reader_fuzz.cpp
)

target_compile_features(nova_dicom_fuzz_reader PRIVATE cxx_std_26)
target_compile_definitions(nova_dicom_fuzz_reader PRIVATE
    NOVA_DICOM_FUZZ_SEED_FILE="${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_data/CTHead1.dcm"
)
target_link_libraries(nova_dicom_fuzz_reader PRIVATE
    nova_dicom
    nova_project_options
)
link_fuzztest(nova_dicom_fuzz_reader)

set_target_properties(nova_dicom_fuzz_reader PROPERTIES
    UNITY_BUILD OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fuzz/dicom"
)

add_dependencies(nova_dicom_fuzz nova_dicom_fuzz_reader)
