// xavc_ffmpeg.h - libavcodec / libavutil, loaded at run time from this DLL's directory.
//
// The FFmpeg DLLs are loaded explicitly (full path) the first time a decoder needs them:
// host processes (Media Player, Explorer's thumbnail host, ...) would not search our
// install directory, and the byte stream handler - which sits in front of every .mp4 the
// user opens - must not depend on them at all. We compile against the headers of the
// exact FFmpeg build we ship next to the DLL.
#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/error.h>
}

struct XavcFF {
    // libavutil
    decltype(&::av_frame_alloc)          frame_alloc;
    decltype(&::av_frame_free)           frame_free;
    decltype(&::av_frame_unref)          frame_unref;
    decltype(&::av_frame_apply_cropping) frame_apply_cropping;
    decltype(&::av_mallocz)              mallocz;
    decltype(&::av_log_set_callback)     log_set_callback;
    decltype(&::av_log_set_level)        log_set_level;
    decltype(&::av_log_format_line2)     log_format_line2;
    decltype(&::av_hwdevice_ctx_create)  hwdevice_ctx_create;
    decltype(&::av_hwframe_transfer_data) hwframe_transfer_data;
    decltype(&::av_buffer_ref)           buffer_ref;
    decltype(&::av_buffer_unref)         buffer_unref;
    decltype(&::av_pix_fmt_desc_get)     pix_fmt_desc_get;
    decltype(&::av_get_pix_fmt_name)     get_pix_fmt_name;
    decltype(&::av_version_info)         version_info;
    decltype(&::av_strerror)             strerror_;
    // libavcodec
    decltype(&::avcodec_find_decoder)    find_decoder;
    decltype(&::avcodec_alloc_context3)  alloc_context3;
    decltype(&::avcodec_free_context)    free_context;
    decltype(&::avcodec_open2)           open2;
    decltype(&::avcodec_send_packet)     send_packet;
    decltype(&::avcodec_receive_frame)   receive_frame;
    decltype(&::avcodec_flush_buffers)   flush_buffers;
    decltype(&::avcodec_profile_name)    profile_name;
    decltype(&::av_packet_alloc)         packet_alloc;
    decltype(&::av_packet_free)          packet_free;
    decltype(&::av_packet_unref)         packet_unref;
    decltype(&::av_new_packet)           new_packet;
};

// Loads FFmpeg once per process; NULL if it is missing or incomplete (the reason is logged).
// Once loaded it stays loaded, and DllCanUnloadNow keeps this DLL loaded too.
const XavcFF* XavcLoadFFmpeg();
bool XavcFFmpegLoaded();

// FFmpeg error text, e.g. XavcAvErr(r).s (no thread-local storage: that would pull in
// libwinpthread, which host processes cannot find).
struct XavcErrText { char s[96]; };
XavcErrText XavcAvErr(int err);
