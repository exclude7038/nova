
add_custom_target(nova_dicom_benchmark)

function(nova_add_dicom_benchmark name)
    set(target "nova_dicom_bench_${name}")

    add_executable(${target} EXCLUDE_FROM_ALL
        ${ARGN}
    )

    target_compile_features(${target} PRIVATE cxx_std_26)

    target_link_libraries(${target}
        PRIVATE
            nova_dicom
            nova_core
            nova_project_options
    )

    set_target_properties(${target}
        PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks/dicom"
    )

    add_dependencies(nova_dicom_benchmark ${target})
endfunction()

nova_add_dicom_benchmark(single_file
    single_file_benchmark.cpp
)

