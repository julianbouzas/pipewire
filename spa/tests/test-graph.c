/* Spa
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>

#include <spa/node.h>
#include <spa/log.h>
#include <spa/log-impl.h>
#include <spa/loop.h>
#include <spa/type-map.h>
#include <spa/type-map-impl.h>
#include <spa/audio/format-utils.h>
#include <spa/format-utils.h>
#include <spa/format-builder.h>
#include <spa/graph.h>

static SPA_TYPE_MAP_IMPL(default_map, 4096);
static SPA_LOG_IMPL(default_log);

struct type {
	uint32_t node;
	uint32_t props;
	uint32_t format;
	uint32_t props_device;
	uint32_t props_freq;
	uint32_t props_volume;
	uint32_t props_min_latency;
	uint32_t props_live;
	struct spa_type_meta meta;
	struct spa_type_data data;
	struct spa_type_media_type media_type;
	struct spa_type_media_subtype media_subtype;
	struct spa_type_format_audio format_audio;
	struct spa_type_audio_format audio_format;
	struct spa_type_event_node event_node;
	struct spa_type_command_node command_node;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->node = spa_type_map_get_id(map, SPA_TYPE__Node);
	type->props = spa_type_map_get_id(map, SPA_TYPE__Props);
	type->format = spa_type_map_get_id(map, SPA_TYPE__Format);
	type->props_device = spa_type_map_get_id(map, SPA_TYPE_PROPS__device);
	type->props_freq = spa_type_map_get_id(map, SPA_TYPE_PROPS__frequency);
	type->props_volume = spa_type_map_get_id(map, SPA_TYPE_PROPS__volume);
	type->props_min_latency = spa_type_map_get_id(map, SPA_TYPE_PROPS__minLatency);
	type->props_live = spa_type_map_get_id(map, SPA_TYPE_PROPS__live);
	spa_type_meta_map(map, &type->meta);
	spa_type_data_map(map, &type->data);
	spa_type_media_type_map(map, &type->media_type);
	spa_type_media_subtype_map(map, &type->media_subtype);
	spa_type_format_audio_map(map, &type->format_audio);
	spa_type_audio_format_map(map, &type->audio_format);
	spa_type_event_node_map(map, &type->event_node);
	spa_type_command_node_map(map, &type->command_node);
}

struct buffer {
	struct spa_buffer buffer;
	struct spa_meta metas[1];
	struct spa_meta_header header;
	struct spa_data datas[1];
	struct spa_chunk chunks[1];
};

struct data {
	struct spa_type_map *map;
	struct spa_log *log;
	struct spa_loop data_loop;
	struct type type;

	struct spa_support support[4];
	uint32_t n_support;

	struct spa_graph graph;
	struct spa_graph_node source_node;
	struct spa_graph_port source_out;
	struct spa_graph_port volume_in;
	struct spa_graph_node volume_node;
	struct spa_graph_port volume_out;
	struct spa_graph_port sink_in;
	struct spa_graph_node sink_node;

	struct spa_node *sink;
	struct spa_port_io volume_sink_io[1];

	struct spa_node *volume;
	struct spa_buffer *volume_buffers[1];
	struct buffer volume_buffer[1];

	struct spa_node *source;
	struct spa_port_io source_volume_io[1];
	struct spa_buffer *source_buffers[1];
	struct buffer source_buffer[1];

	bool running;
	pthread_t thread;

	struct spa_source sources[16];
	unsigned int n_sources;

	bool rebuild_fds;
	struct pollfd fds[16];
	unsigned int n_fds;
};

#define MIN_LATENCY     64

#define BUFFER_SIZE    MIN_LATENCY

static void
init_buffer(struct data *data, struct spa_buffer **bufs, struct buffer *ba, int n_buffers,
	    size_t size)
{
	int i;

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &ba[i];
		bufs[i] = &b->buffer;

		b->buffer.id = i;
		b->buffer.n_metas = 1;
		b->buffer.metas = b->metas;
		b->buffer.n_datas = 1;
		b->buffer.datas = b->datas;

		b->header.flags = 0;
		b->header.seq = 0;
		b->header.pts = 0;
		b->header.dts_offset = 0;
		b->metas[0].type = data->type.meta.Header;
		b->metas[0].data = &b->header;
		b->metas[0].size = sizeof(b->header);

		b->datas[0].type = data->type.data.MemPtr;
		b->datas[0].flags = 0;
		b->datas[0].fd = -1;
		b->datas[0].mapoffset = 0;
		b->datas[0].maxsize = size;
		b->datas[0].data = malloc(size);
		b->datas[0].chunk = &b->chunks[0];
		b->datas[0].chunk->offset = 0;
		b->datas[0].chunk->size = size;
		b->datas[0].chunk->stride = 0;
	}
}

static int make_node(struct data *data, struct spa_node **node, const char *lib, const char *name)
{
	struct spa_handle *handle;
	int res;
	void *hnd;
	spa_handle_factory_enum_func_t enum_func;
	uint32_t i;

	if ((hnd = dlopen(lib, RTLD_NOW)) == NULL) {
		printf("can't load %s: %s\n", lib, dlerror());
		return SPA_RESULT_ERROR;
	}
	if ((enum_func = dlsym(hnd, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME)) == NULL) {
		printf("can't find enum function\n");
		return SPA_RESULT_ERROR;
	}

	for (i = 0;; i++) {
		const struct spa_handle_factory *factory;
		void *iface;

		if ((res = enum_func(&factory, i)) < 0) {
			if (res != SPA_RESULT_ENUM_END)
				printf("can't enumerate factories: %d\n", res);
			break;
		}
		if (strcmp(factory->name, name))
			continue;

		handle = calloc(1, factory->size);
		if ((res =
		     spa_handle_factory_init(factory, handle, NULL, data->support,
					     data->n_support)) < 0) {
			printf("can't make factory instance: %d\n", res);
			return res;
		}
		if ((res = spa_handle_get_interface(handle, data->type.node, &iface)) < 0) {
			printf("can't get interface %d\n", res);
			return res;
		}
		*node = iface;
		return SPA_RESULT_OK;
	}
	return SPA_RESULT_ERROR;
}

static void on_sink_done(struct spa_node *node, int seq, int res, void *user_data)
{
	printf("got done %d %d\n", seq, res);
}

static void on_sink_event(struct spa_node *node, struct spa_event *event, void *user_data)
{
	printf("got event %d\n", SPA_EVENT_TYPE(event));
}

static void on_sink_need_input(struct spa_node *node, void *user_data)
{
	struct data *data = user_data;

	data->sink_node.action = SPA_GRAPH_ACTION_CHECK;
	data->sink_node.state = SPA_RESULT_NEED_BUFFER;

	spa_graph_node_schedule(&data->graph, &data->sink_node);
}

static void
on_sink_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id, void *user_data)
{
	struct data *data = user_data;

	data->volume_sink_io[0].buffer_id = buffer_id;
}

static const struct spa_node_callbacks sink_callbacks = {
	SPA_VERSION_NODE_CALLBACKS,
	&on_sink_done,
	&on_sink_event,
	&on_sink_need_input,
	NULL,
	&on_sink_reuse_buffer
};

static int do_add_source(struct spa_loop *loop, struct spa_source *source)
{
	struct data *data = SPA_CONTAINER_OF(loop, struct data, data_loop);

	data->sources[data->n_sources] = *source;
	data->n_sources++;
	data->rebuild_fds = true;

	return SPA_RESULT_OK;
}

static int do_update_source(struct spa_source *source)
{
	return SPA_RESULT_OK;
}

static void do_remove_source(struct spa_source *source)
{
}

static int
do_invoke(struct spa_loop *loop,
	  spa_invoke_func_t func, uint32_t seq, size_t size, void *data, void *user_data)
{
	return func(loop, false, seq, size, data, user_data);
}

static int make_nodes(struct data *data, const char *device)
{
	int res;
	struct spa_props *props;
	struct spa_pod_builder b = { 0 };
	struct spa_pod_frame f[2];
	uint8_t buffer[128];

	if ((res = make_node(data, &data->sink,
			     "build/spa/plugins/alsa/libspa-alsa.so", "alsa-sink")) < 0) {
		printf("can't create alsa-sink: %d\n", res);
		return res;
	}
	spa_node_set_callbacks(data->sink, &sink_callbacks, data);

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	spa_pod_builder_props(&b, &f[0], data->type.props,
		SPA_POD_PROP(&f[1], data->type.props_device, 0, SPA_POD_TYPE_STRING, 1,
			device ? device : "hw:0"),
		SPA_POD_PROP(&f[1], data->type.props_min_latency, 0, SPA_POD_TYPE_INT, 1,
			MIN_LATENCY));
	props = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_props);

	if ((res = spa_node_set_props(data->sink, props)) < 0)
		printf("got set_props error %d\n", res);

	if ((res = make_node(data, &data->volume,
			     "build/spa/plugins/volume/libspa-volume.so", "volume")) < 0) {
		printf("can't create volume: %d\n", res);
		return res;
	}

	if ((res = make_node(data, &data->source,
			     "build/spa/plugins/audiotestsrc/libspa-audiotestsrc.so",
			     "audiotestsrc")) < 0) {
		printf("can't create audiotestsrc: %d\n", res);
		return res;
	}

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	spa_pod_builder_props(&b, &f[0], data->type.props,
		SPA_POD_PROP(&f[1], data->type.props_freq, 0, SPA_POD_TYPE_DOUBLE, 1,
			600.0),
		SPA_POD_PROP(&f[1], data->type.props_volume, 0, SPA_POD_TYPE_DOUBLE, 1,
			0.5),
		SPA_POD_PROP(&f[1], data->type.props_live, 0, SPA_POD_TYPE_BOOL, 1,
			false));
	props = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_props);

	if ((res = spa_node_set_props(data->source, props)) < 0)
		printf("got set_props error %d\n", res);

	data->source_volume_io[0] = SPA_PORT_IO_INIT;
	data->volume_sink_io[0] = SPA_PORT_IO_INIT;

	spa_node_port_set_io(data->source, SPA_DIRECTION_OUTPUT, 0, &data->source_volume_io[0]);
	spa_node_port_set_io(data->volume, SPA_DIRECTION_INPUT, 0, &data->source_volume_io[0]);
	spa_node_port_set_io(data->volume, SPA_DIRECTION_OUTPUT, 0, &data->volume_sink_io[0]);
	spa_node_port_set_io(data->sink, SPA_DIRECTION_INPUT, 0, &data->volume_sink_io[0]);

	spa_graph_node_add(&data->graph, &data->source_node, spa_graph_node_schedule_default,
			   data->source);
	spa_graph_port_add(&data->graph, &data->source_node, &data->source_out,
			   SPA_DIRECTION_OUTPUT, 0, 0, &data->source_volume_io[0]);

	spa_graph_node_add(&data->graph, &data->volume_node, spa_graph_node_schedule_default,
			   data->volume);
	spa_graph_port_add(&data->graph, &data->volume_node, &data->volume_in, SPA_DIRECTION_INPUT,
			   0, 0, &data->source_volume_io[0]);

	spa_graph_port_link(&data->graph, &data->source_out, &data->volume_in);

	spa_graph_port_add(&data->graph, &data->volume_node,
			   &data->volume_out, SPA_DIRECTION_OUTPUT, 0, 0, &data->volume_sink_io[0]);

	spa_graph_node_add(&data->graph, &data->sink_node, spa_graph_node_schedule_default,
			   data->sink);
	spa_graph_port_add(&data->graph, &data->sink_node, &data->sink_in, SPA_DIRECTION_INPUT, 0,
			   0, &data->volume_sink_io[0]);

	spa_graph_port_link(&data->graph, &data->volume_out, &data->sink_in);

	return res;
}

static int negotiate_formats(struct data *data)
{
	int res;
	struct spa_format *format, *filter;
	uint32_t state = 0;
	struct spa_pod_builder b = { 0 };
	struct spa_pod_frame f[2];
	uint8_t buffer[256];

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	spa_pod_builder_format(&b, &f[0], data->type.format,
		data->type.media_type.audio,
		data->type.media_subtype.raw,
		SPA_POD_PROP(&f[1], data->type.format_audio.format, 0, SPA_POD_TYPE_ID, 1,
			data->type.audio_format.S16),
		SPA_POD_PROP(&f[1], data->type.format_audio.layout, 0, SPA_POD_TYPE_INT, 1,
			SPA_AUDIO_LAYOUT_INTERLEAVED),
		SPA_POD_PROP(&f[1], data->type.format_audio.rate, 0, SPA_POD_TYPE_INT, 1,
			44100),
		SPA_POD_PROP(&f[1], data->type.format_audio.channels, 0, SPA_POD_TYPE_INT, 1,
			2));
	filter = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_format);

	if ((res =
	     spa_node_port_enum_formats(data->sink, SPA_DIRECTION_INPUT, 0, &format, filter,
					state)) < 0)
		return res;


	if ((res = spa_node_port_set_format(data->sink, SPA_DIRECTION_INPUT, 0, 0, format)) < 0)
		return res;

	if ((res = spa_node_port_set_format(data->volume, SPA_DIRECTION_OUTPUT, 0, 0, format)) < 0)
		return res;

	init_buffer(data, data->volume_buffers, data->volume_buffer, 1, BUFFER_SIZE);
	if ((res =
	     spa_node_port_use_buffers(data->sink, SPA_DIRECTION_INPUT, 0, data->volume_buffers,
				       1)) < 0)
		return res;
	if ((res =
	     spa_node_port_use_buffers(data->volume, SPA_DIRECTION_OUTPUT, 0, data->volume_buffers,
				       1)) < 0)
		return res;

	if ((res = spa_node_port_set_format(data->volume, SPA_DIRECTION_INPUT, 0, 0, format)) < 0)
		return res;
	if ((res = spa_node_port_set_format(data->source, SPA_DIRECTION_OUTPUT, 0, 0, format)) < 0)
		return res;

	init_buffer(data, data->source_buffers, data->source_buffer, 1, BUFFER_SIZE);
	if ((res =
	     spa_node_port_use_buffers(data->volume, SPA_DIRECTION_INPUT, 0, data->source_buffers,
				       1)) < 0)
		return res;
	if ((res =
	     spa_node_port_use_buffers(data->source, SPA_DIRECTION_OUTPUT, 0, data->source_buffers,
				       1)) < 0)
		return res;

	return SPA_RESULT_OK;
}

static void *loop(void *user_data)
{
	struct data *data = user_data;

	printf("enter thread %d\n", data->n_sources);
	while (data->running) {
		int i, r;

		/* rebuild */
		if (data->rebuild_fds) {
			for (i = 0; i < data->n_sources; i++) {
				struct spa_source *p = &data->sources[i];
				data->fds[i].fd = p->fd;
				data->fds[i].events = p->mask;
			}
			data->n_fds = data->n_sources;
			data->rebuild_fds = false;
		}

		r = poll(data->fds, data->n_fds, -1);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0) {
			fprintf(stderr, "select timeout");
			break;
		}

		/* after */
		for (i = 0; i < data->n_sources; i++) {
			struct spa_source *p = &data->sources[i];
			p->rmask = 0;
			if (data->fds[i].revents & POLLIN)
				p->rmask |= SPA_IO_IN;
			if (data->fds[i].revents & POLLOUT)
				p->rmask |= SPA_IO_OUT;
			if (data->fds[i].revents & POLLHUP)
				p->rmask |= SPA_IO_HUP;
			if (data->fds[i].revents & POLLERR)
				p->rmask |= SPA_IO_ERR;
		}
		for (i = 0; i < data->n_sources; i++) {
			struct spa_source *p = &data->sources[i];
			if (p->rmask)
				p->func(p);
		}
	}
	printf("leave thread\n");

	return NULL;
}

static void run_async_sink(struct data *data)
{
	int res;
	int err;

	{
		struct spa_command cmd = SPA_COMMAND_INIT(data->type.command_node.Start);
		if ((res = spa_node_send_command(data->source, &cmd)) < 0)
			printf("got source error %d\n", res);
		if ((res = spa_node_send_command(data->volume, &cmd)) < 0)
			printf("got volume error %d\n", res);
		if ((res = spa_node_send_command(data->sink, &cmd)) < 0)
			printf("got sink error %d\n", res);
	}

	data->running = true;
	if ((err = pthread_create(&data->thread, NULL, loop, data)) != 0) {
		printf("can't create thread: %d %s", err, strerror(err));
		data->running = false;
	}

	printf("sleeping for 1000 seconds\n");
	sleep(1000);

	if (data->running) {
		data->running = false;
		pthread_join(data->thread, NULL);
	}

	{
		struct spa_command cmd = SPA_COMMAND_INIT(data->type.command_node.Pause);
		if ((res = spa_node_send_command(data->sink, &cmd)) < 0)
			printf("got error %d\n", res);
		if ((res = spa_node_send_command(data->volume, &cmd)) < 0)
			printf("got volume error %d\n", res);
		if ((res = spa_node_send_command(data->source, &cmd)) < 0)
			printf("got source error %d\n", res);
	}
}

int main(int argc, char *argv[])
{
	struct data data = { NULL };
	int res;
	const char *str;


	spa_graph_init(&data.graph);

	data.map = &default_map.map;
	data.log = &default_log.log;
	data.data_loop.version = SPA_VERSION_LOOP;
	data.data_loop.add_source = do_add_source;
	data.data_loop.update_source = do_update_source;
	data.data_loop.remove_source = do_remove_source;
	data.data_loop.invoke = do_invoke;

	if ((str = getenv("SPA_DEBUG")))
		data.log->level = atoi(str);

	data.support[0].type = SPA_TYPE__TypeMap;
	data.support[0].data = data.map;
	data.support[1].type = SPA_TYPE__Log;
	data.support[1].data = data.log;
	data.support[2].type = SPA_TYPE_LOOP__DataLoop;
	data.support[2].data = &data.data_loop;
	data.support[3].type = SPA_TYPE_LOOP__MainLoop;
	data.support[3].data = &data.data_loop;
	data.n_support = 4;

	init_type(&data.type, data.map);

	if ((res = make_nodes(&data, argc > 1 ? argv[1] : NULL)) < 0) {
		printf("can't make nodes: %d\n", res);
		return -1;
	}
	if ((res = negotiate_formats(&data)) < 0) {
		printf("can't negotiate nodes: %d\n", res);
		return -1;
	}

	run_async_sink(&data);
}
