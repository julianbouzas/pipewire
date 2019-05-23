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

#ifndef PIPEWIRE_EXT_CLIENT_ENDPOINT_H
#define PIPEWIRE_EXT_CLIENT_ENDPOINT_H

#include "endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

struct pw_client_endpoint_proxy;

#define PW_VERSION_CLIENT_ENDPOINT			0
#define PW_EXTENSION_MODULE_CLIENT_ENDPOINT		PIPEWIRE_MODULE_PREFIX "module-endpoint"

#define PW_CLIENT_ENDPOINT_PROXY_METHOD_UPDATE		0
#define PW_CLIENT_ENDPOINT_PROXY_METHOD_NUM		1

struct pw_client_endpoint_proxy_methods {
#define PW_VERSION_CLIENT_ENDPOINT_PROXY_METHODS	0
	uint32_t version;

	/**
	 * Update endpoint info
	 */
	int (*update) (void *object,
#define PW_CLIENT_ENDPOINT_UPDATE_PARAMS		(1 << 0)
#define PW_CLIENT_ENDPOINT_UPDATE_PARAMS_INCREMENTAL	(1 << 1)
#define PW_CLIENT_ENDPOINT_UPDATE_INFO			(1 << 2)
			uint32_t change_mask,
			uint32_t n_params,
			const struct spa_pod **params,
			const struct pw_endpoint_info *info);
};

static inline int
pw_client_endpoint_proxy_update(struct pw_client_endpoint_proxy *p,
				uint32_t change_mask,
				uint32_t n_params,
				const struct spa_pod **params,
				struct pw_endpoint_info *info)
{
	return pw_proxy_do((struct pw_proxy*)p,
			   struct pw_client_endpoint_proxy_methods, update,
			   change_mask, n_params, params, info);
}

#define PW_CLIENT_ENDPOINT_PROXY_EVENT_SET_PARAM	0
#define PW_CLIENT_ENDPOINT_PROXY_EVENT_NUM		1

struct pw_client_endpoint_proxy_events {
#define PW_VERSION_CLIENT_ENDPOINT_PROXY_EVENTS	0
	uint32_t version;

	/**
	 * Set a parameter on the endpoint
	 *
	 * \param id the parameter id to set
	 * \param flags extra parameter flags
	 * \param param the parameter to set
	 */
	void (*set_param) (void *object, uint32_t id, uint32_t flags,
			   const struct spa_pod *param);
};

static inline void
pw_client_endpoint_proxy_add_listener(struct pw_client_endpoint_proxy *p,
				struct spa_hook *listener,
				const struct pw_client_endpoint_proxy_events *events,
				void *data)
{
	pw_proxy_add_proxy_listener((struct pw_proxy*)p, listener, events, data);
}

#define pw_client_endpoint_resource_set_param(r,...)	\
	pw_resource_notify(r,struct pw_client_endpoint_proxy_events,set_param,__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PIPEWIRE_EXT_CLIENT_ENDPOINT_H */
