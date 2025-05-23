// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022, Linaro Limited, All rights reserved.
 * Author: Mike Leach <mike.leach@linaro.org>
 */
#include <linux/coresight.h>
#include <linux/coresight-pmu.h>
#include <linux/cpumask.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include "coresight-trace-id.h"

enum trace_id_flags {
	TRACE_ID_ANY = 0x0,
	TRACE_ID_PREFER_ODD = 0x1,
	TRACE_ID_REQ_STATIC = 0x2,
};

/* Default trace ID map. Used in sysfs mode and for system sources */
static DEFINE_PER_CPU(atomic_t, id_map_default_cpu_ids) = ATOMIC_INIT(0);
static struct coresight_trace_id_map id_map_default = {
	.cpu_map = &id_map_default_cpu_ids,
	.lock = __RAW_SPIN_LOCK_UNLOCKED(id_map_default.lock)
};

/* #define TRACE_ID_DEBUG 1 */
#if defined(TRACE_ID_DEBUG) || defined(CONFIG_COMPILE_TEST)

static void coresight_trace_id_dump_table(struct coresight_trace_id_map *id_map,
					  const char *func_name)
{
	pr_debug("%s id_map::\n", func_name);
	pr_debug("Used = %*pb\n", CORESIGHT_TRACE_IDS_MAX, id_map->used_ids);
}
#define DUMP_ID_MAP(map)   coresight_trace_id_dump_table(map, __func__)
#define DUMP_ID_CPU(cpu, id) pr_debug("%s called;  cpu=%d, id=%d\n", __func__, cpu, id)
#define DUMP_ID(id)   pr_debug("%s called; id=%d\n", __func__, id)
#define PERF_SESSION(n) pr_debug("%s perf count %d\n", __func__, n)
#else
#define DUMP_ID_MAP(map)
#define DUMP_ID(id)
#define DUMP_ID_CPU(cpu, id)
#define PERF_SESSION(n)
#endif

/* unlocked read of current trace ID value for given CPU */
static int _coresight_trace_id_read_cpu_id(int cpu, struct coresight_trace_id_map *id_map)
{
	return atomic_read(per_cpu_ptr(id_map->cpu_map, cpu));
}

/* look for next available odd ID, return 0 if none found */
static int coresight_trace_id_find_odd_id(struct coresight_trace_id_map *id_map)
{
	int found_id = 0, bit = 1, next_id;

	while ((bit < CORESIGHT_TRACE_ID_RES_TOP) && !found_id) {
		/*
		 * bitmap length of CORESIGHT_TRACE_ID_RES_TOP,
		 * search from offset `bit`.
		 */
		next_id = find_next_zero_bit(id_map->used_ids,
					     CORESIGHT_TRACE_ID_RES_TOP, bit);
		if ((next_id < CORESIGHT_TRACE_ID_RES_TOP) && (next_id & 0x1))
			found_id = next_id;
		else
			bit = next_id + 1;
	}
	return found_id;
}

/*
 * Allocate new ID and set in use
 *
 * if @preferred_id is a valid id then try to use that value if available.
 * if @preferred_id is not valid and @prefer_odd_id is true, try for odd id.
 *
 * Otherwise allocate next available ID.
 */
static int coresight_trace_id_alloc_new_id(struct coresight_trace_id_map *id_map,
					   int preferred_id, unsigned int flags)
{
	int id = 0;

	/* for backwards compatibility, cpu IDs may use preferred value */
	if (IS_VALID_CS_TRACE_ID(preferred_id)) {
		if (!test_bit(preferred_id, id_map->used_ids)) {
			id = preferred_id;
			goto trace_id_allocated;
		} else if (flags & TRACE_ID_REQ_STATIC)
			return -EBUSY;
	} else if (flags & TRACE_ID_PREFER_ODD) {
	/* may use odd ids to avoid preferred legacy cpu IDs */
		id = coresight_trace_id_find_odd_id(id_map);
		if (id)
			goto trace_id_allocated;
	} else if (!IS_VALID_CS_TRACE_ID(preferred_id) &&
			(flags & TRACE_ID_REQ_STATIC))
		return -EINVAL;

	/*
	 * skip reserved bit 0, look at bitmap length of
	 * CORESIGHT_TRACE_ID_RES_TOP from offset of bit 1.
	 */
	id = find_next_zero_bit(id_map->used_ids, CORESIGHT_TRACE_ID_RES_TOP, 1);
	if (id >= CORESIGHT_TRACE_ID_RES_TOP)
		return -EINVAL;

	/* mark as used */
trace_id_allocated:
	set_bit(id, id_map->used_ids);
	return id;
}

static void coresight_trace_id_free(int id, struct coresight_trace_id_map *id_map)
{
	if (WARN(!IS_VALID_CS_TRACE_ID(id), "Invalid Trace ID %d\n", id))
		return;
	if (WARN(!test_bit(id, id_map->used_ids), "Freeing unused ID %d\n", id))
		return;
	clear_bit(id, id_map->used_ids);
}

/*
 * Release all IDs and clear CPU associations.
 */
static void coresight_trace_id_release_all(struct coresight_trace_id_map *id_map)
{
	unsigned long flags;
	int cpu;

	raw_spin_lock_irqsave(&id_map->lock, flags);
	bitmap_zero(id_map->used_ids, CORESIGHT_TRACE_IDS_MAX);
	for_each_possible_cpu(cpu)
		atomic_set(per_cpu_ptr(id_map->cpu_map, cpu), 0);
	raw_spin_unlock_irqrestore(&id_map->lock, flags);
	DUMP_ID_MAP(id_map);
}

static int _coresight_trace_id_get_cpu_id(int cpu, struct coresight_trace_id_map *id_map)
{
	unsigned long flags;
	int id;

	raw_spin_lock_irqsave(&id_map->lock, flags);

	/* check for existing allocation for this CPU */
	id = _coresight_trace_id_read_cpu_id(cpu, id_map);
	if (id)
		goto get_cpu_id_out_unlock;

	/*
	 * Find a new ID.
	 *
	 * Use legacy values where possible in the dynamic trace ID allocator to
	 * allow older tools to continue working if they are not upgraded at the
	 * same time as the kernel drivers.
	 *
	 * If the generated legacy ID is invalid, or not available then the next
	 * available dynamic ID will be used.
	 */
	id = coresight_trace_id_alloc_new_id(id_map,
					     CORESIGHT_LEGACY_CPU_TRACE_ID(cpu),
					     TRACE_ID_ANY);
	if (!IS_VALID_CS_TRACE_ID(id))
		goto get_cpu_id_out_unlock;

	/* allocate the new id to the cpu */
	atomic_set(per_cpu_ptr(id_map->cpu_map, cpu), id);

get_cpu_id_out_unlock:
	raw_spin_unlock_irqrestore(&id_map->lock, flags);

	DUMP_ID_CPU(cpu, id);
	DUMP_ID_MAP(id_map);
	return id;
}

static void _coresight_trace_id_put_cpu_id(int cpu, struct coresight_trace_id_map *id_map)
{
	unsigned long flags;
	int id;

	/* check for existing allocation for this CPU */
	id = _coresight_trace_id_read_cpu_id(cpu, id_map);
	if (!id)
		return;

	raw_spin_lock_irqsave(&id_map->lock, flags);

	coresight_trace_id_free(id, id_map);
	atomic_set(per_cpu_ptr(id_map->cpu_map, cpu), 0);

	raw_spin_unlock_irqrestore(&id_map->lock, flags);
	DUMP_ID_CPU(cpu, id);
	DUMP_ID_MAP(id_map);
}

static int coresight_trace_id_map_get_system_id(struct coresight_trace_id_map *id_map,
					int preferred_id, unsigned int traceid_flags)
{
	unsigned long flags;
	int id;

	raw_spin_lock_irqsave(&id_map->lock, flags);
	id = coresight_trace_id_alloc_new_id(id_map, preferred_id, traceid_flags);
	raw_spin_unlock_irqrestore(&id_map->lock, flags);

	DUMP_ID(id);
	DUMP_ID_MAP(id_map);
	return id;
}

static void coresight_trace_id_map_put_system_id(struct coresight_trace_id_map *id_map, int id)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&id_map->lock, flags);
	coresight_trace_id_free(id, id_map);
	raw_spin_unlock_irqrestore(&id_map->lock, flags);

	DUMP_ID(id);
	DUMP_ID_MAP(id_map);
}

/* API functions */

int coresight_trace_id_get_cpu_id(int cpu)
{
	return _coresight_trace_id_get_cpu_id(cpu, &id_map_default);
}
EXPORT_SYMBOL_GPL(coresight_trace_id_get_cpu_id);

int coresight_trace_id_get_cpu_id_map(int cpu, struct coresight_trace_id_map *id_map)
{
	return _coresight_trace_id_get_cpu_id(cpu, id_map);
}
EXPORT_SYMBOL_GPL(coresight_trace_id_get_cpu_id_map);

void coresight_trace_id_put_cpu_id(int cpu)
{
	_coresight_trace_id_put_cpu_id(cpu, &id_map_default);
}
EXPORT_SYMBOL_GPL(coresight_trace_id_put_cpu_id);

void coresight_trace_id_put_cpu_id_map(int cpu, struct coresight_trace_id_map *id_map)
{
	_coresight_trace_id_put_cpu_id(cpu, id_map);
}
EXPORT_SYMBOL_GPL(coresight_trace_id_put_cpu_id_map);

int coresight_trace_id_read_cpu_id(int cpu)
{
	return _coresight_trace_id_read_cpu_id(cpu, &id_map_default);
}
EXPORT_SYMBOL_GPL(coresight_trace_id_read_cpu_id);

int coresight_trace_id_read_cpu_id_map(int cpu, struct coresight_trace_id_map *id_map)
{
	return _coresight_trace_id_read_cpu_id(cpu, id_map);
}
EXPORT_SYMBOL_GPL(coresight_trace_id_read_cpu_id_map);

int coresight_trace_id_get_system_id(void)
{
	/* prefer odd IDs for system components to avoid legacy CPU IDS */
	return coresight_trace_id_map_get_system_id(&id_map_default, 0,
			TRACE_ID_PREFER_ODD);
}
EXPORT_SYMBOL_GPL(coresight_trace_id_get_system_id);

int coresight_trace_id_get_static_system_id(int trace_id)
{
	return coresight_trace_id_map_get_system_id(&id_map_default,
			trace_id, TRACE_ID_REQ_STATIC);
}
EXPORT_SYMBOL_GPL(coresight_trace_id_get_static_system_id);

void coresight_trace_id_put_system_id(int id)
{
	coresight_trace_id_map_put_system_id(&id_map_default, id);
}
EXPORT_SYMBOL_GPL(coresight_trace_id_put_system_id);

void coresight_trace_id_perf_start(struct coresight_trace_id_map *id_map)
{
	atomic_inc(&id_map->perf_cs_etm_session_active);
	PERF_SESSION(atomic_read(&id_map->perf_cs_etm_session_active));
}
EXPORT_SYMBOL_GPL(coresight_trace_id_perf_start);

void coresight_trace_id_perf_stop(struct coresight_trace_id_map *id_map)
{
	if (!atomic_dec_return(&id_map->perf_cs_etm_session_active))
		coresight_trace_id_release_all(id_map);
	PERF_SESSION(atomic_read(&id_map->perf_cs_etm_session_active));
}
EXPORT_SYMBOL_GPL(coresight_trace_id_perf_stop);
