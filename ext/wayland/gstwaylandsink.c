/* GStreamer Wayland video sink
 *
 * Copyright (C) 2011 Intel Corporation
 * Copyright (C) 2011 Sreerenj Balachandran <sreerenj.balachandran@intel.com>
 * Copyright (C) 2012 Wim Taymans <wim.taymans@gmail.com>
 * Copyright (C) 2014 Collabora Ltd.
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
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 */

/**
 * SECTION:element-waylandsink
 * @title: waylandsink
 *
 *  The waylandsink is creating its own window and render the decoded video frames to that.
 *  Setup the Wayland environment as described in
 *  <ulink url="http://wayland.freedesktop.org/building.html">Wayland</ulink> home page.
 *  The current implementaion is based on weston compositor.
 *
 * ## Example pipelines
 * |[
 * gst-launch-1.0 -v videotestsrc ! waylandsink
 * ]| test the video rendering in wayland
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gstwaylandsink.h"
#include "wlvideoformat.h"
#include "wlbuffer.h"
#include "wlshmallocator.h"
#include "wllinuxdmabuf.h"

#include "gstimxcommon.h"

#include <gst/wayland/wayland.h>
#include <gst/video/videooverlay.h>
#include <gst/video/gstvideohdr10meta.h>

#ifdef HAVE_ION_ALLOCATOR
#include <gst/allocators/gstionmemory.h>
#endif

#include <drm_fourcc_imx.h>
#include <libdrm/drm_mode_imx.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

/* signals */
enum
{
  SIGNAL_0,
  LAST_SIGNAL
};

/* Properties */
enum
{
  PROP_0,
  PROP_WINDOW_WIDTH,
  PROP_WINDOW_HEIGHT,
  PROP_DISPLAY,
  PROP_ALPHA,
  PROP_ENABLE_TILE,
  PROP_FULLSCREEN
};

GST_DEBUG_CATEGORY (gstwayland_debug);
#define GST_CAT_DEFAULT gstwayland_debug

#define WL_VIDEO_FORMATS \
    "{ BGRx, BGRA, RGBx, xBGR, xRGB, RGBA, ABGR, ARGB, RGB, BGR, " \
    "RGB16, BGR16, YUY2, YVYU, UYVY, AYUV, NV12, NV21, NV16, " \
    "YUV9, YVU9, Y41B, I420, YV12, Y42B, v308 }"

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (WL_VIDEO_FORMATS) ";"
        GST_VIDEO_CAPS_MAKE_WITH_FEATURES (GST_CAPS_FEATURE_MEMORY_DMABUF,
            WL_VIDEO_FORMATS))
    );

static void gst_wayland_sink_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);
static void gst_wayland_sink_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_wayland_sink_finalize (GObject * object);

static GstStateChangeReturn gst_wayland_sink_change_state (GstElement * element,
    GstStateChange transition);
static void gst_wayland_sink_set_context (GstElement * element,
    GstContext * context);

static GstCaps *gst_wayland_sink_get_caps (GstBaseSink * bsink,
    GstCaps * filter);
static gboolean gst_wayland_sink_set_caps (GstBaseSink * bsink, GstCaps * caps);
static gboolean
gst_wayland_sink_propose_allocation (GstBaseSink * bsink, GstQuery * query);
static gboolean gst_wayland_sink_show_frame (GstVideoSink * bsink,
    GstBuffer * buffer);
static void gst_wayland_sink_config_hdr10 (GstWaylandSink *sink, GstBuffer * buf);

/* VideoOverlay interface */
static void gst_wayland_sink_videooverlay_init (GstVideoOverlayInterface *
    iface);
static void gst_wayland_sink_set_window_handle (GstVideoOverlay * overlay,
    guintptr handle);
static void gst_wayland_sink_set_render_rectangle (GstVideoOverlay * overlay,
    gint x, gint y, gint w, gint h);
static void gst_wayland_sink_expose (GstVideoOverlay * overlay);

/* WaylandVideo interface */
static void gst_wayland_sink_waylandvideo_init (GstWaylandVideoInterface *
    iface);
static void gst_wayland_sink_begin_geometry_change (GstWaylandVideo * video);
static void gst_wayland_sink_end_geometry_change (GstWaylandVideo * video);

#define gst_wayland_sink_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstWaylandSink, gst_wayland_sink, GST_TYPE_VIDEO_SINK,
    G_IMPLEMENT_INTERFACE (GST_TYPE_VIDEO_OVERLAY,
        gst_wayland_sink_videooverlay_init)
    G_IMPLEMENT_INTERFACE (GST_TYPE_WAYLAND_VIDEO,
        gst_wayland_sink_waylandvideo_init));

/* A tiny GstVideoBufferPool subclass that modify the options to remove
 * VideoAlignment. To support VideoAlignment we would need to pass the padded
 * width/height + stride and use the viewporter interface to crop, a bit like
 * we use to do with XV. It would still be quite limited. It's a bit retro,
 * hopefully there will be a better Wayland interface in the future. */

GType gst_wayland_pool_get_type (void);

typedef struct
{
  GstVideoBufferPool parent;
} GstWaylandPool;

typedef struct
{
  GstVideoBufferPoolClass parent;
} GstWaylandPoolClass;

G_DEFINE_TYPE (GstWaylandPool, gst_wayland_pool, GST_TYPE_VIDEO_BUFFER_POOL);

static const gchar **
gst_wayland_pool_get_options (GstBufferPool * pool)
{
  static const gchar *options[] = { GST_BUFFER_POOL_OPTION_VIDEO_META, NULL };
  return options;
}

static void
gst_wayland_pool_class_init (GstWaylandPoolClass * klass)
{
  GstBufferPoolClass *pool_class = GST_BUFFER_POOL_CLASS (klass);
  pool_class->get_options = gst_wayland_pool_get_options;
}

static void
gst_wayland_pool_init (GstWaylandPool * pool)
{
}

static void
gst_wayland_sink_class_init (GstWaylandSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;
  GstVideoSinkClass *gstvideosink_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;
  gstvideosink_class = (GstVideoSinkClass *) klass;

  gobject_class->set_property = gst_wayland_sink_set_property;
  gobject_class->get_property = gst_wayland_sink_get_property;
  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_wayland_sink_finalize);

  gst_element_class_add_static_pad_template (gstelement_class, &sink_template);

  gst_element_class_set_static_metadata (gstelement_class,
      "wayland video sink", "Sink/Video",
      "Output to wayland surface",
      "Sreerenj Balachandran <sreerenj.balachandran@intel.com>, "
      "George Kiagiadakis <george.kiagiadakis@collabora.com>");

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_wayland_sink_change_state);
  gstelement_class->set_context =
      GST_DEBUG_FUNCPTR (gst_wayland_sink_set_context);

  gstbasesink_class->get_caps = GST_DEBUG_FUNCPTR (gst_wayland_sink_get_caps);
  gstbasesink_class->set_caps = GST_DEBUG_FUNCPTR (gst_wayland_sink_set_caps);
  gstbasesink_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_wayland_sink_propose_allocation);

  gstvideosink_class->show_frame =
      GST_DEBUG_FUNCPTR (gst_wayland_sink_show_frame);

  g_object_class_install_property (gobject_class, PROP_WINDOW_WIDTH,
      g_param_spec_int ("window-width", "Wayland sink window width", "Wayland "
          "sink preferred window width in pixel",
          -1, G_MAXINT, -1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_WINDOW_HEIGHT,
      g_param_spec_int ("window-height", "Wayland sink window height", "Wayland "
          "sink preferred window height in pixel",
          -1, G_MAXINT, -1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_DISPLAY,
      g_param_spec_string ("display", "Wayland Display name", "Wayland "
          "display name to connect to, if not supplied via the GstContext",
          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_ALPHA,
      g_param_spec_float ("alpha", "Wayland surface alpha", "Wayland "
          "surface alpha value, apply custom alpha value to wayland surface",
          0.0f, 1.0f, 0.0f, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_ENABLE_TILE,
      g_param_spec_boolean ("enable-tile", "enable hantro tile",
      "When enabled, the sink propose VSI tile modifier to VPU", FALSE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT));

  g_object_class_install_property (gobject_class, PROP_FULLSCREEN,
      g_param_spec_boolean ("fullscreen", "Fullscreen",
      "Configure internally created windows as fullscreen"
      " (does not affect external windows)", TRUE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT));
}

static void
gst_wayland_sink_init (GstWaylandSink * sink)
{
  sink->alpha = 0.0f;
  sink->preferred_width = -1;
  sink->preferred_height = -1;
  g_mutex_init (&sink->display_lock);
  g_mutex_init (&sink->render_lock);
  g_cond_init (&sink->redraw_wait);
  sink->frame_showed = 0;
  sink->run_time = 0;
  sink->enable_tile = FALSE;
  sink->fullscreen = TRUE;
}

static void
gst_wayland_sink_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstWaylandSink *sink = GST_WAYLAND_SINK (object);

  switch (prop_id) {
    case PROP_DISPLAY:
      GST_OBJECT_LOCK (sink);
      g_value_set_string (value, sink->display_name);
      GST_OBJECT_UNLOCK (sink);
      break;
    case PROP_WINDOW_WIDTH:
      g_value_set_int (value, sink->preferred_width);
      break;
    case PROP_WINDOW_HEIGHT:
      g_value_set_int (value, sink->preferred_height);
      break;
    case PROP_ALPHA:
      g_value_set_float (value, sink->alpha);
      break;
    case PROP_ENABLE_TILE:
      g_value_set_boolean (value, sink->enable_tile);
      break;
    case PROP_FULLSCREEN:
      g_value_set_boolean (value, sink->fullscreen);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_wayland_sink_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstWaylandSink *sink = GST_WAYLAND_SINK (object);

  switch (prop_id) {
    case PROP_DISPLAY:
      GST_OBJECT_LOCK (sink);
      sink->display_name = g_value_dup_string (value);
      GST_OBJECT_UNLOCK (sink);
      break;
    case PROP_WINDOW_WIDTH:
      sink->preferred_width = g_value_get_int (value);
      break;
    case PROP_WINDOW_HEIGHT:
      sink->preferred_height = g_value_get_int (value);
      break;
    case PROP_ALPHA:
      sink->alpha = g_value_get_float (value);
      break;
    case PROP_ENABLE_TILE:
      sink->enable_tile = g_value_get_boolean (value);
      break;
    case PROP_FULLSCREEN:
      sink->fullscreen = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_wayland_sink_finalize (GObject * object)
{
  GstWaylandSink *sink = GST_WAYLAND_SINK (object);

  GST_DEBUG_OBJECT (sink, "Finalizing the sink..");

  if (sink->last_buffer)
    gst_buffer_unref (sink->last_buffer);
  if (sink->display)
    g_object_unref (sink->display);
  if (sink->window)
    g_object_unref (sink->window);
  if (sink->pool)
    gst_object_unref (sink->pool);

  g_free (sink->display_name);

  g_mutex_clear (&sink->display_lock);
  g_mutex_clear (&sink->render_lock);
  g_cond_clear (&sink->redraw_wait);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

/* must be called with the display_lock */
static void
gst_wayland_sink_set_display_from_context (GstWaylandSink * sink,
    GstContext * context)
{
  struct wl_display *display;
  GError *error = NULL;

  display = gst_wayland_display_handle_context_get_handle (context);
  sink->display = gst_wl_display_new_existing (display, FALSE, &error);

  if (error) {
    GST_ELEMENT_WARNING (sink, RESOURCE, OPEN_READ_WRITE,
        ("Could not set display handle"),
        ("Failed to use the external wayland display: '%s'", error->message));
    g_error_free (error);
  }
}

static gboolean
gst_wayland_sink_find_display (GstWaylandSink * sink)
{
  GstQuery *query;
  GstMessage *msg;
  GstContext *context = NULL;
  GError *error = NULL;
  gboolean ret = TRUE;

  g_mutex_lock (&sink->display_lock);

  if (!sink->display) {
    /* first query upstream for the needed display handle */
    query = gst_query_new_context (GST_WAYLAND_DISPLAY_HANDLE_CONTEXT_TYPE);
    if (gst_pad_peer_query (GST_VIDEO_SINK_PAD (sink), query)) {
      gst_query_parse_context (query, &context);
      gst_wayland_sink_set_display_from_context (sink, context);
    }
    gst_query_unref (query);

    if (G_LIKELY (!sink->display)) {
      /* now ask the application to set the display handle */
      msg = gst_message_new_need_context (GST_OBJECT_CAST (sink),
          GST_WAYLAND_DISPLAY_HANDLE_CONTEXT_TYPE);

      g_mutex_unlock (&sink->display_lock);
      gst_element_post_message (GST_ELEMENT_CAST (sink), msg);
      /* at this point we expect gst_wayland_sink_set_context
       * to get called and fill sink->display */
      g_mutex_lock (&sink->display_lock);

      if (!sink->display) {
        /* if the application didn't set a display, let's create it ourselves */
        GST_OBJECT_LOCK (sink);
        sink->display = gst_wl_display_new (sink->display_name, &error);
        GST_OBJECT_UNLOCK (sink);

        if (error) {
          GST_ELEMENT_WARNING (sink, RESOURCE, OPEN_READ_WRITE,
              ("Could not initialise Wayland output"),
              ("Failed to create GstWlDisplay: '%s'", error->message));
          g_error_free (error);
          ret = FALSE;
        }
      }
    }
  }

  g_mutex_unlock (&sink->display_lock);

  return ret;
}

static GstStateChangeReturn
gst_wayland_sink_change_state (GstElement * element, GstStateChange transition)
{
  GstWaylandSink *sink = GST_WAYLAND_SINK (element);
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!gst_wayland_sink_find_display (sink))
        return GST_STATE_CHANGE_FAILURE;
      sink->display->preferred_width = sink->preferred_width;
      sink->display->preferred_height = sink->preferred_height;
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      sink->run_time = gst_element_get_start_time (element);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_buffer_replace (&sink->last_buffer, NULL);
      gst_wayland_sink_config_hdr10 (sink, NULL);
      if (sink->window) {
        gst_wl_window_set_alpha(sink->window, 1.0);
        wl_surface_damage (sink->window->area_surface, 0, 0,
              sink->window->render_rectangle.w, sink->window->render_rectangle.h);
        wl_surface_commit (sink->window->area_surface);
        wl_display_roundtrip (sink->display->display);
        if (gst_wl_window_is_toplevel (sink->window)) {
          g_clear_object (&sink->window);
        } else {
          /* remove buffer from surface, show nothing */
          gst_wl_window_render (sink->window, NULL, NULL);
        }
      }
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      g_mutex_lock (&sink->display_lock);
      /* If we had a toplevel window, we most likely have our own connection
       * to the display too, and it is a good idea to disconnect and allow
       * potentially the application to embed us with GstVideoOverlay
       * (which requires to re-use the same display connection as the parent
       * surface). If we didn't have a toplevel window, then the display
       * connection that we have is definitely shared with the application
       * and it's better to keep it around (together with the window handle)
       * to avoid requesting them again from the application if/when we are
       * restarted (GstVideoOverlay behaves like that in other sinks)
       */
      if (sink->display && !sink->window) {     /* -> the window was toplevel */
        g_clear_object (&sink->display);
        g_mutex_lock (&sink->render_lock);
        sink->redraw_pending = FALSE;
        g_mutex_unlock (&sink->render_lock);
      }
      g_mutex_unlock (&sink->display_lock);
      g_clear_object (&sink->pool);
      
      if (sink->run_time > 0) {
        GST_DEBUG ("Total showed frames (%lld), playing for (%"GST_TIME_FORMAT"), fps (%.3f).\n",
                sink->frame_showed, GST_TIME_ARGS (sink->run_time),
                (gfloat)GST_SECOND * sink->frame_showed / sink->run_time);
      }
      sink->frame_showed = 0;
      sink->run_time = 0;
      break;
    default:
      break;
  }

  return ret;
}

static void
gst_wayland_sink_set_context (GstElement * element, GstContext * context)
{
  GstWaylandSink *sink = GST_WAYLAND_SINK (element);

  if (gst_context_has_context_type (context,
          GST_WAYLAND_DISPLAY_HANDLE_CONTEXT_TYPE)) {
    g_mutex_lock (&sink->display_lock);
    if (G_LIKELY (!sink->display)) {
      gst_wayland_sink_set_display_from_context (sink, context);
    } else {
      GST_WARNING_OBJECT (element, "changing display handle is not supported");
      g_mutex_unlock (&sink->display_lock);
      return;
    }
    g_mutex_unlock (&sink->display_lock);
  }

  if (GST_ELEMENT_CLASS (parent_class)->set_context)
    GST_ELEMENT_CLASS (parent_class)->set_context (element, context);
}

static GstCaps *
gst_wayland_sink_get_caps (GstBaseSink * bsink, GstCaps * filter)
{
  GstWaylandSink *sink;
  GstCaps *caps;

  sink = GST_WAYLAND_SINK (bsink);

  caps = gst_pad_get_pad_template_caps (GST_VIDEO_SINK_PAD (sink));
  caps = gst_caps_make_writable (caps);

  g_mutex_lock (&sink->display_lock);

  if (sink->display) {
    GValue shm_list = G_VALUE_INIT, dmabuf_list = G_VALUE_INIT;
    GValue value = G_VALUE_INIT;
    GArray *formats;
    gint i;
    guint fmt;

    g_value_init (&shm_list, GST_TYPE_LIST);
    g_value_init (&dmabuf_list, GST_TYPE_LIST);

    /* Add corresponding shm formats */
    formats = sink->display->shm_formats;
    for (i = 0; i < formats->len; i++) {
      g_value_init (&value, G_TYPE_STRING);
      fmt = g_array_index (formats, uint32_t, i);
      g_value_set_static_string (&value, gst_wl_shm_format_to_string (fmt));
      gst_value_list_append_and_take_value (&shm_list, &value);
    }
    /** FIXME:work around for 10bit format not in the none capsfeature list
     * need vpu add memory:DMABuf capsfeature when output dmabuf
    */
    if (HAS_DCSS()) {
      g_value_init (&value, G_TYPE_STRING);
      g_value_set_static_string (&value,  gst_video_format_to_string(GST_VIDEO_FORMAT_NV12_10LE));
      gst_value_list_append_and_take_value (&shm_list, &value);
    }

    gst_structure_take_value (gst_caps_get_structure (caps, 0), "format",
        &shm_list);

    /* Add corresponding dmabuf formats */
    formats = sink->display->dmabuf_formats;
    for (i = 0; i < formats->len; i++) {
      g_value_init (&value, G_TYPE_STRING);
      fmt = g_array_index (formats, uint32_t, i);
      g_value_set_static_string (&value, gst_wl_dmabuf_format_to_string (fmt));
      gst_value_list_append_and_take_value (&dmabuf_list, &value);
    }

    gst_structure_take_value (gst_caps_get_structure (caps, 1), "format",
        &dmabuf_list);

    GST_DEBUG_OBJECT (sink, "display caps: %" GST_PTR_FORMAT, caps);
  }

  g_mutex_unlock (&sink->display_lock);

  if (filter) {
    GstCaps *intersection;

    intersection =
        gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (caps);
    caps = intersection;
  }

  return caps;
}

#define ISALIGNED(a, b) (!(a & (b-1)))

static GstBufferPool *
gst_wayland_create_pool (GstWaylandSink * sink, GstCaps * caps)
{
  GstBufferPool *pool = NULL;
  GstStructure *structure;
  gsize size = sink->video_info.size;
  GstAllocator *alloc = NULL;

  pool = g_object_new (gst_wayland_pool_get_type (), NULL);

#ifdef HAVE_ION_ALLOCATOR
  GstVideoInfo info;
  gst_video_info_from_caps (&info, caps);
  GstVideoFormat format = GST_VIDEO_INFO_FORMAT (&info);
  if (gst_wl_display_check_format_for_dmabuf (sink->display, format))
    alloc = gst_ion_allocator_obtain ();
#endif

  structure = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_set_params (structure, caps, size, 2, 0);
  if (!alloc)
    alloc = gst_wl_shm_allocator_get ();
  else {
    gint w = GST_VIDEO_INFO_WIDTH (&sink->video_info);
    gint h = GST_VIDEO_INFO_HEIGHT (&sink->video_info);
    if (!ISALIGNED (w, 16) || !ISALIGNED (h, 16)) {
      GstVideoAlignment alignment;

      memset (&alignment, 0, sizeof (GstVideoAlignment));
      alignment.padding_right = GST_ROUND_UP_N (w, 16) - w;
      alignment.padding_bottom = GST_ROUND_UP_N (h, 16) - h;

      GST_DEBUG ("align buffer pool, w(%d) h(%d), padding_right (%d), padding_bottom (%d)",
          w, h, alignment.padding_right, alignment.padding_bottom);

      gst_buffer_pool_config_add_option (structure, GST_BUFFER_POOL_OPTION_VIDEO_META);
      gst_buffer_pool_config_add_option (structure, GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);
      gst_buffer_pool_config_set_video_alignment (structure, &alignment);
    }
  }
  gst_buffer_pool_config_set_allocator (structure, alloc, NULL);
  if (!gst_buffer_pool_set_config (pool, structure)) {
    g_object_unref (pool);
    pool = NULL;
  }
  g_object_unref (alloc);

  return pool;
}

static gboolean
gst_wayland_sink_set_caps (GstBaseSink * bsink, GstCaps * caps)
{
  GstWaylandSink *sink;
  gboolean use_dmabuf;
  GstVideoFormat format;

  sink = GST_WAYLAND_SINK (bsink);

  GST_DEBUG_OBJECT (sink, "set caps %" GST_PTR_FORMAT, caps);

  /* extract info from caps */
  if (!gst_video_info_from_caps (&sink->video_info, caps))
    goto invalid_format;

  sink->src_info = sink->video_info;

  format = GST_VIDEO_INFO_FORMAT (&sink->video_info);
  sink->video_info_changed = TRUE;

  /* create a new pool for the new caps */
  if (sink->pool)
    gst_object_unref (sink->pool);
  sink->pool = gst_wayland_create_pool (sink, caps);

  use_dmabuf = gst_caps_features_contains (gst_caps_get_features (caps, 0),
      GST_CAPS_FEATURE_MEMORY_DMABUF);

  /* validate the format base on the memory type. */
  if (use_dmabuf) {
    if (!gst_wl_display_check_format_for_dmabuf (sink->display, format))
      goto unsupported_format;
  } else if (!gst_wl_display_check_format_for_shm (sink->display, format)) {
    goto unsupported_format;
  }

  sink->use_dmabuf = use_dmabuf;

  return TRUE;

invalid_format:
  {
    GST_ERROR_OBJECT (sink,
        "Could not locate image format from caps %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
unsupported_format:
  {
    GST_ERROR_OBJECT (sink, "Format %s is not available on the display",
        gst_video_format_to_string (format));
    return FALSE;
  }
}

static gboolean
gst_wayland_sink_propose_allocation (GstBaseSink * bsink, GstQuery * query)
{
  GstWaylandSink *sink = GST_WAYLAND_SINK (bsink);
  GstCaps *caps;
  GstBufferPool *pool = NULL;
  gboolean need_pool;
  GstAllocator *alloc = NULL;
  guint64 drm_modifier;

  gst_query_parse_allocation (query, &caps, &need_pool);

  drm_modifier = DRM_FORMAT_MOD_AMPHION_TILED;
  gst_query_add_allocation_dmabuf_meta (query, drm_modifier);

  if (sink->enable_tile && HAS_DCSS()) {
    drm_modifier = DRM_FORMAT_MOD_VSI_G1_TILED;
    gst_query_add_allocation_dmabuf_meta (query, drm_modifier);
    drm_modifier = DRM_FORMAT_MOD_VSI_G2_TILED;
    gst_query_add_allocation_dmabuf_meta (query, drm_modifier);
    drm_modifier = DRM_FORMAT_MOD_VSI_G2_TILED_COMPRESSED;
    gst_query_add_allocation_dmabuf_meta (query, drm_modifier);
  }

  if (need_pool)
    pool = gst_wayland_create_pool (sink, caps);

  if (pool) {
    GstStructure *config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_get_allocator (config, &alloc, NULL);
    g_object_ref (alloc);
    gst_structure_free (config);
  }

  gst_query_add_allocation_pool (query, pool, sink->video_info.size, 2, 0);
  if (pool)
    g_object_unref (pool);

  if (!alloc)
    alloc = gst_wl_shm_allocator_get ();
  gst_query_add_allocation_param (query, alloc, NULL);
  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);
  gst_query_add_allocation_meta (query, GST_VIDEO_CROP_META_API_TYPE, NULL);
  g_object_unref (alloc);

  return TRUE;
}

static void
frame_redraw_callback (void *data, struct wl_callback *callback, uint32_t time)
{
  GstWaylandSink *sink = data;

  GST_LOG ("frame_redraw_cb");

  g_mutex_lock (&sink->render_lock);
  sink->redraw_pending = FALSE;
  g_cond_signal (&sink->redraw_wait);
  g_mutex_unlock (&sink->render_lock);

  wl_callback_destroy (callback);
}

static const struct wl_callback_listener frame_callback_listener = {
  frame_redraw_callback
};

/* must be called with the render lock */
static void
render_last_buffer (GstWaylandSink * sink, gboolean redraw)
{
  GstWlBuffer *wlbuffer;
  const GstVideoInfo *info = NULL;
  struct wl_surface *surface;
  struct wl_callback *callback;

  wlbuffer = gst_buffer_get_wl_buffer (sink->last_buffer);
  surface = gst_wl_window_get_wl_surface (sink->window);

  sink->redraw_pending = TRUE;
  callback = wl_surface_frame (surface);
  wl_callback_add_listener (callback, &frame_callback_listener, sink);

  if (G_UNLIKELY (sink->video_info_changed && !redraw)) {
    info = &sink->video_info;
    sink->video_info_changed = FALSE;
  }
  gst_wl_window_render (sink->window, wlbuffer, info);
}

static void
gst_wayland_sink_config_hdr10 (GstWaylandSink *sink, GstBuffer * buf)
{
  GstVideoHdr10Meta *meta = NULL;
  GstWlDisplay *display = sink->display;
  guint32 eotf = 0;
  guint32 type = 0;
  guint32 display_primaries_red = 0;
  guint32 display_primaries_green = 0;
  guint32 display_primaries_blue = 0;
  guint32 white_point = 0;
  guint32 mastering_display_luminance = 0;
  guint32 max_cll = 0;
  guint32 max_fall = 0;

  /* buf could be NULL when resize */
  if (buf)
    meta = gst_buffer_get_video_hdr10_meta (buf);

  if (meta) {
    GST_INFO_OBJECT (sink, "redPrimary x=%d y=%d", meta->hdr10meta.redPrimary[0], meta->hdr10meta.redPrimary[1]);
    GST_INFO_OBJECT (sink, "greenPrimary x=%d y=%d", meta->hdr10meta.greenPrimary[0], meta->hdr10meta.greenPrimary[1]);
    GST_INFO_OBJECT (sink, "bluePrimary x=%d y=%d", meta->hdr10meta.bluePrimary[0], meta->hdr10meta.bluePrimary[1]);
    GST_INFO_OBJECT (sink, "whitePoint x=%d y=%d", meta->hdr10meta.whitePoint[0], meta->hdr10meta.whitePoint[1]);
    GST_INFO_OBJECT (sink, "maxMasteringLuminance %d", meta->hdr10meta.maxMasteringLuminance);
    GST_INFO_OBJECT (sink, "minMasteringLuminance %d", meta->hdr10meta.minMasteringLuminance);
    GST_INFO_OBJECT (sink, "maxContentLightLevel %d", meta->hdr10meta.maxContentLightLevel);
    GST_INFO_OBJECT (sink, "maxFrameAverageLightLevel %d", meta->hdr10meta.maxFrameAverageLightLevel);
    GST_INFO_OBJECT (sink, "transferCharacteristics %d", meta->hdr10meta.transferCharacteristics);
    GST_INFO_OBJECT (sink, "colourPrimaries %d", meta->hdr10meta.colourPrimaries);
    GST_INFO_OBJECT (sink, "matrixCoeffs %d", meta->hdr10meta.matrixCoeffs);
    GST_INFO_OBJECT (sink, "fullRange %d", meta->hdr10meta.fullRange);
    GST_INFO_OBJECT (sink, "chromaSampleLocTypeTopField %d", meta->hdr10meta.chromaSampleLocTypeTopField);
    GST_INFO_OBJECT (sink, "chromaSampleLocTypeBottomField %d", meta->hdr10meta.chromaSampleLocTypeBottomField);

    if ((meta->hdr10meta.transferCharacteristics == 16 || meta->hdr10meta.transferCharacteristics == 18)
        &&  GST_VIDEO_INFO_FORMAT (&sink->video_info) == GST_VIDEO_FORMAT_NV12_10LE) {
      eotf = SMPTE_ST2084;
      type = 0;
      display_primaries_red = (guint)(meta->hdr10meta.redPrimary[0] << 16 | meta->hdr10meta.redPrimary[1]);
      display_primaries_green = (guint)(meta->hdr10meta.greenPrimary[0] << 16 | meta->hdr10meta.greenPrimary[1]);
      display_primaries_blue = (guint)(meta->hdr10meta.bluePrimary[0] << 16 | meta->hdr10meta.bluePrimary[1]);
      white_point = (guint)(meta->hdr10meta.whitePoint[0] << 16 | meta->hdr10meta.whitePoint[1]);
      mastering_display_luminance = (guint)(((meta->hdr10meta.maxMasteringLuminance / 10000) & 0xffff) << 16
                                            | (meta->hdr10meta.minMasteringLuminance & 0xffff));
      max_cll = meta->hdr10meta.maxContentLightLevel;
      max_fall = meta->hdr10meta.maxFrameAverageLightLevel;
    }
  }

  if (display->hdr10_metadata) {
    zwp_hdr10_metadata_v1_set_metadata (display->hdr10_metadata,
        eotf,
        type,
        display_primaries_red,
        display_primaries_green,
        display_primaries_blue,
        white_point,
        mastering_display_luminance,
        max_cll,
        max_fall);
    wl_display_roundtrip (display->display);
  }
}

static GstFlowReturn
gst_wayland_sink_show_frame (GstVideoSink * vsink, GstBuffer * buffer)
{
  GstWaylandSink *sink = GST_WAYLAND_SINK (vsink);
  GstBuffer *to_render;
  GstWlBuffer *wlbuffer;
  GstVideoMeta *vmeta;
  GstVideoFormat format;
  GstVideoInfo old_vinfo;
  GstMemory *mem;
  struct wl_buffer *wbuf = NULL;

  GstFlowReturn ret = GST_FLOW_OK;

  g_mutex_lock (&sink->render_lock);

  GST_LOG_OBJECT (sink, "render buffer %p", buffer);

  if (G_UNLIKELY (!sink->window)) {
    /* ask for window handle. Unlock render_lock while doing that because
     * set_window_handle & friends will lock it in this context */
    g_mutex_unlock (&sink->render_lock);
    gst_video_overlay_prepare_window_handle (GST_VIDEO_OVERLAY (sink));
    g_mutex_lock (&sink->render_lock);

    if (!sink->window) {
      /* if we were not provided a window, create one ourselves */
      sink->window = gst_wl_window_new_toplevel (sink->display,
          &sink->video_info, &sink->render_lock, sink->fullscreen);
    }
    gst_wl_window_set_alpha (sink->window, sink->alpha);
  }

  while (sink->redraw_pending)
    g_cond_wait (&sink->redraw_wait, &sink->render_lock);

  /* make sure that the application has called set_render_rectangle() */
  if (G_UNLIKELY (sink->window->render_rectangle.w == 0))
    goto no_window_size;
  
  gst_wl_window_set_source_crop (sink->window, buffer);

  if (sink->frame_showed == 0)
    gst_wayland_sink_config_hdr10 (sink, buffer);

  wlbuffer = gst_buffer_get_wl_buffer (buffer);

  if (G_LIKELY (wlbuffer && wlbuffer->display == sink->display)) {
    GST_LOG_OBJECT (sink, "buffer %p has a wl_buffer from our display, "
        "writing directly", buffer);
    to_render = buffer;
    goto render;
  }

  /* update video info from video meta */
  mem = gst_buffer_peek_memory (buffer, 0);

  old_vinfo = sink->video_info;
  vmeta = gst_buffer_get_video_meta (buffer);
  if (vmeta) {
    gint i;

    for (i = 0; i < vmeta->n_planes; i++) {
      sink->video_info.offset[i] = vmeta->offset[i];
      sink->video_info.stride[i] = vmeta->stride[i];
    }
    sink->video_info.size = gst_buffer_get_size (buffer);
  }

  GST_LOG_OBJECT (sink, "buffer %p does not have a wl_buffer from our "
      "display, creating it", buffer);

  format = GST_VIDEO_INFO_FORMAT (&sink->video_info);
  if (gst_wl_display_check_format_for_dmabuf (sink->display, format)) {
    guint i, nb_dmabuf = 0;

    for (i = 0; i < gst_buffer_n_memory (buffer); i++)
      if (gst_is_dmabuf_memory (gst_buffer_peek_memory (buffer, i)))
        nb_dmabuf++;

    if (nb_dmabuf && (nb_dmabuf == gst_buffer_n_memory (buffer)))
      wbuf = gst_wl_linux_dmabuf_construct_wl_buffer (buffer, sink->display,
          &sink->video_info);
  }

  if (!wbuf && gst_wl_display_check_format_for_shm (sink->display, format)) {
    if (gst_buffer_n_memory (buffer) == 1 && gst_is_fd_memory (mem))
      wbuf = gst_wl_shm_memory_construct_wl_buffer (mem, sink->display,
          &sink->video_info);
  }

  /* If nothing worked, copy into our internal pool */
  if (!wbuf) {
    GstVideoFrame src, dst;

    /* rollback video info changes */
    sink->video_info = old_vinfo;

    GST_LOG_OBJECT (sink, "buffer %p cannot have a wl_buffer, "
        "copying to internal memory", buffer);

    /* sink->pool always exists (created in set_caps), but it may not
     * be active if upstream is not using it */
    if (!gst_buffer_pool_is_active (sink->pool)) {
      GstStructure *config;
      GstCaps *caps;

      config = gst_buffer_pool_get_config (sink->pool);
      gst_buffer_pool_config_get_params (config, &caps, NULL, NULL, NULL);

      /* revert back to default strides and offsets */
      gst_video_info_from_caps (&sink->video_info, caps);
      gst_buffer_pool_config_set_params (config, caps, sink->video_info.size,
          2, 0);

      /* This is a video pool, it should not fail with basic setings */
      if (!gst_buffer_pool_set_config (sink->pool, config) ||
          !gst_buffer_pool_set_active (sink->pool, TRUE))
        goto activate_failed;
    }

    ret = gst_buffer_pool_acquire_buffer (sink->pool, &to_render, NULL);
    if (ret != GST_FLOW_OK)
      goto no_buffer;

    wlbuffer = gst_buffer_get_wl_buffer (to_render);

    /* attach a wl_buffer if there isn't one yet */
    if (G_UNLIKELY (!wlbuffer)) {
      mem = gst_buffer_peek_memory (to_render, 0);
      if (gst_wl_display_check_format_for_dmabuf (sink->display, format)
          && gst_is_dmabuf_memory (mem)) {
        GstVideoInfo info = sink->video_info;

        if (sink->pool && gst_buffer_pool_is_active (sink->pool)) {
          GstStructure *config;
          GstVideoAlignment video_align;
          memset (&video_align, 0, sizeof(GstVideoAlignment));
          config = gst_buffer_pool_get_config (sink->pool);

          if (gst_buffer_pool_config_has_option (config,
                    GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT)) {
            gst_buffer_pool_config_get_video_alignment (config, &video_align);

            GST_DEBUG_OBJECT (sink, "pool has alignment (%d, %d) , (%d, %d)",
              video_align.padding_left, video_align.padding_top,
              video_align.padding_right, video_align.padding_bottom);

            gst_video_info_align (&info, &video_align);
          }
          gst_structure_free (config);
        }

        wbuf = gst_wl_linux_dmabuf_construct_wl_buffer (to_render, sink->display,
            &info);
      }

      if (!wbuf) {
        GST_DEBUG_OBJECT (sink, "no dmabuf available, try shm");
        wbuf = gst_wl_shm_memory_construct_wl_buffer (mem, sink->display,
            &sink->video_info);
      }

      if (G_UNLIKELY (!wbuf))
        goto no_wl_buffer_shm;

      gst_buffer_add_wl_buffer (to_render, wbuf, sink->display);
    }

    if (!gst_video_frame_map (&dst, &sink->video_info, to_render,
            GST_MAP_WRITE))
      goto dst_map_failed;

    if (!gst_video_frame_map (&src, &sink->src_info, buffer, GST_MAP_READ)) {
      gst_video_frame_unmap (&dst);
      goto src_map_failed;
    }

    gst_video_frame_copy (&dst, &src);

    gst_video_frame_unmap (&src);
    gst_video_frame_unmap (&dst);

    goto render;
  }

  if (!wbuf)
    goto no_wl_buffer;

  gst_buffer_add_wl_buffer (buffer, wbuf, sink->display);
  to_render = buffer;

render:
  /* drop double rendering */
  if (G_UNLIKELY (to_render == sink->last_buffer)) {
    GST_LOG_OBJECT (sink, "Buffer already being rendered");
    goto done;
  }

  gst_buffer_replace (&sink->last_buffer, to_render);
  render_last_buffer (sink, FALSE);

  if (buffer != to_render)
    gst_buffer_unref (to_render);
  goto done;

no_window_size:
  {
    GST_ELEMENT_ERROR (sink, RESOURCE, WRITE,
        ("Window has no size set"),
        ("Make sure you set the size after calling set_window_handle"));
    ret = GST_FLOW_ERROR;
    goto done;
  }
no_buffer:
  {
    GST_WARNING_OBJECT (sink, "could not create buffer");
    goto done;
  }
no_wl_buffer_shm:
  {
    GST_ERROR_OBJECT (sink, "could not create wl_buffer out of wl_shm memory");
    ret = GST_FLOW_ERROR;
    goto done;
  }
no_wl_buffer:
  {
    GST_ERROR_OBJECT (sink, "buffer %p cannot have a wl_buffer", buffer);
    ret = GST_FLOW_ERROR;
    goto done;
  }
activate_failed:
  {
    GST_ERROR_OBJECT (sink, "failed to activate bufferpool.");
    ret = GST_FLOW_ERROR;
    goto done;
  }
src_map_failed:
  {
    GST_ELEMENT_ERROR (sink, RESOURCE, READ,
        ("Video memory can not be read from userspace."), (NULL));
    ret = GST_FLOW_ERROR;
    goto done;
  }
dst_map_failed:
  {
    GST_ELEMENT_ERROR (sink, RESOURCE, WRITE,
        ("Video memory can not be written from userspace."), (NULL));
    ret = GST_FLOW_ERROR;
    goto done;
  }
done:
  {
    sink->frame_showed++;
    g_mutex_unlock (&sink->render_lock);
    return ret;
  }
}

static void
gst_wayland_sink_videooverlay_init (GstVideoOverlayInterface * iface)
{
  iface->set_window_handle = gst_wayland_sink_set_window_handle;
  iface->set_render_rectangle = gst_wayland_sink_set_render_rectangle;
  iface->expose = gst_wayland_sink_expose;
}

static void
gst_wayland_sink_set_window_handle (GstVideoOverlay * overlay, guintptr handle)
{
  GstWaylandSink *sink = GST_WAYLAND_SINK (overlay);
  struct wl_surface *surface = (struct wl_surface *) handle;

  g_return_if_fail (sink != NULL);

  if (sink->window != NULL) {
    GST_WARNING_OBJECT (sink, "changing window handle is not supported");
    return;
  }

  g_mutex_lock (&sink->render_lock);

  GST_DEBUG_OBJECT (sink, "Setting window handle %" GST_PTR_FORMAT,
      (void *) handle);

  g_clear_object (&sink->window);

  if (handle) {
    if (G_LIKELY (gst_wayland_sink_find_display (sink))) {
      /* we cannot use our own display with an external window handle */
      if (G_UNLIKELY (sink->display->own_display)) {
        GST_ELEMENT_ERROR (sink, RESOURCE, OPEN_READ_WRITE,
            ("Application did not provide a wayland display handle"),
            ("waylandsink cannot use an externally-supplied surface without "
                "an externally-supplied display handle. Consider providing a "
                "display handle from your application with GstContext"));
      } else {
        sink->window = gst_wl_window_new_in_surface (sink->display, surface,
            &sink->render_lock);
      }
    } else {
      GST_ERROR_OBJECT (sink, "Failed to find display handle, "
          "ignoring window handle");
    }
  }

  g_mutex_unlock (&sink->render_lock);
}

static void
gst_wayland_sink_set_render_rectangle (GstVideoOverlay * overlay,
    gint x, gint y, gint w, gint h)
{
  GstWaylandSink *sink = GST_WAYLAND_SINK (overlay);

  g_return_if_fail (sink != NULL);

  g_mutex_lock (&sink->render_lock);
  if (!sink->window) {
    g_mutex_unlock (&sink->render_lock);
    GST_WARNING_OBJECT (sink,
        "set_render_rectangle called without window, ignoring");
    return;
  }

  GST_DEBUG_OBJECT (sink, "window geometry changed to (%d, %d) %d x %d",
      x, y, w, h);
  gst_wl_window_set_render_rectangle (sink->window, x, y, w, h);

  g_mutex_unlock (&sink->render_lock);
}

static void
gst_wayland_sink_expose (GstVideoOverlay * overlay)
{
  GstWaylandSink *sink = GST_WAYLAND_SINK (overlay);

  g_return_if_fail (sink != NULL);

  GST_DEBUG_OBJECT (sink, "expose");

  g_mutex_lock (&sink->render_lock);
  if (sink->last_buffer && !sink->redraw_pending) {
    GST_DEBUG_OBJECT (sink, "redrawing last buffer");
    render_last_buffer (sink, TRUE);
  }
  g_mutex_unlock (&sink->render_lock);
}

static void
gst_wayland_sink_waylandvideo_init (GstWaylandVideoInterface * iface)
{
  iface->begin_geometry_change = gst_wayland_sink_begin_geometry_change;
  iface->end_geometry_change = gst_wayland_sink_end_geometry_change;
}

static void
gst_wayland_sink_begin_geometry_change (GstWaylandVideo * video)
{
  GstWaylandSink *sink = GST_WAYLAND_SINK (video);
  g_return_if_fail (sink != NULL);

  g_mutex_lock (&sink->render_lock);
  if (!sink->window || !sink->window->area_subsurface) {
    g_mutex_unlock (&sink->render_lock);
    GST_INFO_OBJECT (sink,
        "begin_geometry_change called without window, ignoring");
    return;
  }

  wl_subsurface_set_sync (sink->window->area_subsurface);
  g_mutex_unlock (&sink->render_lock);
}

static void
gst_wayland_sink_end_geometry_change (GstWaylandVideo * video)
{
  GstWaylandSink *sink = GST_WAYLAND_SINK (video);
  g_return_if_fail (sink != NULL);

  g_mutex_lock (&sink->render_lock);
  if (!sink->window || !sink->window->area_subsurface) {
    g_mutex_unlock (&sink->render_lock);
    GST_INFO_OBJECT (sink,
        "end_geometry_change called without window, ignoring");
    return;
  }

  wl_subsurface_set_desync (sink->window->area_subsurface);
  g_mutex_unlock (&sink->render_lock);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GstRank rank = GST_RANK_MARGINAL;

  GST_DEBUG_CATEGORY_INIT (gstwayland_debug, "waylandsink", 0,
      " wayland video sink");

  gst_wl_shm_allocator_register ();
  
  if (HAS_DPU()) {
    if (HAS_VPU())
      rank = IMX_GST_PLUGIN_RANK + 1;
  } else if (IS_IMX8MM()) {
      rank = IMX_GST_PLUGIN_RANK + 1;
  } else if (HAS_DCSS()) {
    rank = IMX_GST_PLUGIN_RANK + 1;
  }

  return gst_element_register (plugin, "waylandsink", rank,
      GST_TYPE_WAYLAND_SINK);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    waylandsink,
    "Wayland Video Sink", plugin_init, VERSION, "LGPL", GST_PACKAGE_NAME,
    GST_PACKAGE_ORIGIN)
