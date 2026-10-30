# XAVC Media Extension

让 **Windows 11 自带的「媒体播放器」（新版，Microsoft.ZuneMusic）** 能直接播放索尼相机拍摄的
XAVC 10bit 4:2:2 视频，并显示封面/缩略图。不依赖旧版 Windows Media Player，不依赖第三方播放器。

## 目标格式

| 格式 | 编码 | 现状 |
|------|------|------|
| XAVC HS | HEVC (H.265) Main 4:2:2 10 | **已可播放**（官方 HEVC 扩展已支持，视显卡而定可硬解） |
| XAVC S | H.264 High 4:2:2 Long GOP 10bit | 内置解码器不支持 → 本项目目标 |
| XAVC S-I | H.264 High 4:2:2 Intra 10bit | 内置解码器不支持 → 本项目目标 |

## 核心结论（见 `docs/feasibility.md`）

- 系统内置 H.264 解码器只支持 8bit 4:2:0，无法解 XAVC S / S-I 的 High 4:2:2 (profile_idc=122)。
- H.264 4:2:2 在几乎所有消费级显卡上都**无法硬解**（仅 NVIDIA Blackwell/RTX50 例外），因此
  本插件以**多线程软解为主**，并在运行时探测可用的硬件解码、可用时优先走硬解。
- 解码输出采用 **Y210 (4:2:2 10bit)**，与已经能正常播放的 HEVC 路径一致，不降色度。
- **分发方式的生死问题**：新版播放器是 AppContainer 沙箱应用，是否加载"系统级注册的第三方
  Media Foundation 解码器"尚未在真机确认。这是能否做成"下载即用"插件的前提，需实测。

## 目录

```
tools/Probe-XavcEnv.ps1   环境 + 素材探测脚本（只读，先在目标机器上跑这个）
docs/feasibility.md       可行性考察与实现路径
```

## 当前阶段

**第 0 步：探测 + 验证加载机制。** 先运行 `tools/Probe-XavcEnv.ps1` 收集硬件/系统/素材规格，
再用最小解码器验证新版播放器能否加载第三方系统解码器。结果决定后续架构。
