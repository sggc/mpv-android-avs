#!/bin/bash
# ============================================================================
#  verify-10bit.sh — 位深自检（本地 / CI 均可）
#
#  用法： ./verify-10bit.sh <artifacts-dir>
#  退出： 0 = 全部通过；1 = 有 FAIL；3 = 用法/目录错误
#
#  ── 判据说明（都是实测+源码双重确认过的，别凭直觉改）────────────────────
#
#  davs2 —— 有可靠静态判据。
#     源码 source/common/header.cc 里：
#         #if HIGH_BIT_DEPTH
#             max_pel_value = (1 << g_bit_depth) - 1;
#         #else
#             davs2_log(..., "Un-supported bit-depth %d in this version.\n");
#         #endif
#     即 "Un-supported bit-depth" 这条报错串**只存在于 8bit 构建**。
#     实测：8bit 库 strings 命中 1 次，10bit 库命中 0 次。
#     → 库里有这条串 = 8bit（FAIL）；没有 = 10bit（OK）。
#
#     ⚠️ 不要用版本串后缀判位深。运行时日志里的
#        "davs2: 1.6.206 ef9a9016ce2b622f.10, ..." 那个 ".10" 是 printf 的
#        "%d" 参数（位深）在**运行时**拼上去的，.rodata 里只存
#        "1.6.206 ef9a9016ce2b622f"，strings 永远抓不到 ".10"。
#
#  uavs3d —— 没有可靠静态判据。
#     COMPILE_10BIT 是编译期宏，只改变代码路径；10bit 分支里没有任何字符串
#     字面量，且两版导出符号完全一致（实测 nm -D 差集为空）。
#     → 本脚本只能报 UNKNOWN，**权威验证必须跑 probes/uavs3dprobe**。
#
#  本脚本是构建期第一道闸门，挡"开关没生效"这类回退；不等于功能验证。
# ============================================================================
set -uo pipefail

DIR="${1:-}"
[ -d "$DIR" ] || { echo "用法: $0 <artifacts-dir>" >&2; exit 3; }

fail=0
warn=0

check_davs2() {
    local so="$1" marker
    echo "=== $(basename "$so") ==="
    # 8bit-only 的报错串
    marker=$(strings "$so" 2>/dev/null | grep -c "Un-supported bit-depth")
    if [ "$marker" -gt 0 ]; then
        echo "  [FAIL] 检出 \"Un-supported bit-depth\" —— 这是 8bit 构建的标志串"
        echo "         --bit-depth=10 或 patches/davs2-10bit.patch 未生效"
        return 1
    fi
    echo "  [OK]   无 8bit 标志串，判定为 10bit 构建"

    # 附带：版本串里能看出是否全克隆（浅克隆会丢 patch 号，仅提示）
    local ver
    ver=$(strings "$so" 2>/dev/null | grep -oE '1\.[0-9]+\.[0-9]{1,4}[[:space:]]+[0-9a-f]{8,40}' | head -1)
    [ -n "$ver" ] || ver=$(strings "$so" 2>/dev/null | grep -oE '1\.[0-9]+\. [0-9a-f]{8,40}' | head -1)
    if [ -n "$ver" ]; then
        echo "  [info] version: $ver"
        case "$ver" in
            "1.6. "*) echo "  [warn] 版本串缺 patch 号（形如 '1.6. <hash>'）—— "
                      echo "         是浅克隆导致 version.sh 算不出来。CI 里用的是全克隆，"
                      echo "         正常应为 '1.6.206 <hash>'。仅影响版本号，不影响位深。"
                      warn=1 ;;
        esac
    fi
    return 0
}

check_uavs3d() {
    local so="$1" sha
    echo "=== $(basename "$so") ==="

    # uavs3d 上游的 VERSION_STR 一直停在 "1.2.1"（实测 8bit 旧版与 10bit 新版
    # 都报 1.2.1），版本号没有参考价值。真正可信的是 .rodata 里的 40 位
    # git SHA1（VERSION_SHA1 编译进来的字面量）。
    sha=$(strings "$so" 2>/dev/null | grep -oE '[0-9a-f]{40}' | head -1)
    echo "  [info] VERSION_SHA1: ${sha:-<none>}"

    case "$sha" in
      590f8df384934e9c0464b763bafd5f2b229a114b)
        echo "  [FAIL] 命中已知旧版 590f8df（版本被 .pc 手写锁死在 1.2.1）"
        echo "         workflow 已改为从 version.h 取真实版本，请确认生效"
        return 1 ;;
      "") echo "  [?]    取不到 SHA1（库被 strip？）" ;;
      *)  echo "  [OK]   非已知旧版 commit" ;;
    esac

    echo "  [WARN] uavs3d 无可靠静态位深判据（COMPILE_10BIT 是编译期宏，"
    echo "        10bit 分支无字符串字面量，导出符号与 8bit 完全一致）"
    echo "        → 必须跑 probes/uavs3dprobe 做功能验证，判据："
    echo "           max_out_bit_depth >= 10 且 ERR_BAD_BITDEPTH == 0"
    return 0
}

for so in "$DIR/libdavs2.so" "$DIR/libuavs3d.so"; do
    [ -f "$so" ] || { echo "::warning::$(basename "$so") 不在 $DIR"; continue; }
    case "$(basename "$so")" in
        libdavs2.so)  check_davs2  "$so" || fail=1 ;;
        libuavs3d.so) check_uavs3d "$so" || fail=1 ;;
    esac
done

echo
if [ "$fail" != "0" ]; then
    echo "位深自检未通过。"
    exit 1
fi
if [ "$warn" != "0" ]; then
    echo "位深自检通过（有警告，见上）。"
else
    echo "位深自检通过（uavs3d 仍需跑 probes/uavs3dprobe 做功能确认）。"
fi
exit 0
