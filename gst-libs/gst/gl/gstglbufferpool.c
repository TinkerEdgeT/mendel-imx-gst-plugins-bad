/*
 * GStreamer
 * Copyright (C) 2012 Matthew Waters <>
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
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gl.h"
#include "gstglbufferpool.h"
#include "gstglutils.h"

#if GST_GL_HAVE_PHYMEM
#include <gst/gl/gstglphymemory.h>
#endif

/**
 * SECTION:gstglbufferpool
 * @title: GstGlBufferPool
 * @short_description: buffer pool for #GstGLBaseMemory objects
 * @see_also: #GstBufferPool, #GstGLBaseMemory, #GstGLMemory
 *
 * a #GstGLBufferPool is an object that allocates buffers with #GstGLBaseMemory
 *
 * A #GstGLBufferPool is created with gst_gl_buffer_pool_new()
 *
 * #GstGLBufferPool implements the VideoMeta buffer pool option
 * %GST_BUFFER_POOL_OPTION_VIDEO_META, the VideoAligment buffer pool option
 * %GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT as well as the OpenGL specific
 * %GST_BUFFER_POOL_OPTION_GL_SYNC_META buffer pool option.
 */

/* bufferpool */
struct _GstGLBufferPoolPrivate
{
  GstAllocator *allocator;
  GstGLVideoAllocationParams *gl_params;
  GstCaps *caps;
  gboolean add_videometa;
  gboolean add_glsyncmeta;
};

static void gst_gl_buffer_pool_finalize (GObject * object);

GST_DEBUG_CATEGORY_STATIC (GST_CAT_GL_BUFFER_POOL);
#define GST_CAT_DEFAULT GST_CAT_GL_BUFFER_POOL

#define GST_GL_BUFFER_POOL_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_GL_BUFFER_POOL, GstGLBufferPoolPrivate))

#define gst_gl_buffer_pool_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstGLBufferPool, gst_gl_buffer_pool,
    GST_TYPE_BUFFER_POOL, GST_DEBUG_CATEGORY_INIT (GST_CAT_GL_BUFFER_POOL,
        "glbufferpool", 0, "GL Buffer Pool"));

static const gchar **
gst_gl_buffer_pool_get_options (GstBufferPool * pool)
{
  static const gchar *options[] = { GST_BUFFER_POOL_OPTION_VIDEO_META,
    GST_BUFFER_POOL_OPTION_GL_SYNC_META,
    GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT,
    GST_BUFFER_POOL_OPTION_GL_TEXTURE_TARGET_2D,
    GST_BUFFER_POOL_OPTION_GL_TEXTURE_TARGET_RECTANGLE,
    NULL
  };

  return options;
}

static gboolean
gst_gl_buffer_pool_set_config (GstBufferPool * pool, GstStructure * config)
{
  GstGLBufferPool *glpool = GST_GL_BUFFER_POOL_CAST (pool);
  GstGLBufferPoolPrivate *priv = glpool->priv;
  GstVideoInfo info;
  GstCaps *caps = NULL;
  guint min_buffers, max_buffers;
  guint max_align, n;
  GstAllocator *allocator = NULL;
  GstAllocationParams alloc_params;
  GstGLTextureTarget tex_target;
  gboolean ret = TRUE;
  gint p;

  if (!gst_buffer_pool_config_get_params (config, &caps, NULL, &min_buffers,
          &max_buffers))
    goto wrong_config;

  if (caps == NULL)
    goto no_caps;

  /* now parse the caps from the config */
  if (!gst_video_info_from_caps (&info, caps))
    goto wrong_caps;

  GST_LOG_OBJECT (pool, "%dx%d, caps %" GST_PTR_FORMAT, info.width, info.height,
      caps);

  if (!gst_buffer_pool_config_get_allocator (config, &allocator, &alloc_params))
    goto wrong_config;

  gst_caps_replace (&priv->caps, caps);

  if (priv->allocator)
    gst_object_unref (priv->allocator);

  if (allocator) {
    if (!GST_IS_GL_MEMORY_ALLOCATOR (allocator) 
#if GST_GL_HAVE_PHYMEM
        && (g_strcmp0 (allocator->mem_type, GST_GL_PHY_MEM_ALLOCATOR) != 0)
#endif
        ) {
      gst_object_unref (allocator);
      goto wrong_allocator;
    } else {
      priv->allocator = gst_object_ref (allocator);
    }
  } else {
    priv->allocator =
        GST_ALLOCATOR (gst_gl_memory_allocator_get_default (glpool->context));
    g_assert (priv->allocator);
  }

  priv->add_videometa = gst_buffer_pool_config_has_option (config,
      GST_BUFFER_POOL_OPTION_VIDEO_META);
  priv->add_glsyncmeta = gst_buffer_pool_config_has_option (config,
      GST_BUFFER_POOL_OPTION_GL_SYNC_META);

  if (priv->gl_params)
    gst_gl_allocation_params_free ((GstGLAllocationParams *) priv->gl_params);
  priv->gl_params = (GstGLVideoAllocationParams *)
      gst_buffer_pool_config_get_gl_allocation_params (config);
  if (!priv->gl_params)
    priv->gl_params = gst_gl_video_allocation_params_new (glpool->context,
        &alloc_params, &info, -1, NULL, 0, 0);

  max_align = alloc_params.align;

  if (gst_buffer_pool_config_has_option (config,
          GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT)) {
    priv->add_videometa = TRUE;

    gst_buffer_pool_config_get_video_alignment (config,
        priv->gl_params->valign);

    for (n = 0; n < GST_VIDEO_MAX_PLANES; ++n)
      max_align |= priv->gl_params->valign->stride_align[n];

    for (n = 0; n < GST_VIDEO_MAX_PLANES; ++n)
      priv->gl_params->valign->stride_align[n] = max_align;

    gst_video_info_align (priv->gl_params->v_info, priv->gl_params->valign);

    gst_buffer_pool_config_set_video_alignment (config,
        priv->gl_params->valign);
  }

  if (alloc_params.align < max_align) {
    GST_WARNING_OBJECT (pool, "allocation params alignment %u is smaller "
        "than the max specified video stride alignment %u, fixing",
        (guint) alloc_params.align, max_align);

    alloc_params.align = max_align;
    gst_buffer_pool_config_set_allocator (config, allocator, &alloc_params);
    if (priv->gl_params->parent.alloc_params)
      gst_allocation_params_free (priv->gl_params->parent.alloc_params);
    priv->gl_params->parent.alloc_params =
        gst_allocation_params_copy (&alloc_params);
  }

  {
    GstStructure *s = gst_caps_get_structure (caps, 0);
    const gchar *target_str = gst_structure_get_string (s, "texture-target");
    gboolean multiple_texture_targets = FALSE;

    tex_target = priv->gl_params->target;
    if (target_str)
      tex_target = gst_gl_texture_target_from_string (target_str);

    if (gst_buffer_pool_config_has_option (config,
            GST_BUFFER_POOL_OPTION_GL_TEXTURE_TARGET_2D)) {
      if (tex_target && tex_target != GST_GL_TEXTURE_TARGET_2D)
        multiple_texture_targets = TRUE;
      tex_target = GST_GL_TEXTURE_TARGET_2D;
    }
    if (gst_buffer_pool_config_has_option (config,
            GST_BUFFER_POOL_OPTION_GL_TEXTURE_TARGET_RECTANGLE)) {
      if (tex_target && tex_target != GST_GL_TEXTURE_TARGET_RECTANGLE)
        multiple_texture_targets = TRUE;
      tex_target = GST_GL_TEXTURE_TARGET_RECTANGLE;
    }
    if (gst_buffer_pool_config_has_option (config,
            GST_BUFFER_POOL_OPTION_GL_TEXTURE_TARGET_EXTERNAL_OES)) {
      if (tex_target && tex_target != GST_GL_TEXTURE_TARGET_EXTERNAL_OES)
        multiple_texture_targets = TRUE;
      tex_target = GST_GL_TEXTURE_TARGET_EXTERNAL_OES;
    }

    if (!tex_target)
      tex_target = GST_GL_TEXTURE_TARGET_2D;

    if (multiple_texture_targets) {
      GST_WARNING_OBJECT (pool, "Multiple texture targets configured either "
          "through caps or buffer pool options");
      ret = FALSE;
    }

    priv->gl_params->target = tex_target;
  }

  /* Recalulate the size and offset as we don't add padding between planes. */
  priv->gl_params->v_info->size = 0;
  for (p = 0; p < GST_VIDEO_INFO_N_PLANES (priv->gl_params->v_info); p++) {
    priv->gl_params->v_info->offset[p] = priv->gl_params->v_info->size;
    priv->gl_params->v_info->size +=
        gst_gl_get_plane_data_size (priv->gl_params->v_info,
        priv->gl_params->valign, p);
  }

  gst_buffer_pool_config_set_params (config, caps,
      priv->gl_params->v_info->size, min_buffers, max_buffers);

  return GST_BUFFER_POOL_CLASS (parent_class)->set_config (pool, config) && ret;

  /* ERRORS */
wrong_config:
  {
    GST_WARNING_OBJECT (pool, "invalid config");
    return FALSE;
  }
no_caps:
  {
    GST_WARNING_OBJECT (pool, "no caps in config");
    return FALSE;
  }
wrong_caps:
  {
    GST_WARNING_OBJECT (pool,
        "failed getting geometry from caps %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
wrong_allocator:
  {
    GST_WARNING_OBJECT (pool, "Incorrect allocator type for this pool");
    return FALSE;
  }
}

static gboolean
gst_gl_buffer_pool_start (GstBufferPool * pool)
{
  return GST_BUFFER_POOL_CLASS (parent_class)->start (pool);
}

/* This function handles GstBuffer creation */
static GstFlowReturn
gst_gl_buffer_pool_alloc (GstBufferPool * pool, GstBuffer ** buffer,
    GstBufferPoolAcquireParams * params)
{
  GstGLMemoryAllocator *alloc;
  GstGLBufferPool *glpool = GST_GL_BUFFER_POOL_CAST (pool);
  GstGLBufferPoolPrivate *priv = glpool->priv;
  GstBuffer *buf;

  if (!(buf = gst_buffer_new ())) {
    goto no_buffer;
  }

#if GST_GL_HAVE_PHYMEM
  if ((g_strcmp0 (priv->allocator->mem_type, GST_GL_PHY_MEM_ALLOCATOR) == 0)) {
    if (!gst_gl_physical_memory_setup_buffer (priv->allocator, buf, priv->gl_params)) {
      GST_ERROR_OBJECT (pool, "Can't create physcial buffer.");
      return GST_FLOW_ERROR;
    }
    goto done;
  }
#endif

  alloc = GST_GL_MEMORY_ALLOCATOR (priv->allocator);
  if (!gst_gl_memory_setup_buffer (alloc, buf, priv->gl_params, NULL, NULL, 0))
    goto mem_create_failed;

done:
  if (priv->add_glsyncmeta)
    gst_buffer_add_gl_sync_meta (glpool->context, buf);

  *buffer = buf;

  return GST_FLOW_OK;

  /* ERROR */
no_buffer:
  {
    GST_WARNING_OBJECT (pool, "can't create image");
    return GST_FLOW_ERROR;
  }
mem_create_failed:
  {
    GST_WARNING_OBJECT (pool, "Could not create GL Memory");
    return GST_FLOW_ERROR;
  }
}

/**
 * gst_gl_buffer_pool_new:
 * @context: the #GstGLContext to use
 *
 * Returns: a #GstBufferPool that allocates buffers with #GstGLMemory
 */
GstBufferPool *
gst_gl_buffer_pool_new (GstGLContext * context)
{
  GstGLBufferPool *pool;

  pool = g_object_new (GST_TYPE_GL_BUFFER_POOL, NULL);
  pool->context = gst_object_ref (context);

  GST_LOG_OBJECT (pool, "new GL buffer pool for context %" GST_PTR_FORMAT,
      context);

  return GST_BUFFER_POOL_CAST (pool);
}

static void
gst_gl_buffer_pool_class_init (GstGLBufferPoolClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstBufferPoolClass *gstbufferpool_class = (GstBufferPoolClass *) klass;

  g_type_class_add_private (klass, sizeof (GstGLBufferPoolPrivate));

  gobject_class->finalize = gst_gl_buffer_pool_finalize;

  gstbufferpool_class->get_options = gst_gl_buffer_pool_get_options;
  gstbufferpool_class->set_config = gst_gl_buffer_pool_set_config;
  gstbufferpool_class->alloc_buffer = gst_gl_buffer_pool_alloc;
  gstbufferpool_class->start = gst_gl_buffer_pool_start;
}

static void
gst_gl_buffer_pool_init (GstGLBufferPool * pool)
{
  GstGLBufferPoolPrivate *priv = NULL;

  pool->priv = GST_GL_BUFFER_POOL_GET_PRIVATE (pool);
  priv = pool->priv;

  priv->allocator = NULL;
  priv->caps = NULL;
  priv->add_videometa = TRUE;
  priv->add_glsyncmeta = FALSE;
}

static void
gst_gl_buffer_pool_finalize (GObject * object)
{
  GstGLBufferPool *pool = GST_GL_BUFFER_POOL_CAST (object);
  GstGLBufferPoolPrivate *priv = pool->priv;

  GST_LOG_OBJECT (pool, "finalize GL buffer pool %p", pool);

  if (priv->caps)
    gst_caps_unref (priv->caps);

  G_OBJECT_CLASS (gst_gl_buffer_pool_parent_class)->finalize (object);

  /* only release the context once all our memory have been deleted */
  if (pool->context) {
    gst_object_unref (pool->context);
    pool->context = NULL;
  }

  if (priv->allocator) {
    gst_object_unref (priv->allocator);
    priv->allocator = NULL;
  }

  if (priv->gl_params)
    gst_gl_allocation_params_free ((GstGLAllocationParams *) priv->gl_params);
  priv->gl_params = NULL;
}

/**
 * gst_buffer_pool_config_get_gl_allocation_params:
 * @config: a buffer pool config
 *
 * Returns: (transfer full): the currently set #GstGLAllocationParams or %NULL
 */
GstGLAllocationParams *
gst_buffer_pool_config_get_gl_allocation_params (GstStructure * config)
{
  GstGLAllocationParams *ret;

  if (!gst_structure_get (config, "gl-allocation-params",
          GST_TYPE_GL_ALLOCATION_PARAMS, &ret, NULL))
    ret = NULL;

  return ret;
}

/**
 * gst_buffer_pool_config_set_gl_allocation_params:
 * @config: a buffer pool config
 * @params: (transfer none): a #GstGLAllocationParams
 *
 * Sets @params on @config
 */
void
gst_buffer_pool_config_set_gl_allocation_params (GstStructure * config,
    GstGLAllocationParams * params)
{
  g_return_if_fail (config != NULL);
  g_return_if_fail (params != NULL);

  gst_structure_set (config, "gl-allocation-params",
      GST_TYPE_GL_ALLOCATION_PARAMS, params, NULL);
}
