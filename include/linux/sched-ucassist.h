#ifndef _LINUX_SCHED_UCASSIST_H
#define _LINUX_SCHED_UCASSIST_H

#include <linux/sched.h>
#include <linux/cgroup.h>

#define DISPLAY_UCFLAG	0x001
#define GPU_UCFLAG		0x002

extern bool ucassist_restrict_enabled;

#ifdef CONFIG_UCLAMP_TASK

int __ucassist_get_task_uclamp_data(const char *comm, 
				unsigned int *min, unsigned int *max,
				unsigned int flags);

static inline int ucassist_get_task_uclamp_data(struct task_struct *p, 
				unsigned int *min, unsigned int *max, 
				unsigned int flags)
{
	char comm[TASK_COMM_LEN];
	get_task_comm(comm, p);
	return __ucassist_get_task_uclamp_data(comm, min, max, flags);
}

static inline bool __ucassist_task_is_target(const char *comm,
				unsigned int flags)
{
	unsigned int min, max;
	return !__ucassist_get_task_uclamp_data(comm, &min, &max, flags);
}

static inline bool ucassist_task_is_target(struct task_struct *p,
				unsigned int flags)
{
	unsigned int min, max;
	return !ucassist_get_task_uclamp_data(p, &min, &max, flags);
}

void ucassist_sched_uclamp_set(unsigned int min, unsigned int max);

int setscheduler_task_ucassist(struct task_struct *p, unsigned int flags);

#ifdef CONFIG_UCLAMP_TASK_GROUP
int ucassist_init_cpu_values(struct cgroup_subsys_state *css);
#else
int ucassist_init_cpu_values(struct cgroup_subsys_state *css) { }
#endif

#else /* CONFIG_UCLAMP_TASK */

static inline int __ucassist_get_task_uclamp_data(const char *comm, 
				unsigned int *min, unsigned int *max,
				unsigned int flags) 
{ 
	return 0;
}

static inline int ucassist_get_task_uclamp_data(struct task_struct *p, 
				unsigned int *min, unsigned int *max,
				unsigned int flags) 
{
	return 0;
}

static inline bool __ucassist_task_is_target(const char *comm,
				unsigned int flags)
{
	return false;
}

static inline bool ucassist_task_is_target(struct task_struct *p,
				unsigned int flags)
{
	return false;
}

static inline void ucassist_sched_uclamp_set(struct task_struct *p) 
{	
}

static inline int setscheduler_task_ucassist(struct task_struct *p,
				unsigned int flags) 
{
	return 0;
}

#endif /* CONFIG_UCLAMP_TASK */

#endif /* _LINUX_SCHED_UCASSIST_H */