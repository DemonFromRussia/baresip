/**
 * @file fakevideo.c Fake video source and video display
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 */
#define _DEFAULT_SOURCE 1
#define _BSD_SOURCE 1
#ifdef HAVE_UNISTD_H
#include <unistd.h>
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

// Function to open an RTMP stream
static int open_rtmp_stream(AVFormatContext **out_ctx, const char *output_url, AVCodecContext **out_codec_ctx, int width, int height, int fps) {
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *codec_ctx = NULL;
    AVStream *video_stream = NULL;
    AVCodec *codec = NULL;
    int ret;

    // Allocate the format context for output
    avformat_alloc_output_context2(&fmt_ctx, NULL, "flv", output_url);
    if (!fmt_ctx) {
        warning("Could not create output context\n");
        return -1;
    }

    // Find the H.264 encoder
    codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        warning("Codec not found\n");
        return -1;
    }

    // Add a new stream to the format context
    video_stream = avformat_new_stream(fmt_ctx, codec);
    if (!video_stream) {
        warning("Could not allocate stream\n");
        return -1;
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        warning("Could not allocate codec context\n");
        return -1;
    }

    // Set up codec parameters
    codec_ctx->width = width;
    codec_ctx->height = height;
    codec_ctx->time_base = (AVRational){1, fps};
    codec_ctx->framerate = (AVRational){fps, 1};
    codec_ctx->gop_size = 12; // Set GOP size
    codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    if (fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // Open the codec
    ret = avcodec_open2(codec_ctx, codec, NULL);
    if (ret < 0) {
        warning("Could not open codec\n");
        return -1;
    }

    // Copy the codec parameters to the stream
    ret = avcodec_parameters_from_context(video_stream->codecpar, codec_ctx);
    if (ret < 0) {
        warning("Could not copy codec parameters\n");
        return -1;
    }

    video_stream->time_base = codec_ctx->time_base;

    // Open the output file (RTMP)
    if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&fmt_ctx->pb, output_url, AVIO_FLAG_WRITE);
        if (ret < 0) {
            warning("Could not open output URL\n");
            return -1;
        }
    }

    // Write the stream header
    ret = avformat_write_header(fmt_ctx, NULL);
    if (ret < 0) {
        warning("Error occurred when opening output URL\n");
        return -1;
    }

    *out_ctx = fmt_ctx;
    *out_codec_ctx = codec_ctx;

    return 0;
}

// Function to encode and send a frame
static int send_frame(AVFormatContext *fmt_ctx, AVCodecContext *codec_ctx, AVFrame *frame) {
    AVPacket pkt = {0};
    int ret;

    // Send the frame to the encoder
    ret = avcodec_send_frame(codec_ctx, frame);
    if (ret < 0) {
        warning("Error sending a frame to the encoder\n");
        return -1;
    }

    // Receive the encoded packet
    ret = avcodec_receive_packet(codec_ctx, &pkt);
    if (ret < 0) {
        warning("Error receiving packet from encoder\n");
        return -1;
    }

    // Rescale output packet timestamp values from codec to stream timebase
    av_packet_rescale_ts(&pkt, codec_ctx->time_base, fmt_ctx->streams[0]->time_base);
    pkt.stream_index = 0;

    // Write the packet
    ret = av_interleaved_write_frame(fmt_ctx, &pkt);
    if (ret < 0) {
        warning("Error writing frame to output\n");
        return -1;
    }

    av_packet_unref(&pkt);
    return 0;
}

static const char *output_url = "udp://127.0.0.1:1935";
static int width = 1280, height = 720, fps = 30;
static AVFormatContext *fmt_ctx = NULL;
static AVCodecContext *codec_ctx = NULL;
static int ret;
static bool isStreaming = false;

static int decode(struct vidfilt_dec_st *st, struct vidframe *frame,
			uint64_t *timestamp)
{
	if (!isStreaming) {
		info("restream: start streaming at %s\n", output_url);
		// Open the RTMP stream
		width = frame->size.w;
		height = frame->size.h;
		ret = open_rtmp_stream(&fmt_ctx, output_url, &codec_ctx, width, height, fps);
		if (ret < 0) {
			warning("Failed to open RTMP stream\n");
			return -1;
		}
		isStreaming = true;
	}

	// Allocate  YUV frame
    AVFrame *yuv_frame = av_frame_alloc();
    yuv_frame->format = AV_PIX_FMT_YUV420P;
    yuv_frame->width = width;
    yuv_frame->height = height;

    // Allocate buffers for YUV frame
    av_frame_get_buffer(yuv_frame, 32);

	// Here you should receive your raw frames and convert them to YUV420p
    // For the sake of this example, we will just fill the frame with black pixels
    for (int i = 0; i < yuv_frame->height; i++) {
        for (int j = 0; j < yuv_frame->width; j++) {
            yuv_frame->data[0][i * yuv_frame->linesize[0] + j] = frame->data[0][i * frame->linesize[0] + j];   // Y
        }
    }
    for (int i = 0; i < yuv_frame->height / 2; i++) {
        for (int j = 0; j < yuv_frame->width / 2; j++) {
            yuv_frame->data[1][i * yuv_frame->linesize[1] + j] = frame->data[1][i * frame->linesize[1] + j]; // U
            yuv_frame->data[2][i * yuv_frame->linesize[2] + j] = frame->data[1][i * frame->linesize[2] + j];; // V
        }
    }

    // Send the converted YUV frame
    yuv_frame->pts = 0;
    ret = send_frame(fmt_ctx, codec_ctx, yuv_frame);
    if (ret < 0) {
        warning("Failed to send frame\n");
        return -1;
    }

	return 0;
}

static struct vidfilt restream = {
	.name = "restream",
	.dech = decode,
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

	  // Flush the encoder
    send_frame(fmt_ctx, codec_ctx, NULL);

    // Write the trailer
    av_write_trailer(fmt_ctx);

    // Clean up
    avcodec_free_context(&codec_ctx);
    avformat_free_context(fmt_ctx);
    avformat_network_deinit();

	isStreaming = false;

	return 0;
}

EXPORT_SYM const struct mod_export DECL_EXPORTS(snapshot) = {
	"snapshot",
	"vidfilt",
	module_init,
	module_close
};