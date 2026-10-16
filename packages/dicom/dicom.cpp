#include "dicom.h"
#include "core/result.h"
#include <expected>
#include <string_view>

using namespace nova::dicom;

nova::result<modality> nova::dicom::resolve_modality(std::string_view modality) {
    if (modality == "BDUS") {
        return modality::UltraSoundBoneDensitometry;
    }
    if (modality == "BI") {
        return modality::BiomagneticImaging;
    }
    if (modality == "BMD") {
        return modality::XrayBoneDensitometry;
    }
    if (modality == "CR") {
        return modality::ComputedRadiography;
    }
    if (modality == "CT") {
        return modality::ComputedTomography;
    }
    if (modality == "DG") {
        return modality::Diaphanography;
    }
    if (modality == "DX") {
        return modality::DigitalRadiography;
    }
    if (modality == "IO") {
        return modality::IntraOralRadiography;
    }
    if (modality == "MG") {
        return modality::Mammography;
    }
    if (modality == "MR") {
        return modality::MagneticResonance;
    }
    if (modality == "PLAN") {
        return modality::Plan;
    }
    if (modality == "PT") {
        return modality::PositronEmissionTomography;
    }
    if (modality == "RTIMAGE") {
        return modality::RTImage;
    }
    if (modality == "RTDOSE") {
        return modality::RTDose;
    }
    if (modality == "RTSTRUCT") {
        return modality::RTStruct;
    }
    if (modality == "RTPLAN") {
        return modality::RTPlan;
    }
    if (modality == "RTRECORD") {
        return modality::RTRecord;
    }
    if (modality == "SEG") {
        return modality::Seg;
    }
    if (modality == "US") {
        return modality::Ultrasound;
    }
    return std::unexpected("Incorrect or unsupported modality");
}
