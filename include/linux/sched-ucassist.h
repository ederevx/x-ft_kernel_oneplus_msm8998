#ifndef _LINUX_SCHED_UCASSIST_H
#define _LINUX_SCHED_UCASSIST_H

#include <linux/sched.h>
#include <linux/cgroup.h>

#ifdef CONFIG_UCLAMP_TASK

int task_ucassist_get_uclamp_data(struct task_struct *p, 
				unsigned int *min, unsigned int *max);

void setscheduler_task_ucassist(struct task_struct *p);

#ifdef CONFIG_UCLAMP_TASK_GROUP
int cpu_ucassist_init_values(struct cgroup_subsys_state *css);
#else
int cpu_ucassist_init_values(struct cgroup_subsys_state *css) { }
#endif

#else /* CONFIG_UCLAMP_TASK */

static inline int task_ucassist_get_uclamp_data(struct task_struct *p, 
				unsigned int *min, unsigned int *max) { }

static inline void setscheduler_task_ucassist(struct task_struct *p) { }

#endif /* CONFIG_UCLAMP_TASK */

#endif /* _LINUX_SCHED_UCASSIST_H */