/*
 * Kernel iptables module to track stats for packets based on user tags.
 *
 * (C) 2011 Google, Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/*
 * There are run-time debug flags enabled via the debug_mask module param, or
 * via the DEFAULT_DEBUG_MASK. See xt_qtaguid_internal.h.
 */
#define DEBUG

#include <linux/android_version.h>
#include <linux/file.h>
#include <linux/inetdevice.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/netfilter/x_tables.h>
#include <linux/netfilter/xt_qtaguid.h>
#include <linux/ratelimit.h>
#include <linux/seq_file.h>
#include <linux/skbuff.h>
#include <linux/workqueue.h>
#include <net/addrconf.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <net/udp.h>

#if defined(CONFIG_IP6_NF_IPTABLES) || defined(CONFIG_IP6_NF_IPTABLES_MODULE)
#include <linux/netfilter_ipv6/ip6_tables.h>
#endif

#include <linux/netfilter/xt_socket.h>
#include "xt_qtaguid_internal.h"
#include "xt_qtaguid_print.h"
#include "../../fs/proc/internal.h"

/*
 * We only use the xt_socket funcs within a similar context to avoid unexpected
 * return values.
 */
#define XT_SOCKET_SUPPORTED_HOOKS \
	((1 << NF_INET_PRE_ROUTING) | (1 << NF_INET_LOCAL_IN))


static const char *module_procdirname = "xt_qtaguid";
static struct proc_dir_entry *xt_qtaguid_procdir;

static unsigned int proc_iface_perms = S_IRUGO;
module_param_named(iface_perms, proc_iface_perms, uint, S_IRUGO | S_IWUSR);

static struct proc_dir_entry *xt_qtaguid_stats_file;
static unsigned int proc_stats_perms = S_IRUGO;
module_param_named(stats_perms, proc_stats_perms, uint, S_IRUGO | S_IWUSR);

static struct proc_dir_entry *xt_qtaguid_ctrl_file;

/* Everybody can write. But proc_ctrl_write_limited is true by default which
 * limits what can be controlled. See the can_*() functions.
 */
static unsigned int proc_ctrl_perms = S_IRUGO | S_IWUGO;
module_param_named(ctrl_perms, proc_ctrl_perms, uint, S_IRUGO | S_IWUSR);

/* Limited by default, so the gid of the ctrl and stats proc entries
 * will limit what can be done. See the can_*() functions.
 */
static bool proc_stats_readall_limited = true;
static bool proc_ctrl_write_limited = true;

module_param_named(stats_readall_limited, proc_stats_readall_limited, bool,
		   S_IRUGO | S_IWUSR);
module_param_named(ctrl_write_limited, proc_ctrl_write_limited, bool,
		   S_IRUGO | S_IWUSR);

/*
 * Limit the number of active tags (via socket tags) for a given UID.
 * Multiple processes could share the UID.
 */
static int max_sock_tags = DEFAULT_MAX_SOCK_TAGS;
module_param(max_sock_tags, int, S_IRUGO | S_IWUSR);

/*
 * After the kernel has initiallized this module, it is still possible
 * to make it passive.
 * Setting passive to Y:
 *  - the iface stats handling will not act on notifications.
 *  - iptables matches will never match.
 *  - ctrl commands silently succeed.
 *  - stats are always empty.
 * This is mostly usefull when a bug is suspected.
 */
static bool module_passive;
module_param_named(passive, module_passive, bool, S_IRUGO | S_IWUSR);

/*
 * Control how qtaguid data is tracked per proc/uid.
 * Setting tag_tracking_passive to Y:
 *  - don't create proc specific structs to track tags
 *  - don't check that active tag stats exceed some limits.
 *  - don't clean up socket tags on process exits.
 * This is mostly usefull when a bug is suspected.
 */
static bool qtu_proc_handling_passive;
module_param_named(tag_tracking_passive, qtu_proc_handling_passive, bool,
		   S_IRUGO | S_IWUSR);

#define QTU_DEV_NAME "xt_qtaguid"

uint qtaguid_debug_mask = DEFAULT_DEBUG_MASK;
module_param_named(debug_mask, qtaguid_debug_mask, uint, S_IRUGO | S_IWUSR);

/*---------------------------------------------------------------------------*/
static const char *iface_stat_procdirname = "iface_stat";
static struct proc_dir_entry *iface_stat_procdir;
/*
 * The iface_stat_all* will go away once userspace gets use to the new fields
 * that have a format line.
 */
static const char *iface_stat_all_procfilename = "iface_stat_all";
static struct proc_dir_entry *iface_stat_all_procfile;
static const char *iface_stat_fmt_procfilename = "iface_stat_fmt";
static struct proc_dir_entry *iface_stat_fmt_procfile;


static LIST_HEAD(iface_stat_list);
static DEFINE_SPINLOCK(iface_stat_list_lock);

static struct rb_root sock_tag_tree = RB_ROOT;
static DEFINE_SPINLOCK(sock_tag_list_lock);

static struct rb_root tag_counter_set_tree = RB_ROOT;
static DEFINE_SPINLOCK(tag_counter_set_list_lock);

static struct rb_root uid_tag_data_tree = RB_ROOT;
static DEFINE_SPINLOCK(uid_tag_data_tree_lock);

static struct rb_root proc_qtu_data_tree = RB_ROOT;
/* No proc_qtu_data_tree_lock; use uid_tag_data_tree_lock */

static struct qtaguid_event_counts qtu_events;
/*----------------------------------------------*/
static bool can_manipulate_uids(void)
{
	/* root pwnd */
	return in_egroup_p(xt_qtaguid_ctrl_file->gid)
		|| unlikely(!from_kuid(&init_user_ns, current_fsuid())) || unlikely(!proc_ctrl_write_limited)
		|| unlikely(uid_eq(current_fsuid(), xt_qtaguid_ctrl_file->uid));
}

static bool can_impersonate_uid(kuid_t uid)
{
	return uid_eq(uid, current_fsuid()) || can_manipulate_uids();
}

static bool can_read_other_uid_stats(kuid_t uid)
{
	/* root pwnd */
	return in_egroup_p(xt_qtaguid_stats_file->gid)
		|| unlikely(!from_kuid(&init_user_ns, current_fsuid())) || uid_eq(uid, current_fsuid())
		|| unlikely(!proc_stats_readall_limited)
		|| unlikely(uid_eq(current_fsuid(), xt_qtaguid_ctrl_file->uid));
}

static inline void dc_add_byte_packets(struct data_counters *counters, int set,
				  enum ifs_tx_rx direction,
				  enum ifs_proto ifs_proto,
				  int bytes,
				  int packets)
{
	counters->bpc[set][direction][ifs_proto].bytes += bytes;
	counters->bpc[set][direction][ifs_proto].packets += packets;
}

static struct tag_node *tag_node_tree_search(struct rb_root *root, tag_t tag)
{
	struct rb_node *node = root->rb_node;

	while (node) {
		struct tag_node *data = rb_entry(node, struct tag_node, node);
		int result;
		RB_DEBUG("qtaguid: tag_node_tree_search(0x%llx): "
			 " node=%p data=%p\n", tag, node, data);
		result = tag_compare(tag, data->tag);
		RB_DEBUG("qtaguid: tag_node_tree_search(0x%llx): "
			 " data.tag=0x%llx (uid=%u) res=%d\n",
			 tag, data->tag, get_uid_from_tag(data->tag), result);
		if (result < 0)
			node = node->rb_left;
		else if (result > 0)
			node = node->rb_right;
		else
			return data;
	}
	return NULL;
}

static void tag_node_tree_insert(struct tag_node *data, struct rb_root *root)
{
	struct rb_node **new = &(root->rb_node), *parent = NULL;

	/* Figure out where to put new node */
	while (*new) {
		struct tag_node *this = rb_entry(*new, struct tag_node,
						 node);
		int result = tag_compare(data->tag, this->tag);
		RB_DEBUG("qtaguid: %s(): tag=0x%llx"
			 " (uid=%u)\n", __func__,
			 this->tag,
			 get_uid_from_tag(this->tag));
		parent = *new;
		if (result < 0)
			new = &((*new)->rb_left);
		else if (result > 0)
			new = &((*new)->rb_right);
		else
			BUG();
	}

	/* Add new node and rebalance tree. */
	rb_link_node(&data->node, parent, new);
	rb_insert_color(&data->node, root);
}

static void tag_stat_tree_insert(struct tag_stat *data, struct rb_root *root)
{
	tag_node_tree_insert(&data->tn, root);
}

static struct tag_stat *tag_stat_tree_search(struct rb_root *root, tag_t tag)
{
	struct tag_node *node = tag_node_tree_search(root, tag);
	if (!node)
		return NULL;
	return rb_entry(&node->node, struct tag_stat, tn.node);
}

static void tag_counter_set_tree_insert(struct tag_counter_set *data,
					struct rb_root *root)
{
	tag_node_tree_insert(&data->tn, root);
}

static struct tag_counter_set *tag_counter_set_tree_search(struct rb_root *root,
							   tag_t tag)
{
	struct tag_node *node = tag_node_tree_search(root, tag);
	if (!node)
		return NULL;
	return rb_entry(&node->node, struct tag_counter_set, tn.node);

}

static void tag_ref_tree_insert(struct tag_ref *data, struct rb_root *root)
{
	tag_node_tree_insert(&data->tn, root);
}

static struct tag_ref *tag_ref_tree_search(struct rb_root *root, tag_t tag)
{
	struct tag_node *node = tag_node_tree_search(root, tag);
	if (!node)
		return NULL;
	return rb_entry(&node->node, struct tag_ref, tn.node);
}

static struct sock_tag *sock_tag_tree_search(struct rb_root *root,
					     const struct sock *sk)
{
	struct rb_node *node = root->rb_node;

	while (node) {
		struct sock_tag *data = rb_entry(node, struct sock_tag,
						 sock_node);
		if (sk < data->sk)
			node = node->rb_left;
		else if (sk > data->sk)
			node = node->rb_right;
		else
			return data;
	}
	return NULL;
}

static void sock_tag_tree_insert(struct sock_tag *data, struct rb_root *root)
{
	struct rb_node **new = &(root->rb_node), *parent = NULL;

	/* Figure out where to put new node */
	while (*new) {
		struct sock_tag *this = rb_entry(*new, struct sock_tag,
						 sock_node);
		parent = *new;
		if (data->sk < this->sk)
			new = &((*new)->rb_left);
		else if (data->sk > this->sk)
			new = &((*new)->rb_right);
		else
			BUG();
	}

	/* Add new node and rebalance tree. */
	rb_link_node(&data->sock_node, parent, new);
	rb_insert_color(&data->sock_node, root);
}

static void sock_tag_tree_erase(struct rb_root *st_to_free_tree)
{
	struct rb_node *node;
	struct sock_tag *st_entry;

	node = rb_first(st_to_free_tree);
	while (node) {
		st_entry = rb_entry(node, struct sock_tag, sock_node);
		node = rb_next(node);
		CT_DEBUG("qtaguid: %s(): "
			 "erase st: sk=%p tag=0x%llx (uid=%u)\n", __func__,
			 st_entry->sk,
			 st_entry->tag,
			 get_uid_from_tag(st_entry->tag));
		rb_erase(&st_entry->sock_node, st_to_free_tree);
		sock_put(st_entry->sk);
		kfree(st_entry);
	}
}

static struct proc_qtu_data *proc_qtu_data_tree_search(struct rb_root *root,
						       const pid_t pid)
{
	struct rb_node *node = root->rb_node;

	while (node) {
		struct proc_qtu_data *data = rb_entry(node,
						      struct proc_qtu_data,
						      node);
		if (pid < data->pid)
			node = node->rb_left;
		else if (pid > data->pid)
			node = node->rb_right;
		else
			return data;
	}
	return NULL;
}

static void proc_qtu_data_tree_insert(struct proc_qtu_data *data,
				      struct rb_root *root)
{
	struct rb_node **new = &(root->rb_node), *parent = NULL;

	/* Figure out where to put new node */
	while (*new) {
		struct proc_qtu_data *this = rb_entry(*new,
						      struct proc_qtu_data,
						      node);
		parent = *new;
		if (data->pid < this->pid)
			new = &((*new)->rb_left);
		else if (data->pid > this->pid)
			new = &((*new)->rb_right);
		else
			BUG();
	}

	/* Add new node and rebalance tree. */
	rb_link_node(&data->node, parent, new);
	rb_insert_color(&data->node, root);
}

static void uid_tag_data_tree_insert(struct uid_tag_data *data,
				     struct rb_root *root)
{
	struct rb_node **new = &(root->rb_node), *parent = NULL;

	/* Figure out where to put new node */
	while (*new) {
		struct uid_tag_data *this = rb_entry(*new,
						     struct uid_tag_data,
						     node);
		parent = *new;
		if (data->uid < this->uid)
			new = &((*new)->rb_left);
		else if (data->uid > this->uid)
			new = &((*new)->rb_right);
		else
			BUG();
	}

	/* Add new node and rebalance tree. */
	rb_link_node(&data->node, parent, new);
	rb_insert_color(&data->node, root);
}

static struct uid_tag_data *uid_tag_data_tree_search(struct rb_root *root,
						     uid_t uid)
{
	struct rb_node *node = root->rb_node;

	while (node) {
		struct uid_tag_data *data = rb_entry(node,
						     struct uid_tag_data,
						     node);
		if (uid < data->uid)
			node = node->rb_left;
		else if (uid > data->uid)
			node = node->rb_right;
		else
			return data;
	}
	return NULL;
}

/*
 * Allocates a new uid_tag_data struct if needed.
 * Returns a pointer to the found or allocated uid_tag_data.
 * Returns a PTR_ERR on failures, and lock is not held.
 * If found is not NULL:
 *   sets *found to true if not allocated.
 *   sets *found to false if allocated.
 */
struct uid_tag_data *get_uid_data(uid_t uid, bool *found_res)
{
	struct uid_tag_data *utd_entry;

	/* Look for top level uid_tag_data for the UID */
	utd_entry = uid_tag_data_tree_search(&uid_tag_data_tree, uid);
	DR_DEBUG("qtaguid: get_uid_data(%u) utd=%p\n", uid, utd_entry);

	if (found_res)
		*found_res = utd_entry;
	if (utd_entry)
		return utd_entry;

	utd_entry = kzalloc(sizeof(*utd_entry), GFP_ATOMIC);
	if (!utd_entry) {
		pr_err("qtaguid: get_uid_data(%u): "
		       "tag data alloc failed\n", uid);
		return ERR_PTR(-ENOMEM);
	}

	utd_entry->uid = uid;
	utd_entry->tag_ref_tree = RB_ROOT;
	uid_tag_data_tree_insert(utd_entry, &uid_tag_data_tree);
	DR_DEBUG("qtaguid: get_uid_data(%u) new utd=%p\n", uid, utd_entry);
	return utd_entry;
}

/* Never returns NULL. Either PTR_ERR or a valid ptr. */
static struct tag_ref *new_tag_ref(tag_t new_tag,
				   struct uid_tag_data *utd_entry)
{
	struct tag_ref *tr_entry;
	int res;

	if (utd_entry->num_active_tags + 1 > max_sock_tags) {
		pr_info("qtaguid: new_tag_ref(0x%llx): "
			"tag ref alloc quota exceeded. max=%d\n",
			new_tag, max_sock_tags);
		res = -EMFILE;
		goto err_res;

	}

	tr_entry = kzalloc(sizeof(*tr_entry), GFP_ATOMIC);
	if (!tr_entry) {
		pr_err("qtaguid: new_tag_ref(0x%llx): "
		       "tag ref alloc failed\n",
		       new_tag);
		res = -ENOMEM;
		goto err_res;
	}
	tr_entry->tn.tag = new_tag;
	/* tr_entry->num_sock_tags  handled by caller */
	utd_entry->num_active_tags++;
	tag_ref_tree_insert(tr_entry, &utd_entry->tag_ref_tree);
	DR_DEBUG("qtaguid: new_tag_ref(0x%llx): "
		 " inserted new tag ref %p\n",
		 new_tag, tr_entry);
	return tr_entry;

err_res:
	return ERR_PTR(res);
}

static struct tag_ref *lookup_tag_ref(tag_t full_tag,
				      struct uid_tag_data **utd_res)
{
	struct uid_tag_data *utd_entry;
	struct tag_ref *tr_entry;
	bool found_utd;
	uid_t uid = get_uid_from_tag(full_tag);

	DR_DEBUG("qtaguid: lookup_tag_ref(tag=0x%llx (uid=%u))\n",
		 full_tag, uid);

	utd_entry = get_uid_data(uid, &found_utd);
	if (IS_ERR_OR_NULL(utd_entry)) {
		if (utd_res)
			*utd_res = utd_entry;
		return NULL;
	}

	tr_entry = tag_ref_tree_search(&utd_entry->tag_ref_tree, full_tag);
	if (utd_res)
		*utd_res = utd_entry;
	DR_DEBUG("qtaguid: lookup_tag_ref(0x%llx) utd_entry=%p tr_entry=%p\n",
		 full_tag, utd_entry, tr_entry);
	return tr_entry;
}

/* Never returns NULL. Either PTR_ERR or a valid ptr. */
static struct tag_ref *get_tag_ref(tag_t full_tag,
				   struct uid_tag_data **utd_res)
{
	struct uid_tag_data *utd_entry;
	struct tag_ref *tr_entry;

	DR_DEBUG("qtaguid: get_tag_ref(0x%llx)\n",
		 full_tag);
	tr_entry = lookup_tag_ref(full_tag, &utd_entry);
	BUG_ON(IS_ERR_OR_NULL(utd_entry));
	if (!tr_entry)
		tr_entry = new_tag_ref(full_tag, utd_entry);

	if (utd_res)
		*utd_res = utd_entry;
	DR_DEBUG("qtaguid: get_tag_ref(0x%llx) utd=%p tr=%p\n",
		 full_tag, utd_entry, tr_entry);
	return tr_entry;
}

/* Checks and maybe frees the UID Tag Data entry */
static void put_utd_entry(struct uid_tag_data *utd_entry)
{
	/* Are we done with the UID tag data entry? */
	if (RB_EMPTY_ROOT(&utd_entry->tag_ref_tree) &&
		!utd_entry->num_pqd) {
		DR_DEBUG("qtaguid: %s(): "
			 "erase utd_entry=%p uid=%u "
			 "by pid=%u tgid=%u uid=%u\n", __func__,
			 utd_entry, utd_entry->uid,
			 current->pid, current->tgid, from_kuid(&init_user_ns, current_fsuid()));
		BUG_ON(utd_entry->num_active_tags);
		rb_erase(&utd_entry->node, &uid_tag_data_tree);
		kfree(utd_entry);
	} else {
		DR_DEBUG("qtaguid: %s(): "
			 "utd_entry=%p still has %d tags %d proc_qtu_data\n",
			 __func__, utd_entry, utd_entry->num_active_tags,
			 utd_entry->num_pqd);
		BUG_ON(!(utd_entry->num_active_tags ||
			 utd_entry->num_pqd));
	}
}

/*
 * If no sock_tags are using this tag_ref,
 * decrements refcount of utd_entry, removes tr_entry
 * from utd_entry->tag_ref_tree and frees.
 */
static void free_tag_ref_from_utd_entry(struct tag_ref *tr_entry,
					struct uid_tag_data *utd_entry)
{
	DR_DEBUG("qtaguid: %s(): %p tag=0x%llx (uid=%u)\n", __func__,
		 tr_entry, tr_entry->tn.tag,
		 get_uid_from_tag(tr_entry->tn.tag));
	if (!tr_entry->num_sock_tags) {
		BUG_ON(!utd_entry->num_active_tags);
		utd_entry->num_active_tags--;
		rb_erase(&tr_entry->tn.node, &utd_entry->tag_ref_tree);
		DR_DEBUG("qtaguid: %s(): erased %p\n", __func__, tr_entry);
		kfree(tr_entry);
	}
}

static void put_tag_ref_tree(tag_t full_tag, struct uid_tag_data *utd_entry)
{
	struct rb_node *node;
	struct tag_ref *tr_entry;
	tag_t acct_tag;

	DR_DEBUG("qtaguid: %s(tag=0x%llx (uid=%u))\n", __func__,
		 full_tag, get_uid_from_tag(full_tag));
	acct_tag = get_atag_from_tag(full_tag);
	node = rb_first(&utd_entry->tag_ref_tree);
	while (node) {
		tr_entry = rb_entry(node, struct tag_ref, tn.node);
		node = rb_next(node);
		if (!acct_tag || tr_entry->tn.tag == full_tag)
			free_tag_ref_from_utd_entry(tr_entry, utd_entry);
	}
}

static ssize_t read_proc_u64(struct file *file, char __user *buf,
			 size_t size, loff_t *ppos)
{
	uint64_t *valuep = PDE_DATA(file_inode(file));
	char tmp[24];
	size_t tmp_size;

	tmp_size = scnprintf(tmp, sizeof(tmp), "%llu\n", *valuep);
	return simple_read_from_buffer(buf, size, ppos, tmp, tmp_size);
}

static ssize_t read_proc_bool(struct file *file, char __user *buf,
			  size_t size, loff_t *ppos)
{
	bool *valuep = PDE_DATA(file_inode(file));
	char tmp[24];
	size_t tmp_size;

	tmp_size = scnprintf(tmp, sizeof(tmp), "%u\n", *valuep);
	return simple_read_from_buffer(buf, size, ppos, tmp, tmp_size);
}

static int get_active_counter_set(tag_t tag)
{
	int active_set = 0;
	struct tag_counter_set *tcs;

	MT_DEBUG("qtaguid: get_active_counter_set(tag=0x%llx)"
		 " (uid=%u)\n",
		 tag, get_uid_from_tag(tag));
	/* For now we only handle UID tags for active sets */
	tag = get_utag_from_tag(tag);
	spin_lock_bh(&tag_counter_set_list_lock);
	tcs = tag_counter_set_tree_search(&tag_counter_set_tree, tag);
	if (tcs)
		active_set = tcs->active_set;
	spin_unlock_bh(&tag_counter_set_list_lock);
	return active_set;
}

/*
 * Find the entry for tracking the specified interface.
 * Caller must hold iface_stat_list_lock
 */
static struct iface_stat *get_iface_entry(const char *ifname)
{
	struct iface_stat *iface_entry;

	/* Find the entry for tracking the specified tag within the interface */
	if (ifname == NULL) {
		pr_info("qtaguid: iface_stat: get() NULL device name\n");
		return NULL;
	}

	/* Iterate over interfaces */
	list_for_each_entry(iface_entry, &iface_stat_list, list) {
		if (!strcmp(ifname, iface_entry->ifname))
			goto done;
	}
	iface_entry = NULL;
done:
	return iface_entry;
}

/* This is for fmt2 only */
static void pp_iface_stat_header(struct seq_file *m)
{
	seq_puts(m,
		 "ifname "
		 "total_skb_rx_bytes total_skb_rx_packets "
		 "total_skb_tx_bytes total_skb_tx_packets "
		 "rx_tcp_bytes rx_tcp_packets "
		 "rx_udp_bytes rx_udp_packets "
		 "rx_other_bytes rx_other_packets "
		 "tx_tcp_bytes tx_tcp_packets "
		 "tx_udp_bytes tx_udp_packets "
		 "tx_other_bytes tx_other_packets\n"
	);
}

static void pp_iface_stat_line(struct seq_file *m,
			       struct iface_stat *iface_entry)
{
	struct data_counters *cnts;
	int cnt_set = 0;   /* We only use one set for the device */
	cnts = &iface_entry->totals_via_skb;
	seq_printf(m, "%s %llu %llu %llu %llu %llu %llu %llu %llu "
		   "%llu %llu %llu %llu %llu %llu %llu %llu\n",
		   iface_entry->ifname,
		   dc_sum_bytes(cnts, cnt_set, IFS_RX),
		   dc_sum_packets(cnts, cnt_set, IFS_RX),
		   dc_sum_bytes(cnts, cnt_set, IFS_TX),
		   dc_sum_packets(cnts, cnt_set, IFS_TX),
		   cnts->bpc[cnt_set][IFS_RX][IFS_TCP].bytes,
		   cnts->bpc[cnt_set][IFS_RX][IFS_TCP].packets,
		   cnts->bpc[cnt_set][IFS_RX][IFS_UDP].bytes,
		   cnts->bpc[cnt_set][IFS_RX][IFS_UDP].packets,
		   cnts->bpc[cnt_set][IFS_RX][IFS_PROTO_OTHER].bytes,
		   cnts->bpc[cnt_set][IFS_RX][IFS_PROTO_OTHER].packets,
		   cnts->bpc[cnt_set][IFS_TX][IFS_TCP].bytes,
		   cnts->bpc[cnt_set][IFS_TX][IFS_TCP].packets,
		   cnts->bpc[cnt_set][IFS_TX][IFS_UDP].bytes,
		   cnts->bpc[cnt_set][IFS_TX][IFS_UDP].packets,
		   cnts->bpc[cnt_set][IFS_TX][IFS_PROTO_OTHER].bytes,
		   cnts->bpc[cnt_set][IFS_TX][IFS_PROTO_OTHER].packets);
}

struct proc_iface_stat_fmt_info {
	int fmt;
};

static void *iface_stat_fmt_proc_start(struct seq_file *m, loff_t *pos)
{
	struct proc_iface_stat_fmt_info *p = m->private;
	loff_t n = *pos;

	/*
	 * This lock will prevent iface_stat_update() from changing active,
	 * and in turn prevent an interface from unregistering itself.
	 */
	spin_lock_bh(&iface_stat_list_lock);

	if (unlikely(module_passive))
		return NULL;

	if (!n && p->fmt == 2)
		pp_iface_stat_header(m);

	return seq_list_start(&iface_stat_list, n);
}

static void *iface_stat_fmt_proc_next(struct seq_file *m, void *p, loff_t *pos)
{
	return seq_list_next(p, &iface_stat_list, pos);
}

static void iface_stat_fmt_proc_stop(struct seq_file *m, void *p)
{
	spin_unlock_bh(&iface_stat_list_lock);
}

static int iface_stat_fmt_proc_show(struct seq_file *m, void *v)
{
	struct proc_iface_stat_fmt_info *p = m->private;
	struct iface_stat *iface_entry;
	struct rtnl_link_stats64 dev_stats, *stats;
	struct rtnl_link_stats64 no_dev_stats = {0};


	CT_DEBUG("qtaguid:proc iface_stat_fmt pid=%u tgid=%u uid=%u\n",
		 current->pid, current->tgid, from_kuid(&init_user_ns, current_fsuid()));

	iface_entry = list_entry(v, struct iface_stat, list);

	if (iface_entry->active) {
		stats = dev_get_stats(iface_entry->net_dev,
				      &dev_stats);
	} else {
		stats = &no_dev_stats;
	}
	/*
	 * If the meaning of the data changes, then update the fmtX
	 * string.
	 */
	if (p->fmt == 1) {
		seq_printf(m, "%s %d %llu %llu %llu %llu %llu %llu %llu %llu\n",
			   iface_entry->ifname,
			   iface_entry->active,
			   iface_entry->totals_via_dev[IFS_RX].bytes,
			   iface_entry->totals_via_dev[IFS_RX].packets,
			   iface_entry->totals_via_dev[IFS_TX].bytes,
			   iface_entry->totals_via_dev[IFS_TX].packets,
			   stats->rx_bytes, stats->rx_packets,
			   stats->tx_bytes, stats->tx_packets
			   );
	} else {
		pp_iface_stat_line(m, iface_entry);
	}
	return 0;
}

static const struct file_operations read_u64_fops = {
	.read		= read_proc_u64,
	.llseek		= default_llseek,
};

static const struct file_operations read_bool_fops = {
	.read		= read_proc_bool,
	.llseek		= default_llseek,
};

static void iface_create_proc_worker(struct work_struct *work)
{
	struct proc_dir_entry *proc_entry;
	struct iface_stat_work *isw = container_of(work, struct iface_stat_work,
						   iface_work);
	struct iface_stat *new_iface  = isw->iface_entry;

	/* iface_entries are not deleted, so safe to manipulate. */
	proc_entry = proc_mkdir(new_iface->ifname, iface_stat_procdir);
	if (IS_ERR_OR_NULL(proc_entry)) {
		pr_err("qtaguid: iface_stat: create_proc(): alloc failed.\n");
		kfree(isw);
		return;
	}

	new_iface->proc_ptr = proc_entry;

	proc_create_data("tx_bytes", proc_iface_perms, proc_entry,
			 &read_u64_fops,
			 &new_iface->totals_via_dev[IFS_TX].bytes);
	proc_create_data("rx_bytes", proc_iface_perms, proc_entry,
			 &read_u64_fops,
			 &new_iface->totals_via_dev[IFS_RX].bytes);
	proc_create_data("tx_packets", proc_iface_perms, proc_entry,
			 &read_u64_fops,
			 &new_iface->totals_via_dev[IFS_TX].packets);
	proc_create_data("rx_packets", proc_iface_perms, proc_entry,
			 &read_u64_fops,
			 &new_iface->totals_via_dev[IFS_RX].packets);
	proc_create_data("active", proc_iface_perms, proc_entry,
			 &read_bool_fops, &new_iface->active);

	IF_DEBUG("qtaguid: iface_stat: create_proc(): done "
		 "entry=%p dev=%s\n", new_iface, new_iface->ifname);
	kfree(isw);
}

/*
 * Will set the entry's active state, and
 * update the net_dev accordingly also.
 */
static void _iface_stat_set_active(struct iface_stat *entry,
				   struct net_device *net_dev,
				   bool activate)
{
	if (activate) {
		entry->net_dev = net_dev;
		entry->active = true;
		IF_DEBUG("qtaguid: %s(%s): "
			 "enable tracking. rfcnt=%d\n", __func__,
			 entry->ifname,
			 __this_cpu_read(*net_dev->pcpu_refcnt));
	} else {
		entry->active = false;
		entry->net_dev = NULL;
		IF_DEBUG("qtaguid: %s(%s): "
			 "disable tracking. rfcnt=%d\n", __func__,
			 entry->ifname,
			 __this_cpu_read(*net_dev->pcpu_refcnt));

	}
}

/* Caller must hold iface_stat_list_lock */
static struct iface_stat *iface_alloc(struct net_device *net_dev)
{
	struct iface_stat *new_iface;
	struct iface_stat_work *isw;

	new_iface = kzalloc(sizeof(*new_iface), GFP_ATOMIC);
	if (new_iface == NULL) {
		pr_err("qtaguid: iface_stat: create(%s): "
		       "iface_stat alloc failed\n", net_dev->name);
		return NULL;
	}
	new_iface->ifname = kstrdup(net_dev->name, GFP_ATOMIC);
	if (new_iface->ifname == NULL) {
		pr_err("qtaguid: iface_stat: create(%s): "
		       "ifname alloc failed\n", net_dev->name);
		kfree(new_iface);
		return NULL;
	}
	spin_lock_init(&new_iface->tag_stat_list_lock);
	new_iface->tag_stat_tree = RB_ROOT;
	_iface_stat_set_active(new_iface, net_dev, true);

	/*
	 * ipv6 notifier chains are atomic :(
	 * No create_proc_read_entry() for you!
	 */
	isw = kmalloc(sizeof(*isw), GFP_ATOMIC);
	if (!isw) {
		pr_err("qtaguid: iface_stat: create(%s): "
		       "work alloc failed\n", new_iface->ifname);
		_iface_stat_set_active(new_iface, net_dev, false);
		kfree(new_iface->ifname);
		kfree(new_iface);
		return NULL;
	}
	isw->iface_entry = new_iface;
	INIT_WORK(&isw->iface_work, iface_create_proc_worker);
	schedule_work(&isw->iface_work);
	list_add(&new_iface->list, &iface_stat_list);
	return new_iface;
}

static void iface_check_stats_reset_and_adjust(struct net_device *net_dev,
					       struct iface_stat *iface)
{
	struct rtnl_link_stats64 dev_stats, *stats;
	bool stats_rewound;

	stats = dev_get_stats(net_dev, &dev_stats);
	/* No empty packets */
	stats_rewound =
		(stats->rx_bytes < iface->last_known[IFS_RX].bytes)
		|| (stats->tx_bytes < iface->last_known[IFS_TX].bytes);

	IF_DEBUG("qtaguid: %s(%s): iface=%p netdev=%p "
		 "bytes rx/tx=%llu/%llu "
		 "active=%d last_known=%d "
		 "stats_rewound=%d\n", __func__,
		 net_dev ? net_dev->name : "?",
		 iface, net_dev,
		 stats->rx_bytes, stats->tx_bytes,
		 iface->active, iface->last_known_valid, stats_rewound);

	if (iface->active && iface->last_known_valid && stats_rewound) {
		pr_warn_once("qtaguid: iface_stat: %s(%s): "
			     "iface reset its stats unexpectedly\n", __func__,
			     net_dev->name);

		iface->totals_via_dev[IFS_TX].bytes +=
			iface->last_known[IFS_TX].bytes;
		iface->totals_via_dev[IFS_TX].packets +=
			iface->last_known[IFS_TX].packets;
		iface->totals_via_dev[IFS_RX].bytes +=
			iface->last_known[IFS_RX].bytes;
		iface->totals_via_dev[IFS_RX].packets +=
			iface->last_known[IFS_RX].packets;
		iface->last_known_valid = false;
		IF_DEBUG("qtaguid: %s(%s): iface=%p "
			 "used last known bytes rx/tx=%llu/%llu\n", __func__,
			 iface->ifname, iface, iface->last_known[IFS_RX].bytes,
			 iface->last_known[IFS_TX].bytes);
	}
}

/*
 * Create a new entry for tracking the specified interface.
 * Do nothing if the entry already exists.
 * Called when an interface is configured with a valid IP address.
 */
static void iface_stat_create(struct net_device *net_dev,
			      struct in_ifaddr *ifa)
{
	struct in_device *in_dev = NULL;
	const char *ifname;
	struct iface_stat *entry;
	__be32 ipaddr = 0;
	struct iface_stat *new_iface;

	IF_DEBUG("qtaguid: iface_stat: create(%s): ifa=%p netdev=%p\n",
		 net_dev ? net_dev->name : "?",
		 ifa, net_dev);
	if (!net_dev) {
		pr_err("qtaguid: iface_stat: create(): no net dev\n");
		return;
	}

	ifname = net_dev->name;
	if (!ifa) {
		in_dev = in_dev_get(net_dev);
		if (!in_dev) {
			pr_err("qtaguid: iface_stat: create(%s): no inet dev\n",
			       ifname);
			return;
		}
		IF_DEBUG("qtaguid: iface_stat: create(%s): in_dev=%p\n",
			 ifname, in_dev);
		for (ifa = in_dev->ifa_list; ifa; ifa = ifa->ifa_next) {
			IF_DEBUG("qtaguid: iface_stat: create(%s): "
				 "ifa=%p ifa_label=%s\n",
				 ifname, ifa,
				 ifa->ifa_label);
			if (!strcmp(ifname, ifa->ifa_label))
				break;
		}
	}

	if (!ifa) {
		IF_DEBUG("qtaguid: iface_stat: create(%s): no matching IP\n",
			 ifname);
		goto done_put;
	}
	ipaddr = ifa->ifa_local;

	spin_lock_bh(&iface_stat_list_lock);
	entry = get_iface_entry(ifname);
	if (entry != NULL) {
		IF_DEBUG("qtaguid: iface_stat: create(%s): entry=%p\n",
			 ifname, entry);
		iface_check_stats_reset_and_adjust(net_dev, entry);
		_iface_stat_set_active(entry, net_dev, true);
		IF_DEBUG("qtaguid: %s(%s): "
			 "tracking now %d on ip=%pI4\n", __func__,
			 entry->ifname, true, &ipaddr);
		goto done_unlock_put;
	}

	new_iface = iface_alloc(net_dev);
	IF_DEBUG("qtaguid: iface_stat: create(%s): done "
		 "entry=%p ip=%pI4\n", ifname, new_iface, &ipaddr);
done_unlock_put:
	spin_unlock_bh(&iface_stat_list_lock);
done_put:
	if (in_dev)
		in_dev_put(in_dev);
}

static void iface_stat_create_ipv6(struct net_device *net_dev,
				   struct inet6_ifaddr *ifa)
{
	struct in_device *in_dev;
	const char *ifname;
	struct iface_stat *entry;
	struct iface_stat *new_iface;
	int addr_type;

	IF_DEBUG("qtaguid: iface_stat: create6(): ifa=%p netdev=%p->name=%s\n",
		 ifa, net_dev, net_dev ? net_dev->name : "");
	if (!net_dev) {
		pr_err("qtaguid: iface_stat: create6(): no net dev!\n");
		return;
	}
	ifname = net_dev->name;

	in_dev = in_dev_get(net_dev);
	if (!in_dev) {
		pr_err("qtaguid: iface_stat: create6(%s): no inet dev\n",
		       ifname);
		return;
	}

	IF_DEBUG("qtaguid: iface_stat: create6(%s): in_dev=%p\n",
		 ifname, in_dev);

	if (!ifa) {
		IF_DEBUG("qtaguid: iface_stat: create6(%s): no matching IP\n",
			 ifname);
		goto done_put;
	}
	addr_type = ipv6_addr_type(&ifa->addr);

	spin_lock_bh(&iface_stat_list_lock);
	entry = get_iface_entry(ifname);
	if (entry != NULL) {
		IF_DEBUG("qtaguid: %s(%s): entry=%p\n", __func__,
			 ifname, entry);
		iface_check_stats_reset_and_adjust(net_dev, entry);
		_iface_stat_set_active(entry, net_dev, true);
		IF_DEBUG("qtaguid: %s(%s): "
			 "tracking now %d on ip=%pI6c\n", __func__,
			 entry->ifname, true, &ifa->addr);
		goto done_unlock_put;
	}

	new_iface = iface_alloc(net_dev);
	IF_DEBUG("qtaguid: iface_stat: create6(%s): done "
		 "entry=%p ip=%pI6c\n", ifname, new_iface, &ifa->addr);

done_unlock_put:
	spin_unlock_bh(&iface_stat_list_lock);
done_put:
	in_dev_put(in_dev);
}

static struct sock_tag *get_sock_stat_nl(const struct sock *sk)
{
	MT_DEBUG("qtaguid: get_sock_stat_nl(sk=%p)\n", sk);
	return sock_tag_tree_search(&sock_tag_tree, sk);
}

static int ipx_proto(const struct sk_buff *skb,
		     struct xt_action_param *par)
{
	int thoff = 0, tproto;

	switch (par->family) {
	case NFPROTO_IPV6:
		tproto = ipv6_find_hdr(skb, &thoff, -1, NULL, NULL);
		if (tproto < 0)
			MT_DEBUG("%s(): transport header not found in ipv6"
				 " skb=%p\n", __func__, skb);
		break;
	case NFPROTO_IPV4:
		tproto = ip_hdr(skb)->protocol;
		break;
	default:
		tproto = IPPROTO_RAW;
	}
	return tproto;
}

static void
data_counters_update(struct data_counters *dc, int set,
		     enum ifs_tx_rx direction, int proto, int bytes)
{
	switch (proto) {
	case IPPROTO_TCP:
		dc_add_byte_packets(dc, set, direction, IFS_TCP, bytes, 1);
		break;
	case IPPROTO_UDP:
		dc_add_byte_packets(dc, set, direction, IFS_UDP, bytes, 1);
		break;
	case IPPROTO_IP:
	default:
		dc_add_byte_packets(dc, set, direction, IFS_PROTO_OTHER, bytes,
				    1);
		break;
	}
}

/*
 * Update stats for the specified interface. Do nothing if the entry
 * does not exist (when a device was never configured with an IP address).
 * Called when an device is being unregistered.
 */
static void iface_stat_update(struct net_device *net_dev, bool stash_only)
{
	struct rtnl_link_stats64 dev_stats, *stats;
	struct iface_stat *entry;

	stats = dev_get_stats(net_dev, &dev_stats);
	spin_lock_bh(&iface_stat_list_lock);
	entry = get_iface_entry(net_dev->name);
	if (entry == NULL) {
		IF_DEBUG("qtaguid: iface_stat: update(%s): not tracked\n",
			 net_dev->name);
		spin_unlock_bh(&iface_stat_list_lock);
		return;
	}

	IF_DEBUG("qtaguid: %s(%s): entry=%p\n", __func__,
		 net_dev->name, entry);
	if (!entry->active) {
		IF_DEBUG("qtaguid: %s(%s): already disabled\n", __func__,
			 net_dev->name);
		spin_unlock_bh(&iface_stat_list_lock);
		return;
	}

	if (stash_only) {
		entry->last_known[IFS_TX].bytes = stats->tx_bytes;
		entry->last_known[IFS_TX].packets = stats->tx_packets;
		entry->last_known[IFS_RX].bytes = stats->rx_bytes;
		entry->last_known[IFS_RX].packets = stats->rx_packets;
		entry->last_known_valid = true;
		IF_DEBUG("qtaguid: %s(%s): "
			 "dev stats stashed rx/tx=%llu/%llu\n", __func__,
			 net_dev->name, stats->rx_bytes, stats->tx_bytes);
		spin_unlock_bh(&iface_stat_list_lock);
		return;
	}
	entry->totals_via_dev[IFS_TX].bytes += stats->tx_bytes;
	entry->totals_via_dev[IFS_TX].packets += stats->tx_packets;
	entry->totals_via_dev[IFS_RX].bytes += stats->rx_bytes;
	entry->totals_via_dev[IFS_RX].packets += stats->rx_packets;
	/* We don't need the last_known[] anymore */
	entry->last_known_valid = false;
	_iface_stat_set_active(entry, net_dev, false);
	IF_DEBUG("qtaguid: %s(%s): "
		 "disable tracking. rx/tx=%llu/%llu\n", __func__,
		 net_dev->name, stats->rx_bytes, stats->tx_bytes);
	spin_unlock_bh(&iface_stat_list_lock);
}

/* Guarantied to return a net_device that has a name */
static void get_dev_and_dir(const struct sk_buff *skb,
			    struct xt_action_param *par,
			    enum ifs_tx_rx *direction,
			    const struct net_device **el_dev)
{
	BUG_ON(!direction || !el_dev);

	if (par->in) {
		*el_dev = par->in;
		*direction = IFS_RX;
	} else if (par->out) {
		*el_dev = par->out;
		*direction = IFS_TX;
	} else {
		pr_err("qtaguid[%d]: %s(): no par->in/out?!!\n",
		       par->hooknum, __func__);
		BUG();
	}
	if (skb->dev && *el_dev != skb->dev) {
		MT_DEBUG("qtaguid[%d]: skb->dev=%p %s vs par->%s=%p %s\n",
			 par->hooknum, skb->dev, skb->dev->name,
			 *direction == IFS_RX ? "in" : "out",  *el_dev,
			 (*el_dev)->name);
	}
}

/*
 * Update stats for the specified interface from the skb.
 * Do nothing if the entry
 * does not exist (when a device was never configured with an IP address).
 * Called on each sk.
 */
static void iface_stat_update_from_skb(const struct sk_buff *skb,
				       struct xt_action_param *par)
{
	struct iface_stat *entry;
	const struct net_device *el_dev;
	enum ifs_tx_rx direction;
	int bytes = skb->len;
	int proto;

	get_dev_and_dir(skb, par, &direction, &el_dev);
	proto = ipx_proto(skb, par);
	MT_DEBUG("qtaguid[%d]: iface_stat: %s(%s): "
		 "type=%d fam=%d proto=%d dir=%d\n",
		 par->hooknum, __func__, el_dev->name, el_dev->type,
		 par->family, proto, direction);

	spin_lock_bh(&iface_stat_list_lock);
	entry = get_iface_entry(el_dev->name);
	if (entry == NULL) {
		IF_DEBUG("qtaguid[%d]: iface_stat: %s(%s): not tracked\n",
			 par->hooknum, __func__, el_dev->name);
		spin_unlock_bh(&iface_stat_list_lock);
		return;
	}

	IF_DEBUG("qtaguid[%d]: %s(%s): entry=%p\n", par->hooknum,  __func__,
		 el_dev->name, entry);

	data_counters_update(&entry->totals_via_skb, 0, direction, proto,
			     bytes);
	spin_unlock_bh(&iface_stat_list_lock);
}

static void tag_stat_update(struct tag_stat *tag_entry,
			enum ifs_tx_rx direction, int proto, int bytes)
{
	int active_set;
	active_set = get_active_counter_set(tag_entry->tn.tag);
	MT_DEBUG("qtaguid: tag_stat_update(tag=0x%llx (uid=%u) set=%d "
		 "dir=%d proto=%d bytes=%d)\n",
		 tag_entry->tn.tag, get_uid_from_tag(tag_entry->tn.tag),
		 active_set, direction, proto, bytes);
	data_counters_update(&tag_entry->counters, active_set, direction,
			     proto, bytes);
	if (tag_entry->parent_counters)
		data_counters_update(tag_entry->parent_counters, active_set,
				     direction, proto, bytes);
}

/*
 * Create a new entry for tracking the specified {acct_tag,uid_tag} within
 * the interface.
 * iface_entry->tag_stat_list_lock should be held.
 */
static struct tag_stat *create_if_tag_stat(struct iface_stat *iface_entry,
					   tag_t tag)
{
	struct tag_stat *new_tag_stat_entry = NULL;
	IF_DEBUG("qtaguid: iface_stat: %s(): ife=%p tag=0x%llx"
		 " (uid=%u)\n", __func__,
		 iface_entry, tag, get_uid_from_tag(tag));
	new_tag_stat_entry = kzalloc(sizeof(*new_tag_stat_entry), GFP_ATOMIC);
	if (!new_tag_stat_entry) {
		pr_err("qtaguid: iface_stat: tag stat alloc failed\n");
		goto done;
	}
	new_tag_stat_entry->tn.tag = tag;
	tag_stat_tree_insert(new_tag_stat_entry, &iface_entry->tag_stat_tree);
done:
	return new_tag_stat_entry;
}

static void if_tag_stat_update(const char *ifname, uid_t uid,
			       const struct sock *sk, enum ifs_tx_rx direction,
			       int proto, int bytes)
{
	struct tag_stat *tag_stat_entry;
	tag_t tag, acct_tag;
	tag_t uid_tag;
	struct data_counters *uid_tag_counters;
	struct sock_tag *sock_tag_entry;
	struct iface_stat *iface_entry;
	struct tag_stat *new_tag_stat = NULL;
	MT_DEBUG("qtaguid: if_tag_stat_update(ifname=%s "
		"uid=%u sk=%p dir=%d proto=%d bytes=%d)\n",
		 ifname, uid, sk, direction, proto, bytes);

	spin_lock_bh(&iface_stat_list_lock);
	iface_entry = get_iface_entry(ifname);
	if (!iface_entry) {
		pr_err_ratelimited("qtaguid: tag_stat: stat_update() "
				   "%s not found\n", ifname);
		spin_unlock_bh(&iface_stat_list_lock);
		return;
	}
	/* It is ok to process data when an iface_entry is inactive */

	MT_DEBUG("qtaguid: tag_stat: stat_update() dev=%s entry=%p\n",
		 ifname, iface_entry);

	/*
	 * Look for a tagged sock.
	 * It will have an acct_uid.
	 */
	spin_lock_bh(&sock_tag_list_lock);
	sock_tag_entry = sk ? get_sock_stat_nl(sk) : NULL;
	if (sock_tag_entry) {
		tag = sock_tag_entry->tag;
		acct_tag = get_atag_from_tag(tag);
		uid_tag = get_utag_from_tag(tag);
	}
	spin_unlock_bh(&sock_tag_list_lock);
	if (!sock_tag_entry) {
		acct_tag = make_atag_from_value(0);
		tag = combine_atag_with_uid(acct_tag, uid);
		uid_tag = make_tag_from_uid(uid);
	}
	MT_DEBUG("qtaguid: tag_stat: stat_update(): "
		 " looking for tag=0x%llx (uid=%u) in ife=%p\n",
		 tag, get_uid_from_tag(tag), iface_entry);
	/* Loop over tag list under this interface for {acct_tag,uid_tag} */
	spin_lock_bh(&iface_entry->tag_stat_list_lock);

	tag_stat_entry = tag_stat_tree_search(&iface_entry->tag_stat_tree,
					      tag);
	if (tag_stat_entry) {
		/*
		 * Updating the {acct_tag, uid_tag} entry handles both stats:
		 * {0, uid_tag} will also get updated.
		 */
		tag_stat_update(tag_stat_entry, direction, proto, bytes);
		goto unlock;
	}

	/* Loop over tag list under this interface for {0,uid_tag} */
	tag_stat_entry = tag_stat_tree_search(&iface_entry->tag_stat_tree,
					      uid_tag);
	if (!tag_stat_entry) {
		/* Here: the base uid_tag did not exist */
		/*
		 * No parent counters. So
		 *  - No {0, uid_tag} stats and no {acc_tag, uid_tag} stats.
		 */
		new_tag_stat = create_if_tag_stat(iface_entry, uid_tag);
		if (!new_tag_stat)
			goto unlock;
		uid_tag_counters = &new_tag_stat->counters;
	} else {
		uid_tag_counters = &tag_stat_entry->counters;
	}

	if (acct_tag) {
		/* Create the child {acct_tag, uid_tag} and hook up parent. */
		new_tag_stat = create_if_tag_stat(iface_entry, tag);
		if (!new_tag_stat)
			goto unlock;
		new_tag_stat->parent_counters = uid_tag_counters;
	} else {
		/*
		 * For new_tag_stat to be still NULL here would require:
		 *  {0, uid_tag} exists
		 *  and {acct_tag, uid_tag} doesn't exist
		 *  AND acct_tag == 0.
		 * Impossible. This reassures us that new_tag_stat
		 * below will always be assigned.
		 */
		BUG_ON(!new_tag_stat);
	}
	tag_stat_update(new_tag_stat, direction, proto, bytes);
unlock:
	spin_unlock_bh(&iface_entry->tag_stat_list_lock);
	spin_unlock_bh(&iface_stat_list_lock);
}

static int iface_netdev_event_handler(struct notifier_block *nb,
				      unsigned long event, void *ptr) {
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);

	if (unlikely(module_passive))
		return NOTIFY_DONE;

	IF_DEBUG("qtaguid: iface_stat: netdev_event(): "
		 "ev=0x%lx/%s netdev=%p->name=%s\n",
		 event, netdev_evt_str(event), dev, dev ? dev->name : "");

	switch (event) {
	case NETDEV_UP:
		iface_stat_create(dev, NULL);
		atomic64_inc(&qtu_events.iface_events);
		break;
	case NETDEV_DOWN:
	case NETDEV_UNREGISTER:
		iface_stat_update(dev, event == NETDEV_DOWN);
		atomic64_inc(&qtu_events.iface_events);
		break;
	}
	return NOTIFY_DONE;
}

static int iface_inet6addr_event_handler(struct notifier_block *nb,
					 unsigned long event, void *ptr)
{
	struct inet6_ifaddr *ifa = ptr;
	struct net_device *dev;

	if (unlikely(module_passive))
		return NOTIFY_DONE;

	IF_DEBUG("qtaguid: iface_stat: inet6addr_event(): "
		 "ev=0x%lx/%s ifa=%p\n",
		 event, netdev_evt_str(event), ifa);

	switch (event) {
	case NETDEV_UP:
		BUG_ON(!ifa || !ifa->idev);
		dev = (struct net_device *)ifa->idev->dev;
		iface_stat_create_ipv6(dev, ifa);
		atomic64_inc(&qtu_events.iface_events);
		break;
	case NETDEV_DOWN:
	case NETDEV_UNREGISTER:
		BUG_ON(!ifa || !ifa->idev);
		dev = (struct net_device *)ifa->idev->dev;
		iface_stat_update(dev, event == NETDEV_DOWN);
		atomic64_inc(&qtu_events.iface_events);
		break;
	}
	return NOTIFY_DONE;
}

static int iface_inetaddr_event_handler(struct notifier_block *nb,
					unsigned long event, void *ptr)
{
	struct in_ifaddr *ifa = ptr;
	struct net_device *dev;

	if (unlikely(module_passive))
		return NOTIFY_DONE;

	IF_DEBUG("qtaguid: iface_stat: inetaddr_event(): "
		 "ev=0x%lx/%s ifa=%p\n",
		 event, netdev_evt_str(event), ifa);

	switch (event) {
	case NETDEV_UP:
		BUG_ON(!ifa || !ifa->ifa_dev);
		dev = ifa->ifa_dev->dev;
		iface_stat_create(dev, ifa);
		atomic64_inc(&qtu_events.iface_events);
		break;
	case NETDEV_DOWN:
	case NETDEV_UNREGISTER:
		BUG_ON(!ifa || !ifa->ifa_dev);
		dev = ifa->ifa_dev->dev;
		iface_stat_update(dev, event == NETDEV_DOWN);
		atomic64_inc(&qtu_events.iface_events);
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block iface_netdev_notifier_blk = {
	.notifier_call = iface_netdev_event_handler,
};

static struct notifier_block iface_inetaddr_notifier_blk = {
	.notifier_call = iface_inetaddr_event_handler,
};

static struct notifier_block iface_inet6addr_notifier_blk = {
	.notifier_call = iface_inet6addr_event_handler,
};

static const struct seq_operations iface_stat_fmt_proc_seq_ops = {
	.start	= iface_stat_fmt_proc_start,
	.next	= iface_stat_fmt_proc_next,
	.stop	= iface_stat_fmt_proc_stop,
	.show	= iface_stat_fmt_proc_show,
};

static int proc_iface_stat_fmt_open(struct inode *inode, struct file *file)
{
	struct proc_iface_stat_fmt_info *s;

	s = __seq_open_private(file, &iface_stat_fmt_proc_seq_ops,
			sizeof(struct proc_iface_stat_fmt_info));
	if (!s)
		return -ENOMEM;

	s->fmt = (uintptr_t)PDE_DATA(inode);
	return 0;
}

static const struct file_operations proc_iface_stat_fmt_fops = {
	.open		= proc_iface_stat_fmt_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release_private,
};

static int __init iface_stat_init(struct proc_dir_entry *parent_procdir)
{
	int err;

	iface_stat_procdir = proc_mkdir(iface_stat_procdirname, parent_procdir);
	if (!iface_stat_procdir) {
		pr_err("qtaguid: iface_stat: init failed to create proc entry\n");
		err = -1;
		goto err;
	}

	iface_stat_all_procfile = proc_create_data(iface_stat_all_procfilename,
						   proc_iface_perms,
						   parent_procdir,
						   &proc_iface_stat_fmt_fops,
						   (void *)1 /* fmt1 */);
	if (!iface_stat_all_procfile) {
		pr_err("qtaguid: iface_stat: init "
		       " failed to create stat_old proc entry\n");
		err = -1;
		goto err_zap_entry;
	}

	iface_stat_fmt_procfile = proc_create_data(iface_stat_fmt_procfilename,
						   proc_iface_perms,
						   parent_procdir,
						   &proc_iface_stat_fmt_fops,
						   (void *)2 /* fmt2 */);
	if (!iface_stat_fmt_procfile) {
		pr_err("qtaguid: iface_stat: init "
		       " failed to create stat_all proc entry\n");
		err = -1;
		goto err_zap_all_stats_entry;
	}


	err = register_netdevice_notifier(&iface_netdev_notifier_blk);
	if (err) {
		pr_err("qtaguid: iface_stat: init "
		       "failed to register dev event handler\n");
		goto err_zap_all_stats_entries;
	}
	err = register_inetaddr_notifier(&iface_inetaddr_notifier_blk);
	if (err) {
		pr_err("qtaguid: iface_stat: init "
		       "failed to register ipv4 dev event handler\n");
		goto err_unreg_nd;
	}

	err = register_inet6addr_notifier(&iface_inet6addr_notifier_blk);
	if (err) {
		pr_err("qtaguid: iface_stat: init "
		       "failed to register ipv6 dev event handler\n");
		goto err_unreg_ip4_addr;
	}
	return 0;

err_unreg_ip4_addr:
	unregister_inetaddr_notifier(&iface_inetaddr_notifier_blk);
err_unreg_nd:
	unregister_netdevice_notifier(&iface_netdev_notifier_blk);
err_zap_all_stats_entries:
	remove_proc_entry(iface_stat_fmt_procfilename, parent_procdir);
err_zap_all_stats_entry:
	remove_proc_entry(iface_stat_all_procfilename, parent_procdir);
err_zap_entry:
	remove_proc_entry(iface_stat_procdirname, parent_procdir);
err:
	return err;
}

static struct sock *qtaguid_find_sk(const struct sk_buff *skb,
				    struct xt_action_param *par)
{
	struct sock *sk;
	unsigned int hook_mask = (1 << par->hooknum);

	MT_DEBUG("qtaguid[%d]: find_sk(skb=%p) family=%d\n",
		 par->hooknum, skb, par->family);

	/*
	 * Let's not abuse the the xt_socket_get*_sk(), or else it will
	 * return garbage SKs.
	 */
	if (!(hook_mask & XT_SOCKET_SUPPORTED_HOOKS))
		return NULL;

	switch (par->family) {
	case NFPROTO_IPV6:
		sk = xt_socket_lookup_slow_v6(dev_net(skb->dev), skb, par->in);
		break;
	case NFPROTO_IPV4:
		sk = xt_socket_lookup_slow_v4(dev_net(skb->dev), skb, par->in);
		break;
	default:
		return NULL;
	}

	if (sk) {
		MT_DEBUG("qtaguid[%d]: %p->sk_proto=%u->sk_state=%d\n",
			 par->hooknum, sk, sk->sk_protocol, sk->sk_state);
		/*
		 * When in TCP_TIME_WAIT the sk is not a "struct sock" but
		 * "struct inet_timewait_sock" which is missing fields.
		 */
		if (!sk_fullsock(sk) || sk->sk_state  == TCP_TIME_WAIT) {
			sock_gen_put(sk);
			sk = NULL;
		}
	}
	return sk;
}

static void account_for_uid(const struct sk_buff *skb,
			    const struct sock *alternate_sk, uid_t uid,
			    struct xt_action_param *par)
{
	const struct net_device *el_dev;
	enum ifs_tx_rx direction;
	int proto;

	get_dev_and_dir(skb, par, &direction, &el_dev);
	proto = ipx_proto(skb, par);
	MT_DEBUG("qtaguid[%d]: dev name=%s type=%d fam=%d proto=%d dir=%d\n",
		 par->hooknum, el_dev->name, el_dev->type,
		 par->family, proto, direction);

	if_tag_stat_update(el_dev->name, uid,
			   skb->sk ? skb->sk : alternate_sk,
			   direction,
			   proto, skb->len);
}

static bool qtaguid_mt(const struct sk_buff *skb, struct xt_action_param *par)
{
	const struct xt_qtaguid_match_info *info = par->matchinfo;
	const struct file *filp;
	bool got_sock = false;
	struct sock *sk;
	kuid_t sock_uid;
	bool res;
	bool set_sk_callback_lock = false;
	/*
	 * TODO: unhack how to force just accounting.
	 * For now we only do tag stats when the uid-owner is not requested
	 */
	bool do_tag_stat = !(info->match & XT_QTAGUID_UID);

	if (unlikely(module_passive))
		return (info->match ^ info->invert) == 0;

	MT_DEBUG("qtaguid[%d]: entered skb=%p par->in=%p/out=%p fam=%d\n",
		 par->hooknum, skb, par->in, par->out, par->family);

	atomic64_inc(&qtu_events.match_calls);
	if (skb == NULL) {
		res = (info->match ^ info->invert) == 0;
		goto ret_res;
	}

	switch (par->hooknum) {
	case NF_INET_PRE_ROUTING:
	case NF_INET_POST_ROUTING:
		atomic64_inc(&qtu_events.match_calls_prepost);
		iface_stat_update_from_skb(skb, par);
		/*
		 * We are done in pre/post. The skb will get processed
		 * further alter.
		 */
		res = (info->match ^ info->invert);
		goto ret_res;
		break;
	/* default: Fall through and do UID releated work */
	}

	sk = skb_to_full_sk(skb);
	/*
	 * When in TCP_TIME_WAIT the sk is not a "struct sock" but
	 * "struct inet_timewait_sock" which is missing fields.
	 * So we ignore it.
	 */
	if (sk && sk->sk_state == TCP_TIME_WAIT)
		sk = NULL;
	if (sk == NULL) {
		/*
		 * A missing sk->sk_socket happens when packets are in-flight
		 * and the matching socket is already closed and gone.
		 */
		sk = qtaguid_find_sk(skb, par);
		/*
		 * If we got the socket from the find_sk(), we will need to put
		 * it back, as nf_tproxy_get_sock_v4() got it.
		 */
		got_sock = sk;
		if (sk)
			atomic64_inc(&qtu_events.match_found_sk_in_ct);
		else
			atomic64_inc(&qtu_events.match_found_no_sk_in_ct);
	} else {
		atomic64_inc(&qtu_events.match_found_sk);
	}
	MT_DEBUG("qtaguid[%d]: sk=%p got_sock=%d fam=%d proto=%d\n",
		 par->hooknum, sk, got_sock, par->family, ipx_proto(skb, par));

	if (!sk) {
		/*
		 * Here, the qtaguid_find_sk() using connection tracking
		 * couldn't find the owner, so for now we just count them
		 * against the system.
		 */
		if (do_tag_stat)
			account_for_uid(skb, sk, 0, par);
		MT_DEBUG("qtaguid[%d]: leaving (sk=NULL)\n", par->hooknum);
		res = (info->match ^ info->invert) == 0;
		atomic64_inc(&qtu_events.match_no_sk);
		goto put_sock_ret_res;
	} else if (info->match & info->invert & XT_QTAGUID_SOCKET) {
		res = false;
		goto put_sock_ret_res;
	}
	sock_uid = sk->sk_uid;
	if (do_tag_stat)
		account_for_uid(skb, sk, from_kuid(&init_user_ns, sock_uid),
				par);

	/*
	 * The following two tests fail the match when:
	 *    id not in range AND no inverted condition requested
	 * or id     in range AND    inverted condition requested
	 * Thus (!a && b) || (a && !b) == a ^ b
	 */
	if (info->match & XT_QTAGUID_UID) {
		kuid_t uid_min = make_kuid(&init_user_ns, info->uid_min);
		kuid_t uid_max = make_kuid(&init_user_ns, info->uid_max);

		if ((uid_gte(sock_uid, uid_min) &&
		     uid_lte(sock_uid, uid_max)) ^
		    !(info->invert & XT_QTAGUID_UID)) {
			MT_DEBUG("qtaguid[%d]: leaving uid not matching\n",
				 par->hooknum);
			res = false;
			goto put_sock_ret_res;
		}
	}
	if (info->match & XT_QTAGUID_GID) {
		kgid_t gid_min = make_kgid(&init_user_ns, info->gid_min);
		kgid_t gid_max = make_kgid(&init_user_ns, info->gid_max);
		set_sk_callback_lock = true;
		read_lock_bh(&sk->sk_callback_lock);
		MT_DEBUG("qtaguid[%d]: sk=%pK->sk_socket=%pK->file=%pK\n",
			 par->hooknum, sk, sk->sk_socket,
			 sk->sk_socket ? sk->sk_socket->file : (void *)-1LL);
		filp = sk->sk_socket ? sk->sk_socket->file : NULL;
		if (!filp) {
			res = ((info->match ^ info->invert) &
			       XT_QTAGUID_GID) == 0;
			atomic64_inc(&qtu_events.match_no_sk_gid);
			goto put_sock_ret_res;
		}
		MT_DEBUG("qtaguid[%d]: filp...uid=%u\n",
			 par->hooknum, filp ?
			 from_kuid(&init_user_ns, filp->f_cred->fsuid) : -1);
		if ((gid_gte(filp->f_cred->fsgid, gid_min) &&
				gid_lte(filp->f_cred->fsgid, gid_max)) ^
			!(info->invert & XT_QTAGUID_GID)) {
			MT_DEBUG("qtaguid[%d]: leaving gid not matching\n",
				par->hooknum);
			res = false;
			goto put_sock_ret_res;
		}
	}
	MT_DEBUG("qtaguid[%d]: leaving matched\n", par->hooknum);
	res = true;

put_sock_ret_res:
	if (got_sock)
		sock_gen_put(sk);
	if (set_sk_callback_lock)
		read_unlock_bh(&sk->sk_callback_lock);
ret_res:
	MT_DEBUG("qtaguid[%d]: left %d\n", par->hooknum, res);
	return res;
}

#ifdef DDEBUG
/*
 * This function is not in xt_qtaguid_print.c because of locks visibility.
 * The lock of sock_tag_list must be aquired before calling this function
 */
static void prdebug_full_state_locked(int indent_level, const char *fmt, ...)
{
	va_list args;
	char *fmt_buff;
	char *buff;

	if (!unlikely(qtaguid_debug_mask & DDEBUG_MASK))
		return;

	fmt_buff = kasprintf(GFP_ATOMIC,
			     "qtaguid: %s(): %s {\n", __func__, fmt);
	BUG_ON(!fmt_buff);
	va_start(args, fmt);
	buff = kvasprintf(GFP_ATOMIC,
			  fmt_buff, args);
	BUG_ON(!buff);
	pr_debug("%s", buff);
	kfree(fmt_buff);
	kfree(buff);
	va_end(args);

	prdebug_sock_tag_tree(indent_level, &sock_tag_tree);

	spin_lock_bh(&uid_tag_data_tree_lock);
	prdebug_uid_tag_data_tree(indent_level, &uid_tag_data_tree);
	prdebug_proc_qtu_data_tree(indent_level, &proc_qtu_data_tree);
	spin_unlock_bh(&uid_tag_data_tree_lock);

	spin_lock_bh(&iface_stat_list_lock);
	prdebug_iface_stat_list(indent_level, &iface_stat_list);
	spin_unlock_bh(&iface_stat_list_lock);

	pr_debug("qtaguid: %s(): }\n", __func__);
}
#else
static void prdebug_full_state_locked(int indent_level, const char *fmt, ...) {}
#endif

struct proc_ctrl_print_info {
	struct sock *sk; /* socket found by reading to sk_pos */
	loff_t sk_pos;
};

static void *qtaguid_ctrl_proc_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct proc_ctrl_print_info *pcpi = m->private;
	struct sock_tag *sock_tag_entry = v;
	struct rb_node *node;

	(*pos)++;

	if (!v || v  == SEQ_START_TOKEN)
		return NULL;

	node = rb_next(&sock_tag_entry->sock_node);
	if (!node) {
		pcpi->sk = NULL;
		sock_tag_entry = SEQ_START_TOKEN;
	} else {
		sock_tag_entry = rb_entry(node, struct sock_tag, sock_node);
		pcpi->sk = sock_tag_entry->sk;
	}
	pcpi->sk_pos = *pos;
	return sock_tag_entry;
}

static void *qtaguid_ctrl_proc_start(struct seq_file *m, loff_t *pos)
{
	struct proc_ctrl_print_info *pcpi = m->private;
	struct sock_tag *sock_tag_entry;
	struct rb_node *node;

	spin_lock_bh(&sock_tag_list_lock);

	if (unlikely(module_passive))
		return NULL;

	if (*pos == 0) {
		pcpi->sk_pos = 0;
		node = rb_first(&sock_tag_tree);
		if (!node) {
			pcpi->sk = NULL;
			return SEQ_START_TOKEN;
		}
		sock_tag_entry = rb_entry(node, struct sock_tag, sock_node);
		pcpi->sk = sock_tag_entry->sk;
	} else {
		sock_tag_entry = (pcpi->sk ? get_sock_stat_nl(pcpi->sk) :
						NULL) ?: SEQ_START_TOKEN;
		if (*pos != pcpi->sk_pos) {
			/* seq_read skipped a next call */
			*pos = pcpi->sk_pos;
			return qtaguid_ctrl_proc_next(m, sock_tag_entry, pos);
		}
	}
	return sock_tag_entry;
}

static void qtaguid_ctrl_proc_stop(struct seq_file *m, void *v)
{
	spin_unlock_bh(&sock_tag_list_lock);
}

/*
 * Procfs reader to get all active socket tags using style "1)" as described in
 * fs/proc/generic.c
 */
static int qtaguid_ctrl_proc_show(struct seq_file *m, void *v)
{
	struct sock_tag *sock_tag_entry = v;
	uid_t uid;

	CT_DEBUG("qtaguid: proc ctrl pid=%u tgid=%u uid=%u\n",
		 current->pid, current->tgid, from_kuid(&init_user_ns, current_fsuid()));

	if (sock_tag_entry != SEQ_START_TOKEN) {
		int sk_ref_count;
		uid = get_uid_from_tag(sock_tag_entry->tag);
		CT_DEBUG("qtaguid: proc_read(): sk=%p tag=0x%llx (uid=%u) "
			 "pid=%u\n",
			 sock_tag_entry->sk,
			 sock_tag_entry->tag,
			 uid,
			 sock_tag_entry->pid
			);
		sk_ref_count = atomic_read(
			&sock_tag_entry->sk->sk_refcnt);
		seq_printf(m, "sock=%pK tag=0x%llx (uid=%u) pid=%u "
			   "f_count=%d\n",
			   sock_tag_entry->sk,
			   sock_tag_entry->tag, uid,
			   sock_tag_entry->pid, sk_ref_count);
	} else {
		seq_printf(m, "events: sockets_tagged=%llu "
			   "sockets_untagged=%llu "
			   "counter_set_changes=%llu "
			   "delete_cmds=%llu "
			   "iface_events=%llu "
			   "match_calls=%llu "
			   "match_calls_prepost=%llu "
			   "match_found_sk=%llu "
			   "match_found_sk_in_ct=%llu "
			   "match_found_no_sk_in_ct=%llu "
			   "match_no_sk=%llu "
			   "match_no_sk_gid=%llu\n",
			   (u64)atomic64_read(&qtu_events.sockets_tagged),
			   (u64)atomic64_read(&qtu_events.sockets_untagged),
			   (u64)atomic64_read(&qtu_events.counter_set_changes),
			   (u64)atomic64_read(&qtu_events.delete_cmds),
			   (u64)atomic64_read(&qtu_events.iface_events),
			   (u64)atomic64_read(&qtu_events.match_calls),
			   (u64)atomic64_read(&qtu_events.match_calls_prepost),
			   (u64)atomic64_read(&qtu_events.match_found_sk),
			   (u64)atomic64_read(&qtu_events.match_found_sk_in_ct),
			   (u64)atomic64_read(&qtu_events.match_found_no_sk_in_ct),
			   (u64)atomic64_read(&qtu_events.match_no_sk),
			   (u64)atomic64_read(&qtu_events.match_no_sk_gid));

		/* Count the following as part of the last item_index. No need
		 * to lock the sock_tag_list here since it is already locked when
		 * starting the seq_file operation
		 */
		prdebug_full_state_locked(0, "proc ctrl");
	}

	return 0;
}

/*
 * Delete socket tags, and stat tags associated with a given
 * accouting tag and uid.
 */
static int ctrl_cmd_delete(const char *input)
{
	char cmd;
	int uid_int;
	kuid_t uid;
	uid_t entry_uid;
	tag_t acct_tag;
	tag_t tag;
	int res, argc;
	struct iface_stat *iface_entry;
	struct rb_node *node;
	struct sock_tag *st_entry;
	struct rb_root st_to_free_tree = RB_ROOT;
	struct tag_stat *ts_entry;
	struct tag_counter_set *tcs_entry;
	struct tag_ref *tr_entry;
	struct uid_tag_data *utd_entry;

	argc = sscanf(input, "%c %llu %u", &cmd, &acct_tag, &uid_int);
	uid = make_kuid(&init_user_ns, uid_int);
	CT_DEBUG("qtaguid: ctrl_delete(%s): argc=%d cmd=%c "
		 "user_tag=0x%llx uid=%u\n", input, argc, cmd,
		 acct_tag, uid_int);
	if (argc < 2) {
		res = -EINVAL;
		goto err;
	}
	if (!valid_atag(acct_tag)) {
		pr_info("qtaguid: ctrl_delete(%s): invalid tag\n", input);
		res = -EINVAL;
		goto err;
	}
	if (argc < 3) {
		uid = current_fsuid();
		uid_int = from_kuid(&init_user_ns, uid);
	} else if (!can_impersonate_uid(uid)) {
		pr_info("qtaguid: ctrl_delete(%s): "
			"insufficient priv from pid=%u tgid=%u uid=%u\n",
			input, current->pid, current->tgid, from_kuid(&init_user_ns, current_fsuid()));
		res = -EPERM;
		goto err;
	}

	tag = combine_atag_with_uid(acct_tag, uid_int);
	CT_DEBUG("qtaguid: ctrl_delete(%s): "
		 "looking for tag=0x%llx (uid=%u)\n",
		 input, tag, uid_int);

	/* Delete socket tags */
	spin_lock_bh(&sock_tag_list_lock);
	spin_lock_bh(&uid_tag_data_tree_lock);
	node = rb_first(&sock_tag_tree);
	while (node) {
		st_entry = rb_entry(node, struct sock_tag, sock_node);
		entry_uid = get_uid_from_tag(st_entry->tag);
		node = rb_next(node);
		if (entry_uid != uid_int)
			continue;

		CT_DEBUG("qtaguid: ctrl_delete(%s): st tag=0x%llx (uid=%u)\n",
			 input, st_entry->tag, entry_uid);

		if (!acct_tag || st_entry->tag == tag) {
			rb_erase(&st_entry->sock_node, &sock_tag_tree);
			/* Can't sockfd_put() within spinlock, do it later. */
			sock_tag_tree_insert(st_entry, &st_to_free_tree);
			tr_entry = lookup_tag_ref(st_entry->tag, NULL);
			BUG_ON(tr_entry->num_sock_tags <= 0);
			tr_entry->num_sock_tags--;
			/*
			 * TODO: remove if, and start failing.
			 * This is a hack to work around the fact that in some
			 * places we have "if (IS_ERR_OR_NULL(pqd_entry))"
			 * and are trying to work around apps
			 * that didn't open the /dev/xt_qtaguid.
			 */
			if (st_entry->list.next && st_entry->list.prev)
				list_del(&st_entry->list);
		}
	}
	spin_unlock_bh(&uid_tag_data_tree_lock);
	spin_unlock_bh(&sock_tag_list_lock);

	sock_tag_tree_erase(&st_to_free_tree);

	/* Delete tag counter-sets */
	spin_lock_bh(&tag_counter_set_list_lock);
	/* Counter sets are only on the uid tag, not full tag */
	tcs_entry = tag_counter_set_tree_search(&tag_counter_set_tree, tag);
	if (tcs_entry) {
		CT_DEBUG("qtaguid: ctrl_delete(%s): "
			 "erase tcs: tag=0x%llx (uid=%u) set=%d\n",
			 input,
			 tcs_entry->tn.tag,
			 get_uid_from_tag(tcs_entry->tn.tag),
			 tcs_entry->active_set);
		rb_erase(&tcs_entry->tn.node, &tag_counter_set_tree);
		kfree(tcs_entry);
	}
	spin_unlock_bh(&tag_counter_set_list_lock);

	/*
	 * If acct_tag is 0, then all entries belonging to uid are
	 * erased.
	 */
	spin_lock_bh(&iface_stat_list_lock);
	list_for_each_entry(iface_entry, &iface_stat_list, list) {
		spin_lock_bh(&iface_entry->tag_stat_list_lock);
		node = rb_first(&iface_entry->tag_stat_tree);
		while (node) {
			ts_entry = rb_entry(node, struct tag_stat, tn.node);
			entry_uid = get_uid_from_tag(ts_entry->tn.tag);
			node = rb_next(node);

			CT_DEBUG("qtaguid: ctrl_delete(%s): "
				 "ts tag=0x%llx (uid=%u)\n",
				 input, ts_entry->tn.tag, entry_uid);

			if (entry_uid != uid_int)
				continue;
			if (!acct_tag || ts_entry->tn.tag == tag) {
				CT_DEBUG("qtaguid: ctrl_delete(%s): "
					 "erase ts: %s 0x%llx %u\n",
					 input, iface_entry->ifname,
					 get_atag_from_tag(ts_entry->tn.tag),
					 entry_uid);
				rb_erase(&ts_entry->tn.node,
					 &iface_entry->tag_stat_tree);
				kfree(ts_entry);
			}
		}
		spin_unlock_bh(&iface_entry->tag_stat_list_lock);
	}
	spin_unlock_bh(&iface_stat_list_lock);

	/* Cleanup the uid_tag_data */
	spin_lock_bh(&uid_tag_data_tree_lock);
	node = rb_first(&uid_tag_data_tree);
	while (node) {
		utd_entry = rb_entry(node, struct uid_tag_data, node);
		entry_uid = utd_entry->uid;
		node = rb_next(node);

		CT_DEBUG("qtaguid: ctrl_delete(%s): "
			 "utd uid=%u\n",
			 input, entry_uid);

		if (entry_uid != uid_int)
			continue;
		/*
		 * Go over the tag_refs, and those that don't have
		 * sock_tags using them are freed.
		 */
		put_tag_ref_tree(tag, utd_entry);
		put_utd_entry(utd_entry);
	}
	spin_unlock_bh(&uid_tag_data_tree_lock);

	atomic64_inc(&qtu_events.delete_cmds);
	res = 0;

err:
	return res;
}

static int ctrl_cmd_counter_set(const char *input)
{
	char cmd;
	uid_t uid = 0;
	tag_t tag;
	int res, argc;
	struct tag_counter_set *tcs;
	int counter_set;

	argc = sscanf(input, "%c %d %u", &cmd, &counter_set, &uid);
	CT_DEBUG("qtaguid: ctrl_counterset(%s): argc=%d cmd=%c "
		 "set=%d uid=%u\n", input, argc, cmd,
		 counter_set, uid);
	if (argc != 3) {
		res = -EINVAL;
		goto err;
	}
	if (counter_set < 0 || counter_set >= IFS_MAX_COUNTER_SETS) {
		pr_info("qtaguid: ctrl_counterset(%s): invalid counter_set range\n",
			input);
		res = -EINVAL;
		goto err;
	}
	if (!can_manipulate_uids()) {
		pr_info("qtaguid: ctrl_counterset(%s): "
			"insufficient priv from pid=%u tgid=%u uid=%u\n",
			input, current->pid, current->tgid, from_kuid(&init_user_ns, current_fsuid()));
		res = -EPERM;
		goto err;
	}

	tag = make_tag_from_uid(uid);
	spin_lock_bh(&tag_counter_set_list_lock);
	tcs = tag_counter_set_tree_search(&tag_counter_set_tree, tag);
	if (!tcs) {
		tcs = kzalloc(sizeof(*tcs), GFP_ATOMIC);
		if (!tcs) {
			spin_unlock_bh(&tag_counter_set_list_lock);
			pr_err("qtaguid: ctrl_counterset(%s): "
			       "failed to alloc counter set\n",
			       input);
			res = -ENOMEM;
			goto err;
		}
		tcs->tn.tag = tag;
		tag_counter_set_tree_insert(tcs, &tag_counter_set_tree);
		CT_DEBUG("qtaguid: ctrl_counterset(%s): added tcs tag=0x%llx "
			 "(uid=%u) set=%d\n",
			 input, tag, get_uid_from_tag(tag), counter_set);
	}
	tcs->active_set = counter_set;
	spin_unlock_bh(&tag_counter_set_list_lock);
	atomic64_inc(&qtu_events.counter_set_changes);
	res = 0;

err:
	return res;
}

static int ctrl_cmd_tag(const char *input)
{
	char cmd;
	int sock_fd = 0;
	kuid_t uid;
	unsigned int uid_int = 0;
	tag_t acct_tag = make_atag_from_value(0);
	tag_t full_tag;
	struct socket *el_socket;
	int res, argc;
	struct sock_tag *sock_tag_entry;
	struct tag_ref *tag_ref_entry;
	struct uid_tag_data *uid_tag_data_entry;
	struct proc_qtu_data *pqd_entry;

	/* Unassigned args will get defaulted later. */
	argc = sscanf(input, "%c %d %llu %u", &cmd, &sock_fd, &acct_tag, &uid_int);
	uid = make_kuid(&init_user_ns, uid_int);
	CT_DEBUG("qtaguid: ctrl_tag(%s): argc=%d cmd=%c sock_fd=%d "
		 "acct_tag=0x%llx uid=%u\n", input, argc, cmd, sock_fd,
		 acct_tag, uid_int);
	if (argc < 2) {
		res = -EINVAL;
		goto err;
	}
	el_socket = sockfd_lookup(sock_fd, &res);  /* This locks the file */
	if (!el_socket) {
		pr_info("qtaguid: ctrl_tag(%s): failed to lookup"
			" sock_fd=%d err=%d pid=%u tgid=%u uid=%u\n",
			input, sock_fd, res, current->pid, current->tgid,
			from_kuid(&init_user_ns, current_fsuid()));
		goto err;
	}
	CT_DEBUG("qtaguid: ctrl_tag(%s): socket->...->sk_refcnt=%d ->sk=%p\n",
		 input, atomic_read(&el_socket->sk->sk_refcnt),
		 el_socket->sk);
	if (argc < 3) {
		acct_tag = make_atag_from_value(0);
	} else if (!valid_atag(acct_tag)) {
		pr_info("qtaguid: ctrl_tag(%s): invalid tag\n", input);
		res = -EINVAL;
		goto err_put;
	}
	CT_DEBUG("qtaguid: ctrl_tag(%s): "
		 "pid=%u tgid=%u uid=%u euid=%u fsuid=%u "
		 "ctrl.gid=%u in_group()=%d in_egroup()=%d\n",
		 input, current->pid, current->tgid,
		 from_kuid(&init_user_ns, current_uid()),
		 from_kuid(&init_user_ns, current_euid()),
		 from_kuid(&init_user_ns, current_fsuid()),
		 from_kgid(&init_user_ns, xt_qtaguid_ctrl_file->gid),
		 in_group_p(xt_qtaguid_ctrl_file->gid),
		 in_egroup_p(xt_qtaguid_ctrl_file->gid));
	if (argc < 4) {
		uid = current_fsuid();
		uid_int = from_kuid(&init_user_ns, uid);
	} else if (!can_impersonate_uid(uid)) {
		pr_info("qtaguid: ctrl_tag(%s): "
			"insufficient priv from pid=%u tgid=%u uid=%u\n",
			input, current->pid, current->tgid, from_kuid(&init_user_ns, current_fsuid()));
		res = -EPERM;
		goto err_put;
	}
	full_tag = combine_atag_with_uid(acct_tag, uid_int);

	spin_lock_bh(&sock_tag_list_lock);
	spin_lock_bh(&uid_tag_data_tree_lock);
	sock_tag_entry = get_sock_stat_nl(el_socket->sk);
	tag_ref_entry = get_tag_ref(full_tag, &uid_tag_data_entry);
	if (IS_ERR(tag_ref_entry)) {
		res = PTR_ERR(tag_ref_entry);
		spin_unlock_bh(&uid_tag_data_tree_lock);
		spin_unlock_bh(&sock_tag_list_lock);
		goto err_put;
	}
	tag_ref_entry->num_sock_tags++;
	if (sock_tag_entry) {
		struct tag_ref *prev_tag_ref_entry;

		CT_DEBUG("qtaguid: ctrl_tag(%s): retag for sk=%p "
			 "st@%p ...->sk_refcnt=%d\n",
			 input, el_socket->sk, sock_tag_entry,
			 atomic_read(&el_socket->sk->sk_refcnt));
		prev_tag_ref_entry = lookup_tag_ref(sock_tag_entry->tag,
						    &uid_tag_data_entry);
		BUG_ON(IS_ERR_OR_NULL(prev_tag_ref_entry));
		BUG_ON(prev_tag_ref_entry->num_sock_tags <= 0);
		prev_tag_ref_entry->num_sock_tags--;
		sock_tag_entry->tag = full_tag;
	} else {
		CT_DEBUG("qtaguid: ctrl_tag(%s): newtag for sk=%p\n",
			 input, el_socket->sk);
		sock_tag_entry = kzalloc(sizeof(*sock_tag_entry),
					 GFP_ATOMIC);
		if (!sock_tag_entry) {
			pr_err("qtaguid: ctrl_tag(%s): "
			       "socket tag alloc failed\n",
			       input);
			BUG_ON(tag_ref_entry->num_sock_tags <= 0);
			tag_ref_entry->num_sock_tags--;
			free_tag_ref_from_utd_entry(tag_ref_entry,
						    uid_tag_data_entry);
			spin_unlock_bh(&uid_tag_data_tree_lock);
			spin_unlock_bh(&sock_tag_list_lock);
			res = -ENOMEM;
			goto err_put;
		}
		/*
		 * Hold the sk refcount here to make sure the sk pointer cannot
		 * be freed and reused
		 */
		sock_hold(el_socket->sk);
		sock_tag_entry->sk = el_socket->sk;
		sock_tag_entry->pid = current->tgid;
		sock_tag_entry->tag = combine_atag_with_uid(acct_tag, uid_int);
		pqd_entry = proc_qtu_data_tree_search(
			&proc_qtu_data_tree, current->tgid);
		/*
		 * TODO: remove if, and start failing.
		 * At first, we want to catch user-space code that is not
		 * opening the /dev/xt_qtaguid.
		 */
		if (IS_ERR_OR_NULL(pqd_entry))
			pr_warn_once(
				"qtaguid: %s(): "
				"User space forgot to open /dev/xt_qtaguid? "
				"pid=%u tgid=%u uid=%u\n", __func__,
				current->pid, current->tgid,
				from_kuid(&init_user_ns, current_fsuid()));
		else
			list_add(&sock_tag_entry->list,
				 &pqd_entry->sock_tag_list);

		sock_tag_tree_insert(sock_tag_entry, &sock_tag_tree);
		atomic64_inc(&qtu_events.sockets_tagged);
	}
	spin_unlock_bh(&uid_tag_data_tree_lock);
	spin_unlock_bh(&sock_tag_list_lock);
	/* We keep the ref to the sk until it is untagged */
	CT_DEBUG("qtaguid: ctrl_tag(%s): done st@%p ...->sk_refcnt=%d\n",
		 input, sock_tag_entry,
		 atomic_read(&el_socket->sk->sk_refcnt));
	sockfd_put(el_socket);
	return 0;

err_put:
	CT_DEBUG("qtaguid: ctrl_tag(%s): done. ...->sk_refcnt=%d\n",
		 input, atomic_read(&el_socket->sk->sk_refcnt) - 1);
	/* Release the sock_fd that was grabbed by sockfd_lookup(). */
	sockfd_put(el_socket);
	return res;

err:
	CT_DEBUG("qtaguid: ctrl_tag(%s): done.\n", input);
	return res;
}

static int ctrl_cmd_untag(const char *input)
{
	char cmd;
	int sock_fd = 0;
	struct socket *el_socket;
	int res, argc;

	argc = sscanf(input, "%c %d", &cmd, &sock_fd);
	CT_DEBUG("qtaguid: ctrl_untag(%s): argc=%d cmd=%c sock_fd=%d\n",
		 input, argc, cmd, sock_fd);
	if (argc < 2) {
		res = -EINVAL;
		return res;
	}
	el_socket = sockfd_lookup(sock_fd, &res);  /* This locks the file */
	if (!el_socket) {
		pr_info("qtaguid: ctrl_untag(%s): failed to lookup"
			" sock_fd=%d err=%d pid=%u tgid=%u uid=%u\n",
			input, sock_fd, res, current->pid, current->tgid,
			from_kuid(&init_user_ns, current_fsuid()));
		return res;
	}
	CT_DEBUG("qtaguid: ctrl_untag(%s): socket->...->f_count=%ld ->sk=%p\n",
		 input, atomic_long_read(&el_socket->file->f_count),
		 el_socket->sk);
	res = qtaguid_untag(el_socket, false);
	sockfd_put(el_socket);
	return res;
}

int qtaguid_untag(struct socket *el_socket, bool kernel)
{
	int res;
	pid_t pid;
	struct sock_tag *sock_tag_entry;
	struct tag_ref *tag_ref_entry;
	struct uid_tag_data *utd_entry;
	struct proc_qtu_data *pqd_entry;

	spin_lock_bh(&sock_tag_list_lock);
	sock_tag_entry = get_sock_stat_nl(el_socket->sk);
	if (!sock_tag_entry) {
		spin_unlock_bh(&sock_tag_list_lock);
		res = -EINVAL;
		return res;
	}
	/*
	 * The socket already belongs to the current process
	 * so it can do whatever it wants to it.
	 */
	rb_erase(&sock_tag_entry->sock_node, &sock_tag_tree);

	tag_ref_entry = lookup_tag_ref(sock_tag_entry->tag, &utd_entry);
	BUG_ON(!tag_ref_entry);
	BUG_ON(tag_ref_entry->num_sock_tags <= 0);
	spin_lock_bh(&uid_tag_data_tree_lock);
	if (kernel)
		pid = sock_tag_entry->pid;
	else
		pid = current->tgid;
	pqd_entry = proc_qtu_data_tree_search(
		&proc_qtu_data_tree, pid);
	/*
	 * TODO: remove if, and start failing.
	 * At first, we want to catch user-space code that is not
	 * opening the /dev/xt_qtaguid.
	 */
	if (IS_ERR_OR_NULL(pqd_entry))
		pr_warn_once("qtaguid: %s(): "
			     "User space forgot to open /dev/xt_qtaguid? "
			     "pid=%u tgid=%u sk_pid=%u, uid=%u\n", __func__,
			     current->pid, current->tgid, sock_tag_entry->pid,
			     from_kuid(&init_user_ns, current_fsuid()));
	/*
	 * This check is needed because tagging from a process that
	 * didn’t open /dev/xt_qtaguid still adds the sock_tag_entry
	 * to sock_tag_tree.
	 */
	if (sock_tag_entry->list.next)
		list_del(&sock_tag_entry->list);

	spin_unlock_bh(&uid_tag_data_tree_lock);
	/*
	 * We don't free tag_ref from the utd_entry here,
	 * only during a cmd_delete().
	 */
	tag_ref_entry->num_sock_tags--;
	spin_unlock_bh(&sock_tag_list_lock);
	/*
	 * Release the sock_fd that was grabbed at tag time.
	 */
	sock_put(sock_tag_entry->sk);
	CT_DEBUG("qtaguid: done. st@%p ...->sk_refcnt=%d\n",
		 sock_tag_entry,
		 atomic_read(&el_socket->sk->sk_refcnt));

	kfree(sock_tag_entry);
	atomic64_inc(&qtu_events.sockets_untagged);

	return 0;
}

static ssize_t qtaguid_ctrl_parse(const char *input, size_t count)
{
	char cmd;
	ssize_t res;

	CT_DEBUG("qtaguid: ctrl(%s): pid=%u tgid=%u uid=%u\n",
		 input, current->pid, current->tgid, from_kuid(&init_user_ns, current_fsuid()));

	cmd = input[0];
	/* Collect params for commands */
	switch (cmd) {
	case 'd':
		res = ctrl_cmd_delete(input);
		break;

	case 's':
		res = ctrl_cmd_counter_set(input);
		break;

	case 't':
		res = ctrl_cmd_tag(input);
		break;

	case 'u':
		res = ctrl_cmd_untag(input);
		break;

	default:
		res = -EINVAL;
		goto err;
	}
	if (!res)
		res = count;
err:
	CT_DEBUG("qtaguid: ctrl(%s): res=%zd\n", input, res);
	return res;
}

#define MAX_QTAGUID_CTRL_INPUT_LEN 255
static ssize_t qtaguid_ctrl_proc_write(struct file *file, const char __user *buffer,
				   size_t count, loff_t *offp)
{
	char input_buf[MAX_QTAGUID_CTRL_INPUT_LEN];

	if (unlikely(module_passive))
		return count;

	if (count >= MAX_QTAGUID_CTRL_INPUT_LEN)
		return -EINVAL;

	if (copy_from_user(input_buf, buffer, count))
		return -EFAULT;

	input_buf[count] = '\0';
	return qtaguid_ctrl_parse(input_buf, count);
}

struct proc_print_info {
	struct iface_stat *iface_entry;
	int item_index;
	tag_t tag; /* tag found by reading to tag_pos */
	off_t tag_pos;
	int tag_item_index;
};

static void pp_stats_header(struct seq_file *m)
{
	seq_puts(m,
		 "idx iface acct_tag_hex uid_tag_int cnt_set "
		 "rx_bytes rx_packets "
		 "tx_bytes tx_packets "
		 "rx_tcp_bytes rx_tcp_packets "
		 "rx_udp_bytes rx_udp_packets "
		 "rx_other_bytes rx_other_packets "
		 "tx_tcp_bytes tx_tcp_packets "
		 "tx_udp_bytes tx_udp_packets "
		 "tx_other_bytes tx_other_packets\n");
}

static int pp_stats_line(struct seq_file *m, struct tag_stat *ts_entry,
			 int cnt_set)
{
	struct data_counters *cnts;
	tag_t tag = ts_entry->tn.tag;
	uid_t stat_uid = get_uid_from_tag(tag);
	struct proc_print_info *ppi = m->private;
	/* Detailed tags are not available to everybody */
	if (!can_read_other_uid_stats(make_kuid(&init_user_ns,stat_uid))) {
		CT_DEBUG("qtaguid: stats line: "
			 "%s 0x%llx %u: insufficient priv "
			 "from pid=%u tgid=%u uid=%u stats.gid=%u\n",
			 ppi->iface_entry->ifname,
			 get_atag_from_tag(tag), stat_uid,
			 current->pid, current->tgid, from_kuid(&init_user_ns, current_fsuid()),
			 from_kgid(&init_user_ns,xt_qtaguid_stats_file->gid));
		return 0;
	}
	ppi->item_index++;
	cnts = &ts_entry->counters;
	seq_printf(m, "%d %s 0x%llx %u %u "
		"%llu %llu "
		"%llu %llu "
		"%llu %llu "
		"%llu %llu "
		"%llu %llu "
		"%llu %llu "
		"%llu %llu "
		"%llu %llu\n",
		ppi->item_index,
		ppi->iface_entry->ifname,
		get_atag_from_tag(tag),
		stat_uid,
		cnt_set,
		dc_sum_bytes(cnts, cnt_set, IFS_RX),
		dc_sum_packets(cnts, cnt_set, IFS_RX),
		dc_sum_bytes(cnts, cnt_set, IFS_TX),
		dc_sum_packets(cnts, cnt_set, IFS_TX),
		cnts->bpc[cnt_set][IFS_RX][IFS_TCP].bytes,
		cnts->bpc[cnt_set][IFS_RX][IFS_TCP].packets,
		cnts->bpc[cnt_set][IFS_RX][IFS_UDP].bytes,
		cnts->bpc[cnt_set][IFS_RX][IFS_UDP].packets,
		cnts->bpc[cnt_set][IFS_RX][IFS_PROTO_OTHER].bytes,
		cnts->bpc[cnt_set][IFS_RX][IFS_PROTO_OTHER].packets,
		cnts->bpc[cnt_set][IFS_TX][IFS_TCP].bytes,
		cnts->bpc[cnt_set][IFS_TX][IFS_TCP].packets,
		cnts->bpc[cnt_set][IFS_TX][IFS_UDP].bytes,
		cnts->bpc[cnt_set][IFS_TX][IFS_UDP].packets,
		cnts->bpc[cnt_set][IFS_TX][IFS_PROTO_OTHER].bytes,
		cnts->bpc[cnt_set][IFS_TX][IFS_PROTO_OTHER].packets);
	return seq_has_overflowed(m) ? -ENOSPC : 1;
}

static bool pp_sets(struct seq_file *m, struct tag_stat *ts_entry)
{
	int ret;
	int counter_set;
	for (counter_set = 0; counter_set < IFS_MAX_COUNTER_SETS;
	     counter_set++) {
		ret = pp_stats_line(m, ts_entry, counter_set);
		if (ret < 0)
			return false;
	}
	return true;
}

static int qtaguid_stats_proc_iface_stat_ptr_valid(struct iface_stat *ptr)
{
	struct iface_stat *iface_entry;

	if (!ptr)
		return false;

	list_for_each_entry(iface_entry, &iface_stat_list, list)
		if (iface_entry == ptr)
			return true;
	return false;
}

static void qtaguid_stats_proc_next_iface_entry(struct proc_print_info *ppi)
{
	spin_unlock_bh(&ppi->iface_entry->tag_stat_list_lock);
	list_for_each_entry_continue(ppi->iface_entry, &iface_stat_list, list) {
		spin_lock_bh(&ppi->iface_entry->tag_stat_list_lock);
		return;
	}
	ppi->iface_entry = NULL;
}

static void *qtaguid_stats_proc_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct proc_print_info *ppi = m->private;
	struct tag_stat *ts_entry;
	struct rb_node *node;

	if (!v) {
		pr_err("qtaguid: %s(): unexpected v: NULL\n", __func__);
		return NULL;
	}

	(*pos)++;

	if (!ppi->iface_entry || unlikely(module_passive))
		return NULL;

	if (v == SEQ_START_TOKEN)
		node = rb_first(&ppi->iface_entry->tag_stat_tree);
	else
		node = rb_next(&((struct tag_stat *)v)->tn.node);

	while (!node) {
		qtaguid_stats_proc_next_iface_entry(ppi);
		if (!ppi->iface_entry)
			return NULL;
		node = rb_first(&ppi->iface_entry->tag_stat_tree);
	}

	ts_entry = rb_entry(node, struct tag_stat, tn.node);
	ppi->tag = ts_entry->tn.tag;
	ppi->tag_pos = *pos;
	ppi->tag_item_index = ppi->item_index;
	return ts_entry;
}

static void *qtaguid_stats_proc_start(struct seq_file *m, loff_t *pos)
{
	struct proc_print_info *ppi = m->private;
	struct tag_stat *ts_entry = NULL;

	spin_lock_bh(&iface_stat_list_lock);

	if (*pos == 0) {
		ppi->item_index = 1;
		ppi->tag_pos = 0;
		if (list_empty(&iface_stat_list)) {
			ppi->iface_entry = NULL;
		} else {
			ppi->iface_entry = list_first_entry(&iface_stat_list,
							    struct iface_stat,
							    list);
			spin_lock_bh(&ppi->iface_entry->tag_stat_list_lock);
		}
		return SEQ_START_TOKEN;
	}
	if (!qtaguid_stats_proc_iface_stat_ptr_valid(ppi->iface_entry)) {
		if (ppi->iface_entry) {
			pr_err("qtaguid: %s(): iface_entry %p not found\n",
			       __func__, ppi->iface_entry);
			ppi->iface_entry = NULL;
		}
		return NULL;
	}

	spin_lock_bh(&ppi->iface_entry->tag_stat_list_lock);

	if (!ppi->tag_pos) {
		/* seq_read skipped first next call */
		ts_entry = SEQ_START_TOKEN;
	} else {
		ts_entry = tag_stat_tree_search(
				&ppi->iface_entry->tag_stat_tree, ppi->tag);
		if (!ts_entry) {
			pr_info("qtaguid: %s(): tag_stat.tag 0x%llx not found. Abort.\n",
				__func__, ppi->tag);
			return NULL;
		}
	}

	if (*pos == ppi->tag_pos) { /* normal resume */
		ppi->item_index = ppi->tag_item_index;
	} else {
		/* seq_read skipped a next call */
		*pos = ppi->tag_pos;
		ts_entry = qtaguid_stats_proc_next(m, ts_entry, pos);
	}

	return ts_entry;
}

static void qtaguid_stats_proc_stop(struct seq_file *m, void *v)
{
	struct proc_print_info *ppi = m->private;
	if (ppi->iface_entry)
		spin_unlock_bh(&ppi->iface_entry->tag_stat_list_lock);
	spin_unlock_bh(&iface_stat_list_lock);
}

/*
 * Procfs reader to get all tag stats using style "1)" as described in
 * fs/proc/generic.c
 * Groups all protocols tx/rx bytes.
 */
static int qtaguid_stats_proc_show(struct seq_file *m, void *v)
{
	struct tag_stat *ts_entry = v;

	if (v == SEQ_START_TOKEN)
		pp_stats_header(m);
	else
		pp_sets(m, ts_entry);

	return 0;
}

/*------------------------------------------*/
static int qtudev_open(struct inode *inode, struct file *file)
{
	struct uid_tag_data *utd_entry;
	struct proc_qtu_data  *pqd_entry;
	struct proc_qtu_data  *new_pqd_entry;
	int res;
	bool utd_entry_found;

	if (unlikely(qtu_proc_handling_passive))
		return 0;

	DR_DEBUG("qtaguid: qtudev_open(): pid=%u tgid=%u uid=%u\n",
		 current->pid, current->tgid, from_kuid(&init_user_ns, current_fsuid()));

	spin_lock_bh(&uid_tag_data_tree_lock);

	/* Look for existing uid data, or alloc one. */
	utd_entry = get_uid_data(from_kuid(&init_user_ns, current_fsuid()), &utd_entry_found);
	if (IS_ERR_OR_NULL(utd_entry)) {
		res = PTR_ERR(utd_entry);
		goto err_unlock;
	}

	/* Look for existing PID based proc_data */
	pqd_entry = proc_qtu_data_tree_search(&proc_qtu_data_tree,
					      current->tgid);
	if (pqd_entry) {
		pr_err("qtaguid: qtudev_open(): %u/%u %u "
		       "%s already opened\n",
		       current->pid, current->tgid, from_kuid(&init_user_ns, current_fsuid()),
		       QTU_DEV_NAME);
		res = -EBUSY;
		goto err_unlock_free_utd;
	}

	new_pqd_entry = kzalloc(sizeof(*new_pqd_entry), GFP_ATOMIC);
	if (!new_pqd_entry) {
		pr_err("qtaguid: qtudev_open(): %u/%u %u: "
		       "proc data alloc failed\n",
		       current->pid, current->tgid, from_kuid(&init_user_ns, current_fsuid()));
		res = -ENOMEM;
		goto err_unlock_free_utd;
	}
	new_pqd_entry->pid = current->tgid;
	INIT_LIST_HEAD(&new_pqd_entry->sock_tag_list);
	new_pqd_entry->parent_tag_data = utd_entry;
	utd_entry->num_pqd++;

	proc_qtu_data_tree_insert(new_pqd_entry,
				  &proc_qtu_data_tree);

	spin_unlock_bh(&uid_tag_data_tree_lock);
	DR_DEBUG("qtaguid: tracking data for uid=%u in pqd=%p\n",
		 from_kuid(&init_user_ns, current_fsuid()), new_pqd_entry);
	file->private_data = new_pqd_entry;
	return 0;

err_unlock_free_utd:
	if (!utd_entry_found) {
		rb_erase(&utd_entry->node, &uid_tag_data_tree);
		kfree(utd_entry);
	}
err_unlock:
	spin_unlock_bh(&uid_tag_data_tree_lock);
	return res;
}

static int qtudev_release(struct inode *inode, struct file *file)
{
	struct proc_qtu_data  *pqd_entry = file->private_data;
	struct uid_tag_data  *utd_entry = pqd_entry->parent_tag_data;
	struct sock_tag *st_entry;
	struct rb_root st_to_free_tree = RB_ROOT;
	struct list_head *entry, *next;
	struct tag_ref *tr;

	if (unlikely(qtu_proc_handling_passive))
		return 0;

	/*
	 * Do not trust the current->pid, it might just be a kworker cleaning
	 * up after a dead proc.
	 */
	DR_DEBUG("qtaguid: qtudev_release(): "
		 "pid=%u tgid=%u uid=%u "
		 "pqd_entry=%p->pid=%u utd_entry=%p->active_tags=%d\n",
		 current->pid, current->tgid, pqd_entry->parent_tag_data->uid,
		 pqd_entry, pqd_entry->pid, utd_entry,
		 utd_entry->num_active_tags);

	spin_lock_bh(&sock_tag_list_lock);
	spin_lock_bh(&uid_tag_data_tree_lock);

	list_for_each_safe(entry, next, &pqd_entry->sock_tag_list) {
		st_entry = list_entry(entry, struct sock_tag, list);
		DR_DEBUG("qtaguid: %s(): "
			 "erase sock_tag=%p->sk=%p pid=%u tgid=%u uid=%u\n",
			 __func__,
			 st_entry, st_entry->sk,
			 current->pid, current->tgid,
			 pqd_entry->parent_tag_data->uid);

		utd_entry = uid_tag_data_tree_search(
			&uid_tag_data_tree,
			get_uid_from_tag(st_entry->tag));
		BUG_ON(IS_ERR_OR_NULL(utd_entry));
		DR_DEBUG("qtaguid: %s(): "
			 "looking for tag=0x%llx in utd_entry=%p\n", __func__,
			 st_entry->tag, utd_entry);
		tr = tag_ref_tree_search(&utd_entry->tag_ref_tree,
					 st_entry->tag);
		BUG_ON(!tr);
		BUG_ON(tr->num_sock_tags <= 0);
		tr->num_sock_tags--;
		free_tag_ref_from_utd_entry(tr, utd_entry);

		rb_erase(&st_entry->sock_node, &sock_tag_tree);
		list_del(&st_entry->list);
		/* Can't sockfd_put() within spinlock, do it later. */
		sock_tag_tree_insert(st_entry, &st_to_free_tree);

		/*
		 * Try to free the utd_entry if no other proc_qtu_data is
		 * using it (num_pqd is 0) and it doesn't have active tags
		 * (num_active_tags is 0).
		 */
		put_utd_entry(utd_entry);
	}

	rb_erase(&pqd_entry->node, &proc_qtu_data_tree);
	BUG_ON(pqd_entry->parent_tag_data->num_pqd < 1);
	pqd_entry->parent_tag_data->num_pqd--;
	put_utd_entry(pqd_entry->parent_tag_data);
	kfree(pqd_entry);
	file->private_data = NULL;

	spin_unlock_bh(&uid_tag_data_tree_lock);
	spin_unlock_bh(&sock_tag_list_lock);


	sock_tag_tree_erase(&st_to_free_tree);

	spin_lock_bh(&sock_tag_list_lock);
	prdebug_full_state_locked(0, "%s(): pid=%u tgid=%u", __func__,
			   current->pid, current->tgid);
	spin_unlock_bh(&sock_tag_list_lock);
	return 0;
}

/*------------------------------------------*/
static const struct file_operations qtudev_fops = {
	.owner = THIS_MODULE,
	.open = qtudev_open,
	.release = qtudev_release,
};

static struct miscdevice qtu_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = QTU_DEV_NAME,
	.fops = &qtudev_fops,
	/* How sad it doesn't allow for defaults: .mode = S_IRUGO | S_IWUSR */
};

static const struct seq_operations proc_qtaguid_ctrl_seqops = {
	.start = qtaguid_ctrl_proc_start,
	.next = qtaguid_ctrl_proc_next,
	.stop = qtaguid_ctrl_proc_stop,
	.show = qtaguid_ctrl_proc_show,
};

static int proc_qtaguid_ctrl_open(struct inode *inode, struct file *file)
{
	return seq_open_private(file, &proc_qtaguid_ctrl_seqops,
				sizeof(struct proc_ctrl_print_info));
}

static const struct file_operations proc_qtaguid_ctrl_fops = {
	.open		= proc_qtaguid_ctrl_open,
	.read		= seq_read,
	.write		= qtaguid_ctrl_proc_write,
	.llseek		= seq_lseek,
	.release	= seq_release_private,
};

static const struct seq_operations proc_qtaguid_stats_seqops = {
	.start = qtaguid_stats_proc_start,
	.next = qtaguid_stats_proc_next,
	.stop = qtaguid_stats_proc_stop,
	.show = qtaguid_stats_proc_show,
};

static int proc_qtaguid_stats_open(struct inode *inode, struct file *file)
{
	return seq_open_private(file, &proc_qtaguid_stats_seqops,
				sizeof(struct proc_print_info));
}

static const struct file_operations proc_qtaguid_stats_fops = {
	.open		= proc_qtaguid_stats_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release_private,
};

/*------------------------------------------*/
static int __init qtaguid_proc_register(struct proc_dir_entry **res_procdir)
{
	int ret;
	*res_procdir = proc_mkdir(module_procdirname, init_net.proc_net);
	if (!*res_procdir) {
		pr_err("qtaguid: failed to create proc/.../xt_qtaguid\n");
		ret = -ENOMEM;
		goto no_dir;
	}

	xt_qtaguid_ctrl_file = proc_create_data("ctrl", proc_ctrl_perms,
						*res_procdir,
						&proc_qtaguid_ctrl_fops,
						NULL);
	if (!xt_qtaguid_ctrl_file) {
		pr_err("qtaguid: failed to create xt_qtaguid/ctrl "
			" file\n");
		ret = -ENOMEM;
		goto no_ctrl_entry;
	}

	xt_qtaguid_stats_file = proc_create_data("stats", proc_stats_perms,
						 *res_procdir,
						 &proc_qtaguid_stats_fops,
						 NULL);
	if (!xt_qtaguid_stats_file) {
		pr_err("qtaguid: failed to create xt_qtaguid/stats "
			"file\n");
		ret = -ENOMEM;
		goto no_stats_entry;
	}
	/*
	 * TODO: add support counter hacking
	 * xt_qtaguid_stats_file->write_proc = qtaguid_stats_proc_write;
	 */
	return 0;

no_stats_entry:
	remove_proc_entry("ctrl", *res_procdir);
no_ctrl_entry:
	remove_proc_entry("xt_qtaguid", NULL);
no_dir:
	return ret;
}

static struct xt_match qtaguid_mt_reg __read_mostly = {
	/*
	 * This module masquerades as the "owner" module so that iptables
	 * tools can deal with it.
	 */
	.name       = "owner",
	.revision   = 1,
	.family     = NFPROTO_UNSPEC,
	.match      = qtaguid_mt,
	.matchsize  = sizeof(struct xt_qtaguid_match_info),
	.me         = THIS_MODULE,
};

static int __init qtaguid_mt_init(void)
{
	/* Android 12 uses the upstream "owner" module instead */
	if (get_android_version() > 11)
		return 0;

	if (qtaguid_proc_register(&xt_qtaguid_procdir)
	    || iface_stat_init(xt_qtaguid_procdir)
	    || xt_register_match(&qtaguid_mt_reg)
	    || misc_register(&qtu_device))
		return -1;
	return 0;
}

/*
 * TODO: allow unloading of the module.
 * For now stats are permanent.
 * Kconfig forces'y/n' and never an 'm'.
 */

module_init(qtaguid_mt_init);
MODULE_AUTHOR("jpa <jpa@google.com>");
MODULE_DESCRIPTION("Xtables: socket owner+tag matching and associated stats");
MODULE_LICENSE("GPL");
MODULE_ALIAS("ipt_owner");
MODULE_ALIAS("ip6t_owner");
MODULE_ALIAS("ipt_qtaguid");
MODULE_ALIAS("ip6t_qtaguid");
