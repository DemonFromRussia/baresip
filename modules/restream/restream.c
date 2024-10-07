/**
 * @file fakevideo.c Fake video source and video display
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 */
#define _DEFAULT_SOURCE 1
#define _BSD_SOURCE 1
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#endif
#include <re_atomic.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include <libavutil/pixdesc.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

/**
 * @defgroup snapshot snapshot
 *
 * Take snapshot of the video stream and save it as PNG-files
 *
 *
 * Commands:
 *
 \verbatim
 snapshot           Take video snapshot of both video streams
 snapshot_recv path Take snapshot of receiving video and save it to the path
 snapshot_send path Take snapshot of sending video and save it to the path
 \endverbatim
 */

// // Function to open an RTMP stream
// static int width = 640, height = 480, fps = 25;

struct restream_dec
{
    struct vidfilt_dec_st vf; /**< Inheritance           */
    struct video *vid;
    struct vidfilt_prm *prm;
};

static int open_rtmp_stream(AVFormatContext **out_ctx, const char *output_url, AVCodecContext **out_codec_ctx, int width, int height, int fps)
{
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *codec_ctx = NULL;
    AVStream *video_stream = NULL;
    AVCodec *codec = NULL;
    int ret;

    // Allocate the format context for output
    avformat_alloc_output_context2(&fmt_ctx, NULL, "rtp", output_url);
    if (!fmt_ctx)
    {
        warning("Could not create output context\n");
        return -1;
    }

    // Find the H.264 encoder
    codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec)
    {
        warning("Codec not found\n");
        return -1;
    }

    // Add a new stream to the format context
    video_stream = avformat_new_stream(fmt_ctx, codec);
    if (!video_stream)
    {
        warning("Could not allocate stream\n");
        return -1;
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx)
    {
        warning("Could not allocate codec context\n");
        return -1;
    }

    // Set up codec parameters
    codec_ctx->width = width;
    codec_ctx->height = height;
    codec_ctx->time_base = (AVRational){1, VIDEO_TIMEBASE};
    codec_ctx->framerate = (AVRational){fps, 1};
    codec_ctx->gop_size = fps; // Set GOP size
    codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    if (fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
    {
        codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    codec_ctx->max_b_frames = 0; // не отправлять B фреймы
    // codec_ctx->level = level;               // уровень качества
    codec_ctx->refs = 1;         // количество кадров "ссылок"
    codec_ctx->thread_count = 4; // количество потоков для кодирования. можно увеличить выставив thread_type
    codec_ctx->thread_type = FF_THREAD_SLICE;

    // https://ru.wikipedia.org/wiki/H.264
    av_opt_set(codec_ctx->priv_data, "profile", "baseline", 0); // профиль базовый для мобильных устройств
    av_opt_set(codec_ctx->priv_data, "preset", "fast", 0);      // скорость кодирования. обратна пропорциональна качеству
    av_opt_set(codec_ctx->priv_data, "tune", "zerolatency", 0); // минимальная задержка. обязательно для sip

    // Open the codec
    ret = avcodec_open2(codec_ctx, codec, NULL);
    if (ret < 0)
    {
        warning("Could not open codec\n");
        return -1;
    }

    // Copy the codec parameters to the stream
    ret = avcodec_parameters_from_context(video_stream->codecpar, codec_ctx);
    if (ret < 0)
    {
        warning("Could not copy codec parameters\n");
        return -1;
    }

    video_stream->time_base = codec_ctx->time_base;

    // Open the output file (RTMP)
    if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE))
    {
        ret = avio_open(&fmt_ctx->pb, output_url, AVIO_FLAG_WRITE);
        if (ret < 0)
        {
            warning("Could not open output URL\n");
            return -1;
        }
    }

    // Write the stream header
    ret = avformat_write_header(fmt_ctx, NULL);
    if (ret < 0)
    {
        warning("Error occurred when opening output URL\n");
        return -1;
    }

    *out_ctx = fmt_ctx;
    *out_codec_ctx = codec_ctx;

    return 0;
}

// Function to encode and send a frame
static int encode_and_send_frame(AVCodecContext *codec_ctx, AVFormatContext *fmt_ctx, AVFrame *frame, int frame_number, int fps)
{
    int ret;

    // Calculate the PTS for the frame based on the frame number and time base
    // AVRational time_base = codec_ctx->time_base;
    // frame_number * (time_base.den / time_base.num) / fps;

    // Send frame for encoding
    ret = avcodec_send_frame(codec_ctx, frame);
    if (ret < 0)
    {
        warning(stderr, "Error sending frame to encoder: %s\n", av_err2str(ret));
        return ret;
    }

    // Receive packet from encoder
    AVPacket pkt = {0};
    av_init_packet(&pkt);

    ret = avcodec_receive_packet(codec_ctx, &pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
    {
        return 0; // Not an error, just no more packets to receive right now
    }
    else if (ret < 0)
    {
        warning(stderr, "Error receiving packet from encoder: %s\n", av_err2str(ret));
        return ret;
    }

    // Write the encoded packet to the output format context
    pkt.stream_index = 0; // Make sure the stream index is set correctly
    pkt.pts = frame->pts;
    pkt.dts = pkt.pts;
    ret = av_interleaved_write_frame(fmt_ctx, &pkt);
    if (ret < 0)
    {
        warning(stderr, "Error writing encoded packet: %s\n", av_err2str(ret));
        return ret;
    }

    av_packet_unref(&pkt);
    return 0;
}

// Function to generate and save SDP file
static int write_sdp_file(AVFormatContext *fmt_ctx, const char *sdp_file_path)
{
    char sdp[2048] = {0};
    int ret = av_sdp_create(&fmt_ctx, 1, sdp, sizeof(sdp));
    if (ret < 0)
    {
        warning("Failed to create SDP: %s\n", av_err2str(ret));
        return -1;
    }

    // Write the SDP to a file
    FILE *sdp_file = fopen(sdp_file_path, "w");
    chmod(sdp_file_path, 0777);

    if (!sdp_file)
    {
        warning("Could not open SDP file for writing\n");
        return -1;
    }
    fprintf(sdp_file, "%s", sdp);
    fclose(sdp_file);

    info("SDP file written to %s\n", sdp_file_path);
    return 0;
}

static int delete_sbp_file(const char *sdp_file_path)
{
    int status;
    status = remove(sdp_file_path);

    if (status != 0) {
        warning("Unable to delete SDP file\n");
    }

    return 0;
}

static const char *output_url = "rtp://127.0.0.1:5085";
static const char *sdp_path = "/tmp/stream.sdp";
static AVFormatContext *fmt_ctx = NULL;
static AVCodecContext *codec_ctx = NULL;
static int ret;
static bool isStreaming = false;
static uint64_t *start_timestamp = NULL;
static uint frameNumber = 0;

static int stopStream()
{
    if (!isStreaming)
    {
        return 0;
    }

    info("stopping stream");
    // Flush the encoder
    encode_and_send_frame(fmt_ctx, codec_ctx, NULL, frameNumber, 30);

    // Write the trailer
    av_write_trailer(fmt_ctx);

    // Clean up
    avcodec_free_context(&codec_ctx);
    avformat_free_context(fmt_ctx);
    avformat_network_deinit();

    delete_sbp_file(sdp_path);

    isStreaming = false;
    frameNumber = 0;

    info("restream: stopped streaming at %s\n", output_url);
}

static int startStreamIfNeeded(int width, int height, int fps, uint64_t *new_start_timestamp)
{
    if (isStreaming)
    {
        return 0;
    }

    info(
        "restream: start streaming at %s width %d height %d fps %d\n",
        output_url, width, height, fps);
    // Open the RTMP stream
    ret = open_rtmp_stream(&fmt_ctx, output_url, &codec_ctx, width, height, fps);
    if (ret < 0)
    {
        warning("Failed to open RTMP stream\n");
        return -1;
    }

    // Write the SDP file (e.g., to "stream.sdp")
    write_sdp_file(fmt_ctx, sdp_path);

    isStreaming = true;
    start_timestamp = new_start_timestamp;
    return 0;
}

static void decode_destructor(void *arg)
{
    return;
}

struct vrx
{
    struct video *video;          /**< Parent                    */
    const struct vidcodec *vc;    /**< Current video decoder     */
    struct viddec_state *dec;     /**< Video decoder state       */
    struct vidisp_prm vidisp_prm; /**< Video display parameters  */
    struct vidisp_st *vidisp;     /**< Video display             */
    mtx_t lock;                   /**< Lock for decoder          */
    struct list filtl;            /**< Filters in decoding order */
    struct tmr tmr_picup;         /**< Picture update timer      */
    struct vidsz size;            /**< Incoming video resolution */
    enum vidfmt fmt;              /**< Incoming pixel format     */
    enum vidorient orient;        /**< Display orientation       */
    char module[128];             /**< Display module name       */
    char device[128];             /**< Display device name       */
    int pt_rx;                    /**< Incoming RTP payload type */
    int frames;                   /**< Number of frames received */
    double efps;                  /**< Estimated frame-rate      */
    unsigned n_intra;             /**< Intra-frames decoded      */
    unsigned n_picup;             /**< Picture updates sent      */
};

struct video
{
#ifndef RELEASE
    uint32_t magic;
#endif
    struct config_video cfg; /**< Video configuration                  */
    struct vrx vrx;          /**< Receive/decoder direction            */
};

static int decode_update(struct vidfilt_dec_st **stp, void **ctx,
                         const struct vidfilt *vf, struct vidfilt_prm *prm,
                         const struct video *vid)
{
    stopStream();

    struct restream_dec *st;
    (void)prm;
    (void)vid;

    if (!stp || !ctx || !vf)
        return EINVAL;

    if (*stp)
        return 0;

    st = mem_zalloc(sizeof(*st), decode_destructor);
    if (!st)
        return ENOMEM;

    st->vid = vid;
    st->prm = prm;

    *stp = (struct vidfilt_dec_st *)st;

    return 0;
}

static int decode(struct vidfilt_dec_st *st, struct vidframe *frame,
                  uint64_t *timestamp)
{
    if (!frame)
    {
        debug("restream: no frame\n");
        return 0;
    }

    // struct restream_dec *dec;
    // dec = (struct restream_dec *) st;

    // struct vidsz size = dec->vid->vrx.size;
    // int fps = (int) dec->vid->vrx.efps;

    struct vidsz size;
    size = frame->size;
    int fps;
    fps = 25;
    // if (size.w == 0 || size.h == 0) {
    //     return 0;
    // }

    startStreamIfNeeded(size.w, size.h, fps, timestamp);

    if (!isStreaming)
    {
        return 0;
    }

    // Allocate  YUV frame
    AVFrame *yuv_frame = av_frame_alloc();
    yuv_frame->format = AV_PIX_FMT_YUV420P;
    yuv_frame->width = size.w;
    yuv_frame->height = size.h;

    // yuv_frame->pts = frameNumber;
    // yuv_frame->pts = frameNumber * av_rescale_q(1, codec_ctx->framerate, codec_ctx->time_base);
    // yuv_frame->pts = frameNumber * VIDEO_TIMEBASE * (codec_ctx->time_base.den / codec_ctx->time_base.num);
    // yuv_frame->dts = frame->pts;ы
    // yuv_frame->pts = frameNumber;
    // yuv_frame->pts = *timestamp * fps / VIDEO_TIMEBASE;
    yuv_frame->pts = *timestamp - *start_timestamp;

    debug("Frame: %d, Timestamp: %lld, PTS: %lld\n", frameNumber, *timestamp, yuv_frame->pts);

    // Allocate buffers for YUV frame
    av_frame_get_buffer(yuv_frame, 32);

    // Copy the Y plane
    memcpy(yuv_frame->data[0], frame->data[0], yuv_frame->linesize[0] * yuv_frame->height);

    // Copy the U and V planes
    memcpy(yuv_frame->data[1], frame->data[1], yuv_frame->linesize[1] * yuv_frame->height / 2);
    memcpy(yuv_frame->data[2], frame->data[2], yuv_frame->linesize[2] * yuv_frame->height / 2);

    // Send the converted YUV frame
    ret = encode_and_send_frame(codec_ctx, fmt_ctx, yuv_frame, frameNumber, fps);

    av_frame_unref(yuv_frame);
    av_frame_free(&yuv_frame);

    if (ret < 0)
    {
        warning("Failed to send frame\n");
        return -1;
    }

    frameNumber++;

    return 0;
}

static struct vidfilt restream = {
    .name = "restream",
    .dech = decode,
    .decupdh = decode_update,
};

static int module_init(void)
{
    vidfilt_register(baresip_vidfiltl(), &restream);

    // Initialize FFmpeg
    avformat_network_init();

    return 0;
}

static int module_close(void)
{
    vidfilt_unregister(&restream);

    avformat_network_deinit();

    stopStream();

    return 0;
}

EXPORT_SYM const struct mod_export DECL_EXPORTS(restream) = {
    "restream",
    "vidfilt",
    module_init,
    module_close
};