XAVC S / S-I 解码插件
======================

让 Windows 自带的「媒体播放器」（新版）、资源管理器缩略图，以及其他使用 Media Foundation 的程序，
能播放索尼相机拍摄的 XAVC S 和 XAVC S-I 视频（H.264 High 4:2:2 / High 10，.MP4）。
XAVC HS（HEVC）由微软的 HEVC 扩展负责，本插件不涉及。

安装 / 卸载
-----------
安装：运行 XavcDecoder-<版本>-setup.exe，按向导操作（会请求一次管理员权限）。
      如果正在运行「媒体播放器」，装完后把它关掉再打开。
卸载：设置 → 应用 → 安装的应用（Windows 10 为「应用和功能」）→ XAVC S / S-I Decoder。

（开发者也可以用源码仓库 installer\ 目录里的 install.cmd / uninstall.cmd 脚本安装。）

说明
----
- 只接管系统自带解码器无法解码的 H.264 视频（4:2:2、10bit、4:4:4）。普通 MP4 仍由 Windows 自带
  解码器处理，不受影响。
- 解码在 CPU 上进行（多线程）。H.264 4:2:2 目前只有 NVIDIA RTX 50 系列等少数显卡能硬解；
  检测到可用的 NVIDIA 解码能力时会自动使用。
- 电脑解码速度跟不上时（例如轻薄本播放 4K 60p），会自动跳过部分帧，保持音画同步。
- 4:2:2 色度：如果显卡支持 Y210 格式，画面保持完整的 4:2:2 10bit；否则使用 P010（10bit 4:2:0）。
- 已经生成过的缩略图不会自动更新：可以在安装向导最后一页选择清空缩略图缓存，
  或用「磁盘清理 → 缩略图」。
- 设置（注册表 HKLM\SOFTWARE\XavcDecoder 或 HKCU\Software\XavcDecoder，DWORD）：
    HardwareDecode  1 = 自动使用 GPU 解码（默认），0 = 只用 CPU
    OutputFormat    0 = 自动（默认），1 = 只输出 4:2:0（P010/NV12），2 = 只输出 NV12
    DecoderThreads  0 = 自动（默认）
    LogLevel        0 = 关闭（默认），1 = 一般，2 = 详细；日志写到 %ProgramData%\XavcDecoder\xavc.log
                    （该目录需要允许「所有应用程序包」写入，媒体播放器才能写日志）

系统要求：64 位（x64）Windows 10 / 11。

许可：本插件采用 MIT 许可（LICENSE.txt）。解码核心使用 FFmpeg（LGPL 2.1+，精简构建，以独立 DLL 动态加载），
见 THIRD_PARTY_NOTICES.txt 与 licenses\ 目录。
源码：https://github.com/AsBrings/xavc-media-extension


XAVC S / S-I decoder (English)
------------------------------
Lets the Windows "Media Player" app, Explorer thumbnails and other Media Foundation programs
play Sony XAVC S and XAVC S-I videos (H.264 High 4:2:2 / High 10 in .MP4).

Install: run XavcDecoder-<version>-setup.exe and follow the wizard; restart Media Player if it is open.
Uninstall: Settings > Apps > Installed apps (Windows 10: Apps & features) > XAVC S / S-I Decoder.

Only H.264 streams the built-in decoder cannot handle (4:2:2, 10-bit, 4:4:4) are taken over; all
other MP4 files keep using the Windows decoder. Decoding runs on the CPU (multi-threaded); NVIDIA
GPUs that can decode H.264 4:2:2 (e.g. RTX 50 series) are used automatically. When the computer
cannot decode fast enough, frames are skipped to stay in sync with the audio.
Licence: MIT; FFmpeg (LGPL 2.1+, minimal build) is used as separate DLLs, see THIRD_PARTY_NOTICES.txt.
Source: https://github.com/AsBrings/xavc-media-extension
