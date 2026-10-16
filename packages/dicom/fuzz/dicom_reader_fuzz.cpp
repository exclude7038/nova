#include "dicom_fuzz_support.h"
#include "dicom/dicom_reader.h"
#include <fuzztest/fuzztest.h>
#include <gtest/gtest.h>
#include <fstream>
#include <ios>
#include <tuple>
#include <vector>

namespace nova::dicom::fuzz {
    namespace {
        [[nodiscard]] std::vector<std::tuple<std::string>> corpus_seeds() {
            const auto valid = encode(image_elements({}));
            auto truncated = valid.substr(0, valid.size() - 2);
            auto bad_magic = valid;
            bad_magic.replace(128,3, "BAD!");

             std::vector<std::tuple<std::string>> seeds{
                {std::string{}}, {std::string(2, '\0')}, {std::string(132, '\xff') },
                {valid}, {truncated}, {bad_magic},
                {encode(image_elements({.rows = 1, .columns = 1, .bits = 8, .stored = 8, .high_bit = 7}))},
                {encode(image_elements({.representation = 1, .frames = 2}))}
            };

             std::ifstream fixture(NOVA_DICOM_FUZZ_SEED_FILE, std::ios::binary | std::ios::ate);
             if(!fixture || fixture.tellg() <= 0 || fixture.tellg() > static_cast<std::streamoff>(max_fuzz_input)) {
                 ADD_FAILURE() << "Cannot read bounded CTHead1.dcm seed";
                 return seeds;
             }

            std::string bytes(static_cast<std::size_t>(fixture.tellg()), '\0');
            fixture.seekg(0);

            if(!fixture.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
                ADD_FAILURE() << "Cannot read CTHead1.dcm seed bytes";
                return seeds;
            }

            seeds.emplace_back(std::move(bytes));
            return seeds;
        }

        void check_pixels(dicom_reader& reader, const pixel_data_info& info) {
            const auto pixels = reader.read_pixel_data(info);
            if(!pixels) {
                return;
            }

            ASSERT_LE(pixels->size(), max_decoded_bytes);
            ASSERT_TRUE(info.bits_allocated == 8 || info.bits_allocated == 16 || info.bits_allocated == 32);

            std::size_t expected = info.bits_allocated / 8;
            const auto factors =  std::array<std::uint32_t, 4>{ 
                info.dims.width, 
                info.dims.height, 
                info.dims.frames, 
                info.samples_per_pixel
            };

            for(const auto factor : factors) {
                ASSERT_NE(factor, 0U);
                ASSERT_LE(expected, max_decoded_bytes / factor);
                expected *= factor;
            }

            ASSERT_EQ(pixels->size(), expected);
            volatile std::uint32_t checksum = 0;
            for(const auto value : *pixels) {
                checksum = checksum ^ std::to_integer<std::uint8_t>(value);
            }
            (void)checksum;
        }

        void exercise(const std::string& input) {
            temporary_file file;
            ASSERT_FALSE(file.path().empty());
            ASSERT_TRUE(file.reset(input));
            dicom_reader reader;
            if(!reader.load(file.path())) {
                EXPECT_FALSE(reader.read_pixel_data_info().has_value());
                return;
            }
            (void)reader.read_metadata();
            const auto info = reader.read_pixel_data_info();
            if(info) {
                check_pixels(reader, *info);
            }
        }

        void ArbitraryBytesNeverCrash(const std::string& input) {
            exercise(input);
        }

        void InvalidFilesAreRejected(const std::string& input) {
            auto odd = input;
            if((odd.size() & 1U) == 0) {
                odd.push_back('\0');
            }
            temporary_file file;
            ASSERT_TRUE(file.reset(odd));
            dicom_reader reader;
            EXPECT_FALSE(reader.load(file.path()).has_value());
            EXPECT_FALSE(reader.read_pixel_data_info().has_value());
        }

         void ValidImagesRoundTrip(
            std::uint16_t rows, 
            std::uint16_t columns, 
            std::uint32_t frames,
            bool sixteen_bits, 
            bool signed_samples, 
            std::uint8_t precision,
            std::uint32_t pattern
        ) {
            const auto bits = static_cast<std::uint16_t>(sixteen_bits ? 16 : 8);
            const auto stored = static_cast<std::uint16_t>(1U + precision % bits);
            const image_spec spec{
                .rows = rows, .columns = columns, .bits = bits, .stored = stored,
                .high_bit = static_cast<std::uint16_t>(stored - 1),
                .representation = static_cast<std::uint16_t>(sixteen_bits && signed_samples),
                .frames = frames, .pattern = pattern
            };
            const auto expected = pixel_bytes(spec);

            temporary_file file;
            ASSERT_TRUE(file.reset(encode(image_elements(spec))));
            dicom_reader reader;
            const auto loaded = reader.load(file.path());
            ASSERT_TRUE(loaded.has_value());
            const auto metadata = reader.read_metadata();
            ASSERT_TRUE(metadata.has_value());
            EXPECT_EQ(metadata->patient.name, "FUZZ^PATIENT");
            EXPECT_EQ(metadata->study.instance_uid, instance_uid);
            EXPECT_EQ(metadata->series.instance_uid, "1.2.826.0.1.3680043.10.543.2");

            const auto info = reader.read_pixel_data_info();
            ASSERT_TRUE(info.has_value());
            EXPECT_EQ(info->dims.width, columns);
            EXPECT_EQ(info->dims.height, rows);
            EXPECT_EQ(info->dims.frames, frames);
            EXPECT_EQ(info->samples_per_pixel, 1);
            EXPECT_EQ(info->bits_allocated, bits);
            EXPECT_EQ(info->bits_stored, stored);
            EXPECT_EQ(info->high_bit, stored - 1);
            EXPECT_EQ(info->photometric, photometric_interpretation::monochrome2);
            const auto expected_format = !sixteen_bits 
                ? pixel_sample_format::u8
                : signed_samples 
                    ? pixel_sample_format::s16 
                    : pixel_sample_format::u16;
            EXPECT_EQ(info->format, expected_format);

            const auto pixels = reader.read_pixel_data(*info);
            ASSERT_TRUE(pixels.has_value());
            ASSERT_EQ(pixels->size(), expected.size());
            EXPECT_TRUE(
                std::equal(pixels->begin(), pixels->end(), expected.begin(),
                    [](std::byte actual, char wanted) {
                        return std::to_integer<unsigned char>(actual) == static_cast<unsigned char>(wanted);
                    }
                )
            );
        }

        void InvalidImageAttributesAreRejected(std::uint8_t fault, std::uint32_t pattern) {
            auto fields = image_elements({.pattern = pattern});
            switch(fault) {
                case 0: replace_value(fields, 0x28, 0x10, us(0)); break;
                case 1: replace_value(fields, 0x28, 0x11, us(0)); break;
                case 2: replace_value(fields, 0x28, 2, us(0)); break;
                case 3: replace_value(fields, 0x28, 0x100, us(7)); break;
                case 4: replace_value(fields, 0x28, 0x101, us(0)); break;
                case 5: replace_value(fields, 0x28, 0x101, us(17)); break;
                case 6: replace_value(fields, 0x28, 0x102, us(16)); break;
                case 7: replace_value(fields, 0x28, 0x103, us(2)); break;
                case 8: replace_value(fields, 0x28, 8, "0"); break;
                case 9: replace_value(fields, 0x28, 8, "-1"); break;
                case 10: replace_value(fields, 0x28, 8, "4294967296"); break;
                case 11: replace_value(fields, 0x28, 8, "1x"); break;
                case 12: replace_value(fields, 0x28, 4, "INVALID"); break;
                case 13:
                    std::erase_if(fields, [](const element& field) { return field.group == 0x28 && field.tag == 0x10; });
                    break;
                case 14:
                    replace_value(fields, 0x28, 2, us(2));
                    break;
                default: FAIL() << "Invalid generator fault";
            }
            temporary_file file;
            ASSERT_TRUE(file.reset(encode(fields)));
            dicom_reader reader;
            if(reader.load(file.path())) {
                EXPECT_FALSE(reader.read_pixel_data_info().has_value());
            }
        }

        void ShortPixelDataIsRejected(std::uint16_t missing, std::uint32_t pattern) {
            auto fields = image_elements({.pattern = pattern});
            fields.back().value.resize(128U - static_cast<std::size_t>(missing) * 2U);
            temporary_file file;
            ASSERT_TRUE(file.reset(encode(fields)));
            dicom_reader reader;
            if(!reader.load(file.path())) {
                return;
            }
            const auto info = reader.read_pixel_data_info();
            if(info) {
                EXPECT_FALSE(reader.read_pixel_data(*info).has_value());
            }
        }

        void PartlyCorruptedImagesNeverCrash(
            std::uint8_t operation, 
            std::uint32_t offset,
            const std::string& mutation, 
            std::uint32_t pattern
        ) {
            auto fields = image_elements({.pattern = pattern});
            auto input = encode(fields);
            const auto position = static_cast<std::size_t>(offset) % input.size();
            switch(operation) {
                case 0: input.resize(position); break;
                case 1: input.insert(position, mutation); break;
                case 2: input.erase(position, std::min(mutation.size() + 1, input.size() - position)); break;
                case 3:
                    for(std::size_t i = 0; i < mutation.size() && i < input.size() - position; ++i) {
                        input[position + i] = static_cast<char>(
                            static_cast<unsigned char>(input[position + i]) ^ static_cast<unsigned char>(mutation[i])
                        );
                    }
                    break;
                case 4:
                    fields[static_cast<std::size_t>(offset) % fields.size()].value = mutation;
                    input = encode(fields);
                    break;
                case 5:
                    fields.insert(fields.begin() + 1, fields.back()); // Duplicate, out-of-order Pixel Data.
                    input = encode(fields);
                    break;
                case 6:
                    input.replace(128, 4, "BAD!");
                    break;
                default: FAIL() << "Invalid generator operation";
            }
            exercise(input);
        }

        void ExtremeDimensionsNeverCrash(
            std::uint16_t rows, 
            std::uint16_t columns,
            std::uint32_t frames,                             
            std::uint16_t samples, 
            std::uint16_t bits, 
            std::uint16_t representation
        ) {
            exercise(encode(image_elements({
                .rows = rows, .columns = columns, .bits = bits, .stored = bits,
                .high_bit = static_cast<std::uint16_t>(bits == 0 ? 0 : bits - 1),
                .representation = representation, .samples = samples, .frames = frames
            })));
        }

        void HostileLengthsAndSequencesNeverCrash(
            std::uint32_t length, 
            std::uint8_t depth,
            bool encapsulated, 
            bool terminate, 
            const std::string& payload
        ) {
            std::string nested;
            for(std::uint8_t i = 0; i < depth; ++i) {
                append_u16(nested, 8);
                append_u16(nested, 0x1111);
                nested += "SQ";
                append_u16(nested, 0);
                append_u32(nested, 0xffffffffU);
                append_u16(nested, 0xfffe);
                append_u16(nested, 0xe000);
                append_u32(nested, 0xffffffffU);
            }
            append_element(nested, {.group=0x10, .tag=0x10, .vr="PN", .value=payload});
            if(terminate) {
                for(std::uint8_t i = 0; i < depth; ++i) {
                    append_u16(nested, 0xfffe);
                    append_u16(nested, 0xe00d);
                    append_u32(nested, 0);
                    append_u16(nested, 0xfffe);
                    append_u16(nested, 0xe0dd);
                    append_u32(nested, 0);
                }
            }
            auto fields = image_elements({});
            fields.pop_back();
            auto input = encode(fields, encapsulated ? "1.2.840.10008.1.2.4.50" : native_syntax);
            input += nested;
            append_u16(input, 0x7fe0);
            append_u16(input, 0x10);
            input += "OB";
            append_u16(input, 0);
            append_u32(input, encapsulated ? 0xffffffffU : length);
            if(encapsulated) {
                append_u16(input, 0xfffe);
                append_u16(input, 0xe000);
                append_u32(input, 0); 
                append_u16(input, 0xfffe);
                append_u16(input, 0xe000);
                append_u32(input, length);
            }
            input += payload;
            if(encapsulated && terminate) {
                append_u16(input, 0xfffe);
                append_u16(input, 0xe0dd);
                append_u32(input, 0);
            }

            if((input.size() & 1U) != 0) {
                input.push_back('\0');
            }
            exercise(input);
        }

        void ReaderReuseNeverCrashes(const std::string& input) {
            temporary_file file;
            ASSERT_TRUE(file.reset(encode(image_elements({}))));
            dicom_reader reader;
            ASSERT_TRUE(reader.load(file.path()).has_value());
            ASSERT_TRUE(reader.read_pixel_data_info().has_value());

            ASSERT_TRUE(file.reset(input));
            const bool loaded = reader.load(file.path()).has_value();
            auto _ = reader.read_metadata();
            const auto info = reader.read_pixel_data_info();
            if(!loaded) {
                EXPECT_FALSE(info.has_value());
            }
            else if(info) {
                check_pixels(reader, *info);
            }

            ASSERT_TRUE(file.reset({})); // Definitely invalid, not a heuristic byte sequence.
            EXPECT_FALSE(reader.load(file.path()).has_value());
            EXPECT_FALSE(reader.read_metadata().has_value());
            EXPECT_FALSE(reader.read_pixel_data_info().has_value());
            EXPECT_FALSE(reader.read_pixel_data(pixel_data_info{}).has_value());

            ASSERT_TRUE(file.reset(encode(image_elements({}))));
            ASSERT_TRUE(reader.load(file.path()).has_value());
            const auto recovered = reader.read_pixel_data_info();
            ASSERT_TRUE(recovered.has_value());
            const auto pixels = reader.read_pixel_data(*recovered);
            ASSERT_TRUE(pixels.has_value());
            EXPECT_EQ(pixels->size(), 128U);
        }

        void ForgedPixelInfoNeverCrashes(std::uint32_t width, std::uint32_t height, std::uint32_t frames,
                                        std::uint16_t samples, std::uint16_t bits, std::uint8_t format) {
            temporary_file file;
            ASSERT_TRUE(file.reset(encode(image_elements({}))));
            dicom_reader reader;
            ASSERT_TRUE(reader.load(file.path()).has_value());
            pixel_data_info info{};
            info.dims = {.width = width, .height = height, .frames = frames};
            info.samples_per_pixel = samples;
            info.bits_allocated = bits;
            info.format = static_cast<pixel_sample_format>(format);
            check_pixels(reader, info);
        }

        FUZZ_TEST(DicomFuzz, ArbitraryBytesNeverCrash)
            .WithDomains(fuzztest::Arbitrary<std::string>().WithMaxSize(max_fuzz_input))
            .WithSeeds(corpus_seeds);

        FUZZ_TEST(DicomFuzz, InvalidFilesAreRejected)
            .WithDomains(fuzztest::Arbitrary<std::string>().WithMaxSize(max_fuzz_input - 1));

        FUZZ_TEST(DicomFuzz, ValidImagesRoundTrip)
            .WithDomains(fuzztest::InRange<std::uint16_t>(1, 64), fuzztest::InRange<std::uint16_t>(1, 64),
                         fuzztest::InRange<std::uint32_t>(1, 4), fuzztest::Arbitrary<bool>(), fuzztest::Arbitrary<bool>(),
                         fuzztest::Arbitrary<std::uint8_t>(), fuzztest::Arbitrary<std::uint32_t>())
            .WithSeeds({{1, 1, 1, false, false, 7, 0}, {8, 8, 1, true, false, 15, 1},
                        {64, 64, 4, true, true, 11, 0xffffffffU}});

        FUZZ_TEST(DicomFuzz, InvalidImageAttributesAreRejected)
            .WithDomains(fuzztest::InRange<std::uint8_t>(0, 14), fuzztest::Arbitrary<std::uint32_t>());

        FUZZ_TEST(DicomFuzz, ShortPixelDataIsRejected)
            .WithDomains(fuzztest::InRange<std::uint16_t>(1, 64), fuzztest::Arbitrary<std::uint32_t>());

        FUZZ_TEST(DicomFuzz, PartlyCorruptedImagesNeverCrash)
            .WithDomains(fuzztest::InRange<std::uint8_t>(0, 6), fuzztest::Arbitrary<std::uint32_t>(),
                         fuzztest::Arbitrary<std::string>().WithMaxSize(4096), fuzztest::Arbitrary<std::uint32_t>());

        FUZZ_TEST(DicomFuzz, ExtremeDimensionsNeverCrash)
            .WithDomains(fuzztest::Arbitrary<std::uint16_t>(), fuzztest::Arbitrary<std::uint16_t>(),
                         fuzztest::Arbitrary<std::uint32_t>(), fuzztest::Arbitrary<std::uint16_t>(),
                         fuzztest::ElementOf<std::uint16_t>({0, 1, 8, 16, 32, 65535}),
                         fuzztest::ElementOf<std::uint16_t>({0, 1, 2, 65535}))
            .WithSeeds({{65535, 65535, 0xffffffffU, 65535, 16, 0}, {1, 1, 0, 1, 8, 0},
                        {65535, 65535, 1, 1, 16, 0}});

        FUZZ_TEST(DicomFuzz, HostileLengthsAndSequencesNeverCrash)
            .WithDomains(fuzztest::OneOf(fuzztest::Arbitrary<std::uint32_t>(),
                             fuzztest::ElementOf<std::uint32_t>({0, 1, 2, 0x7fffffffU, 0xfffffffeU, 0xffffffffU})),
                         fuzztest::InRange<std::uint8_t>(0, 64), fuzztest::Arbitrary<bool>(),
                         fuzztest::Arbitrary<bool>(), fuzztest::Arbitrary<std::string>().WithMaxSize(4096))
            .WithSeeds({{0xffffffffU, 0, false, false, ""}, {0xfffffffeU, 32, true, true, ""},
                        {2, 64, false, true, "xx"}});

        FUZZ_TEST(DicomFuzz, ReaderReuseNeverCrashes)
            .WithDomains(fuzztest::Arbitrary<std::string>().WithMaxSize(max_fuzz_input))
            .WithSeeds(corpus_seeds);

        FUZZ_TEST(DicomFuzz, ForgedPixelInfoNeverCrashes)
            .WithDomains(fuzztest::Arbitrary<std::uint32_t>(), fuzztest::Arbitrary<std::uint32_t>(),
                         fuzztest::Arbitrary<std::uint32_t>(), fuzztest::Arbitrary<std::uint16_t>(),
                         fuzztest::ElementOf<std::uint16_t>({0, 8, 16, 32, 65535}), fuzztest::Arbitrary<std::uint8_t>())
            .WithSeeds({{0xffffffffU, 0xffffffffU, 0xffffffffU, 65535, 16, 1},
                        {8, 8, 1, 1, 16, 1}, {0, 8, 1, 1, 16, 1}, {8, 8, 1, 1, 16, 255}});
    }
}
