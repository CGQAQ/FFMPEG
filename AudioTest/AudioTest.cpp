// AudioTest.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <Windows.h>
#include <MMDeviceAPI.h>
#include <Audioclient.h>
#include <queue>

extern "C"{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <SDL.h>
}

#define RAW_OUT_ON_PLANAR false
#define REFTIMES_PER_SEC  10000000
#define REFTIMES_PER_MILLISEC  10000

#define printErr(ret) \
{\
char err[AV_ERROR_MAX_STRING_SIZE] = { 0 }; \
av_strerror(ret, err, sizeof(err)); \
fprintf(stderr, "Error decoding video frame (%s)\n", err);\
}

#define PRINT_FAILD(hr, msg)\
if (FAILED(hr)) {\
printf("unable to CoInitializeEx");\
}


template <class T> void SafeRelease(T** ppT)
{
	if (*ppT)
	{
		(*ppT)->Release();
		*ppT = NULL;
	}
}
	
static SDL_AudioDeviceID id = 0;

static std::queue<float> audio_queue;


int receiveAndHandle(AVCodecContext* codecCtx, AVFrame* frame);
void handleFrame(AVCodecContext* codecCtx, AVFrame* frame);
float getSample(AVCodecContext* codecCtx, uint8_t* buffer, int sampleIndex);

int SDL_main(int argc, char* argv[])
{
	REFERENCE_TIME hnsRequestedDuration = REFTIMES_PER_SEC;
	WAVEFORMATEX* pwfx = NULL;
	IAudioClient* pAudioClient;
	IMMDeviceEnumerator* deviceEnumerator = NULL;
	IMMDevice* device;
	HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	PRINT_FAILD(hr, "unable to CoInitializeEx");

	hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&deviceEnumerator));
	PRINT_FAILD(hr, "CoCreateInstance MMDeviceEnumerator faild");

	hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
	PRINT_FAILD(hr, "GetDefaultAudioEndpoint faild");

	SafeRelease(&deviceEnumerator);

	device->AddRef();

	hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pAudioClient);
	PRINT_FAILD(hr, "device Activate faild");

	hr = pAudioClient->GetMixFormat(&pwfx);
	PRINT_FAILD(hr, "device Activate faild");

	hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, hnsRequestedDuration, 0, pwfx, NULL);
	PRINT_FAILD(hr, "device Activate faild");


	int ret_code{0};
	AVFormatContext* format_context{nullptr};
	avformat_open_input(&format_context, "demo.mp4", nullptr, nullptr);

	ret_code = avformat_find_stream_info(format_context, nullptr);
	if (ret_code < 0)
		printErr(ret_code);

	int audio_stream_index = av_find_best_stream(format_context, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	int video_stream_index = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);


	AVCodec* ac = avcodec_find_decoder(format_context->streams[audio_stream_index]->codecpar->codec_id);
	AVCodecContext* cc = avcodec_alloc_context3(ac);
	avcodec_parameters_to_context(cc, format_context->streams[audio_stream_index]->codecpar);
	//cc->request_sample_fmt = av_get_alt_sample_fmt(cc->sample_fmt, 0);
	AVDictionary* opts = nullptr;
	av_dict_set(&opts, "refcounted_frames", "1", NULL);
	int err = avcodec_open2(cc, ac, &opts);

	AVCodec* vac = avcodec_find_decoder(format_context->streams[video_stream_index]->codecpar->codec_id);
	AVCodecContext* vcc = avcodec_alloc_context3(vac);
	avcodec_parameters_to_context(vcc, format_context->streams[video_stream_index]->codecpar);
	//cc->request_sample_fmt = av_get_alt_sample_fmt(cc->sample_fmt, 0);
	opts = nullptr;
	av_dict_set(&opts, "refcounted_frames", "1", NULL);
	err = avcodec_open2(vcc, vac, &opts);

	av_dump_format(format_context, 0, "demo.mp4", 0);

	AVPacket p;
	av_init_packet(&p);
	p.size = 0;
	p.data = nullptr;

	AVFrame* frame = av_frame_alloc();

	SDL_Init(SDL_INIT_AUDIO|SDL_INIT_VIDEO);

	SDL_Window* window = SDL_CreateWindow("hello", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 760, 0);

	SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);

	SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, vcc->width, vcc->height);

	SDL_AudioSpec want, have;
	SDL_memset(&want, 0, sizeof(want));

	want.freq = 48000;
	want.format = AUDIO_F32LSB;
	want.channels = cc->channels;
	want.samples = 1024;
	want.callback = nullptr;

	id = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_ANY_CHANGE);
	if (id == 0)
	{
		std::cerr << "Failed to open audio: " << SDL_GetError() << "\n";
	}
	else
	{
		SDL_PauseAudioDevice(id, 0);
	}
	
	int ret = 0;
	int count = 0;
	//av_seek_frame(format_context, audio_stream_index, 0, AVSEEK_FLAG_FRAME);
	while ((ret != AVERROR_EOF))
	{
		count++;
		if( (ret = av_read_frame(format_context, &p)) < 0)
		{
			printErr(ret)
		}

		if (p.stream_index == audio_stream_index) {
			int ret{ 0 };
			ret = avcodec_send_packet(cc, &p);
			if (ret == 0) {
				//av_packet_unref(&p);
			}
			else {
				printErr(ret);
			}
			ret = receiveAndHandle(cc, frame);
			if (ret != AVERROR(EAGAIN)) {
				printErr(ret);
			}
		}
		else if (p.stream_index == video_stream_index) {
			int ret{ 0 };
			ret = avcodec_send_packet(vcc, &p);
			if (ret == 0) {
				//av_packet_unref(&p);
			}
			else {
				printErr(ret);
			}

			while (avcodec_receive_frame(vcc, frame) != AVERROR(EAGAIN)) {
				SDL_UpdateYUVTexture(texture, nullptr,
					frame->extended_data[0], frame->linesize[0],
					frame->extended_data[1], frame->linesize[1],
					frame->extended_data[2], frame->linesize[2]
					);
				SDL_RenderClear(renderer);
				SDL_RenderCopy(renderer, texture, nullptr, nullptr);
				SDL_RenderPresent(renderer);
			}
		}
	}
	
	return 0;
}

int receiveAndHandle(AVCodecContext* codecCtx, AVFrame* frame) {
	int err = 0;
	// Read the packets from the decoder.
	// NOTE: Each packet may generate more than one frame, depending on the codec.
	while ((err = avcodec_receive_frame(codecCtx, frame)) == 0) {
		// Let's handle the frame in a function.
		handleFrame(codecCtx, frame);
		// Free any buffers and reset the fields to default values.
		av_frame_unref(frame);
	}
	return err;
}

void handleFrame(AVCodecContext* codecCtx, AVFrame* frame) {
	if (av_sample_fmt_is_planar(codecCtx->sample_fmt) == 1) {
		// This means that the data of each channel is in its own buffer.
		// => frame->extended_data[i] contains data for the i-th channel.
		for (int s = 0; s < frame->nb_samples; ++s) {
			for (int c = 0; c < codecCtx->channels; ++c) {
				float sample = getSample(codecCtx, frame->extended_data[c], s);
				audio_queue.push(sample);
			}
		}
	}
	else {
		// This means that the data of each channel is in the same buffer.
		// => frame->extended_data[0] contains data of all channels.
		if (RAW_OUT_ON_PLANAR) {
			//fwrite(frame->extended_data[0], 1, frame->linesize[0], outFile);
			for (int i = 0; i < frame->linesize[0]; ++i) {
				audio_queue.push(frame->extended_data[0][i]);
			}
			//SDL_QueueAudio(id, frame->extended_data[0], frame->linesize[0]);
		}
		else {
			for (int s = 0; s < frame->nb_samples; ++s) {
				for (int c = 0; c < codecCtx->channels; ++c) {
					float sample = getSample(codecCtx, frame->extended_data[0], s * codecCtx->channels + c);
					//SDL_QueueAudio(id, &sample, 1);
					audio_queue.push(sample);
				}
			}
		}
	}
}

float getSample(AVCodecContext* codecCtx, uint8_t* buffer, int sampleIndex){
	int64_t val = 0;
	float ret = 0;
	int sampleSize = av_get_bytes_per_sample(codecCtx->sample_fmt);

	switch (sampleSize) {
	case 1:
		// 8bit samples are always unsigned
		val = reinterpret_cast<uint8_t * >(buffer)[sampleIndex];
		// make signed
		val -= 127;
		break;

	case 2:
		val = val = reinterpret_cast<uint16_t*>(buffer)[sampleIndex];
		break;

	case 4:
		val = reinterpret_cast<uint32_t*>(buffer)[sampleIndex];
		break;

	case 8:
		val = reinterpret_cast<uint64_t*>(buffer)[sampleIndex];
		break;

	default:
		fprintf(stderr, "Invalid sample size %d.\n", sampleSize);
		return 0;
	}

	val = (val >> (sampleSize * 8 - codecCtx->bits_per_coded_sample));

	// Check which data type is in the sample.
	switch (codecCtx->sample_fmt) {
	case AV_SAMPLE_FMT_U8:
	case AV_SAMPLE_FMT_S16:
	case AV_SAMPLE_FMT_S32:
	case AV_SAMPLE_FMT_U8P:
	case AV_SAMPLE_FMT_S16P:
	case AV_SAMPLE_FMT_S32P:
		// integer => Scale to [-1, 1] and convert to float.
		ret = val / static_cast<float>((1 << (sampleSize * 8 - 1)) - 1);
		break;

	case AV_SAMPLE_FMT_FLT:
	case AV_SAMPLE_FMT_FLTP:
		// float => reinterpret
		ret = *reinterpret_cast<float*>(&val);
		break;

	case AV_SAMPLE_FMT_DBL:
	case AV_SAMPLE_FMT_DBLP:
		// double => reinterpret and then static cast down
		ret = static_cast<float>(*reinterpret_cast<double*>(&val));
		break;

	default:
		fprintf(stderr, "Invalid sample format %s.\n", av_get_sample_fmt_name(codecCtx->sample_fmt));
		return 0;
	}
	return ret;
}