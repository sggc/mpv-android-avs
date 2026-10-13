/* davs2seekprobe — 复现 / 验证 davs2 1.6 分支的 seek 死锁
 *
 * 背景
 * ----
 * source/common/davs2.cc 的 decoder_decode_es_unit()：
 *
 *     davs2_thread_mutex_lock(&mgr->mutex_aec);           // 加锁
 *     if (parse_header(...) == 0) {
 *         if (task_get_references(...) == 0) {
 *             ...
 *             davs2_thread_mutex_unlock(&mgr->mutex_aec);
 *             davs2_threadpool_run(...);
 *         }
 *         // ← 返回 -1 时【没有解锁】← 锁泄漏（1.6 分支的缺陷）
 *     } else {
 *         davs2_thread_mutex_unlock(&mgr->mutex_aec);
 *     }
 *
 * seek 之后 DPB 里还是上一个 GOP 的帧，新位置要参考的帧找不到，
 * task_get_references() 就 return -1 命中这条泄漏路径；
 * 于是下一次 davs2_decoder_send_packet() 永久阻塞在 mutex_aec 上。
 *
 * 本探针怎么复现
 * --------------
 *   1. 喂 AU[0]（序列头 0xB0）——建立序列上下文
 *   2. 喂 AU[1..pre]（正常解码，把 DPB 填起来）
 *   3. 可选：调 davs2_decoder_flush()（FFmpeg wrapper 在 seek 时就会调它）
 *   4. 【跳跃】直接喂 AU[jump]（流中段的 P/B 帧）——它的参考帧不在 DPB 里
 *      → task_get_references() 失败 → 命中锁泄漏路径
 *   5. 再喂 AU[jump+1] ← **老库在这一步永久挂住；修好的库正常返回**
 *
 * 用法： davs2seekprobe <libdavs2.so> <in.avs2> [jumpIndex=40] [preCount=6] [flush=1]
 * 判读： 打印 "RESULT NO_DEADLOCK" 并正常退出 = 走完了整条路径
 *        卡在某次 send 上并最终被 kill   = 复现了阻塞
 *
 * ⚠️ 正确用法（关键）：必须 `flush=0`，且失败点之后要**继续多发几个 AU**。
 * 2026-10-13 第一次尝试用 flush=1，新老库都在 davs2_decoder_flush() 里挂住
 * ——那是「同步线程池 stub + 拿垃圾数据喂 4K 流」的另一类卡顿，**与本 bug 无关**，
 * 会掩盖真正的现象。改成 flush=0 后对照非常干净。
 *
 * 实测结果（2026-10-13，ATV_S905L3 / armv7，cctv4k10.avs2，jump=40 pre=4）
 * ---------------------------------------------------------------------------
 *   老库 libdavs2_OLD.so（无修复）：
 *     STEP 5.1  send AU[41] → "reference frame of [coi: 64, poc: 62]: <COI: 63>
 *                              not found." → 返回 0  ← mutex_aec 在这里被泄漏
 *     STEP 5.2  send AU[42] → **永久挂住**，15s 后 alarm 触发
 *     RESULT DEADLOCK_REPRODUCED   EXIT=99
 *
 *   新库 libdavs2_NEW.so（含修复）：
 *     STEP 5.1  send AU[41] → 同样报 reference not found → 返回 0
 *     STEP 5.2  send AU[42] → 报错但返回 0，继续
 *     STEP 5.3 / 5.4        → 各报一次错，都正常返回
 *     RESULT NO_DEADLOCK           EXIT=0
 *
 * → 同一输入、同一触发点，唯一差异就是 decoder_decode_es_unit() 里那个
 *   `else { unlock; task_unload_packet; }`。**修复有效已被真机 A/B 证实。**
 *
 * 编译： armv7a-linux-androideabi21-clang -O2 -fPIE -pie -march=armv7-a \
 *          -mfpu=neon -mfloat-abi=softfp -o davs2seekprobe32 davs2seekprobe.c -ldl
 *        aarch64-linux-android21-clang -O2 -fPIE -pie -o davs2seekprobe64 davs2seekprobe.c -ldl
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <unistd.h>
#include <signal.h>

#define DAVS2_ERROR      (-1)
#define DAVS2_DEFAULT    0
#define DAVS2_GOT_FRAME  1
#define DAVS2_GOT_HEADER 2
#define DAVS2_END        3

typedef struct davs2_param_t {
    int   threads;
    int   info_level;
    void *opaque;
    int   disable_avx;
} davs2_param_t;

typedef struct davs2_seq_info_t {          /* 与 source/davs2.h 逐字段对齐 */
    uint32_t profile_id;
    uint32_t level_id;
    uint32_t progressive;
    uint32_t width;
    uint32_t height;
    uint32_t chroma_format;
    uint32_t aspect_ratio;
    uint32_t low_delay;
    uint32_t bitrate;
    uint32_t internal_bit_depth;
    uint32_t output_bit_depth;
    uint32_t bytes_per_sample;
    float    frame_rate;
    uint32_t frame_rate_id;
} davs2_seq_info_t;

typedef struct davs2_packet_t {
    const uint8_t *data;
    int   len;
    int64_t pts;
    int64_t dts;
} davs2_packet_t;

typedef struct davs2_picture_t {
    void    *magic;
    uint8_t *planes[3];
    int      widths[3];
    int      lines[3];
    int      strides[3];
    int      pic_order_count;
    int      type;
    int      qp;
    int64_t  pts;
    int64_t  dts;
    int      num_planes;
    int      bytes_per_sample;
    int      bit_depth;
    int      b_decode_error;
    void    *dec_frame;
} davs2_picture_t;

typedef void *(*davs2_open_t)(davs2_param_t *);
typedef int   (*davs2_send_t)(void *, davs2_packet_t *);
typedef int   (*davs2_recv_t)(void *, davs2_seq_info_t *, davs2_picture_t *);
typedef void  (*davs2_unref_t)(void *, davs2_picture_t *);
typedef void  (*davs2_close_t)(void *);
typedef void  (*davs2_flush_t)(void *, davs2_seq_info_t *, davs2_picture_t *);

static davs2_open_t  fn_open;
static davs2_send_t  fn_send;
static davs2_recv_t  fn_recv;
static davs2_unref_t fn_unref;
static davs2_close_t fn_close;
static davs2_flush_t fn_flush;

/* ---------------- AU 索引 ---------------- */
typedef struct { long off; long len; uint8_t type; } au_t;

#define MAX_AU 8192
static au_t g_au[MAX_AU];
static int  g_nau = 0;

static void build_au_index(const uint8_t *b, long n)
{
    long i = 0;
    while (i + 4 < n && g_nau < MAX_AU) {
        if (b[i] == 0 && b[i+1] == 0 && b[i+2] == 1) {
            uint8_t t = b[i+3];
            if (t == 0xB0 || t == 0xB3 || t == 0xB6) {
                if (g_nau > 0) g_au[g_nau-1].len = i - g_au[g_nau-1].off;
                g_au[g_nau].off  = i;
                g_au[g_nau].len  = n - i;      /* 先给到文件尾，稍后修正 */
                g_au[g_nau].type = t;
                g_nau++;
            }
        }
        i++;
    }
    if (g_nau > 0) g_au[g_nau-1].len = n - g_au[g_nau-1].off;
}

static const char *auname(uint8_t t)
{
    return t == 0xB0 ? "SEQHDR(B0)" : t == 0xB3 ? "I(B3)" : "PB(B6)";
}

/* ---------------- 解码器 ---------------- */
static void *g_dec;

static void drain(const char *tag)
{
    davs2_picture_t pic;
    davs2_seq_info_t info;
    int idle = 0, frames = 0, hdrs = 0;
    while (idle < 40) {
        memset(&pic, 0, sizeof(pic));
        memset(&info, 0, sizeof(info));
        int r = fn_recv(g_dec, &info, &pic);
        if (r == DAVS2_GOT_HEADER) { hdrs++;   idle = 0; continue; }
        if (r == DAVS2_GOT_FRAME)  { frames++; idle = 0; fn_unref(g_dec, &pic); continue; }
        if (r == DAVS2_DEFAULT)    { idle++; continue; }
        break;
    }
    fprintf(stderr, "        [drain %s] frames=%d hdrs=%d\n", tag, frames, hdrs);
}

/* 返回 send_packet 的返回值 */
static int feed(int idx)
{
    davs2_packet_t p;
    memset(&p, 0, sizeof(p));
    p.data = NULL;
    p.len  = 0;
    if (idx < 0 || idx >= g_nau) return -999;
    /* data 由调用方填（需要 buf 指针），这里用全局 */
    extern uint8_t *g_buf;
    p.data = g_buf + g_au[idx].off;
    p.len  = (int)g_au[idx].len;
    p.pts  = idx;
    return fn_send(g_dec, &p);
}

uint8_t *g_buf = NULL;

static void on_alarm(int sig)
{
    (void)sig;
    fprintf(stderr, "\n[probe] !!! 15s 超时 —— 卡在最后一次 send_packet 上（mutex_aec 死锁）\n");
    fprintf(stderr, "RESULT DEADLOCK_REPRODUCED\n");
    fflush(stderr);
    _exit(99);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <libdavs2.so> <in.avs2> [jump=40] [pre=6] [flush=1]\n", argv[0]);
        return 2;
    }
    int jump = (argc >= 4) ? atoi(argv[3]) : 40;
    int pre  = (argc >= 5) ? atoi(argv[4]) : 6;
    int dofl = (argc >= 6) ? atoi(argv[5]) : 1;

    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }
    fn_open  = (davs2_open_t) dlsym(h, "davs2_decoder_open");
    fn_send  = (davs2_send_t) dlsym(h, "davs2_decoder_send_packet");
    fn_recv  = (davs2_recv_t) dlsym(h, "davs2_decoder_recv_frame");
    fn_unref = (davs2_unref_t)dlsym(h, "davs2_decoder_frame_unref");
    fn_close = (davs2_close_t)dlsym(h, "davs2_decoder_close");
    fn_flush = (davs2_flush_t)dlsym(h, "davs2_decoder_flush");
    if (!fn_open || !fn_send || !fn_recv || !fn_unref) {
        fprintf(stderr, "missing core symbols\n"); return 1;
    }
    fprintf(stderr, "[probe] davs2_decoder_flush %s\n", fn_flush ? "present" : "MISSING");

    FILE *f = fopen(argv[2], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    g_buf = (uint8_t *)malloc(n);
    if (fread(g_buf, 1, n, f) != (size_t)n) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    build_au_index(g_buf, n);
    fprintf(stderr, "[probe] 文件 %ld 字节, 解析出 %d 个 AU\n", n, g_nau);
    if (g_nau < 3) { fprintf(stderr, "AU 太少，无法测试\n"); return 1; }
    for (int i = 0; i < g_nau && i < 4; i++)
        fprintf(stderr, "        AU[%d] @%ld len=%ld %s\n", i, g_au[i].off, g_au[i].len, auname(g_au[i].type));
    if (jump < g_nau)
        fprintf(stderr, "        AU[%d] @%ld len=%ld %s   <-- 跳跃目标\n",
                jump, g_au[jump].off, g_au[jump].len, auname(g_au[jump].type));
    if (jump + 1 < g_nau)
        fprintf(stderr, "        AU[%d] @%ld len=%ld %s   <-- 跳跃目标+1（老库在这挂）\n",
                jump+1, g_au[jump+1].off, g_au[jump+1].len, auname(g_au[jump+1].type));

    davs2_param_t param;
    memset(&param, 0, sizeof(param));
    param.threads    = 1;
    param.info_level = 1;
    param.disable_avx = 1;

    signal(SIGALRM, on_alarm);
    alarm(15);

    g_dec = fn_open(&param);
    if (!g_dec) { fprintf(stderr, "open NULL\n"); return 1; }

    /* ---- 1. 序列头 ---- */
    fprintf(stderr, "STEP 1  send AU[0] %s -> %d\n", auname(g_au[0].type), feed(0));
    drain("seqhdr");

    /* ---- 2. 正常解几帧，填 DPB ---- */
    for (int i = 1; i <= pre && i < g_nau; i++) {
        int r = feed(i);
        fprintf(stderr, "STEP 2  send AU[%d] %s -> %d\n", i, auname(g_au[i].type), r);
        drain("pre");
    }

    /* ---- 3. flush（模拟 seek 时 FFmpeg wrapper 的行为）---- */
    if (dofl && fn_flush) {
        davs2_seq_info_t inf; davs2_picture_t pic;
        memset(&inf, 0, sizeof(inf)); memset(&pic, 0, sizeof(pic));
        fprintf(stderr, "STEP 3  davs2_decoder_flush() ...\n");
        fn_flush(g_dec, &inf, &pic);
        fprintf(stderr, "STEP 3  flush done\n");
        drain("flush");
    } else {
        fprintf(stderr, "STEP 3  (跳过 flush)\n");
    }

    /* ---- 4. 跳跃：喂流中段的 P/B 帧，参考帧不在 DPB 里 ---- */
    fprintf(stderr, "STEP 4  send AU[%d] %s  <-- 预期 task_get_references() 失败 -> 老库在这里泄漏 mutex_aec\n",
            jump, auname(g_au[jump].type));
    int r4 = feed(jump);
    fprintf(stderr, "STEP 4  send 返回 %d\n", r4);
    drain("jump");

    /* ---- 5. 关键：失败之后的每一次 send 都是试金石 ----
     * 若上一步（或这一步）触发了 task_get_references() 失败，老库会泄漏
     * mutex_aec，那么【下一次】 send 就会永久阻塞。多发几次才抓得住。 */
    int r5 = 0;
    for (int k = 1; k <= 4 && jump + k < g_nau; k++) {
        fprintf(stderr, "STEP 5.%d  send AU[%d] %s  <-- 若之前泄漏了 mutex_aec，这里永久挂住\n",
                k, jump + k, auname(g_au[jump + k].type));
        r5 = feed(jump + k);
        fprintf(stderr, "STEP 5.%d  send 返回 %d\n", k, r5);
        drain("after");
    }

    fprintf(stderr, "\nRESULT NO_DEADLOCK  (send#4=%d last send=%d)\n", r4, r5);
    fflush(stderr);
    fn_close(g_dec);
    return 0;
}
