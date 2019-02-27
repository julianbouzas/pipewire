/* Spa A2DP Source
 *
 * Copyright © 2018 Wim Taymans
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

#include <unistd.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <spa/support/loop.h>
#include <spa/support/log.h>
#include <spa/utils/list.h>

#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/param/param.h>
#include <spa/param/audio/format.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/filter.h>

#include <sbc/sbc.h>

#include "defs.h"
#include "rtp.h"
#include "a2dp-codecs.h"

struct props {
	uint32_t min_latency;
	uint32_t max_latency;
};

#define FILL_FRAMES 2
#define MAX_BUFFERS 32

struct buffer {
	uint32_t id;
	struct spa_buffer *buf;
	struct spa_meta_header *h;
	bool outstanding;
	struct spa_list link;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;
	struct spa_loop *main_loop;
	struct spa_loop *data_loop;

	const struct spa_node_callbacks *callbacks;
	void *callbacks_data;

	struct props props;

	struct spa_bt_transport *transport;

	bool have_format;
	struct spa_audio_info current_format;
	int frame_size;

	struct spa_port_info info;
	struct spa_io_buffers *io;

	struct buffer buffers[MAX_BUFFERS];
	unsigned int n_buffers;

	struct spa_list free;
	struct spa_list ready;

	uint32_t sample_count;

	bool started;
	struct spa_source source;

	sbc_t sbc;
	uint8_t buffer[4096];
	struct timespec now;
};

#define NAME "a2dp-source"

#define CHECK_PORT(this,d,p)    ((d) == SPA_DIRECTION_OUTPUT && (p) == 0)

static const uint32_t default_min_latency = 128;
static const uint32_t default_max_latency = 1024;

static void reset_props(struct props *props)
{
	props->min_latency = default_min_latency;
	props->max_latency = default_max_latency;
}

static int impl_node_enum_params(struct spa_node *node,
				 uint32_t id, uint32_t *index,
				 const struct spa_pod *filter,
				 struct spa_pod **result,
				 struct spa_pod_builder *builder)
{
	struct impl *this;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_List:
	{
		uint32_t list[] = { SPA_PARAM_PropInfo,
				    SPA_PARAM_Props };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamList, id,
				SPA_PARAM_LIST_id, SPA_POD_Id(list[*index]));
		else
			return 0;
		break;
	}
	case SPA_PARAM_PropInfo:
	{
		struct props *p = &this->props;

		switch (*index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_minLatency),
				SPA_PROP_INFO_name, SPA_POD_String("The minimum latency"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(p->min_latency, 1, INT32_MAX));
			break;
		case 1:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_maxLatency),
				SPA_PROP_INFO_name, SPA_POD_String("The maximum latency"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(p->max_latency, 1, INT32_MAX));
			break;
		default:
			return 0;
		}
		break;
	}
	case SPA_PARAM_Props:
	{
		struct props *p = &this->props;

		switch (*index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_Props, id,
				SPA_PROP_minLatency, SPA_POD_Int(p->min_latency),
				SPA_PROP_maxLatency, SPA_POD_Int(p->max_latency));
			break;
		default:
			return 0;
		}
		break;
	}
	default:
		return -ENOENT;
	}

	(*index)++;

	if (spa_pod_filter(builder, result, param, filter) < 0)
		goto next;

	return 1;
}

static int impl_node_set_io(struct spa_node *node, uint32_t id, void *data, size_t size)
{
	return 0;
}

static int impl_node_set_param(struct spa_node *node, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	switch (id) {
	case SPA_PARAM_Props:
	{
		struct props *p = &this->props;

		if (param == NULL) {
			reset_props(p);
			return 0;
		}
		spa_pod_parse_object(param,
			SPA_TYPE_OBJECT_Props, NULL,
			SPA_PROP_minLatency, SPA_POD_OPT_Int(&p->min_latency),
			SPA_PROP_maxLatency, SPA_POD_OPT_Int(&p->max_latency));
		break;
	}
	default:
		return -ENOENT;
	}

	return 0;
}

static void reset_buffers(struct impl *this)
{
	uint32_t i;

	spa_list_init(&this->free);
	spa_list_init(&this->ready);

	for (i = 0; i < this->n_buffers; i++) {
		struct buffer *b = &this->buffers[i];
		spa_list_append(&this->free, &b->link);
		b->outstanding = true;
	}
}

static void decode_data(struct impl *this, uint8_t *src, size_t src_size)
{
	const ssize_t header_size = sizeof(struct rtp_header) + sizeof(struct rtp_payload);
	struct buffer *b;
	struct spa_data *d;
	uint8_t *dest;
	size_t decoded, avail, written;
	struct spa_io_buffers *io;

	src += header_size;
	src_size -= header_size;

	while (src_size > 0) {
		if (spa_list_is_empty(&this->free)) {
			spa_log_warn(this->log, "no more buffers");
			return;
		}

		b = spa_list_first(&this->free, struct buffer, link);

		if (b->h) {
			b->h->seq = this->sample_count;
			b->h->pts = SPA_TIMESPEC_TO_NSEC(&this->now);
			b->h->dts_offset = 0;
		}

		d = b->buf->datas;
		dest = d[0].data;
		avail = d[0].maxsize;

		while (avail > 0 && src_size > 0) {
			decoded = sbc_decode(&this->sbc,
				src, src_size,
				dest, avail, &written);
			if (decoded <= 0) {
				spa_log_error(this->log, "sbc decoder error");
				return;
			}

			src_size -= decoded;
			src += decoded;
			avail -= written;
			dest += written;
		}

		d[0].chunk->offset = 0;
		d[0].chunk->size = d[0].maxsize - avail;
		d[0].chunk->stride = this->frame_size;
		this->sample_count += d[0].chunk->size / this->frame_size;

		spa_list_remove(&b->link);
		b->outstanding = false;

		io = this->io;
		if (io != NULL && io->status != SPA_STATUS_HAVE_BUFFER) {
			io->buffer_id = b->id;
			io->status = SPA_STATUS_HAVE_BUFFER;
		}
		else {
			spa_list_append(&this->ready, &b->link);
		}

		this->callbacks->ready(this->callbacks_data, SPA_STATUS_HAVE_BUFFER);
	}
}

static void a2dp_on_ready_read(struct spa_source *source)
{
	struct impl *this = source->data;
	ssize_t r;

	if ((source->rmask & SPA_IO_IN) == 0) {
		spa_log_error(this->log, "source error, rmask=%d", source->rmask);
		goto stop;
	}

	clock_gettime(CLOCK_MONOTONIC, &this->now);

again:
	r = read(this->transport->fd, this->buffer, sizeof(this->buffer));
	if (r < 0) {
		/* interrupted, retry now */
		if (errno == EINTR)
			goto again;
		/* socket is not ready after all... */
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;
		spa_log_error(this->log, "read: %s", strerror(errno));
		goto stop;
	}

	spa_log_trace(this->log, "a2dp-source %p: read %d", this, (int) r);

	decode_data(this, this->buffer, r);
	return;

stop:
	if (this->source.loop)
		spa_loop_remove_source(this->data_loop, &this->source);
}

static int do_start(struct impl *this)
{
	int res, val;

	if (this->started)
		return 0;

	spa_log_debug(this->log, "a2dp-source %p: start", this);

	if ((res = this->transport->acquire(this->transport, false)) < 0)
		return res;

	sbc_init_a2dp(&this->sbc, 0, this->transport->configuration,
		this->transport->configuration_len);

	val = FILL_FRAMES * this->transport->write_mtu;
	if (setsockopt(this->transport->fd, SOL_SOCKET, SO_SNDBUF, &val, sizeof(val)) < 0)
		spa_log_warn(this->log, "a2dp-source %p: SO_SNDBUF %m", this);

	val = FILL_FRAMES * this->transport->read_mtu;
	if (setsockopt(this->transport->fd, SOL_SOCKET, SO_RCVBUF, &val, sizeof(val)) < 0)
		spa_log_warn(this->log, "a2dp-source %p: SO_RCVBUF %m", this);

	val = 6;
	if (setsockopt(this->transport->fd, SOL_SOCKET, SO_PRIORITY, &val, sizeof(val)) < 0)
		spa_log_warn(this->log, "SO_PRIORITY failed: %m");

	reset_buffers(this);

	this->source.data = this;
	this->source.fd = this->transport->fd;
	this->source.func = a2dp_on_ready_read;
	this->source.mask = SPA_IO_IN;
	this->source.rmask = 0;
	spa_loop_add_source(this->data_loop, &this->source);

	this->sample_count = 0;
	this->started = true;

	return 0;
}

static int do_remove_source(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct impl *this = user_data;

	if (this->source.loop)
		spa_loop_remove_source(this->data_loop, &this->source);

	return 0;
}

static int do_stop(struct impl *this)
{
	int res;

	if (!this->started)
		return 0;

	spa_log_debug(this->log, "a2dp-source %p: stop", this);

	spa_loop_invoke(this->data_loop, do_remove_source, 0, NULL, 0, true, this);

	this->started = false;

	res = this->transport->release(this->transport);

	sbc_finish(&this->sbc);

	return res;
}

static int impl_node_send_command(struct spa_node *node, const struct spa_command *command)
{
	struct impl *this;
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if (!this->have_format)
			return -EIO;
		if (this->n_buffers == 0)
			return -EIO;

		if ((res = do_start(this)) < 0)
			return res;
		break;
	case SPA_NODE_COMMAND_Pause:
		if ((res = do_stop(this)) < 0)
			return res;
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static const struct spa_dict_item node_info_items[] = {
	{ "media.class", "Audio/Source" },
        { "node.driver", "true" },
};

static void emit_node_info(struct impl *this)
{
	if (this->callbacks && this->callbacks->info) {
		struct spa_node_info info;

		info = SPA_NODE_INFO_INIT();
		info.max_output_ports = 1;
		info.change_mask = SPA_NODE_CHANGE_MASK_PROPS;
		info.props = &SPA_DICT_INIT_ARRAY(node_info_items);

		this->callbacks->info(this->callbacks_data, &info);
	}
}

static void emit_port_info(struct impl *this)
{
	if (this->callbacks && this->callbacks->port_info && this->info.change_mask) {
		this->callbacks->port_info(this->callbacks_data, SPA_DIRECTION_OUTPUT, 0, &this->info);
		this->info.change_mask = 0;
	}
}

static int
impl_node_set_callbacks(struct spa_node *node,
			const struct spa_node_callbacks *callbacks,
			void *data)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	this->callbacks = callbacks;
	this->callbacks_data = data;

	emit_node_info(this);
	emit_port_info(this);

	return 0;
}

static int impl_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
		const struct spa_dict *props)
{
	return -ENOTSUP;
}

static int impl_node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int
impl_node_port_enum_params(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t *index,
			   const struct spa_pod *filter,
			   struct spa_pod **result,
			   struct spa_pod_builder *builder)
{

	struct impl *this;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_List:
	{
		uint32_t list[] = { SPA_PARAM_EnumFormat,
				    SPA_PARAM_Format,
				    SPA_PARAM_Buffers,
				    SPA_PARAM_Meta };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_add_object(&b,
					SPA_TYPE_OBJECT_ParamList, id,
					SPA_PARAM_LIST_id, SPA_POD_Id(list[*index]));
		else
			return 0;
		break;
	}
	case SPA_PARAM_EnumFormat:
		if (*index > 0)
			return 0;

		switch (this->transport->codec) {
		case A2DP_CODEC_SBC:
		{
			a2dp_sbc_t *config = this->transport->configuration;
			struct spa_audio_info_raw info = { 0, };

			info.format = SPA_AUDIO_FORMAT_S16;
			if ((info.rate = a2dp_sbc_get_frequency(config)) < 0)
				return -EIO;
			if ((info.channels = a2dp_sbc_get_channels(config)) < 0)
				return -EIO;

			switch (info.channels) {
			case 1:
				info.position[0] = SPA_AUDIO_CHANNEL_MONO;
				break;
			case 2:
				info.position[0] = SPA_AUDIO_CHANNEL_FL;
				info.position[1] = SPA_AUDIO_CHANNEL_FR;
				break;
			default:
				return -EIO;
			}

			param = spa_format_audio_raw_build(&b, id, &info);
			break;
		}
		default:
			return -EIO;
		}
		break;

	case SPA_PARAM_Format:
		if (!this->have_format)
			return -EIO;
		if (*index > 0)
			return 0;

		param = spa_format_audio_raw_build(&b, id, &this->current_format.info.raw);
		break;

	case SPA_PARAM_Buffers:
		if (!this->have_format)
			return -EIO;
		if (*index > 0)
			return 0;

		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, id,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(2, 2, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
								this->props.min_latency * this->frame_size,
								this->props.min_latency * this->frame_size,
								INT32_MAX),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(0),
			SPA_PARAM_BUFFERS_align,   SPA_POD_Int(16));
		break;

	case SPA_PARAM_Meta:
		if (!this->have_format)
			return -EIO;

		switch (*index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamMeta, id,
				SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
				SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
			break;
		default:
			return 0;
		}
		break;

	case SPA_PARAM_IO:
		switch (*index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_buffers)));
			break;
		default:
			return 0;
		}
		break;

	default:
		return -ENOENT;
	}

	(*index)++;

	if (spa_pod_filter(builder, result, param, filter) < 0)
		goto next;

	return 1;
}

static int clear_buffers(struct impl *this)
{
	do_stop(this);
	if (this->n_buffers > 0) {
		spa_list_init(&this->free);
		spa_list_init(&this->ready);
		this->n_buffers = 0;
	}
	return 0;
}

static int port_set_format(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t flags,
			   const struct spa_pod *format)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	int err;

	if (format == NULL) {
		spa_log_info(this->log, "clear format");
		clear_buffers(this);
		this->have_format = false;
	} else {
		struct spa_audio_info info = { 0 };

		if ((err = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0)
			return err;

		if (info.media_type != SPA_MEDIA_TYPE_audio ||
		    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
			return -EINVAL;

		if (spa_format_audio_raw_parse(format, &info.info.raw) < 0)
			return -EINVAL;

		this->frame_size = info.info.raw.channels * 2;
		this->current_format = info;
		this->have_format = true;
	}

	if (this->have_format) {
		this->info.rate = this->current_format.info.raw.rate;
		this->info.change_mask |= SPA_PORT_CHANGE_MASK_RATE;
		emit_port_info(this);
	}

	return 0;
}

static int
impl_node_port_set_param(struct spa_node *node,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	spa_return_val_if_fail(node != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(node, direction, port_id), -EINVAL);

	if (id == SPA_PARAM_Format) {
		return port_set_format(node, direction, port_id, flags, param);
	}
	else
		return -ENOENT;
}

static int
impl_node_port_use_buffers(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id, struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct impl *this;
	uint32_t i;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	spa_log_info(this->log, "use buffers %d", n_buffers);

	if (!this->have_format)
		return -EIO;

	clear_buffers(this);

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &this->buffers[i];
		struct spa_data *d = buffers[i]->datas;

		b->buf = buffers[i];
		b->id = i;
		b->outstanding = true;

		b->h = spa_buffer_find_meta_data(buffers[i], SPA_META_Header, sizeof(*b->h));

		if (!((d[0].type == SPA_DATA_MemFd ||
		       d[0].type == SPA_DATA_DmaBuf ||
		       d[0].type == SPA_DATA_MemPtr) && d[0].data != NULL)) {
			spa_log_error(this->log, NAME " %p: need mapped memory", this);
			return -EINVAL;
		}
		spa_list_append(&this->free, &b->link);
	}
	this->n_buffers = n_buffers;

	return 0;
}

static int
impl_node_port_alloc_buffers(struct spa_node *node,
			     enum spa_direction direction,
			     uint32_t port_id,
			     struct spa_pod **params,
			     uint32_t n_params,
			     struct spa_buffer **buffers,
			     uint32_t *n_buffers)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(buffers != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	if (!this->have_format)
		return -EIO;

	return -ENOTSUP;
}

static int
impl_node_port_set_io(struct spa_node *node,
		      enum spa_direction direction,
		      uint32_t port_id,
		      uint32_t id,
		      void *data, size_t size)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	switch (id) {
	case SPA_IO_Buffers:
		this->io = data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static void recycle_buffer(struct impl *this, uint32_t buffer_id)
{
	struct buffer *b = &this->buffers[buffer_id];

	if (!b->outstanding) {
		spa_log_trace(this->log, NAME " %p: recycle buffer %u", this, buffer_id);
		spa_list_append(&this->free, &b->link);
		b->outstanding = true;
	}
}

static int impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(port_id == 0, -EINVAL);

	if (this->n_buffers == 0)
		return -EIO;

	if (buffer_id >= this->n_buffers)
		return -EINVAL;

	recycle_buffer(this, buffer_id);

	return 0;
}

static int impl_node_process(struct spa_node *node)
{
	struct impl *this;
	struct spa_io_buffers *io;
	struct buffer *b;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	io = this->io;
	spa_return_val_if_fail(io != NULL, -EIO);

	if (io->status == SPA_STATUS_HAVE_BUFFER)
		return SPA_STATUS_HAVE_BUFFER;

	if (io->buffer_id < this->n_buffers) {
		recycle_buffer(this, io->buffer_id);
		io->buffer_id = SPA_ID_INVALID;
	}

	if (spa_list_is_empty(&this->ready))
		return SPA_STATUS_OK;

	b = spa_list_first(&this->ready, struct buffer, link);
	spa_list_remove(&b->link);

	spa_log_trace(this->log, NAME " %p: dequeue buffer %d", node, b->id);

	io->buffer_id = b->id;
	io->status = SPA_STATUS_HAVE_BUFFER;

	return SPA_STATUS_HAVE_BUFFER;
}

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	.enum_params = impl_node_enum_params,
	.set_param = impl_node_set_param,
	.set_io = impl_node_set_io,
	.send_command = impl_node_send_command,
	.set_callbacks = impl_node_set_callbacks,
	.add_port = impl_node_add_port,
	.remove_port = impl_node_remove_port,
	.port_enum_params = impl_node_port_enum_params,
	.port_set_param = impl_node_port_set_param,
	.port_use_buffers = impl_node_port_use_buffers,
	.port_alloc_buffers = impl_node_port_alloc_buffers,
	.port_set_io = impl_node_port_set_io,
	.port_reuse_buffer = impl_node_port_reuse_buffer,
	.process = impl_node_process,
};

static int impl_get_interface(struct spa_handle *handle, uint32_t type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (type == SPA_TYPE_INTERFACE_Node)
		*interface = &this->node;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct impl);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *this;
	uint32_t i;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	for (i = 0; i < n_support; i++) {
		if (support[i].type == SPA_TYPE_INTERFACE_Log)
			this->log = support[i].data;
		else if (support[i].type == SPA_TYPE_INTERFACE_DataLoop)
			this->data_loop = support[i].data;
		else if (support[i].type == SPA_TYPE_INTERFACE_MainLoop)
			this->main_loop = support[i].data;
	}
	if (this->data_loop == NULL) {
		spa_log_error(this->log, "a data loop is needed");
		return -EINVAL;
	}
	if (this->main_loop == NULL) {
		spa_log_error(this->log, "a main loop is needed");
		return -EINVAL;
	}

	this->node = impl_node;
	reset_props(&this->props);

	this->info = SPA_PORT_INFO_INIT();
	this->info.change_mask = SPA_PORT_CHANGE_MASK_FLAGS;
	this->info.flags = SPA_PORT_FLAG_CAN_USE_BUFFERS |
			   SPA_PORT_FLAG_LIVE |
			   SPA_PORT_FLAG_TERMINAL;

	spa_list_init(&this->ready);
	spa_list_init(&this->free);

	for (i = 0; info && i < info->n_items; i++) {
		if (strcmp(info->items[i].key, "bluez5.transport") == 0)
			sscanf(info->items[i].value, "%p", &this->transport);
	}
	if (this->transport == NULL) {
		spa_log_error(this->log, "a transport is needed");
		return -EINVAL;
	}
	if (this->transport->codec != A2DP_CODEC_SBC) {
		spa_log_error(this->log, "codec != SBC not yet supported");
		return -EINVAL;
	}

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Node,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*info = &impl_interfaces[*index];
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}

static const struct spa_dict_item info_items[] = {
	{ "factory.author", "George Kiagiadakis <george.kiagiadakis@collabora.com>" },
	{ "factory.description", "Capture bluetooth audio with a2dp" },
};

static const struct spa_dict info = SPA_DICT_INIT_ARRAY(info_items);

struct spa_handle_factory spa_a2dp_source_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	&info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
