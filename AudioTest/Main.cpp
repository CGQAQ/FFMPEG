#define FILENAME "demo.mkv"
#define CGDEBUG 1
#define VERBOSE 0

// 0 off
// 1 on
#define SYNC 1
#define SYNC_THRESHOLD 1

// decode ahead of time, may consumes lots of memory if you set it very huge
#define AHEAD 10

extern "C" {
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>

#include <SDL.h>
}

#include <cinttypes>
#include <string>
#include <thread>
#include <chrono>
#include <queue>
#include <mutex>
#include <atomic>


#define LOGI(i) printf("[INFO]" i "\n")

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

static bool quit{ false };
static uint64_t skipped_frame{ 0 };

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
		AVRational audio_stream_timebase;
		AVRational video_stream_timebase;

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

#if SYNC == 1
	struct QI {
		std::shared_ptr<std::queue<AudioData>> q_audio;
		SDL_AudioSpec* real_spec;
	};
#endif

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
		video_thread_ = std::make_unique<std::thread>(VideoCbk, renderer, texture,avd_, q_video_);
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
		while (!(result = av_read_frame(avd->format_context, &pkt)) && !quit) {
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
					int bytes_persample = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
					int out_samples = real_spec->freq;
					out_samples =
						av_rescale_rnd(
							swr_get_delay(avd->swr_context, frame->sample_rate) + frame->nb_samples
							, out_samples
							, frame->sample_rate
							, AV_ROUND_UP
							);
					av_samples_alloc(&output, nullptr, 1, out_samples, AV_SAMPLE_FMT_S16, 0);
					out_samples =
						swr_convert(
							avd->swr_context
							, &output
							, out_samples
							, const_cast<const uint8_t**>(frame->extended_data)
							, frame->nb_samples
						);
					size_t unpadded_linesize = static_cast<size_t>(out_samples) * bytes_persample;
					AudioData ad;
					ad.pts = pkt.pts;
					ad.size = unpadded_linesize;
					ad.data = new uint8_t[unpadded_linesize];
					memcpy(ad.data, output, unpadded_linesize);
					av_freep(&output);
					while (q_audio_->size() > AHEAD);
					q_audio_mutex.lock();
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
					data.y = new uint8_t[static_cast<size_t>(frame->linesize[0]) * frame->height];
					data.u = new uint8_t[static_cast<size_t>(frame->linesize[1])* frame->height / 2];
					data.v = new uint8_t[static_cast<size_t>(frame->linesize[2])* frame->height / 2];
					memcpy(data.y, frame->extended_data[0], static_cast<size_t>(frame->linesize[0])* frame->height);
					memcpy(data.u, frame->extended_data[1], static_cast<size_t>(frame->linesize[1])* frame->height / 2);
					memcpy(data.v, frame->extended_data[2], static_cast<size_t>(frame->linesize[2])* frame->height / 2);
					data.y_line_size = frame->linesize[0];
					data.u_line_size = frame->linesize[1];
					data.v_line_size = frame->linesize[2];
					data.pts = pkt.pts;

					while (true) {
						q_video_mutex.lock();
						if (q_video_->size() > AHEAD) {
							q_video_mutex.unlock();
							continue;
						}
						else {
							break;
						}
					}
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
	static void VideoCbk(SDL_Renderer* render, SDL_Texture* texture, std::shared_ptr<AVD> avd, std::shared_ptr<std::queue<YUVI>> q_video){
		static int64_t frame_index{ 0 };
		frame_index++;
		while (!quit)
		{
			q_video_mutex.lock();

			if (q_video->empty()) {
				q_video_mutex.unlock();
				continue;
			}
			else {
				bool repeat = false;
				auto& qv = q_video->front();
#if SYNC==1
				auto dif = static_cast<int64_t>(qv.pts * av_q2d(avd->video_stream_timebase) - audio_pts * av_q2d(avd->audio_stream_timebase));
				if (std::abs(dif) > SYNC_THRESHOLD) {
					if (dif < 0) {
						delete[] qv.y;
						delete[] qv.u;
						delete[] qv.v;
						q_video->pop();
						skipped_frame++;
						printf("%" PRIu64 " frame skipped\n", skipped_frame);
						q_video_mutex.unlock();
						continue;
					}
					else {
						repeat = true;
					}
				}
#endif
				//while (qv.pts > audio_pts);
				SDL_RenderClear(render);
				SDL_UpdateYUVTexture(texture, nullptr, qv.y, qv.y_line_size, qv.u, qv.u_line_size, qv.v, qv.v_line_size);
				SDL_Rect r;
				memset(&r, 0, sizeof(r));
				r.w = w;
				r.h = h;
				SDL_RenderCopy(render, texture, nullptr, &r);
				SDL_RenderPresent(render);
				//printf("current time: %d\n", static_cast<int>(qv.pts * av_q2d(avd->video_stream_timebase)));
				//video_pts = qv.pts;
#if SYNC==1
				if (!repeat) {
#endif
					delete[] qv.y;
					delete[] qv.u;
					delete[] qv.v;
					q_video->pop();
#if SYNC==1
				}
				else {
					printf("#%d video frame repeated\n", frame_index);
				}
#endif
			}
			q_video_mutex.unlock();
		}
	}

	static void
#if SYNC == 0
	AudioCbk(SDL_AudioDeviceID device_id, std::shared_ptr<std::queue<AudioData>> q_audio) {
#elif SYNC == 1
	AudioCbk(void* userdata, uint8_t* stream, int len){
		auto& qi = *reinterpret_cast<QI*>(userdata);
		auto& q_audio = qi.q_audio;
		auto real_spec = qi.real_spec;

#endif
#if SYNC==0
		while (!quit) {
			q_audio_mutex.lock();
			if (q_audio->empty()) {
				q_audio_mutex.unlock();
				continue;
			}
			AudioData& ad = q_audio->front();
			SDL_QueueAudio(device_id, ad.data, ad.size);
			q_audio->pop();
			q_audio_mutex.unlock();
		}
#elif SYNC==1
		q_audio_mutex.lock();
		if (q_audio->empty()) {
			q_audio_mutex.unlock();
			memset(stream, real_spec->silence, len);
			return;
		}
		int audio_len = len;
		int audio_index = 0;
		static int src_index{ 0 };
		int need_len = FFMIN3(audio_len, q_audio->front().size, q_audio->front().size - src_index);

		static int idx = 0;

		while (audio_len > 0) {
			if (q_audio->empty()) {
				memset(stream + audio_index, real_spec->silence, need_len);
				break;
			}
			AudioData& ad = q_audio->front();
			memcpy(stream + audio_index, ad.data + src_index, need_len);
			audio_len -= need_len;
			audio_index += need_len;
			src_index += need_len;
			need_len = len - audio_index;
			audio_pts = ad.pts;
			if (src_index >= ad.size) {
				q_audio->pop();
				src_index = 0;
			}
#if CGDEBUG==1 && VERBOSE==1
			printf("#%d audio idx: %d,audio len: %d,src index: %d, need len: %d\n", idx, audio_index, audio_len, src_index, need_len);
#endif
		}
		q_audio_mutex.unlock();
		idx++;
#endif
	}

private:
	void MainLoop(){
		SDL_Event ev_main{0};
		while (!quit) {
			SDL_WaitEvent(&ev_main);
			switch (ev_main.type){
			case SDL_QUIT:
				quit = true;
				break;
			case SDL_WINDOWEVENT:
				SDL_GetWindowSize(window, &w, &h);
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
			SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN
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
		SDL_memset(&want, 0, sizeof(want));
		want.freq = avd_->audio_codec_context->sample_rate;
		want.format = AUDIO_S16SYS;
		want.channels = 1;
		want.samples = 1024;
#if SYNC==0
		want.callback = nullptr;
#elif SYNC==1
		want.callback = AudioCbk;
		qi_.q_audio = q_audio_;
		qi_.real_spec = &real_spec_;
		want.userdata = &qi_;
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

		avd_->swr_context =
			swr_alloc_set_opts(
				nullptr
				, AV_CH_LAYOUT_MONO
				, AV_SAMPLE_FMT_S16
				, real_spec_.freq
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
	
		avd_->audio_stream_timebase = avd_->format_context->streams[avd_->audio_stream_index]->time_base;
		avd_->video_stream_timebase = avd_->format_context->streams[avd_->video_stream_index]->time_base;

#if CGDEBUG==1
		av_dump_format(avd_->format_context, -1, FILENAME, 0);
#endif

		
	}

public:
	~Player() {
		/*decode_thread_->join();
		audio_thread_->join();
		video_thread_->join();*/

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
	static int w, h;
	SDL_Window* window{ nullptr };
	SDL_Renderer* renderer{ nullptr }; 
	SDL_Texture* texture{ nullptr };

	SDL_AudioSpec real_spec_;
	SDL_AudioDeviceID audio_device_id_;
#if SYNC==1
	QI qi_;
#endif
public:
	using string = std::string;
	const string path_;

private:
	// common stuff
	std::shared_ptr<AVD> avd_;
	std::shared_ptr<std::queue<YUVI>> q_video_;
	std::shared_ptr<std::queue<AudioData>> q_audio_;
};

int Player::w{ 0 };
int Player::h{ 0 };


int SDL_main(int argc, char* argv[])
{
	Player player(FILENAME);
	player.Run();

	return 0;
}