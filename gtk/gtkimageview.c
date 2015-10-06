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

double _new_x;
double _new_y;

struct _GtkImageViewPrivate
{
  double   scale;
  double   angle;
  gboolean snap_angle;
  gboolean fit_allocation;
  gboolean scale_set;
  int      scale_factor;
  gboolean rotate_gesture_enabled;
  gboolean zoom_gesture_enabled;

  GtkGesture *rotate_gesture;
  gboolean    in_rotate;
  double      gesture_start_angle;

  GtkGesture *zoom_gesture;
  gboolean    in_zoom;
  double      gesture_start_scale;

  /* Current anchor point, or -1/-1.
   * Should never include the adjustment values */
  double      anchor_x;
  double      anchor_y;

  gboolean    size_valid;

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
  int                     surface_height;
  int                     animation_timeout;

  /* Transitions */
  gint64 angle_transition_start;
  double transition_start_angle;
  double transition_end_angle;
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

enum
{
  PREPARE_IMAGE,

  LAST_SIGNAL
};

static GParamSpec *widget_props[LAST_WIDGET_PROPERTY] = { NULL, };
static int         widget_signals[LAST_SIGNAL]        = { 0 };


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
                                                 int          *width,
                                                 int          *height,
                                                 double       *scale_out);

static inline void gtk_image_view_restrict_adjustment (GtkAdjustment *adjustment);
/* }}} */



static void
free_load_task_data (LoadTaskData *data)
{
  g_clear_object (&data->source);
}

  /*
   * XXX
   * The code is *slightly* wrong if fit-allocation is set,
   * but it doesn't make sense in that case anyway?
   * XXX
   */

static void
bounding_box_for_angle (GtkImageView *image_view,
                        double        angle,
                        int          *width,
                        int          *height,
                        double       *scale_out)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  GtkAllocation alloc;
  double image_width;
  double image_height;
  int bb_width  = 0;
  int bb_height = 0;
  double upper_right_degrees;
  double upper_left_degrees;
  double r;
  int upper_right_x, upper_right_y;
  int upper_left_x, upper_left_y;
  double scale;
  static int cached_width;
  static int cached_height;
  static double cached_scale;

  if (priv->size_valid)
    {
      *width = cached_width;
      *height = cached_height;
      if (scale_out)
        *scale_out = cached_scale;
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

  upper_right_degrees = DEG_TO_RAD (angle) + atan (image_height / image_width);
  upper_left_degrees  = DEG_TO_RAD (angle) + atan (image_height / -image_width);
  r = sqrtf ((image_width / 2) * (image_width / 2) + (image_height / 2) * (image_height / 2));

  upper_right_x = r * cos (upper_right_degrees);
  upper_right_y = r * sin (upper_right_degrees);

  upper_left_x = r * cos (upper_left_degrees);
  upper_left_y = r * sin (upper_left_degrees);


  bb_width  = MAX (fabs (upper_right_x), fabs (upper_left_x)) * 2;
  bb_height = MAX (fabs (upper_right_y), fabs (upper_left_y)) * 2;

  /* XXX The bounding box is 2px too small when fit-allocation is set */


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

  cached_scale = scale;
  if (scale_out)
    *scale_out = scale;

  if (priv->fit_allocation)
    {
      priv->scale = scale;
      g_object_notify_by_pspec (G_OBJECT (image_view),
                                widget_props[PROP_SCALE]);

      *width  = cached_width  = bb_width  * scale;
      *height = cached_height = bb_height * scale;
    }
  else
    {
      *width  = cached_width  = bb_width  * scale;
      *height = cached_height = bb_height * scale;
    }

  priv->size_valid = TRUE;

}


static void
gtk_image_view_fix_point_rotate (GtkImageView *image_view,
                                 double        angle_before,
                                 int           anchor_x,
                                 int           anchor_y)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  double new_angle = priv->angle;
  int center_x;
  int center_y;
  double value_x;
  double value_y;

  /*if (!priv->hadjustment || !priv->vadjustment)*/
    /*return;*/

  g_assert (priv->anchor_x != -1 && priv->anchor_y != -1);
/*


  /*g_message ("Angle before: %f", angle_before);*/
  /*g_message ("Angle new:    %f", priv->angle);*/


  bounding_box_for_angle (image_view,
                          /*angle_before,*/
                          priv->angle,
                          &center_x,
                          &center_y,
                          NULL);

  /*gtk_image_view_compute_bounding_box (image_view,*/
                                       /*&center_x, &center_y,*/
                                       /*NULL);*/

  center_x /= 2;
  center_y /= 2;

  if (priv->hadjustment && priv->vadjustment)
    {
      value_x = gtk_adjustment_get_value (priv->hadjustment);
      value_y = gtk_adjustment_get_value (priv->vadjustment);
    }
  else
    {
      /*g_assert_not_reached ();*/
      value_x = 0;
      value_y = 0;
    }

  /* Distance between the gesture bounding box center and
   * the rotation center */
  double delta_x =  (anchor_x + value_x) - center_x;
  double delta_y = -(anchor_y + value_y) + center_y;


  /*double delta_x =  anchor_x - center_x;*/
  /*double delta_y = -anchor_y + center_y;*/

  g_message ("Delta: %f/%f", delta_x, delta_y);

  double distance = sqrtf ((delta_x * delta_x) + (delta_y * delta_y));

  double point_angle = atan2 (delta_y, delta_x);

  double new_point_angle = RAD_TO_DEG (point_angle) - (new_angle - priv->gesture_start_angle);

  /* Relative to center_x/center_y! */
  double new_x = cos (DEG_TO_RAD (new_point_angle)) * distance;
  double new_y = sin (DEG_TO_RAD (new_point_angle)) * distance;


  _new_x = center_x + new_x;
  _new_y = center_y - new_y;

  /*_new_x = center_X + */


  g_message ("new x: %f, y: %f", _new_x, _new_y);

  static gboolean first = TRUE;
  static double hvalue;
  static double vvalue;

  if (first)
    {
      hvalue = gtk_adjustment_get_value (priv->hadjustment);
      vvalue = gtk_adjustment_get_value (priv->vadjustment);
      first = FALSE;
    }

  double diff_x = anchor_x - _new_x;
  g_message ("Y: %d, %f", anchor_y, _new_y);
  /*double diff_y = y_before - _new_y;*/
  double diff_y = _new_y - anchor_y;

  g_message ("Differences: %f/%f", diff_x, diff_y);

  if (priv->hadjustment && priv->vadjustment)
    {
      /*double x = gtk_adjustment_get_value (priv->hadjustment);*/
      /*double y = gtk_adjustment_get_value (priv->vadjustment);*/
      double x = hvalue;
      double y = vvalue;

      double new_x = x - diff_x;
      double new_y = y - diff_y;

      g_message ("hvalue: %f -> %f", gtk_adjustment_get_value (priv->hadjustment),
                 new_x);
      g_message ("vvalue: %f -> %f", gtk_adjustment_get_value (priv->vadjustment),
                 new_y);
      gtk_adjustment_set_value (priv->hadjustment,
                                new_x);
      gtk_adjustment_set_value (priv->vadjustment,
                                new_y);


      /*gtk_image_view_restrict_adjustment (priv->hadjustment);*/
      /*gtk_image_view_restrict_adjustment (priv->vadjustment);*/
      /*gtk_image_view_update_adjustments (image_view);*/
    }


  g_message ("-------------------------");
}

static void
gtk_image_view_fix_point (GtkImageView *image_view,
                          double        scale_before,
                          int           x_before,
                          int           y_before)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  double x_after;
  double y_after;
  double x_value;
  double y_value;

  g_assert (!(priv->hadjustment == NULL && priv->vadjustment == NULL));

  x_before += gtk_adjustment_get_value (priv->hadjustment);
  y_before += gtk_adjustment_get_value (priv->vadjustment);

  x_value = gtk_adjustment_get_value (priv->hadjustment);
  y_value = gtk_adjustment_get_value (priv->vadjustment);

  x_after = x_before / scale_before * priv->scale;
  y_after = y_before / scale_before * priv->scale;

  gtk_adjustment_set_value (priv->hadjustment,
                            x_value + x_after - x_before);
  gtk_adjustment_set_value (priv->vadjustment,
                            y_value + y_after - y_before);
}

static void
gesture_rotate_end_cb (GtkGesture       *gesture,
                       GdkEventSequence *sequence,
                       gpointer          user_data)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (user_data);

  priv->gesture_start_angle = 0.0;
  priv->in_rotate = FALSE;

  /*gtk_image_view_set_angle (user_data, priv->angle);*/

  /*priv->anchor_x = -1;*/
  /*priv->anchor_y = -1;*/
}

static void
gesture_rotate_cancel_cb (GtkGesture *gesture,
                          GdkEventSequence *sequence,
                          gpointer          user_data)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (user_data);
  gtk_image_view_set_angle (user_data, priv->gesture_start_angle);
  priv->in_rotate = FALSE;
  priv->gesture_start_angle = FALSE;

  /*priv->anchor_x = -1;*/
  /*priv->anchor_y = -1;*/
}


static void
gesture_angle_changed_cb (GtkGestureRotate *gesture,
                          double            angle,
                          double            delta,
                          GtkWidget        *widget)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (GTK_IMAGE_VIEW (widget));
  double new_angle;
  double old_angle;

  if (!priv->rotate_gesture_enabled)
    {
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
      return;
    }

  if (!priv->in_rotate)
    {
      priv->in_rotate = TRUE;
      priv->gesture_start_angle = priv->angle;
    }

  new_angle = priv->gesture_start_angle + RAD_TO_DEG (delta);

  if (new_angle == priv->angle)
    return;

  priv->size_valid = FALSE;

  /* Don't notify */
  old_angle = priv->angle;
  priv->angle = new_angle;
  gtk_image_view_update_adjustments (GTK_IMAGE_VIEW (widget));

  if (priv->hadjustment && priv->vadjustment)
    gtk_image_view_fix_point_rotate (GTK_IMAGE_VIEW (widget),
                                     old_angle,
                                     priv->anchor_x,
                                     priv->anchor_y);

  if (priv->fit_allocation)
    gtk_widget_queue_draw (widget);
  else
    gtk_widget_queue_resize (widget);
}

static void
gtk_image_view_compute_bounding_box (GtkImageView *image_view,
                                     int          *width,
                                     int          *height,
                                     double       *scale_out)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  GtkAllocation alloc;
  double image_width;
  double image_height;
  int bb_width  = 0;
  int bb_height = 0;
  double upper_right_degrees;
  double upper_left_degrees;
  double r;
  int upper_right_x, upper_right_y;
  int upper_left_x, upper_left_y;
  double scale;
  static int cached_width;
  static int cached_height;
  static double cached_scale;

  if (priv->size_valid)
    {
      *width = cached_width;
      *height = cached_height;
      if (scale_out)
        *scale_out = cached_scale;
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
  r = sqrtf ((image_width / 2) * (image_width / 2) + (image_height / 2) * (image_height / 2));

  upper_right_x = r * cos (upper_right_degrees);
  upper_right_y = r * sin (upper_right_degrees);

  upper_left_x = r * cos (upper_left_degrees);
  upper_left_y = r * sin (upper_left_degrees);


  bb_width  = MAX (fabs (upper_right_x), fabs (upper_left_x)) * 2;
  bb_height = MAX (fabs (upper_right_y), fabs (upper_left_y)) * 2;

  /* XXX The bounding box is 2px too small when fit-allocation is set */


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

  cached_scale = scale;
  if (scale_out)
    *scale_out = scale;

  if (priv->fit_allocation)
    {
      priv->scale = scale;
      g_object_notify_by_pspec (G_OBJECT (image_view),
                                widget_props[PROP_SCALE]);

      *width  = cached_width  = bb_width  * scale;
      *height = cached_height = bb_height * scale;
    }
  else
    {
      *width  = cached_width  = bb_width  * scale;
      *height = cached_height = bb_height * scale;
    }

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
      int width, height;
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
 * _fix_point call at the end, so we can choose the point
 * to fix.
 */
static void
gtk_image_view_set_scale_internal (GtkImageView *image_view,
                                   double        scale)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  scale = MAX (0, scale);

  priv->scale = scale;
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

  priv->size_valid = FALSE;

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
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (GTK_IMAGE_VIEW (widget));
  double new_scale;
  double old_scale = priv->scale;

  /*if (!priv->rotate_gesture_enabled)*/
    /*{*/
      /*gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);*/
      return;
    /*}*/

  if (!priv->in_zoom)
    {
      priv->in_zoom = TRUE;
      priv->gesture_start_scale = priv->scale;
    }

  new_scale = priv->gesture_start_scale * delta;

  /* Don't emit */
  priv->scale = new_scale;
  gtk_image_view_update_adjustments (GTK_IMAGE_VIEW (widget));

  gtk_image_view_set_scale_internal (GTK_IMAGE_VIEW (widget),
                                     new_scale);

  if (priv->hadjustment || priv->vadjustment)
    gtk_image_view_fix_point (GTK_IMAGE_VIEW (widget),
                              old_scale,
                              priv->anchor_x,
                              priv->anchor_y);
}

static void
gesture_begin_cb (GtkGesture       *gesture,
                  GdkEventSequence *sequence,
                  gpointer          user_data)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (user_data);

  if (priv->anchor_x == -1 && priv->anchor_y == -1)
    {
      gtk_gesture_get_bounding_box_center (gesture,
                                           &priv->anchor_x,
                                           &priv->anchor_y);

      priv->anchor_x = 50;
      priv->anchor_y = 50;
    }
}


static void
gtk_image_view_init (GtkImageView *image_view)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  GtkStyleContext *sc = gtk_widget_get_style_context (GTK_WIDGET (image_view));
  GtkWidget *widget = GTK_WIDGET (image_view);

  gtk_widget_set_can_focus (widget, TRUE);
  gtk_widget_set_has_window (widget, FALSE);

  _new_x = -1;
  _new_y = -1;

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
  g_signal_connect (priv->rotate_gesture, "begin", (GCallback)gesture_begin_cb, image_view);
  g_signal_connect (priv->rotate_gesture, "end", (GCallback)gesture_rotate_end_cb, image_view);
  g_signal_connect (priv->rotate_gesture, "cancel", (GCallback)gesture_rotate_cancel_cb, image_view);

  priv->zoom_gesture = gtk_gesture_zoom_new (widget);
  g_signal_connect (priv->zoom_gesture, "scale-changed", (GCallback)gesture_scale_changed_cb, image_view);
  g_signal_connect (priv->zoom_gesture, "begin", (GCallback)gesture_begin_cb, image_view);
  g_signal_connect (priv->zoom_gesture, "end", (GCallback)gesture_zoom_end_cb, image_view);
  g_signal_connect (priv->zoom_gesture, "cancel", (GCallback)gesture_zoom_cancel_cb, image_view);

  gtk_gesture_group (priv->zoom_gesture,
                     priv->rotate_gesture);

  gtk_style_context_add_class (sc, "image-view");
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
      return FALSE;
    }

  return TRUE;
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
  int draw_x;
  int draw_y;
  int draw_width;
  int draw_height;
  double scale = 0.0;
  int widget_width = gtk_widget_get_allocated_width (widget);
  int widget_height = gtk_widget_get_allocated_height (widget);


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


  int image_width  = cairo_image_surface_get_width (priv->image_surface)  * scale;
  int image_height = cairo_image_surface_get_height (priv->image_surface) * scale;


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


  /* XXX Debugging, Remove later. */
  if (priv->anchor_x != -1 && priv->anchor_y != -1)
    {
      double x = priv->anchor_x;
      double y = priv->anchor_y;

      cairo_set_source_rgba (ct, 0, 1, 0, 1);
      cairo_rectangle (ct, x - 2, y - 2, 4, 4);
      cairo_fill (ct);
    }

  if (_new_x != -1 && _new_y != -1)
    {
      double x = _new_x;
      double y = _new_y;

      cairo_set_source_rgba (ct, 0, 0, 1, 1);
      cairo_rectangle (ct, x - 2, y - 2, 4, 4);
      cairo_fill (ct);


      g_assert (priv->anchor_x != -1 && priv->anchor_y != -1);

      /*cairo_move_to (ct, priv->anchor_x, priv->anchor_y);*/
      /*cairo_line_to (ct, _new_x, _new_y);*/
      /*cairo_stroke (ct);*/
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
      g_signal_connect ((GObject *)hadjustment, "value-changed",
                        (GCallback) adjustment_value_changed_cb, image_view);
      priv->hadjustment = g_object_ref_sink (hadjustment);
    }
  else
    {
      priv->hadjustment = hadjustment;
    }

  g_object_notify ((GObject *)image_view, "hadjustment");

  gtk_image_view_update_adjustments (image_view);

  if (priv->fit_allocation)
    gtk_widget_queue_draw ((GtkWidget *)image_view);
  else
    gtk_widget_queue_resize ((GtkWidget *)image_view);

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

  g_object_notify ((GObject *)image_view, "vadjustment");

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
  double old_scale;
  double pointer_x;
  double pointer_y;

  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));
  g_return_if_fail (scale >= 0.0);

  old_scale = priv->scale;

  pointer_x = gtk_widget_get_allocated_width (GTK_WIDGET (image_view))  / 2;
  pointer_y = gtk_widget_get_allocated_height (GTK_WIDGET (image_view)) / 2;

  if (priv->hadjustment)
    pointer_x += gtk_adjustment_get_value (priv->hadjustment);

  if (priv->vadjustment)
    pointer_y += gtk_adjustment_get_value (priv->vadjustment);


  gtk_image_view_set_scale_internal (image_view, scale);

  if (priv->hadjustment != NULL && priv->vadjustment != NULL)
    gtk_image_view_fix_point (image_view,
                              old_scale,
                              pointer_x,
                              pointer_y);
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
 * @image_view:
 * @angle: The angle to rotate the image about, in
 *   degrees. If this is < 0 or > 360, the value wil
 *   be wrapped.
 */
void
gtk_image_view_set_angle (GtkImageView *image_view,
                          double        angle)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  double angle_before;
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));


  angle_before = priv->angle;

  if (angle > 360.0)
    angle -= (int)(angle / 360.0) * 360;
  else if (angle < 0.0)
    angle = 360.0 + (int)(angle / 360.0);

  g_assert (angle >= 0.0);
  g_assert (angle <= 360.0);

  if (priv->snap_angle)
    gtk_image_view_do_snapping (image_view, angle);
  else
    priv->angle = angle;

  priv->size_valid = FALSE;

  g_object_notify_by_pspec (G_OBJECT (image_view),
                            widget_props[PROP_ANGLE]);


  /* XXX DEBUG */
  /*if (priv->hadjustment && priv->vadjustment)*/
    /*{*/
      static gboolean first = TRUE;

      if (first)
        {
          priv->gesture_start_scale = angle_before;
          first = FALSE;
        }

      priv->anchor_x = 50 + (gtk_widget_get_allocated_width (GTK_WIDGET (image_view))) / 2 - 200;
      priv->anchor_y = 50 + (gtk_widget_get_allocated_height (GTK_WIDGET (image_view))) / 2 - 200;
      gtk_image_view_fix_point_rotate (image_view,
                                       angle_before,
                                       priv->anchor_x,
                                       priv->anchor_y);
    /*}*/



  gtk_image_view_update_adjustments (image_view);

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
  priv->size_valid = FALSE;

  priv->scale_set = FALSE;
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

  priv->rotate_gesture_enabled = rotate_gesture_enabled;
  g_object_notify_by_pspec (G_OBJECT (image_view),
                            widget_props[PROP_ROTATE_GESTURE_ENABLED]);
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

  priv->zoom_gesture_enabled = zoom_gesture_enabled;
  g_object_notify_by_pspec (G_OBJECT (image_view),
                            widget_props[PROP_ZOOM_GESTURE_ENABLED]);
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
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private ((GtkImageView *)widget);
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

  int width, height;
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
  int width, height;
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
  double old_scale = priv->scale;
  double new_scale = priv->scale - (0.1 * event->delta_y);

  gtk_image_view_set_scale_internal (image_view, new_scale);

  if (priv->hadjustment || priv->vadjustment)
    gtk_image_view_fix_point (image_view,
                              old_scale,
                              event->x,
                              event->y);

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
      /*case PROP_SCALE_SET:*/
        /*break;*/
      case PROP_ANGLE:
        gtk_image_view_set_angle (image_view, g_value_get_double (value));
        break;
      case PROP_SNAP_ANGLE:
        gtk_image_view_set_snap_angle (image_view, g_value_get_boolean (value));
        break;
      case PROP_FIT_ALLOCATION:
        gtk_image_view_set_fit_allocation (image_view, g_value_get_boolean (value));
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
      /*case PROP_SCALE_SET:*/
        /*g_value_set_boolean (value, priv->scale_set);*/
        /*break;*/
      case PROP_ANGLE:
        g_value_set_double (value, priv->angle);
        break;
      case PROP_SNAP_ANGLE:
        g_value_set_boolean (value, priv->snap_angle);
        break;
      case PROP_FIT_ALLOCATION:
        g_value_set_boolean (value, priv->fit_allocation);
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
   * Since: 3.18
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
   * Since: 3.18
   */
  widget_props[PROP_SCALE_SET] = g_param_spec_boolean ("scale-set",
                                                       P_(""),
                                                       P_("fooar"),
                                                       FALSE,
                                                       GTK_PARAM_READWRITE|G_PARAM_EXPLICIT_NOTIFY);
  /**
   * GtkImageView:angle:
   * The angle the surface gets rotated about.
   * This is in degrees and we rotate in the mathematically negative direction,
   * i.e. clock wise.
   *
   * Since: 3.18
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
   * Since: 3.18
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
   * Since: 3.18
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
   * Since: 3.18
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
   * Since: 3.18
   */
  widget_props[PROP_FIT_ALLOCATION] = g_param_spec_boolean ("fit-allocation",
                                                            P_("Foo"),
                                                            P_("fooar"),
                                                            FALSE,
                                                            GTK_PARAM_READWRITE|G_PARAM_EXPLICIT_NOTIFY);


  /**
   * GtkImageView::prepare-image:
   * @image_view: A #GtkImageView instance
   * @surface: A #cairo_surface_t of type #CAIRO_TYPE_IMAGE_SURFACE.
   */
  widget_signals[PREPARE_IMAGE] = g_signal_new (I_("prepare-image"),
                                                G_TYPE_FROM_CLASS (object_class),
                                                G_SIGNAL_RUN_LAST,
                                                G_STRUCT_OFFSET (GtkImageViewClass, prepare_image),
                                                NULL, NULL, NULL,
                                                G_TYPE_NONE, 1, CAIRO_GOBJECT_TYPE_SURFACE);

  g_object_class_install_properties (object_class, LAST_WIDGET_PROPERTY, widget_props);

  g_object_class_override_property (object_class, PROP_HADJUSTMENT,    "hadjustment");
  g_object_class_override_property (object_class, PROP_VADJUSTMENT,    "vadjustment");
  g_object_class_override_property (object_class, PROP_HSCROLL_POLICY, "hscroll-policy");
  g_object_class_override_property (object_class, PROP_VSCROLL_POLICY, "vscroll-policy");
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
    {
      cairo_surface_reference (priv->image_surface);

      g_signal_emit (image_view, widget_signals[PREPARE_IMAGE], 0, priv->image_surface);
    }
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
  gboolean resize  = TRUE;
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
      /* replace_surface will emit prepare-image */
      gtk_image_view_replace_surface (image_view,
                                      new_surface,
                                      scale_factor);
    }
  else
    {
      gdk_cairo_surface_paint_pixbuf (priv->image_surface, frame);
      g_signal_emit (image_view, widget_signals[PREPARE_IMAGE], 0, priv->image_surface);
      resize = FALSE;
    }
  g_assert (priv->image_surface != NULL);

  if (resize)
    gtk_widget_queue_resize (GTK_WIDGET (image_view));
  else
    gtk_widget_queue_draw (GTK_WIDGET (image_view));
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
}

/**
 * gtk_image_view_set_surface:
 * @image_view: A #GtkImageView instance
 * @surface: (nullable): A #cairo_surface_t of type #CAIRO_SURFACE_TYPE_IMAGE, or
 *   #NULL to unset any internal image data. In case this is #NULL, the scale will
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
