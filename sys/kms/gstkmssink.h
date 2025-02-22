/* GStreamer
 *
 * Copyright (C) 2016 Igalia
 *
 * Authors:
 *  Víctor Manuel Jáquez Leal <vjaquez@igalia.com>
 *  Javier Martin <javiermartin@by.com.es>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#ifndef __GST_KMS_SINK_H__
#define __GST_KMS_SINK_H__

#include <drm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_mode_imx.h>

#include <gst/video/gstvideosink.h>

G_BEGIN_DECLS

#define GST_TYPE_KMS_SINK \
  (gst_kms_sink_get_type())
#define GST_KMS_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_KMS_SINK, GstKMSSink))
#define GST_KMS_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_KMS_SINK, GstKMSSinkClass))
#define GST_IS_KMS_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_KMS_SINK))
#define GST_IS_KMS_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_KMS_SINK))

#define DEFAULT_HOLD_BUFFER_NUM 2

typedef struct _GstKMSSink GstKMSSink;
typedef struct _GstKMSSinkClass GstKMSSinkClass;

struct _GstKMSSink {
  GstVideoSink videosink;

  /*< private >*/
  gint fd;
  gint ctrl_fd;
  gint conn_id;
  gint crtc_id;
  gint plane_id;
  gint primary_plane_id;
  guint pipe;

  /* fps print support */
  guint64 frame_showed;
  GstClockTime run_time;

  /* crtc data */
  guint16 hdisplay, vdisplay;
  guint32 buffer_id;

  /* capabilities */
  gboolean has_prime_import;
  gboolean has_async_page_flip;
  gboolean can_scale;
  gboolean scale_checked;
  gint upscale_ratio;
  gint downscale_ratio;

  /* global alpha */
  gboolean is_kmsproperty_set;
  guint global_alpha;

  gboolean modesetting_enabled;
  gboolean display_connected;
  gboolean hantro_tile_enabled;

  /* hdr10 support */
  struct hdr_static_metadata hdr10meta;

  GstVideoInfo vinfo;
  GstCaps *allowed_caps;
  GstBufferPool *pool;
  GstAllocator *allocator;
  GstBuffer *last_buffer;
  GstBuffer *hold_buf[DEFAULT_HOLD_BUFFER_NUM];
  GstMemory *tmp_kmsmem;

  gchar *devname;

  guint32 mm_width, mm_height;

  GstPoll *poll;
  GstPollFD pollfd;

  /* preferred video rectangle */
  GstVideoRectangle preferred_rect;
  GstVideoRectangle last_rect;

  /* reconfigure info if driver doesn't scale */
  GstVideoRectangle pending_rect;
  gboolean reconfigure;

  /* kept original video size */
  gint original_width;
  gint original_heigth;

};

struct _GstKMSSinkClass {
  GstVideoSinkClass parent_class;
};

GType gst_kms_sink_get_type (void) G_GNUC_CONST;

G_END_DECLS

#endif /* __GST_KMS_SINK_H__ */
