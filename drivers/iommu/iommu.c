/*
 * Copyright (C) 2007-2008 Advanced Micro Devices, Inc.
 * Author: Joerg Roedel <jroedel@suse.de>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#define pr_fmt(fmt)    "iommu: " fmt

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/bug.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/iommu.h>
#include <linux/idr.h>
#include <linux/notifier.h>
#include <linux/err.h>
#include <linux/pci.h>
#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <trace/events/iommu.h>

#include "iommu-debug.h"

static struct kset *iommu_group_kset;
static struct ida iommu_group_ida;
static struct mutex iommu_group_mutex;

struct iommu_callback_data {
	const struct iommu_ops *ops;
};

struct iommu_group {
	struct kobject kobj;
	struct kobject *devices_kobj;
	struct list_head devices;
	struct mutex mutex;
	struct blocking_notifier_head notifier;
	void *iommu_data;
	void (*iommu_data_release)(void *iommu_data);
	char *name;
	int id;
	struct iommu_domain *default_domain;
	struct iommu_domain *domain;
};

struct iommu_device {
	struct list_head list;
	struct device *dev;
	char *name;
};

struct iommu_group_attribute {
	struct attribute attr;
	ssize_t (*show)(struct iommu_group *group, char *buf);
	ssize_t (*store)(struct iommu_group *group,
			 const char *buf, size_t count);
};

#define IOMMU_GROUP_ATTR(_name, _mode, _show, _store)		\
struct iommu_group_attribute iommu_group_attr_##_name =		\
	__ATTR(_name, _mode, _show, _store)

#define to_iommu_group_attr(_attr)	\
	container_of(_attr, struct iommu_group_attribute, attr)
#define to_iommu_group(_kobj)		\
	container_of(_kobj, struct iommu_group, kobj)

static struct iommu_domain *__iommu_domain_alloc(struct bus_type *bus,
						 unsigned type);
static int __iommu_attach_device(struct iommu_domain *domain,
				 struct device *dev);
static int __iommu_attach_group(struct iommu_domain *domain,
				struct iommu_group *group);
static void __iommu_detach_group(struct iommu_domain *domain,
				 struct iommu_group *group);

static ssize_t iommu_group_attr_show(struct kobject *kobj,
				     struct attribute *__attr, char *buf)
{
	struct iommu_group_attribute *attr = to_iommu_group_attr(__attr);
	struct iommu_group *group = to_iommu_group(kobj);
	ssize_t ret = -EIO;

	if (attr->show)
		ret = attr->show(group, buf);
	return ret;
}

static ssize_t iommu_group_attr_store(struct kobject *kobj,
				      struct attribute *__attr,
				      const char *buf, size_t count)
{
	struct iommu_group_attribute *attr = to_iommu_group_attr(__attr);
	struct iommu_group *group = to_iommu_group(kobj);
	ssize_t ret = -EIO;

	if (attr->store)
		ret = attr->store(group, buf, count);
	return ret;
}

static const struct sysfs_ops iommu_group_sysfs_ops = {
	.show = iommu_group_attr_show,
	.store = iommu_group_attr_store,
};

static int iommu_group_create_file(struct iommu_group *group,
				   struct iommu_group_attribute *attr)
{
	return sysfs_create_file(&group->kobj, &attr->attr);
}

static void iommu_group_remove_file(struct iommu_group *group,
				    struct iommu_group_attribute *attr)
{
	sysfs_remove_file(&group->kobj, &attr->attr);
}

static ssize_t iommu_group_show_name(struct iommu_group *group, char *buf)
{
	return sprintf(buf, "%s\n", group->name);
}

static IOMMU_GROUP_ATTR(name, S_IRUGO, iommu_group_show_name, NULL);

static void iommu_group_release(struct kobject *kobj)
{
	struct iommu_group *group = to_iommu_group(kobj);

	pr_debug("Releasing group %d\n", group->id);

	if (group->iommu_data_release)
		group->iommu_data_release(group->iommu_data);

	mutex_lock(&iommu_group_mutex);
	ida_remove(&iommu_group_ida, group->id);
	mutex_unlock(&iommu_group_mutex);

	if (group->default_domain)
		iommu_domain_free(group->default_domain);

	kfree(group->name);
	kfree(group);
}

static struct kobj_type iommu_group_ktype = {
	.sysfs_ops = &iommu_group_sysfs_ops,
	.release = iommu_group_release,
};

/**
 * iommu_group_alloc - Allocate a new group
 * @name: Optional name to associate with group, visible in sysfs
 *
 * This function is called by an iommu driver to allocate a new iommu
 * group.  The iommu group represents the minimum granularity of the iommu.
 * Upon successful return, the caller holds a reference to the supplied
 * group in order to hold the group until devices are added.  Use
 * iommu_group_put() to release this extra reference count, allowing the
 * group to be automatically reclaimed once it has no devices or external
 * references.
 */
struct iommu_group *iommu_group_alloc(void)
{
	struct iommu_group *group;
	int ret;

	group = kzalloc(sizeof(*group), GFP_KERNEL);
	if (!group)
		return ERR_PTR(-ENOMEM);

	group->kobj.kset = iommu_group_kset;
	mutex_init(&group->mutex);
	INIT_LIST_HEAD(&group->devices);
	BLOCKING_INIT_NOTIFIER_HEAD(&group->notifier);

	mutex_lock(&iommu_group_mutex);

again:
	if (unlikely(0 == ida_pre_get(&iommu_group_ida, GFP_KERNEL))) {
		kfree(group);
		mutex_unlock(&iommu_group_mutex);
		return ERR_PTR(-ENOMEM);
	}

	if (-EAGAIN == ida_get_new(&iommu_group_ida, &group->id))
		goto again;

	mutex_unlock(&iommu_group_mutex);

	ret = kobject_init_and_add(&group->kobj, &iommu_group_ktype,
				   NULL, "%d", group->id);
	if (ret) {
		mutex_lock(&iommu_group_mutex);
		ida_remove(&iommu_group_ida, group->id);
		mutex_unlock(&iommu_group_mutex);
		kobject_put(&group->kobj);
		return ERR_PTR(ret);
	}

	group->devices_kobj = kobject_create_and_add("devices", &group->kobj);
	if (!group->devices_kobj) {
		kobject_put(&group->kobj); /* triggers .release & free */
		return ERR_PTR(-ENOMEM);
	}

	/*
	 * The devices_kobj holds a reference on the group kobject, so
	 * as long as that exists so will the group.  We can therefore
	 * use the devices_kobj for reference counting.
	 */
	kobject_put(&group->kobj);

	pr_debug("Allocated group %d\n", group->id);

	return group;
}
EXPORT_SYMBOL_GPL(iommu_group_alloc);

struct iommu_group *iommu_group_get_by_id(int id)
{
	struct kobject *group_kobj;
	struct iommu_group *group;
	const char *name;

	if (!iommu_group_kset)
		return NULL;

	name = kasprintf(GFP_KERNEL, "%d", id);
	if (!name)
		return NULL;

	group_kobj = kset_find_obj(iommu_group_kset, name);
	kfree(name);

	if (!group_kobj)
		return NULL;

	group = container_of(group_kobj, struct iommu_group, kobj);
	BUG_ON(group->id != id);

	kobject_get(group->devices_kobj);
	kobject_put(&group->kobj);

	return group;
}
EXPORT_SYMBOL_GPL(iommu_group_get_by_id);

/**
 * iommu_group_get_iommudata - retrieve iommu_data registered for a group
 * @group: the group
 *
 * iommu drivers can store data in the group for use when doing iommu
 * operations.  This function provides a way to retrieve it.  Caller
 * should hold a group reference.
 */
void *iommu_group_get_iommudata(struct iommu_group *group)
{
	return group->iommu_data;
}
EXPORT_SYMBOL_GPL(iommu_group_get_iommudata);

/**
 * iommu_group_set_iommudata - set iommu_data for a group
 * @group: the group
 * @iommu_data: new data
 * @release: release function for iommu_data
 *
 * iommu drivers can store data in the group for use when doing iommu
 * operations.  This function provides a way to set the data after
 * the group has been allocated.  Caller should hold a group reference.
 */
void iommu_group_set_iommudata(struct iommu_group *group, void *iommu_data,
			       void (*release)(void *iommu_data))
{
	group->iommu_data = iommu_data;
	group->iommu_data_release = release;
}
EXPORT_SYMBOL_GPL(iommu_group_set_iommudata);

/**
 * iommu_group_set_name - set name for a group
 * @group: the group
 * @name: name
 *
 * Allow iommu driver to set a name for a group.  When set it will
 * appear in a name attribute file under the group in sysfs.
 */
int iommu_group_set_name(struct iommu_group *group, const char *name)
{
	int ret;

	if (group->name) {
		iommu_group_remove_file(group, &iommu_group_attr_name);
		kfree(group->name);
		group->name = NULL;
		if (!name)
			return 0;
	}

	group->name = kstrdup(name, GFP_KERNEL);
	if (!group->name)
		return -ENOMEM;

	ret = iommu_group_create_file(group, &iommu_group_attr_name);
	if (ret) {
		kfree(group->name);
		group->name = NULL;
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(iommu_group_set_name);

static int iommu_group_create_direct_mappings(struct iommu_group *group,
					      struct device *dev)
{
	struct iommu_domain *domain = group->default_domain;
	struct iommu_dm_region *entry;
	struct list_head mappings;
	unsigned long pg_size;
	int ret = 0;

	if (!domain || domain->type != IOMMU_DOMAIN_DMA)
		return 0;

	BUG_ON(!domain->ops->pgsize_bitmap);

	pg_size = 1UL << __ffs(domain->ops->pgsize_bitmap);
	INIT_LIST_HEAD(&mappings);

	iommu_get_dm_regions(dev, &mappings);

	/* We need to consider overlapping regions for different devices */
	list_for_each_entry(entry, &mappings, list) {
		dma_addr_t start, end, addr;

		start = ALIGN(entry->start, pg_size);
		end   = ALIGN(entry->start + entry->length, pg_size);

		for (addr = start; addr < end; addr += pg_size) {
			phys_addr_t phys_addr;

			phys_addr = iommu_iova_to_phys(domain, addr);
			if (phys_addr)
				continue;

			ret = iommu_map(domain, addr, addr, pg_size, entry->prot);
			if (ret)
				goto out;
		}

	}

	iommu_flush_tlb_all(domain);

out:
	iommu_put_dm_regions(dev, &mappings);

	return ret;
}

/**
 * iommu_group_add_device - add a device to an iommu group
 * @group: the group into which to add the device (reference should be held)
 * @dev: the device
 *
 * This function is called by an iommu driver to add a device into a
 * group.  Adding a device increments the group reference count.
 */
int iommu_group_add_device(struct iommu_group *group, struct device *dev)
{
	int ret, i = 0;
	struct iommu_device *device;

	device = kzalloc(sizeof(*device), GFP_KERNEL);
	if (!device)
		return -ENOMEM;

	device->dev = dev;

	ret = sysfs_create_link(&dev->kobj, &group->kobj, "iommu_group");
	if (ret)
		goto err_free_device;

	device->name = kasprintf(GFP_KERNEL, "%s", kobject_name(&dev->kobj));
rename:
	if (!device->name) {
		ret = -ENOMEM;
		goto err_remove_link;
	}

	ret = sysfs_create_link_nowarn(group->devices_kobj,
				       &dev->kobj, device->name);
	if (ret) {
		if (ret == -EEXIST && i >= 0) {
			/*
			 * Account for the slim chance of collision
			 * and append an instance to the name.
			 */
			kfree(device->name);
			device->name = kasprintf(GFP_KERNEL, "%s.%d",
						 kobject_name(&dev->kobj), i++);
			goto rename;
		}
		goto err_free_name;
	}

	kobject_get(group->devices_kobj);

	dev->iommu_group = group;

	iommu_group_create_direct_mappings(group, dev);

	mutex_lock(&group->mutex);
	list_add_tail(&device->list, &group->devices);
	if (group->domain)
		ret = __iommu_attach_device(group->domain, dev);
	mutex_unlock(&group->mutex);
	if (ret)
		goto err_put_group;

	/* Notify any listeners about change to group. */
	blocking_notifier_call_chain(&group->notifier,
				     IOMMU_GROUP_NOTIFY_ADD_DEVICE, dev);

	trace_add_device_to_group(group->id, dev);

	pr_info("Adding device %s to group %d\n", dev_name(dev), group->id);

	return 0;

err_put_group:
	mutex_lock(&group->mutex);
	list_del(&device->list);
	mutex_unlock(&group->mutex);
	dev->iommu_group = NULL;
	kobject_put(group->devices_kobj);
	sysfs_remove_link(group->devices_kobj, device->name);
err_free_name:
	kfree(device->name);
err_remove_link:
	sysfs_remove_link(&dev->kobj, "iommu_group");
err_free_device:
	kfree(device);
	pr_err("Failed to add device %s to group %d: %d\n", dev_name(dev), group->id, ret);
	return ret;
}
EXPORT_SYMBOL_GPL(iommu_group_add_device);

/**
 * iommu_group_remove_device - remove a device from it's current group
 * @dev: device to be removed
 *
 * This function is called by an iommu driver to remove the device from
 * it's current group.  This decrements the iommu group reference count.
 */
void iommu_group_remove_device(struct device *dev)
{
	struct iommu_group *group = dev->iommu_group;
	struct iommu_device *tmp_device, *device = NULL;

	pr_info("Removing device %s from group %d\n", dev_name(dev), group->id);

	/* Pre-notify listeners that a device is being removed. */
	blocking_notifier_call_chain(&group->notifier,
				     IOMMU_GROUP_NOTIFY_DEL_DEVICE, dev);

	mutex_lock(&group->mutex);
	list_for_each_entry(tmp_device, &group->devices, list) {
		if (tmp_device->dev == dev) {
			device = tmp_device;
			list_del(&device->list);
			break;
		}
	}
	mutex_unlock(&group->mutex);

	if (!device)
		return;

	sysfs_remove_link(group->devices_kobj, device->name);
	sysfs_remove_link(&dev->kobj, "iommu_group");

	trace_remove_device_from_group(group->id, dev);

	kfree(device->name);
	kfree(device);
	dev->iommu_group = NULL;
	kobject_put(group->devices_kobj);
}
EXPORT_SYMBOL_GPL(iommu_group_remove_device);

static int iommu_group_device_count(struct iommu_group *group)
{
	struct iommu_device *entry;
	int ret = 0;

	list_for_each_entry(entry, &group->devices, list)
		ret++;

	return ret;
}

/**
 * iommu_group_for_each_dev - iterate over each device in the group
 * @group: the group
 * @data: caller opaque data to be passed to callback function
 * @fn: caller supplied callback function
 *
 * This function is called by group users to iterate over group devices.
 * Callers should hold a reference count to the group during callback.
 * The group->mutex is held across callbacks, which will block calls to
 * iommu_group_add/remove_device.
 */
static int __iommu_group_for_each_dev(struct iommu_group *group, void *data,
				      int (*fn)(struct device *, void *))
{
	struct iommu_device *device;
	int ret = 0;

	list_for_each_entry(device, &group->devices, list) {
		ret = fn(device->dev, data);
		if (ret)
			break;
	}
	return ret;
}


int iommu_group_for_each_dev(struct iommu_group *group, void *data,
			     int (*fn)(struct device *, void *))
{
	int ret;

	mutex_lock(&group->mutex);
	ret = __iommu_group_for_each_dev(group, data, fn);
	mutex_unlock(&group->mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(iommu_group_for_each_dev);

/**
 * iommu_group_get - Return the group for a device and increment reference
 * @dev: get the group that this device belongs to
 *
 * This function is called by iommu drivers and users to get the group
 * for the specified device.  If found, the group is returned and the group
 * reference in incremented, else NULL.
 */
struct iommu_group *iommu_group_get(struct device *dev)
{
	struct iommu_group *group = dev->iommu_group;

	if (group)
		kobject_get(group->devices_kobj);

	return group;
}
EXPORT_SYMBOL_GPL(iommu_group_get);

/**
 * iommu_group_put - Decrement group reference
 * @group: the group to use
 *
 * This function is called by iommu drivers and users to release the
 * iommu group.  Once the reference count is zero, the group is released.
 */
void iommu_group_put(struct iommu_group *group)
{
	if (group)
		kobject_put(group->devices_kobj);
}
EXPORT_SYMBOL_GPL(iommu_group_put);

/**
 * iommu_group_register_notifier - Register a notifier for group changes
 * @group: the group to watch
 * @nb: notifier block to signal
 *
 * This function allows iommu group users to track changes in a group.
 * See include/linux/iommu.h for actions sent via this notifier.  Caller
 * should hold a reference to the group throughout notifier registration.
 */
int iommu_group_register_notifier(struct iommu_group *group,
				  struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&group->notifier, nb);
}
EXPORT_SYMBOL_GPL(iommu_group_register_notifier);

/**
 * iommu_group_unregister_notifier - Unregister a notifier
 * @group: the group to watch
 * @nb: notifier block to signal
 *
 * Unregister a previously registered group notifier block.
 */
int iommu_group_unregister_notifier(struct iommu_group *group,
				    struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&group->notifier, nb);
}
EXPORT_SYMBOL_GPL(iommu_group_unregister_notifier);

/**
 * iommu_group_id - Return ID for a group
 * @group: the group to ID
 *
 * Return the unique ID for the group matching the sysfs group number.
 */
int iommu_group_id(struct iommu_group *group)
{
	return group->id;
}
EXPORT_SYMBOL_GPL(iommu_group_id);

static struct iommu_group *get_pci_alias_group(struct pci_dev *pdev,
					       unsigned long *devfns);

/*
 * To consider a PCI device isolated, we require ACS to support Source
 * Validation, Request Redirection, Completer Redirection, and Upstream
 * Forwarding.  This effectively means that devices cannot spoof their
 * requester ID, requests and completions cannot be redirected, and all
 * transactions are forwarded upstream, even as it passes through a
 * bridge where the target device is downstream.
 */
#define REQ_ACS_FLAGS   (PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF)

/*
 * For multifunction devices which are not isolated from each other, find
 * all the other non-isolated functions and look for existing groups.  For
 * each function, we also need to look for aliases to or from other devices
 * that may already have a group.
 */
static struct iommu_group *get_pci_function_alias_group(struct pci_dev *pdev,
							unsigned long *devfns)
{
	struct pci_dev *tmp = NULL;
	struct iommu_group *group;

	if (!pdev->multifunction || pci_acs_enabled(pdev, REQ_ACS_FLAGS))
		return NULL;

	for_each_pci_dev(tmp) {
		if (tmp == pdev || tmp->bus != pdev->bus ||
		    PCI_SLOT(tmp->devfn) != PCI_SLOT(pdev->devfn) ||
		    pci_acs_enabled(tmp, REQ_ACS_FLAGS))
			continue;

		group = get_pci_alias_group(tmp, devfns);
		if (group) {
			pci_dev_put(tmp);
			return group;
		}
	}

	return NULL;
}

/*
 * Look for aliases to or from the given device for exisiting groups.  The
 * dma_alias_devfn only supports aliases on the same bus, therefore the search
 * space is quite small (especially since we're really only looking at pcie
 * device, and therefore only expect multiple slots on the root complex or
 * downstream switch ports).  It's conceivable though that a pair of
 * multifunction devices could have aliases between them that would cause a
 * loop.  To prevent this, we use a bitmap to track where we've been.
 */
static struct iommu_group *get_pci_alias_group(struct pci_dev *pdev,
					       unsigned long *devfns)
{
	struct pci_dev *tmp = NULL;
	struct iommu_group *group;

	if (test_and_set_bit(pdev->devfn & 0xff, devfns))
		return NULL;

	group = iommu_group_get(&pdev->dev);
	if (group)
		return group;

	for_each_pci_dev(tmp) {
		if (tmp == pdev || tmp->bus != pdev->bus)
			continue;

		/* We alias them or they alias us */
		if (((pdev->dev_flags & PCI_DEV_FLAGS_DMA_ALIAS_DEVFN) &&
		     pdev->dma_alias_devfn == tmp->devfn) ||
		    ((tmp->dev_flags & PCI_DEV_FLAGS_DMA_ALIAS_DEVFN) &&
		     tmp->dma_alias_devfn == pdev->devfn)) {

			group = get_pci_alias_group(tmp, devfns);
			if (group) {
				pci_dev_put(tmp);
				return group;
			}

			group = get_pci_function_alias_group(tmp, devfns);
			if (group) {
				pci_dev_put(tmp);
				return group;
			}
		}
	}

	return NULL;
}

struct group_for_pci_data {
	struct pci_dev *pdev;
	struct iommu_group *group;
};

/*
 * DMA alias iterator callback, return the last seen device.  Stop and return
 * the IOMMU group if we find one along the way.
 */
static int get_pci_alias_or_group(struct pci_dev *pdev, u16 alias, void *opaque)
{
	struct group_for_pci_data *data = opaque;

	data->pdev = pdev;
	data->group = iommu_group_get(&pdev->dev);

	return data->group != NULL;
}

/*
 * Generic device_group call-back function. It just allocates one
 * iommu-group per device.
 */
struct iommu_group *generic_device_group(struct device *dev)
{
	struct iommu_group *group;

	group = iommu_group_alloc();
	if (IS_ERR(group))
		return NULL;

	return group;
}

/*
 * Use standard PCI bus topology, isolation features, and DMA alias quirks
 * to find or create an IOMMU group for a device.
 */
struct iommu_group *pci_device_group(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct group_for_pci_data data;
	struct pci_bus *bus;
	struct iommu_group *group = NULL;
	u64 devfns[4] = { 0 };

	if (WARN_ON(!dev_is_pci(dev)))
		return ERR_PTR(-EINVAL);

	/*
	 * Find the upstream DMA alias for the device.  A device must not
	 * be aliased due to topology in order to have its own IOMMU group.
	 * If we find an alias along the way that already belongs to a
	 * group, use it.
	 */
	if (pci_for_each_dma_alias(pdev, get_pci_alias_or_group, &data))
		return data.group;

	pdev = data.pdev;

	/*
	 * Continue upstream from the point of minimum IOMMU granularity
	 * due to aliases to the point where devices are protected from
	 * peer-to-peer DMA by PCI ACS.  Again, if we find an existing
	 * group, use it.
	 */
	for (bus = pdev->bus; !pci_is_root_bus(bus); bus = bus->parent) {
		if (!bus->self)
			continue;

		if (pci_acs_path_enabled(bus->self, NULL, REQ_ACS_FLAGS))
			break;

		pdev = bus->self;

		group = iommu_group_get(&pdev->dev);
		if (group)
			return group;
	}

	/*
	 * Look for existing groups on device aliases.  If we alias another
	 * device or another device aliases us, use the same group.
	 */
	group = get_pci_alias_group(pdev, (unsigned long *)devfns);
	if (group)
		return group;

	/*
	 * Look for existing groups on non-isolated functions on the same
	 * slot and aliases of those funcions, if any.  No need to clear
	 * the search bitmap, the tested devfns are still valid.
	 */
	group = get_pci_function_alias_group(pdev, (unsigned long *)devfns);
	if (group)
		return group;

	/* No shared group found, allocate new */
	group = iommu_group_alloc();
	if (IS_ERR(group))
		return NULL;

	return group;
}

/**
 * iommu_group_get_for_dev - Find or create the IOMMU group for a device
 * @dev: target device
 *
 * This function is intended to be called by IOMMU drivers and extended to
 * support common, bus-defined algorithms when determining or creating the
 * IOMMU group for a device.  On success, the caller will hold a reference
 * to the returned IOMMU group, which will already include the provided
 * device.  The reference should be released with iommu_group_put().
 */
struct iommu_group *iommu_group_get_for_dev(struct device *dev)
{
	const struct iommu_ops *ops = dev->bus->iommu_ops;
	struct iommu_group *group;
	int ret;

	group = iommu_group_get(dev);
	if (group)
		return group;

	group = ERR_PTR(-EINVAL);

	if (ops && ops->device_group)
		group = ops->device_group(dev);

	if (IS_ERR(group))
		return group;

	/*
	 * Try to allocate a default domain - needs support from the
	 * IOMMU driver.
	 */
	if (!group->default_domain) {
		group->default_domain = __iommu_domain_alloc(dev->bus,
							     IOMMU_DOMAIN_DMA);
		if (!group->domain)
			group->domain = group->default_domain;
	}

	ret = iommu_group_add_device(group, dev);
	if (ret) {
		iommu_group_put(group);
		return ERR_PTR(ret);
	}

	return group;
}

struct iommu_domain *iommu_group_default_domain(struct iommu_group *group)
{
	return group->default_domain;
}

static int add_iommu_group(struct device *dev, void *data)
{
	struct iommu_callback_data *cb = data;
	const struct iommu_ops *ops = cb->ops;
	int ret;

	if (!ops->add_device)
		return 0;

	WARN_ON(dev->iommu_group);

	ret = ops->add_device(dev);

	/*
	 * We ignore -ENODEV errors for now, as they just mean that the
	 * device is not translated by an IOMMU. We still care about
	 * other errors and fail to initialize when they happen.
	 */
	if (ret == -ENODEV)
		ret = 0;

	return ret;
}

static int remove_iommu_group(struct device *dev, void *data)
{
	struct iommu_callback_data *cb = data;
	const struct iommu_ops *ops = cb->ops;

	if (ops->remove_device && dev->iommu_group)
		ops->remove_device(dev);

	return 0;
}

static int iommu_bus_notifier(struct notifier_block *nb,
			      unsigned long action, void *data)
{
	struct device *dev = data;
	const struct iommu_ops *ops = dev->bus->iommu_ops;
	struct iommu_group *group;
	unsigned long group_action = 0;

	/*
	 * ADD/DEL call into iommu driver ops if provided, which may
	 * result in ADD/DEL notifiers to group->notifier
	 */
	if (action == BUS_NOTIFY_ADD_DEVICE) {
		if (ops->add_device)
			return ops->add_device(dev);
	} else if (action == BUS_NOTIFY_REMOVED_DEVICE) {
		if (ops->remove_device && dev->iommu_group) {
			ops->remove_device(dev);
			return 0;
		}
	}

	/*
	 * Remaining BUS_NOTIFYs get filtered and republished to the
	 * group, if anyone is listening
	 */
	group = iommu_group_get(dev);
	if (!group)
		return 0;

	switch (action) {
	case BUS_NOTIFY_BIND_DRIVER:
		group_action = IOMMU_GROUP_NOTIFY_BIND_DRIVER;
		break;
	case BUS_NOTIFY_BOUND_DRIVER:
		group_action = IOMMU_GROUP_NOTIFY_BOUND_DRIVER;
		break;
	case BUS_NOTIFY_UNBIND_DRIVER:
		group_action = IOMMU_GROUP_NOTIFY_UNBIND_DRIVER;
		break;
	case BUS_NOTIFY_UNBOUND_DRIVER:
		group_action = IOMMU_GROUP_NOTIFY_UNBOUND_DRIVER;
		break;
	}

	if (group_action)
		blocking_notifier_call_chain(&group->notifier,
					     group_action, dev);

	iommu_group_put(group);
	return 0;
}

static int iommu_bus_init(struct bus_type *bus, const struct iommu_ops *ops)
{
	int err;
	struct notifier_block *nb;
	struct iommu_callback_data cb = {
		.ops = ops,
	};

	nb = kzalloc(sizeof(struct notifier_block), GFP_KERNEL);
	if (!nb)
		return -ENOMEM;

	nb->notifier_call = iommu_bus_notifier;

	err = bus_register_notifier(bus, nb);
	if (err)
		goto out_free;

	err = bus_for_each_dev(bus, NULL, &cb, add_iommu_group);
	if (err)
		goto out_err;


	return 0;

out_err:
	/* Clean up */
	bus_for_each_dev(bus, NULL, &cb, remove_iommu_group);
	bus_unregister_notifier(bus, nb);

out_free:
	kfree(nb);

	return err;
}

/**
 * bus_set_iommu - set iommu-callbacks for the bus
 * @bus: bus.
 * @ops: the callbacks provided by the iommu-driver
 *
 * This function is called by an iommu driver to set the iommu methods
 * used for a particular bus. Drivers for devices on that bus can use
 * the iommu-api after these ops are registered.
 * This special function is needed because IOMMUs are usually devices on
 * the bus itself, so the iommu drivers are not initialized when the bus
 * is set up. With this function the iommu-driver can set the iommu-ops
 * afterwards.
 */
int bus_set_iommu(struct bus_type *bus, const struct iommu_ops *ops)
{
	int err;

	if (bus->iommu_ops != NULL)
		return -EBUSY;

	bus->iommu_ops = ops;

	/* Do IOMMU specific setup for this bus-type */
	err = iommu_bus_init(bus, ops);
	if (err)
		bus->iommu_ops = NULL;

	return err;
}
EXPORT_SYMBOL_GPL(bus_set_iommu);

bool iommu_present(struct bus_type *bus)
{
	if (!bus)
		return false;
	return bus->iommu_ops != NULL;
}
EXPORT_SYMBOL_GPL(iommu_present);

bool iommu_capable(struct bus_type *bus, enum iommu_cap cap)
{
	if (!bus->iommu_ops || !bus->iommu_ops->capable)
		return false;

	return bus->iommu_ops->capable(cap);
}
EXPORT_SYMBOL_GPL(iommu_capable);

/**
 * iommu_set_fault_handler() - set a fault handler for an iommu domain
 * @domain: iommu domain
 * @handler: fault handler
 * @token: user data, will be passed back to the fault handler
 *
 * This function should be used by IOMMU users which want to be notified
 * whenever an IOMMU fault happens.
 *
 * The fault handler itself should return 0 on success, and an appropriate
 * error code otherwise.
 */
void iommu_set_fault_handler(struct iommu_domain *domain,
					iommu_fault_handler_t handler,
					void *token)
{
	BUG_ON(!domain);

	domain->handler = handler;
	domain->handler_token = token;
}
EXPORT_SYMBOL_GPL(iommu_set_fault_handler);

/**
 * iommu_trigger_fault() - trigger an IOMMU fault
 * @domain: iommu domain
 *
 * Triggers a fault on the device to which this domain is attached.
 *
 * This function should only be used for debugging purposes, for obvious
 * reasons.
 */
void iommu_trigger_fault(struct iommu_domain *domain, unsigned long flags)
{
	if (domain->ops->trigger_fault)
		domain->ops->trigger_fault(domain, flags);
}

/**
 * iommu_reg_read() - read an IOMMU register
 *
 * Reads the IOMMU register at the given offset.
 */
unsigned long iommu_reg_read(struct iommu_domain *domain, unsigned long offset)
{
	if (domain->ops->reg_read)
		return domain->ops->reg_read(domain, offset);
	return 0;
}

/**
 * iommu_reg_write() - write an IOMMU register
 *
 * Writes the given value to the IOMMU register at the given offset.
 */
void iommu_reg_write(struct iommu_domain *domain, unsigned long offset,
		     unsigned long val)
{
	if (domain->ops->reg_write)
		domain->ops->reg_write(domain, offset, val);
}

static struct iommu_domain *__iommu_domain_alloc(struct bus_type *bus,
						 unsigned type)
{
	struct iommu_domain *domain;

	if (bus == NULL || bus->iommu_ops == NULL)
		return NULL;

	domain = bus->iommu_ops->domain_alloc(type);
	if (!domain)
		return NULL;

	domain->ops  = bus->iommu_ops;
	domain->type = type;

	return domain;
}

struct iommu_domain *iommu_domain_alloc(struct bus_type *bus)
{
	return __iommu_domain_alloc(bus, IOMMU_DOMAIN_UNMANAGED);
}
EXPORT_SYMBOL_GPL(iommu_domain_alloc);

void iommu_domain_free(struct iommu_domain *domain)
{
	iommu_debug_domain_remove(domain);
	domain->ops->domain_free(domain);
}
EXPORT_SYMBOL_GPL(iommu_domain_free);

static int __iommu_attach_device(struct iommu_domain *domain,
				 struct device *dev)
{
	int ret;
	if (unlikely(domain->ops->attach_dev == NULL))
		return -ENODEV;

	ret = domain->ops->attach_dev(domain, dev);
	if (!ret) {
		trace_attach_device_to_domain(dev);
		iommu_debug_attach_device(domain, dev);
	}
	return ret;
}

int iommu_attach_device(struct iommu_domain *domain, struct device *dev)
{
	struct iommu_group *group;
	int ret;

	group = iommu_group_get(dev);
	/* FIXME: Remove this when groups a mandatory for iommu drivers */
	if (group == NULL)
		return __iommu_attach_device(domain, dev);

	/*
	 * We have a group - lock it to make sure the device-count doesn't
	 * change while we are attaching
	 */
	mutex_lock(&group->mutex);
	ret = -EINVAL;
	if (iommu_group_device_count(group) != 1)
		goto out_unlock;

	ret = __iommu_attach_group(domain, group);

out_unlock:
	mutex_unlock(&group->mutex);
	iommu_group_put(group);

	return ret;
}
EXPORT_SYMBOL_GPL(iommu_attach_device);

static void __iommu_detach_device(struct iommu_domain *domain,
				  struct device *dev)
{
	if (unlikely(domain->ops->detach_dev == NULL))
		return;

	domain->ops->detach_dev(domain, dev);
	trace_detach_device_from_domain(dev);
}

void iommu_detach_device(struct iommu_domain *domain, struct device *dev)
{
	struct iommu_group *group;

	group = iommu_group_get(dev);
	/* FIXME: Remove this when groups a mandatory for iommu drivers */
	if (group == NULL)
		return __iommu_detach_device(domain, dev);

	mutex_lock(&group->mutex);
	if (iommu_group_device_count(group) != 1) {
		WARN_ON(1);
		goto out_unlock;
	}

	__iommu_detach_group(domain, group);

out_unlock:
	mutex_unlock(&group->mutex);
	iommu_group_put(group);
}
EXPORT_SYMBOL_GPL(iommu_detach_device);

struct iommu_domain *iommu_get_domain_for_dev(struct device *dev)
{
	struct iommu_domain *domain;
	struct iommu_group *group;

	group = iommu_group_get(dev);
	/* FIXME: Remove this when groups a mandatory for iommu drivers */
	if (group == NULL)
		return NULL;

	domain = group->domain;

	iommu_group_put(group);

	return domain;
}
EXPORT_SYMBOL_GPL(iommu_get_domain_for_dev);

/*
 * IOMMU groups are really the natrual working unit of the IOMMU, but
 * the IOMMU API works on domains and devices.  Bridge that gap by
 * iterating over the devices in a group.  Ideally we'd have a single
 * device which represents the requestor ID of the group, but we also
 * allow IOMMU drivers to create policy defined minimum sets, where
 * the physical hardware may be able to distiguish members, but we
 * wish to group them at a higher level (ex. untrusted multi-function
 * PCI devices).  Thus we attach each device.
 */
static int iommu_group_do_attach_device(struct device *dev, void *data)
{
	struct iommu_domain *domain = data;

	return __iommu_attach_device(domain, dev);
}

static int __iommu_attach_group(struct iommu_domain *domain,
				struct iommu_group *group)
{
	int ret;

	if (group->default_domain && group->domain != group->default_domain)
		return -EBUSY;

	ret = __iommu_group_for_each_dev(group, domain,
					 iommu_group_do_attach_device);
	if (ret == 0)
		group->domain = domain;

	return ret;
}

int iommu_attach_group(struct iommu_domain *domain, struct iommu_group *group)
{
	int ret;

	mutex_lock(&group->mutex);
	ret = __iommu_attach_group(domain, group);
	mutex_unlock(&group->mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(iommu_attach_group);

static int iommu_group_do_detach_device(struct device *dev, void *data)
{
	struct iommu_domain *domain = data;

	__iommu_detach_device(domain, dev);

	return 0;
}

static void __iommu_detach_group(struct iommu_domain *domain,
				 struct iommu_group *group)
{
	int ret;

	if (!group->default_domain) {
		__iommu_group_for_each_dev(group, domain,
					   iommu_group_do_detach_device);
		group->domain = NULL;
		return;
	}

	if (group->domain == group->default_domain)
		return;

	/* Detach by re-attaching to the default domain */
	ret = __iommu_group_for_each_dev(group, group->default_domain,
					 iommu_group_do_attach_device);
	if (ret != 0)
		WARN_ON(1);
	else
		group->domain = group->default_domain;
}

void iommu_detach_group(struct iommu_domain *domain, struct iommu_group *group)
{
	mutex_lock(&group->mutex);
	__iommu_detach_group(domain, group);
	mutex_unlock(&group->mutex);
}
EXPORT_SYMBOL_GPL(iommu_detach_group);

phys_addr_t iommu_iova_to_phys(struct iommu_domain *domain, dma_addr_t iova)
{
	if (unlikely(domain->ops->iova_to_phys == NULL))
		return 0;

	return domain->ops->iova_to_phys(domain, iova);
}
EXPORT_SYMBOL_GPL(iommu_iova_to_phys);

phys_addr_t iommu_iova_to_phys_hard(struct iommu_domain *domain,
				    dma_addr_t iova)
{
	if (unlikely(domain->ops->iova_to_phys_hard == NULL))
		return 0;

	return domain->ops->iova_to_phys_hard(domain, iova);
}

uint64_t iommu_iova_to_pte(struct iommu_domain *domain,
				    dma_addr_t iova)
{
	if (unlikely(domain->ops->iova_to_pte == NULL))
		return 0;

	return domain->ops->iova_to_pte(domain, iova);
}

bool iommu_is_iova_coherent(struct iommu_domain *domain, dma_addr_t iova)
{
	if (unlikely(domain->ops->is_iova_coherent == NULL))
		return 0;

	return domain->ops->is_iova_coherent(domain, iova);
}
static unsigned long iommu_get_pgsize_bitmap(struct iommu_domain *domain)
{
	if (domain->ops->get_pgsize_bitmap)
		return domain->ops->get_pgsize_bitmap(domain);
	return domain->ops->pgsize_bitmap;
}

size_t iommu_pgsize(unsigned long pgsize_bitmap,
		    unsigned long addr_merge, size_t size)
{
	unsigned int pgsize_idx;
	size_t pgsize;

	/* Max page size that still fits into 'size' */
	pgsize_idx = __fls(size);

	/* need to consider alignment requirements ? */
	if (likely(addr_merge)) {
		/* Max page size allowed by address */
		unsigned int align_pgsize_idx = __ffs(addr_merge);
		pgsize_idx = min(pgsize_idx, align_pgsize_idx);
	}

	/* build a mask of acceptable page sizes */
	pgsize = (1UL << (pgsize_idx + 1)) - 1;

	/* throw away page sizes not supported by the hardware */
	pgsize &= pgsize_bitmap;

	/* make sure we're still sane */
	if (!pgsize) {
		pr_err("invalid pgsize/addr/size! 0x%lx 0x%lx 0x%zx\n",
		       pgsize_bitmap, addr_merge, size);
		BUG();
	}

	/* pick the biggest page */
	pgsize_idx = __fls(pgsize);
	pgsize = 1UL << pgsize_idx;

	return pgsize;
}

int iommu_map(struct iommu_domain *domain, unsigned long iova,
	      phys_addr_t paddr, size_t size, int prot)
{
	unsigned long orig_iova = iova, pgsize_bitmap;
	unsigned int min_pagesz;
	size_t orig_size = size;
	int ret = 0;

	trace_map_start(iova, paddr, size);
	if (unlikely(domain->ops->map == NULL ||
		     (domain->ops->pgsize_bitmap == 0UL &&
		      !domain->ops->get_pgsize_bitmap))) {
		trace_map_end(iova, paddr, size);
		return -ENODEV;
	}

	if (unlikely(!(domain->type & __IOMMU_DOMAIN_PAGING)))
		return -EINVAL;

	pgsize_bitmap = iommu_get_pgsize_bitmap(domain);
	/* find out the minimum page size supported */
	min_pagesz = 1 << __ffs(pgsize_bitmap);

	/*
	 * both the virtual address and the physical one, as well as
	 * the size of the mapping, must be aligned (at least) to the
	 * size of the smallest page supported by the hardware
	 */
	if (!IS_ALIGNED(iova | paddr | size, min_pagesz)) {
		pr_err("unaligned: iova 0x%lx pa %pa size 0x%zx min_pagesz 0x%x\n",
		       iova, &paddr, size, min_pagesz);
		trace_map_end(iova, paddr, size);
		return -EINVAL;
	}

	pr_debug("map: iova 0x%lx pa %pa size 0x%zx\n", iova, &paddr, size);

	while (size) {
		size_t pgsize = iommu_pgsize(pgsize_bitmap, iova | paddr, size);

		pr_debug("mapping: iova 0x%lx pa %pa pgsize 0x%zx\n",
			 iova, &paddr, pgsize);

		ret = domain->ops->map(domain, iova, paddr, pgsize, prot);
		if (ret)
			break;

		iova += pgsize;
		paddr += pgsize;
		size -= pgsize;
	}

	/* unroll mapping in case something went wrong */
	if (ret)
		iommu_unmap(domain, orig_iova, orig_size - size);
	else
		trace_map(orig_iova, paddr, orig_size);

	trace_map_end(iova, paddr, size);
	return ret;
}
EXPORT_SYMBOL_GPL(iommu_map);

static size_t __iommu_unmap(struct iommu_domain *domain,
			    unsigned long iova, size_t size,
			    bool sync)
{
	const struct iommu_ops *ops = domain->ops;
	size_t unmapped_page, unmapped = 0;
	unsigned int min_pagesz;
	unsigned long orig_iova = iova;

	trace_unmap_start(iova, 0, size);
	if (unlikely(ops->unmap == NULL ||
		     (ops->pgsize_bitmap == 0UL &&
		      !ops->get_pgsize_bitmap))) {
		trace_unmap_end(iova, 0, size);
		return -ENODEV;
	}

	if (unlikely(!(domain->type & __IOMMU_DOMAIN_PAGING))) {
		trace_unmap_end(iova, 0, size);
		return -EINVAL;
	}

	/* find out the minimum page size supported */
	min_pagesz = 1 << __ffs(iommu_get_pgsize_bitmap(domain));

	/*
	 * The virtual address, as well as the size of the mapping, must be
	 * aligned (at least) to the size of the smallest page supported
	 * by the hardware
	 */
	if (!IS_ALIGNED(iova | size, min_pagesz)) {
		pr_err("unaligned: iova 0x%lx size 0x%zx min_pagesz 0x%x\n",
		       iova, size, min_pagesz);
		trace_unmap_end(iova, 0, size);
		return -EINVAL;
	}

	pr_debug("unmap this: iova 0x%lx size 0x%zx\n", iova, size);

	/*
	 * Keep iterating until we either unmap 'size' bytes (or more)
	 * or we hit an area that isn't mapped.
	 */
	while (unmapped < size) {
		size_t left = size - unmapped;

		unmapped_page = ops->unmap(domain, iova, left);
		if (!unmapped_page)
			break;

		if (sync && ops->iotlb_range_add)
			ops->iotlb_range_add(domain, iova, left);

		pr_debug("unmapped: iova 0x%lx size 0x%zx\n",
			 iova, unmapped_page);

		iova += unmapped_page;
		unmapped += unmapped_page;
	}

	if (sync && ops->iotlb_sync)
		ops->iotlb_sync(domain);

	trace_unmap(orig_iova, size, unmapped);
	trace_unmap_end(orig_iova, 0, size);
	return unmapped;
}

size_t iommu_unmap(struct iommu_domain *domain,
		   unsigned long iova, size_t size)
{
	return __iommu_unmap(domain, iova, size, true);
}
EXPORT_SYMBOL_GPL(iommu_unmap);

size_t iommu_unmap_fast(struct iommu_domain *domain,
			unsigned long iova, size_t size)
{
	return __iommu_unmap(domain, iova, size, false);
}
EXPORT_SYMBOL_GPL(iommu_unmap_fast);

size_t default_iommu_map_sg(struct iommu_domain *domain, unsigned long iova,
			 struct scatterlist *sg, unsigned int nents, int prot)
{
	struct scatterlist *s;
	size_t mapped = 0;
	unsigned int i, min_pagesz;
	int ret;
	unsigned long pgsize_bitmap;

	if (unlikely(domain->ops->pgsize_bitmap == 0UL &&
		     !domain->ops->get_pgsize_bitmap))
		return 0;

	pgsize_bitmap = iommu_get_pgsize_bitmap(domain);
	min_pagesz = 1 << __ffs(pgsize_bitmap);

	for_each_sg(sg, s, nents, i) {
		phys_addr_t phys = page_to_phys(sg_page(s)) + s->offset;

		/*
		 * We are mapping on IOMMU page boundaries, so offset within
		 * the page must be 0. However, the IOMMU may support pages
		 * smaller than PAGE_SIZE, so s->offset may still represent
		 * an offset of that boundary within the CPU page.
		 */
		if (!IS_ALIGNED(s->offset, min_pagesz))
			goto out_err;

		ret = iommu_map(domain, iova + mapped, phys, s->length, prot);
		if (ret)
			goto out_err;

		mapped += s->length;
	}

	return mapped;

out_err:
	/* undo mappings already done */
	iommu_unmap(domain, iova, mapped);

	return 0;

}
EXPORT_SYMBOL_GPL(default_iommu_map_sg);

/* DEPRECATED */
int iommu_map_range(struct iommu_domain *domain, unsigned int iova,
		    struct scatterlist *sg, unsigned int len, int opt)
{
	return -ENODEV;
}

/* DEPRECATED */
int iommu_unmap_range(struct iommu_domain *domain, unsigned int iova,
		      unsigned int len)
{
	return -ENODEV;
}

int iommu_domain_window_enable(struct iommu_domain *domain, u32 wnd_nr,
			       phys_addr_t paddr, u64 size, int prot)
{
	if (unlikely(domain->ops->domain_window_enable == NULL))
		return -ENODEV;

	return domain->ops->domain_window_enable(domain, wnd_nr, paddr, size,
						 prot);
}
EXPORT_SYMBOL_GPL(iommu_domain_window_enable);

void iommu_domain_window_disable(struct iommu_domain *domain, u32 wnd_nr)
{
	if (unlikely(domain->ops->domain_window_disable == NULL))
		return;

	return domain->ops->domain_window_disable(domain, wnd_nr);
}
EXPORT_SYMBOL_GPL(iommu_domain_window_disable);

struct dentry *iommu_debugfs_top;

static int __init iommu_init(void)
{
	iommu_group_kset = kset_create_and_add("iommu_groups",
					       NULL, kernel_kobj);
	ida_init(&iommu_group_ida);
	mutex_init(&iommu_group_mutex);

	BUG_ON(!iommu_group_kset);

	iommu_debugfs_top = debugfs_create_dir("iommu", NULL);
	if (!iommu_debugfs_top) {
		pr_err("Couldn't create iommu debugfs directory\n");
		return -ENODEV;
	}

	return 0;
}
core_initcall(iommu_init);

int iommu_domain_get_attr(struct iommu_domain *domain,
			  enum iommu_attr attr, void *data)
{
	struct iommu_domain_geometry *geometry;
	bool *paging;
	int ret = 0;
	u32 *count;

	switch (attr) {
	case DOMAIN_ATTR_GEOMETRY:
		geometry  = data;
		*geometry = domain->geometry;

		break;
	case DOMAIN_ATTR_PAGING:
		paging  = data;
		*paging = (iommu_get_pgsize_bitmap(domain) != 0UL);
		break;
	case DOMAIN_ATTR_WINDOWS:
		count = data;

		if (domain->ops->domain_get_windows != NULL)
			*count = domain->ops->domain_get_windows(domain);
		else
			ret = -ENODEV;

		break;
	default:
		if (!domain->ops->domain_get_attr)
			return -EINVAL;

		ret = domain->ops->domain_get_attr(domain, attr, data);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(iommu_domain_get_attr);

int iommu_domain_set_attr(struct iommu_domain *domain,
			  enum iommu_attr attr, void *data)
{
	int ret = 0;
	u32 *count;

	switch (attr) {
	case DOMAIN_ATTR_WINDOWS:
		count = data;

		if (domain->ops->domain_set_windows != NULL)
			ret = domain->ops->domain_set_windows(domain, *count);
		else
			ret = -ENODEV;

		break;
	default:
		if (domain->ops->domain_set_attr == NULL)
			return -EINVAL;

		ret = domain->ops->domain_set_attr(domain, attr, data);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(iommu_domain_set_attr);

int iommu_dma_supported(struct iommu_domain *domain, struct device *dev,
								u64 mask)
{
	if (domain->ops->dma_supported)
		return domain->ops->dma_supported(domain, dev, mask);
	return 0;
}

void iommu_get_dm_regions(struct device *dev, struct list_head *list)
{
	const struct iommu_ops *ops = dev->bus->iommu_ops;

	if (ops && ops->get_dm_regions)
		ops->get_dm_regions(dev, list);
}

void iommu_put_dm_regions(struct device *dev, struct list_head *list)
{
	const struct iommu_ops *ops = dev->bus->iommu_ops;

	if (ops && ops->put_dm_regions)
		ops->put_dm_regions(dev, list);
}

/* Request that a device is direct mapped by the IOMMU */
int iommu_request_dm_for_dev(struct device *dev)
{
	struct iommu_domain *dm_domain;
	struct iommu_group *group;
	int ret;

	/* Device must already be in a group before calling this function */
	group = iommu_group_get(dev);
	if (!group)
		return -EINVAL;

	mutex_lock(&group->mutex);

	/* Check if the default domain is already direct mapped */
	ret = 0;
	if (group->default_domain &&
	    group->default_domain->type == IOMMU_DOMAIN_IDENTITY)
		goto out;

	/* Don't change mappings of existing devices */
	ret = -EBUSY;
	if (iommu_group_device_count(group) != 1)
		goto out;

	/* Allocate a direct mapped domain */
	ret = -ENOMEM;
	dm_domain = __iommu_domain_alloc(dev->bus, IOMMU_DOMAIN_IDENTITY);
	if (!dm_domain)
		goto out;

	/* Attach the device to the domain */
	ret = __iommu_attach_group(dm_domain, group);
	if (ret) {
		iommu_domain_free(dm_domain);
		goto out;
	}

	/* Make the direct mapped domain the default for this group */
	if (group->default_domain)
		iommu_domain_free(group->default_domain);
	group->default_domain = dm_domain;

	pr_info("Using direct mapping for device %s\n", dev_name(dev));

	ret = 0;
out:
	mutex_unlock(&group->mutex);
	iommu_group_put(group);

	return ret;
}
