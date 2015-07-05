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
                                       NULL,
                                       generic_cb,
                                       NULL);
}

void
load_button_cb ()
{
  const char *uri = gtk_entry_get_text (GTK_ENTRY (uri_entry));
  GFile *file = g_file_new_for_uri (uri);
  gtk_image_view_load_from_file_async (GTK_IMAGE_VIEW (image_view),
                                       file,
                                       NULL,
                                       generic_cb,
                                       NULL);
}

void
zoom_mode_changed_cb (GtkComboBox *widget,
                      gpointer     user_data)
{
  const gchar *new_id = gtk_combo_box_get_active_id (widget);

  if (g_strcmp0 (new_id, "fit") == 0)
    gtk_image_view_set_zoom_mode (GTK_IMAGE_VIEW (image_view),
                                  GTK_IMAGE_VIEW_ZOOM_MODE_FIT);
  else if (g_strcmp0 (new_id, "original") == 0)
    gtk_image_view_set_zoom_mode (GTK_IMAGE_VIEW (image_view),
                                  GTK_IMAGE_VIEW_ZOOM_MODE_ORIGINAL);
  else
    g_error (new_id);

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
      gtk_container_remove (GTK_CONTAINER (grandparent), parent);
      gtk_container_add (GTK_CONTAINER (grandparent), image_view);
      gtk_widget_show (image_view);
    }
  else
    {
      GtkWidget *scroller = gtk_scrolled_window_new (NULL, NULL);
      gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                      GTK_POLICY_ALWAYS,
                                      GTK_POLICY_ALWAYS);
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




GtkWidget *
do_image_view (GtkWidget *do_widget)
{
  GtkWidget *window   = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  GtkBuilder *builder = gtk_builder_new_from_resource ("/imageview/image_view.ui");
         image_view   = GTK_WIDGET (gtk_builder_get_object (builder, "image_view"));
          uri_entry   = GTK_WIDGET (gtk_builder_get_object (builder, "uri_entry"));
  GtkWidget *box      = GTK_WIDGET (gtk_builder_get_object (builder, "box"));
  GtkWidget *zoom_mode_combo   = GTK_WIDGET (gtk_builder_get_object (builder, "zoom_mode_combo"));
  GtkWidget *snap_angle_button = GTK_WIDGET (gtk_builder_get_object (builder, "snap_angle_check_button"));
  GtkWidget *header_bar = gtk_header_bar_new ();
  gtk_header_bar_set_show_close_button (GTK_HEADER_BAR (header_bar), TRUE);
  gtk_window_set_titlebar (GTK_WINDOW (window), header_bar);

  GtkAdjustment *scale_adjustment = GTK_ADJUSTMENT (gtk_builder_get_object (builder, "scale_adjustment"));
  GtkAdjustment *angle_adjustment = GTK_ADJUSTMENT (gtk_builder_get_object (builder, "angle_adjustment"));

  g_object_bind_property (scale_adjustment, "value", image_view, "scale",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
  g_object_bind_property (image_view, "angle", angle_adjustment, "value",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
  g_object_bind_property (image_view, "snap-angle", snap_angle_button, "active",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);



  if (gtk_image_view_get_zoom_mode (GTK_IMAGE_VIEW (image_view)) == GTK_IMAGE_VIEW_ZOOM_MODE_FIT)
    gtk_combo_box_set_active_id (GTK_COMBO_BOX (zoom_mode_combo), "fit");
  else if (gtk_image_view_get_zoom_mode (GTK_IMAGE_VIEW (image_view)) == GTK_IMAGE_VIEW_ZOOM_MODE_ORIGINAL)
    gtk_combo_box_set_active_id (GTK_COMBO_BOX (zoom_mode_combo), "original");

  gtk_container_add (GTK_CONTAINER (window), box);
  gtk_builder_connect_signals (builder, NULL);

  gtk_window_resize (GTK_WINDOW (window), 800, 600);
  gtk_widget_show_all (window);
  return window;
}
