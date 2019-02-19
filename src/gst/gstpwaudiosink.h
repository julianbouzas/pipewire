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

#ifndef __GST_PW_AUDIO_SINK_H__
#define __GST_PW_AUDIO_SINK_H__

#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <pipewire/pipewire.h>

G_BEGIN_DECLS

#define GST_TYPE_PW_AUDIO_SINK \
    (gst_pw_audio_sink_get_type ())
#define GST_TYPE_PW_AUDIO_SINK_RING_BUFFER \
    (gst_pw_audio_sink_ring_buffer_get_type ())

G_DECLARE_FINAL_TYPE(GstPwAudioSink, gst_pw_audio_sink,
                     GST, PW_AUDIO_SINK, GstAudioBaseSink);
G_DECLARE_FINAL_TYPE(GstPwAudioSinkRingBuffer, gst_pw_audio_sink_ring_buffer,
                     GST, PW_AUDIO_SINK_RING_BUFFER, GstAudioRingBuffer);

struct _GstPwAudioSink
{
  GstAudioBaseSink parent;

  gchar *path;
  gchar *client_name;
  GstStructure *properties;
  int fd;
};

struct _GstPwAudioSinkRingBuffer
{
  GstAudioRingBuffer parent;
  GstPwAudioSink *sink;

  struct pw_loop *loop;
  struct pw_thread_loop *main_loop;

  struct pw_core *core;
  struct pw_remote *remote;
  struct spa_hook remote_listener;

  struct pw_stream *stream;
  struct spa_hook stream_listener;

  gint segsize;
  gint bpf;
};

G_END_DECLS

#endif