#ifndef _LINUX_SCHED_UCASSIST_H
#define _LINUX_SCHED_UCASSIST_H

#include <linux/sched.h>
#include <linux/cgroup.h>

extern bool ucassist_restrict_enabled;

#ifdef CONFIG_UCLAMP_TASK

int ucassist_get_task_uclamp_data(struct task_struct *p, 
				unsigned int *min, unsigned int *max);

void ucassist_sched_uclamp_set(unsigned int min, unsigned int max);

int setscheduler_task_ucassist(struct task_struct *p);

#ifdef CONFIG_UCLAMP_TASK_GROUP
int ucassist_init_cpu_values(struct cgroup_subsys_state *css);
#else
int ucassist_init_cpu_values(struct cgroup_subsys_state *css) { }
#endif

#else /* CONFIG_UCLAMP_TASK */

static inline int ucassist_get_task_uclamp_data(struct task_struct *p, 
				unsigned int *min, unsigned int *max) { }

static inline void ucassist_sched_uclamp_set(struct task_struct *p) { }

static inline int setscheduler_task_ucassist(struct task_struct *p) { }

#endif /* CONFIG_UCLAMP_TASK */

#endif /* _LINUX_SCHED_UCASSIST_H */