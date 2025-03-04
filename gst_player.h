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
#include "meta_handler.h"
#include <gst/gst.h>

#define PLAY_QUEUE_SIZE 3

struct player;

struct play_queue_item
{
  struct player *player;

  /* info we got from the scheduler */
  gchar *file;
  struct fader_info fader;
  gchar *zone;

  /* info we discovered; rt = running time */
  guint64 duration;
  GstClockTime start_rt;
  GstClockTime fadeout_rt;
  GstClockTime end_rt;

  /* operational variables */
  GstElement *bin;
  GstPad *mixer_sink;

  struct play_queue_item *previous;
  struct play_queue_item *next;
};

struct player
{
  /* external objects */
  struct scheduler *scheduler;
  struct meta_handler *mh;

  /* internal objects */
  GMainLoop *loop;
  GstElement *pipeline;
  GstElement *mixer;

  struct play_queue_item *playlist;
};

int gst_player_init (struct player* self, struct scheduler* scheduler,
    struct meta_handler *mh, const char *audiosink);
void gst_player_cleanup (struct player* self);

void gst_player_loop (struct player* self);
void gst_player_loop_quit (struct player* self);

#endif /* __PLAYER_H__ */
