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

#ifndef __XFADE_PLAYER_H__
#define __XFADE_PLAYER_H__

#include <gst/gst.h>
#include "xfade_stream.h"

#define XFADE_TYPE_PLAYER (xfade_player_get_type ())
G_DECLARE_FINAL_TYPE (XFadePlayer, xfade_player, XFADE, PLAYER, GstPipeline)

struct _XFadePlayer
{
  GstPipeline parent_instance;

  GstElement *adder;
  GstElement *audiosink;

  GThread *bus_thread;

  GMutex mutex;
  XFadeStream *cur_stream;
  XFadeStream *next_stream;
};

XFadePlayer *xfade_player_new (void);

#endif /* __XFADE_PLAYER_H__ */
