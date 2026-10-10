#include "dicom/dicom_reader.h"
#include "result.h"
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <numeric>
#include <print>
#include <ranges>
#include <string_view>
#include <system_error>
#include <vector>

import core.profiler;

namespace {
    using nova::profiler::timestamp;
    namespace fs = std::filesystem;

    struct measurement final {
        std::uint64_t total{};
        std::uint64_t construct{};
        std::uint64_t load{};
        std::uint64_t metadata{};
        std::uint64_t pixel_info{};
        std::uint64_t pixel_data{};
        std::uint64_t destroy{};
        std::uint64_t checksum{};
    };

    [[nodiscard]] nova::result<measurement> measure(const fs::path& path, const bool full, const bool record) {
        const auto total_begin = nova::profiler::tick();

        timestamp construct_end{};
        timestamp load_end{};
        timestamp metadata_end{};
        timestamp pixel_info_end{};
        timestamp pixel_data_end{};
        timestamp destroy_begin{};

        std::uint64_t checksum{};

        {
            nova::dicom::dicom_reader reader;
            construct_end = nova::profiler::tick();

            const auto loaded = reader.load(path);
            load_end = nova::profiler::tick();

            if(!loaded) [[unlikely]] {
                return nova::err(std::format("DICOM load failed: {}", loaded.error()));
            }

            if(full) {
                const auto metadata = reader.read_metadata();
                metadata_end = nova::profiler::tick();

                if(!metadata) [[unlikely]] {
                    return nova::err(std::format("read metadata failed: {}", metadata.error()));
                }

                checksum += static_cast<std::uint64_t>(metadata->patient.id.size());
                checksum += static_cast<std::uint64_t>(metadata->study.instance_uid.size());

                const auto info = reader.read_pixel_data_info();
                pixel_info_end = nova::profiler::tick();

                if(!info) [[unlikely]] {
                    return nova::err(std::format("read pixel info failed: {}", info.error()));
                }

                const auto pixels = reader.read_pixel_data(*info);
                pixel_data_end = nova::profiler::tick();

                if(!pixels) {
                    return nova::err(std::format("read pixel data failed", pixels.error()));
                }

                checksum += static_cast<std::uint64_t>(pixels->size());

                if(!pixels->empty()) {
                    checksum += std::to_integer<std::uint64_t>(pixels->front());
                    checksum += std::to_integer<std::uint64_t>(pixels->back());
                }
            }

            destroy_begin = nova::profiler::tick();
        }

        const auto total_end = nova::profiler::tick();
        const auto elapsed = nova::profiler::elapsed_ticks;

        const measurement result {
            .total = elapsed(total_begin, total_end),
            .construct = elapsed(total_begin, construct_end),
            .load = elapsed(construct_end, load_end),
            .metadata = full ? elapsed(load_end, metadata_end) : 0,
            .pixel_info = full ? elapsed(metadata_end, pixel_info_end) : 0,
            .pixel_data = full ? elapsed(pixel_info_end, pixel_data_end) : 0,
            .destroy = elapsed(destroy_begin, total_end),
            .checksum = checksum
        };

        if(record) {
            nova::profiler::record<"DICOM construct">(total_begin, construct_end);
            nova::profiler::record<"DICOM load">(construct_end, load_end);

            if(full) {
                nova::profiler::record<"DICOM metadata">(load_end, metadata_end);
                nova::profiler::record<"DICOM pixel info">(metadata_end, pixel_info_end);
                nova::profiler::record<"DICOM pixel access">(pixel_info_end, pixel_data_end);
            }
            nova::profiler::record<"DICOM destruction">(destroy_begin, total_end);
            nova::profiler::record<"DICOM total">(total_begin, total_end);
        }

        return result;
    }

    void print_stats(
        const std::string_view name, 
        const std::vector<measurement>& samples,
        const std::uint64_t measurement::* field
    ) {
        std::vector<double> values;
        values.reserve(samples.size());

        for(const auto& sample : samples) {
            values.push_back(nova::profiler::ticks_to_nanoseconds(sample.*field) / 1000.0);
        }

        std::ranges::sort(values);

        const auto count = values.size();
        const auto mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(count);
        const auto p50 = values[(count * 50 + 99) / 100 - 1];
        const auto p95 = values[(count * 95 + 99) / 100 - 1];

        std::println(
            "{:<22}{:>12.3f}{:>12.3f}{:>12.3f}{:>12.3f}{:>12.3f}",
            name,
            values.front(),
            p50,
            mean,
            p95,
            values.back()
        );
    }
}

int main(const int argc, const char* const argv[]) {
    if(argc < 2) {
        std::println(stderr, "Usage: {} <file.dcm> [iterations] [--full]", argv[0]);
        return 2;
    }

    constexpr std::size_t warmup_count = 16;
    constexpr std::size_t max_iterations = 1'000'000;

    std::size_t iterations = 200;
    bool iterations_supplied = false;
    bool full = false;


    for(int i = 2; i < argc; ++i) {
        const std::string_view argument{argv[i]};

        if(argument == "--full") {
            full = true;
            continue;
        }

        if(iterations_supplied) {
            std::println(stderr, "Unexpected argument: {}", argument);
            return 2;
        }

        std::size_t parsed{};

        const auto [ptr, error] = std::from_chars(argument.data(), argument.data() + argument.size(), parsed);

        if(error != std::errc{} ||
           ptr != argument.data() + argument.size() ||
           parsed == 0 ||
           parsed > max_iterations) {
            std::println(stderr, "Invalid iteration count: {}", argument);
            return 2;
        }

        iterations = parsed;
        iterations_supplied = true;
    }

    const std::filesystem::path path{argv[1]};

    std::error_code file_error;
    const auto file_size = std::filesystem::file_size(path, file_error);

    if(file_error) {
        std::println(stderr, "Cannot access input: {}", file_error.message());
        return 2;
    }

    std::println(
        "Nova DICOM baseline | size={} bytes | iterations={} | warmups={} | mode={}",
        file_size,
        iterations,
        warmup_count,
        full ? "full" : "load-only"
    );

    for(std::size_t i = 0; i < warmup_count; ++i) {
        const auto result = measure(path, full, false);

        if(!result) {
            std::println(stderr, "Warmup failed: {}", result.error());
            return 1;
        }
    }

    std::vector<measurement> samples;
    samples.reserve(iterations);

    nova::profiler::start();

    for(std::size_t i = 0; i < iterations; ++i) {
        auto result = measure(path, full, true);

        if(!result) {
            nova::profiler::stop();
            std::println(stderr, "Iteration {} failed: {}", i, result.error());
            return 1;
        }

        samples.push_back(*result);
    }

    nova::profiler::stop();

    std::uint64_t checksum{};

    for(const auto& sample : samples) {
        checksum += sample.checksum;
    }

    std::println("\nLatency (microseconds)");
    std::println(
        "{:<22}{:>12}{:>12}{:>12}{:>12}{:>12}",
        "Stage", "Min", "P50", "Mean", "P95", "Max"
    );

    print_stats("Construct", samples, &measurement::construct);
    print_stats("DCMTK load", samples, &measurement::load);

    if(full)
    {
        print_stats("Metadata", samples, &measurement::metadata);
        print_stats("Pixel info", samples, &measurement::pixel_info);
        print_stats("Pixel access", samples, &measurement::pixel_data);
    }

    print_stats("Destruction", samples, &measurement::destroy);
    print_stats("Total", samples, &measurement::total);

    std::println("\nDiagnostic checksum: {}\n", checksum);

    nova::profiler::report();

    return 0;
}
