# 补丁说明

所有补丁都用 `git apply` 应用（不是 `sed`）——上游源码一变，`git apply` 会明确
报冲突，而不是 sed 静默不匹配后构建产物莫名缺东西。

统一入口：`scripts/apply-patches.sh [--dry-run] <仓库根>`
构建期自检：`scripts/verify-10bit.sh <artifacts 目录>`

---

## davs2-10bit.patch

**目的**：让 davs2 能输出 10bit（AVS2 高位深）。

涉及 3 个文件、3 类改动：

### 1. `build/linux/configure` —— 放开上游的 9/10bit 闸门

上游原样（`--bit-depth=*` 分支）：

```bash
if [ "$bit_depth" -lt "8" -o "$bit_depth" -gt "10" ]; then
    echo "Supplied bit depth must be in range [8,10]."; exit 1
elif [[ "$bit_depth" = "9" || "$bit_depth" = "10" ]]; then
    echo "BitDepth $bit_depth not supported currently."
    exit 1                                    # ← 拦在这
fi
```

改成只做范围检查。

**为什么可以放行**：源码的 `HIGH_BIT_DEPTH` 路径其实是完整的 ——

* `source/common/mc.cc` 里运动补偿用 `g_bit_depth` 做动态位移
  （`shift1 = g_bit_depth - 8`、`shift2 = 20 - g_bit_depth`），不是写死 8；
* `source/common/pixel.cc` 有多处 `#if HIGH_BIT_DEPTH` 分支；
* `source/common/common.h` 的 `pel_t` / `pel4_t` / `itr_t` 按位深条件 typedef。

是上游保守起见没放开，不是没实现。

### 2. `source/common/decoder.cc` —— 零拷贝快路径补强转

```c
if (!shift1 && sizeof(pel_t) == num_bytes_per_sample) {
    pic->dec_frame = frame;
-   pic->planes[0]  = frame->planes[0];
+   pic->planes[0]  = (uint8_t *)(frame->planes[0]);
    ...
}
```

`sizeof(pel_t) == num_bytes_per_sample` 这个条件 **8bit 时 1==1、10bit 时 2==2
都会成立**，所以 10bit 同样会走这条零拷贝路径。上游在这里把内部
`pel_t *` 直接赋给外部 API 的 `uint8_t *planes[]`，C 里是 error。

**为什么强转安全**：外部 API `davs2_picture_t` 本来就有
`bytes_per_sample` / `bit_depth` / `num_planes` 字段，且上方
`pic->strides[i] = ... * num_bytes_per_sample` 已按字节计 ——
调用方就是按 `bytes_per_sample` 解释这块缓冲的。**上游为 10bit 留了口，
只是这两处没写完。**

### 3. `source/common/osdep.h` —— `ALIGN_POINTER` 宏改类型中性

```c
- #define ALIGN_POINTER(p)  (p) = (uint8_t *)((intptr_t)((p)+(CACHE_LINE_SIZE-1)) & ~(intptr_t)(CACHE_LINE_SIZE-1))
+ #define ALIGN_POINTER(p)  (p) = (__typeof__(p))((void *)((intptr_t)((p)+(CACHE_LINE_SIZE-1)) & ~(intptr_t)(CACHE_LINE_SIZE-1)))
```

**不能简单改成 `(uint8_t *)` 或 `(pel_t *)`**：`frame.cc` 里 `mem_ptr`
（`uint8_t *`）和 `frame->planes[]`（`pel_t *`）**都在用这个宏** ——
两种改法各错一次（实测）。`__typeof__(p)` 是唯一正确解。

---

## davs2-seek-deadlock.patch

**目的**：修掉「播放本地 AVS2 文件时拖进度条 → 画面卡死 → 应用被 ANR 杀掉」的根因。

### 症状（真机实测，vivo PD2520 / arm64）

拖动进度条松手后：

```
MpvEngine  W  seekTo(2914ms) prepared=true rtspVod=false
[mkv:v]       queuing seek to 2.937000
[cplayer:v]   hr-seek, skipping to 2.914000
[mkv:v]       EOF reached.
              ← 此后 5 秒 mpv 完全无输出
              ← 5s 后 ANR，进程被杀，应用重启
```

ANR 原文（`dumpsys activity lastanr`）：

```
Reason: Input dispatching timed out (...VideoActivity is not responding.
        Waited 5001ms for MotionEvent).
```

### 完整线程栈（来自 dropbox 的 data_app_anr）

主线程 —— 卡在**阻塞式** mpv 属性读取上：

```
"main" tid=1 Native
  libc.so  pthread_cond_wait
  libmpv.so   (mpv_get_property+104)
  libplayer.so Java_is_xyz_mpv_MPV_getPropertyDouble+68
  MpvPlayerEngine.getPosition
  PlayerManager.getPosition
  CustomSeekView.updateProgressFromManager
  CustomSeekView.updateProgress
  CustomSeekView.seekToTimeBarPosition        ← 松手回调里同步调了 updateProgress()
  CustomSeekView.onScrubStop
  androidx.media3.ui.DefaultTimeBar.stopScrubbing
  androidx.media3.ui.DefaultTimeBar.onTouchEvent   ← 用户的 MotionEvent
```

视频解码线程 —— 卡在 **libdavs2 内部的互斥锁**上：

```
"Thread-20" tid=98 Native
  libc.so  NonPI::MutexLockWithTimeout        ← pthread_mutex_lock，永久阻塞
  libdavs2.so (decoder_decode_es_unit+200)    ← davs2.cc:595 的 mutex_aec
  libdavs2.so (davs2_decoder_send_packet+180)
  libavcodec.so (avcodec_send_packet+172)
  libmpv.so ...
```

### 根因

`source/common/davs2.cc` 的 `decoder_decode_es_unit()`：

```c
davs2_thread_mutex_lock(&mgr->mutex_aec);              // 595 加锁

if (parse_header(h, &es_unit->bs) == 0) {
    h->p_bs = &es_unit->bs;
    if (task_get_references(h, es_unit->pts, es_unit->dts) == 0) {
        ...
        davs2_thread_mutex_unlock(&mgr->mutex_aec);    // 609 解锁
        davs2_threadpool_run(...);
    }
    // ← task_get_references() 返回 -1 时【没有解锁】← 锁泄漏
} else {
    davs2_thread_mutex_unlock(&mgr->mutex_aec);        // 614 解锁
    task_unload_packet(h, es_unit);
}
```

`task_get_references()`（`header.cc:1232`）在多处 `goto fail` → `return -1`，
最典型的是 L1311-1315：

```c
if (j == mgr->dpbsize) {
    davs2_log(h, DAVS2_LOG_ERROR, "reference frame of [coi: %d, poc: %d]: <COI: %d> not found.", ...);
    goto fail;
}
```

**seek 正好会命中这条路**：跳转后 DPB 里还是上一个 GOP 的帧，新位置要参考的
帧找不到 → 返回 -1 → `mutex_aec` **永久留在锁定状态** → 下一次
`davs2_decoder_send_packet()` 在 595 行阻塞到天荒地老。

于是：视频解码线程挂住 → mpv core 跟着卡住 → 主线程那次阻塞式
`mpv_get_property("time-pos")` 永不返回 → 5 秒输入超时 → **ANR → 进程被杀**。
用户看到的就是「卡住然后闪退」。

### 修法

补上和 `if` 配对的 `else` 分支（**与上游 master 完全一致**）：

```c
        } else {
            davs2_thread_mutex_unlock(&mgr->mutex_aec);
            /* task is free */
            task_unload_packet(h, es_unit);
        }
```

> **上游对照**：`master` 分支的这个 `else` 分支**是有的**；工作流 pin 的
> `--branch 1.6` **没有**。也就是说这是 1.6 分支的已知缺陷，上游后来修了。
> 补丁内容就是上游那几行，没有自创逻辑。

修好之后，seek 后参考帧找不到只会**丢这一帧**（`task_unload_packet` + 返回
-1，`send_packet` 返回 `DAVS2_ERROR`，FFmpeg wrapper 走既有的
"Decoder warning: can't read packet, flushing" 分支），不再有锁泄漏，
也就不会再死锁。

### 真机 A/B 实测（决定性，2026-10-13，ATV_S905L3 / armv7）

用 `probes/davs2seekprobe.c` 精确复现：喂序列头 → 正常解几帧 → 跳到流中段的
P 帧（参考帧不在 DPB 里）→ 之后连发几个 AU。

```
老库（无修复）：
  STEP 5.1  send AU[41] → "reference frame of [coi: 64, poc: 62]: <COI: 63> not found."
                          → 返回 0        ← mutex_aec 在这里被泄漏
  STEP 5.2  send AU[42] → 永久挂住（15s 后 alarm 触发）
  RESULT DEADLOCK_REPRODUCED   EXIT=99

新库（含修复）：
  STEP 5.1  send AU[41] → 同样报 reference not found → 返回 0
  STEP 5.2  send AU[42] → 报错但返回 0，继续
  STEP 5.3 / 5.4        → 各报一次错，都正常返回
  RESULT NO_DEADLOCK           EXIT=0
```

同一输入、同一触发点，唯一差异就是这一处 `else { unlock; ... }`。

**回归**（新库）：AVS2 10bit 4K → 16 帧 `bit_depth=10 err=0`；
AVS2 8bit → 3 帧 `bit_depth=8 err=0`，**不退化**。

### 这个补丁**不解决**的事

它只堵住锁泄漏。`CustomSeekView.seekToTimeBarPosition()` 里那句
**主线程同步**的 `updateProgress()` → `mpv_get_property()` 仍然是
"mpv 一旦卡住就必然 ANR" 的结构性隐患（本仓库 app 侧待改）。
davs2 修好只是让这次不再触发它。

---

## uavs3d-10bit.patch

**目的**：让 uavs3d 输出 10bit。官方默认就是 8bit-only。

上游 `source/decore/com_def.h`：

```c
#ifndef COMPILE_10BIT
#define COMPILE_10BIT 0      // ← 默认关
#endif
```

官方 README 原文：

> The default configuration only support 8bit decoding.
> To support 10bit streams decoding: `cmake -DCOMPILE_10BIT=1`

本 patch 把默认值改成 1。**工作流里 cmake 也同时传了 `-DCOMPILE_10BIT=1`**，
双保险 —— 任一处失效都能被发现（`verify-10bit.sh` 会报 FAIL）。

> 只有一行改动，之所以单独成 patch 而不是继续 sed：`sed -i` 改默认值
> 在上游调整该文件结构时会静默失效，`git apply` 会报冲突。

---

## 已知非 BUG 差异：stride 对齐

APK 里旧版 davs2 的 stride 对齐到 256（如 `1024/512/512`），本仓库编出来的是
紧凑值（如 `854/427/427`、`8000/4032/4032`）。

FFmpeg 的 avs2dec wrapper 用的是 `pic.strides[]`，两种情况都能正确工作，
**不是 BUG**。列在这里免得下次有人当问题查。
