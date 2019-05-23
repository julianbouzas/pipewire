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

#include "endpoint-impl.h"
#include <pipewire/private.h>
#include <spa/pod/filter.h>
#include <spa/pod/compare.h>

struct pw_endpoint {
	struct pw_core *core;
	struct pw_global *global;
	struct pw_global *parent;

	struct pw_client_endpoint *client_ep;

	uint32_t n_params;
	struct spa_pod **params;

	struct pw_endpoint_info info;
	struct pw_properties *props;
};

struct pw_client_endpoint {
	struct pw_resource *owner_resource;
	struct spa_hook owner_resource_listener;

	struct pw_endpoint endpoint;
};

struct resource_data {
	struct pw_endpoint *endpoint;
	struct pw_client_endpoint *client_ep;

	struct spa_hook resource_listener;

	uint32_t n_subscribe_ids;
	uint32_t subscribe_ids[32];
};

static int
endpoint_enum_params (void *object, int seq,
		      uint32_t id, uint32_t start, uint32_t num,
		      const struct spa_pod *filter)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	struct pw_endpoint *this = data->endpoint;
	struct spa_pod *result;
	struct spa_pod *param;
	uint8_t buffer[1024];
	struct spa_pod_builder b = { 0 };
	uint32_t index;
	uint32_t next = start;
	uint32_t count = 0;

	while (true) {
		index = next++;
		if (index >= this->n_params)
			break;

		param = this->params[index];

		if (param == NULL || !spa_pod_is_object_id(param, id))
			continue;

		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		if (spa_pod_filter(&b, &result, param, filter) != 0)
			continue;

		pw_log_debug("endpoint %p: %d param %u", this, seq, index);

		pw_endpoint_resource_param(resource, seq, id, index, next, result);

		if (++count == num)
			break;
	}
	return 0;
}

static int
endpoint_subscribe_params (void *object, uint32_t *ids, uint32_t n_ids)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	uint32_t i;

	n_ids = SPA_MIN(n_ids, SPA_N_ELEMENTS(data->subscribe_ids));
	data->n_subscribe_ids = n_ids;

	for (i = 0; i < n_ids; i++) {
		data->subscribe_ids[i] = ids[i];
		pw_log_debug("endpoint %p: resource %d subscribe param %u",
			data->endpoint, resource->id, ids[i]);
		endpoint_enum_params(resource, 1, ids[i], 0, UINT32_MAX, NULL);
	}
	return 0;
}

static int
endpoint_set_param (void *object, uint32_t id, uint32_t flags,
		    const struct spa_pod *param)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	struct pw_client_endpoint *client_ep = data->client_ep;

	pw_client_endpoint_resource_set_param(client_ep->owner_resource,
						id, flags, param);

	return 0;
}

static const struct pw_endpoint_proxy_methods endpoint_methods = {
	PW_VERSION_ENDPOINT_PROXY_METHODS,
	.subscribe_params = endpoint_subscribe_params,
	.enum_params = endpoint_enum_params,
	.set_param = endpoint_set_param,
};

static void
endpoint_unbind(void *data)
{
	struct pw_resource *resource = data;
	spa_list_remove(&resource->link);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = endpoint_unbind,
};

static int
endpoint_bind(void *_data, struct pw_client *client, uint32_t permissions,
	      uint32_t version, uint32_t id)
{
	struct pw_endpoint *this = _data;
	struct pw_global *global = this->global;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions, global->type, version, sizeof(*data));
	if (resource == NULL)
		goto no_mem;

	data = pw_resource_get_user_data(resource);
	data->endpoint = this;
	data->client_ep = this->client_ep;
	pw_resource_add_listener(resource, &data->resource_listener, &resource_events, resource);

	pw_resource_set_implementation(resource, &endpoint_methods, resource);

	pw_log_debug("endpoint %p: bound to %d", this, resource->id);

	spa_list_append(&global->resource_list, &resource->link);

	this->info.change_mask = PW_ENDPOINT_CHANGE_MASK_PARAMS |
				 PW_ENDPOINT_CHANGE_MASK_PROPS;
	pw_endpoint_resource_info(resource, &this->info);
	this->info.change_mask = 0;

	return 0;

      no_mem:
	pw_log_error("can't create node resource");
	return -ENOMEM;
}

static int
pw_endpoint_init(struct pw_endpoint *this,
		 struct pw_core *core,
		 struct pw_client *owner,
		 struct pw_global *parent,
		 struct pw_properties *properties)
{
	struct pw_properties *props = NULL;

	pw_log_debug("endpoint %p: new", this);

	this->core = core;
	this->parent = parent;

	props = properties ? properties : pw_properties_new(NULL, NULL);
	if (!props)
		goto no_mem;

	this->props = pw_properties_copy (props);
	if (!this->props)
		goto no_mem;

	this->global = pw_global_new (core,
			PW_TYPE_INTERFACE_Endpoint,
			PW_VERSION_ENDPOINT,
			props, endpoint_bind, this);
	if (!this->global)
		goto no_mem;

	this->info.id = this->global->id;
	this->info.props = &this->props->dict;

	return pw_global_register(this->global, owner, parent);

      no_mem:
	pw_log_error("can't create endpoint - out of memory");
	if (props && !properties)
		pw_properties_free(props);
	if (this->props)
		pw_properties_free(this->props);
	return -ENOMEM;
}

static void
pw_endpoint_clear(struct pw_endpoint *this)
{
	uint32_t i;

	pw_log_debug("endpoint %p: destroy", this);

	pw_global_destroy(this->global);

	for (i = 0; i < this->n_params; i++)
		free(this->params[i]);
	free(this->params);

	free(this->info.params);

	if (this->props)
		pw_properties_free(this->props);
}

static void
endpoint_notify_subscribed(struct pw_endpoint *this,
			   uint32_t index, uint32_t next)
{
	struct pw_global *global = this->global;
	struct pw_resource *resource;
	struct resource_data *data;
	struct spa_pod *param = this->params[index];
	uint32_t id;
	uint32_t i;

	if (!param || !spa_pod_is_object (param))
		return;

	id = SPA_POD_OBJECT_ID (param);

	spa_list_for_each(resource, &global->resource_list, link) {
		data = pw_resource_get_user_data(resource);
		for (i = 0; i < data->n_subscribe_ids; i++) {
			if (data->subscribe_ids[i] == id) {
				pw_endpoint_resource_param(resource, 1, id,
					index, next, param);
			}
		}
	}
}

static int
client_endpoint_update(void *object,
	uint32_t change_mask,
	uint32_t n_params,
	const struct spa_pod **params,
	const struct pw_endpoint_info *info)
{
	struct pw_client_endpoint *cliep = object;
	struct pw_endpoint *this = &cliep->endpoint;

	if (change_mask & PW_CLIENT_ENDPOINT_UPDATE_PARAMS) {
		uint32_t i;

		pw_log_debug("endpoint %p: update %d params", this, n_params);

		for (i = 0; i < this->n_params; i++)
			free(this->params[i]);
		this->n_params = n_params;
		this->params = realloc(this->params, this->n_params * sizeof(struct spa_pod *));

		for (i = 0; i < this->n_params; i++) {
			this->params[i] = params[i] ? spa_pod_copy(params[i]) : NULL;
			endpoint_notify_subscribed(this, i, i+1);
		}
	}
	else if (change_mask & PW_CLIENT_ENDPOINT_UPDATE_PARAMS_INCREMENTAL) {
		uint32_t i, j;
		const struct spa_pod_prop *pold, *pnew;

		pw_log_debug("endpoint %p: update %d params incremental", this, n_params);

		for (i = 0; i < this->n_params; i++) {
			/* we only support incremental updates for controls */
			if (!spa_pod_is_object_id (this->params[i], PW_ENDPOINT_PARAM_Control))
				continue;

			for (j = 0; j < n_params; j++) {
				if (!spa_pod_is_object_id (params[j], PW_ENDPOINT_PARAM_Control)) {
					pw_log_warn ("endpoint %p: ignoring incremental update "
						"on non-control param", this);
					continue;
				}

				pold = spa_pod_object_find_prop (
					(const struct spa_pod_object *) this->params[i],
					NULL, PW_ENDPOINT_PARAM_CONTROL_id);
				pnew = spa_pod_object_find_prop (
					(const struct spa_pod_object *) params[j],
					NULL, PW_ENDPOINT_PARAM_CONTROL_id);

				if (pold && pnew && spa_pod_compare (&pold->value, &pnew->value) == 0) {
					free (this->params[i]);
					this->params[i] = spa_pod_copy (params[j]);
					endpoint_notify_subscribed(this, i, UINT32_MAX);
				}
			}
		}
	}

	if (change_mask & PW_CLIENT_ENDPOINT_UPDATE_INFO) {
		struct pw_global *global = this->global;
		struct pw_resource *resource;

		if (info->change_mask & PW_ENDPOINT_CHANGE_MASK_PARAMS) {
			size_t size = info->n_params * sizeof(struct spa_param_info);
			free(this->info.params);
			this->info.params = malloc(size);
			this->info.n_params = info->n_params;
			memcpy(this->info.params, info->params, size);
		}

		if (info->change_mask & PW_ENDPOINT_CHANGE_MASK_PROPS) {
			pw_properties_update(this->props, info->props);
		}

		this->info.change_mask = info->change_mask;
		spa_list_for_each(resource, &global->resource_list, link) {
			pw_endpoint_resource_info(resource, &this->info);
		}
		this->info.change_mask = 0;
	}

	return 0;
}

static struct pw_client_endpoint_proxy_methods client_endpoint_methods = {
	PW_VERSION_CLIENT_ENDPOINT_PROXY_METHODS,
	.update = client_endpoint_update,
};

static void
client_endpoint_resource_destroy(void *data)
{
	struct pw_client_endpoint *this = data;

	pw_log_debug("client-endpoint %p: destroy", this);

	pw_endpoint_clear(&this->endpoint);

	this->owner_resource = NULL;
	spa_hook_remove(&this->owner_resource_listener);
	free(this);
}

static const struct pw_resource_events owner_resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = client_endpoint_resource_destroy,
};

struct pw_client_endpoint *
pw_client_endpoint_new(struct pw_resource *owner_resource,
			struct pw_global *parent,
			struct pw_properties *properties)
{
	struct pw_client_endpoint *this;
	struct pw_client *owner = pw_resource_get_client(owner_resource);
	struct pw_core *core = pw_client_get_core(owner);

	this = calloc(1, sizeof(struct pw_client_endpoint));
	if (this == NULL)
		return NULL;

	pw_log_debug("client-endpoint %p: new", this);

	if (pw_endpoint_init(&this->endpoint, core, owner, parent, properties) < 0)
		goto error_no_endpoint;
	this->endpoint.client_ep = this;

	this->owner_resource = owner_resource;
	pw_resource_add_listener(this->owner_resource,
				 &this->owner_resource_listener,
				 &owner_resource_events,
				 this);
	pw_resource_set_implementation(this->owner_resource,
				       &client_endpoint_methods,
				       this);

	return this;

      error_no_endpoint:
	pw_resource_destroy(owner_resource);
	free(this);
	return NULL;
}

void
pw_client_endpoint_destroy(struct pw_client_endpoint *this)
{
	pw_resource_destroy(this->owner_resource);
}
