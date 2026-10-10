set shell := ["bash", "-eu", "-o", "pipefail", "-c"]
set positional-arguments

root := justfile_directory()
packages := root / "packages"
build_tool := packages / "build.py"

# Show available commands.
default:
    @just --list

# Build Nova.
#
# Arguments are order-independent:
#   just build
#   just build all release
#   just build release all
#   just build debug tests
#   just build di release
#   just build release di tests
#   just build fetch release
#   just build run release
#
# Known arguments:
#   debug, release, all, tests, fetch, run/renderer
#
# Any other single argument is treated as a package name.
build *args:
    #!/usr/bin/env bash
    set -euo pipefail

    cd "{{packages}}"

    flags=()
    package=""

    for arg in {{args}}; do
        case "$arg" in
            debug)
                flags+=("--debug")
                ;;
            release)
                flags+=("--release")
                ;;
            all)
                flags+=("--all")
                ;;
            test|tests)
                flags+=("--tests")
                ;;
            fetch)
                flags+=("--fetch")
                ;;
            run|renderer|run-renderer)
                flags+=("--run-renderer")
                ;;
            *)
                if [[ -n "$package" ]]; then
                    echo "error: multiple package names: '$package' and '$arg'" >&2
                    exit 2
                fi

                package="$arg"
                ;;
        esac
    done

    if [[ -n "$package" ]]; then
        flags+=("--package" "$package")
    fi

    exec python3 "{{build_tool}}" "${flags[@]}"

# Build and run nova_renderer.
run *args:
    @just build run {{args}}

# Build and run tests.
#
# Examples:
#   just test
#   just test "Scoped profiler"
#   just test "Scoped profiler" output
#   just test output
#   just test debug "Scoped profiler" output
#
# Arguments are order-independent:
#   debug           Use Debug build.
#   release         Use Release build (default).
#   output          Show output from successful tests.
#   verbose, -v     Alias for output.
#
# Any other argument is treated as the CTest test-name regex.
test *args:
    #!/usr/bin/env bash
    set -euo pipefail

    config="Release"
    pattern=""
    verbose=false

    for arg in "$@"; do
        case "$arg" in
            debug)
                config="Debug"
                ;;
            release)
                config="Release"
                ;;
            output|verbose|-v)
                verbose=true
                ;;
            *)
                if [[ -n "$pattern" ]]; then
                    echo "error: multiple test patterns: '$pattern' and '$arg'" >&2
                    exit 2
                fi

                pattern="$arg"
                ;;
        esac
    done

    cd "{{packages}}"

    build_dir="build/${config}/cmake"

    cmake --build "$build_dir"

    ctest_args=(
        --test-dir "$build_dir"
        --output-on-failure
    )

    if [[ -n "$pattern" ]]; then
        ctest_args+=(
            --tests-regex "$pattern"
        )
    fi

    if [[ "$verbose" == true ]]; then
        ctest_args+=("--verbose")
    fi

    exec ctest "${ctest_args[@]}"

# Run Conan fetch, then configure/build.
fetch *args:
    @just build fetch {{args}}

# Build one package.
pkg name *args:
    @just build {{name}} {{args}}

# Open the Nova log in lnav.
log:
    @log="${XDG_STATE_HOME:-$HOME/.local/state}/nova/nova.log"; \
    if [[ ! -f "$log" ]]; then \
        echo "error: Nova log not found: $log" >&2; \
        exit 1; \
    fi; \
    exec lnav "$log"

# Remove generated build files for one configuration.
clean config="release":
    #!/usr/bin/env bash
    set -euo pipefail

    case "{{config}}" in
        release)
            dir="{{packages}}/build/Release"
            ;;
        debug)
            dir="{{packages}}/build/Debug"
            ;;
        all)
            dir="{{packages}}/build"
            ;;
        *)
            echo "error: expected release, debug or all" >&2
            exit 2
            ;;
    esac

    echo "Removing $dir"
    rm -rf "$dir"

# Completely rebuild one configuration.
rebuild config="release":
    #!/usr/bin/env bash
    set -euo pipefail

    case "{{config}}" in
        release)
            rm -rf "{{packages}}/build/Release"
            exec just build release
            ;;
        debug)
            rm -rf "{{packages}}/build/Debug"
            exec just build debug
            ;;
        *)
            echo "error: expected release or debug" >&2
            exit 2
            ;;
    esac


asan:
    cd "{{packages}}" && \
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:abort_on_error=1:detect_stack_use_after_return=1" \
    UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
    python3 "{{build_tool}}" \
        --debug \
        --all \
        --fetch \
        --tests \
        --sanitizer asan-ubsan \
        --cpu-arch native \
        --cpu-tune native

tsan:
    cd "{{packages}}" && \
    TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1:history_size=7" \
    python3 "{{build_tool}}" \
        --debug \
        --all \
        --fetch \
        --tests \
        --sanitizer tsan \
        --cpu-arch native \
        --cpu-tune native


# Build and run benchmarks.
#
# Examples:
#   just bench dicom
#   just bench dicom single_file
#   just bench dicom single_file --full
#
# Environment overrides:
#   NOVA_DICOM_BENCH_FILE=/path/to/image.dcm
#   NOVA_BENCH_ITERATIONS=1000
#
# Benchmarks always use the Release configuration.
bench *args:
    #!/usr/bin/env bash
    set -euo pipefail

    cd "{{packages}}"

    package="${1:-dicom}"

    if (($# > 0)); then
        shift
    fi

    name="all"

    if (($# > 0)) && [[ "$1" != -* ]]; then
        name="$1"
        shift
    fi

    case "$package" in
        dicom)
            ;;
        *)
            echo "error: unsupported benchmark package: '$package'" >&2
            exit 2
            ;;
    esac

    build_dir="build/Release/cmake"
    bench_dir="$build_dir/benchmarks/$package"

    if [[ ! -f "$build_dir/CMakeCache.txt" ]]; then
        echo "error: Release build not configured. Run 'just build release fetch' first." >&2
        exit 1
    fi

    cmake -S . -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DNOVA_BUILD_BENCHMARKS=ON

    if [[ "$name" == "all" ]]; then
        target="nova_${package}_benchmark"
    else
        if [[ "$name" == nova_"${package}"_bench_* ]]; then
            target="$name"
        else
            target="nova_${package}_bench_${name}"
        fi

        if [[ ! "$target" =~ ^nova_dicom_bench_[a-zA-Z0-9_]+$ ]]; then
            echo "error: invalid benchmark name: '$name'" >&2
            exit 2
        fi
    fi

    cmake --build "$build_dir" --target "$target"

    if [[ "$name" == "all" ]]; then
        shopt -s nullglob
        executables=("$bench_dir"/nova_"$package"_bench_*)
    else
        executables=("$bench_dir/$target")
    fi

    file="${NOVA_DICOM_BENCH_FILE:-dicom/tests/test_data/CTHead1.dcm}"
    iterations="${NOVA_BENCH_ITERATIONS:-500}"

    count=0

    for executable in "${executables[@]}"; do
        if [[ ! -f "$executable" || ! -x "$executable" ]]; then
            continue
        fi

        echo
        echo "=================================================="
        echo "Benchmark: $(basename "$executable")"
        echo "Configuration: Release"
        echo "=================================================="

        "$executable" "$file" "$iterations" "$@"

        ((count+=1))
    done

    if ((count == 0)); then
        echo "error: no benchmarks found for '$package/$name'" >&2
        exit 1
    fi

