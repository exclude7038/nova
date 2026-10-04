#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <filesystem>
#include <format>
#include <print>
#include <string_view>
#include "dicom_reader.h"
#include "logging/logger.h"

const std::filesystem::path test_data_dir = NOVA_TEST_DATA_DIR;

static void log_callback(const char* level, const char* msg, size_t msg_size) {
    const auto line = std::format("[{}] {}", level, std::string_view{msg, msg_size});
    std::println("{}", line);
}

TEST_CASE("Dicom reader") {
    nova::logger::set_log_callback(log_callback);
    nova::logger::init();

    nova::dicom::dicom_reader m_reader;
    SUCCEED();

    SECTION("Load") {
        SECTION("Invalid path should fail") {
            const auto result = m_reader.load("path_invalid");
            CHECK_FALSE(result.has_value());
        }
        SECTION("Valid path should succeed") {
            const std::filesystem::path dcm = test_data_dir/"CTHead1.dcm";
            const auto result = m_reader.load(dcm);

            CHECK(result.has_value());
        }
    }
    SECTION("Read metadata") {
        const std::filesystem::path dcm = test_data_dir/"CTHead1.dcm";
        const auto _ = m_reader.load(dcm);
        const auto metadata = m_reader.read_metadata();

        CHECK(metadata.has_value());
    }
    SECTION("Read pixelbuffer info") {
        const std::filesystem::path dcm = test_data_dir/"CTHead1.dcm";
        const auto _ = m_reader.load(dcm);
        const auto pixel_data = m_reader.read_pixel_data_info();

        CHECK(pixel_data.has_value());
    }
    SECTION("Read pixelbuffer") {
        const std::filesystem::path dcm = test_data_dir/"CTHead1.dcm";
        const auto _ = m_reader.load(dcm);
//        const auto pixel_data = m_reader.read_pixel_data();

 //       CHECK(pixel_data.has_value());
    }
}
