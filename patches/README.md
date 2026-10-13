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

## davs2-flush-crash.patch

**目的**：修掉「拖进度条闪退」的**第二个**缺陷 —— 原生 SIGSEGV。

> 背景：`davs2-seek-deadlock.patch` 修好之后，症状从 **ANR** 变成了 **native crash**。
> 也就是说前一个是"卡死被系统杀"，这一个才是"真崩"。两个都在同一条 flush 路径上。

### 症状（真机实测，vivo PD2520 / arm64 / Android 13）

拖进度条 → 画面卡住 → 应用直接退出（`Fatal signal 11`）。日志：

```
19:02:06.836 [mkv:v] queuing seek to -0.317000      ← 负数 seek（mpv 收敛到 0.023）
19:02:07.533 Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR),
             fault addr 0x70bf8eb2b8, tid 18280 (Thread-20)
```

tombstone 里的栈：

```
#00 pc 0x11174  libdavs2.so
#01 pc 0x35c50  libdavs2.so  (davs2_get_recons+424)
#02 pc 0x159e8  libdavs2.so  (davs2_decode_lcu_recon+3120)
#03 pc 0x14e48  libdavs2.so  (davs2_decode_lcu_recon+144)
#04 pc 0x17ea8  libdavs2.so  (davs2_decoder_decode_picture_data+1988)
#05 pc 0x3989c  libdavs2.so  (davs2_threadpool_run+36)
#06 pc 0x1272c  libdavs2.so  (decoder_decode_es_unit+448)
#07 pc 0x12adc  libdavs2.so  (davs2_decoder_flush+64)     ← 入口
#08 libavcodec.so
```

> 顺带说：`#06 +448` 正好等于新库里 `bl davs2_threadpool_run` 的返回地址减 4
> （老库是 +444），所以这条栈同时也证明了 APK 里跑的确实是新库。

### 机制（寄存器级）

崩溃指令与寄存器：

```
1115c:  madd  w8, w10, w2, w3     ← w8 = w10*w2 + w3
11160:  sbfiz x12, x8, #1, #32    ← 偏移 x12 = w8*2（按 32 位截断）
11168:  add   x11, x1, x12        ← x11 = 平面基址 + 偏移
11174:  ldrh  w9, [x11]           ← 崩在这里
```

真机寄存器：`x1=0x6febebf740`（合法平面基址）、`x3=0x6c69d15d9c`（**是个指针**）、
`w10=8`、`w2=4` → `x12=0xd3a2bb78` → `x11=0x70bf8eb2b8`（帧缓冲之外）→
`SEGV_MAPERR (read)`，`esr=0x92000005`（读操作、一级页表翻译失败）。

也就是说：`davs2_get_recons()` 里

```c
gf_davs2.add_ps[PART_INDEX(bsx, bsy)](p_dst, i_dst, p_dst, p_coeff, i_dst, i_coeff);
```

拿到的**块尺寸/参数是垃圾** → 偏移算出天文数字 → 越界读。
而垃圾的来源是：**正在解码的这帧的比特流与 DPB 状态不一致**。

### 根因

`source/common/davs2.cc` 的 `davs2_decoder_flush()`：

```c
    mgr->b_flushing = 1;
    ...
    // flush buffered bit-stream
    if (mgr->es_unit != NULL && mgr->es_unit->len >= 4) {
        es_unit_t *es_unit = mgr->es_unit;
        mgr->es_unit = NULL;
        decoder_decode_es_unit(mgr, es_unit);   // ← 强制解码这个 AU
    }
```

`mgr->es_unit` 是**还没被下一个起始码"封口"的那个 access unit**
（`davs2_pack_es_unit()` 只在下一个 B0/B1/B7/B3/B6 到来时才把它交出去）。
seek 会把流截在任意位置，这个 AU 的比特流与 DPB 里的参考帧可能对不上，
**强行解码它就会解出垃圾**（预测模式、变换块尺寸全错）→ `add_ps[]` 越界 → SIGSEGV。

### 修法

把"强制解码"改成"丢弃"：

```c
    if (mgr->es_unit != NULL) {
        es_unit_free(mgr->es_unit);
        mgr->es_unit = NULL;
    }
```

理由：**flush 的语义就是"放弃当前位置"**，这个 AU 本来就没封口、也没人再要它。

> ⚠️ **这段上游 master 与 1.6 完全一致（都没改）**，所以这个补丁不是从上游抄的，
> 是我们自己的健壮性修复。

### 真机 A/B 实测（决定性，2026-10-13，vivo PD2520 / arm64）

`probes/davs2seekprobe.c` 复现：喂 20 个 AU（正常解码）→ 调 `davs2_decoder_flush()`。

```
NOFIX（原始 flush）  EXIT=139 (128+11 SIGSEGV)
                     日志停在 flush 里的 "invalid CU (48,1488), POC 43"
                     崩溃栈与 app tombstone 逐帧一致

FIX（flush 丢弃）    EXIT=0，完整跑完
```

**回归**（FIX 版）：AVS2 10bit 4K → 16 帧 `bit_depth=10 err=0`；
AVS2 8bit → 3 帧 `bit_depth=8 err=0`，**不退化**。

> 排查记录（免得下次重走）：
> * 探针的 AU 切分必须与 davs2 的 `DAVS2_ISUNIT`（**B0/B1/B7/B3/B6**）一致 ——
>   漏了 B1/B7 会喂错位数据，得到"新老库都挂死"的**假**结论。
> * 截断 AU（1%~80%）、flush 后重喂同一批 AU，**都不会崩**；
>   真正稳定的复现条件是「解够多帧（约 20 帧）后调 flush」。
> * 这个 bug 与 seek/截断/重放都无关，**是 `davs2_decoder_flush` 本身**。

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
