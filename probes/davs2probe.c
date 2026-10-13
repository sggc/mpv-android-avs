/* davs2 capability probe v2 — correct API usage per source/davs2.h:
 *   void *davs2_decoder_open(davs2_param_t *param);            // NOT int
 *   int  davs2_decoder_recv_frame(void*, davs2_seq_info_t*, davs2_picture_t *out);
 *                                                              // value, not void**
 * Returns DAVS2_GOT_FRAME=1 / GOT_HEADER=2 / END=3 / ERROR=-1 / DEFAULT=0
 *
 * Reports per frame: bit_depth / bytes_per_sample / strides, plus seq info.
 * 10bit output => max_bit_depth == 10.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

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

static davs2_open_t  fn_open;
static davs2_send_t  fn_send;
static davs2_recv_t  fn_recv;
static davs2_unref_t fn_unref;
static davs2_close_t fn_close;

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <libdavs2.so> <in.avs2> [max_bytes]\n", argv[0]);
        return 2;
    }
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }
    fn_open  = (davs2_open_t) dlsym(h, "davs2_decoder_open");
    fn_send  = (davs2_send_t) dlsym(h, "davs2_decoder_send_packet");
    fn_recv  = (davs2_recv_t) dlsym(h, "davs2_decoder_recv_frame");
    fn_unref = (davs2_unref_t)dlsym(h, "davs2_decoder_frame_unref");
    fn_close = (davs2_close_t)dlsym(h, "davs2_decoder_close");
    if (!fn_open || !fn_send || !fn_recv || !fn_unref) {
        fprintf(stderr, "missing symbols: open=%p send=%p recv=%p unref=%p\n",
                (void*)fn_open, (void*)fn_send, (void*)fn_recv, (void*)fn_unref);
        return 1;
    }

    FILE *f = fopen(argv[2], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    long limit = (argc >= 4) ? atol(argv[3]) : n;
    if (limit > n) limit = n;
    uint8_t *buf = (uint8_t *)malloc(limit);
    if (fread(buf, 1, limit, f) != (size_t)limit) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    davs2_param_t param;
    memset(&param, 0, sizeof(param));
    param.threads    = 1;
    param.info_level = 1;      /* suppress debug log */
    param.opaque     = NULL;
    param.disable_avx = 1;

    void *dec = fn_open(&param);
    if (!dec) { fprintf(stderr, "davs2_decoder_open returned NULL\n"); return 1; }

    /* ---- 逐 access-unit send ----
     * davs2 的 send_packet 期望一个 access unit。AVS2 的 AU 边界就是
     * 序列头 00 00 01 B0 / I 帧 00 00 01 B3 / PB 帧 00 00 01 B6。
     * 只发前几个 AU（默认 8 个，够判定位深），避免跑太久。
     */
    int r_send = 0;
    {
        long max_bytes = limit;
        int  max_aus   = (argc >= 5) ? atoi(argv[4]) : 8;
        long pos = 0;
        int  aus = 0;
        int  last_send = 0;
        while (pos + 8 < max_bytes && aus < max_aus) {
            /* 起点必须是起始码 */
            long start = -1;
            for (long i = pos; i + 4 < max_bytes; i++) {
                if (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 1) { start = i; break; }
            }
            if (start < 0) break;
            /* AU 结束 = 下一个 B0/B3/B6 起始码 */
            long end = max_bytes;
            for (long i = start + 4; i + 4 < max_bytes; i++) {
                if (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 1) {
                    uint8_t t = buf[i+3];
                    if (t == 0xB0 || t == 0xB3 || t == 0xB6) { end = i; break; }
                }
            }
            davs2_packet_t p;
            memset(&p, 0, sizeof(p));
            p.data = buf + start;
            p.len  = (int)(end - start);
            p.pts  = aus;
            last_send = fn_send(dec, &p);
            aus++;
            fprintf(stderr, "[probe] AU#%d @%ld len=%ld send=%d\n",
                    aus, start, end - start, last_send);
            pos = end;
            if (last_send < 0) break;
        }
        fprintf(stderr, "[probe] sent %d AUs (last send=%d)\n", aus, last_send);
        r_send = last_send;
    }

    int frames = 0, max_bd = 0, seqs = 0, errors = 0;
    davs2_picture_t pic;
    davs2_seq_info_t info;
    int idle = 0;

    while (frames < 40 && idle < 200) {
        memset(&pic, 0, sizeof(pic));
        memset(&info, 0, sizeof(info));
        int r = fn_recv(dec, &info, &pic);
        if (r == DAVS2_GOT_HEADER) {
            seqs++;
            idle = 0;
            if (seqs <= 2) {
                fprintf(stderr, "[  seq ] %ux%u profile=%u level=%u prog=%u "
                        "internal_bd=%u output_bd=%u bps=%u chroma=%u rate=%.3f\n",
                        info.width, info.height, info.profile_id, info.level_id,
                        info.progressive, info.internal_bit_depth,
                        info.output_bit_depth, info.bytes_per_sample,
                        info.chroma_format, info.frame_rate);
            }
            continue;
        }
        if (r == DAVS2_GOT_FRAME) {
            frames++;
            idle = 0;
            if (pic.bit_depth > max_bd) max_bd = pic.bit_depth;
            if (frames <= 4) {
                fprintf(stderr, "[frame %d] %dx%d bytes/sample=%d bit_depth=%d "
                        "planes=%d stride=%d/%d/%d err=%d\n",
                        frames, pic.widths[0], pic.lines[0],
                        pic.bytes_per_sample, pic.bit_depth, pic.num_planes,
                        pic.strides[0], pic.strides[1], pic.strides[2],
                        pic.b_decode_error);
                if (pic.b_decode_error) errors++;
            }
            fn_unref(dec, &pic);
            continue;
        }
        if (r == DAVS2_DEFAULT) { idle++; continue; }   /* keep polling */
        fprintf(stderr, "[probe] recv_frame -> %d (%s)\n", r,
                r == DAVS2_ERROR ? "DAVS2_ERROR" :
                r == DAVS2_END ? "DAVS2_END" : "?");
        break;
    }

    printf("SUMMARY send=%d frames=%d seqs=%d max_bit_depth=%d err_frames=%d\n",
           r_send, frames, seqs, max_bd, errors);
    (void)0;
    printf("VERDICT %s\n",
           (frames > 0 && max_bd == 10) ? "10BIT_OK"
           : (frames > 0 && max_bd == 8) ? "OUTPUT_8BIT"
           : "NO_FRAMES");

    fn_close(dec);
    free(buf);
    return 0;
}
