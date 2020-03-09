#define FILENAME "demo.mkv"
#define CG_DEBUG 1


extern "C" {
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>

#include <SDL.h>
}

#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <list>

#define PRINT_ERR(func)\
fprintf_s(stderr, "%s faild", #func);

#define PRINT_ERRINFO(func, code)\
char buf[4096];\
av_make_error_string(buf, 4096, code);\
PRINT_ERR(func);\
fprintf_s(stderr, "code: %d (%s)\n", code, buf);

#define EVPLAY SDL_USEREVENT + 1

static SDL_Event evEvent;
static int id{ 0 };

static SDL_Window* window{ nullptr };
static SDL_Renderer* renderer{ nullptr };
static SDL_Texture* texture{ nullptr };

struct VideoData {
	int64_t pts;

	uint8_t* y;
	int y_len;

	uint8_t* u;
	int u_len;

	uint8_t* v;
	int v_len;
};

struct AudioData {
	int64_t pts;
	uint8_t* data;
	size_t size;
};

static std::list<VideoData> vq;
static std::mutex vq_mutex;

static std::list<AudioData> aq;
static std::atomic_int64_t audio_pts = 0;
static std::mutex aq_mutex;

struct TParam{
	AVFormatContext* pFormatContext;
	int video_stream_index;
	int audio_stream_index;
	AVCodecContext* pAudioCodecCtx;
	AVCodecContext* pVideoCodecCtx;
	AVPacket* pPkt;
	AVFrame* pFrame;
};

static void DecodeAudioFrame(
	AVCodecContext* pCodecCtx,
	AVPacket* pPkt,
	AVFrame* pFrame
);
static void DecodeVideoFrame(
	AVCodecContext* pCodecCtx,
	AVPacket* pPkt,
	AVFrame* pFrame
);

static int DelayCalc();

static int loop_delay = 0;

void SDLAudioCbk(void* userdata, Uint8* stream, int len);
static int ThreadSDLLoop(void* data);
static int ThreadSDLDecode(void* data);

static void Output(const AVFrame& frame);

static AVFormatContext* pFormatContext{ nullptr };
static int audio_stream_index{ -1 },
			video_stream_index{ -1 };

int SDL_main(int argc, char* argv[]) {
	int result{ 0 };
	avformat_open_input(&pFormatContext, FILENAME, nullptr, nullptr);
	if (pFormatContext == nullptr) {
		fprintf(stderr, "avformat_open_input failed");
		return -1;
	}

	result = avformat_find_stream_info(pFormatContext, nullptr);
	if (result < 0) {
		PRINT_ERRINFO(avformat_find_stream_info, result);
	}

	AVCodec* pAudioCodec{ nullptr },
		* pVideoCodec{ nullptr };
	audio_stream_index = av_find_best_stream(pFormatContext, AVMEDIA_TYPE_AUDIO, -1, -1, &pAudioCodec, 0);
	video_stream_index = av_find_best_stream(pFormatContext, AVMEDIA_TYPE_VIDEO, -1, -1, &pVideoCodec, 0);
	if (pAudioCodec == nullptr) {
		PRINT_ERR(av_find_best_stream);
		return -1;
	}
	if (pVideoCodec == nullptr) {
		PRINT_ERR(av_find_best_stream);
		return -1;
	}

	AVCodecContext* pAudioCodecCtx{ nullptr },
		* pVideoCodecCtx{ nullptr };
	pAudioCodecCtx = avcodec_alloc_context3(pAudioCodec);
	pVideoCodecCtx = avcodec_alloc_context3(pVideoCodec);
	if (pAudioCodecCtx == nullptr) {
		PRINT_ERR(avcodec_alloc_context3);
		return -1;
	}
	if (pVideoCodecCtx == nullptr) {
		PRINT_ERR(avcodec_alloc_context3);
		return -1;
	}
	avcodec_parameters_to_context(pAudioCodecCtx, pFormatContext->streams[audio_stream_index]->codecpar);
	avcodec_parameters_to_context(pVideoCodecCtx, pFormatContext->streams[video_stream_index]->codecpar);
	avcodec_open2(pAudioCodecCtx, pAudioCodec, nullptr);
	avcodec_open2(pVideoCodecCtx, pVideoCodec, nullptr);

	av_dump_format(pFormatContext, -1, FILENAME, 0);

	AVPacket pkt;
	av_init_packet(&pkt);
	pkt.data = nullptr;
	pkt.size = 0;

	AVFrame* pFrame = av_frame_alloc();
	SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO);

	SDL_AudioSpec want, have;
	SDL_zero(want);
	SDL_zero(have);
	want.freq = pAudioCodecCtx->sample_rate;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 4096;
	want.silence = 0;
	want.callback = nullptr;
	id = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
	if (id == 0)
	{
		PRINT_ERR(SDL_OpenAudioDevice);
		return -1;
	}
	else
	{
		SDL_PauseAudioDevice(id, 0);
	}

	// video part
	window = SDL_CreateWindow("VideoPlayer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 960, SDL_WINDOW_RESIZABLE);
	if (window == nullptr) {
		PRINT_ERR(SDL_CreateWindow);
		return -1;
	}
	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
	if (renderer == nullptr) {
		PRINT_ERR(renderer);
		return -1;
	}
	texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, pVideoCodecCtx->width, pVideoCodecCtx->height);
	if (texture == nullptr) {
		PRINT_ERR(renderer);
		return -1;
	}


	loop_delay = pVideoCodecCtx->framerate.den;

	TParam param;
	param.pFormatContext = pFormatContext;
	param.audio_stream_index = audio_stream_index;
	param.video_stream_index = video_stream_index;
	param.pVideoCodecCtx = pVideoCodecCtx;
	param.pAudioCodecCtx = pAudioCodecCtx;
	param.pPkt = &pkt;
	param.pFrame = pFrame;
	SDL_CreateThread(ThreadSDLDecode, "decode", &param);
	SDL_CreateThread(ThreadSDLLoop, "main_loop", nullptr);

	SDL_Event evMain;
	bool quit = false;
	while (!quit) {
		SDL_WaitEvent(&evMain);
		switch (evMain.type)
		{
		case EVPLAY:
			if (vq.size() > 0) {
				static int result;
				std::lock_guard<std::mutex> lock(vq_mutex);
				VideoData v = vq.front();
				result = SDL_RenderClear(renderer);
				result = SDL_UpdateYUVTexture(texture, nullptr, v.y, v.y_len, v.u, v.u_len, v.v, v.v_len);
				SDL_RenderCopy(renderer, texture, nullptr, nullptr);
				SDL_RenderPresent(renderer);
				printf("%ld\n", v.pts);
				delete[] v.y;
				delete[] v.u;
				delete[] v.v;
				vq.pop_front();
			}
			break;
		case SDL_QUIT:
			quit = true;
			break;
		default:
			break;
		}
	}
	
	return 0;
}

static void
DecodeAudioFrame(
	AVCodecContext* pCodecCtx,
	AVPacket* pPkt,
	AVFrame* pFrame) {
	int result{ 0 };

	result = avcodec_send_packet(pCodecCtx, pPkt);
	if (result < 0) {
		PRINT_ERRINFO(avcodec_send_packet, result);
	}

	while ((result = avcodec_receive_frame(pCodecCtx, pFrame)) != AVERROR(EAGAIN)) {
		if (result < 0) {
			PRINT_ERRINFO(avcodec_receive_frame, result);
			break;
		}
		Output(*pFrame);
	}
}

static void
DecodeVideoFrame(
	AVCodecContext* pCodecCtx,
	AVPacket* pPkt,
	AVFrame* pFrame
) {
	int result{ 0 };

	result = avcodec_send_packet(pCodecCtx, pPkt);
	if (result < 0) {
		PRINT_ERRINFO(avcodec_send_packet, result);
	}

	while ((result = avcodec_receive_frame(pCodecCtx, pFrame)) != AVERROR(EAGAIN)) {
		if (result < 0) {
			PRINT_ERRINFO(avcodec_receive_frame, result);
			break;
		}
		/*SDL_UpdateYUVTexture(texture, nullptr,
			pFrame->extended_data[0], pFrame->linesize[0],
			pFrame->extended_data[1], pFrame->linesize[1],
			pFrame->extended_data[2], pFrame->linesize[2]
		);
		SDL_RenderClear(renderer);
		SDL_RenderCopy(renderer, texture, nullptr, nullptr);
		SDL_RenderPresent(renderer);*/
		std::lock_guard<std::mutex> lock(vq_mutex);
		VideoData vd;
		vd.pts = pFrame->pts;
		vd.y_len = pFrame->linesize[0];
		vd.y = new uint8_t[vd.y_len * pFrame->height];
		SDL_memcpy(vd.y, pFrame->extended_data[0], vd.y_len * pFrame->height);
		vd.u_len = pFrame->linesize[1];
		vd.u = new uint8_t[vd.u_len * pFrame->height];
		SDL_memcpy(vd.u, pFrame->extended_data[1], vd.u_len * pFrame->height/2);
		vd.v_len = pFrame->linesize[2];
		vd.v = new uint8_t[vd.v_len * pFrame->height];
		SDL_memcpy(vd.v, pFrame->extended_data[2], vd.v_len * pFrame->height/2);
		vq.push_back(vd);

		//printf("%lf\n", av_q2d(pCodecCtx->framerate));
		loop_delay = DelayCalc();
	}
}


static int64_t audio_index{0};
void 
SDLAudioCbk(void* userdata, Uint8* stream, int len) {
	//std::lock_guard<std::mutex> lock(aq_mutex);
	if (aq.size() > 0) {
		AudioData ad = aq.front();
		if (ad.size - audio_index > len) {
			memcpy_s(stream, len, ad.data, len);
			//SDL_MixAudio(stream, ad.data, len, SDL_MIX_MAXVOLUME);
			audio_index += len;
		}
		else {
			memcpy_s(stream, len, ad.data + audio_index, ad.size - audio_index);
			//memset(stream + audio_index, 0, len - ad.size + audio_index);
			//SDL_MixAudio(stream, ad.data, ad.size - audio_index, SDL_MIX_MAXVOLUME);
			audio_index = 0;
			av_freep(&ad.data);
			aq.pop_front();
		}

		//SDL_MixAudioFormat(stream, ad.data, AUDIO_S16SYS, len, SDL_MIX_MAXVOLUME);
		//SDL_MixAudio(stream, ad.data, ad.size, SDL_MIX_MAXVOLUME);
		
		audio_pts = ad.pts;
	}
}

static void
Output(const AVFrame& frame) {
	static SwrContext* pSwrCtx = swr_alloc();
	av_opt_set_channel_layout(pSwrCtx, "in_channel_layout", frame.channel_layout, 0);
	av_opt_set_channel_layout(pSwrCtx, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
	av_opt_set_int(pSwrCtx, "in_sample_rate", frame.sample_rate, 0);
	av_opt_set_int(pSwrCtx, "out_sample_rate", frame.sample_rate, 0);
	av_opt_set_sample_fmt(pSwrCtx, "in_sample_fmt", static_cast<AVSampleFormat>(frame.format), 0);
	av_opt_set_sample_fmt(pSwrCtx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
	swr_init(pSwrCtx);

	uint8_t* output;
	int bytes_persample = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
	int out_samples = frame.nb_samples;
	out_samples = av_rescale_rnd(swr_get_delay(pSwrCtx, frame.sample_rate) + frame.nb_samples, frame.sample_rate, frame.sample_rate, AV_ROUND_UP);
	av_samples_alloc(&output, NULL, 2, out_samples, AV_SAMPLE_FMT_S16, 1);
	out_samples = swr_convert(pSwrCtx, &output, out_samples, const_cast<const uint8_t**>(frame.extended_data), frame.nb_samples);
	size_t unpadded_linesize = out_samples * bytes_persample;

	SDL_QueueAudio(id, output, unpadded_linesize * 2);
	//SDL_QueueAudio(id, frame.data[0], frame.linesize[0]);
	av_freep(&output);
	//aq.push_back(AudioData{ frame.pts, output, unpadded_linesize * 2 });
}

static
int
ThreadSDLLoop(void* data) {
	static SDL_Event evPlay;
	evPlay.type = EVPLAY;
	while (true) {
		SDL_PushEvent(&evPlay);
		std::this_thread::sleep_for(std::chrono::microseconds(loop_delay));
	}
}

static
int 
ThreadSDLDecode(void* data) {
	TParam* param = reinterpret_cast<TParam*>(data);
	int result{0};
	while ((result = av_read_frame(param->pFormatContext, param->pPkt)) != AVERROR_EOF) {
		if (result < 0) {
			PRINT_ERRINFO(av_read_frame, result);
		}

		if (param->pPkt->stream_index == param->audio_stream_index) {
			DecodeAudioFrame(param->pAudioCodecCtx, param->pPkt, param->pFrame);
		}
		else if (param->pPkt->stream_index == param->video_stream_index) {
			DecodeVideoFrame(param->pVideoCodecCtx, param->pPkt, param->pFrame);
		}
	}
	return 0;
}


int DelayCalc() {
	int delay = floor(1.0 / av_q2d(pFormatContext->streams[video_stream_index]->r_frame_rate) * 1'000'000);
#if CG_DEBUG==2
	printf("delay: %ld, framerate: %lf\n", delay, ceil(av_q2d(pFormatContext->streams[video_stream_index]->r_frame_rate)));
#endif
	return delay - 500;
}