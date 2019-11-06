/*
 * (C) Copyright 2014 Kurento (http://kurento.org/)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "kmsenctreebin.h"
#include "kmsutils.h"

#define GST_DEFAULT_NAME "enctreebin"
#define GST_CAT_DEFAULT kms_enc_tree_bin_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define LEAKY_TIME 600000000    /*600 ms */
#define kms_enc_tree_bin_parent_class parent_class
G_DEFINE_TYPE (KmsEncTreeBin, kms_enc_tree_bin, KMS_TYPE_TREE_BIN);

#define KMS_ENC_TREE_BIN_GET_PRIVATE(obj) ( \
  G_TYPE_INSTANCE_GET_PRIVATE (             \
    (obj),                                  \
    KMS_TYPE_ENC_TREE_BIN,                  \
    KmsEncTreeBinPrivate                    \
  )                                         \
)

#define KMS_ENC_TREE_BIN_LIMIT(obj, value) \
  MAX((obj)->priv->min_bitrate,MIN((obj)->priv->max_bitrate, (value)))

typedef enum
{
  VAAPIVP8,
  VAAPIH264,
  VP8,
  X264,
  OPENH264,
  OPUS,
  UNSUPPORTED
} EncoderType;

struct _KmsEncTreeBinPrivate
{
  GstElement *enc, *mediator;
  GstElement *capsfilter;
  EncoderType enc_type;
  RembEventManager *remb_manager;

  gchar *width, *height;

  gint remb_bitrate;
  gint tag_bitrate;

  gint current_bitrate;

  gint max_bitrate;
  gint min_bitrate;
};

static const gchar *
kms_enc_tree_bin_get_name_from_type (EncoderType enc_type)
{
  switch (enc_type) {
    case VP8:
      return "vp8";
    case X264:
      return "x264";
    case OPENH264:
      return "openh264";
    case OPUS:
      return "opus";
    case UNSUPPORTED:
    default:
      return NULL;
  }
}

static void
set_encoder_configuration (GstElement * encoder, GstStructure * codec_config,
    const gchar * config_name)
{
  if (!codec_config || !config_name || !encoder) {
    return;
  }

  if (gst_structure_has_field_typed (codec_config, config_name,
          GST_TYPE_STRUCTURE)) {
    GstStructure *config;
    guint n_props = 0, i;
    GParamSpec **props;

    gst_structure_get (codec_config, config_name, GST_TYPE_STRUCTURE, &config,
        NULL);

    props =
        g_object_class_list_properties (G_OBJECT_GET_CLASS (encoder), &n_props);
    for (i = 0; i < n_props; i++) {
      const gchar *name = g_param_spec_get_name (props[i]);

      if (gst_structure_has_field (config, name)) {
        GValue final_value = G_VALUE_INIT;
        gchar *st_value;
        const GValue *val;

        val = gst_structure_get_value (config, name);
        st_value = gst_value_serialize (val);
        g_value_init (&final_value, props[i]->value_type);

        GST_DEBUG_OBJECT (encoder,
            "Trying to configure property: %s with value %s", name, st_value);

        if (gst_value_deserialize (&final_value, st_value)) {
          g_object_set_property (G_OBJECT (encoder), name, &final_value);
        } else {
          GST_WARNING_OBJECT (encoder, "Property %s cannot be configured to %s",
              name, st_value);
        }

        g_free (st_value);
        g_value_reset (&final_value);
      }
    }
    g_free (props);

    gst_structure_free (config);
  }
}

static void
configure_encoder (GstElement * encoder, EncoderType type, gint target_bitrate,
    GstStructure * codec_configs)
{
  GST_DEBUG ("Configure encoder: %" GST_PTR_FORMAT, encoder);
  switch (type) {
    case VP8:
    {
      /* *INDENT-OFF* */
      g_object_set (G_OBJECT (encoder),
                    "deadline", G_GINT64_CONSTANT (200000),
                    "threads", 1,
                    "cpu-used", 16,
                    "resize-allowed", TRUE,
                    "target-bitrate", target_bitrate,
                    "end-usage", /* cbr */ 1,
                    NULL);
      /* *INDENT-ON* */
      break;
    }
    case VAAPIH264:
    case VAAPIVP8:
    {
      /* *INDENT-OFF* */
      g_object_set(G_OBJECT(encoder),
                   "bitrate", target_bitrate / 1000,
                   "rate-control", 2,
                   NULL);
      /* *INDENT-ON* */
      break;
    }
    case X264:
    {
      /* *INDENT-OFF* */
      g_object_set (G_OBJECT (encoder),
                    "speed-preset", /* veryfast */ 3,
                    "threads", (guint) 1,
                    "bitrate", target_bitrate / 1000,
                    "key-int-max", 60,
                    "tune", /* zero-latency */ 4,
                    NULL);
      /* *INDENT-ON* */
      break;
    }
    case OPENH264:
    {
      /* *INDENT-OFF* */
      g_object_set (G_OBJECT (encoder),
                    "rate-control", /* bitrate */ 1,
                    "bitrate", target_bitrate,
                    NULL);
      /* *INDENT-ON* */
      break;
    }
    case OPUS:
    {
      g_object_set (G_OBJECT (encoder), "inband-fec", TRUE,
          "perfect-timestamp", TRUE, NULL);
      break;
    }
    default:
      GST_DEBUG ("Codec %" GST_PTR_FORMAT
          " not configured because it is not supported", encoder);
      break;
  }
  set_encoder_configuration (encoder, codec_configs,
      kms_enc_tree_bin_get_name_from_type (type));
}

static void
kms_enc_tree_bin_set_encoder_type (KmsEncTreeBin * self)
{
  gchar *name;

  g_object_get (self->priv->enc, "name", &name, NULL);

  if (g_str_has_prefix (name, "vaapiencodevp8")) {
    self->priv->enc_type = VAAPIVP8;
  } else if (g_str_has_prefix (name, "vaapiencodeh264")) {
    self->priv->enc_type = VAAPIH264;
  } else if (g_str_has_prefix (name, "vp8enc")) {
    self->priv->enc_type = VP8;
  } else if (g_str_has_prefix (name, "x264enc")) {
    self->priv->enc_type = X264;
  } else if (g_str_has_prefix (name, "openh264enc")) {
    self->priv->enc_type = OPENH264;
  } else if (g_str_has_prefix (name, "opusenc")) {
    self->priv->enc_type = OPUS;
  } else {
    self->priv->enc_type = UNSUPPORTED;
  }

  g_free (name);
}

static void
kms_enc_tree_bin_create_encoder_for_caps (KmsEncTreeBin * self,
    const GstCaps * caps, gint target_bitrate, GstStructure * codec_configs)
{
  GList *encoder_list, *filtered_list, *l;
  GstElementFactory *encoder_factory = NULL;

  encoder_list =
      gst_element_factory_list_get_elements (GST_ELEMENT_FACTORY_TYPE_ENCODER,
      GST_RANK_NONE);

  /* HACK: Augment the openh264 rank */
  for (l = encoder_list; l != NULL; l = l->next) {
    encoder_factory = GST_ELEMENT_FACTORY (l->data);

    if (g_str_has_prefix (GST_OBJECT_NAME (encoder_factory), "openh264")) {
      encoder_list = g_list_remove (encoder_list, l->data);
      encoder_list = g_list_prepend (encoder_list, encoder_factory);
      break;
    }
  }

  encoder_factory = NULL;
  filtered_list =
      gst_element_factory_list_filter (encoder_list, caps, GST_PAD_SRC, FALSE);

  for (l = filtered_list; l != NULL; l = l->next) {
    GST_INFO ("found encoder: %s", GST_OBJECT_NAME (l->data));
  }

  // force VAAPI for H264
  // TODO(mn): remove this hard-coded string comparison...
  if (g_str_has_prefix (GST_OBJECT_NAME (filtered_list->data), "openh264enc")) {
    GST_WARNING ("enforcing VAAPI for H264");
    filtered_list = filtered_list->next;
  }

  for (l = filtered_list; l != NULL && encoder_factory == NULL; l = l->next) {
    encoder_factory = GST_ELEMENT_FACTORY (l->data);
    if (gst_element_factory_get_num_pad_templates (encoder_factory) != 2)
      encoder_factory = NULL;
  }

  if (encoder_factory != NULL) {
    self->priv->enc = gst_element_factory_create (encoder_factory, NULL);
    kms_enc_tree_bin_set_encoder_type (self);
    configure_encoder (self->priv->enc, self->priv->enc_type, target_bitrate,
        codec_configs);
  }

  gst_plugin_feature_list_free (filtered_list);
  gst_plugin_feature_list_free (encoder_list);
}

static gint
kms_enc_tree_bin_get_bitrate (KmsEncTreeBin * self)
{
  gint bitrate;

  if (self->priv->remb_bitrate <= 0) {
    bitrate = KMS_ENC_TREE_BIN_LIMIT (self, self->priv->tag_bitrate);
  } else if (self->priv->tag_bitrate <= 0) {
    bitrate = KMS_ENC_TREE_BIN_LIMIT (self, self->priv->remb_bitrate);
  } else {
    bitrate = KMS_ENC_TREE_BIN_LIMIT (self, MIN (self->priv->remb_bitrate,
            self->priv->tag_bitrate));
  }

  if (bitrate <= 0) {
    bitrate = self->priv->current_bitrate;
  } else {
    self->priv->current_bitrate = bitrate;
  }

  return bitrate;
}

static void
kms_enc_tree_bin_set_target_bitrate (KmsEncTreeBin * self)
{
  gint target_bitrate = kms_enc_tree_bin_get_bitrate (self);

  if (target_bitrate <= 0) {
    return;
  }

  GST_WARNING ("new bitrate estimation: %d", target_bitrate);

  GST_DEBUG_OBJECT (self->priv->enc, "Set target encoding bitrate: %d bps",
      target_bitrate);

  switch (self->priv->enc_type) {
    case VAAPIVP8:
    case VAAPIH264:
    {
      // the default task to to here would be to divide by 1000 to receive the correct
      // target bitrate. this, however, results in way too crappy image quality.
      // the lower the value we divide by, the higher the image quality
      // 500 seems to be the best fit so far (250 results in a too high bitrate
      // and therefore too laggy streaming)
      guint last_br, new_br = target_bitrate / 500;

      g_object_get (self->priv->enc, "bitrate", &last_br, NULL);

      if (last_br != new_br) {
        g_object_set (self->priv->enc, "bitrate", new_br, NULL);
      }

      gchar *new_width, *new_height = NULL;

      // this affects the initial stream resolution. the min value
      // is 300000, the max value 500000. It can be tweaked from the
      // js client via setMinVideoSendBandwidth
      if (target_bitrate >= 4000000) {
        new_width = "1920";
        new_height = "1080";
      } else if (target_bitrate > 2750000) {
        new_width = "1600";
        new_height = "900";
      } else if (target_bitrate > 1500000) {
        new_width = "1280";
        new_height = "720";
      } else if (target_bitrate > 1250000) {
        new_width = "960";
        new_height = "540";
      } else if (target_bitrate > 1000000) {
        new_width = "800";
        new_height = "450";
      } else if (target_bitrate > 800000) {
        new_width = "640";
        new_height = "360";
      } else if (target_bitrate > 400000) {
        new_width = "480";
        new_height = "270";
      } else {
        new_width = "320";
        new_height = "180";
      }

      if (!g_str_equal (new_width, self->priv->width)) {
        GST_WARNING ("(new resolution):: %s", g_strconcat (new_width, "x",
                new_height, NULL));

        self->priv->width = new_width;
        self->priv->height = new_height;

        GstCaps *filter_caps =
            gst_caps_from_string (g_strconcat ("video/x-raw,width=", new_width,
                ",height=", new_height, NULL));

        // attention: this crashes for VAAPI VP8 encoding on most resolutions (1280x720, 1920x1080 and 1600x900, 640x360 and 960x540 seem to work though)
        g_object_set (self->priv->capsfilter, "caps", filter_caps, NULL);
        gst_caps_unref (filter_caps);
      }

      break;
    }
    case VP8:
    {
      gint last_br;

      g_object_get (self->priv->enc, "target-bitrate", &last_br, NULL);
      if (last_br / 1000 != target_bitrate / 1000) {
        g_object_set (self->priv->enc, "target-bitrate", target_bitrate, NULL);
      }
      break;
    }
    case X264:
    {
      guint last_br, new_br = target_bitrate / 1000;

      g_object_get (self->priv->enc, "bitrate", &last_br, NULL);
      if (last_br != new_br) {
        g_object_set (self->priv->enc, "bitrate", new_br, NULL);
      }
      break;
    }
    case OPENH264:
    {
      guint last_br, new_br = target_bitrate;

      g_object_get (self->priv->enc, "bitrate", &last_br, NULL);
      if (last_br / 1000 != new_br / 1000) {
        g_object_set (self->priv->enc, "bitrate", new_br, NULL);
      }
    }
    default:
      GST_ERROR ("Skip setting bitrate, encoder not supported");
      break;
  }
}

void
kms_enc_tree_bin_set_bitrate_limits (KmsEncTreeBin * self, gint min_bitrate,
    gint max_bitrate)
{
  // TODO: Think about adding a mutex here
  self->priv->max_bitrate = max_bitrate;
  self->priv->min_bitrate = min_bitrate;

  kms_enc_tree_bin_set_target_bitrate (self);
}

gint
kms_enc_tree_bin_get_min_bitrate (KmsEncTreeBin * self)
{
  return self->priv->min_bitrate;
}

gint
kms_enc_tree_bin_get_max_bitrate (KmsEncTreeBin * self)
{
  return self->priv->max_bitrate;
}

static void
bitrate_callback (RembEventManager * remb_manager, guint bitrate,
    gpointer user_data)
{
  KmsEncTreeBin *self = user_data;

  if (bitrate != 0) {
    self->priv->remb_bitrate = bitrate;
    kms_enc_tree_bin_set_target_bitrate (self);
  }
}

static GstPadProbeReturn
tag_event_probe (GstPad * pad, GstPadProbeInfo * info, gpointer data)
{
  GstEvent *event = gst_pad_probe_info_get_event (info);

  if (GST_EVENT_TYPE (event) == GST_EVENT_TAG) {
    KmsEncTreeBin *self = data;
    GstTagList *taglist;
    guint bitrate;

    gst_event_parse_tag (event, &taglist);
    if (gst_tag_list_get_uint (taglist, "bitrate", &bitrate)) {

      self->priv->tag_bitrate = bitrate;
      kms_enc_tree_bin_set_target_bitrate (self);
    }
  }

  return GST_PAD_PROBE_OK;
}

/*
 * FIXME: This is a hack to make x264 work.
 *
 * We have notice that x264 doesn't work if width or height is odd,
 * so we force a rescale increasing one pixel that dimension
 * when we detect this situation.
 */
static GstPadProbeReturn
check_caps_probe (GstPad * pad, GstPadProbeInfo * info, gpointer data)
{
  int width, height;
  GstCaps *filter_caps, *caps;
  GstElement *element;
  GstStructure *st;
  gboolean needs_filter = FALSE;
  GstEvent *event = gst_pad_probe_info_get_event (info);

  if (GST_EVENT_TYPE (event) != GST_EVENT_CAPS) {
    return GST_PAD_PROBE_OK;
  }

  gst_event_parse_caps (event, &caps);

  st = gst_caps_get_structure (caps, 0);

  gst_structure_get (st, "width", G_TYPE_INT, &width, NULL);
  gst_structure_get (st, "height", G_TYPE_INT, &height, NULL);

  if (width % 2) {
    GST_WARNING ("Width is odd");
    needs_filter = TRUE;
    width--;
  }

  if (height % 2) {
    GST_WARNING ("Height is odd");
    needs_filter = TRUE;
    height--;
  }

  if (!needs_filter)
    return GST_PAD_PROBE_OK;

  filter_caps = gst_caps_from_string ("video/x-raw,format=I420");

  gst_caps_set_simple (filter_caps, "width", G_TYPE_INT, width, NULL);
  gst_caps_set_simple (filter_caps, "height", G_TYPE_INT, height, NULL);

  element = gst_pad_get_parent_element (pad);
  g_object_set (element, "caps", filter_caps, NULL);
  gst_caps_unref (filter_caps);
  g_object_unref (element);

  return GST_PAD_PROBE_OK;
}

static gboolean
kms_enc_tree_bin_configure (KmsEncTreeBin * self, const GstCaps * caps,
    gint target_bitrate, GstStructure * codec_configs)
{
  KmsTreeBin *tree_bin = KMS_TREE_BIN (self);
  GstElement *rate, *convert, *output_tee = NULL;
  GstElement *queue;
  GstPad *enc_src;

  self->priv->current_bitrate = target_bitrate;

  kms_enc_tree_bin_create_encoder_for_caps (self, caps, target_bitrate,
      codec_configs);

  if (self->priv->enc == NULL) {
    GST_WARNING_OBJECT (self, "Invalid encoder for caps: %" GST_PTR_FORMAT,
        caps);
    return FALSE;
  }

  GST_DEBUG_OBJECT (self, "Encoder found: %" GST_PTR_FORMAT, self->priv->enc);

  enc_src = gst_element_get_static_pad (self->priv->enc, "src");
  self->priv->remb_manager = kms_utils_remb_event_manager_create (enc_src);
  kms_utils_remb_event_manager_set_callback (self->priv->remb_manager,
      bitrate_callback, self, NULL);
  gst_pad_add_probe (enc_src, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      tag_event_probe, self, NULL);
  g_object_unref (enc_src);

  rate = kms_utils_create_rate_for_caps (caps);
  convert = kms_utils_create_convert_for_caps (caps);
  if ((self->priv->enc_type == VAAPIVP8 || self->priv->enc_type ==
          VAAPIH264) && !kms_utils_caps_is_audio (caps)) {
    GST_WARNING ("using vaapipostproc instead of videoscale!");
    self->priv->mediator = gst_element_factory_make ("vaapipostproc", NULL);
  } else {
    self->priv->mediator = kms_utils_create_mediator_element (caps);
  }
  queue = kms_utils_element_factory_make ("queue", "enctreebin_");
  g_object_set (queue, "leaky", 2, "max-size-time", LEAKY_TIME, NULL);

  if (rate) {
    gst_bin_add (GST_BIN (self), rate);
  }
  gst_bin_add_many (GST_BIN (self), convert, self->priv->mediator, queue,
      self->priv->enc, NULL);
  gst_element_sync_state_with_parent (self->priv->enc);
  gst_element_sync_state_with_parent (queue);
  gst_element_sync_state_with_parent (self->priv->mediator);
  gst_element_sync_state_with_parent (convert);
  if (rate) {
    gst_element_sync_state_with_parent (rate);
  }
  // FIXME: This is a hack to avoid an error on x264enc that does not work
  // properly with some raw formats, this should be fixed in gstreamer
  // but until this is done this hack makes it work
  if (self->priv->enc_type == X264) {
    GstCaps *filter_caps = gst_caps_from_string ("video/x-raw,format=I420");
    GstPad *sink;

    self->priv->capsfilter =
        kms_utils_element_factory_make ("capsfilter", "enctreebin_");
    sink = gst_element_get_static_pad (self->priv->capsfilter, "sink");
    gst_pad_add_probe (sink, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
        check_caps_probe, NULL, NULL);
    g_object_unref (sink);

    g_object_set (self->priv->capsfilter, "caps", filter_caps, NULL);
    gst_caps_unref (filter_caps);

    gst_bin_add (GST_BIN (self), self->priv->capsfilter);
    gst_element_sync_state_with_parent (self->priv->capsfilter);
  }

  if (self->priv->enc_type == VAAPIVP8 || self->priv->enc_type == VAAPIH264) {
    // initially start with 320x180 - will be scaled up if the bandwidth is sufficient
    // for vaapivp8enc (does not hold true for vaapih264enc!), the width/height set here
    // are the minimum values to which the stream can be adjusted.

    // if we don't set anything here or if we set a value higher than the value to which we scale, the result is:
    // 0:00:02.467398926 10754 0x7f963c031630 WARN enctreebin kmsenctreebin.c:410:kms_enc_tree_bin_set_target_bitrate: (new resolution):: 320x180
    // Segmentation fault (thread 140282142701312, pid 10754)
    // Stack trace:
    // [gst_plugin_vaapi_register]
    // /usr/lib/x86_64-linux-gnu/gstreamer-1.0/libgstvaapi.so:0x5AE90
    // [gst_plugin_vaapi_register]
    // /usr/lib/x86_64-linux-gnu/gstreamer-1.0/libgstvaapi.so:0x2B7CD
    // [gst_plugin_vaapi_register]
    // /usr/lib/x86_64-linux-gnu/gstreamer-1.0/libgstvaapi.so:0x2BD49
    // [gst_plugin_vaapi_register]
    // /usr/lib/x86_64-linux-gnu/gstreamer-1.0/libgstvaapi.so:0x2BFCE
    // [gst_tag_setter_get_tag_merge_mode]
    // /usr/lib/x86_64-linux-gnu/libgstreamer-1.0.so.0:0xAB269
    // [g_thread_pool_new]
    // /usr/lib/x86_64-linux-gnu/libglib-2.0.so.0:0x74B60
    // [g_test_get_filename]
    // /usr/lib/x86_64-linux-gnu/libglib-2.0.so.0:0x74195
    // [start_thread]
    // /build/glibc-OTsEL5/glibc-2.27/nptl/pthread_create.c:463
    // [clone]
    // sysdeps/unix/sysv/linux/x86_64/clone.S:97
    //
    // Process finished with exit code 134 (interrupted by signal 6: SIGABRT)

    GstCaps *filter_caps =
        gst_caps_from_string ("video/x-raw,width=320,height=180");
    GstPad *sink;

    self->priv->width = "320";
    self->priv->height = "180";

    self->priv->capsfilter =
        kms_utils_element_factory_make ("capsfilter", "enctreebin_");
    sink = gst_element_get_static_pad (self->priv->capsfilter, "sink");
    gst_pad_add_probe (sink, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
        check_caps_probe, NULL, NULL);
    g_object_unref (sink);

    g_object_set (self->priv->capsfilter, "caps", filter_caps, NULL);
    gst_caps_unref (filter_caps);

    gst_bin_add (GST_BIN (self), self->priv->capsfilter);
    gst_element_sync_state_with_parent (self->priv->capsfilter);
  }

  if (rate) {
    kms_tree_bin_set_input_element (tree_bin, rate);
  } else {
    kms_tree_bin_set_input_element (tree_bin, convert);
  }
  output_tee = kms_tree_bin_get_output_tee (tree_bin);
  if (rate) {
    gst_element_link (rate, convert);
  }
  if (self->priv->enc_type == X264) {
    gst_element_link_many (convert, self->priv->mediator,
        self->priv->capsfilter, queue, self->priv->enc, output_tee, NULL);
  } else if (self->priv->enc_type == VAAPIVP8
      || self->priv->enc_type == VAAPIH264) {
    GstElement *streamfilter;
    GstCaps *streamcaps;

    streamfilter =
        kms_utils_element_factory_make ("capsfilter", "kmsenctreebin_");

    if (self->priv->enc_type == VAAPIH264) {
      streamcaps =
          gst_caps_from_string ("video/x-h264,stream-format=byte-stream");
    } else {
      streamcaps = gst_caps_from_string ("video/x-vp8");
    }

    g_object_set (streamfilter, "caps", streamcaps, NULL);
    gst_bin_add_many (GST_BIN (self), streamfilter, NULL);
    gst_caps_unref (streamcaps);
    gst_element_link_many (convert, self->priv->mediator,
        self->priv->capsfilter, queue, self->priv->enc, streamfilter,
        output_tee, NULL);
  } else {
    gst_element_link_many (convert, self->priv->mediator, queue,
        self->priv->enc, output_tee, NULL);
  }

  return TRUE;
}

KmsEncTreeBin *
kms_enc_tree_bin_new (const GstCaps * caps, gint target_bitrate,
    gint min_bitrate, gint max_bitrate, GstStructure * codec_configs)
{
  KmsEncTreeBin *enc;

  enc = g_object_new (KMS_TYPE_ENC_TREE_BIN, NULL);
  enc->priv->max_bitrate = max_bitrate;
  enc->priv->min_bitrate = min_bitrate;

  target_bitrate = KMS_ENC_TREE_BIN_LIMIT (enc, target_bitrate);
  if (!kms_enc_tree_bin_configure (enc, caps, target_bitrate, codec_configs)) {
    g_object_unref (enc);
    return NULL;
  }

  return enc;
}

static void
kms_enc_tree_bin_init (KmsEncTreeBin * self)
{
  self->priv = KMS_ENC_TREE_BIN_GET_PRIVATE (self);

  self->priv->remb_manager = NULL;

  self->priv->remb_bitrate = -1;
  self->priv->tag_bitrate = -1;

  self->priv->max_bitrate = G_MAXINT;
  self->priv->min_bitrate = 0;
}

static void
kms_enc_tree_bin_dispose (GObject * object)
{
  KmsEncTreeBin *self = KMS_ENC_TREE_BIN (object);

  GST_DEBUG_OBJECT (object, "dispose");

  if (self->priv->remb_manager) {
    kms_utils_remb_event_manager_destroy (self->priv->remb_manager);
    self->priv->remb_manager = NULL;
  }

  /* chain up */
  G_OBJECT_CLASS (kms_enc_tree_bin_parent_class)->dispose (object);
}

static void
kms_enc_tree_bin_class_init (KmsEncTreeBinClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_set_details_simple (gstelement_class,
      "EncTreeBin",
      "Generic",
      "Bin to encode and distribute encoding media.",
      "Miguel París Díaz <mparisdiaz@gmail.com>");

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, GST_DEFAULT_NAME, 0,
      GST_DEFAULT_NAME);

  gobject_class->dispose = kms_enc_tree_bin_dispose;

  g_type_class_add_private (klass, sizeof (KmsEncTreeBinPrivate));
}
