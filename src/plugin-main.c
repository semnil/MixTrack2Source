/*
Mix Track to Source
Copyright (C) 2026 semnil info@semnil.com

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <stdatomic.h>
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

typedef struct {
	obs_source_t *source;
	atomic_size_t mix_idx; // audio thread (read) / UI thread (read-write)
} mt2s_context_t;

void mt2s_avoid_loopback(mt2s_context_t *context, size_t mix_idx)
{
	// override output settings to avoid loopback
	size_t mixers = obs_source_get_audio_mixers(context->source);
	if (mixers != (mixers & ~((size_t)1 << mix_idx)))
		obs_source_set_audio_mixers(context->source, mixers & ~((size_t)1 << mix_idx));
}

static void mt2s_callback(void *param, size_t mix_idx, struct audio_data *data)
{
	mt2s_context_t *context = (mt2s_context_t *)param;
	if (!context || atomic_load(&context->mix_idx) != mix_idx || !data || !data->frames)
		return;

	mt2s_avoid_loopback(context, mix_idx);

	struct obs_audio_info oai;
	if (obs_get_audio_info(&oai)) {
		// bridge data from the mix track to the audio source
		struct obs_source_audio output_audio = {.frames = data->frames,
							.speakers = oai.speakers,
							.format = AUDIO_FORMAT_FLOAT_PLANAR,
							.samples_per_sec = oai.samples_per_sec,
							.timestamp = data->timestamp};

		for (int i = 0; i < MAX_AV_PLANES; i++) {
			output_audio.data[i] = data->data[i];
		}

		obs_source_output_audio(context->source, &output_audio);
	}
}

void mt2s_disconnect(mt2s_context_t *context)
{
	if (!context)
		return;

	size_t idx = atomic_load(&context->mix_idx);
	if (idx >= MAX_AUDIO_MIXES)
		return;

	// disconnect the connected mix track
	audio_output_disconnect(obs_get_audio(), idx, mt2s_callback, context);
	obs_log(LOG_INFO, "audio_output_disconnect(%zu)", idx);
	// initialize the connected mix track id
	atomic_store(&context->mix_idx, SIZE_MAX);
}

void mt2s_connect(mt2s_context_t *context, size_t mix_idx)
{
	if (!context || mix_idx >= MAX_AUDIO_MIXES)
		return;

	audio_t *audio = obs_get_audio();

	if (atomic_load(&context->mix_idx) == mix_idx)
		return;

	// disconnect the connected mix track
	mt2s_disconnect(context);

	// connect the mix track
	if (audio_output_connect(audio, mix_idx, NULL, mt2s_callback, context)) {
		// save the connected mix track id
		atomic_store(&context->mix_idx, mix_idx);
		obs_log(LOG_INFO, "audio_output_connect(%zu)", mix_idx);
	}
}

// lifecycle
void *mt2s_create(obs_data_t *settings, obs_source_t *source)
{
	mt2s_context_t *context = malloc(sizeof(mt2s_context_t));

	if (context) {
		context->source = source;
		atomic_init(&context->mix_idx, SIZE_MAX);

		// deselect all outputs
		obs_source_set_audio_mixers(context->source, 0);

		mt2s_connect(context, obs_data_get_int(settings, "MixTrack"));
	}

	return context;
}

void mt2s_destroy(void *data)
{
	if (!data)
		return;

	mt2s_context_t *context = (mt2s_context_t *)data;
	mt2s_disconnect(context);
	free(context);
}

void mt2s_update(void *data, obs_data_t *settings)
{
	if (!data || !settings)
		return;

	mt2s_context_t *context = (mt2s_context_t *)data;
	mt2s_connect(context, obs_data_get_int(settings, "MixTrack"));
}

// configs
const char *obs_module_name(void)
{
	return "Mix Track to Source";
}

const char *obs_module_description(void)
{
	return "Routing Mix Track to Source";
}

static const char *get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);

	return obs_module_text("MixTrack");
}

static void get_defaults(obs_data_t *settings)
{
	if (!settings)
		return;

	obs_data_set_default_int(settings, "MixTrack", 0);
}

static obs_properties_t *get_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_property_t *list = obs_properties_add_list(props, "MixTrack", obs_module_text("Input"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(list, obs_module_text("Track1"), 0);
	obs_property_list_add_int(list, obs_module_text("Track2"), 1);
	obs_property_list_add_int(list, obs_module_text("Track3"), 2);
	obs_property_list_add_int(list, obs_module_text("Track4"), 3);
	obs_property_list_add_int(list, obs_module_text("Track5"), 4);
	obs_property_list_add_int(list, obs_module_text("Track6"), 5);

	return props;
}

// module
bool obs_module_load(void)
{
	struct obs_source_info info = {.id = PLUGIN_NAME,
				       .type = OBS_SOURCE_TYPE_INPUT,
				       .output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
				       .icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT,
				       .get_name = get_name,
				       .create = mt2s_create,
				       .destroy = mt2s_destroy,
				       .get_defaults = get_defaults,
				       .get_properties = get_properties,
				       .update = mt2s_update};
	obs_register_source(&info);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
