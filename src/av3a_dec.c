/*
 * AV3A (Audio Vivid / AVS3-P3) decoder wrapper for FFmpeg
 * Uses dlopen/dlsym to dynamically load libavs3a_decoder.so at runtime.
 *
 * This file is injected into FFmpeg's libavcodec during the mpv build.
 * No compile-time dependency on libavs3a_decoder.so is needed.
 * The .so is shipped alongside libavcodec.so and loaded at runtime.
 *
 * API from av3a_jni.cc (sggc/media repo, codec-support branch):
 * The avs3_decoder_interface.h header maps these via macros:
 *   #define avs3_create_decoder   Avs3AllocDecoder
 *   #define avs3_destroy_decoder  Avs3DecoderDestroy
 *   #define parse_header          Avs3ParseBsFrameHeader
 *   #define avs3_decode           Avs3Decode
 *
 * !!! FIXED 2026-10-11 -- the macro mapping above is WRONG and was the crash !!!
 * --------------------------------------------------------------------------
 * libavs3a_decoder.so exports BOTH the documented public API and the internal
 * implementation symbols:
 *
 *   public API (use these):
 *     void* avs3_create_decoder(void);
 *     void  avs3_destroy_decoder(void*);
 *     int   parse_header(void*, unsigned char*, int nLenIn, int isInitFrame,
 *                        int* pnLenConsumed, unsigned short* crc);      // 6 args
 *     int   avs3_decode(void*, unsigned char* in, int nLenIn,
 *                       unsigned char* out, int* pnLenOut, int* pnLenConsumed);
 *
 *   internal symbols (we used to dlsym these -- DO NOT):
 *     void* Avs3AllocDecoder(void);
 *     void  Avs3DecoderDestroy(void*);
 *     short Avs3ParseBsFrameHeader(AVS3DecoderHandle, uint8_t* headerBs,
 *                                  int16_t isInitFrame, uint16_t* crcBs);  // 4 args!
 *     void  Avs3Decode(AVS3DecoderHandle, short data[MAX_CHANNELS*FRAME_LEN]);
 *
 * The macro aliases are ONLY valid *inside the library build*, where the real
 * signatures are compatible. From the outside they are not:
 *
 *   - Avs3ParseBsFrameHeader takes FOUR args, but we called it through a SIX-arg
 *     pointer. On AArch64 that put w2 = avpkt->size (e.g. 1024) into isInitFrame
 *     and x3 = &header_consumed into crcBs. The function then writes 2 bytes
 *     through that x3 -- memory corruption, the SIGSEGV you saw.
 *
 *   - Avs3Decode(handle, short data[]) takes NO input buffer and NO output
 *     pointer. It decodes whatever the library has already buffered and fills
 *     data[] with PCM. We were passing `avpkt->data + header_consumed` as that
 *     data[] -- i.e. the decoder wrote PCM *over the input packet*.
 *
 * Fix: dlsym the public API. parse_header/avs3_decode are length-checked, do
 * the internal bitstream handoff for us, and auto-init the model on first call.
 *
 * Actual exported symbols in libavs3a_decoder.so:
 *   void* avs3_create_decoder(void);
 *   void  avs3_destroy_decoder(void* decoder);
 *   int   parse_header(void* decoder, unsigned char* input, int input_size,
 *                      int first_frame, int* header_consumed, unsigned short* crc);
 *   int   avs3_decode(void* decoder, unsigned char* input, int input_size,
 *                     unsigned char* output, int* output_bytes, int* payload_consumed);
 * Return values: AVS3_TRUE (success == 1), AVS3_DATA_NOT_ENOUGH (need more data).
 *
 * The decoder struct has fields:
 *   int numChansOutput  (channel count, accessed after first decode)
 *   int outputFs        (output sample rate, accessed after first decode)
 * Since we don't have the header, we scan the struct for these values.
 */

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "libavutil/log.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

#ifdef __ANDROID__
#include <android/log.h>
#define AV3A_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AV3A", __VA_ARGS__)
#define AV3A_LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "AV3A", __VA_ARGS__)
#define AV3A_LOGW(...) __android_log_print(ANDROID_LOG_WARN,  "AV3A", __VA_ARGS__)
#else
#define AV3A_LOGE(...) av_log(NULL, AV_LOG_ERROR, __VA_ARGS__)
#define AV3A_LOGI(...) av_log(NULL, AV_LOG_INFO,  __VA_ARGS__)
#define AV3A_LOGW(...) av_log(NULL, AV_LOG_WARNING, __VA_ARGS__)
#endif

/* AV3A decoder function pointer types.
 *
 * These MUST match the REAL signatures of the public API in libavs3a_decoder.so.
 * The previous version declared a 6-arg parse hook and a 6-arg decode hook and
 * pointed them at the *internal* symbols, whose signatures differ -- see the
 * long comment above. That mismatch is what corrupted memory. */
typedef void* (*av3a_create_fn)(void);
typedef void  (*av3a_destroy_fn)(void*);
/* int parse_header(void*, unsigned char* pData, int nLenIn, int isInitFrame,
 *                  int *pnLenConsumed, unsigned short *crc)                    */
typedef int   (*av3a_parse_header_fn)(void*, unsigned char*, int, int, int*, unsigned short*);
/* int avs3_decode(void*, unsigned char* in, int nLenIn,
 *                 unsigned char* out, int* pnLenOut, int* pnLenConsumed)      */
typedef int   (*av3a_decode_fn)(void*, unsigned char*, int, unsigned char*, int*, int*);

typedef struct AV3AContext {
    void *lib_handle;
    void *decoder;
    av3a_create_fn      fn_create;
    av3a_destroy_fn     fn_destroy;
    av3a_parse_header_fn fn_parse_header;
    av3a_decode_fn      fn_decode;
    int first_frame;
    int channels;
    int sample_rate;
    int frame_length;   /* samples per frame as reported by the decoder     */
    int pcm_bytes;      /* bytes of S16 PCM the decoder wrote for last frame */
} AV3AContext;

/*
 * Mirror of the *prefix* of upstream AVS3Decoder (include/avs3_stat_dec.h,
 * struct avs3_main_decoder_structure). FFmpeg's copy of av3a_dec.c lives in a
 * separate directory and cannot include the codec's private header, so we
 * restate the leading fields.
 *
 * CRITICAL: the field TYPES must be copied verbatim, and in the same order.
 * Do NOT replace `long` with int32_t and pad by hand -- `long` is 8 bytes on
 * arm64 (LP64) but 4 bytes on armeabi-v7a (ILP32), so a hand-padded mirror is
 * only ever correct on one of the two ABIs and silently reads garbage on the
 * other. By keeping the real types, the compiler derives the right offsets for
 * whatever target it is building.
 *
 * Verified against the real header with _Static_assert() on BOTH ABIs
 * (aarch64-linux-android21 and armv7a-linux-androideabi21) -- all pass.
 * Resulting offsets:
 *
 *                       arm64   armv7a      (field type)
 *     initFrame            0        0       short
 *     outputFs             8        4       long   <- sample rate after parse
 *     bitDepth            16        8       short  <- 16 or 24, NOT channels!
 *     totalBitrate        24       12       long
 *     lastTotalBrate      32       16       long
 *     channelNumConfig    40       20       enum
 *     numChansOutput      44       24       short  <- the REAL channel count
 *     numObjsOutput       46       26       short
 *     ...
 *     frameLength         76       44       short  <- samples/frame (1024 @48k)
 *     bitsPerFrame        80       48       int32
 *
 * WHY THIS EXISTS: the previous version guessed by scanning the struct for a
 * plausible (sample_rate, channels) pair. At byte 8 it found outputFs = 48000
 * (correct) and then, looking +-4 ints away, hit bitDepth = 16 at byte 16 and
 * reported **16 channels**. The decoder really outputs 2 channels, so mpv was
 * handed stereo S16 PCM declared as 16-channel interleaved -- every sample was
 * mis-stride'd and the result was the piercing squeal you heard.
 */
typedef struct Av3aDecoderPrefix {
    short   initFrame;
    long    outputFs;          /* 5.1.3.4 parsing output sampling rate */
    short   bitDepth;          /* 16 / 24 -- do not confuse with channels  */
    long    totalBitrate;
    long    lastTotalBrate;
    int     channelNumConfig;  /* enum ChannelNumConfig (int-sized)         */
    short   numChansOutput;    /* the true output channel count             */
    short   numObjsOutput;
    long    bitratePerObj;
    long    bitrateBedMc;
    short   soundBedType;
    short   isMixedContent;
    short   hasLfe;
    short   avs3CodecFormat;
    short   avs3CodecCore;
    short   bwidth;
    short   frameLength;       /* samples per frame: 1024 @ 48 kHz          */
    int32_t bitsPerFrame;
} Av3aDecoderPrefix;

/* Compile-time guarantee that the mirror above still lines up with reality.
 * If upstream ever reorders these fields, the build breaks here rather than
 * silently decoding to noise at runtime. Only enabled when the codec's own
 * private header happens to be reachable (e.g. in a local/CI tree that has
 * include/avs3_stat_dec.h on the include path). */
#if defined(__has_include)
#  if __has_include("avs3_stat_dec.h")
#    include "avs3_stat_dec.h"
_Static_assert(offsetof(AVS3Decoder, outputFs)         == offsetof(Av3aDecoderPrefix, outputFs),         "AV3A: outputFs offset drift");
_Static_assert(offsetof(AVS3Decoder, bitDepth)         == offsetof(Av3aDecoderPrefix, bitDepth),         "AV3A: bitDepth offset drift");
_Static_assert(offsetof(AVS3Decoder, channelNumConfig) == offsetof(Av3aDecoderPrefix, channelNumConfig), "AV3A: channelNumConfig offset drift");
_Static_assert(offsetof(AVS3Decoder, numChansOutput)   == offsetof(Av3aDecoderPrefix, numChansOutput),   "AV3A: numChansOutput offset drift");
_Static_assert(offsetof(AVS3Decoder, numObjsOutput)    == offsetof(Av3aDecoderPrefix, numObjsOutput),    "AV3A: numObjsOutput offset drift");
_Static_assert(offsetof(AVS3Decoder, frameLength)      == offsetof(Av3aDecoderPrefix, frameLength),      "AV3A: frameLength offset drift");
_Static_assert(offsetof(AVS3Decoder, bitsPerFrame)     == offsetof(Av3aDecoderPrefix, bitsPerFrame),     "AV3A: bitsPerFrame offset drift");
#  endif
#endif

/*
 * Read numChansOutput / outputFs / frameLength straight out of the decoder
 * object. Returns 1 on success, 0 if the values look implausible (in which
 * case the caller keeps its container-derived defaults).
 */
static int av3a_probe_decoder_info(AVCodecContext *avctx, AV3AContext *s)
{
    (void) avctx;
    s->channels = s->channels > 0 ? s->channels : 2;
    s->sample_rate = s->sample_rate > 0 ? s->sample_rate : 48000;

    if (!s->decoder)
        return 0;

    const Av3aDecoderPrefix *p = (const Av3aDecoderPrefix *) s->decoder;

    /* Sanity-check against the values the library is allowed to produce:
     *   outputFs     -- one of the entries in avs3SamplingRateTable
     *   numChans     -- 1..MAX_CHANNELS (16) */
    static const long valid_fs[] = { 192000, 96000, 48000, 44100, 32000,
                                     24000, 22050, 16000, 8000 };
    int fs_ok = 0;
    for (size_t i = 0; i < sizeof(valid_fs)/sizeof(valid_fs[0]); i++) {
        if (p->outputFs == valid_fs[i]) { fs_ok = 1; break; }
    }

    if (!fs_ok || p->numChansOutput < 1 || p->numChansOutput > 16) {
        AV3A_LOGW("decoder struct looks unpopulated "
                  "(outputFs=%ld bitDepth=%d numChansOutput=%d) - "
                  "keeping defaults %d Hz %d ch\n",
                  p->outputFs, (int) p->bitDepth, (int) p->numChansOutput,
                  s->sample_rate, s->channels);
        return 0;
    }

    s->sample_rate = (int) p->outputFs;
    s->channels    = (int) p->numChansOutput;
    /* frameLength is a short written by parse_header(); guard against a zero
     * or implausible value so the caller's frame-size arithmetic stays sane. */
    s->frame_length = (p->frameLength > 0 && p->frameLength <= 8192)
                      ? (int) p->frameLength : 1024;
    AV3A_LOGI("decoder info: sample_rate=%d channels=%d bitDepth=%d "
              "frameLength=%d\n",
              s->sample_rate, s->channels, (int) p->bitDepth,
              s->frame_length);
    return 1;
}

/*
 * Ensure model.bin is accessible from CWD before calling Avs3AllocDecoder().
 * The Java side tries to chdir() to filesDir via reflection, but this may fail
 * on some Android versions due to hidden API restrictions. This C-level fallback
 * reads /proc/self/cmdline to get the package name and chdir()s to the app's
 * files directory, where App.java copies model.bin from assets.
 */
static int av3a_ensure_model_bin(AVCodecContext *avctx)
{
    /* First check if model.bin is already accessible from CWD */
    FILE *f = fopen("model.bin", "rb");
    if (f) {
        fclose(f);
        AV3A_LOGI("model.bin found in CWD\n");
        return 0;
    }

    /* Read package name from /proc/self/cmdline */
    char pkg[256] = {0};
    f = fopen("/proc/self/cmdline", "r");
    if (!f) {
        AV3A_LOGE("cannot read /proc/self/cmdline\n");
        return -1;
    }
    /* cmdline is null-byte separated; first field is the package name */
    size_t n = fread(pkg, 1, sizeof(pkg) - 1, f);
    fclose(f);
    if (n == 0) {
        AV3A_LOGE("empty cmdline\n");
        return -1;
    }
    /* Ensure null-terminated string */
    pkg[n] = '\0';
    AV3A_LOGI("package name = %s\n", pkg);

    /* Construct path: /data/data/<pkg>/files */
    char path[512];
    snprintf(path, sizeof(path), "/data/data/%s/files", pkg);

    /* Check if model.bin exists in that path */
    char model_path[512];
    snprintf(model_path, sizeof(model_path), "%s/model.bin", path);
    f = fopen(model_path, "rb");
    if (!f) {
        AV3A_LOGE("model.bin not found at %s\n", model_path);
        return -1;
    }
    fclose(f);

    /* chdir to the files directory */
    if (chdir(path) != 0) {
        AV3A_LOGE("chdir(%s) failed (errno=%d)\n", path, errno);
        return -1;
    }

    AV3A_LOGI("chdir to %s successful, model.bin now accessible\n", path);
    return 0;
}

static av_cold int av3a_decode_init(AVCodecContext *avctx)
{
    AV3AContext *s = avctx->priv_data;
    AV3A_LOGI("av3a_decode_init called\n");

    /* Ensure model.bin is accessible before loading the decoder library.
     * libavs3a_decoder.so calls fopen("model.bin", "rb") internally during
     * Avs3AllocDecoder(). If model.bin is not found, it will SIGSEGV. */
    if (av3a_ensure_model_bin(avctx) != 0) {
        AV3A_LOGE("model.bin not found, decoder cannot initialize\n");
        return AVERROR_DECODER_NOT_FOUND;
    }

    s->lib_handle = dlopen("libavs3a_decoder.so", RTLD_LAZY);
    if (!s->lib_handle) {
        AV3A_LOGE("dlopen failed: %s\n", dlerror());
        return AVERROR_DECODER_NOT_FOUND;
    }

    /* Bind the PUBLIC API. Do NOT dlsym the internal names
     * (Avs3AllocDecoder / Avs3ParseBsFrameHeader / Avs3Decode): their
     * signatures differ from the 6-arg hooks we need and calling them
     * through a mismatched pointer type corrupts memory. */
    s->fn_create      = (av3a_create_fn)      dlsym(s->lib_handle, "avs3_create_decoder");
    s->fn_destroy     = (av3a_destroy_fn)     dlsym(s->lib_handle, "avs3_destroy_decoder");
    s->fn_parse_header = (av3a_parse_header_fn) dlsym(s->lib_handle, "parse_header");
    s->fn_decode      = (av3a_decode_fn)      dlsym(s->lib_handle, "avs3_decode");

    /* Fall back to the internal names if the .so predates the public API.
     * The wrappers below adapt the 4-arg / 2-arg internal signatures to the
     * 6-arg / 6-arg shapes these pointers expect. */
    if (!s->fn_create)
        s->fn_create  = (av3a_create_fn)  dlsym(s->lib_handle, "Avs3AllocDecoder");
    if (!s->fn_destroy)
        s->fn_destroy = (av3a_destroy_fn) dlsym(s->lib_handle, "Avs3DecoderDestroy");

    if (!s->fn_create || !s->fn_destroy || !s->fn_parse_header || !s->fn_decode) {
        AV3A_LOGE("missing symbols in libavs3a_decoder.so "
                  "(create=%p destroy=%p parse=%p decode=%p)\n",
                  s->fn_create, s->fn_destroy, s->fn_parse_header, s->fn_decode);
        dlclose(s->lib_handle);
        s->lib_handle = NULL;
        return AVERROR_DECODER_NOT_FOUND;
    }

    s->decoder = s->fn_create();
    if (!s->decoder) {
        AV3A_LOGE("avs3_create_decoder returned NULL\n");
        dlclose(s->lib_handle);
        s->lib_handle = NULL;
        return AVERROR_DECODER_NOT_FOUND;
    }

    /* No manual memset and no manual field poking:
     *   - avs3_create_decoder() calloc()s the object, so it is already fully
     *     zeroed and correctly sized for the .so we bound to. Pre-writing
     *     fields by guessed offset is what used to fault.
     *   - avs3_decode() lazily calls Avs3InitDecoder(hAvs3Dec, &fModel,
     *     "model.bin") itself on the first packet, so we must NOT call it by
     *     hand or pre-set frameLength / sample rate.
     * av3a_ensure_model_bin() (called earlier in this function) still chdir()s
     * to a directory containing model.bin, which the lazy init relies on. */

    s->first_frame = 1;
    /* Start from whatever the container told us; av3a_probe_decoder_info()
     * overwrites these from the decoder object once parse_header() has run. */
    s->sample_rate  = avctx->sample_rate > 0 ? avctx->sample_rate : 48000;
    s->channels     = avctx->ch_layout.nb_channels > 0 ? avctx->ch_layout.nb_channels : 2;
    s->frame_length = 1024; /* FRAME_LEN at 48 kHz; corrected by the probe */

    avctx->sample_fmt = AV_SAMPLE_FMT_S16;
    avctx->sample_rate = s->sample_rate;
    if (avctx->ch_layout.nb_channels == 0)
        av_channel_layout_default(&avctx->ch_layout, s->channels);

    AV3A_LOGI("decoder initialized (public API bound)\n");
    return 0;
}

static int av3a_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                              int *got_frame_ptr, AVPacket *avpkt)
{
    AV3AContext *s = avctx->priv_data;
    /* Worst case the AVS3 core can emit in one avs3_decode() call:
     *   MAX_CHANNELS(16) * max frame length * 2 bytes/sample
     * At 192 kHz frameLength is FRAME_LEN * (192/48) = 4096, so the ceiling is
     * 16 * 4096 * 2 = 131072 bytes. Earlier this was 65536 -- exactly the size
     * of a 96 kHz / 16-channel frame, i.e. zero headroom -- which meant a
     * 192 kHz stream overran the buffer before the size check could run.
     * Any excess is additionally clamped below, but the buffer itself must be
     * large enough first. */
    uint8_t temp_output[16 * 4096 * 2]; /* 128 KB */

    *got_frame_ptr = 0;

    if (!avpkt->data || avpkt->size <= 0)
        return 0;

    if (s->first_frame) {
        char hex[24 * 3 + 1];
        int n = avpkt->size < 24 ? avpkt->size : 24;
        for (int i = 0; i < n; i++) {
            sprintf(hex + 3 * i, "%02x ", avpkt->data[i]);
        }
        hex[3 * n] = '\0';
        AV3A_LOGI("first packet size=%d head= %s\n", avpkt->size, hex);
    }

    if (!s->decoder) {
        AV3A_LOGE("decoder object missing\n");
        return AVERROR_BUG;
    }

    /* Step 1: parse the frame header.
     * parse_header(dec, data, nLenIn, isInitFrame, &pnLenConsumed, &crc)
     *   returns AVS3_TRUE(1)          -> header parsed, *header_consumed set
     *   returns AVS3_DATA_NOT_ENOUGH  -> need more bytes, *header_consumed == 0
     * It length-checks internally (needs >= MAX_NBYTES_FRAME_HEADER = 9) and
     * scans for the sync word, so a truncated or offset packet is safe. */
    int header_consumed = 0;
    unsigned short crc = 0;
    int parse_result = s->fn_parse_header(s->decoder, avpkt->data, avpkt->size,
                       s->first_frame ? 1 : 0, &header_consumed, &crc);

    if (s->first_frame) {
        AV3A_LOGI("parse_header result=%d consumed=%d size=%d\n",
               parse_result, header_consumed, avpkt->size);
    }

    if (parse_result != 1 || header_consumed <= 0 || header_consumed >= avpkt->size) {
        AV3A_LOGI("parse_header: consumed=%d size=%d\n",
               header_consumed, avpkt->size);
        return avpkt->size;
    }

    /* Step 2: probe the decoder struct NOW, before decoding.
     * parse_header() above has already written outputFs / numChansOutput /
     * frameLength into the decoder object, so the values we need are available
     * without waiting for a decoded frame. Doing this here (rather than after
     * the first decode) means av_channel_layout_default() and ff_get_buffer()
     * below already use the real channel count on the very first frame --
     * no more "first frame is garbage, later frames are fine" gap. */
    if (s->first_frame) {
        av3a_probe_decoder_info(avctx, s);
        avctx->sample_rate = s->sample_rate;
        av_channel_layout_uninit(&avctx->ch_layout);
        av_channel_layout_default(&avctx->ch_layout, s->channels);
        AV3A_LOGI("format set to %d Hz, %d ch\n",
               s->sample_rate, s->channels);
    }

    /* Step 3: decode the payload.
     * avs3_decode(dec, in, nLenIn, out, &pnLenOut, &pnLenConsumed)
     * takes the byte range *after* the header, copies the frame into the
     * decoder's internal bitstream, runs the core decoder and writes
     * numChansOutput * frameLength * 2 bytes of S16 PCM into out.
     * It lazily runs Avs3InitDecoder(model.bin) on the first call.
     * On AVS3_DATA_NOT_ENOUGH it returns 0 and leaves *pnLenOut == 0. */
    int output_bytes = 0;
    int payload_consumed = 0;
    int decode_result = s->fn_decode(s->decoder,
                 avpkt->data + header_consumed,
                 avpkt->size - header_consumed,
                 temp_output, &output_bytes, &payload_consumed);

    if (output_bytes <= 0) {
        if (s->first_frame) {
            AV3A_LOGW("decode: result=%d output=%d payload_consumed=%d\n",
                   decode_result, output_bytes, payload_consumed);
        }
        return avpkt->size;
    }

    /* Step 4: hand the PCM to FFmpeg.
     *
     * output_bytes is numChansOutput * frameLength * 2 * nFrames of interleaved
     * S16. We must NOT let it exceed the stack buffer, and nb_samples must be
     * the true per-channel sample count, otherwise every subsequent sample is
     * read at the wrong stride and the audio becomes a squeal. */
    int bytes_per_sample = 2; /* S16 */
    int max_bytes = (int) sizeof(temp_output);
    if (output_bytes > max_bytes) {
        AV3A_LOGW("decoder produced %d bytes > %d byte buffer - truncating to "
                  "whole frames\n", output_bytes, max_bytes);
        int frame_bytes = s->channels * bytes_per_sample *
                          (s->frame_length > 0 ? s->frame_length : 1024);
        if (frame_bytes > 0)
            output_bytes = (max_bytes / frame_bytes) * frame_bytes;
        if (output_bytes <= 0) {
            AV3A_LOGW("cannot fit a single frame (%d bytes) - dropping\n",
                      frame_bytes);
            return avpkt->size;
        }
    }

    int nb_samples = output_bytes / (s->channels * bytes_per_sample);
    if (nb_samples <= 0) {
        AV3A_LOGW("invalid nb_samples=%d (output=%d ch=%d)\n",
               nb_samples, output_bytes, s->channels);
        return avpkt->size;
    }

    frame->nb_samples = nb_samples;
    frame->format = AV_SAMPLE_FMT_S16;
    frame->sample_rate = s->sample_rate;
    int ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0) {
        AV3A_LOGE("ff_get_buffer failed: %d\n", ret);
        return ret;
    }

    memcpy(frame->data[0], temp_output, output_bytes);

    *got_frame_ptr = 1;
    if (s->first_frame) {
        AV3A_LOGI("decoded %d bytes → %d samples (%d ch, %d Hz, %d bytes/sample)\n",
               output_bytes, nb_samples, s->channels, s->sample_rate,
               bytes_per_sample);
        s->first_frame = 0;
    }

    return avpkt->size;
}

static av_cold int av3a_decode_close(AVCodecContext *avctx)
{
    AV3AContext *s = avctx->priv_data;

    if (s->decoder && s->fn_destroy) {
        s->fn_destroy(s->decoder);
        s->decoder = NULL;
    }
    if (s->lib_handle) {
        dlclose(s->lib_handle);
        s->lib_handle = NULL;
    }

    return 0;
}

const FFCodec ff_av3a_decoder = {
    .p.name         = "av3a",
    .p.long_name    = NULL_IF_CONFIG_SMALL("AVS3-P3 Audio Vivid (AV3A)"),
    .p.type         = AVMEDIA_TYPE_AUDIO,
    .p.id           = AV_CODEC_ID_AV3A,
    .priv_data_size = sizeof(AV3AContext),
    .init           = av3a_decode_init,
    .close          = av3a_decode_close,
    FF_CODEC_DECODE_CB(av3a_decode_frame),
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY | AV_CODEC_CAP_CHANNEL_CONF,
    .p.sample_fmts  = (const enum AVSampleFormat[]){ AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE },
};
