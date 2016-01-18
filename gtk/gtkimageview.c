#include "config.h"
#include "gtkimageview.h"
#include "gtktypebuiltins.h"
#include "gtkintl.h"
#include "gtkprivate.h"
#include "gtkrender.h"
#include "gtkgesture.h"
#include "gtkgesturerotate.h"
#include "gtkgesturezoom.h"
#include "gtkscrollable.h"
#include "gtkmarshalers.h"
#include "gtkadjustment.h"
#include <gdk/gdkcairo.h>

#include <cairo-gobject.h>
#include <math.h>

#define DEG_TO_RAD(x) (((x) / 360.0) * (2 * M_PI))
#define RAD_TO_DEG(x) (((x) / (2.0 * M_PI) * 360.0))

#define TRANSITION_DURATION (150.0 * 1000.0)

typedef struct
{
  double hupper;
  double vupper;
  double hvalue;
  double vvalue;
  double angle;
  double scale;
} State;


// XXX Add gtk_image_reset_view that transitions back to scale = 1 and angle = 0.


struct _GtkImageViewPrivate
{
  double   scale;
  double   angle;
  int      scale_factor;

  gboolean fit_allocation         : 1;
  gboolean scale_set              : 1;
  gboolean snap_angle             : 1;
  gboolean rotate_gesture_enabled : 1;
  gboolean zoom_gesture_enabled   : 1;
  gboolean in_rotate              : 1;
  gboolean in_zoom                : 1;
  gboolean size_valid             : 1;

  GtkGesture *rotate_gesture;
  double      gesture_start_angle;

  GtkGesture *zoom_gesture;
  double      gesture_start_scale;

  /* Current anchor point, or -1/-1.
   * In widget coordinates. */
  double      anchor_x;
  double      anchor_y;


  GdkWindow *event_window;

  /* GtkScrollable stuff */
  GtkAdjustment       *hadjustment;
  GtkAdjustment       *vadjustment;
  GtkScrollablePolicy  hscroll_policy;
  GtkScrollablePolicy  vscroll_policy;

  gboolean                is_animation;
  GdkPixbufAnimation     *source_animation;
  GdkPixbufAnimationIter *source_animation_iter;
  cairo_surface_t        *image_surface;
  int                     animation_timeout;

  /* Transitions */
  gint64 angle_transition_start;
  double transition_start_angle;
  double transition_end_angle;

  double cached_width;
  double cached_height;
  double cached_scale;
};
// XXX Actually honour the scroll policies

enum
{
  PROP_SCALE = 1,
  PROP_SCALE_SET,
  PROP_ANGLE,
  PROP_ROTATE_GESTURE_ENABLED,
  PROP_ZOOM_GESTURE_ENABLED,
  PROP_SNAP_ANGLE,
  PROP_FIT_ALLOCATION,

  LAST_WIDGET_PROPERTY,
  PROP_HADJUSTMENT,
  PROP_VADJUSTMENT,
  PROP_HSCROLL_POLICY,
  PROP_VSCROLL_POLICY,

  LAST_PROPERTY
};

static GParamSpec *widget_props[LAST_WIDGET_PROPERTY] = { NULL, };


G_DEFINE_TYPE_WITH_CODE (GtkImageView, gtk_image_view, GTK_TYPE_WIDGET,
                         G_ADD_PRIVATE (GtkImageView)
                         G_IMPLEMENT_INTERFACE (GTK_TYPE_SCROLLABLE, NULL))

typedef struct _LoadTaskData LoadTaskData;

struct _LoadTaskData
{
  int scale_factor;
  gpointer source;
};


/* Prototypes {{{ */
static void gtk_image_view_update_surface (GtkImageView    *image_view,
                                           const GdkPixbuf *frame,
                                           int              scale_factor);

static void adjustment_value_changed_cb (GtkAdjustment *adjustment,
                                         gpointer       user_data);

static void gtk_image_view_update_adjustments (GtkImageView *image_view);

static void gtk_image_view_compute_bounding_box (GtkImageView *image_view,
                                                 double          *width,
                                                 double          *height,
                                                 double       *scale_out);

static inline void gtk_image_view_restrict_adjustment (GtkAdjustment *adjustment);
/* }}} */




static void
gtk_image_view_get_current_state (GtkImageView *image_view,
                                  State        *state)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  if (priv->hadjustment != NULL && priv->vadjustment != NULL)
    {
      state->hvalue = gtk_adjustment_get_value (priv->hadjustment);
      state->vvalue = gtk_adjustment_get_value (priv->vadjustment);
      state->hupper = gtk_adjustment_get_upper (priv->hadjustment);
      state->vupper = gtk_adjustment_get_upper (priv->vadjustment);
    }
  state->angle  = priv->angle;
  state->scale  = priv->scale;
}

static gchar *
state_str (State *s)
{
  gchar *str = g_strdup_printf ("(Angle: %f, Scale: %f, hvalue: %f, vvalue: %f, hupper: %f, vupper: %f)",
                                s->angle, s->scale, s->hvalue, s->vvalue, s->hupper, s->vupper);
  return str;
}




static void
free_load_task_data (LoadTaskData *data)
{
  g_clear_object (&data->source);
}

  /* XXX What if the image is rotated by 45deg and the user presses outside of it?
   *     I.e. the anchor point would lie outside of the image? */


static void
to_rotate_coords (GtkImageView *image_view,
                  State *state,
                  double  in_x,  double  in_y,
                  double *out_x, double *out_y)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  g_message ("hupper: %f", gtk_adjustment_get_upper (priv->hadjustment));
  g_message ("hvalue: %f", gtk_adjustment_get_value (priv->hadjustment));

  double cx = state->hupper / 2.0 - state->hvalue;
  double cy = state->vupper / 2.0 - state->vvalue;

  *out_x = in_x - cx;
  *out_y = in_y - cy;
}


static void
gtk_image_view_fix_anchor (GtkImageView *image_view,
                           double        anchor_x,
                           double        anchor_y,
                           State        *old_state)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  double hupper_delta = gtk_adjustment_get_upper (priv->hadjustment) - old_state->hupper;
  double vupper_delta = gtk_adjustment_get_upper (priv->vadjustment) - old_state->vupper;


  g_assert (priv->hadjustment);
  g_assert (priv->vadjustment);
  g_assert (priv->size_valid);
  g_assert (anchor_x >= 0);
  g_assert (anchor_y >= 0);
  g_assert (anchor_x < gtk_widget_get_allocated_width (GTK_WIDGET (image_view)));
  g_assert (anchor_y < gtk_widget_get_allocated_height (GTK_WIDGET (image_view)));



  g_message ("Old State: %s", state_str (old_state));
  g_message ("New angle: %f", priv->angle);
  g_message ("New scale: %f", priv->scale);

  g_message ("Anchor: %f/%f", priv->anchor_x, priv->anchor_y);
  g_message ("hupper_delta: %f", hupper_delta);
  g_message ("vupper_delta: %f", vupper_delta);




  double hupper = gtk_adjustment_get_upper (priv->hadjustment);
  double hdiff_scale = (old_state->hupper / old_state->scale) * priv->scale;

  double hd = hdiff_scale - old_state->hupper;

  g_message ("hupper diff: %f", (hupper - old_state->hupper));
  g_message ("scale_diff: %f", hd);
  g_message ("angle_diff: %f", hupper_delta - hd);


  /* Amount of upper change caused by scale */
  double hupper_delta_scale = ((old_state->hupper / old_state->scale) * priv->scale)
                              - old_state->hupper;
  double vupper_delta_scale = ((old_state->vupper / old_state->scale) * priv->scale)
                              - old_state->vupper;

  /* Amount of upper change caused by angle */
  double hupper_delta_angle = hupper_delta - hupper_delta_scale;
  double vupper_delta_angle = vupper_delta - vupper_delta_scale;




  /* As a first step, fix the anchor point with regard to the
   * updated scale
   */
  {
    double hvalue = gtk_adjustment_get_value (priv->hadjustment);
    double vvalue = gtk_adjustment_get_value (priv->vadjustment);

    double px = anchor_x + hvalue;
    double py = anchor_y + vvalue;

    double px_after = (px / old_state->scale) * priv->scale;
    double py_after = (py / old_state->scale) * priv->scale;

    gtk_adjustment_set_value (priv->hadjustment,
                              hvalue + px_after - px);
    gtk_adjustment_set_value (priv->vadjustment,
                              vvalue + py_after - py);
  }



  gtk_adjustment_set_value (priv->hadjustment,
                            gtk_adjustment_get_value (priv->hadjustment) + hupper_delta_angle / 2.0);

  gtk_adjustment_set_value (priv->vadjustment,
                            gtk_adjustment_get_value (priv->vadjustment) + vupper_delta_angle / 2.0);




  double rotate_anchor_x = 0;
  double rotate_anchor_y = 0;

  to_rotate_coords (image_view, old_state,
                    anchor_x, anchor_y,
                    &rotate_anchor_x, &rotate_anchor_y);

  g_message ("Rotate anchor coords: %f/%f", rotate_anchor_x, rotate_anchor_y);

  /* 1) Calculate the angle of our anchor point. */
  double anchor_angle = atan2 (rotate_anchor_y, rotate_anchor_x);


  double anchor_length = sqrt ((rotate_anchor_x * rotate_anchor_x) +
                               (rotate_anchor_y * rotate_anchor_y));
  g_message ("Anchor angle: %f", RAD_TO_DEG (anchor_angle));
  g_message ("Anchor length: %f", anchor_length);

  /* 2) Calculate the position of our anchor point with increased angle */
  double angle_diff = priv->angle - old_state->angle;
  anchor_angle += DEG_TO_RAD (angle_diff);

  g_message ("anchor angle after: %f", RAD_TO_DEG (anchor_angle));


  g_message ("Angle got increased by %f", angle_diff);
  double scale_diff = priv->scale - old_state->scale;
  g_message ("Scale got increased by %f", scale_diff);
  double new_anchor_x = cos (anchor_angle) * anchor_length;
  double new_anchor_y = sin (anchor_angle) * anchor_length;

  g_message ("New anchor: %f, %f", new_anchor_x, new_anchor_y);


  double diff_x = rotate_anchor_x - new_anchor_x;
  double diff_y = rotate_anchor_y - new_anchor_y;

  g_message ("Diff: %f/%f", diff_x, diff_y);

  gtk_adjustment_set_value (priv->hadjustment,
                            gtk_adjustment_get_value (priv->hadjustment) - diff_x);


  gtk_adjustment_set_value (priv->vadjustment,
                            gtk_adjustment_get_value (priv->vadjustment) - diff_y);


  g_message ("-------------------------");
  gtk_widget_queue_draw (GTK_WIDGET (image_view));
}

static void
gesture_rotate_end_cb (GtkGesture       *gesture,
                       GdkEventSequence *sequence,
                       gpointer          user_data)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (user_data);

  priv->gesture_start_angle = 0.0;
  priv->in_rotate = FALSE;

  priv->anchor_x = -1;
  priv->anchor_y = -1;
}

static void
gesture_rotate_cancel_cb (GtkGesture       *gesture,
                          GdkEventSequence *sequence,
                          gpointer          user_data)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (user_data);
  gtk_image_view_set_angle (user_data, priv->gesture_start_angle);
  priv->in_rotate = FALSE;
  priv->gesture_start_angle = 0.0;

  priv->anchor_x = -1;
  priv->anchor_y = -1;
}


static void
gesture_angle_changed_cb (GtkGestureRotate *gesture,
                          double            angle,
                          double            delta,
                          GtkWidget        *widget)
{
  GtkImageView *image_view = GTK_IMAGE_VIEW (widget);
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  State old_state;
  double new_angle;

  if (!priv->in_rotate)
    {
      priv->in_rotate = TRUE;
      priv->gesture_start_angle = priv->angle;
    }


  new_angle = priv->gesture_start_angle + RAD_TO_DEG (delta);

  if (new_angle == priv->angle)
    return;


  gtk_image_view_get_current_state (image_view, &old_state);

  /* Don't notify */
  priv->angle = new_angle;
  priv->size_valid = FALSE;
  gtk_image_view_update_adjustments (image_view);

  if (priv->hadjustment && priv->vadjustment)
    gtk_image_view_fix_anchor (image_view,
                                      priv->anchor_x,
                                      priv->anchor_y,
                                      &old_state);

  // XXX Even if fit_allocation is not set, we still don't need to query a resize
  //     if we are in a scrolledwindow, right?
  if (priv->fit_allocation)
    gtk_widget_queue_draw (widget);
  else
    gtk_widget_queue_resize (widget);
}

static void
gtk_image_view_compute_bounding_box (GtkImageView *image_view,
                                     double       *width,
                                     double       *height,
                                     double       *scale_out)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  GtkAllocation alloc;
  double image_width;
  double image_height;
  double bb_width  = 0;
  double bb_height = 0;
  double upper_right_degrees;
  double upper_left_degrees;
  double r;
  double upper_right_x, upper_right_y;
  double upper_left_x, upper_left_y;
  double scale;
  if (priv->size_valid)
    {
      *width = priv->cached_width;
      *height = priv->cached_height;
      if (scale_out)
        *scale_out = priv->cached_scale;
      return;
    }


  if (!priv->image_surface)
    {
      *width  = 0;
      *height = 0;
      return;
    }

  gtk_widget_get_allocation (GTK_WIDGET (image_view), &alloc);

  image_width  = cairo_image_surface_get_width (priv->image_surface);
  image_height = cairo_image_surface_get_height (priv->image_surface);

  upper_right_degrees = DEG_TO_RAD (priv->angle) + atan (image_height / image_width);
  upper_left_degrees  = DEG_TO_RAD (priv->angle) + atan (image_height / -image_width);
  r = sqrt ((image_width / 2.0) * (image_width / 2.0) + (image_height / 2.0) * (image_height / 2.0));

  upper_right_x = r * cos (upper_right_degrees);
  upper_right_y = r * sin (upper_right_degrees);

  upper_left_x = r * cos (upper_left_degrees);
  upper_left_y = r * sin (upper_left_degrees);

  bb_width  = round (MAX (fabs (upper_right_x), fabs (upper_left_x)) * 2.0);
  bb_height = round (MAX (fabs (upper_right_y), fabs (upper_left_y)) * 2.0);

  if (priv->scale_set)
    {
      scale = priv->scale;
    }
  else
    {
      if (priv->fit_allocation)
        {
          double scale_x = (double)alloc.width / (double)bb_width;
          double scale_y = (double)alloc.height / (double)bb_height;

          scale = MIN (MIN (scale_x, scale_y), 1.0);
        }
      else
        {
          scale = 1.0;
        }
    }

  priv->cached_scale = scale;
  if (scale_out)
    *scale_out = scale;

  if (priv->fit_allocation)
    {
      priv->scale = scale;
      g_object_notify_by_pspec (G_OBJECT (image_view),
                                widget_props[PROP_SCALE]);
    }

  *width  = priv->cached_width  = bb_width  * scale;
  *height = priv->cached_height = bb_height * scale;

  priv->size_valid = TRUE;
}


static inline void
gtk_image_view_restrict_adjustment (GtkAdjustment *adjustment)
{
  double value     = gtk_adjustment_get_value (adjustment);
  double upper     = gtk_adjustment_get_upper (adjustment);
  double page_size = gtk_adjustment_get_page_size (adjustment);

  value = gtk_adjustment_get_value (adjustment);
  upper = gtk_adjustment_get_upper (adjustment);

  if (value > upper - page_size)
    gtk_adjustment_set_value (adjustment, upper - page_size);
  else if (value < 0)
    gtk_adjustment_set_value (adjustment, 0);
}

static void
gtk_image_view_update_adjustments (GtkImageView *image_view)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  int widget_width  = gtk_widget_get_allocated_width  (GTK_WIDGET (image_view));
  int widget_height = gtk_widget_get_allocated_height (GTK_WIDGET (image_view));


  if (!priv->hadjustment && !priv->vadjustment)
    return;

  if (!priv->image_surface)
    {
      if (priv->hadjustment)
        gtk_adjustment_configure (priv->hadjustment, 0, 0, 1, 0, 0, 1);

      if (priv->vadjustment)
        gtk_adjustment_configure (priv->vadjustment, 0, 0, 1, 0, 0, 1);

      return;
    }


  if (priv->fit_allocation)
    {
      if (priv->hadjustment)
        gtk_adjustment_set_upper (priv->hadjustment, widget_width);

      if (priv->vadjustment)
        gtk_adjustment_set_upper (priv->vadjustment, widget_height);
    }
  else
    {
      double width, height;
      gtk_image_view_compute_bounding_box (image_view,
                                           &width,
                                           &height,
                                           NULL);

      if (priv->hadjustment)
        gtk_adjustment_set_upper (priv->hadjustment, MAX (width,  widget_width));

      if (priv->vadjustment)
        gtk_adjustment_set_upper (priv->vadjustment, MAX (height, widget_height));
    }


  if (priv->hadjustment)
    {
      gtk_adjustment_set_page_size (priv->hadjustment, widget_width);
      gtk_image_view_restrict_adjustment (priv->hadjustment);
    }

  if (priv->vadjustment)
    {
      gtk_adjustment_set_page_size (priv->vadjustment, widget_height);
      gtk_image_view_restrict_adjustment (priv->vadjustment);
    }
}




/*
 * This is basically the normal _set_scale without the
 * _fix_anchor call at the end, so we can choose the point
 * to fix.
 */
static void
gtk_image_view_set_scale_internal (GtkImageView *image_view,
                                   double        scale)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  scale = MAX (0, scale);

  priv->scale = scale;
  priv->size_valid = FALSE;
  g_object_notify_by_pspec (G_OBJECT (image_view),
                            widget_props[PROP_SCALE]);


  if (!priv->scale_set)
    {
      priv->scale_set = TRUE;
      g_object_notify_by_pspec (G_OBJECT (image_view),
                                widget_props[PROP_SCALE_SET]);
    }

  if (priv->fit_allocation)
    {
      priv->fit_allocation = FALSE;
      g_object_notify_by_pspec (G_OBJECT (image_view),
                                widget_props[PROP_FIT_ALLOCATION]);
    }


  gtk_image_view_update_adjustments (image_view);

  gtk_widget_queue_resize (GTK_WIDGET (image_view));
}

static void
gesture_zoom_end_cb (GtkGesture       *gesture,
                     GdkEventSequence *sequence,
                     gpointer          user_data)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (user_data);

  gtk_image_view_set_scale (user_data, priv->scale);

  priv->gesture_start_scale = 0.0;
  priv->in_zoom = FALSE;

  priv->anchor_x = -1;
  priv->anchor_y = -1;
}

static void
gesture_zoom_cancel_cb (GtkGesture       *gesture,
                        GdkEventSequence *sequence,
                        gpointer          user_data)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (user_data);

  gtk_image_view_set_scale (user_data, priv->gesture_start_scale);

  priv->gesture_start_scale = 0.0;
  priv->in_zoom = FALSE;
  priv->anchor_x = -1;
  priv->anchor_y = -1;
}


static void
gesture_scale_changed_cb (GtkGestureZoom *gesture,
                          double          delta,
                          GtkWidget      *widget)
{
  GtkImageView *image_view = GTK_IMAGE_VIEW (widget);
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  State state;
  double new_scale;

  if (!priv->in_zoom)
    {
      priv->in_zoom = TRUE;
      priv->gesture_start_scale = priv->scale;
    }

  new_scale = priv->gesture_start_scale * delta;
  gtk_image_view_get_current_state (image_view, &state);

  /* Don't emit */
  priv->scale = new_scale;
  gtk_image_view_update_adjustments (image_view);

  gtk_image_view_set_scale_internal (image_view, new_scale);

  if (priv->hadjustment != NULL && priv->vadjustment != NULL)
    {
      gtk_image_view_fix_anchor (image_view,
                                        priv->anchor_x,
                                        priv->anchor_y,
                                        &state);
    }
}

static void
gesture_rotate_begin_cb (GtkGesture       *gesture,
                         GdkEventSequence *sequence,
                         gpointer          user_data)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (user_data);

  if (!priv->rotate_gesture_enabled)
    {
      gtk_gesture_set_state (gesture, GTK_EVENT_SEQUENCE_DENIED);
      return;
    }


  if (priv->anchor_x == -1 && priv->anchor_y == -1)
    {
      gtk_gesture_get_bounding_box_center (gesture,
                                           &priv->anchor_x,
                                           &priv->anchor_y);
    }
}

static void
gesture_zoom_begin_cb (GtkGesture       *gesture,
                       GdkEventSequence *sequence,
                       gpointer          user_data)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (user_data);

  if (!priv->zoom_gesture_enabled)
    {
      gtk_gesture_set_state (gesture, GTK_EVENT_SEQUENCE_DENIED);
      return;
    }


  if (priv->anchor_x == -1 && priv->anchor_y == -1)
    {
      gtk_gesture_get_bounding_box_center (gesture,
                                           &priv->anchor_x,
                                           &priv->anchor_y);
    }
}




static void
gtk_image_view_init (GtkImageView *image_view)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  GtkWidget *widget = GTK_WIDGET (image_view);

  gtk_widget_set_can_focus (widget, TRUE);
  gtk_widget_set_has_window (widget, FALSE);

  priv->scale = 1.0;
  priv->angle = 0.0;
  priv->snap_angle = FALSE;
  priv->fit_allocation = FALSE;
  priv->scale_set = FALSE;
  priv->size_valid = FALSE;
  priv->anchor_x = -1;
  priv->anchor_y = -1;
  priv->rotate_gesture_enabled = TRUE;
  priv->zoom_gesture_enabled = TRUE;
  priv->rotate_gesture = gtk_gesture_rotate_new (widget);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (priv->rotate_gesture),
                                              GTK_PHASE_CAPTURE);
  g_signal_connect (priv->rotate_gesture, "angle-changed", (GCallback)gesture_angle_changed_cb, image_view);
  g_signal_connect (priv->rotate_gesture, "begin", (GCallback)gesture_rotate_begin_cb, image_view);
  g_signal_connect (priv->rotate_gesture, "end", (GCallback)gesture_rotate_end_cb, image_view);
  g_signal_connect (priv->rotate_gesture, "cancel", (GCallback)gesture_rotate_cancel_cb, image_view);

  priv->zoom_gesture = gtk_gesture_zoom_new (widget);
  g_signal_connect (priv->zoom_gesture, "scale-changed", (GCallback)gesture_scale_changed_cb, image_view);
  g_signal_connect (priv->zoom_gesture, "begin", (GCallback)gesture_zoom_begin_cb, image_view);
  g_signal_connect (priv->zoom_gesture, "end", (GCallback)gesture_zoom_end_cb, image_view);
  g_signal_connect (priv->zoom_gesture, "cancel", (GCallback)gesture_zoom_cancel_cb, image_view);

  gtk_gesture_group (priv->zoom_gesture,
                     priv->rotate_gesture);
}


static GdkPixbuf *
gtk_image_view_get_current_frame (GtkImageView *image_view)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  g_assert (priv->source_animation);

  if (priv->is_animation)
    return gdk_pixbuf_animation_iter_get_pixbuf (priv->source_animation_iter);
  else
    return gdk_pixbuf_animation_get_static_image (priv->source_animation);
}


static gboolean
gtk_image_view_update_animation (gpointer user_data)
{
  GtkImageView *image_view = user_data;
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  gdk_pixbuf_animation_iter_advance (priv->source_animation_iter, NULL);
  gtk_image_view_update_surface (image_view,
                                 gtk_image_view_get_current_frame (image_view),
                                 priv->scale_factor);

  return priv->is_animation;
}


static void
gtk_image_view_start_animation (GtkImageView *image_view)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  int delay_ms;

  g_assert (priv->is_animation);

  delay_ms = gdk_pixbuf_animation_iter_get_delay_time (priv->source_animation_iter);

  priv->animation_timeout = g_timeout_add (delay_ms, gtk_image_view_update_animation, image_view);
}

static void
gtk_image_view_stop_animation (GtkImageView *image_view)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  if (priv->animation_timeout != 0)
    {
      g_assert (priv->is_animation);
      g_source_remove (priv->animation_timeout);
      priv->animation_timeout = 0;
    }
}


static gboolean
frameclock_cb (GtkWidget     *widget,
               GdkFrameClock *frame_clock,
               gpointer       user_data)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (GTK_IMAGE_VIEW (widget));
  gint64 now = gdk_frame_clock_get_frame_time (frame_clock);

  double t = (now - priv->angle_transition_start) / TRANSITION_DURATION;

  double new_angle = (priv->transition_end_angle - priv->transition_start_angle) * t;

  priv->angle = priv->transition_start_angle + new_angle;

  if (priv->fit_allocation)
    gtk_widget_queue_draw (widget);
  else
    gtk_widget_queue_resize (widget);

  if (t >= 1.0)
    {
      priv->angle = priv->transition_end_angle;
      return G_SOURCE_REMOVE;
    }

  return G_SOURCE_CONTINUE;
}


static void
gtk_image_view_animate_to_angle (GtkImageView *image_view,
                                 double        start_angle)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  /* target angle is priv->angle! */

  priv->transition_start_angle = start_angle;
  priv->transition_end_angle   = priv->angle;
  priv->angle_transition_start = gdk_frame_clock_get_frame_time (gtk_widget_get_frame_clock (GTK_WIDGET (image_view)));
  gtk_widget_add_tick_callback (GTK_WIDGET (image_view), frameclock_cb, NULL, NULL);
}

static void
gtk_image_view_do_snapping (GtkImageView *image_view,
                            double        angle)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  int new_angle;

  g_assert (priv->snap_angle);

  /* Snap to angles of 0, 90, 180 and 270 degrees */

  new_angle = (int) ((angle) / 90.0) * 90;

  if (new_angle != priv->angle)
    {
      double old_angle = priv->angle;
      priv->angle = new_angle;
      /* XXX Make this conditional */
      gtk_image_view_animate_to_angle (image_view,
                                       old_angle);
    }

  priv->angle = new_angle;
}

static gboolean
gtk_image_view_draw (GtkWidget *widget, cairo_t *ct)
{
  GtkImageView *image_view = GTK_IMAGE_VIEW (widget);
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  GtkStyleContext *sc = gtk_widget_get_style_context (widget);
  int widget_width = gtk_widget_get_allocated_width (widget);
  int widget_height = gtk_widget_get_allocated_height (widget);
  int draw_x;
  int draw_y;
  int image_width;
  int image_height;
  double draw_width;
  double draw_height;
  double scale = 0.0;


  if (priv->vadjustment && priv->hadjustment)
    {
      int x = - gtk_adjustment_get_value (priv->hadjustment);
      int y = - gtk_adjustment_get_value (priv->vadjustment);
      int w = gtk_adjustment_get_upper (priv->hadjustment);
      int h = gtk_adjustment_get_upper (priv->vadjustment);

      gtk_render_background (sc, ct, x, y, w, h);
      gtk_render_frame (sc, ct, x, y, w, h);
    }
  else
    {
      gtk_render_background (sc, ct, 0, 0, widget_width, widget_height);
      gtk_render_frame      (sc, ct, 0, 0, widget_width, widget_height);
    }

  if (!priv->image_surface)
    return GDK_EVENT_PROPAGATE;

  gtk_image_view_compute_bounding_box (image_view,
                                       &draw_width, &draw_height,
                                       &scale);


  if (draw_width == 0 || draw_height == 0)
    return GDK_EVENT_PROPAGATE;

  image_width  = cairo_image_surface_get_width (priv->image_surface)  * scale;
  image_height = cairo_image_surface_get_height (priv->image_surface) * scale;


  if (priv->hadjustment && priv->vadjustment)
    {
      draw_x = (gtk_adjustment_get_page_size (priv->hadjustment) - image_width)  / 2;
      draw_y = (gtk_adjustment_get_page_size (priv->vadjustment) - image_height) / 2;
    }
  else
    {
      draw_x = (widget_width  - image_width)  / 2;
      draw_y = (widget_height - image_height) / 2;
    }

  cairo_save (ct);
  /* XXX This is unnecessarily big */
  /*cairo_rectangle (ct, draw_x, draw_y, draw_width, draw_height);*/
  cairo_rectangle (ct, 0, 0, widget_width, widget_height);


  if (priv->hadjustment && draw_width >= widget_width)
    {
      draw_x = (draw_width - image_width) / 2;
      draw_x -= gtk_adjustment_get_value (priv->hadjustment);
    }


  if (priv->vadjustment && draw_height >= widget_height)
    {
      draw_y = (draw_height - image_height) / 2;
      draw_y -= gtk_adjustment_get_value (priv->vadjustment);
    }


  /* Rotate around the center */
  cairo_translate (ct,
                   draw_x + (image_width  / 2.0),
                   draw_y + (image_height / 2.0));
  cairo_rotate (ct, DEG_TO_RAD (priv->angle));
  cairo_translate (ct,
                   - draw_x - (image_width  / 2.0),
                   - draw_y - (image_height / 2.0));


  cairo_scale (ct, scale * priv->scale_factor, scale * priv->scale_factor);
  cairo_set_source_surface (ct,
                            priv->image_surface,
                            draw_x / scale / priv->scale_factor,
                            draw_y / scale / priv->scale_factor);
  cairo_pattern_set_filter (cairo_get_source (ct), CAIRO_FILTER_FAST);
  cairo_fill (ct);
  cairo_restore (ct);


  /* XXX @debug */
  if (priv->anchor_x != -1 && priv->anchor_y != -1)
    {
      cairo_set_source_rgba (ct, 0, 1, 0, 1);
      cairo_rectangle (ct, priv->anchor_x - 1, priv->anchor_y - 1, 2, 2);
      cairo_fill (ct);
    }

  return GDK_EVENT_PROPAGATE;
}

/* Property Getter/Setter {{{ */
static void
gtk_image_view_set_hadjustment (GtkImageView  *image_view,
                                GtkAdjustment *hadjustment)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  if (priv->hadjustment && priv->hadjustment == hadjustment)
    return;

  if (priv->hadjustment)
    {
      g_signal_handlers_disconnect_by_func (priv->hadjustment, adjustment_value_changed_cb, image_view);
      g_object_unref (priv->hadjustment);
    }


  if (hadjustment)
    {
      g_signal_connect (G_OBJECT (hadjustment), "value-changed",
                        G_CALLBACK (adjustment_value_changed_cb), image_view);
      priv->hadjustment = g_object_ref_sink (hadjustment);
    }
  else
    {
      priv->hadjustment = hadjustment;
    }

  g_object_notify (G_OBJECT (image_view), "hadjustment");

  gtk_image_view_update_adjustments (image_view);

  if (priv->fit_allocation)
    gtk_widget_queue_draw (GTK_WIDGET (image_view));
  else
    gtk_widget_queue_resize (GTK_WIDGET (image_view));

}

static void
gtk_image_view_set_vadjustment (GtkImageView  *image_view,
                                GtkAdjustment *vadjustment)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  if (priv->vadjustment == vadjustment)
    return;

  if (priv->vadjustment)
    {
      g_signal_handlers_disconnect_by_func (priv->vadjustment, adjustment_value_changed_cb, image_view);
      g_object_unref (priv->vadjustment);
    }

  if (vadjustment)
    {
      g_signal_connect ((GObject *)vadjustment, "value-changed",
                        (GCallback) adjustment_value_changed_cb, image_view);
      priv->vadjustment = g_object_ref_sink (vadjustment);
    }
  else
    {
      priv->vadjustment = vadjustment;
    }

  g_object_notify (G_OBJECT (image_view), "vadjustment");

  gtk_image_view_update_adjustments (image_view);

  if (priv->fit_allocation)
    gtk_widget_queue_draw ((GtkWidget *)image_view);
  else
    gtk_widget_queue_resize ((GtkWidget *)image_view);
}

static void
gtk_image_view_set_hscroll_policy (GtkImageView        *image_view,
                                   GtkScrollablePolicy  hscroll_policy)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  if (priv->hscroll_policy == hscroll_policy)
    return;

  priv->hscroll_policy = hscroll_policy;
  gtk_image_view_update_adjustments (image_view);
}

static void
gtk_image_view_set_vscroll_policy (GtkImageView        *image_view,
                                   GtkScrollablePolicy  vscroll_policy)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  if (priv->vscroll_policy == vscroll_policy)
    return;

  priv->vscroll_policy = vscroll_policy;
  gtk_image_view_update_adjustments (image_view);
}

/**
 * gtk_image_view_set_scale:
 * @image_view: A #GtkImageView instance
 * @scale: The new scale value
 *
 * Sets the value of the #scale property. This will cause the
 * #scale-set property to be set to #TRUE as well. If the given
 * value of @scale is below zero, 0 will be set instead.
 *
 * If #fit-allocation is #TRUE, it will be set to #FALSE, and @image_view
 * will be resized to the image's current size, taking the new scale into
 * account.
 */
void
gtk_image_view_set_scale (GtkImageView *image_view,
                          double        scale)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  State state;
  double pointer_x;
  double pointer_y;

  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));
  g_return_if_fail (scale >= 0.0);

  pointer_x = gtk_widget_get_allocated_width (GTK_WIDGET (image_view))  / 2;
  pointer_y = gtk_widget_get_allocated_height (GTK_WIDGET (image_view)) / 2;

  gtk_image_view_get_current_state (image_view, &state);
  gtk_image_view_set_scale_internal (image_view, scale);

  if (priv->hadjustment != NULL && priv->vadjustment != NULL)
    {
      gtk_image_view_fix_anchor (image_view,
                                        pointer_x,
                                        pointer_y,
                                        &state);
    }
}

double
gtk_image_view_get_scale (GtkImageView *image_view)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  g_return_val_if_fail (GTK_IS_IMAGE_VIEW (image_view), 0.0);

  return priv->scale;
}

/**
 * gtk_image_view_set_angle:
 * @image_view: A #GtkImageView
 * @angle: The angle to rotate the image about, in
 *   degrees. If this is < 0 or > 360, the value will
 *   be wrapped. So e.g. setting this to 362 will result in a
 *   angle of 2, setting it to -2 will result in 358.
 *   Both 0 and 360 are possible.
 */
void
gtk_image_view_set_angle (GtkImageView *image_view,
                          double        angle)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  State state;

  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));



  if (angle > 360.0)
    angle -= (int)(angle / 360.0) * 360;
  else if (angle < 0.0)
    angle = 360.0 + (int)(angle / 360.0);

  g_assert (angle >= 0.0);
  g_assert (angle <= 360.0);


  gtk_image_view_get_current_state (image_view, &state);

  priv->angle = angle;
  priv->size_valid = FALSE;



  gtk_image_view_update_adjustments (image_view);


  g_object_notify_by_pspec (G_OBJECT (image_view),
                            widget_props[PROP_ANGLE]);

  // XXX Pass a width/2, height/2 anchor here.
  //
  // TODO: Would we have to document this behavior? Or make it configurable?


  if (priv->hadjustment != NULL && priv->vadjustment != NULL)
    gtk_image_view_fix_anchor (image_view,
                                priv->anchor_x,
                                priv->anchor_y,
                                &state);

  if (priv->fit_allocation)
    gtk_widget_queue_draw (GTK_WIDGET (image_view));
  else
    gtk_widget_queue_resize (GTK_WIDGET (image_view));
}

double
gtk_image_view_get_angle (GtkImageView *image_view)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  g_return_val_if_fail (GTK_IS_IMAGE_VIEW (image_view), 0.0);

  return priv->angle;
}



/**
 * gtk_image_view_set_snap_angle:
 * @image_view: A #GtkImageView instance
 * @snap_angle: The new value of the #snap-angle property
 *
 * Setting #snap-angle to #TRUE will cause @image_view's  angle to
 * be snapped to 90° steps. Setting the #angle property will cause it to
 * be set to the lower 90° step, e.g. setting #angle to 359 will cause
 * the new value to be 270.
 */
void
gtk_image_view_set_snap_angle (GtkImageView *image_view,
                               gboolean     snap_angle)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));

  snap_angle = !!snap_angle;

  if (snap_angle == priv->snap_angle)
    return;

  priv->snap_angle = snap_angle;
  g_object_notify_by_pspec (G_OBJECT (image_view),
                            widget_props[PROP_SNAP_ANGLE]);

  if (priv->snap_angle)
    gtk_image_view_do_snapping (image_view, priv->angle);
}

gboolean
gtk_image_view_get_snap_angle (GtkImageView *image_view)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  g_return_val_if_fail (GTK_IS_IMAGE_VIEW (image_view), FALSE);

  return priv->snap_angle;
}



/**
 * gtk_image_view_set_fit_allocation:
 * @image_view: A #GtkImageView instance
 * @fit_allocation: The new value of the #fit-allocation property.
 *
 * Setting #fit-allocation to #TRUE will cause the image to be scaled
 * to the widget's allocation, unless it would cause the image to be
 * scaled up.
 *
 * Setting #fit-allocation will have the side effect of setting
 * #scale-set set to #FALSE, thus giving the #GtkImageView the control
 * over the image's scale. Additionally, if the new #fit-allocation
 * value is #FALSE, the scale will be reset to 1.0 and the #GtkImageView
 * will be resized to take at least the image's real size.
 */
void
gtk_image_view_set_fit_allocation (GtkImageView *image_view,
                                   gboolean      fit_allocation)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));

  fit_allocation = !!fit_allocation;

  if (fit_allocation == priv->fit_allocation)
    return;

  priv->fit_allocation = fit_allocation;
  g_object_notify_by_pspec (G_OBJECT (image_view),
                            widget_props[PROP_FIT_ALLOCATION]);

  priv->scale_set = FALSE;
  priv->size_valid = FALSE;
  g_object_notify_by_pspec (G_OBJECT (image_view),
                            widget_props[PROP_SCALE_SET]);

  if (!priv->fit_allocation && !priv->scale_set)
    {
      priv->scale = 1.0;
      g_object_notify_by_pspec (G_OBJECT (image_view),
                                widget_props[PROP_SCALE]);
    }

  gtk_image_view_update_adjustments (image_view);

  gtk_widget_queue_resize (GTK_WIDGET (image_view));
  gtk_image_view_update_adjustments (image_view);
}

gboolean
gtk_image_view_get_fit_allocation (GtkImageView *image_view)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  g_return_val_if_fail (GTK_IS_IMAGE_VIEW (image_view), FALSE);

  return priv->fit_allocation;
}



void
gtk_image_view_set_rotate_gesture_enabled (GtkImageView *image_view,
                                           gboolean      rotate_gesture_enabled)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));

  rotate_gesture_enabled = !!rotate_gesture_enabled;

  if (priv->rotate_gesture_enabled != rotate_gesture_enabled)
    {
      priv->rotate_gesture_enabled = rotate_gesture_enabled;
      g_object_notify_by_pspec (G_OBJECT (image_view),
                                widget_props[PROP_ROTATE_GESTURE_ENABLED]);
    }
}

gboolean
gtk_image_view_get_rotate_gesture_enabled (GtkImageView *image_view)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  g_return_val_if_fail (GTK_IS_IMAGE_VIEW (image_view), FALSE);

  return priv->rotate_gesture_enabled;
}



void
gtk_image_view_set_zoom_gesture_enabled (GtkImageView *image_view,
                                         gboolean      zoom_gesture_enabled)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));

  zoom_gesture_enabled = !!zoom_gesture_enabled;

  if (zoom_gesture_enabled != priv->zoom_gesture_enabled)
    {
      priv->zoom_gesture_enabled = zoom_gesture_enabled;
      g_object_notify_by_pspec (G_OBJECT (image_view),
                                widget_props[PROP_ZOOM_GESTURE_ENABLED]);
    }
}

gboolean
gtk_image_view_get_zoom_gesture_enabled (GtkImageView *image_view)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  g_return_val_if_fail (GTK_IS_IMAGE_VIEW (image_view), FALSE);

  return priv->zoom_gesture_enabled;
}
/* }}} */


/* GtkWidget API {{{ */

static void
gtk_image_view_realize (GtkWidget *widget)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (GTK_IMAGE_VIEW (widget));
  GtkAllocation allocation;
  GdkWindowAttr attributes = { 0, };
  GdkWindow *window;

  gtk_widget_get_allocation (widget, &allocation);
  gtk_widget_set_realized (widget, TRUE);

  attributes.x = allocation.x;
  attributes.y = allocation.y;
  attributes.width  = allocation.width;
  attributes.height = allocation.height;
  attributes.window_type = GDK_WINDOW_CHILD;
  attributes.event_mask = gtk_widget_get_events (widget) |
                          GDK_POINTER_MOTION_MASK |
                          GDK_BUTTON_PRESS_MASK |
                          GDK_BUTTON_RELEASE_MASK |
                          GDK_SMOOTH_SCROLL_MASK |
                          GDK_SCROLL_MASK;
  attributes.wclass = GDK_INPUT_ONLY;

  window = gtk_widget_get_parent_window (widget);

  gtk_widget_set_window (widget, window);
  g_object_ref (G_OBJECT (window));

  window = gdk_window_new (gtk_widget_get_parent_window (widget),
                           &attributes, GDK_WA_X | GDK_WA_Y);
  priv->event_window = window;

  gtk_widget_register_window (widget, priv->event_window);
  gdk_window_set_user_data (window, widget);
}

static void
gtk_image_view_size_allocate (GtkWidget     *widget,
                              GtkAllocation *allocation)
{
  GtkImageView *image_view = GTK_IMAGE_VIEW (widget);
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  gtk_widget_set_allocation (widget, allocation);

  if (gtk_widget_get_realized (widget))
    {
      gdk_window_move_resize (priv->event_window,
                              allocation->x, allocation->y,
                              allocation->width, allocation->height);
    }

  if (priv->fit_allocation)
    priv->size_valid = FALSE;

  gtk_image_view_update_adjustments (image_view);
}

static void
gtk_image_view_map (GtkWidget *widget)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (GTK_IMAGE_VIEW (widget));

  if (priv->is_animation)
    gtk_image_view_start_animation (GTK_IMAGE_VIEW (widget));

  if (priv->event_window)
    gdk_window_show (priv->event_window);

  GTK_WIDGET_CLASS (gtk_image_view_parent_class)->map (widget);
}

static void
gtk_image_view_unmap (GtkWidget *widget)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (GTK_IMAGE_VIEW (widget));


  if (priv->is_animation)
    gtk_image_view_stop_animation (GTK_IMAGE_VIEW (widget));

  GTK_WIDGET_CLASS (gtk_image_view_parent_class)->unmap (widget);
}

static void
adjustment_value_changed_cb (GtkAdjustment *adjustment,
                             gpointer       user_data)
{
  GtkImageView *image_view = user_data;

  gtk_image_view_update_adjustments (image_view);

  gtk_widget_queue_draw (GTK_WIDGET (image_view));
}

static void
gtk_image_view_get_preferred_height (GtkWidget *widget,
                                     int       *minimal,
                                     int       *natural)
{
  GtkImageView *image_view  = GTK_IMAGE_VIEW (widget);
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  double width, height;
  gtk_image_view_compute_bounding_box (image_view,
                                       &width,
                                       &height,
                                       NULL);

  if (priv->fit_allocation)
    {
      *minimal = 0;
      *natural = height;
    }
  else
    {
      *minimal = height;
      *natural = height;
    }
}

static void
gtk_image_view_get_preferred_width (GtkWidget *widget,
                                    int       *minimal,
                                    int       *natural)
{
  GtkImageView *image_view  = GTK_IMAGE_VIEW (widget);
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  double width, height;
  gtk_image_view_compute_bounding_box (image_view,
                                       &width,
                                       &height,
                                       NULL);

  if (priv->fit_allocation)
    {
      *minimal = 0;
      *natural = width;
    }
  else
    {
      *minimal = width;
      *natural = width;
    }

}


static gboolean
gtk_image_view_scroll_event (GtkWidget       *widget,
                             GdkEventScroll  *event)
{
  GtkImageView *image_view = GTK_IMAGE_VIEW (widget);
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  double new_scale = priv->scale - (0.1 * event->delta_y);
  State state;

  gtk_image_view_get_current_state (image_view, &state);

  gtk_image_view_set_scale_internal (image_view, new_scale);

  if (priv->hadjustment != NULL && priv->vadjustment != NULL)
    {
      gtk_image_view_fix_anchor (image_view,
                                        event->x,
                                        event->y,
                                        &state);
    }

  return GDK_EVENT_STOP;
}

/* }}} */


/* GObject API {{{ */
static void
gtk_image_view_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)

{
  GtkImageView *image_view = (GtkImageView *) object;

  switch (prop_id)
    {
      case PROP_SCALE:
        gtk_image_view_set_scale (image_view, g_value_get_double (value));
        break;
      case PROP_ANGLE:
        gtk_image_view_set_angle (image_view, g_value_get_double (value));
        break;
      case PROP_SNAP_ANGLE:
        gtk_image_view_set_snap_angle (image_view, g_value_get_boolean (value));
        break;
      case PROP_FIT_ALLOCATION:
        gtk_image_view_set_fit_allocation (image_view, g_value_get_boolean (value));
        break;
      case PROP_ROTATE_GESTURE_ENABLED:
        gtk_image_view_set_rotate_gesture_enabled (image_view, g_value_get_boolean (value));
        break;
      case PROP_ZOOM_GESTURE_ENABLED:
        gtk_image_view_set_zoom_gesture_enabled (image_view, g_value_get_boolean (value));
        break;
      case PROP_HADJUSTMENT:
        gtk_image_view_set_hadjustment (image_view, g_value_get_object (value));
        break;
       case PROP_VADJUSTMENT:
        gtk_image_view_set_vadjustment (image_view, g_value_get_object (value));
        break;
      case PROP_HSCROLL_POLICY:
        gtk_image_view_set_hscroll_policy (image_view, g_value_get_enum (value));
        break;
      case PROP_VSCROLL_POLICY:
        gtk_image_view_set_vscroll_policy (image_view, g_value_get_enum (value));
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
gtk_image_view_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  GtkImageView *image_view  = (GtkImageView *)object;
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  switch (prop_id)
    {
      case PROP_SCALE:
        g_value_set_double (value, priv->scale);
        break;
      case PROP_SCALE_SET:
        g_value_set_boolean (value, priv->scale_set);
        break;
      case PROP_ANGLE:
        g_value_set_double (value, priv->angle);
        break;
      case PROP_SNAP_ANGLE:
        g_value_set_boolean (value, priv->snap_angle);
        break;
      case PROP_FIT_ALLOCATION:
        g_value_set_boolean (value, priv->fit_allocation);
        break;
      case PROP_ROTATE_GESTURE_ENABLED:
        g_value_set_boolean (value, priv->rotate_gesture_enabled);
        break;
      case PROP_ZOOM_GESTURE_ENABLED:
        g_value_set_boolean (value, priv->zoom_gesture_enabled);
        break;
      case PROP_HADJUSTMENT:
        g_value_set_object (value, priv->hadjustment);
        break;
      case PROP_VADJUSTMENT:
        g_value_set_object (value, priv->vadjustment);
        break;
      case PROP_HSCROLL_POLICY:
        g_value_set_enum (value, priv->hscroll_policy);
        break;
      case PROP_VSCROLL_POLICY:
        g_value_set_enum (value, priv->vscroll_policy);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gtk_image_view_finalize (GObject *object)
{
  GtkImageView *image_view  = (GtkImageView *)object;
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  gtk_image_view_stop_animation (image_view);

  g_clear_object (&priv->source_animation);

  g_clear_object (&priv->rotate_gesture);
  g_clear_object (&priv->zoom_gesture);

  g_clear_object (&priv->hadjustment);
  g_clear_object (&priv->vadjustment);

  if (priv->image_surface)
    cairo_surface_destroy (priv->image_surface);

  G_OBJECT_CLASS (gtk_image_view_parent_class)->finalize (object);
}

/* }}} GObject API */

static void
gtk_image_view_class_init (GtkImageViewClass *view_class)
{
  GObjectClass   *object_class = G_OBJECT_CLASS (view_class);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (view_class);

  object_class->set_property = gtk_image_view_set_property;
  object_class->get_property = gtk_image_view_get_property;
  object_class->finalize     = gtk_image_view_finalize;

  widget_class->draw          = gtk_image_view_draw;
  widget_class->realize       = gtk_image_view_realize;
  widget_class->size_allocate = gtk_image_view_size_allocate;
  widget_class->map           = gtk_image_view_map;
  widget_class->unmap         = gtk_image_view_unmap;
  widget_class->scroll_event  = gtk_image_view_scroll_event;
  widget_class->get_preferred_width  = gtk_image_view_get_preferred_width;
  widget_class->get_preferred_height = gtk_image_view_get_preferred_height;

  /**
   * GtkImageView:scale:
   * The scale the internal surface gets drawn with.
   *
   * Since: 3.20
   */
  widget_props[PROP_SCALE] = g_param_spec_double ("scale",
                                                  P_("Scale"),
                                                  P_("The scale the internal surface gets drawn with"),
                                                  0.0,
                                                  G_MAXDOUBLE,
                                                  1.0,
                                                  GTK_PARAM_READWRITE|G_PARAM_EXPLICIT_NOTIFY);
  /**
   * GtkImageView:scale-set:
   * Whether or not the current value of the scale property was set by the user.
   * This is to distringuish between scale values set by the GtkImageView itself,
   * e.g. when fit-allocation is true, which will change the scale depeding on the
   * widget allocation.
   *
   * Since: 3.20
   */
  widget_props[PROP_SCALE_SET] = g_param_spec_boolean ("scale-set",
                                                       P_(""),
                                                       P_("fooar"),
                                                       FALSE,
                                                       GTK_PARAM_READABLE|G_PARAM_EXPLICIT_NOTIFY);
  /**
   * GtkImageView:angle:
   * The angle the surface gets rotated about.
   * This is in degrees and we rotate in the mathematically negative direction,
   * i.e. clock wise.
   *
   * Since: 3.20
   */
  widget_props[PROP_ANGLE] = g_param_spec_double ("angle",
                                                  P_("angle"),
                                                  P_("angle"),
                                                  0.0,
                                                  360.0,
                                                  0.0,
                                                  GTK_PARAM_READWRITE|G_PARAM_EXPLICIT_NOTIFY);
  /**
   * GtkImageView:rotate-gesture-enabled:
   * Whether or not the image can be rotated using a two-finger rotate gesture.
   *
   * Since: 3.20
   */
  widget_props[PROP_ROTATE_GESTURE_ENABLED] = g_param_spec_boolean ("rotate-gesture-enabled",
                                                                    P_("Foo"),
                                                                    P_("fooar"),
                                                                    TRUE,
                                                                    GTK_PARAM_READWRITE|G_PARAM_EXPLICIT_NOTIFY);
  /**
   * GtkImageView:zoom-gesture-enabled:
   * Whether or not image can be scaled using a two-finger zoom gesture or not.
   *
   * Since: 3.20
   */
  widget_props[PROP_ZOOM_GESTURE_ENABLED] = g_param_spec_boolean ("zoom-gesture-enabled",
                                                                  P_("Foo"),
                                                                  P_("fooar"),
                                                                  TRUE,
                                                                  GTK_PARAM_READWRITE|G_PARAM_EXPLICIT_NOTIFY);
  /**
   * GtkImageView:snap-angle:
   * Whether or not the angle property snaps to 90° steps. If this is enabled
   * and the angle property gets set to a non-90° step, the new value will be
   * set to the closest 90° step that is lower than the given angle.
   * Changing the angle from one 90° step to another will be transitioned
   *
   * Since: 3.20
   */
  widget_props[PROP_SNAP_ANGLE] = g_param_spec_boolean ("snap-angle",
                                                        P_("Foo"),
                                                        P_("fooar"),
                                                        FALSE,
                                                        GTK_PARAM_READWRITE|G_PARAM_EXPLICIT_NOTIFY);

  /**
   * GtkImageView:fit-allocation:
   * If this is TRUE, the scale the image will be drawn in will depend on the current
   * widget allocation. The image will be scaled down to fit into the widget allocation,
   * but never scaled up.
   *
   * Since: 3.20
   */
  widget_props[PROP_FIT_ALLOCATION] = g_param_spec_boolean ("fit-allocation",
                                                            P_("Foo"),
                                                            P_("fooar"),
                                                            FALSE,
                                                            GTK_PARAM_READWRITE|G_PARAM_EXPLICIT_NOTIFY);


  g_object_class_install_properties (object_class, LAST_WIDGET_PROPERTY, widget_props);

  g_object_class_override_property (object_class, PROP_HADJUSTMENT,    "hadjustment");
  g_object_class_override_property (object_class, PROP_VADJUSTMENT,    "vadjustment");
  g_object_class_override_property (object_class, PROP_HSCROLL_POLICY, "hscroll-policy");
  g_object_class_override_property (object_class, PROP_VSCROLL_POLICY, "vscroll-policy");

  gtk_widget_class_set_css_name (widget_class, "imageview");
}

GtkWidget *
gtk_image_view_new ()
{
  return g_object_new (GTK_TYPE_IMAGE_VIEW, NULL);
}

static void
gtk_image_view_replace_surface (GtkImageView    *image_view,
                                cairo_surface_t *surface,
                                int              scale_factor)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  if (priv->image_surface)
    cairo_surface_destroy (priv->image_surface);

  priv->scale_factor = scale_factor;
  priv->image_surface = surface;
  priv->size_valid = FALSE;

  if (surface)
    cairo_surface_reference (priv->image_surface);
}

static void
gtk_image_view_update_surface (GtkImageView    *image_view,
                               const GdkPixbuf *frame,
                               int              scale_factor)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  int new_width    = gdk_pixbuf_get_width (frame);
  int new_height   = gdk_pixbuf_get_height (frame);
  int widget_scale = gtk_widget_get_scale_factor (GTK_WIDGET (image_view));
  int real_width   = (new_width * scale_factor)  / widget_scale;
  int real_height  = (new_height * scale_factor) / widget_scale;

  if (!priv->image_surface ||
      cairo_image_surface_get_width (priv->image_surface)  != real_width ||
      cairo_image_surface_get_height (priv->image_surface) != real_height ||
      priv->scale_factor != scale_factor)
    {
      GdkWindow *window = gtk_widget_get_window (GTK_WIDGET (image_view));
      cairo_surface_t *new_surface = gdk_cairo_surface_create_from_pixbuf (frame,
                                                                           scale_factor,
                                                                           window);
      g_assert (new_surface != NULL);
      gtk_image_view_replace_surface (image_view,
                                      new_surface,
                                      scale_factor);
      gtk_widget_queue_resize (GTK_WIDGET (image_view));
    }
  else
    {
      gdk_cairo_surface_paint_pixbuf (priv->image_surface, frame);
      gtk_widget_queue_draw (GTK_WIDGET (image_view));
    }

  g_assert (priv->image_surface != NULL);
}

static void
gtk_image_view_replace_animation (GtkImageView       *image_view,
                                  GdkPixbufAnimation *animation,
                                  int                 scale_factor)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  if (priv->source_animation)
    {
      g_assert (priv->image_surface);
      if (priv->is_animation)
        gtk_image_view_stop_animation (image_view);
    }

  priv->is_animation = !gdk_pixbuf_animation_is_static_image (animation);

  if (priv->is_animation)
    {
      priv->source_animation = animation;
      priv->source_animation_iter = gdk_pixbuf_animation_get_iter (priv->source_animation,
                                                                   NULL);
      gtk_image_view_update_surface (image_view,
                                     gtk_image_view_get_current_frame (image_view),
                                     scale_factor);

      gtk_image_view_start_animation (image_view);
    }
  else
    {
      gtk_image_view_update_surface (image_view,
                                     gdk_pixbuf_animation_get_static_image (animation),
                                     scale_factor);
    }

}



static void
gtk_image_view_load_image_from_stream (GtkImageView *image_view,
                                       GInputStream *input_stream,
                                       int           scale_factor,
                                       GCancellable *cancellable,
                                       GError       *error)
{
  GdkPixbufAnimation *result;


  g_assert (error == NULL);
  result = gdk_pixbuf_animation_new_from_stream (G_INPUT_STREAM (input_stream),
                                                 cancellable,
                                                 &error);

  g_object_unref (input_stream);
  if (!error)
    gtk_image_view_replace_animation (image_view, result,scale_factor);
}

static void
gtk_image_view_load_image_contents (GTask        *task,
                                    gpointer      source_object,
                                    gpointer      task_data,
                                    GCancellable *cancellable)
{
  GtkImageView *image_view = source_object;
  LoadTaskData *data = task_data;
  GFile *file = G_FILE (data->source);
  GError *error = NULL;
  GFileInputStream *in_stream;

  in_stream = g_file_read (file, cancellable, &error);

  if (error)
    {
      g_free (data);
      g_task_return_error (task, error);
      return;
    }


  gtk_image_view_load_image_from_stream (image_view,
                                         G_INPUT_STREAM (in_stream),
                                         data->scale_factor,
                                         cancellable,
                                         error);

  if (error)
    g_task_return_error (task, error);

  g_slice_free (LoadTaskData, data);
}

static void
gtk_image_view_load_from_input_stream (GTask *task,
                                       gpointer source_object,
                                       gpointer task_data,
                                       GCancellable *cancellable)
{
  GtkImageView *image_view = source_object;
  LoadTaskData *data = task_data;
  GInputStream *in_stream = G_INPUT_STREAM (data->source);
  GError *error = NULL;

  gtk_image_view_load_image_from_stream (image_view,
                                         in_stream,
                                         data->scale_factor,
                                         cancellable,
                                         error);

  if (error)
    g_task_return_error (task, error);
}



void
gtk_image_view_load_from_file_async (GtkImageView        *image_view,
                                     GFile               *file,
                                     int                  scale_factor,
                                     GCancellable        *cancellable,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data)
{
  GTask *task;
  LoadTaskData *task_data;
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));
  g_return_if_fail (G_IS_FILE (file));
  g_return_if_fail (scale_factor > 0);

  task_data = g_slice_new (LoadTaskData);
  task_data->scale_factor = scale_factor;
  task_data->source = file;

  task = g_task_new (image_view, cancellable, callback, user_data);
  g_task_set_task_data (task, task_data, (GDestroyNotify)free_load_task_data);
  g_task_run_in_thread (task, gtk_image_view_load_image_contents);

  g_object_unref (task);
}
void
gtk_image_view_load_from_file_finish   (GtkImageView  *image_view,
                                        GAsyncResult  *result,
                                        GError       **error)
{
  g_return_if_fail (g_task_is_valid (result, image_view));
}




void
gtk_image_view_load_from_stream_async (GtkImageView        *image_view,
                                       GInputStream        *input_stream,
                                       int                  scale_factor,
                                       GCancellable        *cancellable,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data)
{
  GTask *task;
  LoadTaskData *task_data;
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));
  g_return_if_fail (G_IS_INPUT_STREAM (input_stream));
  g_return_if_fail (scale_factor > 0);

  task_data = g_slice_new (LoadTaskData);
  task_data->scale_factor = scale_factor;
  task_data->source = input_stream;

  task = g_task_new (image_view, cancellable, callback, user_data);
  g_task_set_task_data (task, task_data, (GDestroyNotify)free_load_task_data);
  g_task_run_in_thread (task, gtk_image_view_load_from_input_stream);

  g_object_unref (task);
}
void
gtk_image_view_load_from_stream_finish (GtkImageView  *image_view,
                                        GAsyncResult  *result,
                                        GError       **error)
{
  g_return_if_fail (g_task_is_valid (result, image_view));
}

/*
 * gtk_image_view_set_pixbuf:
 * @image_view: A #GtkImageView instance
 * @pixbuf: A #GdkPixbuf instance
 * @scale_factor: The scale factor of the pixbuf. This will
 *   be interpreted as "the given pixbuf is supposed to be used
 *   with the given scale factor", i.e. if the pixbuf's scale
 *   factor is 2, and the screen's scale factor is also 2, the
 *   pixbuf won't be scaled up.
 */
void
gtk_image_view_set_pixbuf (GtkImageView    *image_view,
                           const GdkPixbuf *pixbuf,
                           int              scale_factor)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));
  g_return_if_fail (GDK_IS_PIXBUF (pixbuf));
  g_return_if_fail (scale_factor > 0);


  if (priv->is_animation)
    {
      g_clear_object (&priv->source_animation);
      gtk_image_view_stop_animation (image_view);
      priv->is_animation = FALSE;
    }

  gtk_image_view_update_surface (image_view, pixbuf, scale_factor);

  gtk_image_view_update_adjustments (image_view);
}

/**
 * gtk_image_view_set_surface:
 * @image_view: A #GtkImageView instance
 * @surface: (nullable): A #cairo_surface_t of type #CAIRO_SURFACE_TYPE_IMAGE, or
 *   %NULL to unset any internal image data. In case this is %NULL, the scale will
 *   be reset to 1.0.
 */
void
gtk_image_view_set_surface (GtkImageView    *image_view,
                            cairo_surface_t *surface)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  double scale_x = 0.0;
  double scale_y;

  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));

  if (surface)
    {
      g_return_if_fail (cairo_surface_get_type (surface) == CAIRO_SURFACE_TYPE_IMAGE);

      cairo_surface_get_device_scale (surface, &scale_x, &scale_y);

      g_return_if_fail (scale_x == scale_y);
    }
  else
    {
      priv->scale = 1.0;
      g_object_notify_by_pspec (G_OBJECT (image_view),
                                widget_props[PROP_SCALE]);
    }



  if (priv->is_animation)
    {
      g_clear_object (&priv->source_animation);
      gtk_image_view_stop_animation (image_view);
      priv->is_animation = FALSE;
    }

  gtk_image_view_replace_surface (image_view,
                                  surface,
                                  scale_x);

  gtk_image_view_update_adjustments (image_view);

  if (priv->fit_allocation)
    gtk_widget_queue_draw (GTK_WIDGET (image_view));
  else
    gtk_widget_queue_resize (GTK_WIDGET (image_view));
}

/**
 * gtk_image_view_set_animation:
 * @image_view: A #GtkImageView instance
 * @animation: The #GdkPixbufAnimation to use
 * @scale_factor: The scale factor of the animation. This will
 *   be interpreted as "the given animation is supposed to be used
 *   with the given scale factor", i.e. if the animation's scale
 *   factor is 2, and the screen's scale factor is also 2, the
 *   animation won't be scaled up.
 */
void
gtk_image_view_set_animation (GtkImageView       *image_view,
                              GdkPixbufAnimation *animation,
                              int                 scale_factor)
{
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));
  g_return_if_fail (GDK_IS_PIXBUF_ANIMATION (animation));
  g_return_if_fail (scale_factor > 0);

  gtk_image_view_replace_animation (image_view, animation, scale_factor);
}
