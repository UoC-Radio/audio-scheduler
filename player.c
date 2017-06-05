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

static void player_link_next_async (struct player *self);
static void player_relink_current_async (struct player *self);

static struct play_queue_item *
play_queue_get_next (struct player *self)
{
  gint ptr = self->play_queue_ptr;
  if (++ptr >= PLAY_QUEUE_SIZE)
    ptr = 0;
  return &self->play_queue[ptr];
}

static struct play_queue_item *
play_queue_get_previous (struct player *self)
{
  gint ptr = self->play_queue_ptr;
  if (--ptr < 0)
    ptr = PLAY_QUEUE_SIZE - 1;
  return &self->play_queue[ptr];
}

static void
play_queue_item_cleanup (struct play_queue_item * item)
{
  if (item->file) {
    utils_dbg (PLR, "item %p: cleaning up\n", item);
    g_free (item->file);
    item->file = NULL;
    if (item->decodebin) {
      gst_element_set_locked_state (item->decodebin, TRUE);
      gst_element_set_state (item->decodebin, GST_STATE_NULL);
      gst_bin_remove (GST_BIN (item->player->pipeline), item->decodebin);
      item->decodebin = NULL;
    }
    if (item->mixer_sink) {
      gst_element_release_request_pad (item->player->mixer, item->mixer_sink);
      gst_object_unref (item->mixer_sink);
      item->mixer_sink = NULL;
    }
  }
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

static GstPadProbeReturn
mixer_sink_event (GstPad * pad, GstPadProbeInfo * info,
    struct play_queue_item * item)
{
  GstEvent *event = gst_pad_probe_info_get_event (info);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEGMENT:
    {
      /*
       * We got a segment, so it's now safe to execute the duration query.
       * The demuxer obviously knows this information at this time.
       */
      gint64 duration;
      GstPad *peer;
      const GstSegment *segment;
      GstClockTime fadeout, end;

      peer = gst_pad_get_peer (pad);
      if (!gst_pad_query_duration (peer, GST_FORMAT_TIME, &duration) ||
                duration <= 0)
      {
        utils_wrn (PLR, "item %p: unknown file duration, consider remuxing; "
            "skipping playback: %s\n", item, item->file);

        /* schedule recycling this item */
        player_relink_current_async (item->player);

        /* and get out of here... */
        gst_object_unref (peer);
        break;
      }
      gst_object_unref (peer);

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

      gst_event_parse_segment (event, &segment);

      item->duration = duration;
      item->fadeout_rt = gst_segment_to_running_time (segment, GST_FORMAT_TIME,
          gst_segment_position_from_stream_time (segment, GST_FORMAT_TIME,
              fadeout));
      item->end_rt = gst_segment_to_running_time (segment, GST_FORMAT_TIME,
          gst_segment_position_from_stream_time (segment, GST_FORMAT_TIME,
              end));

      utils_dbg (PLR, "item %p: duration is %" GST_TIME_FORMAT "\n", item,
          GST_TIME_ARGS (duration));
      utils_dbg (PLR, "\tfadeout starts at running time: %" GST_TIME_FORMAT "\n",
          GST_TIME_ARGS (item->fadeout_rt));
      utils_dbg (PLR, "\titem ends at running time: %" GST_TIME_FORMAT "\n",
          GST_TIME_ARGS (item->end_rt));

      item->player->sched_unix_time += end / GST_USECOND;

      /* make sure we have enough items linked */
      player_link_next_async (item->player);
      break;
    }
    case GST_EVENT_EOS:
    {
      /*
       * this item has finished playing, call link_next()
       * to link another item in its place
       */
      g_atomic_int_set (&item->active, 0);
      player_link_next_async (item->player);
      break;
    }
    default:
      break;
  }

  return GST_PAD_PROBE_OK;
}

static void
decodebin_pad_added (GstElement * decodebin, GstPad * pad,
    struct play_queue_item * item)
{
  GstClockTime offset = 0;
  struct play_queue_item *previous = play_queue_get_previous (item->player);

  /* link the pad to a sink of the mixer and add a probe there to peek events */
  item->mixer_sink =
      gst_element_get_request_pad (item->player->mixer, "sink_%u");
  gst_pad_add_probe (item->mixer_sink, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      (GstPadProbeCallback) mixer_sink_event, item, NULL);
  gst_pad_link (pad, item->mixer_sink);

  /* start mixing this stream in the future; if there is a fade in,
   * start at the time the previous stream starts fading out,
   * otherwise start at the end of the previous stream */
  if (previous->end_rt > 0) {
    if (item->fader.fadein_duration_secs > 0)
      offset = previous->fadeout_rt;
    else
      offset = previous->end_rt;

    gst_pad_set_offset (item->mixer_sink, offset);
  }

  item->start_rt = offset;

  utils_dbg (PLR, "item %p: decodebin pad added, linked to %s:%s, start_rt: %"
      GST_TIME_FORMAT "\n", item, GST_DEBUG_PAD_NAME (item->mixer_sink),
      GST_TIME_ARGS (item->start_rt));
}

/*
 * This function gets the next item from the scheduler and links
 * the necessary elements to get it playing at the right time.
 * The state of this item is tracked in a play_queue_item.
 * If there are no available positions in the play_queue for linking
 * a new item, this function returns without doing anything.
 * This function is not thread safe and is meant to be called only
 * from the event loop thread. Other threads should use player_link_next_async()
 */
static void
player_link_next (struct player * self)
{
  struct play_queue_item *item;
  char *file;
  struct fader *fader;
  gchar *uri;
  GError *error = NULL;
  time_t sched_time_secs;

  /* check if the next item in the queue is available for recycling */
  item = play_queue_get_next (self);
  if (g_atomic_int_get (&item->active))
    return;

  /* advance pointer to the next item */
  if (++self->play_queue_ptr >= PLAY_QUEUE_SIZE)
    self->play_queue_ptr = 0;

  /* release resources if they were previously on use */
  play_queue_item_cleanup (item);

  /* time_t is in seconds, sched_unix_time is in microseconds */
  sched_time_secs =
      gst_util_uint64_scale (self->sched_unix_time, GST_USECOND, GST_SECOND);

next:
  /* ask scheduler for the next item */
  if (sched_get_next (self->scheduler, sched_time_secs, &file, &fader) != 0) {
    utils_err (PLR, "No more files to play!!\n");
    return;
  }

  /* convert to file:// URI */
  uri = gst_filename_to_uri (file, &error);
  if (error) {
    utils_wrn (PLR, "Failed to convert filename '%s' to URI: %s\n", file,
        error->message);
    g_clear_error (&error);
    goto next;
  }

  item->file = g_strdup (file);
  g_atomic_int_set (&item->active, 1);

  utils_dbg (PLR, "item %p: scheduling to play '%s'\n", item, uri);

  /* configure fade properties */
  if (fader) {
    item->fader = *fader;
  } else {
    item->fader.fadein_duration_secs = 0;
    item->fader.fadeout_duration_secs = 0;
  }

  /* create the decodebin and link it */
  item->decodebin = gst_element_factory_make ("uridecodebin", NULL);
  gst_util_set_object_arg (G_OBJECT (item->decodebin), "caps", "audio/x-raw");
  g_object_set (item->decodebin, "uri", uri, NULL);
  g_signal_connect (item->decodebin, "pad-added",
      (GCallback) decodebin_pad_added, item);
  gst_bin_add (GST_BIN (self->pipeline), item->decodebin);
  gst_element_sync_state_with_parent (item->decodebin);

  g_free (uri);
  /* continued async in decodebin_pad_added */
}

/*
 * This function posts a message on the bus that triggers calling
 * player_link_next() in the event loop thread.
 */
static void
player_link_next_async (struct player *self)
{
  GstBus *bus;
  GstMessage *msg;

  msg = gst_message_new_application (GST_OBJECT (self->pipeline),
      gst_structure_new_empty ("link-next"));
  bus = gst_pipeline_get_bus (GST_PIPELINE (self->pipeline));
  gst_bus_post (bus, msg);
  g_object_unref (bus);
}

static void
player_relink_current (struct player *self)
{
  struct play_queue_item *item;

  /* mark the current item as inactive */
  item = &self->play_queue[self->play_queue_ptr];
  g_atomic_int_set (&item->active, 0);

  /* go back one item in the play queue */
  if (--self->play_queue_ptr < 0)
    self->play_queue_ptr = PLAY_QUEUE_SIZE - 1;

  /* then call player_link_next() to recycle it */
  player_link_next (self);
}

static void
player_relink_current_async (struct player *self)
{
  GstBus *bus;
  GstMessage *msg;

  msg = gst_message_new_application (GST_OBJECT (self->pipeline),
      gst_structure_new_empty ("relink-current"));
  bus = gst_pipeline_get_bus (GST_PIPELINE (self->pipeline));
  gst_bus_post (bus, msg);
  g_object_unref (bus);
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
    case GST_MESSAGE_APPLICATION:
      s = gst_message_get_structure (msg);
      name = gst_structure_get_name (s);

      if (g_str_equal (name, "link-next")) {
        player_link_next (self);
      } else if (g_str_equal (name, "relink-current")) {
        player_relink_current (self);
      }
      break;

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
      gst_message_parse_error (msg, &error, &debug);
      utils_wrn (PLR, "ERROR from element %s: %s\n",
          GST_OBJECT_NAME (GST_MESSAGE_SRC (msg)),
          error->message);
      g_clear_error (&error);
      if (debug) {
        utils_wrn (PLR, "ERROR debug message: %s\n", debug);
        g_free (debug);
      }

      /* check if the message came from a decodebin and attempt to recover;
       * it is possible to get an error there, in case of an unsupported
       * codec for example, or maybe a file read error... */
      for (i = 0; i < PLAY_QUEUE_SIZE; i++) {
        gint ptr;
        struct play_queue_item *item;

        ptr = self->play_queue_ptr - i;
        if (ptr < 0)
          ptr = PLAY_QUEUE_SIZE - ptr;
        item = &self->play_queue[ptr];

        if (item->decodebin && gst_object_has_as_ancestor (GST_MESSAGE_SRC (msg),
                  GST_OBJECT (item->decodebin)))
        {
          if (i == 0) {
            /*
             * i == 0 --> we decreased play_queue_ptr by 0, so this is the
             * item that we linked last; for this case we can recover by
             * getting the next file in that play queue slot
             */
            utils_info (PLR, "error message originated from last play "
                "queue item's decodebin; attempting to recover\n");

            player_relink_current (self);
          } else {
            /*
             * i > 0, so this is a decodebin that we had plugged earlier
             * and probably worked before; we can't do much here, we just
             * leave it deactivated and hope that the play queue will catch
             * up and fill that slot later. there will *probably* be an audio
             * gap here, of course.
             */
            utils_info (PLR, "error message originated from a play queue "
                "item's decodebin; deactivating and hoping for the best\n");

            g_atomic_int_set (&item->active, 0);
          }

          /* break out of the for loop; we found the guilty element already */
          break;
        }
      }

      /* if we didn't find the guilty element... */
      if (i == PLAY_QUEUE_SIZE) {
        utils_err (PLR, "error originated from a critical element; "
            "the pipeline cannot continue working, sorry!\n");
        g_main_loop_quit (self->loop);
      }

      break;
    default:
      break;
  }

  return G_SOURCE_CONTINUE;
}

int
player_init (struct player* self, struct scheduler* scheduler)
{
  GstElement *sink;
  int i;

  gst_init (NULL, NULL);

  self->scheduler = scheduler;
  self->loop = g_main_loop_new (NULL, FALSE);
  self->pipeline = gst_pipeline_new ("player");
  self->mixer = gst_element_factory_make ("audiomixer", NULL);
  sink = gst_element_factory_make ("autoaudiosink", NULL);

  if (!self->mixer || !sink) {
    utils_err (PLR, "Your GStreamer installation is missing required elements\n");
    g_clear_object (&self->mixer);
    g_clear_object (&sink);
    player_cleanup (self);
    return -1;
  }

  gst_bin_add_many (GST_BIN (self->pipeline), self->mixer, sink, NULL);
  if (!gst_element_link (self->mixer, sink)) {
    utils_err (PLR, "Failed to link audiomixer to audio sink. Check caps\n");
    player_cleanup (self);
    return -1;
  }

  /* initialize the player pointer in all items; this is needed for callbacks */
  for (i = 0; i < PLAY_QUEUE_SIZE; i++)
    self->play_queue[i].player = self;

  utils_dbg (PLR, "player initialized\n");

  return 0;
}

void
player_cleanup (struct player* self)
{
  gst_element_set_state (self->pipeline, GST_STATE_NULL);
  g_clear_object (&self->pipeline);
  g_clear_pointer (&self->loop, g_main_loop_unref);

  memset (self, 0, sizeof (struct player));

  utils_dbg (PLR, "player destroyed\n");
}

void
player_loop (struct player* self)
{
  GstBus *bus;
  gint i;

  self->sched_unix_time = g_get_real_time ();
  player_link_next (self);

  bus = gst_pipeline_get_bus (GST_PIPELINE (self->pipeline));
  gst_bus_add_watch (bus, (GstBusFunc) player_bus_watch, self);

  utils_dbg (PLR, "Beginning playback\n");
  gst_element_set_state (self->pipeline, GST_STATE_PLAYING);

  g_main_loop_run (self->loop);

  gst_element_set_state (self->pipeline, GST_STATE_NULL);
  utils_dbg (PLR, "Playback stopped\n");

  gst_bus_remove_watch (bus);
  g_object_unref (bus);

  for (i = 0; i < PLAY_QUEUE_SIZE; i++) {
    self->play_queue[i].active = FALSE;
    play_queue_item_cleanup (&self->play_queue[i]);
  }
}

void
player_loop_quit (struct player* self)
{
  g_main_loop_quit (self->loop);
}
