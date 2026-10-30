# 可行性考察与实现路径

本文记录对 Windows 11 新版「媒体播放器」及官方编解码扩展的静态分析结论，以及由此确定的实现路径。

## 1. 官方编解码扩展是怎么工作的

从微软官方渠道取得并解包分析了以下 x64 包的清单（`AppxManifest.xml`）：
`Microsoft.ZuneMusic`（新版媒体播放器本体）、`HEVCVideoExtension`、`AV1VideoExtension`、
`VP9VideoExtensions`、`MPEG2VideoExtension`、`RawImageExtension`。

所有编解码扩展用的是同一套机制：MSIX 包里声明一段 `uap4:MediaCodec` 扩展——
指定一个输入格式（codec 的 FourCC）、一组输出像素格式，并指向一个标准的
Media Foundation Transform (MFT) 解码器 DLL（带注册的 `ActivatableClassId`），
配合权限 `<wincap:Capability Name="inProcessMediaExtension" />` 让 MFT 在播放器进程内加载。

### 关键观察

- **HEVC 扩展的输出格式已含 4:2:2 / 4:4:4 10bit**（`Y210`=422/10, `Y410`=444/10, `P010`=420/10）。
  这说明新版播放器的渲染管线原生接受 4:2:2 10bit 输出。→ 我们给 H.264 做解码器时同样输出
  `Y210`，保留完整 4:2:2，不必降采样。
- **H.264 没有任何官方扩展**，系统只有内置 H.264 解码器（8bit 4:2:0）。XAVC S/S-I 的
  High 4:2:2 (profile_idc=122) 落在它能力之外。
- **`inProcessMediaExtension` 是受限权限。** `RawImageExtension` 包内含
  `inProcessMediaExtension_Microsoft.sccd`，用"包族名 + 证书指纹"把授权绑死在微软自己的包上。
  第三方自签名侧载声明该权限的包，按社区/微软反馈会被拒装。→ 不走申请的话，MSIX 扩展这条
  正规路大概率不通。

## 2. H.264 4:2:2 的硬件解码现实

H.264 High 4:2:2 Profile 在消费级 GPU 上几乎无硬解单元：NVIDIA 仅 Blackwell(RTX50) 新增；
此前各代及 Intel/AMD 的 H.264 硬解只到 8bit 4:2:0。
（对比 HEVC 4:2:2，较新 Intel 核显/N 卡有硬解，这正是 XAVC HS 能播且可硬解的原因。）

**结论**：XAVC S/S-I 在绝大多数机器上只能软解。实现上采用多线程 CPU 软解（libavcodec），
运行时探测硬解、可用时优先，但不把硬解当作前提。

## 3. 两条可能的加载/分发路径

| 路径 | 机制 | 可分发性 | 风险 |
|------|------|----------|------|
| **A. 系统 MFT 注册** | 安装程序注册 DLL 为系统级视频解码 MFT（类 LAV Filters） | 好（双击安装） | 新版播放器(AppContainer)**是否读取**未知——需实测 |
| B. MSIX MediaCodec 扩展 | 打成商店包声明扩展 | 差（需上架签名=申请，或人人手动开侧载+信任自签证书） | 受限权限拦截；且不满足"下载即用" |

用户已排除"申请"，故目标锁定路径 A。**路径 A 的生死问题**：新版播放器能否加载系统注册的
第三方解码器 MFT，必须在真机验证。

### 路径 A 的附加难点：解码器选择

H.264 存在内置解码器竞争。为 XAVC S/S-I 选到我们的解码器而非会失败的内置解码器，需要：
- 以较高 merit 注册，使拓扑解析优先尝试我们的 MFT；或
- 我们的 MFT 对非 4:2:2 输入干净拒绝、让解析回退。
此行为依赖 MF 运行时实际表现，属经验性问题，需在真机迭代。（HEVC/AV1 无此问题，因无内置竞争。）

## 4. 分阶段计划

0. **探测 + 验证**（当前）：`tools/Probe-XavcEnv.ps1` 收集硬件/系统/素材规格并确认 HS 基线；
   随后以最小解码器验证路径 A 能否被新版播放器加载。
1. **解码器实现**：H.264 High 4:2:2 10bit → Y210，多线程软解，硬解探测+回退。
2. **接入 + 缩略图**：确认播放后做资源管理器缩略图/封面（传统桌面扩展，不受上述受限权限限制）。
3. **工程化**：x64/ARM64、安装程序、兼容性测试。
