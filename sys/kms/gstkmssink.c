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

/**
 * SECTION:element-kmssink
 * @title: kmssink
 * @short_description: A KMS/DRM based video sink
 *
 * kmssink is a simple video sink that renders video frames directly
 * in a plane of a DRM device.
 *
 * ## Example launch line
 * |[
 * gst-launch-1.0 videotestsrc ! kmssink
 * ]|
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/video/video.h>
#include <gst/video/videooverlay.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/allocators/gstdmabufmeta.h>
#include <gst/allocators/gstphymemmeta.h>

#include <stdint.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc_imx.h>

#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/version.h>

#include "gstkmssink.h"
#include "gstkmsutils.h"
#include "gstkmsbufferpool.h"
#include "gstkmsallocator.h"
#include "gstimxcommon.h"

#include <gst/video/gstvideohdr10meta.h>

#define GST_PLUGIN_NAME "kmssink"
#define GST_PLUGIN_DESC "Video sink using the Linux kernel mode setting API"

GST_DEBUG_CATEGORY_STATIC (gst_kms_sink_debug);
GST_DEBUG_CATEGORY_STATIC (CAT_PERFORMANCE);
#define GST_CAT_DEFAULT gst_kms_sink_debug

static GstFlowReturn gst_kms_sink_show_frame (GstVideoSink * vsink,
    GstBuffer * buf);
static void gst_kms_sink_video_overlay_init (GstVideoOverlayInterface * iface);
static void gst_kms_sink_drain (GstKMSSink * self);
static void ensure_kms_allocator (GstKMSSink * self);

#define parent_class gst_kms_sink_parent_class
G_DEFINE_TYPE_WITH_CODE (GstKMSSink, gst_kms_sink, GST_TYPE_VIDEO_SINK,
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, GST_PLUGIN_NAME, 0,
        GST_PLUGIN_DESC);
    GST_DEBUG_CATEGORY_GET (CAT_PERFORMANCE, "GST_PERFORMANCE");
    G_IMPLEMENT_INTERFACE (GST_TYPE_VIDEO_OVERLAY,
        gst_kms_sink_video_overlay_init));

enum
{
  PROP_DRIVER_NAME = 1,
  PROP_BUS_ID,
  PROP_CONNECTOR_ID,
  PROP_PLANE_ID,
  PROP_FORCE_MODESETTING,
  PROP_CAN_SCALE,
  PROP_DISPLAY_WIDTH,
  PROP_DISPLAY_HEIGHT,
  PROP_GLOBAL_ALPHA,
  PROP_FORCE_HANTROTILE,
  PROP_N
};

static GParamSpec *g_properties[PROP_N] = { NULL, };

#define SCALE_RATIO_NO_LIMITATION 100000

static void
gst_kms_sink_set_render_rectangle (GstVideoOverlay * overlay,
    gint x, gint y, gint width, gint height)
{
  GstKMSSink *self = GST_KMS_SINK (overlay);

  GST_DEBUG_OBJECT (self, "Setting render rectangle to (%d,%d) %dx%d", x, y,
      width, height);

  GST_OBJECT_LOCK (self);

  if (width == -1 && height == -1) {
    x = 0;
    y = 0;
    width = self->hdisplay;
    height = self->vdisplay;
  }

  if (width <= 0 || height <= 0)
    goto done;

  self->pending_rect.x = x;
  self->pending_rect.y = y;
  self->pending_rect.w = width;
  self->pending_rect.h = height;

  if (self->can_scale ||
      (self->render_rect.w == width && self->render_rect.h == height)) {
    self->render_rect = self->pending_rect;
  } else {
    self->reconfigure = TRUE;
    GST_DEBUG_OBJECT (self, "Waiting for new caps to apply render rectangle");
  }

done:
  GST_OBJECT_UNLOCK (self);
}

static void
gst_kms_sink_expose (GstVideoOverlay * overlay)
{
  GstKMSSink *self = GST_KMS_SINK (overlay);

  GST_DEBUG_OBJECT (overlay, "Expose called by application");

  if (!self->can_scale) {
    GST_OBJECT_LOCK (self);
    if (self->reconfigure) {
      GST_OBJECT_UNLOCK (self);
      GST_DEBUG_OBJECT (overlay, "Sending a reconfigure event");
      gst_pad_push_event (GST_BASE_SINK_PAD (self),
          gst_event_new_reconfigure ());
    } else {
      GST_DEBUG_OBJECT (overlay, "Applying new render rectangle");
      /* size of the rectangle does not change, only the (x,y) position changes */
      self->render_rect = self->pending_rect;
      GST_OBJECT_UNLOCK (self);
    }
  }

  gst_kms_sink_show_frame (GST_VIDEO_SINK (self), NULL);
}

static void
gst_kms_sink_video_overlay_init (GstVideoOverlayInterface * iface)
{
  iface->expose = gst_kms_sink_expose;
  iface->set_render_rectangle = gst_kms_sink_set_render_rectangle;
}

static int
kms_open (gchar ** driver)
{
  static const char *drivers[] = { "i915", "radeon", "nouveau", "vmwgfx",
    "exynos", "amdgpu", "imx-drm", "rockchip", "atmel-hlcdc", "msm",
    "xlnx", "vc4", "meson", "sun4i-drm", "mxsfb-drm",
    "xilinx_drm",               /* DEPRECATED. Replaced by xlnx */
  };
  int i, fd = -1;

  for (i = 0; i < G_N_ELEMENTS (drivers); i++) {
    fd = drmOpen (drivers[i], NULL);
    if (fd >= 0) {
      if (driver)
        *driver = g_strdup (drivers[i]);
      break;
    }
  }

  return fd;
}

static drmModePlane *
find_plane_for_crtc (int fd, drmModeRes * res, drmModePlaneRes * pres,
    int crtc_id)
{
  drmModePlane *plane;
  int i, pipe;

  plane = NULL;
  pipe = -1;
  for (i = 0; i < res->count_crtcs; i++) {
    if (crtc_id == res->crtcs[i]) {
      pipe = i;
      break;
    }
  }

  if (pipe == -1)
    return NULL;

  for (i = 0; i < pres->count_planes; i++) {
    plane = drmModeGetPlane (fd, pres->planes[i]);
    if (plane->possible_crtcs & (1 << pipe))
      return plane;
    drmModeFreePlane (plane);
  }

  return NULL;
}

static drmModeCrtc *
find_crtc_for_connector (int fd, drmModeRes * res, drmModeConnector * conn,
    guint * pipe)
{
  int i;
  int crtc_id;
  drmModeEncoder *enc;
  drmModeCrtc *crtc;
  guint32 crtcs_for_connector = 0;

  crtc_id = -1;
  for (i = 0; i < res->count_encoders; i++) {
    enc = drmModeGetEncoder (fd, res->encoders[i]);
    if (enc) {
      if (enc->encoder_id == conn->encoder_id) {
        crtc_id = enc->crtc_id;
        drmModeFreeEncoder (enc);
        break;
      }
      drmModeFreeEncoder (enc);
    }
  }

  /* If no active crtc was found, pick the first possible crtc */
  if (crtc_id == -1) {
    for (i = 0; i < conn->count_encoders; i++) {
      enc = drmModeGetEncoder (fd, conn->encoders[i]);
      crtcs_for_connector |= enc->possible_crtcs;
      drmModeFreeEncoder (enc);
    }

    if (crtcs_for_connector != 0)
      crtc_id = res->crtcs[ffs (crtcs_for_connector) - 1];
  }

  if (crtc_id == -1)
    return NULL;

  for (i = 0; i < res->count_crtcs; i++) {
    crtc = drmModeGetCrtc (fd, res->crtcs[i]);
    if (crtc) {
      if (crtc_id == crtc->crtc_id) {
        if (pipe)
          *pipe = i;
        return crtc;
      }
      drmModeFreeCrtc (crtc);
    }
  }

  return NULL;
}

static gboolean
connector_is_used (int fd, drmModeRes * res, drmModeConnector * conn)
{
  gboolean result;
  drmModeCrtc *crtc;

  result = FALSE;
  crtc = find_crtc_for_connector (fd, res, conn, NULL);
  if (crtc) {
    result = crtc->buffer_id != 0;
    drmModeFreeCrtc (crtc);
  }

  return result;
}

static drmModeConnector *
find_used_connector_by_type (int fd, drmModeRes * res, int type)
{
  int i;
  drmModeConnector *conn;

  conn = NULL;
  for (i = 0; i < res->count_connectors; i++) {
    conn = drmModeGetConnector (fd, res->connectors[i]);
    if (conn) {
      if ((conn->connector_type == type) && connector_is_used (fd, res, conn))
        return conn;
      drmModeFreeConnector (conn);
    }
  }

  return NULL;
}

static drmModeConnector *
find_first_used_connector (int fd, drmModeRes * res)
{
  int i;
  drmModeConnector *conn;

  conn = NULL;
  for (i = 0; i < res->count_connectors; i++) {
    conn = drmModeGetConnector (fd, res->connectors[i]);
    if (conn) {
      if (connector_is_used (fd, res, conn))
        return conn;
      drmModeFreeConnector (conn);
    }
  }

  return NULL;
}

static drmModeConnector *
find_main_monitor (int fd, drmModeRes * res)
{
  /* Find the LVDS and eDP connectors: those are the main screens. */
  static const int priority[] = { DRM_MODE_CONNECTOR_LVDS,
    DRM_MODE_CONNECTOR_eDP
  };
  int i;
  drmModeConnector *conn;

  conn = NULL;
  for (i = 0; !conn && i < G_N_ELEMENTS (priority); i++)
    conn = find_used_connector_by_type (fd, res, priority[i]);

  /* if we didn't find a connector, grab the first one in use */
  if (!conn)
    conn = find_first_used_connector (fd, res);

  /* if no connector is used, grab the first one */
  if (!conn)
    conn = drmModeGetConnector (fd, res->connectors[0]);

  return conn;
}

static void
log_drm_version (GstKMSSink * self)
{
#ifndef GST_DISABLE_GST_DEBUG
  drmVersion *v;

  v = drmGetVersion (self->fd);
  if (v) {
    GST_INFO_OBJECT (self, "DRM v%d.%d.%d [%s — %s — %s]", v->version_major,
        v->version_minor, v->version_patchlevel, GST_STR_NULL (v->name),
        GST_STR_NULL (v->desc), GST_STR_NULL (v->date));
    drmFreeVersion (v);
  } else {
    GST_WARNING_OBJECT (self, "could not get driver information: %s",
        GST_STR_NULL (self->devname));
  }
#endif
  return;
}

static gboolean
get_drm_caps (GstKMSSink * self)
{
  gint ret;
  guint64 has_dumb_buffer;
  guint64 has_prime;
  guint64 has_async_page_flip;

  has_dumb_buffer = 0;
  ret = drmGetCap (self->fd, DRM_CAP_DUMB_BUFFER, &has_dumb_buffer);
  if (ret)
    GST_WARNING_OBJECT (self, "could not get dumb buffer capability");
  if (has_dumb_buffer == 0) {
    GST_ERROR_OBJECT (self, "driver cannot handle dumb buffers");
    return FALSE;
  }

  has_prime = 0;
  ret = drmGetCap (self->fd, DRM_CAP_PRIME, &has_prime);
  if (ret)
    GST_WARNING_OBJECT (self, "could not get prime capability");
  else {
    self->has_prime_import = (gboolean) (has_prime & DRM_PRIME_CAP_IMPORT);
    self->has_prime_export = (gboolean) (has_prime & DRM_PRIME_CAP_EXPORT);
  }

  has_async_page_flip = 0;
  ret = drmGetCap (self->fd, DRM_CAP_ASYNC_PAGE_FLIP, &has_async_page_flip);
  if (ret)
    GST_WARNING_OBJECT (self, "could not get async page flip capability");
  else
    self->has_async_page_flip = (gboolean) has_async_page_flip;

  GST_INFO_OBJECT (self,
      "prime import (%s) / prime export (%s) / async page flip (%s)",
      self->has_prime_import ? "✓" : "✗",
      self->has_prime_export ? "✓" : "✗",
      self->has_async_page_flip ? "✓" : "✗");

  return TRUE;
}

static gint
get_commit_fd (GstKMSSink * self)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
  return self->ctrl_fd;
#else
  return self->fd;
#endif
}

static guint
check_upscale (GstKMSSink * self, guint32 fb_id) {
  guint32 src_w = self->hdisplay / 10;
  guint32 src_h = self->vdisplay / 10;
  guint ratio;
  gint fd = get_commit_fd(self);

  for (ratio = 10; ratio > 0; ratio--) {
    if (!drmModeSetPlane (fd, self->plane_id, self->crtc_id, fb_id, 0,
          0, 0, src_w * ratio, src_h * ratio,
          0, 0, src_w << 16, src_h << 16))
      break;
  }

  return ratio;
}

static guint
check_downscale (GstKMSSink * self, guint32 fb_id) {
  guint32 src_w = self->hdisplay / 10;
  guint32 src_h = self->vdisplay / 10;
  guint ratio;
  gint fd = get_commit_fd(self);

  for (ratio = 10; ratio > 0; ratio--) {
    if (!drmModeSetPlane (fd, self->plane_id, self->crtc_id, fb_id, 0,
          0, 0, src_w, src_h,
          0, 0, (src_w * ratio) << 16, (src_h * ratio) << 16))
      break;
  }

  return ratio;
}

static void
check_scaleable (GstKMSSink * self)
{
  guint32 fb_id;
  GstKMSMemory *kmsmem = NULL;
  GstVideoInfo vinfo;

  /* we assume driver can scale at initialize,
   * if scale is checked or can not scale, we
   * don't need check again */
  if (self->scale_checked || !self->can_scale)
    return;

  if (self->conn_id < 0 || !self->display_connected)
    return;

  /* FIXME: for dpu, we can only hard code the scale ratio,
   * dpu has no limitation when do upscale but can not support
   * downscale */
  if (strcmp (get_imx_drm_device_name(), "DPU") == 0) {
    self->downscale_ratio = 1;
    self->upscale_ratio = SCALE_RATIO_NO_LIMITATION;
    return;
  }

  gst_video_info_init (&vinfo);
  gst_video_info_set_format (&vinfo, GST_VIDEO_FORMAT_NV12, self->hdisplay, self->vdisplay);

  ensure_kms_allocator (self);

  kmsmem = (GstKMSMemory *) gst_kms_allocator_bo_alloc (self->allocator, &vinfo);
  if (!kmsmem)
    return;

  fb_id = kmsmem->fb_id;

  GST_INFO_OBJECT (self, "checking scaleable");
  self->downscale_ratio = check_downscale (self, fb_id);
  self->upscale_ratio = check_upscale (self, fb_id);

  GST_INFO_OBJECT (self, "got scale ratio: up (%d) down (%d)",
      self->upscale_ratio, self->downscale_ratio);

  self->scale_checked = TRUE;
  g_clear_pointer (&kmsmem, gst_memory_unref);
}

static gboolean
check_vsi_tile_enable (GstKMSSink * self, GstBuffer * buffer)
{
  GstDmabufMeta *dmabuf_meta;
  gint64 drm_modifier = 0;

  if (!buffer)
    buffer = self->hold_buf[0];

  if (!buffer)
    return FALSE;

  if (!gst_is_dmabuf_memory (gst_buffer_peek_memory (buffer, 0)))
    return FALSE;

  dmabuf_meta = gst_buffer_get_dmabuf_meta (buffer);
  if (dmabuf_meta)
    drm_modifier = dmabuf_meta->drm_modifier;

  GST_INFO_OBJECT (self, "buffer modifier type %d", drm_modifier);

  return drm_modifier == DRM_FORMAT_MOD_VSI_G1_TILED
         || drm_modifier == DRM_FORMAT_MOD_VSI_G2_TILED
         || drm_modifier == DRM_FORMAT_MOD_VSI_G2_TILED_COMPRESSED;
}

static gboolean
configure_mode_setting (GstKMSSink * self, GstVideoInfo * vinfo)
{
  gboolean ret;
  drmModeConnector *conn;
  int err;
  drmModeFB *fb;
  gint i;
  drmModeModeInfo *mode;
  guint32 fb_id;
  GstKMSMemory *kmsmem;

  ret = FALSE;
  conn = NULL;
  fb = NULL;
  mode = NULL;
  kmsmem = NULL;

  if (self->conn_id < 0)
    goto bail;

  GST_INFO_OBJECT (self, "configuring mode setting");

  kmsmem = (GstKMSMemory *) gst_kms_allocator_bo_alloc (self->allocator, vinfo);
  if (!kmsmem)
    goto bo_failed;
  fb_id = kmsmem->fb_id;

  conn = drmModeGetConnector (self->fd, self->conn_id);
  if (!conn)
    goto connector_failed;

  fb = drmModeGetFB (self->fd, fb_id);
  if (!fb)
    goto framebuffer_failed;

  for (i = 0; i < conn->count_modes; i++) {
    if (conn->modes[i].vdisplay == fb->height &&
        conn->modes[i].hdisplay == fb->width) {
      mode = &conn->modes[i];
      break;
    }
  }
  if (!mode)
    goto mode_failed;

  err = drmModeSetCrtc (self->fd, self->crtc_id, fb_id, 0, 0,
      (uint32_t *) & self->conn_id, 1, mode);
  if (err)
    goto modesetting_failed;

  self->tmp_kmsmem = (GstMemory *) kmsmem;

  ret = TRUE;

bail:
  if (fb)
    drmModeFreeFB (fb);
  if (conn)
    drmModeFreeConnector (conn);

  return ret;

  /* ERRORS */
bo_failed:
  {
    GST_ERROR_OBJECT (self,
        "failed to allocate buffer object for mode setting");
    goto bail;
  }
connector_failed:
  {
    GST_ERROR_OBJECT (self, "Could not find a valid monitor connector");
    goto bail;
  }
framebuffer_failed:
  {
    GST_ERROR_OBJECT (self, "drmModeGetFB failed: %s (%d)",
        strerror (errno), errno);
    goto bail;
  }
mode_failed:
  {
    GST_ERROR_OBJECT (self, "cannot find appropriate mode");
    goto bail;
  }
modesetting_failed:
  {
    GST_ERROR_OBJECT (self, "Failed to set mode: %s", strerror (errno));
    goto bail;
  }
}

static gboolean
ensure_allowed_caps (GstKMSSink * self, drmModeConnector * conn,
    drmModePlane * plane, drmModeRes * res)
{
  GstCaps *out_caps, *tmp_caps, *caps;
  int i, j;
  GstVideoFormat fmt;
  const gchar *format;
  drmModeModeInfo *mode;
  gint count_modes;

  if (self->allowed_caps)
    return TRUE;

  out_caps = gst_caps_new_empty ();
  if (!out_caps)
    return FALSE;

  if (conn && self->modesetting_enabled && self->display_connected)
    count_modes = conn->count_modes;
  else
    count_modes = 1;

  for (i = 0; i < count_modes; i++) {
    tmp_caps = gst_caps_new_empty ();
    if (!tmp_caps)
      return FALSE;

    mode = NULL;
    if (conn && self->modesetting_enabled)
      mode = &conn->modes[i];

    for (j = 0; j < plane->count_formats; j++) {
      fmt = gst_video_format_from_drm (plane->formats[j]);
      if (fmt == GST_VIDEO_FORMAT_UNKNOWN) {
        GST_INFO_OBJECT (self, "ignoring format %" GST_FOURCC_FORMAT,
            GST_FOURCC_ARGS (plane->formats[j]));
        continue;
      }

      format = gst_video_format_to_string (fmt);

      if (mode) {
        caps = gst_caps_new_simple ("video/x-raw",
            "format", G_TYPE_STRING, format,
            "width", G_TYPE_INT, mode->hdisplay,
            "height", G_TYPE_INT, mode->vdisplay,
            "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);
      } else {
        caps = gst_caps_new_simple ("video/x-raw",
            "format", G_TYPE_STRING, format,
            "width", GST_TYPE_INT_RANGE, res->min_width, res->max_width,
            "height", GST_TYPE_INT_RANGE, res->min_height, res->max_height,
            "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);
      }
      if (!caps)
        continue;

      tmp_caps = gst_caps_merge (tmp_caps, caps);
    }
    /* FIXME: Add NV12_10LE caps here, no need this code
     * when new drm fourcc added*/
    caps = gst_caps_new_simple ("video/x-raw",
        "format", G_TYPE_STRING, "NV12_10LE",
        "width", GST_TYPE_INT_RANGE, res->min_width, res->max_width,
        "height", GST_TYPE_INT_RANGE, res->min_height, res->max_height,
        "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);

    tmp_caps = gst_caps_merge (tmp_caps, caps);

    out_caps = gst_caps_merge (out_caps, gst_caps_simplify (tmp_caps));
  }

  self->allowed_caps = gst_caps_simplify (out_caps);

  GST_DEBUG_OBJECT (self, "allowed caps = %" GST_PTR_FORMAT,
      self->allowed_caps);

  return (self->allowed_caps && !gst_caps_is_empty (self->allowed_caps));
}

static gint
get_drm_minor_base (gint type)
{
  switch (type) {
    case DRM_NODE_PRIMARY:
      return 0;
    case DRM_NODE_CONTROL:
      return 64;
    case DRM_NODE_RENDER:
      return 128;
    default:
      return -1;
  }
}

static gboolean
gst_kms_sink_start (GstBaseSink * bsink)
{
  GstKMSSink *self;
  drmModeRes *res;
  drmModeConnector *conn;
  drmModeCrtc *crtc;
  drmModePlaneRes *pres;
  drmModePlane *plane;
  gboolean universal_planes;
  gboolean ret;

  self = GST_KMS_SINK (bsink);
  universal_planes = FALSE;
  ret = FALSE;
  res = NULL;
  conn = NULL;
  crtc = NULL;
  pres = NULL;
  plane = NULL;

  if (self->devname || self->bus_id)
    self->fd = drmOpen (self->devname, self->bus_id);
  else
    self->fd = kms_open (&self->devname);

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
  gint minor;
  minor = get_drm_minor_base (DRM_NODE_CONTROL);
  self->ctrl_fd = drmOpenControl(minor);
  if (self->ctrl_fd < 0) {
    if (self->fd >= 0)
      drmClose (self->fd);
    GST_ELEMENT_ERROR (self, RESOURCE, OPEN_READ_WRITE,
        ("Could not open DRM ctrl node %s", GST_STR_NULL (self->devname)),
        ("reason: %s (%d)", strerror (errno), errno));
    return FALSE;
  }
#endif

  if (self->fd < 0)
    goto open_failed;

  log_drm_version (self);
  if (!get_drm_caps (self))
    goto bail;

  res = drmModeGetResources (self->fd);
  if (!res)
    goto resources_failed;

  if (self->conn_id == -1)
    conn = find_main_monitor (self->fd, res);
  else
    conn = drmModeGetConnector (self->fd, self->conn_id);
  if (!conn)
    goto connector_failed;

  if (conn->connection == DRM_MODE_CONNECTED)
    self->display_connected = TRUE;
  else
    self->display_connected = FALSE;

  GST_DEBUG_OBJECT (self, "display connection status: %s",
      self->display_connected ? "Connected" : "disconnected");

  crtc = find_crtc_for_connector (self->fd, res, conn, &self->pipe);
  if (!crtc)
    goto crtc_failed;

  if ((!crtc->mode_valid || self->modesetting_enabled) && self->display_connected) {
    GST_DEBUG_OBJECT (self, "enabling modesetting");
    self->modesetting_enabled = TRUE;
    universal_planes = TRUE;
  }

retry_find_plane:
  if (universal_planes &&
      drmSetClientCap (self->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1))
    goto set_cap_failed;

  pres = drmModeGetPlaneResources (self->fd);
  if (!pres)
    goto plane_resources_failed;

  if (self->plane_id == -1)
    plane = find_plane_for_crtc (self->fd, res, pres, crtc->crtc_id);
  else
    plane = drmModeGetPlane (self->fd, self->plane_id);
  if (!plane)
    goto plane_failed;

  if (!ensure_allowed_caps (self, conn, plane, res))
    goto allowed_caps_failed;

  self->conn_id = conn->connector_id;
  self->crtc_id = crtc->crtc_id;
  self->plane_id = plane->plane_id;

  GST_INFO_OBJECT (self, "connector id = %d / crtc id = %d / plane id = %d",
      self->conn_id, self->crtc_id, self->plane_id);

  GST_OBJECT_LOCK (self);
  self->hdisplay = crtc->mode.hdisplay;
  self->vdisplay = crtc->mode.vdisplay;

  if (self->render_rect.w == 0 || self->render_rect.h == 0) {
    self->render_rect.x = 0;
    self->render_rect.y = 0;
    self->render_rect.w = self->hdisplay;
    self->render_rect.h = self->vdisplay;
  }

  self->pending_rect = self->render_rect;
  GST_OBJECT_UNLOCK (self);

  self->buffer_id = crtc->buffer_id;

  self->mm_width = conn->mmWidth;
  self->mm_height = conn->mmHeight;

  GST_INFO_OBJECT (self, "display size: pixels = %dx%d / millimeters = %dx%d",
      self->hdisplay, self->vdisplay, self->mm_width, self->mm_height);

  check_scaleable (self);

  self->pollfd.fd = self->fd;
  gst_poll_add_fd (self->poll, &self->pollfd);
  gst_poll_fd_ctl_read (self->poll, &self->pollfd, TRUE);

  g_object_notify_by_pspec (G_OBJECT (self), g_properties[PROP_DISPLAY_WIDTH]);
  g_object_notify_by_pspec (G_OBJECT (self), g_properties[PROP_DISPLAY_HEIGHT]);

  ret = TRUE;

bail:
  if (plane)
    drmModeFreePlane (plane);
  if (pres)
    drmModeFreePlaneResources (pres);
  if (crtc)
    drmModeFreeCrtc (crtc);
  if (conn)
    drmModeFreeConnector (conn);
  if (res)
    drmModeFreeResources (res);

  if (!ret && self->fd >= 0) {
    drmClose (self->fd);
    self->fd = -1;
  }

  return ret;

  /* ERRORS */
open_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, OPEN_READ_WRITE,
        ("Could not open DRM module %s", GST_STR_NULL (self->devname)),
        ("reason: %s (%d)", strerror (errno), errno));
    return FALSE;
  }

resources_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
        ("drmModeGetResources failed"),
        ("reason: %s (%d)", strerror (errno), errno));
    goto bail;
  }

connector_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
        ("Could not find a valid monitor connector"), (NULL));
    goto bail;
  }

crtc_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
        ("Could not find a crtc for connector"), (NULL));
    goto bail;
  }

set_cap_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
        ("Could not set universal planes capability bit"), (NULL));
    goto bail;
  }

plane_resources_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
        ("drmModeGetPlaneResources failed"),
        ("reason: %s (%d)", strerror (errno), errno));
    goto bail;
  }

plane_failed:
  {
    if (universal_planes) {
      GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
          ("Could not find a plane for crtc"), (NULL));
      goto bail;
    } else {
      universal_planes = TRUE;
      goto retry_find_plane;
    }
  }

allowed_caps_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
        ("Could not get allowed GstCaps of device"),
        ("driver does not provide mode settings configuration"));
    goto bail;
  }
}

static gboolean
gst_kms_sink_stop (GstBaseSink * bsink)
{
  GstKMSSink *self;

  self = GST_KMS_SINK (bsink);

  if (self->allocator)
    gst_kms_allocator_clear_cache (self->allocator);

  gst_buffer_replace (&self->last_buffer, NULL);
  gst_caps_replace (&self->allowed_caps, NULL);
  gst_object_replace ((GstObject **) & self->pool, NULL);
  gst_object_replace ((GstObject **) & self->allocator, NULL);

  gst_poll_remove_fd (self->poll, &self->pollfd);
  gst_poll_restart (self->poll);
  gst_poll_fd_init (&self->pollfd);

  if (self->fd >= 0) {
    drmClose (self->fd);
    self->fd = -1;
  }

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
  if (self->ctrl_fd >= 0) {
    drmClose (self->ctrl_fd);
    self->ctrl_fd = -1;
  }
#endif

  GST_OBJECT_LOCK (bsink);
  self->hdisplay = 0;
  self->vdisplay = 0;
  self->pending_rect.x = 0;
  self->pending_rect.y = 0;
  self->pending_rect.w = 0;
  self->pending_rect.h = 0;
  self->render_rect = self->pending_rect;
  GST_OBJECT_UNLOCK (bsink);

  g_object_notify_by_pspec (G_OBJECT (self), g_properties[PROP_DISPLAY_WIDTH]);
  g_object_notify_by_pspec (G_OBJECT (self), g_properties[PROP_DISPLAY_HEIGHT]);

  return TRUE;
}

static GstCaps *
gst_kms_sink_get_allowed_caps (GstKMSSink * self)
{
  if (!self->allowed_caps)
    return NULL;                /* base class will return the template caps */
  return gst_caps_ref (self->allowed_caps);
}

static GstCaps *
gst_kms_sink_get_caps (GstBaseSink * bsink, GstCaps * filter)
{
  GstKMSSink *self;
  GstCaps *caps, *out_caps;
  GstStructure *s;
  guint dpy_par_n, dpy_par_d;

  self = GST_KMS_SINK (bsink);

  caps = gst_kms_sink_get_allowed_caps (self);
  if (!caps)
    return NULL;

  GST_OBJECT_LOCK (self);

  if (!self->can_scale) {
    out_caps = gst_caps_new_empty ();
    gst_video_calculate_device_ratio (self->hdisplay, self->vdisplay,
        self->mm_width, self->mm_height, &dpy_par_n, &dpy_par_d);

    s = gst_structure_copy (gst_caps_get_structure (caps, 0));
    gst_structure_set (s, "width", G_TYPE_INT, self->pending_rect.w,
        "height", G_TYPE_INT, self->pending_rect.h,
        "pixel-aspect-ratio", GST_TYPE_FRACTION, dpy_par_n, dpy_par_d, NULL);

    gst_caps_append_structure (out_caps, s);

    out_caps = gst_caps_merge (out_caps, caps);
    caps = NULL;

    /* enforce our display aspect ratio */
    gst_caps_set_simple (out_caps, "pixel-aspect-ratio", GST_TYPE_FRACTION,
        dpy_par_n, dpy_par_d, NULL);
  } else {
    out_caps = gst_caps_make_writable (caps);
    caps = NULL;
  }

  GST_OBJECT_UNLOCK (self);

  GST_DEBUG_OBJECT (self, "Proposing caps %" GST_PTR_FORMAT, out_caps);

  if (filter) {
    caps = out_caps;
    out_caps = gst_caps_intersect_full (caps, filter, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (caps);
  }

  return out_caps;
}

static void
ensure_kms_allocator (GstKMSSink * self)
{
  if (self->allocator)
    return;
  self->allocator = gst_kms_allocator_new (self->fd);
}

static GstBufferPool *
gst_kms_sink_create_pool (GstKMSSink * self, GstCaps * caps, gsize size,
    gint min)
{
  GstBufferPool *pool;
  GstStructure *config;

  pool = gst_kms_buffer_pool_new ();
  if (!pool)
    goto pool_failed;

  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_set_params (config, caps, size, min, 0);
  gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);

  ensure_kms_allocator (self);
  gst_buffer_pool_config_set_allocator (config, self->allocator, NULL);

  if (!gst_buffer_pool_set_config (pool, config))
    goto config_failed;

  return pool;

  /* ERRORS */
pool_failed:
  {
    GST_ERROR_OBJECT (self, "failed to create buffer pool");
    return NULL;
  }
config_failed:
  {
    GST_ERROR_OBJECT (self, "failed to set config");
    gst_object_unref (pool);
    return NULL;
  }
}

static gboolean
gst_kms_sink_calculate_display_ratio (GstKMSSink * self, GstVideoInfo * vinfo)
{
  guint dar_n, dar_d;
  guint video_width, video_height;
  guint video_par_n, video_par_d;
  guint dpy_par_n, dpy_par_d;

  video_width = GST_VIDEO_INFO_WIDTH (vinfo);
  video_height = GST_VIDEO_INFO_HEIGHT (vinfo);
  video_par_n = GST_VIDEO_INFO_PAR_N (vinfo);
  video_par_d = GST_VIDEO_INFO_PAR_D (vinfo);

  if (self->can_scale) {
    gst_video_calculate_device_ratio (self->hdisplay, self->vdisplay,
        self->mm_width, self->mm_height, &dpy_par_n, &dpy_par_d);
  } else {
    GST_VIDEO_SINK_WIDTH (self) = video_width;
    GST_VIDEO_SINK_HEIGHT (self) = video_height;
    goto out;
  }

  if (!gst_video_calculate_display_ratio (&dar_n, &dar_d, video_width,
          video_height, video_par_n, video_par_d, dpy_par_n, dpy_par_d))
    return FALSE;

  GST_DEBUG_OBJECT (self, "video calculated display ratio: %d/%d", dar_n,
      dar_d);

  /* now find a width x height that respects this display ratio.
   * prefer those that have one of w/h the same as the incoming video
   * using wd / hd = dar_n / dar_d */

  /* start with same height, because of interlaced video */
  /* check hd / dar_d is an integer scale factor, and scale wd with the PAR */
  if (video_height % dar_d == 0) {
    GST_DEBUG_OBJECT (self, "keeping video height");
    GST_VIDEO_SINK_WIDTH (self) = (guint)
        gst_util_uint64_scale_int (video_height, dar_n, dar_d);
    GST_VIDEO_SINK_HEIGHT (self) = video_height;
  } else if (video_width % dar_n == 0) {
    GST_DEBUG_OBJECT (self, "keeping video width");
    GST_VIDEO_SINK_WIDTH (self) = video_width;
    GST_VIDEO_SINK_HEIGHT (self) = (guint)
        gst_util_uint64_scale_int (video_width, dar_d, dar_n);
  } else {
    GST_DEBUG_OBJECT (self, "approximating while keeping video height");
    GST_VIDEO_SINK_WIDTH (self) = (guint)
        gst_util_uint64_scale_int (video_height, dar_n, dar_d);
    GST_VIDEO_SINK_HEIGHT (self) = video_height;
  }

out:
  GST_DEBUG_OBJECT (self, "scaling to %dx%d", GST_VIDEO_SINK_WIDTH (self),
      GST_VIDEO_SINK_HEIGHT (self));

  return TRUE;
}

static gboolean
gst_kms_sink_set_caps (GstBaseSink * bsink, GstCaps * caps)
{
  GstKMSSink *self;
  GstVideoInfo vinfo;
  GstBufferPool *newpool, *oldpool;

  self = GST_KMS_SINK (bsink);

  /* We are going to change the internal buffer pool, which means it will no
   * longer be compatbile with the last_buffer size. Drain now, as we won't be
   * able to do that later on. */
  gst_kms_sink_drain (self);

  if (!gst_video_info_from_caps (&vinfo, caps))
    goto invalid_format;

  if (!gst_kms_sink_calculate_display_ratio (self, &vinfo))
    goto no_disp_ratio;

  if (GST_VIDEO_SINK_WIDTH (self) <= 0 || GST_VIDEO_SINK_HEIGHT (self) <= 0)
    goto invalid_size;

  /* create a new pool for the new configuration */
  newpool = gst_kms_sink_create_pool (self, caps, GST_VIDEO_INFO_SIZE (&vinfo),
      2);
  if (!newpool)
    goto no_pool;

  /* we don't activate the internal pool yet as it may not be needed */
  oldpool = self->pool;
  self->pool = newpool;

  if (oldpool) {
    gst_buffer_pool_set_active (oldpool, FALSE);
    gst_object_unref (oldpool);
  }

  if (self->modesetting_enabled && !configure_mode_setting (self, &vinfo))
    goto modesetting_failed;

  self->vinfo = vinfo;

  GST_OBJECT_LOCK (self);
  if (self->reconfigure) {
    self->reconfigure = FALSE;
    self->render_rect = self->pending_rect;
  }
  GST_OBJECT_UNLOCK (self);

  GST_DEBUG_OBJECT (self, "negotiated caps = %" GST_PTR_FORMAT, caps);

  return TRUE;

  /* ERRORS */
invalid_format:
  {
    GST_ERROR_OBJECT (self, "caps invalid");
    return FALSE;
  }

invalid_size:
  {
    GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, (NULL),
        ("Invalid image size."));
    return FALSE;
  }

no_disp_ratio:
  {
    GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, (NULL),
        ("Error calculating the output display ratio of the video."));
    return FALSE;
  }
no_pool:
  {
    /* Already warned in create_pool */
    return FALSE;
  }

modesetting_failed:
  {
    GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, (NULL),
        ("failed to configure video mode"));
    return FALSE;
  }

}

static gboolean
gst_kms_sink_propose_allocation (GstBaseSink * bsink, GstQuery * query)
{
  GstKMSSink *self;
  GstCaps *caps;
  gboolean need_pool;
  GstVideoInfo vinfo;
  GstBufferPool *pool;
  guint64 drm_modifier;
  drmModeObjectPropertiesPtr props = NULL;
  drmModePropertyPtr prop = NULL;
  guint i;
  gsize size;

  self = GST_KMS_SINK (bsink);

  gst_query_parse_allocation (query, &caps, &need_pool);
  if (!caps)
    goto no_caps;
  if (!gst_video_info_from_caps (&vinfo, caps))
    goto invalid_caps;

  size = GST_VIDEO_INFO_SIZE (&vinfo);

  pool = NULL;
  if (need_pool) {
    pool = gst_kms_sink_create_pool (self, caps, size, 0);
    if (!pool)
      goto no_pool;

    /* Only export for pool used upstream */
    if (self->has_prime_export) {
      GstStructure *config = gst_buffer_pool_get_config (pool);
      gst_buffer_pool_config_add_option (config,
          GST_BUFFER_POOL_OPTION_KMS_PRIME_EXPORT);
      gst_buffer_pool_set_config (pool, config);
    }
  }

  /* we need at least 2 buffer because we hold on to the last one */
  gst_query_add_allocation_pool (query, pool, size, 2, 0);
  if (pool)
    gst_object_unref (pool);

  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);
  gst_query_add_allocation_meta (query, GST_VIDEO_CROP_META_API_TYPE, NULL);

  drm_modifier = DRM_FORMAT_MOD_AMPHION_TILED;
  gst_query_add_allocation_dmabuf_meta (query, drm_modifier);

  if (self->hantro_tile_enabled) {
    props = drmModeObjectGetProperties (self->fd, self->plane_id, DRM_MODE_OBJECT_PLANE);
    for (i = 0; i < props->count_props; ++i) {
      prop = drmModeGetProperty(self->fd, props->props[i]);
      if (!strcmp(prop->name, "dtrc_table_ofs")) {
        GST_DEBUG ("has dtrc_table_ofs property, can support VSI tile format");
        drm_modifier = DRM_FORMAT_MOD_VSI_G1_TILED;
        gst_query_add_allocation_dmabuf_meta (query, drm_modifier);
        drm_modifier = DRM_FORMAT_MOD_VSI_G2_TILED;
        gst_query_add_allocation_dmabuf_meta (query, drm_modifier);
        drm_modifier = DRM_FORMAT_MOD_VSI_G2_TILED_COMPRESSED;
        gst_query_add_allocation_dmabuf_meta (query, drm_modifier);
      }
      drmModeFreeProperty (prop);
      prop = NULL;
    }
  }

  return TRUE;

  /* ERRORS */
no_caps:
  {
    GST_DEBUG_OBJECT (bsink, "no caps specified");
    return FALSE;
  }
invalid_caps:
  {
    GST_DEBUG_OBJECT (bsink, "invalid caps specified");
    return FALSE;
  }
no_pool:
  {
    /* Already warned in create_pool */
    return FALSE;
  }
}

static void
sync_handler (gint fd, guint frame, guint sec, guint usec, gpointer data)
{
  gboolean *waiting;

  waiting = data;
  *waiting = FALSE;
}

static gboolean
gst_kms_sink_sync (GstKMSSink * self)
{
  gint ret;
  gboolean waiting;
  drmEventContext evctxt = {
    .version = DRM_EVENT_CONTEXT_VERSION,
    .page_flip_handler = sync_handler,
    .vblank_handler = sync_handler,
  };
  drmVBlank vbl = {
    .request = {
          .type = DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT,
          .sequence = 1,
          .signal = (gulong) & waiting,
        },
  };

  if (self->pipe == 1)
    vbl.request.type |= DRM_VBLANK_SECONDARY;
  else if (self->pipe > 1)
    vbl.request.type |= self->pipe << DRM_VBLANK_HIGH_CRTC_SHIFT;

  waiting = TRUE;
  if (!self->has_async_page_flip && !self->modesetting_enabled) {
    ret = drmWaitVBlank (self->fd, &vbl);
    if (ret)
      goto vblank_failed;
  } else {
    ret = drmModePageFlip (self->fd, self->crtc_id, self->buffer_id,
        DRM_MODE_PAGE_FLIP_EVENT, &waiting);
    if (ret)
      goto pageflip_failed;
  }

  while (waiting) {
    do {
      ret = gst_poll_wait (self->poll, 3 * GST_SECOND);
    } while (ret == -1 && (errno == EAGAIN || errno == EINTR));

    ret = drmHandleEvent (self->fd, &evctxt);
    if (ret)
      goto event_failed;
  }

  return TRUE;

  /* ERRORS */
vblank_failed:
  {
    GST_WARNING_OBJECT (self, "drmWaitVBlank failed: %s (%d)", strerror (-ret),
        ret);
    return FALSE;
  }
pageflip_failed:
  {
    GST_WARNING_OBJECT (self, "drmModePageFlip failed: %s (%d)",
        strerror (-ret), ret);
    return FALSE;
  }
event_failed:
  {
    GST_ERROR_OBJECT (self, "drmHandleEvent failed: %s (%d)", strerror (-ret),
        ret);
    return FALSE;
  }
}

static gboolean
gst_kms_sink_import_dmabuf (GstKMSSink * self, GstBuffer * inbuf,
    GstBuffer ** outbuf)
{
  gint prime_fds[GST_VIDEO_MAX_PLANES] = { 0, };
  GstVideoMeta *meta;
  GstDmabufMeta *dmabuf_meta;
  gint64 drm_modifier = 0;
  guint i, n_mem, n_planes;
  GstKMSMemory *kmsmem;
  guint mems_idx[GST_VIDEO_MAX_PLANES];
  gsize mems_skip[GST_VIDEO_MAX_PLANES];
  GstMemory *mems[GST_VIDEO_MAX_PLANES];

  if (!self->has_prime_import)
    return FALSE;

  /* This will eliminate most non-dmabuf out there */
  if (!gst_is_dmabuf_memory (gst_buffer_peek_memory (inbuf, 0)))
    return FALSE;

  n_planes = GST_VIDEO_INFO_N_PLANES (&self->vinfo);
  n_mem = gst_buffer_n_memory (inbuf);
  meta = gst_buffer_get_video_meta (inbuf);
  dmabuf_meta = gst_buffer_get_dmabuf_meta (inbuf);
  if (dmabuf_meta)
    drm_modifier = dmabuf_meta->drm_modifier;

  GST_TRACE_OBJECT (self, "Found a dmabuf with %u planes and %u memories",
      n_planes, n_mem);

  /* We cannot have multiple dmabuf per plane */
  if (n_mem > n_planes)
    return FALSE;
  g_assert (n_planes != 0);

  /* Update video info based on video meta */
  if (meta) {
    GST_VIDEO_INFO_WIDTH (&self->vinfo) = meta->width;
    GST_VIDEO_INFO_HEIGHT (&self->vinfo) = meta->height;

    for (i = 0; i < meta->n_planes; i++) {
      GST_VIDEO_INFO_PLANE_OFFSET (&self->vinfo, i) = meta->offset[i];
      GST_VIDEO_INFO_PLANE_STRIDE (&self->vinfo, i) = meta->stride[i];
    }
  }

  /* Find and validate all memories */
  for (i = 0; i < n_planes; i++) {
    guint length;

    if (!gst_buffer_find_memory (inbuf,
            GST_VIDEO_INFO_PLANE_OFFSET (&self->vinfo, i), 1,
            &mems_idx[i], &length, &mems_skip[i]))
      return FALSE;

    mems[i] = gst_buffer_peek_memory (inbuf, mems_idx[i]);

    /* adjust for memory offset, in case data does not
     * start from byte 0 in the dmabuf fd */
    mems_skip[i] += mems[i]->offset;

    /* And all memory found must be dmabuf */
    if (!gst_is_dmabuf_memory (mems[i]))
      return FALSE;
  }

  kmsmem = (GstKMSMemory *) gst_kms_allocator_get_cached (mems[0]);
  if (kmsmem) {
    GST_LOG_OBJECT (self, "found KMS mem %p in DMABuf mem %p with fb id = %d",
        kmsmem, mems[0], kmsmem->fb_id);
    goto wrap_mem;
  }

  for (i = 0; i < n_planes; i++)
    prime_fds[i] = gst_dmabuf_memory_get_fd (mems[i]);

  GST_LOG_OBJECT (self, "found these prime ids: %d, %d, %d, %d", prime_fds[0],
      prime_fds[1], prime_fds[2], prime_fds[3]);

  kmsmem = gst_kms_allocator_dmabuf_import (self->allocator,
      prime_fds, n_planes, drm_modifier, mems_skip, &self->vinfo);
  if (!kmsmem)
    return FALSE;

  GST_LOG_OBJECT (self, "setting KMS mem %p to DMABuf mem %p with fb id = %d",
      kmsmem, mems[0], kmsmem->fb_id);
  gst_kms_allocator_cache (self->allocator, mems[0], GST_MEMORY_CAST (kmsmem));

wrap_mem:
  *outbuf = gst_buffer_new ();
  if (!*outbuf)
    return FALSE;
  gst_buffer_append_memory (*outbuf, gst_memory_ref (GST_MEMORY_CAST (kmsmem)));
  gst_buffer_add_parent_buffer_meta (*outbuf, inbuf);

  return TRUE;
}

static void
gst_kms_sink_set_kmsproperty (GstKMSSink * self, guint alpha, guint64 dtrc_table_ofs)
{
  drmModeRes *res = NULL;
  drmModePlaneRes *pres = NULL;
  drmModePlane *plane = NULL;
  drmModeObjectPropertiesPtr props = NULL;
  drmModePropertyPtr prop = NULL;
  guint i;
  gint fd = get_commit_fd (self);

  props = drmModeObjectGetProperties (self->fd, self->plane_id, DRM_MODE_OBJECT_PLANE);
  for (i = 0; i < props->count_props; ++i) {
    prop = drmModeGetProperty(self->fd, props->props[i]);
    if (!strcmp(prop->name, "dtrc_table_ofs") && dtrc_table_ofs) {
      GST_DEBUG ("set DTRC table offset %lld to primary plane %d property %d",
          dtrc_table_ofs, self->plane_id, prop->prop_id);
      drmModeObjectSetProperty (fd, self->plane_id, DRM_MODE_OBJECT_PLANE, prop->prop_id, dtrc_table_ofs);
    }
    drmModeFreeProperty (prop);
    prop = NULL;
  }

  res = drmModeGetResources (self->fd);
  if (!res)
    goto out;

  drmSetClientCap (self->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

  pres = drmModeGetPlaneResources (self->fd);
  if (!pres)
    goto out;

  plane = find_plane_for_crtc (self->fd, res, pres, self->crtc_id);
  if (!plane)
    goto out;

  props = drmModeObjectGetProperties (self->fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
  for (i = 0; i < props->count_props; ++i) {
    prop = drmModeGetProperty(self->fd, props->props[i]);
    if (!strcmp(prop->name, "alpha")) {
      GST_DEBUG ("set global alpha %d to primary plane %d property %d",
          alpha, plane->plane_id, prop->prop_id);
      drmModeObjectSetProperty (fd, plane->plane_id, DRM_MODE_OBJECT_PLANE, prop->prop_id, alpha);
      self->primary_plane_id = plane->plane_id;
    }
    drmModeFreeProperty (prop);
    prop = NULL;
  }

out:
  if (res)
    drmModeFreeResources (res);
  if (pres)
    drmModeFreePlaneResources (pres);
  if (plane)
    drmModeFreePlane (plane);
  if (props)
    drmModeFreeObjectProperties (props);
  drmSetClientCap (self->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 0);
}

static GstStateChangeReturn
gst_kms_sink_change_state (GstElement * element, GstStateChange transition)
{
  GstKMSSink *self;
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  gint fd;

  GST_DEBUG ("changing state: %s => %s",
      gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT (transition)),
      gst_element_state_get_name (GST_STATE_TRANSITION_NEXT (transition)));

  self = GST_KMS_SINK (element);
  fd = get_commit_fd (self);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      self->is_kmsproperty_set = FALSE;
      memset (&self->hdr10meta, 0, sizeof (self->hdr10meta));
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
    {
      guint i;
      for (i = 0; i < DEFAULT_HOLD_BUFFER_NUM; i++)
        self->hold_buf[i] = NULL;
      break;
    }
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
    {
      self->run_time = gst_element_get_start_time (element);
      break;
    }
    case GST_STATE_CHANGE_PAUSED_TO_READY:
    {
      guint i;
      gst_kms_sink_set_kmsproperty (self, 255, 0);
      for (i = 0; i < DEFAULT_HOLD_BUFFER_NUM; i++) {
        if (self->hold_buf[i])
          gst_buffer_unref (self->hold_buf[i]);
      }
      if (self->hdr10meta.eotf != 0) {
        guint blob_id = 0;
        gint err = 0;
        self->hdr10meta.eotf = 0;
        drmModeCreatePropertyBlob (self->fd, &self->hdr10meta, 1, &blob_id);
        err = drmModeObjectSetProperty (fd, self->conn_id, DRM_MODE_OBJECT_CONNECTOR, self->hdr_prop_id, blob_id);
        drmModeDestroyPropertyBlob (self->fd, blob_id);
        if (err)
          GST_ERROR_OBJECT (self, "reset blob property fail %d", err);
      }

      break;
    }
    case GST_STATE_CHANGE_READY_TO_NULL:
      if (self->run_time > 0) {
        GST_DEBUG ("Total showed frames (%lld), playing for (%"GST_TIME_FORMAT"), fps (%.3f).\n",
                self->frame_showed, GST_TIME_ARGS (self->run_time),
                (gfloat)GST_SECOND * self->frame_showed / self->run_time);
      }

      self->frame_showed = 0;
      self->run_time = 0;
      break;
    default:
      break;
  }

  return ret;
}

static GstBuffer *
gst_kms_sink_copy_to_dumb_buffer (GstKMSSink * self, GstBuffer * inbuf)
{
  GstFlowReturn ret;
  GstVideoFrame inframe, outframe;
  gboolean success;
  GstBuffer *buf = NULL;

  if (!gst_buffer_pool_set_active (self->pool, TRUE))
    goto activate_pool_failed;

  ret = gst_buffer_pool_acquire_buffer (self->pool, &buf, NULL);
  if (ret != GST_FLOW_OK)
    goto create_buffer_failed;

  if (!gst_video_frame_map (&inframe, &self->vinfo, inbuf, GST_MAP_READ))
    goto error_map_src_buffer;

  if (!gst_video_frame_map (&outframe, &self->vinfo, buf, GST_MAP_WRITE))
    goto error_map_dst_buffer;

  success = gst_video_frame_copy (&outframe, &inframe);
  gst_video_frame_unmap (&outframe);
  gst_video_frame_unmap (&inframe);
  if (!success)
    goto error_copy_buffer;

  return buf;

bail:
  {
    if (buf)
      gst_buffer_unref (buf);
    return NULL;
  }

  /* ERRORS */
activate_pool_failed:
  {
    GST_ELEMENT_ERROR (self, STREAM, FAILED, ("failed to activate buffer pool"),
        ("failed to activate buffer pool"));
    return NULL;
  }
create_buffer_failed:
  {
    GST_ELEMENT_ERROR (self, STREAM, FAILED, ("allocation failed"),
        ("failed to create buffer"));
    return NULL;
  }
error_copy_buffer:
  {
    GST_WARNING_OBJECT (self, "failed to upload buffer");
    goto bail;
  }
error_map_dst_buffer:
  {
    gst_video_frame_unmap (&inframe);
    /* fall-through */
  }
error_map_src_buffer:
  {
    GST_WARNING_OBJECT (self, "failed to map buffer");
    goto bail;
  }
}

static GstBuffer *
gst_kms_sink_get_input_buffer (GstKMSSink * self, GstBuffer * inbuf)
{
  GstMemory *mem;
  GstBuffer *buf = NULL;

  mem = gst_buffer_peek_memory (inbuf, 0);
  if (!mem)
    return NULL;

  if (gst_is_kms_memory (mem))
    return gst_buffer_ref (inbuf);

  if (gst_kms_sink_import_dmabuf (self, inbuf, &buf))
    goto done;

  GST_CAT_INFO_OBJECT (CAT_PERFORMANCE, self, "frame copy");
  buf = gst_kms_sink_copy_to_dumb_buffer (self, inbuf);

done:
  /* Copy all the non-memory related metas, this way CropMeta will be
   * available upon GstVideoOverlay::expose calls. */
  if (buf)
    gst_buffer_copy_into (buf, inbuf, GST_BUFFER_COPY_METADATA, 0, -1);

  return buf;
}

void
gst_kms_sink_config_hdr10 (GstKMSSink *self, GstBuffer * buf)
{
  guint blob_id = 0;
  int err;
  gint i;
  drmModeObjectPropertiesPtr props = NULL;
  drmModePropertyPtr prop = NULL;
  GstVideoHdr10Meta *meta = NULL;
  gint fd = get_commit_fd (self);

  if (self->conn_id < 0) {
    GST_ERROR_OBJECT (self, "no connector");
    return;
  }

  /* buf could be NULL when resize */
  if (buf)
    meta = gst_buffer_get_video_hdr10_meta (buf);

  if (meta && self->hdr10meta.eotf == 0) {
    GST_INFO_OBJECT (self, "redPrimary x=%d y=%d", meta->hdr10meta.redPrimary[0], meta->hdr10meta.redPrimary[1]);
    GST_INFO_OBJECT (self, "greenPrimary x=%d y=%d", meta->hdr10meta.greenPrimary[0], meta->hdr10meta.greenPrimary[1]);
    GST_INFO_OBJECT (self, "bluePrimary x=%d y=%d", meta->hdr10meta.bluePrimary[0], meta->hdr10meta.bluePrimary[1]);
    GST_INFO_OBJECT (self, "whitePoint x=%d y=%d", meta->hdr10meta.whitePoint[0], meta->hdr10meta.whitePoint[1]);
    GST_INFO_OBJECT (self, "maxMasteringLuminance %d", meta->hdr10meta.maxMasteringLuminance);
    GST_INFO_OBJECT (self, "minMasteringLuminance %d", meta->hdr10meta.minMasteringLuminance);
    GST_INFO_OBJECT (self, "maxContentLightLevel %d", meta->hdr10meta.maxContentLightLevel);
    GST_INFO_OBJECT (self, "maxFrameAverageLightLevel %d", meta->hdr10meta.maxFrameAverageLightLevel);
    GST_INFO_OBJECT (self, "transferCharacteristics %d", meta->hdr10meta.transferCharacteristics);
    GST_INFO_OBJECT (self, "colourPrimaries %d", meta->hdr10meta.colourPrimaries);
    GST_INFO_OBJECT (self, "matrixCoeffs %d", meta->hdr10meta.matrixCoeffs);
    GST_INFO_OBJECT (self, "fullRange %d", meta->hdr10meta.fullRange);
    GST_INFO_OBJECT (self, "chromaSampleLocTypeTopField %d", meta->hdr10meta.chromaSampleLocTypeTopField);
    GST_INFO_OBJECT (self, "chromaSampleLocTypeBottomField %d", meta->hdr10meta.chromaSampleLocTypeBottomField);

    /* FIXME: better to use marcos instead of const value */
    self->hdr10meta.eotf = 2;
    self->hdr10meta.type = 0;
    self->hdr10meta.display_primaries_x [0] = meta->hdr10meta.redPrimary[0];
    self->hdr10meta.display_primaries_x [1] = meta->hdr10meta.greenPrimary[0];
    self->hdr10meta.display_primaries_x [2] = meta->hdr10meta.bluePrimary[0];
    self->hdr10meta.display_primaries_y [0] = meta->hdr10meta.redPrimary[1];
    self->hdr10meta.display_primaries_y [1] = meta->hdr10meta.greenPrimary[1];
    self->hdr10meta.display_primaries_y [2] = meta->hdr10meta.bluePrimary[1];
    self->hdr10meta.white_point_x = meta->hdr10meta.whitePoint[0];
    self->hdr10meta.white_point_y = meta->hdr10meta.whitePoint[1];
    self->hdr10meta.max_mastering_display_luminance = (meta->hdr10meta.maxMasteringLuminance / 10000) & 0xffff;;
    self->hdr10meta.min_mastering_display_luminance = meta->hdr10meta.minMasteringLuminance & 0xffff;
    self->hdr10meta.max_fall = meta->hdr10meta.maxFrameAverageLightLevel;
    self->hdr10meta.max_cll =  meta->hdr10meta.maxContentLightLevel;

    if (!self->hdr_prop_id) {
      props = drmModeObjectGetProperties (self->fd, self->conn_id, DRM_MODE_OBJECT_CONNECTOR);
      for (i = 0; i < props->count_props; ++i) {
        prop = drmModeGetProperty(self->fd, props->props[i]);
        if (!strcmp(prop->name, "HDR_SOURCE_METADATA")) {
          GST_DEBUG_OBJECT (self, "found HDR_SOURCE_METADATA property on connector %d property id %d",
              self->conn_id, prop->prop_id);
          self->hdr_prop_id = prop->prop_id;
        }
        drmModeFreeProperty (prop);
        prop = NULL;
      }
    }

    if (self->hdr_prop_id == 0) {
      GST_WARNING_OBJECT (self, "no HDR_SOURCE_METADATA property found");
      return;
    }

    drmModeCreatePropertyBlob (self->fd, &self->hdr10meta, sizeof (self->hdr10meta), &blob_id);
    GST_INFO_OBJECT (self, "create blob id %d", blob_id);
    err = drmModeObjectSetProperty (fd, self->conn_id, DRM_MODE_OBJECT_CONNECTOR, self->hdr_prop_id, blob_id);
    drmModeDestroyPropertyBlob (self->fd, blob_id);
    if (err) {
      GST_ERROR_OBJECT (self, "set blob property fail %d", err);
      return;
    }
  }
}

static gboolean
gst_kms_sink_check_scale_ratio (GstKMSSink * self, GstVideoRectangle dst, GstVideoRectangle src)
{
  gboolean can_scale = TRUE;
  GST_INFO_OBJECT (self, "dst rectangle (%d, %d)-(%d x %d)", dst.x, dst.y, dst.w, dst.h);
  GST_INFO_OBJECT (self, "src rectangle (%d, %d)-(%d x %d)", src.x, src.y, src.w, src.h);

  can_scale = (dst.w * self->downscale_ratio >= src.w
              && dst.w <= src.w * self->upscale_ratio
              && dst.h * self->downscale_ratio >= src.h
              && dst.h <= src.h * self->upscale_ratio);

  GST_INFO_OBJECT (self, "can use hardware scale: %s", can_scale ? "TRUE" : "FALSE");
  return can_scale;
}

static GstFlowReturn
gst_kms_sink_show_frame (GstVideoSink * vsink, GstBuffer * buf)
{
  gint ret;
  GstBuffer *buffer = NULL;
  guint32 fb_id;
  GstKMSSink *self;
  GstVideoCropMeta *crop;
  GstVideoRectangle src = { 0, };
  GstVideoRectangle dst = { 0, };
  GstVideoRectangle result;
  GstPhyMemMeta *phymemmeta = NULL;
  guint64 dtrc_table_ofs;
  GstFlowReturn res;
  gboolean can_scale = TRUE;
  guint32 fmt, alignment;
  gint fd;
  
  self = GST_KMS_SINK (vsink);
  fd = get_commit_fd (self);

  res = GST_FLOW_ERROR;

  buffer = NULL;

  if (!self->display_connected) {
    GST_WARNING_OBJECT (self, "display not connected, drop this buffer");
    return GST_FLOW_OK;
  }

  if (strcmp (get_imx_drm_device_name(), "DPU") == 0) {
    fmt = gst_drm_format_from_video (GST_VIDEO_INFO_FORMAT (&self->vinfo));
    alignment = gst_drm_alignment_from_drm_format (fmt);
  } else {
    alignment = 1;
  }

  if (buf)
    buffer = gst_kms_sink_get_input_buffer (self, buf);
  else if (self->last_buffer)
    buffer = gst_buffer_ref (self->last_buffer);

  /* Make sure buf is not used accidentally */
  buf = NULL;

  if (!buffer)
    return GST_FLOW_ERROR;
  
  gst_kms_sink_config_hdr10 (self, buffer);

  fb_id = gst_kms_memory_get_fb_id (gst_buffer_peek_memory (buffer, 0));
  if (fb_id == 0)
    goto buffer_invalid;

  GST_TRACE_OBJECT (self, "displaying fb %d", fb_id);

  if (!self->is_kmsproperty_set) {
    phymemmeta = GST_PHY_MEM_META_GET (buffer);
    if (phymemmeta) {
      GST_DEBUG_OBJECT (self, "physical memory meta x_padding: %d y_padding: %d \
          RFC luma offset: %d chroma offset: %d",
          phymemmeta->x_padding, phymemmeta->y_padding, phymemmeta->rfc_luma_offset, phymemmeta->rfc_chroma_offset);
      dtrc_table_ofs = phymemmeta->rfc_luma_offset | ((guint64)phymemmeta->rfc_chroma_offset << 32);
      gst_kms_sink_set_kmsproperty (self, self->global_alpha, dtrc_table_ofs);
    } else
      gst_kms_sink_set_kmsproperty (self, self->global_alpha, 0);

    self->is_kmsproperty_set = TRUE;
  }

  GST_OBJECT_LOCK (self);
  if (self->modesetting_enabled) {
    self->buffer_id = fb_id;
    goto sync_frame;
  }

  if ((crop = gst_buffer_get_video_crop_meta (buffer))) {
    GstVideoInfo vinfo = self->vinfo;
    vinfo.width = crop->width;
    vinfo.height = crop->height;

    if (!gst_kms_sink_calculate_display_ratio (self, &vinfo))
      goto no_disp_ratio;

    src.x = crop->x;
    src.y = crop->y;
  }

  src.w = GST_VIDEO_SINK_WIDTH (self);
  src.h = GST_VIDEO_SINK_HEIGHT (self);

  dst.w = self->render_rect.w;
  dst.h = self->render_rect.h;

retry_set_plane:
  gst_video_sink_center_rect (src, dst, &result, can_scale);

  result.x = GST_ROUND_DOWN_N (result.x + self->render_rect.x, alignment);
  result.y = GST_ROUND_DOWN_N (result.y + self->render_rect.y, alignment);;

  if (result.x < 0 || result.y < 0) {
    /* FIXME: need improve cropping handle when DTRC is not enable */
    if (!check_vsi_tile_enable (self, buf)) {
      result.x = result.x < 0 ? 0 : result.x;
      result.y = result.y < 0 ? 0 : result.y;
    }
  }

  if (crop) {
    src.w = crop->width;
    src.h = crop->height;
  } else {
    src.w = GST_ROUND_DOWN_N (GST_VIDEO_INFO_WIDTH (&self->vinfo), alignment);
    src.h = GST_ROUND_DOWN_N (GST_VIDEO_INFO_HEIGHT (&self->vinfo), alignment);
  }

  if (!gst_kms_sink_check_scale_ratio (self, result, src)) {
    if (can_scale) {
      can_scale = FALSE;
      dst.w = MAX (self->hdisplay, src.w);
      dst.h = MAX (self->vdisplay, src.h);
      GST_WARNING_OBJECT (self, "try not scale");
      goto retry_set_plane;
    } else
      goto check_ratio_fail;
  }

  GST_TRACE_OBJECT (self,
      "scaling result at (%i,%i) %ix%i sourcing at (%i,%i) %ix%i",
      result.x, result.y, result.w, result.h, src.x, src.y, src.w, src.h);

  /* handle out of screen case */
  if ((result.x + result.w) > self->hdisplay) {
    gint crop_width = self->hdisplay - result.x;
    if (crop_width > 0)
      src.w = GST_ROUND_UP_2 (crop_width * src.w / result.w);
    result.w = crop_width;
  }

  if ((result.y + result.h) > self->vdisplay) {
    gint crop_height = self->vdisplay - result.y;
    if (crop_height > 0)
      src.h = GST_ROUND_UP_2 (crop_height * src.h / result.h);
    result.h = crop_height;
  }

  if (result.w <= 0 || result.h <= 0 || src.h <= 0 || src.w <= 0) {
    GST_WARNING_OBJECT (self, "video is out of display range, use previous area");
    self->render_rect = self->last_rect;
    goto done;
  }

  GST_TRACE_OBJECT (self,
      "drmModeSetPlane at (%i,%i) %ix%i sourcing at (%i,%i) %ix%i",
      result.x, result.y, result.w, result.h, src.x, src.y, src.w, src.h);

  ret = drmModeSetPlane (fd, self->plane_id, self->crtc_id, fb_id, 0,
      result.x, result.y, result.w, result.h,
      /* source/cropping coordinates are given in Q16 */
      src.x << 16, src.y << 16, src.w << 16, src.h << 16);
  if (ret) {
    goto set_plane_failed;
  } else
    goto done;

sync_frame:
  /* Wait for the previous frame to complete redraw */
  if (!gst_kms_sink_sync (self)) {
    GST_OBJECT_UNLOCK (self);
    goto bail;
  }

done:
  if (buffer != self->last_buffer)
    gst_buffer_replace (&self->last_buffer, buffer);
  g_clear_pointer (&self->tmp_kmsmem, gst_memory_unref);

  GST_OBJECT_UNLOCK (self);
  res = GST_FLOW_OK;

  self->frame_showed++;
  self->last_rect = self->render_rect;

bail:
  if (buf) {
    guint i;

    if (self->hold_buf[DEFAULT_HOLD_BUFFER_NUM-1])
      gst_buffer_unref (self->hold_buf[DEFAULT_HOLD_BUFFER_NUM-1]);

    for (i = DEFAULT_HOLD_BUFFER_NUM - 1; i > 0; i--)
      self->hold_buf[i] = self->hold_buf[i-1];

    self->hold_buf[0] = gst_buffer_ref (buf);
  }

  gst_buffer_unref (buffer);
  return res;

  /* ERRORS */
buffer_invalid:
  {
    GST_ERROR_OBJECT (self, "invalid buffer: it doesn't have a fb id");
    goto bail;
  }
set_plane_failed:
  {
    GST_OBJECT_UNLOCK (self);
    GST_DEBUG_OBJECT (self, "result = { %d, %d, %d, %d} / "
        "src = { %d, %d, %d %d } / dst = { %d, %d, %d %d }", result.x, result.y,
        result.w, result.h, src.x, src.y, src.w, src.h, dst.x, dst.y, dst.w,
        dst.h);
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
        (NULL), ("drmModeSetPlane failed: %s (%d)", strerror (-ret), ret));
    goto bail;
  }
no_disp_ratio:
  {
    GST_OBJECT_UNLOCK (self);
    GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, (NULL),
        ("Error calculating the output display ratio of the video."));
    goto bail;
  }
check_ratio_fail:
  {
    GST_OBJECT_UNLOCK (self);
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED, (NULL),
        ("Checking scale ratio fail."));
    goto bail;
  }
}

static void
gst_kms_sink_drain (GstKMSSink * self)
{
  GstParentBufferMeta *parent_meta;

  GST_DEBUG_OBJECT (self, "draining");

  if (!self->last_buffer)
    return;

  /* We only need to return the last_buffer if it depends on upstream buffer.
   * In this case, the last_buffer will have a GstParentBufferMeta set. */
  parent_meta = gst_buffer_get_parent_buffer_meta (self->last_buffer);
  if (parent_meta) {
    GstBuffer *dumb_buf;
    dumb_buf = gst_kms_sink_copy_to_dumb_buffer (self, parent_meta->buffer);
    gst_kms_allocator_clear_cache (self->allocator);
    gst_kms_sink_show_frame (GST_VIDEO_SINK (self), dumb_buf);
    gst_buffer_unref (dumb_buf);
  }
}

static gboolean
gst_kms_sink_query (GstBaseSink * bsink, GstQuery * query)
{
  GstKMSSink *self = GST_KMS_SINK (bsink);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_ALLOCATION:
    case GST_QUERY_DRAIN:
    {
      gst_kms_sink_drain (self);
      break;
    }
    default:
      break;
  }

  return GST_BASE_SINK_CLASS (parent_class)->query (bsink, query);
}

static void
gst_kms_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstKMSSink *sink;

  sink = GST_KMS_SINK (object);

  switch (prop_id) {
    case PROP_DRIVER_NAME:
      g_free (sink->devname);
      sink->devname = g_value_dup_string (value);
      break;
    case PROP_BUS_ID:
      g_free (sink->bus_id);
      sink->bus_id = g_value_dup_string (value);
      break;
    case PROP_CONNECTOR_ID:
      sink->conn_id = g_value_get_int (value);
      break;
    case PROP_PLANE_ID:
      sink->plane_id = g_value_get_int (value);
      break;
    case PROP_FORCE_MODESETTING:
      sink->modesetting_enabled = g_value_get_boolean (value);
      break;
    case PROP_CAN_SCALE:
      sink->can_scale = g_value_get_boolean (value);
      break;
    case PROP_GLOBAL_ALPHA:
      sink->global_alpha = g_value_get_int (value);
      break;
    case PROP_FORCE_HANTROTILE:
      sink->hantro_tile_enabled = g_value_get_boolean (value);
      break;
    default:
      if (!gst_video_overlay_set_property (object, PROP_N, prop_id, value))
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_kms_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstKMSSink *sink;

  sink = GST_KMS_SINK (object);

  switch (prop_id) {
    case PROP_DRIVER_NAME:
      g_value_set_string (value, sink->devname);
      break;
    case PROP_BUS_ID:
      g_value_take_string (value, sink->bus_id);
      break;
    case PROP_CONNECTOR_ID:
      g_value_set_int (value, sink->conn_id);
      break;
    case PROP_PLANE_ID:
      g_value_set_int (value, sink->plane_id);
      break;
    case PROP_FORCE_MODESETTING:
      g_value_set_boolean (value, sink->modesetting_enabled);
      break;
    case PROP_CAN_SCALE:
      g_value_set_boolean (value, sink->can_scale);
      break;
    case PROP_DISPLAY_WIDTH:
      GST_OBJECT_LOCK (sink);
      g_value_set_int (value, sink->hdisplay);
      GST_OBJECT_UNLOCK (sink);
      break;
    case PROP_DISPLAY_HEIGHT:
      GST_OBJECT_LOCK (sink);
      g_value_set_int (value, sink->vdisplay);
      GST_OBJECT_UNLOCK (sink);
      break;
    case PROP_GLOBAL_ALPHA:
      g_value_set_int (value, sink->global_alpha);
      break;
    case PROP_FORCE_HANTROTILE:
      g_value_set_boolean (value, sink->hantro_tile_enabled);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_kms_sink_finalize (GObject * object)
{
  GstKMSSink *sink;

  sink = GST_KMS_SINK (object);
  g_clear_pointer (&sink->devname, g_free);
  g_clear_pointer (&sink->bus_id, g_free);
  gst_poll_free (sink->poll);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_kms_sink_init (GstKMSSink * sink)
{
  sink->fd = -1;
  sink->ctrl_fd = -1;
  sink->conn_id = -1;
  sink->plane_id = -1;
  sink->primary_plane_id = -1;
  sink->hdr_prop_id = 0;
  sink->can_scale = TRUE;
  sink->scale_checked = FALSE;
  sink->upscale_ratio = 1;
  sink->downscale_ratio = 1;
  sink->hantro_tile_enabled = FALSE;
  gst_poll_fd_init (&sink->pollfd);
  sink->poll = gst_poll_new (TRUE);
  gst_video_info_init (&sink->vinfo);
  sink->frame_showed = 0;
  sink->run_time = 0;
  sink->global_alpha = 0;
}

static void
gst_kms_sink_class_init (GstKMSSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;
  GstBaseSinkClass *basesink_class;
  GstVideoSinkClass *videosink_class;
  GstCaps *caps;

  gobject_class = G_OBJECT_CLASS (klass);
  element_class = GST_ELEMENT_CLASS (klass);
  basesink_class = GST_BASE_SINK_CLASS (klass);
  videosink_class = GST_VIDEO_SINK_CLASS (klass);

  gst_element_class_set_static_metadata (element_class, "KMS video sink",
      "Sink/Video", GST_PLUGIN_DESC, "Víctor Jáquez <vjaquez@igalia.com>");

  caps = gst_kms_sink_caps_template_fill ();
  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps));
  gst_caps_unref (caps);

  element_class->change_state = GST_DEBUG_FUNCPTR (gst_kms_sink_change_state);
  basesink_class->start = GST_DEBUG_FUNCPTR (gst_kms_sink_start);
  basesink_class->stop = GST_DEBUG_FUNCPTR (gst_kms_sink_stop);
  basesink_class->set_caps = GST_DEBUG_FUNCPTR (gst_kms_sink_set_caps);
  basesink_class->get_caps = GST_DEBUG_FUNCPTR (gst_kms_sink_get_caps);
  basesink_class->propose_allocation = gst_kms_sink_propose_allocation;
  basesink_class->query = gst_kms_sink_query;

  videosink_class->show_frame = gst_kms_sink_show_frame;

  gobject_class->finalize = gst_kms_sink_finalize;
  gobject_class->set_property = gst_kms_sink_set_property;
  gobject_class->get_property = gst_kms_sink_get_property;

  /**
   * kmssink:driver-name:
   *
   * If you have a system with multiple GPUs, you can choose which GPU
   * to use setting the DRM device driver name. Otherwise, the first
   * one from an internal list is used.
   */
  g_properties[PROP_DRIVER_NAME] = g_param_spec_string ("driver-name",
      "device name", "DRM device driver name", NULL,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

  /**
   * kmssink:bus-id:
   *
   * If you have a system with multiple displays for the same driver-name,
   * you can choose which display to use by setting the DRM bus ID. Otherwise,
   * the driver decides which one.
   */
  g_properties[PROP_BUS_ID] = g_param_spec_string ("bus-id",
      "Bus ID", "DRM bus ID", NULL,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

  /**
   * kmssink:connector-id:
   *
   * A GPU has several output connectors, for example: LVDS, VGA,
   * HDMI, etc. By default the first LVDS is tried, then the first
   * eDP, and at the end, the first connected one.
   */
  g_properties[PROP_CONNECTOR_ID] = g_param_spec_int ("connector-id",
      "Connector ID", "DRM connector id", -1, G_MAXINT32, -1,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

   /**
   * kmssink:plane-id:
   *
   * There could be several planes associated with a CRTC.
   * By default the first plane that's possible to use with a given
   * CRTC is tried.
   */
  g_properties[PROP_PLANE_ID] = g_param_spec_int ("plane-id",
      "Plane ID", "DRM plane id", -1, G_MAXINT32, -1,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

  /**
   * kmssink:force-modesetting:
   *
   * If the output connector is already active, the sink automatically uses an
   * overlay plane. Enforce mode setting in the kms sink and output to the
   * base plane to override the automatic behavior.
   */
  g_properties[PROP_FORCE_MODESETTING] =
      g_param_spec_boolean ("force-modesetting", "Force modesetting",
      "When enabled, the sink try to configure the display mode", FALSE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);
  
  /**
   * kmssink:force-hantrotile:
   *
   * If enable, the sink propose hantro tile modifier to VPU.
   */
  g_properties[PROP_FORCE_HANTROTILE] =
      g_param_spec_boolean ("force-hantrotile", "Force to use hantro tile",
      "When enabled, the sink propose hantro tile modifier to VPU", FALSE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

  /**
   * kmssink:can-scale:
   *
   * User can tell kmssink if the driver can support scale.
   */
  g_properties[PROP_CAN_SCALE] =
      g_param_spec_boolean ("can-scale", "can scale",
      "User can tell kmssink if the driver can support scale", TRUE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

  /**
   * kmssink:display-width
   *
   * Actual width of the display. This is read only and only available in
   * PAUSED and PLAYING state. It's meant to be used with
   * gst_video_overlay_set_render_rectangle() function.
   */
  g_properties[PROP_DISPLAY_WIDTH] =
      g_param_spec_int ("display-width", "Display Width",
      "Width of the display surface in pixels", 0, G_MAXINT, 0,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * kmssink:display-height
   *
   * Actual height of the display. This is read only and only available in
   * PAUSED and PLAYING state. It's meant to be used with
   * gst_video_overlay_set_render_rectangle() function.
   */
  g_properties[PROP_DISPLAY_HEIGHT] =
      g_param_spec_int ("display-height", "Display Height",
      "Height of the display surface in pixels", 0, G_MAXINT, 0,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

   /**
   * kmssink:global-alpha:
   *
   * configure global alpha on mscale
   */
  g_properties[PROP_GLOBAL_ALPHA] = g_param_spec_int ("global-alpha",
      "global alpha", "global alpha", 0, 255, 0,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

  g_object_class_install_properties (gobject_class, PROP_N, g_properties);

  gst_video_overlay_install_properties (gobject_class, PROP_N);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GstRank rank = GST_RANK_SECONDARY;

  if (HAS_DPU()) {
    if (HAS_VPU())
      rank = IMX_GST_PLUGIN_RANK;
  } else if (HAS_DCSS()) {
    rank = IMX_GST_PLUGIN_RANK;
  }

  if (!gst_element_register (plugin, GST_PLUGIN_NAME, rank,
          GST_TYPE_KMS_SINK))
    return FALSE;

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, kms,
    GST_PLUGIN_DESC, plugin_init, VERSION, GST_LICENSE, GST_PACKAGE_NAME,
    GST_PACKAGE_ORIGIN)
