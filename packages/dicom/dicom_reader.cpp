#include "dicom_reader.h"
#include "core/result.h"
#include "dicom.h"
#include <array>
#include <assert.hpp>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dcmdata/dcdatset.h>
#include <dcmdata/dcdeftag.h>
#include <dcmdata/dcfilefo.h>
#include <dcmdata/dcitem.h>
#include <dcmdata/dctagkey.h>
#include <dcmdata/dcxfer.h>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include "logging/logger.h"
#include <format>
#include <magic_enum/magic_enum.hpp>
#include <memory>
#include <ofstd/ofcond.h>
#include <ofstd/oftypes.h>
#include <quill/bundled/fmt/format.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>
#include <dcmdata/dcistrmb.h>
#include <dcmdata/dcelem.h>
#include <dcmdata/dcstack.h>

using namespace nova::dicom;
namespace fs = std::filesystem;

namespace {
    template<class T>
    concept valid_tag_type =
        std::is_same_v<T, std::string> ||
        std::is_same_v<T, uint16_t> ||
        std::is_same_v<T, int16_t>  ||
        std::is_same_v<T, uint32_t> ||
        std::is_same_v<T, int32_t>;

    template<class T>
    concept valid_pixel_data_type =
        std::is_same_v<T, uint8_t> ||
        std::is_same_v<T, uint16_t> ||
        std::is_same_v<T, int16_t>  ||
        std::is_same_v<T, uint32_t> ||
        std::is_same_v<T, int32_t>;

    template<pixel_sample_format Format>
    struct format_type_mapper;

    template<>

    struct format_type_mapper<pixel_sample_format::u8> {
        using Type = uint8_t;
    };

    template<>
    struct format_type_mapper<pixel_sample_format::u16> {
        using Type = uint16_t;
    };

    template<>
    struct format_type_mapper<pixel_sample_format::s16> {
        using Type = int16_t;
    };

    template<pixel_sample_format Format>
    using format_type_mapper_t = format_type_mapper<Format>::Type;

    class file_descriptor final {
    public:
        explicit file_descriptor(const int fd) noexcept 
            :
            m_fd(fd)
        {}

        file_descriptor(const file_descriptor&) = delete;
        file_descriptor& operator=(const file_descriptor&) = delete;
        file_descriptor(file_descriptor&&) = delete;
        file_descriptor& operator=(file_descriptor&&) = delete;

        ~file_descriptor() noexcept {
            if(m_fd >= 0) {
                auto _ = ::close(m_fd);
            }
        }

        [[nodiscard]] int native_handle() const noexcept {
            return m_fd;
        }
    private:
        int m_fd{-1};
    };

    struct input_buffer final {
        std::unique_ptr<std::byte[]> data;
        std::size_t size{};
    };

    [[nodiscard]] nova::result<input_buffer> read_file(const fs::path& path) {
        constexpr std::size_t max_file_size = 256ull * 1024ull * 1024ull;

        const file_descriptor file{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};

        if(file.native_handle() < 0) [[unlikely]] {
            return nova::err(std::format("Failed to open DICOM: errno={}", errno));
        }

        struct stat info{};

        if(::fstat(file.native_handle(), &info) != 0) [[unlikely]] {
            return nova::err(std::format("Failed to stat DICOM: errno={}", errno));
        }

        if(!S_ISREG(info.st_mode)) [[unlikely]] {
            return nova::err(std::string{"DICOM input is not a regular file"});
        }

        if(info.st_size <= 0 ||
            static_cast<std::uintmax_t>(info.st_size) > max_file_size) [[unlikely]] {
            return nova::err(std::string{"DICOM input exceeds the configured size limit"});
        }

        const auto size = static_cast<std::size_t>(info.st_size);

        if((size & 1U) != 0) [[unlikely]] {
            return nova::err(std::string{"DICOM input length must be even"});
        }

        auto data = std::make_unique_for_overwrite<std::byte[]>(size);
        std::size_t offset = 0;

        while(offset < size) {
            const auto bytes = ::read(file.native_handle(), data.get() + offset, size - offset);

            if(bytes < 0) [[unlikely]] {
                if(errno == EINTR) {
                    continue;
                }
                return nova::err(std::format("DICOM read failed: errno={}", errno));
            }

            if(bytes == 0) [[unlikely]] {
                return nova::err(std::string{"Unexpected end of DICOM file"});
            }

            offset += static_cast<std::size_t>(bytes);
        }

        return input_buffer {
            .data = std::move(data),
            .size = size
        };
    }
};

class dicom_reader::impl final {
public:
    [[nodiscard]] nova::result<nova::ok> load(const std::filesystem::path& path) noexcept {
        try {
            clear();

            auto buffer = read_file(path);
            if(!buffer) [[unlikely]] {
                return nova::err(std::move(buffer.error()));
            }

            auto file = std::make_unique<DcmFileFormat>();
            DcmInputBufferStream stream;
            stream.setBuffer(buffer->data.get(), static_cast<offile_off_t>(buffer->size));
            stream.setEos();

            if(stream.status().bad()) [[unlikely]] {
                return nova::err(std::string{stream.status().text()});
            }

            file->transferInit();
            const auto status = file->read(stream, EXS_Unknown, EGL_noChange, static_cast<Uint32>(buffer->size));
            file->transferEnd();

            if(status.bad()) [[unlikely]] {
                return nova::err(std::string{status.text()});
            }

            m_file_path = path;
            m_file = std::move(file);

            return nova::ok{};
        }
        catch(const std::exception& e) {
            return nova::err(e.what());
        }
        catch(...) {
           return nova::err(std::string{"Unknown DICOM loading failure"});
        }
    }

    [[nodiscard]] DcmDataset* dataset() const noexcept {
        DEBUG_ASSERT(m_file != nullptr);
        return m_file->getDataset();
    }

    [[nodiscard]] nova::result<metadata> read_metadata() const {
        if(!is_loaded()) [[unlikely]] {
            logger::error("unable to read metadata. Reason: no dicom file loaded");
            return nova::err();
        }

        metadata result{};

        constexpr std::size_t max_metadata_value_length = 4096;
        const auto assign_str = [](DcmElement& elem, std::string& target) {
             if(elem.getLength() > max_metadata_value_length) [[unlikely]] {
                return;
            }

            OFString value;

            if(elem.getOFString(value, 0).good() && value.size() <= max_metadata_value_length) {
                target.assign(value.data(), value.size());
            }
        };

        const auto assign_numeric_str = assign_str;

        auto* dataset = this->dataset();
        for(auto* obj = dataset->nextInContainer(nullptr); obj != nullptr; obj = dataset->nextInContainer(obj)) {
            if(!obj->isElement()) {
                continue;
            }

            auto& elem = *static_cast<DcmElement*>(obj);
            const auto& tag = elem.getTag();
            const auto key = static_cast<dicom_tag>(
                (static_cast<std::uint32_t>(tag.getGroup()) << 16) | 
                static_cast<std::uint32_t>(tag.getElement())
            );

             switch(key) {
                case dicom_tag::patient_name:
                    assign_str(elem, result.patient.name);
                    break;
                case dicom_tag::patient_id:
                    assign_str(elem, result.patient.id);
                    break;
                case dicom_tag::patient_birth_date:
                    assign_str(elem, result.patient.birth_date);
                    break;
                case dicom_tag::patient_birth_time:
                    assign_str(elem, result.patient.birth_time);
                    break;
                case dicom_tag::patient_sex:
                    assign_str(elem, result.patient.sex);
                    break;
                case dicom_tag::study_instance_uid:
                    assign_str(elem, result.study.instance_uid);
                    break;
                case dicom_tag::study_id:
                    assign_str(elem, result.study.id);
                    break;
                case dicom_tag::study_date:
                    assign_str(elem, result.study.date);
                    break;
                case dicom_tag::study_time:
                    assign_str(elem, result.study.time);
                    break;
                case dicom_tag::study_accession_number:
                    assign_str(elem, result.study.accession_number);
                    break;
                case dicom_tag::study_description:
                    assign_str(elem, result.study.description);
                    break;
                case dicom_tag::study_referring_physician_name:
                    assign_str(elem, result.study.referring_physician_name);
                    break;
                case dicom_tag::series_instance_uid:
                    assign_str(elem, result.series.instance_uid);
                    break;
                case dicom_tag::series_date:
                    assign_str(elem, result.series.date);
                    break;
                case nova::dicom::dicom_tag::series_time:
                    assign_str(elem, result.series.time);
                    break;
                case dicom_tag::series_description:
                    assign_str(elem, result.series.description);
                    break;
                case dicom_tag::series_number:
                    assign_str(elem, result.series.number);
                    break;
                case dicom_tag::series_body_part_examined:
                    assign_str(elem, result.series.body_part_examined);
                    break;
                case dicom_tag::series_performing_physician_name:
                    assign_str(elem, result.series.performing_physician_name);
                    break;
                case dicom_tag::series_smallest_pixel_value:
                    assign_numeric_str(elem, result.series.smallest_pixel_value);
                    break;
                case dicom_tag::series_largest_pixel_value:
                    assign_numeric_str(elem, result.series.largest_pixel_value);
                    break;
                case dicom_tag::series_modality: {
                    char* value = nullptr;

                    if(elem.getString(value).good() && value != nullptr) {
                        const auto resolved = resolve_modality(std::string_view{value});

                        if(resolved) {
                            result.series.modality = *resolved;
                        }
                    }
                    break;
                }
                default:
                    break;
            }
        }
        return result;
    }

    template<pixel_sample_format sampleFormat>
    [[nodiscard]] nova::result<std::span<const std::byte>> read_pixel_data(
        std::size_t expected_sample_count
    ) const noexcept {
        auto* dataset = this->dataset();
        DEBUG_ASSERT(dataset != nullptr);

        using T = ::format_type_mapper_t<sampleFormat>;

        const auto read_array = [&]<class U>(pixel_reader_fnc_ptr<U> reader) noexcept 
            -> nova::result<std::span<const std::byte>> {
            DEBUG_ASSERT(reader != nullptr);

            const U* src = nullptr;
            unsigned long count = 0;

            const auto status = (dataset->*reader)(
                DCM_PixelData,
                src,
                &count,
                OFFalse
            );

            if (status.bad() || src == nullptr) {
                nova::logger::error("Failed to read dicom pixel data: {}", status.text());
                return nova::err();
            }

            const auto padded_count = expected_sample_count + (std::is_same_v<U, std::uint8_t> 
                ? (expected_sample_count & 1U) 
                : 0U);

            if(static_cast<std::size_t>(count) != padded_count) [[unlikely]] {
                return nova::err(std::string{"DICOM pixel data length does not match image dimensions"});
            }

            return std::as_bytes(std::span<const  U>{src, expected_sample_count});
        };

        if constexpr (std::is_same_v<T, std::uint8_t>) {
            const auto buffer = read_array(&DcmItem::findAndGetUint8Array);
            if(buffer) {
                return buffer;
            }

             // 8 bit pixels may also be stored using OW.
             const std::uint16_t* words = nullptr;
            unsigned long count = 0;

            const auto status = dataset->findAndGetUint16Array(
                DCM_PixelData,
                words,
                &count,
                OFFalse
            );

            if(status.bad() || words == nullptr) [[unlikely]] {
                return nova::err(std::string{"Cannot access DICOM pixel data"});
            }

            const auto padded_bytes = expected_sample_count + (expected_sample_count & 1U);

            if(static_cast<std::size_t>(count) != padded_bytes / sizeof(std::uint16_t)) [[unlikely]] {
                return nova::err(std::string{"Invalid OW pixel data length"});
            }

            return std::as_bytes(std::span<const std::uint16_t>{words, count}).first(expected_sample_count);
        }
        else if constexpr(std::is_same_v<T, std::uint16_t> || std::is_same_v<T, std::int16_t>) {
            return read_array(&DcmItem::findAndGetUint16Array);
        }
        else {
            UNREACHABLE();
        }
    }

    [[nodiscard]] nova::result<std::span<const std::byte>> read_pixel_data(const pixel_data_info& info) const noexcept {
        if (!is_loaded()) [[unlikely]] {
            logger::error("unable to read pixeldata. Reason: no dicom file loaded");
            return nova::err();
        }

        const DcmXfer transfer_syntax{dataset()->getOriginalXfer()};
        const auto valid_transfer_syntax = transfer_syntax.usesNativeFormat() &&
                                   !transfer_syntax.isPixelDataCompressed() &&
                                   !transfer_syntax.isDatasetCompressed(); 

        if(!valid_transfer_syntax) [[unlikely]] {
            return nova::err(std::string{"Unsupported DICOM transfer syntax"});
        }

        std::size_t bytes_per_sample{};
        switch(info.format) {
            case pixel_sample_format::u8:
                bytes_per_sample = 1;
                break;
            case pixel_sample_format::u16:
            case pixel_sample_format::s16:
                bytes_per_sample = 2;
                break;
            case pixel_sample_format::u32:
            case pixel_sample_format::s32:
                bytes_per_sample = 4;
                break;
            default:
                return nova::err(std::string{"Invalid pixel sample format"});
        }

        if(info.bits_allocated != bytes_per_sample * 8) [[unlikely]] {
            return nova::err(std::string{"Inconsistent pixel bit depth"});
        }

        constexpr std::size_t max_decoded_bytes = 256ULL * 1024ULL * 1024ULL;
        const auto max_samples = max_decoded_bytes / bytes_per_sample;
        std::size_t expected_sample_count = 1;

        const auto factors = std::array<std::size_t, 4> { 
            info.dims.width,
            info.dims.height,
            info.dims.frames,
            info.samples_per_pixel
        };

        for(const auto factor : factors) {
            if(factor == 0 || expected_sample_count > max_samples / factor) [[unlikely]] {
                return nova::err(std::string{"DICOM pixel dimensions exceed limits"});
            }

            expected_sample_count *= factor;
        }

        switch(info.format) {
            case pixel_sample_format::u8:
                return read_pixel_data<pixel_sample_format::u8>(expected_sample_count);
            case pixel_sample_format::u16:
                return read_pixel_data<pixel_sample_format::u16>(expected_sample_count);
            case pixel_sample_format::s16:
                return read_pixel_data<pixel_sample_format::s16>(expected_sample_count);
            default:
                return nova::err(std::string{"Invalid pixel sample format"});
        }      
    }

    [[nodiscard]] nova::result<pixel_data_info> read_pixel_data_info() const {
        if(!is_loaded()) [[unlikely]] {
            return nova::err(std::string{"No DICOM file loaded"});
        }

        auto* dataset = this->dataset();

        const auto read_u16 = [dataset](dicom_tag tag, std::uint16_t& value) {
            return dataset->findAndGetUint16(to_dcm_tag(tag), value).good();
        };

        const auto read_string = [dataset](dicom_tag tag, OFString& value) {
            return dataset->findAndGetOFString(to_dcm_tag(tag), value).good() && !value.empty();
        };

        pixel_data_info info{};

        std::uint16_t rows{};
        std::uint16_t columns{};
        std::uint16_t pixel_representation{};
        OFString photometric_value;

        const bool valid_attributes =
            read_u16(dicom_tag::rows, rows) &&
            read_u16(dicom_tag::columns, columns) &&
            read_u16(dicom_tag::samples_per_pixel, info.samples_per_pixel) &&
            read_u16(dicom_tag::bits_allocated, info.bits_allocated) &&
            read_u16(dicom_tag::bits_stored, info.bits_stored) &&
            read_u16(dicom_tag::high_bit, info.high_bit) &&
            read_u16(dicom_tag::pixel_representation, pixel_representation) &&
            read_string(dicom_tag::photometric_interpretation, photometric_value);

        if(!valid_attributes) [[unlikely]] {
            return nova::err(std::string{"Missing or invalid required DICOM image attributes"});
        }

        info.dims.width = columns;
        info.dims.height = rows;
        info.dims.frames = 1;

        if(dataset->tagExists(to_dcm_tag(dicom_tag::number_of_frames))) {
            OFString value;

            if(!read_string(dicom_tag::number_of_frames, value)) [[unlikely]] {
                return nova::err(std::string{"Invalid number of frames"});
            }

            std::string_view frames{value.data(), value.size()};

            if(frames.starts_with('+')) {
                frames.remove_prefix(1);
            }

            const auto [ptr, error] = std::from_chars(
                frames.data(),
                frames.data() + frames.size(),
                info.dims.frames
            );

            if(error != std::errc{} || ptr != frames.data() + frames.size() || info.dims.frames == 0) [[unlikely]] {
                return nova::err(std::string{"Invalid number of frames"});
            }
        }

        if(info.samples_per_pixel > 1) {
            if(!read_u16(dicom_tag::planar_configuration, info.planar_configuration) || 
                info.planar_configuration > 1) [[unlikely]]
            {
                return nova::err(std::string{"Invalid planar configuration"});
            }
        }

        if(info.dims.width == 0 || info.dims.height == 0 || info.samples_per_pixel == 0) [[unlikely]] {
            return nova::err(std::string{"Invalid image dimensions"});
        }

        if(pixel_representation > 1) [[unlikely]] {
            return nova::err(std::string{"Invalid pixel representation"});
        }

        if(info.bits_allocated != 8 && info.bits_allocated != 16) [[unlikely]] {
            return nova::err(std::string{"Unsupported bits allocated"});
        }

        if(info.bits_stored == 0 || info.bits_stored > info.bits_allocated ||
           static_cast<std::uint32_t>(info.high_bit) + 1 != info.bits_stored) [[unlikely]] 
        {
            return nova::err(std::string{"Invalid pixel bit layout"});
        }

        if(info.bits_allocated == 8 && pixel_representation == 1) [[unlikely]] {
            return nova::err(std::string{"Signed 8-bit samples are not supported"});
        }

        const auto photometric = resolve_photometric(
            std::string_view{photometric_value.data(), photometric_value.size()}
        );

        if(!photometric) [[unlikely]] {
            return nova::err(photometric.error());
        }

        const auto format = resolve_pixel_sample_format(info.bits_allocated, pixel_representation);

        if(!format) [[unlikely]] {
            return nova::err(format.error());
        }

        info.photometric = *photometric;
        info.format = *format;

        constexpr std::size_t max_decoded_bytes = 256ULL * 1024ULL * 1024ULL;

        auto bytes = static_cast<std::size_t>(info.bits_allocated / 8);
        const auto factors = std::array<std::size_t, 4> {
            info.dims.width,
            info.dims.height,
            info.dims.frames,
            info.samples_per_pixel
        };

        for(const auto factor : factors) {
            if(factor == 0 || bytes > max_decoded_bytes / factor) [[unlikely]] {
                return nova::err(std::string{"DICOM pixel size exceeds limits"});
            }
            bytes *= factor;
        }
        return info;
    }
    void clear() noexcept {
        m_file.reset();
        m_file_path.clear();
    }

    [[nodiscard]] bool is_loaded() const noexcept {
        return m_file != nullptr;
    }

    [[nodiscard]] const std::filesystem::path& file_path() const noexcept {
        return m_file_path;
    }
private:
    template<class T = std::string>
    requires valid_tag_type<T>
    [[nodiscard]] T read_tag(dicom_tag tag, const T& default_value = {}) const noexcept {
        auto* dataset = this->dataset();
        DEBUG_ASSERT(dataset != nullptr);

        if constexpr (std::is_same_v<T, std::string>) {
            const char* value = nullptr;
            const auto[group, element] = resolve_dicom_tag(tag);
            const auto status = dataset->findAndGetString({group, element}, value);

            const auto key = std::to_underlying(tag);
            if(status.bad() || value == nullptr) {
                nova::logger::warn(
                    "Failed to read tag ({:04X},{:04X})",
                    key >> 16u,
                    key & 0xffffu
                );
                return default_value;
            }
            return value;
        }
        else if constexpr (std::is_same_v<T, uint16_t>) {
            uint16_t value{};
            const auto[group, element] = resolve_dicom_tag(tag);
            const auto status = dataset->findAndGetUint16({group, element}, value);

            const auto key = std::to_underlying(tag);
            if(status.bad()) {
                nova::logger::warn(
                    "Failed to read tag ({:04X},{:04X})",
                    key >> 16u,
                    key & 0xffffu
                );
                return default_value;
            }
            return value;
        }
        else if constexpr (std::is_same_v<T, int16_t>) {
            int16_t value{};
            const auto[group, element] = resolve_dicom_tag(tag);
            const auto status = dataset->findAndGetSint16({group, element}, value);

            const auto key = std::to_underlying(tag);
            if(status.bad()) {
                nova::logger::warn(
                    "Failed to read tag ({:04X},{:04X})",
                    key >> 16u,
                    key & 0xffffu
                );
                return default_value;
            }
            return value;
        }
        else if constexpr (std::is_same_v<T, uint32_t>) {
            uint32_t value{};
            const auto[group, element] = resolve_dicom_tag(tag);
            const auto status = dataset->findAndGetUint32({group, element}, value);
            
            const auto key = std::to_underlying(tag);
            if(status.bad()) {
                nova::logger::warn(
                    "Failed to read tag ({:04X},{:04X})",
                    key >> 16u,
                    key & 0xffffu
                );
                return default_value;
            }
            return value;
        }
        else if constexpr (std::is_same_v<T, int32_t>) {
            int32_t value{};
            const auto[group, element] = resolve_dicom_tag(tag);
            const auto status = dataset->findAndGetSint32({group, element}, value);

            const auto key = std::to_underlying(tag);
            if(status.bad()) { 
                nova::logger::warn(
                    "Failed to read tag ({:04X},{:04X})",
                    key >> 16u,
                    key & 0xffffu
                );
                return default_value;
            }
            return value;
        }
        else {
            UNREACHABLE();
        }
    }

    template<class T>
    using pixel_reader_fnc_ptr = OFCondition(DcmItem::*)(const DcmTagKey&, const T*& value, unsigned long*, const OFBool);

    template<pixel_sample_format sampleFormat>
    [[nodiscard]] nova::result<std::vector<std::uint8_t>> read_pixel_buffer(size_t expected_count) const noexcept {
        auto* dataset = this->dataset();
        DEBUG_ASSERT(dataset != nullptr);

        using T = ::format_type_mapper_t<sampleFormat>;

        const auto pixel_reader_lambda = [&dataset, &expected_count]<class Type>(pixel_reader_fnc_ptr<Type> reader) noexcept -> nova::result<std::vector<std::uint8_t>> {
            DEBUG_ASSERT(reader != nullptr);

            const Type* src = nullptr;
            uint64_t count = 0;
            const auto status = (dataset->*reader)(DCM_PixelData, src, &count, OFFalse);

            if(status.bad() || src == nullptr) {
                nova::logger::error("Failed to read dicom pixel data: {}", status.text());
                return nova::err();
            }

            if(count < expected_count) {
                nova::logger::error("pixel data shorter than expected");
                return nova::err();
            }

            std::vector<std::uint8_t> bytes(expected_count * sizeof(T));
            std::memcpy(bytes.data(), src, bytes.size());
            return bytes;
        };

        if constexpr (std::is_same_v<T, uint8_t>) {
            return pixel_reader_lambda(&DcmItem::findAndGetUint8Array);
        }
        else if constexpr (std::is_same_v<T, uint16_t> || std::is_same_v<T, int16_t>) {
            return pixel_reader_lambda(&DcmItem::findAndGetUint16Array);
        }
        else {
            UNREACHABLE();
        }
    }

    [[nodiscard]] static nova::result<photometric_interpretation> resolve_photometric(std::string_view value) noexcept {
        using enum photometric_interpretation;

        if(value == "MONOCHROME1") { return { monochrome1 }; }
        if(value == "MONOCHROME2") { return { monochrome2 }; }
        if(value == "PALETTE COLOR") { return { palette_color }; }
        if(value == "RGB") { return { rgb }; }
        if(value == "HSV") { return { hsv }; }
        if(value == "ARGB") { return { argb }; }
        if(value == "CMYK") { return { cmyk }; }
        if(value == "YBR_FULL") { return { ybr_full }; }
        if(value == "YBR_FULL_422") { return { ybr_full_422 }; }
        if(value == "YBR_PARTIAL_422") { return { ybr_partial_422 }; }
        if(value == "YBR_PARTIAL_420") { return { ybr_partial_420 }; }
        if(value == "YBR_ICT") { return { ybr_ict }; }
        if(value == "YBR_RCT") { return { ybr_rct }; }

        return nova::err("failed to resolve photometric interpretation from dicom file");
    }

   [[nodiscard]] static nova::result<pixel_sample_format> resolve_pixel_sample_format(uint16_t bits_allocated, uint16_t pixel_representation) {
       if(bits_allocated == 1) {
           return nova::err("Failed to resolve pixel sample format: 1 bit images are not supported");
       }

       if(bits_allocated == 8) {
           return pixel_sample_format::u8;
       }

       if(bits_allocated == 16) {
           return pixel_representation == 0
               ? pixel_sample_format::u16
               : pixel_sample_format::s16;
       }

       return nova::err(
           std::format(
               "Failed to resolve  pixel sample forma: Unsupported format. bits allocated={}, pixel representation={}",
               bits_allocated,
               pixel_representation
           )
       );
   }

    std::unique_ptr<DcmFileFormat> m_file{nullptr};
    std::filesystem::path m_file_path;
};

dicom_reader::dicom_reader() = default;
dicom_reader::dicom_reader(dicom_reader&&) noexcept = default;
dicom_reader& dicom_reader::operator=(dicom_reader&&) noexcept = default;
dicom_reader::~dicom_reader() = default;

nova::result<nova::ok> dicom_reader::load(const std::filesystem::path& path) noexcept {
    return m_impl->load(path);
}

nova::result<metadata> dicom_reader::read_metadata() const noexcept {
    return m_impl->read_metadata();
}

nova::result<std::span<const std::byte>> dicom_reader::read_pixel_data(const pixel_data_info& info) const noexcept {
    return m_impl->read_pixel_data(info);
}

nova::result<pixel_data_info> dicom_reader::read_pixel_data_info() const noexcept {
    return m_impl->read_pixel_data_info();
}
