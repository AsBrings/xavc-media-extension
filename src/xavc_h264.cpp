// xavc_h264.cpp - H.264 sequence parameter set parsing (ITU-T H.264 7.3.2.1.1, E.1.1).

#include "xavc_h264.h"
#include <string.h>
#include <stdlib.h>

namespace {

// Exp-Golomb bit reader over an RBSP (emulation prevention bytes already removed).
class BitReader {
public:
    BitReader(const uint8_t* p, size_t n) : m_p(p), m_bits(n * 8), m_pos(0), m_err(false) {}
    bool error() const { return m_err; }

    uint32_t u(int n) {               // n <= 32
        uint32_t v = 0;
        for (int i = 0; i < n; i++) v = (v << 1) | bit();
        return v;
    }
    uint32_t ue() {
        int zeros = 0;
        while (!bit()) {
            if (m_err || ++zeros > 31) { m_err = true; return 0; }
        }
        if (!zeros) return 0;
        return ((1u << zeros) - 1) + u(zeros);
    }
    int32_t se() {
        uint32_t k = ue();
        return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
    }

private:
    uint32_t bit() {
        if (m_pos >= m_bits) { m_err = true; return 0; }
        uint32_t b = (m_p[m_pos >> 3] >> (7 - (m_pos & 7))) & 1;
        m_pos++;
        return b;
    }
    const uint8_t* m_p;
    size_t m_bits, m_pos;
    bool m_err;
};

void SkipScalingList(BitReader& br, int size) {
    int last = 8, next = 8;
    for (int j = 0; j < size; j++) {
        if (next != 0) {
            int delta = br.se();
            next = (last + delta + 256) % 256;
        }
        last = next == 0 ? last : next;
    }
}

bool HasChromaInfo(int profile) {
    switch (profile) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86:
    case 118: case 128: case 138: case 139: case 134: case 135:
        return true;
    default:
        return false;
    }
}

const int kSar[17][2] = {
    { 0, 0 }, { 1, 1 }, { 12, 11 }, { 10, 11 }, { 16, 11 }, { 40, 33 }, { 24, 11 }, { 20, 11 },
    { 32, 11 }, { 80, 33 }, { 18, 11 }, { 15, 11 }, { 64, 33 }, { 160, 99 }, { 4, 3 }, { 3, 2 }, { 2, 1 },
};

// Next Annex-B start code at or after i; returns len if none. *scLen = 3 or 4.
size_t FindStartCode(const uint8_t* p, size_t len, size_t i, size_t* scLen) {
    for (; i + 3 <= len; i++) {
        if (p[i] == 0 && p[i + 1] == 0) {
            if (p[i + 2] == 1) { *scLen = 3; return i; }
            if (i + 4 <= len && p[i + 2] == 0 && p[i + 3] == 1) { *scLen = 4; return i; }
        }
    }
    *scLen = 0;
    return len;
}

} // namespace

bool XavcParseSpsNal(const uint8_t* nal, size_t len, XavcSps* out) {
    if (!nal || len < 4 || (nal[0] & 0x1f) != 7) return false;
    if (len > 4096) len = 4096;   // an SPS is tiny; bound the work on garbage

    // RBSP: drop emulation prevention bytes (00 00 03 -> 00 00)
    uint8_t rbsp[4096];
    size_t n = 0;
    int zeros = 0;
    for (size_t i = 1; i < len; i++) {
        uint8_t b = nal[i];
        if (zeros >= 2 && b == 3) { zeros = 0; continue; }
        rbsp[n++] = b;
        zeros = b == 0 ? zeros + 1 : 0;
    }

    XavcSps s;
    memset(&s, 0, sizeof(s));
    s.video_full_range = -1;
    s.colour_primaries = s.transfer_characteristics = s.matrix_coefficients = -1;

    BitReader br(rbsp, n);
    s.profile_idc = (int)br.u(8);
    br.u(8);                                   // constraint_set flags + reserved
    s.level_idc = (int)br.u(8);
    if (br.ue() > 31) return false;            // seq_parameter_set_id
    s.chroma_format_idc = 1;
    s.bit_depth_luma = s.bit_depth_chroma = 8;
    int separate_colour_plane = 0;
    if (HasChromaInfo(s.profile_idc)) {
        s.chroma_format_idc = (int)br.ue();
        if (s.chroma_format_idc > 3) return false;
        if (s.chroma_format_idc == 3) separate_colour_plane = (int)br.u(1);
        s.bit_depth_luma = 8 + (int)br.ue();
        s.bit_depth_chroma = 8 + (int)br.ue();
        if (s.bit_depth_luma > 14 || s.bit_depth_chroma > 14) return false;
        br.u(1);                               // qpprime_y_zero_transform_bypass_flag
        if (br.u(1)) {                         // seq_scaling_matrix_present_flag
            int lists = s.chroma_format_idc != 3 ? 8 : 12;
            for (int i = 0; i < lists; i++)
                if (br.u(1)) SkipScalingList(br, i < 6 ? 16 : 64);
        }
    }
    br.ue();                                   // log2_max_frame_num_minus4
    uint32_t poc_type = br.ue();
    if (poc_type == 0) {
        br.ue();                               // log2_max_pic_order_cnt_lsb_minus4
    } else if (poc_type == 1) {
        br.u(1);                               // delta_pic_order_always_zero_flag
        br.se();                               // offset_for_non_ref_pic
        br.se();                               // offset_for_top_to_bottom_field
        uint32_t cycle = br.ue();
        if (cycle > 255) return false;
        for (uint32_t i = 0; i < cycle; i++) br.se();
    } else if (poc_type != 2) {
        return false;
    }
    br.ue();                                   // max_num_ref_frames
    br.u(1);                                   // gaps_in_frame_num_value_allowed_flag
    uint32_t wMbs = br.ue() + 1;
    uint32_t hMapUnits = br.ue() + 1;
    s.frame_mbs_only = (int)br.u(1);
    if (!s.frame_mbs_only) br.u(1);            // mb_adaptive_frame_field_flag
    br.u(1);                                   // direct_8x8_inference_flag
    if (wMbs > 1024 || hMapUnits > 1024) return false;
    int width = (int)wMbs * 16;
    int height = (2 - s.frame_mbs_only) * (int)hMapUnits * 16;
    if (br.u(1)) {                             // frame_cropping_flag
        uint32_t l = br.ue(), r = br.ue(), t = br.ue(), b = br.ue();
        int cropX = 1, cropY = 2 - s.frame_mbs_only;
        if (s.chroma_format_idc != 0 && !separate_colour_plane) {
            cropX = s.chroma_format_idc == 3 ? 1 : 2;
            cropY *= s.chroma_format_idc == 1 ? 2 : 1;
        }
        long w = (long)width - (long)(l + r) * cropX;
        long h = (long)height - (long)(t + b) * cropY;
        if (l > 8192 || r > 8192 || t > 8192 || b > 8192 || w <= 0 || h <= 0) return false;
        width = (int)w;
        height = (int)h;
    }
    s.width = width;
    s.height = height;

    if (br.u(1)) {                             // vui_parameters_present_flag
        if (br.u(1)) {                         // aspect_ratio_info_present_flag
            uint32_t idc = br.u(8);
            if (idc == 255) {
                s.sar_num = (int)br.u(16);
                s.sar_den = (int)br.u(16);
            } else if (idc < 17) {
                s.sar_num = kSar[idc][0];
                s.sar_den = kSar[idc][1];
            }
        }
        if (br.u(1)) br.u(1);                  // overscan_info_present_flag, overscan_appropriate_flag
        if (br.u(1)) {                         // video_signal_type_present_flag
            br.u(3);                           // video_format
            s.video_full_range = (int)br.u(1);
            if (br.u(1)) {                     // colour_description_present_flag
                s.colour_primaries = (int)br.u(8);
                s.transfer_characteristics = (int)br.u(8);
                s.matrix_coefficients = (int)br.u(8);
            }
        }
        if (br.u(1)) { br.ue(); br.ue(); }     // chroma_loc_info_present_flag
        if (br.u(1)) {                         // timing_info_present_flag
            s.num_units_in_tick = br.u(32);
            s.time_scale = br.u(32);
            br.u(1);                           // fixed_frame_rate_flag
        }
        if (br.error()) {                      // truncated VUI: keep what the core SPS gave us
            s.sar_num = s.sar_den = 0;
            s.video_full_range = -1;
            s.colour_primaries = s.transfer_characteristics = s.matrix_coefficients = -1;
            s.num_units_in_tick = s.time_scale = 0;
            *out = s;
            return true;
        }
    }
    if (br.error()) return false;
    *out = s;
    return true;
}

bool XavcParseSpsFromConfig(const uint8_t* cfg, size_t len, XavcSps* out) {
    if (!cfg || len < 4) return false;
    if (cfg[0] == 1) {                          // avcC
        if (len < 8 || (cfg[5] & 0x1f) == 0) return false;
        size_t spsLen = ((size_t)cfg[6] << 8) | cfg[7];
        if (8 + spsLen > len) return false;
        return XavcParseSpsNal(cfg + 8, spsLen, out);
    }
    size_t sc = 0;
    size_t i = FindStartCode(cfg, len, 0, &sc);
    while (i < len) {
        size_t start = i + sc;
        size_t sc2 = 0;
        size_t next = FindStartCode(cfg, len, start, &sc2);
        if (start < len && (cfg[start] & 0x1f) == 7) {
            size_t end = next;
            while (end > start && cfg[end - 1] == 0) end--;   // trailing_zero_8bits
            return XavcParseSpsNal(cfg + start, end - start, out);
        }
        i = next;
        sc = sc2;
    }
    return false;
}
