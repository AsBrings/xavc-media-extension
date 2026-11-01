#!/usr/bin/env python3
"""make_test_clip.py - cut a short, valid MP4 test clip out of a (remote or local)
camera MP4 without downloading the whole file.

* Only the box headers, the moov index and the bytes of the kept samples are read
  (HTTP Range requests for URLs), so a few-second clip of a multi-GB file costs a
  few tens of MB.
* Keeps the video track only.
* Cuts in decode order right before a sync sample (IDR), so every kept Long-GOP
  B/P frame still has its references; preserves ctts (composition offsets), stss,
  sdtp and the edit list.

usage: python -I make_test_clip.py <url-or-path> <out.mp4> [--seconds N]
"""
import argparse
import os
import struct
import sys
import time
import urllib.request

# Some hosts reject urllib's default User-Agent (403 / dropped TLS handshake).
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36"


def fetch(url, headers=None, method="GET", tries=4):
    for attempt in range(tries):
        try:
            req = urllib.request.Request(url, method=method, headers={"User-Agent": UA, **(headers or {})})
            with urllib.request.urlopen(req, timeout=600) as r:
                return r.headers, (r.read() if method != "HEAD" else b"")
        except Exception:
            if attempt == tries - 1:
                raise
            time.sleep(2 ** attempt)


class Source:
    def __init__(self, loc):
        self.loc = loc
        self.remote = loc.startswith(("http://", "https://"))
        if self.remote:
            hdrs, _ = fetch(loc, method="HEAD")
            self.size = int(hdrs["Content-Length"])
        else:
            self.size = os.path.getsize(loc)
            self.f = open(loc, "rb")

    def read(self, off, n, chunk=32 << 20):
        if n <= 0:
            return b""
        if not self.remote:
            self.f.seek(off)
            return self.f.read(n)
        out = bytearray()
        while len(out) < n:
            a = off + len(out)
            b = min(off + n, a + chunk) - 1
            _, data = fetch(self.loc, headers={"Range": f"bytes={a}-{b}"})
            if not data:
                raise IOError(f"empty read at {a}")
            out += data
        if len(out) != n:
            raise IOError(f"short read {len(out)}/{n} at {off}")
        return bytes(out)


def iter_boxes(buf, start, end):
    i = start
    while i + 8 <= end:
        size, typ = struct.unpack_from(">I4s", buf, i)
        hdr = 8
        if size == 1:
            size = struct.unpack_from(">Q", buf, i + 8)[0]
            hdr = 16
        elif size == 0:
            size = end - i
        if size < hdr:
            break
        yield typ, i, hdr, size
        i += size


def find(buf, s, e, *path):
    """Return (content_start, end, box_start) of the first box matching path."""
    cur = (s, e, s)
    for tag in path:
        for typ, o, h, sz in iter_boxes(buf, cur[0], cur[1]):
            if typ == tag:
                cur = (o + h, o + sz, o)
                break
        else:
            return None
    return cur


def raw(buf, box):
    return bytes(buf[box[2]:box[1]])


def mkbox(tag, payload):
    return struct.pack(">I4s", 8 + len(payload), tag) + payload


def full(tag, version, flags, payload):
    return mkbox(tag, struct.pack(">I", (version << 24) | flags) + payload)


def top_level(src):
    """Scan top-level boxes reading only headers."""
    boxes, i = [], 0
    while i + 8 <= src.size:
        h = src.read(i, 16 if i + 16 <= src.size else 8)
        size, typ = struct.unpack_from(">I4s", h, 0)
        hdr = 8
        if size == 1:
            size = struct.unpack_from(">Q", h, 8)[0]
            hdr = 16
        elif size == 0:
            size = src.size - i
        boxes.append((typ, i, hdr, size))
        i += size
    return boxes


def video_trak(moov):
    for typ, o, h, sz in iter_boxes(moov, 8, len(moov)):
        if typ != b"trak":
            continue
        hd = find(moov, o + h, o + sz, b"mdia", b"hdlr")
        if hd and moov[hd[0] + 8:hd[0] + 12] == b"vide":
            return (o + h, o + sz, o)
    raise SystemExit("no video track")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--seconds", type=float, default=3.0)
    a = ap.parse_args()

    src = Source(a.src)
    tops = top_level(src)
    ftyp = next(b for b in tops if b[0] == b"ftyp")
    moovb = next(b for b in tops if b[0] == b"moov")
    ftyp_raw = src.read(ftyp[1], ftyp[3])
    moov = src.read(moovb[1], moovb[3])          # whole moov box (index only)

    ts, te, tb = video_trak(moov)
    sb, se, _ = find(moov, ts, te, b"mdia", b"minf", b"stbl")

    def sub(tag):
        return find(moov, sb, se, tag)

    # --- timing ---
    md = find(moov, ts, te, b"mdia", b"mdhd")
    mver = moov[md[0]]
    mts = struct.unpack_from(">I", moov, md[0] + (20 if mver == 1 else 12))[0]
    mv = find(moov, 8, len(moov), b"mvhd")
    vver = moov[mv[0]]
    mvts = struct.unpack_from(">I", moov, mv[0] + (20 if vver == 1 else 12))[0]

    o = sub(b"stts")[0]
    deltas = []
    for k in range(struct.unpack_from(">I", moov, o + 4)[0]):
        c, d = struct.unpack_from(">II", moov, o + 8 + 8 * k)
        deltas += [d] * c

    o = sub(b"stsz")[0]
    dflt, count = struct.unpack_from(">II", moov, o + 4)
    sizes = list(struct.unpack_from(f">{count}I", moov, o + 12)) if dflt == 0 else [dflt] * count

    ctts = None
    cb = sub(b"ctts")
    if cb:
        cver = moov[cb[0]]
        ctts = []
        for k in range(struct.unpack_from(">I", moov, cb[0] + 4)[0]):
            c, off = struct.unpack_from(">Ii" if cver == 1 else ">II", moov, cb[0] + 8 + 8 * k)
            ctts += [off] * c

    sync = None
    sb_ss = sub(b"stss")
    if sb_ss:
        n = struct.unpack_from(">I", moov, sb_ss[0] + 4)[0]
        sync = list(struct.unpack_from(f">{n}I", moov, sb_ss[0] + 8))

    sdtp = sub(b"sdtp")

    # --- sample file offsets (stsc + stco/co64) ---
    o = sub(b"stsc")[0]
    stsc = [struct.unpack_from(">III", moov, o + 8 + 12 * k)
            for k in range(struct.unpack_from(">I", moov, o + 4)[0])]
    co = sub(b"stco")
    is64 = co is None
    if is64:
        co = sub(b"co64")
    n = struct.unpack_from(">I", moov, co[0] + 4)[0]
    chunks = list(struct.unpack_from(f">{n}{'Q' if is64 else 'I'}", moov, co[0] + 8))
    offs, si = [], 0
    for ci, base in enumerate(chunks):
        per = stsc[0][1]
        for first, spc, _ in stsc:
            if ci + 1 >= first:
                per = spc
        cur = base
        for _ in range(per):
            if si >= count:
                break
            offs.append(cur)
            cur += sizes[si]
            si += 1

    # --- choose cut: just before the first IDR at/after the target frame ---
    fps = mts / deltas[0]
    target = max(1, round(a.seconds * fps))
    if sync is None or len(sync) == count:
        K = min(count, target)
    else:
        later = [s - 1 for s in sync if s - 1 >= target]
        K = later[0] if later else count
    K = max(1, K)

    # --- fetch only the bytes that hold the kept samples ---
    lo = min(offs[:K])
    hi = max(offs[i] + sizes[i] for i in range(K))
    blob = src.read(lo, hi - lo)
    data = b"".join(blob[offs[i] - lo:offs[i] - lo + sizes[i]] for i in range(K))

    media_dur = sum(deltas[:K])

    # --- rebuild sample tables for K samples ---
    def runs(vals):
        out = []
        for v in vals:
            if out and out[-1][1] == v:
                out[-1][0] += 1
            else:
                out.append([1, v])
        return out

    tbl = raw(moov, sub(b"stsd"))
    r = runs(deltas[:K])
    tbl += full(b"stts", 0, 0, struct.pack(">I", len(r)) + b"".join(struct.pack(">II", c, v) for c, v in r))
    if ctts is not None:
        r = runs(ctts[:K])
        tbl += full(b"ctts", cver, 0, struct.pack(">I", len(r)) +
                    b"".join(struct.pack(">Ii" if cver == 1 else ">II", c, v) for c, v in r))
    if sync is not None:
        kept = [s for s in sync if s <= K]
        tbl += full(b"stss", 0, 0, struct.pack(">I", len(kept)) + b"".join(struct.pack(">I", s) for s in kept))
    tbl += full(b"stsc", 0, 0, struct.pack(">IIII", 1, 1, K, 1))
    tbl += full(b"stsz", 0, 0, struct.pack(">II", 0, K) + b"".join(struct.pack(">I", s) for s in sizes[:K]))
    stco_at = len(tbl) + 16           # offset of the single chunk-offset value inside tbl
    tbl += full(b"stco", 0, 0, struct.pack(">II", 1, 0))
    if sdtp:
        tbl += full(b"sdtp", 0, 0, bytes(moov[sdtp[0] + 4:sdtp[0] + 4 + K]))
    stbl = mkbox(b"stbl", tbl)

    minf_s, minf_e, _ = find(moov, ts, te, b"mdia", b"minf")
    minf_body = b"".join(raw(moov, (oo + hh, oo + ss, oo))
                         for t, oo, hh, ss in iter_boxes(moov, minf_s, minf_e) if t != b"stbl")
    minf = mkbox(b"minf", minf_body + stbl)

    mdhd = bytearray(raw(moov, md))
    if mver == 1:
        struct.pack_into(">Q", mdhd, 8 + 24, media_dur)
    else:
        struct.pack_into(">I", mdhd, 8 + 16, media_dur)
    hdlr = raw(moov, find(moov, ts, te, b"mdia", b"hdlr"))
    mdia = mkbox(b"mdia", bytes(mdhd) + hdlr + minf)

    mov_dur = round(media_dur * mvts / mts)
    edts = b""
    el = find(moov, ts, te, b"edts", b"elst")
    if el:
        ev = moov[el[0]]
        n = struct.unpack_from(">I", moov, el[0] + 4)[0]
        ents = []
        p = el[0] + 8
        for _ in range(n):
            if ev == 1:
                seg, mt, rate = struct.unpack_from(">QqI", moov, p)
                p += 20
            else:
                seg, mt, rate = struct.unpack_from(">IiI", moov, p)
                p += 12
            ents.append((seg, mt, rate))
        new = []
        for seg, mt, rate in ents:
            if mt >= 0:
                seg = max(1, round((media_dur - mt) * mvts / mts))
                mov_dur = seg
            new.append((seg, mt, rate))
        fmt = ">QqI" if ev == 1 else ">IiI"
        edts = mkbox(b"edts", full(b"elst", ev, 0, struct.pack(">I", len(new)) +
                                   b"".join(struct.pack(fmt, *e) for e in new)))

    tk = find(moov, ts, te, b"tkhd")
    tkhd = bytearray(raw(moov, tk))
    if moov[tk[0]] == 1:
        struct.pack_into(">Q", tkhd, 8 + 28, mov_dur)
    else:
        struct.pack_into(">I", tkhd, 8 + 20, mov_dur)
    trak = mkbox(b"trak", bytes(tkhd) + edts + mdia)

    mvhd = bytearray(raw(moov, mv))
    if vver == 1:
        struct.pack_into(">Q", mvhd, 8 + 24, mov_dur)
    else:
        struct.pack_into(">I", mvhd, 8 + 16, mov_dur)
    moov_new = bytearray(mkbox(b"moov", bytes(mvhd) + trak))

    # patch the chunk offset: stbl is the last box inside moov (moov>trak>mdia>minf>stbl)
    stbl_pos = len(moov_new) - len(stbl)
    data_off = len(ftyp_raw) + len(moov_new) + 8
    struct.pack_into(">I", moov_new, stbl_pos + 8 + stco_at, data_off)

    with open(a.out, "wb") as f:
        f.write(ftyp_raw)
        f.write(moov_new)
        f.write(struct.pack(">I4s", 8 + len(data), b"mdat"))
        f.write(data)

    av = find(moov, sb, se, b"stsd")
    prof = None
    i = bytes(moov[av[0]:av[1]]).find(b"avcC")
    if i >= 0:
        prof = (moov[av[0] + i + 5], moov[av[0] + i + 7])
    print(f"source : {src.size/1e6:.1f} MB, {count} frames @ {fps:.3f} fps, "
          f"sync={'all' if sync is None or len(sync) == count else len(sync)}, "
          f"ctts={'yes' if ctts is not None else 'no'}, elst={'yes' if el else 'no'}")
    if prof:
        print(f"avcC   : profile_idc={prof[0]} level_idc={prof[1]}")
    print(f"clip   : {K} frames = {media_dur/mts:.3f}s, fetched {(hi-lo)/1e6:.1f} MB -> {a.out} "
          f"({os.path.getsize(a.out)/1e6:.1f} MB)")


if __name__ == "__main__":
    sys.exit(main())
