/* Image View
 */
#include <gtk/gtk.h>

GtkWidget *image_view;
GtkWidget *uri_entry;

void
generic_cb ()
{
  g_message ("Generic");
}

void
file_set_cb (GtkFileChooserButton *widget,
             gpointer              user_data)
{
  char *filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (widget));
  GFile *file = g_file_new_for_path (filename);
  gtk_image_view_load_from_file_async (GTK_IMAGE_VIEW (image_view),
                                       file,
                                       2,
                                       NULL,
                                       generic_cb,
                                       NULL);

  g_free (filename);
}


static void
image_loaded_cb (GObject      *source_object,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  GtkImageView *image_view = GTK_IMAGE_VIEW (source_object);
  GError *error = NULL;

  gtk_image_view_load_from_file_finish (image_view,
                                        result,
                                        &error);

  if (error)
    {
      g_message ("Error: %s", error->message);
      return;
    }
}

void
load_button_cb ()
{
  const char *uri = gtk_entry_get_text (GTK_ENTRY (uri_entry));
  GFile *file = g_file_new_for_uri (uri);
  gtk_image_view_load_from_file_async (GTK_IMAGE_VIEW (image_view),
                                       file,
                                       1,
                                       NULL,
                                       image_loaded_cb,
                                       NULL);
}

void
angle_changed_cb (GtkRange *range,
                  gpointer  user_data)
{
  double value = gtk_range_get_value (range);

  gtk_image_view_set_angle (GTK_IMAGE_VIEW (image_view), value);
}

void
scale_changed_cb (GtkRange *range,
                  gpointer user_data)
{
  double value = gtk_range_get_value (range);

  gtk_image_view_set_scale (GTK_IMAGE_VIEW (image_view), value);
}

void
rotate_left_clicked_cb ()
{
  double current_angle = gtk_image_view_get_angle (GTK_IMAGE_VIEW (image_view));

  gtk_image_view_set_angle (GTK_IMAGE_VIEW (image_view), current_angle - 90);
}


void
rotate_right_clicked_cb ()
{
  double current_angle = gtk_image_view_get_angle (GTK_IMAGE_VIEW (image_view));

  gtk_image_view_set_angle (GTK_IMAGE_VIEW (image_view), current_angle + 90);
}

void
scrolled_check_button_active_cb (GObject *source)
{
  GtkWidget *parent = gtk_widget_get_parent (image_view);

  if (GTK_IS_SCROLLED_WINDOW (parent))
    {
      GtkWidget *grandparent = gtk_widget_get_parent (parent);
      g_assert (grandparent != NULL);
      g_object_ref (G_OBJECT (image_view));
      gtk_container_remove (GTK_CONTAINER (parent), image_view);
      gtk_container_remove (GTK_CONTAINER (grandparent), parent);
      gtk_container_add (GTK_CONTAINER (grandparent), image_view);
      gtk_widget_show (image_view);
      g_object_unref (G_OBJECT (image_view));
    }
  else
    {
      GtkWidget *scroller = gtk_scrolled_window_new (NULL, NULL);
      gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                      GTK_POLICY_ALWAYS,
                                      GTK_POLICY_ALWAYS);
      gtk_scrolled_window_set_overlay_scrolling (GTK_SCROLLED_WINDOW (scroller), FALSE);
      gtk_widget_show (scroller);
      gtk_container_remove (GTK_CONTAINER (parent), image_view);
      gtk_container_add (GTK_CONTAINER (scroller), image_view);
      gtk_container_add (GTK_CONTAINER (parent), scroller);
    }
}

gchar *
angle_scale_format_value_cb (GtkScale *scale,
                             double    value,
                             gpointer  user_data)
{
  return g_strdup_printf ("%f°", value);
}


gchar *
scale_scale_format_value_cb (GtkScale *scale,
                             double    value,
                             gpointer  user_data)
{
  return g_strdup_printf ("%f", value);
}


void
load_pixbuf_button_clicked_cb ()
{
  GdkPixbuf *pixbuf;

  /* I really hope you have this. */
  pixbuf = gdk_pixbuf_new_from_file ("/home/baedert/imageview2.png",
                                     NULL);

  g_assert (pixbuf != NULL);
  gtk_image_view_set_pixbuf (GTK_IMAGE_VIEW (image_view), pixbuf, 2);

  g_object_unref (G_OBJECT (pixbuf));
}


void
load_hidpi_pixbuf_button_clicked_cb ()
{
  GdkPixbuf *pixbuf;

  /* I really hope you have this. */
  pixbuf = gdk_pixbuf_new_from_file ("/usr/share/backgrounds/gnome/adwaita-day.jpg",
                                     NULL);

  g_assert (pixbuf != NULL);
  gtk_image_view_set_pixbuf (GTK_IMAGE_VIEW (image_view), pixbuf, 2);

  g_object_unref (G_OBJECT (pixbuf));
}

void
load_surface_button_clicked_cb ()
{
  GdkPixbuf *pixbuf;
  cairo_surface_t *surface;

  /* I really hope you have this. */
  pixbuf = gdk_pixbuf_new_from_file ("/usr/share/backgrounds/gnome/adwaita-day.jpg",
                                     NULL);

  g_assert (pixbuf != NULL);

  surface = gdk_cairo_surface_create_from_pixbuf (pixbuf, 1, NULL);

  g_object_unref (G_OBJECT (pixbuf));

  gtk_image_view_set_surface (GTK_IMAGE_VIEW (image_view), surface);
}


void
clear_button_clicked_cb ()
{
  gtk_image_view_set_surface (GTK_IMAGE_VIEW (image_view), NULL);
}

void
prepare_image_cb (GtkImageView    *image_view,
                  cairo_surface_t *surface)
{
  cairo_t *ct;
  int width;
  int height;

  g_assert (GTK_IS_IMAGE_VIEW (image_view));

  g_assert (surface != NULL);
  g_assert (cairo_surface_get_type (surface) == CAIRO_SURFACE_TYPE_IMAGE);


  ct = cairo_create (surface);
  width  = cairo_image_surface_get_width (surface);
  height = cairo_image_surface_get_height (surface);


  cairo_set_source_rgba (ct, 0, 1, 0, 1);
  cairo_set_line_width (ct, 5.0);
  cairo_rectangle (ct, 0, 0, width, height);
  cairo_stroke (ct);
}


GtkWidget *
do_image_view (GtkWidget *do_widget)
{
  GtkWidget *window   = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  GtkBuilder *builder = gtk_builder_new_from_resource ("/imageview/image_view.ui");
  GtkWidget *box      = GTK_WIDGET (gtk_builder_get_object (builder, "box"));
  GtkWidget *snap_angle_button = GTK_WIDGET (gtk_builder_get_object (builder, "snap_angle_check_button"));
  GtkWidget *fit_allocation_button = GTK_WIDGET (gtk_builder_get_object (builder, "fit_allocation_check_button"));
  GtkWidget *rotate_gesture_button = GTK_WIDGET (gtk_builder_get_object (builder, "rotate_gesture_check_button"));
  GtkWidget *zoom_gesture_button = GTK_WIDGET (gtk_builder_get_object (builder, "zoom_gesture_check_button"));

  GtkAdjustment *scale_adjustment = GTK_ADJUSTMENT (gtk_builder_get_object (builder, "scale_adjustment"));
  GtkAdjustment *angle_adjustment = GTK_ADJUSTMENT (gtk_builder_get_object (builder, "angle_adjustment"));
         image_view   = GTK_WIDGET (gtk_builder_get_object (builder, "image_view"));
          uri_entry   = GTK_WIDGET (gtk_builder_get_object (builder, "uri_entry"));


  /*gtk_window_set_title (GTK_WINDOW (window), "blue: current, green: anchor");*/

  g_object_bind_property (scale_adjustment, "value", image_view, "scale",
                          G_BINDING_BIDIRECTIONAL);
  g_object_bind_property (image_view, "angle", angle_adjustment, "value",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
  g_object_bind_property (image_view, "snap-angle", snap_angle_button, "active",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
  g_object_bind_property (image_view, "fit-allocation", fit_allocation_button, "active",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

  g_object_bind_property (image_view, "rotate-gesture-enabled",
                          rotate_gesture_button, "active",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

  g_object_bind_property (image_view, "zoom-gesture-enabled",
                          zoom_gesture_button, "active",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);



  gtk_container_add (GTK_CONTAINER (window), box);
  gtk_builder_connect_signals (builder, NULL);

  gtk_window_resize (GTK_WINDOW (window), 800, 600);
  gtk_widget_show_all (window);

  g_object_unref (builder);

  return window;
}
