/* GDK - The GIMP Drawing Kit
 * Copyright (C) 2009 Carlos Garnacho <carlosg@gnome.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <string.h>
#include <gdk/gdkwindow.h>
#include <gdk/gdktypes.h>
#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"
#include "gdkwayland.h"
#include "gdkkeysyms.h"
#include "gdkdeviceprivate.h"
#include "gdkdevicemanagerprivate.h"
#include "gdkseatprivate.h"
#include "pointer-gestures-unstable-v1-client-protocol.h"
#include "tablet-unstable-v1-client-protocol.h"

#include <xkbcommon/xkbcommon.h>

#include <linux/input.h>

#include <sys/time.h>
#include <sys/mman.h>

#define BUTTON_BASE (BTN_LEFT - 1) /* Used to translate to 1-indexed buttons */

typedef struct _GdkWaylandTouchData GdkWaylandTouchData;
typedef struct _GdkWaylandPointerFrameData GdkWaylandPointerFrameData;
typedef struct _GdkWaylandPointerData GdkWaylandPointerData;
typedef struct _GdkWaylandTabletData GdkWaylandTabletData;
typedef struct _GdkWaylandTabletToolData GdkWaylandTabletToolData;

struct _GdkWaylandTouchData
{
  uint32_t id;
  gdouble x;
  gdouble y;
  GdkWindow *window;
  uint32_t touch_down_serial;
  guint initial_touch : 1;
};

struct _GdkWaylandPointerFrameData
{
  GdkEvent *event;

  /* Specific to the scroll event */
  gdouble delta_x, delta_y;
  int32_t discrete_x, discrete_y;
  gint8 is_scroll_stop;
};

struct _GdkWaylandPointerData {
  GdkWindow *focus;

  double surface_x, surface_y;

  GdkModifierType button_modifiers;

  uint32_t time;
  uint32_t enter_serial;
  uint32_t press_serial;

  GdkWindow *grab_window;
  uint32_t grab_time;

  struct wl_surface *pointer_surface;
  GdkCursor *cursor;
  guint cursor_timeout_id;
  guint cursor_image_index;
  guint cursor_image_delay;

  guint current_output_scale;
  GSList *pointer_surface_outputs;

  /* Accumulated event data for a pointer frame */
  GdkWaylandPointerFrameData frame;
};

struct _GdkWaylandTabletToolData
{
  GdkSeat *seat;
  struct zwp_tablet_tool_v1 *wp_tablet_tool;
  GdkAxisFlags axes;
  GdkDeviceToolType type;
  guint64 hwserial;
  guint64 hwid;

  GdkDeviceTool *tool;
  GdkWaylandTabletData *current_tablet;
};

struct _GdkWaylandTabletData
{
  struct zwp_tablet_v1 *wp_tablet;
  gchar *name;
  gchar *path;
  uint32_t vid;
  uint32_t pid;

  GdkDevice *master;
  GdkDevice *stylus_device;
  GdkDevice *eraser_device;
  GdkDevice *current_device;
  GdkSeat *seat;
  GdkWaylandPointerData pointer_info;

  GdkWaylandTabletToolData *current_tool;
};

struct _GdkWaylandSeat
{
  GdkSeat parent_instance;

  guint32 id;
  struct wl_seat *wl_seat;
  struct wl_pointer *wl_pointer;
  struct wl_keyboard *wl_keyboard;
  struct wl_touch *wl_touch;
  struct zwp_pointer_gesture_swipe_v1 *wp_pointer_gesture_swipe;
  struct zwp_pointer_gesture_pinch_v1 *wp_pointer_gesture_pinch;
  struct zwp_tablet_seat_v1 *wp_tablet_seat;

  GdkDisplay *display;
  GdkDeviceManager *device_manager;

  GdkDevice *master_pointer;
  GdkDevice *master_keyboard;
  GdkDevice *pointer;
  GdkDevice *keyboard;
  GdkDevice *touch_master;
  GdkDevice *touch;
  GdkCursor *cursor;
  GdkKeymap *keymap;

  GHashTable *touches;
  GList *tablets;
  GList *tablet_tools;

  GdkWaylandPointerData pointer_info;
  GdkWaylandPointerData touch_info;

  GdkModifierType key_modifiers;
  GdkWindow *keyboard_focus;
  GdkAtom pending_selection;
  GdkWindow *grab_window;
  uint32_t grab_time;
  gboolean have_server_repeat;
  uint32_t server_repeat_rate;
  uint32_t server_repeat_delay;
  guint32 repeat_timer;
  guint32 repeat_key;
  guint32 repeat_count;
  GSettings *keyboard_settings;

  struct wl_data_device *data_device;
  GdkDragContext *drop_context;

  /* Source/dest for non-local dnd */
  GdkWindow *foreign_dnd_window;

  /* Some tracking on gesture events */
  guint gesture_n_fingers;
  gdouble gesture_scale;

  GdkCursor *grab_cursor;
};

G_DEFINE_TYPE (GdkWaylandSeat, gdk_wayland_seat, GDK_TYPE_SEAT)

struct _GdkWaylandDevice
{
  GdkDevice parent_instance;
  GdkWaylandDeviceData *device;
  GdkWaylandTouchData *emulating_touch; /* Only used on wd->touch_master */
  GdkWaylandPointerData *pointer;
};

struct _GdkWaylandDeviceClass
{
  GdkDeviceClass parent_class;
};

G_DEFINE_TYPE (GdkWaylandDevice, gdk_wayland_device, GDK_TYPE_DEVICE)

#define GDK_TYPE_WAYLAND_DEVICE_MANAGER        (gdk_wayland_device_manager_get_type ())
#define GDK_WAYLAND_DEVICE_MANAGER(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), GDK_TYPE_WAYLAND_DEVICE_MANAGER, GdkWaylandDeviceManager))
#define GDK_WAYLAND_DEVICE_MANAGER_CLASS(c)     (G_TYPE_CHECK_CLASS_CAST ((c), GDK_TYPE_WAYLAND_DEVICE_MANAGER, GdkWaylandDeviceManagerClass))
#define GDK_IS_WAYLAND_DEVICE_MANAGER(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), GDK_TYPE_WAYLAND_DEVICE_MANAGER))
#define GDK_IS_WAYLAND_DEVICE_MANAGER_CLASS(c)  (G_TYPE_CHECK_CLASS_TYPE ((c), GDK_TYPE_WAYLAND_DEVICE_MANAGER))
#define GDK_WAYLAND_DEVICE_MANAGER_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), GDK_TYPE_WAYLAND_DEVICE_MANAGER, GdkWaylandDeviceManagerClass))

#define GDK_SLOT_TO_EVENT_SEQUENCE(s) ((GdkEventSequence *) GUINT_TO_POINTER((s) + 1))
#define GDK_EVENT_SEQUENCE_TO_SLOT(s) (GPOINTER_TO_UINT(s) - 1)

typedef struct _GdkWaylandDeviceManager GdkWaylandDeviceManager;
typedef struct _GdkWaylandDeviceManagerClass GdkWaylandDeviceManagerClass;

struct _GdkWaylandDeviceManager
{
  GdkDeviceManager parent_object;
  GList *devices;
};

struct _GdkWaylandDeviceManagerClass
{
  GdkDeviceManagerClass parent_class;
};

GType gdk_wayland_device_manager_get_type (void);

G_DEFINE_TYPE (GdkWaylandDeviceManager,
	       gdk_wayland_device_manager, GDK_TYPE_DEVICE_MANAGER)

static gboolean
gdk_wayland_device_get_history (GdkDevice      *device,
                                GdkWindow      *window,
                                guint32         start,
                                guint32         stop,
                                GdkTimeCoord ***events,
                                gint           *n_events)
{
  return FALSE;
}

static void
gdk_wayland_device_get_state (GdkDevice       *device,
                              GdkWindow       *window,
                              gdouble         *axes,
                              GdkModifierType *mask)
{
  gdouble x, y;

  gdk_window_get_device_position_double (window, device, &x, &y, mask);

  if (axes)
    {
      axes[0] = x;
      axes[1] = y;
    }
}

static void
gdk_wayland_pointer_stop_cursor_animation (GdkWaylandPointerData *pointer)
{
  if (pointer->cursor_timeout_id > 0)
    {
      g_source_remove (pointer->cursor_timeout_id);
      pointer->cursor_timeout_id = 0;
    }

  pointer->cursor_image_index = 0;
}

static GdkWaylandTabletData *
gdk_wayland_device_manager_find_tablet (GdkWaylandSeat *seat,
                                        GdkDevice      *device)
{
  GList *l;

  for (l = seat->tablets; l; l = l->next)
    {
      GdkWaylandTabletData *tablet = l->data;

      if (tablet->master == device ||
          tablet->stylus_device == device ||
          tablet->eraser_device == device)
        return tablet;
    }

  return NULL;
}

static gboolean
gdk_wayland_device_update_window_cursor (GdkDevice *device)
{
  GdkWaylandSeat *seat = GDK_WAYLAND_SEAT (gdk_device_get_seat (device));
  GdkWaylandPointerData *pointer = GDK_WAYLAND_DEVICE (device)->pointer;
  struct wl_buffer *buffer;
  int x, y, w, h, scale;
  guint next_image_index, next_image_delay;
  gboolean retval = G_SOURCE_REMOVE;
  GdkWaylandTabletData *tablet;

  tablet = gdk_wayland_device_manager_find_tablet (seat, device);

  if (pointer->cursor)
    {
      buffer = _gdk_wayland_cursor_get_buffer (pointer->cursor,
                                               pointer->cursor_image_index,
                                               &x, &y, &w, &h, &scale);
    }
  else
    {
      pointer->cursor_timeout_id = 0;
      return TRUE;
    }

  if (tablet)
    {
      if (!tablet->current_tool)
        return retval;

      zwp_tablet_tool_v1_set_cursor (tablet->current_tool->wp_tablet_tool,
                                     pointer->enter_serial,
                                     pointer->pointer_surface,
                                     x, y);
    }
  else if (seat->wl_pointer)
    {
      wl_pointer_set_cursor (seat->wl_pointer,
                             pointer->enter_serial,
                             pointer->pointer_surface,
                             x, y);
    }
  else
    return retval;

  if (buffer)
    {
      wl_surface_attach (pointer->pointer_surface, buffer, 0, 0);
      wl_surface_set_buffer_scale (pointer->pointer_surface, scale);
      wl_surface_damage (pointer->pointer_surface,  0, 0, w, h);
      wl_surface_commit (pointer->pointer_surface);
    }
  else
    {
      wl_surface_attach (pointer->pointer_surface, NULL, 0, 0);
      wl_surface_commit (pointer->pointer_surface);
    }

  next_image_index =
    _gdk_wayland_cursor_get_next_image_index (pointer->cursor,
                                              pointer->cursor_image_index,
                                              &next_image_delay);

  if (next_image_index != pointer->cursor_image_index)
    {
      if (next_image_delay != pointer->cursor_image_delay)
        {
          guint id;

          gdk_wayland_pointer_stop_cursor_animation (pointer);

          /* Queue timeout for next frame */
          id = g_timeout_add (next_image_delay,
                              (GSourceFunc)gdk_wayland_device_update_window_cursor,
                              device);
          g_source_set_name_by_id (id, "[gtk+] gdk_wayland_device_update_window_cursor");
          pointer->cursor_timeout_id = id;
        }
      else
        retval = G_SOURCE_CONTINUE;

      pointer->cursor_image_index = next_image_index;
      pointer->cursor_image_delay = next_image_delay;
    }
  else
    gdk_wayland_pointer_stop_cursor_animation (pointer);

  return retval;
}

static void
gdk_wayland_device_set_window_cursor (GdkDevice *device,
                                      GdkWindow *window,
                                      GdkCursor *cursor)
{
  GdkWaylandDeviceData *wd = GDK_WAYLAND_DEVICE (device)->device;
  GdkWaylandPointerData *pointer = GDK_WAYLAND_DEVICE (device)->pointer;

  if (device == wd->touch_master)
    return;

  if (wd->grab_cursor)
    cursor = wd->grab_cursor;

  /* Setting the cursor to NULL means that we should use
   * the default cursor
   */
  if (!cursor)
    {
      guint scale = pointer->current_output_scale;
      cursor =
        _gdk_wayland_display_get_cursor_for_type_with_scale (wd->display,
                                                             GDK_LEFT_PTR,
                                                             scale);
    }
  else
    _gdk_wayland_cursor_set_scale (cursor, pointer->current_output_scale);

  if (cursor == pointer->cursor)
    return;

  gdk_wayland_pointer_stop_cursor_animation (pointer);

  if (pointer->cursor)
    g_object_unref (pointer->cursor);

  pointer->cursor = g_object_ref (cursor);

  gdk_wayland_device_update_window_cursor (device);
}

static void
gdk_wayland_device_warp (GdkDevice *device,
                         GdkScreen *screen,
                         gdouble    x,
                         gdouble    y)
{
}

static void
get_coordinates (GdkDevice *device,
                 double    *x,
                 double    *y,
                 double    *x_root,
                 double    *y_root)
{
  GdkWaylandPointerData *pointer = GDK_WAYLAND_DEVICE (device)->pointer;
  int root_x, root_y;

  if (x)
    *x = pointer->surface_x;
  if (y)
    *y = pointer->surface_y;

  if (pointer->focus)
    {
      gdk_window_get_root_coords (pointer->focus,
                                  pointer->surface_x,
                                  pointer->surface_y,
                                  &root_x, &root_y);
    }
  else
    {
      root_x = pointer->surface_x;
      root_y = pointer->surface_y;
    }

  if (x_root)
    *x_root = root_x;
  if (y_root)
    *y_root = root_y;
}

static GdkModifierType
device_get_modifiers (GdkDevice *device)
{
  GdkWaylandSeat *seat = GDK_WAYLAND_SEAT (gdk_device_get_seat (device));
  GdkWaylandPointerData *pointer = GDK_WAYLAND_DEVICE (device)->pointer;
  GdkModifierType mask;

  mask = seat->key_modifiers;

  if (pointer)
    mask |= pointer->button_modifiers;

  return mask;
}

static void
gdk_wayland_device_query_state (GdkDevice        *device,
                                GdkWindow        *window,
                                GdkWindow       **root_window,
                                GdkWindow       **child_window,
                                gdouble          *root_x,
                                gdouble          *root_y,
                                gdouble          *win_x,
                                gdouble          *win_y,
                                GdkModifierType  *mask)
{
  GdkWaylandDeviceData *wd;
  GdkWaylandPointerData *pointer;
  GdkScreen *default_screen;

  wd = GDK_WAYLAND_DEVICE (device)->device;
  pointer = GDK_WAYLAND_DEVICE (device)->pointer;
  default_screen = gdk_display_get_default_screen (wd->display);

  if (root_window)
    *root_window = gdk_screen_get_root_window (default_screen);
  if (child_window)
    *child_window = pointer->focus;
  if (mask)
    *mask = device_get_modifiers (device);

  get_coordinates (device, win_x, win_y, root_x, root_y);
}

static void
emulate_crossing (GdkWindow       *window,
                  GdkWindow       *subwindow,
                  GdkDevice       *device,
                  GdkEventType     type,
                  GdkCrossingMode  mode,
                  guint32          time_)
{
  GdkEvent *event;

  event = gdk_event_new (type);
  event->crossing.window = window ? g_object_ref (window) : NULL;
  event->crossing.subwindow = subwindow ? g_object_ref (subwindow) : NULL;
  event->crossing.time = time_;
  event->crossing.mode = mode;
  event->crossing.detail = GDK_NOTIFY_NONLINEAR;
  gdk_event_set_device (event, device);
  gdk_event_set_source_device (event, device);
  gdk_event_set_seat (event, gdk_device_get_seat (device));

  gdk_window_get_device_position_double (window, device,
                                         &event->crossing.x, &event->crossing.y,
                                         &event->crossing.state);
  event->crossing.x_root = event->crossing.x;
  event->crossing.y_root = event->crossing.y;

  _gdk_wayland_display_deliver_event (gdk_window_get_display (window), event);
}

static void
emulate_touch_crossing (GdkWindow           *window,
                        GdkWindow           *subwindow,
                        GdkDevice           *device,
                        GdkDevice           *source,
                        GdkWaylandTouchData *touch,
                        GdkEventType         type,
                        GdkCrossingMode      mode,
                        guint32              time_)
{
  GdkEvent *event;

  event = gdk_event_new (type);
  event->crossing.window = window ? g_object_ref (window) : NULL;
  event->crossing.subwindow = subwindow ? g_object_ref (subwindow) : NULL;
  event->crossing.time = time_;
  event->crossing.mode = mode;
  event->crossing.detail = GDK_NOTIFY_NONLINEAR;
  gdk_event_set_device (event, device);
  gdk_event_set_source_device (event, source);
  gdk_event_set_seat (event, gdk_device_get_seat (device));

  event->crossing.x = touch->x;
  event->crossing.y = touch->y;
  event->crossing.x_root = event->crossing.x;
  event->crossing.y_root = event->crossing.y;

  _gdk_wayland_display_deliver_event (gdk_window_get_display (window), event);
}

static void
emulate_focus (GdkWindow *window,
               GdkDevice *device,
               gboolean   focus_in,
               guint32    time_)
{
  GdkEvent *event;

  event = gdk_event_new (GDK_FOCUS_CHANGE);
  event->focus_change.window = g_object_ref (window);
  event->focus_change.in = focus_in;
  gdk_event_set_device (event, device);
  gdk_event_set_source_device (event, device);
  gdk_event_set_seat (event, gdk_device_get_seat (device));

  _gdk_wayland_display_deliver_event (gdk_window_get_display (window), event);
}

static void
device_emit_grab_crossing (GdkDevice       *device,
                           GdkWindow       *from,
                           GdkWindow       *to,
                           GdkCrossingMode  mode,
                           guint32          time_)
{
  if (gdk_device_get_source (device) == GDK_SOURCE_KEYBOARD)
    {
      if (from)
        emulate_focus (from, device, FALSE, time_);
      if (to)
        emulate_focus (to, device, TRUE, time_);
    }
  else
    {
      if (from)
        emulate_crossing (from, to, device, GDK_LEAVE_NOTIFY, mode, time_);
      if (to)
        emulate_crossing (to, from, device, GDK_ENTER_NOTIFY, mode, time_);
    }
}

static GdkWindow *
gdk_wayland_device_get_focus (GdkDevice *device)
{
  GdkWaylandSeat *wayland_seat = GDK_WAYLAND_DEVICE (device)->device;
  GdkWaylandPointerData *pointer;

  if (device == wayland_seat->master_keyboard)
    return wayland_seat->keyboard_focus;
  else
    {
      pointer = GDK_WAYLAND_DEVICE (device)->pointer;

      if (pointer)
        return pointer->focus;
    }

  return NULL;
}

static GdkGrabStatus
gdk_wayland_device_grab (GdkDevice    *device,
                         GdkWindow    *window,
                         gboolean      owner_events,
                         GdkEventMask  event_mask,
                         GdkWindow    *confine_to,
                         GdkCursor    *cursor,
                         guint32       time_)
{
  GdkWaylandDeviceData *wayland_device = GDK_WAYLAND_DEVICE (device)->device;
  GdkWindow *prev_focus = gdk_wayland_device_get_focus (device);
  GdkWaylandPointerData *pointer = GDK_WAYLAND_DEVICE (device)->pointer;

  if (prev_focus != window)
    device_emit_grab_crossing (device, prev_focus, window, GDK_CROSSING_GRAB, time_);

  if (gdk_device_get_source (device) == GDK_SOURCE_KEYBOARD)
    {
      /* Device is a keyboard */
      return GDK_GRAB_SUCCESS;
    }
  else
    {
      /* Device is a pointer */
      if (pointer->grab_window != NULL &&
          time_ != 0 && pointer->grab_time > time_)
        {
          return GDK_GRAB_ALREADY_GRABBED;
        }

      if (time_ == 0)
        time_ = pointer->time;

      pointer->grab_window = window;
      pointer->grab_time = time_;
      _gdk_wayland_window_set_grab_seat (window, GDK_SEAT (wayland_device));

      g_clear_object (&wayland_device->cursor);

      if (cursor)
        wayland_device->cursor = g_object_ref (cursor);

      gdk_wayland_device_update_window_cursor (device);
    }

  return GDK_GRAB_SUCCESS;
}

static void
gdk_wayland_device_ungrab (GdkDevice *device,
                           guint32    time_)
{
  GdkWaylandPointerData *pointer = GDK_WAYLAND_DEVICE (device)->pointer;
  GdkDisplay *display;
  GdkDeviceGrabInfo *grab;
  GdkWindow *focus, *prev_focus = NULL;

  display = gdk_device_get_display (device);

  grab = _gdk_display_get_last_device_grab (display, device);

  if (grab)
    {
      grab->serial_end = grab->serial_start;
      prev_focus = grab->window;
    }

  focus = gdk_wayland_device_get_focus (device);

  if (focus != prev_focus)
    device_emit_grab_crossing (device, prev_focus, focus, GDK_CROSSING_UNGRAB, time_);

  if (gdk_device_get_source (device) == GDK_SOURCE_KEYBOARD)
    {
      /* Device is a keyboard */
    }
  else
    {
      /* Device is a pointer */
      gdk_wayland_device_update_window_cursor (device);

      if (pointer->grab_window)
        _gdk_wayland_window_set_grab_seat (pointer->grab_window,
                                           NULL);
    }
}

static GdkWindow *
gdk_wayland_device_window_at_position (GdkDevice       *device,
                                       gdouble         *win_x,
                                       gdouble         *win_y,
                                       GdkModifierType *mask,
                                       gboolean         get_toplevel)
{
  GdkWaylandPointerData *pointer;

  pointer = GDK_WAYLAND_DEVICE(device)->pointer;

  if (!pointer)
    return NULL;

  if (win_x)
    *win_x = pointer->surface_x;
  if (win_y)
    *win_y = pointer->surface_y;
  if (mask)
    *mask = device_get_modifiers (device);

  return pointer->focus;
}

static void
gdk_wayland_device_select_window_events (GdkDevice    *device,
                                         GdkWindow    *window,
                                         GdkEventMask  event_mask)
{
}

static void
gdk_wayland_device_class_init (GdkWaylandDeviceClass *klass)
{
  GdkDeviceClass *device_class = GDK_DEVICE_CLASS (klass);

  device_class->get_history = gdk_wayland_device_get_history;
  device_class->get_state = gdk_wayland_device_get_state;
  device_class->set_window_cursor = gdk_wayland_device_set_window_cursor;
  device_class->warp = gdk_wayland_device_warp;
  device_class->query_state = gdk_wayland_device_query_state;
  device_class->grab = gdk_wayland_device_grab;
  device_class->ungrab = gdk_wayland_device_ungrab;
  device_class->window_at_position = gdk_wayland_device_window_at_position;
  device_class->select_window_events = gdk_wayland_device_select_window_events;
}

static void
gdk_wayland_device_init (GdkWaylandDevice *device_core)
{
  GdkDevice *device;

  device = GDK_DEVICE (device_core);

  _gdk_device_add_axis (device, GDK_NONE, GDK_AXIS_X, 0, 0, 1);
  _gdk_device_add_axis (device, GDK_NONE, GDK_AXIS_Y, 0, 0, 1);
}

/**
 * gdk_wayland_device_get_wl_seat:
 * @device: (type GdkWaylandDevice): a #GdkDevice
 *
 * Returns the Wayland wl_seat of a #GdkDevice.
 *
 * Returns: (transfer none): a Wayland wl_seat
 *
 * Since: 3.10
 */
struct wl_seat *
gdk_wayland_device_get_wl_seat (GdkDevice *device)
{
  g_return_val_if_fail (GDK_IS_WAYLAND_DEVICE (device), NULL);

  return GDK_WAYLAND_DEVICE (device)->device->wl_seat;
}

/**
 * gdk_wayland_device_get_wl_pointer:
 * @device: (type GdkWaylandDevice): a #GdkDevice
 *
 * Returns the Wayland wl_pointer of a #GdkDevice.
 *
 * Returns: (transfer none): a Wayland wl_pointer
 *
 * Since: 3.10
 */
struct wl_pointer *
gdk_wayland_device_get_wl_pointer (GdkDevice *device)
{
  g_return_val_if_fail (GDK_IS_WAYLAND_DEVICE (device), NULL);

  return GDK_WAYLAND_DEVICE (device)->device->wl_pointer;
}

/**
 * gdk_wayland_device_get_wl_keyboard:
 * @device: (type GdkWaylandDevice): a #GdkDevice
 *
 * Returns the Wayland wl_keyboard of a #GdkDevice.
 *
 * Returns: (transfer none): a Wayland wl_keyboard
 *
 * Since: 3.10
 */
struct wl_keyboard *
gdk_wayland_device_get_wl_keyboard (GdkDevice *device)
{
  g_return_val_if_fail (GDK_IS_WAYLAND_DEVICE (device), NULL);

  return GDK_WAYLAND_DEVICE (device)->device->wl_keyboard;
}

GdkKeymap *
_gdk_wayland_device_get_keymap (GdkDevice *device)
{
  return GDK_WAYLAND_DEVICE (device)->device->keymap;
}

static void
emit_selection_owner_change (GdkWindow *window,
                             GdkAtom    atom)
{
  GdkEvent *event;

  event = gdk_event_new (GDK_OWNER_CHANGE);
  event->owner_change.window = g_object_ref (window);
  event->owner_change.owner = NULL;
  event->owner_change.reason = GDK_OWNER_CHANGE_NEW_OWNER;
  event->owner_change.selection = atom;
  event->owner_change.time = GDK_CURRENT_TIME;
  event->owner_change.selection_time = GDK_CURRENT_TIME;

  gdk_event_put (event);
  gdk_event_free (event);
}

static void
data_device_data_offer (void                  *data,
                        struct wl_data_device *data_device,
                        struct wl_data_offer  *offer)
{
  GdkWaylandDeviceData *device = (GdkWaylandDeviceData *)data;

  GDK_NOTE (EVENTS,
            g_message ("data device data offer, data device %p, offer %p",
                       data_device, offer));

  gdk_wayland_selection_ensure_offer (device->display, offer);
}

static void
data_device_enter (void                  *data,
                   struct wl_data_device *data_device,
                   uint32_t               serial,
                   struct wl_surface     *surface,
                   wl_fixed_t             x,
                   wl_fixed_t             y,
                   struct wl_data_offer  *offer)
{
  GdkWaylandDeviceData *device = (GdkWaylandDeviceData *)data;
  GdkWindow *dest_window, *dnd_owner;
  GdkAtom selection;

  dest_window = wl_surface_get_user_data (surface);

  if (!GDK_IS_WINDOW (dest_window))
    return;

  GDK_NOTE (EVENTS,
            g_message ("data device enter, data device %p serial %u, surface %p, x %f y %f, offer %p",
                       data_device, serial, surface, wl_fixed_to_double (x), wl_fixed_to_double (y), offer));

  /* Update pointer state, so device state queries work during DnD */
  device->pointer_info.focus = g_object_ref (dest_window);
  device->pointer_info.surface_x = wl_fixed_to_double (x);
  device->pointer_info.surface_y = wl_fixed_to_double (y);

  gdk_wayland_drop_context_update_targets (device->drop_context);

  selection = gdk_drag_get_selection (device->drop_context);
  dnd_owner = gdk_selection_owner_get_for_display (device->display, selection);

  if (!dnd_owner)
    dnd_owner = device->foreign_dnd_window;

  _gdk_wayland_drag_context_set_source_window (device->drop_context, dnd_owner);

  _gdk_wayland_drag_context_set_dest_window (device->drop_context,
                                             dest_window, serial);
  _gdk_wayland_drag_context_set_coords (device->drop_context,
                                        wl_fixed_to_double (x),
                                        wl_fixed_to_double (y));
  _gdk_wayland_drag_context_emit_event (device->drop_context, GDK_DRAG_ENTER,
                                        GDK_CURRENT_TIME);

  gdk_wayland_selection_set_offer (device->display, selection, offer);

  emit_selection_owner_change (dest_window, selection);
}

static void
data_device_leave (void                  *data,
                   struct wl_data_device *data_device)
{
  GdkWaylandDeviceData *device = (GdkWaylandDeviceData *) data;

  GDK_NOTE (EVENTS,
            g_message ("data device leave, data device %p", data_device));

  if (!gdk_drag_context_get_dest_window (device->drop_context))
    return;

  g_object_unref (device->pointer_info.focus);
  device->pointer_info.focus = NULL;

  _gdk_wayland_drag_context_set_coords (device->drop_context, -1, -1);
  _gdk_wayland_drag_context_emit_event (device->drop_context, GDK_DRAG_LEAVE,
                                        GDK_CURRENT_TIME);
  _gdk_wayland_drag_context_set_dest_window (device->drop_context, NULL, 0);
}

static void
data_device_motion (void                  *data,
                    struct wl_data_device *data_device,
                    uint32_t               time,
                    wl_fixed_t             x,
                    wl_fixed_t             y)
{
  GdkWaylandDeviceData *device = (GdkWaylandDeviceData *) data;

  g_debug (G_STRLOC ": %s data_device = %p, time = %d, x = %f, y = %f",
           G_STRFUNC, data_device, time, wl_fixed_to_double (x), wl_fixed_to_double (y));

  if (!gdk_drag_context_get_dest_window (device->drop_context))
    return;

  /* Update pointer state, so device state queries work during DnD */
  device->pointer_info.surface_x = wl_fixed_to_double (x);
  device->pointer_info.surface_y = wl_fixed_to_double (y);

  gdk_wayland_drop_context_update_targets (device->drop_context);
  _gdk_wayland_drag_context_set_coords (device->drop_context,
                                        wl_fixed_to_double (x),
                                        wl_fixed_to_double (y));
  _gdk_wayland_drag_context_emit_event (device->drop_context,
                                        GDK_DRAG_MOTION, time);
}

static void
data_device_drop (void                  *data,
                  struct wl_data_device *data_device)
{
  GdkWaylandDeviceData *device = (GdkWaylandDeviceData *) data;

  GDK_NOTE (EVENTS,
            g_message ("data device drop, data device %p", data_device));

  _gdk_wayland_drag_context_emit_event (device->drop_context,
                                        GDK_DROP_START, GDK_CURRENT_TIME);
}

static void
data_device_selection (void                  *data,
                       struct wl_data_device *wl_data_device,
                       struct wl_data_offer  *offer)
{
  GdkWaylandDeviceData *device = (GdkWaylandDeviceData *) data;
  GdkAtom selection;

  GDK_NOTE (EVENTS,
            g_message ("data device selection, data device %p, data offer %p",
                       wl_data_device, offer));

  selection = gdk_atom_intern_static_string ("CLIPBOARD");
  gdk_wayland_selection_set_offer (device->display, selection, offer);

  /* If we already have keyboard focus, the selection was targeted at the
   * focused surface. If we don't we will receive keyboard focus directly after
   * this, so lets wait and find out what window will get the focus before
   * emitting the owner-changed event.
   */
  if (device->keyboard_focus)
    emit_selection_owner_change (device->keyboard_focus, selection);
  else
    device->pending_selection = selection;
}

static const struct wl_data_device_listener data_device_listener = {
  data_device_data_offer,
  data_device_enter,
  data_device_leave,
  data_device_motion,
  data_device_drop,
  data_device_selection
};

static GdkEvent *
create_scroll_event (GdkWaylandSeat *seat,
                     gboolean        emulated)
{
  GdkWaylandDisplay *display = GDK_WAYLAND_DISPLAY (seat->display);
  GdkEvent *event;

  event = gdk_event_new (GDK_SCROLL);
  event->scroll.window = g_object_ref (seat->pointer_info.focus);
  gdk_event_set_device (event, seat->master_pointer);
  gdk_event_set_source_device (event, seat->pointer);
  event->scroll.time = seat->pointer_info.time;
  event->scroll.state = device_get_modifiers (seat->master_pointer);
  gdk_event_set_screen (event, display->screen);

  _gdk_event_set_pointer_emulated (event, emulated);

  get_coordinates (seat->master_pointer,
                   &event->scroll.x,
                   &event->scroll.y,
                   &event->scroll.x_root,
                   &event->scroll.y_root);

  return event;
}

static void
flush_discrete_scroll_event (GdkWaylandSeat     *seat,
                             GdkScrollDirection  direction)
{
  GdkEvent *event;

  event = create_scroll_event (seat, TRUE);
  event->scroll.direction = direction;

  _gdk_wayland_display_deliver_event (seat->display, event);
}

static void
flush_smooth_scroll_event (GdkWaylandSeat *seat,
                           gdouble         delta_x,
                           gdouble         delta_y,
                           gboolean        is_stop)
{
  GdkEvent *event;

  event = create_scroll_event (seat, FALSE);
  event->scroll.direction = GDK_SCROLL_SMOOTH;
  event->scroll.delta_x = delta_x;
  event->scroll.delta_y = delta_y;
  event->scroll.is_stop = is_stop;

  _gdk_wayland_display_deliver_event (seat->display, event);
}

static void
flush_scroll_event (GdkWaylandSeat             *seat,
                    GdkWaylandPointerFrameData *pointer_frame)
{
  gboolean is_stop = FALSE;

  if (pointer_frame->discrete_x || pointer_frame->discrete_y)
    {
      GdkScrollDirection direction;

      if (pointer_frame->discrete_x > 0)
        direction = GDK_SCROLL_LEFT;
      else if (pointer_frame->discrete_x < 0)
        direction = GDK_SCROLL_RIGHT;
      else if (pointer_frame->discrete_y > 0)
        direction = GDK_SCROLL_UP;
      else
        direction = GDK_SCROLL_DOWN;

      flush_discrete_scroll_event (seat, direction);
    }

  /* Axes can stop independently, if we stop on one axis but have a
   * delta on the other, we don't count it as a stop event.
   */
  if (pointer_frame->is_scroll_stop &&
      pointer_frame->delta_x == 0 &&
      pointer_frame->delta_y == 0)
    is_stop = TRUE;

  flush_smooth_scroll_event (seat,
                             pointer_frame->delta_x,
                             pointer_frame->delta_y,
                             is_stop);

  pointer_frame->delta_x = 0;
  pointer_frame->delta_y = 0;
  pointer_frame->discrete_x = 0;
  pointer_frame->discrete_y = 0;
  pointer_frame->is_scroll_stop = FALSE;
}

static void
gdk_wayland_seat_flush_frame_event (GdkWaylandSeat *seat)
{
  if (seat->pointer_info.frame.event)
    {
      _gdk_wayland_display_deliver_event (gdk_seat_get_display (GDK_SEAT (seat)),
                                          seat->pointer_info.frame.event);
      seat->pointer_info.frame.event = NULL;
    }
  else
    flush_scroll_event (seat, &seat->pointer_info.frame);
}

static GdkEvent *
gdk_wayland_seat_get_frame_event (GdkWaylandSeat *seat,
                                  GdkEventType    evtype)
{
  if (seat->pointer_info.frame.event &&
      seat->pointer_info.frame.event->type != evtype)
    gdk_wayland_seat_flush_frame_event (seat);

  seat->pointer_info.frame.event = gdk_event_new (evtype);
  return seat->pointer_info.frame.event;
}

static void
pointer_handle_enter (void              *data,
                      struct wl_pointer *pointer,
                      uint32_t           serial,
                      struct wl_surface *surface,
                      wl_fixed_t         sx,
                      wl_fixed_t         sy)
{
  GdkWaylandDeviceData *device = data;
  GdkEvent *event;
  GdkWaylandDisplay *wayland_display =
    GDK_WAYLAND_DISPLAY (device->display);

  if (!surface)
    return;
  if (!GDK_IS_WINDOW (wl_surface_get_user_data (surface)))
    return;

  _gdk_wayland_display_update_serial (wayland_display, serial);

  device->pointer_info.focus = wl_surface_get_user_data(surface);
  g_object_ref(device->pointer_info.focus);

  device->pointer_info.button_modifiers = 0;

  device->pointer_info.surface_x = wl_fixed_to_double (sx);
  device->pointer_info.surface_y = wl_fixed_to_double (sy);
  device->pointer_info.enter_serial = serial;

  event = gdk_wayland_seat_get_frame_event (device, GDK_ENTER_NOTIFY);
  event->crossing.window = g_object_ref (device->pointer_info.focus);
  gdk_event_set_device (event, device->master_pointer);
  gdk_event_set_source_device (event, device->pointer);
  gdk_event_set_seat (event, gdk_device_get_seat (device->master_pointer));
  event->crossing.subwindow = NULL;
  event->crossing.time = (guint32)(g_get_monotonic_time () / 1000);
  event->crossing.mode = GDK_CROSSING_NORMAL;
  event->crossing.detail = GDK_NOTIFY_NONLINEAR;
  event->crossing.focus = TRUE;
  event->crossing.state = 0;

  gdk_wayland_device_update_window_cursor (device->master_pointer);

  get_coordinates (device->master_pointer,
                   &event->crossing.x,
                   &event->crossing.y,
                   &event->crossing.x_root,
                   &event->crossing.y_root);

  GDK_NOTE (EVENTS,
            g_message ("enter, device %p surface %p",
                       device, device->pointer_info.focus));

  if (wayland_display->seat_version < WL_POINTER_HAS_FRAME)
    gdk_wayland_seat_flush_frame_event (device);
}

static void
pointer_handle_leave (void              *data,
                      struct wl_pointer *pointer,
                      uint32_t           serial,
                      struct wl_surface *surface)
{
  GdkWaylandDeviceData *device = data;
  GdkEvent *event;
  GdkWaylandDisplay *wayland_display = GDK_WAYLAND_DISPLAY (device->display);

  if (!surface)
    return;

  if (!GDK_IS_WINDOW (wl_surface_get_user_data (surface)))
    return;

  if (!device->pointer_info.focus)
    return;

  _gdk_wayland_display_update_serial (wayland_display, serial);

  event = gdk_wayland_seat_get_frame_event (device, GDK_LEAVE_NOTIFY);
  event->crossing.window = g_object_ref (device->pointer_info.focus);
  gdk_event_set_device (event, device->master_pointer);
  gdk_event_set_source_device (event, device->pointer);
  gdk_event_set_seat (event, gdk_device_get_seat (device->master_pointer));
  event->crossing.subwindow = NULL;
  event->crossing.time = (guint32)(g_get_monotonic_time () / 1000);
  event->crossing.mode = GDK_CROSSING_NORMAL;
  event->crossing.detail = GDK_NOTIFY_NONLINEAR;
  event->crossing.focus = TRUE;
  event->crossing.state = 0;

  gdk_wayland_device_update_window_cursor (device->master_pointer);

  get_coordinates (device->master_pointer,
                   &event->crossing.x,
                   &event->crossing.y,
                   &event->crossing.x_root,
                   &event->crossing.y_root);

  GDK_NOTE (EVENTS,
            g_message ("leave, device %p surface %p",
                       device, device->pointer_info.focus));

  g_object_unref (device->pointer_info.focus);
  device->pointer_info.focus = NULL;
  if (device->cursor)
    gdk_wayland_pointer_stop_cursor_animation (&device->pointer_info);

  device->pointer_info.focus = NULL;

  if (wayland_display->seat_version < WL_POINTER_HAS_FRAME)
    gdk_wayland_seat_flush_frame_event (device);
}

static void
pointer_handle_motion (void              *data,
                       struct wl_pointer *pointer,
                       uint32_t           time,
                       wl_fixed_t         sx,
                       wl_fixed_t         sy)
{
  GdkWaylandDeviceData *device = data;
  GdkWaylandDisplay *display = GDK_WAYLAND_DISPLAY (device->display);
  GdkEvent *event;

  if (!device->pointer_info.focus)
    return;

  device->pointer_info.time = time;
  device->pointer_info.surface_x = wl_fixed_to_double (sx);
  device->pointer_info.surface_y = wl_fixed_to_double (sy);

  event = gdk_wayland_seat_get_frame_event (device, GDK_MOTION_NOTIFY);
  event->motion.window = g_object_ref (device->pointer_info.focus);
  gdk_event_set_device (event, device->master_pointer);
  gdk_event_set_source_device (event, device->pointer);
  gdk_event_set_seat (event, gdk_device_get_seat (device->master_pointer));
  event->motion.time = time;
  event->motion.axes = NULL;
  event->motion.state = device_get_modifiers (device->master_pointer);
  event->motion.is_hint = 0;
  gdk_event_set_screen (event, display->screen);

  get_coordinates (device->master_pointer,
                   &event->motion.x,
                   &event->motion.y,
                   &event->motion.x_root,
                   &event->motion.y_root);

  GDK_NOTE (EVENTS,
            g_message ("motion %f %f, device %p state %d",
                       wl_fixed_to_double (sx), wl_fixed_to_double (sy),
		       device, event->motion.state));

  if (display->seat_version < WL_POINTER_HAS_FRAME)
    gdk_wayland_seat_flush_frame_event (device);
}

static void
pointer_handle_button (void              *data,
                       struct wl_pointer *pointer,
                       uint32_t           serial,
                       uint32_t           time,
                       uint32_t           button,
                       uint32_t           state)
{
  GdkWaylandDeviceData *device = data;
  GdkWaylandDisplay *display = GDK_WAYLAND_DISPLAY (device->display);
  GdkEvent *event;
  uint32_t modifier;
  int gdk_button;

  if (!device->pointer_info.focus)
    return;

  _gdk_wayland_display_update_serial (display, serial);

  switch (button)
    {
    case BTN_LEFT:
      gdk_button = GDK_BUTTON_PRIMARY;
      break;
    case BTN_MIDDLE:
      gdk_button = GDK_BUTTON_MIDDLE;
      break;
    case BTN_RIGHT:
      gdk_button = GDK_BUTTON_SECONDARY;
      break;
    default:
       /* For compatibility reasons, all additional buttons go after the old 4-7 scroll ones */
      gdk_button = button - BUTTON_BASE + 4;
      break;
    }

  device->pointer_info.time = time;
  if (state)
    device->pointer_info.press_serial = serial;

  event = gdk_wayland_seat_get_frame_event (device,
                                            state ? GDK_BUTTON_PRESS :
                                            GDK_BUTTON_RELEASE);
  event->button.window = g_object_ref (device->pointer_info.focus);
  gdk_event_set_device (event, device->master_pointer);
  gdk_event_set_source_device (event, device->pointer);
  gdk_event_set_seat (event, gdk_device_get_seat (device->master_pointer));
  event->button.time = time;
  event->button.axes = NULL;
  event->button.state = device_get_modifiers (device->master_pointer);
  event->button.button = gdk_button;
  gdk_event_set_screen (event, display->screen);

  get_coordinates (device->master_pointer,
                   &event->button.x,
                   &event->button.y,
                   &event->button.x_root,
                   &event->button.y_root);

  modifier = 1 << (8 + gdk_button - 1);
  if (state)
    device->pointer_info.button_modifiers |= modifier;
  else
    device->pointer_info.button_modifiers &= ~modifier;

  GDK_NOTE (EVENTS,
	    g_message ("button %d %s, device %p state %d",
		       event->button.button,
		       state ? "press" : "release",
                       device,
                       event->button.state));

  if (display->seat_version < WL_POINTER_HAS_FRAME)
    gdk_wayland_seat_flush_frame_event (device);
}

static void
pointer_handle_axis (void              *data,
                     struct wl_pointer *pointer,
                     uint32_t           time,
                     uint32_t           axis,
                     wl_fixed_t         value)
{
  GdkWaylandSeat *seat = data;
  GdkWaylandPointerFrameData *pointer_frame = &seat->pointer_info.frame;
  GdkWaylandDisplay *display = GDK_WAYLAND_DISPLAY (seat->display);

  if (!seat->pointer_info.focus)
    return;

  /* get the delta and convert it into the expected range */
  switch (axis)
    {
    case WL_POINTER_AXIS_VERTICAL_SCROLL:
      pointer_frame->delta_y = wl_fixed_to_double (value) / 10.0;
      break;
    case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
      pointer_frame->delta_x = wl_fixed_to_double (value) / 10.0;
      break;
    default:
      g_return_if_reached ();
    }

  seat->pointer_info.time = time;

  GDK_NOTE (EVENTS,
            g_message ("scroll, axis %d, value %f, seat %p",
                       axis, wl_fixed_to_double (value) / 10.0,
                       seat));

  if (display->seat_version < WL_POINTER_HAS_FRAME)
    gdk_wayland_seat_flush_frame_event (seat);
}

static void
pointer_handle_frame (void              *data,
                      struct wl_pointer *pointer)
{
  GdkWaylandSeat *seat = data;

  if (!seat->pointer_info.focus)
    return;

  GDK_NOTE (EVENTS,
            g_message ("frame, seat %p", seat));

  gdk_wayland_seat_flush_frame_event (seat);
}

static void
pointer_handle_axis_source (void                        *data,
                            struct wl_pointer           *pointer,
                            enum wl_pointer_axis_source  source)
{
  GdkWaylandSeat *seat = data;

  if (!seat->pointer_info.focus)
    return;

  /* We don't need to handle the scroll source right now. It only has real
   * meaning for 'finger' (to trigger kinetic scrolling). The axis_stop
   * event will generate the zero delta required to trigger kinetic
   * scrolling, so explicity handling the source is not required.
   */

  GDK_NOTE (EVENTS,
            g_message ("axis source %d, seat %p", source, seat));
}

static void
pointer_handle_axis_stop (void              *data,
                          struct wl_pointer *pointer,
                          uint32_t           time,
                          uint32_t           axis)
{
  GdkWaylandSeat *seat = data;
  GdkWaylandPointerFrameData *pointer_frame = &seat->pointer_info.frame;

  if (!seat->pointer_info.focus)
    return;

  seat->pointer_info.time = time;

  switch (axis)
    {
    case WL_POINTER_AXIS_VERTICAL_SCROLL:
      pointer_frame->delta_y = 0;
      break;
    case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
      pointer_frame->delta_x = 0;
      break;
    default:
      g_return_if_reached ();
    }

  pointer_frame->is_scroll_stop = TRUE;

  GDK_NOTE (EVENTS,
            g_message ("axis stop, seat %p", seat));
}

static void
pointer_handle_axis_discrete (void              *data,
                              struct wl_pointer *pointer,
                              uint32_t           axis,
                              int32_t            value)
{
  GdkWaylandSeat *seat = data;
  GdkWaylandPointerFrameData *pointer_frame = &seat->pointer_info.frame;

  if (!seat->pointer_info.focus)
    return;

  switch (axis)
    {
    case WL_POINTER_AXIS_VERTICAL_SCROLL:
      pointer_frame->discrete_y = value;
      break;
    case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
      pointer_frame->discrete_x = value;
      break;
    default:
      g_return_if_reached ();
    }

  GDK_NOTE (EVENTS,
            g_message ("discrete scroll, axis %d, value %d, seat %p",
                       axis, value, seat));
}

static void
keyboard_handle_keymap (void               *data,
                        struct wl_keyboard *keyboard,
                        uint32_t            format,
                        int                 fd,
                        uint32_t            size)
{
  GdkWaylandDeviceData *device = data;

  _gdk_wayland_keymap_update_from_fd (device->keymap, format, fd, size);

  g_signal_emit_by_name (device->keymap, "keys-changed");
  g_signal_emit_by_name (device->keymap, "state-changed");
  g_signal_emit_by_name (device->keymap, "direction-changed");
}

static void
keyboard_handle_enter (void               *data,
                       struct wl_keyboard *keyboard,
                       uint32_t            serial,
                       struct wl_surface  *surface,
                       struct wl_array    *keys)
{
  GdkWaylandDeviceData *device = data;
  GdkEvent *event;
  GdkWaylandDisplay *display = GDK_WAYLAND_DISPLAY (device->display);

  if (!surface)
    return;

  if (!GDK_IS_WINDOW (wl_surface_get_user_data (surface)))
    return;

  _gdk_wayland_display_update_serial (display, serial);

  device->keyboard_focus = wl_surface_get_user_data (surface);
  g_object_ref (device->keyboard_focus);

  event = gdk_event_new (GDK_FOCUS_CHANGE);
  event->focus_change.window = g_object_ref (device->keyboard_focus);
  event->focus_change.send_event = FALSE;
  event->focus_change.in = TRUE;
  gdk_event_set_device (event, device->master_keyboard);
  gdk_event_set_source_device (event, device->keyboard);
  gdk_event_set_seat (event, gdk_device_get_seat (device->master_pointer));

  GDK_NOTE (EVENTS,
            g_message ("focus in, device %p surface %p",
                       device, device->keyboard_focus));

  _gdk_wayland_display_deliver_event (device->display, event);

  if (device->pending_selection != GDK_NONE)
    {
      emit_selection_owner_change (device->keyboard_focus,
                                   device->pending_selection);
      device->pending_selection = GDK_NONE;
    }
}

static void stop_key_repeat (GdkWaylandDeviceData *device);

static void
keyboard_handle_leave (void               *data,
                       struct wl_keyboard *keyboard,
                       uint32_t            serial,
                       struct wl_surface  *surface)
{
  GdkWaylandDeviceData *device = data;
  GdkEvent *event;
  GdkWaylandDisplay *display = GDK_WAYLAND_DISPLAY (device->display);

  if (!device->keyboard_focus)
    return;

  /* gdk_window_is_destroyed() might already return TRUE for
   * device->keyboard_focus here, which would happen if we destroyed the
   * window before loosing keyboard focus.
   */

  stop_key_repeat (device);

  _gdk_wayland_display_update_serial (display, serial);

  event = gdk_event_new (GDK_FOCUS_CHANGE);
  event->focus_change.window = g_object_ref (device->keyboard_focus);
  event->focus_change.send_event = FALSE;
  event->focus_change.in = FALSE;
  gdk_event_set_device (event, device->master_keyboard);
  gdk_event_set_source_device (event, device->keyboard);
  gdk_event_set_seat (event, gdk_device_get_seat (device->master_keyboard));

  g_object_unref (device->keyboard_focus);
  device->keyboard_focus = NULL;

  GDK_NOTE (EVENTS,
            g_message ("focus out, device %p surface %p",
                       device, device->keyboard_focus));

  _gdk_wayland_display_deliver_event (device->display, event);
}

static gboolean keyboard_repeat (gpointer data);

static void
translate_keyboard_string (GdkEventKey *event)
{
  gunichar c = 0;
  gchar buf[7];

  /* Fill in event->string crudely, since various programs
   * depend on it.
   */
  event->string = NULL;

  if (event->keyval != GDK_KEY_VoidSymbol)
    c = gdk_keyval_to_unicode (event->keyval);

  if (c)
    {
      gsize bytes_written;
      gint len;

      /* Apply the control key - Taken from Xlib */
      if (event->state & GDK_CONTROL_MASK)
        {
          if ((c >= '@' && c < '\177') || c == ' ')
            c &= 0x1F;
          else if (c == '2')
            {
              event->string = g_memdup ("\0\0", 2);
              event->length = 1;
              buf[0] = '\0';
              return;
            }
          else if (c >= '3' && c <= '7')
            c -= ('3' - '\033');
          else if (c == '8')
            c = '\177';
          else if (c == '/')
            c = '_' & 0x1F;
        }

      len = g_unichar_to_utf8 (c, buf);
      buf[len] = '\0';

      event->string = g_locale_from_utf8 (buf, len,
                                          NULL, &bytes_written,
                                          NULL);
      if (event->string)
        event->length = bytes_written;
    }
  else if (event->keyval == GDK_KEY_Escape)
    {
      event->length = 1;
      event->string = g_strdup ("\033");
    }
  else if (event->keyval == GDK_KEY_Return ||
           event->keyval == GDK_KEY_KP_Enter)
    {
      event->length = 1;
      event->string = g_strdup ("\r");
    }

  if (!event->string)
    {
      event->length = 0;
      event->string = g_strdup ("");
    }
}

static GSettings *
get_keyboard_settings (GdkWaylandDeviceData *device)
{
  if (!device->keyboard_settings)
    {
      GSettingsSchemaSource *source;
      GSettingsSchema *schema;

      source = g_settings_schema_source_get_default ();
      schema = g_settings_schema_source_lookup (source, "org.gnome.settings-daemon.peripherals.keyboard", FALSE);
      if (schema != NULL)
        {
          device->keyboard_settings = g_settings_new_full (schema, NULL, NULL);
          g_settings_schema_unref (schema);
        }
    }

  return device->keyboard_settings;
}

static gboolean
get_key_repeat (GdkWaylandDeviceData *device,
                guint                *delay,
                guint                *interval)
{
  gboolean repeat;

  if (device->have_server_repeat)
    {
      if (device->server_repeat_rate > 0)
        {
          repeat = TRUE;
          *delay = device->server_repeat_delay;
          *interval = (1000 / device->server_repeat_rate);
        }
      else
        {
          repeat = FALSE;
        }
    }
  else
    {
      GSettings *keyboard_settings = get_keyboard_settings (device);

      if (keyboard_settings)
        {
          repeat = g_settings_get_boolean (keyboard_settings, "repeat");
          *delay = g_settings_get_uint (keyboard_settings, "delay");
          *interval = g_settings_get_uint (keyboard_settings, "repeat-interval");
        }
      else
        {
          repeat = TRUE;
          *delay = 400;
          *interval = 80;
        }
    }

  return repeat;
}

static void
stop_key_repeat (GdkWaylandDeviceData *device)
{
  if (device->repeat_timer)
    {
      g_source_remove (device->repeat_timer);
      device->repeat_timer = 0;
    }
}

static gboolean
deliver_key_event (GdkWaylandDeviceData *device,
                   uint32_t              time_,
                   uint32_t              key,
                   uint32_t              state)
{
  GdkEvent *event;
  struct xkb_state *xkb_state;
  struct xkb_keymap *xkb_keymap;
  GdkKeymap *keymap;
  xkb_keysym_t sym;
  guint delay, interval;

  keymap = device->keymap;
  xkb_state = _gdk_wayland_keymap_get_xkb_state (keymap);
  xkb_keymap = _gdk_wayland_keymap_get_xkb_keymap (keymap);

  sym = xkb_state_key_get_one_sym (xkb_state, key);

  device->pointer_info.time = time_;
  device->key_modifiers = gdk_keymap_get_modifier_state (keymap);

  event = gdk_event_new (state ? GDK_KEY_PRESS : GDK_KEY_RELEASE);
  event->key.window = device->keyboard_focus ? g_object_ref (device->keyboard_focus) : NULL;
  gdk_event_set_device (event, device->master_keyboard);
  gdk_event_set_source_device (event, device->keyboard);
  gdk_event_set_seat (event, gdk_device_get_seat (device->master_keyboard));
  event->key.time = time_;
  event->key.state = device_get_modifiers (device->master_pointer);
  event->key.group = 0;
  event->key.hardware_keycode = key;
  event->key.keyval = sym;
  event->key.is_modifier = _gdk_wayland_keymap_key_is_modifier (keymap, key);

  translate_keyboard_string (&event->key);

  _gdk_wayland_display_deliver_event (device->display, event);

  GDK_NOTE (EVENTS,
            g_message ("keyboard event, code %d, sym %d, "
                       "string %s, mods 0x%x",
                       event->key.hardware_keycode, event->key.keyval,
                       event->key.string, event->key.state));

  if (!xkb_keymap_key_repeats (xkb_keymap, key))
    return FALSE;

  if (!get_key_repeat (device, &delay, &interval))
    return FALSE;

  device->repeat_count++;
  device->repeat_key = key;

  if (state == 0)
    {
      stop_key_repeat (device);
      return FALSE;
    }
  else
    {
      switch (device->repeat_count)
        {
        case 1:
          stop_key_repeat (device);
          device->repeat_timer =
            gdk_threads_add_timeout (delay, keyboard_repeat, device);
          g_source_set_name_by_id (device->repeat_timer, "[gtk+] keyboard_repeat");
          return TRUE;
        case 2:
          device->repeat_timer =
            gdk_threads_add_timeout (interval, keyboard_repeat, device);
          g_source_set_name_by_id (device->repeat_timer, "[gtk+] keyboard_repeat");
          return FALSE;
        default:
          return TRUE;
        }
    }
}

static gboolean
keyboard_repeat (gpointer data)
{
  GdkWaylandDeviceData *device = data;

  return deliver_key_event (device, device->pointer_info.time, device->repeat_key, 1);
}

static void
keyboard_handle_key (void               *data,
                     struct wl_keyboard *keyboard,
                     uint32_t            serial,
                     uint32_t            time,
                     uint32_t            key,
                     uint32_t            state_w)
{
  GdkWaylandDeviceData *device = data;
  GdkWaylandDisplay *display = GDK_WAYLAND_DISPLAY (device->display);

  if (!device->keyboard_focus)
    return;

  device->repeat_count = 0;
  _gdk_wayland_display_update_serial (display, serial);
  deliver_key_event (data, time, key + 8, state_w);
}

static void
keyboard_handle_modifiers (void               *data,
                           struct wl_keyboard *keyboard,
                           uint32_t            serial,
                           uint32_t            mods_depressed,
                           uint32_t            mods_latched,
                           uint32_t            mods_locked,
                           uint32_t            group)
{
  GdkWaylandDeviceData *device = data;
  GdkKeymap *keymap;
  struct xkb_state *xkb_state;
  PangoDirection direction;

  keymap = device->keymap;
  direction = gdk_keymap_get_direction (keymap);
  xkb_state = _gdk_wayland_keymap_get_xkb_state (keymap);
  device->key_modifiers = mods_depressed | mods_latched | mods_locked;

  xkb_state_update_mask (xkb_state, mods_depressed, mods_latched, mods_locked, group, 0, 0);

  g_signal_emit_by_name (keymap, "state-changed");
  if (direction != gdk_keymap_get_direction (keymap))
    g_signal_emit_by_name (keymap, "direction-changed");
}

static void
keyboard_handle_repeat_info (void               *data,
                             struct wl_keyboard *keyboard,
                             int32_t             rate,
                             int32_t             delay)
{
  GdkWaylandDeviceData *device = data;

  device->have_server_repeat = TRUE;
  device->server_repeat_rate = rate;
  device->server_repeat_delay = delay;
}

static GdkWaylandTouchData *
gdk_wayland_device_add_touch (GdkWaylandDeviceData *device,
                              uint32_t              id,
                              struct wl_surface    *surface)
{
  GdkWaylandTouchData *touch;

  touch = g_new0 (GdkWaylandTouchData, 1);
  touch->id = id;
  touch->window = wl_surface_get_user_data (surface);
  touch->initial_touch = (g_hash_table_size (device->touches) == 0);

  g_hash_table_insert (device->touches, GUINT_TO_POINTER (id), touch);

  return touch;
}

static GdkWaylandTouchData *
gdk_wayland_device_get_touch (GdkWaylandDeviceData *device,
                              uint32_t              id)
{
  return g_hash_table_lookup (device->touches, GUINT_TO_POINTER (id));
}

static void
gdk_wayland_device_remove_touch (GdkWaylandDeviceData *device,
                                 uint32_t              id)
{
  g_hash_table_remove (device->touches, GUINT_TO_POINTER (id));
}

static GdkEvent *
_create_touch_event (GdkWaylandDeviceData *device,
                     GdkWaylandTouchData  *touch,
                     GdkEventType          evtype,
                     uint32_t              time)
{
  GdkWaylandDisplay *display = GDK_WAYLAND_DISPLAY (device->display);
  gint x_root, y_root;
  GdkEvent *event;

  event = gdk_event_new (evtype);
  event->touch.window = g_object_ref (touch->window);
  gdk_event_set_device (event, device->touch_master);
  gdk_event_set_source_device (event, device->touch);
  gdk_event_set_seat (event, gdk_device_get_seat (device->touch_master));
  event->touch.time = time;
  event->touch.state = device_get_modifiers (device->touch_master);
  gdk_event_set_screen (event, display->screen);
  event->touch.sequence = GDK_SLOT_TO_EVENT_SEQUENCE (touch->id);

  if (touch->initial_touch)
    {
      _gdk_event_set_pointer_emulated (event, TRUE);
      event->touch.emulating_pointer = TRUE;
    }

  gdk_window_get_root_coords (touch->window,
                              touch->x, touch->y,
                              &x_root, &y_root);

  event->touch.x = touch->x;
  event->touch.y = touch->y;
  event->touch.x_root = x_root;
  event->touch.y_root = y_root;

  return event;
}

static void
mimic_pointer_emulating_touch_info (GdkDevice           *device,
                                    GdkWaylandTouchData *touch)
{
  GdkWaylandPointerData *pointer;

  pointer = GDK_WAYLAND_DEVICE (device)->pointer;
  g_set_object (&pointer->focus, touch->window);
  pointer->press_serial = pointer->enter_serial = touch->touch_down_serial;
  pointer->surface_x = touch->x;
  pointer->surface_y = touch->y;
}

static void
touch_handle_down (void              *data,
                   struct wl_touch   *wl_touch,
                   uint32_t           serial,
                   uint32_t           time,
                   struct wl_surface *wl_surface,
                   int32_t            id,
                   wl_fixed_t         x,
                   wl_fixed_t         y)
{
  GdkWaylandDeviceData *device = data;
  GdkWaylandDisplay *display = GDK_WAYLAND_DISPLAY (device->display);
  GdkWaylandTouchData *touch;
  GdkEvent *event;

  _gdk_wayland_display_update_serial (display, serial);

  touch = gdk_wayland_device_add_touch (device, id, wl_surface);
  touch->x = wl_fixed_to_double (x);
  touch->y = wl_fixed_to_double (y);
  touch->touch_down_serial = serial;

  event = _create_touch_event (device, touch, GDK_TOUCH_BEGIN, time);

  if (touch->initial_touch)
    {
      emulate_touch_crossing (touch->window, NULL,
                              device->touch_master, device->touch, touch,
                              GDK_ENTER_NOTIFY, GDK_CROSSING_NORMAL, time);

      GDK_WAYLAND_DEVICE(device->touch_master)->emulating_touch = touch;
      mimic_pointer_emulating_touch_info (device->touch_master, touch);
    }

  GDK_NOTE (EVENTS,
            g_message ("touch begin %f %f", event->touch.x, event->touch.y));

  _gdk_wayland_display_deliver_event (device->display, event);
}

static void
touch_handle_up (void            *data,
                 struct wl_touch *wl_touch,
                 uint32_t         serial,
                 uint32_t         time,
                 int32_t          id)
{
  GdkWaylandDeviceData *device = data;
  GdkWaylandDisplay *display = GDK_WAYLAND_DISPLAY (device->display);
  GdkWaylandTouchData *touch;
  GdkEvent *event;

  _gdk_wayland_display_update_serial (display, serial);

  touch = gdk_wayland_device_get_touch (device, id);
  event = _create_touch_event (device, touch, GDK_TOUCH_END, time);

  GDK_NOTE (EVENTS,
            g_message ("touch end %f %f", event->touch.x, event->touch.y));

  _gdk_wayland_display_deliver_event (device->display, event);

  if (touch->initial_touch)
    {
      GdkWaylandPointerData *pointer_info;

      emulate_touch_crossing (touch->window, NULL,
                              device->touch_master, device->touch, touch,
                              GDK_LEAVE_NOTIFY, GDK_CROSSING_NORMAL, time);
      GDK_WAYLAND_DEVICE(device->touch_master)->emulating_touch = NULL;
      pointer_info = GDK_WAYLAND_DEVICE (device->touch_master)->pointer;
      g_clear_object (&pointer_info->focus);
    }

  gdk_wayland_device_remove_touch (device, id);
}

static void
touch_handle_motion (void            *data,
                     struct wl_touch *wl_touch,
                     uint32_t         time,
                     int32_t          id,
                     wl_fixed_t       x,
                     wl_fixed_t       y)
{
  GdkWaylandDeviceData *device = data;
  GdkWaylandTouchData *touch;
  GdkEvent *event;

  touch = gdk_wayland_device_get_touch (device, id);
  touch->x = wl_fixed_to_double (x);
  touch->y = wl_fixed_to_double (y);

  if (touch->initial_touch)
    mimic_pointer_emulating_touch_info (device->touch_master, touch);

  event = _create_touch_event (device, touch, GDK_TOUCH_UPDATE, time);

  GDK_NOTE (EVENTS,
            g_message ("touch update %f %f", event->touch.x, event->touch.y));

  _gdk_wayland_display_deliver_event (device->display, event);
}

static void
touch_handle_frame (void            *data,
                    struct wl_touch *wl_touch)
{
}

static void
touch_handle_cancel (void            *data,
                     struct wl_touch *wl_touch)
{
  GdkWaylandSeat *wayland_seat = data;
  GdkWaylandTouchData *touch;
  GHashTableIter iter;
  GdkEvent *event;

  if (GDK_WAYLAND_DEVICE (wayland_seat->touch_master)->emulating_touch)
    {
      touch = GDK_WAYLAND_DEVICE (wayland_seat->touch_master)->emulating_touch;
      GDK_WAYLAND_DEVICE (wayland_seat->touch_master)->emulating_touch = NULL;
      emulate_touch_crossing (touch->window, NULL,
                              wayland_seat->touch_master, wayland_seat->touch,
                              touch, GDK_LEAVE_NOTIFY, GDK_CROSSING_NORMAL,
                              GDK_CURRENT_TIME);
    }

  g_hash_table_iter_init (&iter, wayland_seat->touches);

  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &touch))
    {
      event = _create_touch_event (wayland_seat, touch, GDK_TOUCH_CANCEL,
                                   GDK_CURRENT_TIME);
      _gdk_wayland_display_deliver_event (wayland_seat->display, event);
      g_hash_table_iter_remove (&iter);
    }

  GDK_NOTE (EVENTS, g_message ("touch cancel"));
}

static void
emit_gesture_swipe_event (GdkWaylandDeviceData    *device,
                          GdkTouchpadGesturePhase  phase,
                          guint32                  _time,
                          guint32                  n_fingers,
                          gdouble                  dx,
                          gdouble                  dy)
{
  GdkWaylandDisplay *display = GDK_WAYLAND_DISPLAY (device->display);
  GdkEvent *event;

  if (!device->pointer_info.focus)
    return;

  device->pointer_info.time = _time;

  event = gdk_event_new (GDK_TOUCHPAD_SWIPE);
  event->touchpad_swipe.phase = phase;
  event->touchpad_swipe.window = g_object_ref (device->pointer_info.focus);
  gdk_event_set_device (event, device->master_pointer);
  gdk_event_set_source_device (event, device->pointer);
  gdk_event_set_seat (event, gdk_device_get_seat (device->master_pointer));
  event->touchpad_swipe.time = _time;
  event->touchpad_swipe.state = device_get_modifiers (device->master_pointer);
  gdk_event_set_screen (event, display->screen);
  event->touchpad_swipe.dx = dx;
  event->touchpad_swipe.dy = dy;
  event->touchpad_swipe.n_fingers = n_fingers;

  get_coordinates (device->master_pointer,
                   &event->touchpad_swipe.x,
                   &event->touchpad_swipe.y,
                   &event->touchpad_swipe.x_root,
                   &event->touchpad_swipe.y_root);

  GDK_NOTE (EVENTS,
            g_message ("swipe event %d, coords: %f %f, device %p state %d",
                       event->type, event->touchpad_swipe.x,
                       event->touchpad_swipe.y, device,
                       event->touchpad_swipe.state));

  _gdk_wayland_display_deliver_event (device->display, event);
}

static void
gesture_swipe_begin (void                                *data,
                     struct zwp_pointer_gesture_swipe_v1 *swipe,
                     uint32_t                             serial,
                     uint32_t                             time,
                     struct wl_surface                   *surface,
                     uint32_t                             fingers)
{
  GdkWaylandDeviceData *device = data;
  GdkWaylandDisplay *display = GDK_WAYLAND_DISPLAY (device->display);

  _gdk_wayland_display_update_serial (display, serial);

  emit_gesture_swipe_event (device,
                            GDK_TOUCHPAD_GESTURE_PHASE_BEGIN,
                            time, fingers, 0, 0);
  device->gesture_n_fingers = fingers;
}

static void
gesture_swipe_update (void                                *data,
                      struct zwp_pointer_gesture_swipe_v1 *swipe,
                      uint32_t                             time,
                      wl_fixed_t                           dx,
                      wl_fixed_t                           dy)
{
  GdkWaylandDeviceData *device = data;

  emit_gesture_swipe_event (device,
                            GDK_TOUCHPAD_GESTURE_PHASE_UPDATE,
                            time,
                            device->gesture_n_fingers,
                            wl_fixed_to_double (dx),
                            wl_fixed_to_double (dy));
}

static void
gesture_swipe_end (void                                *data,
                   struct zwp_pointer_gesture_swipe_v1 *swipe,
                   uint32_t                             serial,
                   uint32_t                             time,
                   int32_t                              cancelled)
{
  GdkWaylandDeviceData *device = data;
  GdkWaylandDisplay *display = GDK_WAYLAND_DISPLAY (device->display);
  GdkTouchpadGesturePhase phase;

  _gdk_wayland_display_update_serial (display, serial);

  phase = (cancelled) ?
    GDK_TOUCHPAD_GESTURE_PHASE_CANCEL :
    GDK_TOUCHPAD_GESTURE_PHASE_END;

  emit_gesture_swipe_event (device, phase, time,
                            device->gesture_n_fingers, 0, 0);
}

static void
emit_gesture_pinch_event (GdkWaylandDeviceData    *device,
                          GdkTouchpadGesturePhase  phase,
                          guint32                  _time,
                          guint                    n_fingers,
                          gdouble                  dx,
                          gdouble                  dy,
                          gdouble                  scale,
                          gdouble                  angle_delta)
{
  GdkWaylandDisplay *display = GDK_WAYLAND_DISPLAY (device->display);
  GdkEvent *event;

  if (!device->pointer_info.focus)
    return;

  device->pointer_info.time = _time;

  event = gdk_event_new (GDK_TOUCHPAD_PINCH);
  event->touchpad_pinch.phase = phase;
  event->touchpad_pinch.window = g_object_ref (device->pointer_info.focus);
  gdk_event_set_device (event, device->master_pointer);
  gdk_event_set_source_device (event, device->pointer);
  gdk_event_set_seat (event, gdk_device_get_seat (device->master_pointer));
  event->touchpad_pinch.time = _time;
  event->touchpad_pinch.state = device_get_modifiers (device->master_pointer);
  gdk_event_set_screen (event, display->screen);
  event->touchpad_pinch.dx = dx;
  event->touchpad_pinch.dy = dy;
  event->touchpad_pinch.scale = scale;
  event->touchpad_pinch.angle_delta = angle_delta * G_PI / 180;
  event->touchpad_pinch.n_fingers = n_fingers;

  get_coordinates (device->master_pointer,
                   &event->touchpad_pinch.x,
                   &event->touchpad_pinch.y,
                   &event->touchpad_pinch.x_root,
                   &event->touchpad_pinch.y_root);

  GDK_NOTE (EVENTS,
            g_message ("pinch event %d, coords: %f %f, device %p state %d",
                       event->type, event->touchpad_pinch.x,
                       event->touchpad_pinch.y, device,
                       event->touchpad_pinch.state));

  _gdk_wayland_display_deliver_event (device->display, event);
}

static void
gesture_pinch_begin (void                                *data,
                     struct zwp_pointer_gesture_pinch_v1 *pinch,
                     uint32_t                             serial,
                     uint32_t                             time,
                     struct wl_surface                   *surface,
                     uint32_t                             fingers)
{
  GdkWaylandDeviceData *device = data;
  GdkWaylandDisplay *display = GDK_WAYLAND_DISPLAY (device->display);

  _gdk_wayland_display_update_serial (display, serial);
  emit_gesture_pinch_event (device,
                            GDK_TOUCHPAD_GESTURE_PHASE_BEGIN,
                            time, fingers, 0, 0, 1, 0);
  device->gesture_n_fingers = fingers;
}

static void
gesture_pinch_update (void                                *data,
                      struct zwp_pointer_gesture_pinch_v1 *pinch,
                      uint32_t                             time,
                      wl_fixed_t                           dx,
                      wl_fixed_t                           dy,
                      wl_fixed_t                           scale,
                      wl_fixed_t                           rotation)
{
  GdkWaylandDeviceData *device = data;

  emit_gesture_pinch_event (device,
                            GDK_TOUCHPAD_GESTURE_PHASE_UPDATE, time,
                            device->gesture_n_fingers,
                            wl_fixed_to_double (dx),
                            wl_fixed_to_double (dy),
                            wl_fixed_to_double (scale),
                            wl_fixed_to_double (rotation));
}

static void
gesture_pinch_end (void                                *data,
                   struct zwp_pointer_gesture_pinch_v1 *pinch,
                   uint32_t                             serial,
                   uint32_t                             time,
                   int32_t                              cancelled)
{
  GdkWaylandDeviceData *device = data;
  GdkWaylandDisplay *display = GDK_WAYLAND_DISPLAY (device->display);
  GdkTouchpadGesturePhase phase;

  _gdk_wayland_display_update_serial (display, serial);

  phase = (cancelled) ?
    GDK_TOUCHPAD_GESTURE_PHASE_CANCEL :
    GDK_TOUCHPAD_GESTURE_PHASE_END;

  emit_gesture_pinch_event (device, phase,
                            time, device->gesture_n_fingers,
                            0, 0, 1, 0);
}

static GdkDevice *
tablet_select_device_for_tool (GdkWaylandTabletData *tablet,
                               GdkDeviceTool        *tool)
{
  GdkDevice *device;

  if (gdk_device_tool_get_tool_type (tool) == GDK_DEVICE_TOOL_TYPE_ERASER)
    device = tablet->eraser_device;
  else
    device = tablet->stylus_device;

  return device;
}

static void
_gdk_wayland_seat_remove_tool (GdkWaylandSeat           *seat,
                               GdkWaylandTabletToolData *tool)
{
  seat->tablet_tools = g_list_remove (seat->tablet_tools, tool);

  gdk_seat_tool_removed (GDK_SEAT (seat), tool->tool);

  zwp_tablet_tool_v1_destroy (tool->wp_tablet_tool);
  g_object_unref (tool->tool);
  g_free (tool);
}

static void
_gdk_wayland_seat_remove_tablet (GdkWaylandSeat       *seat,
                                 GdkWaylandTabletData *tablet)
{
  GdkWaylandDeviceManager *device_manager =
    GDK_WAYLAND_DEVICE_MANAGER (seat->device_manager);

  seat->tablets = g_list_remove (seat->tablets, tablet);

  zwp_tablet_v1_destroy (tablet->wp_tablet);

  device_manager->devices =
    g_list_remove (device_manager->devices, tablet->master);
  device_manager->devices =
    g_list_remove (device_manager->devices, tablet->stylus_device);
  device_manager->devices =
    g_list_remove (device_manager->devices, tablet->eraser_device);

  g_signal_emit_by_name (device_manager, "device-removed",
                         tablet->stylus_device);
  g_signal_emit_by_name (device_manager, "device-removed",
                         tablet->eraser_device);
  g_signal_emit_by_name (device_manager, "device-removed",
                         tablet->master);

  _gdk_device_set_associated_device (tablet->master, NULL);
  _gdk_device_set_associated_device (tablet->stylus_device, NULL);
  _gdk_device_set_associated_device (tablet->eraser_device, NULL);

  if (tablet->pointer_info.focus)
    g_object_unref (tablet->pointer_info.focus);

  wl_surface_destroy (tablet->pointer_info.pointer_surface);
  g_object_unref (tablet->master);
  g_object_unref (tablet->stylus_device);
  g_object_unref (tablet->eraser_device);
  g_free (tablet);
}

static void
tablet_handle_name (void                 *data,
                    struct zwp_tablet_v1 *wp_tablet,
                    const char           *name)
{
  GdkWaylandTabletData *tablet = data;

  tablet->name = g_strdup (name);
}

static void
tablet_handle_id (void                 *data,
                  struct zwp_tablet_v1 *wp_tablet,
                  uint32_t              vid,
                  uint32_t              pid)
{
  GdkWaylandTabletData *tablet = data;

  tablet->vid = vid;
  tablet->pid = pid;
}

static void
tablet_handle_path (void                 *data,
                    struct zwp_tablet_v1 *wp_tablet,
                    const char           *path)
{
  GdkWaylandTabletData *tablet = data;

  tablet->path = g_strdup (path);
}

static void
tablet_handle_done (void                 *data,
                    struct zwp_tablet_v1 *wp_tablet)
{
  GdkWaylandTabletData *tablet = data;
  GdkWaylandSeat *seat = GDK_WAYLAND_SEAT (tablet->seat);
  GdkDisplay *display = gdk_seat_get_display (GDK_SEAT (seat));
  GdkWaylandDeviceManager *device_manager =
    GDK_WAYLAND_DEVICE_MANAGER (seat->device_manager);
  GdkDevice *master, *stylus_device, *eraser_device;
  gchar *master_name, *eraser_name;

  master_name = g_strdup_printf ("Master pointer for %s", tablet->name);
  master = g_object_new (GDK_TYPE_WAYLAND_DEVICE,
                         "name", master_name,
                         "type", GDK_DEVICE_TYPE_MASTER,
                         "input-source", GDK_SOURCE_MOUSE,
                         "input-mode", GDK_MODE_SCREEN,
                         "has-cursor", TRUE,
                         "display", display,
                         "device-manager", device_manager,
                         "seat", seat,
                         NULL);
  GDK_WAYLAND_DEVICE (master)->device = seat;
  GDK_WAYLAND_DEVICE (master)->pointer = &tablet->pointer_info;

  eraser_name = g_strconcat (tablet->name, " (Eraser)", NULL);

  stylus_device = g_object_new (GDK_TYPE_WAYLAND_DEVICE,
                                "name", tablet->name,
                                "type", GDK_DEVICE_TYPE_SLAVE,
                                "input-source", GDK_SOURCE_PEN,
                                "input-mode", GDK_MODE_SCREEN,
                                "has-cursor", FALSE,
                                "display", display,
                                "device-manager", device_manager,
                                "seat", seat,
                                NULL);
  GDK_WAYLAND_DEVICE (stylus_device)->device = seat;

  eraser_device = g_object_new (GDK_TYPE_WAYLAND_DEVICE,
                                "name", eraser_name,
                                "type", GDK_DEVICE_TYPE_SLAVE,
                                "input-source", GDK_SOURCE_ERASER,
                                "input-mode", GDK_MODE_SCREEN,
                                "has-cursor", FALSE,
                                "display", display,
                                "device-manager", device_manager,
                                "seat", seat,
                                NULL);
  GDK_WAYLAND_DEVICE (eraser_device)->device = seat;

  tablet->master = master;
  device_manager->devices =
    g_list_prepend (device_manager->devices, tablet->master);
  g_signal_emit_by_name (device_manager, "device-added", master);

  tablet->stylus_device = stylus_device;
  device_manager->devices =
    g_list_prepend (device_manager->devices, tablet->stylus_device);
  g_signal_emit_by_name (device_manager, "device-added", stylus_device);

  tablet->eraser_device = eraser_device;
  device_manager->devices =
    g_list_prepend (device_manager->devices, tablet->eraser_device);
  g_signal_emit_by_name (device_manager, "device-added", eraser_device);

  _gdk_device_set_associated_device (master, seat->master_keyboard);
  _gdk_device_set_associated_device (stylus_device, master);
  _gdk_device_set_associated_device (eraser_device, master);

  g_free (eraser_name);
  g_free (master_name);
}

static void
tablet_handle_removed (void                 *data,
                       struct zwp_tablet_v1 *wp_tablet)
{
  GdkWaylandTabletData *tablet = data;

  _gdk_wayland_seat_remove_tablet (GDK_WAYLAND_SEAT (tablet->seat), tablet);
}

static const struct wl_pointer_listener pointer_listener = {
  pointer_handle_enter,
  pointer_handle_leave,
  pointer_handle_motion,
  pointer_handle_button,
  pointer_handle_axis,
  pointer_handle_frame,
  pointer_handle_axis_source,
  pointer_handle_axis_stop,
  pointer_handle_axis_discrete,
};

static const struct wl_keyboard_listener keyboard_listener = {
  keyboard_handle_keymap,
  keyboard_handle_enter,
  keyboard_handle_leave,
  keyboard_handle_key,
  keyboard_handle_modifiers,
  keyboard_handle_repeat_info,
};

static const struct wl_touch_listener touch_listener = {
  touch_handle_down,
  touch_handle_up,
  touch_handle_motion,
  touch_handle_frame,
  touch_handle_cancel
};

static const struct zwp_pointer_gesture_swipe_v1_listener gesture_swipe_listener = {
  gesture_swipe_begin,
  gesture_swipe_update,
  gesture_swipe_end
};

static const struct zwp_pointer_gesture_pinch_v1_listener gesture_pinch_listener = {
  gesture_pinch_begin,
  gesture_pinch_update,
  gesture_pinch_end
};

static const struct zwp_tablet_v1_listener tablet_listener = {
  tablet_handle_name,
  tablet_handle_id,
  tablet_handle_path,
  tablet_handle_done,
  tablet_handle_removed,
};

static void
seat_handle_capabilities (void                    *data,
                          struct wl_seat          *seat,
                          enum wl_seat_capability  caps)
{
  GdkWaylandDeviceData *device = data;
  GdkWaylandDeviceManager *device_manager = GDK_WAYLAND_DEVICE_MANAGER (device->device_manager);
  GdkWaylandDisplay *wayland_display = GDK_WAYLAND_DISPLAY (device->display);

  GDK_NOTE (MISC,
            g_message ("seat %p with %s%s%s", seat,
                       (caps & WL_SEAT_CAPABILITY_POINTER) ? " pointer, " : "",
                       (caps & WL_SEAT_CAPABILITY_KEYBOARD) ? " keyboard, " : "",
                       (caps & WL_SEAT_CAPABILITY_TOUCH) ? " touch" : ""));

  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !device->wl_pointer)
    {
      device->wl_pointer = wl_seat_get_pointer (seat);
      wl_pointer_set_user_data (device->wl_pointer, device);
      wl_pointer_add_listener (device->wl_pointer, &pointer_listener, device);

      device->pointer = g_object_new (GDK_TYPE_WAYLAND_DEVICE,
                                      "name", "Wayland Pointer",
                                      "type", GDK_DEVICE_TYPE_SLAVE,
                                      "input-source", GDK_SOURCE_MOUSE,
                                      "input-mode", GDK_MODE_SCREEN,
                                      "has-cursor", TRUE,
                                      "display", device->display,
                                      "device-manager", device->device_manager,
                                      "seat", device,
                                      NULL);
      _gdk_device_set_associated_device (device->pointer, device->master_pointer);
      GDK_WAYLAND_DEVICE (device->pointer)->device = device;

      device_manager->devices =
        g_list_prepend (device_manager->devices, device->pointer);

      if (wayland_display->pointer_gestures)
        {
          device->wp_pointer_gesture_swipe =
            zwp_pointer_gestures_v1_get_swipe_gesture (wayland_display->pointer_gestures,
                                                       device->wl_pointer);
          zwp_pointer_gesture_swipe_v1_set_user_data (device->wp_pointer_gesture_swipe,
                                                      device);
          zwp_pointer_gesture_swipe_v1_add_listener (device->wp_pointer_gesture_swipe,
                                                     &gesture_swipe_listener, device);

          device->wp_pointer_gesture_pinch =
            zwp_pointer_gestures_v1_get_pinch_gesture (wayland_display->pointer_gestures,
                                                       device->wl_pointer);
          zwp_pointer_gesture_pinch_v1_set_user_data (device->wp_pointer_gesture_pinch,
                                                      device);
          zwp_pointer_gesture_pinch_v1_add_listener (device->wp_pointer_gesture_pinch,
                                                     &gesture_pinch_listener, device);
        }

      g_signal_emit_by_name (device_manager, "device-added", device->pointer);
    }
  else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && device->wl_pointer)
    {
      wl_pointer_release (device->wl_pointer);
      device->wl_pointer = NULL;
      _gdk_device_set_associated_device (device->pointer, NULL);

      device_manager->devices =
        g_list_remove (device_manager->devices, device->pointer);

      g_signal_emit_by_name (device_manager, "device-removed", device->pointer);
      g_object_unref (device->pointer);
      device->pointer = NULL;
    }

  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !device->wl_keyboard)
    {
      device->wl_keyboard = wl_seat_get_keyboard (seat);
      wl_keyboard_set_user_data (device->wl_keyboard, device);
      wl_keyboard_add_listener (device->wl_keyboard, &keyboard_listener, device);

      device->keyboard = g_object_new (GDK_TYPE_WAYLAND_DEVICE,
                                       "name", "Wayland Keyboard",
                                       "type", GDK_DEVICE_TYPE_SLAVE,
                                       "input-source", GDK_SOURCE_KEYBOARD,
                                       "input-mode", GDK_MODE_SCREEN,
                                       "has-cursor", FALSE,
                                       "display", device->display,
                                       "device-manager", device->device_manager,
                                       "seat", device,
                                       NULL);
      _gdk_device_set_associated_device (device->keyboard, device->master_keyboard);
      GDK_WAYLAND_DEVICE (device->keyboard)->device = device;

      device_manager->devices =
        g_list_prepend (device_manager->devices, device->keyboard);

      g_signal_emit_by_name (device_manager, "device-added", device->keyboard);
    }
  else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && device->wl_keyboard)
    {
      wl_keyboard_release (device->wl_keyboard);
      device->wl_keyboard = NULL;
      _gdk_device_set_associated_device (device->keyboard, NULL);

      device_manager->devices =
        g_list_remove (device_manager->devices, device->keyboard);

      g_signal_emit_by_name (device_manager, "device-removed", device->keyboard);
      g_object_unref (device->keyboard);
      device->keyboard = NULL;
    }

  if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !device->wl_touch)
    {
      device->wl_touch = wl_seat_get_touch (seat);
      wl_touch_set_user_data (device->wl_touch, device);
      wl_touch_add_listener (device->wl_touch, &touch_listener, device);

      device->touch_master = g_object_new (GDK_TYPE_WAYLAND_DEVICE,
                                           "name", "Wayland Touch Master Pointer",
                                           "type", GDK_DEVICE_TYPE_MASTER,
                                           "input-source", GDK_SOURCE_MOUSE,
                                           "input-mode", GDK_MODE_SCREEN,
                                           "has-cursor", TRUE,
                                           "display", device->display,
                                           "device-manager", device->device_manager,
                                           "seat", device,
                                           NULL);
      GDK_WAYLAND_DEVICE (device->touch_master)->device = device;
      GDK_WAYLAND_DEVICE (device->touch_master)->pointer = &device->touch_info;
      _gdk_device_set_associated_device (device->touch_master, device->master_keyboard);

      device_manager->devices =
        g_list_prepend (device_manager->devices, device->touch_master);
      g_signal_emit_by_name (device_manager, "device-added", device->touch_master);

      device->touch = g_object_new (GDK_TYPE_WAYLAND_DEVICE,
                                    "name", "Wayland Touch",
                                    "type", GDK_DEVICE_TYPE_SLAVE,
                                    "input-source", GDK_SOURCE_TOUCHSCREEN,
                                    "input-mode", GDK_MODE_SCREEN,
                                    "has-cursor", FALSE,
                                    "display", device->display,
                                    "device-manager", device->device_manager,
                                    "seat", device,
                                    NULL);
      _gdk_device_set_associated_device (device->touch, device->touch_master);
      GDK_WAYLAND_DEVICE (device->touch)->device = device;

      device_manager->devices =
        g_list_prepend (device_manager->devices, device->touch);

      g_signal_emit_by_name (device_manager, "device-added", device->touch);
    }
  else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && device->wl_touch)
    {
      wl_touch_release (device->wl_touch);
      device->wl_touch = NULL;
      _gdk_device_set_associated_device (device->touch_master, NULL);
      _gdk_device_set_associated_device (device->touch, NULL);

      device_manager->devices =
        g_list_remove (device_manager->devices, device->touch_master);
      device_manager->devices =
        g_list_remove (device_manager->devices, device->touch);

      g_signal_emit_by_name (device_manager, "device-removed", device->touch_master);
      g_signal_emit_by_name (device_manager, "device-removed", device->touch);
      g_object_unref (device->touch_master);
      g_object_unref (device->touch);
      device->touch_master = NULL;
      device->touch = NULL;
    }

  if (device->master_pointer)
    gdk_drag_context_set_device (device->drop_context, device->master_pointer);
  else if (device->touch_master)
    gdk_drag_context_set_device (device->drop_context, device->touch_master);
}

static void
seat_handle_name (void           *data,
                  struct wl_seat *seat,
                  const char     *name)
{
  /* We don't care about the name. */
  GDK_NOTE (MISC,
            g_message ("seat %p name %s", seat, name));
}

static const struct wl_seat_listener seat_listener = {
  seat_handle_capabilities,
  seat_handle_name,
};

static void
tablet_tool_handle_type (void                      *data,
                         struct zwp_tablet_tool_v1 *wp_tablet_tool,
                         uint32_t                   tool_type)
{
  GdkWaylandTabletToolData *tool = data;

  switch (tool_type)
    {
    case ZWP_TABLET_TOOL_V1_TYPE_PEN:
      tool->type = GDK_DEVICE_TOOL_TYPE_PEN;
      break;
    case ZWP_TABLET_TOOL_V1_TYPE_BRUSH:
      tool->type = GDK_DEVICE_TOOL_TYPE_BRUSH;
      break;
    case ZWP_TABLET_TOOL_V1_TYPE_AIRBRUSH:
      tool->type = GDK_DEVICE_TOOL_TYPE_AIRBRUSH;
      break;
    case ZWP_TABLET_TOOL_V1_TYPE_PENCIL:
      tool->type = GDK_DEVICE_TOOL_TYPE_PENCIL;
      break;
    case ZWP_TABLET_TOOL_V1_TYPE_ERASER:
      tool->type = GDK_DEVICE_TOOL_TYPE_ERASER;
      break;
    default:
      tool->type = GDK_DEVICE_TOOL_TYPE_UNKNOWN;
      break;
    };
}

static void
tablet_tool_handle_hwserial (void                      *data,
                             struct zwp_tablet_tool_v1 *wp_tablet_tool,
                             uint32_t                   serial_hi,
                             uint32_t                   serial_lo)
{
  GdkWaylandTabletToolData *tool = data;

  tool->hwserial = ((guint64) serial_hi) << 32 | serial_lo;
}

static void
tablet_tool_handle_hardware_id (void *data,
                                struct zwp_tablet_tool_v1 *wp_tablet_tool,
                                uint32_t                   format,
                                uint32_t                   hwid_hi,
                                uint32_t                   hwid_lo)
{
  GdkWaylandTabletToolData *tool = data;

  /* FIXME: not handling format */
  tool->hwserial = ((guint64) hwid_hi) << 32 | hwid_lo;
}

static void
tablet_tool_handle_capability (void                      *data,
                               struct zwp_tablet_tool_v1 *wp_tablet_tool,
                               uint32_t                   capability)
{
  GdkWaylandTabletToolData *tool = data;

  switch (capability)
    {
    case ZWP_TABLET_TOOL_V1_CAPABILITY_TILT:
      tool->axes |= GDK_AXIS_FLAG_XTILT | GDK_AXIS_FLAG_YTILT;
      break;
    case ZWP_TABLET_TOOL_V1_CAPABILITY_PRESSURE:
      tool->axes |= GDK_AXIS_FLAG_PRESSURE;
      break;
    case ZWP_TABLET_TOOL_V1_CAPABILITY_DISTANCE:
      tool->axes |= GDK_AXIS_FLAG_DISTANCE;
      break;
    }
}

static void
tablet_tool_handle_done (void                      *data,
                         struct zwp_tablet_tool_v1 *wp_tablet_tool)
{
  GdkWaylandTabletToolData *tool = data;

  tool->tool = gdk_device_tool_new (tool->hwid, tool->type, tool->axes);
  gdk_seat_tool_added (tool->seat, tool->tool);
}

static void
tablet_tool_handle_removed (void                      *data,
                            struct zwp_tablet_tool_v1 *wp_tablet_tool)
{
  GdkWaylandTabletToolData *tool = data;

  _gdk_wayland_seat_remove_tool (GDK_WAYLAND_SEAT (tool->seat), tool);
}

static void
gdk_wayland_tablet_flush_frame_event (GdkWaylandTabletData *tablet,
                                      guint32               time)
{
  GdkEvent *event;

  event = tablet->pointer_info.frame.event;
  tablet->pointer_info.frame.event = NULL;

  if (!event)
    return;

  switch (event->type)
    {
    case GDK_MOTION_NOTIFY:
      event->motion.time = time;
      break;
    case GDK_BUTTON_PRESS:
    case GDK_BUTTON_RELEASE:
      event->button.time = time;
      break;
    case GDK_PROXIMITY_IN:
    case GDK_PROXIMITY_OUT:
      event->proximity.time = time;
      break;
    default:
      return;
    }

  if (event->type == GDK_PROXIMITY_OUT)
    emulate_crossing (event->proximity.window, NULL,
                      tablet->master, GDK_LEAVE_NOTIFY,
                      GDK_CROSSING_NORMAL, time);

  _gdk_wayland_display_deliver_event (gdk_seat_get_display (tablet->seat),
                                      event);

  if (event->type == GDK_PROXIMITY_IN)
    emulate_crossing (event->proximity.window, NULL,
                      tablet->master, GDK_ENTER_NOTIFY,
                      GDK_CROSSING_NORMAL, time);
}

static GdkEvent *
gdk_wayland_tablet_get_frame_event (GdkWaylandTabletData *tablet,
                                    GdkEventType          evtype)
{
  if (tablet->pointer_info.frame.event &&
      tablet->pointer_info.frame.event->type != evtype)
    gdk_wayland_tablet_flush_frame_event (tablet, GDK_CURRENT_TIME);

  tablet->pointer_info.frame.event = gdk_event_new (evtype);
  return tablet->pointer_info.frame.event;
}

static void
tablet_tool_handle_proximity_in (void                      *data,
                                 struct zwp_tablet_tool_v1 *wp_tablet_tool,
                                 uint32_t                   serial,
                                 struct zwp_tablet_v1      *wp_tablet,
                                 struct wl_surface         *surface)
{
  GdkWaylandTabletToolData *tool = data;
  GdkWaylandTabletData *tablet = zwp_tablet_v1_get_user_data (wp_tablet);
  GdkWaylandSeat *seat = GDK_WAYLAND_SEAT (tablet->seat);
  GdkWaylandDisplay *wayland_display = GDK_WAYLAND_DISPLAY (seat->display);
  GdkWindow *window = wl_surface_get_user_data (surface);
  GdkEvent *event;

  if (!surface)
      return;
  if (!GDK_IS_WINDOW (window))
      return;

  tool->current_tablet = tablet;
  tablet->current_tool = tool;

  _gdk_wayland_display_update_serial (wayland_display, serial);
  tablet->pointer_info.enter_serial = serial;

  tablet->pointer_info.focus = g_object_ref (window);
  tablet->current_device =
    tablet_select_device_for_tool (tablet, tool->tool);

  gdk_device_update_tool (tablet->current_device, tool->tool);

  event = gdk_wayland_tablet_get_frame_event (tablet, GDK_PROXIMITY_IN);
  event->proximity.window = g_object_ref (tablet->pointer_info.focus);
  gdk_event_set_device (event, tablet->master);
  gdk_event_set_source_device (event, tablet->current_device);

  GDK_NOTE (EVENTS,
            g_message ("proximity in, seat %p surface %p tool %d",
                       seat, tablet->pointer_info.focus,
                       gdk_device_tool_get_tool_type (tool->tool)));
}

static void
tablet_tool_handle_proximity_out (void                      *data,
                                  struct zwp_tablet_tool_v1 *wp_tablet_tool)
{
  GdkWaylandTabletToolData *tool = data;
  GdkWaylandTabletData *tablet = tool->current_tablet;
  GdkWaylandSeat *seat = GDK_WAYLAND_SEAT (tool->seat);
  GdkEvent *event;

  GDK_NOTE (EVENTS,
            g_message ("proximity out, seat %p, tool %d", seat,
                       gdk_device_tool_get_tool_type (tool->tool)));

  event = gdk_wayland_tablet_get_frame_event (tablet, GDK_PROXIMITY_OUT);
  event->proximity.window = g_object_ref (tablet->pointer_info.focus);
  gdk_event_set_device (event, tablet->master);
  gdk_event_set_source_device (event, tablet->current_device);

  gdk_wayland_pointer_stop_cursor_animation (&tablet->pointer_info);

  gdk_wayland_device_update_window_cursor (tablet->master);
  g_object_unref (tablet->pointer_info.focus);
  tablet->pointer_info.focus = NULL;
}

static void
tablet_tool_handle_motion (void                      *data,
                           struct zwp_tablet_tool_v1 *wp_tablet_tool,
                           wl_fixed_t                 sx,
                           wl_fixed_t                 sy)
{
  GdkWaylandTabletToolData *tool = data;
  GdkWaylandTabletData *tablet = tool->current_tablet;
  GdkWaylandSeat *seat = GDK_WAYLAND_SEAT (tool->seat);
  GdkWaylandDisplay *display = GDK_WAYLAND_DISPLAY (seat->display);
  GdkEvent *event;

  tablet->pointer_info.surface_x = wl_fixed_to_double (sx);
  tablet->pointer_info.surface_y = wl_fixed_to_double (sy);

  GDK_NOTE (EVENTS,
            g_message ("tablet motion %f %f",
                       tablet->pointer_info.surface_x,
                       tablet->pointer_info.surface_y));

  event = gdk_wayland_tablet_get_frame_event (tablet, GDK_MOTION_NOTIFY);
  event->motion.window = g_object_ref (tablet->pointer_info.focus);
  gdk_event_set_device (event, tablet->master);
  gdk_event_set_source_device (event, tablet->current_device);
  event->motion.time = tablet->pointer_info.time;
  event->motion.state = device_get_modifiers (tablet->master);
  event->motion.is_hint = FALSE;
  gdk_event_set_screen (event, display->screen);

  get_coordinates (tablet->master,
                   &event->motion.x,
                   &event->motion.y,
                   &event->motion.x_root,
                   &event->motion.y_root);
}

static void
tablet_tool_handle_frame (void                      *data,
                          struct zwp_tablet_tool_v1 *wl_tablet_tool,
                          uint32_t                   time)
{
  GdkWaylandTabletToolData *tool = data;
  GdkWaylandTabletData *tablet = tool->current_tablet;
  GdkEvent *frame_event;

  GDK_NOTE (EVENTS,
            g_message ("tablet frame, time %d", time));

  frame_event = tablet->pointer_info.frame.event;

  if (frame_event && frame_event->type == GDK_PROXIMITY_OUT)
    {
      tool->current_tablet = NULL;
      tablet->current_tool = NULL;
    }

  tablet->pointer_info.time = time;
  gdk_wayland_tablet_flush_frame_event (tablet, time);
}

static void
tablet_handler_placeholder ()
{
}

static const struct zwp_tablet_tool_v1_listener tablet_tool_listener = {
  tablet_tool_handle_type,
  tablet_tool_handle_hwserial,
  tablet_tool_handle_hardware_id,
  tablet_tool_handle_capability,
  tablet_tool_handle_done,
  tablet_tool_handle_removed,
  tablet_tool_handle_proximity_in,
  tablet_tool_handle_proximity_out,
  tablet_handler_placeholder, /* down */
  tablet_handler_placeholder, /* up */
  tablet_tool_handle_motion,
  tablet_handler_placeholder, /* pressure */
  tablet_handler_placeholder, /* distance */
  tablet_handler_placeholder, /* tilt */
  tablet_handler_placeholder, /* button_state */
  tablet_tool_handle_frame,
};

static void
tablet_seat_handle_tablet_added (void                      *data,
                                 struct zwp_tablet_seat_v1 *wp_tablet_seat,
                                 struct zwp_tablet_v1      *wp_tablet)
{
  GdkWaylandSeat *seat = data;
  GdkWaylandDisplay *wayland_display = GDK_WAYLAND_DISPLAY (seat->display);
  GdkWaylandTabletData *tablet;

  tablet = g_new0 (GdkWaylandTabletData, 1);
  tablet->seat = GDK_SEAT (seat);
  tablet->pointer_info.current_output_scale = 1;
  tablet->pointer_info.pointer_surface =
    wl_compositor_create_surface (wayland_display->compositor);
  tablet->wp_tablet = wp_tablet;

  seat->tablets = g_list_prepend (seat->tablets, tablet);

  zwp_tablet_v1_add_listener (wp_tablet, &tablet_listener, tablet);
  zwp_tablet_v1_set_user_data (wp_tablet, tablet);
}

static void
tablet_seat_handle_tool_added (void                      *data,
                               struct zwp_tablet_seat_v1 *wp_tablet_seat,
                               struct zwp_tablet_tool_v1 *wp_tablet_tool)
{
  GdkWaylandSeat *seat = data;
  GdkWaylandTabletToolData *tool;

  tool = g_new0 (GdkWaylandTabletToolData, 1);
  tool->wp_tablet_tool = wp_tablet_tool;
  tool->seat = GDK_SEAT (seat);

  zwp_tablet_tool_v1_add_listener (wp_tablet_tool, &tablet_tool_listener, tool);
  zwp_tablet_tool_v1_set_user_data (wp_tablet_tool, tool);

  seat->tablet_tools = g_list_prepend (seat->tablet_tools, tool);
}

static const struct zwp_tablet_seat_v1_listener tablet_seat_listener = {
  tablet_seat_handle_tablet_added,
  tablet_seat_handle_tool_added,
};

static void
init_devices (GdkWaylandDeviceData *device)
{
  GdkWaylandDeviceManager *device_manager = GDK_WAYLAND_DEVICE_MANAGER (device->device_manager);

  /* pointer */
  device->master_pointer = g_object_new (GDK_TYPE_WAYLAND_DEVICE,
                                         "name", "Core Pointer",
                                         "type", GDK_DEVICE_TYPE_MASTER,
                                         "input-source", GDK_SOURCE_MOUSE,
                                         "input-mode", GDK_MODE_SCREEN,
                                         "has-cursor", TRUE,
                                         "display", device->display,
                                         "device-manager", device_manager,
                                         "seat", device,
                                         NULL);
  GDK_WAYLAND_DEVICE (device->master_pointer)->device = device;
  GDK_WAYLAND_DEVICE (device->master_pointer)->pointer = &device->pointer_info;

  device_manager->devices =
    g_list_prepend (device_manager->devices, device->master_pointer);
  g_signal_emit_by_name (device_manager, "device-added", device->master_pointer);

  /* keyboard */
  device->master_keyboard = g_object_new (GDK_TYPE_WAYLAND_DEVICE,
                                          "name", "Core Keyboard",
                                          "type", GDK_DEVICE_TYPE_MASTER,
                                          "input-source", GDK_SOURCE_KEYBOARD,
                                          "input-mode", GDK_MODE_SCREEN,
                                          "has-cursor", FALSE,
                                          "display", device->display,
                                          "device-manager", device_manager,
                                          "seat", device,
                                          NULL);
  GDK_WAYLAND_DEVICE (device->master_keyboard)->device = device;

  device_manager->devices =
    g_list_prepend (device_manager->devices, device->master_keyboard);
  g_signal_emit_by_name (device_manager, "device-added", device->master_keyboard);

  /* link both */
  _gdk_device_set_associated_device (device->master_pointer, device->master_keyboard);
  _gdk_device_set_associated_device (device->master_keyboard, device->master_pointer);
}

static void
pointer_surface_update_scale (GdkDevice *device)
{
  GdkWaylandSeat *seat = GDK_WAYLAND_SEAT (gdk_device_get_seat (device));
  GdkWaylandPointerData *pointer = GDK_WAYLAND_DEVICE (device)->pointer;
  GdkWaylandDisplay *wayland_display = GDK_WAYLAND_DISPLAY (seat->display);
  guint32 scale;
  GSList *l;

  if (wayland_display->compositor_version < WL_SURFACE_HAS_BUFFER_SCALE)
    {
      /* We can't set the scale on this surface */
      return;
    }

  scale = 1;
  for (l = pointer->pointer_surface_outputs; l != NULL; l = l->next)
    {
      guint32 output_scale =
        _gdk_wayland_screen_get_output_scale (wayland_display->screen,
                                              l->data);
      scale = MAX (scale, output_scale);
    }

  pointer->current_output_scale = scale;

  if (pointer->cursor)
    _gdk_wayland_cursor_set_scale (pointer->cursor, scale);

  gdk_wayland_device_update_window_cursor (device);
}

static void
pointer_surface_enter (void              *data,
                       struct wl_surface *wl_surface,
                       struct wl_output  *output)

{
  GdkWaylandDeviceData *device = data;

  GDK_NOTE (EVENTS,
            g_message ("pointer surface of device %p entered output %p",
                       device, output));

  device->pointer_info.pointer_surface_outputs =
    g_slist_append (device->pointer_info.pointer_surface_outputs, output);

  pointer_surface_update_scale (device->master_pointer);
}

static void
pointer_surface_leave (void              *data,
                       struct wl_surface *wl_surface,
                       struct wl_output  *output)
{
  GdkWaylandDeviceData *device = data;

  GDK_NOTE (EVENTS,
            g_message ("pointer surface of device %p left output %p",
                       device, output));

  device->pointer_info.pointer_surface_outputs =
    g_slist_remove (device->pointer_info.pointer_surface_outputs, output);

  pointer_surface_update_scale (device->master_pointer);
}

static const struct wl_surface_listener pointer_surface_listener = {
  pointer_surface_enter,
  pointer_surface_leave
};

static GdkWindow *
create_foreign_dnd_window (GdkDisplay *display)
{
  GdkWindowAttr attrs;
  GdkScreen *screen;
  guint mask;

  screen = gdk_display_get_default_screen (display);

  attrs.x = attrs.y = 0;
  attrs.width = attrs.height = 1;
  attrs.wclass = GDK_INPUT_OUTPUT;
  attrs.window_type = GDK_WINDOW_TEMP;
  attrs.visual = gdk_screen_get_system_visual (screen);

  mask = GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL;

  return gdk_window_new (gdk_screen_get_root_window (screen), &attrs, mask);
}

static void
gdk_wayland_pointer_data_finalize (GdkWaylandPointerData *pointer)
{
  g_clear_object (&pointer->focus);
  g_clear_object (&pointer->cursor);
  wl_surface_destroy (pointer->pointer_surface);
  g_slist_free (pointer->pointer_surface_outputs);
}

static void
gdk_wayland_seat_finalize (GObject *object)
{
  GdkWaylandSeat *seat = GDK_WAYLAND_SEAT (object);
  GList *l;

  for (l = seat->tablet_tools; l != NULL; l = l->next)
    _gdk_wayland_seat_remove_tool (seat, l->data);

  for (l = seat->tablets; l != NULL; l = l->next)
    _gdk_wayland_seat_remove_tablet (seat, l->data);

  seat_handle_capabilities (seat, seat->wl_seat, 0);
  g_object_unref (seat->keymap);
  gdk_wayland_pointer_data_finalize (&seat->pointer_info);
  /* FIXME: destroy data_device */
  g_clear_object (&seat->keyboard_settings);
  g_clear_object (&seat->drop_context);
  g_hash_table_destroy (seat->touches);
  gdk_window_destroy (seat->foreign_dnd_window);
  zwp_tablet_seat_v1_destroy (seat->wp_tablet_seat);
  stop_key_repeat (seat);

  G_OBJECT_CLASS (gdk_wayland_seat_parent_class)->finalize (object);
}

static GdkSeatCapabilities
gdk_wayland_seat_get_capabilities (GdkSeat *seat)
{
  GdkWaylandSeat *wayland_seat = GDK_WAYLAND_SEAT (seat);
  GdkSeatCapabilities caps = 0;

  if (wayland_seat->master_pointer)
    caps |= GDK_SEAT_CAPABILITY_POINTER;
  if (wayland_seat->master_keyboard)
    caps |= GDK_SEAT_CAPABILITY_KEYBOARD;
  if (wayland_seat->touch_master)
    caps |= GDK_SEAT_CAPABILITY_TOUCH;

  return caps;
}

static void
gdk_wayland_seat_set_grab_window (GdkWaylandSeat *seat,
                                  GdkWindow      *window)
{
  if (seat->grab_window)
    {
      _gdk_wayland_window_set_grab_seat (seat->grab_window, NULL);
      g_object_remove_weak_pointer (G_OBJECT (seat->grab_window),
                                    (gpointer *) &seat->grab_window);
      seat->grab_window = NULL;
    }

  if (window)
    {
      seat->grab_window = window;
      g_object_add_weak_pointer (G_OBJECT (window),
                                 (gpointer *) &seat->grab_window);
      _gdk_wayland_window_set_grab_seat (window, GDK_SEAT (seat));
    }
}

static GdkGrabStatus
gdk_wayland_seat_grab (GdkSeat                *seat,
                       GdkWindow              *window,
                       GdkSeatCapabilities     capabilities,
                       gboolean                owner_events,
                       GdkCursor              *cursor,
                       const GdkEvent         *event,
                       GdkSeatGrabPrepareFunc  prepare_func,
                       gpointer                prepare_func_data)
{
  GdkWaylandSeat *wayland_seat = GDK_WAYLAND_SEAT (seat);
  guint32 evtime = event ? gdk_event_get_time (event) : GDK_CURRENT_TIME;
  GdkDisplay *display = gdk_seat_get_display (seat);
  GdkWindow *native;

  native = gdk_window_get_toplevel (window);

  while (native->window_type == GDK_WINDOW_OFFSCREEN)
    {
      native = gdk_offscreen_window_get_embedder (native);

      if (native == NULL ||
          (!_gdk_window_has_impl (native) &&
           !gdk_window_is_viewable (native)))
        return GDK_GRAB_NOT_VIEWABLE;

      native = gdk_window_get_toplevel (native);
    }

  if (native == NULL || GDK_WINDOW_DESTROYED (native))
    return GDK_GRAB_NOT_VIEWABLE;

  gdk_wayland_seat_set_grab_window (wayland_seat, window);
  wayland_seat->grab_time = evtime;

  if (prepare_func)
    (prepare_func) (seat, window, prepare_func_data);

  if (!gdk_window_is_visible (window))
    {
      gdk_wayland_seat_set_grab_window (wayland_seat, NULL);
      g_critical ("Window %p has not been made visible in GdkSeatGrabPrepareFunc",
                  window);
      return GDK_GRAB_NOT_VIEWABLE;
    }

  if (wayland_seat->master_pointer &&
      capabilities & GDK_SEAT_CAPABILITY_POINTER)
    {
      GdkWindow *prev_focus = gdk_wayland_device_get_focus (wayland_seat->master_pointer);

      if (prev_focus != window)
        device_emit_grab_crossing (wayland_seat->master_pointer, prev_focus,
                                   window, GDK_CROSSING_GRAB, evtime);

      _gdk_display_add_device_grab (display,
                                    wayland_seat->master_pointer,
                                    window,
                                    native,
                                    GDK_OWNERSHIP_NONE,
                                    owner_events,
                                    GDK_ALL_EVENTS_MASK,
                                    _gdk_display_get_next_serial (display),
                                    evtime,
                                    FALSE);

      gdk_wayland_seat_set_global_cursor (seat, cursor);
      g_set_object (&wayland_seat->cursor, cursor);
      gdk_wayland_device_update_window_cursor (wayland_seat->master_pointer);
    }

  if (wayland_seat->touch_master &&
      capabilities & GDK_SEAT_CAPABILITY_TOUCH)
    {
      GdkWindow *prev_focus = gdk_wayland_device_get_focus (wayland_seat->touch_master);

      if (prev_focus != window)
        device_emit_grab_crossing (wayland_seat->touch_master, prev_focus,
                                   window, GDK_CROSSING_GRAB, evtime);

      _gdk_display_add_device_grab (display,
                                    wayland_seat->touch_master,
                                    window,
                                    native,
                                    GDK_OWNERSHIP_NONE,
                                    owner_events,
                                    GDK_ALL_EVENTS_MASK,
                                    _gdk_display_get_next_serial (display),
                                    evtime,
                                    FALSE);
    }

  if (wayland_seat->master_keyboard &&
      capabilities & GDK_SEAT_CAPABILITY_KEYBOARD)
    {
      GdkWindow *prev_focus = gdk_wayland_device_get_focus (wayland_seat->master_keyboard);

      if (prev_focus != window)
        device_emit_grab_crossing (wayland_seat->master_keyboard, prev_focus,
                                   window, GDK_CROSSING_GRAB, evtime);

      _gdk_display_add_device_grab (display,
                                    wayland_seat->master_keyboard,
                                    window,
                                    native,
                                    GDK_OWNERSHIP_NONE,
                                    owner_events,
                                    GDK_ALL_EVENTS_MASK,
                                    _gdk_display_get_next_serial (display),
                                    evtime,
                                    FALSE);
    }

  return GDK_GRAB_SUCCESS;
}

static void
gdk_wayland_seat_ungrab (GdkSeat *seat)
{
  GdkWaylandSeat *wayland_seat = GDK_WAYLAND_SEAT (seat);
  GdkDisplay *display = gdk_seat_get_display (seat);
  GdkDeviceGrabInfo *grab;

  g_clear_object (&wayland_seat->grab_cursor);

  gdk_wayland_seat_set_grab_window (wayland_seat, NULL);

  if (wayland_seat->master_pointer)
    {
      GdkWindow *focus, *prev_focus = NULL;

      grab = _gdk_display_get_last_device_grab (display, wayland_seat->master_pointer);

      if (grab)
        {
          grab->serial_end = grab->serial_start;
          prev_focus = grab->window;
        }

      focus = gdk_wayland_device_get_focus (wayland_seat->master_pointer);

      if (focus != prev_focus)
        device_emit_grab_crossing (wayland_seat->master_pointer, prev_focus,
                                   focus, GDK_CROSSING_UNGRAB,
                                   GDK_CURRENT_TIME);

      gdk_wayland_device_update_window_cursor (wayland_seat->master_pointer);
    }

  if (wayland_seat->master_keyboard)
    {
      grab = _gdk_display_get_last_device_grab (display, wayland_seat->master_keyboard);

      if (grab)
        grab->serial_end = grab->serial_start;
    }

  if (wayland_seat->touch_master)
    {
      grab = _gdk_display_get_last_device_grab (display, wayland_seat->touch_master);

      if (grab)
        grab->serial_end = grab->serial_start;
    }
}

static GdkDevice *
gdk_wayland_seat_get_master (GdkSeat             *seat,
                             GdkSeatCapabilities  capabilities)
{
  GdkWaylandSeat *wayland_seat = GDK_WAYLAND_SEAT (seat);

  if (capabilities == GDK_SEAT_CAPABILITY_POINTER)
    return wayland_seat->master_pointer;
  else if (capabilities == GDK_SEAT_CAPABILITY_KEYBOARD)
    return wayland_seat->master_keyboard;
  else if (capabilities == GDK_SEAT_CAPABILITY_TOUCH)
    return wayland_seat->touch_master;

  return NULL;
}

static GList *
gdk_wayland_seat_get_slaves (GdkSeat             *seat,
                             GdkSeatCapabilities  capabilities)
{
  GdkWaylandSeat *wayland_seat = GDK_WAYLAND_SEAT (seat);
  GList *slaves = NULL;

  if (capabilities & GDK_SEAT_CAPABILITY_POINTER)
    slaves = g_list_prepend (slaves, wayland_seat->pointer);
  if (capabilities & GDK_SEAT_CAPABILITY_KEYBOARD)
    slaves = g_list_prepend (slaves, wayland_seat->keyboard);
  if (capabilities & GDK_SEAT_CAPABILITY_TOUCH)
    slaves = g_list_prepend (slaves, wayland_seat->touch);

  return slaves;
}

static void
gdk_wayland_seat_class_init (GdkWaylandSeatClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GdkSeatClass *seat_class = GDK_SEAT_CLASS (klass);

  object_class->finalize = gdk_wayland_seat_finalize;

  seat_class->get_capabilities = gdk_wayland_seat_get_capabilities;
  seat_class->grab = gdk_wayland_seat_grab;
  seat_class->ungrab = gdk_wayland_seat_ungrab;
  seat_class->get_master = gdk_wayland_seat_get_master;
  seat_class->get_slaves = gdk_wayland_seat_get_slaves;
}

static void
gdk_wayland_seat_init (GdkWaylandSeat *seat)
{
}

void
_gdk_wayland_device_manager_add_seat (GdkDeviceManager *device_manager,
                                      guint32           id,
				      struct wl_seat   *wl_seat)
{
  GdkDisplay *display;
  GdkWaylandDisplay *display_wayland;
  GdkWaylandSeat *seat;

  display = gdk_device_manager_get_display (device_manager);
  display_wayland = GDK_WAYLAND_DISPLAY (display);

  seat = g_object_new (GDK_TYPE_WAYLAND_SEAT,
                       "display", display,
                       NULL);
  seat->id = id;
  seat->keymap = _gdk_wayland_keymap_new ();
  seat->display = display;
  seat->device_manager = device_manager;
  seat->touches = g_hash_table_new_full (NULL, NULL, NULL,
                                         (GDestroyNotify) g_free);
  seat->foreign_dnd_window = create_foreign_dnd_window (display);
  seat->wl_seat = wl_seat;

  seat->pending_selection = GDK_NONE;

  wl_seat_add_listener (seat->wl_seat, &seat_listener, seat);
  wl_seat_set_user_data (seat->wl_seat, seat);

  seat->data_device =
    wl_data_device_manager_get_data_device (display_wayland->data_device_manager,
                                            seat->wl_seat);
  seat->drop_context = _gdk_wayland_drop_context_new (seat->data_device);
  wl_data_device_add_listener (seat->data_device,
                               &data_device_listener, seat);

  seat->pointer_info.current_output_scale = 1;
  seat->pointer_info.pointer_surface =
    wl_compositor_create_surface (display_wayland->compositor);
  wl_surface_add_listener (seat->pointer_info.pointer_surface,
                           &pointer_surface_listener,
                           seat);

  init_devices (seat);

  seat->wp_tablet_seat =
    zwp_tablet_manager_v1_get_tablet_seat (display_wayland->tablet_manager,
                                           wl_seat);
  zwp_tablet_seat_v1_add_listener (seat->wp_tablet_seat, &tablet_seat_listener,
                                   seat);

  gdk_display_add_seat (display, GDK_SEAT (seat));
}

void
_gdk_wayland_device_manager_remove_seat (GdkDeviceManager *manager,
                                         guint32           id)
{
  GdkDisplay *display = gdk_device_manager_get_display (manager);
  GList *l, *seats;

  seats = gdk_display_list_seats (display);

  for (l = seats; l != NULL; l = l->next)
    {
      GdkWaylandSeat *seat = l->data;

      if (seat->id != id)
        continue;

      gdk_display_remove_seat (display, GDK_SEAT (seat));
      break;
    }

  g_list_free (seats);
}

static void
free_device (gpointer data)
{
  g_object_unref (data);
}

static void
gdk_wayland_device_manager_finalize (GObject *object)
{
  GdkWaylandDeviceManager *device_manager;

  device_manager = GDK_WAYLAND_DEVICE_MANAGER (object);

  g_list_free_full (device_manager->devices, free_device);

  G_OBJECT_CLASS (gdk_wayland_device_manager_parent_class)->finalize (object);
}

static GList *
gdk_wayland_device_manager_list_devices (GdkDeviceManager *device_manager,
                                         GdkDeviceType     type)
{
  GdkWaylandDeviceManager *wayland_device_manager;
  GList *devices = NULL, *l;

  wayland_device_manager = GDK_WAYLAND_DEVICE_MANAGER (device_manager);

  for (l = wayland_device_manager->devices; l; l = l->next)
    {
      if (gdk_device_get_device_type (l->data) == type)
        devices = g_list_prepend (devices, l->data);
    }

  return devices;
}

static GdkDevice *
gdk_wayland_device_manager_get_client_pointer (GdkDeviceManager *device_manager)
{
  GdkWaylandDeviceManager *wayland_device_manager;
  GdkWaylandDeviceData *wd;
  GdkDevice *device;

  wayland_device_manager = GDK_WAYLAND_DEVICE_MANAGER (device_manager);

  /* Find the master pointer of the first GdkWaylandDeviceData we find */
  device = wayland_device_manager->devices->data;
  wd = GDK_WAYLAND_DEVICE (device)->device;

  return wd->master_pointer;
}

static void
gdk_wayland_device_manager_class_init (GdkWaylandDeviceManagerClass *klass)
{
  GdkDeviceManagerClass *device_manager_class = GDK_DEVICE_MANAGER_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gdk_wayland_device_manager_finalize;
  device_manager_class->list_devices = gdk_wayland_device_manager_list_devices;
  device_manager_class->get_client_pointer = gdk_wayland_device_manager_get_client_pointer;
}

static void
gdk_wayland_device_manager_init (GdkWaylandDeviceManager *device_manager)
{
}

GdkDeviceManager *
_gdk_wayland_device_manager_new (GdkDisplay *display)
{
  return g_object_new (GDK_TYPE_WAYLAND_DEVICE_MANAGER,
                       "display", display,
                       NULL);
}

uint32_t
_gdk_wayland_device_get_implicit_grab_serial (GdkWaylandDevice *device,
                                              const GdkEvent   *event)
{
  GdkEventSequence *sequence = NULL;
  GdkWaylandTouchData *touch = NULL;

  if (event)
    sequence = gdk_event_get_event_sequence (event);

  if (sequence)
    touch = gdk_wayland_device_get_touch (device->device,
                                          GDK_EVENT_SEQUENCE_TO_SLOT (sequence));
  if (touch)
    return touch->touch_down_serial;
  else
    return device->pointer->press_serial;
}

uint32_t
_gdk_wayland_device_get_last_implicit_grab_serial (GdkWaylandDevice  *device,
                                                   GdkEventSequence **sequence)
{
  GdkWaylandTouchData *touch;
  GHashTableIter iter;
  uint32_t serial = 0;

  g_hash_table_iter_init (&iter, device->device->touches);

  if (sequence)
    *sequence = NULL;

  if (device->pointer->press_serial > serial)
    serial = device->pointer->press_serial;

  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &touch))
    {
      if (touch->touch_down_serial > serial)
        {
          if (sequence)
            *sequence = GDK_SLOT_TO_EVENT_SEQUENCE (touch->id);
          serial = touch->touch_down_serial;
        }
    }

  return serial;
}

void
gdk_wayland_device_unset_touch_grab (GdkDevice        *gdk_device,
                                     GdkEventSequence *sequence)
{
  GdkWaylandDeviceData *device;
  GdkWaylandTouchData *touch;
  GdkEvent *event;

  g_return_if_fail (GDK_IS_WAYLAND_DEVICE (gdk_device));

  device = GDK_WAYLAND_DEVICE (gdk_device)->device;
  touch = gdk_wayland_device_get_touch (device,
                                        GDK_EVENT_SEQUENCE_TO_SLOT (sequence));

  if (GDK_WAYLAND_DEVICE (device->touch_master)->emulating_touch == touch)
    {
      GDK_WAYLAND_DEVICE (device->touch_master)->emulating_touch = NULL;
      emulate_touch_crossing (touch->window, NULL,
                              device->touch_master, device->touch,
                              touch, GDK_LEAVE_NOTIFY, GDK_CROSSING_NORMAL,
                              GDK_CURRENT_TIME);
    }

  event = _create_touch_event (device, touch, GDK_TOUCH_CANCEL,
                               GDK_CURRENT_TIME);
  _gdk_wayland_display_deliver_event (device->display, event);
}

void
gdk_wayland_seat_set_global_cursor (GdkSeat   *seat,
                                    GdkCursor *cursor)
{
  GdkWaylandSeat *wayland_seat = GDK_WAYLAND_SEAT (seat);
  GdkDevice *pointer;

  pointer = gdk_seat_get_pointer (seat);

  g_set_object (&wayland_seat->grab_cursor, cursor);
  gdk_wayland_device_set_window_cursor (pointer,
                                        gdk_wayland_device_get_focus (pointer),
                                        NULL);
}

struct wl_data_device *
gdk_wayland_device_get_data_device (GdkDevice *gdk_device)
{
  g_return_val_if_fail (GDK_IS_WAYLAND_DEVICE (gdk_device), NULL);

  return GDK_WAYLAND_DEVICE (gdk_device)->device->data_device;
}

void
gdk_wayland_device_set_selection (GdkDevice             *gdk_device,
                                  struct wl_data_source *source)
{
  GdkWaylandDeviceData *device;
  GdkWaylandDisplay *display_wayland;

  g_return_if_fail (GDK_IS_WAYLAND_DEVICE (gdk_device));

  device = GDK_WAYLAND_DEVICE (gdk_device)->device;
  display_wayland = GDK_WAYLAND_DISPLAY (gdk_device_get_display (gdk_device));

  wl_data_device_set_selection (device->data_device, source,
                                _gdk_wayland_display_get_serial (display_wayland));
}

struct wl_seat *
gdk_wayland_seat_get_wl_seat (GdkSeat *seat)
{
  g_return_val_if_fail (GDK_IS_WAYLAND_SEAT (seat), NULL);

  return GDK_WAYLAND_SEAT (seat)->wl_seat;
}

GdkDragContext *
gdk_wayland_device_get_drop_context (GdkDevice *device)
{
  GdkSeat *seat = gdk_device_get_seat (device);

  return GDK_WAYLAND_SEAT (seat)->drop_context;
}
