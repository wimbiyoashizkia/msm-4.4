
#include <linux/sched.h>
#include <linux/sched/sysctl.h>
#include <linux/sched/rt.h>
#include <linux/sched/smt.h>
#include <linux/sched/deadline.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/stop_machine.h>
#include <linux/irq_work.h>
#include <linux/tick.h>
#include <linux/slab.h>

#include "cpupri.h"
#include "cpudeadline.h"
#include "cpuacct.h"
#include "features.h"

#ifdef CONFIG_SCHED_DEBUG
# define SCHED_WARN_ON(x)	WARN_ONCE(x, #x)
#else
# define SCHED_WARN_ON(x)	({ (void)(x), 0; })
#endif

struct rq;
struct cpuidle_state;

/* task_struct::on_rq states: */
#define TASK_ON_RQ_QUEUED	1
#define TASK_ON_RQ_MIGRATING	2

extern __read_mostly int scheduler_running;

extern unsigned long calc_load_update;
extern atomic_long_t calc_load_tasks;

extern void calc_global_load_tick(struct rq *this_rq);

extern long calc_load_fold_active(struct rq *this_rq);

#ifdef CONFIG_SMP
extern void update_cpu_load_active(struct rq *this_rq);
extern void check_for_migration(struct rq *rq, struct task_struct *p);
#else
static inline void update_cpu_load_active(struct rq *this_rq) { }
static inline void check_for_migration(struct rq *rq, struct task_struct *p) { }
#endif

extern bool energy_aware(void);

/*
 * Helpers for converting nanosecond timing to jiffy resolution
 */
#define NS_TO_JIFFIES(TIME)	((unsigned long)(TIME) / (NSEC_PER_SEC / HZ))

/*
 * Increase resolution of nice-level calculations for 64-bit architectures.
 * The extra resolution improves shares distribution and load balancing of
 * low-weight task groups (eg. nice +19 on an autogroup), deeper taskgroup
 * hierarchies, especially on larger systems. This is not a user-visible change
 * and does not change the user-interface for setting shares/weights.
 *
 * We increase resolution only if we have enough bits to allow this increased
 * resolution (i.e. BITS_PER_LONG > 32). The costs for increasing resolution
 * when BITS_PER_LONG <= 32 are pretty high and the returns do not justify the
 * increased costs.
 */
#if 0 /* BITS_PER_LONG > 32 -- currently broken: it increases power usage under light load  */
# define SCHED_LOAD_RESOLUTION	10
# define scale_load(w)		((w) << SCHED_LOAD_RESOLUTION)
# define scale_load_down(w)	((w) >> SCHED_LOAD_RESOLUTION)
#else
# define SCHED_LOAD_RESOLUTION	0
# define scale_load(w)		(w)
# define scale_load_down(w)	(w)
#endif

#define SCHED_LOAD_SHIFT	(10 + SCHED_LOAD_RESOLUTION)
#define SCHED_LOAD_SCALE	(1L << SCHED_LOAD_SHIFT)

#define NICE_0_LOAD		SCHED_LOAD_SCALE
#define NICE_0_SHIFT		SCHED_LOAD_SHIFT

/*
 * Single value that decides SCHED_DEADLINE internal math precision.
 * 10 -> just above 1us
 * 9  -> just above 0.5us
 */
#define DL_SCALE (10)

/*
 * These are the 'tuning knobs' of the scheduler:
 */

/*
 * single value that denotes runtime == period, ie unlimited time.
 */
#define RUNTIME_INF	((u64)~0ULL)

static inline int idle_policy(int policy)
{
	return policy == SCHED_IDLE;
}
static inline int fair_policy(int policy)
{
	return policy == SCHED_NORMAL || policy == SCHED_BATCH;
}

static inline int rt_policy(int policy)
{
	return policy == SCHED_FIFO || policy == SCHED_RR;
}

static inline int dl_policy(int policy)
{
	return policy == SCHED_DEADLINE;
}
static inline bool valid_policy(int policy)
{
	return idle_policy(policy) || fair_policy(policy) ||
		rt_policy(policy) || dl_policy(policy);
}

static inline int task_has_rt_policy(struct task_struct *p)
{
	return rt_policy(p->policy);
}

static inline int task_has_dl_policy(struct task_struct *p)
{
	return dl_policy(p->policy);
}

/*
 * Tells if entity @a should preempt entity @b.
 */
static inline bool
dl_entity_preempt(struct sched_dl_entity *a, struct sched_dl_entity *b)
{
	return dl_time_before(a->deadline, b->deadline);
}

/*
 * This is the priority-queue data structure of the RT scheduling class:
 */
struct rt_prio_array {
	DECLARE_BITMAP(bitmap, MAX_RT_PRIO+1); /* include 1 bit for delimiter */
	struct list_head queue[MAX_RT_PRIO];
};

struct rt_bandwidth {
	/* nests inside the rq lock: */
	raw_spinlock_t		rt_runtime_lock;
	ktime_t			rt_period;
	u64			rt_runtime;
	struct hrtimer		rt_period_timer;
	unsigned int		rt_period_active;
};

void __dl_clear_params(struct task_struct *p);

/*
 * To keep the bandwidth of -deadline tasks and groups under control
 * we need some place where:
 *  - store the maximum -deadline bandwidth of the system (the group);
 *  - cache the fraction of that bandwidth that is currently allocated.
 *
 * This is all done in the data structure below. It is similar to the
 * one used for RT-throttling (rt_bandwidth), with the main difference
 * that, since here we are only interested in admission control, we
 * do not decrease any runtime while the group "executes", neither we
 * need a timer to replenish it.
 *
 * With respect to SMP, the bandwidth is given on a per-CPU basis,
 * meaning that:
 *  - dl_bw (< 100%) is the bandwidth of the system (group) on each CPU;
 *  - dl_total_bw array contains, in the i-eth element, the currently
 *    allocated bandwidth on the i-eth CPU.
 * Moreover, groups consume bandwidth on each CPU, while tasks only
 * consume bandwidth on the CPU they're running on.
 * Finally, dl_total_bw_cpu is used to cache the index of dl_total_bw
 * that will be shown the next time the proc or cgroup controls will
 * be red. It on its turn can be changed by writing on its own
 * control.
 */
struct dl_bandwidth {
	raw_spinlock_t dl_runtime_lock;
	u64 dl_runtime;
	u64 dl_period;
};

static inline int dl_bandwidth_enabled(void)
{
	return sysctl_sched_rt_runtime >= 0;
}

extern struct dl_bw *dl_bw_of(int i);

struct dl_bw {
	raw_spinlock_t lock;
	u64 bw, total_bw;
};

static inline
void __dl_clear(struct dl_bw *dl_b, u64 tsk_bw)
{
	dl_b->total_bw -= tsk_bw;
}

static inline
void __dl_add(struct dl_bw *dl_b, u64 tsk_bw)
{
	dl_b->total_bw += tsk_bw;
}

static inline
bool __dl_overflow(struct dl_bw *dl_b, int cpus, u64 old_bw, u64 new_bw)
{
	return dl_b->bw != -1 &&
	       dl_b->bw * cpus < dl_b->total_bw - old_bw + new_bw;
}

extern struct mutex sched_domains_mutex;

#ifdef CONFIG_CGROUP_SCHED

#include <linux/cgroup.h>

struct cfs_rq;
struct rt_rq;

extern struct list_head task_groups;

struct cfs_bandwidth {
#ifdef CONFIG_CFS_BANDWIDTH
	raw_spinlock_t lock;
	ktime_t period;
	u64 quota, runtime;
	s64 hierarchical_quota;

	short idle, period_active;
	struct hrtimer period_timer, slack_timer;
	struct list_head throttled_cfs_rq;

	/* statistics */
	int nr_periods, nr_throttled;
	u64 throttled_time;

	bool distribute_running;
#endif
};

/* task group related information */
struct task_group {
	struct cgroup_subsys_state css;

#ifdef CONFIG_SCHED_HMP
	bool upmigrate_discouraged;
#endif

#ifdef CONFIG_FAIR_GROUP_SCHED
	/* schedulable entities of this group on each cpu */
	struct sched_entity **se;
	/* runqueue "owned" by this group on each cpu */
	struct cfs_rq **cfs_rq;
	unsigned long shares;

#ifdef	CONFIG_SMP
	atomic_long_t load_avg;
#endif
#endif

#ifdef CONFIG_RT_GROUP_SCHED
	struct sched_rt_entity **rt_se;
	struct rt_rq **rt_rq;

	struct rt_bandwidth rt_bandwidth;
#endif

	struct rcu_head rcu;
	struct list_head list;

	struct task_group *parent;
	struct list_head siblings;
	struct list_head children;

#ifdef CONFIG_SCHED_AUTOGROUP
	struct autogroup *autogroup;
#endif

	struct cfs_bandwidth cfs_bandwidth;

#ifdef CONFIG_UCLAMP_TASK_GROUP
	/* The two decimal precision [%] value requested from user-space */
	unsigned int		uclamp_pct[UCLAMP_CNT];
	/* Clamp values requested for a task group */
	struct uclamp_se	uclamp_req[UCLAMP_CNT];
	/* Effective clamp values used for a task group */
	struct uclamp_se	uclamp[UCLAMP_CNT];
	/* Latency-sensitive flag used for a task group */
	unsigned int		latency_sensitive;
#endif

};

#ifdef CONFIG_FAIR_GROUP_SCHED
#define ROOT_TASK_GROUP_LOAD	NICE_0_LOAD

/*
 * A weight of 0 or 1 can cause arithmetics problems.
 * A weight of a cfs_rq is the sum of weights of which entities
 * are queued on this cfs_rq, so a weight of a entity should not be
 * too large, so as the shares value of a task group.
 * (The default weight is 1024 - so there's no practical
 *  limitation from this.)
 */
#define MIN_SHARES	(1UL <<  1)
#define MAX_SHARES	(1UL << 18)
#endif

typedef int (*tg_visitor)(struct task_group *, void *);

extern int walk_tg_tree_from(struct task_group *from,
			     tg_visitor down, tg_visitor up, void *data);

/*
 * Iterate the full tree, calling @down when first entering a node and @up when
 * leaving it for the final time.
 *
 * Caller must hold rcu_lock or sufficient equivalent.
 */
static inline int walk_tg_tree(tg_visitor down, tg_visitor up, void *data)
{
	return walk_tg_tree_from(&root_task_group, down, up, data);
}

extern int tg_nop(struct task_group *tg, void *data);

extern void free_fair_sched_group(struct task_group *tg);
extern int alloc_fair_sched_group(struct task_group *tg, struct task_group *parent);
extern void unregister_fair_sched_group(struct task_group *tg);
extern void init_tg_cfs_entry(struct task_group *tg, struct cfs_rq *cfs_rq,
			struct sched_entity *se, int cpu,
			struct sched_entity *parent);
extern void init_cfs_bandwidth(struct cfs_bandwidth *cfs_b);
extern int sched_group_set_shares(struct task_group *tg, unsigned long shares);

extern void __refill_cfs_bandwidth_runtime(struct cfs_bandwidth *cfs_b);
extern void start_cfs_bandwidth(struct cfs_bandwidth *cfs_b);
extern void unthrottle_cfs_rq(struct cfs_rq *cfs_rq);

extern void free_rt_sched_group(struct task_group *tg);
extern int alloc_rt_sched_group(struct task_group *tg, struct task_group *parent);
extern void init_tg_rt_entry(struct task_group *tg, struct rt_rq *rt_rq,
		struct sched_rt_entity *rt_se, int cpu,
		struct sched_rt_entity *parent);

extern struct task_group *sched_create_group(struct task_group *parent);
extern void sched_online_group(struct task_group *tg,
			       struct task_group *parent);
extern void sched_destroy_group(struct task_group *tg);
extern void sched_offline_group(struct task_group *tg);

extern void sched_move_task(struct task_struct *tsk);

#ifdef CONFIG_FAIR_GROUP_SCHED
extern int sched_group_set_shares(struct task_group *tg, unsigned long shares);

#ifdef CONFIG_SMP
extern void set_task_rq_fair(struct sched_entity *se,
			     struct cfs_rq *prev, struct cfs_rq *next);
#else /* !CONFIG_SMP */
static inline void set_task_rq_fair(struct sched_entity *se,
			     struct cfs_rq *prev, struct cfs_rq *next) { }
#endif /* CONFIG_SMP */
#endif /* CONFIG_FAIR_GROUP_SCHED */

extern struct task_group *css_tg(struct cgroup_subsys_state *css);
#else /* CONFIG_CGROUP_SCHED */

struct cfs_bandwidth { };

#endif	/* CONFIG_CGROUP_SCHED */

#ifdef CONFIG_SCHED_HMP

#define NUM_TRACKED_WINDOWS 2
#define NUM_LOAD_INDICES 1000

struct hmp_sched_stats {
	int nr_big_tasks;
	u64 cumulative_runnable_avg;
	u64 pred_demands_sum;
};

struct load_subtractions {
	u64 window_start;
	u64 subs;
	u64 new_subs;
};

struct group_cpu_time {
	u64 curr_runnable_sum;
	u64 prev_runnable_sum;
	u64 nt_curr_runnable_sum;
	u64 nt_prev_runnable_sum;
};

struct sched_cluster {
	raw_spinlock_t load_lock;
	struct list_head list;
	struct cpumask cpus;
	int id;
	int max_power_cost;
	int min_power_cost;
	int max_possible_capacity;
	int capacity;
	int efficiency; /* Differentiate cpus with different IPC capability */
	int load_scale_factor;
	unsigned int exec_scale_factor;
	/*
	 * max_freq = user maximum
	 * max_mitigated_freq = thermal defined maximum
	 * max_possible_freq = maximum supported by hardware
	 */
	unsigned int cur_freq, max_freq, max_mitigated_freq, min_freq;
	unsigned int max_possible_freq;
	bool freq_init_done;
	int dstate, dstate_wakeup_latency, dstate_wakeup_energy;
	unsigned int static_cluster_pwr_cost;
	int notifier_sent;
	bool wake_up_idle;
	atomic64_t last_cc_update;
	atomic64_t cycles;
};

extern unsigned long all_cluster_ids[];

static inline int cluster_first_cpu(struct sched_cluster *cluster)
{
	return cpumask_first(&cluster->cpus);
}

struct related_thread_group {
	int id;
	raw_spinlock_t lock;
	struct list_head tasks;
	struct list_head list;
	struct sched_cluster *preferred_cluster;
	struct rcu_head rcu;
	u64 last_update;
};

extern struct list_head cluster_head;
extern struct sched_cluster *sched_cluster[NR_CPUS];

struct cpu_cycle {
	u64 cycles;
	u64 time;
};

#define for_each_sched_cluster(cluster) \
	list_for_each_entry_rcu(cluster, &cluster_head, list)

extern unsigned int sched_disable_window_stats;
#endif /* CONFIG_SCHED_HMP */

/* CFS-related fields in a runqueue */
struct cfs_rq {
	struct load_weight load;
	unsigned int nr_running, h_nr_running;

	u64 exec_clock;
	u64 min_vruntime;
#ifndef CONFIG_64BIT
	u64 min_vruntime_copy;
#endif

	struct rb_root tasks_timeline;
	struct rb_node *rb_leftmost;

	/*
	 * 'curr' points to currently running entity on this cfs_rq.
	 * It is set to NULL otherwise (i.e when none are currently running).
	 */
	struct sched_entity *curr, *next, *last, *skip;

#ifdef	CONFIG_SCHED_DEBUG
	unsigned int nr_spread_over;
#endif

#ifdef CONFIG_SMP
	/*
	 * CFS load tracking
	 */
	struct sched_avg avg;
	u64 runnable_load_sum;
	unsigned long runnable_load_avg;
#ifdef CONFIG_FAIR_GROUP_SCHED
	unsigned long tg_load_avg_contrib;
	unsigned long propagate_avg;
#endif
	atomic_long_t removed_load_avg, removed_util_avg;
#ifndef CONFIG_64BIT
	u64 load_last_update_time_copy;
#endif

#ifdef CONFIG_FAIR_GROUP_SCHED
	/*
	 *   h_load = weight * f(tg)
	 *
	 * Where f(tg) is the recursive weight fraction assigned to
	 * this group.
	 */
	unsigned long h_load;
	u64 last_h_load_update;
	struct sched_entity *h_load_next;
#endif /* CONFIG_FAIR_GROUP_SCHED */
#endif /* CONFIG_SMP */

#ifdef CONFIG_FAIR_GROUP_SCHED
	struct rq *rq;	/* cpu runqueue to which this cfs_rq is attached */

	/*
	 * leaf cfs_rqs are those that hold tasks (lowest schedulable entity in
	 * a hierarchy). Non-leaf lrqs hold other higher schedulable entities
	 * (like users, containers etc.)
	 *
	 * leaf_cfs_rq_list ties together list of leaf cfs_rq's in a cpu. This
	 * list is used during load balance.
	 */
	int on_list;
	struct list_head leaf_cfs_rq_list;
	struct task_group *tg;	/* group that "owns" this runqueue */

#ifdef CONFIG_SCHED_WALT
	u64 cumulative_runnable_avg;
#endif

#ifdef CONFIG_CFS_BANDWIDTH

#ifdef CONFIG_SCHED_HMP
	struct hmp_sched_stats hmp_stats;
#endif

	int runtime_enabled;
	s64 runtime_remaining;

	u64 throttled_clock, throttled_clock_task;
	u64 throttled_clock_task_time;
	int throttled, throttle_count, throttle_uptodate;
	struct list_head throttled_list;
#endif /* CONFIG_CFS_BANDWIDTH */
#endif /* CONFIG_FAIR_GROUP_SCHED */
};

static inline int rt_bandwidth_enabled(void)
{
	return sysctl_sched_rt_runtime >= 0;
}

/* RT IPI pull logic requires IRQ_WORK */
#if defined(CONFIG_IRQ_WORK) && defined(CONFIG_SMP)
# define HAVE_RT_PUSH_IPI
#endif

/* Real-Time classes' related field in a runqueue: */
struct rt_rq {
	struct rt_prio_array active;
	unsigned int rt_nr_running;
#if defined CONFIG_SMP || defined CONFIG_RT_GROUP_SCHED
	struct {
		int curr; /* highest queued rt task prio */
#ifdef CONFIG_SMP
		int next; /* next highest */
#endif
	} highest_prio;
#endif
#ifdef CONFIG_SMP
	unsigned long rt_nr_migratory;
	unsigned long rt_nr_total;
	int overloaded;
	struct plist_head pushable_tasks;

	struct sched_avg avg;

#endif /* CONFIG_SMP */
	int rt_queued;

	int rt_throttled;
	u64 rt_time;
	u64 rt_runtime;
	/* Nests inside the rq lock: */
	raw_spinlock_t rt_runtime_lock;

#ifdef CONFIG_RT_GROUP_SCHED
	unsigned long rt_nr_boosted;

	struct rq *rq;
	struct task_group *tg;
#endif
};

/* Deadline class' related fields in a runqueue */
struct dl_rq {
	/* runqueue is an rbtree, ordered by deadline */
	struct rb_root rb_root;
	struct rb_node *rb_leftmost;

	unsigned long dl_nr_running;

#ifdef CONFIG_SMP
	/*
	 * Deadline values of the currently executing and the
	 * earliest ready task on this rq. Caching these facilitates
	 * the decision wether or not a ready but not running task
	 * should migrate somewhere else.
	 */
	struct {
		u64 curr;
		u64 next;
	} earliest_dl;

	unsigned long dl_nr_migratory;
	int overloaded;

	/*
	 * Tasks on this rq that can be pushed away. They are kept in
	 * an rb-tree, ordered by tasks' deadlines, with caching
	 * of the leftmost (earliest deadline) element.
	 */
	struct rb_root pushable_dl_tasks_root;
	struct rb_node *pushable_dl_tasks_leftmost;
#else
	struct dl_bw dl_bw;
#endif
	/* This is the "average utilization" for this runqueue */
	s64 avg_bw;
};

#ifdef CONFIG_SMP

struct max_cpu_capacity {
	raw_spinlock_t lock;
	unsigned long val;
	int cpu;
};

/*
 * We add the notion of a root-domain which will be used to define per-domain
 * variables. Each exclusive cpuset essentially defines an island domain by
 * fully partitioning the member cpus from any other cpuset. Whenever a new
 * exclusive cpuset is created, we also create and attach a new root-domain
 * object.
 *
 */
struct root_domain {
	atomic_t refcount;
	atomic_t rto_count;
	struct rcu_head rcu;
	cpumask_var_t span;
	cpumask_var_t online;

	/* Indicate more than one runnable task for any CPU */
	bool overload;

	/* Indicate one or more cpus over-utilized (tipping point) */
	bool overutilized;

	/*
	 * The bit corresponding to a CPU gets set here if such CPU has more
	 * than one runnable -deadline task (as it is below for RT tasks).
	 */
	cpumask_var_t dlo_mask;
	atomic_t dlo_count;
	struct dl_bw dl_bw;
	struct cpudl cpudl;

#ifdef HAVE_RT_PUSH_IPI
	/*
	 * For IPI pull requests, loop across the rto_mask.
	 */
	struct irq_work rto_push_work;
	raw_spinlock_t rto_lock;
	/* These are only updated and read within rto_lock */
	int rto_loop;
	int rto_cpu;
	/* These atomics are updated outside of a lock */
	atomic_t rto_loop_next;
	atomic_t rto_loop_start;
#endif
	/*
	 * The "RT overload" flag: it gets set if a CPU has more than
	 * one runnable RT task.
	 */
	cpumask_var_t rto_mask;
	struct cpupri cpupri;

	/* Maximum cpu capacity in the system. */
	struct max_cpu_capacity max_cpu_capacity;

	/* First cpu with maximum and minimum original capacity */
	int max_cap_orig_cpu, min_cap_orig_cpu;
};

extern struct root_domain def_root_domain;
extern void sched_get_rd(struct root_domain *rd);
extern void sched_put_rd(struct root_domain *rd);

#ifdef HAVE_RT_PUSH_IPI
extern void rto_push_irq_work_func(struct irq_work *work);
#endif
#endif /* CONFIG_SMP */

#ifdef CONFIG_UCLAMP_TASK
/*
 * struct uclamp_bucket - Utilization clamp bucket
 * @value: utilization clamp value for tasks on this clamp bucket
 * @tasks: number of RUNNABLE tasks on this clamp bucket
 *
 * Keep track of how many tasks are RUNNABLE for a given utilization
 * clamp value.
 */
struct uclamp_bucket {
	unsigned long value : bits_per(SCHED_CAPACITY_SCALE);
	unsigned long tasks : BITS_PER_LONG - bits_per(SCHED_CAPACITY_SCALE);
};

/*
 * struct uclamp_rq - rq's utilization clamp
 * @value: currently active clamp values for a rq
 * @bucket: utilization clamp buckets affecting a rq
 *
 * Keep track of RUNNABLE tasks on a rq to aggregate their clamp values.
 * A clamp value is affecting a rq when there is at least one task RUNNABLE
 * (or actually running) with that value.
 *
 * There are up to UCLAMP_CNT possible different clamp values, currently there
 * are only two: minimum utilization and maximum utilization.
 *
 * All utilization clamping values are MAX aggregated, since:
 * - for util_min: we want to run the CPU at least at the max of the minimum
 *   utilization required by its currently RUNNABLE tasks.
 * - for util_max: we want to allow the CPU to run up to the max of the
 *   maximum utilization allowed by its currently RUNNABLE tasks.
 *
 * Since on each system we expect only a limited number of different
 * utilization clamp values (UCLAMP_BUCKETS), use a simple array to track
 * the metrics required to compute all the per-rq utilization clamp values.
 */
struct uclamp_rq {
	unsigned int value;
	struct uclamp_bucket bucket[UCLAMP_BUCKETS];
};

DECLARE_STATIC_KEY_FALSE(sched_uclamp_used);
#endif /* CONFIG_UCLAMP_TASK */

/*
 * This is the main, per-CPU runqueue data structure.
 *
 * Locking rule: those places that want to lock multiple runqueues
 * (such as the load balancing or the thread migration code), lock
 * acquire operations must be ordered by ascending &runqueue.
 */
struct rq {
	/* runqueue lock: */
	raw_spinlock_t lock;

	/*
	 * nr_running and cpu_load should be in the same cacheline because
	 * remote CPUs use both these fields when doing load calculation.
	 */
	unsigned int nr_running;
#ifdef CONFIG_NUMA_BALANCING
	unsigned int nr_numa_running;
	unsigned int nr_preferred_running;
#endif
	#define CPU_LOAD_IDX_MAX 5
	unsigned long cpu_load[CPU_LOAD_IDX_MAX];
	unsigned long last_load_update_tick;
	unsigned int misfit_task;
#ifdef CONFIG_NO_HZ_COMMON
	u64 nohz_stamp;
	unsigned long nohz_flags;
#endif
#ifdef CONFIG_NO_HZ_FULL
	unsigned long last_sched_tick;
#endif

#ifdef CONFIG_CPU_QUIET
	/* time-based average load */
	u64 nr_last_stamp;
	u64 nr_running_integral;
	seqcount_t ave_seqcnt;
#endif

#ifdef CONFIG_UCLAMP_TASK
	/* Utilization clamp values based on CPU's RUNNABLE tasks */
	struct uclamp_rq	uclamp[UCLAMP_CNT] ____cacheline_aligned;
	unsigned int		uclamp_flags;
#define UCLAMP_FLAG_IDLE 0x01
#endif

	/* capture load from *all* tasks on this cpu: */
	struct load_weight load;
	unsigned long nr_load_updates;
	u64 nr_switches;

	struct cfs_rq cfs;
	struct rt_rq rt;
	struct dl_rq dl;

#ifdef CONFIG_FAIR_GROUP_SCHED
	/* list of leaf cfs_rq on this cpu: */
	struct list_head leaf_cfs_rq_list;
	struct list_head *tmp_alone_branch;
#endif /* CONFIG_FAIR_GROUP_SCHED */

	/*
	 * This is part of a global counter where only the total sum
	 * over all CPUs matters. A task can increase this counter on
	 * one CPU and if it got migrated afterwards it may decrease
	 * it on another CPU. Always updated under the runqueue lock:
	 */
	unsigned long nr_uninterruptible;

	struct task_struct *curr, *idle, *stop;
	unsigned long next_balance;
	struct mm_struct *prev_mm;

	unsigned int clock_skip_update;
	u64 clock;
	u64 clock_task;

	atomic_t nr_iowait;

#ifdef CONFIG_SMP
	struct root_domain *rd;
	struct sched_domain *sd;

	unsigned long cpu_capacity;
	unsigned long cpu_capacity_orig;

	struct callback_head *balance_callback;

	unsigned char idle_balance;
	/* For active balancing */
	int active_balance;
	int push_cpu;
	struct task_struct *push_task;
	struct cpu_stop_work active_balance_work;
	/* cpu of this runqueue: */
	int cpu;
	int online;

	struct list_head cfs_tasks;

	u64 rt_avg;
	u64 age_stamp;
	u64 idle_stamp;
	u64 avg_idle;

	/* This is used to determine avg_idle's max value */
	u64 max_idle_balance_cost;
#endif

#ifdef CONFIG_SCHED_HMP
	struct sched_cluster *cluster;
	struct cpumask freq_domain_cpumask;
	struct hmp_sched_stats hmp_stats;

	int cstate, wakeup_latency, wakeup_energy;
	u64 window_start;
	u64 load_reported_window;
	unsigned long hmp_flags;

	u64 cur_irqload;
	u64 avg_irqload;
	u64 irqload_ts;
	unsigned int static_cpu_pwr_cost;
	struct task_struct *ed_task;
	struct cpu_cycle cc;
	u64 old_busy_time, old_busy_time_group;
	u64 old_estimated_time;
	u64 curr_runnable_sum;
	u64 prev_runnable_sum;
	u64 nt_curr_runnable_sum;
	u64 nt_prev_runnable_sum;
	struct group_cpu_time grp_time;
	struct load_subtractions load_subs[NUM_TRACKED_WINDOWS];
	DECLARE_BITMAP_ARRAY(top_tasks_bitmap,
			NUM_TRACKED_WINDOWS, NUM_LOAD_INDICES);
	u8 *top_tasks[NUM_TRACKED_WINDOWS];
	u8 curr_table;
	int prev_top;
	int curr_top;
#endif

#ifdef CONFIG_SCHED_WALT
	unsigned int cur_freq;
	u64 cumulative_runnable_avg;
	u64 window_start;
	u64 curr_runnable_sum;
	u64 prev_runnable_sum;
	u64 nt_curr_runnable_sum;
	u64 nt_prev_runnable_sum;
	u64 cur_irqload;
	u64 avg_irqload;
	u64 irqload_ts;
	u64 cum_window_demand;
#endif /* CONFIG_SCHED_WALT */

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
	u64 prev_irq_time;
#endif
#ifdef CONFIG_PARAVIRT
	u64 prev_steal_time;
#endif
#ifdef CONFIG_PARAVIRT_TIME_ACCOUNTING
	u64 prev_steal_time_rq;
#endif

	/* calc_load related fields */
	unsigned long calc_load_update;
	long calc_load_active;

#ifdef CONFIG_SCHED_HRTICK
#ifdef CONFIG_SMP
	int hrtick_csd_pending;
	struct call_single_data hrtick_csd;
#endif
	struct hrtimer hrtick_timer;
#endif

#ifdef CONFIG_SCHEDSTATS
	/* latency stats */
	struct sched_info rq_sched_info;
	unsigned long long rq_cpu_time;
	/* could above be rq->cfs_rq.exec_clock + rq->rt_rq.rt_runtime ? */

	/* sys_sched_yield() stats */
	unsigned int yld_count;

	/* schedule() stats */
	unsigned int sched_count;
	unsigned int sched_goidle;

	/* try_to_wake_up() stats */
	unsigned int ttwu_count;
	unsigned int ttwu_local;
#ifdef CONFIG_SMP
	struct eas_stats eas_stats;
#endif
#endif

#ifdef CONFIG_SMP
#if SCHED_FEAT_TTWU_QUEUE
	struct llist_head wake_list;
#endif
#endif

#ifdef CONFIG_CPU_IDLE
	/* Must be inspected within a rcu lock section */
	struct cpuidle_state *idle_state;
	int idle_state_idx;
#endif
};

static inline int cpu_of(struct rq *rq)
{
#ifdef CONFIG_SMP
	return rq->cpu;
#else
	return 0;
#endif
}

DECLARE_PER_CPU_SHARED_ALIGNED(struct rq, runqueues);

#define cpu_rq(cpu)		(&per_cpu(runqueues, (cpu)))
#define this_rq()		this_cpu_ptr(&runqueues)
#define task_rq(p)		cpu_rq(task_cpu(p))
#define cpu_curr(cpu)		(cpu_rq(cpu)->curr)
#define raw_rq()		raw_cpu_ptr(&runqueues)

static inline u64 __rq_clock_broken(struct rq *rq)
{
	return READ_ONCE(rq->clock);
}

static inline u64 rq_clock(struct rq *rq)
{
	lockdep_assert_held(&rq->lock);
	return rq->clock;
}

static inline u64 rq_clock_task(struct rq *rq)
{
	lockdep_assert_held(&rq->lock);
	return rq->clock_task;
}

#define RQCF_REQ_SKIP	0x01
#define RQCF_ACT_SKIP	0x02

static inline void rq_clock_skip_update(struct rq *rq, bool skip)
{
	lockdep_assert_held(&rq->lock);
	if (skip)
		rq->clock_skip_update |= RQCF_REQ_SKIP;
	else
		rq->clock_skip_update &= ~RQCF_REQ_SKIP;
}

#ifdef CONFIG_NUMA
enum numa_topology_type {
	NUMA_DIRECT,
	NUMA_GLUELESS_MESH,
	NUMA_BACKPLANE,
};
extern enum numa_topology_type sched_numa_topology_type;
extern int sched_max_numa_distance;
extern bool find_numa_distance(int distance);
#endif

#ifdef CONFIG_NUMA_BALANCING
/* The regions in numa_faults array from task_struct */
enum numa_faults_stats {
	NUMA_MEM = 0,
	NUMA_CPU,
	NUMA_MEMBUF,
	NUMA_CPUBUF
};
extern void sched_setnuma(struct task_struct *p, int node);
extern int migrate_task_to(struct task_struct *p, int cpu);
extern int migrate_swap(struct task_struct *, struct task_struct *);
#endif /* CONFIG_NUMA_BALANCING */

#ifdef CONFIG_SMP

static inline void
queue_balance_callback(struct rq *rq,
		       struct callback_head *head,
		       void (*func)(struct rq *rq))
{
	lockdep_assert_held(&rq->lock);

	if (unlikely(head->next))
		return;

	head->func = (void (*)(struct callback_head *))func;
	head->next = rq->balance_callback;
	rq->balance_callback = head;
}

#if SCHED_FEAT_TTWU_QUEUE
extern void sched_ttwu_pending(void);
#else
static inline void sched_ttwu_pending(void) { }
#endif

#define rcu_dereference_check_sched_domain(p) \
	rcu_dereference_check((p), \
			      lockdep_is_held(&sched_domains_mutex))

/*
 * The domain tree (rq->sd) is protected by RCU's quiescent state transition.
 * See detach_destroy_domains: synchronize_sched for details.
 *
 * The domain tree of any CPU may only be accessed from within
 * preempt-disabled sections.
 */
#define for_each_domain(cpu, __sd) \
	for (__sd = rcu_dereference_check_sched_domain(cpu_rq(cpu)->sd); \
			__sd; __sd = __sd->parent)

#define for_each_lower_domain(sd) for (; sd; sd = sd->child)

/**
 * highest_flag_domain - Return highest sched_domain containing flag.
 * @cpu:	The cpu whose highest level of sched domain is to
 *		be returned.
 * @flag:	The flag to check for the highest sched_domain
 *		for the given cpu.
 *
 * Returns the highest sched_domain of a cpu which contains the given flag.
 */
static inline struct sched_domain *highest_flag_domain(int cpu, int flag)
{
	struct sched_domain *sd, *hsd = NULL;

	for_each_domain(cpu, sd) {
		if (!(sd->flags & flag))
			break;
		hsd = sd;
	}

	return hsd;
}

static inline struct sched_domain *lowest_flag_domain(int cpu, int flag)
{
	struct sched_domain *sd;

	for_each_domain(cpu, sd) {
		if (sd->flags & flag)
			break;
	}

	return sd;
}

DECLARE_PER_CPU(struct sched_domain *, sd_llc);
DECLARE_PER_CPU(int, sd_llc_size);
DECLARE_PER_CPU(int, sd_llc_id);
DECLARE_PER_CPU(struct sched_domain *, sd_numa);
DECLARE_PER_CPU(struct sched_domain *, sd_busy);
DECLARE_PER_CPU(struct sched_domain *, sd_asym);
DECLARE_PER_CPU(struct sched_domain *, sd_ea);
DECLARE_PER_CPU(struct sched_domain *, sd_scs);

struct sched_group_capacity {
	atomic_t ref;
	/*
	 * CPU capacity of this group, SCHED_LOAD_SCALE being max capacity
	 * for a single CPU.
	 */
	unsigned long capacity;
	unsigned long max_capacity; /* Max per-cpu capacity in group */
	unsigned long min_capacity; /* Min per-CPU capacity in group */
	unsigned long next_update;
	int imbalance; /* XXX unrelated to capacity but shared group state */
	/*
	 * Number of busy cpus in this group.
	 */
	atomic_t nr_busy_cpus;

	unsigned long cpumask[0]; /* iteration mask */
};

struct sched_group {
	struct sched_group *next;	/* Must be a circular list */
	atomic_t ref;

	unsigned int group_weight;
	struct sched_group_capacity *sgc;
	const struct sched_group_energy *sge;

	/*
	 * The CPUs this group covers.
	 *
	 * NOTE: this field is variable length. (Allocated dynamically
	 * by attaching extra space to the end of the structure,
	 * depending on how many CPUs the kernel has booted up with)
	 */
	unsigned long cpumask[0];
};

static inline struct cpumask *sched_group_cpus(struct sched_group *sg)
{
	return to_cpumask(sg->cpumask);
}

/*
 * cpumask masking which cpus in the group are allowed to iterate up the domain
 * tree.
 */
static inline struct cpumask *sched_group_mask(struct sched_group *sg)
{
	return to_cpumask(sg->sgc->cpumask);
}

/**
 * group_first_cpu - Returns the first cpu in the cpumask of a sched_group.
 * @group: The group whose first cpu is to be returned.
 */
static inline unsigned int group_first_cpu(struct sched_group *group)
{
	return cpumask_first(sched_group_cpus(group));
}

extern int group_balance_cpu(struct sched_group *sg);

extern void flush_smp_call_function_from_idle(void);

#else /* !CONFIG_SMP: */
static inline void flush_smp_call_function_from_idle(void) { }
static inline void sched_ttwu_pending(void) { }
#endif

#include "stats.h"
#include "auto_group.h"

enum sched_boost_policy {
	SCHED_BOOST_NONE,
	SCHED_BOOST_ON_BIG,
	SCHED_BOOST_ON_ALL,
};

#ifdef CONFIG_SCHED_HMP

#define WINDOW_STATS_RECENT		0
#define WINDOW_STATS_MAX		1
#define WINDOW_STATS_MAX_RECENT_AVG	2
#define WINDOW_STATS_AVG		3
#define WINDOW_STATS_INVALID_POLICY	4

#define SCHED_UPMIGRATE_MIN_NICE 15
#define EXITING_TASK_MARKER	0xdeaddead

#define UP_MIGRATION		1
#define DOWN_MIGRATION		2
#define IRQLOAD_MIGRATION	3

extern struct mutex policy_mutex;
extern unsigned int sched_ravg_window;
extern unsigned int sched_disable_window_stats;
extern unsigned int max_possible_freq;
extern unsigned int min_max_freq;
extern unsigned int pct_task_load(struct task_struct *p);
extern unsigned int max_possible_efficiency;
extern unsigned int min_possible_efficiency;
extern unsigned int max_capacity;
extern unsigned int min_capacity;
extern unsigned int max_load_scale_factor;
extern unsigned int max_possible_capacity;
extern unsigned int min_max_possible_capacity;
extern unsigned int max_power_cost;
extern unsigned int sched_init_task_load_windows;
extern unsigned int up_down_migrate_scale_factor;
extern unsigned int sysctl_sched_restrict_cluster_spill;
extern unsigned int sched_pred_alert_load;
extern struct sched_cluster init_cluster;
extern unsigned int  __read_mostly sched_short_sleep_task_threshold;
extern unsigned int  __read_mostly sched_long_cpu_selection_threshold;
extern unsigned int  __read_mostly sched_big_waker_task_load;
extern unsigned int  __read_mostly sched_small_wakee_task_load;
extern unsigned int  __read_mostly sched_spill_load;
extern unsigned int  __read_mostly sched_upmigrate;
extern unsigned int  __read_mostly sched_downmigrate;
extern unsigned int  __read_mostly sched_load_granule;

extern void init_new_task_load(struct task_struct *p);
extern u64 sched_ktime_clock(void);
extern int got_boost_kick(void);
extern int register_cpu_cycle_counter_cb(struct cpu_cycle_counter_cb *cb);
extern void update_task_ravg(struct task_struct *p, struct rq *rq, int event,
						u64 wallclock, u64 irqtime);
extern bool early_detection_notify(struct rq *rq, u64 wallclock);
extern void clear_ed_task(struct task_struct *p, struct rq *rq);
extern void fixup_busy_time(struct task_struct *p, int new_cpu);
extern void clear_boost_kick(int cpu);
extern void clear_hmp_request(int cpu);
extern void mark_task_starting(struct task_struct *p);
extern void set_window_start(struct rq *rq);
extern void update_cluster_topology(void);
extern void note_task_waking(struct task_struct *p, u64 wallclock);
extern void set_task_last_switch_out(struct task_struct *p, u64 wallclock);
extern void init_clusters(void);
extern void reset_cpu_hmp_stats(int cpu, int reset_cra);
extern unsigned int max_task_load(void);
extern void sched_account_irqtime(int cpu, struct task_struct *curr,
				 u64 delta, u64 wallclock);
extern void sched_account_irqstart(int cpu, struct task_struct *curr,
				   u64 wallclock);
extern unsigned int cpu_temp(int cpu);
extern unsigned int nr_eligible_big_tasks(int cpu);
extern int update_preferred_cluster(struct related_thread_group *grp,
			struct task_struct *p, u32 old_load);
extern void set_preferred_cluster(struct related_thread_group *grp);
extern void add_new_task_to_grp(struct task_struct *new);
extern unsigned int update_freq_aggregate_threshold(unsigned int threshold);
extern void update_avg_burst(struct task_struct *p);
extern void update_avg(u64 *avg, u64 sample);

#define NO_BOOST 0
#define FULL_THROTTLE_BOOST 1
#define CONSERVATIVE_BOOST 2
#define RESTRAINED_BOOST 3

static inline struct sched_cluster *cpu_cluster(int cpu)
{
	return cpu_rq(cpu)->cluster;
}

static inline int cpu_capacity(int cpu)
{
	return cpu_rq(cpu)->cluster->capacity;
}

static inline int cpu_max_possible_capacity(int cpu)
{
	return cpu_rq(cpu)->cluster->max_possible_capacity;
}

static inline int cpu_load_scale_factor(int cpu)
{
	return cpu_rq(cpu)->cluster->load_scale_factor;
}

static inline int cpu_efficiency(int cpu)
{
	return cpu_rq(cpu)->cluster->efficiency;
}

static inline unsigned int cpu_cur_freq(int cpu)
{
	return cpu_rq(cpu)->cluster->cur_freq;
}

static inline unsigned int cpu_min_freq(int cpu)
{
	return cpu_rq(cpu)->cluster->min_freq;
}

static inline unsigned int cluster_max_freq(struct sched_cluster *cluster)
{
	/*
	 * Governor and thermal driver don't know the other party's mitigation
	 * voting. So struct cluster saves both and return min() for current
	 * cluster fmax.
	 */
	return min(cluster->max_mitigated_freq, cluster->max_freq);
}

static inline unsigned int cpu_max_freq(int cpu)
{
	return cluster_max_freq(cpu_rq(cpu)->cluster);
}

static inline unsigned int cpu_max_possible_freq(int cpu)
{
	return cpu_rq(cpu)->cluster->max_possible_freq;
}

static inline int same_cluster(int src_cpu, int dst_cpu)
{
	return cpu_rq(src_cpu)->cluster == cpu_rq(dst_cpu)->cluster;
}

static inline int cpu_max_power_cost(int cpu)
{
	return cpu_rq(cpu)->cluster->max_power_cost;
}

static inline int cpu_min_power_cost(int cpu)
{
	return cpu_rq(cpu)->cluster->min_power_cost;
}

static inline u32 cpu_cycles_to_freq(u64 cycles, u64 period)
{
	return div64_u64(cycles, period);
}

static inline bool hmp_capable(void)
{
	return max_possible_capacity != min_max_possible_capacity;
}

static inline bool is_max_capacity_cpu(int cpu)
{
	return cpu_max_possible_capacity(cpu) == max_possible_capacity;
}

static inline bool is_min_capacity_cpu(int cpu)
{
	return cpu_max_possible_capacity(cpu) == min_max_possible_capacity;
}

/*
 * 'load' is in reference to "best cpu" at its best frequency.
 * Scale that in reference to a given cpu, accounting for how bad it is
 * in reference to "best cpu".
 */
static inline u64 scale_load_to_cpu(u64 task_load, int cpu)
{
	u64 lsf = cpu_load_scale_factor(cpu);

	if (lsf != 1024) {
		task_load *= lsf;
		task_load /= 1024;
	}

	return task_load;
}

static inline unsigned int task_load(struct task_struct *p)
{
	return p->ravg.demand;
}

static inline void
inc_cumulative_runnable_avg(struct hmp_sched_stats *stats,
				 struct task_struct *p)
{
	u32 task_load;

	if (sched_disable_window_stats)
		return;

	task_load = sched_disable_window_stats ? 0 : p->ravg.demand;

	stats->cumulative_runnable_avg += task_load;
	stats->pred_demands_sum += p->ravg.pred_demand;
}

static inline void
dec_cumulative_runnable_avg(struct hmp_sched_stats *stats,
				struct task_struct *p)
{
	u32 task_load;

	if (sched_disable_window_stats)
		return;

	task_load = sched_disable_window_stats ? 0 : p->ravg.demand;

	stats->cumulative_runnable_avg -= task_load;

	BUG_ON((s64)stats->cumulative_runnable_avg < 0);

	stats->pred_demands_sum -= p->ravg.pred_demand;
	BUG_ON((s64)stats->pred_demands_sum < 0);
}

static inline void
fixup_cumulative_runnable_avg(struct hmp_sched_stats *stats,
			      struct task_struct *p, s64 task_load_delta,
			      s64 pred_demand_delta)
{
	if (sched_disable_window_stats)
		return;

	stats->cumulative_runnable_avg += task_load_delta;
	BUG_ON((s64)stats->cumulative_runnable_avg < 0);

	stats->pred_demands_sum += pred_demand_delta;
	BUG_ON((s64)stats->pred_demands_sum < 0);
}

#define pct_to_real(tunable)	\
		(div64_u64((u64)tunable * (u64)max_task_load(), 100))

#define real_to_pct(tunable)	\
		(div64_u64((u64)tunable * (u64)100, (u64)max_task_load()))

#define SCHED_HIGH_IRQ_TIMEOUT 3
static inline u64 sched_irqload(int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	s64 delta;

	delta = get_jiffies_64() - rq->irqload_ts;
	/*
	 * Current context can be preempted by irq and rq->irqload_ts can be
	 * updated by irq context so that delta can be negative.
	 * But this is okay and we can safely return as this means there
	 * was recent irq occurrence.
	 */

	if (delta < SCHED_HIGH_IRQ_TIMEOUT)
		return rq->avg_irqload;
	else
		return 0;
}

static inline int sched_cpu_high_irqload(int cpu)
{
	return sched_irqload(cpu) >= sysctl_sched_cpu_high_irqload;
}

static inline bool task_in_related_thread_group(struct task_struct *p)
{
	return !!(rcu_access_pointer(p->grp) != NULL);
}

static inline
struct related_thread_group *task_related_thread_group(struct task_struct *p)
{
	return rcu_dereference(p->grp);
}

#define PRED_DEMAND_DELTA ((s64)new_pred_demand - p->ravg.pred_demand)

extern void
check_for_freq_change(struct rq *rq, bool check_pred, bool check_groups);

extern void notify_migration(int src_cpu, int dest_cpu,
			bool src_cpu_dead, struct task_struct *p);

/* Is frequency of two cpus synchronized with each other? */
static inline int same_freq_domain(int src_cpu, int dst_cpu)
{
	struct rq *rq = cpu_rq(src_cpu);

	if (src_cpu == dst_cpu)
		return 1;

	return cpumask_test_cpu(dst_cpu, &rq->freq_domain_cpumask);
}

#define	BOOST_KICK	0
#define	CPU_RESERVED	1

static inline int is_reserved(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	return test_bit(CPU_RESERVED, &rq->hmp_flags);
}

static inline int mark_reserved(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	/* Name boost_flags as hmp_flags? */
	return test_and_set_bit(CPU_RESERVED, &rq->hmp_flags);
}

static inline void clear_reserved(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	clear_bit(CPU_RESERVED, &rq->hmp_flags);
}

static inline u64 cpu_cravg_sync(int cpu, int sync)
{
	struct rq *rq = cpu_rq(cpu);
	u64 load;

	load = rq->hmp_stats.cumulative_runnable_avg;

	/*
	 * If load is being checked in a sync wakeup environment,
	 * we may want to discount the load of the currently running
	 * task.
	 */
	if (sync && cpu == smp_processor_id()) {
		if (load > rq->curr->ravg.demand)
			load -= rq->curr->ravg.demand;
		else
			load = 0;
	}

	return load;
}

static inline bool is_short_burst_task(struct task_struct *p)
{
	return p->ravg.avg_burst < sysctl_sched_short_burst &&
	       p->ravg.avg_sleep_time > sysctl_sched_short_sleep;
}

extern void pre_big_task_count_change(const struct cpumask *cpus);
extern void post_big_task_count_change(const struct cpumask *cpus);
extern void set_hmp_defaults(void);
extern int power_delta_exceeded(unsigned int cpu_cost, unsigned int base_cost);
extern unsigned int power_cost(int cpu, u64 demand);
extern void reset_all_window_stats(u64 window_start, unsigned int window_size);
extern int sched_boost(void);
extern int task_load_will_fit(struct task_struct *p, u64 task_load, int cpu,
					enum sched_boost_policy boost_policy);
extern enum sched_boost_policy sched_boost_policy(void);
extern int task_will_fit(struct task_struct *p, int cpu);
extern u64 cpu_load(int cpu);
extern u64 cpu_load_sync(int cpu, int sync);
extern int preferred_cluster(struct sched_cluster *cluster,
						struct task_struct *p);
extern void inc_nr_big_task(struct hmp_sched_stats *stats,
					struct task_struct *p);
extern void dec_nr_big_task(struct hmp_sched_stats *stats,
					struct task_struct *p);
extern void inc_rq_hmp_stats(struct rq *rq,
				struct task_struct *p, int change_cra);
extern void dec_rq_hmp_stats(struct rq *rq,
				struct task_struct *p, int change_cra);
extern void reset_hmp_stats(struct hmp_sched_stats *stats, int reset_cra);
extern int is_big_task(struct task_struct *p);
extern int upmigrate_discouraged(struct task_struct *p);
extern struct sched_cluster *rq_cluster(struct rq *rq);
extern int nr_big_tasks(struct rq *rq);
extern void fixup_nr_big_tasks(struct hmp_sched_stats *stats,
					struct task_struct *p, s64 delta);
extern void reset_task_stats(struct task_struct *p);
extern void reset_cfs_rq_hmp_stats(int cpu, int reset_cra);
extern void _inc_hmp_sched_stats_fair(struct rq *rq,
			struct task_struct *p, int change_cra);
extern u64 cpu_upmigrate_discourage_read_u64(struct cgroup_subsys_state *css,
					struct cftype *cft);
extern int cpu_upmigrate_discourage_write_u64(struct cgroup_subsys_state *css,
				struct cftype *cft, u64 upmigrate_discourage);
extern void sched_boost_parse_dt(void);
extern void clear_top_tasks_bitmap(unsigned long *bitmap);

#if defined(CONFIG_SCHED_TUNE) && defined(CONFIG_CGROUP_SCHEDTUNE)
extern bool task_sched_boost(struct task_struct *p);
extern int sync_cgroup_colocation(struct task_struct *p, bool insert);
extern bool same_schedtune(struct task_struct *tsk1, struct task_struct *tsk2);
extern void update_cgroup_boost_settings(void);
extern void restore_cgroup_boost_settings(void);

#else
static inline bool
same_schedtune(struct task_struct *tsk1, struct task_struct *tsk2)
{
	return true;
}

static inline bool task_sched_boost(struct task_struct *p)
{
	return true;
}

static inline void update_cgroup_boost_settings(void) { }
static inline void restore_cgroup_boost_settings(void) { }
#endif

extern int alloc_related_thread_groups(void);

#else	/* CONFIG_SCHED_HMP */

struct hmp_sched_stats;
struct related_thread_group;
struct sched_cluster;

static inline enum sched_boost_policy sched_boost_policy(void)
{
	return SCHED_BOOST_NONE;
}

static inline bool task_sched_boost(struct task_struct *p)
{
	return true;
}

static inline int got_boost_kick(void)
{
	return 0;
}

static inline void update_task_ravg(struct task_struct *p, struct rq *rq,
				int event, u64 wallclock, u64 irqtime) { }

static inline bool early_detection_notify(struct rq *rq, u64 wallclock)
{
	return 0;
}

static inline void clear_ed_task(struct task_struct *p, struct rq *rq) { }
static inline void fixup_busy_time(struct task_struct *p, int new_cpu) { }
static inline void clear_boost_kick(int cpu) { }
static inline void clear_hmp_request(int cpu) { }
static inline void mark_task_starting(struct task_struct *p) { }
static inline void set_window_start(struct rq *rq) { }
static inline void init_clusters(void) {}
static inline void update_cluster_topology(void) { }
static inline void note_task_waking(struct task_struct *p, u64 wallclock) { }
static inline void set_task_last_switch_out(struct task_struct *p,
					    u64 wallclock) { }

static inline int task_will_fit(struct task_struct *p, int cpu)
{
	return 1;
}

static inline int select_best_cpu(struct task_struct *p, int target,
				  int reason, int sync)
{
	return 0;
}

static inline unsigned int power_cost(int cpu, u64 demand)
{
	return SCHED_CAPACITY_SCALE;
}

static inline int sched_boost(void)
{
	return 0;
}

static inline int is_big_task(struct task_struct *p)
{
	return 0;
}

static inline int nr_big_tasks(struct rq *rq)
{
	return 0;
}

static inline int is_cpu_throttling_imminent(int cpu)
{
	return 0;
}

static inline int is_task_migration_throttled(struct task_struct *p)
{
	return 0;
}

static inline unsigned int cpu_temp(int cpu)
{
	return 0;
}

static inline void
inc_rq_hmp_stats(struct rq *rq, struct task_struct *p, int change_cra) { }

static inline void
dec_rq_hmp_stats(struct rq *rq, struct task_struct *p, int change_cra) { }

static inline void
inc_hmp_sched_stats_fair(struct rq *rq, struct task_struct *p) { }

static inline void
dec_hmp_sched_stats_fair(struct rq *rq, struct task_struct *p) { }

static inline int
preferred_cluster(struct sched_cluster *cluster, struct task_struct *p)
{
	return 1;
}

static inline struct sched_cluster *rq_cluster(struct rq *rq)
{
	return NULL;
}

static inline void init_new_task_load(struct task_struct *p)
{
}

static inline u64 scale_load_to_cpu(u64 load, int cpu)
{
	return load;
}

static inline unsigned int nr_eligible_big_tasks(int cpu)
{
	return 0;
}

static inline bool is_max_capacity_cpu(int cpu) { return true; }

static inline int pct_task_load(struct task_struct *p) { return 0; }

static inline int cpu_capacity(int cpu)
{
	return SCHED_LOAD_SCALE;
}

static inline int same_cluster(int src_cpu, int dst_cpu) { return 1; }

static inline void inc_cumulative_runnable_avg(struct hmp_sched_stats *stats,
		 struct task_struct *p)
{
}

static inline void dec_cumulative_runnable_avg(struct hmp_sched_stats *stats,
		 struct task_struct *p)
{
}

static inline void sched_account_irqtime(int cpu, struct task_struct *curr,
				 u64 delta, u64 wallclock)
{
}

static inline void sched_account_irqstart(int cpu, struct task_struct *curr,
					  u64 wallclock)
{
}

static inline int sched_cpu_high_irqload(int cpu) { return 0; }

static inline void set_preferred_cluster(struct related_thread_group *grp) { }

static inline bool task_in_related_thread_group(struct task_struct *p)
{
	return false;
}

static inline
struct related_thread_group *task_related_thread_group(struct task_struct *p)
{
	return NULL;
}

static inline u32 task_load(struct task_struct *p) { return 0; }

static inline int update_preferred_cluster(struct related_thread_group *grp,
			 struct task_struct *p, u32 old_load)
{
	return 0;
}

static inline void add_new_task_to_grp(struct task_struct *new) {}

#define PRED_DEMAND_DELTA (0)

static inline void
check_for_freq_change(struct rq *rq, bool check_pred, bool check_groups) { }

static inline void notify_migration(int src_cpu, int dest_cpu,
			bool src_cpu_dead, struct task_struct *p) { }

static inline int same_freq_domain(int src_cpu, int dst_cpu)
{
	return 1;
}

static inline void pre_big_task_count_change(void) { }
static inline void post_big_task_count_change(void) { }
static inline void set_hmp_defaults(void) { }

static inline void clear_reserved(int cpu) { }
static inline void sched_boost_parse_dt(void) {}
static inline int alloc_related_thread_groups(void) { return 0; }

#define trace_sched_cpu_load(...)
#define trace_sched_cpu_load_lb(...)
#define trace_sched_cpu_load_cgroup(...)
#define trace_sched_cpu_load_wakeup(...)

static inline void update_avg_burst(struct task_struct *p) {}

#endif	/* CONFIG_SCHED_HMP */

/*
 * Returns the rq capacity of any rq in a group. This does not play
 * well with groups where rq capacity can change independently.
 */
#define group_rq_capacity(group) cpu_capacity(group_first_cpu(group))

#ifdef CONFIG_CGROUP_SCHED

/*
 * Return the group to which this tasks belongs.
 *
 * We cannot use task_css() and friends because the cgroup subsystem
 * changes that value before the cgroup_subsys::attach() method is called,
 * therefore we cannot pin it and might observe the wrong value.
 *
 * The same is true for autogroup's p->signal->autogroup->tg, the autogroup
 * core changes this before calling sched_move_task().
 *
 * Instead we use a 'copy' which is updated from sched_move_task() while
 * holding both task_struct::pi_lock and rq::lock.
 */
static inline struct task_group *task_group(struct task_struct *p)
{
	return p->sched_task_group;
}

/* Change a task's cfs_rq and parent entity if it moves across CPUs/groups */
static inline void set_task_rq(struct task_struct *p, unsigned int cpu)
{
#if defined(CONFIG_FAIR_GROUP_SCHED) || defined(CONFIG_RT_GROUP_SCHED)
	struct task_group *tg = task_group(p);
#endif

#ifdef CONFIG_FAIR_GROUP_SCHED
	set_task_rq_fair(&p->se, p->se.cfs_rq, tg->cfs_rq[cpu]);
	p->se.cfs_rq = tg->cfs_rq[cpu];
	p->se.parent = tg->se[cpu];
#endif

#ifdef CONFIG_RT_GROUP_SCHED
	p->rt.rt_rq  = tg->rt_rq[cpu];
	p->rt.parent = tg->rt_se[cpu];
#endif
}

#else /* CONFIG_CGROUP_SCHED */

static inline void set_task_rq(struct task_struct *p, unsigned int cpu) { }
static inline struct task_group *task_group(struct task_struct *p)
{
	return NULL;
}
#endif /* CONFIG_CGROUP_SCHED */

static inline void __set_task_cpu(struct task_struct *p, unsigned int cpu)
{
	set_task_rq(p, cpu);
#ifdef CONFIG_SMP
	/*
	 * After ->cpu is set up to a new value, task_rq_lock(p, ...) can be
	 * successfuly executed on another CPU. We must ensure that updates of
	 * per-task data have been completed by this moment.
	 */
	smp_wmb();
#ifdef CONFIG_THREAD_INFO_IN_TASK
	p->cpu = cpu;
#else
	task_thread_info(p)->cpu = cpu;
#endif
	p->wake_cpu = cpu;
#endif
}

/*
 * Tunables that become constants when CONFIG_SCHED_DEBUG is off:
 */
#ifdef CONFIG_SCHED_DEBUG
# include <linux/static_key.h>
# define const_debug __read_mostly
#else
# define const_debug const
#endif

#define sched_feat(x) SCHED_FEAT_##x

extern struct static_key_false sched_numa_balancing;

static inline u64 global_rt_period(void)
{
	return (u64)sysctl_sched_rt_period * NSEC_PER_USEC;
}

static inline u64 global_rt_runtime(void)
{
	if (sysctl_sched_rt_runtime < 0)
		return RUNTIME_INF;

	return (u64)sysctl_sched_rt_runtime * NSEC_PER_USEC;
}

static inline int task_current(struct rq *rq, struct task_struct *p)
{
	return rq->curr == p;
}

static inline int task_running(struct rq *rq, struct task_struct *p)
{
#ifdef CONFIG_SMP
	return p->on_cpu;
#else
	return task_current(rq, p);
#endif
}

static inline int task_on_rq_queued(struct task_struct *p)
{
	return p->on_rq == TASK_ON_RQ_QUEUED;
}

static inline int task_on_rq_migrating(struct task_struct *p)
{
	return p->on_rq == TASK_ON_RQ_MIGRATING;
}

#ifndef prepare_arch_switch
# define prepare_arch_switch(next)	do { } while (0)
#endif
#ifndef finish_arch_post_lock_switch
# define finish_arch_post_lock_switch()	do { } while (0)
#endif

static inline void prepare_lock_switch(struct rq *rq, struct task_struct *next)
{
#ifdef CONFIG_SMP
	/*
	 * We can optimise this out completely for !SMP, because the
	 * SMP rebalancing from interrupt is the only thing that cares
	 * here.
	 */
	next->on_cpu = 1;
#endif
}

static inline void finish_lock_switch(struct rq *rq, struct task_struct *prev)
{
#ifdef CONFIG_SMP
	/*
	 * After ->on_cpu is cleared, the task can be moved to a different CPU.
	 * We must ensure this doesn't happen until the switch is completely
	 * finished.
	 *
	 * In particular, the load of prev->state in finish_task_switch() must
	 * happen before this.
	 *
	 * Pairs with the smp_cond_load_acquire() in try_to_wake_up().
	 */
	smp_store_release(&prev->on_cpu, 0);
#endif
#ifdef CONFIG_DEBUG_SPINLOCK
	/* this is a valid case when another task releases the spinlock */
	rq->lock.owner = current;
#endif
	/*
	 * If we are tracking spinlock dependencies then we have to
	 * fix up the runqueue lock - which gets 'carried over' from
	 * prev into current:
	 */
	spin_acquire(&rq->lock.dep_map, 0, 0, _THIS_IP_);

	raw_spin_unlock_irq(&rq->lock);
}

/*
 * wake flags
 */
#define WF_SYNC		0x01		/* waker goes to sleep after wakeup */
#define WF_FORK		0x02		/* child wakeup after fork */
#define WF_MIGRATED	0x4		/* internal use, task got migrated */
#define WF_NO_NOTIFIER	0x08		/* do not notify governor */
#define WF_ON_RQ	0x09	/* wakee is on_rq */

/*
 * To aid in avoiding the subversion of "niceness" due to uneven distribution
 * of tasks with abnormal "nice" values across CPUs the contribution that
 * each task makes to its run queue's load is weighted according to its
 * scheduling class and "nice" value. For SCHED_NORMAL tasks this is just a
 * scaled version of the new time slice allocation that they receive on time
 * slice expiry etc.
 */

#define WEIGHT_IDLEPRIO                3
#define WMULT_IDLEPRIO         1431655765

/*
 * Nice levels are multiplicative, with a gentle 10% change for every
 * nice level changed. I.e. when a CPU-bound task goes from nice 0 to
 * nice 1, it will get ~10% less CPU time than another CPU-bound task
 * that remained on nice 0.
 *
 * The "10% effect" is relative and cumulative: from _any_ nice level,
 * if you go up 1 level, it's -10% CPU usage, if you go down 1 level
 * it's +10% CPU usage. (to achieve that we use a multiplier of 1.25.
 * If a task goes up by ~10% and another task goes down by ~10% then
 * the relative distance between them is ~25%.)
 */
static const int prio_to_weight[40] = {
 /* -20 */     88761,     71755,     56483,     46273,     36291,
 /* -15 */     29154,     23254,     18705,     14949,     11916,
 /* -10 */      9548,      7620,      6100,      4904,      3906,
 /*  -5 */      3121,      2501,      1991,      1586,      1277,
 /*   0 */      1024,       820,       655,       526,       423,
 /*   5 */       335,       272,       215,       172,       137,
 /*  10 */       110,        87,        70,        56,        45,
 /*  15 */        36,        29,        23,        18,        15,
};

/*
 * Inverse (2^32/x) values of the prio_to_weight[] array, precalculated.
 *
 * In cases where the weight does not change often, we can use the
 * precalculated inverse to speed up arithmetics by turning divisions
 * into multiplications:
 */
static const u32 prio_to_wmult[40] = {
 /* -20 */     48388,     59856,     76040,     92818,    118348,
 /* -15 */    147320,    184698,    229616,    287308,    360437,
 /* -10 */    449829,    563644,    704093,    875809,   1099582,
 /*  -5 */   1376151,   1717300,   2157191,   2708050,   3363326,
 /*   0 */   4194304,   5237765,   6557202,   8165337,  10153587,
 /*   5 */  12820798,  15790321,  19976592,  24970740,  31350126,
 /*  10 */  39045157,  49367440,  61356676,  76695844,  95443717,
 /*  15 */ 119304647, 148102320, 186737708, 238609294, 286331153,
};

/*
 * {de,en}queue flags:
 *
 * DEQUEUE_SLEEP  - task is no longer runnable
 * ENQUEUE_WAKEUP - task just became runnable
 *
 * SAVE/RESTORE - an otherwise spurious dequeue/enqueue, done to ensure tasks
 *                are in a known state which allows modification. Such pairs
 *                should preserve as much state as possible.
 *
 * MOVE - paired with SAVE/RESTORE, explicitly does not preserve the location
 *        in the runqueue.
 *
 * ENQUEUE_HEAD      - place at front of runqueue (tail if not specified)
 * ENQUEUE_REPLENISH - CBS (replenish runtime and postpone deadline)
 * ENQUEUE_WAKING    - sched_class::task_waking was called
 *
 */

#define DEQUEUE_SLEEP		0x01
#define DEQUEUE_SAVE		0x02 /* matches ENQUEUE_RESTORE */
#define DEQUEUE_MOVE		0x04 /* matches ENQUEUE_MOVE */
#define DEQUEUE_IDLE		0x80 /* The last dequeue before IDLE */

#define ENQUEUE_WAKEUP		0x01
#define ENQUEUE_RESTORE		0x02
#define ENQUEUE_MOVE		0x04

#define ENQUEUE_HEAD		0x08
#define ENQUEUE_REPLENISH	0x10
#ifdef CONFIG_SMP
#define ENQUEUE_WAKING		0x20
#else
#define ENQUEUE_WAKING		0x00
#endif
#define ENQUEUE_WAKEUP_NEW	0x40

#define RETRY_TASK		((void *)-1UL)

struct sched_class {
	const struct sched_class *next;

#ifdef CONFIG_UCLAMP_TASK
	int uclamp_enabled;
#endif

	void (*enqueue_task) (struct rq *rq, struct task_struct *p, int flags);
	void (*dequeue_task) (struct rq *rq, struct task_struct *p, int flags);
	void (*yield_task) (struct rq *rq);
	bool (*yield_to_task) (struct rq *rq, struct task_struct *p, bool preempt);

	void (*check_preempt_curr) (struct rq *rq, struct task_struct *p, int flags);

	/*
	 * It is the responsibility of the pick_next_task() method that will
	 * return the next task to call put_prev_task() on the @prev task or
	 * something equivalent.
	 *
	 * May return RETRY_TASK when it finds a higher prio class has runnable
	 * tasks.
	 */
	struct task_struct * (*pick_next_task) (struct rq *rq,
						struct task_struct *prev);
	void (*put_prev_task) (struct rq *rq, struct task_struct *p);

#ifdef CONFIG_SMP
	int  (*select_task_rq)(struct task_struct *p, int task_cpu, int sd_flag, int flags,
			       int subling_count_hint);
	void (*migrate_task_rq)(struct task_struct *p);

	void (*task_waking) (struct task_struct *task);
	void (*task_woken) (struct rq *this_rq, struct task_struct *task);

	void (*set_cpus_allowed)(struct task_struct *p,
				 const struct cpumask *newmask);

	void (*rq_online)(struct rq *rq);
	void (*rq_offline)(struct rq *rq);
#endif

	void (*set_curr_task) (struct rq *rq);
	void (*task_tick) (struct rq *rq, struct task_struct *p, int queued);
	void (*task_fork) (struct task_struct *p);
	void (*task_dead) (struct task_struct *p);

	/*
	 * The switched_from() call is allowed to drop rq->lock, therefore we
	 * cannot assume the switched_from/switched_to pair is serliazed by
	 * rq->lock. They are however serialized by p->pi_lock.
	 */
	void (*switched_from) (struct rq *this_rq, struct task_struct *task);
	void (*switched_to) (struct rq *this_rq, struct task_struct *task);
	void (*prio_changed) (struct rq *this_rq, struct task_struct *task,
			     int oldprio);

	unsigned int (*get_rr_interval) (struct rq *rq,
					 struct task_struct *task);

	void (*update_curr) (struct rq *rq);

#define TASK_SET_GROUP  0
#define TASK_MOVE_GROUP	1

#ifdef CONFIG_FAIR_GROUP_SCHED
	void (*task_change_group)(struct task_struct *p, int type);
#endif
#ifdef CONFIG_SCHED_HMP
	void (*inc_hmp_sched_stats)(struct rq *rq, struct task_struct *p);
	void (*dec_hmp_sched_stats)(struct rq *rq, struct task_struct *p);
	void (*fixup_hmp_sched_stats)(struct rq *rq, struct task_struct *p,
				      u32 new_task_load, u32 new_pred_demand);
#endif
};

static inline void put_prev_task(struct rq *rq, struct task_struct *prev)
{
	prev->sched_class->put_prev_task(rq, prev);
}

#define sched_class_highest (&stop_sched_class)
#define for_each_class(class) \
   for (class = sched_class_highest; class; class = class->next)

extern const struct sched_class stop_sched_class;
extern const struct sched_class dl_sched_class;
extern const struct sched_class rt_sched_class;
extern const struct sched_class fair_sched_class;
extern const struct sched_class idle_sched_class;


#ifdef CONFIG_SMP

extern void init_max_cpu_capacity(struct max_cpu_capacity *mcc);
extern void update_group_capacity(struct sched_domain *sd, int cpu);

extern void trigger_load_balance(struct rq *rq);
extern void nohz_balance_clear_nohz_mask(int cpu);

extern void idle_enter_fair(struct rq *this_rq);
extern void idle_exit_fair(struct rq *this_rq);

extern void set_cpus_allowed_common(struct task_struct *p, const struct cpumask *new_mask);

#else

static inline void idle_enter_fair(struct rq *rq) { }
static inline void idle_exit_fair(struct rq *rq) { }

#endif

#ifdef CONFIG_CPU_IDLE
static inline void idle_set_state(struct rq *rq,
				  struct cpuidle_state *idle_state)
{
	rq->idle_state = idle_state;
}

static inline struct cpuidle_state *idle_get_state(struct rq *rq)
{
	SCHED_WARN_ON(!rcu_read_lock_held());
	return rq->idle_state;
}

static inline void idle_set_state_idx(struct rq *rq, int idle_state_idx)
{
	rq->idle_state_idx = idle_state_idx;
}

static inline int idle_get_state_idx(struct rq *rq)
{
	WARN_ON(!rcu_read_lock_held());
	return rq->idle_state_idx;
}
#else
static inline void idle_set_state(struct rq *rq,
				  struct cpuidle_state *idle_state)
{
}

static inline struct cpuidle_state *idle_get_state(struct rq *rq)
{
	return NULL;
}

static inline void idle_set_state_idx(struct rq *rq, int idle_state_idx)
{
}

static inline int idle_get_state_idx(struct rq *rq)
{
	return -1;
}
#endif

#ifdef CONFIG_SYSRQ_SCHED_DEBUG
extern void sysrq_sched_debug_show(void);
#endif
extern void sched_init_granularity(void);
extern void update_max_interval(void);

extern void init_sched_dl_class(void);
extern void init_sched_rt_class(void);
extern void init_sched_fair_class(void);

extern void resched_curr(struct rq *rq);
extern void resched_cpu(int cpu);

extern struct rt_bandwidth def_rt_bandwidth;
extern void init_rt_bandwidth(struct rt_bandwidth *rt_b, u64 period, u64 runtime);
extern void init_rt_schedtune_timer(struct sched_rt_entity *rt_se);

extern struct dl_bandwidth def_dl_bandwidth;
extern void init_dl_bandwidth(struct dl_bandwidth *dl_b, u64 period, u64 runtime);
extern void init_dl_task_timer(struct sched_dl_entity *dl_se);

unsigned long to_ratio(u64 period, u64 runtime);

extern void init_entity_runnable_average(struct sched_entity *se);
extern void post_init_entity_util_avg(struct sched_entity *se);

static inline void __add_nr_running(struct rq *rq, unsigned count)
{
	unsigned prev_nr = rq->nr_running;

	sched_update_nr_prod(cpu_of(rq), count, true);
	rq->nr_running = prev_nr + count;

	if (prev_nr < 2 && rq->nr_running >= 2) {
#ifdef CONFIG_SMP
		if (!rq->rd->overload)
			rq->rd->overload = true;
#endif

#ifdef CONFIG_NO_HZ_FULL
		if (tick_nohz_full_cpu(rq->cpu)) {
			/*
			 * Tick is needed if more than one task runs on a CPU.
			 * Send the target an IPI to kick it out of nohz mode.
			 *
			 * We assume that IPI implies full memory barrier and the
			 * new value of rq->nr_running is visible on reception
			 * from the target.
			 */
			tick_nohz_full_kick_cpu(rq->cpu);
		}
#endif
	}
}

static inline void __sub_nr_running(struct rq *rq, unsigned count)
{
	sched_update_nr_prod(cpu_of(rq), count, false);
	rq->nr_running -= count;
}

#ifdef CONFIG_CPU_QUIET
#define NR_AVE_SCALE(x)		((x) << FSHIFT)
static inline u64 do_nr_running_integral(struct rq *rq)
{
	s64 nr, deltax;
	u64 nr_running_integral = rq->nr_running_integral;

	deltax = rq->clock_task - rq->nr_last_stamp;
	nr = NR_AVE_SCALE(rq->nr_running);

	nr_running_integral += nr * deltax;

	return nr_running_integral;
}

static inline void add_nr_running(struct rq *rq, unsigned count)
{
	write_seqcount_begin(&rq->ave_seqcnt);
	rq->nr_running_integral = do_nr_running_integral(rq);
	rq->nr_last_stamp = rq->clock_task;
	__add_nr_running(rq, count);
	write_seqcount_end(&rq->ave_seqcnt);
}

static inline void sub_nr_running(struct rq *rq, unsigned count)
{
	write_seqcount_begin(&rq->ave_seqcnt);
	rq->nr_running_integral = do_nr_running_integral(rq);
	rq->nr_last_stamp = rq->clock_task;
	__sub_nr_running(rq, count);
	write_seqcount_end(&rq->ave_seqcnt);
}
#else
#define add_nr_running __add_nr_running
#define sub_nr_running __sub_nr_running
#endif

static inline void rq_last_tick_reset(struct rq *rq)
{
#ifdef CONFIG_NO_HZ_FULL
	rq->last_sched_tick = jiffies;
#endif
}

extern void update_rq_clock(struct rq *rq);

extern void activate_task(struct rq *rq, struct task_struct *p, int flags);
extern void deactivate_task(struct rq *rq, struct task_struct *p, int flags);

extern void check_preempt_curr(struct rq *rq, struct task_struct *p, int flags);

extern const_debug unsigned int sysctl_sched_time_avg;
extern const_debug unsigned int sysctl_sched_nr_migrate;
extern const_debug unsigned int sysctl_sched_migration_cost;

static inline u64 sched_avg_period(void)
{
	return (u64)sysctl_sched_time_avg * NSEC_PER_MSEC / 2;
}

#ifdef CONFIG_SCHED_HRTICK

/*
 * Use hrtick when:
 *  - enabled by features
 *  - hrtimer is actually high res
 */
static inline int hrtick_enabled(struct rq *rq)
{
	if (!sched_feat(HRTICK))
		return 0;
	if (!cpu_active(cpu_of(rq)))
		return 0;
	return hrtimer_is_hres_active(&rq->hrtick_timer);
}

void hrtick_start(struct rq *rq, u64 delay);

#else

static inline int hrtick_enabled(struct rq *rq)
{
	return 0;
}

#endif /* CONFIG_SCHED_HRTICK */

#ifdef CONFIG_SMP
extern void sched_avg_update(struct rq *rq);
extern unsigned long sched_get_rt_rq_util(int cpu);

#ifndef arch_scale_freq_capacity
static __always_inline
unsigned long arch_scale_freq_capacity(struct sched_domain *sd, int cpu)
{
	return SCHED_CAPACITY_SCALE;
}
#endif

#ifndef arch_scale_max_freq_capacity
static __always_inline
unsigned long arch_scale_max_freq_capacity(struct sched_domain *sd, int cpu)
{
	return SCHED_CAPACITY_SCALE;
}
#endif

#ifndef arch_scale_min_freq_capacity
static __always_inline
unsigned long arch_scale_min_freq_capacity(struct sched_domain *sd, int cpu)
{
	/*
	 * Multiplied with any capacity value, this scale factor will return
	 * 0, which represents an un-capped state
	 */
	return 0;
}
#endif

#ifndef arch_scale_cpu_capacity
static __always_inline
unsigned long arch_scale_cpu_capacity(struct sched_domain *sd, int cpu)
{
	if (sd && (sd->flags & SD_SHARE_CPUCAPACITY) && (sd->span_weight > 1))
		return sd->smt_gain / sd->span_weight;

	return SCHED_CAPACITY_SCALE;
}
#endif

#ifndef arch_update_cpu_capacity
static __always_inline
void arch_update_cpu_capacity(int cpu)
{
}
#endif

#ifdef CONFIG_SMP
static inline unsigned long capacity_of(int cpu)
{
	return cpu_rq(cpu)->cpu_capacity;
}

static inline unsigned long capacity_orig_of(int cpu)
{
	return cpu_rq(cpu)->cpu_capacity_orig;
}

extern unsigned int sysctl_sched_use_walt_cpu_util;
extern unsigned int walt_ravg_window;
extern bool walt_disabled;

static inline unsigned long task_util(struct task_struct *p)
{

#ifdef CONFIG_SCHED_WALT
	if (!walt_disabled && sysctl_sched_use_walt_task_util) {
		unsigned long demand = p->ravg.demand;
		return (demand << 10) / walt_ravg_window;
	}
#endif
	return READ_ONCE(p->se.avg.util_avg);
}


/*
 * cpu_util returns the amount of capacity of a CPU that is used by CFS
 * tasks. The unit of the return value must be the one of capacity so we can
 * compare the utilization with the capacity of the CPU that is available for
 * CFS task (ie cpu_capacity).
 *
 * cfs_rq.avg.util_avg is the sum of running time of runnable tasks plus the
 * recent utilization of currently non-runnable tasks on a CPU. It represents
 * the amount of utilization of a CPU in the range [0..capacity_orig] where
 * capacity_orig is the cpu_capacity available at the highest frequency
 * (arch_scale_freq_capacity()).
 * The utilization of a CPU converges towards a sum equal to or less than the
 * current capacity (capacity_curr <= capacity_orig) of the CPU because it is
 * the running time on this CPU scaled by capacity_curr.
 *
 * Nevertheless, cfs_rq.avg.util_avg can be higher than capacity_curr or even
 * higher than capacity_orig because of unfortunate rounding in
 * cfs.avg.util_avg or just after migrating tasks and new task wakeups until
 * the average stabilizes with the new running time. We need to check that the
 * utilization stays within the range of [0..capacity_orig] and cap it if
 * necessary. Without utilization capping, a group could be seen as overloaded
 * (CPU0 utilization at 121% + CPU1 utilization at 80%) whereas CPU1 has 20% of
 * available capacity. We allow utilization to overshoot capacity_curr (but not
 * capacity_orig) as it useful for predicting the capacity required after task
 * migrations (scheduler-driven DVFS).
 */
static inline unsigned long __cpu_util(int cpu, int delta)
{
	struct cfs_rq *cfs_rq;
	unsigned long util;

#ifdef CONFIG_SCHED_WALT
	if (!walt_disabled && sysctl_sched_use_walt_cpu_util) {
		util = div64_u64(cpu_rq(cpu)->cumulative_runnable_avg,
				 walt_ravg_window >> SCHED_LOAD_SHIFT);

		return min_t(unsigned long, util, capacity_orig_of(cpu));
	}
#endif

	cfs_rq = &cpu_rq(cpu)->cfs;
	util = READ_ONCE(cfs_rq->avg.util_avg);

	if (sched_feat(UTIL_EST))
		util = max_t(unsigned long, util,
			READ_ONCE(cfs_rq->avg.util_est.enqueued));

	return min_t(unsigned long, util, capacity_orig_of(cpu));
}

static inline unsigned long cpu_util(int cpu)
{
	return __cpu_util(cpu, 0);
}

static inline unsigned long cpu_util_freq(int cpu)
{
	unsigned long util = READ_ONCE(cpu_rq(cpu)->cfs.avg.util_avg);
	unsigned long capacity = capacity_orig_of(cpu);

	/* UTIL_EST */
	if (sched_feat(UTIL_EST)) {
		util = max_t(unsigned long, util,
			     READ_ONCE(cpu_rq(cpu)->cfs.avg.util_est.enqueued));
	}

#ifdef CONFIG_SCHED_WALT
	if (!walt_disabled && sysctl_sched_use_walt_cpu_util)
		util = div64_u64(cpu_rq(cpu)->prev_runnable_sum,
				 walt_ravg_window >> SCHED_LOAD_SHIFT);
#endif
	return (util >= capacity) ? capacity : util;
}

#endif

static inline void sched_rt_avg_update(struct rq *rq, u64 rt_delta)
{
	rq->rt_avg += rt_delta * arch_scale_freq_capacity(NULL, cpu_of(rq));
}
#else
static inline void sched_rt_avg_update(struct rq *rq, u64 rt_delta) { }
static inline void sched_avg_update(struct rq *rq) { }
#endif

struct rq_flags {
	unsigned long flags;
};

struct rq *__task_rq_lock(struct task_struct *p, struct rq_flags *rf)
	__acquires(rq->lock);
struct rq *task_rq_lock(struct task_struct *p, struct rq_flags *rf)
	__acquires(p->pi_lock)
	__acquires(rq->lock);

static inline void __task_rq_unlock(struct rq *rq, struct rq_flags *rf)
	__releases(rq->lock)
{
	lockdep_unpin_lock(&rq->lock);
	raw_spin_unlock(&rq->lock);
}

static inline void
task_rq_unlock(struct rq *rq, struct task_struct *p, struct rq_flags *rf)
	__releases(rq->lock)
	__releases(p->pi_lock)
{
	lockdep_unpin_lock(&rq->lock);
	raw_spin_unlock(&rq->lock);
	raw_spin_unlock_irqrestore(&p->pi_lock, rf->flags);
}

extern struct rq *lock_rq_of(struct task_struct *p, struct rq_flags *rf);
extern void unlock_rq_of(struct rq *rq, struct task_struct *p, struct rq_flags *rf);

#ifdef CONFIG_SMP
#ifdef CONFIG_PREEMPT

static inline void double_rq_lock(struct rq *rq1, struct rq *rq2);

/*
 * fair double_lock_balance: Safely acquires both rq->locks in a fair
 * way at the expense of forcing extra atomic operations in all
 * invocations.  This assures that the double_lock is acquired using the
 * same underlying policy as the spinlock_t on this architecture, which
 * reduces latency compared to the unfair variant below.  However, it
 * also adds more overhead and therefore may reduce throughput.
 */
static inline int _double_lock_balance(struct rq *this_rq, struct rq *busiest)
	__releases(this_rq->lock)
	__acquires(busiest->lock)
	__acquires(this_rq->lock)
{
	raw_spin_unlock(&this_rq->lock);
	double_rq_lock(this_rq, busiest);

	return 1;
}

#else
/*
 * Unfair double_lock_balance: Optimizes throughput at the expense of
 * latency by eliminating extra atomic operations when the locks are
 * already in proper order on entry.  This favors lower cpu-ids and will
 * grant the double lock to lower cpus over higher ids under contention,
 * regardless of entry order into the function.
 */
static inline int _double_lock_balance(struct rq *this_rq, struct rq *busiest)
	__releases(this_rq->lock)
	__acquires(busiest->lock)
	__acquires(this_rq->lock)
{
	int ret = 0;

	if (unlikely(!raw_spin_trylock(&busiest->lock))) {
		if (busiest < this_rq) {
			raw_spin_unlock(&this_rq->lock);
			raw_spin_lock(&busiest->lock);
			raw_spin_lock_nested(&this_rq->lock,
					      SINGLE_DEPTH_NESTING);
			ret = 1;
		} else
			raw_spin_lock_nested(&busiest->lock,
					      SINGLE_DEPTH_NESTING);
	}
	return ret;
}

#endif /* CONFIG_PREEMPT */

/*
 * double_lock_balance - lock the busiest runqueue, this_rq is locked already.
 */
static inline int double_lock_balance(struct rq *this_rq, struct rq *busiest)
{
	if (unlikely(!irqs_disabled())) {
		/* printk() doesn't work good under rq->lock */
		raw_spin_unlock(&this_rq->lock);
		BUG_ON(1);
	}

	return _double_lock_balance(this_rq, busiest);
}

static inline void double_unlock_balance(struct rq *this_rq, struct rq *busiest)
	__releases(busiest->lock)
{
	if (this_rq != busiest)
		raw_spin_unlock(&busiest->lock);
	lock_set_subclass(&this_rq->lock.dep_map, 0, _RET_IP_);
}

static inline void double_lock(spinlock_t *l1, spinlock_t *l2)
{
	if (l1 > l2)
		swap(l1, l2);

	spin_lock(l1);
	spin_lock_nested(l2, SINGLE_DEPTH_NESTING);
}

static inline void double_lock_irq(spinlock_t *l1, spinlock_t *l2)
{
	if (l1 > l2)
		swap(l1, l2);

	spin_lock_irq(l1);
	spin_lock_nested(l2, SINGLE_DEPTH_NESTING);
}

static inline void double_raw_lock(raw_spinlock_t *l1, raw_spinlock_t *l2)
{
	if (l1 > l2)
		swap(l1, l2);

	raw_spin_lock(l1);
	raw_spin_lock_nested(l2, SINGLE_DEPTH_NESTING);
}

/*
 * double_rq_lock - safely lock two runqueues
 *
 * Note this does not disable interrupts like task_rq_lock,
 * you need to do so manually before calling.
 */
static inline void double_rq_lock(struct rq *rq1, struct rq *rq2)
	__acquires(rq1->lock)
	__acquires(rq2->lock)
{
	BUG_ON(!irqs_disabled());
	if (rq1 == rq2) {
		raw_spin_lock(&rq1->lock);
		__acquire(rq2->lock);	/* Fake it out ;) */
	} else {
		if (rq1 < rq2) {
			raw_spin_lock(&rq1->lock);
			raw_spin_lock_nested(&rq2->lock, SINGLE_DEPTH_NESTING);
		} else {
			raw_spin_lock(&rq2->lock);
			raw_spin_lock_nested(&rq1->lock, SINGLE_DEPTH_NESTING);
		}
	}
}

/*
 * double_rq_unlock - safely unlock two runqueues
 *
 * Note this does not restore interrupts like task_rq_unlock,
 * you need to do so manually after calling.
 */
static inline void double_rq_unlock(struct rq *rq1, struct rq *rq2)
	__releases(rq1->lock)
	__releases(rq2->lock)
{
	raw_spin_unlock(&rq1->lock);
	if (rq1 != rq2)
		raw_spin_unlock(&rq2->lock);
	else
		__release(rq2->lock);
}

/*
 * task_may_not_preempt - check whether a task may not be preemptible soon
 */
extern bool task_may_not_preempt(struct task_struct *task, int cpu);

#else /* CONFIG_SMP */

/*
 * double_rq_lock - safely lock two runqueues
 *
 * Note this does not disable interrupts like task_rq_lock,
 * you need to do so manually before calling.
 */
static inline void double_rq_lock(struct rq *rq1, struct rq *rq2)
	__acquires(rq1->lock)
	__acquires(rq2->lock)
{
	BUG_ON(!irqs_disabled());
	BUG_ON(rq1 != rq2);
	raw_spin_lock(&rq1->lock);
	__acquire(rq2->lock);	/* Fake it out ;) */
}

/*
 * double_rq_unlock - safely unlock two runqueues
 *
 * Note this does not restore interrupts like task_rq_unlock,
 * you need to do so manually after calling.
 */
static inline void double_rq_unlock(struct rq *rq1, struct rq *rq2)
	__releases(rq1->lock)
	__releases(rq2->lock)
{
	BUG_ON(rq1 != rq2);
	raw_spin_unlock(&rq1->lock);
	__release(rq2->lock);
}

#endif

extern struct sched_entity *__pick_first_entity(struct cfs_rq *cfs_rq);
extern struct sched_entity *__pick_last_entity(struct cfs_rq *cfs_rq);

#ifdef	CONFIG_SCHED_DEBUG
extern void print_cfs_stats(struct seq_file *m, int cpu);
extern void print_rt_stats(struct seq_file *m, int cpu);
extern void print_dl_stats(struct seq_file *m, int cpu);
extern void
print_cfs_rq(struct seq_file *m, int cpu, struct cfs_rq *cfs_rq);

#ifdef CONFIG_NUMA_BALANCING
extern void
show_numa_stats(struct task_struct *p, struct seq_file *m);
extern void
print_numa_stats(struct seq_file *m, int node, unsigned long tsf,
	unsigned long tpf, unsigned long gsf, unsigned long gpf);
#endif /* CONFIG_NUMA_BALANCING */
#endif /* CONFIG_SCHED_DEBUG */

extern void init_cfs_rq(struct cfs_rq *cfs_rq);
extern void init_rt_rq(struct rt_rq *rt_rq);
extern void init_dl_rq(struct dl_rq *dl_rq);

extern void cfs_bandwidth_usage_inc(void);
extern void cfs_bandwidth_usage_dec(void);

#ifdef CONFIG_NO_HZ_COMMON
enum rq_nohz_flag_bits {
	NOHZ_TICK_STOPPED,
	NOHZ_BALANCE_KICK,
};

#define NOHZ_KICK_ANY 0
#define NOHZ_KICK_RESTRICT 1

#define nohz_flags(cpu)	(&cpu_rq(cpu)->nohz_flags)
#endif

#ifdef CONFIG_IRQ_TIME_ACCOUNTING

DECLARE_PER_CPU(u64, cpu_hardirq_time);
DECLARE_PER_CPU(u64, cpu_softirq_time);

#ifndef CONFIG_64BIT
DECLARE_PER_CPU(seqcount_t, irq_time_seq);

static inline void irq_time_write_begin(void)
{
	__this_cpu_inc(irq_time_seq.sequence);
	smp_wmb();
}

static inline void irq_time_write_end(void)
{
	smp_wmb();
	__this_cpu_inc(irq_time_seq.sequence);
}

static inline u64 irq_time_read(int cpu)
{
	u64 irq_time;
	unsigned seq;

	do {
		seq = read_seqcount_begin(&per_cpu(irq_time_seq, cpu));
		irq_time = per_cpu(cpu_softirq_time, cpu) +
			   per_cpu(cpu_hardirq_time, cpu);
	} while (read_seqcount_retry(&per_cpu(irq_time_seq, cpu), seq));

	return irq_time;
}
#else /* CONFIG_64BIT */
static inline void irq_time_write_begin(void)
{
}

static inline void irq_time_write_end(void)
{
}

static inline u64 irq_time_read(int cpu)
{
	return per_cpu(cpu_softirq_time, cpu) + per_cpu(cpu_hardirq_time, cpu);
}
#endif /* CONFIG_64BIT */
#endif /* CONFIG_IRQ_TIME_ACCOUNTING */

#ifdef CONFIG_CPU_FREQ
DECLARE_PER_CPU(struct update_util_data *, cpufreq_update_util_data);

/**
 * cpufreq_update_util - Take a note about CPU utilization changes.
 * @rq: Runqueue to carry out the update for.
 * @flags: Update reason flags.
 *
 * This function is called by the scheduler on the CPU whose utilization is
 * being updated.
 *
 * It can only be called from RCU-sched read-side critical sections.
 *
 * The way cpufreq is currently arranged requires it to evaluate the CPU
 * performance state (frequency/voltage) on a regular basis to prevent it from
 * being stuck in a completely inadequate performance level for too long.
 * That is not guaranteed to happen if the updates are only triggered from CFS,
 * though, because they may not be coming in if RT or deadline tasks are active
 * all the time (or there are RT and DL tasks only).
 *
 * As a workaround for that issue, this function is called by the RT and DL
 * sched classes to trigger extra cpufreq updates to prevent it from stalling,
 * but that really is a band-aid.  Going forward it should be replaced with
 * solutions targeted more specifically at RT and DL tasks.
 */
static inline void cpufreq_update_util(struct rq *rq, unsigned int flags)
{
        struct update_util_data *data;

#ifdef CONFIG_SCHED_HMP
	/*
	 * Skip if we've already reported, but not if this is an inter-cluster
	 * migration. Also only allow WALT update sites.
	 */
	if (!(flags & SCHED_CPUFREQ_WALT))
		return;
	if (!sched_disable_window_stats &&
		(rq->load_reported_window == rq->window_start) &&
		!(flags & SCHED_CPUFREQ_INTERCLUSTER_MIG))
		return;
	rq->load_reported_window = rq->window_start;
#endif

		data = rcu_dereference_sched(*per_cpu_ptr(&cpufreq_update_util_data,
						cpu_of(rq)));
        if (data)
                data->func(data, rq_clock(rq), flags);
}

static inline void cpufreq_update_this_cpu(struct rq *rq, unsigned int flags)
{
        if (cpu_of(rq) == smp_processor_id())
                cpufreq_update_util(rq, flags);
}
#else
static inline void cpufreq_update_util(struct rq *rq, unsigned int flags) {}
static inline void cpufreq_update_this_cpu(struct rq *rq, unsigned int flags) {}
#endif /* CONFIG_CPU_FREQ */

#ifdef CONFIG_UCLAMP_TASK
unsigned long uclamp_eff_value(struct task_struct *p, enum uclamp_id clamp_id);

/**
 * uclamp_rq_util_with - clamp @util with @rq and @p effective uclamp values.
 * @rq:		The rq to clamp against. Must not be NULL.
 * @util:	The util value to clamp.
 * @p:		The task to clamp against. Can be NULL if you want to clamp
 *		against @rq only.
 *
 * Clamps the passed @util to the max(@rq, @p) effective uclamp values.
 *
 * If sched_uclamp_used static key is disabled, then just return the util
 * without any clamping since uclamp aggregation at the rq level in the fast
 * path is disabled, rendering this operation a NOP.
 *
 * Use uclamp_eff_value() if you don't care about uclamp values at rq level. It
 * will return the correct effective uclamp value of the task even if the
 * static key is disabled.
 */
static __always_inline
unsigned long uclamp_rq_util_with(struct rq *rq, unsigned long util,
			       struct task_struct *p)
{
	unsigned long min_util = 0;
	unsigned long max_util = 0;

	if (!static_branch_likely(&sched_uclamp_used))
		return util;

	if (p) {
		min_util = uclamp_eff_value(p, UCLAMP_MIN);
		max_util = uclamp_eff_value(p, UCLAMP_MAX);

		/*
		 * Ignore last runnable task's max clamp, as this task will
		 * reset it. Similarly, no need to read the rq's min clamp.
		 */
		if (rq->uclamp_flags & UCLAMP_FLAG_IDLE)
			goto out;
	}

	min_util = max_t(unsigned long, min_util, READ_ONCE(rq->uclamp[UCLAMP_MIN].value));
	max_util = max_t(unsigned long, max_util, READ_ONCE(rq->uclamp[UCLAMP_MAX].value));
out:
	/*
	 * Since CPU's {min,max}_util clamps are MAX aggregated considering
	 * RUNNABLE tasks with _different_ clamps, we can end up with an
	 * inversion. Fix it now when the clamps are applied.
	 */
	if (unlikely(min_util >= max_util))
		return min_util;

	return clamp(util, min_util, max_util);
}

/*
 * When uclamp is compiled in, the aggregation at rq level is 'turned off'
 * by default in the fast path and only gets turned on once userspace performs
 * an operation that requires it.
 *
 * Returns true if userspace opted-in to use uclamp and aggregation at rq level
 * hence is active.
 */
static inline bool uclamp_is_used(void)
{
	return static_branch_likely(&sched_uclamp_used);
}

static inline bool uclamp_boosted(struct task_struct *p)
{
	return uclamp_eff_value(p, UCLAMP_MIN) > 0;
}
#else /* CONFIG_UCLAMP_TASK */
static inline unsigned long uclamp_rq_util_with(struct rq *rq, unsigned long util,
					     struct task_struct *p)
{
	return util;
}
static inline bool uclamp_is_used(void)
{
	return false;
}
static inline bool uclamp_boosted(struct task_struct *p)
{
	return false;
}
#endif /* CONFIG_UCLAMP_TASK */

#ifdef CONFIG_UCLAMP_TASK_GROUP
static inline bool uclamp_latency_sensitive(struct task_struct *p)
{
	struct cgroup_subsys_state *css = task_css(p, cpu_cgrp_id);
	struct task_group *tg;

	if (!css)
		return false;
	tg = container_of(css, struct task_group, css);

	return tg->latency_sensitive;
}
#else
static inline bool uclamp_latency_sensitive(struct task_struct *p)
{
	return false;
}
#endif /* CONFIG_UCLAMP_TASK_GROUP */

#ifdef CONFIG_SCHED_WALT

static inline bool
walt_task_in_cum_window_demand(struct rq *rq, struct task_struct *p)
{
	return cpu_of(rq) == task_cpu(p) &&
	       (p->on_rq || p->last_sleep_ts >= rq->window_start);
}

#endif /* CONFIG_SCHED_WALT */

#ifdef arch_scale_freq_capacity
#ifndef arch_scale_freq_invariant
#define arch_scale_freq_invariant()     (true)
#endif
#else /* arch_scale_freq_capacity */
#define arch_scale_freq_invariant()     (false)
#endif
