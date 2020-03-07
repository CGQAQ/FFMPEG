/*
 * Copyright (c) 2001 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

 /**
  * @file
  * audio decoding with libavcodec API example
  *
  * @example decode_audio.c
  */

#pragma warning(disable : 4996)
extern "C"{

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/frame.h>
#include <libavutil/mem.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

}
#define AUDIO_INBUF_SIZE 20480
#define AUDIO_REFILL_THRESH 4096

static int get_format_from_sample_fmt(const char** fmt,
    enum AVSampleFormat sample_fmt)
{
    int i;
    struct sample_fmt_entry {
        enum AVSampleFormat sample_fmt; const char* fmt_be, * fmt_le;
    } sample_fmt_entries[] = {
        { AV_SAMPLE_FMT_U8,  "u8",    "u8"    },
        { AV_SAMPLE_FMT_S16, "s16be", "s16le" },
        { AV_SAMPLE_FMT_S32, "s32be", "s32le" },
        { AV_SAMPLE_FMT_FLT, "f32be", "f32le" },
        { AV_SAMPLE_FMT_DBL, "f64be", "f64le" },
    };
    *fmt = NULL;

    for (i = 0; i < FF_ARRAY_ELEMS(sample_fmt_entries); i++) {
        struct sample_fmt_entry* entry = &sample_fmt_entries[i];
        if (sample_fmt == entry->sample_fmt) {
            *fmt = AV_NE(entry->fmt_be, entry->fmt_le);
            return 0;
        }
    }

    fprintf(stderr,
        "sample format %s is not supported as output format\n",
        av_get_sample_fmt_name(sample_fmt));
    return -1;
}

static void decode(AVCodecContext* dec_ctx, AVPacket* pkt, AVFrame* frame,
    FILE* outfile)
{
    int i, ch;
    int ret, data_size;

    /* send the packet with the compressed data to the decoder */
    ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error submitting the packet to the decoder\n");
        exit(1);
    }

    /* read all the output frames (in general there may be any number of them */
    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            fprintf(stderr, "Error during decoding\n");
            exit(1);
        }
        data_size = av_get_bytes_per_sample(dec_ctx->sample_fmt);
        if (data_size < 0) {
            /* This should not occur, checking just for paranoia */
            fprintf(stderr, "Failed to calculate data size\n");
            exit(1);
        }
        for (i = 0; i < frame->nb_samples; i++)
            for (ch = 0; ch < dec_ctx->channels; ch++)
                fwrite(frame->data[ch] + data_size * i, 1, data_size, outfile);
    }
}
static AVFormatContext* fmt_ctx;
static AVCodecContext* dec_ctx;
static int audio_stream_index = -1;
static int open_input_file(const char* filename)
{
	int ret;
	AVCodec* dec;

	if ((ret = avformat_open_input(&fmt_ctx, filename, NULL, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
		return ret;
	}

	if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
		return ret;
	}

	/* select the audio stream */
	ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot find an audio stream in the input file\n");
		return ret;
	}
	audio_stream_index = ret;

	/* create decoding context */
	dec_ctx = avcodec_alloc_context3(dec);
	if (!dec_ctx)
		return AVERROR(ENOMEM);
	avcodec_parameters_to_context(dec_ctx, fmt_ctx->streams[audio_stream_index]->codecpar);

	/* init the audio decoder */
	if ((ret = avcodec_open2(dec_ctx, dec, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Cannot open audio decoder\n");
		return ret;
	}

	return 0;
}

int main(int argc, char** argv)
{
    const char* outfilename, * filename;
  
    int len, ret;
    FILE* f, * outfile;
    uint8_t inbuf[AUDIO_INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
    uint8_t* data;
    size_t   data_size;
    AVPacket* pkt;
    AVFrame* decoded_frame = NULL;
    enum AVSampleFormat sfmt;
    int n_channels = 0;
    const char* fmt;

    /*if (argc <= 2) {
        fprintf(stderr, "Usage: %s <input file> <output file>\n", argv[0]);
        exit(0);
    }*/
    filename = "demo.mp4";
    outfilename = "demo.wav";

    pkt = av_packet_alloc();

    FILE* out = fopen(outfilename, "wb");
    open_input_file(filename);
	decoded_frame = av_frame_alloc();

    while (av_read_frame(fmt_ctx, pkt) != AVERROR_EOF)
    {
        if (pkt->stream_index == audio_stream_index)
        {
            decode(dec_ctx, pkt, decoded_frame, out);
        }
    }

    /* flush the decoder */
    pkt->data = NULL;
    pkt->size = 0;
    decode(dec_ctx, pkt, decoded_frame, out);
end:
    fclose(out);

    avcodec_free_context(&dec_ctx);
    av_frame_free(&decoded_frame);
    av_packet_free(&pkt);

    return 0;
}