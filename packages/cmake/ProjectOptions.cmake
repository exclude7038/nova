include_guard(GLOBAL)
add_library(nova_project_options INTERFACE)

set(NOVA_SANITIZER "none" CACHE STRING "Nova sanitizer: none, asan-ubsan, tsan")

set_property(
    CACHE NOVA_SANITIZER
    PROPERTY STRINGS
        none
        asan-ubsan
        tsan
)
set(NOVA_CPU_ARCH "native" CACHE STRING "CPU architecture passed to -march")
set(NOVA_CPU_TUNE "native" CACHE STRING "CPU tuning target passed to -mtune")

function(nova_enable_unity_build target)
    if(NOVA_ENABLE_UNITY_BUILD)
        set_target_properties(${target} PROPERTIES
            UNITY_BUILD ON
            UNITY_BUILD_BATCH_SIZE 16
        )
    endif()
endfunction()

target_compile_options(nova_project_options INTERFACE
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

  -U_FORTIFY_SOURCE
  -D_FORTIFY_SOURCE=3
  -D_GLIBCXX_ASSERTIONS

  -fstack-clash-protection
  -fstack-protector-strong
  -fstrict-flex-arrays=3
  -ftrivial-auto-var-init=zero

  #-fsanitize=address
  -fno-omit-frame-pointer

  -march=${NOVA_CPU_ARCH}
  -mtune=${NOVA_CPU_TUNE}
  $<$<STREQUAL:${CMAKE_SYSTEM_PROCESSOR},x86_64>:-fcf-protection=full>

  $<$<CONFIG:Release>:-O3>
  $<$<CONFIG:Release>:-fdata-sections>
  $<$<CONFIG:Release>:-flto=thin>
  #$<$<CONFIG:Release>:-fsanitize=cfi>
  #$<$<CONFIG:Release>:-fsanitize-trap=cfi>
  $<$<CONFIG:Release>:-fwhole-program-vtables>
  $<$<CONFIG:Release>:-fstrict-vtable-pointers>
  $<$<CONFIG:Release>:-ffunction-sections>
  $<$<CONFIG:Release>:-fvisibility=hidden>
  $<$<CONFIG:Release>:-fvisibility-inlines-hidden>

  $<$<CONFIG:RelWithDebInfo>:-O3>
  $<$<CONFIG:RelWithDebInfo>:-g>
  $<$<CONFIG:RelWithDebInfo>:-flto=thin>
  $<$<CONFIG:RelWithDebInfo>:-fsanitize=cfi>
  $<$<CONFIG:RelWithDebInfo>:-fsanitize-trap=cfi>
  $<$<CONFIG:RelWithDebInfo>:-fwhole-program-vtables>
  $<$<CONFIG:RelWithDebInfo>:-fstrict-vtable-pointers>
  $<$<CONFIG:RelWithDebInfo>:-ffunction-sections>
  $<$<CONFIG:RelWithDebInfo>:-fdata-sections>
)

set(NOVA_SANITIZER_COMPILE_OPTIONS)
set(NOVA_SANITIZER_LINK_OPTIONS)

if(NOVA_SANITIZER STREQUAL "asan-ubsan")
    list(APPEND NOVA_SANITIZER_COMPILE_OPTIONS
        -O1
        -g3
        -fno-omit-frame-pointer
        -fno-optimize-sibling-calls
        -fsanitize=address,undefined
        -fsanitize-address-use-after-scope
        -fno-sanitize-recover=all
    )
    list(APPEND NOVA_SANITIZER_LINK_OPTIONS
        -fsanitize=address,undefined
    )

elseif(NOVA_SANITIZER STREQUAL "tsan")
    list(APPEND NOVA_SANITIZER_COMPILE_OPTIONS
        -O1
        -g3
        -fno-omit-frame-pointer
        -fno-optimize-sibling-calls
        -fsanitize=thread
    )
    list(APPEND NOVA_SANITIZER_LINK_OPTIONS
        -fsanitize=thread
    )
elseif(NOT NOVA_SANITIZER STREQUAL "none")
    message(
        FATAL_ERROR
        "Unknown NOVA_SANITIZER='${NOVA_SANITIZER}'"
    )
endif()

target_compile_options(
    nova_project_options
    INTERFACE
        ${NOVA_SANITIZER_COMPILE_OPTIONS}
)

target_link_options(
    nova_project_options
    INTERFACE
        ${NOVA_SANITIZER_LINK_OPTIONS}
)

target_link_options(nova_project_options INTERFACE
  -Wl,-z,nodlopen
  -fuse-ld=lld

  -Wl,-z,noexecstack

  -Wl,-z,relro
  -Wl,-z,now

  -Wl,-z,text
  -Wl,-z,separate-code
  -Wl,-z,defs

  -Wl,--as-needed
  -Wl,--no-copy-dt-needed-entries
  -Wl,--fatal-warnings

  #-fsanitize=address

  $<$<CONFIG:Release>:-flto=thin>
  #$<$<CONFIG:Release>:-fsanitize=cfi>
  #$<$<CONFIG:Release>:-fsanitize-trap=cfi>

  $<$<CONFIG:Release>:-Wl,--gc-sections>
  $<$<CONFIG:Release>:-Wl,--icf=safe>
  $<$<CONFIG:Release>:-Wl,--strip-all>

  $<$<CONFIG:RelWithDebInfo>:-flto=thin>
  $<$<CONFIG:RelWithDebInfo>:-fsanitize=cfi>
  $<$<CONFIG:RelWithDebInfo>:-fsanitize-trap=cfi>

  $<$<CONFIG:RelWithDebInfo>:-Wl,--gc-sections>
  $<$<CONFIG:RelWithDebInfo>:-Wl,--icf=safe>
)
