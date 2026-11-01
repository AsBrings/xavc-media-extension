// xavc_convert.h - copy decoded pictures into Media Foundation output buffers.
//
// libavcodec delivers planar YUV (yuv422p10le for XAVC S / S-I, yuv420p10le for 4:2:0 10-bit,
// semi-planar P210/P216/P010/NV12/NV16 from GPU decoders). Media Foundation wants one of
//   Y210 - packed 4:2:2, 10 bit (keeps the full XAVC chroma resolution)
//   YUY2 - packed 4:2:2, 8 bit
//   P010 - semi-planar 4:2:0, 10 bit
//   NV12 - semi-planar 4:2:0, 8 bit
// Rows are converted through 16-bit MSB-aligned intermediates; chroma is averaged when a
// format halves it (4:2:2 -> 4:2:0, 4:4:4 -> 4:2:x), and 8-bit outputs are rounded.
// Bands of rows run in parallel on the Windows thread pool.
#pragma once
#include <windows.h>
#include <stdint.h>

enum XavcOutFmt { XOF_NONE = 0, XOF_NV12, XOF_P010, XOF_YUY2, XOF_Y210 };

struct XavcSrcPlanes {
    const uint8_t* data[3];   // Y, U (or interleaved UV), V
    int linesize[3];          // bytes per row
    int width, height;        // luma size of the decoded picture
    int log2_cw, log2_ch;     // chroma subsampling shifts (0 or 1)
    int depth;                // significant bits per sample (8..16)
    int bytes;                // bytes per sample: 1 or 2
    int shift;                // stored sample >> shift = value (P010-style layouts: 6)
    int semi;                 // U and V interleaved in data[1]
    int vu;                   // ... with V first (NV21-style)
    int gray;                 // no chroma at all (4:0:0): neutral chroma is written
    int interlaced;           // field-interleaved frame: chroma is only combined within a field
};

struct XavcDstPlanes {
    uint8_t* p0;              // luma plane (NV12 / P010) or the packed picture (YUY2 / Y210)
    uint8_t* p1;              // interleaved chroma plane (NV12 / P010)
    long pitch;               // bytes per row, both planes
};

class XavcConverter {
public:
    XavcConverter();
    ~XavcConverter();
    // Converts the top-left width x height region (both even, within the source picture).
    // False if nothing was converted (bad arguments or out of memory).
    bool Run(const XavcSrcPlanes& s, XavcOutFmt fmt, const XavcDstPlanes& d, int width, int height);

private:
    static void CALLBACK WorkCallback(PTP_CALLBACK_INSTANCE, PVOID ctx, PTP_WORK);
    void DoBands();

    PTP_WORK m_work;
    int m_workers;
    // the job being run
    const XavcSrcPlanes* m_s;
    const XavcDstPlanes* m_d;
    XavcOutFmt m_fmt;
    int m_w, m_h, m_bandRows, m_bands;
    volatile LONG m_next;
    void* m_scratch;          // m_workers slots of scratch rows for width m_scratchW
    int m_scratchW;
    volatile LONG m_slotNext;
};

// Bytes per row of plane 0 (default stride) and the whole buffer, for a width x height picture.
void XavcFrameLayout(XavcOutFmt f, UINT32 width, UINT32 height, UINT32* stride, UINT32* size);
