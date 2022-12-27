/* GStreamer
 * Copyright (C) 2022 Intel Corporation
 *     Author: U. Artie Eoff <ullysses.a.eoff@intel.com>
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
 * License along with this library; if not, write to the0
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-avvideocompare
 * @title: avvideocompare
 * @short_description: A libav based video compare element
 *
 * avvideocompare accepts two input video streams with the same width, height,
 * framerate and format.  The two incoming buffers are compared to each other
 * via the chosen compare method (e.g. ssim or psnr).
 *
 * The computed result for each comparison will be written to the file that
 * is specified by the stats-file property.  The default stats-file is stdout.
 *
 * The first incoming buffer is passed through, unchanged, to the srcpad.
 *
 * ## Sample pipelines
 * ```
 * gst-launch-1.0 videotestsrc num-buffers=100  \
 *   ! videobalance brightness=0.005 hue=0.005  \
 *   ! avvideocompare method=psnr name=cmp      \
 *   ! fakesink videotestsrc ! cmp.
 * ```
 * ```
 * gst-launch-1.0 videotestsrc num-buffers=100  \
 *   ! tee name=orig ! queue ! avenc_mjpeg      \
 *   ! jpegparse ! avdec_mjpeg                  \
 *   ! avvideocompare method=ssim name=cmp      \
 *   ! fakesink orig. ! queue ! cmp.
 * ```
 *
 * Since: 1.22
 *
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>

#include <glib/gprintf.h>

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideoaggregator.h>

#include "gstav.h"
#include "gstavcodecmap.h"

typedef enum
{
  GST_FFMPEGVIDCMP_METHOD_SSIM,
  GST_FFMPEGVIDCMP_METHOD_PSNR,
} GstFFMpegVidCmpMethod;

#define GST_FFMPEGVIDCMP_METHOD_TYPE (gst_ffmpegvidcmp_method_get_type())

static GType
gst_ffmpegvidcmp_method_get_type (void)
{
  static gsize g_type = 0;

  static const GEnumValue enum_values[] = {
    {GST_FFMPEGVIDCMP_METHOD_SSIM, "SSIM", "ssim"},
    {GST_FFMPEGVIDCMP_METHOD_PSNR, "PSNR", "psnr"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&g_type)) {
    const GType type =
        g_enum_register_static ("GstFFMpegVidCmpMethod", enum_values);
    g_once_init_leave (&g_type, type);

    gst_type_mark_as_plugin_api (type, 0);
  }
  return g_type;
}

GType gst_ffmpegvidcmp_get_type (void);

#define GST_TYPE_FFMPEGVIDCMP \
  (gst_ffmpegvidcmp_get_type())
#define GST_FFMPEGVIDCMP(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_FFMPEGVIDCMP,GstFFMpegVidCmp))
#define GST_FFMPEGVIDCMP_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_FFMPEGVIDCMP,GstFFMpegVidCmpClass))
#define GST_IS_FFMPEGVIDCMP(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_FFMPEGVIDCMP))
#define GST_IS_FFMPEGVIDCMP_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_FFMPEGVIDCMP))

typedef struct _GstFFMpegVidCmp GstFFMpegVidCmp;
typedef struct _GstFFMpegVidCmpClass GstFFMpegVidCmpClass;

struct _GstFFMpegVidCmp
{
  GstVideoAggregator parent;

  /* pads */
  GstPad *sinkpad1;
  GstPad *sinkpad2;

  /* negotiated format */
  gint width;
  gint height;
  gint fps_num;
  gint fps_denom;

  AVFilterGraph *filter_graph;
  AVFilterContext *in1_ctx;
  AVFilterContext *in2_ctx;
  AVFilterContext *out_ctx;
  enum AVPixelFormat pixfmt;

  GstFFMpegVidCmpMethod method;
  gchar *stats_file;
};

struct _GstFFMpegVidCmpClass
{
  GstVideoAggregatorClass parent_class;
};

#define DEFAULT_STATS_FILE "-"
#define DEFAULT_VIDCMP_METHOD GST_FFMPEGVIDCMP_METHOD_SSIM

enum
{
  PROP_0,
  PROP_STATS_FILE,
  PROP_VIDCMP_METHOD,
};

#define GST_FFMPEGVIDCMP_FORMATS "{ " \
  "ARGB, BGRA, ABGR, RGBA, xRGB, BGRx, xBGR, RGBx, RGB16, " \
  "GRAY8, NV12, NV21, YUY2, UYVY, I420, Y42B, Y444" \
  " }"

/* *INDENT-OFF* */
static GstStaticPadTemplate gst_ffmpegvidcmp_src_tmpl =
  GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (GST_FFMPEGVIDCMP_FORMATS)));
/* *INDENT-ON* */

/* *INDENT-OFF* */
static GstStaticPadTemplate gst_ffmpegvidcmp_sink1_tmpl =
  GST_STATIC_PAD_TEMPLATE ("sink1",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (GST_FFMPEGVIDCMP_FORMATS)));
/* *INDENT-ON* */

/* *INDENT-OFF* */
static GstStaticPadTemplate gst_ffmpegvidcmp_sink2_tmpl =
  GST_STATIC_PAD_TEMPLATE ("sink2",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (GST_FFMPEGVIDCMP_FORMATS)));
/* *INDENT-ON* */

/* Child proxy implementation */
static GObject *
gst_ffmpegvidcmp_child_proxy_get_child_by_index (GstChildProxy * proxy,
    guint index)
{
  GstFFMpegVidCmp *self = GST_FFMPEGVIDCMP (proxy);
  GObject *obj = NULL;

  GST_OBJECT_LOCK (self);
  obj = g_list_nth_data (GST_ELEMENT_CAST (self)->sinkpads, index);
  if (obj)
    gst_object_ref (obj);
  GST_OBJECT_UNLOCK (self);

  return obj;
}

static guint
gst_ffmpegvidcmp_child_proxy_get_children_count (GstChildProxy * proxy)
{
  guint count = 0;
  GstFFMpegVidCmp *self = GST_FFMPEGVIDCMP (proxy);

  GST_OBJECT_LOCK (self);
  count = GST_ELEMENT_CAST (self)->numsinkpads;
  GST_OBJECT_UNLOCK (self);

  return count;
}

static void
gst_ffmpegvidcmp_child_proxy_init (gpointer g_iface, gpointer iface_data)
{
  GstChildProxyInterface *iface = g_iface;

  iface->get_child_by_index = gst_ffmpegvidcmp_child_proxy_get_child_by_index;
  iface->get_children_count = gst_ffmpegvidcmp_child_proxy_get_children_count;
}

#define gst_ffmpegvidcmp_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstFFMpegVidCmp, gst_ffmpegvidcmp,
    GST_TYPE_VIDEO_AGGREGATOR, G_IMPLEMENT_INTERFACE (GST_TYPE_CHILD_PROXY,
        gst_ffmpegvidcmp_child_proxy_init));

static void
gst_ffmpegvidcmp_reset (GstFFMpegVidCmp * self)
{
  GST_OBJECT_LOCK (self);

  self->width = -1;
  self->height = -1;
  self->fps_num = 0;
  self->fps_denom = 1;
  self->pixfmt = AV_PIX_FMT_NONE;

  self->in1_ctx = NULL;
  self->in2_ctx = NULL;
  self->out_ctx = NULL;

  if (self->filter_graph)
    avfilter_graph_free (&self->filter_graph);

  GST_OBJECT_UNLOCK (self);
}

static gboolean
gst_ffmpegvidcmp_setcaps (GstFFMpegVidCmp * self, GstCaps * caps)
{
  GstVideoInfo vinfo;

  gst_video_info_init (&vinfo);
  if (!gst_video_info_from_caps (&vinfo, caps))
    return FALSE;

  GST_OBJECT_LOCK (self);

  self->width = GST_VIDEO_INFO_WIDTH (&vinfo);
  self->height = GST_VIDEO_INFO_HEIGHT (&vinfo);
  self->fps_num = GST_VIDEO_INFO_FPS_N (&vinfo);
  self->fps_denom = GST_VIDEO_INFO_FPS_D (&vinfo);

  self->pixfmt =
      gst_ffmpeg_videoformat_to_pixfmt (GST_VIDEO_INFO_FORMAT (&vinfo));
  if (self->pixfmt == AV_PIX_FMT_NONE) {
    GST_OBJECT_UNLOCK (self);
    GST_ERROR_OBJECT (self, "failed to find suitable ffmpeg pixfmt");
    return FALSE;
  }

  GST_OBJECT_UNLOCK (self);

  return TRUE;
}

static gboolean
gst_ffmpegvidcmp_sink_event (GstAggregator * agg, GstAggregatorPad * aggpad,
    GstEvent * event)
{
  GstFFMpegVidCmp *self = GST_FFMPEGVIDCMP (agg);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      if (!gst_ffmpegvidcmp_setcaps (self, caps)) {
        gst_event_unref (event);
        return FALSE;
      }
      break;
    }
    default:
      break;
  }

  return GST_AGGREGATOR_CLASS (parent_class)->sink_event (agg, aggpad, event);
}

static gint
_make_str (gchar ** dest, const gchar * format, ...)
{
  gint res;
  va_list var_args;

  va_start (var_args, format);
  res = g_vasprintf (dest, format, var_args);
  va_end (var_args);

  return res;
}

static gint
init_filter_graph (GstFFMpegVidCmp * self)
{
  AVFilterInOut *inputs = NULL;
  AVFilterInOut *outputs = NULL;
  GEnumClass *enum_class;
  GEnumValue *method;
  gchar *args = NULL;
  gint res = -1;

  enum_class = g_type_class_ref (GST_FFMPEGVIDCMP_METHOD_TYPE);
  method = g_enum_get_value (enum_class, self->method);
  g_type_class_unref (enum_class);
  if (!method) {
    GST_ERROR_OBJECT (self, "unknown compare method");
    return -1;
  }

  GST_INFO_OBJECT (self, "    method : %s", method->value_nick);
  GST_INFO_OBJECT (self, "stats-file : %s", self->stats_file);

  res = _make_str (&args,
      "buffer=video_size=%dx%d:pix_fmt=%d:time_base=1/1:pixel_aspect=0/1[in1];"
      "buffer=video_size=%dx%d:pix_fmt=%d:time_base=1/1:pixel_aspect=0/1[in2];"
      "[in1][in2]%s=f=\\'%s\\'[out];[out]buffersink",
      self->width, self->height, self->pixfmt, self->width, self->height,
      self->pixfmt, method->value_nick, self->stats_file);
  if (res < 0) {
    GST_ERROR_OBJECT (self, "failed to format filter graph arguments");
    return res;
  }

  self->filter_graph = avfilter_graph_alloc ();
  if (!self->filter_graph) {
    GST_ERROR_OBJECT (self, "failed to allocate filter graph");
    g_free (args);
    return -1;
  }

  res = avfilter_graph_parse2 (self->filter_graph, args, &inputs, &outputs);
  g_free (args);
  if (res < 0) {
    GST_ERROR_OBJECT (self, "failed to parse filter graph");
    return res;
  }

  if (inputs || outputs) {
    GST_ERROR_OBJECT (self, "unlinked inputs/outputs in filter graph");
    return -1;
  }

  res = avfilter_graph_config (self->filter_graph, NULL);
  if (res < 0) {
    GST_ERROR_OBJECT (self, "failed to configure filter graph");
    return res;
  }

  self->in1_ctx =
      avfilter_graph_get_filter (self->filter_graph, "Parsed_buffer_0");
  self->in2_ctx =
      avfilter_graph_get_filter (self->filter_graph, "Parsed_buffer_1");
  self->out_ctx =
      avfilter_graph_get_filter (self->filter_graph, "Parsed_buffersink_3");

  if (!self->in1_ctx || !self->in2_ctx || !self->out_ctx) {
    GST_ERROR_OBJECT (self, "failed to get filter contexts");
    return -1;
  }

  return res;
}

static gint
process_filter_graph (GstFFMpegVidCmp * self, AVFrame * in1, AVFrame * in2)
{
  AVFrame *out;
  gint res;

  if (!self->filter_graph) {
    res = init_filter_graph (self);
    if (res < 0)
      return res;
  }

  res = av_buffersrc_add_frame (self->in1_ctx, in1);
  if (res < 0)
    return res;

  res = av_buffersrc_add_frame (self->in2_ctx, in2);
  if (res < 0)
    return res;

  out = av_frame_alloc ();
  out->width = self->width;
  out->height = self->height;
  out->format = self->pixfmt;

  res = av_buffersink_get_frame (self->out_ctx, out);

  av_frame_unref (out);
  av_frame_free (&out);

  return res;
}

static void
_fill_avpicture (GstFFMpegVidCmp * self, AVFrame * picture,
    GstVideoFrame * vframe)
{
  gint i;

  for (i = 0; i < GST_VIDEO_FRAME_N_COMPONENTS (vframe); ++i) {
    picture->data[i] = GST_VIDEO_FRAME_COMP_DATA (vframe, i);
    picture->linesize[i] = GST_VIDEO_FRAME_COMP_STRIDE (vframe, i);
  }

  picture->width = GST_VIDEO_FRAME_WIDTH (vframe);
  picture->height = GST_VIDEO_FRAME_HEIGHT (vframe);
  picture->format = self->pixfmt;
}

static GstFlowReturn
gst_ffmpegvidcmp_aggregate_frames (GstVideoAggregator * vagg,
    GstBuffer * outbuf)
{
  GstFFMpegVidCmp *self = GST_FFMPEGVIDCMP (vagg);
  GstVideoFrame *ref, *cmp;
  GstVideoFrame out;
  GstFlowReturn res = GST_FLOW_OK;

  GST_OBJECT_LOCK (self);

  ref = gst_video_aggregator_pad_get_prepared_frame (
      GST_VIDEO_AGGREGATOR_PAD (self->sinkpad1));
  cmp = gst_video_aggregator_pad_get_prepared_frame (
      GST_VIDEO_AGGREGATOR_PAD (self->sinkpad2));

  if (ref && cmp) {
    AVFrame avref = { 0, };
    AVFrame avcmp = { 0, };

    _fill_avpicture (self, &avref, ref);
    _fill_avpicture (self, &avcmp, cmp);

    if (process_filter_graph (self, &avref, &avcmp) < 0)
      GST_WARNING_OBJECT (self, "Could not process filter graph");

    if (!gst_video_frame_map (&out, &vagg->info, outbuf, GST_MAP_WRITE))
    {
      res = GST_FLOW_ERROR;
      goto done;
    }
    gst_video_frame_copy (&out, ref);
    gst_video_frame_unmap (&out);
  } else {
    res = GST_FLOW_EOS;
  }

done:
  GST_OBJECT_UNLOCK (self);
  return res;
}

static GstCaps *
gst_ffmpegvidcmp_update_caps (GstVideoAggregator * vagg, GstCaps * caps)
{
  GstFFMpegVidCmp *self = GST_FFMPEGVIDCMP (vagg);
  GstVideoAggregatorPad *vaggpad = GST_VIDEO_AGGREGATOR_PAD (self->sinkpad1);

  return GST_VIDEO_AGGREGATOR_CLASS (parent_class)->update_caps (
      vagg, gst_video_info_to_caps (&vaggpad->info));
}

static void
gst_ffmpegvidcmp_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstFFMpegVidCmp *self = GST_FFMPEGVIDCMP (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_STATS_FILE:
    {
      if (self->filter_graph) {
        GST_WARNING_OBJECT (self, "changing the stats file after the filter "
            "graph is initialized is not supported");
        break;
      }
      g_free (self->stats_file);
      self->stats_file = g_strdup (g_value_get_string (value));
      break;
    }
    case PROP_VIDCMP_METHOD:
    {
      if (self->filter_graph) {
        GST_WARNING_OBJECT (self, "changing the method after the filter "
            "graph is initialized is not supported");
        break;
      }
      self->method = g_value_get_enum (value);
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_ffmpegvidcmp_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstFFMpegVidCmp *self = GST_FFMPEGVIDCMP (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_STATS_FILE:
      g_value_set_string (value, self->stats_file);
      break;
    case PROP_VIDCMP_METHOD:
      g_value_set_enum (value, self->method);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_ffmpegvidcmp_finalize (GObject * object)
{
  GstFFMpegVidCmp *self = GST_FFMPEGVIDCMP (object);

  g_free (self->stats_file);

  gst_ffmpegvidcmp_reset (self);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_ffmpegvidcmp_class_init (GstFFMpegVidCmpClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstAggregatorClass *agg_class = GST_AGGREGATOR_CLASS (klass);
  GstVideoAggregatorClass *vagg_class = GST_VIDEO_AGGREGATOR_CLASS (klass);

  gobject_class->set_property = gst_ffmpegvidcmp_set_property;
  gobject_class->get_property = gst_ffmpegvidcmp_get_property;
  gobject_class->finalize = (GObjectFinalizeFunc) gst_ffmpegvidcmp_finalize;

  g_object_class_install_property (gobject_class, PROP_STATS_FILE,
      g_param_spec_string ("stats-file", "Stats File Location",
          "Set file where to store per-frame difference information"
          ", '-' for stdout", DEFAULT_STATS_FILE, G_PARAM_READWRITE |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_VIDCMP_METHOD,
      g_param_spec_enum ("method", "Method", "Method to compare video frames",
          GST_FFMPEGVIDCMP_METHOD_TYPE, DEFAULT_VIDCMP_METHOD,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template_with_gtype (gstelement_class,
      &gst_ffmpegvidcmp_sink1_tmpl, GST_TYPE_VIDEO_AGGREGATOR_CONVERT_PAD);
  gst_element_class_add_static_pad_template_with_gtype (gstelement_class,
      &gst_ffmpegvidcmp_sink2_tmpl, GST_TYPE_VIDEO_AGGREGATOR_CONVERT_PAD);
  gst_element_class_add_static_pad_template_with_gtype (gstelement_class,
      &gst_ffmpegvidcmp_src_tmpl, GST_TYPE_AGGREGATOR_PAD);
  gst_element_class_set_static_metadata (gstelement_class,
      "A libav video compare element", "Filter/Compare/Video",
      "Compare Video", "U. Artie Eoff <ullysses.a.eoff@intel.com");

  agg_class->sink_event = gst_ffmpegvidcmp_sink_event;
  vagg_class->update_caps = gst_ffmpegvidcmp_update_caps;
  vagg_class->aggregate_frames = gst_ffmpegvidcmp_aggregate_frames;
}

static void
gst_ffmpegvidcmp_init (GstFFMpegVidCmp * self)
{
  gst_ffmpegvidcmp_reset (self);

  self->stats_file = g_strdup (DEFAULT_STATS_FILE);
  self->method = DEFAULT_VIDCMP_METHOD;

  self->sinkpad1 = gst_pad_new_from_template (
      gst_element_get_pad_template (GST_ELEMENT (self), "sink1"), "sink1");
  gst_element_add_pad (GST_ELEMENT (self), self->sinkpad1);

  self->sinkpad2 = gst_pad_new_from_template (
      gst_element_get_pad_template (GST_ELEMENT (self), "sink2"), "sink2");
  gst_element_add_pad (GST_ELEMENT (self), self->sinkpad2);
}

gboolean
gst_ffmpegvidcmp_register (GstPlugin * plugin)
{
  return gst_element_register (plugin, "avvideocompare", GST_RANK_NONE,
      GST_TYPE_FFMPEGVIDCMP);
}
