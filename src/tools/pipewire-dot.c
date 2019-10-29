/* PipeWire
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

#include <stdio.h>
#include <signal.h>

#include <spa/debug/pod.h>
#include <spa/debug/format.h>
#include <spa/debug/types.h>

#include <pipewire/interfaces.h>
#include "pipewire/map.h"
#include <pipewire/type.h>
#include <pipewire/remote.h>
#include <pipewire/main-loop.h>
#include <pipewire/pipewire.h>

#define GLOBAL_ID_NONE UINT32_MAX

struct data {
	struct pw_main_loop *loop;
	struct pw_core *core;

	struct pw_remote *remote;
	struct spa_hook remote_listener;

	struct pw_core_proxy *core_proxy;
	struct spa_hook core_listener;

	struct pw_registry_proxy *registry_proxy;
	struct spa_hook registry_listener;

	struct pw_map nodes;
	struct pw_map links;
	struct pw_map clients;
	struct pw_map modules;
	struct pw_map devices;

	char *dot_str;
};

struct proxy_data {
	struct data *data;

	struct pw_proxy *proxy;
	uint32_t id;
	uint32_t type;
	struct pw_properties *props;

	struct spa_hook proxy_listener;
	struct spa_hook object_listener;
};

struct port {
        uint32_t id;
        struct pw_properties *props;
        struct pw_port_info *info;
};

struct node {
        uint32_t id;
        struct pw_properties *props;
        uint32_t client_id;
        struct pw_node_info *info;
        struct pw_map ports;
};

struct link {
        uint32_t id;
        struct pw_properties *props;
        uint32_t client_id;
        struct pw_link_info *info;
};

struct client {
        uint32_t id;
        struct pw_properties *props;
        struct pw_client_info *info;
};

struct module {
        uint32_t id;
        struct pw_properties *props;
        struct pw_module_info *info;
};

struct device {
        uint32_t id;
        struct pw_properties *props;
        struct pw_device_info *info;
};

static void map_add(struct pw_map *map, uint32_t id, void *obj) {
        size_t size = pw_map_get_size(map);
        while (id > size)
                pw_map_insert_at(map, size++, NULL);
        pw_map_insert_at(map, id, obj);
}

static void *map_lookup(struct pw_map *map, uint32_t id) {
        void *obj = NULL;
        if ((obj = pw_map_lookup(map, id)) != NULL)
                return obj;
        return NULL;
}

static struct node *create_node(uint32_t id) {
        struct node *n = calloc(1, sizeof(struct node));
        if (n == NULL)
                return NULL;
        n->id = id;
        n->client_id = GLOBAL_ID_NONE;
        pw_map_init(&n->ports, 8, 8);
        return n;
}

static struct port *create_port(uint32_t id) {
        struct port *p = calloc(1, sizeof(struct port));
        if (p == NULL)
                return NULL;
        p->id = id;
        return p;
}

static struct link *create_link(uint32_t id) {
        struct link *l = calloc(1, sizeof(struct link));
        if (l == NULL)
                  return NULL;
        l->id = id;
        l->client_id = GLOBAL_ID_NONE;
        return l;
}

static struct client *create_client(uint32_t id) {
        struct client *c = calloc(1, sizeof(struct client));
        if (c == NULL)
                  return NULL;
        c->id = id;
        return c;
}

static struct module *create_module(uint32_t id) {
        struct module *m = calloc(1, sizeof(struct module));
        if (m == NULL)
                  return NULL;
        m->id = id;
        return m;
}

static struct device *create_device(uint32_t id) {
        struct device *d = calloc(1, sizeof(struct device));
        if (d == NULL)
                  return NULL;
        d->id = id;
        return d;
}

static void destroy_port(struct port *p) {
        if (p->props) {
                pw_properties_free(p->props);
                p->props = NULL;
        }
        if (p->info) {
                pw_port_info_free(p->info);
                p->info = NULL;
        }
        free(p);
}

static int destroy_ports_foreach(void *port, void *data) {
        if (port)
                destroy_port(port);
        return 0;
}

static void destroy_node(struct node *n) {
        if (n->props) {
                pw_properties_free(n->props);
                n->props = NULL;
        };
        if (n->info) {
                pw_node_info_free(n->info);
                n->info = NULL;
        }
        pw_map_for_each(&n->ports, destroy_ports_foreach, NULL);
        free(n);
}

static int destroy_node_foreach(void *node, void *data) {
        if (node)
                destroy_node(node);
        return 0;
}

static void destroy_link(struct link *l) {
        if (l->props) {
                pw_properties_free(l->props);
                l->props = NULL;
        }
        if (l->info) {
                pw_link_info_free(l->info);
                l->info = NULL;
        }
        free(l);
}

static int destroy_link_foreach(void *link, void *data) {
        if (link)
                destroy_link(link);
        return 0;
}

static void destroy_client(struct client *c) {
        if (c->props) {
                pw_properties_free(c->props);
                c->props = NULL;
        }
        if (c->info) {
                pw_client_info_free(c->info);
                c->info = NULL;
        }
        free(c);
}

static int destroy_client_foreach(void *client, void *data) {
        if (client)
                destroy_client(client);
        return 0;
}

static void destroy_module(struct module *m) {
        if (m->props) {
                pw_properties_free(m->props);
                m->props = NULL;
        }
        if (m->info) {
                pw_module_info_free(m->info);
                m->info = NULL;
        }
        free(m);
}

static int destroy_module_foreach(void *module, void *data) {
        if (module)
                destroy_module(module);
        return 0;
}

static void destroy_device(struct device *d) {
        if (d->props) {
                pw_properties_free(d->props);
                d->props = NULL;
        }
        if (d->info) {
                pw_device_info_free(d->info);
                d->info = NULL;
        }
        free(d);
}

static int destroy_device_foreach(void *device, void *data) {
        if (device)
                destroy_device(device);
        return 0;
}

struct node_linked_data {
        uint32_t node_id;
        bool res;
};

static int node_linked_foreach(void *link, void *data) {
        struct node_linked_data *d = data;
        struct link *l = link;

        if (l == NULL || l->info == NULL)
                return 0;

        if (l->info->input_node_id == d->node_id ||
            l->info->output_node_id == d->node_id)
          d->res = true;

        return 0;
}

static bool is_node_linked(struct node *n, struct pw_map *links) {
        struct node_linked_data d = {n->id, false};
        pw_map_for_each(links, node_linked_foreach, &d);
        return d.res;
}

static char *dot_str_new() {
	return strdup("");
}

static void dot_str_clear(char **str) {
	if (str && *str) {
		  free(*str);
		  *str = NULL;
	}
}

static int dot_str_add(char **str, const char *fmt, ...) {
	char *res = NULL;
	char *fmt2 = NULL;
	va_list varargs;

	if (!str || !fmt)
		return -EINVAL;

	if (asprintf(&fmt2, "%s%s", *str, fmt) < 0) {
		return -ENOMEM;
	}

	va_start(varargs, fmt);
	if (vasprintf(&res, fmt2, varargs) < 0) {
		free (fmt2);
		return -ENOMEM;
	}
	va_end(varargs);
	free (fmt2);

	free(*str);
	*str = res;
	return 0;
}

static int dot_str_add_header(char **str) {
	return dot_str_add(str, "digraph pipewire {\n");
}

static int dot_str_add_footer(char **str) {
	return dot_str_add(str, "}\n");
}

static int dot_str_add_port(char **str, struct port *p) {
	const char *name = spa_dict_lookup(p->info->props, PW_KEY_PORT_NAME);
	const char *node_id = spa_dict_lookup(p->info->props, PW_KEY_NODE_ID);
	const char *prop_node_id = pw_properties_get(p->props, PW_KEY_NODE_ID);
	const char *color = p->info->direction == PW_DIRECTION_INPUT ? "lightslateblue" : "lightcoral";
	return dot_str_add(str,
		"port_%u [shape=box style=filled fillcolor=%s]\n"
		"port_%u [label=\"port_id: %u\\lname: %s\\ldirection: %s\\lnode_id: %s\\lprop_node_id: %s\\l\"]\n",
		p->id, color, p->id, p->id, name, pw_direction_as_string(p->info->direction), node_id, prop_node_id
	);
}

static int dot_str_add_ports_foreach(void *port, void *data) {
	struct port *p = port;
	char **str = data;

	if (p == NULL || p->info == NULL)
		return 0;

	dot_str_add_port(str, p);
	return 0;
}

static int dot_str_add_client(char **str, struct client *c) {
	return dot_str_add(str,
		"client_%u [shape=box style=filled fillcolor=lightblue];\n"
		"client_%u [label=\"client_id: %u\\lname: %s\\lpid: %s\\l\"];\n",
		c->id, c->id, c->id,
		spa_dict_lookup(c->info->props, PW_KEY_APP_NAME),
		spa_dict_lookup(c->info->props, PW_KEY_APP_PROCESS_ID)
	);
}

static int dot_str_add_clients_foreach(void *client, void *data) {
	struct data *d = data;
	struct client *c = client;

	if (c == NULL || c->info == NULL)
		return 0;

	dot_str_add_client(&d->dot_str, c);
	return 0;
}

static int dot_str_add_node(char **str, struct node *n, struct pw_map *links) {
	uint32_t object_id = GLOBAL_ID_NONE;

	/* draw the header */
	if (n->info) {
		const char *name = spa_dict_lookup(n->info->props, PW_KEY_NODE_NAME);
		const char *media_class = spa_dict_lookup(n->info->props, PW_KEY_MEDIA_CLASS);
		const char *object_id_str = spa_dict_lookup(n->info->props, PW_KEY_OBJECT_ID);
		/* Skip unlinked nodes */
		if (!is_node_linked(n, links))
			return 0;
		dot_str_add(str, "subgraph cluster_node_%u {\n", n->id);
		dot_str_add(str, "style=filled;\n");
		object_id = atoi(object_id_str);
		dot_str_add(str, "label=\"node_id: %u\\lname: %s\\lmedia_class: %s\\lobject_id: %u\\l\";\n",
			n->id, name, media_class, object_id);
		dot_str_add(str, "color=palegreen;\n");
	} else {
		dot_str_add(str, "subgraph cluster_node_%u {\n", n->id);
		dot_str_add(str, "style=filled;\n");
		dot_str_add(str, "label=\"node_id: %u\\l\";\n", n->id);
		dot_str_add(str, "color=gold;\n");
	}

	/* Drawa the clients box */
	dot_str_add(str, "node_%u [shape=box style=filled fillcolor=lightblue];\n", n->id);
	dot_str_add(str, "node_%u [label=\"client\"];\n", n->id);

	/* draw the ports inside the node */
	pw_map_for_each(&n->ports, dot_str_add_ports_foreach, str);

	/* draw the footer */
	dot_str_add(str, "}\n");

	/* draw the client arrow if it is valid */
	if (n->client_id != GLOBAL_ID_NONE)
		dot_str_add(str, "client_%u -> node_%u [style=dashed];\n", n->client_id, n->id);

	/* draw an arrow when object_id is different than node id */
	if (object_id != GLOBAL_ID_NONE && n->id != object_id)
		dot_str_add(str, "client_%u -> node_%u [style=dashed];\n", n->client_id, object_id);
	return 0;
}

static int dot_str_add_nodes_foreach(void *node, void *data) {
	struct data *d = data;
	struct node *n = node;

	if (n == NULL)
		return 0;

	dot_str_add_node(&d->dot_str, n, &d->links);
	return 0;
}

static int dot_str_add_link(char **str, struct link *l) {
	dot_str_add(str,
		"link_%u [shape=ellipse style=filled];\n"
		"link_%u [label=\"link_id: %u\n state: %s\"];\n",
		l->id, l->id, l->id, pw_link_state_as_string(l->info->state)
	);
	dot_str_add(str, "port_%u -> link_%u -> port_%u;\n", l->info->output_port_id, l->id, l->info->input_port_id);
	return 0;
}

static int dot_str_add_links_foreach(void *link, void *data) {
	struct data *d = data;
	struct link *l = link;

	if (l == NULL || l->info == NULL)
		return 0;

	dot_str_add_link(&d->dot_str, l);
	return 0;
}

static int dot_str_add_module(char **str, struct module *m) {
	return dot_str_add(str,
		"module_%u [shape=box style=filled];\n"
		"module_%u [label=\"module_id: %u\\lname: %s\\l\"];\n",
		m->id, m->id, m->id, m->info->name
	);
}

static int dot_str_add_modules_foreach(void *module, void *data) {
	struct data *d = data;
	struct module *m = module;

	if (m == NULL)
		return 0;

	dot_str_add_module(&d->dot_str, m);
	return 0;
}

static int dot_str_add_device(char **str, struct device *d) {
	const char *client_id_str = spa_dict_lookup(d->info->props, PW_KEY_CLIENT_ID);
	uint32_t client_id = atoi(client_id_str);
	dot_str_add(str,
		"device_%u [shape=box style=filled fillcolor=yellow];\n"
		"device_%u [label=\"device_id: %u\\lname: %s\\lmedia_class: %s\\lclient_id: %u\\lobject_id: %s\\lapi: %s\\lpath: %s\\l\"];\n",
		d->id, d->id, d->id,
		spa_dict_lookup(d->info->props, PW_KEY_DEVICE_NAME),
		spa_dict_lookup(d->info->props, PW_KEY_MEDIA_CLASS),
		client_id,
		spa_dict_lookup(d->info->props, PW_KEY_OBJECT_ID),
		spa_dict_lookup(d->info->props, PW_KEY_DEVICE_API),
		spa_dict_lookup(d->info->props, PW_KEY_OBJECT_PATH)
	);
	dot_str_add(str, "device_%u -> client_%u [style=dashed];\n", d->id, client_id);
	return 0;
}

static int dot_str_add_devices_foreach(void *device, void *data) {
	struct data *d = data;
	struct device *dev = device;

	if (dev == NULL || dev->info == NULL)
		return 0;

	dot_str_add_device(&d->dot_str, dev);
	return 0;
}

static void print_dot(struct data *d) {
	dot_str_add_header(&d->dot_str);

	pw_map_for_each(&d->nodes, dot_str_add_nodes_foreach, d);
	pw_map_for_each(&d->links, dot_str_add_links_foreach, d);
	pw_map_for_each(&d->clients, dot_str_add_clients_foreach, d);
	// pw_map_for_each(&d->modules, dot_str_add_modules_foreach, d);
	(void)dot_str_add_modules_foreach;
	pw_map_for_each(&d->devices, dot_str_add_devices_foreach, d);

	dot_str_add_footer(&d->dot_str);

	printf("%s\n", d->dot_str);
}

static void do_quit(void *data, int signal_number)
{
	struct data *d = data;
	pw_main_loop_quit(d->loop);
}

static void on_core_done(void *data, uint32_t id, int seq)
{
	struct data *d = data;
	pw_main_loop_quit(d->loop);
}

static void on_core_info(void *data, const struct pw_core_info *info)
{
}

static void node_event_info(void *object, const struct pw_node_info *info)
{
        struct proxy_data *data = object;
        struct node *n;
        const char *client_id_str;

	/* add the node if it does not exist */
	n = map_lookup(&data->data->nodes, data->id);
	if (n == NULL) {
		n = create_node(data->id);
		map_add(&data->data->nodes, data->id, n);
	}

	/* set the properties */
	n->props = pw_properties_copy(data->props);

	/* set the client id if any */
	client_id_str = spa_dict_lookup(info->props, PW_KEY_CLIENT_ID);
	if (client_id_str)
		n->client_id = atoi(client_id_str);

	/* set the node info */
	n->info = pw_node_info_update(n->info, info);
}

static const struct pw_node_proxy_events node_events = {
	PW_VERSION_NODE_PROXY_EVENTS,
        .info = node_event_info,
};

static void port_event_info(void *object, const struct pw_port_info *info)
{
        struct proxy_data *data = object;
        struct port *p;
        const char *node_id_str;
        uint32_t node_id = 0;
        struct node *n;

        /* get the node id */
        node_id_str = spa_dict_lookup(info->props, PW_KEY_NODE_ID);
        if (node_id_str == NULL) {
                printf("Skipping port with Id: %u\n", data->id);
                return;
        }
        node_id = atoi(node_id_str);

        /* add the node if it does not exist */
        n = map_lookup(&data->data->nodes, node_id);
        if (n == NULL) {
                n = create_node(node_id);
                map_add(&data->data->nodes, node_id, n);
        }

        /* add the port if it does not exist */
        p = map_lookup(&n->ports, node_id);
        if (p == NULL) {
                p = create_port(data->id);
                map_add(&n->ports, data->id, p);
        }

	/* set the properties */
	p->props = pw_properties_copy(data->props);

        /* set the port info */
        p->info = pw_port_info_update(p->info, info);
}

static const struct pw_port_proxy_events port_events = {
	PW_VERSION_PORT_PROXY_EVENTS,
        .info = port_event_info,
};

static void link_event_info(void *object, const struct pw_link_info *info)
{
        struct proxy_data *data = object;
        struct link *l;
        const char *client_id_str;

        /* add the link if it does not exist */
        l = map_lookup(&data->data->links, data->id);
        if (l == NULL) {
                l = create_link(data->id);
                map_add(&data->data->links, data->id, l);
        }

	/* set the properties */
	l->props = pw_properties_copy(data->props);

	/* set the client id if any */
	client_id_str = spa_dict_lookup(info->props, PW_KEY_CLIENT_ID);
	if (client_id_str)
		l->client_id = atoi(client_id_str);

        l->info = pw_link_info_update(l->info, info);
}

static const struct pw_link_proxy_events link_events = {
	PW_VERSION_LINK_PROXY_EVENTS,
	.info = link_event_info
};

static void client_event_info(void *object, const struct pw_client_info *info)
{
        struct proxy_data *data = object;
        struct client *c;

        /* add the client if it does not exist */
        c = map_lookup(&data->data->clients, data->id);
        if (c == NULL) {
                c = create_client(data->id);
                map_add(&data->data->clients, data->id, c);
        }

	/* set the properties */
	c->props = pw_properties_copy(data->props);

        c->info = pw_client_info_update(c->info, info);
}

static const struct pw_client_proxy_events client_events = {
	PW_VERSION_CLIENT_PROXY_EVENTS,
	.info = client_event_info
};

static void module_event_info(void *object, const struct pw_module_info *info)
{
        struct proxy_data *data = object;
        struct module *m;

        /* add the module if it does not exist */
        m = map_lookup(&data->data->modules, data->id);
        if (m == NULL) {
                m = create_module(data->id);
                map_add(&data->data->modules, data->id, m);
        }

	/* set the properties */
	m->props = pw_properties_copy(data->props);

        m->info = pw_module_info_update(m->info, info);
}

static const struct pw_module_proxy_events module_events = {
	PW_VERSION_MODULE_PROXY_EVENTS,
	.info = module_event_info
};

static void device_event_info(void *object, const struct pw_device_info *info)
{
        struct proxy_data *data = object;
        struct device *d;

        /* add the device if it does not exist */
        d = map_lookup(&data->data->devices, data->id);
        if (d == NULL) {
                d = create_device(data->id);
                map_add(&data->data->devices, data->id, d);
        }

	/* set the properties */
	d->props = pw_properties_copy(data->props);

        d->info = pw_device_info_update(d->info, info);
}

static const struct pw_device_proxy_events device_events = {
	PW_VERSION_DEVICE_PROXY_EVENTS,
	.info = device_event_info
};

static void
destroy_proxy (void *data)
{
        struct proxy_data *pd = data;

        pw_properties_free(pd->props);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = destroy_proxy,
};

static void registry_event_global(void *data, uint32_t id,
				  uint32_t permissions, uint32_t type, uint32_t version,
				  const struct spa_dict *props)
{
        struct data *d = data;
        struct pw_proxy *proxy;
        uint32_t client_version;
        const void *events;
	struct proxy_data *pd;

	switch (type) {
	case PW_TYPE_INTERFACE_Node:
		events = &node_events;
		client_version = PW_VERSION_NODE_PROXY;
		break;
	case PW_TYPE_INTERFACE_Port:
		events = &port_events;
		client_version = PW_VERSION_PORT_PROXY;
		break;
	case PW_TYPE_INTERFACE_Link:
		events = &link_events;
		client_version = PW_VERSION_LINK_PROXY;
		break;
	case PW_TYPE_INTERFACE_Client:
		events = &client_events;
		client_version = PW_VERSION_CLIENT_PROXY;
		break;
	case PW_TYPE_INTERFACE_Module:
		events = &module_events;
		client_version = PW_VERSION_MODULE_PROXY;
		break;
	case PW_TYPE_INTERFACE_Device:
		events = &device_events;
		client_version = PW_VERSION_DEVICE_PROXY;
		break;
	case PW_TYPE_INTERFACE_Core:
		pw_core_proxy_sync(d->core_proxy, 0, 0);
		return;
	default:
		return;
	}

        proxy = pw_registry_proxy_bind(d->registry_proxy, id, type,
				       client_version,
				       sizeof(struct proxy_data));
	if (proxy == NULL)
		return;

	pd = pw_proxy_get_user_data(proxy);
	pd->data = d;
	pd->proxy = proxy;
	pd->id = id;
	pd->type = type;
	pd->props = props ? pw_properties_new_dict(props) : NULL;

        pw_proxy_add_object_listener(proxy, &pd->object_listener, events, pd);
        pw_proxy_add_listener(proxy, &pd->proxy_listener, &proxy_events, pd);
}

static void registry_event_global_remove(void *object, uint32_t id)
{
	printf("removed: %u\n", id);
}

static const struct pw_registry_proxy_events registry_events = {
	PW_VERSION_REGISTRY_PROXY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

static const struct pw_core_proxy_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.info = on_core_info,
	.done = on_core_done,
};

static void on_state_changed(void *_data, enum pw_remote_state old,
			     enum pw_remote_state state, const char *error)
{
	struct data *data = _data;

	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		printf("remote error: %s\n", error);
		pw_main_loop_quit(data->loop);
		break;

	case PW_REMOTE_STATE_CONNECTED:
		data->core_proxy = pw_remote_get_core_proxy(data->remote);
		pw_core_proxy_add_listener(data->core_proxy,
					   &data->core_listener,
					   &core_events, data);
		data->registry_proxy = pw_core_proxy_get_registry(data->core_proxy,
								  PW_VERSION_REGISTRY_PROXY, 0);
		pw_registry_proxy_add_listener(data->registry_proxy,
					       &data->registry_listener,
					       &registry_events, data);
		break;

	default:
		break;
	}
}

static const struct pw_remote_events remote_events = {
	PW_VERSION_REMOTE_EVENTS,
	.state_changed = on_state_changed,
};

int main(int argc, char *argv[])
{
	struct data data = { 0 };
	struct pw_loop *l;
	struct pw_properties *props = NULL;

	pw_init(&argc, &argv);

	data.loop = pw_main_loop_new(NULL);
	if (data.loop == NULL)
		return -1;

	l = pw_main_loop_get_loop(data.loop);
	pw_loop_add_signal(l, SIGINT, do_quit, &data);
	pw_loop_add_signal(l, SIGTERM, do_quit, &data);

	data.core = pw_core_new(l, NULL, 0);
	if (data.core == NULL)
		return -1;

	if (argc > 1)
		props = pw_properties_new(PW_KEY_REMOTE_NAME, argv[1], NULL);

	data.remote = pw_remote_new(data.core, props, 0);
	if (data.remote == NULL)
		return -1;

	pw_remote_add_listener(data.remote, &data.remote_listener, &remote_events, &data);
	if (pw_remote_connect(data.remote) < 0)
		return -1;

	data.dot_str = dot_str_new();
	if (data.dot_str == NULL)
		return -1;

	pw_map_init(&data.nodes, 64, 64);
	pw_map_init(&data.links, 16, 16);
	pw_map_init(&data.clients, 16, 16);
	pw_map_init(&data.modules, 16, 16);
	pw_map_init(&data.devices, 16, 16);

	pw_main_loop_run(data.loop);

	print_dot(&data);

	pw_map_for_each(&data.nodes, destroy_node_foreach, NULL);
	pw_map_for_each(&data.links, destroy_link_foreach, NULL);
	pw_map_for_each(&data.clients, destroy_client_foreach, NULL);
	pw_map_for_each(&data.modules, destroy_module_foreach, NULL);
	pw_map_for_each(&data.devices, destroy_device_foreach, NULL);
	dot_str_clear(&data.dot_str);
	pw_remote_destroy(data.remote);
	pw_core_destroy(data.core);
	pw_main_loop_destroy(data.loop);

	return 0;
}
