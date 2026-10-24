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
#include <plugin-support.h>
#include <util/threading.h>
#include <util/bmem.h>
#include <string.h>

#ifndef _MSC_VER
#include <stdatomic.h>
#else
// MSVC does not support stdatomic.h in C mode under the OBS build config
// Use the `volatile` keyword to prevent the compiler from reordering instructions
typedef volatile size_t atomic_size_t;
static inline void _mt2s_atomic_init(volatile size_t *p, size_t v)
{
	*p = v;
}
static inline size_t _mt2s_atomic_load(volatile size_t *p)
{
	return *p;
}
static inline void _mt2s_atomic_store(volatile size_t *p, size_t v)
{
	*p = v;
}
#define atomic_init(p, v)  _mt2s_atomic_init(p, v)
#define atomic_load(p)     _mt2s_atomic_load(p)
#define atomic_store(p, v) _mt2s_atomic_store(p, v)
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

// settings key that persists the output bits this instance cleared from other sources
#define MT2S_CLEARED_KEY "mt2s_cleared"

// one entry per other source this instance cleared output bits from
typedef struct mt2s_record {
	char *uuid;
	uint32_t cleared;     // output bits cleared from the source and still owned by this instance
	uint32_t provisional; // subset of cleared held while a loaded source's routing has not settled
	uint32_t user_owned;  // output bits the user re-enabled; never cleared or restored again
	bool watched;         // the source's audio_mixers signal is connected
	bool pending;         // added at source_create and still awaiting that source's load-settle
	struct mt2s_record *next;
} mt2s_record_t;

typedef struct {
	obs_source_t *source;
	atomic_size_t mix_idx; // audio thread (read) / UI thread (read-write)
	pthread_mutex_t lock;  // guards records and out_mask
	mt2s_record_t *records;
	uint32_t out_mask; // output mixers used for the last sweep
} mt2s_context_t;

static void mt2s_on_source_mixers(void *param, calldata_t *cd);

void mt2s_avoid_loopback(mt2s_context_t *context, size_t mix_idx)
{
	// override output settings to avoid loopback
	size_t mixers = obs_source_get_audio_mixers(context->source);
	if (mixers != (mixers & ~((size_t)1 << mix_idx)))
		obs_source_set_audio_mixers(context->source, mixers & ~((size_t)1 << mix_idx));
}

// --- record list (caller holds context->lock) ---

static mt2s_record_t *mt2s_find_record(mt2s_context_t *context, const char *uuid)
{
	for (mt2s_record_t *r = context->records; r; r = r->next) {
		if (strcmp(r->uuid, uuid) == 0)
			return r;
	}
	return NULL;
}

static mt2s_record_t *mt2s_add_record(mt2s_context_t *context, const char *uuid)
{
	mt2s_record_t *r = bzalloc(sizeof(mt2s_record_t));
	r->uuid = bstrdup(uuid);
	r->next = context->records;
	context->records = r;
	return r;
}

// --- audio_mixers signal connection (caller must not hold context->lock) ---

static void mt2s_watch(mt2s_context_t *context, obs_source_t *x)
{
	signal_handler_t *sh = obs_source_get_signal_handler(x);
	if (sh)
		signal_handler_connect(sh, "audio_mixers", mt2s_on_source_mixers, context);
}

static void mt2s_unwatch(mt2s_context_t *context, obs_source_t *x)
{
	signal_handler_t *sh = obs_source_get_signal_handler(x);
	if (sh)
		signal_handler_disconnect(sh, "audio_mixers", mt2s_on_source_mixers, context);
}

// --- cross-instance ownership (caller must not hold context->lock) ---

// Module-wide claims on output bits cleared from a source, refcounted across instances so a
// bit is restored only once the last instance releases it. Each bit is held in one of two
// tiers: provisional while it comes from a source's not-yet-settled routing (or from joining a
// peer's provisional claim), confirmed once it comes from the settled routing. The tiers are
// counted apart so a transient provisional claim can be purged at load-settle without granting
// a confirmed restore right, while live new sources are still shared between instances.
struct mt2s_claim {
	char *uuid;
	uint32_t bit;
	int conf;
	int prov;
	struct mt2s_claim *next;
};

static pthread_mutex_t mt2s_claims_lock = PTHREAD_MUTEX_INITIALIZER;
static struct mt2s_claim *mt2s_claims;

static void mt2s_claim_acquire(const char *uuid, uint32_t bit, bool confirmed)
{
	pthread_mutex_lock(&mt2s_claims_lock);
	struct mt2s_claim *c = mt2s_claims;
	for (; c; c = c->next) {
		if (c->bit == bit && strcmp(c->uuid, uuid) == 0)
			break;
	}
	if (!c) {
		c = bzalloc(sizeof(struct mt2s_claim));
		c->uuid = bstrdup(uuid);
		c->bit = bit;
		c->next = mt2s_claims;
		mt2s_claims = c;
	}
	if (confirmed)
		c->conf++;
	else
		c->prov++;
	pthread_mutex_unlock(&mt2s_claims_lock);
}

static bool mt2s_claim_held(const char *uuid, uint32_t bit, bool confirmed)
{
	bool held = false;
	pthread_mutex_lock(&mt2s_claims_lock);
	for (struct mt2s_claim *c = mt2s_claims; c; c = c->next) {
		if (c->bit == bit && strcmp(c->uuid, uuid) == 0) {
			held = confirmed ? c->conf > 0 : c->prov > 0;
			break;
		}
	}
	pthread_mutex_unlock(&mt2s_claims_lock);
	return held;
}

// Drops one reference from the given tier; returns true when no holder remains in either
// tier afterwards (the bit may then be restored).
static bool mt2s_claim_release(const char *uuid, uint32_t bit, bool confirmed)
{
	bool empty = false;
	pthread_mutex_lock(&mt2s_claims_lock);
	struct mt2s_claim **pp = &mt2s_claims;
	while (*pp) {
		struct mt2s_claim *c = *pp;
		if (c->bit == bit && strcmp(c->uuid, uuid) == 0) {
			if (confirmed) {
				if (c->conf > 0)
					c->conf--;
			} else if (c->prov > 0) {
				c->prov--;
			}
			if (c->conf <= 0 && c->prov <= 0) {
				*pp = c->next;
				bfree(c->uuid);
				bfree(c);
				empty = true;
			}
			break;
		}
		pp = &c->next;
	}
	pthread_mutex_unlock(&mt2s_claims_lock);
	return empty;
}

// Clears the instance's output bits from one candidate source. Caller must not hold context->lock.
// from_create marks a source_create sweep (a loaded source's routing has not settled yet); its clears
// are held provisionally. join_prov lets a settled live sweep also join a peer's provisional claim,
// holding that bit provisionally too, so a source created after the plugins is shared; a source_load
// re-derivation keeps join_prov false so a loaded source's transient provisional claims are never
// captured as confirmed restore rights.
static void mt2s_sweep_source(mt2s_context_t *context, obs_source_t *x, uint32_t out_mask, size_t in_bit,
			      bool from_create, bool join_prov)
{
	if (x == context->source)
		return;
	if ((obs_source_get_output_flags(x) & OBS_SOURCE_AUDIO) == 0)
		return;

	const char *id = obs_source_get_id(x);
	if (id && strcmp(id, PLUGIN_NAME) == 0)
		return;

	const char *uuid = obs_source_get_uuid(x);
	if (!uuid || !*uuid)
		return;

	pthread_mutex_lock(&context->lock);
	mt2s_record_t *r = mt2s_find_record(context, uuid);
	bool exists = r != NULL;
	uint32_t user_owned = r ? r->user_owned : 0;
	pthread_mutex_unlock(&context->lock);

	uint32_t xm = obs_source_get_audio_mixers(x);
	uint32_t to_turn_off = 0; // bits currently on in the source that this sweep switches off
	uint32_t hold_conf = 0;   // bits to hold in the confirmed tier
	uint32_t hold_prov = 0;   // bits to hold in the provisional tier
	if (!(from_create && exists) && (xm & ((uint32_t)1 << in_bit)) && out_mask) {
		uint32_t candidates = out_mask & ~user_owned;
		to_turn_off = candidates & xm;
		// our own clear is provisional while the routing is unsettled (source_create), else confirmed
		if (from_create)
			hold_prov = to_turn_off;
		else
			hold_conf = to_turn_off;
		// a bit already off that a peer holds off is joined so restore waits for the last instance;
		// the join takes the tier of the peer claim it joins
		uint32_t shared = candidates & ~xm;
		for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
			uint32_t bit = (uint32_t)1 << i;
			if (!(shared & bit))
				continue;
			if (from_create) {
				if (mt2s_claim_held(uuid, bit, false))
					hold_prov |= bit;
			} else if (mt2s_claim_held(uuid, bit, true)) {
				hold_conf |= bit;
			} else if (join_prov && mt2s_claim_held(uuid, bit, false)) {
				hold_prov |= bit;
			}
		}
	}

	uint32_t prov_acquire = 0, conf_acquire = 0, prov_release = 0;
	bool need_watch = false;
	pthread_mutex_lock(&context->lock);
	r = mt2s_find_record(context, uuid);
	if (hold_conf || hold_prov) {
		if (!r)
			r = mt2s_add_record(context, uuid);
		uint32_t already_conf = r->cleared & ~r->provisional;
		prov_release = hold_conf & r->provisional; // promote these out of the provisional tier
		conf_acquire = hold_conf & ~already_conf;  // promotions plus brand-new confirmed holds
		prov_acquire = hold_prov & ~r->cleared;    // brand-new provisional holds
		r->provisional = (r->provisional & ~hold_conf) | prov_acquire;
		r->cleared |= hold_conf | hold_prov;
		if (from_create)
			r->pending = true;
	}
	if (r && !r->watched) {
		r->watched = true;
		need_watch = true;
	}
	pthread_mutex_unlock(&context->lock);

	for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
		uint32_t bit = (uint32_t)1 << i;
		if (prov_release & bit)
			mt2s_claim_release(uuid, bit, false);
		if (prov_acquire & bit)
			mt2s_claim_acquire(uuid, bit, false);
		if (conf_acquire & bit)
			mt2s_claim_acquire(uuid, bit, true);
	}
	if (need_watch)
		mt2s_watch(context, x);
	if (to_turn_off)
		obs_source_set_audio_mixers(x, xm & ~to_turn_off);
}

struct mt2s_sweep_args {
	mt2s_context_t *context;
	uint32_t out_mask;
	size_t in_bit;
};

static bool mt2s_sweep_enum(void *param, obs_source_t *x)
{
	struct mt2s_sweep_args *a = param;
	// sweep_all runs from a live output/input change, so it may join a peer's provisional claim
	mt2s_sweep_source(a->context, x, a->out_mask, a->in_bit, false, true);
	return true;
}

static void mt2s_sweep_all(mt2s_context_t *context, uint32_t out_mask, size_t in_bit)
{
	struct mt2s_sweep_args a = {context, out_mask, in_bit};
	obs_enum_sources(mt2s_sweep_enum, &a);
}

// Gives every cleared bit back to its source and forgets all records. Caller must not hold context->lock.
static void mt2s_restore_all(mt2s_context_t *context)
{
	pthread_mutex_lock(&context->lock);
	mt2s_record_t *list = context->records;
	context->records = NULL;
	pthread_mutex_unlock(&context->lock);

	mt2s_record_t *r = list;
	while (r) {
		mt2s_record_t *next = r->next;
		obs_source_t *x = obs_get_source_by_uuid(r->uuid);
		// release this record's claims in its tier; a bit is restorable only once no holder
		// remains in either tier
		uint32_t give = 0;
		for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
			uint32_t bit = (uint32_t)1 << i;
			if ((r->cleared & bit) && mt2s_claim_release(r->uuid, bit, !(r->provisional & bit)))
				give |= bit;
		}
		if (x) {
			if (r->watched)
				mt2s_unwatch(context, x);
			uint32_t xm = obs_source_get_audio_mixers(x);
			give &= ~xm;
			if (give)
				obs_source_set_audio_mixers(x, xm | give);
			obs_source_release(x);
		}
		bfree(r->uuid);
		bfree(r);
		r = next;
	}
}

struct mt2s_giveback {
	char *uuid;
	uint32_t give;
	uint32_t give_prov; // subset of give that was held in the provisional tier
	struct mt2s_giveback *next;
};

// Applies a new output mask: returns bits no longer in the output, then clears for the new output.
static void mt2s_reconfigure(mt2s_context_t *context, uint32_t new_out, size_t in_bit)
{
	struct mt2s_giveback *backs = NULL;

	pthread_mutex_lock(&context->lock);
	if (new_out == context->out_mask) {
		pthread_mutex_unlock(&context->lock);
		return;
	}
	context->out_mask = new_out;
	for (mt2s_record_t *r = context->records; r; r = r->next) {
		uint32_t give = r->cleared & ~new_out;
		if (give) {
			r->cleared &= new_out;
			struct mt2s_giveback *b = bzalloc(sizeof(struct mt2s_giveback));
			b->uuid = bstrdup(r->uuid);
			b->give = give;
			b->give_prov = give & r->provisional;
			r->provisional &= new_out;
			b->next = backs;
			backs = b;
		}
	}
	pthread_mutex_unlock(&context->lock);

	struct mt2s_giveback *b = backs;
	while (b) {
		struct mt2s_giveback *next = b->next;
		obs_source_t *x = obs_get_source_by_uuid(b->uuid);
		// release the dropped claims in their tier; restore a bit only once no holder remains
		uint32_t give = 0;
		for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
			uint32_t bit = (uint32_t)1 << i;
			if ((b->give & bit) && mt2s_claim_release(b->uuid, bit, !(b->give_prov & bit)))
				give |= bit;
		}
		if (x) {
			uint32_t xm = obs_source_get_audio_mixers(x);
			give &= ~xm;
			if (give)
				obs_source_set_audio_mixers(x, xm | give);
			obs_source_release(x);
		}
		bfree(b->uuid);
		bfree(b);
		b = next;
	}

	if (new_out)
		mt2s_sweep_all(context, new_out, in_bit);
}

static uint32_t mt2s_output_mask(mt2s_context_t *context, size_t in_bit)
{
	return (uint32_t)obs_source_get_audio_mixers(context->source) & ~((uint32_t)1 << in_bit);
}

// --- signal callbacks ---

// the instance's own output changed (Advanced Audio Properties)
static void mt2s_on_p_mixers(void *param, calldata_t *cd)
{
	mt2s_context_t *context = param;
	size_t in_bit = atomic_load(&context->mix_idx);
	if (in_bit >= MAX_AUDIO_MIXES)
		return;

	uint32_t new_out = (uint32_t)calldata_int(cd, "mixers") & ~((uint32_t)1 << in_bit);
	if (new_out == context->out_mask)
		return;

	mt2s_reconfigure(context, new_out, in_bit);
}

// another source was created: clear from its current routing (settled for a newly added source)
static void mt2s_on_source_created(void *param, calldata_t *cd)
{
	mt2s_context_t *context = param;
	obs_source_t *x = calldata_ptr(cd, "source");
	if (!x)
		return;

	size_t in_bit = atomic_load(&context->mix_idx);
	if (in_bit >= MAX_AUDIO_MIXES)
		return;

	mt2s_sweep_source(context, x, mt2s_output_mask(context, in_bit), in_bit, true, false);
}

// a source finished loading: drop any provisional record made from its unsettled routing, then clear
static void mt2s_on_source_loaded(void *param, calldata_t *cd)
{
	mt2s_context_t *context = param;
	obs_source_t *x = calldata_ptr(cd, "source");
	if (!x)
		return;

	size_t in_bit = atomic_load(&context->mix_idx);
	if (in_bit >= MAX_AUDIO_MIXES)
		return;

	const char *uuid = obs_source_get_uuid(x);
	if (uuid && *uuid) {
		// drop every provisional hold on this source; a record this session added at
		// source_create also loses its confirmed holds and any re-enable seen during the
		// unsettled routing, so the sweep below re-derives it cleanly from the settled routing
		uint32_t drop_prov = 0, drop_conf = 0;
		pthread_mutex_lock(&context->lock);
		mt2s_record_t *r = mt2s_find_record(context, uuid);
		if (r) {
			drop_prov = r->provisional;
			r->cleared &= ~drop_prov;
			r->provisional = 0;
			if (r->pending) {
				drop_conf = r->cleared;
				r->cleared = 0;
				r->user_owned = 0;
				r->pending = false;
			}
		}
		pthread_mutex_unlock(&context->lock);

		for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
			uint32_t bit = (uint32_t)1 << i;
			if (drop_prov & bit)
				mt2s_claim_release(uuid, bit, false);
			if (drop_conf & bit)
				mt2s_claim_release(uuid, bit, true);
		}
	}

	// re-derivation after load must not capture a peer's transient provisional claim
	mt2s_sweep_source(context, x, mt2s_output_mask(context, in_bit), in_bit, false, false);
}

// another source changed its output: drop the bits the user re-enabled
static void mt2s_on_source_mixers(void *param, calldata_t *cd)
{
	mt2s_context_t *context = param;
	obs_source_t *x = calldata_ptr(cd, "source");
	if (!x)
		return;

	uint32_t newval = (uint32_t)calldata_int(cd, "mixers");
	uint32_t oldval = obs_source_get_audio_mixers(x);
	uint32_t turned_on = newval & ~oldval;
	if (!turned_on)
		return;

	const char *uuid = obs_source_get_uuid(x);
	if (!uuid || !*uuid)
		return;

	uint32_t reenabled = 0;
	uint32_t reenabled_prov = 0;
	pthread_mutex_lock(&context->lock);
	mt2s_record_t *r = mt2s_find_record(context, uuid);
	if (r) {
		reenabled = turned_on & r->cleared;
		reenabled_prov = reenabled & r->provisional;
		r->cleared &= ~reenabled;
		r->provisional &= ~reenabled;
		r->user_owned |= reenabled;
	}
	pthread_mutex_unlock(&context->lock);

	// the user took these bits back; drop this instance's claim on them in each bit's tier
	for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
		uint32_t bit = (uint32_t)1 << i;
		if (reenabled & bit)
			mt2s_claim_release(uuid, bit, !(reenabled_prov & bit));
	}
}

// another source is being destroyed: forget its record
static void mt2s_on_source_destroy(void *param, calldata_t *cd)
{
	mt2s_context_t *context = param;
	obs_source_t *x = calldata_ptr(cd, "source");
	if (!x)
		return;

	const char *uuid = obs_source_get_uuid(x);
	if (!uuid || !*uuid)
		return;

	mt2s_record_t *dead = NULL;
	pthread_mutex_lock(&context->lock);
	mt2s_record_t **pp = &context->records;
	while (*pp) {
		mt2s_record_t *r = *pp;
		if (strcmp(r->uuid, uuid) == 0) {
			*pp = r->next;
			dead = r;
			break;
		}
		pp = &r->next;
	}
	pthread_mutex_unlock(&context->lock);

	if (dead) {
		// the source is gone; drop its claims in each bit's tier so the refcount does not leak
		for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
			uint32_t bit = (uint32_t)1 << i;
			if (dead->cleared & bit)
				mt2s_claim_release(uuid, bit, !(dead->provisional & bit));
		}
		bfree(dead->uuid);
		bfree(dead);
	}
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

static void mt2s_load_records(mt2s_context_t *context, obs_data_t *settings)
{
	obs_data_array_t *arr = obs_data_get_array(settings, MT2S_CLEARED_KEY);
	if (!arr)
		return;

	size_t count = obs_data_array_count(arr);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);
		const char *uuid = obs_data_get_string(item, "uuid");
		if (uuid && *uuid) {
			mt2s_record_t *r = mt2s_add_record(context, uuid);
			r->cleared = (uint32_t)obs_data_get_int(item, "cleared");
			r->user_owned = (uint32_t)obs_data_get_int(item, "user_owned");
			// re-register this instance's confirmed claims on the loaded cleared bits
			for (size_t bitpos = 0; bitpos < MAX_AUDIO_MIXES; bitpos++) {
				uint32_t bit = (uint32_t)1 << bitpos;
				if (r->cleared & bit)
					mt2s_claim_acquire(uuid, bit, true);
			}
		}
		obs_data_release(item);
	}
	obs_data_array_release(arr);
}

static void mt2s_save(void *data, obs_data_t *settings)
{
	mt2s_context_t *context = (mt2s_context_t *)data;
	if (!context || !settings)
		return;

	obs_data_array_t *arr = obs_data_array_create();

	pthread_mutex_lock(&context->lock);
	for (mt2s_record_t *r = context->records; r; r = r->next) {
		if (r->cleared == 0 && r->user_owned == 0)
			continue;
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "uuid", r->uuid);
		obs_data_set_int(item, "cleared", r->cleared);
		obs_data_set_int(item, "user_owned", r->user_owned);
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	pthread_mutex_unlock(&context->lock);

	obs_data_set_array(settings, MT2S_CLEARED_KEY, arr);
	obs_data_array_release(arr);
}

// lifecycle
void *mt2s_create(obs_data_t *settings, obs_source_t *source)
{
	mt2s_context_t *context = bzalloc(sizeof(mt2s_context_t));

	if (context) {
		context->source = source;
		atomic_init(&context->mix_idx, SIZE_MAX);
		pthread_mutex_init(&context->lock, NULL);

		// restore the output bits cleared in a previous session
		mt2s_load_records(context, settings);

		// watch the instance output and the source lifecycle
		signal_handler_t *psh = obs_source_get_signal_handler(source);
		if (psh)
			signal_handler_connect(psh, "audio_mixers", mt2s_on_p_mixers, context);
		signal_handler_t *gsh = obs_get_signal_handler();
		if (gsh) {
			signal_handler_connect(gsh, "source_create", mt2s_on_source_created, context);
			signal_handler_connect(gsh, "source_load", mt2s_on_source_loaded, context);
			signal_handler_connect(gsh, "source_destroy", mt2s_on_source_destroy, context);
		}

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

	// stop receiving signals; disconnect waits for any in-flight callback to finish
	signal_handler_t *psh = obs_source_get_signal_handler(context->source);
	if (psh)
		signal_handler_disconnect(psh, "audio_mixers", mt2s_on_p_mixers, context);
	signal_handler_t *gsh = obs_get_signal_handler();
	if (gsh) {
		signal_handler_disconnect(gsh, "source_create", mt2s_on_source_created, context);
		signal_handler_disconnect(gsh, "source_load", mt2s_on_source_loaded, context);
		signal_handler_disconnect(gsh, "source_destroy", mt2s_on_source_destroy, context);
	}

	mt2s_disconnect(context);
	mt2s_restore_all(context);

	pthread_mutex_destroy(&context->lock);
	bfree(context);
}

void mt2s_update(void *data, obs_data_t *settings)
{
	if (!data || !settings)
		return;

	mt2s_context_t *context = (mt2s_context_t *)data;

	size_t new_idx = obs_data_get_int(settings, "MixTrack");
	if (new_idx != atomic_load(&context->mix_idx)) {
		// the input track changed: give back everything cleared for the old track
		mt2s_restore_all(context);
		pthread_mutex_lock(&context->lock);
		context->out_mask = 0;
		pthread_mutex_unlock(&context->lock);
	}

	mt2s_connect(context, new_idx);

	size_t in_bit = atomic_load(&context->mix_idx);
	if (in_bit < MAX_AUDIO_MIXES) {
		uint32_t out_mask = mt2s_output_mask(context, in_bit);
		pthread_mutex_lock(&context->lock);
		context->out_mask = out_mask;
		pthread_mutex_unlock(&context->lock);
		if (out_mask)
			mt2s_sweep_all(context, out_mask, in_bit);
	}
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
				       .save = mt2s_save,
				       .update = mt2s_update};
	obs_register_source(&info);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
