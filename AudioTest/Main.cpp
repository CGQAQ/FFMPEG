#define FILENAME "demo.mkv"
#define CGDEBUG 1

extern "C" 
{
#include <libavformat/avformat.h>

#include <SDL.h>
}
#include <string>


#define MKERRC(f, c, r)\
fprintf(stderr, "Function <%s> faild, error code: %d, reason: %s", #f, c, r)

#define MKERR(f, r)\
MKERRC(f, 0, r)


class Player {
public:
	Player(const char* path): path_(path) {
		InitSDL();
		InitFFMPEG();
	}

public:
	void MainLoop(){
		SDL_Event ev_main;
		while (true) {
			SDL_WaitEvent(&ev_main);
			switch (ev_main.type){}
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
	}
	void InitFFMPEG() {
		format_context_ = avformat_alloc_context();
		if (format_context_ == nullptr) {
			throw MKERR(avformat_alloc_context, "format_context_ is nullptr");
		}

		int result{ 0 };
		result = avformat_open_input(&format_context_, path_.c_str(), nullptr, nullptr);
		if (result < 0)
			throw MKERRC(avformat_open_input, result, "");

		result = avformat_find_stream_info(format_context_, nullptr);
		if (result < 0)
			throw MKERRC(avformat_find_stream_info, result, "");
		audio_stream_index_ = result;

		result = av_find_best_stream(format_context_, AVMEDIA_TYPE_AUDIO, -1, -1, &audio_codec_, 0);
		if (result == AVERROR_STREAM_NOT_FOUND || result == AVERROR_DECODER_NOT_FOUND)
			throw MKERRC(av_find_best_stream, result, "av_find_best_stream(format_context_, AVMEDIA_TYPE_AUDIO, -1, -1, &audio_codec_, 0)");
		video_stream_index_ = result;
		
		result = av_find_best_stream(format_context_, AVMEDIA_TYPE_VIDEO, -1, -1, &video_codec_, 0);
		if (result == AVERROR_STREAM_NOT_FOUND || result == AVERROR_DECODER_NOT_FOUND)
			throw MKERRC(av_find_best_stream, result, "av_find_best_stream(format_context_, AVMEDIA_TYPE_VIDEO, -1, -1, &video_codec_, 0)");
	
		audio_codec_context_ = avcodec_alloc_context3(audio_codec_);
		if (audio_codec_context_ == nullptr)
			throw MKERR(avcodec_alloc_context3, "avcodec_alloc_context3(audio_codec_)");

		video_codec_context_ = avcodec_alloc_context3(video_codec_);
		if (video_codec_context_ == nullptr)
			throw MKERR(avcodec_alloc_context3, "avcodec_alloc_context3(video_codec_)");

		result = avcodec_open2(audio_codec_context_, audio_codec_, nullptr);
		if (result < 0) 
			throw MKERR(avcodec_open2, "avcodec_open2(audio_codec_context_, audio_codec_, nullptr)");

		result = avcodec_open2(video_codec_context_, video_codec_, nullptr);
		if (result < 0)
			throw MKERR(avcodec_open2, "avcodec_open2(video_codec_context_, video_codec_, nullptr)");
	}

private:
	SDL_Window* window{ nullptr };
	SDL_Renderer* renderer{ nullptr }; 
public:
	using string = std::string;
	const string path_;
	AVFormatContext* format_context_{ nullptr };
	AVCodec* audio_codec_{ nullptr };
	AVCodec* video_codec_{ nullptr };
	AVCodecContext* audio_codec_context_{ nullptr };
	AVCodecContext* video_codec_context_{ nullptr };
	int audio_stream_index_;
	int video_stream_index_;
};

int main()
{
	Player player(FILENAME);

	return 0;
}