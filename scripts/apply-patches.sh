#!/bin/bash
# ============================================================================
#  apply-patches.sh — 把本仓库的补丁应用到 mpv-android / davs2 / uavs3d 源码
#
#  用法：
#     ./apply-patches.sh [--dry-run] <repo-root>
#
#  设计原则：用 git apply 而不是 sed。
#    上游源码一变，git apply 会明确报冲突（非零退出），而不是 sed 静默不匹配
#    后构建产物莫名缺东西。--dry-run 只做检查不改文件，适合 CI 里先跑一遍。
#
#  目录约定（与 build-libmpv-android.yml 一致）：
#    davs2   <root>/mpv-android/buildscripts/deps/davs2
#    uavs3d  <root>/uavs3d-src
# ============================================================================
set -uo pipefail

DRY=0
[ "${1:-}" = "--dry-run" ] && { DRY=1; shift; }
ROOT="${1:-$PWD}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCH_DIR="$HERE/../patches"

rc=0

apply() {
    local name="$1" dir="$2" patch="$3"
    if [ ! -d "$dir" ]; then
        echo "::warning::$name 源码目录不存在，跳过: $dir"
        return 0
    fi
    if [ ! -f "$PATCH_DIR/$patch" ]; then
        echo "::error::补丁文件缺失: $PATCH_DIR/$patch"
        return 1
    fi

    echo "=== [$name] $patch -> $dir ==="

    # 已打过（反向可应用）则跳过，保证脚本可重复执行
    if (cd "$dir" && git apply --reverse --check "$PATCH_DIR/$patch" 2>/dev/null); then
        echo "  [SKIP] 已应用过"
        return 0
    fi

    if [ "$DRY" = "1" ]; then
        if (cd "$dir" && git apply --check "$PATCH_DIR/$patch" 2>&1); then
            echo "  [OK]   --dry-run: 可干净应用"
            return 0
        fi
        echo "::error::$patch 无法干净应用到 $name —— 上游源码变动？"
        return 1
    fi

    if (cd "$dir" && git apply "$PATCH_DIR/$patch"); then
        echo "  [OK]   已应用"
        return 0
    fi
    echo "::error::$patch 应用到 $name 失败"
    return 1
}

DAVS2=$(find "$ROOT/mpv-android/buildscripts/deps" -maxdepth 1 -type d -name davs2 2>/dev/null | head -1)
UAVS3D="$ROOT/uavs3d-src"

apply "davs2"  "$DAVS2"  davs2-10bit.patch  || rc=1
apply "uavs3d" "$UAVS3D" uavs3d-10bit.patch || rc=1

echo
if [ "$rc" = "0" ]; then
    echo "所有补丁处理完成。"
else
    echo "有补丁未应用成功，见上方 error。" >&2
fi
exit $rc
