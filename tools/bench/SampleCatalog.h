#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace steplens::bench {

struct SampleEntry {
    std::string_view id;
    std::string_view filename;
};

// 与 scripts/samples_manifest.json 的 id、文件名一致。链接和 sha256 只放在清单里。
inline constexpr std::array<SampleEntry, 36> kSampleCatalog{{
    {"S-01", "screw.step"},
    {"S-02", "linkrods.step"},
    {"S-03", "as1-ac-214.stp"},
    {"S-04", "Schenkel.stp"},
    {"S-05", "as1_pe.stp"},
    {"S-06", "moon_buggy_asm.stp"},
    {"S-07", "123Block_Color.stp"},
    {"S-08", "nist_ctc_02_asme1_ap242-e2.stp"},
    {"S-09", "nist_ftc_09_asme1_ap242-e1.stp"},
    {"S-10", "nist_stc_10_asme1_ap242-e2.stp"},
    {"S-11", "ROTOR-201NAL-Z7.STEP"},
    {"S-12", "bearing.iges"},
    {"S-13", "hammer.iges"},
    {"S-14", "1030490B.igs"},
    {"M-01", "Rocky_House.stp"},
    {"M-02", "SO101 Assembly.step"},
    {"M-03", "YARR_assembly.STEP"},
    {"M-04", "Cruise_Assembly.stp"},
    {"M-05", "SP218P001.IGS"},
    {"M-06", "1026868C.IGS"},
    {"M-07", "PX13298F.IGS"},
    {"L-01", "UMC-500_SS_Solid_Model_2019-06_r1.stp"},
    {"L-02", "Ai-14R.stp"},
    {"L-03", "NissanGT-R.STEP"},
    {"L-04", "PX12820C.igs"},
    {"L-05", "1025454D.IGS"},
    {"L-06", "1028862.IGS"},
    {"L-07", "1028499B.IGS"},
    {"X-01", "Scania-8x4.stp"},
    {"X-02", "Scania-Engine-V8-XT-Turbo.step"},
    {"X-03", "LUPA-A1000.STEP"},
    {"X-04", "1026064M.STEP"},
    {"X-05", "1027247E.STEP"},
    {"X-06", "1026064M.IGS"},
    {"X-07", "PX13226.IGS"},
    {"X-08", "1027247E.IGS"},
}};

inline std::optional<std::string_view> filenameForSample(std::string_view id) noexcept {
    for (const SampleEntry& entry : kSampleCatalog) {
        if (entry.id == id) {
            return entry.filename;
        }
    }
    return std::nullopt;
}

} // namespace steplens::bench
