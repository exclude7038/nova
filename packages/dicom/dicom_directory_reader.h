#pragma once

#include "dicom.h"
#include "dicom_reader.h"
#include "result.h"
#include <bits/indirect.h>
#include <cstddef>
#include <filesystem>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace nova::dicom {
    class dicom_file {
    public:
        dicom_file(const dicom_file&) = delete;
        dicom_file& operator=(const dicom_file&) = delete;
        dicom_file(dicom_file&&) noexcept = default;
        dicom_file& operator=(dicom_file&&) noexcept = default;
        ~dicom_file() = default;

        [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }
        [[nodiscard]] const nova::dicom::metadata& metadata() const noexcept { return m_metadata; }
        [[nodiscard]] const pixel_data_info& pixel_info() const noexcept { return m_pixel_data_info; }

        // Returns a view to the underlying pixels. This view remains valis while this dicom_file owns the
        // underlying DICOM reader.
        [[nodiscard]] std::span<const std::byte> pixels() const noexcept { return m_pixels; }
    private:
        friend nova::result<dicom_file> read_file(const std::filesystem::path&);

        dicom_file(
            std::filesystem::path path,
            dicom_reader reader,
            nova::dicom::metadata metadata,
            pixel_data_info pixel_data_info,
            std::span<const std::byte> pixels
        ) noexcept
            :
            m_reader(std::move(reader)),
            m_path(std::move(path)),
            m_metadata(std::move(metadata)),
            m_pixel_data_info(pixel_data_info),
            m_pixels(pixels)
        {}

        dicom_reader m_reader;
        std::filesystem::path m_path;
        nova::dicom::metadata m_metadata;
        pixel_data_info m_pixel_data_info;
        std::span<const std::byte> m_pixels;
    };

    struct file_failure {
        std::filesystem::path path;
        std::string message;
    };

    struct directory_result {
        std::vector<dicom_file> files;
        std::vector<file_failure> failures;
    };

    struct directory_read_options final {
        bool recursive = true;
        bool skip_invalid_files = false;
        std::size_t max_files = 100'000;
        std::size_t grain_size = 1;
    };

    [[nodiscard]] nova::result<dicom_file> read_file(const std::filesystem::path& path);

    class directory_reader {
    public:
        [[nodiscard]] static nova::result<directory_result> read_files(
            std::span<const std::filesystem::path> paths,
            const directory_read_options& options = {}
        );

        [[nodiscard]] static nova::result<directory_result> read_directory(
            const std::filesystem::path& root,
            const directory_read_options& options = {}
        );
    };
}
