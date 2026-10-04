include_guard(GLOBAL)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Nova requires Clang")
endif()

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    message(FATAL_ERROR "Nova requires x86-64")
endif()

add_library(nova_project_options INTERFACE)

set(NOVA_SANITIZER "none" CACHE STRING "Nova sanitizer: none, asan-ubsan, tsan")

set_property(
    CACHE NOVA_SANITIZER
    PROPERTY STRINGS
        none
        asan-ubsan
        tsan
)

set(NOVA_CPU_ARCH "x86-64-v4" CACHE STRING "CPU architecture passed to -march")
set(NOVA_CPU_TUNE "generic" CACHE STRING "CPU tuning target passed to -mtune")

function(nova_enable_unity_build target)
    if(NOVA_ENABLE_UNITY_BUILD)
        set_target_properties(
            ${target}
            PROPERTIES
                UNITY_BUILD ON
                UNITY_BUILD_BATCH_SIZE 16
        )
    endif()
endfunction()

target_compile_options(
    nova_project_options
    INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wnon-virtual-dtor
        -Woverloaded-virtual
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
        -Wmissing-declarations
        -Wunreachable-code
        -Wundef
        -Wno-c2y-extensions
        -Winvalid-utf8

        -march=${NOVA_CPU_ARCH}
        -mtune=${NOVA_CPU_TUNE}

        -fstrict-flex-arrays=3

        -U_FORTIFY_SOURCE
)

if(NOVA_SANITIZER STREQUAL "none")
    target_compile_options(
        nova_project_options
        INTERFACE
            -fstack-clash-protection
            -fstack-protector-strong
            -fcf-protection=full

            $<$<CONFIG:Debug>:-O0>
            $<$<CONFIG:Debug>:-g3>
            $<$<CONFIG:Debug>:-gdwarf-5>
            $<$<CONFIG:Debug>:-fstandalone-debug>
            $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
            $<$<CONFIG:Debug>:-fno-optimize-sibling-calls>
            $<$<CONFIG:Debug>:-ftrivial-auto-var-init=pattern>

            $<$<CONFIG:Release>:-O3>
            $<$<CONFIG:Release>:-fomit-frame-pointer>
            $<$<CONFIG:Release>:-ftrivial-auto-var-init=zero>
            $<$<CONFIG:Release>:-flto=thin>
            $<$<CONFIG:Release>:-ffunction-sections>
            $<$<CONFIG:Release>:-fdata-sections>
            $<$<CONFIG:Release>:-fwhole-program-vtables>
            $<$<CONFIG:Release>:-fstrict-vtable-pointers>
            $<$<CONFIG:Release>:-fvisibility=hidden>
            $<$<CONFIG:Release>:-fvisibility-inlines-hidden>

            $<$<CONFIG:RelWithDebInfo>:-O3>
            $<$<CONFIG:RelWithDebInfo>:-g3>
            $<$<CONFIG:RelWithDebInfo>:-gdwarf-5>
            $<$<CONFIG:RelWithDebInfo>:-fomit-frame-pointer>
            $<$<CONFIG:RelWithDebInfo>:-ftrivial-auto-var-init=zero>
            $<$<CONFIG:RelWithDebInfo>:-flto=thin>
            $<$<CONFIG:RelWithDebInfo>:-ffunction-sections>
            $<$<CONFIG:RelWithDebInfo>:-fdata-sections>
            $<$<CONFIG:RelWithDebInfo>:-fwhole-program-vtables>
            $<$<CONFIG:RelWithDebInfo>:-fstrict-vtable-pointers>
            $<$<CONFIG:RelWithDebInfo>:-fvisibility=hidden>
            $<$<CONFIG:RelWithDebInfo>:-fvisibility-inlines-hidden>
    )

    target_compile_definitions(
        nova_project_options
        INTERFACE
            $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>
            $<$<CONFIG:Release>:_FORTIFY_SOURCE=3>
            $<$<CONFIG:RelWithDebInfo>:_FORTIFY_SOURCE=3>
    )

    target_link_options(
        nova_project_options
        INTERFACE
            -fuse-ld=lld

            -Wl,-z,noexecstack
            -Wl,-z,relro
            -Wl,-z,now
            -Wl,-z,text
            -Wl,-z,separate-code
            -Wl,-z,defs

            -Wl,--as-needed
            -Wl,--fatal-warnings

            $<$<CONFIG:Release>:-flto=thin>
            $<$<CONFIG:Release>:-Wl,--gc-sections>
            $<$<CONFIG:Release>:-Wl,--icf=safe>
            $<$<CONFIG:Release>:-Wl,--strip-all>

            $<$<CONFIG:RelWithDebInfo>:-flto=thin>
            $<$<CONFIG:RelWithDebInfo>:-Wl,--gc-sections>
            $<$<CONFIG:RelWithDebInfo>:-Wl,--icf=safe>
    )

elseif(NOVA_SANITIZER STREQUAL "asan-ubsan")
    target_compile_options(
        nova_project_options
        INTERFACE
            -O1
            -g3
            -gdwarf-5
            -fno-omit-frame-pointer
            -fno-optimize-sibling-calls
            -fsanitize=address,undefined
            -fsanitize-address-use-after-scope
            -fno-sanitize-recover=all
    )

    target_link_options(
        nova_project_options
        INTERFACE
            -fuse-ld=lld
            -fsanitize=address,undefined
    )

elseif(NOVA_SANITIZER STREQUAL "tsan")
    target_compile_options(
        nova_project_options
        INTERFACE
            -O1
            -g3
            -gdwarf-5
            -fno-omit-frame-pointer
            -fno-optimize-sibling-calls
            -fsanitize=thread
    )

    target_link_options(
        nova_project_options
        INTERFACE
            -fuse-ld=lld
            -fsanitize=thread
    )

else()
    message(
        FATAL_ERROR
        "Unknown NOVA_SANITIZER='${NOVA_SANITIZER}'"
    )
endif()
