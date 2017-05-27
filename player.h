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

#ifndef __PLAYER_H__
#define __PLAYER_H__

#include "scheduler.h"
#include <gst/gst.h>

#define PLAY_QUEUE_SIZE 3

struct player;

struct play_queue_item
{
  struct player *player;

  gchar *uri;
  struct fader fader;

  volatile gint active;
  GstElement *decodebin;
  GstPad *mixer_sink;
};

struct player
{
  struct scheduler *scheduler;

  GMainLoop *loop;
  GstElement *pipeline;
  GstElement *mixer;

  struct play_queue_item play_queue[PLAY_QUEUE_SIZE];
  gint play_queue_ptr;

  /* the running time at which the last item in the play queue is
   * scheduled to start fading out, therefore the time at which
   * the next item needs to start a fade in */
  GstClockTime sched_running_time;
};

int player_init (struct player* self, struct scheduler* scheduler);
void player_cleanup (struct player* self);

void player_loop (struct player* self);
void player_loop_quit (struct player* self);

#endif /* __PLAYER_H__ */
