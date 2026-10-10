#include "dicom_reader.h"
#include "core/result.h"
#include "dicom.h"
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

    template<>
    struct format_type_mapper<pixel_sample_format::u32> {
        using Type = uint32_t;
    };

    template<>
    struct format_type_mapper<pixel_sample_format::s32> {
        using Type = int32_t;
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

        const auto assign_str = [](DcmElement& elem, std::string& target) {
            char* value = nullptr;

            if(elem.getString(value).good() && value != nullptr) {
                target.assign(value);
            }
        };

        const auto assign_numeric_str = [](DcmElement& elem, std::string& target) {
            OFString value;

            if(elem.getOFString(value, 0).good()) {
                target.assign(value.data(), value.size());
            }
        };

        auto* dataset = this->dataset();
        for(auto* obj = dataset->nextInContainer(nullptr); obj != nullptr; obj = dataset->nextInContainer(obj)) {
            if(!obj->isElement()) {
                continue;
            }

            auto& elem = *static_cast<DcmElement*>(obj);
            const auto& tag = elem.getTag();
            const auto key = (static_cast<std::uint32_t>(tag.getGroup()) << 16) | 
                             static_cast<std::uint32_t>(tag.getElement());

             switch(key) {
                // Patient
                case 0x00100010:
                    assign_str(elem, result.patient.name);
                    break;
                case 0x00100020:
                    assign_str(elem, result.patient.id);
                    break;

                case 0x00100030:
                    assign_str(elem, result.patient.birth_date);
                    break;

                case 0x00100032:
                    assign_str(elem, result.patient.birth_time);
                    break;

                case 0x00100040:
                    assign_str(elem, result.patient.sex);
                    break;

                // Study
                case 0x0020000D:
                    assign_str(elem, result.study.instance_uid);
                    break;

                case 0x00200010:
                    assign_str(elem, result.study.id);
                    break;

                case 0x00080020:
                    assign_str(elem, result.study.date);
                    break;

                case 0x00080030:
                    assign_str(elem, result.study.time);
                    break;

                case 0x00080050:
                    assign_str(elem, result.study.accession_number);
                    break;

                case 0x00081030:
                    assign_str(elem, result.study.description);
                    break;

                case 0x00080090:
                    assign_str(elem, result.study.referring_physician_name);
                    break;

                // Series
                case 0x0020000E:
                    assign_str(elem, result.series.instance_uid);
                    break;

                case 0x00080021:
                    assign_str(elem, result.series.date);
                    break;

                case 0x00080031:
                    assign_str(elem, result.series.time);
                    break;

                case 0x0008103E:
                    assign_str(elem, result.series.description);
                    break;

                case 0x00200011:
                    assign_str(elem, result.series.number);
                    break;

                case 0x00180015:
                    assign_str(elem, result.series.body_part_examined);
                    break;

                case 0x00081050:
                    assign_str(elem, result.series.performing_physician_name);
                    break;

                case 0x00280108:
                    assign_numeric_str(elem, result.series.smallest_pixel_value);
                    break;

                case 0x00280109:
                    assign_numeric_str(elem, result.series.largest_pixel_value);
                    break;

                case 0x00080060: {
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

            if (static_cast<std::size_t>(count) < expected_sample_count) {
                nova::logger::error("pixel data shorter than expected");
                return nova::err();
            }

            return std::as_bytes(std::span<const  U>{src, expected_sample_count});
        };

        if constexpr (std::is_same_v<T, std::uint8_t>) {
            return read_array(&DcmItem::findAndGetUint8Array);
        }
        else if constexpr (std::is_same_v<T, std::uint16_t>) {
            return read_array(&DcmItem::findAndGetUint16Array);
        }
        else if constexpr (std::is_same_v<T, std::int16_t>) {
            return read_array(&DcmItem::findAndGetUint16Array);
        }
        else if constexpr (std::is_same_v<T, std::uint32_t>) {
            return read_array(&DcmItem::findAndGetUint32Array);
        }
        else if constexpr (std::is_same_v<T, std::int32_t>) {
            return read_array(&DcmItem::findAndGetSint32Array);
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

        const auto expected_sample_count =
            info.pixel_count() * static_cast<std::size_t>(info.samples_per_pixel);

        switch (info.format) {
            case pixel_sample_format::u8:
                return read_pixel_data<pixel_sample_format::u8>(expected_sample_count);
            case pixel_sample_format::u16:
                return read_pixel_data<pixel_sample_format::u16>(expected_sample_count);
            case pixel_sample_format::s16:
                return read_pixel_data<pixel_sample_format::s16>(expected_sample_count);
            case pixel_sample_format::u32:
                return read_pixel_data<pixel_sample_format::u32>(expected_sample_count);
            case pixel_sample_format::s32:
                return read_pixel_data<pixel_sample_format::s32>(expected_sample_count);
            default:
                UNREACHABLE();
        }
    }

    
    [[nodiscard]] nova::result<pixel_data_info> read_pixel_data_info() const {
        if(!is_loaded()) [[unlikely]] {
            return nova::err(std::string{"No DICOM file loaded"});
        }

        pixel_data_info info{};
        info.dims.frames = 1;

        std::uint16_t pixel_representation{};
        std::string_view photometric_value{};

        bool has_rows = false;
        bool has_columns = false;
        bool has_samples = false;
        bool has_bits = false;
        bool has_representation = false;
        bool has_photometric = false;

        const auto get_uint16 = [](DcmElement& element, std::uint16_t& value) noexcept {
            return element.getUint16(value).good();
        };

        const auto get_string = [](DcmElement& element, std::string_view& value) noexcept {
            char* data = nullptr;
            Uint32 length = 0;

            if(element.getString(data, length).bad() || data == nullptr) {
                return false;
            }

            value = std::string_view{data, length};

            while(!value.empty() && (value.front() == ' ' || value.front() == '\0')) {
                value.remove_prefix(1);
            }

            while(!value.empty() && (value.back() == ' ' || value.back() == '\0')) {
                value.remove_suffix(1);
            }

            return !value.empty();
        };

        const auto get_frames = [&](DcmElement& element, std::uint32_t& frames) noexcept {
            std::string_view value;

            if(!get_string(element, value)) {
                return false;
            }

            if(value.starts_with('+')) {
                value.remove_prefix(1);
            }

            const auto [ptr, error] = std::from_chars(
                value.data(),
                value.data() + value.size(),
                frames
            );

            return error == std::errc{} && ptr == value.data() + value.size() && frames > 0;
        };

        auto* dataset = this->dataset();

        for(auto* obj = dataset->nextInContainer(nullptr); obj != nullptr; obj = dataset->nextInContainer(obj)) {
            if(!obj->isElement()) {
                continue;
            }

            auto& element = *static_cast<DcmElement*>(obj);
            const auto& tag = element.getTag();

            const auto key =
                (static_cast<std::uint32_t>(tag.getGroup()) << 16) |
                static_cast<std::uint32_t>(tag.getElement());

            switch(key) {
                case 0x00280002:
                    has_samples = get_uint16(element, info.samples_per_pixel);
                    break;

                case 0x00280004:
                    has_photometric = get_string(element, photometric_value);
                    break;

                case 0x00280006:
                    if(!get_uint16(element, info.planar_configuration)) {
                        return nova::err(std::string{"Invalid PlanarConfiguration"});
                    }
                    break;

                case 0x00280008:
                    if(!get_frames(element, info.dims.frames)) {
                        return nova::err(std::string{"Invalid NumberOfFrames"});
                    }
                    break;

                case 0x00280010: {
                    std::uint16_t value{};
                    has_rows = get_uint16(element, value);
                    info.dims.height = value;
                    break;
                }

                case 0x00280011: {
                    std::uint16_t value{};
                    has_columns = get_uint16(element, value);
                    info.dims.width = value;
                    break;
                }

                case 0x00280100:
                    has_bits = get_uint16(element, info.bits_allocated);
                    break;

                case 0x00280101:
                    if(!get_uint16(element, info.bits_stored)) {
                        return nova::err(std::string{"Invalid BitsStored"});
                    }
                    break;

                case 0x00280102:
                    if(!get_uint16(element, info.high_bit)) {
                        return nova::err(std::string{"Invalid HighBit"});
                    }
                    break;

                case 0x00280103:
                    has_representation = get_uint16(element, pixel_representation);
                    break;

                default:
                    break;
            }
        }

        if(!has_rows || !has_columns || !has_samples || !has_bits || !has_representation || !has_photometric) {
            return nova::err(std::string{"Missing required DICOM image attributes"});
        }

        if(info.dims.width == 0 || info.dims.height == 0 ||
        info.dims.frames == 0 || info.samples_per_pixel == 0) {
            return nova::err(std::string{"Invalid image dimensions"});
        }

        if(pixel_representation > 1) {
            return nova::err(std::string{"Invalid PixelRepresentation"});
        }

        if(info.bits_stored == 0 ||
        info.bits_stored > info.bits_allocated ||
        info.high_bit >= info.bits_allocated ||
        static_cast<std::uint32_t>(info.high_bit) + 1 < info.bits_stored) {
            return nova::err(std::string{"Invalid pixel bit layout"});
        }

        if(info.bits_allocated == 8 && pixel_representation == 1) {
            return nova::err(std::string{"Signed 8-bit samples are not supported"});
        }

        const auto photometric = resolve_photometric(photometric_value);

        if(!photometric) {
            return nova::err(photometric.error());
        }

        const auto format = resolve_pixel_sample_format(
            info.bits_allocated,
            pixel_representation
        );

        if(!format) {
            return nova::err(format.error());
        }

        info.photometric = *photometric;
        info.format = *format;

        constexpr std::size_t max_decoded_bytes = 256ULL * 1024ULL * 1024ULL;

        auto bytes = static_cast<std::size_t>(info.bits_allocated / 8);

        for(const auto factor : std::array<std::uint32_t, 4> {
            info.dims.width,
            info.dims.height,
            info.dims.frames,
            info.samples_per_pixel
        }) {
            if(factor == 0 || bytes > max_decoded_bytes / factor) {
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

            if(status.bad() || value == nullptr) {
                nova::logger::warn("Failed to read tag {}", magic_enum::enum_name(tag));
                return default_value;
            }
            return value;
        }
        else if constexpr (std::is_same_v<T, uint16_t>) {
            uint16_t value{};
            const auto[group, element] = resolve_dicom_tag(tag);
            const auto status = dataset->findAndGetUint16({group, element}, value);

            if(status.bad()) {
                nova::logger::warn("Failed to read tag {}", magic_enum::enum_name(tag));
                return default_value;
            }
            return value;
        }
        else if constexpr (std::is_same_v<T, int16_t>) {
            int16_t value{};
            const auto[group, element] = resolve_dicom_tag(tag);
            const auto status = dataset->findAndGetSint16({group, element}, value);

            if(status.bad()) {
                nova::logger::warn("Failed to read tag {}", magic_enum::enum_name(tag));
                return default_value;
            }
            return value;
        }
        else if constexpr (std::is_same_v<T, uint32_t>) {
            uint32_t value{};
            const auto[group, element] = resolve_dicom_tag(tag);
            const auto status = dataset->findAndGetUint32({group, element}, value);

            if(status.bad()) {
                nova::logger::warn("Failed to read tag {}", magic_enum::enum_name(tag));
                return default_value;
            }
            return value;
        }
        else if constexpr (std::is_same_v<T, int32_t>) {
            int32_t value{};
            const auto[group, element] = resolve_dicom_tag(tag);
            const auto status = dataset->findAndGetSint32({group, element}, value);

            if(status.bad()) {
                nova::logger::warn("Failed to read tag {}", magic_enum::enum_name(tag));
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
        else if constexpr (std::is_same_v<T, uint16_t>) {
            return pixel_reader_lambda(&DcmItem::findAndGetUint16Array);
        }
        else if constexpr (std::is_same_v<T, int16_t>) {
            return pixel_reader_lambda(&DcmItem::findAndGetUint16Array);
        }
        else if constexpr (std::is_same_v<T, uint32_t>) {
            return pixel_reader_lambda(&DcmItem::findAndGetUint32Array);
        }
        else if constexpr (std::is_same_v<T, int32_t>) {
            return pixel_reader_lambda(&DcmItem::findAndGetSint32Array);
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
