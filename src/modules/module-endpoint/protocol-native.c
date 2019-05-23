/* PipeWire
 *
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

#include <pipewire/pipewire.h>
#include <spa/pod/parser.h>

#include <extensions/client-endpoint.h>
#include <extensions/endpoint.h>
#include <extensions/protocol-native.h>

static void
serialize_pw_endpoint_info(struct spa_pod_builder *b,
			   const struct pw_endpoint_info *info)
{
	struct spa_pod_frame f;
	uint32_t i, n_props;

	n_props = info->props ? info->props->n_items : 0;

	spa_pod_builder_push_struct(b, &f);
	spa_pod_builder_add(b,
		SPA_POD_Id(info->id),
		SPA_POD_Int(info->change_mask),
		SPA_POD_Int(info->n_params),
		SPA_POD_Int(n_props),
		NULL);

	for (i = 0; i < info->n_params; i++) {
		spa_pod_builder_add(b,
			SPA_POD_Id(info->params[i].id),
			SPA_POD_Int(info->params[i].flags), NULL);
	}

	for (i = 0; i < n_props; i++) {
		spa_pod_builder_add(b,
			SPA_POD_String(info->props->items[i].key),
			SPA_POD_String(info->props->items[i].value),
			NULL);
	}

	spa_pod_builder_pop(b, &f);
}

/* macro because of alloca() */
#define deserialize_pw_endpoint_info(p, f, info) \
do { \
	if (spa_pod_parser_push_struct(p, f) < 0 || \
	    spa_pod_parser_get(p, \
	    		SPA_POD_Id(&(info)->id), \
			SPA_POD_Int(&(info)->change_mask), \
			SPA_POD_Int(&(info)->n_params), \
			SPA_POD_Int(&(info)->props->n_items), \
			NULL) < 0) \
		return -EINVAL; \
	\
	if ((info)->n_params > 0) \
		(info)->params = alloca((info)->n_params * sizeof(struct spa_param_info)); \
	if ((info)->props->n_items > 0) \
		(info)->props->items = alloca((info)->props->n_items * sizeof(struct spa_dict_item)); \
	\
	for (i = 0; i < (info)->n_params; i++) { \
		if (spa_pod_parser_get(p, \
				SPA_POD_Id(&(info)->params[i].id), \
				SPA_POD_Int(&(info)->params[i].flags), \
				NULL) < 0) \
			return -EINVAL; \
	} \
	\
	for (i = 0; i < (info)->props->n_items; i++) { \
		if (spa_pod_parser_get(p, \
				SPA_POD_String(&(info)->props->items[i].key), \
				SPA_POD_String(&(info)->props->items[i].value), \
				NULL) < 0) \
			return -EINVAL; \
	} \
	\
	spa_pod_parser_pop(p, f); \
} while(0)

static int
endpoint_marshal_subscribe_params(void *object, uint32_t *ids, uint32_t n_ids)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy,
		PW_ENDPOINT_PROXY_METHOD_SUBSCRIBE_PARAMS, NULL);

	spa_pod_builder_add_struct(b,
			SPA_POD_Array(sizeof(uint32_t), SPA_TYPE_Id, n_ids, ids));

	return pw_protocol_native_end_proxy(proxy, b);
}

static int
endpoint_demarshal_subscribe_params(void *object, const struct pw_protocol_native_message *msg)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t csize, ctype, n_ids;
	uint32_t *ids;

	spa_pod_parser_init(&prs, msg->data, msg->size);
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_Array(&csize, &ctype, &n_ids, &ids)) < 0)
		return -EINVAL;

	if (ctype != SPA_TYPE_Id)
		return -EINVAL;

	return pw_resource_do(resource, struct pw_endpoint_proxy_methods,
			subscribe_params, 0, ids, n_ids);
}

static int
endpoint_marshal_enum_params(void *object, int seq, uint32_t id,
		uint32_t index, uint32_t num, const struct spa_pod *filter)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy,
		PW_ENDPOINT_PROXY_METHOD_ENUM_PARAMS, NULL);

	spa_pod_builder_add_struct(b,
			SPA_POD_Int(seq),
			SPA_POD_Id(id),
			SPA_POD_Int(index),
			SPA_POD_Int(num),
			SPA_POD_Pod(filter));

	return pw_protocol_native_end_proxy(proxy, b);
}

static int
endpoint_demarshal_enum_params(void *object, const struct pw_protocol_native_message *msg)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t id, index, num;
	int seq;
	struct spa_pod *filter;

	spa_pod_parser_init(&prs, msg->data, msg->size);
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_Int(&seq),
				SPA_POD_Id(&id),
				SPA_POD_Int(&index),
				SPA_POD_Int(&num),
				SPA_POD_Pod(&filter)) < 0)
		return -EINVAL;

	return pw_resource_do(resource, struct pw_endpoint_proxy_methods,
			enum_params, 0, seq, id, index, num, filter);
}

static int
endpoint_marshal_set_param(void *object, uint32_t id, uint32_t flags,
		const struct spa_pod *param)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy,
		PW_ENDPOINT_PROXY_METHOD_SET_PARAM, NULL);

	spa_pod_builder_add_struct(b,
			SPA_POD_Id(id),
			SPA_POD_Int(flags),
			SPA_POD_Pod(param));
	return pw_protocol_native_end_proxy(proxy, b);
}

static int
endpoint_demarshal_set_param(void *object, const struct pw_protocol_native_message *msg)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t id, flags;
	struct spa_pod *param;

	spa_pod_parser_init(&prs, msg->data, msg->size);
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_Id(&id),
				SPA_POD_Int(&flags),
				SPA_POD_Pod(&param)) < 0)
		return -EINVAL;

	return pw_resource_do(resource, struct pw_endpoint_proxy_methods,
			set_param, 0, id, flags, param);
}

static void
endpoint_marshal_info(void *object, const struct pw_endpoint_info *info)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource,
			PW_ENDPOINT_PROXY_EVENT_INFO, NULL);
	serialize_pw_endpoint_info (b, info);
	pw_protocol_native_end_resource(resource, b);
}

static int
endpoint_demarshal_info(void *object, const struct pw_protocol_native_message *msg)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	struct spa_pod_frame f;
	struct spa_dict props = SPA_DICT_INIT(NULL, 0);
	struct pw_endpoint_info info = { .props = &props };
	uint32_t i;

	spa_pod_parser_init(&prs, msg->data, msg->size);

	deserialize_pw_endpoint_info(&prs, &f, &info);

	return pw_proxy_notify(proxy, struct pw_endpoint_proxy_events,
		info, 0, &info);
}

static void
endpoint_marshal_param(void *object, int seq, uint32_t id,
		uint32_t index, uint32_t next, const struct spa_pod *param)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource,
			PW_ENDPOINT_PROXY_EVENT_PARAM, NULL);

	spa_pod_builder_add_struct(b,
			SPA_POD_Int(seq),
			SPA_POD_Id(id),
			SPA_POD_Int(index),
			SPA_POD_Int(next),
			SPA_POD_Pod(param));

	pw_protocol_native_end_resource(resource, b);
}

static int
endpoint_demarshal_param(void *object, const struct pw_protocol_native_message *msg)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t id, index, next;
	int seq;
	struct spa_pod *param;

	spa_pod_parser_init(&prs, msg->data, msg->size);
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_Int(&seq),
				SPA_POD_Id(&id),
				SPA_POD_Int(&index),
				SPA_POD_Int(&next),
				SPA_POD_Pod(&param)) < 0)
		return -EINVAL;

	return pw_proxy_notify(proxy, struct pw_endpoint_proxy_events, param, 0,
			seq, id, index, next, param);
}

static const struct pw_endpoint_proxy_methods pw_protocol_native_endpoint_method_marshal = {
	PW_VERSION_ENDPOINT_PROXY_METHODS,
	&endpoint_marshal_subscribe_params,
	&endpoint_marshal_enum_params,
	&endpoint_marshal_set_param,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_endpoint_method_demarshal[] = {
	{ &endpoint_demarshal_subscribe_params, 0 },
	{ &endpoint_demarshal_enum_params, 0 },
	{ &endpoint_demarshal_set_param, 0 }
};

static const struct pw_endpoint_proxy_events pw_protocol_native_endpoint_event_marshal = {
	PW_VERSION_ENDPOINT_PROXY_EVENTS,
	&endpoint_marshal_info,
	&endpoint_marshal_param,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_endpoint_event_demarshal[] = {
	{ &endpoint_demarshal_info, 0 },
	{ &endpoint_demarshal_param, 0 }
};

static const struct pw_protocol_marshal pw_protocol_native_endpoint_marshal = {
	PW_TYPE_INTERFACE_Endpoint,
	PW_VERSION_ENDPOINT,
	PW_ENDPOINT_PROXY_METHOD_NUM,
	PW_ENDPOINT_PROXY_EVENT_NUM,
	&pw_protocol_native_endpoint_method_marshal,
	&pw_protocol_native_endpoint_method_demarshal,
	&pw_protocol_native_endpoint_event_marshal,
	&pw_protocol_native_endpoint_event_demarshal,
};


static int
client_endpoint_marshal_update(
	void *object,
	uint32_t change_mask,
	uint32_t n_params,
	const struct spa_pod **params,
	const struct pw_endpoint_info *info)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;
	uint32_t i;

	b = pw_protocol_native_begin_proxy(proxy,
		PW_CLIENT_ENDPOINT_PROXY_METHOD_UPDATE, NULL);

	spa_pod_builder_push_struct(b, &f);
	spa_pod_builder_add(b,
		SPA_POD_Int(change_mask),
		SPA_POD_Int(n_params), NULL);

	for (i = 0; i < n_params; i++)
		spa_pod_builder_add(b, SPA_POD_Pod(params[i]), NULL);

	if (change_mask & PW_CLIENT_ENDPOINT_UPDATE_INFO)
		serialize_pw_endpoint_info(b, info);

	spa_pod_builder_pop(b, &f);

	return pw_protocol_native_end_proxy(proxy, b);
}

static int
client_endpoint_demarshal_update(void *object,
	const struct pw_protocol_native_message *msg)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	struct spa_pod_frame f[2];
	uint32_t change_mask, n_params;
	const struct spa_pod **params = NULL;
	struct spa_dict props = SPA_DICT_INIT(NULL, 0);
	struct pw_endpoint_info info = { .props = &props };
	uint32_t i;

	spa_pod_parser_init(&prs, msg->data, msg->size);
	if (spa_pod_parser_push_struct(&prs, &f[0]) < 0 ||
	    spa_pod_parser_get(&prs,
			SPA_POD_Int(&change_mask),
			SPA_POD_Int(&n_params), NULL) < 0)
		return -EINVAL;

	if (n_params > 0)
		params = alloca(n_params * sizeof(struct spa_pod *));
	for (i = 0; i < n_params; i++)
		if (spa_pod_parser_get(&prs,
				SPA_POD_PodObject(&params[i]), NULL) < 0)
			return -EINVAL;

	if (change_mask & PW_CLIENT_ENDPOINT_UPDATE_INFO)
		deserialize_pw_endpoint_info(&prs, &f[1], &info);

	pw_resource_do(resource, struct pw_client_endpoint_proxy_methods,
		update, 0, change_mask, n_params, params, &info);
	return 0;
}

static void
client_endpoint_marshal_set_param (void *object,
	uint32_t id, uint32_t flags,
	const struct spa_pod *param)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource,
		PW_CLIENT_ENDPOINT_PROXY_EVENT_SET_PARAM, NULL);

	spa_pod_builder_add_struct(b,
				SPA_POD_Id(id),
				SPA_POD_Int(flags),
				SPA_POD_Pod(param));

	pw_protocol_native_end_resource(resource, b);
}

static int
client_endpoint_demarshal_set_param(void *object,
	const struct pw_protocol_native_message *msg)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t id, flags;
	const struct spa_pod *param = NULL;

	spa_pod_parser_init(&prs, msg->data, msg->size);
	if (spa_pod_parser_get_struct(&prs,
			SPA_POD_Id(&id),
			SPA_POD_Int(&flags),
			SPA_POD_PodObject(&param)) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_client_endpoint_proxy_events,
			set_param, 0, id, flags, param);
	return 0;
}

static const struct pw_client_endpoint_proxy_methods pw_protocol_native_client_endpoint_method_marshal = {
	PW_VERSION_CLIENT_ENDPOINT_PROXY_METHODS,
	&client_endpoint_marshal_update,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_client_endpoint_method_demarshal[] = {
	{ &client_endpoint_demarshal_update, 0 }
};

static const struct pw_client_endpoint_proxy_events pw_protocol_native_client_endpoint_event_marshal = {
	PW_VERSION_CLIENT_ENDPOINT_PROXY_EVENTS,
	&client_endpoint_marshal_set_param,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_client_endpoint_event_demarshal[] = {
	{ &client_endpoint_demarshal_set_param, 0 }
};

static const struct pw_protocol_marshal pw_protocol_native_client_endpoint_marshal = {
	PW_TYPE_INTERFACE_ClientEndpoint,
	PW_VERSION_CLIENT_ENDPOINT,
	PW_CLIENT_ENDPOINT_PROXY_METHOD_NUM,
	PW_CLIENT_ENDPOINT_PROXY_EVENT_NUM,
	&pw_protocol_native_client_endpoint_method_marshal,
	&pw_protocol_native_client_endpoint_method_demarshal,
	&pw_protocol_native_client_endpoint_event_marshal,
	&pw_protocol_native_client_endpoint_event_demarshal,
};

struct pw_protocol *pw_protocol_native_ext_endpoint_init(struct pw_core *core)
{
	struct pw_protocol *protocol;

	protocol = pw_core_find_protocol(core, PW_TYPE_INFO_PROTOCOL_Native);

	if (protocol == NULL)
		return NULL;

	pw_protocol_add_marshal(protocol, &pw_protocol_native_client_endpoint_marshal);
	pw_protocol_add_marshal(protocol, &pw_protocol_native_endpoint_marshal);

	return protocol;
}
