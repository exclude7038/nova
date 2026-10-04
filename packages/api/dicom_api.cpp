#include "dicom_api.h"
#include "dicom/dicom_reader.h"
#include "dicom/dicom.h"
#include <assert.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <magic_enum/magic_enum.hpp>
#include <memory>
#include <nlohmann/detail/macro_scope.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "core/binary_writer.h"

nova::ffi::dicom::dicom_api::dicom_api()
    :
    m_reader(std::make_unique<nova::dicom::dicom_reader>())
{}
nova::ffi::dicom::dicom_api::~dicom_api() = default;

void nova::ffi::dicom::dicom_api::load(const std::string& path) {
    DEBUG_ASSERT(m_reader != nullptr);

    const auto result = m_reader->load(path);
    if (!result) {
        throw std::runtime_error(std::format("Failed to load DICOM file {}", path));
    }
}

std::unique_ptr<std::vector<std::uint8_t>> nova::ffi::dicom::dicom_api::read_file() const {
    DEBUG_ASSERT(m_reader != nullptr);

    const auto metadata = m_reader->read_metadata();
    if (!metadata) {
        throw std::runtime_error("Failed to read DICOM metadata");
    }

    const auto pixel_info = m_reader->read_pixel_data_info();
    if (!pixel_info) {
        throw std::runtime_error("Failed to read DICOM pixel data info");
    }

    const auto pixel_view = m_reader->read_pixel_data(*pixel_info);
    if (!pixel_view) {
        throw std::runtime_error("Failed to read DICOM pixel data");
    }

    const auto reserve_size =
        std::size_t{4096} +
        sizeof(std::uint64_t) +
        pixel_view->size();

    binary_writer bw{reserve_size};
    bw.write_string(metadata->patient.name);
    bw.write_string(metadata->patient.id);
    bw.write_string(metadata->patient.birth_date);
    bw.write_string(metadata->patient.birth_time);
    bw.write_string(metadata->patient.sex);
    bw.write_string(metadata->study.instance_uid);
    bw.write_string(metadata->study.id);
    bw.write_string(metadata->study.date);
    bw.write_string(metadata->study.time);
    bw.write_string(metadata->study.accession_number);
    bw.write_string(metadata->study.description);
    bw.write_string(metadata->study.referring_physician_name);
    bw.write_string(metadata->series.instance_uid);
    bw.write_string(metadata->series.date);
    bw.write_string(metadata->series.time);
    bw.write_string(metadata->series.description);
    bw.write_string(metadata->series.number);
    bw.write_string(metadata->series.body_part_examined);
    bw.write_string(metadata->series.performing_physician_name);
    bw.write_string(metadata->series.smallest_pixel_value);
    bw.write_string(metadata->series.largest_pixel_value);
    bw.write_u8(std::to_underlying(metadata->series.modality));

    bw.write_u32(pixel_info->dims.width);
    bw.write_u32(pixel_info->dims.height);
    bw.write_u32(pixel_info->dims.frames);
    bw.write_u16(pixel_info->samples_per_pixel);
    bw.write_u16(pixel_info->bits_allocated);
    bw.write_u16(pixel_info->bits_stored);
    bw.write_u16(pixel_info->high_bit);
    bw.write_u16(pixel_info->planar_configuration);
    bw.write_u8(std::to_underlying(pixel_info->photometric));
    bw.write_u8(std::to_underlying(pixel_info->format));

    bw.write_u64(static_cast<std::uint64_t>(pixel_view->size()));
    bw.write_bytes(*pixel_view);

    return std::make_unique<std::vector<std::uint8_t>>(std::move(bw).take());
}

[[nodiscard]] std::unique_ptr<nova::ffi::dicom::dicom_api> nova::ffi::dicom::dicom_api_create() {
    return std::make_unique<dicom_api>();
}
