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

#ifndef __XFADE_STREAM_H__
#define __XFADE_STREAM_H__

#include <gst/gst.h>
#include <gst/controller/controller.h>

#define XFADE_TYPE_STREAM (xfade_stream_get_type ())
G_DECLARE_FINAL_TYPE (XFadeStream, xfade_stream, XFADE, STREAM, GstBin)

struct _XFadeStream
{
  GstBin parent_instance;

  /* internal components */
  GstElement *uridecodebin;
  GstElement *audioconvert;
  GstElement *audioresample;
  GstElement *capsfilter;
  GstElement *preroll_queue;
  GstElement *volume;
  GstTimedValueControlSource *fader;
  GstPad *src_pad;

  /* internal variables */
  gulong block_probe_id;

  /* r/w properties */
  gboolean fadein;
  gboolean fadeout;
  gdouble fadein_start_lvl;
  gdouble fadeout_end_lvl;
  GstClockTime fadein_end_pos;
  GstClockTime fadeout_start_pos;
  GstClockTime fadeout_end_pos;

  /* read-only properties */
  gboolean live;
};

gboolean xfade_stream_preroll (XFadeStream *self);
void xfade_stream_play (XFadeStream *self);

#endif /* __XFADE_STREAM_H__ */
