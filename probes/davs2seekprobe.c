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
 * 两个独立的缺陷都能用本探针复现/验证（2026-10-13，真机 arm64 + armv7）
 * ---------------------------------------------------------------------------
 *
 * 【A】mutex_aec 锁泄漏死锁（→ ANR）—— 用 flush=0
 *   老库：AU[41] 报 "reference frame ... not found" → 返回 0（锁在此泄漏）
 *         AU[42] → 永久挂住 → 15s alarm → DEADLOCK_REPRODUCED  EXIT=99
 *   新库：同样报错但返回 0，AU[42]/[43]/[44] 各报一次错都正常返回
 *         → NO_DEADLOCK  EXIT=0
 *
 * 【B】davs2_decoder_flush() 强制解码缓冲 AU → SIGSEGV —— 用 flush=1 pre=20
 *   NOFIX：EXIT=139 (SIGSEGV)，日志停在 flush 里的 "invalid CU ... POC 43"，
 *          崩溃栈与 app tombstone 逐帧一致：
 *            #00 libdavs2+0x11174 / #01 davs2_get_recons+424
 *            #04 davs2_decoder_decode_picture_data+1988
 *            #05 davs2_threadpool_run+36 / #06 decoder_decode_es_unit+448
 *            #07 davs2_decoder_flush+64
 *   FIX  ：EXIT=0，完整跑完
 *   → 不涉及 seek/截断/重放，**就是 flush 本身**（pre=8/12/30 不崩，依赖 DPB 状态）
 *
 * ⚠️ 两个必须注意的坑（都踩过）
 *   1) AU 切分必须与 davs2 的 DAVS2_ISUNIT（B0/B1/B7/B3/B6）完全一致；
 *      漏了 B1/B7 会喂错位数据 → 得到"新老库都挂死"的**假**结论。
 *   2) 用 flush=0 验【A】时，失败点之后要**继续多发几个 AU** —— 只发一个的话
 *      泄漏了也没人暴露，会误判成"没复现"。
 *
 * 其它已试过、**不**触发【B】的构造（免得重复劳动）：截断 AU 1%~80%、
 * flush 后重喂同一批 AU。
 * * 编译： armv7a-linux-androideabi21-clang -O2 -fPIE -pie -march=armv7-a \
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
            /* 必须与 davs2 自己的 DAVS2_ISUNIT 完全一致：
             *   B0 / B1 / B7 / B3 / B6
             * 少一个都会让喂进去的"AU"和 davs2 认定的单元边界错位，
             * 于是它解出来的是错位数据 —— 会得到假的崩溃/卡死结论。
             * （第一次就漏了 B1/B7，白折腾一轮。） */
            if (t == 0xB0 || t == 0xB1 || t == 0xB7 || t == 0xB3 || t == 0xB6) {
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
    switch (t) {
    case 0xB0: return "SEQHDR(B0)";
    case 0xB1: return "SEQEXT(B1)";
    case 0xB7: return "SEQDIS(B7)";
    case 0xB3: return "I(B3)";
    case 0xB6: return "PB(B6)";
    default:   return "?(?)";
    }
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
static long g_trunc_len = 0;   /* >0 = 只发这么多字节：模拟"被 seek 截断的 AU" */

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
    if (g_trunc_len > 0 && g_trunc_len < g_au[idx].len) {
        p.len = (int)g_trunc_len;      /* 截断：只喂前 N 字节 */
    }
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
        fprintf(stderr, "usage: %s <libdavs2.so> <in.avs2> [jump=40] [pre=6] [flush=1] [truncPct=0] [replay=0]\n", argv[0]);
        return 2;
    }
    int jump = (argc >= 4) ? atoi(argv[3]) : 40;
    int pre  = (argc >= 5) ? atoi(argv[4]) : 6;
    int dofl = (argc >= 6) ? atoi(argv[5]) : 1;
    int truncPct = (argc >= 7) ? atoi(argv[6]) : 0;
    int replay   = (argc >= 8) ? atoi(argv[7]) : 0;

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
    /* truncPct>0 时，最后一个 pre AU 只喂前 truncPct% 字节 ——
     * 模拟"seek 把流截断在帧中间"，此时 mgr->es_unit 里留的是半截 AU。
     * 紧接着的 davs2_decoder_flush() 会强制解码这半截 AU → 真实场景的崩溃。 */
    for (int i = 1; i <= pre && i < g_nau; i++) {
        g_trunc_len = 0;
        if (truncPct > 0 && i == pre) {
            g_trunc_len = g_au[i].len * (long)truncPct / 100;
            if (g_trunc_len < 8) g_trunc_len = 8;
            fprintf(stderr, "STEP 2  ** 截断 AU[%d]: 只喂 %ld / %ld 字节 (%d%%) **\n",
                    i, g_trunc_len, g_au[i].len, truncPct);
        }
        int r = feed(i);
        g_trunc_len = 0;
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

    /* ---- 3b. replay：flush 之后重新喂同一批 AU ----
     * 模拟"seek 回到同一位置、重解同一批帧"。davs2_decoder_flush() 并不清
     * DPB，于是新帧的 COI 会撞上 DPB 里的旧帧 -> task_get_references()
     * 以为"找到了" -> 拿陈旧参考帧解码 -> 预测全错 -> 变换块尺寸变垃圾 ->
     * add_ps[PART_INDEX()] 越界取到别的函数 -> 参数错位 -> 越界读 -> SIGSEGV。
     */
    if (replay) {
        fprintf(stderr, "STEP 3b REPLAY：flush 后重新喂 AU[1..%d]（DPB 未清，COI 会撞旧帧）\n", pre);
        for (int i = 1; i <= pre && i < g_nau; i++) {
            int r = feed(i);
            fprintf(stderr, "STEP 3b send AU[%d] %s -> %d\n", i, auname(g_au[i].type), r);
            drain("replay");
        }
        fprintf(stderr, "\nRESULT REPLAY_SURVIVED\n");
        fflush(stderr);
        fn_close(g_dec);
        return 0;
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
