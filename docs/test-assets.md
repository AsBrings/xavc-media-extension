# 测试素材

用于开发/验证解码器的真实索尼样片来源（Imaging Resource，Sony A7S III 原始机内文件）。
页面：https://old.imaging-resource.com/PRODS/sony-a7s-iii/sony-a7s-iiiVIDEO.HTM

直链目录前缀：`https://old.imaging-resource.com/PRODS/sony-a7s-iii/FULLRES/`

| 文件 | 格式 | 色度/位深 | 分辨率 | 帧率 | 码率 | 大小 |
|------|------|-----------|--------|------|------|------|
| `yvid-a7siii-C0054-xavcsi-alli-42210bit-4k24p240Mbps.MP4` | **XAVC S-I** (All-Intra) | 4:2:2 10bit | 3840×2160 | 24p | 240M | 772 MB |
| `yvid-a7siii-C0100-xavcsi-alli-42210bit-4k30p300Mbps.MP4` | XAVC S-I | 4:2:2 10bit | 4K | 30p | 300M | 2.32 GB |
| `yvid-a7siii-C0099-xavcsi-alli-42210bit-4k60p600Mbps.MP4` | XAVC S-I | 4:2:2 10bit | 4K | 60p | 600M | 3.86 GB |
| `yvid-a7siii-C0069-xavcs-42210bit-4k60p200Mbps.MP4` | **XAVC S** (Long GOP) | 4:2:2 10bit | 4K | 60p | 200M | 1.48 GB |
| `yvid-a7siii-C0070-xavcs-4208bit-4k60p150Mbps.MP4` | XAVC S | 4:2:0 8bit（内置可解，对照组） | 4K | 60p | 150M | 1.48 GB |

## 已核对的流特征（C0054，用 `tools/` 的 mp4 解析确认）

- H.264 `avcC`：configurationVersion=1，**profile_idc=122（High 4:2:2）**，level_idc=51（5.1）
- SPS：**chroma_format_idc=2（4:2:2）**，bit_depth_luma=bit_depth_chroma=**10**
- 3840×2160，23.976 fps，636 帧，26.5s，`moov` 在文件尾部
- 这正是 Windows 内置 H.264 解码器无法处理的 profile（内置仅 8bit 4:2:0 = profile ≤ High/100）

## 短测试片段

从 C0054 无损截取前 72 帧（3.0s，约 73MB）作为快速测试片，命名约定：
`xavc-si_4k24p_422_10bit_3s.MP4`。因 S-I 为全帧内、无 B 帧重排，按样本前缀截取不改变画面。
（Long GOP 的 XAVC S 不可简单截取，需保留 ctts 帧序信息——测试时请下载完整文件。）

> 页面上 C0069 的链接写成了双后缀 `.MP4.MP4`（笔误），正确地址是单个 `.MP4`。

> 注意：样片仅用于本插件的解码测试，版权归原拍摄者 / Imaging Resource。
