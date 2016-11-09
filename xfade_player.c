/*
 * Audio Scheduler - An audio clip scheduler for use in radio broadcasting
 * Main header file
 *
 * Copyright (C) 2016 George Kiagiadakis <gkiagia@tolabaki.gr>
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

#include "xfade_player.h"

#define parent_class xfade_player_parent_class
G_DEFINE_TYPE (XFadePlayer, xfade_player, GST_TYPE_PIPELINE);


static XFadeStream*
xfade_player_create_stream (XFadePlayer *self)
{
  XFadeStream *stream;
  gchar *next_uri = NULL;
  //TODO call get_next to fill next_uri and the numbers below

  stream = g_object_new (XFADE_TYPE_STREAM,
        "uri", next_uri,
        "fadein-start-level", 1.0,
        "fadeout-end-level", 1.0,
        "fadein-end-pos", 0,
        "fadeout-start-pos", -1,
        "fadeout-end-pos", -1,
        NULL);
  g_object_ref_sink (stream);

  gst_bin_add (GST_BIN (self), GST_ELEMENT (stream));

  if (!xfade_stream_preroll (stream)) {
    gst_bin_remove (GST_BIN (self), GST_ELEMENT (stream));
    g_object_unref (stream);
    stream = NULL;
  }

  GST_DEBUG_OBJECT (self, "created new stream %" GST_PTR_FORMAT, stream);

  return stream;
}

static void
xfade_player_link_stream (XFadePlayer *self,
    XFadeStream *stream)
{
  GstPad *src_pad;
  GstPad *adder_pad;

  GST_DEBUG_OBJECT (self, "linking stream %" GST_PTR_FORMAT, stream);

  src_pad = gst_element_get_static_pad (GST_ELEMENT (stream), "src");
  adder_pad = gst_element_get_request_pad (self->adder, "sink_%d");

  gst_pad_link (src_pad, adder_pad);

  g_object_unref (src_pad);
  g_object_unref (adder_pad);

  xfade_stream_play (stream);
}

static void
xfade_player_unlink_stream (XFadePlayer *self,
    XFadeStream *stream)
{
  GstPad *src_pad;
  GstPad *adder_pad;

  GST_DEBUG_OBJECT (self, "unlinking stream %" GST_PTR_FORMAT, stream);

  gst_element_set_state (GST_ELEMENT (stream), GST_STATE_NULL);

  src_pad = gst_element_get_static_pad (GST_ELEMENT (stream), "src");
  adder_pad = gst_pad_get_peer (src_pad);

  gst_pad_unlink (src_pad, adder_pad);

  g_object_unref (src_pad);
  gst_element_release_request_pad (self->adder, adder_pad);
  g_object_unref (adder_pad);

  gst_bin_remove (GST_BIN (self), GST_ELEMENT (stream));
}

static void
xfade_player_switch_streams (XFadePlayer *self,
    XFadeStream *old_stream)
{
  g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&self->mutex);

  /* looks like we've already switched, nothing to do */
  if (old_stream != self->cur_stream)
    return;

  GST_DEBUG_OBJECT (self, "Switching streams %" GST_PTR_FORMAT
      " -> %" GST_PTR_FORMAT, self->cur_stream, self->next_stream);

  //TODO handle the possibility that there is no next_stream available.

  gst_object_replace ((GstObject **) &self->cur_stream,
      (GstObject *) self->next_stream);
  gst_object_replace ((GstObject **) &self->next_stream, NULL);

  gst_element_post_message (GST_ELEMENT (self),
      gst_message_new_application (GST_OBJECT (self),
              gst_structure_new ("player-need-next-stream", NULL)));

  xfade_player_link_stream (self, self->cur_stream);
}

/* sync bus handler */
static GstBusSyncReply
xfade_player_sync_bus_handler (GstBus *bus,
    GstMessage *msg,
    XFadePlayer *self)
{
  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_APPLICATION) {
    const GstStructure *s = gst_message_get_structure (msg);
    /* This catches both "stream-fadeout-start" and "stream-eos"
     * In both cases, we need to switch to the next stream, if that
     * has not already happened */
    if (g_str_has_prefix (gst_structure_get_name (s), "stream-")) {
      xfade_player_switch_streams (self, XFADE_STREAM (GST_MESSAGE_SRC (msg)));
    }
  }

  return GST_BUS_PASS;
}

/* async bus handler (on a thread) */
static gpointer
xfade_player_bus_thread (XFadePlayer *self)
{
  GstBus *bus = gst_pipeline_get_bus (GST_PIPELINE (self));
  GstMessage *msg;
  const GstStructure *s;

  gst_bus_set_sync_handler (bus,
      (GstBusSyncHandler) xfade_player_sync_bus_handler, self, NULL);

  while (TRUE) {
    /* block indefinitely waiting for interesting messages */
    msg = gst_bus_timed_pop_filtered (bus, GST_CLOCK_TIME_NONE,
        GST_MESSAGE_APPLICATION |
        GST_MESSAGE_ERROR |
        GST_MESSAGE_TAG);

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_APPLICATION:
        s = gst_message_get_structure (msg);

        if (g_str_equal (s, "stream-eos")) {
          XFadeStream *stream = XFADE_STREAM (GST_MESSAGE_SRC (msg));

          /* This stream was switched out from the sync bus handler */
          g_assert (stream != self->cur_stream);

          /* This is where we unlink and dispose streams.
           * Basically we wait until they push EOS to the adder and
           * then we dispose them asynchronously from this thread */
          xfade_player_unlink_stream (self, stream);
        }
        else if (g_str_equal (s, "player-need-next-stream")) {
          g_mutex_lock (&self->mutex);

          g_assert (self->next_stream == NULL);
          self->next_stream = xfade_player_create_stream (self);

          if (!self->next_stream) {
            /* couldn't load a next stream, try again but async
             * - no blocking in this function */
            gst_bus_post (bus, gst_message_new_application (GST_OBJECT (self),
                      gst_structure_new ("player-need-next-stream", NULL)));
          }
          g_mutex_unlock (&self->mutex);
        }
        else if (g_str_equal (s, "quit-bus-thread")) {
          goto exit;
        }
        break;
      case GST_MESSAGE_ERROR:
        //TODO handle errors gracefully
        break;
      case GST_MESSAGE_TAG:
        //TODO catch metadata and do something with it
        break;
      default:
        break;
    }

    gst_message_unref (msg);
  }

exit:
  gst_message_unref (msg);
  gst_bus_set_sync_handler (bus, NULL, NULL, NULL);
  g_object_unref (bus);
  return NULL;
}

static void
xfade_player_init (XFadePlayer *self)
{
  self->adder = gst_element_factory_make("adder", NULL);
  self->audiosink = gst_element_factory_make("autoaudiosink", NULL);

  gst_bin_add_many (GST_BIN (self),
      self->adder,
      self->audiosink,
      NULL);
  gst_element_link (self->adder, self->audiosink);

  g_mutex_init (&self->mutex);
  self->bus_thread = g_thread_new ("bus_thread",
      (GThreadFunc) xfade_player_bus_thread, self);
}

static void
xfade_player_dispose (GObject *gobject)
{
  XFadePlayer *self = XFADE_PLAYER (gobject);
  GstBus *bus;

  bus = gst_pipeline_get_bus (GST_PIPELINE (self));
  gst_bus_post (bus, gst_message_new_application (GST_OBJECT (self),
                      gst_structure_new ("quit-bus-thread", NULL)));
  g_object_unref (bus);

  G_OBJECT_CLASS (xfade_player_parent_class)->dispose (gobject);
}

static void
xfade_player_finalize (GObject *gobject)
{
  XFadePlayer *self = XFADE_PLAYER (gobject);

  g_thread_join (self->bus_thread);
  g_mutex_clear (&self->mutex);

  G_OBJECT_CLASS (xfade_player_parent_class)->finalize (gobject);
}

static GstStateChangeReturn
xfade_player_change_state (GstElement *element, GstStateChange change)
{
  XFadePlayer *self = XFADE_PLAYER (element);

  switch (change) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!(self->next_stream = xfade_player_create_stream (self)))
        return GST_STATE_CHANGE_FAILURE;
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      xfade_player_switch_streams (self, NULL);
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      g_clear_pointer (&self->cur_stream);
      g_clear_pointer (&self->next_stream);
      break;
    default:
      break;
  }

  return GST_ELEMENT_CLASS (parent_class)->change_state (element, change);
}

static void
xfade_player_class_init (XFadePlayerClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose = xfade_player_dispose;
  gobject_class->finalize = xfade_player_finalize;

  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  gstelement_class->change_state = xfade_player_change_state;
}

XFadePlayer*
xfade_player_new (void)
{
  return (XFadePlayer*) g_object_new (XFADE_TYPE_PLAYER, NULL);
}
