#pragma warning(disable : 4996)
#include <iostream>
#include <thread>
#include <mutex>
#include <chrono>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>

#include <SDL.h>
}

#define CHK_NZ(code, reason)\
	if((code)!=0){\
		std::cerr << code << " " << reason << "\n";\
		return code;\
	}

#define CHK_PTR(ptr, reason)\
	if((ptr) == nullptr){	\
		std::cerr << reason << "\n";	\
		return -1;\
	}

#define CHK_PTR_V(ptr, reason)\
	if((ptr) == nullptr){	\
		std::cerr << reason << "\n";	\
        return;\
	}

//#define CHK_PTR(ptr, reason)\
//	if((ptr) == nullptr){	\
//		std::cerr << reason << "\n";	\
//		__debugbreak();\
//	}



static const char* src_file_name{ nullptr };

static int video_stream_index{ -1 },
        audio_stream_index{ -1 };


static int width{ -1 };
static int height{ -1 };

static AVCodecContext* video_codec_ctx{ nullptr },
*audio_codec_ctx{ nullptr };

static AVFrame* video_frame{ nullptr };
static AVFrame* audio_frame{nullptr};
static std::mutex audio_frame_mutex;

static AVPixelFormat pix_fmt;

static uint8_t* video_dst_data[4] = { nullptr };
static int video_dst_linesize[4];
static int video_dst_bufsize;

static AVPacket packet;

static int video_frame_count = 0;
static int audio_frame_count = 0;

static int refcount = 0;


static int open_codec_context(int* stream_idx,
    AVCodecContext** dec_ctx, AVFormatContext* fmt_ctx, enum AVMediaType type);

static int decode_packet(int* got_frame, int cached);

int SDL_main(int argc, char* argv[])
{
    int result{ 0 };
    SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO);
    CHK_NZ(result, "sdl init faild");

    
    SwrContext* swrContext = swr_alloc();

	AVFormatContext* format_context{ nullptr };

    src_file_name = "demo.mp4";

	result = avformat_open_input(&format_context, src_file_name, nullptr, nullptr);
	CHK_NZ(result, "open video failed!");


	result = avformat_find_stream_info(format_context, nullptr);
	CHK_NZ(result, "avformat_find_stream_info faild");

    SDL_AudioDeviceID audio_device_id;



    std::thread audio_thread{ [&audio_device_id, &swrContext] () {
        while (true)
        {
            if (audio_frame != nullptr) {
                std::lock_guard<std::mutex> lock(audio_frame_mutex);
                
                uint8_t* ptr;

                int ret = swr_convert(swrContext, &ptr, audio_frame->nb_samples, (const uint8_t**)&audio_frame->data[0], audio_frame->nb_samples);
                if(ptr != nullptr && ret >= 0)
				    SDL_QueueAudio(audio_device_id, ptr, audio_frame->nb_samples);
            }
            using std::chrono_literals::operator ""ms;
            std::this_thread::sleep_for(30ms);
        }
    }};
    AVStream* video_stream{ nullptr };
    if (open_codec_context(&video_stream_index, &video_codec_ctx, format_context, AVMEDIA_TYPE_VIDEO) >= 0
        &&
        open_codec_context(&audio_stream_index, &audio_codec_ctx, format_context, AVMEDIA_TYPE_AUDIO) >= 0
        )
    {
        width = video_codec_ctx->width;
        height = video_codec_ctx->height;

        av_opt_set_channel_layout(swrContext, "in_channel_layout", audio_codec_ctx->channel_layout, 0);
        av_opt_set_channel_layout(swrContext, "out_channel_layout", audio_codec_ctx->channel_layout, 0);

		av_opt_set_int(swrContext, "in_sample_rate", audio_codec_ctx->sample_rate, 0);
		av_opt_set_int(swrContext, "out_sample_rate", audio_codec_ctx->sample_rate, 0);
		av_opt_set_sample_fmt(swrContext, "in_sample_fmt", audio_codec_ctx->sample_fmt, 0);
		av_opt_set_sample_fmt(swrContext, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
		int rv = swr_init(swrContext);
        CHK_NZ(rv, "swr init faild");



		SDL_AudioSpec want, have;
		SDL_memset(&want, 0, sizeof(want));

		want.freq = audio_codec_ctx->sample_rate * 1.1;
		want.format = AUDIO_U8;
		want.channels = audio_codec_ctx->channels;
        want.samples = 1024;
        want.callback = nullptr;

		//want.callback = [](void* userdata, Uint8* stream,int len) {
  //          if (video_frame != nullptr) {
  //              int size = audio_frame->linesize[0],
  //                  offset = 0;
  //              while (size > 0) {
  //                  int ds = FFMIN(len, size);
  //                  SDL_memcpy(stream, audio_frame->data[0 + offset], ds);
  //                  size -= ds;
  //                  offset += ds;
  //              }
		//		
		//		//SDL_memcpy(stream, audio_frame->data[1], audio_frame->linesize[1]);
  //          }
  //      };


		audio_device_id = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_ANY_CHANGE);
		if (audio_device_id == 0)
		{
			std::cerr << "Failed to open audio: " << SDL_GetError() << "\n";
		}
		else
		{
			SDL_PauseAudioDevice(audio_device_id, 0);
		}
    }


    av_dump_format(format_context, 0, src_file_name, 0);

    video_frame = av_frame_alloc();
    CHK_PTR(video_frame, "av_frame_alloc video_frame failed");

    audio_frame = av_frame_alloc();
    CHK_PTR(video_frame, "av_frame_alloc audio_frame failed");

    av_init_packet(&packet);
    packet.size =  0 ;
    packet.data = nullptr;

    int got_frame{ 0 };


	SDL_Window* window = SDL_CreateWindow("VideoPlayer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, video_codec_ctx->width / 2, video_codec_ctx->height / 2, 0);
	CHK_PTR(window, "SDL_CreateWindow failed");

	SDL_Renderer* renderer =
		//SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
		SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);

    SDL_Texture* texture = 
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, video_codec_ctx->width, video_codec_ctx->height);
    while (av_read_frame(format_context, &packet) >= 0)
    {
        AVPacket orig_pkt = packet;
        do {
            result = decode_packet(&got_frame, 0);
            if (result < 0)
                break;
            packet.data += result;
            packet.size -= result;

            SDL_RenderClear(renderer);


			SDL_UpdateYUVTexture(texture, nullptr, video_frame->data[0], video_frame->linesize[0],
				video_frame->data[1], video_frame->linesize[1],
				video_frame->data[2], video_frame->linesize[2]
			);
            //SDL_UpdateTexture(texture, nullptr, frame->data[0], frame->linesize[0]);

            SDL_RenderCopy(renderer, texture, nullptr, nullptr);
            SDL_RenderPresent(renderer);

			
			//SDL_QueueAudio(audio_device_id, audio_frame->data[1], audio_frame->linesize[1]);

        } while (packet.size > 0);
        av_packet_unref(&orig_pkt);
    }
    
	return 0;
}


static int open_codec_context(int* stream_idx,
    AVCodecContext** dec_ctx, AVFormatContext* fmt_ctx, enum AVMediaType type)
{
    int ret, stream_index;
    AVStream* st;
    AVCodec* dec = NULL;
    AVDictionary* opts = NULL;

    ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not find %s stream in input file '%s'\n",
            av_get_media_type_string(type), src_file_name);
        return ret;
    }
    else {
        stream_index = ret;
        st = fmt_ctx->streams[stream_index];

        /* find decoder for the stream */
        dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) {
            fprintf(stderr, "Failed to find %s codec\n",
                av_get_media_type_string(type));
            return AVERROR(EINVAL);
        }

        /* Allocate a codec context for the decoder */
        *dec_ctx = avcodec_alloc_context3(dec);
        if (!*dec_ctx) {
            fprintf(stderr, "Failed to allocate the %s codec context\n",
                av_get_media_type_string(type));
            return AVERROR(ENOMEM);
        }

        /* Copy codec parameters from input stream to output codec context */
        if ((ret = avcodec_parameters_to_context(*dec_ctx, st->codecpar)) < 0) {
            fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
                av_get_media_type_string(type));
            return ret;
        }

        /* Init the decoders, with or without reference counting */
        av_dict_set(&opts, "refcounted_frames", refcount ? "1" : "0", 0);
        if ((ret = avcodec_open2(*dec_ctx, dec, &opts)) < 0) {
            fprintf(stderr, "Failed to open %s codec\n",
                av_get_media_type_string(type));
            return ret;
        }
        *stream_idx = stream_index;
    }

    return 0;
}

static void SaveAvFrame(AVFrame* avFrame);

static int decode_packet(int* got_frame, int cached)
{
    int ret = 0;
    int decoded = packet.size;

    *got_frame = 0;

    if (packet.stream_index == video_stream_index) {
        /* decode video frame */
        
        // deprecated
        //ret = avcodec_decode_video2(video_codec_ctx, frame, got_frame, &packet);
        ret = avcodec_send_packet(video_codec_ctx, &packet);

        if (ret < 0) {
            char err[AV_ERROR_MAX_STRING_SIZE] = {0};
            fprintf(stderr, "Error decoding video frame (%s)\n", av_make_error_string(err, AV_ERROR_MAX_STRING_SIZE, ret));
            return ret;
        }
        
        ret = avcodec_receive_frame(video_codec_ctx, video_frame);


        if (!ret) {

            if (video_frame->width != width || video_frame->height != height ||
                video_frame->format != pix_fmt) {
                /* To handle this change, one could call av_image_alloc again and
                 * decode the following frames into another rawvideo file. */
                fprintf(stderr, "Error: Width, height and pixel format have to be "
                    "constant in a rawvideo file, but the width, height or "
                    "pixel format of the input video changed:\n"
                    "old: width = %d, height = %d, format = %s\n"
                    "new: width = %d, height = %d, format = %s\n",
                    width, height, av_get_pix_fmt_name(pix_fmt),
                    video_frame->width, video_frame->height,
                    av_get_pix_fmt_name(static_cast<AVPixelFormat>(video_frame->format)));
                return -1;
            }

            
            

            printf("video_frame%s n:%d coded_n:%d\n",
                cached ? "(cached)" : "",
                video_frame_count++, video_frame->coded_picture_number);


            /* copy decoded frame to destination buffer:
             * this is required since rawvideo expects non aligned data */
            av_image_copy(video_dst_data, video_dst_linesize,
                (const uint8_t**)(video_frame->data), video_frame->linesize,
                pix_fmt, width, height);

            /* write to rawvideo file */
            //fwrite(video_dst_data[0], 1, video_dst_bufsize, video_dst_file);
        }
    }
    else if (packet.stream_index == audio_stream_index) {
        /* decode audio frame */
        //ret = avcodec_decode_audio4(&audio_codec_ctx, frame, got_frame, &packet);
        ret = avcodec_send_packet(audio_codec_ctx, &packet);
        if (ret < 0) {
            char err[AV_ERROR_MAX_STRING_SIZE] = { 0 };
            fprintf(stderr, "Error decoding audio frame (%s)\n", av_make_error_string(err, AV_ERROR_MAX_STRING_SIZE, ret));
            return ret;
        }
        /* Some audio decoders decode only part of the packet, and have to be
         * called again with the remainder of the packet data.
         * Sample: fate-suite/lossless-audio/luckynight-partial.shn
         * Also, some decoders might over-read the packet. */
        decoded = packet.size;

        std::lock_guard<std::mutex> lock(audio_frame_mutex);
        ret = avcodec_receive_frame(audio_codec_ctx, audio_frame);

        if (!ret) {
            size_t unpadded_linesize = audio_frame->nb_samples * av_get_bytes_per_sample(static_cast<AVSampleFormat>(audio_frame->format));
            char timestr[AV_TS_MAX_STRING_SIZE]{0};
            printf("audio_frame%s n:%d nb_samples:%d pts:%s\n",
                cached ? "(cached)" : "",
                audio_frame_count++, audio_frame->nb_samples,
                av_ts_make_time_string(timestr, audio_frame->pts, &audio_codec_ctx->time_base)
                );

            /* Write the raw audio data samples of the first plane. This works
             * fine for packed formats (e.g. AV_SAMPLE_FMT_S16). However,
             * most audio decoders output planar audio, which uses a separate
             * plane of audio samples for each channel (e.g. AV_SAMPLE_FMT_S16P).
             * In other words, this code will write only the first audio channel
             * in these cases.
             * You should use libswresample or libavfilter to convert the frame
             * to packed data. */
            //fwrite(frame->extended_data[0], 1, unpadded_linesize, audio_dst_file);
        }
    }

    /* If we use frame reference counting, we own the data and need
     * to de-reference it when we don't use it anymore */
    if (!ret && refcount) {
		av_frame_unref(audio_frame);
        av_frame_unref(video_frame);
    }


    return decoded;
}


//static void SaveAvFrame(AVFrame* avFrame)
//{
//    FILE* fDump = fopen("a.yuv", "ab");
//    uint32_t pitchY = avFrame->linesize[0];
//    uint32_t pitchU = avFrame->linesize[1];
//    uint32_t pitchV = avFrame->linesize[2];
//
//    uint8_t* avY = avFrame->data[0];
//    uint8_t* avU = avFrame->data[1];
//    uint8_t* avV = avFrame->data[2];
//
//    for (uint32_t i = 0; i < avFrame->height; i++) {
//        fwrite(avY, avFrame->width, 1, fDump);
//        avY += pitchY;
//    }
//
//    for (uint32_t i = 0; i < avFrame->height / 2; i++) {
//        fwrite(avU, avFrame->width / 2, 1, fDump);
//        avU += pitchU;
//    }
//
//    for (uint32_t i = 0; i < avFrame->height / 2; i++) {
//        fwrite(avV, avFrame->width / 2, 1, fDump);
//        avV += pitchV;
//    }
//
//    fclose(fDump);
//}

static void SaveAvFrame(AVFrame* avFrame)
{
    AVCodec* jpeg_codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    CHK_PTR_V(jpeg_codec, "cannot find jpeg encoder");


    AVCodecContext* jpeg_context = avcodec_alloc_context3(jpeg_codec);
    CHK_PTR_V(jpeg_context,"avcodec_alloc_context3 failed");

    jpeg_context->time_base = AVRational{ 1,25 };

    jpeg_context->pix_fmt = AV_PIX_FMT_YUVJ420P;
    jpeg_context->height = avFrame->height;
    jpeg_context->width = avFrame->width;

    if (avcodec_open2(jpeg_context, jpeg_codec, nullptr) < 0)
    {
        return;
    }

    FILE* jpeg_file{ nullptr };
    char filename[256]{ 0 };

    AVPacket packet;
    packet.data = nullptr;
    packet.size = 0;
    av_init_packet(&packet);
    avcodec_send_frame(jpeg_context, video_frame);
    avcodec_receive_packet(jpeg_context, &packet);
    sprintf(filename, "file-%d.jpg", video_frame_count);
    jpeg_file = fopen(filename, "wb");
    fwrite(packet.data, 1, packet.size, jpeg_file);
    fclose(jpeg_file);

    av_free_packet(&packet);
    avcodec_close(jpeg_context);
}