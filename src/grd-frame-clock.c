/*
 * Copyright (C) 2025 Pascal Nowack
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include "config.h"

#include "grd-frame-clock.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sys/timerfd.h>

#include "grd-utils.h"

struct _GrdFrameClock
{
  GObject parent;

  GrdFrameClockCallback on_trigger;
  gpointer on_trigger_user_data;

  GSource *timer_source;
  int timer_fd;

  uint64_t start_time_ns;
  uint64_t interval_ns;
  gboolean armed;
};

G_DEFINE_TYPE (GrdFrameClock, grd_frame_clock,
               G_TYPE_OBJECT)

gboolean
grd_frame_clock_is_armed (GrdFrameClock *frame_clock)
{
  return frame_clock->armed;
}

static inline uint64_t
timespec_to_ns (struct timespec *spec)
{
  return s2ns (spec->tv_sec) + spec->tv_nsec;
}

static uint64_t
extrapolate_next_interval_boundary (uint64_t boundary_ns,
                                    uint64_t reference_ns,
                                    uint64_t interval_ns)
{
  uint64_t num_intervals;

  num_intervals = MAX ((reference_ns - boundary_ns + interval_ns - 1) /
                       interval_ns, 0);
  return boundary_ns + num_intervals * interval_ns;
}

static void
schedule_next_frame (GrdFrameClock *frame_clock)
{
  struct itimerspec timer_spec = {};
  struct timespec now = {};
  uint64_t next_dispatch_time_ns;

  clock_gettime (CLOCK_MONOTONIC, &now);

  next_dispatch_time_ns =
    extrapolate_next_interval_boundary (frame_clock->start_time_ns,
                                        timespec_to_ns (&now),
                                        frame_clock->interval_ns);

  timer_spec.it_value.tv_sec = next_dispatch_time_ns / s2ns (1);
  timer_spec.it_value.tv_nsec = next_dispatch_time_ns % s2ns (1);

  timerfd_settime (frame_clock->timer_fd, TFD_TIMER_ABSTIME, &timer_spec, NULL);
}

void
grd_frame_clock_arm_timer (GrdFrameClock *frame_clock,
                           uint64_t       clock_rate_num,
                           uint64_t       clock_rate_denom)
{
  struct timespec start_time = {};

  clock_gettime (CLOCK_MONOTONIC, &start_time);
  frame_clock->start_time_ns = timespec_to_ns (&start_time);
  frame_clock->interval_ns = s2ns (clock_rate_denom) / clock_rate_num;

  schedule_next_frame (frame_clock);
  frame_clock->armed = TRUE;
}

void
grd_frame_clock_disarm_timer (GrdFrameClock *frame_clock)
{
  struct itimerspec timer_spec = {};

  timerfd_settime (frame_clock->timer_fd, 0, &timer_spec, NULL);
  frame_clock->armed = FALSE;
}

static gboolean
timer_source_prepare (GSource *source,
                      int     *timeout)
{
  *timeout = -1;

  return FALSE;
}

static gboolean
timer_source_dispatch (GSource     *source,
                       GSourceFunc  callback,
                       gpointer     user_data)
{
  g_source_set_ready_time (source, -1);

  return callback (user_data);
}

static GSourceFuncs timer_source_funcs =
{
  .prepare = timer_source_prepare,
  .dispatch = timer_source_dispatch,
};

static gboolean
handle_triggered_timer (gpointer user_data)
{
  GrdFrameClock *frame_clock = user_data;
  uint64_t expirations;
  ssize_t ret;

  ret = read (frame_clock->timer_fd, &expirations, sizeof (uint64_t));
  if (ret == -1)
    {
      g_warning ("Failed to read from timerfd: %s", g_strerror (errno));
      return G_SOURCE_CONTINUE;
    }
  else if (ret != sizeof (expirations))
    {
      g_warning ("Failed to read from timerfd: unexpected size %zi", ret);
      return G_SOURCE_CONTINUE;
    }

  frame_clock->on_trigger (frame_clock->on_trigger_user_data);

  g_assert (frame_clock->armed);
  schedule_next_frame (frame_clock);

  return G_SOURCE_CONTINUE;
}

GrdFrameClock *
grd_frame_clock_new (GMainContext           *main_context,
                     GrdFrameClockCallback   on_trigger,
                     gpointer                on_trigger_user_data,
                     GError                **error)
{
  g_autoptr (GrdFrameClock) frame_clock = NULL;

  frame_clock = g_object_new (GRD_TYPE_FRAME_CLOCK, NULL);
  frame_clock->on_trigger = on_trigger;
  frame_clock->on_trigger_user_data = on_trigger_user_data;

  frame_clock->timer_fd = timerfd_create (CLOCK_MONOTONIC,
                                          TFD_NONBLOCK | TFD_CLOEXEC);
  if (frame_clock->timer_fd == -1)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create timerfd: %s", g_strerror (errno));
      return NULL;
    }

  frame_clock->timer_source = g_source_new (&timer_source_funcs,
                                            sizeof (GSource));
  g_source_set_callback (frame_clock->timer_source, handle_triggered_timer,
                         frame_clock, NULL);
  g_source_set_ready_time (frame_clock->timer_source, -1);
  g_source_add_unix_fd (frame_clock->timer_source,
                        frame_clock->timer_fd, G_IO_IN);
  g_source_attach (frame_clock->timer_source, main_context);

  return g_steal_pointer (&frame_clock);
}

static void
grd_frame_clock_dispose (GObject *object)
{
  GrdFrameClock *frame_clock = GRD_FRAME_CLOCK (object);

  if (frame_clock->timer_source)
    {
      g_source_destroy (frame_clock->timer_source);
      g_clear_pointer (&frame_clock->timer_source, g_source_unref);
    }

  g_clear_fd (&frame_clock->timer_fd, NULL);

  G_OBJECT_CLASS (grd_frame_clock_parent_class)->dispose (object);
}

static void
grd_frame_clock_init (GrdFrameClock *frame_clock)
{
  frame_clock->timer_fd = -1;
}

static void
grd_frame_clock_class_init (GrdFrameClockClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_frame_clock_dispose;
}
