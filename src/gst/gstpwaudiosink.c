/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
 * Copyright © 2019 Collabora Ltd.
 *   @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstpwaudiosink.h"

#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

GST_DEBUG_CATEGORY_STATIC (pw_audio_sink_debug);
#define GST_CAT_DEFAULT pw_audio_sink_debug

G_DEFINE_TYPE (GstPwAudioSink, gst_pw_audio_sink, GST_TYPE_AUDIO_BASE_SINK);

enum
{
  PROP_0,
  PROP_PATH,
  PROP_CLIENT_NAME,
  PROP_STREAM_PROPERTIES,
  PROP_FD
};

static GstStaticPadTemplate gst_pw_audio_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_AUDIO_CAPS_MAKE (GST_AUDIO_NE (F32))
                     ", layout = (string)\"interleaved\"")
);


static void
gst_pw_audio_sink_init (GstPwAudioSink * self)
{
  self->fd = -1;
}

static void
gst_pw_audio_sink_finalize (GObject * object)
{
  GstPwAudioSink *pwsink = GST_PW_AUDIO_SINK (object);

  g_free (pwsink->path);
  g_free (pwsink->client_name);
  if (pwsink->properties)
    gst_structure_free (pwsink->properties);
}

static void
gst_pw_audio_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstPwAudioSink *pwsink = GST_PW_AUDIO_SINK (object);

  switch (prop_id) {
    case PROP_PATH:
      g_free (pwsink->path);
      pwsink->path = g_value_dup_string (value);
      break;

    case PROP_CLIENT_NAME:
      g_free (pwsink->client_name);
      pwsink->client_name = g_value_dup_string (value);
      break;

    case PROP_STREAM_PROPERTIES:
      if (pwsink->properties)
        gst_structure_free (pwsink->properties);
      pwsink->properties =
          gst_structure_copy (gst_value_get_structure (value));
      break;

    case PROP_FD:
      pwsink->fd = g_value_get_int (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pw_audio_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstPwAudioSink *pwsink = GST_PW_AUDIO_SINK (object);

  switch (prop_id) {
    case PROP_PATH:
      g_value_set_string (value, pwsink->path);
      break;

    case PROP_CLIENT_NAME:
      g_value_set_string (value, pwsink->client_name);
      break;

    case PROP_STREAM_PROPERTIES:
      gst_value_set_structure (value, pwsink->properties);
      break;

    case PROP_FD:
      g_value_set_int (value, pwsink->fd);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstAudioRingBuffer *
gst_pw_audio_sink_create_ringbuffer (GstAudioBaseSink * sink)
{
  GstAudioRingBuffer *buffer;

  GST_DEBUG_OBJECT (sink, "creating ringbuffer");
  buffer = g_object_new (GST_TYPE_PW_AUDIO_SINK_RING_BUFFER,
      "sink", sink,
      NULL);
  GST_DEBUG_OBJECT (sink, "created ringbuffer @%p", buffer);

  return buffer;
}

static void
gst_pw_audio_sink_class_init (GstPwAudioSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstAudioBaseSinkClass *gstaudiobsink_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstaudiobsink_class = (GstAudioBaseSinkClass *) klass;

  gobject_class->finalize = gst_pw_audio_sink_finalize;
  gobject_class->set_property = gst_pw_audio_sink_set_property;
  gobject_class->get_property = gst_pw_audio_sink_get_property;

  gstaudiobsink_class->create_ringbuffer = gst_pw_audio_sink_create_ringbuffer;

  g_object_class_install_property (gobject_class, PROP_PATH,
      g_param_spec_string ("path", "Path",
          "The sink path to connect to (NULL = default)", NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_CLIENT_NAME,
      g_param_spec_string ("client-name", "Client Name",
          "The client name to use (NULL = default)", NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

   g_object_class_install_property (gobject_class, PROP_STREAM_PROPERTIES,
      g_param_spec_boxed ("stream-properties", "Stream properties",
          "List of PipeWire stream properties",
          GST_TYPE_STRUCTURE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

   g_object_class_install_property (gobject_class, PROP_FD,
      g_param_spec_int ("fd", "Fd", "The fd to connect with",
          -1, G_MAXINT, -1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_static_metadata (gstelement_class,
      "PipeWire Audio sink", "Sink/Audio",
      "Send audio to PipeWire",
      "George Kiagiadakis <george.kiagiadakis@collabora.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_pw_audio_sink_template));

  GST_DEBUG_CATEGORY_INIT (pw_audio_sink_debug, "pipewireaudiosink", 0,
      "PipeWire Audio Sink");
}


#define gst_pw_audio_sink_ring_buffer_parent_class parent_class
G_DEFINE_TYPE (GstPwAudioSinkRingBuffer, gst_pw_audio_sink_ring_buffer, GST_TYPE_AUDIO_RING_BUFFER);

enum
{
  RBUF_PROP_0,
  RBUF_PROP_SINK,
};

static void
gst_pw_audio_sink_ring_buffer_init (GstPwAudioSinkRingBuffer * self)
{
  self->loop = pw_loop_new (NULL);
  self->main_loop = pw_thread_loop_new (self->loop, "pw-audiosink-ringbuffer-loop");
  self->core = pw_core_new (self->loop, NULL, 0);
}

static void
gst_pw_audio_sink_ring_buffer_finalize (GObject * object)
{
  GstPwAudioSinkRingBuffer *self = GST_PW_AUDIO_SINK_RING_BUFFER (object);

  pw_core_destroy (self->core);
  pw_thread_loop_destroy (self->main_loop);
  pw_loop_destroy (self->loop);
}

static void
gst_pw_audio_sink_ring_buffer_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstPwAudioSinkRingBuffer *self = GST_PW_AUDIO_SINK_RING_BUFFER (object);

  switch (prop_id) {
    case RBUF_PROP_SINK:
      self->sink = g_value_get_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
on_remote_state_changed (void *data, enum pw_remote_state old,
    enum pw_remote_state state, const char *error)
{
  GstPwAudioSinkRingBuffer *self = GST_PW_AUDIO_SINK_RING_BUFFER (data);

  GST_DEBUG_OBJECT (self->sink, "got remote state %d", state);

  switch (state) {
    case PW_REMOTE_STATE_UNCONNECTED:
    case PW_REMOTE_STATE_CONNECTING:
    case PW_REMOTE_STATE_CONNECTED:
      break;
    case PW_REMOTE_STATE_ERROR:
      GST_ELEMENT_ERROR (self->sink, RESOURCE, FAILED,
          ("remote error: %s", error), (NULL));
      break;
  }
  pw_thread_loop_signal (self->main_loop, FALSE);
}

static const struct pw_remote_events remote_events = {
  PW_VERSION_REMOTE_EVENTS,
  .state_changed = on_remote_state_changed,
};

static gboolean
wait_for_remote_state (GstPwAudioSinkRingBuffer *self,
    enum pw_remote_state target)
{
  while (TRUE) {
    enum pw_remote_state state = pw_remote_get_state (self->remote, NULL);
    if (state == target)
      return TRUE;
    if (state == PW_REMOTE_STATE_ERROR)
      return FALSE;
    pw_thread_loop_wait (self->main_loop);
  }
}

static gboolean
gst_pw_audio_sink_ring_buffer_open_device (GstAudioRingBuffer *buf)
{
  GstPwAudioSinkRingBuffer *self = GST_PW_AUDIO_SINK_RING_BUFFER (buf);

  GST_DEBUG_OBJECT (self->sink, "open device");

  if (pw_thread_loop_start (self->main_loop) < 0)
    goto mainloop_error;

  pw_thread_loop_lock (self->main_loop);

  self->remote = pw_remote_new (self->core, NULL, 0);
  pw_remote_add_listener (self->remote, &self->remote_listener, &remote_events,
      self);

  if (self->sink->fd == -1)
    pw_remote_connect (self->remote);
  else
    pw_remote_connect_fd (self->remote, self->sink->fd);

  GST_DEBUG_OBJECT (self->sink, "waiting for connection");

  if (!wait_for_remote_state (self, PW_REMOTE_STATE_CONNECTED))
    goto connect_error;

  pw_thread_loop_unlock (self->main_loop);

  return TRUE;

  /* ERRORS */
mainloop_error:
  {
    GST_ELEMENT_ERROR (self->sink, RESOURCE, FAILED,
        ("Failed to start mainloop"), (NULL));
    return FALSE;
  }
connect_error:
  {
    pw_thread_loop_unlock (self->main_loop);
    return FALSE;
  }
}

static gboolean
gst_pw_audio_sink_ring_buffer_close_device (GstAudioRingBuffer *buf)
{
  GstPwAudioSinkRingBuffer *self = GST_PW_AUDIO_SINK_RING_BUFFER (buf);

  GST_DEBUG_OBJECT (self->sink, "closing device");

  pw_thread_loop_lock (self->main_loop);
  if (self->remote) {
    pw_remote_disconnect (self->remote);
    wait_for_remote_state (self, PW_REMOTE_STATE_UNCONNECTED);
  }
  pw_thread_loop_unlock (self->main_loop);

  pw_thread_loop_stop (self->main_loop);

  if (self->remote) {
    pw_remote_destroy (self->remote);
    self->remote = NULL;
  }
  return TRUE;
}

static void
on_stream_state_changed (void *data, enum pw_stream_state old,
    enum pw_stream_state state, const char *error)
{
  GstPwAudioSinkRingBuffer *self = GST_PW_AUDIO_SINK_RING_BUFFER (data);

  GST_DEBUG_OBJECT (self->sink, "got stream state: %s",
      pw_stream_state_as_string (state));

  switch (state) {
    case PW_STREAM_STATE_UNCONNECTED:
      GST_ELEMENT_ERROR (self->sink, RESOURCE, FAILED,
          ("stream disconnected unexpectedly"), (NULL));
      break;
    case PW_STREAM_STATE_CONNECTING:
    case PW_STREAM_STATE_CONFIGURE:
    case PW_STREAM_STATE_READY:
    case PW_STREAM_STATE_PAUSED:
    case PW_STREAM_STATE_STREAMING:
      break;
    case PW_STREAM_STATE_ERROR:
      GST_ELEMENT_ERROR (self->sink, RESOURCE, FAILED,
          ("stream error: %s", error), (NULL));
      break;
  }
  pw_thread_loop_signal (self->main_loop, FALSE);
}

static gboolean
wait_for_stream_state (GstPwAudioSinkRingBuffer *self,
    enum pw_stream_state target)
{
  while (TRUE) {
    enum pw_stream_state state = pw_stream_get_state (self->stream, NULL);
    if (state >= target)
      return TRUE;
    if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED)
      return FALSE;
    pw_thread_loop_wait (self->main_loop);
  }
}

static void
on_stream_format_changed (void *data, const struct spa_pod *format)
{
  GstPwAudioSinkRingBuffer *self = GST_PW_AUDIO_SINK_RING_BUFFER (data);
  const struct spa_pod *params[1];
  struct spa_pod_builder b = { NULL };
  uint8_t buffer[512];

  spa_pod_builder_init (&b, buffer, sizeof (buffer));
  params[0] = spa_pod_builder_add_object (&b,
      SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
      SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(2, 1, INT32_MAX),
      SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
      SPA_PARAM_BUFFERS_size,    SPA_POD_Int(self->segsize),
      SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(self->bpf),
      SPA_PARAM_BUFFERS_align,   SPA_POD_Int(16));

  GST_DEBUG_OBJECT (self->sink, "doing finish format, buffer size:%d", self->segsize);
  pw_stream_finish_format (self->stream, 0, params, 1);
}

static void
on_stream_process (void *data)
{
  GstPwAudioSinkRingBuffer *self = GST_PW_AUDIO_SINK_RING_BUFFER (data);
  GstAudioRingBuffer *buf = GST_AUDIO_RING_BUFFER (data);
  struct pw_buffer *b;
  gint segment;
  guint8 *readptr;
  gint len;

  if (g_atomic_int_get (&buf->state) != GST_AUDIO_RING_BUFFER_STATE_STARTED) {
    GST_LOG_OBJECT (self->sink, "ring buffer is not started");
    return;
  }

  if (gst_audio_ring_buffer_prepare_read (buf, &segment, &readptr, &len)) {
    b = pw_stream_dequeue_buffer (self->stream);
    if (!b) {
      GST_WARNING_OBJECT (self->sink, "no pipewire buffer available");
      return;
    }

    memcpy (b->buffer->datas[0].data, readptr, len);
    b->buffer->datas[0].chunk->offset = 0;
    b->buffer->datas[0].chunk->size = len;
    b->size = len / self->bpf;

    gst_audio_ring_buffer_clear (buf, segment);
    gst_audio_ring_buffer_advance (buf, 1);

    GST_TRACE_OBJECT (self->sink, "writing segment %d", segment);

    pw_stream_queue_buffer (self->stream, b);
  }
}

static const struct pw_stream_events stream_events = {
  PW_VERSION_STREAM_EVENTS,
  .state_changed = on_stream_state_changed,
  .format_changed = on_stream_format_changed,
  .process = on_stream_process,
};

static gboolean
copy_properties (GQuark field_id, const GValue *value, gpointer user_data)
{
  struct pw_properties *properties = user_data;

  if (G_VALUE_HOLDS_STRING (value))
    pw_properties_set (properties,
                       g_quark_to_string (field_id),
                       g_value_get_string (value));
  return TRUE;
}

static gboolean
gst_pw_audio_sink_ring_buffer_acquire (GstAudioRingBuffer *buf,
    GstAudioRingBufferSpec *spec)
{
  GstPwAudioSinkRingBuffer *self = GST_PW_AUDIO_SINK_RING_BUFFER (buf);
  struct pw_properties *props;
  struct spa_pod_builder b = { NULL };
  uint8_t buffer[512];
  const struct spa_pod *params[1];

  g_return_val_if_fail (spec, FALSE);
  g_return_val_if_fail (GST_AUDIO_INFO_IS_VALID (&spec->info), FALSE);
  g_return_val_if_fail (!self->stream, TRUE); /* already acquired */

  g_return_val_if_fail (spec->type == GST_AUDIO_RING_BUFFER_FORMAT_TYPE_RAW, FALSE);
  g_return_val_if_fail (GST_AUDIO_INFO_IS_FLOAT (&spec->info), FALSE);

  GST_DEBUG_OBJECT (self->sink, "acquire");

  /* construct param & props objects */

  if (self->sink->properties) {
    props = pw_properties_new (NULL, NULL);
    gst_structure_foreach (self->sink->properties, copy_properties, props);
  } else {
    props = NULL;
  }

  spa_pod_builder_init (&b, buffer, sizeof (buffer));
  params[0] = spa_pod_builder_add_object (&b,
      SPA_TYPE_OBJECT_Format,    SPA_PARAM_EnumFormat,
      SPA_FORMAT_mediaType,      SPA_POD_Id (SPA_MEDIA_TYPE_audio),
      SPA_FORMAT_mediaSubtype,   SPA_POD_Id (SPA_MEDIA_SUBTYPE_raw),
      SPA_FORMAT_AUDIO_format,   SPA_POD_Id (SPA_AUDIO_FORMAT_F32),
      SPA_FORMAT_AUDIO_rate,     SPA_POD_Int (GST_AUDIO_INFO_RATE (&spec->info)),
      SPA_FORMAT_AUDIO_channels, SPA_POD_Int (GST_AUDIO_INFO_CHANNELS (&spec->info)));

  self->segsize = spec->segsize;
  self->bpf = GST_AUDIO_INFO_BPF (&spec->info);

  /* connect stream */

  pw_thread_loop_lock (self->main_loop);

  GST_DEBUG_OBJECT (self->sink, "creating stream");

  self->stream = pw_stream_new (self->remote, self->sink->client_name, props);
  pw_stream_add_listener(self->stream, &self->stream_listener, &stream_events,
      self);

  if (pw_stream_connect (self->stream,
          PW_DIRECTION_OUTPUT,
          self->sink->path ? (uint32_t)atoi(self->sink->path) : SPA_ID_INVALID,
          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
          params, 1) < 0)
    goto start_error;

  GST_DEBUG_OBJECT (self->sink, "waiting for stream READY");

  if (!wait_for_stream_state (self, PW_STREAM_STATE_READY))
    goto start_error;

  pw_thread_loop_unlock (self->main_loop);

  /* allocate the internal ringbuffer */

  spec->seglatency = spec->segtotal + 1;
  buf->size = spec->segtotal * spec->segsize;
  buf->memory = g_malloc (buf->size);

  gst_audio_format_fill_silence (buf->spec.info.finfo, buf->memory,
      buf->size);

  GST_DEBUG_OBJECT (self->sink, "acquire done");

  return TRUE;

start_error:
  {
    GST_ERROR_OBJECT (self->sink, "could not start stream");
    pw_stream_destroy (self->stream);
    self->stream = NULL;
    pw_thread_loop_unlock (self->main_loop);
    return FALSE;
  }
}

static gboolean
gst_pw_audio_sink_ring_buffer_release (GstAudioRingBuffer *buf)
{
  GstPwAudioSinkRingBuffer *self = GST_PW_AUDIO_SINK_RING_BUFFER (buf);

  GST_DEBUG_OBJECT (self->sink, "release");

  pw_thread_loop_lock (self->main_loop);
  if (self->stream) {
    spa_hook_remove (&self->stream_listener);
    pw_stream_disconnect (self->stream);
    pw_stream_destroy (self->stream);
    self->stream = NULL;
  }
  pw_thread_loop_unlock (self->main_loop);

  /* free the buffer */
  g_free (buf->memory);
  buf->memory = NULL;

  return TRUE;
}

static guint
gst_pw_audio_sink_ring_buffer_delay (GstAudioRingBuffer *buf)
{
  GstPwAudioSinkRingBuffer *self = GST_PW_AUDIO_SINK_RING_BUFFER (buf);
  struct pw_time t;

  if (self->stream) {
    if (pw_stream_get_time (self->stream, &t) == 0)
      return t.queued;
  }

  return 0;
}

static void
gst_pw_audio_sink_ring_buffer_class_init (GstPwAudioSinkRingBufferClass * klass)
{
  GObjectClass *gobject_class;
  GstAudioRingBufferClass *gstaudiorbuf_class;

  gobject_class = (GObjectClass *) klass;
  gstaudiorbuf_class = (GstAudioRingBufferClass *) klass;

  gobject_class->finalize = gst_pw_audio_sink_ring_buffer_finalize;
  gobject_class->set_property = gst_pw_audio_sink_ring_buffer_set_property;

  gstaudiorbuf_class->open_device = gst_pw_audio_sink_ring_buffer_open_device;
  gstaudiorbuf_class->acquire = gst_pw_audio_sink_ring_buffer_acquire;
  gstaudiorbuf_class->release = gst_pw_audio_sink_ring_buffer_release;
  gstaudiorbuf_class->close_device = gst_pw_audio_sink_ring_buffer_close_device;
  gstaudiorbuf_class->delay = gst_pw_audio_sink_ring_buffer_delay;

  g_object_class_install_property (gobject_class, RBUF_PROP_SINK,
      g_param_spec_object ("sink", "Sink", "The audio sink",
          GST_TYPE_PW_AUDIO_SINK,
          G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}
