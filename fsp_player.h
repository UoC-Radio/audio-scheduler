/*
 * SPDX-FileType: SOURCE
 *
 * SPDX-FileCopyrightText: 2025 Nick Kossifidis <mickflemm@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef __PLAYER_H__
#define __PLAYER_H__

#include <pthread.h>			/* For pthread support */
#include <libavcodec/avcodec.h>		/* For AVCodecContext / decoder */
#include <libavformat/avformat.h>	/* For AVFormatContext / demuxer */
#include <libswresample/swresample.h>	/* For SwrContext / resampler/converter */
#include <pipewire/pipewire.h>		/* For pipewire support */
#include <jack/ringbuffer.h>		/* For using JACK's ringbuffer API (also available through pipewire) */
#include "scheduler.h"
#include "meta_handler.h"
#include "sig_dispatcher.h"

 /* Configuration */
 #define FSP_PERIOD_SIZE		2048	/* TODO: query pipewire for this */
 #define FSP_OUTPUT_SAMPLE_RATE		48000
 #define FSP_OUTPUT_CHANNELS		2
 #define FSP_RING_BUFFER_SECONDS	4
 #define FSP_CACHE_LINE_SIZE		64	/* TODO: get that from the OS through sysconf() */

 /* Slope for the 2sec gain curve, for fade in/out during pause/resume */
 #define FSP_STATE_FADE_SLOPE		(1.0f / (FSP_OUTPUT_SAMPLE_RATE * 2))
 
 /* Player states */
 enum fsp_state {
	FSP_STATE_STOPPED,
	FSP_STATE_PLAYING,
	FSP_STATE_PAUSING,	/* Fading out before pause */
	FSP_STATE_PAUSED,	/* Fully paused */
	FSP_STATE_RESUMING,	/* Fading in from pause */
	FSP_STATE_STOPPING,
	FSP_STATE_ERROR
 };

 /* Structures */
struct fsp_decoder_state {
	AVFormatContext	*fmt_ctx;
	AVCodecContext	*codec_ctx;
	SwrContext	*swr_ctx;
	AVFrame		*decoded_avframe;
	AVFrame		*resampled_avframe;
	AVPacket	*stream_packet;
	int		audio_stream_idx;
	size_t		consumed_frames;
	size_t		avail_frames;
	int		eof_reached;
	volatile int	started;
};

struct fsp_af_fader_state {
	float	fade_in_slope;
	float	fade_out_slope;
};

struct fsp_state_fader_state {
	float	state_fade_slope;
	size_t	state_fade_samples_tot;	/* Total samples for state fade */
	size_t	state_fade_samples_out;	/* Current position in state fade */
	int	state_fade_active;	/* Whether we're in a state fade */
	float	state_fade_gain;	/* Current gain during state fade */
};

struct fsp_replaygain_state {
	float	replay_gain;
	float	gain_limit;
};

struct fsp_audiofile_ctx {
	struct fsp_decoder_state	decoder;
	struct fsp_af_fader_state	fader;
	struct fsp_replaygain_state	replaygain;
	struct audiofile_info		info;
	size_t	total_samples;
	size_t	samples_played;
};

struct fsp_player {
	/* State */
	volatile sig_atomic_t	state;

	/* Current and next file info */
	struct fsp_audiofile_ctx	current;
	struct fsp_audiofile_ctx	next;
	pthread_mutex_t	file_mutex;

	/* Fader for state changes */
	struct fsp_state_fader_state	fader;

	/* Scheduler thread */
	pthread_t	scheduler_thread;
	pthread_mutex_t	scheduler_mutex;
	pthread_cond_t	scheduler_cond;
	struct scheduler	*sched;

	/* Decoder thread */
	pthread_t	decoder_thread;
	pthread_mutex_t	decoder_mutex;
	pthread_cond_t	decoder_cond;

	/* Ring buffer */
	jack_ringbuffer_t *ring;
	pthread_cond_t	space_available;

	/* Pipewire specific members */
	struct pw_main_loop 	*loop;
	struct pw_context	*context;
	struct pw_core		*core;
	struct pw_stream	*stream;
};

/* Functions */

void fsp_cleanup(struct fsp_player *player);
int fsp_init(struct fsp_player *player, struct scheduler *sched,
	     struct meta_handler *mh, struct sig_dispatcher *sd);
int fsp_start(struct fsp_player *player);
void fsp_stop(struct fsp_player *player);

#endif /* __PLAYER_H__ */