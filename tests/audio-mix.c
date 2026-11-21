#include <obs-module.h>
#include <util/platform.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdatomic.h>

static atomic_bool enumeration_requested, audio_enumeration_entered;
static void observed_enumeration(bool (*callback)(void *, obs_source_t *), void *param)
{
	if (atomic_load(&enumeration_requested))
		atomic_store(&audio_enumeration_entered, true);
	obs_enum_all_sources(callback, param);
}
#ifndef MT2S_TEST_SOURCE
#define MT2S_TEST_SOURCE "../src/plugin-main.c"
#endif
#ifdef MT2S_CHECK_MUTEX
#include <pthread.h>
#include <errno.h>
static int checked_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes)
{
	if (attributes)
		return pthread_mutex_init(mutex, attributes);
	pthread_mutexattr_t checked;
	assert(pthread_mutexattr_init(&checked) == 0);
	assert(pthread_mutexattr_settype(&checked, PTHREAD_MUTEX_ERRORCHECK) == 0);
	int result = pthread_mutex_init(mutex, &checked);
	assert(pthread_mutexattr_destroy(&checked) == 0);
	return result;
}
static int checked_mutex_lock(pthread_mutex_t *mutex)
{
	int result = pthread_mutex_lock(mutex);
	assert(result == 0);
	return result;
}
static int checked_mutex_unlock(pthread_mutex_t *mutex)
{
	int result = pthread_mutex_unlock(mutex);
	if (result != 0)
		fprintf(stderr, "MUTEX unlock error=%d (EPERM=%d)\n", result, EPERM);
	assert(result == 0);
	return result;
}
#define pthread_mutex_init checked_mutex_init
#define pthread_mutex_lock checked_mutex_lock
#define pthread_mutex_unlock checked_mutex_unlock
#endif
#define obs_enum_all_sources observed_enumeration
#include MT2S_TEST_SOURCE
#undef obs_enum_all_sources
#ifdef MT2S_CHECK_MUTEX
#undef pthread_mutex_init
#undef pthread_mutex_lock
#undef pthread_mutex_unlock
#endif

static bool opaque_changing;
static bool power_fade;

const char *PLUGIN_NAME = "mix-track-to-source";
const char *PLUGIN_DISPLAY_NAME = "Mix Track to Source";
const char *PLUGIN_VERSION = "test";
void obs_log(int level, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	blogva(level, format, args);
	va_end(args);
}

struct task {
	obs_task_t callback;
	void *data;
	struct task *next;
};
static pthread_mutex_t tasks_lock = PTHREAD_MUTEX_INITIALIZER;
static struct task *tasks, **tasks_tail = &tasks;
static void queue_ui(obs_task_t callback, void *data, bool wait)
{
	assert(!wait);
	struct task *task = calloc(1, sizeof(*task));
	task->callback = callback;
	task->data = data;
	pthread_mutex_lock(&tasks_lock);
	*tasks_tail = task;
	tasks_tail = &task->next;
	pthread_mutex_unlock(&tasks_lock);
}
static void drain_ui(void)
{
	for (unsigned count = 0; count < 10000; count++) {
		pthread_mutex_lock(&tasks_lock);
		struct task *task = tasks;
		if (task) {
			tasks = task->next;
			if (!tasks)
				tasks_tail = &tasks;
		}
		pthread_mutex_unlock(&tasks_lock);
		if (!task)
			return;
		task->callback(task->data);
		free(task);
	}
	assert(false);
}
static void settle(void)
{
	obs_wait_for_destroy_queue();
	drain_ui();
	obs_wait_for_destroy_queue();
	drain_ui();
}
static const char *get_test_name(void *data)
{
	(void)data;
	return "Audio regression input";
}
static void *create_input(obs_data_t *settings, obs_source_t *source)
{
	(void)settings;
	return source;
}
static void destroy_input(void *data)
{
	(void)data;
}
static struct obs_audio_data *filter_half(void *data, struct obs_audio_data *audio)
{
	(void)data;
	for (size_t ch = 0; ch < 2; ch++)
		for (uint32_t frame = 0; frame < audio->frames; frame++)
			((float *)audio->data[ch])[frame] *= 0.5f;
	return audio;
}
static float recorded[3][500000];
static size_t counts[3];
static uint64_t timestamps[3][500];
static size_t blocks[3];
static atomic_uint_fast64_t mixed[3];
static atomic_uint_fast64_t max_input_delivery[2];
static void arrival_notification(void *param, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	(void)source;
	(void)muted;
	uint64_t now = os_gettime_ns();
	const char *name = param;
	if (!strcmp(name, "input-a") || !strcmp(name, "input-b")) {
		size_t index = !strcmp(name, "input-b");
		uint_fast64_t delay = now > audio->timestamp ? now - audio->timestamp : 0;
		uint_fast64_t previous = atomic_load(&max_input_delivery[index]);
		while (delay > previous &&
		       !atomic_compare_exchange_weak(&max_input_delivery[index], &previous, delay)) {
		}
	}
	for (size_t frame = 0; frame < audio->frames; frame++) {
		float value = ((const float *)audio->data[0])[frame];
		if (fabsf(value) > 0.001f)
			printf("ARRIVAL source=%s timestamp=%" PRIu64 " frame=%zu wall_ns=%" PRIu64 " value=%.9g\n",
			       (const char *)param, audio->timestamp, frame, now, value);
	}
}
static void capture(void *param, size_t track, struct audio_data *audio)
{
	(void)param;
	uint64_t now = os_gettime_ns();
	for (size_t i = 0; i < audio->frames; i++)
		if (fabsf(((float *)audio->data[0])[i]) > 0.001f)
			printf("OUTPUT track=%zu timestamp=%" PRIu64 " frame=%zu wall_ns=%" PRIu64 " value=%.9g\n",
			       track, audio->timestamp, i, now, ((float *)audio->data[0])[i]);
	assert(track < 3 && counts[track] + audio->frames <= 500000 && blocks[track] < 500);
	assert(audio->frames == AUDIO_OUTPUT_FRAMES);
	assert(memcmp(audio->data[0], audio->data[1], audio->frames * sizeof(float)) == 0);
	timestamps[track][blocks[track]++] = audio->timestamp;
	memcpy(recorded[track] + counts[track], audio->data[0], audio->frames * sizeof(float));
	counts[track] += audio->frames;
	atomic_store(&mixed[track], audio->timestamp);
}
// Report whether every captured track has delivered a block that starts at or after timestamp.
static bool mixed_from(uint64_t timestamp)
{
	for (size_t track = 0; track < 3; track++)
		if (atomic_load(&mixed[track]) < timestamp)
			return false;
	return true;
}
static obs_source_t *route(const char *name, int input, uint32_t outputs)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "MixTrack", input);
	obs_source_t *source = obs_source_create(PLUGIN_NAME, name, settings, NULL);
	obs_data_release(settings);
	assert(source);
	obs_source_set_audio_mixers(source, outputs);
	return source;
}
static void set_track(obs_source_t *source, int track)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "MixTrack", track);
	obs_source_update(source, settings);
	obs_data_release(settings);
}
static bool update_during_enumeration(void *param, obs_source_t *source)
{
	if (source != param)
		return true;
	atomic_store(&enumeration_requested, true);
	uint64_t deadline = os_gettime_ns() + 200000000;
	while (!atomic_load(&audio_enumeration_entered) && os_gettime_ns() < deadline)
		os_sleep_ms(1);
	// The enumeration owns OBS's source-list lock while input switching waits for audio callbacks.
	uint32_t outputs = obs_source_get_audio_mixers(source);
	set_track(source, 1);
	set_track(source, 0);
	// Selecting track 2 as the input removes that output; explicitly select it again.
	obs_source_set_audio_mixers(source, outputs);
	assert(obs_source_get_audio_mixers(source) == outputs);
	atomic_store(&enumeration_requested, false);
	puts("ENUMERATION UPDATE PASS");
	return false;
}
static void feed(obs_source_t *source, uint32_t rate, uint64_t timestamp, float value)
{
	float samples[2][960] = {{0}};
	samples[0][0] = samples[1][0] = value;
	struct obs_source_audio audio = {.frames = rate / 100,
					 .samples_per_sec = rate,
					 .speakers = SPEAKERS_STEREO,
					 .format = AUDIO_FORMAT_FLOAT_PLANAR,
					 .timestamp = timestamp};
	audio.data[0] = (uint8_t *)samples[0];
	audio.data[1] = (uint8_t *)samples[1];
	obs_source_output_audio(source, &audio);
}
// Feed a silent block to both inputs and move the rest of the scenario 10 ms later.
static void hold(obs_source_t *a, obs_source_t *b, uint32_t rate, uint64_t *start, uint64_t *ts)
{
	feed(a, rate, *ts, 0.0f);
	feed(b, rate, *ts, 0.0f);
	*start += 10000000;
	*ts += 10000000;
	os_sleepto_ns_fast(*ts);
}
struct late_feeder {
	bool intermittent;
	obs_source_t *source;
	uint32_t rate;
	uint64_t start;
};
static void *feed_late_source(void *data)
{
	struct late_feeder *feeder = data;
	for (unsigned block = 0; block < 450; block++) {
		uint64_t ts = feeder->start + (uint64_t)block * 10000000;
		uint64_t delay = feeder->intermittent ? (block > 40 && block < 390 && block % 37 == 0 ? 12000000 : 0)
						      : 20000000;
		os_sleepto_ns_fast(ts + delay);
		float pulse = block >= 100 && block < 400 && block % 25 == 0 ? 0.3f : 0.0f;
		feed(feeder->source, feeder->rate, ts, pulse);
	}
	return NULL;
}
static bool remove_item(obs_scene_t *scene, obs_sceneitem_t *item, void *data)
{
	(void)scene;
	(void)data;
	obs_sceneitem_remove(item);
	return true;
}
static float fade_a(void *data, float t)
{
	(void)data;
	return power_fade ? cosf(t * 1.5707963267948966f) : 1.0f - t;
}
static float fade_b(void *data, float t)
{
	(void)data;
	return power_fade ? sinf(t * 1.5707963267948966f) : t;
}
static bool fade_audio(void *data, uint64_t *ts, struct obs_source_audio_mix *audio, uint32_t mixers, size_t channels,
		       size_t rate)
{
	bool result = obs_transition_audio_render(data, ts, audio, mixers, channels, rate, fade_a, fade_b);
	uint64_t now = os_gettime_ns();
	if (result)
		for (size_t i = 0; i < AUDIO_OUTPUT_FRAMES; i++)
			if (fabsf(audio->output[0].data[0][i]) > 0.001f)
				printf("NATIVE source=fade timestamp=%" PRIu64 " frame=%zu wall_ns=%" PRIu64
				       " value=%.9g\n",
				       *ts, i, now, audio->output[0].data[0][i]);
	return result;
}
static void fade_video(void *data, gs_effect_t *effect)
{
	(void)effect;
	obs_transition_video_render(data, NULL);
}
// A composite owns a gain that is not its public source-volume setting.
// Its audio_render output is not delivered to source-capture callbacks.
struct opaque_mix {
	obs_source_t *self;
	obs_source_t *child;
	size_t rendered_pulses;
	size_t captured_pulses;
	size_t input_capture_pulses;
};
static void *opaque_create(obs_data_t *settings, obs_source_t *self)
{
	(void)settings;
	struct opaque_mix *mix = bzalloc(sizeof(*mix));
	mix->self = self;
	mix->child = obs_get_source_by_name("input-a");
	assert(mix->child && obs_source_add_active_child(self, mix->child));
	return mix;
}
static void opaque_destroy(void *data)
{
	struct opaque_mix *mix = data;
	obs_source_remove_active_child(mix->self, mix->child);
	obs_source_release(mix->child);
	bfree(mix);
}
static void opaque_enum(void *data, obs_source_enum_proc_t callback, void *param)
{
	struct opaque_mix *mix = data;
	callback(mix->self, mix->child, param);
}
static bool opaque_audio(void *data, uint64_t *timestamp, struct obs_source_audio_mix *out, uint32_t mixers,
			 size_t channels, size_t rate)
{
	(void)rate;
	struct opaque_mix *mix = data;
	if (obs_source_audio_pending(mix->child))
		return false;
	*timestamp = obs_source_get_audio_timestamp(mix->child);
	if (!*timestamp)
		return false;
	struct obs_source_audio_mix input;
	obs_source_get_audio_mix(mix->child, &input);
	float gain = opaque_changing && mix->rendered_pulses >= 6 ? 0.75f : 0.25f;
	for (size_t track = 0; track < MAX_AUDIO_MIXES; track++) {
		if (!(mixers & (1u << track)))
			continue;
		for (size_t ch = 0; ch < channels; ch++)
			for (size_t frame = 0; frame < AUDIO_OUTPUT_FRAMES; frame++)
				out->output[track].data[ch][frame] = input.output[track].data[ch][frame] * gain;
	}
	uint64_t now = os_gettime_ns();
	for (size_t frame = 0; frame < AUDIO_OUTPUT_FRAMES; frame++)
		if (fabsf(out->output[0].data[0][frame]) > 0.001f) {
			mix->rendered_pulses++;
			printf("NATIVE source=opaque timestamp=%" PRIu64 " frame=%zu wall_ns=%" PRIu64 " value=%.9g\n",
			       *timestamp, frame, now, out->output[0].data[0][frame]);
		}
	return true;
}
static void opaque_capture(void *data, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	(void)muted;
	struct opaque_mix *mix = data;
	for (size_t frame = 0; frame < audio->frames; frame++)
		if (fabsf(((const float *)audio->data[0])[frame]) > 0.001f) {
			if (source == mix->child)
				mix->input_capture_pulses++;
			else
				mix->captured_pulses++;
		}
}
static void check_controls(obs_source_t *input, obs_source_t *router, obs_scene_t *scene, obs_scene_t *other)
{
	assert(obs_scene_find_source(scene, "route-a") && obs_scene_find_source(other, "route-a"));
	obs_source_set_audio_mixers(input, 3);
	obs_source_set_audio_mixers(router, 4);
	settle();
	obs_source_set_audio_mixers(router, 2);
	settle();
	assert(obs_source_get_audio_mixers(input) == 1);
	obs_data_t *saved = obs_save_source(router);
	obs_data_t *settings = obs_data_get_obj(saved, "settings");
	obs_data_array_t *records = obs_data_get_array(settings, MT2S_CLEARED_KEY);
	assert(records && obs_data_array_count(records) > 0);
	obs_data_t *record = obs_data_array_item(records, 0);
	assert(obs_data_get_int(record, "cleared") == 2);
	assert(strcmp(obs_data_get_string(record, "uuid"), obs_source_get_uuid(input)) == 0);
	obs_data_release(record);
	obs_data_array_release(records);
	obs_data_release(settings);
	obs_data_release(saved);
	obs_source_set_audio_mixers(router, 4);
	settle();
	assert(obs_source_get_audio_mixers(input) == 3);
	obs_source_set_audio_mixers(router, 2);
	settle();
	assert(obs_source_get_audio_mixers(input) == 1);
	obs_source_set_audio_mixers(input, 3);
	settle();
	obs_source_set_audio_mixers(router, 4);
	settle();
	obs_source_set_audio_mixers(router, 2);
	settle();
	assert(obs_source_get_audio_mixers(input) == 3);
	for (int track = 0; track < MAX_AUDIO_MIXES; track++) {
		set_track(router, track);
		settle();
		for (uint32_t mask = 0; mask < (1u << MAX_AUDIO_MIXES); mask++) {
			obs_source_set_audio_mixers(router, mask);
			drain_ui();
			assert(obs_source_get_audio_mixers(router) == (mask & ~(1u << track)));
		}
		printf("CONTROL track=%d masks=%u passed\n", track, 1u << MAX_AUDIO_MIXES);
	}
	set_track(router, 0);
	obs_source_set_audio_mixers(router, 2);
	settle();
	obs_source_set_audio_mixers(input, 1);
	settle();
	puts("CONTROL PASS spread, removal, restoration, user override, serialized claim, 384 output masks");
}
int main(int argc, char **argv)
{
	assert(argc == 6);
	const char *scenario = argv[1];
	opaque_changing = !strcmp(scenario, "opaque-changing");
	power_fade = !strcmp(scenario, "fade-power");
	bool hybrid_switch = !strcmp(scenario, "hybrid-switch") || !strcmp(scenario, "hybrid-edge");
	bool hybrid_edge = !strcmp(scenario, "hybrid-edge");
	bool monitor = !strncmp(scenario, "monitor-", 8);
	bool monitor_own = !strcmp(scenario, "monitor-own");
	bool monitor_switch = monitor && strcmp(scenario, "monitor-only") && !monitor_own;
	uint32_t rate = (uint32_t)atoi(argv[2]);
	const char *deps = argv[3];
	uint32_t buffer_ms = (uint32_t)atoi(argv[5]);
	assert(buffer_ms == 0 || buffer_ms == 64 || buffer_ms == 128);
	if (!buffer_ms && !strcmp(scenario, "late-source"))
		buffer_ms = 64;
	assert(rate == 44100 || rate == 48000 || rate == 96000);
	setvbuf(stdout, NULL, _IOLBF, 0);
	assert(obs_startup("en-US", "work/config", NULL));
	obs_set_ui_task_handler(queue_ui);
	char resources[4096], graphics[4096];
	snprintf(resources, sizeof(resources), "%s/Frameworks/libobs.framework/Resources", deps);
	snprintf(graphics, sizeof(graphics), "%s/Frameworks/libobs-opengl.dylib", deps);
	obs_add_data_path(resources);
	struct obs_audio_info ai = {.samples_per_sec = rate, .speakers = SPEAKERS_STEREO};
	if (buffer_ms) {
		// Use a declared buffer budget without relaxing waveform or timestamp checks.
		struct obs_audio_info2 buffered = {.samples_per_sec = rate,
						   .speakers = SPEAKERS_STEREO,
						   .max_buffering_ms = buffer_ms,
						   .fixed_buffering = true};
		assert(obs_reset_audio2(&buffered));
	} else {
		assert(obs_reset_audio(&ai));
	}
	struct obs_video_info vi = {.graphics_module = graphics,
				    .fps_num = 30,
				    .fps_den = 1,
				    .base_width = 32,
				    .base_height = 32,
				    .output_width = 32,
				    .output_height = 32,
				    .output_format = VIDEO_FORMAT_RGBA,
				    .colorspace = VIDEO_CS_709,
				    .range = VIDEO_RANGE_FULL,
				    .scale_type = OBS_SCALE_BILINEAR};
	assert(obs_reset_video(&vi) == OBS_VIDEO_SUCCESS);
	printf("RUN %s rate=%u libobs=%s buffer_ms=%u\n", scenario, rate, obs_get_version_string(), buffer_ms);
	struct obs_source_info input_info = {.id = "regression-input",
					     .type = OBS_SOURCE_TYPE_INPUT,
					     .output_flags = OBS_SOURCE_AUDIO,
					     .get_name = get_test_name,
					     .create = create_input,
					     .destroy = destroy_input};
	obs_register_source(&input_info);
	struct obs_source_info filter_info = {.id = "regression-half",
					      .type = OBS_SOURCE_TYPE_FILTER,
					      .output_flags = OBS_SOURCE_AUDIO,
					      .get_name = get_test_name,
					      .create = create_input,
					      .destroy = destroy_input,
					      .filter_audio = filter_half};
	obs_register_source(&filter_info);
	struct obs_source_info fade_info = {.id = "regression-fade",
					    .type = OBS_SOURCE_TYPE_TRANSITION,
					    .output_flags = OBS_SOURCE_COMPOSITE | OBS_SOURCE_VIDEO |
							    OBS_SOURCE_CUSTOM_DRAW,
					    .get_name = get_test_name,
					    .create = create_input,
					    .destroy = destroy_input,
					    .audio_render = fade_audio,
					    .video_render = fade_video};
	obs_register_source(&fade_info);
	struct obs_source_info opaque_info = {.id = "regression-opaque",
					      .type = OBS_SOURCE_TYPE_INPUT,
					      .output_flags = OBS_SOURCE_COMPOSITE,
					      .get_name = get_test_name,
					      .create = opaque_create,
					      .destroy = opaque_destroy,
					      .enum_active_sources = opaque_enum,
					      .audio_render = opaque_audio};
	obs_register_source(&opaque_info);
	// An unavailable device prevents the monitoring backend from starting playback.
	if (monitor)
		assert(obs_set_audio_monitoring_device("Regression unavailable sink", "mt2s-regression-no-device"));
	obs_module_load();
	obs_source_t *a = obs_source_create("regression-input", "input-a", NULL, NULL);
	obs_source_t *b = obs_source_create("regression-input", "input-b", NULL, NULL);
	obs_source_set_audio_mixers(a, 1);
	obs_source_set_audio_mixers(b, !strcmp(scenario, "input-switch") ? 2 : 1);
	obs_scene_t *scene = obs_scene_create("scene-a"), *other = obs_scene_create("scene-b");
	bool cycle_three = !strcmp(scenario, "cycle-three") || !strcmp(scenario, "monitor-three");
	bool cycle_branch = !strcmp(scenario, "cycle-branch") || !strcmp(scenario, "monitor-branch");
	obs_source_t *first = route("route-a", 0, cycle_branch ? 6 : 2), *second = NULL, *third = NULL;
	if (!strcmp(scenario, "chain") || !strcmp(scenario, "cycle") || !strcmp(scenario, "cycle-live") ||
	    cycle_three || cycle_branch || monitor)
		second = route("route-b", 1,
			       (!strcmp(scenario, "cycle") || cycle_branch || (monitor && !cycle_three)) ? 1 : 4);
	if (cycle_three)
		third = route("route-c", 2, 1);
	obs_source_t *monitored = monitor_own ? first : cycle_three ? third : second;
	if (monitor)
		obs_source_set_monitoring_type(monitored, monitor_switch ? OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT
									 : OBS_MONITORING_TYPE_MONITOR_ONLY);
	if (!strcmp(scenario, "parallel"))
		second = route("route-b", 0, 4);
	if (!strcmp(scenario, "input-switch"))
		obs_source_set_audio_mixers(first, 4);
	if (!strcmp(scenario, "reverse"))
		obs_scene_add(scene, first);
	obs_source_t *opaque = NULL;
	if (!strncmp(scenario, "opaque", 6) || hybrid_switch) {
		opaque = obs_source_create("regression-opaque", "opaque", NULL, NULL);
		assert(opaque);
		obs_source_add_audio_capture_callback(opaque, opaque_capture, obs_obj_get_data(opaque));
		obs_source_add_audio_capture_callback(a, opaque_capture, obs_obj_get_data(opaque));
	}
	obs_sceneitem_t *item = obs_scene_add(scene, opaque && !hybrid_switch ? opaque : a);
	obs_sceneitem_t *opaque_item = NULL;
	if (hybrid_switch) {
		opaque_item = obs_scene_add(scene, opaque);
		obs_sceneitem_set_visible(opaque_item, false);
	}
	bool multi = !strcmp(scenario, "sync-offset") || !strcmp(scenario, "stalled-source") ||
		     !strcmp(scenario, "late-source") || (!strcmp(scenario, "multi") || !strcmp(scenario, "jitter")) ||
		     !strcmp(scenario, "offset") || !strcmp(scenario, "input-switch");
	if (multi)
		obs_scene_add(scene, b);
	if (strcmp(scenario, "reverse"))
		obs_scene_add(scene, first);
	if (second)
		obs_scene_add(scene, second);
	if (third)
		obs_scene_add(scene, third);
	obs_scene_add(other, b);
	obs_scene_t *nested = NULL;
	if (!strcmp(scenario, "shared-leaf")) {
		nested = obs_scene_create_private("nested");
		obs_scene_add(nested, a);
		obs_source_set_volume(obs_scene_get_source(nested), 0.5f);
		obs_scene_add(scene, obs_scene_get_source(nested));
	}
	if (!strcmp(scenario, "sync-offset")) {
		obs_source_set_sync_offset(b, 5000000);
		assert(obs_source_get_sync_offset(b) == 5000000);
	}
	settle();
	if (!strcmp(scenario, "controls"))
		check_controls(a, first, scene, other);
	if (!strcmp(scenario, "filter")) {
		obs_source_set_volume(a, 0.4f);
		obs_source_t *filter = obs_source_create("regression-half", "half", NULL, NULL);
		obs_source_filter_add(first, filter);
		obs_source_release(filter);
	}
	if (!strcmp(scenario, "scene-volume"))
		obs_source_set_volume(obs_scene_get_source(scene), 0.5f);
	obs_source_t *transition = NULL;
	if ((!strcmp(scenario, "fade") || power_fade)) {
		transition = obs_source_create_private("regression-fade", "fade", NULL);
		assert(transition);
		obs_transition_set(transition, obs_scene_get_source(scene));
	}
	obs_set_output_source(0, transition ? transition : obs_scene_get_source(scene));
	if (!strcmp(scenario, "global") || !strcmp(scenario, "global-first")) {
		obs_sceneitem_remove(item);
		item = NULL;
		if (!strcmp(scenario, "global-first")) {
			obs_set_output_source(0, a);
			obs_set_output_source(1, obs_scene_get_source(scene));
		} else
			obs_set_output_source(1, a);
	}
	for (size_t track = 0; track < 3; track++)
		assert(audio_output_connect(obs_get_audio(), track, NULL, capture, NULL));
	obs_source_add_audio_capture_callback(a, arrival_notification, "input-a");
	obs_source_add_audio_capture_callback(b, arrival_notification, "input-b");
	obs_source_add_audio_capture_callback(first, arrival_notification, "route-a");
	if (second)
		obs_source_add_audio_capture_callback(second, arrival_notification, "route-b");
	if (third)
		obs_source_add_audio_capture_callback(third, arrival_notification, "route-c");
	uint64_t start = os_gettime_ns();
	pthread_t late_thread;
	struct late_feeder late = {.source = b,
				   .rate = rate,
				   .start = start,
				   .intermittent = !strcmp(scenario, "stalled-source")};
	bool late_source = !strcmp(scenario, "late-source") || !strcmp(scenario, "stalled-source");
	if (late_source)
		assert(pthread_create(&late_thread, NULL, feed_late_source, &late) == 0);
	bool registry_locked = false;
	// Before a change, silent input blocks hold the scenario until every captured track has mixed
	// the audio up to 100 ms before the change block. After the change, silent blocks continue for
	// 100 ms, except in hybrid-edge, whose change coincides with its block's pulse. The input
	// timestamps stay continuous. Only these scenarios make changes.
	unsigned change_blocks[2] = {hybrid_edge ? 225u : 220u, hybrid_edge ? 325u : 320u};
	bool second_change = hybrid_switch || monitor_switch || !strcmp(scenario, "registry-busy") ||
			     !strcmp(scenario, "cycle-live") || !strcmp(scenario, "hide") || !strcmp(scenario, "mute");
	bool first_change = second_change || !strcmp(scenario, "fade") || power_fade || !strcmp(scenario, "switch") ||
			    !strcmp(scenario, "input-switch") || !strcmp(scenario, "cycle") || cycle_three ||
			    cycle_branch;
	// The late feeder keeps its own start timestamp.
	assert(!late_source || !first_change);
	for (unsigned block = 0; block < 450; block++) {
		bool change = (block == change_blocks[0] && first_change) ||
			      (block == change_blocks[1] && second_change);
		uint64_t ts = start + (uint64_t)block * 10000000;
		os_sleepto_ns_fast(ts);
		if (change) {
			uint64_t before = ts - 100000000, deadline = os_gettime_ns() + 1000000000;
			while (!mixed_from(before)) {
				assert(os_gettime_ns() < deadline);
				hold(a, b, rate, &start, &ts);
			}
		}
		if (!registry_locked)
			drain_ui();
		if (block == 50 && !strcmp(scenario, "enumeration-update")) {
			obs_enum_sources(update_during_enumeration, first);
			ts = os_gettime_ns();
			start = ts - (uint64_t)block * 10000000;
		}
		if (change && hybrid_switch) {
			bool custom = block == change_blocks[0];
			obs_sceneitem_set_visible(item, !custom);
			obs_sceneitem_set_visible(opaque_item, custom);
			printf("PATH_SWITCH native=%d block=%u wall_ns=%" PRIu64 "\n", custom, block, os_gettime_ns());
		}
		if (change && monitor_switch) {
			obs_source_set_monitoring_type(monitored, block == 220
									  ? OBS_MONITORING_TYPE_MONITOR_ONLY
									  : OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
			assert(obs_source_get_monitoring_type(monitored) ==
			       (block == 220 ? OBS_MONITORING_TYPE_MONITOR_ONLY
					     : OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT));
		}
		if (change && block == 220) {
			if (!strcmp(scenario, "registry-busy")) {
				assert(pthread_mutex_lock(&mt2s_instances_lock) == 0);
				registry_locked = true;
			}
			if ((!strcmp(scenario, "fade") || power_fade))
				assert(obs_transition_start(transition, OBS_TRANSITION_MODE_AUTO, 1000,
							    obs_scene_get_source(other)));
			if (!strcmp(scenario, "switch"))
				obs_set_output_source(0, obs_scene_get_source(other));
			if (!strcmp(scenario, "hide"))
				obs_sceneitem_set_visible(item, false);
			if (!strcmp(scenario, "mute"))
				obs_source_set_muted(a, true);
			if (!strcmp(scenario, "input-switch"))
				set_track(first, 1);
			if (!strcmp(scenario, "cycle"))
				obs_source_set_audio_mixers(second, 4);
			if (cycle_three && !monitor)
				obs_source_set_audio_mixers(third, 8);
			if (cycle_branch && !monitor)
				obs_source_set_audio_mixers(second, 8);
			if (!strcmp(scenario, "cycle-live"))
				obs_source_set_audio_mixers(second, 1);
		}
		if (change && block == 320) {
			if (registry_locked) {
				assert(pthread_mutex_unlock(&mt2s_instances_lock) == 0);
				registry_locked = false;
			}
			if (!strcmp(scenario, "cycle-live"))
				obs_source_set_audio_mixers(second, 4);
			if (!strcmp(scenario, "hide"))
				obs_sceneitem_set_visible(item, true);
			if (!strcmp(scenario, "mute"))
				obs_source_set_muted(a, false);
		}
		if (change) {
			uint64_t changed = os_gettime_ns();
			while (!hybrid_edge && ts < changed + 100000000)
				hold(a, b, rate, &start, &ts);
			printf("CHANGE block=%u wall_ns=%" PRIu64 " input_ns=%" PRIu64 "\n", block, changed, ts);
		}
		float pulse = block >= 100 && block < 400 && block % 25 == 0 ? 1.0f : 0.0f;
		feed(a, rate, ts, 0.2f * pulse);
		if (!strcmp(scenario, "jitter") && block > 40 && block < 390 && block % 37 == 0)
			os_sleep_ms(12);
		if (!late_source)
			feed(b, rate, ts + (!strcmp(scenario, "offset") ? 5000000 : 0), 0.3f * pulse);
	}
	if (late_source)
		assert(pthread_join(late_thread, NULL) == 0);
	os_sleep_ms(200);
	obs_source_remove_audio_capture_callback(a, arrival_notification, "input-a");
	obs_source_remove_audio_capture_callback(b, arrival_notification, "input-b");
	printf("INPUT_DELIVERY_MAX source=input-a delay_ns=%" PRIuFAST64 "\n", atomic_load(&max_input_delivery[0]));
	printf("INPUT_DELIVERY_MAX source=input-b delay_ns=%" PRIuFAST64 "\n", atomic_load(&max_input_delivery[1]));
	obs_source_remove_audio_capture_callback(first, arrival_notification, "route-a");
	if (second)
		obs_source_remove_audio_capture_callback(second, arrival_notification, "route-b");
	if (third)
		obs_source_remove_audio_capture_callback(third, arrival_notification, "route-c");
	for (size_t track = 0; track < 3; track++) {
		audio_output_disconnect(obs_get_audio(), track, capture, NULL);
		char filename[4096];
		snprintf(filename, sizeof(filename), "%s/%s-%u-%zu.f32", argv[4], scenario, rate, track);
		FILE *f = fopen(filename, "wb");
		assert(f);
		assert(fwrite(recorded[track], sizeof(float), counts[track], f) == counts[track]);
		fclose(f);
		snprintf(filename, sizeof(filename), "%s/%s-%u-%zu.timestamps", argv[4], scenario, rate, track);
		f = fopen(filename, "w");
		assert(f);
		for (size_t i = 0; i < blocks[track]; i++)
			fprintf(f, "%" PRIu64 "\n", timestamps[track][i]);
		fclose(f);
		unsigned peaks = 0;
		float sum = 0;
		for (size_t i = 0; i < counts[track]; i++)
			if (fabsf(recorded[track][i]) > 0.001f) {
				peaks++;
				sum += recorded[track][i];
			}
		printf("AUDIO track=%zu frames=%zu peaks=%u sum=%.6f\n", track, counts[track], peaks, sum);
	}
	if (opaque) {
		struct opaque_mix *mix = obs_obj_get_data(opaque);
		obs_source_remove_audio_capture_callback(opaque, opaque_capture, mix);
		obs_source_remove_audio_capture_callback(a, opaque_capture, mix);
		printf("OPAQUE volume=%.6f rendered_pulses=%zu capture_pulses=%zu input_capture_pulses=%zu\n",
		       obs_source_get_volume(opaque), mix->rendered_pulses, mix->captured_pulses,
		       mix->input_capture_pulses);
		assert(obs_source_get_volume(opaque) == 1.0f);
		assert((hybrid_switch || mix->rendered_pulses == 12) && mix->captured_pulses == 0 &&
		       mix->input_capture_pulses == 12);
	}
	obs_set_output_source(0, NULL);
	obs_set_output_source(1, NULL);
	if (transition) {
		obs_transition_clear(transition);
		obs_source_release(transition);
	}
	obs_scene_enum_items(scene, remove_item, NULL);
	obs_scene_enum_items(other, remove_item, NULL);
	if (nested) {
		obs_scene_enum_items(nested, remove_item, NULL);
		obs_scene_release(nested);
	}
	obs_source_remove(obs_scene_get_source(scene));
	obs_source_remove(obs_scene_get_source(other));
	obs_scene_release(scene);
	obs_scene_release(other);
	obs_source_release(first);
	if (second)
		obs_source_release(second);
	if (third)
		obs_source_release(third);
	settle();
	if (opaque)
		obs_source_release(opaque);
	obs_source_release(a);
	obs_source_release(b);
	settle();
	obs_module_unload();
	obs_shutdown();
	puts("DONE");
	return 0;
}
