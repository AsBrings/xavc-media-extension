# 可行性考察与实现路径

本文记录对 Windows 11 新版「媒体播放器」与 Media Foundation（MF）的逆向分析结论、真机实验结果，
以及由此确定、**已在真机验证可行**的实现路径。

测试环境：Windows 11 Pro 10.0.26300（26H2），媒体播放器 Microsoft.ZuneMusic 11.2609.11.0，
mfplat.dll SHA-256 `149a947b…2fcfe1`，i5-8265U + UHD 620 + Radeon 520。

## 1. 结论速览

| 目标 | 结论 |
|------|------|
| 新版媒体播放器播放 XAVC S / S-I（H.264 High 4:2:2 10bit） | **已实现并验证**（libavcodec 解码，画面与 FFmpeg 逐帧一致） |
| 资源管理器缩略图 / 封面 | **已实现并验证**（缩略图代理进程 dllhost 走同一条管线） |
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

### 路径验证（最初用输出纯洋红帧的诊断版解码器，`tools/experiments/bsh-probe.ps1` 安装）

- **新版媒体播放器**（`ActivateForFile`，相当于双击）：日志显示处理器把流改名为 `XAVC`，
  我们的解码器 `SetInputType` / `SetOutputType(NV12)` 被接受并输出帧，**播放器窗口显示洋红画面**；
  进程中未加载 msmpeg2vdec.dll。
- **普通 MP4 对照**（4:2:0 8bit）：处理器透传（未改名），由 msmpeg2vdec 正常解码显示。
- **资源管理器缩略图**：dllhost 代理进程经同一路径生成 256×144 缩略图（100% 洋红）。
- **IMFSourceReader**：系统内存 / D3D11 / RGB32 视频处理三种模式均选中我们的解码器。

## 4. 解码器实现（`src/xavc_decoder.cpp`）

- **输入**：MPEG-4 源交给解码器的样本为 Annex-B（4 字节起始码，关键帧内带 SPS/PPS，每帧 8 个 slice），
  样本时间 = 显示时间（解码顺序送入，B 帧乱序）；`MF_MT_MPEG_SEQUENCE_HEADER` 也是 Annex-B。
  直接交给 libavcodec（帧级多线程），由它完成 B 帧重排。
- **输出格式**：在第一帧之前由 SPS 推出（色度格式、位深、裁剪后尺寸、VUI 色彩信息）。4:2:2 10bit 依次提供
  **Y210 → P010 → YUY2 → NV12**，渲染器取它的视频处理器支持的第一个。实测本机 Intel UHD 620 的
  D3D11 视频处理器不支持 Y210（支持 P010、YUY2），媒体播放器因此选 P010；支持 Y210 的显卡上保持完整 4:2:2。
- **DRAIN / FLUSH** 对应 avcodec 的 drain（之后 flush 以便继续接收输入）与 flush（拖动）。
- **FFmpeg 加载**：解码器第一次需要时，从本 DLL 所在目录 `LoadLibraryEx` 加载 avutil/avcodec（宿主进程
  不会搜索我们的目录）；字节流处理器完全不碰 FFmpeg。DLL 不依赖任何 MinGW 运行库（构建时检查）。
- **精简 FFmpeg**（`build/ffmpeg-minimal.sh`，WSL 交叉编译）：只含 H.264 解码器（带 x86 汇编）与 NVDEC，
  无外部库，LGPL 2.1+；DLL 名带 `-xavc` 后缀（`avcodec-xavc-63.dll`），不会与宿主进程自带的 FFmpeg 冲突。
  avcodec 从 92.7MB 降到 2.0MB，发布包 zip 约 1.2MB；解码结果与完整版 FFmpeg 逐帧一致。
- **硬件解码**：H.264 4:2:2 只有 NVDEC（NVIDIA Blackwell 及以后）能解。存在 CUDA 设备时通过
  libavcodec 的 hwaccel 尝试，失败自动回退软件解码，并在本进程内记住结果。（本机无 NVIDIA 显卡，此路径未实测。）

### 跟不上实时的处理（`IMFQualityAdvise2`）

逆向 + 实测：媒体引擎（新版媒体播放器）会 QI `IMFQualityAdvise2`，把渲染器的每个 `MEQualityNotify`
通过 `NotifyQualityEvent` 转发给解码器，**延迟量放在事件值里（VT_I8，100ns）**，自己从不调用
`SetDropMode` / `SetQualityLevel`，渲染器也不丢晚到的帧——不处理的话画面会越来越落后于声音（实测 4K60
在 i5-8265U 上 12 秒落后 5.5 秒）。因此解码器自己根据延迟调整：

| 自动级别 | 措施 |
|---|---|
| 1 | 非参考帧不做去块滤波 |
| 2 | 所有帧不做去块滤波 |
| 3 | 不解码非参考帧（索尼 Long GOP 的 B 帧） |
| 4 | 不解码任何 B 帧 |

到最高级别仍落后超过 300ms 时，在**输入端**丢弃样本直到下一个关键帧（GOP = 60 帧 = 1 秒），从关键帧
干净地恢复（不会有引用缺失的花屏）。连续准时一段时间后逐级恢复（失败则加倍等待时间）。
实测 4K60 Long GOP：约 1 秒内升到级别 4，跳一次关键帧后音画同步，13～16fps 显示（本机 CPU 上限）；
S-I 4K24 稳定 24fps，不需要降级。

## 5. 测试记录（i5-8265U，Windows 11 26300）

| 测试 | 结果 |
|---|---|
| 与纯 FFmpeg 逐帧比对（`avtest hash` vs `mftest decode`，亮度哈希） | S-I、4K60 Long GOP，Y210/P010/YUY2/NV12 全部比特一致 |
| 拖动后（Long GOP 10.5s） | 从 10.03s 关键帧开始，60 帧全部一致 |
| 结尾（drain） | 最后一帧（第 636 帧，26.48s）正确输出 |
| 新版媒体播放器 S-I 4K24 | 实时 24fps，P010 输出，画面正确 |
| 新版媒体播放器 S 4K60 Long GOP | 自动降级后音画同步，约 14fps |
| 资源管理器缩略图（`thumbforce`） | 正确画面 |
| 普通 4:2:0 MP4 | 仍由 msmpeg2vdec 解码 |
| 卸载机器部分后残留的每用户设置 | 无害：MF 跳过未注册的处理器，普通 MP4 在桌面程序和媒体播放器中照常播放 |
| 生成的测试片段（Ubuntu ffmpeg/x264）：1440×1080、720×480 全帧内、848×480、1080×1920 竖屏（4:2:2 10bit），4:2:0 10bit（High 10），8bit 4:2:2，隔行 1080i，4:4:4 10bit | 每种可协商的输出格式都与 FFmpeg 逐帧一致；竖屏与 720×480 在媒体播放器中画面正确（MF 会填充这些宽度的行距） |
| 4:4:4 以 GBR 编码（libx264rgb） | 在 SetInputType 时干净地拒绝（MF_E_INVALIDMEDIATYPE），不会黑屏 |

时间戳说明：Windows 的 MP4 解析器不应用编辑列表（edit list），与 FFmpeg 相比有固定一帧的偏移；
解码器原样输出收到的时间，这是正确行为。

回归测试脚本：`tools/regress.ps1`（四种输出格式、Long GOP、拖动、结尾，逐帧比对）。

## 6. 安装方式

- 机器部分（管理员）：文件装到 `%ProgramFiles%\XavcDecoder`，注册 COM 类与 `XAVC` 子类型的 MFT，
  「设置 → 应用」卸载项，Active Setup（本机其他用户下次登录时自动执行每用户注册）。
- 每用户部分（无需管理员）：`regsvr32 /n /i:user xavc_decoder.dll`，由 DLL 自己写入上表的 HKCU 设置并
  授予 AppContainer 读权限；原有值保存在 `HKCU\Software\XavcDecoder\Saved`，卸载时恢复。
- 升级时被占用的旧文件改名后于重启时删除；已验证安装、升级（文件被媒体播放器占用时）与卸载后状态干净。

## 7. 已知限制 / 下一步

1. 改名为 `XAVC` 后，只想"原样转封装"H.264 流（不解码）的程序看到的不再是 `H264` 子类型。
2. 在带 Y210 视频处理器的显卡、NVIDIA RTX 50（NVDEC 4:2:2）上验证（本机均不具备）。
3. Windows 10 上验证；更多素材（1080p、4:2:0 10bit、XAVC S-I HD、隔行）测试。
