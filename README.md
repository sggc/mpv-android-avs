# mpv-android-avs

给 [mpv-android/mpv-android](https://github.com/mpv-android/mpv-android) 的
**构建流程**加中国自主音视频编解码支持，产出带 AVS2/AVS3(AVS3-P2) 视频与
AV3A(Audio Vivid) 音频的 `libmpv.so` / `libavcodec.so` 等一套 `.so`。

本仓库**不放播放器源码**，只放：

| 目录 | 内容 |
|---|---|
| `.github/workflows/` | 构建工作流（Android / 其他平台 / Windows） |
| `patches/` | 应用在上游依赖上的补丁，**每处都写了为什么** |
| `src/` | `av3a_dec.c` —— FFmpeg 的 AV3A 解码器 wrapper 源码 |
| `probes/` | 真机能力探针，用来验证"到底能不能解" |
| `scripts/` | 补丁应用、位深自检、native 库提取 |
| `logs/` | 历史构建日志（含失败与成功案例，作对照） |

> 迁移自 `sggc/IPTV-Scanner-Editor-Pro`：原仓库是 IPTV 应用本身，
> mpv 构建只是它的附属。拆出来后补丁才有独立的版本历史，
> 追踪上游变动也更清楚。

---

## 支持矩阵

| 编解码 | 容器/承载 | 状态 | 说明 |
|---|---|---|---|
| CAVS | 全部 | ✅ | 上游 `llawsxx/FFmpeg` 的 6 个 patch |
| AVS2 (8bit) | TS / MKV | ✅ | `libdavs2` |
| **AVS2 (10bit)** | MKV | ✅ **已真机验证** | 需 `patches/davs2-10bit.patch` |
| AVS3-P2 (8bit) | TS / MKV | ✅ | `libuavs3d` |
| **AVS3-P2 (10bit)** | TS / MKV | ✅ **已真机验证** | 需 `patches/uavs3d-10bit.patch` |
| AV3A | TS | ✅ | `libavs3a_decoder.so` + `src/av3a_dec.c` |

### 已验证的实测记录

设备 ATV_S905L3（armeabi-v7a），样本取自
<https://gitee.com/zhengtianbo/avs2_avs3_test_video>。

**AVS2 10bit**（`CCTV4K.AVS2-10bit` 3840×2160 50fps）：

| | 打补丁前 | 打补丁后 |
|---|---|---|
| davs2 输出 | `Un-supported bit-depth 10 in this version.` | `frames=16, bit_depth=10, bytes/sample=2, err=0` |
| 8bit 回归 | — | `test1`/`风电` 均正常出帧，**不退化** |

davs2 的版本串后缀自证位深：`.10` = 10bit、`.8` = 8bit。

**AVS3 10bit**（`jellyfish-640x360-avs3-10bit`，640×360，`yuv420p10le`）：

| | 旧库（APK 内） | 新编 10bit 库 |
|---|---|---|
| 结果 | 第 1 个 NAL 即 `ERR_BAD_BITDEPTH`，**0 帧** | **14 帧，`max_out_bit_depth=10`，`ERR_BAD_BITDEPTH=0`** |
| 8bit 回归（`test5_8.avs3`） | — | 14 帧，`max_out_bit_depth=8`，**不退化** |

版本标识：新库 `VERSION_SHA1 = 0e20d2c…`，旧库 `590f8df…`。

### 两个容易踩的判据陷阱

1. **davs2 的位深不能看版本串后缀。** 运行时日志里
   `davs2: 1.6.206 ef9a9016ce2b622f.10, …` 的 `.10` 是 printf `%d`
   （位深）**运行时**拼的，`.rodata` 里只有 `1.6.206 ef9a9016ce2b622f`，
   `strings` 永远抓不到 `.10`。
   可靠判据是**反向**的：`Un-supported bit-depth %d in this version.`
   这条报错串在源码里位于 `#if HIGH_BIT_DEPTH` 的 `#else` 分支，
   **只存在于 8bit 构建**。有它 = 8bit，没有 = 10bit。
2. **uavs3d 没有静态位深判据。** `COMPILE_10BIT` 是编译期宏，10bit 分支里
   没有任何字符串字面量，且两版导出符号完全一致（`nm -D` 差集为空）。
   而且 uavs3d 上游的 `VERSION_STR` **一直停在 "1.2.1"**（实测新旧版都报
   1.2.1），所以版本号也没参考价值 —— 可信的只有 `.rodata` 里的 40 位
   `VERSION_SHA1`。
   → uavs3d 的位深**只能跑 `probes/uavs3dprobe` 功能验证**。

### NAL 0xB2 说明

`uavs3d` 对 AVS3 的用户数据 NAL（类型 `0xB2` 等）一律返回 `ERR_UNKNOWN_NAL`，
新旧版均如此。**这不是错误状态** —— 后续 picture NAL 仍能正常解，
所以探针遇到它必须跳过而不是中止（`probes/uavs3dprobe.c` 里已这么处理）。

---

## 构建

工作流是 `workflow_dispatch` 手动触发，**`release_tag` 留空即可自动递增**：

```yaml
release_tag: ""     # 留空 = 自动 +1
```

版本号规则：`libmpv-cavs-dynamic-vNNN`（3 位补零）。触发时读取本仓库所有
release tag，取最大 N 加一。手动填了就用填的值。

Android 产物在 `artifacts/{arm64-v8a,armeabi-v7a}/`，最终上传到对应 release。

> ⚠️ 补零 + bash 算术有个坑：`$((012))` 会被当**八进制**解析成 10，
> 所以必须先 `awk '{printf "%d",$0}'` 去掉前导零再做加法 ——
> 否则从 `v010` 起版本号就会开始算错。工作流里已处理并注释。

### 工作流里 10bit 是怎么进去的

```
checkout 本仓库
  → 下载依赖（davs2 1.6 / uavs3d / ...）
  → Apply 10-bit patches        ← git apply，不是 sed
  → Patch davs2 source          ← 上游的线程池同步化 + NULL 安全
  → Build libuavs3d             ← cmake -DCOMPILE_10BIT=1
  → Build libmpv / FFmpeg       ← davs2.sh 里 --bit-depth=10
  → Verify 10-bit support       ← 位深不对就让流水线红
  → Upload artifacts
```

补丁**为什么用 `git apply` 而不是 `sed`**：上游源码一变，`git apply` 会明确报
冲突（并在 CI 里 fail），而 `sed -i` 静默不匹配后你会得到一份缺胳膊少腿的
`.so`，且要等到真机播放才发现。

---

## 本地应用补丁 / 自检

```bash
# 先看不改文件，确认能干净应用
scripts/apply-patches.sh --dry-run /path/to/build-workspace

# 实际应用
scripts/apply-patches.sh /path/to/build-workspace

# 构建产物位深自检
scripts/verify-10bit.sh artifacts/arm64-v8a
```

---

## 验证（真机探针）

`probes/` 下两个探针直接 dlopen `.so` 并调用其公开 API，绕开 FFmpeg/mpv，
用来回答一个具体问题：**这个 `.so` 到底能不能解 10bit**。

```bash
# 编译（替换成你的 NDK 路径，两个 ABI 各编一次）
$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/armv7a-linux-androideabi21-clang \
  --target=armv7a-linux-androideabi21 -O2 -fPIE -pie \
  -march=armv7-a -mfpu=neon -mfloat-abi=softfp \
  -o davs2probe32 probes/davs2probe.c -ldl -lm

adb push davs2probe32 /data/local/tmp/avsprobe/
adb push libdavs2.so    /data/local/tmp/avsprobe/
adb push libc++_shared.so /data/local/tmp/avsprobe/   # davs2 是 C++ 编的，必需

adb shell "cd /data/local/tmp/avsprobe && \
  LD_LIBRARY_PATH=. ./davs2probe32 ./libdavs2.so in.avs2 3000000 25"
```

判定：输出 `VERDICT 10BIT_OK` 且 `max_bit_depth=10`、`err_frames=0`。
8bit 流应得 `VERDICT OUTPUT_8BIT`（回归）。

### 从容器提取裸流

```bash
ffmpeg -v error -y -i input.mkv -map 0:v:0 -c:v copy -f data out.avs2
# AVS3 同理，扩展名用 .avs3
```

### 已知坑

* **davs2 是 C++ 编的**（`.so` 有 `_Z` 符号），dlopen 时找不到
  `libc++_shared.so` 会直接失败 —— 记得一起推。
* **`send_packet` 必须按 access unit 喂**（起始码 `00 00 01` + `B0`/`B3`/`B6`
  边界），整包 send 不产帧；`recv_frame` 返回 `DAVS2_DEFAULT` 时要继续轮询，
  不能 break。
* `davs2_seq_info_t` 的字段顺序和直觉不同，别照感觉写，以 `source/davs2.h` 为准。
* 4K/10bit 在 armv7 盒子上很慢，探针要限制 AU 数量并设 idle 上限。

---

## 待办

- [x] uavs3d 10bit 真机复验 —— **通过**（14 帧，`bit_depth=10`，`ERR_BAD_BITDEPTH=0`）
- [ ] 10bit 出帧后验证 mpv VO 能否吃 `yuv420p10le`（下一步）
- [ ] HDR 元数据透传（`CCTV8K` 是 bt2020 + HLG / `arib-std-b67`）；
      app 侧 `Avs2Reader` 目前只报 `video/avs3`、不带色彩信息
- [ ] 找一份 **10bit TS 承载**样本（现有 10bit 流都是 MKV）
- [ ] arm64-v8a 的 uavs3d 10bit 验证（本次只在 armeabi-v7a 上验过；
      workflow 对 arm64 走 arm64 asm 路径，需在 arm64 设备复跑）

---

## 许可

跟随上游 mpv-android（GPL-2.0+）与各依赖（davs2/uavs3d 等）。
`src/av3a_dec.c` 是本仓库为接入 `libavs3a_decoder.so` 写的 wrapper，
同样按 GPL 发布。
