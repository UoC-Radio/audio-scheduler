/*
 * SPDX-FileType: SOURCE
 *
 * SPDX-FileCopyrightText: 2025 Nick Kossifidis <mickflemm@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * This is a player backend based on FFmpeg and Pipewire, in our case we compile FFmpeg
 * with libsoxr support, which is how this is intended to work (and how we use and test
 * it), but the built-in resampler should also work, since we use the swr API.
 */

#include <string.h>		/* For memset/memcpy */
#include <signal.h>		/* For signal numbers */
#include <stdlib.h>		/* For posix_memalign / malloc */
#include <libavutil/opt.h>	/* For av_opt_* */
#include <spa/pod/builder.h>	/* For spa_pod_builder_* */
#include <spa/param/audio/format-utils.h> /* For spa_format_audio_raw_build */
#include "utils.h"
#include "fsp_player.h"

/****************\
* SIGNAL HANDLER *
\****************/

static void fsp_signal_handler(int signal_number, void *userdata)
{
	struct fsp_player *player = (struct fsp_player*) userdata;

	switch (signal_number) {
	case SIGINT:
	case SIGTERM:
		fsp_stop(player);
		break;
	case SIGUSR1:
		utils_info(PLR, "Pausing\n");
		player->state = FSP_STATE_PAUSING;
		break;
	case SIGUSR2:
		utils_info(PLR, "Resuming\n");
		player->state = FSP_STATE_RESUMING;
		break;
	default:
		break;
	}
}


/****************************\
* FADER / REPLAYGAIN HELPERS *
\****************************/

static int fsp_replaygain_setup(struct fsp_replaygain_state *rgain,
			       const struct audiofile_info *info)
{
	/* Convert track gain from dB to linear */
	if (info->track_gain)
		rgain->replay_gain = powf(10.0f, info->track_gain / 20.0f);
	else
		rgain->replay_gain = 1.0f;

	/* Calculate gain limit from peak (already in linear scale) */
	if (info->track_peak)
		rgain->gain_limit = 1.0f / info->track_peak;
	else
		rgain->gain_limit = 1.0f;

	/* Limit replay gain by peak */
	if (rgain->replay_gain > rgain->gain_limit) {
		utils_dbg(PLR, "Limiting replay gain to peak: %f\n", rgain->gain_limit);
		rgain->replay_gain = rgain->gain_limit;
	}

	return 0;
}

static int fsp_fader_setup(struct fsp_af_fader_state *fader,
			   const struct audiofile_info *info)
{
	fader->fade_in_slope = 0.0f;
	fader->fade_out_slope = 0.0f;

	if (!info->fader_info)
		return 0;

	const struct fader_info *fdr = info->fader_info;

	if (fdr->fadein_duration_secs > 0 && (fdr->fadein_duration_secs < info->duration_secs))
		fader->fade_in_slope = 1.0f / (FSP_OUTPUT_SAMPLE_RATE * 
					      fdr->fadein_duration_secs);
	if (fdr->fadeout_duration_secs > 0 && (fdr->fadeout_duration_secs < info->duration_secs))
		fader->fade_out_slope = 1.0f / (FSP_OUTPUT_SAMPLE_RATE * 
					       fdr->fadeout_duration_secs);

	return 0;
}

static void fsp_state_fader_setup(struct fsp_state_fader_state *fader)
{
	fader->state_fade_samples_tot = FSP_OUTPUT_SAMPLE_RATE * 2; /* 2 seconds */
	fader->state_fade_slope = 1.0f / fader->state_fade_samples_tot;
	fader->state_fade_samples_out = 0;
	fader->state_fade_active = 0;
	fader->state_fade_gain = 1.0f;
}

static void fsp_fader_state_fade_start(struct fsp_state_fader_state *fader, int fade_in)
{
	fader->state_fade_samples_out = 0;
	fader->state_fade_active = 1;
	fader->state_fade_gain = fade_in ? 0.0f : 1.0f;
}

static float fsp_fader_state_fade_step(struct fsp_state_fader_state *fader, size_t frames, int fade_in)
{
	if (!fader->state_fade_active)
		goto done;

	/* Check if fade is complete */
	if (fader->state_fade_samples_out >= fader->state_fade_samples_tot) {
		fader->state_fade_active = 0;
		fader->state_fade_gain = fade_in ? 1.0f : 0.0f;
		goto done;
	}

	/* Calculate how many frames we can process in this step */
	size_t frames_remaining = fader->state_fade_samples_tot - fader->state_fade_samples_out;

	/* Calculate new gain */
	if (fade_in) {
		fader->state_fade_gain = (float)fader->state_fade_samples_out * fader->state_fade_slope;
	} else {
		fader->state_fade_gain = (float)frames_remaining * fader->state_fade_slope;
	}

	/* Update fade position */
	fader->state_fade_samples_out += frames;

 done:
	return fader->state_fade_gain;
}


/**********************\
* DECODER INIT/CLEANUP *
\**********************/

static void fsp_decoder_cleanup(struct fsp_decoder_state *dec)
{
	if (!dec)
		return;

	/* Note: av_*_free functions also set their pointer
	 * arg to NULL */
	if (dec->decoded_avframe)
		av_frame_free(&dec->decoded_avframe);
	if (dec->resampled_avframe)
		av_frame_free(&dec->resampled_avframe);
	if (dec->stream_packet)
		av_packet_free(&dec->stream_packet);
	if (dec->codec_ctx)
		avcodec_free_context(&dec->codec_ctx);
	if (dec->fmt_ctx)
		avformat_close_input(&dec->fmt_ctx);
	if (dec->swr_ctx)
		swr_free(&dec->swr_ctx);
}

static int fsp_decoder_init(struct fsp_decoder_state *dec, const char *filepath)
{
	int ret;

	/* Open input file */
	ret = avformat_open_input(&dec->fmt_ctx, filepath, NULL, NULL);
	if (ret < 0) {
		utils_err(PLR, "Failed to open file: %s\n", av_err2str(ret));
		return -1;
	}

	/* Find stream info */
	ret = avformat_find_stream_info(dec->fmt_ctx, NULL);
	if (ret < 0) {
		utils_err(PLR, "Failed to find stream info: %s\n", av_err2str(ret));
		goto cleanup;
	}

	/* Find audio stream */
	dec->audio_stream_idx = av_find_best_stream(dec->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (dec->audio_stream_idx < 0) {
		utils_err(PLR, "Failed to find audio stream\n");
		goto cleanup;
	}

	/* Get decoder */
	int codec_id = dec->fmt_ctx->streams[dec->audio_stream_idx]->codecpar->codec_id;
	const AVCodec *codec = avcodec_find_decoder(codec_id);
	if (!codec) {
		utils_err(PLR, "Failed to find decoder\n");
		goto cleanup;
	}

	/* Allocate codec context */
	dec->codec_ctx = avcodec_alloc_context3(codec);
	if (!dec->codec_ctx) {
		utils_err(PLR, "Failed to allocate decoder context\n");
		goto cleanup;
	}

	/* Copy codec parameters */
	ret = avcodec_parameters_to_context(dec->codec_ctx,
					  dec->fmt_ctx->streams[dec->audio_stream_idx]->codecpar);
	if (ret < 0) {
		utils_err(PLR, "Failed to copy codec params: %s\n", av_err2str(ret));
		goto cleanup;
	}

	/* Set decoder output format to interleaved float */
	ret = av_opt_set_int(dec->codec_ctx, "request_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
	if (ret < 0) {
		utils_err(PLR, "Failed to set decoder output format: %s\n", av_err2str(ret));
		goto cleanup;
	}

	/* Open the codec */
	ret = avcodec_open2(dec->codec_ctx, NULL, NULL);
	if (ret < 0) {
		utils_err(PLR, "Failed to open codec: %s\n", av_err2str(ret));
		goto cleanup;
	}

	/* Allocate decoded_avframe and stream_packet here so that we
	 * don't allocate them on each loop. Note those are just
	 * the structs, their content is managed by ffmpeg when we
	 * request new packets/frames */
	dec->decoded_avframe = av_frame_alloc();
	dec->stream_packet = av_packet_alloc();
	if (!dec->decoded_avframe || !dec->stream_packet) {
		utils_err(PLR, "Failed to allocate incoming frame/packet\n");
		goto cleanup;
	}

	/* For ffmpeg to fill out resampled_avframe, we need to allocate
	 * its buffers and set its properties here, since we give it to
	 * ffmpeg (in contrast to decoded_avframe that ffmpeg gives to us) */
	dec->resampled_avframe = av_frame_alloc();
	if (!dec->resampled_avframe) {
		utils_err(PLR, "Failed to allocate outgoing frame\n");
		goto cleanup;
	}

	/* Here nb_samples is samples per channel, so audio frames,
	 * not raw samples (which is why we don't multiply it by
	 * FSP_OUTPUT_CHANNELS). Set the output format (interleaved float),
	 * channel layout, and sample rate. We'll pass the same values
	 * to the resampler / converter initialization below.
	 *
	 * Note that the resampler may need more frames for output in
	 * case it adds a delay, FSP_PERIOD_SIZE should be a safe start, but
	 * we can query the codec context in case the decoder outputs a
	 * fixed number of samples on the decoded_avframe for a more accurate
	 * approach. No matter what we may still need to grow resampled_avframe
	 * if needed. */
	if (dec->codec_ctx->frame_size) {
		dec->resampled_avframe->nb_samples = av_rescale_rnd(dec->codec_ctx->frame_size,
								FSP_OUTPUT_SAMPLE_RATE,
								dec->codec_ctx->sample_rate,
								AV_ROUND_UP);
	} else
		dec->resampled_avframe->nb_samples = FSP_PERIOD_SIZE;
	av_channel_layout_copy(&dec->resampled_avframe->ch_layout,
			       &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO);
	dec->resampled_avframe->format = AV_SAMPLE_FMT_FLT;
	dec->resampled_avframe->sample_rate = FSP_OUTPUT_SAMPLE_RATE;

	ret = av_frame_get_buffer(dec->resampled_avframe, 0);
	if (ret < 0) {
		utils_err(PLR, "Failed to allocate resampled_avframe: %s\n",
			  av_err2str(ret));
		goto cleanup;
	}

	/* Initialize resampler / converter */
	ret = swr_alloc_set_opts2(&dec->swr_ctx,
				  &dec->resampled_avframe->ch_layout, dec->resampled_avframe->format, dec->resampled_avframe->sample_rate,
				  &dec->codec_ctx->ch_layout, dec->codec_ctx->sample_fmt, dec->codec_ctx->sample_rate,
				  0, NULL);
	if (ret < 0) {
		utils_err(PLR, "Failed to allocate resampler context: %s\n",
			 av_err2str(ret));
		return -1;
	}

	/* Try using the SoXr backend if available */
	#ifdef SWR_FLAG_RESAMPLE_SOXR
	av_opt_set (dec->swr_ctx, "resampler", "soxr", 0);
	av_opt_set (dec->swr_ctx, "precision", "28", 0);	/* Very high quality */
	#else
	/* Fallback to swr default backend */
	av_opt_set_int(dec->swr_ctx, "dither_method", SWR_DITHER_TRIANGULAR_HIGHPASS, 0);
	av_opt_set_int(dec->swr_ctx, "filter_size", 64,0); 
	#endif
	/* Full phase shift (linear responce) */
	av_opt_set_double (dec->swr_ctx, "phase_shift", 1.0, 0);
	/* Full bandwidth (a bit reduced for anti-aliasing) */
	av_opt_set_int(dec->swr_ctx, "cutoff", 0.98, 0);

	/* Initialize the resampling context */
	ret = swr_init(dec->swr_ctx);
	if (ret < 0) {
		utils_err(PLR, "Failed to initialize resampler: %s\n", av_err2str(ret));
		goto cleanup;
	}

	dec->consumed_frames = 0;
	dec->avail_frames = 0;
	dec->eof_reached = 0;

	return 0;

cleanup:
	fsp_decoder_cleanup(dec);
	return -1;
}


/****************************\
* PLAYBACK/AUDIOFILE CONTEXT *
\****************************/

static void fsp_audiofile_ctx_cleanup(struct fsp_audiofile_ctx *ctx)
{
	if (!ctx)
		return;
	mldr_cleanup_audiofile(&ctx->info);
	fsp_decoder_cleanup(&ctx->decoder);
}

static int fsp_audiofile_ctx_init(struct fsp_audiofile_ctx *ctx,
				  const struct audiofile_info *info)
{
	int ret;

	/* Copy file info from fsp_load_next_file's stack to ctx */
	memcpy(&ctx->info, info, sizeof(struct audiofile_info));
	ctx->samples_played = 0;
	ctx->total_samples = info->duration_secs * FSP_OUTPUT_SAMPLE_RATE * FSP_OUTPUT_CHANNELS;

	/* Initialize decoder/resampler first */
	ret = fsp_decoder_init(&ctx->decoder, info->filepath);
	if (ret < 0) {
		utils_err(PLR, "Failed to initialize decoder\n");
		return ret;
	}

	/* Setup ReplayGain */
	ret = fsp_replaygain_setup(&ctx->replaygain, info);
	if (ret < 0) {
		utils_err(PLR, "Failed to setup ReplayGain\n");
		goto cleanup;
	}

	/* Setup fader */
	ret = fsp_fader_setup(&ctx->fader, info);
	if (ret < 0) {
		utils_err(PLR, "Failed to setup fader\n");
		goto cleanup;
	}

	return 0;

cleanup:
	fsp_decoder_cleanup(&ctx->decoder);
	return ret;
}


/****************\
* DECODER THREAD *
\****************/

/* Extracts audio frames from the provided fsp_audiofile_ctx, each audio frame
 * contains FSP_OUTPUT_CHANNELS audio samples. Since FFmpeg also uses the term
 * "frame" for the decoded audio chunks, I use "avframe" for those, don't let
 * the name of av_read_frame() confuse you, its for audio chunks (multiple audio
 * frames), not a single audio frame. */
static size_t
fsp_extract_frames(struct fsp_player *player, struct fsp_audiofile_ctx *ctx,
		   float *output, size_t frames_needed)
{
	struct fsp_decoder_state *dec = &ctx->decoder;
	size_t frames_decoded = 0;
	int ret;

	/* Process until we have enough frames or hit EOF */
	while (frames_decoded < frames_needed && !dec->eof_reached && player->state != FSP_STATE_STOPPING) {

		/* We consumed all frames from the last resampled
		 * avframe, ask for a new one from the decoder. */
		if (dec->consumed_frames >= dec->avail_frames) {
			dec->consumed_frames = 0;
			dec->avail_frames = 0;

			/* Try to get next avframe from the decoder, note that according
			 * to docs, this will also unref decoded_frame before providing a new one.*/
			ret = avcodec_receive_frame(dec->codec_ctx, dec->decoded_avframe);
			if (ret == AVERROR(EAGAIN)) {

				/* Out of data, grab the next packet from the demuxer that
				 * handles the audio file. */
				while ((ret = av_read_frame(dec->fmt_ctx, dec->stream_packet)) >= 0) {

					/* Check if it's an audio packet and send it to the decoder
					 * According o the docs the packet is always fully consumed
					 * and is owned by the caller, so we unref afterwards */
					if (dec->stream_packet->stream_index == dec->audio_stream_idx) {
						if ((ret = avcodec_send_packet(dec->codec_ctx, dec->stream_packet)) < 0) {
							utils_err(PLR, "Error sending packet to decoder: %s\n",
								  av_err2str(ret));
							av_packet_unref(dec->stream_packet);
							goto cleanup;
						}
						av_packet_unref(dec->stream_packet);
						/* We should have decoded avframes now */
						break;
					}

					/* Not an audio packet, unref and try next one */
					av_packet_unref(dec->stream_packet);
				}

				if (ret < 0) {
					if (ret == AVERROR_EOF) {
						/* No more packets left on the stream, we need a new file
						 * Flush any pending avframes out of the decoder and retry */
						avcodec_send_packet(dec->codec_ctx, NULL);
						utils_dbg(PLR, "flushed decoder\n");
					} else {
						utils_err(PLR, "Error reading packet: %s\n", av_err2str(ret));
						goto cleanup;
					}
				}

				continue;
			} else if (ret == AVERROR_EOF) {
				 /* No more avframes available on the decoder */
				 dec->eof_reached = 1;
				 av_frame_unref(dec->decoded_avframe);
			} else if (ret < 0) {
				utils_err(PLR, "Error receiving frame from decoder: %s\n",
					  av_err2str(ret));
				goto cleanup;
			}

			/* Got a new avframe, resample / convert it to the requested output config 
			 * (including channel layout, so this will also downmix multi-channel files
			 * and handle mono files). Note that we may have pending frames inside the
			 * resampler from a previous run, in which case decoded_avframe will be empty
			 * and we need to flush it the same way we flush the decoder. */

			/* Check if the next call to swr_convert_frame will need extra frames
			 * for delay compensation, or we have any pending frames to flush out of
			 * the resampler. */
			int64_t swr_delay = swr_get_delay(dec->swr_ctx, dec->codec_ctx->sample_rate);
			if (swr_delay < 0) {
				utils_err(PLR, "Error requesting delay from resampler: %s\n",
						av_err2str(ret));
				goto cleanup;
			} 

			/* Check if we have enough space on resampled_frame for the resampled output */
			uint32_t required_samples = av_rescale_rnd(swr_delay + dec->decoded_avframe->nb_samples,
								 FSP_OUTPUT_SAMPLE_RATE, dec->codec_ctx->sample_rate,
								 AV_ROUND_UP);
			size_t required_bytes = required_samples * FSP_OUTPUT_CHANNELS * sizeof(float);
			size_t allocated_bytes = dec->resampled_avframe->buf[0]->size;
			if (required_bytes > allocated_bytes) {
				utils_dbg(PLR, "re-sizing resampled_avframe, required: %i, allocated: %i\n",
					 required_bytes, allocated_bytes);

				 /* Free up the current one */
				av_frame_unref(dec->resampled_avframe);

				/* Initialize and allocate the updated one */
				dec->resampled_avframe->nb_samples = required_samples;
				av_channel_layout_copy(&dec->resampled_avframe->ch_layout,
					&(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO);
		 		dec->resampled_avframe->format = AV_SAMPLE_FMT_FLT;
		 		dec->resampled_avframe->sample_rate = FSP_OUTPUT_SAMPLE_RATE;

				 ret = av_frame_get_buffer(dec->resampled_avframe, 0);
				 if (ret < 0) {
					 utils_err(PLR, "Failed to allocate resampled_avframe: %s\n",
						   av_err2str(ret));
					 goto cleanup;
				 }
			}

			if (dec->decoded_avframe->nb_samples > 0)
				ret = swr_convert_frame(dec->swr_ctx, dec->resampled_avframe, dec->decoded_avframe);
			else {
				ret = swr_convert_frame(dec->swr_ctx, dec->resampled_avframe, NULL);
				utils_dbg(PLR, "flushed resampler\n");
			}

			if (ret < 0) {
				utils_wrn(PLR, "Error during resampling: %s\n",
					  av_err2str(ret));
				goto cleanup;
			}

			/* Note that nb_samples is samples per channel, so audio frames */
			dec->avail_frames = dec->resampled_avframe->nb_samples;
		}

		/* Copy frames from the last resampled_avframe to output */
		if (dec->avail_frames > 0) {
			/* Since we use interleaved format (AV_SAMPLE_FMT_FLT) we have all samples from both channels on data[0] */
			const float *src = (float *)dec->resampled_avframe->data[0] + (dec->consumed_frames * FSP_OUTPUT_CHANNELS);
			float *dst = output + (frames_decoded * FSP_OUTPUT_CHANNELS);

			size_t remaining_decoded_frames = frames_needed - frames_decoded;
			size_t remaining_ctx_samples = ctx->total_samples - ctx->samples_played;

			size_t frames_to_copy = dec->avail_frames - dec->consumed_frames;

			/* Decoder gave us more frames than we require, output up to remaining_decoded_frames
			 * and leave the rest there for the next run to process. If it has less we'll return
			 * a value less than frames_needed and the caller will handle calling us again with
			 * a new ctx and an updated output pointer. */
			if (frames_to_copy > remaining_decoded_frames)
				frames_to_copy = remaining_decoded_frames;

			/* Calculate fader gain */
			float fader_gain = 1.0f;
			struct audiofile_info *info = &ctx->info;
			const struct fader_info *fdr = info->fader_info;

			/* Fade in on song start */
			if (ctx->fader.fade_in_slope > 0 && 
				   ctx->samples_played < fdr->fadein_duration_secs * FSP_OUTPUT_SAMPLE_RATE) {
				fader_gain = ctx->fader.fade_in_slope * ctx->samples_played;
			/* Fade out on song end */
			} else if (ctx->fader.fade_out_slope > 0 &&
				   remaining_ctx_samples < fdr->fadeout_duration_secs * FSP_OUTPUT_SAMPLE_RATE) {
				fader_gain = ctx->fader.fade_out_slope * remaining_ctx_samples;
			}

			/* Combine fader gain modifier with replaygain */
			float gain_factor = fader_gain * ctx->replaygain.replay_gain;

			/* Apply combined gain and copy to output and fill the remaining buffer */
			size_t samples_to_copy = frames_to_copy * FSP_OUTPUT_CHANNELS;
			for (size_t i = 0; i < samples_to_copy; i++) {
				dst[i] = src[i] * gain_factor;
			}

			dec->consumed_frames += frames_to_copy;
			frames_decoded += frames_to_copy;
			ctx->samples_played += samples_to_copy;
		}
	}

 cleanup:
	return frames_decoded;
}

struct fsp_decoder_thread_data {
	float *decode_buffer;
	size_t buffer_max_frames;
	size_t buffer_size;
};


static void fsp_decoder_thread_data_cleanup(struct fsp_decoder_thread_data *data)
{
	if (!data)
		return;
	free(data->decode_buffer);
	free(data);
}

static struct fsp_decoder_thread_data *fsp_decoder_thread_data_init(void)
{
	struct fsp_decoder_thread_data *data;
	void *aligned_buf;
	int ret;

	data = malloc(sizeof(*data));
	if (!data) {
		utils_err(PLR, "Failed to allocate decoder thread data\n");
		return NULL;
	}

	/* The decoder thread outputs a period at a time to put to the ringbuffer
	 * which holds up to FSP_RING_BUFFER_SECONDS of data. */
	data->buffer_max_frames = FSP_PERIOD_SIZE;
	data->buffer_size = data->buffer_max_frames * FSP_OUTPUT_CHANNELS * sizeof(float);
	ret = posix_memalign(&aligned_buf, FSP_CACHE_LINE_SIZE, data->buffer_size);
	if (ret != 0) {
		utils_err(PLR, "Failed to allocate aligned decode buffer\n");
		free(data);
		return NULL;
	}
	data->decode_buffer = aligned_buf;

	return data;
}

static void *fsp_decoder_thread(void *arg)
{
	struct fsp_player *player = arg;
	struct fsp_decoder_thread_data *data;
	size_t frames_decoded = 0;

	utils_dbg(PLR, "Decoder thread started\n");

	data = fsp_decoder_thread_data_init();
	if (!data) {
		utils_err(PLR, "Failed to initialize decoder thread data\n");
		return NULL;
	}

	/* Wait until we have at least one file to decode */
	pthread_cond_wait(&player->decoder_cond, &player->decoder_mutex);

	while (player->state != FSP_STATE_STOPPING) {
		/* Wait if ring buffer doesn't have space for double buffer */
		if (jack_ringbuffer_write_space(player->ring) < data->buffer_size) {
			pthread_cond_wait(&player->space_available, &player->decoder_mutex);
			continue;
		}

		/* Decode frames from file into the decode_buffer */
		pthread_mutex_lock(&player->file_mutex);
		frames_decoded = fsp_extract_frames(player, &player->current, 
						  data->decode_buffer, 
						  data->buffer_max_frames);

		/* Got fewer frames than requested, so no more frames on the decoder
		 * switch to the next file and keep filling the decode_buffer */
		if (frames_decoded < data->buffer_max_frames && player->next.decoder.fmt_ctx) {
			utils_dbg(PLR, "Switching to next file\n");

			/* Check if we missed any frames and warn the user, resampler may generate
			 * extra samples but we shouldn't be very off (under normal cirumstances I
			 * got 1 extra sample which is due to the round-up), this check is here
			 * for debugging mostly, to make sure that if sometihng tottaly weird happens
			 * we catch it and report it.*/
			int diff = &player->current.total_samples - &player->current.samples_played;
			if (abs(diff) > 100)
				utils_wrn(PLR, "inconsistent playback diff: %i samples\n", diff);

			/* Cleanup current audiofile context */
			fsp_audiofile_ctx_cleanup(&player->current);

			/* Move next to current */
			memcpy(&player->current, &player->next, sizeof(struct fsp_audiofile_ctx));
			memset(&player->next, 0, sizeof(struct fsp_audiofile_ctx));

			/* Signal scheduler to load next file */
			pthread_cond_signal(&player->scheduler_cond);

			/* Continue extracting frames with new current file
			 * and any frames remaining on the last decoded_avframe */
			size_t remaining = data->buffer_max_frames - frames_decoded;
			size_t buffer_offt = (frames_decoded * FSP_OUTPUT_CHANNELS);
			size_t extra_frames = fsp_extract_frames(player, &player->current,
								 data->decode_buffer + buffer_offt,
								 remaining);
			frames_decoded += extra_frames;
		}
		pthread_mutex_unlock(&player->file_mutex);

		if (frames_decoded > 0) {
			/* Write to ring buffer */
			size_t samples_to_write = (frames_decoded * FSP_OUTPUT_CHANNELS);
			size_t bytes_to_write = samples_to_write * sizeof(float);
			size_t written = jack_ringbuffer_write(player->ring, 
							     (const char *)data->decode_buffer, 
							     bytes_to_write);

			if (written < bytes_to_write)
				utils_wrn(PLR, "Ring buffer overrun\n");
		}

		/* Small sleep if no data */
		if (frames_decoded == 0 && player->state != FSP_STATE_STOPPING) {
			struct timespec ts = {0, 1000000}; /* 1ms */
			nanosleep(&ts, NULL);
		}
	}

	utils_dbg(PLR, "Decoder thread stopping\n");
	fsp_decoder_thread_data_cleanup(data);
	fsp_stop(player);
	return NULL;
}


/******************\
* SCHEDULER THREAD *
\******************/

static int fsp_load_next_file(struct fsp_player *player, time_t sched_time)
{
	struct audiofile_info next_info;
	int ret;

	ret = sched_get_next(player->sched, sched_time, &next_info);
	if (ret < 0) {
		utils_err(PLR, "Failed to get next file from scheduler\n");
		return -1;
	}

	utils_dbg(PLR, "Loading next file: %s\n", next_info.filepath);

	/* Initialize next audiofile context */
	ret = fsp_audiofile_ctx_init(&player->next, &next_info);
	if (ret < 0) {
		utils_err(PLR, "Failed to initialize next audiofile context\n");
		return -1;
	}

	return 0;
}

static void *fsp_scheduler_thread(void *arg)
{
	struct fsp_player *player = arg;
	int ret;
	time_t now;
	time_t sched_time;
	time_t curr_duration;

	utils_dbg(PLR, "Scheduler thread started\n");

	/* First run - get current song */
	sched_time = time(NULL);
	ret = fsp_load_next_file(player, sched_time);
	if (ret < 0) {
		utils_err(PLR, "Failed to load initial file\n");
		return NULL;
	}

	/* Move next to current */
	pthread_mutex_lock(&player->file_mutex);
	memcpy(&player->current, &player->next, sizeof(struct fsp_audiofile_ctx));
	memset(&player->next, 0, sizeof(struct fsp_audiofile_ctx));
	pthread_mutex_unlock(&player->file_mutex);

	curr_duration = player->current.info.duration_secs;

	/* Immediately get next song */
	sched_time += curr_duration;
	ret = fsp_load_next_file(player, sched_time);
	if (ret < 0) {
		utils_err(PLR, "Failed to load second file\n");
		return NULL;
	}

	/* Signal decoder that we have files ready */
	pthread_cond_signal(&player->decoder_cond);

	while (player->state != FSP_STATE_STOPPING) {
		/* Save curation of next song here, before decoder does
		 * the switch and lose the information., so that when
		 * we start playing it and becomes the current song,
		 * we can correctly schedule the one after it. */
		curr_duration = player->next.info.duration_secs;

		/* Wait for signal from decoder when it switches files */
		pthread_cond_wait(&player->scheduler_cond, &player->scheduler_mutex);

		if (player->state == FSP_STATE_STOPPING)
			break;

		/* Calculate next schedule time based on current file */
		now = time(NULL);
		if (utils_is_debug_enabled(PLR)) {
			char datestr[26];
			struct tm tm = *localtime(&now);
			strftime (datestr, 26, "%a %d %b %Y, %H:%M:%S", &tm);
			utils_dbg (PLR, "Scheduler triggered at: %s\n", datestr);
		}
		sched_time = now + curr_duration;

		/* Load next file */
		pthread_mutex_lock(&player->file_mutex);
		ret = fsp_load_next_file(player, sched_time);
		pthread_mutex_unlock(&player->file_mutex);
		if (ret < 0) {
			utils_err(PLR, "Failed to load next file\n");
			break;  /* Fatal error */
		}

		pthread_cond_signal(&player->decoder_cond);
	}

	utils_dbg(PLR, "Scheduler thread stopping\n");
	fsp_stop(player);
	return NULL;
}


/*****************\ 
* OUTPUT HANDLING *
\*****************/

static void fsp_on_process(void *userdata)
{
	struct fsp_player *player = userdata;
	struct pw_buffer *buf;
	struct spa_buffer *spa_buf;
	float *dest;
	size_t bytes_to_write;
	uint32_t n_frames;

	/* Should we terminate ? */
	if (player->state == FSP_STATE_STOPPING) {
		fsp_stop(player);
		return;
	}

	/* Early return if no buffer available */
	buf = pw_stream_dequeue_buffer(player->stream);
	if (!buf) {
		utils_wrn(PLR, "Pipewire output buffer overrun\n");
		return;
	}

	spa_buf = buf->buffer;
	dest = spa_buf->datas[0].data;
	if (!dest)
		goto queue_buffer;


	size_t stride = sizeof(float) * FSP_OUTPUT_CHANNELS;
	n_frames = spa_buf->datas[0].maxsize / stride;
	if (buf->requested)
		n_frames = SPA_MIN((int)buf->requested, n_frames);
	bytes_to_write = n_frames * stride;

	/* Output silence if paused/stopped */
	if (player->state == FSP_STATE_PAUSED || player->state == FSP_STATE_STOPPED) {
		memset(dest, 0, bytes_to_write);
		goto set_buffer_meta;
	}

	/* Handle state transitions */
	if (player->state == FSP_STATE_PAUSING && !player->fader.state_fade_active) {
		utils_dbg(PLR, "Starting fade out for pause\n");
		fsp_fader_state_fade_start(&player->fader, 0);
	} else if (player->state == FSP_STATE_RESUMING && !player->fader.state_fade_active) {
		utils_dbg(PLR, "Starting fade in for resume\n");
		fsp_fader_state_fade_start(&player->fader, 1);
	}

	/* Check for data availability */
	if (jack_ringbuffer_read_space(player->ring) < bytes_to_write) {
		memset(dest, 0, bytes_to_write);
		if (player->state == FSP_STATE_PLAYING) {
			utils_wrn(PLR, "Decoder ring buffer underrun: needed %u bytes, available %zu\n",
				 bytes_to_write, jack_ringbuffer_read_space(player->ring));
		}
	} else {
		/* Read data from ring buffer and notify the decoder about it */
		jack_ringbuffer_read(player->ring, (char *)dest, bytes_to_write);
		pthread_cond_signal(&player->space_available);

		/* Apply state fade if active */
		if (player->fader.state_fade_active) {
			float fade_gain = fsp_fader_state_fade_step(&player->fader, 
								  n_frames,
								  player->state == FSP_STATE_RESUMING);

			/* Apply fade gain */
			float *samples = dest;
			for (size_t i = 0; i < n_frames * FSP_OUTPUT_CHANNELS; i++) {
				samples[i] *= fade_gain;
			}

			/* Check if fade is complete */
			if (!player->fader.state_fade_active) {
				if (player->state == FSP_STATE_PAUSING) {
					player->state = FSP_STATE_PAUSED;
					utils_dbg(PLR, "Fade out complete, now paused\n");
				} else if (player->state == FSP_STATE_RESUMING) {
					player->state = FSP_STATE_PLAYING;
					utils_dbg(PLR, "Fade in complete, now playing\n");
				}
			}
		}

	}

set_buffer_meta:
	spa_buf->datas[0].chunk->offset = 0;
	spa_buf->datas[0].chunk->stride = stride;
	spa_buf->datas[0].chunk->size = bytes_to_write;

queue_buffer:
	pw_stream_queue_buffer(player->stream, buf);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = fsp_on_process,
};

static void fsp_stream_cleanup(struct fsp_player *player)
{
	if (!player)
		return;

	if (player->stream) {
		pw_stream_destroy(player->stream);
		player->stream = NULL;
	}
}

static int fsp_stream_init(struct fsp_player *player)
{
	struct pw_properties *props;
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[1];

	props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Audio",
		PW_KEY_MEDIA_CATEGORY, "Playback",
		PW_KEY_MEDIA_ROLE, "Music",
		PW_KEY_APP_NAME, "Audio Scheduler",
		PW_KEY_NODE_NAME, "audio_scheduler",
		PW_KEY_NODE_LATENCY, "1024/48000",
		PW_KEY_NODE_WANT_DRIVER, "false",
		NULL);

	if (!props) {
		utils_err(PLR, "Failed to create stream properties\n");
		goto cleanup;
	}

	player->stream = pw_stream_new_simple(pw_main_loop_get_loop(player->loop),
					      "audio-scheduler", props,
					      &stream_events, player);

	if (!player->stream) {
		utils_err(PLR, "Failed to create stream\n");
		goto cleanup;
	}

	/* Setup fixed audio format */
	params[0] = spa_format_audio_raw_build(&b,
		SPA_PARAM_EnumFormat,
		&SPA_AUDIO_INFO_RAW_INIT(
			.format = SPA_AUDIO_FORMAT_F32,
			.channels = FSP_OUTPUT_CHANNELS,
			.rate = FSP_OUTPUT_SAMPLE_RATE,
			.position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR }
		));

	if (pw_stream_connect(player->stream,
			     PW_DIRECTION_OUTPUT,
			     PW_ID_ANY,
			     PW_STREAM_FLAG_MAP_BUFFERS |
			     PW_STREAM_FLAG_RT_PROCESS,
			     params, 1) < 0) {
		utils_err(PLR, "Failed to connect stream\n");
		goto cleanup;
	}

	return 0;

cleanup:
	fsp_stream_cleanup(player);
	return -1;
}


/***********************\
* META HANDLER CALLBACK *
\***********************/

int fsp_mh_cb(struct audiofile_info *cur, struct audiofile_info *next,
	      uint32_t *elapsed_sec, void *player_data)
{
	struct fsp_player *player = player_data;

	if(elapsed_sec)
		*elapsed_sec = player->current.samples_played /
			      (FSP_OUTPUT_SAMPLE_RATE * FSP_OUTPUT_CHANNELS);

	if(!cur && !next)
		return 0;

	pthread_mutex_lock(&player->file_mutex);
	if(cur)
		mldr_copy_audiofile(cur, &player->current.info);
	if(next)
		mldr_copy_audiofile(next, &player->next.info);
	pthread_mutex_unlock(&player->file_mutex);
	return 0;
}

/**************\
* ENTRY POINTS *
\**************/

void fsp_stop(struct fsp_player *player)
{
	if (!player || player->state == FSP_STATE_STOPPED
		    || player->state == FSP_STATE_STOPPING)
		return;

	player->state = FSP_STATE_STOPPING;
	utils_dbg(PLR, "Stopping\n");

	/* Stop pipewire loop */
	pw_main_loop_quit(player->loop);

	/* Signal threads to stop */
	pthread_cond_signal(&player->scheduler_cond);
	pthread_cond_signal(&player->decoder_cond);
	pthread_cond_signal(&player->space_available);

	/* Wait for threads */
	pthread_join(player->scheduler_thread, NULL);
	pthread_join(player->decoder_thread, NULL);

	player->state = FSP_STATE_STOPPED;
	utils_dbg(PLR, "Player stopped\n");
}

int fsp_start(struct fsp_player *player)
{
	utils_dbg(PLR, "Starting\n");

	if (player->state != FSP_STATE_STOPPED) {
		utils_err(PLR, "Player not in stopped state\n");
		return -1;
	}

	/* Reset state */
	player->state = FSP_STATE_RESUMING;

	/* Start scheduler thread */
	if (pthread_create(&player->scheduler_thread, NULL, fsp_scheduler_thread, player) != 0) {
		utils_err(PLR, "Failed to create scheduler thread\n");
		return -1;
	}

	/* Start decoder thread */
	if (pthread_create(&player->decoder_thread, NULL, fsp_decoder_thread, player) != 0) {
		utils_err(PLR, "Failed to create decoder thread\n");
		player->state = FSP_STATE_STOPPING;
		pthread_join(player->scheduler_thread, NULL);
		return -1;
	}

	/* Start loop */
	if (pw_main_loop_run(player->loop) < 0) {
		utils_err(PLR, "Failed to start pipewire loop");
		player->state = FSP_STATE_STOPPING;
		pthread_join(player->scheduler_thread, NULL);
		pthread_join(player->decoder_thread, NULL);
		return -1;
	}

	utils_dbg(PLR, "Exit from pw_main_loop\n");
	fsp_stop(player);
	return 0;
}

void fsp_cleanup(struct fsp_player *player)
{
	if (!player)
		return;

	utils_dbg(PLR, "Destroying player\n");

	/* Stop playback if running */
	fsp_stop(player);

	/* Cleanup files */
	fsp_audiofile_ctx_cleanup(&player->current);
	fsp_audiofile_ctx_cleanup(&player->next);

	/* Cleanup ring buffer */
	if (player->ring)
		jack_ringbuffer_free(player->ring);

	/* Cleanup pipewire */
	fsp_stream_cleanup(player);
	if (player->loop)
		pw_main_loop_destroy(player->loop);

	/* Cleanup synchronization primitives */
	pthread_mutex_destroy(&player->file_mutex);
	pthread_mutex_destroy(&player->scheduler_mutex);
	pthread_mutex_destroy(&player->decoder_mutex);
	pthread_cond_destroy(&player->scheduler_cond);
	pthread_cond_destroy(&player->decoder_cond);
	pthread_cond_destroy(&player->space_available);

	pw_deinit();

	utils_dbg(PLR, "Player destroyed\n");
}

int fsp_init(struct fsp_player *player, struct scheduler *sched, struct meta_handler *mh, struct sig_dispatcher *sd)
{
	memset(player, 0, sizeof(struct fsp_player));

	/* Initialize synchronization primitives */
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
	pthread_mutex_init(&player->file_mutex, &attr);
	pthread_mutex_init(&player->scheduler_mutex, &attr);
	pthread_mutex_init(&player->decoder_mutex, &attr);
	pthread_cond_init(&player->scheduler_cond, NULL);
	pthread_cond_init(&player->decoder_cond, NULL);
	pthread_cond_init(&player->space_available, NULL);

	/* Create ring buffer */
	size_t ring_size = FSP_RING_BUFFER_SECONDS * FSP_OUTPUT_SAMPLE_RATE * 
			   FSP_OUTPUT_CHANNELS * sizeof(float);
	player->ring = jack_ringbuffer_create(ring_size);
	if (!player->ring) {
		utils_err(PLR, "Failed to create ring buffer\n");
		goto cleanup;
	}

	/* Make ring buffer memory lock-resident */
	jack_ringbuffer_mlock(player->ring);

	player->sched = sched;
	player->state = FSP_STATE_STOPPED;

	/* Register with media handler and signal dispatcher */
	mh_register_state_callback(mh, fsp_mh_cb, player);
	sig_dispatcher_register(sd, SIG_UNIT_PLAYER, fsp_signal_handler, player);

	fsp_state_fader_setup(&player->fader);

	/* Initialize pipewire */
	pw_init(NULL, NULL);

	/* Create loop */
	player->loop = pw_main_loop_new(NULL);
	if (!player->loop) {
		utils_err(PLR, "Failed to create pipewire loop");
		goto cleanup;
	}

	/* Create and connect stream */
	if (fsp_stream_init(player) < 0)
		goto cleanup;

	utils_dbg(PLR, "Initialized\n");
	return 0;

cleanup:
	fsp_cleanup(player);
	return -1;
}
