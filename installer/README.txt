XAVC S / S-I 解码插件 0.1.0
============================

让 Windows 自带的「媒体播放器」（新版）、资源管理器缩略图，以及其他使用 Media Foundation 的程序，
能播放索尼相机拍摄的 XAVC S 和 XAVC S-I 视频（H.264 High 4:2:2 / High 10，.MP4）。
XAVC HS（HEVC）由微软的 HEVC 扩展负责，本插件不涉及。

安装
----
1. 解压整个文件夹。
2. 双击 install.cmd，在弹出的管理员权限窗口中点「是」。
3. 如果媒体播放器正开着，关掉再打开。

卸载：双击 uninstall.cmd，或在「设置 → 应用 → 已安装的应用」中卸载
「XAVC S / S-I Decoder (Media Foundation)」。

说明
----
- 只接管系统自带解码器无法解码的 H.264 视频（4:2:2、10bit、4:4:4）。普通 MP4 仍由 Windows 自带
  解码器处理，不受影响。
- 解码在 CPU 上进行（多线程）。H.264 4:2:2 目前只有 NVIDIA RTX 50 系列等少数显卡能硬解；
  检测到可用的 NVIDIA 解码能力时会自动使用。
- 电脑解码速度跟不上时（例如轻薄本播放 4K 60p），会自动降低画质、跳过部分帧以保持音画同步。
- 4:2:2 色度：如果显卡支持 Y210 格式，画面保持完整的 4:2:2 10bit；否则使用 P010（10bit 4:2:0）。
- 已经生成过的缩略图不会自动更新。可以用「磁盘清理 → 缩略图」清空缓存，或运行
  install.cmd -ResetThumbnails（会重启资源管理器）。
- 诊断日志：install.cmd -LogLevel 1，日志在 %ProgramData%\XavcDecoder\xavc.log。
- 设置（注册表 HKLM\SOFTWARE\XavcDecoder 或 HKCU\Software\XavcDecoder，DWORD）：
    HardwareDecode  1 = 自动使用 GPU 解码（默认），0 = 只用 CPU
    OutputFormat    0 = 自动（默认），1 = 只输出 4:2:0（P010/NV12），2 = 只输出 NV12
    DecoderThreads  0 = 自动（默认）
    LogLevel        0 = 关闭（默认），1 = 一般，2 = 详细

系统要求：64 位 Windows 10 / 11。

许可：本插件采用 MIT 许可（LICENSE.txt）。解码核心使用 FFmpeg（LGPL 2.1+，精简构建，以独立 DLL 动态加载），
见 THIRD_PARTY_NOTICES.txt 与 licenses\ 目录。


XAVC S / S-I decoder 0.1.0 (English)
------------------------------------
Lets the Windows "Media Player" app, Explorer thumbnails and other Media Foundation programs
play Sony XAVC S and XAVC S-I videos (H.264 High 4:2:2 / High 10 in .MP4).

Install: run install.cmd and accept the administrator prompt; restart Media Player if it is open.
Uninstall: run uninstall.cmd, or remove "XAVC S / S-I Decoder (Media Foundation)" in Settings > Apps.

Only H.264 streams the built-in decoder cannot handle (4:2:2, 10-bit, 4:4:4) are taken over; all
other MP4 files keep using the Windows decoder. Decoding runs on the CPU (multi-threaded); NVIDIA
GPUs that can decode H.264 4:2:2 (e.g. RTX 50 series) are used automatically. When the computer
cannot decode fast enough, quality is reduced and frames are skipped to stay in sync with the audio.
Licence: MIT; FFmpeg (LGPL 2.1+, minimal build) is used as separate DLLs, see THIRD_PARTY_NOTICES.txt.
