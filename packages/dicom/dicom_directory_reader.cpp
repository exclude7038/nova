#include "dicom_directory_reader.h"
#include "dicom.h"
#include "result.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>
#include <coroutine>

import core.profiler;
import cpu_scheduler;
import coro_parallel_foreach;
import nova.di.singleton;

namespace fs = std::filesystem;

namespace {
    [[nodiscard]] bool skip(const fs::path& path) {
        const auto filename = path.filename().string();
        if(filename.empty() || filename.front() == '.') {
            return true;
        }

        constexpr std::array<std::string_view,6> suffixes = {
            "~", ".tmp", ".bak", ".json", ".txt", ".zip"
        };
        const auto value = std::string_view{filename};
        return std::ranges::any_of(suffixes, [value](const std::string_view suffix) {
            return value.ends_with(suffix);
        });
    }

    [[nodiscard]] nova::result<std::vector<fs::path>> collect_files(
        const fs::path& root,
        const nova::dicom::directory_read_options& options
    ) {
        nova::profiler::scope<"DICOM discover files"> profile;
        std::error_code ec;

        if(!fs::is_directory(root, ec)) {
            return nova::err(std::format("Not a readable DICOM directory: {} ({})", root.string(), ec.message()));
        }

        std::vector<fs::path> paths;
        paths.reserve(250);

        const auto push_file = [&](const fs::directory_entry& entry) {
            if(paths.size() >= options.max_files) {
                return false;
            }
            paths.push_back(entry.path());
            return true;
        };

        if(options.recursive) {
            fs::recursive_directory_iterator it{root, fs::directory_options::none, ec};
            if(ec) {
                return nova::err(std::format("Cannot scan {}: {}", root.string(), ec.message()));
            }

            const fs::recursive_directory_iterator end{};
            while (it != end) {
                const auto& entry = *it;
                const auto status = entry.symlink_status(ec);
                if(ec) {
                    return nova::err(std::format("Cannot stat: {}: {}", entry.path().string(), ec.message()));
                }

                if(status.type() == fs::file_type::directory && skip(entry.path())) {
                    it.disable_recursion_pending();
                }
                else if(status.type() == fs::file_type::regular && !skip(entry.path())) {
                    if(!push_file(entry)) {
                        return nova::err(std::format("Failed to collect file {}", entry.path().string()));
                    }
                }

                it.increment(ec);
                if(ec) {
                    return nova::err(std::format("Directory traversal failed: {}", ec.message()));
                }
            }
        }
        else {
            fs::directory_iterator it{root, fs::directory_options::none, ec};
            if(ec) {
                return nova::err(std::format("Cannot scan {}: {}", root.string(), ec.message()));
            }

            const fs::directory_iterator end{};
            while(it != end) {
                const auto& entry = *it;
                const auto status = entry.symlink_status(ec);
                if(ec) {
                    return nova::err(std::format("Cannot stat: {}: {}", entry.path().string(), ec.message()));
                }

                if(status.type() == fs::file_type::regular && !skip(entry.path())) {
                    if(!push_file(entry)) {
                        return nova::err(std::format("Failed to collect file {}", entry.path().string()));
                    }
                }

                it.increment(ec);
                if(ec) {
                    return nova::err(std::format("Directory traversal failed: {}", ec.message()));
                }
            }
        }

        std::ranges::sort(paths);
        return paths;
    }
}

nova::result<nova::dicom::dicom_file> nova::dicom::read_file(const fs::path& path) {
    nova::profiler::scope<"DICOM complete file"> complete;
    dicom_reader reader;

    {
        nova::profiler::scope<"DICOM load"> load;
        auto result = reader.load(path);
        if(!result) [[unlikely]] {
            return nova::err(std::move(result.error()));
        }
    }

    nova::result<nova::dicom::metadata> attributes;
    {
        nova::profiler::scope<"DICOM metadata"> profile;
        attributes = reader.read_metadata();
        if(!attributes) [[unlikely]] {
            return nova::err(std::move(attributes.error()));
        }
    }

    nova::result<pixel_data_info> pixel_data_info;
    {
        nova::profiler::scope<"DICOM pixel info"> info;
        pixel_data_info = reader.read_pixel_data_info();
        if(!pixel_data_info) [[unlikely]] {
            return nova::err(std::move(pixel_data_info.error()));
        }
    }

    nova::result<std::span<const std::byte>> pixels;
    {
        nova::profiler::scope<"DICOM pixel access"> pixel_access;
        pixels = reader.read_pixel_data(pixel_data_info.value());
        if(!pixels) [[unlikely]] {
            return nova::err(std::move(pixels.error()));
        }
    }

    return dicom_file(
        path, std::move(reader), 
        std::move(attributes.value()), 
        pixel_data_info.value(), 
        pixels.value()
    );
}

nova::result<nova::dicom::directory_result> nova::dicom::directory_reader::read_directory(
    const fs::path& root,
    const directory_read_options& options
) {
    nova::profiler::scope<"DICOM read directory"> profile;

    if(options.max_files == 0) {
        return nova::err("max files must be greater than zero");
    }

    auto paths = collect_files(root, options);
    if(!paths) [[unlikely]] {
        return nova::err(std::move(paths.error()));
    }
    return read_files(paths.value(), options);
}

nova::result<nova::dicom::directory_result> nova::dicom::directory_reader::read_files(
    const std::span<const fs::path> paths,
    const directory_read_options& options
) {
    nova::profiler::scope<"DICOM read all files"> profile;

    if(paths.size() > options.max_files) {
        return nova::err("DICOM file count exceeds max_files");
    }

    if(options.grain_size == 0) {
        return nova::err("grain_size must be greater than zero");
    }

    directory_result result;
    if(paths.empty()) {
        return result;
    }

    try {
        auto& scheduler = nova::ioc::ioc().resolve<nova::cpu_scheduler<>>();
        std::vector<std::optional<nova::result<dicom_file>>> slots(paths.size());
        const auto indices = std::views::indices(paths.size());

        auto work = nova::coro::parallel_foreach(
            scheduler,
            indices,
            [&](const std::size_t index) {
                slots[index].emplace(nova::dicom::read_file(paths[index]));
            },
            nova::coro::parallel_foreach_options {
                .grain_size = options.grain_size,
                .min_parallel_size = 2
            }
        );
        scheduler.submit(std::move(work)).get();

        result.files.reserve(paths.size());

        for(std::size_t i = 0; i < slots.size(); ++i) {
            auto& slot = slots[i];
            if(!slot) [[unlikely]] {
                return nova::err("DICOM operation did not complete successfully");
            }

            if(!slot->has_value()) {
                if(!options.skip_invalid_files) {
                    return nova::err(std::format("{}: {}", paths[i].string(), slot->error()));
                }

                result.failures.push_back(file_failure{.path=paths[i], .message=std::move(slot->error())});
                continue;
            }

            result.files.push_back(std::move(slot->value()));
        }
        return result;
    }
    catch(const std::exception& e) {
        return nova::err(e.what());
    }
}
