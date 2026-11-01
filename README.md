# XAVC Media Extension

让 **Windows 自带的「媒体播放器」（新版，Microsoft.ZuneMusic）**、资源管理器缩略图，以及其他使用
Media Foundation 的程序，直接播放索尼相机拍摄的 **XAVC S / XAVC S-I**（H.264 High 4:2:2 10bit）视频。
不依赖旧版 Windows Media Player，不依赖第三方播放器，不需要微软签名或审批。

## 支持的格式

| 格式 | 编码 | 由谁解码 |
|------|------|----------|
| XAVC S（Long GOP） | H.264 High 4:2:2 10bit / High 10 4:2:0 | **本插件** |
| XAVC S-I（All-Intra） | H.264 High 4:2:2 Intra 10bit | **本插件** |
| XAVC S 8bit 4:2:0 | H.264 High | Windows 自带解码器（不受影响） |
| XAVC HS | HEVC 4:2:2 10bit | 微软 HEVC 扩展（不受影响） |

## 工作原理（详见 [`docs/feasibility.md`](docs/feasibility.md)）

Windows 把 H.264 写死交给自带解码器，而它不支持 4:2:2 / 10bit。本插件不和它抢：

1. **字节流处理器**（每用户注册）用系统自带的 MP4 解析器打开文件，只把自带解码器解不了的
   H.264 视频流（profile 110/122/244/44）标记成私有格式 `XAVC`；
2. **解码器 MFT** 只处理 `XAVC` 格式，用 FFmpeg（libavcodec）多线程解码，输出 Y210 / P010 / YUY2 / NV12；
3. 普通 MP4 完全透传，仍由系统解码器处理。

已在真机验证：解码结果与 FFmpeg 逐帧比特一致（含拖动、结尾）；新版媒体播放器播放 S-I 4K24 实时 24fps；
4K60 Long GOP 在轻薄本 CPU 上自动降级（跳过 B 帧）以保持音画同步；资源管理器缩略图正常。

## 安装 / 卸载

从 [Releases](https://github.com/AsBrings/xavc-media-extension/releases) 下载 `XavcDecoder-<版本>-setup.exe`，
运行后按向导操作（会请求一次管理员权限）。安装程序没有代码签名，Windows SmartScreen 可能提示
"Windows 已保护你的电脑"，点「更多信息 → 仍要运行」即可。

卸载：设置 → 应用 → 已安装的应用 → XAVC S / S-I Decoder。

开发者也可以用 `installer/install.cmd` / `uninstall.cmd` 脚本安装（见 [`installer/README.txt`](installer/README.txt)）。

## 构建

需要 MinGW-w64 g++（路径不能含非 ASCII 字符，可用目录联接），以及用 `build/ffmpeg-minimal.sh`
（WSL / Ubuntu 中交叉编译）构建的精简 FFmpeg。参考解码器 `tools/avtest` 另需一份完整的 FFmpeg
shared 构建（例如 BtbN 的 `ffmpeg-master-latest-win64-lgpl-shared`），只用于测试：

```powershell
powershell -ExecutionPolicy Bypass -File build\build-win.ps1 -MinGW <mingw64> -FFmpeg <ffmin-xavc> [-FFmpegRef <完整 FFmpeg>]
powershell -ExecutionPolicy Bypass -File build\package.ps1 -FFmpeg <ffmin-xavc>   # 装有 Inno Setup 6 时同时生成 setup.exe
powershell -ExecutionPolicy Bypass -File tools\regress.ps1 -SI <S-I 素材> -LongGop <S 素材>
```

## 目录

```
src/                 解码器 MFT、字节流处理器、FFmpeg 加载、像素格式转换、SPS 解析、注册
installer/           安装 / 卸载脚本与发布包说明
build/               构建与打包脚本
tools/mftest/        Media Foundation 测试工具（解码比对、缩略图、媒体播放器启动等）
tools/avtest/        参考解码（纯 FFmpeg），用于逐帧比对
tools/experiments/   验证过程中用到的实验脚本
docs/                可行性分析、逆向结论与测试记录
```

## 许可

MIT（见 `LICENSE`）。FFmpeg 以独立 DLL 形式动态加载（精简构建，见 `build/ffmpeg-minimal.sh`），遵循 LGPL 2.1+。
