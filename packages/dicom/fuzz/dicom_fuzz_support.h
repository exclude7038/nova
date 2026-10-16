#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace nova::dicom::fuzz {
    inline constexpr std::size_t max_fuzz_input = 1024uz * 1024;
    inline constexpr std::size_t max_decoded_bytes = 256uz * 1024 * 1024;
    inline constexpr std::string_view native_syntax = "1.2.840.10008.1.2.1";
    inline constexpr std::string_view instance_uid = "1.2.826.0.1.3680043.10.543.1";

    inline void append_u16(std::string& bytes, std::uint16_t value) {
        bytes.push_back(static_cast<char>(value & 0xffU));
        bytes.push_back(static_cast<char>(value >> 8));
    }

    inline void append_u32(std::string& bytes, std::uint32_t value) {
        append_u16(bytes, static_cast<std::uint16_t>(value));
        append_u16(bytes, static_cast<std::uint16_t>(value >> 16));
    }

    [[nodiscard]] inline std::string us(std::uint16_t value) {
        std::string bytes;
        append_u16(bytes, value);
        return bytes;
    }

    struct element final {
        std::uint16_t group;
        std::uint16_t tag;
        std::string_view vr;
        std::string value;
    };

    inline void append_element(std::string& bytes, const element& field) {
        const bool long_vr = field.vr == "OB" || field.vr == "OW" || field.vr == "SQ" || field.vr == "UN";
        auto value = field.value;

        if((value.size() & 1U) != 0) {
            value.push_back(field.vr == "UI" || long_vr ? '\0' : ' ');
        }

        append_u16(bytes, field.group);
        append_u16(bytes, field.tag);
        bytes.append(field.vr);

        if(long_vr) {
            append_u16(bytes, 0);
            append_u32(bytes, static_cast<std::uint32_t>(value.size()));
        }
        else {
            append_u16(bytes, static_cast<std::uint16_t>(value.size()));
        }
        bytes += value;
    }

    struct image_spec final {
        std::uint16_t rows{8};
        std::uint16_t columns{8};
        std::uint16_t bits{16};
        std::uint16_t stored{16};
        std::uint16_t high_bit{15};
        std::uint16_t representation{0};
        std::uint16_t samples{1};
        std::uint32_t frames{1};
        std::uint32_t pattern{0x12345678U};
    };

    [[nodiscard]] inline std::string pixel_bytes(const image_spec& spec, std::size_t byte_limit = 128uz * 1024) {
        const auto sample_bytes = spec.bits == 16 ? 2U : 1U;
        std::size_t count = 1;
        const auto limit = byte_limit / sample_bytes;
        for(const auto factor : std::array<std::uint32_t, 4>{spec.rows, spec.columns, spec.frames, spec.samples}) {
            if(factor == 0) {
                count = 0;
                break;
            }
            if(count > limit / factor) {
                count = limit;
                break;
            }
            count *= factor;
        }

        const auto stored = std::clamp<unsigned>(spec.stored, 1U, sample_bytes * 8U);
        const auto mask = (1U << stored) - 1U;
        auto state = spec.pattern;
        std::string bytes;
        bytes.reserve(count * sample_bytes);

        for(std::size_t i = 0; i < count; ++i) {
            state = state * 1664525U + 1013904223U;
            const auto value = static_cast<std::uint16_t>((state >> 8) & mask);
            if(sample_bytes == 2) {
                append_u16(bytes, value);
            }
            else {
                bytes.push_back(static_cast<char>(value));
            }
        }
        return bytes;
    }

    [[nodiscard]] inline std::vector<element> image_elements(const image_spec& spec) {
        return {
            {.group=8, .tag=0x16, .vr="UI", .value="1.2.840.10008.5.1.4.1.1.7"},
            {.group=8, .tag=0x18, .vr="UI", .value=std::string{instance_uid}},
            {.group=8, .tag=0x60, .vr="CS", .value="OT"},
            {.group=0x10, .tag=0x10, .vr="PN", .value="FUZZ^PATIENT"},
            {.group=0x10, .tag=0x20, .vr="LO", .value="FUZZ-ID0"},
            {.group=0x20, .tag=0x0d, .vr="UI", .value=std::string{instance_uid}},
            {.group=0x20, .tag=0x0e, .vr="UI", .value="1.2.826.0.1.3680043.10.543.2"},
            {.group=0x28, .tag=2, .vr="US", .value=us(spec.samples)},
            {.group=0x28, .tag=4, .vr="CS", .value="MONOCHROME2"},
            {.group=0x28, .tag=8, .vr="IS", .value=std::to_string(spec.frames)},
            {.group=0x28, .tag=0x10, .vr="US", .value=us(spec.rows)},
            {.group=0x28, .tag=0x11, .vr="US", .value=us(spec.columns)},
            {.group=0x28, .tag=0x100, .vr="US", .value=us(spec.bits)},
            {.group=0x28, .tag=0x101, .vr="US", .value=us(spec.stored)},
            {.group=0x28, .tag=0x102, .vr="US", .value=us(spec.high_bit)},
            {.group=0x28, .tag=0x103, .vr="US", .value=us(spec.representation)},
            {.group=0x7fe0, .tag=0x10, .vr=spec.bits == 16 ? "OW" : "OB", .value=pixel_bytes(spec)}
        };
    }

    [[nodiscard]] inline std::string encode(
        const std::vector<element>& fields, 
        std::string_view syntax = native_syntax
    ) {
        std::string meta;
        append_element(meta, {.group=2, .tag=1, .vr="OB", .value=std::string{"\0\1", 2}});
        append_element(meta, {.group=2, .tag=2, .vr="UI", .value="1.2.840.10008.5.1.4.1.1.7"});
        append_element(meta, {.group=2, .tag=3, .vr="UI", .value=std::string{instance_uid}});
        append_element(meta, {.group=2, .tag=0x10, .vr="UI", .value=std::string{syntax}});
        append_element(meta, {.group=2, .tag=0x12, .vr="UI", .value="1.2.826.0.1.3680043.10.543.3"});

        std::string data(128, '\0');
        data += "DICM";
        std::string size;
        append_u32(size, static_cast<std::uint32_t>(meta.size()));
        append_element(data, {.group=2, .tag=0, .vr="UL", .value=std::move(size)});
        data += meta;
        for(const auto& field : fields) {
            append_element(data, field);
        }
        return data;
    }

    inline void replace_value(
        std::vector<element>& fields, 
        std::uint16_t group, 
        std::uint16_t tag, 
        std::string value
    ) {
        for(auto& field : fields) {
            if(field.group == group && field.tag == tag) {
                field.value = std::move(value);
                return;
            }
        }
    }

    class temporary_file final {
    public:
        temporary_file() {
            char path[] = "/tmp/nova_dicom_fuzz_XXXXXX";
            m_fd = ::mkstemp(path);
            if(m_fd >= 0) {
                m_path = path;
                (void)::fcntl(m_fd, F_SETFD, FD_CLOEXEC);
            }
        }

        temporary_file(const temporary_file&) = delete;
        temporary_file& operator=(const temporary_file&) = delete;

        ~temporary_file() {
            if(m_fd >= 0) {
                (void)::close(m_fd);
                (void)::unlink(m_path.c_str());
            }
        }

        [[nodiscard]] bool reset(std::string_view input) const noexcept {
            if(m_fd < 0 || ::ftruncate(m_fd, 0) != 0) {
                return false;
            }
            std::size_t offset{};
            while(offset < input.size()) {
                const auto count = ::pwrite(m_fd, input.data() + offset, input.size() - offset,
                                            static_cast<off_t>(offset));
                if(count < 0 && errno == EINTR) {
                    continue;
                }
                if(count <= 0) {
                    return false;
                }
                offset += static_cast<std::size_t>(count);
            }
            return true;
        }

        [[nodiscard]] const std::filesystem::path& path() const noexcept {
            return m_path;
        }

    private:
        int m_fd{-1};
        std::filesystem::path m_path;
    };
}

