/*
 * SPDX-FileType: SOURCE
 *
 * SPDX-FileCopyrightText: 2025 Nick Kossifidis <mickflemm@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * This part loads and pre-processes audio files before passing them on to the player.
 * It performs metadata parsing, integrity checking, and accurate duration calculation.
 */

#include <libavformat/avformat.h>	/* For AVFormat / demuxer */
#include <libavcodec/avcodec.h>		/* For AvDecoder / decoder */
#include <libavutil/dict.h>		/* For av_dict* */
#include "scheduler.h"
#include "utils.h"


/*********\
* HELPERS *
\*********/

typedef enum {
	ARTIST,
	ALBUM,
	TITLE,
	ALBUM_GAIN,
	ALBUM_PEAK,
	ALBUM_ID,
	TRACK_GAIN,
	TRACK_PEAK,
	RELEASE_TID,
} AudioTagType;

/* Helper function to get ReplayGain tags both upper/lower case */
static char*
mldr_get_tag(AVDictionary *metadata, AudioTagType tag_type)
{
	char tag_name[128];
	AVDictionaryEntry *tag = NULL;

	if (!metadata)
		return NULL;

	/* Note that av_dict_get is case insensitive by default */
	switch (tag_type) {
	case ARTIST:
		snprintf(tag_name, sizeof(tag_name), "ARTIST");
		tag = av_dict_get(metadata, tag_name, NULL, 0);
		break;
	case ALBUM:
		snprintf(tag_name, sizeof(tag_name), "ALBUM");
		tag = av_dict_get(metadata, tag_name, NULL, 0);
		break;
	case TITLE:
		snprintf(tag_name, sizeof(tag_name), "TITLE");
		tag = av_dict_get(metadata, tag_name, NULL, 0);
		break;
	case ALBUM_GAIN:
		snprintf(tag_name, sizeof(tag_name), "REPLAYGAIN_ALBUM_GAIN");
		tag = av_dict_get(metadata, tag_name, NULL, 0);
		break;
	case ALBUM_PEAK:
		snprintf(tag_name, sizeof(tag_name), "REPLAYGAIN_ALBUM_PEAK");
		tag = av_dict_get(metadata, tag_name, NULL, 0);
		break;
	case ALBUM_ID:
		snprintf(tag_name, sizeof(tag_name), "MUSICBRAINZ_ALBUMID");
		tag = av_dict_get(metadata, tag_name, NULL, 0);
		if (!tag) {
			/* Try idv3 variant */
			snprintf(tag_name, sizeof(tag_name), "MusicBrainz Album Id");
			tag = av_dict_get(metadata, tag_name, NULL, 0);
		}
		break;
	case TRACK_GAIN:
		snprintf(tag_name, sizeof(tag_name), "REPLAYGAIN_TRACK_GAIN");
		tag = av_dict_get(metadata, tag_name, NULL, 0);
		break;
	case TRACK_PEAK:
		snprintf(tag_name, sizeof(tag_name), "REPLAYGAIN_TRACK_PEAK");
		tag = av_dict_get(metadata, tag_name, NULL, 0);
		break;
	case RELEASE_TID:
		snprintf(tag_name, sizeof(tag_name), "MUSICBRAINZ_RELEASETRACKID");
		tag = av_dict_get(metadata, tag_name, NULL, 0);
		if (!tag) {
			snprintf(tag_name, sizeof(tag_name), "MusicBrainz Release Track Id");
			tag = av_dict_get(metadata, tag_name, NULL, 0);
		}
		break;
	default:
		return NULL;
	}

	if (tag)
		return strdup(tag->value);

	return NULL;
}

static float
mldr_get_replaygain_tag(AVDictionary *metadata, AudioTagType tag_type)
{
	float db_val = 0.0f;
	char *str_val = mldr_get_tag(metadata, tag_type);

	if (!str_val)
		return 0.0f;

	if (sscanf(str_val, "%f", &db_val) != 1) {
		utils_wrn(LDR, "Invalid ReplayGain format: %s\n", str_val);
		db_val = 0.0f;
	}

	free(str_val);
	return db_val;
}


/**************\
* ENTRY POINTS *
\**************/

void
mldr_copy_audiofile(struct audiofile_info *dst, struct audiofile_info *src)
{
	dst->filepath = src->filepath ? strdup(src->filepath) : NULL;
	dst->artist = src->artist ? strdup(src->artist) : NULL;
	dst->album = src->album ? strdup(src->album) : NULL;
	dst->title = src->title ? strdup(src->title) : NULL;
	dst->albumid = src->albumid ? strdup(src->albumid) : NULL;
	dst->release_trackid = src->release_trackid ? strdup(src->release_trackid) : NULL;
	dst->album_gain = src->album_gain;
	dst->album_peak = src->album_peak;
	dst->track_gain = src->track_gain;
	dst->track_peak = src->track_peak;
	dst->duration_secs = src->duration_secs;
	dst->zone_name = src->zone_name ? strdup(src->zone_name) : NULL;
	/* No need to cary this around outside the player */
	dst->fader_info = NULL;
	dst->is_copy = 1;
}

void
mldr_cleanup_audiofile(struct audiofile_info *info)
{
	/* Note: const pointers come from pls/zone so don't free them
	 * unless they are copies, or we'll corrupt pls/zone structs. */

	 if (info->is_copy && info->filepath)
		free((char*) info->filepath);
	info->filepath = NULL;
	if(info->artist) {
		free(info->artist);
		info->artist = NULL;
	}
	if(info->album) {
		free(info->album);
		info->album = NULL;
	}
	if(info->title) {
		free(info->title);
		info->title = NULL;
	}
	if(info->albumid) {
		free(info->albumid);
		info->albumid = NULL;
	}
	if(info->release_trackid) {
		free(info->release_trackid);
		info->release_trackid = NULL;
	}
	if (info->is_copy && info->zone_name)
		free((char*) info->zone_name);
	info->zone_name = NULL;
	info->fader_info = NULL;
}

int mldr_init_audiofile(char* filepath, const char* zone_name, const struct fader_info *fdr, struct audiofile_info *info, int strict) {
	AVFormatContext *format_ctx = NULL;
	AVCodecContext *codec_ctx = NULL;
	int ret = 0;

	memset(info, 0, sizeof(struct audiofile_info));

	/* We 've already checked that this is a readable file */
	info->filepath = filepath;

	/* Set zone_name and fader_info from the scheduler */
	info->fader_info = fdr;
	info->zone_name = zone_name;

	if (avformat_open_input(&format_ctx, info->filepath, NULL, NULL) != 0) {
		utils_err(LDR, "Could not open file %s\n", info->filepath);
		ret = -1;
		goto cleanup;
	}

	/* Find the audio stream inside the file */
	if (avformat_find_stream_info(format_ctx, NULL) < 0) {
		utils_err(LDR, "Could not get stream info for %s\n", info->filepath);
		ret = -1;
		goto cleanup;
	}

	int audio_stream_index = av_find_best_stream(format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (audio_stream_index < 0) {
		utils_err(LDR, "Could not find audio stream in %s\n", info->filepath);
		ret = -1;
		goto cleanup;
	}

	codec_ctx = avcodec_alloc_context3(NULL);
	if (!codec_ctx) {
		utils_err(LDR, "Could not allocate codec context for %s\n", info->filepath);
		ret = -1;
		goto cleanup;
	}

	if (avcodec_parameters_to_context(codec_ctx, format_ctx->streams[audio_stream_index]->codecpar) < 0) {
		utils_err(LDR, "Could not copy codec params to context for %s\n", info->filepath);
		ret = -1;
		goto cleanup;
	}

	if (avcodec_open2(codec_ctx, avcodec_find_decoder(codec_ctx->codec_id), NULL) < 0) {
		utils_err(LDR, "Could not open codec for %s\n", info->filepath);
		ret = -1;
		goto cleanup;
	}


	/* Grab metadata */
	info->artist = mldr_get_tag(format_ctx->metadata, ARTIST);
	info->album = mldr_get_tag(format_ctx->metadata, ALBUM);
	info->title = mldr_get_tag(format_ctx->metadata, TITLE);
	info->albumid = mldr_get_tag(format_ctx->metadata, ALBUM_ID);
	info->release_trackid = mldr_get_tag(format_ctx->metadata, RELEASE_TID);

	info->album_gain = mldr_get_replaygain_tag(format_ctx->metadata, ALBUM_GAIN);
	info->album_peak = mldr_get_replaygain_tag(format_ctx->metadata, ALBUM_PEAK);
	info->track_gain = mldr_get_replaygain_tag(format_ctx->metadata, TRACK_GAIN);
	info->track_peak = mldr_get_replaygain_tag(format_ctx->metadata, TRACK_PEAK);


	/* If strict duration calculation and checking wasn't requested, skip this part
	 * and use whatever values ffmpeg gave us, if it didn't, go for it. */
	if (!strict) {
		if (format_ctx->duration != AV_NOPTS_VALUE) {
			info->duration_secs = format_ctx->duration / AV_TIME_BASE;
			ret = 0;
			goto cleanup;
		}
	}

	/* Determine duration in a reliable way, since metadata can't be trusted (especially
	 * for VBR mp3s), this is done by decoding the file, so we can check for any decoding errors
	 * while at it. Note that this also brings the file in the page cache, so when the player gets
	 * it again it'll get it (or most of it) from the cache instead of readingit again, so consider
	 * this also as a form of pre-buffering. */
	info->duration_secs = 0;
	double duration_secs_frac = 0.0f;
	int frame_count = 0;
	int decode_errors = 0;
	AVFrame *frame = av_frame_alloc();
	AVPacket packet;

	/* Get encoded packets, grab decoded frames */
	while (av_read_frame(format_ctx, &packet) >= 0) {
		if (packet.stream_index == audio_stream_index) {

			/* Send the packet to the decoder */
			ret = avcodec_send_packet(codec_ctx, &packet);
			if (ret < 0) {
				decode_errors++;
				utils_wrn(LDR, "Error sending packet to decoder: %s (frame %d)\n",
					  av_err2str(ret), frame_count);
				av_packet_unref(&packet);
				break;
			}
			/* Safe to unref here, decoder has a copy */
			av_packet_unref(&packet);

			/* Receive frames from the decoder */
			while (ret >= 0) {
				ret = avcodec_receive_frame(codec_ctx, frame);
				/* No frame available yet, go for next packet */
				if (ret == AVERROR(EAGAIN))
					break;
				/* No more frames */
				else if (ret == AVERROR_EOF)
					break;
				else if (ret < 0) {
					decode_errors++;
					utils_wrn(LDR, "Error receiving frame from decoder: %s (last frame %d)\n",
						 av_err2str(ret), frame_count);
					break;
				} else {
					duration_secs_frac += (double)frame->nb_samples * av_q2d(codec_ctx->time_base);
					frame_count++;
					av_frame_unref(frame);
				}
			}
		}
	}
	ret = 0;
	av_frame_free(&frame);
	/* Round duration_secs_frac to the closest higher integer */
	info->duration_secs = (time_t)(duration_secs_frac + 0.5f);

	if (decode_errors > 0) {
		utils_err(LDR, "File %s has %d decoding errors.\n", info->filepath, decode_errors);
		ret = -1;
		goto cleanup;
	}

	if (frame_count == 0) {
		utils_wrn(LDR, "File %s contains no audio frames.\n", info->filepath);
		ret = -1;
	}

	/* Compare calculated duration with metadata duration (if available) */
	if (format_ctx->duration != AV_NOPTS_VALUE) {
		time_t metadata_duration_seconds = format_ctx->duration / AV_TIME_BASE;
		int difference = abs((int)(info->duration_secs - metadata_duration_seconds));
		int tolerance_secs = 1;
		if (difference > tolerance_secs) {
			utils_wrn(LDR, "Duration mismatch in %s: Metadata: %lu seconds, Calculated: %lu seconds (tolerance: %i secs)\n",
			info->filepath, metadata_duration_seconds, info->duration_secs, tolerance_secs);
		}
	} else {
		utils_wrn(LDR, "No Duration Metadata in %s\n", info->filepath);
	}

cleanup:
	if (codec_ctx) {
		avcodec_close(codec_ctx);
		avcodec_free_context(&codec_ctx);
	}
	if (format_ctx) {
		avformat_close_input(&format_ctx);
	}

	if (ret != 0) {
		utils_err(LDR, "Error initializing audio file.\n");
		mldr_cleanup_audiofile(info);
		return ret;
	}

	if (utils_is_debug_enabled(LDR)) {
		utils_dbg(LDR, "File: %s\n", info->filepath);
		utils_dbg(LDR, "Artist: %s\n", info->artist ? info->artist : "N/A");
		utils_dbg(LDR, "Album: %s\n", info->album ? info->album : "N/A");
		utils_dbg(LDR, "Title: %s\n", info->title ? info->title : "N/A");
		utils_dbg(LDR, "Album ID: %s\n", info->albumid ? info->albumid : "N/A");
		utils_dbg(LDR, "Release Track ID: %s\n", info->release_trackid ? info->release_trackid : "N/A");
		utils_dbg(LDR, "Album Gain: %f\n", info->album_gain);
		utils_dbg(LDR, "Album Peak: %f\n", info->album_peak);
		utils_dbg(LDR, "Track Gain: %f\n", info->track_gain);
		utils_dbg(LDR, "Track Peak: %f\n", info->track_peak);
		utils_dbg(LDR, "Duration: %u\n", info->duration_secs);
	}

	return ret;
}