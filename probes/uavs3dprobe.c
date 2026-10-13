/* uavs3d capability probe: drives the *shipped* libuavs3d.so on device to
 * measure whether it can decode a 10-bit AVS3 stream.
 *
 * Input: an AVS3 elementary stream (.avs3) — either raw NALs with start codes,
 *        or a TS/PES wrapper (we strip PES just like AV3A).
 *
 * Output per NAL/frame: uavs3d_decode return code, seqhdr->sample_precision,
 *        and the output frame bit_depth / num_plane / stride.
 * A ret of ERR_BAD_BITDEPTH(-3) on a 10-bit stream == 8bit-only build.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct {
    const char* input_file;
    const char* output_file;
    int frm_threads;
    int dec_frames;
    int log_level;
    int check_md5;
} uavs3d_cfg_t;

#define RET_OK            0
#define RET_DELAYED       1
#define RET_SEQ_END       2
#define ERR_OUT_OF_MEMORY (-1)
#define ERR_UNEXPECTED    (-2)
#define ERR_BAD_BITDEPTH  (-3)
#define ERR_UNKNOWN_NAL   (-4)
#define ERR_LOSS_REF_FRAME (-5)
#define ERR_PIC_HEADER_UNREADY (-6)
#define ERR_SEQ_HEADER_UNREADY (-7)
#define ERR_SEQ_MAININFO_CHANGED (-8)
#define ERR_PIC_BUFFER_FULL (-9)
#define ERR_UNKNOWN (-100)

#define FRAME_MAX_PLANES 3
typedef struct {
    int num;
    int active;
    int delta_doi[17];
} com_rpl_t;

typedef struct {
    unsigned char  profile_id;
    unsigned char  level_id;
    unsigned char  progressive_sequence;
    unsigned char  field_coded_sequence;
    unsigned char  chroma_format;
    unsigned char  encoding_precision;
    unsigned char  output_reorder_delay;
    unsigned char  sample_precision;   /* <<-- 3 bits: bit depth minus 8 */
    unsigned char  aspect_ratio;
    unsigned char  frame_rate_code;
    unsigned int   bit_rate_lower;
    unsigned int   bit_rate_upper;
    unsigned char  low_delay;
    unsigned char  temporal_id_enable_flag;
    unsigned int   bbv_buffer_size;
    int            horizontal_size;
    int            vertical_size;
    int            display_horizontal_size;
    int            display_vertical_size;
    unsigned char  log2_max_cu_width_height;
    unsigned char  min_cu_size;
    unsigned char  max_part_ratio_log2;
    unsigned char  max_split_times;
    unsigned char  min_qt_size;
    unsigned char  max_bt_size;
    unsigned char  max_eqt_size;
    unsigned char  max_dt_size;
    int            rpl1_index_exist_flag;
    int            rpl1_same_as_rpl0_flag;
    com_rpl_t      rpls_l0[32];
    com_rpl_t      rpls_l1[32];
    int            rpls_l0_num;
    int            rpls_l1_num;
    int            num_ref_default_active_minus1[2];
    int            max_dpb_size;
    int            ipcm_enable_flag;
    unsigned char  amvr_enable_flag;
    int            umve_enable_flag;
    int            ipf_enable_flag;
    int            emvr_enable_flag;
    unsigned char  affine_enable_flag;
    unsigned char  smvd_enable_flag;
    unsigned char  dt_intra_enable_flag;
    unsigned char  num_of_hmvp_cand;
    unsigned char  tscpm_enable_flag;
    unsigned char  sample_adaptive_offset_enable_flag;
    unsigned char  adaptive_leveling_filter_enable_flag;
    unsigned char  secondary_transform_enable_flag;
    unsigned char  position_based_transform_enable_flag;
    unsigned char  wq_enable;
    unsigned char  seq_wq_mode;
    unsigned char  wq_4x4_matrix[16];
    unsigned char  wq_8x8_matrix[64];
    unsigned char  patch_stable;
    unsigned char  cross_patch_loop_filter;
    unsigned char  patch_ref_colocated;
    unsigned char  patch_uniform;
    unsigned char  patch_width;
    unsigned char  patch_height;
    int            pic_width;
    int            pic_height;
    int            max_cuwh;
    int            log2_max_cuwh;
    int            pic_width_in_lcu;
    int            pic_height_in_lcu;
    int            f_lcu;
    int            pic_width_in_scu;
    int            pic_height_in_scu;
    int            i_scu;
    int            f_scu;
    int            a_scu;
    int            bit_depth_internal;
    int            bit_depth_input;
    int            bit_depth_2_qp_offset;
    int            patch_columns;
    int            patch_rows;
    int            patch_column_width[64];
    int            patch_row_height[128];
    unsigned char *alf_idx_map;
    unsigned char colour_description;
    unsigned char colour_primaries;
    unsigned char transfer_characteristics;
    unsigned char matrix_coefficients;
} com_seqh_t;

typedef struct uavs3d_io_frm_t {
    void  * priv;
    int     got_pic;
    int     num_plane;
    int     bit_depth;
    int     width[FRAME_MAX_PLANES];
    int     height[FRAME_MAX_PLANES];
    int     stride[FRAME_MAX_PLANES];
    void  * buffer[FRAME_MAX_PLANES];
    long long   ptr;
    long long   pts;
    long long   dtr;
    long long   dts;
    int         type;
    int         refpic_num[2];
    long long   refpic[2][16];
    long long   pkt_pos;
    int         pkt_size;
    unsigned char *bs;
    int bs_len;
    int nal_type;
    com_seqh_t *seqhdr;
} uavs3d_io_frm_t;

typedef void (*uavs3d_cb_t)(uavs3d_io_frm_t *frm);
typedef void* (*uavs3d_create_t)(uavs3d_cfg_t*, uavs3d_cb_t, int*);
typedef void  (*uavs3d_delete_t)(void*);
typedef int   (*uavs3d_flush_t)(void*, uavs3d_io_frm_t*);
typedef int   (*uavs3d_decode_t)(void*, uavs3d_io_frm_t*);

/* ---- minimal dlopen shim (link with -ldl) ---- */
#include <dlfcn.h>

static uavs3d_cfg_t cfg;
static uavs3d_create_t   fn_create;
static uavs3d_delete_t   fn_delete;
static uavs3d_flush_t    fn_flush;
static uavs3d_decode_t   fn_decode;

static int g_frames = 0;
static int g_errbitdepth = 0;
static int g_okframes = 0;
static int g_seqs = 0;
static com_seqh_t g_last_seq;
static int g_seq_seen = 0;
static int g_max_out_bd = 0;

static const char *retname(int r) {
    switch (r) {
    case RET_OK: return "RET_OK";
    case RET_DELAYED: return "RET_DELAYED";
    case RET_SEQ_END: return "RET_SEQ_END";
    case ERR_OUT_OF_MEMORY: return "ERR_OUT_OF_MEMORY";
    case ERR_UNEXPECTED: return "ERR_UNEXPECTED";
    case ERR_BAD_BITDEPTH: return "ERR_BAD_BITDEPTH";
    case ERR_UNKNOWN_NAL: return "ERR_UNKNOWN_NAL";
    case ERR_LOSS_REF_FRAME: return "ERR_LOSS_REF_FRAME";
    case ERR_PIC_HEADER_UNREADY: return "ERR_PIC_HEADER_UNREADY";
    case ERR_SEQ_HEADER_UNREADY: return "ERR_SEQ_HEADER_UNREADY";
    case ERR_SEQ_MAININFO_CHANGED: return "ERR_SEQ_MAININFO_CHANGED";
    case ERR_PIC_BUFFER_FULL: return "ERR_PIC_BUFFER_FULL";
    case ERR_UNKNOWN: return "ERR_UNKNOWN";
    default: return "OTHER";
    }
}

static void out_cb(uavs3d_io_frm_t *frm)
{
    if (!frm) return;
    if (frm->got_pic) {
        g_okframes++;
        if (frm->bit_depth > g_max_out_bd) g_max_out_bd = frm->bit_depth;
    }
}

/* find next AVS3 start code; returns offset of the 00 00 01, sets *sc_len */
static long find_sc(const uint8_t *b, long len, long from, int *sc_len)
{
    for (long i = from; i + 3 <= len; i++) {
        if (b[i] == 0 && b[i+1] == 0 && b[i+2] == 1) {
            *sc_len = (i > 0 && b[i-1] == 0) ? 4 : 3;
            return (i > 0 && b[i-1] == 0) ? i - 1 : i;
        }
    }
    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <libuavs3d.so> <in.avs3> [max_nal]\n", argv[0]);
        return 2;
    }
    const char *libpath = argv[1];
    const char *inpath  = argv[2];
    int max_nal = (argc >= 4) ? atoi(argv[3]) : 200;

    void *h = dlopen(libpath, RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }
    fn_create = (uavs3d_create_t)dlsym(h, "uavs3d_create");
    fn_delete = (uavs3d_delete_t)dlsym(h, "uavs3d_delete");
    fn_flush  = (uavs3d_flush_t )dlsym(h, "uavs3d_flush");
    fn_decode = (uavs3d_decode_t)dlsym(h, "uavs3d_decode");
    if (!fn_create || !fn_decode || !fn_delete || !fn_flush) {
        fprintf(stderr, "missing symbols create=%p decode=%p delete=%p flush=%p\n",
                (void*)fn_create, (void*)fn_decode, (void*)fn_delete, (void*)fn_flush);
        return 1;
    }

    FILE *f = fopen(inpath, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", inpath); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t*)malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    memset(&cfg, 0, sizeof(cfg));
    cfg.input_file = inpath;
    cfg.frm_threads = 1;
    cfg.log_level = 0;

    int err = 0;
    void *dec = fn_create(&cfg, out_cb, &err);
    if (!dec) { fprintf(stderr, "uavs3d_create failed err=%d\n", err); return 1; }

    /* Walk NAL by NAL. AVS3 NAL unit header: forbidden_zero(1)+nal_ref_idc(2)+
       nal_unit_type(5) + ... so type = (byte & 0x1F) >> 0 after the first byte
       uses 2 bytes header (start code excluded). Actually uavs3d expects the
       full NAL *including* the 2-byte nal header and start codes. */
    long pos = 0;
    int nal_cnt = 0;
    while (pos < n && nal_cnt < max_nal) {
        int sc_len = 0;
        long sc = find_sc(buf, n, pos, &sc_len);
        if (sc < 0) break;
        long data_start = sc + sc_len;
        /* next start code after data_start */
        int sc2 = 0;
        long next = find_sc(buf, n, data_start, &sc2);
        long data_end = (next < 0) ? n : next;
        /* trim leading zeros of a 4-byte start code chain */
        if (next >= 0 && sc2 == 4) data_end = next + 1; /* include the extra 0 */
        long len = data_end - sc;
        if (len <= 0) { pos = data_start; continue; }

        uavs3d_io_frm_t frm;
        memset(&frm, 0, sizeof(frm));
        frm.bs = buf + sc;
        frm.bs_len = (int)len;
        frm.pts = nal_cnt;
        frm.type = 0;

        int r = fn_decode(dec, &frm);
        nal_cnt++;

        if (r == ERR_BAD_BITDEPTH) g_errbitdepth++;
        if (r == RET_OK) g_frames++;

        /* report first seq header and every interesting transition */
        if (frm.seqhdr && (g_seq_seen == 0 ||
             frm.seqhdr->sample_precision != g_last_seq.sample_precision ||
             frm.seqhdr->horizontal_size != g_last_seq.horizontal_size ||
             frm.seqhdr->vertical_size != g_last_seq.vertical_size)) {
            if (g_seq_seen == 0 || frm.seqhdr->sample_precision != g_last_seq.sample_precision) {
                g_seqs++;
                g_last_seq = *frm.seqhdr;
                g_seq_seen = 1;
                fprintf(stderr, "[seq#%d] %dx%d sample_precision=%d (=> %d-bit input) "
                        "chroma=%d profile=%d level=%d\n",
                        g_seqs,
                        frm.seqhdr->horizontal_size, frm.seqhdr->vertical_size,
                        frm.seqhdr->sample_precision, 8 + frm.seqhdr->sample_precision,
                        frm.seqhdr->chroma_format, frm.seqhdr->profile_id, frm.seqhdr->level_id);
            }
        }

        if (nal_cnt <= 8 || r < 0) {
            fprintf(stderr, "[nal %3d] len=%-7ld ret=%-24s got_pic=%d out_bd=%d plane=%d "
                    "stride=%d/%d/%d\n", nal_cnt, len, retname(r),
                    frm.got_pic, frm.bit_depth, frm.num_plane,
                    frm.stride[0], frm.stride[1], frm.stride[2]);
        }
        /* ERR_UNKNOWN_NAL(-4) 不致命：AVS3 的 0xB2 等用户数据 NAL 会让解码器
         * 返回它，但后续 picture NAL 仍可正常解。uavs3d 1.2.x 新旧版均如此，
         * 所以这里必须跳过而不是中止，否则探针永远测不到后面的帧。
         * 其余负返回码仍视为致命。 */
        if (r < 0 && r != ERR_UNKNOWN_NAL) {
            fprintf(stderr, "  --> fatal, stop (nal %d of %ld bytes)\n", nal_cnt, n);
            break;
        }
        pos = data_end;
    }

    /* drain */
    for (int i = 0; i < 64; i++) {
        uavs3d_io_frm_t frm;
        memset(&frm, 0, sizeof(frm));
        int r = fn_flush(dec, &frm);
        if (r == RET_SEQ_END || r < 0) break;
        if (frm.got_pic) { g_okframes++; if (frm.bit_depth > g_max_out_bd) g_max_out_bd = frm.bit_depth; }
        if (r != RET_OK && r != RET_DELAYED) break;
    }

    printf("SUMMARY nals=%d seqs=%d RET_OK=%d ERR_BAD_BITDEPTH=%d out_frames=%d max_out_bit_depth=%d\n",
           nal_cnt, g_seqs, g_frames, g_errbitdepth, g_okframes, g_max_out_bd);
    printf("VERDICT %s\n",
           g_errbitdepth ? "8BIT_ONLY (10-bit stream rejected: ERR_BAD_BITDEPTH)"
                         : (g_max_out_bd > 8 ? "10BIT_OK" : "8BIT_OR_UNKNOWN"));

    free(buf);
    fn_delete(dec);
    return 0;
}
