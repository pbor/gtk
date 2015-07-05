#ifndef __GTK_IMAGE_VIEW_H__
#define __GTK_IMAGE_VIEW_H__

#if !defined (__GTK_H_INSIDE__) && !defined (GTK_COMPILATION)
#error "Only <gtk/gtk.h> can be included directly."
#endif

#include <gtk/gtkwidget.h>


G_BEGIN_DECLS


#define GTK_TYPE_IMAGE_VIEW            (gtk_image_view_get_type ())
#define GTK_IMAGE_VIEW(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), GTK_TYPE_IMAGE_VIEW, GtkImageView))
#define GTK_IMAGE_VIEW_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), GTK_TYPE_IMAGE_VIEW, GtkImageViewClass))
#define GTK_IS_IMAGE_VIEW(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GTK_TYPE_IMAGE_VIEW))
#define GTK_IS_IMAGE_VIEW_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), GTK_TYPE_IMAGE_VIEW))
#define GTK_IMAGE_VIEW_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), GTK_TYPE_IMAGE_VIEW, GtkImageViewClass))


typedef struct _GtkImageView              GtkImageView;
typedef struct _GtkImageViewPrivate       GtkImageViewPrivate;
typedef struct _GtkImageViewClass         GtkImageViewClass;


struct _GtkImageView
{
  GtkWidget parent_instance;
};

struct _GtkImageViewClass
{
  GtkWidgetClass parent_class;
  void (* prepare_image) (cairo_surface_t *image);
};

typedef enum
{
  GTK_IMAGE_VIEW_ZOOM_MODE_ORIGINAL,
  GTK_IMAGE_VIEW_ZOOM_MODE_FIT,
  GTK_IMAGE_VIEW_ZOOM_MODE_NONE
} GtkImageViewZoomMode;


GDK_AVAILABLE_IN_3_18
GType         gtk_image_view_get_type (void) G_GNUC_CONST;

GDK_AVAILABLE_IN_3_18
GtkWidget *   gtk_image_view_new      ();


/* Loading {{{ */

GDK_AVAILABLE_IN_3_18
void          gtk_image_view_load_from_file_async    (GtkImageView        *image_view,
                                                      GFile               *file,
                                                      GCancellable        *cancellable,
                                                      GAsyncReadyCallback  callback,
                                                      gpointer             user_data);
GDK_AVAILABLE_IN_3_18
void          gtk_image_view_load_from_file_finish   (GtkImageView  *image_view,
                                                      GAsyncResult  *result,
                                                      GError       **error);
GDK_AVAILABLE_IN_3_18
void          gtk_image_view_load_from_stream_async  (GtkImageView  *image_view,
                                                      GInputStream  *stream);
GDK_AVAILABLE_IN_3_18
void          gtk_image_view_load_from_stream_finish (GtkImageView  *image_view,
                                                      GAsyncResult  *result,
                                                      GError       **error);

/* }}} */

/* Setters/Getters {{{ */
GDK_AVAILABLE_IN_3_18
void   gtk_image_view_set_scale (GtkImageView *image_view, double scale);

GDK_AVAILABLE_IN_3_18
double gtk_image_view_get_scale (GtkImageView *image_view);



GDK_AVAILABLE_IN_3_18
void  gtk_image_view_set_angle (GtkImageView *image_view, double angle);

GDK_AVAILABLE_IN_3_18
double gtk_image_view_get_angle (GtkImageView *image_view);



GDK_AVAILABLE_IN_3_18
void gtk_image_view_set_zoom_mode (GtkImageView         *image_view,
                                   GtkImageViewZoomMode  zoom_mode);

GDK_AVAILABLE_IN_3_18
GtkImageViewZoomMode gtk_image_view_get_zoom_mode (GtkImageView *image_view);



GDK_AVAILABLE_IN_3_18
gboolean gtk_image_view_snap_angle (GtkImageView *image_view);

GDK_AVAILABLE_IN_3_18
void gtk_image_view_set_snap_angle (GtkImageView *image_view,
                                    gboolean      snap_rotation);



// XXX Adding a gtk_image_view_set_pixbuf would work, but we are working with animations internally...



/* }}} */

G_END_DECLS

#endif
