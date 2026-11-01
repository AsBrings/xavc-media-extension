// xavc_convert.cpp - planar / semi-planar YUV -> Y210 / YUY2 / P010 / NV12.
//
// Built with -O3: the per-row loops below are written so that GCC vectorizes them (SSE2).

#include "xavc_convert.h"
#include <stdlib.h>
#include <string.h>

namespace {

// 16-bit MSB-aligned -> 8 bit, rounded (0xFF80.. would round to 256: saturate).
inline uint8_t To8(uint16_t v) {
    uint16_t t = (uint16_t)((v >> 8) + ((v >> 7) & 1));
    return (uint8_t)(t - (t >> 8));
}

template <int STEP>
void Load8(const uint8_t* __restrict src, uint16_t* __restrict dst, int n) {
    for (int i = 0; i < n; i++) dst[i] = (uint16_t)(src[i * STEP] << 8);
}

template <int STEP>
void Load16(const uint16_t* __restrict src, uint16_t* __restrict dst, int n, int rs, int ls) {
    if (rs == 0) {
        for (int i = 0; i < n; i++) dst[i] = (uint16_t)(src[i * STEP] << ls);
    } else {
        for (int i = 0; i < n; i++) dst[i] = (uint16_t)((src[i * STEP] >> rs) << ls);
    }
}

void Average(uint16_t* __restrict a, const uint16_t* __restrict b, int n) {
    for (int i = 0; i < n; i++) a[i] = (uint16_t)(((uint32_t)a[i] + b[i] + 1) >> 1);
}

void AveragePairs(const uint16_t* __restrict src, uint16_t* __restrict dst, int n) {
    for (int i = 0; i < n; i++) dst[i] = (uint16_t)(((uint32_t)src[2 * i] + src[2 * i + 1] + 1) >> 1);
}

// Scratch rows for one worker (a slice of XavcConverter's preallocated memory).
const size_t kRowsPerSlot = 7;
inline size_t RowLen(int w) { return ((size_t)w + 16) & ~(size_t)15; }   // in uint16_t

struct Rows {
    uint16_t *Y, *U, *V, *U2, *V2, *T, *T2;
    Rows(uint16_t* p, size_t lw) {
        Y = p; U = p + lw; V = p + 2 * lw; U2 = p + 3 * lw; V2 = p + 4 * lw; T = p + 5 * lw; T2 = p + 6 * lw;
    }
};

void LoadLuma(const XavcSrcPlanes& s, int y, uint16_t* Y, int w) {
    const uint8_t* row = s.data[0] + (ptrdiff_t)y * s.linesize[0];
    if (s.bytes == 1) Load8<1>(row, Y, w);
    else Load16<1>((const uint16_t*)row, Y, w, s.shift, 16 - s.depth);
}

// Chroma of source chroma row srow at the output's horizontal resolution (cw = width / 2).
void LoadChroma(const XavcSrcPlanes& s, int srow, uint16_t* U, uint16_t* V, int cw, Rows& r) {
    if (s.gray) {
        for (int i = 0; i < cw; i++) { U[i] = 0x8000; V[i] = 0x8000; }
        return;
    }
    const bool half = s.log2_cw != 0;      // source already has half-width chroma
    const int n = half ? cw : 2 * cw;
    uint16_t* tu = half ? U : r.T;
    uint16_t* tv = half ? V : r.T2;
    const int ls = 16 - s.depth;
    if (s.semi) {
        const uint8_t* row = s.data[1] + (ptrdiff_t)srow * s.linesize[1];
        if (s.bytes == 1) {
            Load8<2>(row + (s.vu ? 1 : 0), tu, n);
            Load8<2>(row + (s.vu ? 0 : 1), tv, n);
        } else {
            const uint16_t* r16 = (const uint16_t*)row;
            Load16<2>(r16 + (s.vu ? 1 : 0), tu, n, s.shift, ls);
            Load16<2>(r16 + (s.vu ? 0 : 1), tv, n, s.shift, ls);
        }
    } else {
        const uint8_t* ru = s.data[1] + (ptrdiff_t)srow * s.linesize[1];
        const uint8_t* rv = s.data[2] + (ptrdiff_t)srow * s.linesize[2];
        if (s.bytes == 1) {
            Load8<1>(ru, tu, n);
            Load8<1>(rv, tv, n);
        } else {
            Load16<1>((const uint16_t*)ru, tu, n, s.shift, ls);
            Load16<1>((const uint16_t*)rv, tv, n, s.shift, ls);
        }
    }
    if (!half) {
        AveragePairs(r.T, U, cw);
        AveragePairs(r.T2, V, cw);
    }
}

void PackLuma8(const uint16_t* __restrict Y, uint8_t* __restrict d, int w) {
    for (int i = 0; i < w; i++) d[i] = To8(Y[i]);
}
void PackLuma10(const uint16_t* __restrict Y, uint16_t* __restrict d, int w) {
    for (int i = 0; i < w; i++) d[i] = (uint16_t)(Y[i] & 0xFFC0);
}
void PackUV8(const uint16_t* __restrict U, const uint16_t* __restrict V, uint8_t* __restrict d, int cw) {
    for (int i = 0; i < cw; i++) { d[2 * i] = To8(U[i]); d[2 * i + 1] = To8(V[i]); }
}
void PackUV10(const uint16_t* __restrict U, const uint16_t* __restrict V, uint16_t* __restrict d, int cw) {
    for (int i = 0; i < cw; i++) { d[2 * i] = (uint16_t)(U[i] & 0xFFC0); d[2 * i + 1] = (uint16_t)(V[i] & 0xFFC0); }
}
void PackYUY2(const uint16_t* __restrict Y, const uint16_t* __restrict U, const uint16_t* __restrict V,
              uint8_t* __restrict d, int cw) {
    for (int i = 0; i < cw; i++) {
        d[4 * i + 0] = To8(Y[2 * i]);
        d[4 * i + 1] = To8(U[i]);
        d[4 * i + 2] = To8(Y[2 * i + 1]);
        d[4 * i + 3] = To8(V[i]);
    }
}
void PackY210(const uint16_t* __restrict Y, const uint16_t* __restrict U, const uint16_t* __restrict V,
              uint16_t* __restrict d, int cw) {
    for (int i = 0; i < cw; i++) {
        d[4 * i + 0] = (uint16_t)(Y[2 * i] & 0xFFC0);
        d[4 * i + 1] = (uint16_t)(U[i] & 0xFFC0);
        d[4 * i + 2] = (uint16_t)(Y[2 * i + 1] & 0xFFC0);
        d[4 * i + 3] = (uint16_t)(V[i] & 0xFFC0);
    }
}

// Source chroma row(s) for output 4:2:0 chroma row j. In an interlaced frame the rows
// alternate between the two fields, so only rows of the same field may be combined.
inline void ChromaRowsFor420(const XavcSrcPlanes& s, int j, int srcRows, int* r0, int* r1) {
    if (s.log2_ch) {                                   // already 4:2:0 (same field layout)
        *r0 = *r1 = j < srcRows ? j : srcRows - 1;
        return;
    }
    if (s.interlaced) {                                // field p = j & 1: rows 4k+p and 4k+2+p
        *r0 = 2 * j - (j & 1);
        *r1 = *r0 + 2;
    } else {
        *r0 = 2 * j;
        *r1 = 2 * j + 1;
    }
    if (*r0 >= srcRows) *r0 = srcRows - 1;
    if (*r1 >= srcRows) *r1 = *r0;
}

// Source chroma row for output (4:2:2) row y.
inline int ChromaRowFor422(const XavcSrcPlanes& s, int y, int srcRows) {
    int sr = y;
    if (s.log2_ch) sr = s.interlaced ? (((y >> 2) << 1) | (y & 1)) : (y >> 1);
    while (sr >= srcRows && sr >= 2) sr -= 2;          // keep the field parity when clamping
    return sr < srcRows ? sr : srcRows - 1;
}

// Rows [y0, y1) of the output; y0 and y1 even.
void ConvertRows(const XavcSrcPlanes& s, XavcOutFmt fmt, const XavcDstPlanes& d, int w, int y0, int y1, Rows& r) {
    const int cw = w / 2;
    const int srcChromaRows = (s.height + (1 << s.log2_ch) - 1) >> s.log2_ch;
    switch (fmt) {
    case XOF_NV12:
    case XOF_P010:
        for (int y = y0; y < y1; y += 2) {
            for (int k = 0; k < 2; k++) {
                LoadLuma(s, y + k, r.Y, w);
                uint8_t* dl = d.p0 + (ptrdiff_t)(y + k) * d.pitch;
                if (fmt == XOF_NV12) PackLuma8(r.Y, dl, w); else PackLuma10(r.Y, (uint16_t*)dl, w);
            }
            const int j = y / 2;
            int r0 = 0, r1 = 0;
            ChromaRowsFor420(s, j, srcChromaRows, &r0, &r1);
            LoadChroma(s, r0, r.U, r.V, cw, r);
            if (r1 != r0) {                              // 4:2:2 / 4:4:4 -> 4:2:0: average two rows
                LoadChroma(s, r1, r.U2, r.V2, cw, r);
                Average(r.U, r.U2, cw);
                Average(r.V, r.V2, cw);
            }
            uint8_t* dc = d.p1 + (ptrdiff_t)j * d.pitch;
            if (fmt == XOF_NV12) PackUV8(r.U, r.V, dc, cw); else PackUV10(r.U, r.V, (uint16_t*)dc, cw);
        }
        break;
    case XOF_YUY2:
    case XOF_Y210:
        for (int y = y0; y < y1; y++) {
            LoadLuma(s, y, r.Y, w);
            LoadChroma(s, ChromaRowFor422(s, y, srcChromaRows), r.U, r.V, cw, r);
            uint8_t* dl = d.p0 + (ptrdiff_t)y * d.pitch;
            if (fmt == XOF_YUY2) PackYUY2(r.Y, r.U, r.V, dl, cw); else PackY210(r.Y, r.U, r.V, (uint16_t*)dl, cw);
        }
        break;
    default:
        break;
    }
}

} // namespace

void XavcFrameLayout(XavcOutFmt f, UINT32 w, UINT32 h, UINT32* stride, UINT32* size) {
    switch (f) {
    case XOF_NV12: *stride = w;     *size = w * h + w * (h / 2); break;
    case XOF_P010: *stride = w * 2; *size = w * 2 * h + w * 2 * (h / 2); break;
    case XOF_YUY2: *stride = w * 2; *size = w * 2 * h; break;
    case XOF_Y210: *stride = w * 4; *size = w * 4 * h; break;
    default:       *stride = 0;     *size = 0; break;
    }
}

XavcConverter::XavcConverter()
    : m_work(NULL), m_workers(1), m_s(NULL), m_d(NULL), m_fmt(XOF_NONE),
      m_w(0), m_h(0), m_bandRows(0), m_bands(0), m_next(0), m_scratch(NULL), m_scratchW(0), m_slotNext(0) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    m_workers = (int)si.dwNumberOfProcessors;
    if (m_workers > 4) m_workers = 4;
    if (m_workers > 1) m_work = CreateThreadpoolWork(WorkCallback, this, NULL);
    if (!m_work) m_workers = 1;
}

XavcConverter::~XavcConverter() {
    if (m_work) {
        WaitForThreadpoolWorkCallbacks(m_work, FALSE);
        CloseThreadpoolWork(m_work);
    }
    free(m_scratch);
}

void CALLBACK XavcConverter::WorkCallback(PTP_CALLBACK_INSTANCE, PVOID ctx, PTP_WORK) {
    static_cast<XavcConverter*>(ctx)->DoBands();
}

void XavcConverter::DoBands() {
    // Each participating thread (at most m_workers) takes its own scratch slot.
    const LONG slot = InterlockedIncrement(&m_slotNext) - 1;
    const size_t lw = RowLen(m_scratchW);
    Rows rows((uint16_t*)m_scratch + (size_t)slot * kRowsPerSlot * lw, lw);
    for (;;) {
        LONG b = InterlockedIncrement(&m_next) - 1;
        if (b >= m_bands) return;
        int y0 = (int)b * m_bandRows;
        int y1 = y0 + m_bandRows;
        if (y1 > m_h) y1 = m_h;
        ConvertRows(*m_s, m_fmt, *m_d, m_w, y0, y1, rows);
    }
}

bool XavcConverter::Run(const XavcSrcPlanes& s, XavcOutFmt fmt, const XavcDstPlanes& d, int width, int height) {
    width &= ~1;
    height &= ~1;
    if (width <= 0 || height <= 0 || width > s.width || height > s.height) return false;
    if (!m_scratch || m_scratchW < width) {            // scratch rows for every worker, kept between frames
        void* p = malloc((size_t)m_workers * kRowsPerSlot * RowLen(width) * sizeof(uint16_t));
        if (!p) return false;
        free(m_scratch);
        m_scratch = p;
        m_scratchW = width;
    }
    m_s = &s; m_d = &d; m_fmt = fmt; m_w = width; m_h = height;
    m_slotNext = 0;
    int bands = m_work ? m_workers * 3 : 1;
    int rows = (height + bands - 1) / bands;
    rows = (rows + 1) & ~1;
    if (rows < 16) rows = 16;
    m_bandRows = rows;
    m_bands = (height + rows - 1) / rows;
    m_next = 0;
    int helpers = m_bands - 1 < m_workers - 1 ? m_bands - 1 : m_workers - 1;
    for (int i = 0; i < helpers; i++) SubmitThreadpoolWork(m_work);
    DoBands();                                           // the calling thread works too
    if (helpers > 0) WaitForThreadpoolWorkCallbacks(m_work, FALSE);
    return true;
}
