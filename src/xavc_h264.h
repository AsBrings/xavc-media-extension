// xavc_h264.h - just enough H.264 SPS parsing to describe the decoded pictures before
// the first frame is decoded (chroma format, bit depth, size, aspect ratio, colour).
#pragma once
#include <stdint.h>
#include <stddef.h>

struct XavcSps {
    int profile_idc;
    int level_idc;
    int chroma_format_idc;       // 0 = 4:0:0, 1 = 4:2:0, 2 = 4:2:2, 3 = 4:4:4
    int bit_depth_luma;          // 8..14
    int bit_depth_chroma;
    int width, height;           // after frame cropping
    int frame_mbs_only;          // 0 = may contain field pictures / MBAFF
    int sar_num, sar_den;        // sample aspect ratio, 0/0 if not signalled
    int video_full_range;        // -1 = not signalled
    int colour_primaries;        // -1 = not signalled (else ISO/IEC 23091-2 code points)
    int transfer_characteristics;
    int matrix_coefficients;
    uint32_t num_units_in_tick;  // VUI timing, 0 if absent
    uint32_t time_scale;
};

// Finds the first SPS in an avcC record or an Annex-B byte string (MF_MT_MPEG_SEQUENCE_HEADER)
// and parses it. Returns false if there is none or it is malformed.
bool XavcParseSpsFromConfig(const uint8_t* cfg, size_t len, XavcSps* out);

// Parses one SPS NAL unit (starting at the NAL header byte, emulation prevention included).
bool XavcParseSpsNal(const uint8_t* nal, size_t len, XavcSps* out);
