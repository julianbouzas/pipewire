/* PipeWire
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __PIPEWIRE_AUDIO_DSP_H__
#define __PIPEWIRE_AUDIO_DSP_H__

#include <pipewire/core.h>
#include <pipewire/node.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pw_node *
pw_audio_dsp_new(struct pw_core *core,
		 const struct pw_properties *properties,
		 enum pw_direction direction,
		 uint32_t channels,
		 uint64_t channelmask,
		 uint32_t sample_rate,
		 uint32_t max_buffer_size,
		 size_t user_data_size);

void *pw_audio_dsp_get_user_data(struct pw_node *node);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_AUDIO_DSP_H__ */
