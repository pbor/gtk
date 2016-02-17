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

#include "config.h"
#include "gtkabstractimage.h"


G_DEFINE_ABSTRACT_TYPE (GtkAbstractImage, gtk_abstract_image, G_TYPE_OBJECT)

/* GtkAbstractImage {{{ */

static void
gtk_abstract_image_init (GtkAbstractImage *image)
{}

static void
gtk_abstract_image_class_init (GtkAbstractImageClass *klass)
{}

int
gtk_abstract_image_get_width (GtkAbstractImage *image)
{
  g_return_val_if_fail (GTK_IS_ABSTRACT_IMAGE (image), 0);

  return GTK_ABSTRACT_IMAGE_GET_CLASS (image)->get_width (image);
}

int
gtk_abstract_image_get_height (GtkAbstractImage *image)
{
  g_return_val_if_fail (GTK_IS_ABSTRACT_IMAGE (image), 0);

  return GTK_ABSTRACT_IMAGE_GET_CLASS (image)->get_height (image);
}

void
gtk_abstract_image_draw (GtkAbstractImage *image, cairo_t *ct)
{
  g_return_if_fail (GTK_IS_ABSTRACT_IMAGE (image));

  GTK_ABSTRACT_IMAGE_GET_CLASS (image)->draw (image, ct);
}

/* }}} */


/* GtkPixbufImage {{{ */

G_DEFINE_TYPE (GtkPixbufImage, gtk_pixbuf_image, GTK_TYPE_ABSTRACT_IMAGE)


GtkPixbufImage *
gtk_pixbuf_image_new (const char *path, int scale_factor)
{
  GtkPixbufImage *image = g_object_new (GTK_TYPE_PIXBUF_IMAGE, NULL);
  image->scale_factor = scale_factor;
  image->pixbuf = gdk_pixbuf_new_from_file (path, NULL);

  return image;
}

static int
gtk_pixbuf_image_get_width (GtkAbstractImage *image)
{
  return gdk_pixbuf_get_width (GTK_PIXBUF_IMAGE (image)->pixbuf);
}

static int
gtk_pixbuf_image_get_height (GtkAbstractImage *image)
{
  return gdk_pixbuf_get_height (GTK_PIXBUF_IMAGE (image)->pixbuf);
}

static int
gtk_pixbuf_image_get_scale_factor (GtkAbstractImage *image)
{
  return GTK_PIXBUF_IMAGE (image)->scale_factor;
}

static void
gtk_pixbuf_image_draw (GtkAbstractImage *image, cairo_t *ct)
{
  cairo_surface_t *surface = gdk_cairo_surface_create_from_pixbuf (
      GTK_PIXBUF_IMAGE (image)->pixbuf, 1, NULL);

  cairo_set_source_surface (ct, surface, 0, 0);
}


static void
gtk_pixbuf_image_init (GtkPixbufImage *image)
{
}

static void
gtk_pixbuf_image_class_init (GtkPixbufImageClass *klass)
{
  GtkAbstractImageClass *image_class = GTK_ABSTRACT_IMAGE_CLASS (klass);

  image_class->get_width = gtk_pixbuf_image_get_width;
  image_class->get_height = gtk_pixbuf_image_get_height;
  image_class->get_scale_factor = gtk_pixbuf_image_get_scale_factor;
  image_class->draw = gtk_pixbuf_image_draw;
}


/* }}} */
