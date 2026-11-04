#include "Generated/Scripts.hpp"

#include "Asset/Sound.hpp"
#include "Asset/Surface.hpp"
#include "AppHandler.hpp"

extern "C"
{
	#include <libavutil/opt.h>
	#include <libavutil/mathematics.h>
	#include <libavcodec/avcodec.h>
	#include <libavformat/avformat.h>
	#include <libswscale/swscale.h>
	#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cstdlib>
#include <cstdio>

#define INPUT_PIXEL_FORMAT					AV_PIX_FMT_RGBA
#define STREAM_VIDEO_PIXEL_FORMAT			AV_PIX_FMT_YUV420P

#define STREAM_AUDIO_BIT_RATE				320000
#define STREAM_AUDIO_SAMPLE_RATE			44100
#define STREAM_AUDIO_FRAME_SIZE				1152
#define STREAM_AUDIO_SAMPLE_FORMAT			AV_SAMPLE_FMT_S16
#define STREAM_AUDIO_SAMPLE_FORMAT_MOVIE	AV_SAMPLE_FMT_FLTP
#define STREAM_AUDIO_CHANNEL_LAYOUT			AV_CH_LAYOUT_STEREO
#define STREAM_AUDIO_CHANNELS				2

namespace CppProject
{
	// Media file output
	AVFormatContext* outContext;

	// Video
	IntType videoWidth, videoHeight, videoBitRate, videoFrameRate;
	AVStream* videoStream;
	AVCodecContext* videoCodecContext;
	IntType videoFrameNum;
	SwsContext* videoSwsContext;
	uchar* videoFrameData = nullptr;

	// Audio
	BoolType audioEnabled;
	AVStream* audioStream;
	AVCodec* audioCodec;
	AVCodecContext* audioCodecContext;
	AVRational audioTimeBase;
	IntType audioFrameNum;

	// Movie sound
	struct MovieSound
	{
		Sound* sound;
		IntType frame, start, end;
		RealType volume, pitch;
	};
	QVector<MovieSound*> movieSounds;

	RealType lib_movie_init()
	{
		// Do nothing
		return 0;
	}

	RealType lib_movie_audio_file_decode(StringType, StringType)
	{
		// Unused
		return 0;
	}

	Sound::Sound(StringType filename) : Asset(ID_Sound)
	{
		AVFormatContext* formatContext = nullptr;
		const AVCodec* codec = nullptr;
		AVCodecContext* codecContext = nullptr;
		SwrContext* swrContext = nullptr;
		AVFrame* decodedFrame = nullptr;
		uint8_t* convertedData = nullptr;
		AVPacket* inPacket = nullptr;

		// Decode file into buffer
		buffer.open(QIODevice::WriteOnly);
		QDataStream outStream(&buffer);

		try
		{
			Timer tmr;
			std::string filenameStd = filename.ToStdString();

			// Get format from audio file
			formatContext = avformat_alloc_context();
			if (avformat_open_input(&formatContext, filenameStd.c_str(), nullptr, nullptr) != 0)
				throw "avformat_open_input failed";

			if (avformat_find_stream_info(formatContext, nullptr) < 0)
				throw "avformat_find_stream_info failed";

			codecContext = avcodec_alloc_context3(nullptr);
			for (IntType i = 0; i < formatContext->nb_streams; i++)
			{
				AVCodecParameters* par = formatContext->streams[i]->codecpar;
				if (par->codec_type == AVMEDIA_TYPE_AUDIO)
				{
					codec = avcodec_find_decoder(par->codec_id);
					if (!codec)
						throw "avcodec_find_decoder failed";
					if (avcodec_parameters_to_context(codecContext, par) < 0)
						throw "avcodec_parameters_to_context failed";
					break;
				}
			}

			if (!codec)
				throw "No codec found";

			// Open codec of the audio file
			if (avcodec_open2(codecContext, codec, nullptr) < 0)
				throw "avcodec_open2 failed";

			// Prepare resampling
			swrContext = swr_alloc();
			if (!swrContext)
				throw "swr_alloc failed";

			av_opt_set_int(swrContext, "in_channel_count", codecContext->channels, 0);
			av_opt_set_int(swrContext, "in_channel_layout", codecContext->channel_layout, 0);
			av_opt_set_int(swrContext, "in_sample_rate", codecContext->sample_rate, 0);
			av_opt_set_sample_fmt(swrContext, "in_sample_fmt", codecContext->sample_fmt, 0);

			av_opt_set_int(swrContext, "out_channel_count", STREAM_AUDIO_CHANNELS, 0);
			av_opt_set_int(swrContext, "out_channel_layout", STREAM_AUDIO_CHANNEL_LAYOUT, 0);
			av_opt_set_int(swrContext, "out_sample_rate", STREAM_AUDIO_SAMPLE_RATE, 0);
			av_opt_set_sample_fmt(swrContext, "out_sample_fmt", STREAM_AUDIO_SAMPLE_FORMAT, 0);

			if (swr_init(swrContext))
				throw "swr_init failed";

			// Create packet
			inPacket = av_packet_alloc();
			if (!inPacket)
				throw "av_packet_alloc failed";

			// Prepare to read data
			decodedFrame = av_frame_alloc();
			if (!decodedFrame)
				throw "av_packet_alloc failed";

			// Read frames and store the decoded buffer in the resample context
			IntType inSamples = 0;
			while (av_read_frame(formatContext, inPacket) >= 0)
			{
				if (avcodec_send_packet(codecContext, inPacket) < 0 ||
					avcodec_receive_frame(codecContext, decodedFrame) < 0)
				{
					av_frame_unref(decodedFrame);
					av_packet_unref(inPacket);
					continue;
				}

				swr_convert(swrContext, nullptr, 0, (const uint8_t**)decodedFrame->data, decodedFrame->nb_samples);
				inSamples += decodedFrame->nb_samples;
			}

			// Allocate data
			if (av_samples_alloc(&convertedData, nullptr, STREAM_AUDIO_CHANNELS, STREAM_AUDIO_FRAME_SIZE, STREAM_AUDIO_SAMPLE_FORMAT, 0) < 0)
				throw "av_samples_alloc failed";

			// Read from the resample context buffer and convert
			RealType rateDiv = codecContext->sample_rate / (RealType)STREAM_AUDIO_SAMPLE_RATE;
			IntType totalSamples = (IntType)(inSamples / rateDiv);
			while (samples < totalSamples)
			{
				// Convert
				IntType outSamples = swr_convert(swrContext, &convertedData, STREAM_AUDIO_FRAME_SIZE, nullptr, 0);
				if (outSamples == 0)
					break;
				else if (outSamples < 0)
					throw "swr_convert failed";
				samples += outSamples;

				// Calculate buffer size
				IntType bufferSize = av_samples_get_buffer_size(nullptr, STREAM_AUDIO_CHANNELS, outSamples, STREAM_AUDIO_SAMPLE_FORMAT, 0);
				if (bufferSize < 0)
					throw "av_samples_get_buffer_size failed";

				outStream.writeRawData((const char*)convertedData, bufferSize);
			}

			tmr.Print("Decoded " + (QString)filename + " into " + NumStr(samples) + " samples");

			// Create OpenAL buffer
			if (App->audioSupported)
			{
				alGenBuffers(1, &alBuffer);
				alBufferData(alBuffer, AL_FORMAT_STEREO16, buffer.data().constData(), buffer.size(), sample_rate_);
			}

			buffer.close();
		}
		catch (const char* err)
		{
			log({ err });
			WARNING(err);
			samples = 0;
			buffer.close();
			buffer.buffer().clear();
		}

		// Cleanup
		if (decodedFrame)
			av_frame_free(&decodedFrame);
		if (swrContext)
			swr_free(&swrContext);
		if (codecContext)
			avcodec_close(codecContext);
		if (formatContext)
		{
			avformat_close_input(&formatContext);
			avformat_free_context(formatContext);
		}
		if (convertedData)
			av_freep(&convertedData);
		if (inPacket)
			av_packet_free(&inPacket);
	}

	Sound::~Sound()
	{
		if (alBuffer)
			alDeleteBuffers(1, &alBuffer);
	}

	RealType lib_movie_set(RealType width, RealType height, RealType bitRate, RealType frameRate, RealType audio)
	{
		videoWidth = width;
		videoHeight = height;
		videoBitRate = bitRate;
		videoFrameRate = frameRate;
		audioEnabled = audio > 0.0;
		return 0;
	}

	RealType lib_movie_start(StringType outFile, StringType outFormat)
	{
		std::string outFileStd   = outFile.ToStdString();
		std::string outFormatStd = outFormat.ToStdString();

		// MI3 web export: "contenedor|perfil[|gopN]" -- el perfil viaja en el mismo
		// string para no tener que declarar una funcion externa nueva (lib_startup).
		std::string containerStd = outFormatStd, profileStd;
		int webGop = 1;
		{
			size_t bar = outFormatStd.find('|');
			if (bar != std::string::npos)
			{
				containerStd = outFormatStd.substr(0, bar);
				profileStd   = outFormatStd.substr(bar + 1);
				size_t bar2  = profileStd.find('|');
				if (bar2 != std::string::npos)
				{
					std::string tail = profileStd.substr(bar2 + 1);
					profileStd = profileStd.substr(0, bar2);
					if (tail.rfind("gop", 0) == 0)
						webGop = std::max(1, atoi(tail.c_str() + 3));
				}
			}
		}
		const bool webMode   = (profileStd == "web" || profileStd == "webscrub");
		const bool scrubMode = (profileStd == "webscrub");

		try
		{
			// Find format
			const AVOutputFormat* outFmt = av_guess_format(containerStd.c_str(), 0, 0);
			if (!outFmt)
				throw "av_guess_format failed";

			// Allocate the output media context. Web mode needs outContext->url
			// set to the real path: "movflags=+faststart" makes the mov muxer
			// reopen the output by URL to shift mdat when relocating moov to
			// the front at trailer time -- with url unset that reopen throws
			// ENOENT ("No such file or directory") and av_write_trailer fails,
			// producing a truncated .mp4 with no moov atom (moov/mdat empirical
			// check on the file: no moov anywhere, front or back). Classic path
			// keeps passing nullptr (bit-identical to before): it never shifts.
			avformat_alloc_output_context2(&outContext, outFmt, 0, webMode ? outFileStd.c_str() : nullptr);
			if (!outContext)
				throw "avformat_alloc_output_context2 failed";

			// Find video encoder
			const AVCodec* videoCodec = nullptr;
			if (webMode)
			{
				// El camino web EXIGE libx264: es el unico encoder del build que consume
				// crf/preset/x264-params. Si no esta, fallar RUIDOSAMENTE en vez de
				// encodear con otros ajustes en silencio (h264_mf ignora el diccionario).
				videoCodec = avcodec_find_encoder_by_name("libx264");
				if (!videoCodec)
					throw "web export: libx264 encoder not available in this build";
			}
			else
				videoCodec = avcodec_find_encoder(outContext->oformat->video_codec);
			if (!videoCodec)
				throw "avcodec_find_encoder failed";

			DEBUG("Encoder: " + QString(videoCodec->long_name));

			// Web: H.264 4:2:0 exige dimensiones PARES. La UI ya lo impide antes de
			// abrir el dialogo, pero el binario no puede fiarse.
			if (webMode && ((videoWidth % 2) != 0 || (videoHeight % 2) != 0))
				throw "web export: width and height must be even (H.264 4:2:0)";

			// Start video stream
			videoStream = avformat_new_stream(outContext, videoCodec);
			if (!videoStream)
				throw "avformat_new_stream failed";

			videoStream->id = 0;
			videoStream->codecpar->width = videoWidth;
			videoStream->codecpar->height = videoHeight;
			videoStream->codecpar->codec_id = videoCodec->id;
			videoStream->codecpar->codec_type = videoCodec->type;
			videoStream->avg_frame_rate = { (int)videoFrameRate, 1 };
			
			// Setup context
			videoCodecContext = avcodec_alloc_context3(videoCodec);
			videoCodecContext->width = videoWidth;
			videoCodecContext->height = videoHeight;
			videoCodecContext->time_base = { 1, (int)videoFrameRate };
			videoCodecContext->framerate = { (int)videoFrameRate, 1 };
			videoCodecContext->pix_fmt = STREAM_VIDEO_PIXEL_FORMAT;
			if (webMode)
			{
				videoCodecContext->gop_size     = (scrubMode ? webGop : (int)videoFrameRate * 2);
				videoCodecContext->keyint_min   = videoCodecContext->gop_size;
				videoCodecContext->max_b_frames = (scrubMode ? 0 : 2);
				videoCodecContext->flags       |= AV_CODEC_FLAG_CLOSED_GOP;
			}
			else
				// Keyframe every ~5s; the old 12-frame GOP wasted bitrate on keyframes
				videoCodecContext->gop_size = (int)videoFrameRate * 5;
			if (outContext->oformat->flags & AVFMT_GLOBALHEADER)
				videoCodecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

			// Tag BT.709 so players interpret colors correctly
			videoCodecContext->color_range = AVCOL_RANGE_MPEG;
			videoCodecContext->color_primaries = AVCOL_PRI_BT709;
			videoCodecContext->color_trc = AVCOL_TRC_BT709;
			videoCodecContext->colorspace = AVCOL_SPC_BT709;

			// Open the codec. x264/x265 get constant-quality CRF with a real
			// lookahead (the old path was ABR + tune=zerolatency: no B-frames,
			// no lookahead, quality far below what the bitrate paid for);
			// other codecs keep the bitrate target.
			AVDictionary* opt = NULL;
			bool crfCodec = (videoCodecContext->codec_id == AV_CODEC_ID_H264 ||
							 videoCodecContext->codec_id == AV_CODEC_ID_HEVC);
			char x264params[256] = { 0 };
			if (webMode)
			{
				// Calidad IDENTICA al MP4 clasico (crf 16): el modo web no puede salir
				// peor que el export de siempre. Lo que cambia es la ENTREGA.
				av_dict_set(&opt, "crf", "16", 0);
				av_dict_set(&opt, "preset", "slow", 0);
				av_dict_set(&opt, "profile", "high", 0);
				snprintf(x264params, sizeof(x264params),
						 "keyint=%d:min-keyint=%d:scenecut=0:open-gop=0:bframes=%d",
						 (int)videoCodecContext->gop_size, (int)videoCodecContext->gop_size,
						 scrubMode ? 0 : 2);
				av_dict_set(&opt, "x264-params", x264params, 0);   // se aplica DESPUES de las AVOptions: gana
			}
			else if (crfCodec)
			{
				av_dict_set(&opt, "crf", "16", 0);
				av_dict_set(&opt, "preset", "slow", 0);
			}
			else
				videoCodecContext->bit_rate = videoBitRate * 5;
			if (avcodec_open2(videoCodecContext, videoCodec, &opt) < 0)
				throw "avcodec_open2 failed";

			// avcodec_open2 NO falla por opciones desconocidas: las deja en el diccionario.
			// Loguearlas es la unica prueba de que crf/preset/x264-params se consumieron.
			{
				AVDictionaryEntry* e = nullptr;
				while ((e = av_dict_get(opt, "", e, AV_DICT_IGNORE_SUFFIX)))
				{
					std::string ign = std::string("MI3 movie encoder: IGNORED option ") + e->key + "=" + e->value;
					log({ ign.c_str() });
				}
			}
			av_dict_free(&opt);

			videoFrameNum = 0;
			avcodec_parameters_from_context(videoStream->codecpar, videoCodecContext);

			// Scaling context
			// Web: el submuestreo 4:2:0 es obligatorio para decodificar en HW, asi que la
			// unica palanca de calidad que queda es COMO se submuestrea el croma. Con
			// interpolacion completa de croma los bordes duros de Minecraft no fringean.
			const int swsFlags = webMode
				? (SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP)
				: SWS_BILINEAR;
			videoSwsContext = sws_getContext(videoWidth, videoHeight, INPUT_PIXEL_FORMAT,
											 videoWidth, videoHeight, STREAM_VIDEO_PIXEL_FORMAT,
											 swsFlags, nullptr, nullptr, nullptr);
			if (!videoSwsContext)
				throw "sws_getContext failed";

			// RGB(full) -> YUV(limited) with BT.709 coefficients, matching the
			// stream tags above (default was BT.601: visibly shifted colors)
			sws_setColorspaceDetails(videoSwsContext,
									 sws_getCoefficients(SWS_CS_ITU709), 1,
									 sws_getCoefficients(SWS_CS_ITU709), 0,
									 0, 1 << 16, 1 << 16);

			deleteArrayAndReset(videoFrameData);
			videoFrameData = new uchar[videoWidth * videoHeight * 4];

			if (audioEnabled)
			{
				// Find audio encoder
				const AVCodec* audioCodec = avcodec_find_encoder(outContext->oformat->audio_codec);
				if (!audioCodec)
					throw "avcodec_find_encoder failed";

				// Start audio stream
				audioStream = avformat_new_stream(outContext, audioCodec);
				if (!audioStream)
					throw "avformat_new_stream failed";
				audioStream->id = 1;
				audioStream->codecpar->sample_rate = STREAM_AUDIO_SAMPLE_RATE;
				audioStream->codecpar->frame_size = STREAM_AUDIO_FRAME_SIZE;
				audioStream->codecpar->codec_id = audioCodec->id;
				audioStream->codecpar->codec_type = audioCodec->type;

				// Setup context
				audioCodecContext = avcodec_alloc_context3(audioCodec);
				audioCodecContext->sample_fmt = STREAM_AUDIO_SAMPLE_FORMAT_MOVIE;
				audioCodecContext->sample_rate = STREAM_AUDIO_SAMPLE_RATE;
				audioCodecContext->bit_rate = STREAM_AUDIO_BIT_RATE;
				audioCodecContext->channels = STREAM_AUDIO_CHANNELS;
				audioCodecContext->channel_layout = STREAM_AUDIO_CHANNEL_LAYOUT;
				if (outContext->oformat->flags & AVFMT_GLOBALHEADER)
					audioCodecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

				// Open the codec
				if (avcodec_open2(audioCodecContext, audioCodec, nullptr) < 0)
					throw "avcodec_open2 failed";

				audioFrameNum = 0;
				audioTimeBase = { audioCodecContext->frame_size, STREAM_AUDIO_SAMPLE_RATE };
				avcodec_parameters_from_context(audioStream->codecpar, audioCodecContext);
			}

			// Open the output file. Web mode needs READ_WRITE, not just WRITE:
			// "movflags=+faststart" makes the mov muxer relocate moov to the
			// front at av_write_trailer time by seeking back and re-reading
			// the mdat bytes it already wrote -- with a write-only handle that
			// seek/read silently fails av_write_trailer (moov never gets
			// written at all, not even at the end: seen empirically -- .mp4
			// truncated to ftyp+free+mdat, no moov anywhere). Classic path is
			// untouched (still WRITE-only, bit-identical to before).
			int avioFlags = webMode ? AVIO_FLAG_READ_WRITE : AVIO_FLAG_WRITE;
			if (avio_open(&outContext->pb, outFileStd.c_str(), avioFlags) < 0)
				throw "avio_open failed";

			// Write the stream header, if any
			AVDictionary* muxOpt = NULL;
			int webTimescale = 0;
			if (webMode)
			{
				// moov delante: el navegador puede buscar sin descargar el fichero entero.
				av_dict_set(&muxOpt, "movflags", "+faststart", 0);
				// Timescale del track: 90000 es el estandar, pero solo si divide exacto al
				// fps (si no, las duraciones por frame se vuelven irregulares y el
				// currentTime del navegador cae entre frames). Si no divide, fps*1000 da
				// exactamente 1000 ticks por frame.
				webTimescale = ((int)videoFrameRate > 0 && (90000 % (int)videoFrameRate) == 0)
							 ? 90000 : (int)videoFrameRate * 1000;
				char tsbuf[16];
				snprintf(tsbuf, sizeof(tsbuf), "%d", webTimescale);
				av_dict_set(&muxOpt, "video_track_timescale", tsbuf, 0);
			}
			if (avformat_write_header(outContext, &muxOpt) < 0)
				throw "avformat_write_header failed";
			{
				AVDictionaryEntry* e = nullptr;
				while ((e = av_dict_get(muxOpt, "", e, AV_DICT_IGNORE_SUFFIX)))
				{
					std::string ign = std::string("MI3 movie muxer: IGNORED option ") + e->key + "=" + e->value;
					log({ ign.c_str() });
				}
			}
			av_dict_free(&muxOpt);

			// MI3 movie encoder: linea canonica de log, objetivo de las aserciones
			// del arnes headless (movietest, §6 del spec de video web).
			{
				char line[512];
				snprintf(line, sizeof(line),
					"MI3 movie encoder: profile=%s encoder=%s container=%s size=%dx%d fps=%d "
					"gop=%d bframes=%d crf=%s faststart=%d timescale=%d pixfmt=yuv420p",
					(profileStd.empty() ? "classic" : profileStd.c_str()),
					videoCodec->name, containerStd.c_str(),
					(int)videoWidth, (int)videoHeight, (int)videoFrameRate,
					(int)videoCodecContext->gop_size, (int)videoCodecContext->max_b_frames,
					(webMode || crfCodec) ? "16" : "n/a",
					webMode ? 1 : 0, webTimescale);
				log({ line });
			}

			return 0;
		}
		catch (const char* err)
		{
			log({ err });
			WARNING(err);
			return -1;
		}
	}

	RealType lib_movie_audio_file_add(StringType)
	{
		if (Sound* sound = FindSound(global::_app->exportmovie_current_sound))
			return sound->id;

		return -1;
	}

	RealType lib_movie_audio_sound_add(RealType id, RealType time, RealType volume, RealType pitch, RealType start, RealType end)
	{
		if (!audioEnabled)
			return 0.0;

		movieSounds.append(
			new MovieSound({
				FindSound(id),
				av_rescale_q(time * 1000, { 1, 1000 }, audioTimeBase),
				av_rescale_q(start * 1000, { 1, 1000 }, audioTimeBase),
				av_rescale_q(end * 1000, { 1, 1000 }, audioTimeBase),
				volume,
				pitch
			})
		);
		return 0.0;
	}

	void Encode(AVCodecContext* context, AVFrame* frame, AVStream* stream)
	{
		AVPacket* packet = av_packet_alloc();
		if (!packet)
			throw "av_packet_alloc failed";

		// Send in a frame, a nullptr will enable flush mode
		if (avcodec_send_frame(context, frame) < 0)
			throw "avcodec_send_frame failed";

		// Encode a submitted video or audio frame to the stream
		while (true)
		{
			switch (avcodec_receive_packet(context, packet))
			{
				case 0:
				{
					av_packet_rescale_ts(packet, context->time_base, stream->time_base);
					packet->stream_index = stream->index;

					if (av_interleaved_write_frame(outContext, packet) < 0)
						throw "av_interleaved_write_frame failed";
					break;
				}
				case AVERROR(EAGAIN):
				case AVERROR_EOF:
					av_packet_free(&packet);
					return;

				default:
					throw "avcodec_receive_packet failed";
			}
		}

		av_packet_free(&packet);
	}

	RealType lib_movie_frame(StringType)
	{
		try
		{
			// Allocate and init frame
			AVFrame* videoFrame = av_frame_alloc();
			if (!videoFrame)
				throw "av_frame_alloc failed";

			videoFrame->pts = videoFrameNum++;
			videoFrame->format = videoCodecContext->pix_fmt;
			videoFrame->width = videoCodecContext->width;
			videoFrame->height = videoCodecContext->height;

			if (av_frame_get_buffer(videoFrame, 0) < 0)
				throw "av_frame_get_buffer failed";

			// Convert surface into frame
			FindSurface(global::_app->export_surface)->frameBuffer->CopyColorData(videoFrameData);
			uint8_t* inData[1] = { videoFrameData }; // RGBA have one plane
			int inLinesize[1] = { 4 * (int)videoWidth }; // RGBA stride
			sws_scale(videoSwsContext, inData, inLinesize, 0, videoHeight, videoFrame->data, videoFrame->linesize);
			
			// Submit frame and encode
			Encode(videoCodecContext, videoFrame, videoStream);

			// Cleanup video
			av_frame_free(&videoFrame);

			// Write interleaved audio
			while (audioEnabled && av_compare_ts(videoFrameNum, videoCodecContext->time_base, audioFrameNum, audioTimeBase) > 0)
			{
				// Allocate frame
				AVFrame* audioFrame = av_frame_alloc();
				if (!audioFrame)
					throw "av_frame_alloc failed";

				audioFrame->nb_samples = audioCodecContext->frame_size;
				audioFrame->format = STREAM_AUDIO_SAMPLE_FORMAT_MOVIE;
				audioFrame->channel_layout = STREAM_AUDIO_CHANNEL_LAYOUT;
				audioFrame->channels = STREAM_AUDIO_CHANNELS;
				audioFrame->sample_rate = STREAM_AUDIO_SAMPLE_RATE;

				if (av_frame_get_buffer(audioFrame, 0) < 0)
					throw "av_frame_get_buffer failed";

				if (av_frame_make_writable(audioFrame) < 0)
					throw "av_frame_make_writable failed";

				// Find sounds
				QVector<MovieSound*> frameSounds;
				for (MovieSound* sound : movieSounds)
					if (audioFrameNum >= sound->frame &&
						audioFrameNum < sound->frame + sound->sound->samples / audioCodecContext->frame_size / sound->pitch - sound->start + sound->end)
						frameSounds.append(sound);

				// Write to frame (mix sounds)
				for (IntType c = 0; c < 2; c++)
				{
					float* dstData = (float*)audioFrame->data[c];
					for (IntType s = 0; s < audioCodecContext->frame_size; s++)
					{
						float dstVal = 0.f; // 0 = silence

						for (MovieSound* sound : frameSounds)
						{
							const int16_t* srcData = (int16_t*)sound->sound->buffer.data().constData();
							IntType srcSample = ((((audioFrameNum - sound->frame + sound->start) * audioCodecContext->frame_size) % (uint64_t)(sound->sound->samples / sound->pitch)) + s) * sound->pitch;

							// Add and clamp audio if in buffer range
							if (srcSample < sound->sound->samples)
							{
								float srcVal = (float)srcData[srcSample * 2 + c] / sample_max;
								dstVal = std::clamp(dstVal + srcVal * (float)sound->volume, -1.f, 1.f);
							}
						}

						dstData[s] = dstVal;
					}
				}

				audioFrame->pts = av_rescale_q(audioFrameNum++, audioTimeBase, audioCodecContext->time_base);

				// Submit frame and encode
				Encode(audioCodecContext, audioFrame, audioStream);

				// Cleanup audio
				av_frame_free(&audioFrame);
			}

			return 0;
		}
		catch (const char* err)
		{
			log({ err });
			WARNING(err);
			return -1;
		}
	}

	RealType lib_movie_done()
	{
		try
		{
			// Flush video & audio
			Encode(videoCodecContext, nullptr, videoStream);
			if (audioEnabled)
				Encode(audioCodecContext, nullptr, audioStream);

			// Write the trailer
			int trailerRet = av_write_trailer(outContext);
			if (trailerRet < 0)
			{
				char errbuf[128] = { 0 };
				av_strerror(trailerRet, errbuf, sizeof(errbuf));
				std::string diag = std::string("av_write_trailer failed: ") + errbuf + " (code " + std::to_string(trailerRet) + ")";
				log({ diag.c_str() });
				throw "av_write_trailer failed";
			}

			// Close video
			sws_freeContext(videoSwsContext);
			avcodec_close(videoCodecContext);

			// Close audio
			if (audioEnabled)
			{
				avcodec_close(audioCodecContext);

				for (MovieSound* snd : movieSounds)
					delete snd;
				movieSounds.clear();
			}

			// Close the output file.
			if (avio_close(outContext->pb) < 0)
				throw "avio_close failed";

			// Free the stream
			avformat_free_context(outContext);

			deleteArrayAndReset(videoFrameData);

			return 0;
		}
		catch (const char* err)
		{
			log({ err });
			WARNING(err);
			return -1;
		}
	}

}