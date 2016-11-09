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

#include "xfade_stream.h"
#include <gst/base/base.h>

G_DEFINE_TYPE (XFadeStream, xfade_stream, GST_TYPE_BIN);

enum {
  PROP_0,
  PROP_URI,
  PROP_FADEIN_START_LEVEL,
  PROP_FADEOUT_END_LEVEL,
  PROP_FADEIN_END_POS,
  PROP_FADEOUT_START_POS,
  PROP_FADEOUT_END_POS,
  PROP_LIVE,
  N_PROPERTIES
};

static GParamSpec *obj_properties[N_PROPERTIES] = { NULL, };

static void
decodebin_pad_added_cb (GstElement *element,
    GstPad *pad,
    XFadeStream *self)
{
  GstCaps *caps;
  GstStructure *structure;
  GstPad *sink;

  caps = gst_pad_get_current_caps (pad);
  structure = gst_caps_get_structure (caps, 0);
  if (g_str_equal (gst_structure_get_name (structure), "audio/x-raw")) {
    GST_DEBUG_OBJECT (self, "Linking decoded pad to chain");
    sink = gst_element_get_static_pad (self->audioconvert, "sink");
    gst_pad_link (pad, sink);
    g_object_unref (sink);
  }

  gst_caps_unref (caps);
}

static GstPadProbeReturn
src_pad_probe_cb (GstPad *pad,
    GstPadProbeInfo *info,
    XFadeStream *self)
{
  GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);
  GstMessage *msg = NULL;

  switch (GST_PAD_PROBE_INFO_TYPE (info)) {
    case GST_PAD_PROBE_TYPE_BUFFER_LIST:
    {
      GstBufferList *list = gst_pad_probe_info_get_buffer_list (info);
      buf = gst_buffer_list_get (list, gst_buffer_list_length (list) - 1);
      /* fall through */
    }
    case GST_PAD_PROBE_TYPE_BUFFER:
      if (GST_BUFFER_PTS (buf) > self->fadeout_start_pos) {
        msg = gst_message_new_application (GST_OBJECT (self),
            gst_structure_new ("stream-fadeout-started", NULL));
      }
      break;
    case GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM:
      if (GST_EVENT_TYPE (event) == GST_EVENT_EOS) {
        msg = gst_message_new_application (GST_OBJECT (self),
            gst_structure_new ("stream-eos", NULL));
      }
      break;
  }

  if (msg) {
    gst_element_post_message (GST_ELEMENT (self), msg);
  }

  return GST_PAD_PROBE_OK;
}

static void
xfade_stream_init (XFadeStream *self)
{
  GstPad *volume_src_pad;

  self->uridecodebin = gst_element_factory_make("uridecodebin", NULL);
  self->audioconvert = gst_element_factory_make("audioconvert", NULL);
  self->audioresample = gst_element_factory_make("audioresample", NULL);
  self->capsfilter = gst_element_factory_make("capsfilter", NULL);
  self->preroll_queue = gst_element_factory_make("queue", NULL);
  self->volume = gst_element_factory_make("volume", NULL);

  gst_bin_add_many (GST_BIN (self),
      self->uridecodebin,
      self->audioconvert,
      self->audioresample,
      self->capsfilter,
      self->preroll_queue,
      self->volume,
      NULL);

  /* link the part that we can statically link now */
  gst_element_link_many (self->audioconvert,
      self->audioresample,
      self->capsfilter,
      self->preroll_queue,
      self->volume,
      NULL);

  /* link the decoder later */
  g_signal_connect (self->uridecodebin, "pad-added",
      G_CALLBACK (decodebin_pad_added_cb), self);

  /* add a src pad */
  volume_src_pad = gst_element_get_static_pad (self->volume, "src");
  self->src_pad = gst_ghost_pad_new ("src", volume_src_pad);
  gst_element_add_pad (GST_ELEMENT (self), self->src_pad);

  /* install probe to monitor for progress and EOS */
  gst_pad_add_probe (self->src_pad, GST_PAD_PROBE_TYPE_DATA_DOWNSTREAM,
      (GstPadProbeCallback) src_pad_probe_cb,
      self, NULL);

  /* setup fader */
  self->fader =
      GST_TIMED_VALUE_CONTROL_SOURCE (gst_interpolation_control_source_new ());
  g_object_set (self->fader, "mode", GST_INTERPOLATION_MODE_LINEAR, NULL);
}

static void
xfade_stream_finalize (GObject *gobject)
{
  XFadeStream *self = XFADE_STREAM (gobject);

  g_object_unref (self->fader);

  G_OBJECT_CLASS (xfade_stream_parent_class)->finalize (gobject);
}

static void
xfade_stream_set_property (GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *pspec)
{
  XFadeStream *self = XFADE_STREAM (object);

  switch (property_id) {
    case PROP_URI:
      g_object_set_property (G_OBJECT (self->uridecodebin), "uri", value);
      break;
    case PROP_FADEIN_START_LEVEL:
      self->fadein_start_lvl = g_value_get_double (value);
      break;
    case PROP_FADEOUT_END_LEVEL:
      self->fadeout_end_lvl = g_value_get_double (value);
      break;
    case PROP_FADEIN_END_POS:
      self->fadein_end_pos = g_value_get_uint64 (value);
      break;
    case PROP_FADEOUT_START_POS:
      self->fadeout_start_pos = g_value_get_uint64 (value);
      break;
    case PROP_FADEOUT_END_POS:
      self->fadeout_end_pos = g_value_get_uint64 (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
xfade_stream_get_property (GObject *object,
    guint property_id,
    GValue *value,
    GParamSpec *pspec)
{
  XFadeStream *self = XFADE_STREAM (object);

  switch (property_id) {
    case PROP_URI:
      g_object_get_property (G_OBJECT (self->uridecodebin), "uri", value);
      break;
    case PROP_FADEIN_START_LEVEL:
      g_value_set_double (value, self->fadein_start_lvl);
      break;
    case PROP_FADEOUT_END_LEVEL:
      g_value_set_double (value, self->fadeout_end_lvl);
      break;
    case PROP_FADEIN_END_POS:
      g_value_set_uint64 (value, self->fadein_end_pos);
      break;
    case PROP_FADEOUT_START_POS:
      g_value_set_uint64 (value, self->fadeout_start_pos);
      break;
    case PROP_FADEOUT_END_POS:
      g_value_set_uint64 (value, self->fadeout_end_pos);
      break;
    case PROP_LIVE:
      g_value_set_boolean (value, self->live);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
xfade_stream_class_init (XFadeStreamClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = xfade_stream_finalize;
  gobject_class->set_property = xfade_stream_set_property;
  gobject_class->get_property = xfade_stream_get_property;

  obj_properties[PROP_URI] =
      g_param_spec_string ("uri", "URI", "The URI to play", "",
              G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE);

  obj_properties[PROP_FADEIN_START_LEVEL] =
      g_param_spec_double ("fadein-start-level", "Fade-in start level",
            "The starting level of the fade-in",
            0.0, 1.0, 1.0,
            G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE);

  obj_properties[PROP_FADEOUT_END_LEVEL] =
      g_param_spec_double ("fadeout-end-level", "Fade-out end level",
            "The ending level of the fade-out",
            0.0, 1.0, 1.0,
            G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE);

  obj_properties[PROP_FADEIN_END_POS] =
      g_param_spec_uint64 ("fadein-end-pos", "Fade-in ending position",
            "The ending position of the fade-in",
            0, GST_CLOCK_TIME_NONE, 0,
            G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE);

  obj_properties[PROP_FADEOUT_START_POS] =
      g_param_spec_uint64 ("fadeout-start-pos", "Fade-out starting position",
            "The starting position of the fade-out",
            0, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE,
            G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE);

  obj_properties[PROP_FADEOUT_END_POS] =
      g_param_spec_uint64 ("fadeout-end-pos", "Fade-out ending position",
            "The ending position of the fade-out",
            0, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE,
            G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE);

  obj_properties[PROP_LIVE] =
      g_param_spec_boolean ("live", "Live", "Whether the stream is live",
            FALSE, G_PARAM_STATIC_STRINGS | G_PARAM_READABLE);

  g_object_class_install_properties (gobject_class, N_PROPERTIES, obj_properties);
}

static GstPadProbeReturn
src_blocked_cb (GstPad *pad,
    GstPadProbeInfo *info,
    XFadeStream *self)
{
  GST_DEBUG_OBJECT (self, "src pad is now blocked");
  return GST_PAD_PROBE_OK;
}

gboolean
xfade_stream_preroll (XFadeStream *self)
{
  GstControlBinding *binding;
  GstStateChangeReturn ret;

  /* block the output */
  self->block_probe_id = gst_pad_add_probe (self->src_pad,
      GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
      (GstPadProbeCallback) src_blocked_cb,
      self, NULL);

  /* make sure there is no other binding on the volume property */
  binding = gst_object_get_control_binding (GST_OBJECT (self->volume), "volume");
  if (binding) {
    gst_object_remove_control_binding (GST_OBJECT (self->volume), binding);
    g_object_unref (binding);
  }

  /* configure the fader if needed */
  if (self->fadein || self->fadeout) {
    binding = gst_direct_control_binding_new (GST_OBJECT (self->volume),
        "volume", GST_CONTROL_SOURCE (self->fader));
    gst_object_add_control_binding (GST_OBJECT (self->volume), binding);

    /* clear any previous fader values */
    gst_timed_value_control_source_unset_all (self->fader);

    /* default values in case we are not doing some operation */
    if (!self->fadein) {
      self->fadein_start_lvl = 1.0;
      self->fadein_end_pos = 0;
    }
    if (!self->fadeout) {
      self->fadeout_end_lvl = 1.0;
      self->fadeout_end_pos = self->fadeout_start_pos = GST_CLOCK_TIME_NONE;
    }

    /* set the initial volume */
    g_object_set (self->volume, "volume", self->fadein_start_lvl, NULL);
    gst_timed_value_control_source_set (self->fader, 0, self->fadein_start_lvl);

    if (self->fadein) {
      gst_timed_value_control_source_set (self->fader,
          self->fadein_end_pos, 1.0);
    }
    if (self->fadeout) {
      gst_timed_value_control_source_set (self->fader,
          self->fadeout_start_pos, 1.0);
    }

    /* set the final volume */
    gst_timed_value_control_source_set (self->fader,
        self->fadeout_end_pos, self->fadeout_end_lvl);
  }

  GST_INFO_OBJECT (self, "Prerolling...");

  ret = gst_element_set_state (GST_ELEMENT (self), GST_STATE_PAUSED);
  switch (ret) {
    case GST_STATE_CHANGE_FAILURE:
      GST_ERROR_OBJECT (self, "Failed to start stream");
      /* cleanup */
      gst_pad_remove_probe (self->src_pad, self->block_probe_id);
      self->block_probe_id = 0;
      gst_element_set_state (GST_ELEMENT (self), GST_STATE_NULL);
      break;
    case GST_STATE_CHANGE_NO_PREROLL:
      GST_INFO_OBJECT (self, "Live stream detected");
      self->live = TRUE;
      break;
    case GST_STATE_CHANGE_SUCCESS:
    case GST_STATE_CHANGE_ASYNC:
      self->live = FALSE;
      break;
    default:
      g_assert_not_reached ();
  }

  return (ret != GST_STATE_CHANGE_FAILURE);
}

void
xfade_stream_play (XFadeStream *self)
{
  /* advance element state */
  gst_element_set_state (GST_ELEMENT (self), GST_STATE_PLAYING);

  /* unblocking pad to actually start playback */
  gst_pad_remove_probe (self->src_pad, self->block_probe_id);
  self->block_probe_id = 0;
}
