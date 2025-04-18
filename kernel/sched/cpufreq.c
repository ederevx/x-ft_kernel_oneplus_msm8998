/*
 * Scheduler code and data structures related to cpufreq.
 *
 * Copyright (C) 2016, Intel Corporation
 * Author: Rafael J. Wysocki <rafael.j.wysocki@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/cpufreq.h>

#include "sched.h"

DEFINE_PER_CPU(struct update_util_data *, cpufreq_update_util_data);

/**
 * cpufreq_add_update_util_hook - Populate the CPU's update_util_data pointer.
 * @cpu: The CPU to set the pointer for.
 * @data: New pointer value.
 * @func: Callback function to set for the CPU.
 *
 * Set and publish the update_util_data pointer for the given CPU.
 *
 * The update_util_data pointer of @cpu is set to @data and the callback
 * function pointer in the target struct update_util_data is set to @func.
 * That function will be called by cpufreq_update_util() from RCU-sched
 * read-side critical sections, so it must not sleep.  @data will always be
 * passed to it as the first argument which allows the function to get to the
 * target update_util_data structure and its container.
 *
 * The update_util_data pointer of @cpu must be NULL when this function is
 * called or it will WARN() and return with no effect.
 */
void cpufreq_add_update_util_hook(int cpu, struct update_util_data *data,
			void (*func)(struct update_util_data *data, u64 time,
				     unsigned int flags))
{
	if (WARN_ON(!data || !func))
		return;

	if (WARN_ON(per_cpu(cpufreq_update_util_data, cpu)))
		return;

	data->func = func;
	rcu_assign_pointer(per_cpu(cpufreq_update_util_data, cpu), data);
}
EXPORT_SYMBOL_GPL(cpufreq_add_update_util_hook);

/**
 * cpufreq_remove_update_util_hook - Clear the CPU's update_util_data pointer.
 * @cpu: The CPU to clear the pointer for.
 *
 * Clear the update_util_data pointer for the given CPU.
 *
 * Callers must use RCU-sched callbacks to free any memory that might be
 * accessed via the old update_util_data pointer or invoke synchronize_sched()
 * right after this function to avoid use-after-free.
 */
void cpufreq_remove_update_util_hook(int cpu)
{
	rcu_assign_pointer(per_cpu(cpufreq_update_util_data, cpu), NULL);
}
EXPORT_SYMBOL_GPL(cpufreq_remove_update_util_hook);

/**
 * cpufreq_this_cpu_can_update - Check if cpufreq policy can be updated.
 * @policy: cpufreq policy to check.
 *
 * Return 'true' if:
 * - the local and remote CPUs share @policy,
 * - dvfs_possible_from_any_cpu is set in @policy and the local CPU is not going
 *   offline (in which case it is not expected to run cpufreq updates any more).
 */
bool cpufreq_this_cpu_can_update(struct cpufreq_policy *policy)
{
	return cpumask_test_cpu(smp_processor_id(), policy->cpus) ||
		(policy->dvfs_possible_from_any_cpu &&
		 rcu_dereference_sched(*this_cpu_ptr(&cpufreq_update_util_data)));
}

/**
 * cpufreq_request_update_util - Request all CPUs to update their utilization.
 *
 * This is a function that can be called outside of the core scheduler. 
 * 
 * It allows scheduler features that do not deal directly with scheduling but can 
 * affect perceived CPU utilization, such as UCLAMP, to request CPUs to update their
 * utilization immediately without needing to call on cpufreq_update_util themselves
 * for each registered CPU.
 */
static void cpufreq_update_util_irq_fn(struct irq_work *irq_work)
{
	struct rq *rq;
	int cpu;

	for_each_online_cpu(cpu) {
		rq = cpu_rq(cpu);
		raw_spin_lock(&rq->lock);
		cpufreq_update_util(rq, 0);
		raw_spin_unlock(&rq->lock);
	}
}
DEFINE_IRQ_WORK(cpufreq_update_util_irq_work, cpufreq_update_util_irq_fn);

void cpufreq_request_update_util(void)
{
	irq_work_queue(&cpufreq_update_util_irq_work);
}