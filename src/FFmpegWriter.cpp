/*
 * This file is originally based on the Libavformat API example, and then modified
 * by the libopenshot project.
 *
 * Copyright (c) 2003 Fabrice Bellard, OpenShot Studios, LLC
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

#include "../include/FFmpegWriter.h"

using namespace openshot;

FFmpegWriter::FFmpegWriter(string path) throw (InvalidFile, InvalidFormat, InvalidCodec, InvalidOptions, OutOfMemory) :
		path(path), fmt(NULL), oc(NULL), audio_st(NULL), video_st(NULL), audio_pts(0), video_pts(0), samples(NULL),
		audio_outbuf(NULL), audio_outbuf_size(0), audio_input_frame_size(0), audio_input_position(0),
		converted_audio(NULL), initial_audio_input_frame_size(0), resampler(NULL)
{

	// Init FileInfo struct (clear all values)
	InitFileInfo();

	// Disable audio & video (so they can be independently enabled)
	info.has_audio = false;
	info.has_video = false;

	// Initialize FFMpeg, and register all formats and codecs
	av_register_all();

	// auto detect format
	auto_detect_format();
}

// auto detect format (from path)
void FFmpegWriter::auto_detect_format()
{
	// Auto detect the output format from the name. default is mpeg.
	fmt = av_guess_format(NULL, path.c_str(), NULL);
	if (!fmt)
		throw InvalidFormat("Could not deduce output format from file extension.", path);

	// Allocate the output media context
	oc = avformat_alloc_context();
	if (!oc)
		throw OutOfMemory("Could not allocate memory for AVFormatContext.", path);

	// Set the AVOutputFormat for the current AVFormatContext
	oc->oformat = fmt;

	// Update codec names
	if (fmt->video_codec != CODEC_ID_NONE)
		// Update video codec name
		info.vcodec = avcodec_find_encoder(fmt->video_codec)->name;

	if (fmt->audio_codec != CODEC_ID_NONE)
		// Update audio codec name
		info.acodec = avcodec_find_encoder(fmt->audio_codec)->name;
}

// initialize streams
void FFmpegWriter::initialize_streams()
{
	// Add the audio and video streams using the default format codecs and initialize the codecs
	video_st = NULL;
	audio_st = NULL;
	if (fmt->video_codec != CODEC_ID_NONE && info.has_video)
		// Add video stream
		video_st = add_video_stream();

	if (fmt->audio_codec != CODEC_ID_NONE && info.has_audio)
		// Add audio stream
		audio_st = add_audio_stream();
}

// Set video export options
void FFmpegWriter::SetVideoOptions(bool has_video, string codec, Fraction fps, int width, int height,
		Fraction pixel_ratio, bool interlaced, bool top_field_first, int bit_rate)
{
	// Set the video options
	if (codec.length() > 0)
	{
		AVCodec *new_codec = avcodec_find_encoder_by_name(codec.c_str());
		if (new_codec == NULL)
			throw InvalidCodec("A valid audio codec could not be found for this file.", path);
		else {
			// Set video codec
			info.vcodec = new_codec->name;

			// Update video codec in fmt
			fmt->video_codec = new_codec->id;
		}
	}
	if (fps.num > 0)
	{
		// Set frames per second (if provided)
		info.fps.num = fps.num;
		info.fps.den = fps.den;

		// Set the timebase (inverse of fps)
		info.video_timebase.num = info.fps.den;
		info.video_timebase.den = info.fps.num;
	}
	if (width >= 1)
		info.width = width;
	if (height >= 1)
		info.height = height;
	if (pixel_ratio.num > 0)
	{
		info.pixel_ratio.num = pixel_ratio.num;
		info.pixel_ratio.den = pixel_ratio.den;
	}
	if (bit_rate >= 1000)
		info.video_bit_rate = bit_rate;

	info.interlaced_frame = interlaced;
	info.top_field_first = top_field_first;

	// Calculate the DAR (display aspect ratio)
	Fraction size(info.width * info.pixel_ratio.num, info.height * info.pixel_ratio.den);

	// Reduce size fraction
	size.Reduce();

	// Set the ratio based on the reduced fraction
	info.display_ratio.num = size.num;
	info.display_ratio.den = size.den;

	// Enable / Disable video
	info.has_video = has_video;
}

// Set audio export options
void FFmpegWriter::SetAudioOptions(bool has_audio, string codec, int sample_rate, int channels, int bit_rate)
{
	// Set audio options
	if (codec.length() > 0)
	{
		AVCodec *new_codec = avcodec_find_encoder_by_name(codec.c_str());
		if (new_codec == NULL)
			throw InvalidCodec("A valid audio codec could not be found for this file.", path);
		else
		{
			// Set audio codec
			info.acodec = new_codec->name;

			// Update audio codec in fmt
			fmt->audio_codec = new_codec->id;
		}
	}
	if (sample_rate > 7999)
		info.sample_rate = sample_rate;
	if (channels > 0)
		info.channels = channels;
	if (bit_rate > 999)
		info.audio_bit_rate = bit_rate;

	// Enable / Disable audio
	info.has_audio = has_audio;
}

// Set custom options (some codecs accept additional params)
void FFmpegWriter::SetOption(Stream_Type stream, string name, string value)
{
	// Declare codec context
	AVCodecContext *c = NULL;
	stringstream convert(value);

	if (info.has_video && stream == VIDEO_STREAM)
		c = video_st->codec;
	else if (info.has_audio && stream == AUDIO_STREAM)
		c = audio_st->codec;

	// Init AVOption
	const AVOption *option = NULL;

	// Was a codec / stream found?
	if (c)
		// Find AVOption (if it exists)
		option = av_find_opt(c->priv_data, name.c_str(), NULL, NULL, NULL);

	// Was option found?
	if (option || (name == "g" || name == "qmin" || name == "qmax" || name == "max_b_frames" || name == "mb_decision"))
	{
		// Check for specific named options
		if (name == "g")
			// Set gop_size
			convert >> c->gop_size;

		else if (name == "qmin")
			// minimum quantizer
			convert >> c->qmin;

		else if (name == "qmax")
			// maximum quantizer
			convert >> c->qmax;

		else if (name == "max_b_frames")
			// maximum number of B-frames between non-B-frames
			convert >> c->max_b_frames;

		else if (name == "mb_decision")
			// macroblock decision mode
			convert >> c->mb_decision;

		else
			// Set AVOption
			av_set_string3 (c->priv_data, name.c_str(), value.c_str(), 0, NULL);
	}
	else
		throw InvalidOptions("The option is not valid for this codec.", path);

}

// Prepare & initialize streams and open codecs
void FFmpegWriter::PrepareStreams()
{
	if (!info.has_audio && !info.has_video)
		throw InvalidOptions("No video or audio options have been set.  You must set has_video or has_audio (or both).", path);

	// Initialize the streams (i.e. add the streams)
	initialize_streams();

	// Now that all the parameters are set, we can open the audio and video codecs and allocate the necessary encode buffers
	if (info.has_video && video_st)
		open_video(oc, video_st);
	if (info.has_audio && audio_st)
		open_audio(oc, audio_st);
}

// Write the file header (after the options are set)
void FFmpegWriter::WriteHeader()
{
	if (!info.has_audio && !info.has_video)
		throw InvalidOptions("No video or audio options have been set.  You must set has_video or has_audio (or both).", path);

	// Open the output file, if needed
	if (!(fmt->flags & AVFMT_NOFILE)) {
		if (avio_open(&oc->pb, path.c_str(), AVIO_FLAG_WRITE) < 0)
			throw InvalidFile("Could not open or write file.", path);
	}

	// Write the stream header, if any
	// TODO: add avoptions / parameters instead of NULL
	avformat_write_header(oc, NULL);
}

// Write a single frame
void FFmpegWriter::WriteFrame(Frame* frame)
{
	// Encode and add the frame to the output file
	if (info.has_video && video_st)
		write_video_packet(frame);

	if (info.has_audio && audio_st)
		write_audio_packet(frame);
}

// Write a block of frames from a reader
void FFmpegWriter::WriteFrame(FileReaderBase* reader, int start, int length)
{
	// Loop through each frame (and encoded it)
	for (int number = start; number <= length; number++)
	{
		// Get the frame
		Frame f = reader->GetFrame(number);

		// Encode frame
		WriteFrame(&f);
	}
}

// Write the file trailer (after all frames are written)
void FFmpegWriter::WriteTrailer()
{
	/* write the trailer, if any.  the trailer must be written
	 * before you close the CodecContexts open when you wrote the
	 * header; otherwise write_trailer may try to use memory that
	 * was freed on av_codec_close() */
	av_write_trailer(oc);
}

// Close the video codec
void FFmpegWriter::close_video(AVFormatContext *oc, AVStream *st)
{
	avcodec_close(st->codec);

	//av_free(picture->data[0]);
	//av_free(picture);
	//if (tmp_picture) {
	//    av_free(tmp_picture->data[0]);
	//    av_free(tmp_picture);
	//}
	//av_free(video_outbuf);
}

// Close the audio codec
void FFmpegWriter::close_audio(AVFormatContext *oc, AVStream *st)
{
	avcodec_close(st->codec);

	delete[] samples;
	delete[] audio_outbuf;
	delete[] converted_audio;

	delete resampler;
}

// Close the writer
void FFmpegWriter::Close()
{
	// Close each codec
	if (video_st)
		close_video(oc, video_st);
	if (audio_st)
		close_audio(oc, audio_st);

	// Free the streams
	for (int i = 0; i < oc->nb_streams; i++) {
		av_freep(&oc->streams[i]->codec);
		av_freep(&oc->streams[i]);
	}

	if (!(fmt->flags & AVFMT_NOFILE)) {
		/* close the output file */
		avio_close(oc->pb);
	}

	// Free the stream
	av_free(oc);
}

// Add an audio output stream
AVStream* FFmpegWriter::add_audio_stream()
{
	AVCodecContext *c;
	AVStream *st;

	// Find the audio codec
	AVCodec *codec = avcodec_find_encoder_by_name(info.acodec.c_str());
	if (codec == NULL)
		throw InvalidCodec("A valid audio codec could not be found for this file.", path);

	// Create a new audio stream
	st = avformat_new_stream(oc, codec);
	if (!st)
		throw OutOfMemory("Could not allocate memory for the audio stream.", path);

	c = st->codec;
	c->codec_id = codec->id;
	c->codec_type = AVMEDIA_TYPE_AUDIO;

	// Set the sample parameters
	c->bit_rate = info.audio_bit_rate;
	c->channels = info.channels;

	// Check for valid timebase
	if (c->time_base.den == 0 || c->time_base.num == 0)
	{
		c->time_base.num = st->time_base.num;
		c->time_base.den = st->time_base.den;
	}

	// Set valid sample rate (or throw error)
	if (codec->supported_samplerates) {
		int i;
	    for (i = 0; codec->supported_samplerates[i] != 0; i++)
	        if (info.sample_rate == codec->supported_samplerates[i])
	        {
	        	// Set the valid sample rate
	        	c->sample_rate = info.sample_rate;
	            break;
	        }
	    if (codec->supported_samplerates[i] == 0)
	    	throw InvalidSampleRate("An invalid sample rate was detected for this codec.", path);
	} else
		// Set sample rate
		c->sample_rate = info.sample_rate;


	// Set a valid number of channels (or throw error)
	int channel_layout = info.channels == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
	if (codec->channel_layouts) {
		int i;
		for (i = 0; codec->channel_layouts[i] != 0; i++)
			if (channel_layout == codec->channel_layouts[i])
			{
				// Set valid channel layout
				c->channel_layout = channel_layout;
				break;
			}
		if (codec->channel_layouts[i] == 0)
			throw InvalidChannels("An invalid channel layout was detected (i.e. MONO / STEREO).", path);
	} else
		// Set valid channel layout
		c->channel_layout = channel_layout;

	// Choose a valid sample_fmt
	if (codec->sample_fmts) {
		for (int i = 0; codec->sample_fmts[i] != AV_SAMPLE_FMT_NONE; i++)
		{
			// Set sample format to 1st valid format (and then exit loop)
			c->sample_fmt = codec->sample_fmts[i];
			break;
		}
	}
	if (c->sample_fmt == AV_SAMPLE_FMT_NONE) {
		// Default if no sample formats found
		c->sample_fmt = AV_SAMPLE_FMT_S16;
	}

	// some formats want stream headers to be separate
	if (oc->oformat->flags & AVFMT_GLOBALHEADER)
		c->flags |= CODEC_FLAG_GLOBAL_HEADER;

	return st;
}

// Add a video output stream
AVStream* FFmpegWriter::add_video_stream()
{
	AVCodecContext *c;
	AVStream *st;

	// Find the audio codec
	AVCodec *codec = avcodec_find_encoder_by_name(info.vcodec.c_str());
	if (codec == NULL)
		throw InvalidCodec("A valid video codec could not be found for this file.", path);

	// Create a new stream
	st = avformat_new_stream(oc, codec);
	if (!st)
		throw OutOfMemory("Could not allocate memory for the video stream.", path);

	c = st->codec;
	c->codec_id = codec->id;
	c->codec_type = AVMEDIA_TYPE_VIDEO;

	/* put sample parameters */
	c->bit_rate = info.video_bit_rate;
	/* resolution must be a multiple of two */
	// TODO: require /2 height and width
	c->width = info.width;
	c->height = info.height;

	/* time base: this is the fundamental unit of time (in seconds) in terms
	 of which frame timestamps are represented. for fixed-fps content,
	 timebase should be 1/framerate and timestamp increments should be
	 identically 1. */
	c->time_base.den = info.video_timebase.den;
	c->time_base.num = info.video_timebase.num;
	c->gop_size = 12; /* TODO: add this to "info"... emit one intra frame every twelve frames at most */
	c->pix_fmt = PIX_FMT_YUV420P;
	if (c->codec_id == CODEC_ID_MPEG2VIDEO) {
		/* just for testing, we also add B frames */
		c->max_b_frames = 2;
	}
	if (c->codec_id == CODEC_ID_MPEG1VIDEO) {
		/* Needed to avoid using macroblocks in which some coeffs overflow.
		 This does not happen with normal video, it just happens here as
		 the motion of the chroma plane does not match the luma plane. */
		c->mb_decision = 2;
	}
	// some formats want stream headers to be separate
	if (oc->oformat->flags & AVFMT_GLOBALHEADER)
		c->flags |= CODEC_FLAG_GLOBAL_HEADER;

	return st;
}

// open audio codec
void FFmpegWriter::open_audio(AVFormatContext *oc, AVStream *st)
{
	AVCodecContext *c;
	AVCodec *codec;

	c = st->codec;

	// Find the audio encoder
	codec = avcodec_find_encoder(c->codec_id);
	if (!codec)
		throw InvalidCodec("Could not find codec", path);

	// Open the codec
	if (avcodec_open2(c, codec, NULL) < 0)
		throw InvalidCodec("Could not open codec", path);

	// Calculate the size of the input frame (i..e how many samples per packet), and the output buffer
	// TODO: Ugly hack for PCM codecs (will be removed ASAP with new PCM support to compute the input frame size in samples
	if (c->frame_size <= 1) {
		// No frame size found... so calculate
		audio_input_frame_size = 50000 / info.channels;

		switch (st->codec->codec_id) {
		case CODEC_ID_PCM_S16LE:
		case CODEC_ID_PCM_S16BE:
		case CODEC_ID_PCM_U16LE:
		case CODEC_ID_PCM_U16BE:
			audio_input_frame_size >>= 1;
			break;
		default:
			break;
		}
	} else {
		// Set frame size based on the codec
		audio_input_frame_size = c->frame_size * info.channels;
	}

	// Set the initial frame size (since it might change during resampling)
	initial_audio_input_frame_size = audio_input_frame_size;

	// Allocate array for samples
	samples = new int16_t[AVCODEC_MAX_AUDIO_FRAME_SIZE + FF_INPUT_BUFFER_PADDING_SIZE];

	// Set audio output buffer (used to store the encoded audio)
	audio_outbuf_size = AVCODEC_MAX_AUDIO_FRAME_SIZE + FF_INPUT_BUFFER_PADDING_SIZE;
	audio_outbuf = new uint8_t[audio_outbuf_size];

	// create a new array (to hold the re-sampled audio)
	converted_audio = new int16_t[AVCODEC_MAX_AUDIO_FRAME_SIZE + FF_INPUT_BUFFER_PADDING_SIZE];
}

// open video codec
void FFmpegWriter::open_video(AVFormatContext *oc, AVStream *st)
{
	AVCodec *codec;
	AVCodecContext *c;

	c = st->codec;

	/* find the video encoder */
	codec = avcodec_find_encoder(c->codec_id);
	if (!codec)
		throw InvalidCodec("Could not find codec", path);

	/* open the codec */
	if (avcodec_open2(c, codec, NULL) < 0)
		throw InvalidCodec("Could not open codec", path);
}

// write audio frame
void FFmpegWriter::write_audio_packet(Frame* frame)
{
	// Get the codec
	AVCodecContext *c;
	c = audio_st->codec;

	// Create a resampler (only once)
	if (!resampler)
		resampler = new AudioResampler();

	// Get the audio details from this frame
	int sample_rate_in_frame = info.sample_rate; // resampling happens when getting the interleaved audio samples below
	int samples_in_frame = frame->GetAudioSamplesCount(); // this is updated if resampling happens
	int channels_in_frame = frame->GetAudioChannelsCount();

	// Get audio sample array
	float* frame_samples_float = frame->GetInterleavedAudioSamples(info.sample_rate, resampler, &samples_in_frame);
	int16_t* frame_samples = new int16_t[AVCODEC_MAX_AUDIO_FRAME_SIZE + FF_INPUT_BUFFER_PADDING_SIZE];
	int samples_position = 0;

	// Calculate total samples
	int total_frame_samples = samples_in_frame * channels_in_frame;
	int remaining_frame_samples = total_frame_samples;

	// Translate audio sample values back to 16 bit integers
	for (int s = 0; s < total_frame_samples; s++)
		// Translate sample value and copy into buffer
		frame_samples[s] = int(frame_samples_float[s] * (1 << 15));

	// Re-sample audio samples (into additinal channels or changing the sample format / number format)
	// The sample rate has already been resampled using the GetInterleavedAudioSamples method.
	if (c->sample_fmt != AV_SAMPLE_FMT_S16 || info.channels != channels_in_frame) {

		// Audio needs to be converted
		// Create an audio resample context object (used to convert audio samples)
		ReSampleContext *resampleCtx = av_audio_resample_init(
				info.channels, channels_in_frame,
				info.sample_rate, sample_rate_in_frame,
				c->sample_fmt, AV_SAMPLE_FMT_S16, 0, 0, 0, 0.0f);

		if (!resampleCtx)
			throw ResampleError("Failed to resample & convert audio samples for encoding.", path);
		else {
			// FFmpeg audio resample & sample format conversion
			audio_resample(resampleCtx, (short *) converted_audio, (short *) frame_samples, total_frame_samples);

			// Update total frames & input frame size (due to bigger or smaller data types)
			total_frame_samples *= (av_get_bytes_per_sample(c->sample_fmt) / av_get_bytes_per_sample(AV_SAMPLE_FMT_S16));
			audio_input_frame_size = initial_audio_input_frame_size * (av_get_bytes_per_sample(c->sample_fmt) / av_get_bytes_per_sample(AV_SAMPLE_FMT_S16));

			// Set remaining samples
			remaining_frame_samples = total_frame_samples;

			// Copy audio samples over original samples
			memcpy(frame_samples, converted_audio, total_frame_samples * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16));

			// Close context
			audio_resample_close(resampleCtx);
		}
	}

	// Loop until no more samples
	while (remaining_frame_samples > 0) {
		// Get remaining samples needed for this packet
		int remaining_packet_samples = audio_input_frame_size - audio_input_position;

		// Determine how many samples we need
		int diff = 0;
		if (remaining_frame_samples >= remaining_packet_samples)
			diff = remaining_packet_samples;
		else if (remaining_frame_samples < remaining_packet_samples)
			diff = remaining_frame_samples;

		// Copy frame samples into the packet samples array
		memcpy(samples + audio_input_position, frame_samples + samples_position, diff * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16));

		// Increment counters
		audio_input_position += diff;
		samples_position += diff;
		remaining_frame_samples -= diff;
		remaining_packet_samples -= diff;

		// Do we have enough samples to proceed?
		if (audio_input_position < audio_input_frame_size)
			// Not enough samples to encode... so wait until the next frame
			break;

		// Init the packet
		AVPacket pkt;
		av_init_packet(&pkt);

		// Encode audio data
		pkt.size = avcodec_encode_audio(c, audio_outbuf, audio_outbuf_size, (short *) samples);

		if (c->coded_frame && c->coded_frame->pts != AV_NOPTS_VALUE)
			pkt.pts = c->coded_frame->pts;
		pkt.flags |= AV_PKT_FLAG_KEY;
		pkt.stream_index = audio_st->index;
		pkt.data = audio_outbuf;

		/* write the compressed frame in the media file */
		int averror = av_interleaved_write_frame(oc, &pkt);
		if (averror != 0)
		{
			//string error_description = av_err2str(averror);
			throw ErrorEncodingAudio("Error while writing audio frame", frame->number);
		}

		// deallocate memory for packet
		av_free_packet(&pkt);

		// Reset position
		audio_input_position = 0;
	}

	// Delete arrays
	delete[] frame_samples;
	delete[] frame_samples_float;

}

// Allocate an AVFrame object
AVFrame* FFmpegWriter::allocate_avframe(PixelFormat pix_fmt, int width, int height, int *buffer_size)
{
	// Create an RGB AVFrame
	AVFrame *new_av_frame = NULL;
	uint8_t *new_buffer = NULL;

	// Allocate an AVFrame structure
	new_av_frame = avcodec_alloc_frame();
	if (new_av_frame == NULL)
		throw OutOfMemory("Could not allocate AVFrame", path);

	// Determine required buffer size and allocate buffer
	*buffer_size = avpicture_get_size(pix_fmt, width, height);
	new_buffer = new uint8_t[*buffer_size];

	// Attach buffer to AVFrame
	avpicture_fill((AVPicture *)new_av_frame, new_buffer, pix_fmt, width, height);

	// return AVFrame
	return new_av_frame;
}

// write video frame
void FFmpegWriter::write_video_packet(Frame* frame)
{
	// Get the codec
	AVCodecContext *c;
	c = video_st->codec;

	// Allocate an RGB frame & final output frame
	int bytes_source = 0;
	int bytes_final = 0;
	AVFrame *frame_source = allocate_avframe(PIX_FMT_RGB24, frame->GetWidth(), frame->GetHeight(), &bytes_source);
	AVFrame *frame_final = allocate_avframe(c->pix_fmt, info.width, info.height, &bytes_final);

	// Get a list of pixels from the frame.
	const Magick::PixelPacket *pixel_packets = frame->GetPixels();

	// Fill the AVFrame with RGB image data
	for (int packet = 0, row = 0; row < bytes_source; packet++, row+=3)
	{
		// Update buffer (which is already linked to the AVFrame: pFrameRGB)
		frame_source->data[0][row] = pixel_packets[packet].red;
		frame_source->data[0][row+1] = pixel_packets[packet].green;
		frame_source->data[0][row+2] = pixel_packets[packet].blue;
	}


	// Resize image and convet pixel format to correct output format (for example: RGB to YUV420P)
	SwsContext *img_convert_ctx = NULL;

	// Init the software scaler from FFMpeg
	img_convert_ctx = sws_getContext(frame->GetWidth(), frame->GetHeight(), PIX_FMT_RGB24, info.width, info.height, c->pix_fmt, SWS_FAST_BILINEAR, NULL, NULL, NULL);
	if (img_convert_ctx == NULL)
		throw OutOfMemory("Could not allocate SwsContext.", path);

	// Resize & convert pixel format
	sws_scale(img_convert_ctx, frame_source->data, frame_source->linesize, 0,
			frame->GetHeight(), frame_final->data, frame_final->linesize);

	// Deallocate swscontext
	sws_freeContext(img_convert_ctx);


	// Encode Picture and Write Frame
	int video_outbuf_size = 200000;
	uint8_t *video_outbuf = new uint8_t[200000];

	if (oc->oformat->flags & AVFMT_RAWPICTURE) {
		// Raw video case.
		AVPacket pkt;
		av_init_packet(&pkt);

		pkt.flags |= AV_PKT_FLAG_KEY;
		pkt.stream_index= video_st->index;
		pkt.data= (uint8_t *)frame_final;
		pkt.size= sizeof(AVPicture);

		/* write the compressed frame in the media file */
		int averror = av_interleaved_write_frame(oc, &pkt);
		if (averror != 0)
		{
			//string error_description = av_err2str(averror);
			throw ErrorEncodingVideo("Error while writing raw video frame", frame->number);
		}

		// Deallocate packet
		av_free_packet(&pkt);

	} else {

		/* encode the image */
		int out_size = avcodec_encode_video(c, video_outbuf, video_outbuf_size, frame_final);

		/* if zero size, it means the image was buffered */
		if (out_size > 0) {
			AVPacket pkt;
			av_init_packet(&pkt);

			if (c->coded_frame->pts != AV_NOPTS_VALUE)
				pkt.pts= c->coded_frame->pts;
			if(c->coded_frame->key_frame)
				pkt.flags |= AV_PKT_FLAG_KEY;
			pkt.stream_index= video_st->index;
			pkt.data= video_outbuf;
			pkt.size= out_size;

			/* write the compressed frame in the media file */
			int averror = av_interleaved_write_frame(oc, &pkt);
			if (averror != 0)
			{
				//string error_description = av_err2str(averror);
				throw ErrorEncodingVideo("Error while writing compressed video frame", frame->number);
			}

			// Deallocate packet
			av_free_packet(&pkt);
		}
	}

	// Deallocate memory
	av_free(frame_source->data[0]);
	av_free(frame_source);
	av_free(frame_final->data[0]);
	av_free(frame_final);
	delete[] video_outbuf;
}

// Output the ffmpeg info about this format, streams, and codecs (i.e. dump format)
void FFmpegWriter::OutputStreamInfo()
{
	// output debug info
	av_dump_format(oc, 0, path.c_str(), 1);
}
