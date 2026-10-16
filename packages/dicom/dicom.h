#pragma once

#include <cstddef>
#include <cstdint>
#include <dcmdata/dctagkey.h>
#include <string>
#include <string_view>
#include <utility>
#include "core/result.h"

namespace nova::dicom {
    enum class dicom_tag : std::uint32_t {
        patient_name = 0x00100010U,
        patient_id = 0x00100020U,
        patient_birth_date = 0x00100030U,
        patient_birth_time = 0x00100032U,
        patient_sex = 0x00100040U,
        study_date = 0x00080020U,
        study_time = 0x00080030U,
        study_instance_uid = 0x0020000DU,
        study_id = 0x00200010U,
        study_accession_number = 0x00080050U,
        study_description = 0x00081030U,
        study_referring_physician_name = 0x00080090U,
        series_date = 0x00080021U,
        series_time = 0x00080031U,
        series_instance_uid = 0x0020000EU,
        series_description = 0x0008103EU,
        series_number = 0x00200011U,
        series_modality = 0x00080060U,
        series_body_part_examined = 0x00180015U,
        series_performing_physician_name = 0x00081050U,
        series_smallest_pixel_value = 0x00280108U,
        series_largest_pixel_value = 0x00280109U,
        samples_per_pixel = 0x00280002U,
        photometric_interpretation = 0x00280004U,
        rows = 0x00280010U,
        columns = 0x00280011U,
        pixel_aspect_ratio = 0x00280034U,
        bits_allocated = 0x00280100U,
        bits_stored = 0x00280101U,
        high_bit = 0x00280102U,
        pixel_representation = 0x00280103U,
        smallest_image_pixel_value = 0x00280106U,
        largest_image_pixel_value = 0x00280107U,
        pixel_padding_range_limit = 0x00280121U,
        color_space = 0x00282002U,
        pixel_data = 0x7FE00010U,
        image_type = 0x00080008U,
        slice_thickness = 0x00180050U,
        spacing_between_slices = 0x00180088U,
        image_position = 0x00200032U,
        image_orientation = 0x00200037U,
        slice_location = 0x00201041U,
        pixel_spacing = 0x00280030U,
        number_of_frames = 0x00280008U,
        planar_configuration = 0x00280006U,
        window_center = 0x00281050U,
        window_width = 0x00281051U,
        rescale_intercept = 0x00281052U,
        rescale_slope = 0x00281053U,
    };

    enum class modality : uint8_t {
        Unknown,
        // Ultrasound-based measurement of bone density
        UltraSoundBoneDensitometry,
        // Measurement of magnetic fields produced by electrical activity in organs
        BiomagneticImaging,
        // X-ray based measurement of bone density
        XrayBoneDensitometry,
        // Digital X-ray imaging using phosphor plates instead of film
        ComputedRadiography,
        // 3D X-ray imaging using computer-processed combinations of many X-ray measurements taken from different angles
        ComputedTomography,
        // Transillumination technique for breast imaging
        Diaphanography,
        //Direct digital capture of X-ray images
        DigitalRadiography,
        // X-ray imaging of teeth and supporting structures of the jaw
        IntraOralRadiography,
        // Specialized X-ray imaging for breast examination
        Mammography,
        // Imaging technique that uses strong magnetic fields and radio waves to generate detailed anatomical images
        MagneticResonance,
        //For radiotherapy plans
        Plan,
        // Functional imaging technique using radioactive tracers to observe metabolic processes
        PositronEmissionTomography,
        // Radiotherapy images
        RTImage,
        // Radiotherapy dose data
        RTDose,
        // Radiotherapy structure sets
        RTStruct,
        // Radiotherapy treatment plans
        RTPlan,
        // Radiotherapy treatment records
        RTRecord,
        // Segmented data from images
        Seg,
        // Imaging using high-frequency sound waves to visualize internal body structures
        Ultrasound,
    };

    struct dicom_tag_data {
        uint16_t group;
        uint16_t element;
    };

    struct patient {
        std::string name;
        std::string id;
        std::string birth_date;
        std::string birth_time;
        std::string sex;
    };

    struct study {
        std::string instance_uid;
        std::string id;
        std::string date;
        std::string time;
        std::string accession_number;
        std::string description;
        std::string referring_physician_name;
    };

    struct series {
        std::string instance_uid;
        std::string date;
        std::string time;
        std::string description;
        std::string number;
        std::string body_part_examined ;
        std::string performing_physician_name;
        std::string smallest_pixel_value;
        std::string largest_pixel_value;
        modality modality;
    };

    struct metadata {
        patient patient;
        study study;
        series series;
    };

    enum class photometric_interpretation : uint8_t {
        monochrome1 = 0,
        monochrome2,
        palette_color,
        rgb,
        hsv,
        argb,
        cmyk,
        ybr_full,
        ybr_full_422,
        ybr_partial_422,
        ybr_partial_420,
        ybr_ict,
        ybr_rct,
    };

    enum class pixel_sample_format : uint8_t {
        u8 = 0,
        u16,
        s16,
        u32,
        s32,
    };

    struct image_dimensions final {
        uint32_t width{};
        uint32_t height{};
        uint32_t frames{1};
    };

    struct pixel_data_info {
        image_dimensions dims;
        uint16_t samples_per_pixel;
        uint16_t planar_configuration;
        uint16_t bits_allocated{};
        uint16_t bits_stored{};
        uint16_t high_bit{};
        photometric_interpretation photometric;
        pixel_sample_format format;

        [[nodiscard]] std::size_t pixel_count() const noexcept {
            return static_cast<size_t>(dims.width) * 
                static_cast<size_t>(dims.height) *
                static_cast<size_t>(dims.frames);
        }
    };

    [[nodiscard]] constexpr dicom_tag_data resolve_dicom_tag(dicom_tag tag) noexcept {
        const auto value = std::to_underlying(tag);

        return {
            .group = static_cast<std::uint16_t>(value >> 16),
            .element = static_cast<std::uint16_t>(value & 0xffffU)
        };
    }

    [[nodiscard]] constexpr DcmTagKey to_dcm_tag(dicom_tag tag) noexcept {
        const auto value = std::to_underlying(tag);

        return {
            static_cast<std::uint16_t>(value >> 16),
            static_cast<std::uint16_t>(value & 0xffffU)
        };
    }

    [[nodiscard]] nova::result<modality> resolve_modality(std::string_view modality);
}
