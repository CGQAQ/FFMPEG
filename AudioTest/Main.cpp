#define FILENAME "demo.mp4"
#define CGDEBUG 1

// 0 off
// 1 on
#define SYNC 1

// decode ahead of time, may consumes lots of memory if you set it very huge
#define AHEAD 10

extern "C" {
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>

#include <SDL.h>
}

#include <string>
#include <thread>
#include <chrono>
#include <queue>
#include <mutex>
#include <atomic>


#define LOGI(i) printf("[INFO]" i)

#define MKERRC(f, c, r)\
fprintf(stderr, "Function <%s> faild, error code: %d, reason: %s", #f, c, r)

#define MKERR(f, r)\
MKERRC(f, 0, r)

#define AVTHROW(f, c)\
char errbuf[1024];\
throw MKERRC(f, c, av_make_error_string(errbuf, 1024, c))

#define CHK_SEND(f, c, t)\
if (c == AVERROR(EAGAIN)) {\
LOGI(t " " #f " skipped\n");\
continue;\
}\
else if (result != 0) {\
	AVTHROW(f, c);\
}

static std::mutex q_video_mutex;
static std::mutex q_audio_mutex;

static std::atomic_uint64_t audio_pts{ 0 };
static std::atomic_uint64_t video_pts{ 0 };

class Player {
public:
	struct AVD {
		AVFormatContext* format_context{ nullptr };
		AVCodec* audio_codec{ nullptr };
		AVCodec* video_codec{ nullptr };
		AVCodecContext* audio_codec_context{ nullptr };
		AVCodecContext* video_codec_context{ nullptr };
		int audio_stream_index{ 0 };
		int video_stream_index{ 0 };

		SwrContext* swr_context{ nullptr };
	};

	struct YUVI {
		uint8_t* y{ nullptr };
		int y_line_size{ 0 };
		uint8_t* u{ nullptr };
		int u_line_size{ 0 };
		uint8_t* v{ nullptr };
		int v_line_size{ 0 };
		int64_t pts{ 0 };
	};

	struct AudioData {
		int64_t pts;
		uint8_t* data;
		size_t size;
	};

//#if SYNC == 1
//	struct QI {
//		std::shared_ptr<std::queue<AudioData>> q_audio;
//		SDL_AudioDeviceID id;
//	};
//#endif

public:
	Player(const char* path): path_(path) {
		avd_ = std::make_shared<AVD>();
		q_video_ = std::make_shared<std::queue<YUVI>>();
		q_audio_ = std::make_shared<std::queue<AudioData>>();
		InitFFMPEG();
		InitSDL();
	}
public:
	void Run() {
		// DecodeCbk(std::shared_ptr<AVD> avd, std::shared_ptr<std::queue<YUVI>> q_video_)
		decode_thread_ = std::make_unique<std::thread>(DecodeCbk, avd_, &real_spec_, q_video_, q_audio_);
		// VideoCbk(SDL_Renderer* render, SDL_Texture* texture, std::shared_ptr<std::queue<YUVI>> q_video_)
		video_thread_ = std::make_unique<std::thread>(VideoCbk, renderer, texture, q_video_);
#if SYNC==0
		audio_thread_ = std::make_unique<std::thread>(AudioCbk, audio_device_id_, q_audio_);
#endif
		MainLoop();
	}
private:
	static void
		DecodeCbk(
			std::shared_ptr<AVD> avd,
			SDL_AudioSpec* real_spec,
			std::shared_ptr<std::queue<YUVI>> q_video_, 
			std::shared_ptr<std::queue<AudioData>> q_audio_
		) {
		static AVFrame* frame = av_frame_alloc();
		AVPacket pkt;
		int result{ 0 };
		av_init_packet(&pkt);
		while (!(result = av_read_frame(avd->format_context, &pkt))) {
			// decode audio stream
			if (pkt.stream_index == avd->audio_stream_index) {
				result = avcodec_send_packet(avd->audio_codec_context, &pkt);
				CHK_SEND(avcodec_send_packet, result, "AUDIO");
				while (true) {
					result = avcodec_receive_frame(avd->audio_codec_context, frame);
					if (result != 0 && result != AVERROR(EAGAIN)) {
						AVTHROW(avcodec_receive_frame, result);
						break;
					}
					else if (result == AVERROR(EAGAIN)) {
						break;
					}
					//TODO: push to queue
					uint8_t* output{ nullptr };
					int bytes_persample = av_get_bytes_per_sample(AV_SAMPLE_FMT_U8);
					/*int out_samples = frame->nb_samples;
					out_samples =
						av_rescale_rnd(
							swr_get_delay(avd->swr_context, frame->sample_rate) + frame->nb_samples
							, frame->sample_rate
							, frame->sample_rate
							, AV_ROUND_UP
						);*/
					av_samples_alloc(&output, nullptr, 1, real_spec->samples, AV_SAMPLE_FMT_U8, 1);
					int out_samples =
						swr_convert(
							avd->swr_context
							, &output
							, real_spec->samples
							, const_cast<const uint8_t**>(frame->extended_data)
							, frame->nb_samples
						);
					size_t unpadded_linesize = out_samples * bytes_persample;
					AudioData ad;
					ad.pts = pkt.pts;
					ad.size = unpadded_linesize;
					ad.data = new uint8_t[unpadded_linesize];
					memcpy(ad.data, output, unpadded_linesize);
					av_freep(&output);
					q_audio_mutex.lock();
					while (q_audio_->size() > AHEAD);
					q_audio_->push(ad);
					q_audio_mutex.unlock();
				}
			}
			// decode video stream
			else if (pkt.stream_index == avd->video_stream_index) {
				result = avcodec_send_packet(avd->video_codec_context, &pkt);
				CHK_SEND(avcodec_send_packet, result, "VIDEO");
				while (true) {
					result = avcodec_receive_frame(avd->video_codec_context, frame);
					if (result != 0 && result != AVERROR(EAGAIN)) {
						AVTHROW(avcodec_receive_frame, result);
						break;
					}
					else if (result == AVERROR(EAGAIN)) {
						break;
					}
					YUVI data;
					data.y = new uint8_t[frame->linesize[0] * frame->height];
					data.u = new uint8_t[frame->linesize[1] * frame->height / 2];
					data.v = new uint8_t[frame->linesize[2] * frame->height / 2];
					memcpy(data.y, frame->extended_data[0], frame->linesize[0] * frame->height);
					memcpy(data.u, frame->extended_data[1], frame->linesize[1] * frame->height / 2);
					memcpy(data.v, frame->extended_data[2], frame->linesize[2] * frame->height / 2);
					data.y_line_size = frame->linesize[0];
					data.u_line_size = frame->linesize[1];
					data.v_line_size = frame->linesize[2];
					data.pts = pkt.pts;
					while (q_video_->size() > AHEAD);
					q_video_mutex.lock();
					q_video_->push(data);
					q_video_mutex.unlock();
				}
			}
		}
		if (result != AVERROR_EOF) {
			AVTHROW(av_read_frame, result);
		}
		av_frame_free(&frame);
	}
	static void VideoCbk(SDL_Renderer* render, SDL_Texture* texture, std::shared_ptr<std::queue<YUVI>> q_video){
		while (true)
		{
			//std::this_thread::sleep_for(std::chrono::milliseconds(16));
			q_video_mutex.lock();
			if (q_video->empty()) {
				q_video_mutex.unlock();
				continue;
			}
			else {
				auto& qv = q_video->front();
				//while (qv.pts > audio_pts);
				SDL_RenderClear(render);
				SDL_UpdateYUVTexture(texture, nullptr, qv.y, qv.y_line_size, qv.u, qv.u_line_size, qv.v, qv.v_line_size);
				SDL_RenderCopy(render, texture, nullptr, nullptr);
				SDL_RenderPresent(render);
				video_pts = qv.pts;
				delete[] qv.y;
				delete[] qv.u;
				delete[] qv.v;
				q_video->pop();
			}
			q_video_mutex.unlock();
		}
	}

#if SYNC == 0
	static void AudioCbk(SDL_AudioDeviceID device_id, std::shared_ptr<std::queue<AudioData>> q_audio) {
#elif SYNC == 1
	static void AudioCbk(void* userdata, uint8_t* stream, int len){
		auto q_audio = *reinterpret_cast<std::shared_ptr<std::queue<AudioData>>*>(userdata);
#endif
#if SYNC==0
		while (true) {
#endif
			q_audio_mutex.lock();
			if (q_audio->empty()) {
				q_audio_mutex.unlock();
#if SYNC==0
				continue;
#elif SYNC==1
				return;
#endif
			}
			AudioData& ad = q_audio->front();
			//while (ad.pts > video_pts);
#if SYNC==1
			memset(stream, 0, len);
			memcpy(stream, ad.data, ad.size);
			audio_pts = ad.pts;
#elif SYNC==0
			SDL_QueueAudio(device_id, ad.data, ad.size);
#endif
			q_audio->pop();
			q_audio_mutex.unlock();
#if SYNC==0
		}
#endif
	}

private:
	void MainLoop(){
		static bool quit{false};
		SDL_Event ev_main;
		while (!quit) {
			SDL_WaitEvent(&ev_main);
			switch (ev_main.type){
			case SDL_QUIT:
				quit = true;
				break;
			default:
				break;
			}
		}
	}

private:

	void InitSDL() {
		int result{ 0 };
		result = SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO);
		if (result != 0) {
			throw MKERR(SDL_Init, SDL_GetError());
		}

		window = SDL_CreateWindow("Player: " FILENAME,
			SDL_WINDOWPOS_CENTERED,
			SDL_WINDOWPOS_CENTERED,
			1024, 768,
			SDL_WINDOW_RESIZABLE
		);
		if (window == nullptr) {
			throw MKERR(SDL_CreateWindow, SDL_GetError());
		}

		renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
		if (renderer == nullptr) {
			throw MKERR(SDL_CreateRenderer, SDL_GetError());
		}

		texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, w, h);
		if (renderer == nullptr) {
			throw MKERR(SDL_CreateTexture, SDL_GetError());
		}

		SDL_AudioSpec want;
		SDL_AudioDeviceID dev;
		SDL_memset(&want, 0, sizeof(want));
		want.freq = 44100;
		want.format = AUDIO_U8;
		want.channels = 1;
		want.samples = 1024;
#if SYNC==0
		want.callback = nullptr;
#elif SYNC==1
		want.callback = AudioCbk;
		want.userdata = &q_audio_;
#endif

		audio_device_id_ = SDL_OpenAudioDevice(nullptr, 0, &want, &real_spec_, SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
		if (audio_device_id_ == 0) {
			MKERR(SDL_OpenAudioDevice, SDL_GetError());
		}
		else {
			if (want.samples != real_spec_.samples) { /* we let this one thing change. */
				LOGI("samples changed");
			}
			SDL_PauseAudioDevice(audio_device_id_, 0); /* start audio playing. */
		}
	}
	void InitFFMPEG() {
		int result{ 0 };
		result = avformat_open_input(&avd_->format_context, path_.c_str(), nullptr, nullptr);
		if (result < 0)
			throw MKERRC(avformat_open_input, result, "");

		result = avformat_find_stream_info(avd_->format_context, nullptr);
		if (result < 0)
			throw MKERRC(avformat_find_stream_info, result, "");

		result = av_find_best_stream(avd_->format_context, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
		if (result == AVERROR_STREAM_NOT_FOUND || result == AVERROR_DECODER_NOT_FOUND)
			throw MKERRC(av_find_best_stream, result, "av_find_best_stream(format_context_, AVMEDIA_TYPE_AUDIO, -1, -1, &audio_codec_, 0)");
		avd_->audio_stream_index = result;

		result = av_find_best_stream(avd_->format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
		if (result == AVERROR_STREAM_NOT_FOUND || result == AVERROR_DECODER_NOT_FOUND)
			throw MKERRC(av_find_best_stream, result, "av_find_best_stream(format_context_, AVMEDIA_TYPE_VIDEO, -1, -1, &video_codec_, 0)");
		avd_->video_stream_index = result;

		avd_->audio_codec = avcodec_find_decoder(avd_->format_context->streams[avd_->audio_stream_index]->codecpar->codec_id);
		avd_->video_codec = avcodec_find_decoder(avd_->format_context->streams[avd_->video_stream_index]->codecpar->codec_id);

		avd_->audio_codec_context = avcodec_alloc_context3(avd_->audio_codec);
		if (avd_->audio_codec_context == nullptr)
			throw MKERR(avcodec_alloc_context3, "avcodec_alloc_context3(audio_codec_)");

		avd_->video_codec_context = avcodec_alloc_context3(avd_->video_codec);
		if (avd_->video_codec_context == nullptr)
			throw MKERR(avcodec_alloc_context3, "avcodec_alloc_context3(video_codec_)");

		result = avcodec_parameters_to_context(avd_->audio_codec_context, avd_->format_context->streams[avd_->audio_stream_index]->codecpar);
		if (result < 0) {
			throw MKERRC(avcodec_parameters_to_context, result, "audio_codec_context_");
		}
		
		result = avcodec_parameters_to_context(avd_->video_codec_context, avd_->format_context->streams[avd_->video_stream_index]->codecpar);
		if (result < 0) {
			throw MKERRC(avcodec_parameters_to_context, result, "video_codec_context_");
		}

		w = avd_->video_codec_context->width;
		h = avd_->video_codec_context->height;

		result = avcodec_open2(avd_->audio_codec_context, avd_->audio_codec, nullptr);
		if (result < 0) 
			throw MKERR(avcodec_open2, "avcodec_open2(audio_codec_context_, audio_codec_, nullptr)");

		result = avcodec_open2(avd_->video_codec_context, avd_->video_codec, nullptr);
		if (result < 0)
			throw MKERR(avcodec_open2, "avcodec_open2(video_codec_context_, video_codec_, nullptr)");
	
		avd_->swr_context =
			swr_alloc_set_opts(
				nullptr
				, AV_CH_LAYOUT_MONO
				, AV_SAMPLE_FMT_U8
				, 44100
				, avd_->audio_codec_context->channel_layout
				, avd_->audio_codec_context->sample_fmt
				, avd_->audio_codec_context->sample_rate
				, 0
				, nullptr
			);
		if (avd_->swr_context == nullptr) {
			throw MKERR(swr_alloc_set_opts, "unknown error");
		}

		result = swr_init(avd_->swr_context);
		if (result != 0) {
			AVTHROW(swr_init, result);
		}
	}

public:
	~Player() {
		decode_thread_->join();
		audio_thread_->join();
		video_thread_->join();

		avcodec_close(avd_->audio_codec_context);
		avcodec_close(avd_->video_codec_context);
		avcodec_free_context(&avd_->audio_codec_context);
		avcodec_free_context(&avd_->video_codec_context);
		avformat_close_input(&avd_->format_context);

		SDL_DestroyTexture(texture);
		SDL_DestroyRenderer(renderer);
		SDL_DestroyWindow(window);
		SDL_Quit();
	}

private:
	// threads
	std::unique_ptr<std::thread> decode_thread_{ nullptr };
	std::unique_ptr<std::thread> audio_thread_{nullptr};
	std::unique_ptr<std::thread> video_thread_{nullptr};

private:
	// sdl stuff
	int w{ 0 }, h{ 0 };
	SDL_Window* window{ nullptr };
	SDL_Renderer* renderer{ nullptr }; 
	SDL_Texture* texture{ nullptr };

	SDL_AudioSpec real_spec_;
	SDL_AudioDeviceID audio_device_id_;
public:
	using string = std::string;
	const string path_;

private:
	// common stuff
	std::shared_ptr<AVD> avd_;
	std::shared_ptr<std::queue<YUVI>> q_video_;
	std::shared_ptr<std::queue<AudioData>> q_audio_;
};

int SDL_main(int argc, char* argv[])
{
	Player player(FILENAME);
	player.Run();

	return 0;
}