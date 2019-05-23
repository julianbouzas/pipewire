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

#ifndef PIPEWIRE_EXT_ENDPOINT_H
#define PIPEWIRE_EXT_ENDPOINT_H

#include <spa/utils/defs.h>
#include <spa/utils/type-info.h>
#include <pipewire/proxy.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pw_endpoint_proxy;

#define PW_VERSION_ENDPOINT			0
#define PW_EXTENSION_MODULE_ENDPOINT		PIPEWIRE_MODULE_PREFIX "module-endpoint"

/* extending enum spa_param_type */
enum endpoint_param_type {
	PW_ENDPOINT_PARAM_EnumControl = 0x1000,
	PW_ENDPOINT_PARAM_Control,
	PW_ENDPOINT_PARAM_EnumStream,
};

enum endpoint_param_object_type {
	PW_ENDPOINT_OBJECT_ParamControl = PW_TYPE_FIRST + SPA_TYPE_OBJECT_START + 0x1001,
	PW_ENDPOINT_OBJECT_ParamStream,
};

/** properties for PW_ENDPOINT_OBJECT_ParamControl */
enum endpoint_param_control {
	PW_ENDPOINT_PARAM_CONTROL_START,	/**< object id, one of enum endpoint_param_type */
	PW_ENDPOINT_PARAM_CONTROL_id,		/**< control id (Int) */
	PW_ENDPOINT_PARAM_CONTROL_stream_id,	/**< stream id (Int) */
	PW_ENDPOINT_PARAM_CONTROL_name,		/**< control name (String) */
	PW_ENDPOINT_PARAM_CONTROL_type,		/**< control type (Range) */
	PW_ENDPOINT_PARAM_CONTROL_value,	/**< control value */
};

/** properties for PW_ENDPOINT_OBJECT_ParamStream */
enum endpoint_param_stream {
	PW_ENDPOINT_PARAM_STREAM_START,		/**< object id, one of enum endpoint_param_type */
	PW_ENDPOINT_PARAM_STREAM_id,		/**< stream id (Int) */
	PW_ENDPOINT_PARAM_STREAM_name,		/**< stream name (String) */
};

static const struct spa_type_info endpoint_param_type_info[] = {
	{ PW_ENDPOINT_PARAM_EnumControl, SPA_TYPE_Int, SPA_TYPE_INFO_PARAM_ID_BASE "EnumControl", NULL },
	{ PW_ENDPOINT_PARAM_Control, SPA_TYPE_Int, SPA_TYPE_INFO_PARAM_ID_BASE "Control", NULL },
	{ PW_ENDPOINT_PARAM_EnumStream, SPA_TYPE_Int, SPA_TYPE_INFO_PARAM_ID_BASE "EnumStream", NULL },
	{ 0, 0, NULL, NULL },
};

#define PW_ENDPOINT_TYPE_INFO_ParamControl		SPA_TYPE_INFO_PARAM_BASE "ParamControl"
#define PW_ENDPOINT_TYPE_INFO_PARAM_CONTROL_BASE	PW_ENDPOINT_TYPE_INFO_ParamControl ":"

static const struct spa_type_info endpoint_param_control_info[] = {
	{ PW_ENDPOINT_PARAM_CONTROL_START, SPA_TYPE_Id, PW_ENDPOINT_TYPE_INFO_PARAM_CONTROL_BASE, spa_type_param, },
	{ PW_ENDPOINT_PARAM_CONTROL_id, SPA_TYPE_Int, PW_ENDPOINT_TYPE_INFO_PARAM_CONTROL_BASE "id", NULL },
	{ PW_ENDPOINT_PARAM_CONTROL_stream_id, SPA_TYPE_Int, PW_ENDPOINT_TYPE_INFO_PARAM_CONTROL_BASE "streamId", NULL },
	{ PW_ENDPOINT_PARAM_CONTROL_name, SPA_TYPE_String, PW_ENDPOINT_TYPE_INFO_PARAM_CONTROL_BASE "name", NULL },
	{ PW_ENDPOINT_PARAM_CONTROL_type, SPA_TYPE_Pod, PW_ENDPOINT_TYPE_INFO_PARAM_CONTROL_BASE "type", NULL },
	{ PW_ENDPOINT_PARAM_CONTROL_value, SPA_TYPE_Struct, PW_ENDPOINT_TYPE_INFO_PARAM_CONTROL_BASE "value", NULL },
	{ 0, 0, NULL, NULL },
};

#define PW_ENDPOINT_TYPE_INFO_ParamStream		SPA_TYPE_INFO_PARAM_BASE "ParamStream"
#define PW_ENDPOINT_TYPE_INFO_PARAM_STREAM_BASE		PW_ENDPOINT_TYPE_INFO_ParamStream ":"

static const struct spa_type_info endpoint_param_stream_info[] = {
	{ PW_ENDPOINT_PARAM_STREAM_START, SPA_TYPE_Id, PW_ENDPOINT_TYPE_INFO_PARAM_STREAM_BASE, spa_type_param, },
	{ PW_ENDPOINT_PARAM_STREAM_id, SPA_TYPE_Int, PW_ENDPOINT_TYPE_INFO_PARAM_STREAM_BASE "id", NULL },
	{ PW_ENDPOINT_PARAM_STREAM_name, SPA_TYPE_String, PW_ENDPOINT_TYPE_INFO_PARAM_STREAM_BASE "name", NULL },
	{ 0, 0, NULL, NULL },
};

static const struct spa_type_info endpoint_param_object_type_info[] = {
	{ PW_ENDPOINT_OBJECT_ParamControl, SPA_TYPE_Object, SPA_TYPE_INFO_OBJECT_BASE "ParamControl", endpoint_param_control_info, },
	{ PW_ENDPOINT_OBJECT_ParamStream, SPA_TYPE_Object, SPA_TYPE_INFO_OBJECT_BASE "ParamStream", endpoint_param_stream_info },
	{ 0, 0, NULL, NULL },
};

struct pw_endpoint_info {
	uint32_t id;				/**< id of the global */
#define PW_ENDPOINT_CHANGE_MASK_PARAMS		(1 << 0)
#define PW_ENDPOINT_CHANGE_MASK_PROPS		(1 << 1)
	uint32_t change_mask;			/**< bitfield of changed fields since last call */
	uint32_t n_params;			/**< number of items in \a params */
	struct spa_param_info *params;		/**< parameters */
	struct spa_dict *props;			/**< extra properties */
};

#define PW_ENDPOINT_PROXY_METHOD_SUBSCRIBE_PARAMS	0
#define PW_ENDPOINT_PROXY_METHOD_ENUM_PARAMS		1
#define PW_ENDPOINT_PROXY_METHOD_SET_PARAM		2
#define PW_ENDPOINT_PROXY_METHOD_NUM			3

struct pw_endpoint_proxy_methods {
#define PW_VERSION_ENDPOINT_PROXY_METHODS		0
	uint32_t version;

	/**
	 * Subscribe to parameter changes
	 *
	 * Automatically emit param events for the given ids when
	 * they are changed.
	 *
	 * \param ids an array of param ids
	 * \param n_ids the number of ids in \a ids
	 */
	int (*subscribe_params) (void *object, uint32_t *ids, uint32_t n_ids);

	/**
	 * Enumerate endpoint parameters
	 *
	 * Start enumeration of endpoint parameters. For each param, a
	 * param event will be emited.
	 *
	 * \param seq a sequence number to place in the reply
	 * \param id the parameter id to enum or SPA_ID_INVALID for all
	 * \param start the start index or 0 for the first param
	 * \param num the maximum number of params to retrieve
	 * \param filter a param filter or NULL
	 */
	int (*enum_params) (void *object, int seq,
			    uint32_t id, uint32_t start, uint32_t num,
			    const struct spa_pod *filter);

	/**
	 * Set a parameter on the endpoint
	 *
	 * \param id the parameter id to set
	 * \param flags extra parameter flags
	 * \param param the parameter to set
	 */
	int (*set_param) (void *object, uint32_t id, uint32_t flags,
			  const struct spa_pod *param);
};

static inline int
pw_endpoint_proxy_subscribe_params(struct pw_endpoint_proxy *p, uint32_t *ids, uint32_t n_ids)
{
	return pw_proxy_do((struct pw_proxy*)p, struct pw_endpoint_proxy_methods,
			   subscribe_params, ids, n_ids);
}

static inline int
pw_endpoint_proxy_enum_params(struct pw_endpoint_proxy *p, int seq,
				uint32_t id, uint32_t start, uint32_t num,
				const struct spa_pod *filter)
{
	return pw_proxy_do((struct pw_proxy*)p, struct pw_endpoint_proxy_methods,
			    enum_params, seq, id, start, num, filter);
}

static inline int
pw_endpoint_proxy_set_param(struct pw_endpoint_proxy *p, uint32_t id,
			    uint32_t flags, const struct spa_pod *param)
{
	return pw_proxy_do((struct pw_proxy*)p, struct pw_endpoint_proxy_methods,
			   set_param, id, flags, param);
}

#define PW_ENDPOINT_PROXY_EVENT_INFO		0
#define PW_ENDPOINT_PROXY_EVENT_PARAM		1
#define PW_ENDPOINT_PROXY_EVENT_NUM		2

struct pw_endpoint_proxy_events {
#define PW_VERSION_ENDPOINT_PROXY_EVENTS	0
	uint32_t version;

	/**
	 * Notify endpoint info
	 *
	 * \param info info about the endpoint
	 */
	void (*info) (void *object, const struct pw_endpoint_info * info);

	/**
	 * Notify an endpoint param
	 *
	 * Event emited as a result of the enum_params method.
	 *
	 * \param seq the sequence number of the request
	 * \param id the param id
	 * \param index the param index
	 * \param next the param index of the next param
	 * \param param the parameter
	 */
	void (*param) (void *object, int seq, uint32_t id,
		       uint32_t index, uint32_t next,
		       const struct spa_pod *param);
};

static inline void
pw_endpoint_proxy_add_listener(struct pw_endpoint_proxy *p,
				struct spa_hook *listener,
				const struct pw_endpoint_proxy_events *events,
				void *data)
{
	pw_proxy_add_proxy_listener((struct pw_proxy*)p, listener, events, data);
}

#define pw_endpoint_resource_info(r,...)	\
	pw_resource_notify(r,struct pw_endpoint_proxy_events,info,__VA_ARGS__)
#define pw_endpoint_resource_param(r,...)	\
	pw_resource_notify(r,struct pw_endpoint_proxy_events,param,__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PIPEWIRE_EXT_ENDPOINT_H */
