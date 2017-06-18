/* Spa
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#include <string.h>
#include <stdio.h>

#include <spa/log.h>
#include <spa/list.h>
#include <spa/type-map.h>
#include <spa/node.h>
#include <spa/audio/format-utils.h>
#include <spa/format-builder.h>
#include <lib/props.h>

#define NAME "audiomixer"

#define MAX_BUFFERS     64
#define MAX_PORTS       128

struct buffer {
	struct spa_buffer *outbuf;
	bool outstanding;
	struct spa_meta_header *h;
	struct spa_list link;
};

struct port {
	struct spa_port_io *io;

	bool have_format;
	struct spa_port_info info;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	struct spa_list queue;
	size_t queued_offset;
	size_t queued_bytes;
};

struct type {
	uint32_t node;
	uint32_t format;
	struct spa_type_media_type media_type;
	struct spa_type_media_subtype media_subtype;
	struct spa_type_format_audio format_audio;
	struct spa_type_audio_format audio_format;
	struct spa_type_command_node command_node;
	struct spa_type_meta meta;
	struct spa_type_data data;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->node = spa_type_map_get_id(map, SPA_TYPE__Node);
	type->format = spa_type_map_get_id(map, SPA_TYPE__Format);
	spa_type_media_type_map(map, &type->media_type);
	spa_type_media_subtype_map(map, &type->media_subtype);
	spa_type_format_audio_map(map, &type->format_audio);
	spa_type_audio_format_map(map, &type->audio_format);
	spa_type_command_node_map(map, &type->command_node);
	spa_type_meta_map(map, &type->meta);
	spa_type_data_map(map, &type->data);
}

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct type type;
	struct spa_type_map *map;
	struct spa_log *log;

	const struct spa_node_callbacks *callbacks;
	void *user_data;

	int port_count;
	int port_queued;
	struct port in_ports[MAX_PORTS];
	struct port out_ports[1];

	bool have_format;
	struct spa_audio_info format;
	uint8_t format_buffer[4096];

	bool started;
};

#define CHECK_PORT_NUM(this,d,p)     (((d) == SPA_DIRECTION_INPUT && (p) < MAX_PORTS) || \
                                      ((d) == SPA_DIRECTION_OUTPUT && (p) == 0))
#define CHECK_FREE_IN_PORT(this,d,p) ((d) == SPA_DIRECTION_INPUT && (p) < MAX_PORTS && !this->in_ports[(p)].io)
#define CHECK_IN_PORT(this,d,p)      ((d) == SPA_DIRECTION_INPUT && (p) < MAX_PORTS && this->in_ports[(p)].io)
#define CHECK_OUT_PORT(this,d,p)     ((d) == SPA_DIRECTION_OUTPUT && (p) == 0)
#define CHECK_PORT(this,d,p)         (CHECK_OUT_PORT(this,d,p) || CHECK_IN_PORT (this,d,p))

#define PROP(f,key,type,...)							\
	SPA_POD_PROP (f,key,0,type,1,__VA_ARGS__)
#define PROP_MM(f,key,type,...)							\
	SPA_POD_PROP (f,key,SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)
#define PROP_U_MM(f,key,type,...)						\
	SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_UNSET |				\
			SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)
#define PROP_EN(f,key,type,n,...)						\
	SPA_POD_PROP (f,key,SPA_POD_PROP_RANGE_ENUM,type,n,__VA_ARGS__)
#define PROP_U_EN(f,key,type,n,...)						\
	SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_UNSET |				\
			SPA_POD_PROP_RANGE_ENUM,type,n,__VA_ARGS__)

static int impl_node_get_props(struct spa_node *node, struct spa_props **props)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int impl_node_set_props(struct spa_node *node, const struct spa_props *props)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int impl_node_send_command(struct spa_node *node, struct spa_command *command)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(command != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (SPA_COMMAND_TYPE(command) == this->type.command_node.Start) {
		this->started = true;
	} else if (SPA_COMMAND_TYPE(command) == this->type.command_node.Pause) {
		this->started = false;
	} else
		return SPA_RESULT_NOT_IMPLEMENTED;

	return SPA_RESULT_OK;
}

static int
impl_node_set_callbacks(struct spa_node *node,
			const struct spa_node_callbacks *callbacks,
			void *user_data)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	this->callbacks = callbacks;
	this->user_data = user_data;

	return SPA_RESULT_OK;
}

static int
impl_node_get_n_ports(struct spa_node *node,
		      uint32_t *n_input_ports,
		      uint32_t *max_input_ports,
		      uint32_t *n_output_ports,
		      uint32_t *max_output_ports)
{
	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	if (n_input_ports)
		*n_input_ports = 0;
	if (max_input_ports)
		*max_input_ports = MAX_PORTS;
	if (n_output_ports)
		*n_output_ports = 1;
	if (max_output_ports)
		*max_output_ports = 1;

	return SPA_RESULT_OK;
}

static int
impl_node_get_port_ids(struct spa_node *node,
		       uint32_t n_input_ports,
		       uint32_t *input_ids,
		       uint32_t n_output_ports,
		       uint32_t *output_ids)
{
	struct impl *this;
	int i, idx;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (input_ids) {
		for (i = 0, idx = 0; i < MAX_PORTS && idx < n_input_ports; i++) {
			if (this->in_ports[i].io)
				input_ids[idx++] = i;
		}
	}
	if (n_output_ports > 0 && output_ids)
		output_ids[0] = 0;

	return SPA_RESULT_OK;
}

static int impl_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_FREE_IN_PORT(this, direction, port_id),
			       SPA_RESULT_INVALID_PORT);

	this->port_count++;
	spa_list_init(&this->in_ports[port_id].queue);

	this->in_ports[port_id].info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
	    SPA_PORT_INFO_FLAG_REMOVABLE |
	    SPA_PORT_INFO_FLAG_OPTIONAL | SPA_PORT_INFO_FLAG_IN_PLACE;
	return SPA_RESULT_OK;
}

static int
impl_node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	struct impl *this;
	struct spa_port_io *io;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_IN_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	io = this->in_ports[port_id].io;
	if (io && io->buffer_id)
		this->port_queued--;

	this->in_ports[port_id].io = NULL;
	this->port_count--;

	return SPA_RESULT_OK;
}


static int
impl_node_port_enum_formats(struct spa_node *node,
			    enum spa_direction direction,
			    uint32_t port_id,
			    struct spa_format **format,
			    const struct spa_format *filter,
			    uint32_t index)
{
	struct impl *this;
	int res;
	struct spa_format *fmt;
	uint8_t buffer[256];
	struct spa_pod_builder b = { NULL, };
	struct spa_pod_frame f[2];
	uint32_t count, match;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(format != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	count = match = filter ? 0 : index;

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (count++) {
	case 0:
		spa_pod_builder_format(&b, &f[0], this->type.format,
			this->type.media_type.audio,
			this->type.media_subtype.raw,
			PROP(&f[1], this->type.format_audio.format, SPA_POD_TYPE_ID,
				this->type.audio_format.S16),
			PROP_U_MM(&f[1], this->type.format_audio.rate, SPA_POD_TYPE_INT,
				44100,
				1, INT32_MAX),
			PROP_U_MM(&f[1], this->type.format_audio.channels, SPA_POD_TYPE_INT,
				2,
				1, INT32_MAX));
		break;
	default:
		return SPA_RESULT_ENUM_END;
	}
	fmt = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_format);

	spa_pod_builder_init(&b, this->format_buffer, sizeof(this->format_buffer));

	if ((res = spa_format_filter(fmt, filter, &b)) != SPA_RESULT_OK || match++ != index)
		goto next;

	*format = SPA_POD_BUILDER_DEREF(&b, 0, struct spa_format);

	return SPA_RESULT_OK;
}

static int clear_buffers(struct impl *this, struct port *port)
{
	if (port->n_buffers > 0) {
		spa_log_info(this->log, NAME " %p: clear buffers %p", this, port);
		port->n_buffers = 0;
		spa_list_init(&port->queue);
	}
	return SPA_RESULT_OK;
}

static int
impl_node_port_set_format(struct spa_node *node,
			  enum spa_direction direction,
			  uint32_t port_id,
			  uint32_t flags,
			  const struct spa_format *format)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[0];

	if (format == NULL) {
		port->have_format = false;
		clear_buffers(this, port);
	} else {
		struct spa_audio_info info = { SPA_FORMAT_MEDIA_TYPE(format),
			SPA_FORMAT_MEDIA_SUBTYPE(format),
		};

		if (info.media_type != this->type.media_type.audio ||
		    info.media_subtype != this->type.media_subtype.raw)
			return SPA_RESULT_INVALID_MEDIA_TYPE;

		if (!spa_format_audio_raw_parse(format, &info.info.raw, &this->type.format_audio))
			return SPA_RESULT_INVALID_MEDIA_TYPE;

		this->format = info;
		port->have_format = true;
	}

	return SPA_RESULT_OK;
}

static int
impl_node_port_get_format(struct spa_node *node,
			  enum spa_direction direction,
			  uint32_t port_id,
			  const struct spa_format **format)
{
	struct impl *this;
	struct port *port;
	struct spa_pod_builder b = { NULL, };
	struct spa_pod_frame f[2];

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(format != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[0];

	if (!port->have_format)
		return SPA_RESULT_NO_FORMAT;

	spa_pod_builder_init(&b, this->format_buffer, sizeof(this->format_buffer));
	spa_pod_builder_format(&b, &f[0], this->type.format,
		this->type.media_type.audio,
		this->type.media_subtype.raw,
		PROP(&f[1], this->type.format_audio.format, SPA_POD_TYPE_ID,
			this->format.info.raw.format),
		PROP(&f[1], this->type.format_audio.rate, SPA_POD_TYPE_INT,
			this->format.info.raw.rate),
		PROP(&f[1], this->type.format_audio.channels, SPA_POD_TYPE_INT,
			this->format.info.raw.channels));
	*format = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_format);

	return SPA_RESULT_OK;
}

static int
impl_node_port_get_info(struct spa_node *node,
			enum spa_direction direction,
			uint32_t port_id,
			const struct spa_port_info **info)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[0];
	*info = &port->info;

	return SPA_RESULT_OK;
}

static int
impl_node_port_enum_params(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t index,
			   struct spa_param **param)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_node_port_set_param(struct spa_node *node,
			 enum spa_direction direction,
			 uint32_t port_id,
			 const struct spa_param *param)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_node_port_use_buffers(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct impl *this;
	struct port *port;
	uint32_t i;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	port =
	    direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

	spa_return_val_if_fail(port->have_format, SPA_RESULT_NO_FORMAT);

	clear_buffers(this, port);

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b;
		struct spa_data *d = buffers[i]->datas;

		b = &port->buffers[i];
		b->outbuf = buffers[i];
		b->outstanding = direction == SPA_DIRECTION_INPUT ? true : false;
		b->h = spa_buffer_find_meta(buffers[i], this->type.meta.Header);

		if (!((d[0].type == this->type.data.MemPtr ||
		       d[0].type == this->type.data.MemFd ||
		       d[0].type == this->type.data.DmaBuf) && d[0].data != NULL)) {
			spa_log_error(this->log, NAME " %p: invalid memory on buffer %p", this,
				      buffers[i]);
			return SPA_RESULT_ERROR;
		}
		if (!b->outstanding)
			spa_list_insert(port->queue.prev, &b->link);
	}
	port->n_buffers = n_buffers;

	return SPA_RESULT_OK;
}

static int
impl_node_port_alloc_buffers(struct spa_node *node,
			     enum spa_direction direction,
			     uint32_t port_id,
			     struct spa_param **params,
			     uint32_t n_params,
			     struct spa_buffer **buffers,
			     uint32_t *n_buffers)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_node_port_set_io(struct spa_node *node,
		      enum spa_direction direction,
		      uint32_t port_id,
		      struct spa_port_io *io)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT_NUM(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	port =
	    direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];
	port->io = io;

	return SPA_RESULT_OK;
}

static void recycle_buffer(struct impl *this, uint32_t id)
{
	struct port *port = &this->out_ports[0];
	struct buffer *b = &port->buffers[id];

	if (!b->outstanding) {
		spa_log_warn(this->log, NAME "%p: buffer %d not outstanding", this, id);
		return;
	}

	spa_list_insert(port->queue.prev, &b->link);
	b->outstanding = false;
	spa_log_trace(this->log, NAME " %p: recycle buffer %d", this, id);
}

static int impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, SPA_DIRECTION_OUTPUT, port_id),
			       SPA_RESULT_INVALID_PORT);

	recycle_buffer(this, buffer_id);

	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_node_port_send_command(struct spa_node *node,
			    enum spa_direction direction,
			    uint32_t port_id,
			    struct spa_command *command)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static inline void
add_port_data(struct impl *this, void *out, size_t outsize, struct port *port, int layer)
{
	void *in;
	size_t insize;
	struct buffer *b;
	struct spa_data *id;

	b = spa_list_first(&port->queue, struct buffer, link);

	id = b->outbuf->datas;
	in = SPA_MEMBER(id[0].data, port->queued_offset + id[0].chunk->offset, void);
	insize = id[0].chunk->size - port->queued_offset;
	outsize = SPA_MIN(outsize, insize);

	{
		int i, size = outsize / 2;
		int16_t *op = out, *ip = in;

		if (layer == 0) {
			memcpy(op, ip, outsize);
		} else {
			for (i = 0; i < size; i++)
				op[i] = SPA_CLAMP(op[i] + ip[i], INT16_MIN, INT16_MAX);
		}
	}

	port->queued_offset += outsize;
	port->queued_bytes -= outsize;

	if (outsize == insize) {
		spa_log_trace(this->log, NAME " %p: return buffer %d on port %p %zd",
			      this, b->outbuf->id, port, outsize);
		port->io->buffer_id = b->outbuf->id;
		spa_list_remove(&b->link);
		b->outstanding = true;
		port->queued_offset = 0;
	} else {
		spa_log_trace(this->log, NAME " %p: keeping buffer %d on port %p %zd %zd",
			      this, b->outbuf->id, port, port->queued_bytes, outsize);
	}
}

static int mix_output(struct impl *this, size_t n_bytes)
{
	struct buffer *outbuf;
	int i, layer;
	struct port *outport;
	struct spa_port_io *output;
	struct spa_data *od;

	outport = &this->out_ports[0];
	output = outport->io;

	if (spa_list_is_empty(&outport->queue))
		return SPA_RESULT_OUT_OF_BUFFERS;

	outbuf = spa_list_first(&outport->queue, struct buffer, link);
	spa_list_remove(&outbuf->link);
	outbuf->outstanding = true;

	od = outbuf->outbuf->datas;
	n_bytes = SPA_MIN(n_bytes, od[0].maxsize);
	od[0].chunk->offset = 0;
	od[0].chunk->size = n_bytes;
	od[0].chunk->stride = 0;

	spa_log_trace(this->log, NAME " %p: dequeue output buffer %d %zd",
		      this, outbuf->outbuf->id, n_bytes);
	for (layer = 0, i = 0; i < MAX_PORTS; i++) {
		struct port *port = &this->in_ports[i];

		if (port->io == NULL || port->n_buffers == 0)
			continue;

		if (spa_list_is_empty(&port->queue)) {
			spa_log_warn(this->log, NAME " %p: underrun stream %d", this, i);
			port->queued_bytes = 0;
			port->queued_offset = 0;
			continue;
		}
		add_port_data(this, od[0].data, n_bytes, port, layer++);
	}
	output->buffer_id = outbuf->outbuf->id;
	output->status = SPA_RESULT_HAVE_BUFFER;

	return SPA_RESULT_HAVE_BUFFER;
}

static int impl_node_process_input(struct spa_node *node)
{
	struct impl *this;
	uint32_t i;
	struct port *outport;
	size_t min_queued = SIZE_MAX;
	struct spa_port_io *output;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	outport = &this->out_ports[0];
	output = outport->io;
	spa_return_val_if_fail(output != NULL, SPA_RESULT_ERROR);

	if (output->status == SPA_RESULT_HAVE_BUFFER)
		return SPA_RESULT_HAVE_BUFFER;

	for (i = 0; i < MAX_PORTS; i++) {
		struct port *port = &this->in_ports[i];
		struct spa_port_io *input;

		if ((input = port->io) == NULL || port->n_buffers == 0)
			continue;

		if (port->queued_bytes == 0 &&
		    input->status == SPA_RESULT_HAVE_BUFFER && input->buffer_id != SPA_ID_INVALID) {
			struct buffer *b = &port->buffers[input->buffer_id];

			if (!b->outstanding) {
				spa_log_warn(this->log, NAME " %p: buffer %u in use", this,
					     input->buffer_id);
				input->status = SPA_RESULT_INVALID_BUFFER_ID;
				continue;
			}

			b->outstanding = false;
			input->buffer_id = SPA_ID_INVALID;
			input->status = SPA_RESULT_OK;

			spa_list_insert(port->queue.prev, &b->link);
			port->queued_bytes += b->outbuf->datas[0].chunk->size;

			spa_log_trace(this->log, NAME " %p: queue buffer %d on port %d %zd %zd",
				      this, b->outbuf->id, i, port->queued_bytes, min_queued);
		}
		if (port->queued_bytes > 0 && port->queued_bytes < min_queued)
			min_queued = port->queued_bytes;
	}

	if (min_queued != SIZE_MAX && min_queued > 0) {
		output->status = mix_output(this, min_queued);
	} else {
		output->status = SPA_RESULT_NEED_BUFFER;
	}
	return output->status;
}

static int impl_node_process_output(struct spa_node *node)
{
	struct impl *this;
	struct port *port;
	struct spa_port_io *output;
	int i;
	size_t min_queued = SIZE_MAX;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	port = &this->out_ports[0];
	output = port->io;
	spa_return_val_if_fail(output != NULL, SPA_RESULT_ERROR);

	if (output->status == SPA_RESULT_HAVE_BUFFER)
		return SPA_RESULT_HAVE_BUFFER;

	/* recycle */
	if (output->buffer_id != SPA_ID_INVALID) {
		recycle_buffer(this, output->buffer_id);
		output->buffer_id = SPA_ID_INVALID;
	}
	/* produce more output if possible */
	for (i = 0; i < MAX_PORTS; i++) {
		struct port *port = &this->in_ports[i];

		if (port->io == NULL || port->n_buffers == 0)
			continue;

		if (port->queued_bytes < min_queued)
			min_queued = port->queued_bytes;
	}
	if (min_queued != SIZE_MAX && min_queued > 0) {
		output->status = mix_output(this, min_queued);
	} else {
		/* take requested output range and apply to input */
		for (i = 0; i < MAX_PORTS; i++) {
			struct port *port = &this->in_ports[i];
			struct spa_port_io *input;

			if ((input = port->io) == NULL || port->n_buffers == 0)
				continue;

			if (port->queued_bytes == 0) {
				input->range = output->range;
				input->status = SPA_RESULT_NEED_BUFFER;
			} else {
				input->status = SPA_RESULT_OK;
			}
			spa_log_trace(this->log, NAME " %p: port %d %d queued %zd, res %d", this,
				      i, output->range.min_size, port->queued_bytes, input->status);
		}
	}
	return output->status;
}

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	NULL,
	impl_node_get_props,
	impl_node_set_props,
	impl_node_send_command,
	impl_node_set_callbacks,
	impl_node_get_n_ports,
	impl_node_get_port_ids,
	impl_node_add_port,
	impl_node_remove_port,
	impl_node_port_enum_formats,
	impl_node_port_set_format,
	impl_node_port_get_format,
	impl_node_port_get_info,
	impl_node_port_enum_params,
	impl_node_port_set_param,
	impl_node_port_use_buffers,
	impl_node_port_alloc_buffers,
	impl_node_port_set_io,
	impl_node_port_reuse_buffer,
	impl_node_port_send_command,
	impl_node_process_input,
	impl_node_process_output,
};

static int impl_get_interface(struct spa_handle *handle, uint32_t interface_id, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(interface != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = (struct impl *) handle;

	if (interface_id == this->type.node)
		*interface = &this->node;
	else
		return SPA_RESULT_UNKNOWN_INTERFACE;

	return SPA_RESULT_OK;
}

static int impl_clear(struct spa_handle *handle)
{
	return SPA_RESULT_OK;
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

	spa_return_val_if_fail(factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	for (i = 0; i < n_support; i++) {
		if (strcmp(support[i].type, SPA_TYPE__TypeMap) == 0)
			this->map = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE__Log) == 0)
			this->log = support[i].data;
	}
	if (this->map == NULL) {
		spa_log_error(this->log, "an id-map is needed");
		return SPA_RESULT_ERROR;
	}
	init_type(&this->type, this->map);

	this->node = impl_node;

	this->out_ports[0].io = NULL;
	this->out_ports[0].info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
	    SPA_PORT_INFO_FLAG_NO_REF;
	spa_list_init(&this->out_ports[0].queue);

	return SPA_RESULT_OK;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE__Node,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t index)
{
	spa_return_val_if_fail(factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	switch (index) {
	case 0:
		*info = &impl_interfaces[index];
		break;
	default:
		return SPA_RESULT_ENUM_END;
	}
	return SPA_RESULT_OK;
}

const struct spa_handle_factory spa_audiomixer_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	NULL,
	sizeof(struct impl),
	impl_init,
	impl_enum_interface_info,
};
