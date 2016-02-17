/*  Copyright 2016 Timm Bäder
 *
 * GTK+ is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * GLib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GTK+; see the file COPYING.  If not,
 * see <http://www.gnu.org/licenses/>.
 */

#ifndef __GTK_ABSTRACT_IMAGE_H__
#define __GTK_ABSTRACT_IMAGE_H__

#if !defined (__GTK_H_INSIDE__) && !defined (GTK_COMPILATION)
#error "Only <gtk/gtk.h> can be included directly."
#endif

#include <gtk/gtkwidget.h>

G_BEGIN_DECLS

typedef struct _GtkAbstractImage GtkAbstractImage;
typedef struct _GtkAbstractImageClass GtkAbstractImageClass;

#define GTK_TYPE_ABSTRACT_IMAGE           (gtk_abstract_image_get_type ())
#define GTK_ABSTRACT_IMAGE(obj)           (G_TYPE_CHECK_INSTANCE_CAST (obj, GTK_TYPE_ABSTRACT_IMAGE, GtkAbstractImage))
#define GTK_ABSTRACT_IMAGE_CLASS(cls)     (G_TYPE_CHECK_CLASS_CAST (cls, GTK_TYPE_ABSTRACT_IMAGE, GtkAbstractImageClass))
#define GTK_IS_ABSTRACT_IMAGE(obj)        (G_TYPE_CHECK_INSTANCE_TYPE (obj, GTK_TYPE_ABSTRACT_IMAGE))
#define GTK_IS_ABSTRACT_IMAGE_CLASS(obj)  (G_TYPE_CHECK_CLASS_TYPE (obj, GTK_TYPE_ABSTRACT_IMAGE))
#define GTK_ABSTRACT_IMAGE_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), GTK_TYPE_ABSTRACT_IMAGE, GtkAbstractImageClass))


struct _GtkAbstractImage
{
  GObject parent;
};


struct _GtkAbstractImageClass
{
  GObjectClass parent_class;
  int    (*get_width) (GtkAbstractImage *image);
  int    (*get_height) (GtkAbstractImage *image);
  int    (*get_scale_factor) (GtkAbstractImage *image);
  void   (*draw) (GtkAbstractImage *image, cairo_t *ct);
};

GDK_AVAILABLE_IN_3_20
GType gtk_abstract_image_get_type (void) G_GNUC_CONST;

int gtk_abstract_image_get_width (GtkAbstractImage *image);

int gtk_abstract_image_get_height (GtkAbstractImage *image);

void gtk_abstract_image_draw (GtkAbstractImage *image, cairo_t *ct);


typedef struct _GtkPixbufImage GtkPixbufImage;
typedef struct _GtkPixbufImageClass GtkPixbufImageClass;

#define GTK_TYPE_PIXBUF_IMAGE           (gtk_pixbuf_image_get_type ())
#define GTK_PIXBUF_IMAGE(obj)           (G_TYPE_CHECK_INSTANCE_CAST (obj, GTK_TYPE_PIXBUF_IMAGE, GtkPixbufImage))
#define GTK_PIXBUF_IMAGE_CLASS(cls)     (G_TYPE_CHECK_CLASS_CAST (cls, GTK_TYPE_PIXBUF_IMAGE, GtkPixbufImageClass))
#define GTK_IS_pixbuf_image(obj)        (G_TYPE_CHECK_INSTANCE_TYPE (obj, GTK_TYPE_PIXBUF_IMAGE))
#define GTK_IS_pixbuf_image_CLASS(obj)  (G_TYPE_CHECK_CLASS_TYPE (obj, GTK_TYPE_PIXBUF_IMAGE))
#define GTK_PIXBUF_IMAGE_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), GTK_TYPE_PIXBUF_IMAGE, GtkPixbufImageClass))

struct _GtkPixbufImage
{
  GtkAbstractImage parent;
  GdkPixbuf *pixbuf;
  int scale_factor;
};

struct _GtkPixbufImageClass
{
  GtkAbstractImageClass parent_class;
};

GDK_AVAILABLE_IN_3_20
GtkPixbufImage *gtk_pixbuf_image_new (const char *path, int scale_factor);



G_END_DECLS

#endif
