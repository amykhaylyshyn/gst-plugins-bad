/* 
 * Copyright (C) 2012 Collabora Ltd.
 *     Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstopenjpegdec.h"

GST_DEBUG_CATEGORY_STATIC (gst_openjpeg_dec_debug);
#define GST_CAT_DEFAULT gst_openjpeg_dec_debug

static gboolean gst_openjpeg_dec_start (GstVideoDecoder * decoder);
static gboolean gst_openjpeg_dec_stop (GstVideoDecoder * decoder);
static gboolean gst_openjpeg_dec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state);
static gboolean gst_openjpeg_dec_reset (GstVideoDecoder * decoder,
    gboolean hard);
static GstFlowReturn gst_openjpeg_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame);
static gboolean gst_openjpeg_dec_decide_allocation (GstVideoDecoder * decoder,
    GstQuery * query);

#if G_BYTE_ORDER == G_LITTLE_ENDIAN
#define GRAY16 "GRAY16_LE"
#define YUV10 "Y444_10LE, I422_10LE, I420_10LE"
#else
#define GRAY16 "GRAY16_BE"
#define YUV10 "Y444_10BE, I422_10BE, I420_10BE"
#endif

static GstStaticPadTemplate gst_openjpeg_dec_sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("image/x-j2c, "
        "colorspace = (string) { sRGB, sYUV, GRAY }; "
        "image/x-jpc, "
        "colorspace = (string) { sRGB, sYUV, GRAY }; " "image/jp2")
    );

static GstStaticPadTemplate gst_openjpeg_dec_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("{ ARGB64, ARGB, xRGB, "
            "AYUV64, " YUV10 ", "
            "AYUV, Y444, Y42B, I420, Y41B, YUV9, " "GRAY8, " GRAY16 " }"))
    );

#define parent_class gst_openjpeg_dec_parent_class
G_DEFINE_TYPE (GstOpenJPEGDec, gst_openjpeg_dec, GST_TYPE_VIDEO_DECODER);

static void
gst_openjpeg_dec_class_init (GstOpenJPEGDecClass * klass)
{
  GstElementClass *element_class;
  GstVideoDecoderClass *video_decoder_class;

  element_class = (GstElementClass *) klass;
  video_decoder_class = (GstVideoDecoderClass *) klass;

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_openjpeg_dec_src_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_openjpeg_dec_sink_template));

  gst_element_class_set_static_metadata (element_class,
      "OpenJPEG JPEG2000 decoder",
      "Codec/Decoder/Video",
      "Decode JPEG2000 streams",
      "Sebastian Dröge <sebastian.droege@collabora.co.uk>");

  video_decoder_class->start = GST_DEBUG_FUNCPTR (gst_openjpeg_dec_start);
  video_decoder_class->stop = GST_DEBUG_FUNCPTR (gst_openjpeg_dec_stop);
  video_decoder_class->reset = GST_DEBUG_FUNCPTR (gst_openjpeg_dec_reset);
  video_decoder_class->set_format =
      GST_DEBUG_FUNCPTR (gst_openjpeg_dec_set_format);
  video_decoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_openjpeg_dec_handle_frame);
  video_decoder_class->decide_allocation = gst_openjpeg_dec_decide_allocation;

  GST_DEBUG_CATEGORY_INIT (gst_openjpeg_dec_debug, "openjpegdec", 0,
      "VP8 Decoder");
}

static void
gst_openjpeg_dec_init (GstOpenJPEGDec * self)
{
  GstVideoDecoder *decoder = (GstVideoDecoder *) self;

  gst_video_decoder_set_packetized (decoder, TRUE);
  opj_set_default_decoder_parameters (&self->params);
  self->params.cp_limit_decoding = NO_LIMITATION;
}

static gboolean
gst_openjpeg_dec_start (GstVideoDecoder * decoder)
{
  GstOpenJPEGDec *self = GST_OPENJPEG_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Starting");

  return TRUE;
}

static gboolean
gst_openjpeg_dec_stop (GstVideoDecoder * video_decoder)
{
  GstOpenJPEGDec *self = GST_OPENJPEG_DEC (video_decoder);

  GST_DEBUG_OBJECT (self, "Stopping");

  if (self->output_state) {
    gst_video_codec_state_unref (self->output_state);
    self->output_state = NULL;
  }

  if (self->input_state) {
    gst_video_codec_state_unref (self->input_state);
    self->input_state = NULL;
  }

  GST_DEBUG_OBJECT (self, "Stopped");

  return TRUE;
}

static gboolean
gst_openjpeg_dec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state)
{
  GstOpenJPEGDec *self = GST_OPENJPEG_DEC (decoder);
  GstStructure *s;
  const gchar *color_space;

  GST_DEBUG_OBJECT (self, "Setting format: %" GST_PTR_FORMAT, state->caps);

  s = gst_caps_get_structure (state->caps, 0);

  self->color_space = CLRSPC_UNKNOWN;

  if (gst_structure_has_name (s, "image/jp2")) {
    self->codec_format = CODEC_JP2;
    self->is_jp2c = FALSE;
  } else if (gst_structure_has_name (s, "image/x-j2c")) {
    self->codec_format = CODEC_J2K;
    self->is_jp2c = TRUE;
  } else if (gst_structure_has_name (s, "image/x-jpc")) {
    self->codec_format = CODEC_J2K;
    self->is_jp2c = FALSE;
  } else {
    g_return_val_if_reached (FALSE);
  }

  if ((color_space = gst_structure_get_string (s, "colorspace"))) {
    if (g_str_equal (color_space, "sRGB"))
      self->color_space = CLRSPC_SRGB;
    else if (g_str_equal (color_space, "GRAY"))
      self->color_space = CLRSPC_GRAY;
    else if (g_str_equal (color_space, "sYUV"))
      self->color_space = CLRSPC_SYCC;
  }

  self->ncomps = 0;
  gst_structure_get_int (s, "num-components", &self->ncomps);

  if (self->input_state)
    gst_video_codec_state_unref (self->input_state);
  self->input_state = gst_video_codec_state_ref (state);

  return TRUE;
}

static gboolean
gst_openjpeg_dec_reset (GstVideoDecoder * decoder, gboolean hard)
{
  GstOpenJPEGDec *self = GST_OPENJPEG_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Resetting");

  if (self->output_state) {
    gst_video_codec_state_unref (self->output_state);
    self->output_state = NULL;
  }

  return TRUE;
}

static void
fill_frame_packed8_4 (GstVideoFrame * frame, opj_image_t * image)
{
  gint x, y, w, h;
  guint8 *data_out, *tmp;
  const gint *data_in[4];
  gint dstride;

  w = GST_VIDEO_FRAME_WIDTH (frame);
  h = GST_VIDEO_FRAME_HEIGHT (frame);
  data_out = GST_VIDEO_FRAME_PLANE_DATA (frame, 0);
  dstride = GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0);

  data_in[0] = image->comps[0].data;
  data_in[1] = image->comps[1].data;
  data_in[2] = image->comps[2].data;
  data_in[3] = image->comps[3].data;

  for (y = 0; y < h; y++) {
    tmp = data_out;

    for (x = 0; x < w; x++) {
      tmp[0] = *data_in[3];
      tmp[1] = *data_in[0];
      tmp[2] = *data_in[1];
      tmp[3] = *data_in[2];

      tmp += 4;
      data_in[0]++;
      data_in[1]++;
      data_in[2]++;
      data_in[3]++;
    }
    data_out += dstride;
  }
}

static void
fill_frame_packed16_4 (GstVideoFrame * frame, opj_image_t * image)
{
  gint x, y, w, h;
  guint16 *data_out, *tmp;
  const gint *data_in[4];
  gint dstride;
  gint shift[4];

  w = GST_VIDEO_FRAME_WIDTH (frame);
  h = GST_VIDEO_FRAME_HEIGHT (frame);
  data_out = GST_VIDEO_FRAME_PLANE_DATA (frame, 0);
  dstride = GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0) / 2;

  data_in[0] = image->comps[0].data;
  data_in[1] = image->comps[1].data;
  data_in[2] = image->comps[2].data;
  data_in[3] = image->comps[3].data;

  shift[0] = 16 - image->comps[0].prec;
  shift[1] = 16 - image->comps[1].prec;
  shift[2] = 16 - image->comps[2].prec;
  shift[3] = 16 - image->comps[3].prec;

  for (y = 0; y < h; y++) {
    tmp = data_out;

    for (x = 0; x < w; x++) {
      tmp[0] = *data_in[3] << shift[3];
      tmp[1] = *data_in[0] << shift[0];
      tmp[2] = *data_in[1] << shift[1];
      tmp[3] = *data_in[2] << shift[2];

      tmp += 4;
      data_in[0]++;
      data_in[1]++;
      data_in[2]++;
      data_in[3]++;
    }
    data_out += dstride;
  }
}

static void
fill_frame_packed8_3 (GstVideoFrame * frame, opj_image_t * image)
{
  gint x, y, w, h;
  guint8 *data_out, *tmp;
  const gint *data_in[3];
  gint dstride;

  w = GST_VIDEO_FRAME_WIDTH (frame);
  h = GST_VIDEO_FRAME_HEIGHT (frame);
  data_out = GST_VIDEO_FRAME_PLANE_DATA (frame, 0);
  dstride = GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0);

  data_in[0] = image->comps[0].data;
  data_in[1] = image->comps[1].data;
  data_in[2] = image->comps[2].data;

  for (y = 0; y < h; y++) {
    tmp = data_out;

    for (x = 0; x < w; x++) {
      tmp[1] = *data_in[0];
      tmp[2] = *data_in[1];
      tmp[3] = *data_in[2];

      tmp += 4;
      data_in[0]++;
      data_in[1]++;
      data_in[2]++;
    }
    data_out += dstride;
  }
}

static void
fill_frame_packed16_3 (GstVideoFrame * frame, opj_image_t * image)
{
  gint x, y, w, h;
  guint16 *data_out, *tmp;
  const gint *data_in[3];
  gint dstride;
  gint shift[3];

  w = GST_VIDEO_FRAME_WIDTH (frame);
  h = GST_VIDEO_FRAME_HEIGHT (frame);
  data_out = GST_VIDEO_FRAME_PLANE_DATA (frame, 0);
  dstride = GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0) / 2;

  data_in[0] = image->comps[0].data;
  data_in[1] = image->comps[1].data;
  data_in[2] = image->comps[2].data;

  shift[0] = 16 - image->comps[0].prec;
  shift[1] = 16 - image->comps[1].prec;
  shift[2] = 16 - image->comps[2].prec;

  for (y = 0; y < h; y++) {
    tmp = data_out;

    for (x = 0; x < w; x++) {
      tmp[1] = *data_in[0] << shift[0];
      tmp[2] = *data_in[1] << shift[1];
      tmp[3] = *data_in[2] << shift[2];

      tmp += 4;
      data_in[0]++;
      data_in[1]++;
      data_in[2]++;
    }
    data_out += dstride;
  }
}

static void
fill_frame_planar8_1 (GstVideoFrame * frame, opj_image_t * image)
{
  gint x, y, w, h;
  guint8 *data_out, *tmp;
  const gint *data_in;
  gint dstride;

  w = GST_VIDEO_FRAME_WIDTH (frame);
  h = GST_VIDEO_FRAME_HEIGHT (frame);
  data_out = GST_VIDEO_FRAME_PLANE_DATA (frame, 0);
  dstride = GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0);

  data_in = image->comps[0].data;

  for (y = 0; y < h; y++) {
    tmp = data_out;

    for (x = 0; x < w; x++) {
      *tmp = *data_in;

      tmp++;
      data_in++;
    }
    data_out += dstride;
  }
}

static void
fill_frame_planar16_1 (GstVideoFrame * frame, opj_image_t * image)
{
  gint x, y, w, h;
  guint16 *data_out, *tmp;
  const gint *data_in;
  gint dstride;
  gint shift;

  w = GST_VIDEO_FRAME_WIDTH (frame);
  h = GST_VIDEO_FRAME_HEIGHT (frame);
  data_out = GST_VIDEO_FRAME_PLANE_DATA (frame, 0);
  dstride = GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0) / 2;

  data_in = image->comps[0].data;

  shift = 16 - image->comps[0].prec;

  for (y = 0; y < h; y++) {
    tmp = data_out;

    for (x = 0; x < w; x++) {
      *tmp = *data_in << shift;

      tmp++;
      data_in++;
    }
    data_out += dstride;
  }
}

static void
fill_frame_planar8_3 (GstVideoFrame * frame, opj_image_t * image)
{
  gint c, x, y, w, h;
  guint8 *data_out, *tmp;
  const gint *data_in;
  gint dstride;

  for (c = 0; c < 3; c++) {
    w = GST_VIDEO_FRAME_COMP_WIDTH (frame, c);
    h = GST_VIDEO_FRAME_COMP_HEIGHT (frame, c);
    dstride = GST_VIDEO_FRAME_COMP_STRIDE (frame, c);
    data_out = GST_VIDEO_FRAME_COMP_DATA (frame, c);
    data_in = image->comps[c].data;

    for (y = 0; y < h; y++) {
      tmp = data_out;

      for (x = 0; x < w; x++) {
        *tmp = *data_in;
        tmp++;
        data_in++;
      }
      data_out += dstride;
    }
  }
}

static void
fill_frame_planar16_3 (GstVideoFrame * frame, opj_image_t * image)
{
  gint c, x, y, w, h;
  guint16 *data_out, *tmp;
  const gint *data_in;
  gint dstride;
  gint shift;

  for (c = 0; c < 3; c++) {
    w = GST_VIDEO_FRAME_COMP_WIDTH (frame, c);
    h = GST_VIDEO_FRAME_COMP_HEIGHT (frame, c);
    dstride = GST_VIDEO_FRAME_COMP_STRIDE (frame, c) / 2;
    data_out = (guint16 *) GST_VIDEO_FRAME_COMP_DATA (frame, c);
    data_in = image->comps[c].data;
    shift = 16 - image->comps[c].prec;

    for (y = 0; y < h; y++) {
      tmp = data_out;

      for (x = 0; x < w; x++) {
        *tmp = *data_in << shift;
        tmp++;
        data_in++;
      }
      data_out += dstride;
    }
  }
}

static void
fill_frame_planar8_3_generic (GstVideoFrame * frame, opj_image_t * image)
{
  gint x, y, w, h;
  guint8 *data_out, *tmp;
  const gint *data_in[3];
  gint dstride;
  gint dx[3], dy[3];

  w = GST_VIDEO_FRAME_WIDTH (frame);
  h = GST_VIDEO_FRAME_HEIGHT (frame);
  data_out = GST_VIDEO_FRAME_PLANE_DATA (frame, 0);
  dstride = GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0);

  data_in[0] = image->comps[0].data;
  data_in[1] = image->comps[1].data;
  data_in[2] = image->comps[2].data;

  dx[0] = image->comps[0].dx;
  dx[1] = image->comps[1].dx;
  dx[2] = image->comps[2].dx;

  dy[0] = image->comps[0].dy;
  dy[1] = image->comps[1].dy;
  dy[2] = image->comps[2].dy;

  for (y = 0; y < h; y++) {
    tmp = data_out;

    for (x = 0; x < w; x++) {
      tmp[0] = 0xff;
      tmp[1] = data_in[0][((y / dy[0]) * w + x) / dx[0]];
      tmp[2] = data_in[1][((y / dy[1]) * w + x) / dx[1]];
      tmp[3] = data_in[2][((y / dy[2]) * w + x) / dx[2]];
      tmp += 4;
    }
    data_out += dstride;
  }
}

static void
fill_frame_planar8_4_generic (GstVideoFrame * frame, opj_image_t * image)
{
  gint x, y, w, h;
  guint8 *data_out, *tmp;
  const gint *data_in[4];
  gint dstride;
  gint dx[4], dy[4];

  w = GST_VIDEO_FRAME_WIDTH (frame);
  h = GST_VIDEO_FRAME_HEIGHT (frame);
  data_out = GST_VIDEO_FRAME_PLANE_DATA (frame, 0);
  dstride = GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0);

  data_in[0] = image->comps[0].data;
  data_in[1] = image->comps[1].data;
  data_in[2] = image->comps[2].data;
  data_in[3] = image->comps[3].data;

  dx[0] = image->comps[0].dx;
  dx[1] = image->comps[1].dx;
  dx[2] = image->comps[2].dx;
  dx[3] = image->comps[3].dx;

  dy[0] = image->comps[0].dy;
  dy[1] = image->comps[1].dy;
  dy[2] = image->comps[2].dy;
  dy[3] = image->comps[3].dy;

  for (y = 0; y < h; y++) {
    tmp = data_out;

    for (x = 0; x < w; x++) {
      tmp[0] = data_in[3][((y / dy[3]) * w + x) / dx[3]];
      tmp[1] = data_in[0][((y / dy[0]) * w + x) / dx[0]];
      tmp[2] = data_in[1][((y / dy[1]) * w + x) / dx[1]];
      tmp[3] = data_in[2][((y / dy[2]) * w + x) / dx[2]];
      tmp += 4;
    }
    data_out += dstride;
  }
}

static void
fill_frame_planar16_3_generic (GstVideoFrame * frame, opj_image_t * image)
{
  gint x, y, w, h;
  guint16 *data_out, *tmp;
  const gint *data_in[3];
  gint dstride;
  gint dx[3], dy[3], shift[3];

  w = GST_VIDEO_FRAME_WIDTH (frame);
  h = GST_VIDEO_FRAME_HEIGHT (frame);
  data_out = (guint16 *) GST_VIDEO_FRAME_PLANE_DATA (frame, 0);
  dstride = GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0) / 2;

  data_in[0] = image->comps[0].data;
  data_in[1] = image->comps[1].data;
  data_in[2] = image->comps[2].data;

  dx[0] = image->comps[0].dx;
  dx[1] = image->comps[1].dx;
  dx[2] = image->comps[2].dx;

  dy[0] = image->comps[0].dy;
  dy[1] = image->comps[1].dy;
  dy[2] = image->comps[2].dy;

  shift[0] = 16 - image->comps[0].prec;
  shift[1] = 16 - image->comps[1].prec;
  shift[2] = 16 - image->comps[2].prec;

  for (y = 0; y < h; y++) {
    tmp = data_out;

    for (x = 0; x < w; x++) {
      tmp[0] = 0xff;
      tmp[1] = data_in[0][((y / dy[0]) * w + x) / dx[0]] << shift[0];
      tmp[2] = data_in[1][((y / dy[1]) * w + x) / dx[1]] << shift[1];
      tmp[3] = data_in[2][((y / dy[2]) * w + x) / dx[2]] << shift[2];
      tmp += 4;
    }
    data_out += dstride;
  }
}

static void
fill_frame_planar16_4_generic (GstVideoFrame * frame, opj_image_t * image)
{
  gint x, y, w, h;
  guint16 *data_out, *tmp;
  const gint *data_in[4];
  gint dstride;
  gint dx[4], dy[4], shift[4];

  w = GST_VIDEO_FRAME_WIDTH (frame);
  h = GST_VIDEO_FRAME_HEIGHT (frame);
  data_out = (guint16 *) GST_VIDEO_FRAME_PLANE_DATA (frame, 0);
  dstride = GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0) / 2;

  data_in[0] = image->comps[0].data;
  data_in[1] = image->comps[1].data;
  data_in[2] = image->comps[2].data;
  data_in[3] = image->comps[3].data;

  dx[0] = image->comps[0].dx;
  dx[1] = image->comps[1].dx;
  dx[2] = image->comps[2].dx;
  dx[3] = image->comps[3].dx;

  dy[0] = image->comps[0].dy;
  dy[1] = image->comps[1].dy;
  dy[2] = image->comps[2].dy;
  dy[3] = image->comps[3].dy;

  shift[0] = 16 - image->comps[0].prec;
  shift[1] = 16 - image->comps[1].prec;
  shift[2] = 16 - image->comps[2].prec;
  shift[3] = 16 - image->comps[3].prec;

  for (y = 0; y < h; y++) {
    tmp = data_out;

    for (x = 0; x < w; x++) {
      tmp[0] = data_in[3][((y / dy[3]) * w + x) / dx[3]] << shift[3];
      tmp[1] = data_in[0][((y / dy[0]) * w + x) / dx[0]] << shift[0];
      tmp[2] = data_in[1][((y / dy[1]) * w + x) / dx[1]] << shift[1];
      tmp[3] = data_in[2][((y / dy[2]) * w + x) / dx[2]] << shift[2];
      tmp += 4;
    }
    data_out += dstride;
  }
}

static gint
get_highest_prec (opj_image_t * image)
{
  gint i;
  gint ret = 0;

  for (i = 0; i < image->numcomps; i++)
    ret = MAX (image->comps[i].prec, ret);

  return ret;
}

static GstFlowReturn
gst_openjpeg_dec_negotiate (GstOpenJPEGDec * self, opj_image_t * image)
{
  GstVideoFormat format;
  gint width, height;

  if (image->color_space == CLRSPC_UNKNOWN || image->color_space == 0)
    image->color_space = self->color_space;

  switch (image->color_space) {
    case CLRSPC_SRGB:
      if (image->numcomps == 4) {
        if (image->comps[0].dx != 1 || image->comps[0].dy != 1 ||
            image->comps[1].dx != 1 || image->comps[1].dy != 1 ||
            image->comps[2].dx != 1 || image->comps[2].dy != 1 ||
            image->comps[3].dx != 1 || image->comps[3].dy != 1) {
          GST_ERROR_OBJECT (self, "Sub-sampling for RGB not supported");
          return GST_FLOW_NOT_NEGOTIATED;
        }

        if (get_highest_prec (image) == 8) {
          self->fill_frame = fill_frame_packed8_4;
          format = GST_VIDEO_FORMAT_ARGB;
        } else if (get_highest_prec (image) <= 16) {
          self->fill_frame = fill_frame_packed16_4;
          format = GST_VIDEO_FORMAT_ARGB64;
        } else {
          GST_ERROR_OBJECT (self, "Unsupported depth %d", image->comps[3].prec);
          return GST_FLOW_NOT_NEGOTIATED;
        }
      } else if (image->numcomps == 3) {
        if (image->comps[0].dx != 1 || image->comps[0].dy != 1 ||
            image->comps[1].dx != 1 || image->comps[1].dy != 1 ||
            image->comps[2].dx != 1 || image->comps[2].dy != 1) {
          GST_ERROR_OBJECT (self, "Sub-sampling for RGB not supported");
          return GST_FLOW_NOT_NEGOTIATED;
        }

        if (get_highest_prec (image) == 8) {
          self->fill_frame = fill_frame_packed8_3;
          format = GST_VIDEO_FORMAT_ARGB;
        } else if (get_highest_prec (image) <= 16) {
          self->fill_frame = fill_frame_packed16_3;
          format = GST_VIDEO_FORMAT_ARGB64;
        } else {
          GST_ERROR_OBJECT (self, "Unsupported depth %d",
              get_highest_prec (image));
          return GST_FLOW_NOT_NEGOTIATED;
        }
      } else {
        GST_ERROR_OBJECT (self, "Unsupported number of RGB components: %d",
            image->numcomps);
        return GST_FLOW_NOT_NEGOTIATED;
      }
      break;
    case CLRSPC_GRAY:
      if (image->numcomps == 1) {
        if (image->comps[0].dx != 1 && image->comps[0].dy != 1) {
          GST_ERROR_OBJECT (self, "Sub-sampling for GRAY not supported");
          return GST_FLOW_NOT_NEGOTIATED;
        }

        if (get_highest_prec (image) == 8) {
          self->fill_frame = fill_frame_planar8_1;
          format = GST_VIDEO_FORMAT_GRAY8;
        } else if (get_highest_prec (image) <= 16) {
          self->fill_frame = fill_frame_planar16_1;
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
          format = GST_VIDEO_FORMAT_GRAY16_LE;
#else
          format = GST_VIDEO_FORMAT_GRAY16_BE;
#endif
        } else {
          GST_ERROR_OBJECT (self, "Unsupported depth %d",
              get_highest_prec (image));
          return GST_FLOW_NOT_NEGOTIATED;
        }
      } else {
        GST_ERROR_OBJECT (self, "Unsupported number of GRAY components: %d",
            image->numcomps);
        return GST_FLOW_NOT_NEGOTIATED;
      }
      break;
    case CLRSPC_SYCC:
      if (image->numcomps != 3 && image->numcomps != 4) {
        GST_ERROR_OBJECT (self, "Unsupported number of YUV components: %d",
            image->numcomps);
        return GST_FLOW_NOT_NEGOTIATED;
      }

      if (image->comps[0].dx != 1 || image->comps[0].dy != 1) {
        GST_ERROR_OBJECT (self, "Sub-sampling of luma plane not supported");
        return GST_FLOW_NOT_NEGOTIATED;
      }

      if (image->comps[1].dx != image->comps[2].dx ||
          image->comps[1].dy != image->comps[2].dy) {
        GST_ERROR_OBJECT (self,
            "Different sub-sampling of chroma planes not supported");
        return GST_FLOW_ERROR;
      }

      if (image->numcomps == 4) {
        if (image->comps[3].dx != 1 || image->comps[3].dy != 1) {
          GST_ERROR_OBJECT (self, "Sub-sampling of alpha plane not supported");
          return GST_FLOW_NOT_NEGOTIATED;
        }

        if (get_highest_prec (image) == 8) {
          self->fill_frame = fill_frame_planar8_4_generic;
          format = GST_VIDEO_FORMAT_AYUV;
        } else if (image->comps[3].prec <= 16) {
          self->fill_frame = fill_frame_planar16_4_generic;
          format = GST_VIDEO_FORMAT_AYUV64;
        } else {
          GST_ERROR_OBJECT (self, "Unsupported depth %d", image->comps[0].prec);
          return GST_FLOW_NOT_NEGOTIATED;
        }
      } else if (image->numcomps == 3) {
        if (get_highest_prec (image) == 8) {
          if (image->comps[1].dx == 1 && image->comps[1].dy == 1) {
            self->fill_frame = fill_frame_planar8_3;
            format = GST_VIDEO_FORMAT_Y444;
          } else if (image->comps[1].dx == 2 && image->comps[1].dy == 1) {
            self->fill_frame = fill_frame_planar8_3;
            format = GST_VIDEO_FORMAT_Y42B;
          } else if (image->comps[1].dx == 2 && image->comps[1].dy == 2) {
            self->fill_frame = fill_frame_planar8_3;
            format = GST_VIDEO_FORMAT_I420;
          } else if (image->comps[1].dx == 4 && image->comps[1].dy == 1) {
            self->fill_frame = fill_frame_planar8_3;
            format = GST_VIDEO_FORMAT_Y41B;
          } else if (image->comps[1].dx == 4 && image->comps[1].dy == 4) {
            self->fill_frame = fill_frame_planar8_3;
            format = GST_VIDEO_FORMAT_YUV9;
          } else {
            self->fill_frame = fill_frame_planar8_3_generic;
            format = GST_VIDEO_FORMAT_AYUV;
          }
        } else if (get_highest_prec (image) <= 16) {
          if (image->comps[1].dx == 1 && image->comps[1].dy == 1) {
            self->fill_frame = fill_frame_planar16_3;
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
            format = GST_VIDEO_FORMAT_Y444_10LE;
#else
            format = GST_VIDEO_FORMAT_Y444_10BE;
#endif
          } else if (image->comps[1].dx == 2 && image->comps[1].dy == 1) {
            self->fill_frame = fill_frame_planar16_3;
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
            format = GST_VIDEO_FORMAT_I422_10LE;
#else
            format = GST_VIDEO_FORMAT_I422_10BE;
#endif
          } else if (image->comps[1].dx == 2 && image->comps[1].dy == 2) {
            self->fill_frame = fill_frame_planar16_3;
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
            format = GST_VIDEO_FORMAT_I420_10LE;
#else
            format = GST_VIDEO_FORMAT_I420_10BE;
#endif
          } else {
            self->fill_frame = fill_frame_planar16_3_generic;
            format = GST_VIDEO_FORMAT_AYUV64;
          }
        } else {
          GST_ERROR_OBJECT (self, "Unsupported depth %d",
              get_highest_prec (image));
          return GST_FLOW_NOT_NEGOTIATED;
        }
      } else {
        GST_ERROR_OBJECT (self, "Unsupported number of YUV components: %d",
            image->numcomps);
        return GST_FLOW_NOT_NEGOTIATED;
      }
      break;
    default:
      GST_ERROR_OBJECT (self, "Unsupported colorspace %d", image->color_space);
      return GST_FLOW_NOT_NEGOTIATED;
  }

  width = image->x1 - image->x0;
  height = image->y1 - image->y0;

  if (!self->output_state ||
      self->output_state->info.finfo->format != format ||
      self->output_state->info.width != width ||
      self->output_state->info.height != height) {
    if (self->output_state)
      gst_video_codec_state_unref (self->output_state);
    self->output_state =
        gst_video_decoder_set_output_state (GST_VIDEO_DECODER (self), format,
        width, height, self->input_state);

    if (!gst_video_decoder_negotiate (GST_VIDEO_DECODER (self)))
      return GST_FLOW_NOT_NEGOTIATED;
  }

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_openjpeg_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame)
{
  GstOpenJPEGDec *self = GST_OPENJPEG_DEC (decoder);
  GstFlowReturn ret = GST_FLOW_OK;
  gint64 deadline;
  GstMapInfo map;
  opj_dinfo_t *dec;
  opj_cio_t *io;
  opj_image_t *image;
  GstVideoFrame vframe;
  opj_dparameters_t params;

  GST_DEBUG_OBJECT (self, "Handling frame");

  deadline = gst_video_decoder_get_max_decode_time (decoder, frame);
  if (deadline < 0) {
    GST_LOG_OBJECT (self, "Dropping too late frame: deadline %" G_GINT64_FORMAT,
        deadline);
    ret = gst_video_decoder_drop_frame (decoder, frame);
    return ret;
  }

  dec = opj_create_decompress (self->codec_format);
  if (!dec)
    goto initialization_error;

  opj_set_event_mgr ((opj_common_ptr) dec, NULL, NULL);

  params = self->params;
  if (self->ncomps)
    params.jpwl_exp_comps = self->ncomps;
  opj_setup_decoder (dec, &params);

  if (!gst_buffer_map (frame->input_buffer, &map, GST_MAP_READ))
    goto map_read_error;

  io = opj_cio_open ((opj_common_ptr) dec, map.data + (self->is_jp2c ? 8 : 0),
      map.size - (self->is_jp2c ? 8 : 0));
  if (!io)
    goto open_error;

  image = opj_decode (dec, io);
  if (!image)
    goto decode_error;

  gst_buffer_unmap (frame->input_buffer, &map);

  ret = gst_openjpeg_dec_negotiate (self, image);
  if (ret != GST_FLOW_OK)
    goto negotiate_error;

  ret = gst_video_decoder_allocate_output_frame (decoder, frame);
  if (ret != GST_FLOW_OK)
    goto allocate_error;

  if (!gst_video_frame_map (&vframe, &self->output_state->info,
          frame->output_buffer, GST_MAP_WRITE))
    goto map_write_error;

  self->fill_frame (&vframe, image);

  gst_video_frame_unmap (&vframe);

  opj_image_destroy (image);
  opj_cio_close (io);
  opj_destroy_decompress (dec);

  ret = gst_video_decoder_finish_frame (decoder, frame);

  return ret;

initialization_error:
  {
    gst_video_codec_frame_unref (frame);
    GST_ELEMENT_ERROR (self, LIBRARY, INIT,
        ("Failed to initialize OpenJPEG decoder"), (NULL));
    return GST_FLOW_ERROR;
  }
map_read_error:
  {
    opj_destroy_decompress (dec);
    gst_video_codec_frame_unref (frame);

    GST_ELEMENT_ERROR (self, CORE, FAILED,
        ("Failed to map input buffer"), (NULL));
    return GST_FLOW_ERROR;
  }
open_error:
  {
    opj_destroy_decompress (dec);
    gst_buffer_unmap (frame->input_buffer, &map);
    gst_video_codec_frame_unref (frame);

    GST_ELEMENT_ERROR (self, LIBRARY, INIT,
        ("Failed to open OpenJPEG stream"), (NULL));
    return GST_FLOW_ERROR;
  }
decode_error:
  {
    opj_cio_close (io);
    opj_destroy_decompress (dec);
    gst_buffer_unmap (frame->input_buffer, &map);
    gst_video_codec_frame_unref (frame);

    GST_VIDEO_DECODER_ERROR (self, 1, STREAM, DECODE,
        ("Failed to decode OpenJPEG stream"), (NULL), ret);
    return ret;
  }
negotiate_error:
  {
    opj_image_destroy (image);
    opj_cio_close (io);
    opj_destroy_decompress (dec);
    gst_video_codec_frame_unref (frame);

    GST_ELEMENT_ERROR (self, CORE, NEGOTIATION,
        ("Failed to negotiate"), (NULL));
    return ret;
  }
allocate_error:
  {
    opj_image_destroy (image);
    opj_cio_close (io);
    opj_destroy_decompress (dec);
    gst_video_codec_frame_unref (frame);

    GST_ELEMENT_ERROR (self, CORE, FAILED,
        ("Failed to allocate output buffer"), (NULL));
    return ret;
  }
map_write_error:
  {
    opj_image_destroy (image);
    opj_cio_close (io);
    opj_destroy_decompress (dec);
    gst_video_codec_frame_unref (frame);

    GST_ELEMENT_ERROR (self, CORE, FAILED,
        ("Failed to map output buffer"), (NULL));
    return GST_FLOW_ERROR;
  }
}

static gboolean
gst_openjpeg_dec_decide_allocation (GstVideoDecoder * decoder, GstQuery * query)
{
  GstBufferPool *pool;
  GstStructure *config;

  if (!GST_VIDEO_DECODER_CLASS (parent_class)->decide_allocation (decoder,
          query))
    return FALSE;

  g_assert (gst_query_get_n_allocation_pools (query) > 0);
  gst_query_parse_nth_allocation_pool (query, 0, &pool, NULL, NULL, NULL);
  g_assert (pool != NULL);

  config = gst_buffer_pool_get_config (pool);
  if (gst_query_find_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL)) {
    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);
  }
  gst_buffer_pool_set_config (pool, config);
  gst_object_unref (pool);

  return TRUE;
}
