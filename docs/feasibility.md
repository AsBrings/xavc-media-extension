# 可行性考察与实现路径

本文记录对 Windows 11 新版「媒体播放器」与 Media Foundation（MF）的逆向分析结论、真机实验结果，
以及由此确定、**已在真机验证可行**的实现路径。

测试环境：Windows 11 Pro 10.0.26300（26H2），媒体播放器 Microsoft.ZuneMusic 11.2609.11.0，
mfplat.dll SHA-256 `149a947b…2fcfe1`，i5-8265U + UHD 620 + Radeon 520。

## 1. 结论速览

| 目标 | 结论 |
|------|------|
| 新版媒体播放器播放 XAVC S / S-I（H.264 High 4:2:2 10bit） | **可行，已验证**（诊断解码器的输出画面出现在播放器窗口中） |
| 资源管理器缩略图 / 封面 | **可行，已验证**（缩略图代理进程 dllhost 走同一条管线） |
| 其他 MF 应用（IMFSourceReader 等） | **可行，已验证** |
| 普通 MP4（4:2:0 8bit 等） | 不受影响（我们的组件透传，仍由系统解码器解码） |
| 需要微软批准 / 改受保护的系统键 | **不需要** |

## 2. 为什么"注册一个 H.264 解码器"行不通

1. **硬编码的优先解码器表**：`mfplat.dll` 中 `IsPriorityVideoSubtype` / `IsPriorityVideoDecoder`
   查询一张写死的表（运行时读出）：`H264`、`H264_ES` → Microsoft H264 Video Decoder MFT
   （msmpeg2vdec.dll），另有 WVC1、MPEG1/2 各自的内置解码器。
2. **排序**（`SortAndFilterMFTs`，`MFT_ENUM_FLAG_SORTANDFILTER`）依次为：进程内本地注册的 MFT →
   带 Windows 签名的商店编解码包 → **上述优先表中的内置解码器** → merit 档 → Preferred → 其余。
   因此第三方无论怎么注册、写 Preferred、填 merit，H.264 都**一定先交给微软解码器**。
3. 微软解码器在 `SetInputType` 阶段**接受** profile 122（4:2:2）的输入类型，解码时才失败；
   管线不会回退到下一个解码器。
4. **自填 merit 反而有害**：`CMFTransformActivate::InstantiateTransform` 对注册了
   `MFT_CODEC_MERIT_Attribute` 的 MFT 调用 `MFGetMFTMerit`（基于证书）校验，失败即丢弃实例。
5. 打包（MSIX）编解码器：只有带 **Windows 签名**的包才能进优先档；其余走进程外 App Service
   且排在末尾。`inProcessMediaExtension` 为受限权限，第三方拿不到。

## 3. 可行的路径：在容器层给 4:2:2 流"改名"

不去和微软解码器抢 H264 子类型，而是让 4:2:2 视频流**根本不以 H264 的身份出现**：

1. **字节流处理器（BSH）** `CLSID {7F9A3C24-…}`：内部用系统的 MPEG-4 字节流处理器
   `{271C3902-…}` 创建媒体源，然后只把 profile_idc ∈ {110, 122, 244, 44} 的 H.264 视频流的
   `MF_MT_SUBTYPE` 改成私有 FourCC **`XAVC`**（`{43564158-0000-0010-8000-00AA00389B71}`）。
   媒体类型对象在源的流描述符与表示描述符副本之间共享（`Clone` 是浅拷贝），原地修改即生效；
   源对改名后的类型 `IsMediaTypeSupported` / `SetCurrentMediaType` 都返回 S_OK。
2. **解码器 MFT** `CLSID {0B1E6D2A-…}`：**只注册 `XAVC` 子类型**，不在 H.264 解码器列表里，
   普通 H.264 播放完全不受影响；**不注册 merit**。

### 让 MF 选中我们的处理器（全部是每用户设置，不碰受保护的键）

| 机制（逆向确认） | 设置 |
|---|---|
| `CacheByteStreamHandlersForKey` 依次读 **HKCU** 再读 HKLM 的 `…\Windows Media Foundation\ByteStreamHandlers\<扩展名/MIME>`（值名为 CLSID） | `HKCU\Software\Microsoft\Windows Media Foundation\ByteStreamHandlers\.mp4` 与 `\video/mp4` → 处理器 CLSID |
| `ApplyPreferredAndDontUse` 把首选处理器移到最前；首选表由 `CPluginList::Initialize` 从 **HKCR**\MediaFoundation\MediaSources\Preferred 读取。HKCR 按**值**合并 HKCU 与 HKLM | `HKCU\Software\Classes\MediaFoundation\MediaSources\Preferred` 中 `.mp4`、`video/mp4` → 处理器 CLSID（系统默认是 `{271C3902…}`；其他扩展名的首选不变） |
| `PluginAllowedByPolicy`：策略 APPROVED 下，**在 Preferred 表中**即视为已批准；WEB 策略只认写死的 25 项名单 | `HKCU\Software\Classes\MediaFoundation\Transforms\Preferred` 中 `{XAVC 子类型}` → 解码器 CLSID |
| 播放器在 AppContainer 中运行 | 给 `HKCU\Software\Microsoft\Windows Media Foundation` 加 ALL APPLICATION PACKAGES 读权限；DLL 放在 `Program Files` 下 |

另：`HKCU\…\MediaSources\UserPreferred` 虽在 mfplat 路径表中，实测未被使用。

### 真机验证（`tools/experiments/bsh-probe.ps1` 安装的诊断版，解码器输出纯洋红帧）

- **新版媒体播放器**（`ActivateForFile`，相当于双击）：日志显示处理器把流改名为 `XAVC`，
  我们的解码器 `SetInputType` / `SetOutputType(NV12)` 被接受并输出帧，**播放器窗口显示洋红画面**；
  进程中未加载 msmpeg2vdec.dll。
- **普通 MP4 对照**（4:2:0 8bit）：处理器透传（未改名），由 msmpeg2vdec 正常解码显示。
- **资源管理器缩略图**：dllhost 代理进程经同一路径生成 256×144 缩略图（100% 洋红）。
- **IMFSourceReader**：系统内存 / D3D11 / RGB32 视频处理三种模式均选中我们的解码器。

## 4. 解码核心与性能

libavcodec（FFmpeg，LGPL 动态链接）正确识别 *High 4:2:2 Intra*（S-I）与 *High 4:2:2*（S），
输出 `yuv422p10le`，画面正确。i5-8265U 纯软解：S-I 4K24 约 40 fps（可实时），
S 4K60 Long GOP 约 31 fps（达不到实时，属 CPU 上限）。

H.264 High 4:2:2 在消费级 GPU 上几乎无硬解（NVIDIA 仅 Blackwell/RTX 50 新增），
因此以多线程软解为主；硬解探测作为后续优化。

## 5. 下一步

1. **正式解码器**：在 MFT 中接入 libavcodec（帧/片多线程、B 帧重排、flush/drain/seek、
   不连续处理），输出 Y210 / P010 / NV12。
2. **处理器健壮性**：处理器位于该用户所有 MF 应用打开 .mp4 的路径上，必须绝对稳定、透明。
3. **安装程序**：机器部分（DLL、COM、MFT 注册）+ 每用户部分（上表）；卸载完全还原；
   安装后清理受影响文件的缩略图缓存。
4. 兼容性测试：Long GOP 拖动、长文件、音频、HS（HEVC）不受影响、多显示器/HDR 等。
