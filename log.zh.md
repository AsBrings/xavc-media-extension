# XAVC Media Extension 开发日志（中文翻译）

> 说明：原日志中的代码 diff、源码片段与脚本内容均已省略，只翻译叙述、状态说明与操作记录。
> 文件名、命令名、路径、注册表键保持原样。部分文件写入记录附了一句该文件的用途说明（取自原文件头部注释）。

---

## 一、辅助模块完成，开始写解码器核心

状态：辅助模块已写好（SPS 解析器、FFmpeg 加载器、像素格式转换器）。现在开始写解码器核心（MFT），随后编译并测试。

  运行了 1 条 shell 命令

**● 写入 src\xavc_decoder.cpp**（990 行，代码略）
说明：XAVC 解码器 MFT——用 libavcodec 解码 H.264 High 4:2:2 / High 10 / High 4:4:4（索尼 XAVC S、XAVC S-I 等）。输入是我们字节流处理器为这类 H.264 流打上的私有 `XAVC` 子类型（Annex-B 访问单元，样本时间 = 显示时间，SPS/PPS 在 MF_MT_MPEG_SEQUENCE_HEADER 与码流内），或是同档位的普通 `H264`；输出系统内存帧，4:2:2 源优先提供 Y210（10bit）/ YUY2（8bit），渲染器接受时即保留色度分辨率，P010 / NV12 作为回退；`HKLM\SOFTWARE\XavcDecoder\OutputFormat` 可强制 4:2:0。解码采用帧级多线程软解；若 GPU 解码器能处理该码流则……

**● 写入 src\xavc_dll.cpp**（176 行，代码略）
说明：DLL 入口点——类工厂与（反）注册。xavc_decoder.dll 里有两个 COM 类：`CLSID_XavcH264Decoder`（解码器 MFT，只为私有 `XAVC` 子类型注册，不设 merit）、`CLSID_XavcByteStreamHandler`（把 4:2:2 / 10bit H.264 改标记的 MP4 字节流处理器）。regsvr32 注册这两个 COM 类和 MFT（全机范围，需要管理员）；要让 Media Foundation 真正使用它们，则是安装程序写入的每用户设置。

**● 写入 src\xavc.def**（新增 1 行，删除 2 行）

**● Bash**（用 Python 脚本批量改写 src\xavc_bsh.cpp 的日志调用，输出略）

更新 src\xavc_bsh.cpp（+9 −8）

  运行了 1 条 shell 命令

**● 更新 build\build-win.ps1**（新增 30 行，删除 6 行）

**● 更新 build\build-win.ps1**（新增 6 行，删除 8 行）

状态：解码器代码已写完，现在第一次编译。

  运行了 1 条 shell 命令

编译通过、无警告。现在加上逐帧精确校验：让参考解码器和 MF 测试工具都按帧打印亮度哈希，与纯 FFmpeg 的结果逐一比对。

**● 更新 tools\avtest\avtest.c**（新增 68 行，删除 1 行）

**● 更新 tools\mftest\mftest.cpp**（新增 165 行）

**● Bash**（Python 脚本补 mftest 的头文件与用法说明）

**● 更新 tools\mftest\mftest.cpp**（新增 2 行）

**● Bash**（Python 脚本，输出略）

**● 更新 tools\experiments\bsh-probe.ps1**（新增 6 行）

**● 更新 tools\experiments\bsh-probe.ps1**（新增 1 行）

**● 更新 tools\experiments\bsh-probe.ps1**（新增 1 行）

  运行了 2 条 shell 命令

---

## 二、去掉 libwinpthread 依赖

状态：第一次构建能跑，但 DLL 因为一个线程局部变量而带上了 libwinpthread-1.dll 依赖。宿主程序找不到这个 DLL，会导致在媒体播放器里加载失败，所以要把它去掉。

**● Bash**（用 Python 脚本把 `XavcAvErr()` 改为返回结构体 `XavcErrText`）

更新 src\xavc_decoder.cpp（+3 −3）

更新 src\xavc_ffmpeg.cpp（+5 −5）

更新 src\xavc_ffmpeg.h（+4 −2）

**● 更新 build\build-win.ps1**（新增 9 行）

  运行了 2 条 shell 命令

**● 更新 src\xavc_dll.cpp**（新增 14 行）

  运行了 1 条 shell 命令

现在导入表干净了（只有系统 DLL）。安装构建产物，跑第一次真实解码测试。

  运行了 2 条 shell 命令

那段裁剪出来的 3 秒 S-I 片段本身是坏的：FFmpeg 自己的解复用器在它上面也报 NAL 尺寸错误，所以是测试文件的问题，而不是解码器的 bug。改用两个原始相机文件。

  运行了 1 条 shell 命令

S-I 解码比特级一致：全部 24 帧与纯 FFmpeg 的哈希一一相同，时间戳也正确。接着测 Long GOP 文件（B 帧重排）和其他输出格式。

  运行了 1 条 shell 命令

Long GOP 也逐帧吻合，唯一的差异是一个恒定的一帧时间戳偏移——原因是 Windows 的 MP4 解析器忽略了 FFmpeg 的编辑列表（edit list），解码器本身的行为是正确的。现在来看实际画面。

  读取了 2 个文件，运行了 1 条 shell 命令

两张画面都正确解码、颜色自然。由于资源管理器的缩略图缓存里可能还留着诊断版本生成的洋红图，给测试工具加一个强制重新提取的模式。

**● 更新 tools\mftest\mftest.cpp**（新增 27 行，删除 8 行）

**● Bash**（Python 脚本补用法说明）

  搜索了 1 个模式，读取了 1 个文件，运行了 2 条 shell 命令

**● 更新 tools\mftest\mftest.cpp**（新增 2 行，删除 1 行）

  搜索了 1 个模式

状态：真实解码已与 FFmpeg 逐帧验证一致。现在给测试工具加上强制重新提取缩略图，然后测试资源管理器缩略图和新版媒体播放器。

**● 更新 tools\mftest\mftest.cpp**（新增 1 行）

**● 更新 tools\mftest\mftest.cpp**（新增 2 行）

**● 更新 tools\mftest\mftest.cpp**（新增 1 行，删除 1 行）

  读取了 1 个文件，运行了 2 条 shell 命令

缩略图现在显示的是真实画面（苍鹭，不再是洋红）。接下来是新版媒体播放器本身。

  搜索了 1 个模式，读取了 1 个文件，运行了 2 条 shell 命令

**● 更新 src\xavc_decoder.cpp**（新增 15 行，删除 4 行）

**● Bash**（Python 脚本，输出略）

更新 src\xavc_decoder.cpp（+4 −1）

**● 写入 tools\experiments\mediaplayer-test.ps1**（61 行）
说明：在新版媒体播放器里打开文件（等同双击）、让它播放、截取播放器窗口、打印该播放器进程的解码器日志，然后关闭播放器（仅当脚本自己启动的）。

  运行了 1 条 shell 命令

状态：缩略图已确认可用。现在带播放速度日志重新构建，并在新版媒体播放器里测试。

  运行了 1 条 shell 命令

S-I 片段在新版媒体播放器里以 CPU 解码流畅播放 24 fps，但它协商到的是 P010 而不是 Y210——因为这块 Intel GPU 的渲染器不直接支持 Y210。看一下截图……（原文在此处截断）

  读取了 1 个文件，运行了 2 条 shell 命令

**● 更新 tools\experiments\mediaplayer-test.ps1**（新增 18 行，删除 1 行）

**● 更新 tools\experiments\mediaplayer-test.ps1**（新增 3 行，删除 2 行）

  运行了 1 条 shell 命令

S-I 4K24 能稳定解码，但 Long GOP 4K60 把 CPU 压垮，退化成只播关键帧。所以要实现 IMFQualityAdvise，让播放器能平滑降质。先做一些清理，并查清窗口截图为什么失败。

  读取了 1 个文件，运行了 4 条 shell 命令

**● Bash**（Python 脚本修掉脚本里中英混排的标题匹配）

更新 tools\experiments\mediaplayer-test.ps1（+1 −1）

状态：修好了截图脚本（窗口标题的编码问题）。现在加质量控制接口，让 4K60 在慢 CPU 上平滑降质，而不是退化成只播关键帧。

  运行了 3 条 shell 命令

**● 更新 src\xavc_common.h**（新增 20 行）
说明：补上 mingw-w64 里缺失的 IMFQualityAdvise / IMFQualityAdvise2 接口定义（管线里的质量管理器用它们请求解码器在播放落后时少干点活）。

**● 更新 src\xavc_guids.h**（新增 8 行）
说明：补 IID_IMFQualityAdvise / IID_IMFQualityAdvise2 等 GUID（值对照 msmpeg2vdec.dll / mf.dll 校验过）。

  读取了 1 个文件

**● 更新 src\xavc_decoder.cpp**（新增 4 行，删除 2 行）

**● 更新 src\xavc_decoder.cpp**（新增 12 行，删除 4 行）

状态：给解码器加质量控制方法（播放器在落后时可以要求它跳过去块滤波或 B 帧）。

**● 更新 src\xavc_decoder.cpp**（新增 67 行）

**● Bash**（Python 脚本，输出略）

  运行了 2 条 shell 命令

问题找到了：质量管理器根本没到达解码器，因为它通过 IMFGetService 去查 MF_QUALITY_SERVICES，而这条路径没暴露；于是播放只有大约 0.5 倍速，还带追帧 seek。先看截图，然后实现 IMFGetService 支持。

  读取了 1 个文件

媒体播放器已经在显示真实视频了。现在通过 IMFGetService 暴露质量接口：

  运行了 2 条 shell 命令

**● Bash**（Python 脚本，输出略）

  运行了 2 条 shell 命令

新版媒体播放器的质量管理器通过 IMFQualityAdvise2 连上了解码器，每秒发来约 36 个 MEQualityNotify 事件（类型 311），把速度与质量的权衡完全交给解码器，依据是帧的迟到程度。现在来看事件里都有什么属性。

  运行了 1 条 shell 命令

确认了微软自家的 H.264 解码器会对 MF_QUALITY_NOTIFY_SAMPLE_LAG 事件作出反应，于是实现类似的自适应行为：帧迟到时逐级降级（先跳过去块滤波，再跳过非参考 B 帧），落后太多就直接跳帧追上去，追上后再恢复。

**● 更新 src\xavc_guids.h**（新增 5 行）
说明：补 MF_QUALITY_NOTIFY_SAMPLE_LAG 属性 GUID（渲染器发的 MEQualityNotify 事件里，记录样本迟到多少，单位 100ns）。

**● Bash**（Python 脚本，输出略）

**● Bash**（Python 脚本，输出略）

  运行了 1 条 shell 命令

完全没有迟到（lag）的 trace，说明属性读取失败了。把事件里实际带了什么打出来看看：

**● 更新 src\xavc_decoder.cpp**（新增 21 行）

**● Bash**（Python 脚本，输出略）

  运行了 1 条 shell 命令

找到了：媒体播放器的渲染器把迟到量放在事件值里（VT_I8），它唯一的一个属性装的是样本时间戳。先确认这个值的行为确实像迟到量：

**● Bash**（Python 脚本，输出略）

  运行了 1 条 shell 命令

确认了 lag 在持续增长（12 秒内从 0.6 秒涨到 5.5 秒），因为媒体播放器把每个迟到的帧都照常渲染、不丢弃，所以解码器必须自己处理。现在把代码改成读事件值。

**● 更新 src\xavc_decoder.cpp**（新增 12 行，删除 3 行）

  运行了 1 条 shell 命令

跳帧逻辑反而更糟：lag 从 0.4 秒涨到 1.6 秒，因为跳过的这段窗口比它跳过的媒体还长；只有禁用 B 帧后才终于追得上——这说明这台笔记本解 I/P 帧就已经比实时慢了。下一步看 GOP 长度。

  运行了 1 条 shell 命令

重新设计追帧逻辑：在输入端丢弃压缩样本，直到目标时间之后的那个关键帧，再从那干净地恢复——因为跳过的帧没人引用，不会出现花屏。

  搜索了 1 个模式

**● Bash**（Python 脚本，输出略）

  运行了 1 条 shell 命令

现在行了：升到 4 级后靠一次关键帧跳跃就追上了，之后画面保持同步，之前的漂移和落后于声音都没了。为避免弱 CPU 上频繁顿挫，给降级加指数退避，然后测试 S-I 帧和拖动。

**● Bash**（Python 脚本，输出略）

**● Bash**（Python 脚本，输出略）

  运行了 2 条 shell 命令

Python 那次编辑没生效（转义问题），所以那一轮其实没有真的拖动。改用编辑器直接改：

**● 更新 tools\mftest\mftest.cpp**（新增 1 行，删除 1 行）

**● 更新 tools\mftest\mftest.cpp**（新增 8 行）

**● 更新 tools\mftest\mftest.cpp**（新增 2 行，删除 1 行）

**● 更新 tools\mftest\mftest.cpp**（新增 1 行，删除 1 行）

  运行了 1 条 shell 命令

拖到 10.5 秒后，解码从 10.03 秒的关键帧重新开始，60 帧全部与参考逐位一致。接着测码流结束（drain）：最后一帧也必须出来。

  运行了 1 条 shell 命令

码流结束也正常：最后一帧（#635，26.48 秒）出来了，尾部 37 帧全部吻合。现在带自适应逻辑在媒体播放器里测 S-I，再用普通 4:2:0 的 MP4 做对照：

  运行了 1 条 shell 命令

S-I 稳定在 24 fps，级别 0（不需要降级）。用普通 4:2:0 8bit MP4 做对照测试：

  运行了 1 条 shell 命令

对照文件走的是微软自带解码器（msmpeg2vdec），我们的解码器没有参与。截图：

  读取了 2 个文件

S-I 和对照组在新版媒体播放器里都渲染正确。现在给每次输出类型协商加 trace，看媒体播放器到底是先试了 Y210 才落到 P010，还是压根没问过。

**● 更新 src\xavc_decoder.cpp**（新增 1 行）

  运行了 1 条 shell 命令

媒体播放器直接跳过了 Y210、设置 P010，看来是按渲染器能力在挑格式。查一下这台机器的各块 GPU 的 D3D11 视频处理器到底支不支持 Y210 输入。

**● 更新 tools\mftest\mftest.cpp**（新增 50 行）

**● Bash**（Python 脚本给 mftest 加 vpcaps 命令，输出略）

  运行了 1 条 shell 命令

确认了：这台笔记本的 Intel UHD 620 不支持 Y210（只支持 P010 和 YUY2），所以媒体播放器在这里回退到 P010；换到支持 Y210 的新显卡上，同一个构建就能给出完整 4:2:2。再给既没有 Y210 也没有 P010 的渲染器补一条 YUY2 回退。

**● 更新 src\xavc_decoder.cpp**（新增 3 行）

  搜索了 1 个模式

**● 更新 src\xavc_decoder.cpp**（新增 7 行，删除 4 行）

  运行了 1 条 shell 命令

每种格式各一处不一致。看看是哪一帧：

  运行了 1 条 shell 命令

那个"不一致"是我 PowerShell 校验脚本的 bug（时间戳键为 0 时被当成假值）。哈希本身是吻合的，所以四种输出格式在全部 30 帧上都比特级一致。提交这个里程碑：

  运行了 3 条 shell 命令

已推送（794ed61）。在多智能体审查开始前，我重读一遍解码器现在的样子——它已经经历过很多次脚本化修改了：

  读取了 1 个文件

代码看起来是自洽的。现在把对抗式审查作为一个多智能体工作流跑起来（4 个审查者各自配一个挑剔的验证者，共 8 个智能体），同时并行做安装程序。

**● Workflow**（对 XAVC 解码器 MFT、FFmpeg 集成、字节流处理器与转换器的对抗式审查）
用 `/workflows` 可以查看动态工作流的运行情况

审查跑着的时候，我先测一件事：如果去掉全机范围的处理器注册、但每用户设置仍指向我们，普通 .mp4 播放是否还能通过 Media Foundation 的回退正常工作。

  运行了 1 条 shell 命令

在桌面进程里 Media Foundation 回退得很干净：即使每用户条目还在，普通 MP4 仍能打开并解码。再到媒体播放器里确认一遍，然后重新注册：

  读取了 1 个文件，运行了 1 条 shell 命令

确认了即使残留每用户条目，媒体播放器也能干净回退，所以 .mp4 播放是安全的。现在转向安装程序：把每用户注册挪进 DLL 本身，通过 `regsvr32 /n /i:user` 完成，它会保存并恢复原值，并且只在该权限缺失时才补上 AppContainer 读权限。

**● 写入 src\xavc_userreg.cpp**（219 行）
说明：每用户注册（`regsvr32 /n /i:user xavc_decoder.dll`，无需管理员权限）。全机部分（DllRegisterServer：COM 类 + `XAVC` 子类型的 MFT）只让组件"可用"；Media Foundation 是否使用它们由每用户设置决定，mfplat.dll 会先读 HKCU 再读 HKLM。

**● 更新 src\xavc_dll.cpp**（新增 8 行）

**● 更新 src\xavc_common.h**（新增 2 行）

**● Bash**（用 printf 重写 src\xavc.def 并更新构建脚本）

**● 更新 src\xavc_userreg.cpp**（新增 6 行）

**● 更新 src\xavc_userreg.cpp**（删除 4 行）

  运行了 2 条 shell 命令

值被正确删除并恢复了。测试里删除 ACE 失败（PowerShell 的名称转换问题），所以按 SID 重做那一步，顺便看看 .mp4 键里还剩什么：

  运行了 1 条 shell 命令

权限授予是有效的：Media Foundation 的键拿到了显式读权限，已有的子键会继承。

  运行了 1 条 shell 命令

DLL 方式注册可用；S-I 因为启动时的迟到被自动降到了 2 级，所以要加一个预热期来忽略最初的迟到报告。同时开始做安装程序，先看有哪些许可证文件可用。

  列出了 1 个目录，运行了 1 条 shell 命令

许可情况已确认：项目本身 MIT，FFmpeg 是 LGPLv3 并附带其许可证文本。现在写双语安装程序，保存为带 BOM 的 UTF-8，好让 PowerShell 5.1 正确显示中文。

**● 写入 installer\Install-XavcDecoder.ps1**（新增 146 行，删除 50 行）
说明：安装 XAVC S / S-I 解码插件，双击 install.cmd 即可；Windows 会请求一次管理员权限（装到 Program Files 并注册组件），然后为当前用户启用（不需要管理员权限）。支持 `-LogLevel`、`-NoAllUsers`、`-ResetThumbnails`、`-NoPause`、`-Machine` 等参数。

**● Bash**（检查文件 BOM 头）

**● 更新 installer\Install-XavcDecoder.ps1**（新增 165 行，删除 162 行）

**● 写入 installer\Uninstall-XavcDecoder.ps1**（新增 85 行，删除 21 行）
说明：卸载 XAVC S / S-I 解码插件——先撤销当前用户的设置（恢复安装前的值），再以管理员身份删除程序文件与组件注册。

**● Bash**（生成 install.cmd / uninstall.cmd）

**● 写入 installer\README.txt**（52 行，原文即中文）

**● 写入 build\package.ps1**（39 行）
说明：从 build\out 组装可发布的文件夹与 zip。

**● 写入 installer\THIRD_PARTY_NOTICES.txt**（14 行）

代码审查还在后台跑，这期间把安装、卸载和打包脚本都写完了。接下来应用审查的修复，并测试完整的安装/卸载流程。

  运行了 2 条 shell 命令

审查者还在处理上下文，为了保持行号引用有效，先不动 src/。这段时间先更新文档，从 README 开始。

**● 写入 README.md**（新增 44 行，删除 20 行）

**● Bash**（Python 脚本更新 docs/feasibility.md）

审查还在跑，顺便看看做一个精简版 FFmpeg 构建需要什么（能把发布包显著缩小）：

  运行了 1 条 shell 命令

WSL 里的 Ubuntu 已经装好了 MinGW 交叉编译器、并且 sudo 免密码，WinLibs 提供 nasm，所以精简 FFmpeg 构建放到审查之后跑。现在先把真正的安装程序端到端测一遍，第一步是清掉实验性的探针安装。

  运行了 1 条 shell 命令

打包出来是 38.8 MB（zip）。像用户那样（通过 install.cmd）跑一遍安装，然后检查结果状态：

  运行了 1 条 shell 命令

安装程序可用（中文提示显示正常）。在这个真实安装上检查播放和缩略图：

  运行了 1 条 shell 命令

真实安装下播放和缩略图都正常。现在按"设置 → 应用"的方式测卸载程序，这也会走到"文件被占用"的路径，因为缩略图宿主进程还持有这个 DLL。

  运行了 1 条 shell 命令

卸载很干净：文件、全机注册、COM/MFT 条目、每用户值和设置全都没了。重新安装，然后在媒体播放器占用 DLL 的情况下测试原地升级：

  运行了 1 条 shell 命令

升级逻辑能工作，但发现一个 bug：PowerShell 在 .NET 调用里会把 `$null` 转成空字符串，导致 MoveFileEx 静默失败、没能登记"重启后删除"。用 `[NullString]::Value` 修掉两个脚本里的这个问题，并清理残留的 .old 文件。

**● Bash**（Python 脚本在 installer 目录里批量修正）

更新 installer\Install-XavcDecoder.ps1（+1 −1）

更新 installer\Uninstall-XavcDecoder.ps1（+91 −91）

  运行了 1 条 shell 命令

用 `[NullString]::Value` 之后，"重启后删除"的登记就正确了（用一个无害的临时文件排队删除做了验证）。看看审查进展：

  运行了 2 条 shell 命令

四个审查者都还在干活——其中一个甚至在反汇编 mfplat 来验证行为。等他们的同时，在 WSL 里启动精简 FFmpeg 构建（只要 H.264/NVDEC，命名加 `-xavc` 后缀），先准备构建工具。

  运行了 1 条 shell 命令

引号在 PowerShell、WSL 和 sh 之间被吞掉了。交叉编译器用的是 win32 线程模型，正好是我们想要的（不会引入 libwinpthread）。改用脚本文件：

  运行了 2 条 shell 命令

源码就位：FFmpeg 处在完全相同的 commit（4811625b5f），nv-codec-headers 是最新的，nasm 2.16。现在只配置并构建 H.264 解码器（带汇编优化）加 NVDEC：

  运行了 2 条 shell 命令

精简 FFmpeg 构建成功：avcodec 从 92.7MB 降到 2.0MB，许可纯 LGPL，DLL 名字唯一，不会和宿主自带的 FFmpeg 冲突。等这轮审查结束就更新加载器去用这些新 DLL 名，现在先把构建脚本整理出来。

  读取了 1 个文件，运行了 1 条 shell 命令

又是 Python 转义的问题。直接用编辑器改：

  读取了 1 个文件

**● 更新 build\build-win.ps1**（新增 5 行，删除 2 行）

**● 更新 build\build-win.ps1**（新增 4 行）

**● 更新 build\build-win.ps1**（新增 4 行，删除 3 行）

**● 更新 build\build-win.ps1**（新增 11 行，删除 6 行）

为精简构建更新打包脚本、安装程序的文件过滤和第三方声明，并把 FFmpeg 构建配方写进仓库（便于复现，也满足 LGPL 要求）：

**● 更新 build\package.ps1**（新增 12 行，删除 2 行）

**● 创建 build\ffmpeg-minimal.sh**（50 行）
说明：在 WSL / Ubuntu 24.04 里构建解码器随附的 FFmpeg——libavutil + libavcodec，只开 H.264 解码器（x86 汇编）与 NVDEC hwaccel，共享 DLL 命名为 `avcodec-xavc-NN.dll` / `avutil-xavc-NN.dll`，不与同进程内其他 FFmpeg 冲突，无外部库，LGPL 2.1+。

**● 更新 installer\THIRD_PARTY_NOTICES.txt**（新增 13 行，删除 9 行）

**● Bash**（Python 脚本，有两条 SyntaxWarning，输出略）

**● Bash**（sed 把文档里的 "LGPL v3" 统一改成 "LGPL 2.1+"）

全代码库审查还在跑，先做这个小的加载器改动（带后缀的 DLL 名），然后测精简 FFmpeg 构建的速度与正确性。

**● 更新 src\xavc_ffmpeg.cpp**（新增 9 行，删除 2 行）

  运行了 1 条 shell 命令

对着精简 FFmpeg 编译干净通过。清掉 build\out 里残留的完整版 FFmpeg DLL，然后用真正的安装程序打包并安装：

  运行了 1 条 shell 命令

✻ 已达 5 小时使用上限 · 17:30（Asia/Shanghai）重置
用 `/upgrade` 或 `/usage-credits` 可以继续手上的工作。

✻ 用时 1 小时 2 分 36 秒 · 13:35 完成

**● 动态工作流"对 XAVC 解码器 MFT、FFmpeg 集成、字节流处理器与转换器的对抗式审查"完成 · 用时 20 分 20 秒**

❯ 继续

---

## 三、继续：修复审查发现的问题、补回归脚本

在账号用量上限之前，只有转换器/解析器这一路审查者跑完了，所以我先自己核实并修复它发现的问题，剩下的领域直接内联审一遍，从读完整结果开始。

  运行了 2 条 shell 命令

最关键的 bug：Media Foundation 给缓冲区行距做的对齐填充（例如 1440/720 宽，或竖屏 1080x1920）被错误地判为"缓冲区太小"而拒绝，导致这些视频根本播不了——测试素材都是 3840/1920 宽，所以一直没暴露。再加上四个较小的问题（隔行色度场混合、GBR 4:4:4 静默输出黑屏、左侧裁剪的取整、malloc 导致的半帧失败）以及我自己发现的三个问题，先从转换器着手：改成预分配的临时行、正确的成功/失败返回值、以及感知场的色度处理。

  读取了 1 个文件，运行了 1 条 shell 命令

那段长内联脚本被 shell 弄坏了。先把补丁脚本写成文件：

  做了 1 次 scratchpad 编辑（+149 行）

**● Bash**（运行 patch_convert.py）——转换器已打补丁

更新 src\xavc_convert.cpp（+56 −22）

更新 src\xavc_convert.h（+6 −1）

转换器补好了。现在到解码器这边，先看当前的 DeliverFrame：

  做了 1 次 scratchpad 编辑（+258 行），搜索了 1 个模式，读取了 1 个文件

**● Bash**（运行 patch_decoder.py）——解码器、FFmpeg 函数表和字节流处理器已打补丁

更新 src\xavc_bsh.cpp（+10 −4）

更新 src\xavc_decoder.cpp（+56 −13）

更新 src\xavc_ffmpeg.cpp（+1 −0）

更新 src\xavc_ffmpeg.h（+1 −0）

往仓库里加一个可复用的回归脚本：通过已安装的插件解码，把每一帧的哈希与 FFmpeg 参考比对，覆盖四种输出格式、文件中途拖动以及文件结尾。

**● 写入 tools\regress.ps1**（93 行）
说明：对已安装解码器做逐帧精确回归测试——通过 Media Foundation（mftest decode：Source Reader + 已安装的 XAVC 解码器）解码，与纯 libavcodec（build\out\ref\avtest.exe）逐帧比对亮度哈希与时间。

  运行了 1 条 shell 命令

已构建、打包（zip 1.2 MB）、安装。对着已装插件跑回归套件（解码器基于精简 FFmpeg 构建，参考端是完整 FFmpeg）：

  运行了 1 条 shell 命令

S-I 的五项检查全部比特级通过，但 Long GOP 那一步失败了——原因是 PowerShell 5.1 把参考工具无害的 stderr 警告当成了致命错误。改脚本正确处理。

**● 更新 tools\regress.ps1**（新增 3 行）

  运行了 1 条 shell 命令

7 项检查全过，包括 Long GOP 的 B 帧重排和拖动。现在用这个新构建测媒体播放器，先 S-I 再 4K60：

  运行了 1 条 shell 命令

两个用例在精简 FFmpeg 构建下都正常：S-I 靠预热期的修复稳定在 24 fps，4K60 Long GOP 稳定在约 16 fps 并平滑地跳关键帧。不过安装目录里还留着上一版本完整 FFmpeg 的 DLL，所以给安装程序加上升级时清除它们的逻辑。

**● 更新 installer\Install-XavcDecoder.ps1**（新增 7 行）

  运行了 1 条 shell 命令

陈旧的 DLL 清掉了，安装目录很干净。检查脚本是否保住了 BOM，并再读一遍重写后的 DeliverFrame：

  读取了 1 个文件，运行了 1 条 shell 命令

改写没问题。更新文档（精简 FFmpeg 构建已完成，构建说明也变了），然后提交：

  做了 1 次 scratchpad 编辑（+66 行）

**● Bash**（运行 patch_docs.py 并提交）

更新 README.md（+6 −4）

更新 docs\feasibility.md（+17 −5）

**● Bash**（更新 .gitignore 并提交）

更新 .gitignore（新增 1 行）

改动已提交并推送（9d2f779）。我注意到 Windows 上的 Git 可能用 CRLF 检出 ffmpeg-minimal.sh 从而破坏 bash 脚本，所以在回退这台机器的安装设置、注销之前，先把它锁成 LF。

**● Bash**（新增 .gitattributes 固定 `*.sh` 为 LF 并提交）

6f3dc8e 让 shell 脚本保持 LF 行尾
创建 .gitattributes（新增 2 行）

  运行了 1 条 shell 命令

一切都已按最终形态安装好（日志关闭）。把安装包和媒体播放器播放 S-I 素材的截图发给你：

● 新版「媒体播放器」正在用插件播放 XAVC S-I 4K24 素材（实时 24fps，画面与 FFmpeg 逐帧一致）
  › [image] build\out\tmp\mp_c54_min.png（1.7MB）

● 安装包 XavcDecoder-0.1.0（1.2MB）：解压后双击 install.cmd，卸载用 uninstall.cmd 或「设置 → 应用」
  › [file] build\package\XavcDecoder-0.1.0.zip（1.2MB）

---

## 四、最终总结

XAVC S 和 XAVC S-I 文件现在能在新版 Windows 媒体播放器里播放，资源管理器也能显示真实缩略图。普通 MP4 文件仍然走 Windows 自带的解码器，不受影响。安装包（1.2 MB）和媒体播放器播放你 S-I 文件的截图都已发出。

这台笔记本上的测试结果：

| 测试 | 结果 |
|---|---|
| 解码帧与 FFmpeg 对比 | 逐帧完全一致；四种输出格式、拖动之后、文件结尾均是 |
| 媒体播放器里的 S-I 4K24 | 稳定实时 24 fps |
| 媒体播放器里的 S 4K60 Long GOP | 约 16 fps，音画保持同步 |
| 资源管理器缩略图 | 真实画面 |
| 普通 MP4 | 仍由 Windows 自带解码器播放 |

关于 4K60 文件：这颗 i5-8265U 满画质只能解到约 30 fps。媒体播放器自己不会丢弃迟到的帧，所以画面越来越落后于声音（12 秒后落后 5.5 秒）。现在解码器自己处理这件事：逐步降质、跳过 B 帧，如果还落后就直接跳到下一个关键帧；等 CPU 跟得上了再逐步恢复。更快的台式机 CPU 应该能满帧播放 4K60。

你之前的两个问题：
- 4:2:2 不会被插件降采样。它会优先提供 4:2:2 输出（Y210）。这台笔记本的 Intel UHD 620 无法渲染该格式，所以媒体播放器改选 10bit 4:2:0（P010）。在支持 Y210 的显卡上会保留完整 4:2:2。
- 硬件解码是自动探测的。只有 NVIDIA RTX 50 系列能硬解 H.264 4:2:2，其他显卡都走 CPU。这台笔记本没有 NVIDIA 显卡，所以硬解路径没有实测。

安装包：
- 解压后双击 install.cmd，会请求一次管理员权限。
- 它还会在本机其他用户账户下次登录时为他们启用插件。
- 用 uninstall.cmd 或「设置 → 应用」卸载。我测过安装、在媒体播放器占用文件时升级、以及卸载；卸载后不残留任何东西。

代码审查：多智能体审查撞上了账号的 5 小时用量上限，4 个审查者只有 1 个跑完。它发现 5 个问题，现在都已修复。最严重的一个：Windows 在内存里做过填充的视频宽度（1440、720、竖屏 1080×1920）原本根本播不了；你的 3840 宽素材永远碰不到这个。另外三个领域我自己检查了，并修复了 MP4 处理器里的一处启动竞态。这些领域还没有做过完整的独立审查。

尚未覆盖：
- 那些把 H.264 码流原样拷贝进新文件而不解码的程序，可能会拒绝这些文件，因为插件改了流的标签。
- 没有在 Windows 10、支持 Y210 的显卡、以及 RTX 50 上测试过。

这台电脑的状态：最终版本已安装，日志已关闭。如果任何 XAVC 文件仍显示洋红缩略图（测试版本残留），运行 `install.cmd -ResetThumbnails`，它会重启资源管理器。所有改动都已提交并推送（9d2f779、6f3dc8e）。

最有价值的下一步，是把它装到另一台电脑上——最好是有更新显卡或 NVIDIA RTX 50 的机器——看看 XAVC 文件在那里能不能播。

✻ 用时 5 小时 46 分 9 秒 · 18:18 完成

※ 回顾：XAVC S / S-I 插件现已在新版 Windows 媒体播放器和资源管理器缩略图中可用。已构建、测试并发送了 1.2 MB 的安装包。下一步是把它装到另一台电脑上，最好是有更新显卡的机器。（可在 `/config` 中关闭回顾）
