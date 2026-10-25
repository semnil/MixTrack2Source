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

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

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

// audio_mixers bits that map to real mix tracks
#define MT2S_MIX_MASK (((uint32_t)1 << MAX_AUDIO_MIXES) - 1)

// one entry per other source this instance cleared output bits from
typedef struct mt2s_record {
	char *uuid;
	uint32_t cleared;    // output bits cleared from the source and still owned by this instance
	uint32_t user_owned; // output bits the user re-enabled; never cleared or restored again
	bool watched;        // the source's audio_mixers signal is connected
	struct mt2s_record *next;
} mt2s_record_t;

typedef struct {
	obs_source_t *source;
	obs_weak_source_t *weak;  // this instance, as listed in the module-wide instance list
	atomic_size_t mix_idx;    // audio thread (read) / UI thread (read-write)
	atomic_size_t own_mixers; // this instance's audio_mixers as its last audio_mixers signal carried them
	pthread_mutex_t lock;     // guards records, out_mask and routed_in
	mt2s_record_t *records;
	uint32_t out_mask; // output mixers used for the last sweep
	size_t routed_in;  // input track the records were made for
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
// bit is restored only once the last instance releases it.
struct mt2s_claim {
	char *uuid;
	uint32_t bit;
	int refs;
	struct mt2s_claim *next;
};

// Output bits whose last claim was released while their source was absent (deleted and still
// reachable by undo). They go back on when a source with that uuid appears, unless an instance
// takes them over first. A bit is never both claimed and waiting.
struct mt2s_pending {
	char *uuid;
	uint32_t bits;
	struct mt2s_pending *next;
};

static pthread_mutex_t mt2s_claims_lock = PTHREAD_MUTEX_INITIALIZER; // guards claims and pendings
static struct mt2s_claim *mt2s_claims;
static struct mt2s_pending *mt2s_pendings;
static bool mt2s_pendings_off; // the scene collection is being cleared: no bit starts waiting

// caller holds mt2s_claims_lock
static struct mt2s_pending **mt2s_pending_slot(const char *uuid)
{
	struct mt2s_pending **pp = &mt2s_pendings;
	while (*pp && strcmp((*pp)->uuid, uuid) != 0)
		pp = &(*pp)->next;
	return pp;
}

// Removes bits from what waits for a uuid and returns the bits removed. Caller holds mt2s_claims_lock.
static uint32_t mt2s_pending_take_locked(const char *uuid, uint32_t bits)
{
	struct mt2s_pending **pp = mt2s_pending_slot(uuid);
	struct mt2s_pending *p = *pp;
	if (!p)
		return 0;

	uint32_t taken = p->bits & bits;
	p->bits &= ~bits;
	if (!p->bits) {
		*pp = p->next;
		bfree(p->uuid);
		bfree(p);
	}
	return taken;
}

static bool mt2s_pending_waiting(const char *uuid)
{
	pthread_mutex_lock(&mt2s_claims_lock);
	bool waiting = *mt2s_pending_slot(uuid) != NULL;
	pthread_mutex_unlock(&mt2s_claims_lock);
	return waiting;
}

// Drops every waiting bit when off is set; while it stays set no bit starts waiting.
static void mt2s_pending_reset(bool off)
{
	pthread_mutex_lock(&mt2s_claims_lock);
	mt2s_pendings_off = off;
	while (off && mt2s_pendings) {
		struct mt2s_pending *p = mt2s_pendings;
		mt2s_pendings = p->next;
		bfree(p->uuid);
		bfree(p);
	}
	pthread_mutex_unlock(&mt2s_claims_lock);
}

static void mt2s_claim_acquire(const char *uuid, uint32_t bit)
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
	c->refs++;
	// a holder owns the bit from now on, so it stops waiting for its source
	mt2s_pending_take_locked(uuid, bit);
	pthread_mutex_unlock(&mt2s_claims_lock);
}

// whether a peer holds the bit off or it waits for its source; a sweep that keeps the bit off joins either
static bool mt2s_claim_joinable(const char *uuid, uint32_t bit)
{
	bool held = false;
	pthread_mutex_lock(&mt2s_claims_lock);
	for (struct mt2s_claim *c = mt2s_claims; c; c = c->next) {
		if (c->bit == bit && strcmp(c->uuid, uuid) == 0) {
			held = c->refs > 0;
			break;
		}
	}
	if (!held) {
		struct mt2s_pending *p = *mt2s_pending_slot(uuid);
		held = p && (p->bits & bit);
	}
	pthread_mutex_unlock(&mt2s_claims_lock);
	return held;
}

// Drops one reference; returns true when no holder remains afterwards. Caller holds mt2s_claims_lock.
static bool mt2s_claim_release_locked(const char *uuid, uint32_t bit)
{
	struct mt2s_claim **pp = &mt2s_claims;
	while (*pp) {
		struct mt2s_claim *c = *pp;
		if (c->bit == bit && strcmp(c->uuid, uuid) == 0) {
			if (--c->refs > 0)
				return false;
			*pp = c->next;
			bfree(c->uuid);
			bfree(c);
			return true;
		}
		pp = &c->next;
	}
	return false;
}

static bool mt2s_claim_release(const char *uuid, uint32_t bit)
{
	pthread_mutex_lock(&mt2s_claims_lock);
	bool empty = mt2s_claim_release_locked(uuid, bit);
	pthread_mutex_unlock(&mt2s_claims_lock);
	return empty;
}

// Releases this instance's claims on bits of one source and turns back on the bits no holder needs;
// while the source is absent those bits wait for it instead.
static void mt2s_return_bits(const char *uuid, uint32_t bits)
{
	if (!bits)
		return;

	obs_source_t *x = obs_get_source_by_uuid(uuid);
	uint32_t give = 0;
	pthread_mutex_lock(&mt2s_claims_lock);
	for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
		uint32_t bit = (uint32_t)1 << i;
		if ((bits & bit) && mt2s_claim_release_locked(uuid, bit))
			give |= bit;
	}
	if (!x && give && !mt2s_pendings_off) {
		struct mt2s_pending **pp = mt2s_pending_slot(uuid);
		if (!*pp) {
			*pp = bzalloc(sizeof(struct mt2s_pending));
			(*pp)->uuid = bstrdup(uuid);
		}
		(*pp)->bits |= give;
	}
	pthread_mutex_unlock(&mt2s_claims_lock);

	if (x) {
		uint32_t xm = obs_source_get_audio_mixers(x);
		give &= ~xm;
		if (give)
			obs_source_set_audio_mixers(x, xm | give);
		obs_source_release(x);
	}
}

// --- live instances ---

struct mt2s_instance {
	obs_weak_source_t *weak;
	struct mt2s_instance *next;
};

static pthread_mutex_t mt2s_instances_lock = PTHREAD_MUTEX_INITIALIZER;
static struct mt2s_instance *mt2s_instances;

static void mt2s_instance_add(obs_weak_source_t *weak)
{
	struct mt2s_instance *i = bzalloc(sizeof(struct mt2s_instance));
	i->weak = weak;
	pthread_mutex_lock(&mt2s_instances_lock);
	i->next = mt2s_instances;
	mt2s_instances = i;
	pthread_mutex_unlock(&mt2s_instances_lock);
}

static void mt2s_instance_remove(obs_weak_source_t *weak)
{
	struct mt2s_instance *dead = NULL;
	pthread_mutex_lock(&mt2s_instances_lock);
	for (struct mt2s_instance **pp = &mt2s_instances; *pp; pp = &(*pp)->next) {
		if ((*pp)->weak == weak) {
			dead = *pp;
			*pp = dead->next;
			break;
		}
	}
	pthread_mutex_unlock(&mt2s_instances_lock);
	bfree(dead);
}

// --- sweeps ---

// another audio source that is not a MixTrack2Source instance
static bool mt2s_is_candidate(mt2s_context_t *context, obs_source_t *x)
{
	if (x == context->source)
		return false;
	if ((obs_source_get_output_flags(x) & OBS_SOURCE_AUDIO) == 0)
		return false;

	const char *id = obs_source_get_id(x);
	return !(id && strcmp(id, PLUGIN_NAME) == 0);
}

// Clears the instance's output bits from one candidate source. Caller must not hold context->lock.
static void mt2s_sweep_source(mt2s_context_t *context, obs_source_t *x, uint32_t out_mask, size_t in_bit)
{
	if (!mt2s_is_candidate(context, x))
		return;

	const char *uuid = obs_source_get_uuid(x);
	if (!uuid || !*uuid)
		return;

	pthread_mutex_lock(&context->lock);
	mt2s_record_t *r = mt2s_find_record(context, uuid);
	uint32_t user_owned = r ? r->user_owned : 0;
	pthread_mutex_unlock(&context->lock);

	uint32_t xm = obs_source_get_audio_mixers(x);
	uint32_t to_turn_off = 0;
	uint32_t to_hold = 0;
	if ((xm & ((uint32_t)1 << in_bit)) && out_mask) {
		uint32_t candidates = out_mask & ~user_owned;
		to_turn_off = candidates & xm;
		to_hold = to_turn_off;
		// join a bit a peer already holds off, or one waiting for this source to come back, so
		// restore waits for the last instance
		uint32_t shared = candidates & ~xm;
		for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
			uint32_t bit = (uint32_t)1 << i;
			if ((shared & bit) && mt2s_claim_joinable(uuid, bit))
				to_hold |= bit;
		}
	}

	uint32_t acquire = 0;
	bool need_watch = false;
	pthread_mutex_lock(&context->lock);
	r = mt2s_find_record(context, uuid);
	if (to_hold) {
		if (!r)
			r = mt2s_add_record(context, uuid);
		acquire = to_hold & ~r->cleared;
		r->cleared |= to_hold;
	}
	if (r && !r->watched) {
		r->watched = true;
		need_watch = true;
	}
	pthread_mutex_unlock(&context->lock);

	for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
		uint32_t bit = (uint32_t)1 << i;
		if (acquire & bit)
			mt2s_claim_acquire(uuid, bit);
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
	mt2s_sweep_source(a->context, x, a->out_mask, a->in_bit);
	return true;
}

static void mt2s_sweep_all(mt2s_context_t *context, uint32_t out_mask, size_t in_bit)
{
	struct mt2s_sweep_args a = {context, out_mask, in_bit};
	obs_enum_sources(mt2s_sweep_enum, &a);
}

static uint32_t mt2s_output_mask(mt2s_context_t *context, size_t in_bit)
{
	return (uint32_t)atomic_load(&context->own_mixers) & MT2S_MIX_MASK & ~((uint32_t)1 << in_bit);
}

// --- give-back ---

struct mt2s_giveback {
	char *uuid;
	uint32_t give;
	struct mt2s_giveback *next;
};

// Gives back the cleared bits outside keep; every record stays, with its user-owned bits.
// Caller must not hold context->lock.
static void mt2s_give_back(mt2s_context_t *context, uint32_t keep)
{
	struct mt2s_giveback *backs = NULL;

	pthread_mutex_lock(&context->lock);
	for (mt2s_record_t *r = context->records; r; r = r->next) {
		uint32_t give = r->cleared & ~keep;
		if (give) {
			r->cleared &= keep;
			struct mt2s_giveback *b = bzalloc(sizeof(struct mt2s_giveback));
			b->uuid = bstrdup(r->uuid);
			b->give = give;
			b->next = backs;
			backs = b;
		}
	}
	pthread_mutex_unlock(&context->lock);

	struct mt2s_giveback *b = backs;
	while (b) {
		struct mt2s_giveback *next = b->next;
		mt2s_return_bits(b->uuid, b->give);
		bfree(b->uuid);
		bfree(b);
		b = next;
	}
}

// UI task: gives every cleared bit of a destroyed instance's detached records back and frees them
static void mt2s_give_back_records(void *param)
{
	mt2s_record_t *r = param;
	while (r) {
		mt2s_record_t *next = r->next;
		mt2s_return_bits(r->uuid, r->cleared);
		bfree(r->uuid);
		bfree(r);
		r = next;
	}
}

// Applies a new output mask: returns bits no longer in the output, then clears for the new output.
// Returns false when the output was already applied.
static bool mt2s_reconfigure(mt2s_context_t *context, uint32_t new_out, size_t in_bit)
{
	pthread_mutex_lock(&context->lock);
	if (new_out == context->out_mask) {
		pthread_mutex_unlock(&context->lock);
		return false;
	}
	context->out_mask = new_out;
	pthread_mutex_unlock(&context->lock);

	mt2s_give_back(context, new_out);
	if (new_out)
		mt2s_sweep_all(context, new_out, in_bit);
	return true;
}

// sweeps one source with the instance's current input and output
static void mt2s_sweep_current(mt2s_context_t *context, obs_source_t *x)
{
	size_t in_bit = atomic_load(&context->mix_idx);
	if (in_bit < MAX_AUDIO_MIXES)
		mt2s_sweep_source(context, x, mt2s_output_mask(context, in_bit), in_bit);
}

// Applies the instance's current input and output tracks. A new input track first gives back
// everything cleared for the old one; the bits the user re-enabled stay theirs.
static void mt2s_reconcile(mt2s_context_t *context, bool force)
{
	size_t in_bit = atomic_load(&context->mix_idx);
	if (in_bit >= MAX_AUDIO_MIXES)
		return;

	pthread_mutex_lock(&context->lock);
	bool new_input = context->routed_in != in_bit;
	if (new_input) {
		context->routed_in = in_bit;
		context->out_mask = 0;
	}
	pthread_mutex_unlock(&context->lock);
	if (new_input)
		mt2s_give_back(context, 0);

	uint32_t out_mask = mt2s_output_mask(context, in_bit);
	if (!mt2s_reconfigure(context, out_mask, in_bit) && force && out_mask)
		mt2s_sweep_all(context, out_mask, in_bit);
}

struct mt2s_ref {
	obs_source_t *source;
	struct mt2s_ref *next;
};

// Lets every live instance take over the bits waiting for this source, then turns the rest back on.
static void mt2s_settle(obs_source_t *x)
{
	const char *uuid = obs_source_get_uuid(x);
	if (!uuid || !*uuid)
		return;

	struct mt2s_ref *refs = NULL;
	pthread_mutex_lock(&mt2s_instances_lock);
	for (struct mt2s_instance *i = mt2s_instances; i; i = i->next) {
		obs_source_t *s = obs_weak_source_get_source(i->weak);
		if (s) {
			struct mt2s_ref *ref = bzalloc(sizeof(struct mt2s_ref));
			ref->source = s;
			ref->next = refs;
			refs = ref;
		}
	}
	pthread_mutex_unlock(&mt2s_instances_lock);

	while (refs) {
		struct mt2s_ref *next = refs->next;
		mt2s_context_t *context = obs_obj_get_data(refs->source);
		if (context)
			mt2s_sweep_current(context, x);
		obs_source_release(refs->source);
		bfree(refs);
		refs = next;
	}

	pthread_mutex_lock(&mt2s_claims_lock);
	uint32_t give = mt2s_pending_take_locked(uuid, MT2S_MIX_MASK);
	pthread_mutex_unlock(&mt2s_claims_lock);
	uint32_t xm = obs_source_get_audio_mixers(x);
	give &= ~xm;
	if (give)
		obs_source_set_audio_mixers(x, xm | give);
}

// --- work deferred to the UI thread ---
// Signal callbacks and update only note what changed. The sweeps, give-backs, source lookups and
// enumerations that follow run as UI tasks, outside the signal that queued them. They take the
// instance's output tracks from its last audio_mixers signal, whose value libobs stores only after the
// signal returns, and the rest of the routing as it is when they run. A UI task queued from the UI
// thread runs at once, so one is queued from the destroy thread instead.

enum mt2s_job_kind {
	MT2S_JOB_SWEEP,     // sweep target for self
	MT2S_JOB_RECONCILE, // apply self's input and output tracks
	MT2S_JOB_SETTLE,    // settle the bits waiting for target
};

struct mt2s_job {
	enum mt2s_job_kind kind;
	obs_weak_source_t *self;
	obs_weak_source_t *target;
	bool force; // reconcile: sweep even when the output is already applied
};

static void mt2s_job_run(void *param)
{
	struct mt2s_job *job = param;
	obs_source_t *self = obs_weak_source_get_source(job->self);
	obs_source_t *x = obs_weak_source_get_source(job->target);
	mt2s_context_t *context = self ? obs_obj_get_data(self) : NULL;
	switch (job->kind) {
	case MT2S_JOB_SWEEP:
		if (context && x)
			mt2s_sweep_current(context, x);
		break;
	case MT2S_JOB_RECONCILE:
		if (context)
			mt2s_reconcile(context, job->force);
		break;
	case MT2S_JOB_SETTLE:
		if (x)
			mt2s_settle(x);
		break;
	}
	obs_source_release(x);
	obs_source_release(self);
	obs_weak_source_release(job->target);
	obs_weak_source_release(job->self);
	bfree(job);
}

static void mt2s_job_queue(void *param)
{
	obs_queue_task(OBS_TASK_UI, mt2s_job_run, param, false);
}

static void mt2s_defer(enum mt2s_job_kind kind, obs_source_t *self, obs_source_t *target, bool force)
{
	struct mt2s_job *job = bzalloc(sizeof(struct mt2s_job));
	job->kind = kind;
	job->self = obs_source_get_weak_source(self);
	job->target = obs_source_get_weak_source(target);
	job->force = force;
	if (obs_in_task_thread(OBS_TASK_UI))
		obs_queue_task(OBS_TASK_DESTROY, mt2s_job_queue, job, false);
	else
		mt2s_job_queue(job);
}

// --- signal callbacks ---

// the instance's own output changed (Advanced Audio Properties, or the loopback guard)
static void mt2s_on_p_mixers(void *param, calldata_t *cd)
{
	mt2s_context_t *context = param;
	uint32_t newval = (uint32_t)calldata_int(cd, "mixers") & MT2S_MIX_MASK;
	uint32_t oldval = (uint32_t)atomic_load(&context->own_mixers);
	atomic_store(&context->own_mixers, newval);

	size_t in_bit = atomic_load(&context->mix_idx);
	if (in_bit >= MAX_AUDIO_MIXES)
		return;

	uint32_t in_mask = (uint32_t)1 << in_bit;
	// a change of the input bit alone is not an output change
	if (((newval ^ oldval) & ~in_mask) == 0)
		return;

	mt2s_defer(MT2S_JOB_RECONCILE, context->source, NULL, false);
}

// another source was created (swept once its creator has set its routing) or loaded (scene
// collection or undo/redo)
static void mt2s_on_source_appeared(void *param, calldata_t *cd)
{
	mt2s_context_t *context = param;
	obs_source_t *x = calldata_ptr(cd, "source");
	if (x && mt2s_is_candidate(context, x))
		mt2s_defer(MT2S_JOB_SWEEP, context->source, x, false);
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
	pthread_mutex_lock(&context->lock);
	mt2s_record_t *r = mt2s_find_record(context, uuid);
	if (r) {
		reenabled = turned_on & r->cleared;
		r->cleared &= ~reenabled;
		r->user_owned |= reenabled;
	}
	pthread_mutex_unlock(&context->lock);

	// the user took these bits back; drop this instance's claim on them
	for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
		uint32_t bit = (uint32_t)1 << i;
		if (reenabled & bit)
			mt2s_claim_release(uuid, bit);
	}
}

// another source is being destroyed: its record and claims stay so a source that comes back with the
// same uuid (undo) still gets its bits back; its audio_mixers connection goes away with it
static void mt2s_on_source_destroy(void *param, calldata_t *cd)
{
	mt2s_context_t *context = param;
	obs_source_t *x = calldata_ptr(cd, "source");
	if (!x)
		return;

	const char *uuid = obs_source_get_uuid(x);
	if (!uuid || !*uuid)
		return;

	pthread_mutex_lock(&context->lock);
	mt2s_record_t *r = mt2s_find_record(context, uuid);
	if (r)
		r->watched = false;
	pthread_mutex_unlock(&context->lock);
}

// --- module-wide signal callbacks, connected with the first instance ---

// a source was created or loaded: settle the bits waiting for its uuid
static void mt2s_on_module_source(void *param, calldata_t *cd)
{
	UNUSED_PARAMETER(param);
	obs_source_t *x = calldata_ptr(cd, "source");
	const char *uuid = x ? obs_source_get_uuid(x) : NULL;
	if (uuid && *uuid && mt2s_pending_waiting(uuid))
		mt2s_defer(MT2S_JOB_SETTLE, NULL, x, false);
}

static volatile long mt2s_module_signals;

static void mt2s_connect_module_signals(void)
{
	if (!os_atomic_compare_swap_long(&mt2s_module_signals, 0, 1))
		return;

	signal_handler_t *gsh = obs_get_signal_handler();
	if (gsh) {
		signal_handler_connect(gsh, "source_create", mt2s_on_module_source, NULL);
		signal_handler_connect(gsh, "source_load", mt2s_on_module_source, NULL);
	}
}

// --- frontend events ---
// obs-frontend-api is looked up once at module load instead of linked. Without it no event arrives, and
// a waiting bit stays until a source with its uuid appears or the module unloads.

// obs_frontend_event values from obs-frontend-api.h
enum mt2s_frontend_event {
	MT2S_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED = 13,
	MT2S_FRONTEND_EVENT_EXIT = 17,
	MT2S_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP = 25,
};

typedef void (*mt2s_frontend_event_cb)(enum mt2s_frontend_event event, void *private_data);
typedef void (*mt2s_frontend_callback_fn)(mt2s_frontend_event_cb callback, void *private_data);

static mt2s_frontend_callback_fn mt2s_frontend_remove; // set while the event callback is registered

static void mt2s_on_frontend_event(enum mt2s_frontend_event event, void *private_data);

static void mt2s_disconnect_frontend(void)
{
	if (!mt2s_frontend_remove)
		return;

	mt2s_frontend_remove(mt2s_on_frontend_event, NULL);
	mt2s_frontend_remove = NULL;
}

// The frontend clears a scene collection (switch or exit) after removing its sources and before undo
// is cleared; the waiting bits are dropped and none start waiting until the next collection has
// loaded. The callback leaves at exit, before the frontend API goes away.
static void mt2s_on_frontend_event(enum mt2s_frontend_event event, void *private_data)
{
	UNUSED_PARAMETER(private_data);
	switch (event) {
	case MT2S_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		mt2s_pending_reset(true);
		break;
	case MT2S_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		mt2s_pending_reset(false);
		break;
	case MT2S_FRONTEND_EVENT_EXIT:
		mt2s_disconnect_frontend();
		break;
	}
}

// a function of the obs-frontend-api library loaded in the process, or NULL
static mt2s_frontend_callback_fn mt2s_frontend_function(const char *name)
{
	mt2s_frontend_callback_fn fn = NULL;
#ifdef _WIN32
	HMODULE module = GetModuleHandleW(L"obs-frontend-api.dll");
	if (module)
		fn = (mt2s_frontend_callback_fn)GetProcAddress(module, name);
#else
	void *program = dlopen(NULL, RTLD_LAZY);
	if (program) {
		void *symbol = dlsym(program, name);
		memcpy(&fn, &symbol, sizeof(fn));
		dlclose(program);
	}
#endif
	return fn;
}

static void mt2s_connect_frontend(void)
{
	mt2s_frontend_callback_fn add = mt2s_frontend_function("obs_frontend_add_event_callback");
	mt2s_frontend_callback_fn remove = mt2s_frontend_function("obs_frontend_remove_event_callback");
	if (add && remove) {
		add(mt2s_on_frontend_event, NULL);
		mt2s_frontend_remove = remove;
	}
	obs_log(LOG_INFO, "frontend events %s", mt2s_frontend_remove ? "connected" : "not available");
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
			r->cleared = (uint32_t)obs_data_get_int(item, "cleared") & MT2S_MIX_MASK;
			r->user_owned = (uint32_t)obs_data_get_int(item, "user_owned") & MT2S_MIX_MASK;
			// re-register this instance's claims on the loaded cleared bits
			for (size_t bitpos = 0; bitpos < MAX_AUDIO_MIXES; bitpos++) {
				uint32_t bit = (uint32_t)1 << bitpos;
				if (r->cleared & bit)
					mt2s_claim_acquire(uuid, bit);
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

	// copy the records so the source lookups below run without context->lock
	mt2s_record_t *copy = NULL;
	pthread_mutex_lock(&context->lock);
	for (mt2s_record_t *r = context->records; r; r = r->next) {
		if (r->cleared == 0 && r->user_owned == 0)
			continue;
		mt2s_record_t *c = bzalloc(sizeof(mt2s_record_t));
		c->uuid = bstrdup(r->uuid);
		c->cleared = r->cleared;
		c->user_owned = r->user_owned;
		c->next = copy;
		copy = c;
	}
	pthread_mutex_unlock(&context->lock);

	obs_data_array_t *arr = obs_data_array_create();
	while (copy) {
		mt2s_record_t *next = copy->next;
		// a record kept for a destroyed source only serves an undo within this session
		obs_source_t *x = obs_get_source_by_uuid(copy->uuid);
		if (x) {
			obs_data_t *item = obs_data_create();
			obs_data_set_string(item, "uuid", copy->uuid);
			obs_data_set_int(item, "cleared", copy->cleared);
			obs_data_set_int(item, "user_owned", copy->user_owned);
			obs_data_array_push_back(arr, item);
			obs_data_release(item);
			obs_source_release(x);
		}
		bfree(copy->uuid);
		bfree(copy);
		copy = next;
	}

	obs_data_set_array(settings, MT2S_CLEARED_KEY, arr);
	obs_data_array_release(arr);
}

// lifecycle
void *mt2s_create(obs_data_t *settings, obs_source_t *source)
{
	mt2s_context_t *context = bzalloc(sizeof(mt2s_context_t));

	if (context) {
		context->source = source;
		context->weak = obs_source_get_weak_source(source);
		atomic_init(&context->mix_idx, SIZE_MAX);
		atomic_init(&context->own_mixers, obs_source_get_audio_mixers(source) & MT2S_MIX_MASK);
		pthread_mutex_init(&context->lock, NULL);

		// restore the output bits cleared in a previous session
		mt2s_load_records(context, settings);

		// watch the instance output and the source lifecycle
		signal_handler_t *psh = obs_source_get_signal_handler(source);
		if (psh)
			signal_handler_connect(psh, "audio_mixers", mt2s_on_p_mixers, context);
		signal_handler_t *gsh = obs_get_signal_handler();
		if (gsh) {
			signal_handler_connect(gsh, "source_create", mt2s_on_source_appeared, context);
			signal_handler_connect(gsh, "source_load", mt2s_on_source_appeared, context);
			signal_handler_connect(gsh, "source_destroy", mt2s_on_source_destroy, context);
		}

		// deselect all outputs
		obs_source_set_audio_mixers(context->source, 0);

		mt2s_connect(context, obs_data_get_int(settings, "MixTrack"));

		// the restored records were made for the saved input track
		pthread_mutex_lock(&context->lock);
		context->routed_in = atomic_load(&context->mix_idx);
		pthread_mutex_unlock(&context->lock);

		mt2s_instance_add(context->weak);
		mt2s_connect_module_signals();
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
		signal_handler_disconnect(gsh, "source_create", mt2s_on_source_appeared, context);
		signal_handler_disconnect(gsh, "source_load", mt2s_on_source_appeared, context);
		signal_handler_disconnect(gsh, "source_destroy", mt2s_on_source_destroy, context);
	}

	mt2s_disconnect(context);
	mt2s_instance_remove(context->weak);

	// detach the records and stop watching their sources while this instance still exists
	pthread_mutex_lock(&context->lock);
	mt2s_record_t *records = context->records;
	context->records = NULL;
	pthread_mutex_unlock(&context->lock);
	for (mt2s_record_t *r = records; r; r = r->next) {
		if (!r->watched)
			continue;
		obs_source_t *x = obs_get_source_by_uuid(r->uuid);
		if (x) {
			mt2s_unwatch(context, x);
			obs_source_release(x);
		}
		r->watched = false;
	}
	// give the cleared bits back on the UI thread, where every sweep runs
	if (records)
		obs_queue_task(OBS_TASK_UI, mt2s_give_back_records, records, false);

	obs_weak_source_release(context->weak);
	pthread_mutex_destroy(&context->lock);
	bfree(context);
}

void mt2s_update(void *data, obs_data_t *settings)
{
	if (!data || !settings)
		return;

	mt2s_context_t *context = (mt2s_context_t *)data;

	mt2s_connect(context, obs_data_get_int(settings, "MixTrack"));
	mt2s_defer(MT2S_JOB_RECONCILE, context->source, NULL, true);
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
	mt2s_connect_frontend();

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	if (os_atomic_load_long(&mt2s_module_signals)) {
		signal_handler_t *gsh = obs_get_signal_handler();
		if (gsh) {
			signal_handler_disconnect(gsh, "source_create", mt2s_on_module_source, NULL);
			signal_handler_disconnect(gsh, "source_load", mt2s_on_module_source, NULL);
		}
	}
	mt2s_disconnect_frontend();
	mt2s_pending_reset(true);

	obs_log(LOG_INFO, "plugin unloaded");
}
