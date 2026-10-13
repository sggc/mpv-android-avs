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
