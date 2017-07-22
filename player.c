/*
 * Audio Scheduler - An audio clip scheduler for use in radio broadcasting
 * Crossfade-capable player
 *
 * Copyright (C) 2017 George Kiagiadakis <gkiagia@tolabaki.gr>
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

#include "player.h"
#include "utils.h"
#include <gst/controller/gstdirectcontrolbinding.h>
#include <gst/controller/gstinterpolationcontrolsource.h>
#include <string.h>   /* for memset */

static void play_queue_item_set_fade (struct play_queue_item * item,
    GstClockTime start, gdouble start_value, GstClockTime end,
    gdouble end_value);
static gboolean player_ensure_next (struct player * self);
static gboolean player_recycle_item (struct play_queue_item * item);
static gboolean player_handle_item_eos (struct play_queue_item * item);

static GstPadProbeReturn
itembin_srcpad_buffer_probe (GstPad * pad, GstPadProbeInfo * info,
    struct play_queue_item * item)
{
  gint64 duration;
  GstEvent *event;
  const GstSegment *segment;
  GstClockTime fadeout, end;

  if (!gst_pad_query_duration (pad, GST_FORMAT_TIME, &duration) ||
            duration <= 0)
  {
    utils_wrn (PLR, "item %p: unknown file duration, consider remuxing; "
        "skipping playback: %s\n", item, item->file);

    /* Here we unlink the pad from the audiomixer because letting the buffer
     * go in the GstAggregator (parent class of audiomixer) may cause some
     * locking on this thread, which will delay freeing this item and may block
     * the main thread for significant time.
     * As a side-effect, this causes an ERROR GstMessage, which gets posted
     * on the bus and we recycle the item from the handler of the message,
     * in a similar way we do when another error occurs (for example, when
     * a decoder is missing) */
    gst_pad_unlink (pad, item->mixer_sink);

    /* and now get out of here */
    return GST_PAD_PROBE_REMOVE;
  }

  /* schedule fade in */
  if (item->fader.fadein_duration_secs > 0) {
    play_queue_item_set_fade (item, 0, item->fader.min_lvl,
        item->fader.fadein_duration_secs * GST_SECOND, item->fader.max_lvl);
  }

  /* schedule fade out */
  if (item->fader.fadeout_duration_secs > 0) {
    end = duration;
    fadeout = end - item->fader.fadeout_duration_secs * GST_SECOND;

    play_queue_item_set_fade (item, fadeout, item->fader.max_lvl,
        end, item->fader.min_lvl);
  } else {
    fadeout = end = duration;
  }

  event = gst_pad_get_sticky_event (item->mixer_sink, GST_EVENT_SEGMENT, 0);
  gst_event_parse_segment (event, &segment);

  item->duration = duration;
  item->fadeout_rt = gst_segment_to_running_time (segment, GST_FORMAT_TIME,
      gst_segment_position_from_stream_time (segment, GST_FORMAT_TIME,
          fadeout));
  item->end_rt = gst_segment_to_running_time (segment, GST_FORMAT_TIME,
      gst_segment_position_from_stream_time (segment, GST_FORMAT_TIME,
          end));
  gst_event_unref (event);

  utils_dbg (PLR, "item %p: duration is %" GST_TIME_FORMAT "\n", item,
      GST_TIME_ARGS (duration));
  utils_dbg (PLR, "\tfadeout starts at running time: %" GST_TIME_FORMAT "\n",
      GST_TIME_ARGS (item->fadeout_rt));
  utils_dbg (PLR, "\titem ends at running time: %" GST_TIME_FORMAT "\n",
      GST_TIME_ARGS (item->end_rt));

  item->player->sched_unix_time += end / GST_USECOND;

  /* make sure we have enough items linked */
  g_idle_add ((GSourceFunc) player_ensure_next, item->player);

  return GST_PAD_PROBE_REMOVE;
}

static GstPadProbeReturn
mixer_sinkpad_event_probe (GstPad * pad, GstPadProbeInfo * info,
    struct play_queue_item * item)
{
  GstEvent *event = gst_pad_probe_info_get_event (info);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_EOS:
      g_idle_add ((GSourceFunc) player_handle_item_eos, item);
      return GST_PAD_PROBE_REMOVE;

    default:
      break;
  }

  return GST_PAD_PROBE_OK;
}

static void
decodebin_pad_added (GstElement * decodebin, GstPad * src, GstPad * sink)
{
  gst_pad_link (src, sink);
}

static struct play_queue_item *
play_queue_item_new (struct player * self, struct play_queue_item * previous)
{
  struct play_queue_item *item;
  struct fader *fader;
  gchar *file;
  gchar *uri;
  GError *error = NULL;
  time_t sched_time_secs;
  GstElement *decodebin;
  GstElement *audioconvert;
  GstPad *convert_src, *convert_sink;
  GstPad *ghost;
  GstClockTime offset = 0;

  /* time_t is in seconds, sched_unix_time is in microseconds */
  sched_time_secs =
      gst_util_uint64_scale (self->sched_unix_time, GST_USECOND, GST_SECOND);

next:
  /* ask scheduler for the next item */
  if (sched_get_next (self->scheduler, sched_time_secs, &file, &fader) != 0) {
    utils_err (PLR, "No more files to play!!\n");
    return NULL;
  }

  /* convert to file:// URI */
  uri = gst_filename_to_uri (file, &error);
  if (error) {
    utils_wrn (PLR, "Failed to convert filename '%s' to URI: %s\n", file,
        error->message);
    g_clear_error (&error);
    goto next;
  }

  item = g_new0 (struct play_queue_item, 1);
  item->player = self;
  item->previous = previous;
  item->file = g_strdup (file);

  utils_dbg (PLR, "item %p: scheduling to play '%s'\n", item, uri);

  /* configure fade properties */
  if (fader) {
    item->fader = *fader;
  } else {
    item->fader.fadein_duration_secs = 0;
    item->fader.fadeout_duration_secs = 0;
  }

  item->bin = gst_bin_new (NULL);
  gst_bin_add (GST_BIN (self->pipeline), item->bin);

  /* create the decodebin and link it */
  decodebin = gst_element_factory_make ("uridecodebin", NULL);
  gst_util_set_object_arg (G_OBJECT (decodebin), "caps", "audio/x-raw");
  g_object_set (decodebin, "uri", uri, NULL);
  gst_bin_add (GST_BIN (item->bin), decodebin);

  /* plug audioconvert in between;
   * audiomixer cannot handle different formats on different sink pads */
  audioconvert = gst_parse_bin_from_description (
      "audioconvert ! audioresample", TRUE, NULL);
  gst_bin_add (GST_BIN (item->bin), audioconvert);

  /* link the audioconvert bin's src pad to the audiomixer's sink */
  item->mixer_sink = gst_element_get_request_pad (self->mixer, "sink_%u");

  convert_src = gst_element_get_static_pad (audioconvert, "src");
  ghost = gst_ghost_pad_new ("src", convert_src);
  gst_element_add_pad (item->bin, ghost);
  gst_pad_link (ghost, item->mixer_sink);
  gst_object_unref (convert_src);

  /* and the decodebin's src pad to the audioconvert bin's sink */
  convert_sink = gst_element_get_static_pad (audioconvert, "sink");
  g_signal_connect_object (decodebin, "pad-added",
      (GCallback) decodebin_pad_added, convert_sink, 0);
  gst_object_unref (convert_sink);

  /* link the pad to a sink of the mixer and add probes */
  gst_pad_add_probe (ghost,
      GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BLOCK,
      (GstPadProbeCallback) itembin_srcpad_buffer_probe, item, NULL);
  gst_pad_add_probe (item->mixer_sink, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      (GstPadProbeCallback) mixer_sinkpad_event_probe, item, NULL);

  /* start mixing this stream in the future; if there is a fade in,
   * start at the time the previous stream starts fading out,
   * otherwise start at the end of the previous stream */
  if (item->previous && item->previous->end_rt > 0) {
    if (item->fader.fadein_duration_secs > 0)
      offset = item->previous->fadeout_rt;
    else
      offset = item->previous->end_rt;

    gst_pad_set_offset (item->mixer_sink, offset);
  }

  item->start_rt = offset;

  gst_element_sync_state_with_parent (item->bin);

  utils_dbg (PLR, "item %p: created, linked to %s:%s, start_rt: %"
      GST_TIME_FORMAT "\n", item, GST_DEBUG_PAD_NAME (item->mixer_sink),
      GST_TIME_ARGS (item->start_rt));

  g_free (uri);
  return item;
}

static void
play_queue_item_free (struct play_queue_item * item)
{
  utils_dbg (PLR, "item %p: freeing item\n", item);

  g_free (item->file);

  gst_element_set_locked_state (item->bin, TRUE);
  gst_element_set_state (item->bin, GST_STATE_NULL);
  gst_bin_remove (GST_BIN (item->player->pipeline), item->bin);

  if (item->mixer_sink) {
    gst_element_release_request_pad (item->player->mixer, item->mixer_sink);
    gst_object_unref (item->mixer_sink);
  }

  g_free (item);
}

static void
play_queue_item_set_fade (struct play_queue_item * item,
    GstClockTime start, gdouble start_value, GstClockTime end,
    gdouble end_value)
{
  GstControlBinding *binding;
  GstControlSource *cs;
  GstTimedValueControlSource *tvcs;

  utils_dbg (PLR, "item %p: scheduling fade from %lf (@ %" GST_TIME_FORMAT ") "
      "to %lf (@ %" GST_TIME_FORMAT ")\n", item,
      start_value, GST_TIME_ARGS (start), end_value, GST_TIME_ARGS (end));

  cs = gst_interpolation_control_source_new ();
  tvcs = GST_TIMED_VALUE_CONTROL_SOURCE (cs);
  g_object_set (cs, "mode", GST_INTERPOLATION_MODE_LINEAR, NULL);
  gst_timed_value_control_source_set (tvcs, start, start_value);
  gst_timed_value_control_source_set (tvcs, end, end_value);

  binding = gst_direct_control_binding_new_absolute (
      GST_OBJECT_CAST (item->mixer_sink), "volume", cs);
  gst_object_add_control_binding (GST_OBJECT_CAST (item->mixer_sink),
      binding);
  gst_object_unref (cs);
}

static gboolean
player_ensure_next (struct player * self)
{
  if (!self->playlist->next)
    self->playlist->next = play_queue_item_new (self, self->playlist);
  return G_SOURCE_REMOVE;
}

static gboolean
player_recycle_item (struct play_queue_item * item)
{
  struct player * self = item->player;
  struct play_queue_item ** ptr;

  utils_dbg (PLR, "recycling item %p\n", item);

  /* this can happen when the very first loaded item fails to play
   * and we want to recycle it, otherwise normally it's the ->next
   * item that we recycle */
  if (G_UNLIKELY (item == self->playlist))
    ptr = &self->playlist;
  else
    ptr = &self->playlist->next;

  g_assert (*ptr == item);
  g_assert (item->next == NULL);

  *ptr = play_queue_item_new (self, item->previous);

  play_queue_item_free (item);
  return G_SOURCE_REMOVE;
}

static gboolean
player_handle_item_eos (struct play_queue_item * item)
{
  struct player * self = item->player;

  utils_dbg (PLR, "item %p EOS\n", item);

  if (G_UNLIKELY (item == self->playlist->next)) {
    utils_wrn (PLR, "next item finished before the current; corrupt file?\n");
    player_recycle_item (item);
    return G_SOURCE_REMOVE;
  }

  g_assert (item == self->playlist);

  self->playlist = self->playlist->next;
  player_ensure_next (self);

  play_queue_item_free (item);
  return G_SOURCE_REMOVE;
}

static gboolean
player_bus_watch (GstBus *bus, GstMessage *msg, struct player *self)
{
  const GstStructure *s;
  GError *error;
  gchar *debug = NULL;
  const gchar *name;
  gint i;

  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_EOS:
      /* EOS can only be received when audiomixer receives GST_EVENT_EOS
       * on a sink and has no other sink with more data available at that
       * moment, which can only happen if the scheduler stopped giving
       * us new files to enqueue */
      utils_info (PLR, "we got EOS, which means there is no file "
          "in the play queue; exiting...\n");
      g_main_loop_quit (self->loop);
      break;

    case GST_MESSAGE_INFO:
      gst_message_parse_info (msg, &error, &debug);
      utils_info (PLR, "INFO from element %s: %s\n",
          GST_OBJECT_NAME (GST_MESSAGE_SRC (msg)),
          error->message);
      g_clear_error (&error);
      if (debug) {
        utils_info (PLR, "INFO debug message: %s\n", debug);
        g_free (debug);
      }
      break;

    case GST_MESSAGE_WARNING:
      gst_message_parse_warning (msg, &error, &debug);
      utils_wrn (PLR, "WARNING from element %s: %s\n",
          GST_OBJECT_NAME (GST_MESSAGE_SRC (msg)),
          error->message);
      g_clear_error (&error);
      if (debug) {
        utils_wrn (PLR, "WARNING debug message: %s\n", debug);
        g_free (debug);
      }
      break;

    case GST_MESSAGE_ERROR:
    {
      struct play_queue_item *item = self->playlist->next ?
          self->playlist->next : self->playlist;

      gst_message_parse_error (msg, &error, &debug);
      utils_wrn (PLR, "ERROR from element %s: %s\n",
          GST_OBJECT_NAME (GST_MESSAGE_SRC (msg)),
          error->message);
      g_clear_error (&error);
      if (debug) {
        utils_wrn (PLR, "ERROR debug message: %s\n", debug);
        g_free (debug);
      }

      /* check if the message came from an item's bin and attempt to recover;
       * it is possible to get an error there, in case of an unsupported
       * codec for example, or maybe a file read error... */
      if (gst_object_has_as_ancestor (GST_MESSAGE_SRC (msg),
                GST_OBJECT (item->bin))) {
        /*
         * this is the last item in the queue; for this case we can recover
         * by calling the recycle function
         */
        utils_info (PLR, "error message originated from the next "
              "item's bin; recycling item\n");

        player_recycle_item (item);

        /* ensure the pipeline is PLAYING state;
         * error messages tamper with it */
        gst_element_set_state (self->pipeline, GST_STATE_PLAYING);

      } else if (self->playlist->next && gst_object_has_as_ancestor (
                GST_MESSAGE_SRC (msg),
                GST_OBJECT (self->playlist->bin))) {
        /*
         * this is the decodebin of the currently playing item, but we
         * have already linked the next item; no graceful recover here...
         * we need to get rid of the next item, then recycle the current one;
         * there *will* be an audio glitch here.
         */
        utils_info (PLR, "error message originated from the current "
            "item's bin; recycling the whole playlist\n");

        play_queue_item_free (self->playlist->next);
        self->playlist->next = NULL;
        player_recycle_item (self->playlist);

        /* ensure the pipeline is PLAYING state;
         * error messages tamper with it */
        gst_element_set_state (self->pipeline, GST_STATE_PLAYING);

      } else if (!gst_object_has_as_ancestor (GST_MESSAGE_SRC (msg),
                GST_OBJECT (self->pipeline))) {
        /*
         * this is an element that we have already removed from the pipeline.
         * this can happen for example when a decodebin posts 2 errors in a row
         */
        utils_info (PLR, "error message originated from already removed item; "
            "ignoring\n");

      } else {
        utils_err (PLR, "error originated from a critical element; "
          "the pipeline cannot continue working, sorry!\n");
        g_main_loop_quit (self->loop);
      }

      break;
    }
    default:
      break;
  }

  return G_SOURCE_CONTINUE;
}

static void
populate_song_info (struct play_queue_item * item, struct song_info * song)
{
  GstEvent *tag_event;
  GstTagList *taglist = NULL;
  gint64 pos = GST_CLOCK_TIME_NONE;

  /* cleanup song_info */
  g_clear_pointer (&song->path, g_free);
  g_clear_pointer (&song->artist, g_free);
  g_clear_pointer (&song->album, g_free);
  g_clear_pointer (&song->title, g_free);
  song->duration_sec = song->elapsed_sec = 0;

  if (!item)
    return;

  song->path = g_strdup (item->file);

  tag_event = gst_pad_get_sticky_event (item->mixer_sink, GST_EVENT_TAG, 0);
  if (tag_event)
    gst_event_parse_tag (tag_event, &taglist);

  if (taglist) {
    gst_tag_list_get_string (taglist, GST_TAG_ARTIST, &song->artist);
    gst_tag_list_get_string (taglist, GST_TAG_ALBUM, &song->album);
    gst_tag_list_get_string (taglist, GST_TAG_TITLE, &song->title);
  }

  if (gst_pad_peer_query_position (item->mixer_sink, GST_FORMAT_TIME, &pos))
    song->elapsed_sec =
        (uint32_t) gst_util_uint64_scale_round (pos, 1, GST_SECOND);
  song->duration_sec =
      (uint32_t) gst_util_uint64_scale_round (item->duration, 1, GST_SECOND);

  if (tag_event)
    gst_event_unref (tag_event);
}

static gboolean
refresh_metadata (struct player * self)
{
  struct current_state *mstate;

  mstate = meta_get_state (self->mh);
  pthread_mutex_lock (&mstate->proc_mutex);

  populate_song_info (self->playlist, &mstate->current);
  populate_song_info (self->playlist->next, &mstate->next);

  if (self->playlist->next)
    mstate->overlap_sec = self->playlist->next->fader.fadein_duration_secs;

  pthread_mutex_unlock (&mstate->proc_mutex);

  return G_SOURCE_CONTINUE;
}

static void
cleanup_metadata (struct meta_handler *mh)
{
  struct current_state *mstate;

  mstate = meta_get_state (mh);
  pthread_mutex_lock (&mstate->proc_mutex);

  populate_song_info (NULL, &mstate->current);
  populate_song_info (NULL, &mstate->next);
  mstate->overlap_sec = 0;

  pthread_mutex_unlock (&mstate->proc_mutex);
}

int
player_init (struct player* self, struct scheduler* scheduler,
    struct meta_handler *mh, const char *audiosink)
{
  GstElement *sink = NULL;
  GstElement *convert = NULL;

  gst_init (NULL, NULL);

  self->scheduler = scheduler;
  self->mh = mh;
  self->loop = g_main_loop_new (NULL, FALSE);
  self->pipeline = gst_pipeline_new ("player");
  self->mixer = gst_element_factory_make ("audiomixer", NULL);
  convert = gst_element_factory_make ("audioconvert", NULL);
  if (audiosink) {
    GError *error = NULL;
    sink = gst_parse_bin_from_description (audiosink, TRUE, &error);
    if (error)
      utils_wrn (PLR, "Failed to parse audiosink description: %s\n",
          error->message);
    g_clear_error (&error);
  }

  if (!sink)
    sink = gst_element_factory_make ("autoaudiosink", NULL);

  if (!self->mixer || !convert || !sink) {
    utils_err (PLR, "Your GStreamer installation is missing required elements\n");
    g_clear_object (&self->mixer);
    g_clear_object (&convert);
    g_clear_object (&sink);
    return -1;
  }

  gst_bin_add_many (GST_BIN (self->pipeline), self->mixer, convert, sink, NULL);
  if (!gst_element_link_many (self->mixer, convert, sink, NULL)) {
    utils_err (PLR, "Failed to link audiomixer to audio sink. Check caps\n");
    return -1;
  }

  utils_dbg (PLR, "player initialized\n");

  return 0;
}

void
player_cleanup (struct player* self)
{
  g_clear_object (&self->pipeline);
  g_clear_pointer (&self->loop, g_main_loop_unref);

  memset (self, 0, sizeof (struct player));

  utils_dbg (PLR, "player destroyed\n");
}

void
player_loop (struct player* self)
{
  GstBus *bus;
  guint timeout_id;

  self->sched_unix_time = g_get_real_time ();
  self->playlist = play_queue_item_new (self, NULL);

  bus = gst_pipeline_get_bus (GST_PIPELINE (self->pipeline));
  gst_bus_add_watch (bus, (GstBusFunc) player_bus_watch, self);

  timeout_id = g_timeout_add_seconds (1, (GSourceFunc) refresh_metadata, self);

  utils_dbg (PLR, "Beginning playback\n");
  gst_element_set_state (self->pipeline, GST_STATE_PLAYING);

  g_main_loop_run (self->loop);

  gst_element_set_state (self->pipeline, GST_STATE_NULL);
  utils_dbg (PLR, "Playback stopped\n");

  g_source_remove (timeout_id);
  cleanup_metadata (self->mh);

  gst_bus_remove_watch (bus);
  g_object_unref (bus);

  if (self->playlist->next)
    play_queue_item_free (self->playlist->next);
  play_queue_item_free (self->playlist);
}

void
player_loop_quit (struct player* self)
{
  g_main_loop_quit (self->loop);
}
