/*
 * Audio Scheduler - An audio clip scheduler for use in radio broadcasting
 * Media file loader / integrity checker
 *
 * Copyright (C) 2025 Nick Kossifidis <mickflemm@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>

#include "scheduler.h"
#include "utils.h"

/*********\
* HELPERS *
\*********/

typedef enum {
	ARTIST,
	ALBUM,
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
	char tag_name[64];
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

	if (sscanf(str_val, "%f", &db_val) == 1) {
            return db_val;
        } else
            utils_wrn(LDR, "Invalid ReplayGain format: %s\n", str_val);

	return 0.0f;
}

void
mldr_cleanup_audiofile(struct audiofile_info *info)
{
	if(info->artist) {
		free(info->artist);
		info->artist = NULL;
	}
	if(info->album) {
		free(info->album);
		info->album = NULL;
	}
	if(info->albumid) {
		free(info->albumid);
		info->albumid = NULL;
	}
	if(info->release_trackid) {
		free(info->release_trackid);
		info->release_trackid = NULL;
	}
}

int mldr_init_audiofile(char* filepath, struct audiofile_info *info, int strict) {
	AVFormatContext *format_ctx = NULL;
	AVCodecContext *codec_ctx = NULL;
	int ret = 0;

	/* We 've already checked that this is a readable file */
	info->filepath = filepath;

	if (avformat_open_input(&format_ctx, info->filepath, NULL, NULL) != 0) {
		utils_err(LDR, "Could not open file %s\n", info->filepath);
		ret = -1;
		goto cleanup;
	}

	if (avformat_find_stream_info(format_ctx, NULL) < 0) {
		utils_err(LDR, "Could not get stream info for %s\n", info->filepath);
		ret = -1;
		goto cleanup;
	}

	/* Find the audio stream inside the file */
	int audio_stream_index = -1;
	for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
		if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			audio_stream_index = i;
			break;
		}
	}

	if (audio_stream_index == -1) {
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
			info->duration_secs = (float)format_ctx->duration / AV_TIME_BASE;
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
					utils_wrn(LDR, "Error receiving frame from decoder: %s (frame %d)\n",
						 av_err2str(ret), frame_count);
					break;
				} else {
					info->duration_secs += (float)(frame->nb_samples * av_q2d(codec_ctx->time_base));
					frame_count++;
					av_frame_unref(frame);
				}
			}
		}
		av_packet_unref(&packet);
	}
	ret = 0;
	av_frame_free(&frame);

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
		float metadata_duration_seconds = (float)format_ctx->duration / AV_TIME_BASE;
		float duration_difference = fabsf(info->duration_secs - metadata_duration_seconds);

		const float DURATION_TOLERANCE_PERCENTAGE = 0.01f; // 1% tolerance
		float duration_tolerance_seconds = metadata_duration_seconds * DURATION_TOLERANCE_PERCENTAGE;

		if (duration_difference > duration_tolerance_seconds) {
			utils_wrn(LDR, "Duration mismatch in %s: Metadata: %.3f seconds, Calculated: %.3f seconds (Difference: %.3f seconds, Tolerance: %.3f seconds)\n",
			info->filepath, metadata_duration_seconds, info->duration_secs, duration_difference, duration_tolerance_seconds);
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
		utils_dbg(LDR, "Album ID: %s\n", info->albumid ? info->albumid : "N/A");
		utils_dbg(LDR, "Release Track ID: %s\n", info->release_trackid ? info->release_trackid : "N/A");
		utils_dbg(LDR, "Album Gain: %f\n", info->album_gain);
		utils_dbg(LDR, "Album Peak: %f\n", info->album_peak);
		utils_dbg(LDR, "Track Gain: %f\n", info->track_gain);
		utils_dbg(LDR, "Track Peak: %f\n", info->track_peak);
		utils_dbg(LDR, "Duration: %f\n", info->duration_secs);
	}

	return ret;
}