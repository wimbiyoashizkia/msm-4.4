/* Copyright (c) 2002,2007-2017,2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/export.h>
#include <linux/vmalloc.h>
#include <asm/cacheflush.h>
#include <linux/slab.h>
#include <linux/kmemleak.h>
#include <linux/highmem.h>
#include <linux/scatterlist.h>
#include <soc/qcom/scm.h>
#include <soc/qcom/secure_buffer.h>
#include <linux/ratelimit.h>

#include "kgsl.h"
#include "kgsl_sharedmem.h"
#include "kgsl_cffdump.h"
#include "kgsl_device.h"
#include "kgsl_log.h"
#include "kgsl_mmu.h"

/*
 * The user can set this from debugfs to force failed memory allocations to
 * fail without trying OOM first.  This is a debug setting useful for
 * stress applications that want to test failure cases without pushing the
 * system into unrecoverable OOM panics
 */

static bool sharedmem_noretry_flag;

static DEFINE_MUTEX(kernel_map_global_lock);

struct cp2_mem_chunks {
	unsigned int chunk_list;
	unsigned int chunk_list_size;
	unsigned int chunk_size;
} __attribute__ ((__packed__));

struct cp2_lock_req {
	struct cp2_mem_chunks chunks;
	unsigned int mem_usage;
	unsigned int lock;
} __attribute__ ((__packed__));

#define MEM_PROTECT_LOCK_ID2		0x0A
#define MEM_PROTECT_LOCK_ID2_FLAT	0x11

/* An attribute for showing per-process memory statistics */
struct kgsl_mem_entry_attribute {
	struct attribute attr;
	int memtype;
	ssize_t (*show)(struct kgsl_process_private *priv,
		int type, char *buf);
};

#define to_mem_entry_attr(a) \
container_of(a, struct kgsl_mem_entry_attribute, attr)

#define __MEM_ENTRY_ATTR(_type, _name, _show) \
{ \
	.attr = { .name = __stringify(_name), .mode = 0444 }, \
	.memtype = _type, \
	.show = _show, \
}

/*
 * A structure to hold the attributes for a particular memory type.
 * For each memory type in each process we store the current and maximum
 * memory usage and display the counts in sysfs.  This structure and
 * the following macro allow us to simplify the definition for those
 * adding new memory types
 */

struct mem_entry_stats {
	int memtype;
	struct kgsl_mem_entry_attribute attr;
	struct kgsl_mem_entry_attribute max_attr;
};


#define MEM_ENTRY_STAT(_type, _name) \
{ \
	.memtype = _type, \
	.attr = __MEM_ENTRY_ATTR(_type, _name, mem_entry_show), \
	.max_attr = __MEM_ENTRY_ATTR(_type, _name##_max, \
		mem_entry_max_show), \
}

static void kgsl_cma_unlock_secure(struct kgsl_memdesc *memdesc);

static ssize_t
imported_mem_show(struct kgsl_process_private *priv,
				int type, char *buf)
{
	struct kgsl_mem_entry *entry;
	uint64_t imported_mem = 0;
	int id = 0;

	spin_lock(&priv->mem_lock);
	for (entry = idr_get_next(&priv->mem_idr, &id); entry;
		id++, entry = idr_get_next(&priv->mem_idr, &id)) {

		int egl_surface_count = 0, egl_image_count = 0;
		struct kgsl_memdesc *m = &entry->memdesc;

		if ((kgsl_memdesc_usermem_type(m) != KGSL_MEM_ENTRY_ION) ||
			entry->pending_free || (kgsl_mem_entry_get(entry) == 0))
			continue;
		spin_unlock(&priv->mem_lock);

		kgsl_get_egl_counts(entry, &egl_surface_count,
				&egl_image_count);

		if (kgsl_memdesc_get_memtype(m) ==
				KGSL_MEMTYPE_EGL_SURFACE)
			imported_mem += m->size;
		else if (egl_surface_count == 0) {
			uint64_t size = m->size;

			do_div(size, (egl_image_count ?
					egl_image_count : 1));
			imported_mem += size;
		}

		kgsl_mem_entry_put(entry);
		spin_lock(&priv->mem_lock);
	}
	spin_unlock(&priv->mem_lock);

	return scnprintf(buf, PAGE_SIZE, "%llu\n", imported_mem);
}

static ssize_t
gpumem_mapped_show(struct kgsl_process_private *priv,
				int type, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%llu\n",
			priv->gpumem_mapped);
}

static ssize_t
gpumem_unmapped_show(struct kgsl_process_private *priv, int type, char *buf)
{
	if (priv->gpumem_mapped > priv->stats[type].cur)
		return -EIO;

	return scnprintf(buf, PAGE_SIZE, "%llu\n",
			priv->stats[type].cur - priv->gpumem_mapped);
}

static struct kgsl_mem_entry_attribute debug_memstats[] = {
	__MEM_ENTRY_ATTR(0, imported_mem, imported_mem_show),
	__MEM_ENTRY_ATTR(0, gpumem_mapped, gpumem_mapped_show),
	__MEM_ENTRY_ATTR(KGSL_MEM_ENTRY_KERNEL, gpumem_unmapped,
				gpumem_unmapped_show),
};

/**
 * Show the current amount of memory allocated for the given memtype
 */

static ssize_t
mem_entry_show(struct kgsl_process_private *priv, int type, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%llu\n", priv->stats[type].cur);
}

/**
 * Show the maximum memory allocated for the given memtype through the life of
 * the process
 */

static ssize_t
mem_entry_max_show(struct kgsl_process_private *priv, int type, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%llu\n", priv->stats[type].max);
}

static ssize_t mem_entry_sysfs_show(struct kobject *kobj,
	struct attribute *attr, char *buf)
{
	struct kgsl_mem_entry_attribute *pattr = to_mem_entry_attr(attr);
	struct kgsl_process_private *priv;
	ssize_t ret;

	/*
	 * kgsl_process_init_sysfs takes a refcount to the process_private,
	 * which is put when the kobj is released. This implies that priv will
	 * not be freed until this function completes, and no further locking
	 * is needed.
	 */
	priv = kobj ? container_of(kobj, struct kgsl_process_private, kobj) :
			NULL;

	if (priv && pattr->show)
		ret = pattr->show(priv, pattr->memtype, buf);
	else
		ret = -EIO;

	return ret;
}

static void mem_entry_release(struct kobject *kobj)
{
	struct kgsl_process_private *priv;

	priv = container_of(kobj, struct kgsl_process_private, kobj);
	/* Put the refcount we got in kgsl_process_init_sysfs */
	kgsl_process_private_put(priv);
}

static const struct sysfs_ops mem_entry_sysfs_ops = {
	.show = mem_entry_sysfs_show,
};

static struct kobj_type ktype_mem_entry = {
	.sysfs_ops = &mem_entry_sysfs_ops,
	.release = &mem_entry_release,
};

static struct mem_entry_stats mem_stats[] = {
	MEM_ENTRY_STAT(KGSL_MEM_ENTRY_KERNEL, kernel),
	MEM_ENTRY_STAT(KGSL_MEM_ENTRY_USER, user),
#ifdef CONFIG_ION
	MEM_ENTRY_STAT(KGSL_MEM_ENTRY_ION, ion),
#endif
};

void
kgsl_process_uninit_sysfs(struct kgsl_process_private *private)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mem_stats); i++) {
		sysfs_remove_file(&private->kobj, &mem_stats[i].attr.attr);
		sysfs_remove_file(&private->kobj,
			&mem_stats[i].max_attr.attr);
	}

	kobject_put(&private->kobj);
}

/**
 * kgsl_process_init_sysfs() - Initialize and create sysfs files for a process
 *
 * @device: Pointer to kgsl device struct
 * @private: Pointer to the structure for the process
 *
 * kgsl_process_init_sysfs() is called at the time of creating the
 * process struct when a process opens the kgsl device for the first time.
 * This function creates the sysfs files for the process.
 */
void kgsl_process_init_sysfs(struct kgsl_device *device,
		struct kgsl_process_private *private)
{
	unsigned char name[16];
	int i;

	/* Keep private valid until the sysfs enries are removed. */
	kgsl_process_private_get(private);

	snprintf(name, sizeof(name), "%d", pid_nr(private->pid));

	if (kobject_init_and_add(&private->kobj, &ktype_mem_entry,
		kgsl_driver.prockobj, name)) {
		WARN(1, "Unable to add sysfs dir '%s'\n", name);
		return;
	}

	for (i = 0; i < ARRAY_SIZE(mem_stats); i++) {
		if (sysfs_create_file(&private->kobj,
			&mem_stats[i].attr.attr))
			WARN(1, "Couldn't create sysfs file '%s'\n",
				mem_stats[i].attr.attr.name);

		if (sysfs_create_file(&private->kobj,
			&mem_stats[i].max_attr.attr))
			WARN(1, "Couldn't create sysfs file '%s'\n",
				mem_stats[i].max_attr.attr.name);
	}

	for (i = 0; i < ARRAY_SIZE(debug_memstats); i++) {
		if (sysfs_create_file(&private->kobj,
			&debug_memstats[i].attr))
			WARN(1, "Couldn't create sysfs file '%s'\n",
				debug_memstats[i].attr.name);
	}
}

static ssize_t kgsl_drv_memstat_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	uint64_t val = 0;

	if (!strcmp(attr->attr.name, "vmalloc"))
		val = atomic_long_read(&kgsl_driver.stats.vmalloc);
	else if (!strcmp(attr->attr.name, "vmalloc_max"))
		val = atomic_long_read(&kgsl_driver.stats.vmalloc_max);
	else if (!strcmp(attr->attr.name, "page_alloc"))
		val = atomic_long_read(&kgsl_driver.stats.page_alloc);
	else if (!strcmp(attr->attr.name, "page_alloc_max"))
		val = atomic_long_read(&kgsl_driver.stats.page_alloc_max);
	else if (!strcmp(attr->attr.name, "coherent"))
		val = atomic_long_read(&kgsl_driver.stats.coherent);
	else if (!strcmp(attr->attr.name, "coherent_max"))
		val = atomic_long_read(&kgsl_driver.stats.coherent_max);
	else if (!strcmp(attr->attr.name, "secure"))
		val = atomic_long_read(&kgsl_driver.stats.secure);
	else if (!strcmp(attr->attr.name, "secure_max"))
		val = atomic_long_read(&kgsl_driver.stats.secure_max);
	else if (!strcmp(attr->attr.name, "mapped"))
		val = atomic_long_read(&kgsl_driver.stats.mapped);
	else if (!strcmp(attr->attr.name, "mapped_max"))
		val = atomic_long_read(&kgsl_driver.stats.mapped_max);

	return snprintf(buf, PAGE_SIZE, "%llu\n", val);
}

static ssize_t kgsl_drv_full_cache_threshold_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	int ret;
	unsigned int thresh = 0;

	ret = kgsl_sysfs_store(buf, &thresh);
	if (ret)
		return ret;

	kgsl_driver.full_cache_threshold = thresh;
	return count;
}

static ssize_t kgsl_drv_full_cache_threshold_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n",
			kgsl_driver.full_cache_threshold);
}

static DEVICE_ATTR(vmalloc, 0444, kgsl_drv_memstat_show, NULL);
static DEVICE_ATTR(vmalloc_max, 0444, kgsl_drv_memstat_show, NULL);
static DEVICE_ATTR(page_alloc, 0444, kgsl_drv_memstat_show, NULL);
static DEVICE_ATTR(page_alloc_max, 0444, kgsl_drv_memstat_show, NULL);
static DEVICE_ATTR(coherent, 0444, kgsl_drv_memstat_show, NULL);
static DEVICE_ATTR(coherent_max, 0444, kgsl_drv_memstat_show, NULL);
static DEVICE_ATTR(secure, 0444, kgsl_drv_memstat_show, NULL);
static DEVICE_ATTR(secure_max, 0444, kgsl_drv_memstat_show, NULL);
static DEVICE_ATTR(mapped, 0444, kgsl_drv_memstat_show, NULL);
static DEVICE_ATTR(mapped_max, 0444, kgsl_drv_memstat_show, NULL);
static DEVICE_ATTR(full_cache_threshold, 0644,
		kgsl_drv_full_cache_threshold_show,
		kgsl_drv_full_cache_threshold_store);

static const struct device_attribute *drv_attr_list[] = {
	&dev_attr_vmalloc,
	&dev_attr_vmalloc_max,
	&dev_attr_page_alloc,
	&dev_attr_page_alloc_max,
	&dev_attr_coherent,
	&dev_attr_coherent_max,
	&dev_attr_secure,
	&dev_attr_secure_max,
	&dev_attr_mapped,
	&dev_attr_mapped_max,
	&dev_attr_full_cache_threshold,
	NULL
};

void
kgsl_sharedmem_uninit_sysfs(void)
{
	kgsl_remove_device_sysfs_files(&kgsl_driver.virtdev, drv_attr_list);
}

int
kgsl_sharedmem_init_sysfs(void)
{
	return kgsl_create_device_sysfs_files(&kgsl_driver.virtdev,
		drv_attr_list);
}

static int kgsl_cma_alloc_secure(struct kgsl_device *device,
			struct kgsl_memdesc *memdesc, uint64_t size);

static int kgsl_allocate_secure(struct kgsl_device *device,
				struct kgsl_memdesc *memdesc,
				uint64_t size) {
	int ret;

	if (MMU_FEATURE(&device->mmu, KGSL_MMU_HYP_SECURE_ALLOC))
		ret = kgsl_sharedmem_page_alloc_user(memdesc, size);
	else
		ret = kgsl_cma_alloc_secure(device, memdesc, size);

	return ret;
}

int kgsl_allocate_user(struct kgsl_device *device,
		struct kgsl_memdesc *memdesc,
		uint64_t size, uint64_t flags)
{
	int ret;

	kgsl_memdesc_init(device, memdesc, flags);
	spin_lock_init(&memdesc->lock);

	if (kgsl_mmu_get_mmutype(device) == KGSL_MMU_TYPE_NONE)
		ret = kgsl_sharedmem_alloc_contig(device, memdesc, size);
	else if (flags & KGSL_MEMFLAGS_SECURE)
		ret = kgsl_allocate_secure(device, memdesc, size);
	else
		ret = kgsl_sharedmem_page_alloc_user(memdesc, size);

	return ret;
}

static int kgsl_page_alloc_vmfault(struct kgsl_memdesc *memdesc,
				struct vm_area_struct *vma,
				struct vm_fault *vmf)
{
	int pgoff;
	unsigned int offset;

	offset = ((unsigned long) vmf->virtual_address - vma->vm_start);

	if (offset >= memdesc->size)
		return VM_FAULT_SIGBUS;

	pgoff = offset >> PAGE_SHIFT;

	if (pgoff < memdesc->page_count) {
		struct page *page = memdesc->pages[pgoff];

		get_page(page);
		vmf->page = page;

		return 0;
	}

	return VM_FAULT_SIGBUS;
}

/*
 * kgsl_page_alloc_unmap_kernel() - Unmap the memory in memdesc
 *
 * @memdesc: The memory descriptor which contains information about the memory
 *
 * Unmaps the memory mapped into kernel address space
 */
static void kgsl_page_alloc_unmap_kernel(struct kgsl_memdesc *memdesc)
{
	mutex_lock(&kernel_map_global_lock);
	if (!memdesc->hostptr) {
		BUG_ON(memdesc->hostptr_count);
		goto done;
	}
	memdesc->hostptr_count--;
	if (memdesc->hostptr_count)
		goto done;
	vunmap(memdesc->hostptr);

	atomic_long_sub(memdesc->size, &kgsl_driver.stats.vmalloc);
	memdesc->hostptr = NULL;
done:
	mutex_unlock(&kernel_map_global_lock);
}

static int kgsl_lock_sgt(struct sg_table *sgt, uint64_t size)
{
	struct scatterlist *sg;
	int dest_perms = PERM_READ | PERM_WRITE;
	int source_vm = VMID_HLOS;
	int dest_vm = VMID_CP_PIXEL;
	int ret;
	int i;

	ret = hyp_assign_table(sgt, &source_vm, 1, &dest_vm, &dest_perms, 1);
	if (ret) {
		/*
		 * If returned error code is EADDRNOTAVAIL, then this
		 * memory may no longer be in a usable state as security
		 * state of the pages is unknown after this failure. This
		 * memory can neither be added back to the pool nor buddy
		 * system.
		 */
		if (ret == -EADDRNOTAVAIL)
			pr_err("Failure to lock secure GPU memory 0x%llx bytes will not be recoverable\n",
				size);

		return ret;
	}

	/* Set private bit for each sg to indicate that its secured */
	for_each_sg(sgt->sgl, sg, sgt->nents, i)
		SetPagePrivate(sg_page(sg));

	return 0;
}

static int kgsl_unlock_sgt(struct sg_table *sgt)
{
	int dest_perms = PERM_READ | PERM_WRITE | PERM_EXEC;
	int source_vm = VMID_CP_PIXEL;
	int dest_vm = VMID_HLOS;
	int ret;
	struct sg_page_iter sg_iter;

	ret = hyp_assign_table(sgt, &source_vm, 1, &dest_vm, &dest_perms, 1);

	for_each_sg_page(sgt->sgl, &sg_iter, sgt->nents, 0)
		ClearPagePrivate(sg_page_iter_page(&sg_iter));
	return ret;
}

static void kgsl_page_alloc_free(struct kgsl_memdesc *memdesc)
{
	if (memdesc->priv & KGSL_MEMDESC_MAPPED)
		return;

	kgsl_page_alloc_unmap_kernel(memdesc);
	/* we certainly do not expect the hostptr to still be mapped */
	BUG_ON(memdesc->hostptr);

	/* Secure buffers need to be unlocked before being freed */
	if (memdesc->priv & KGSL_MEMDESC_TZ_LOCKED) {
		int ret;

		ret = kgsl_unlock_sgt(memdesc->sgt);
		if (ret) {
			pr_err("Secure buf unlock failed: gpuaddr: %llx size: %llx ret: %d\n",
					memdesc->gpuaddr, memdesc->size, ret);
			BUG();
		}

		atomic_long_sub(memdesc->size, &kgsl_driver.stats.secure);
	} else {
		atomic_long_add(memdesc->size,
			&kgsl_driver.stats.page_free_pending);
	}

	/* Free pages using the pages array for non secure paged memory */
	if (memdesc->pages != NULL)
		kgsl_pool_free_pages(memdesc->pages, memdesc->page_count);
	else
		kgsl_pool_free_sgt(memdesc->sgt);

	if (!(memdesc->priv & KGSL_MEMDESC_TZ_LOCKED)) {
		atomic_long_sub(memdesc->size, &kgsl_driver.stats.page_alloc);
		atomic_long_sub(memdesc->size,
			&kgsl_driver.stats.page_free_pending);
	}
}

/*
 * kgsl_page_alloc_map_kernel - Map the memory in memdesc to kernel address
 * space
 *
 * @memdesc - The memory descriptor which contains information about the memory
 *
 * Return: 0 on success else error code
 */
static int kgsl_page_alloc_map_kernel(struct kgsl_memdesc *memdesc)
{
	int ret = 0;

	/* Sanity check - don't map more than we could possibly chew */
	if (memdesc->size > ULONG_MAX)
		return -ENOMEM;

	mutex_lock(&kernel_map_global_lock);
	if ((!memdesc->hostptr) && (memdesc->pages != NULL)) {
		pgprot_t page_prot = pgprot_writecombine(PAGE_KERNEL);

		memdesc->hostptr = vmap(memdesc->pages, memdesc->page_count,
					VM_IOREMAP, page_prot);
		if (memdesc->hostptr)
			KGSL_STATS_ADD(memdesc->size,
				&kgsl_driver.stats.vmalloc,
				&kgsl_driver.stats.vmalloc_max);
		else
			ret = -ENOMEM;
	}
	if (memdesc->hostptr)
		memdesc->hostptr_count++;

	mutex_unlock(&kernel_map_global_lock);

	return ret;
}

static int kgsl_contiguous_vmfault(struct kgsl_memdesc *memdesc,
				struct vm_area_struct *vma,
				struct vm_fault *vmf)
{
	unsigned long offset, pfn;
	int ret;

	offset = ((unsigned long) vmf->virtual_address - vma->vm_start) >>
		PAGE_SHIFT;

	pfn = (memdesc->physaddr >> PAGE_SHIFT) + offset;
	ret = vm_insert_pfn(vma, (unsigned long) vmf->virtual_address, pfn);

	if (ret == -ENOMEM || ret == -EAGAIN)
		return VM_FAULT_OOM;
	else if (ret == -EFAULT)
		return VM_FAULT_SIGBUS;

	return VM_FAULT_NOPAGE;
}

static void kgsl_cma_coherent_free(struct kgsl_memdesc *memdesc)
{
	struct dma_attrs *attrs = NULL;

	if (memdesc->priv & KGSL_MEMDESC_MAPPED)
		return;

	if (memdesc->hostptr) {
		if (memdesc->priv & KGSL_MEMDESC_SECURE) {
			atomic_long_sub(memdesc->size,
				&kgsl_driver.stats.secure);

			kgsl_cma_unlock_secure(memdesc);
			attrs = &memdesc->attrs;
		} else
			atomic_long_sub(memdesc->size,
				&kgsl_driver.stats.coherent);

		dma_free_attrs(memdesc->dev, (size_t) memdesc->size,
			memdesc->hostptr, memdesc->physaddr, attrs);
	}
}

/* Global */
static struct kgsl_memdesc_ops kgsl_page_alloc_ops = {
	.free = kgsl_page_alloc_free,
	.vmflags = VM_DONTDUMP | VM_DONTEXPAND | VM_DONTCOPY,
	.vmfault = kgsl_page_alloc_vmfault,
	.map_kernel = kgsl_page_alloc_map_kernel,
	.unmap_kernel = kgsl_page_alloc_unmap_kernel,
};

/* CMA ops - used during NOMMU mode */
static struct kgsl_memdesc_ops kgsl_cma_ops = {
	.free = kgsl_cma_coherent_free,
	.vmflags = VM_DONTDUMP | VM_PFNMAP | VM_DONTEXPAND | VM_DONTCOPY,
	.vmfault = kgsl_contiguous_vmfault,
};

#ifdef CONFIG_ARM64
/*
 * For security reasons, ARMv8 doesn't allow invalidate only on read-only
 * mapping. It would be performance prohibitive to read the permissions on
 * the buffer before the operation. Every use case that we have found does not
 * assume that an invalidate operation is invalidate only, so we feel
 * comfortable turning invalidates into flushes for these targets
 */
static inline unsigned int _fixup_cache_range_op(unsigned int op)
{
	if (op == KGSL_CACHE_OP_INV)
		return KGSL_CACHE_OP_FLUSH;
	return op;
}
#else
static inline unsigned int _fixup_cache_range_op(unsigned int op)
{
	return op;
}
#endif

static int kgsl_do_cache_op(struct page *page, void *addr,
		uint64_t offset, uint64_t size, unsigned int op)
{
	void (*cache_op)(const void *, const void *);

	/*
	 * The dmac_xxx_range functions handle addresses and sizes that
	 * are not aligned to the cacheline size correctly.
	 */
	switch (_fixup_cache_range_op(op)) {
	case KGSL_CACHE_OP_FLUSH:
		cache_op = dmac_flush_range;
		break;
	case KGSL_CACHE_OP_CLEAN:
		cache_op = dmac_clean_range;
		break;
	case KGSL_CACHE_OP_INV:
		cache_op = dmac_inv_range;
		break;
	default:
		return -EINVAL;
	}

	if (page != NULL) {
		unsigned long pfn = page_to_pfn(page) + offset / PAGE_SIZE;
		/*
		 *  page_address() returns the kernel virtual address of page.
		 *  For high memory kernel virtual address exists only if page
		 *  has been mapped. So use a version of kmap rather than
		 *  page_address() for high memory.
		 */
		if (PageHighMem(page)) {
			offset &= ~PAGE_MASK;

			do {
				unsigned int len = size;

				if (len + offset > PAGE_SIZE)
					len = PAGE_SIZE - offset;

				page = pfn_to_page(pfn++);
				addr = kmap_atomic(page);
				cache_op(addr + offset, addr + offset + len);
				kunmap_atomic(addr);

				size -= len;
				offset = 0;
			} while (size);

			return 0;
		}

		addr = page_address(page);
	}

	cache_op(addr + offset, addr + offset + (size_t) size);
	return 0;
}

int kgsl_cache_range_op(struct kgsl_memdesc *memdesc, uint64_t offset,
		uint64_t size, unsigned int op)
{
	void *addr = NULL;
	struct sg_table *sgt = NULL;
	struct scatterlist *sg;
	unsigned int i, pos = 0;
	int ret = 0;

	if (size == 0 || size > UINT_MAX)
		return -EINVAL;

	/* Make sure that the offset + size does not overflow */
	if ((offset + size < offset) || (offset + size < size))
		return -ERANGE;

	/* Check that offset+length does not exceed memdesc->size */
	if (offset + size > memdesc->size)
		return -ERANGE;

	if (memdesc->hostptr) {
		addr = memdesc->hostptr;
		/* Make sure the offset + size do not overflow the address */
		if (addr + ((size_t) offset + (size_t) size) < addr)
			return -ERANGE;

		ret = kgsl_do_cache_op(NULL, addr, offset, size, op);
		return ret;
	}

	/*
	 * If the buffer is not to mapped to kernel, perform cache
	 * operations after mapping to kernel.
	 */
	if (memdesc->sgt != NULL)
		sgt = memdesc->sgt;
	else {
		if (memdesc->pages == NULL)
			return ret;

		sgt = kgsl_alloc_sgt_from_pages(memdesc);
		if (IS_ERR(sgt))
			return PTR_ERR(sgt);
	}

	for_each_sg(sgt->sgl, sg, sgt->nents, i) {
		uint64_t sg_offset, sg_left;

		if (offset >= (pos + sg->length)) {
			pos += sg->length;
			continue;
		}
		sg_offset = offset > pos ? offset - pos : 0;
		sg_left = (sg->length - sg_offset > size) ? size :
					sg->length - sg_offset;
		ret = kgsl_do_cache_op(sg_page(sg), NULL, sg_offset,
							sg_left, op);
		size -= sg_left;
		if (size == 0)
			break;
		pos += sg->length;
	}

	if (memdesc->sgt == NULL)
		kgsl_free_sgt(sgt);

	return ret;
}
EXPORT_SYMBOL(kgsl_cache_range_op);

void kgsl_memdesc_init(struct kgsl_device *device,
			struct kgsl_memdesc *memdesc, uint64_t flags)
{
	struct kgsl_mmu *mmu = &device->mmu;
	unsigned int align;

	memset(memdesc, 0, sizeof(*memdesc));
	/* Turn off SVM if the system doesn't support it */
	if (!kgsl_mmu_use_cpu_map(mmu))
		flags &= ~((uint64_t) KGSL_MEMFLAGS_USE_CPU_MAP);

	/* Secure memory disables advanced addressing modes */
	if (flags & KGSL_MEMFLAGS_SECURE)
		flags &= ~((uint64_t) KGSL_MEMFLAGS_USE_CPU_MAP);

	if (MMU_FEATURE(mmu, KGSL_MMU_NEED_GUARD_PAGE))
		memdesc->priv |= KGSL_MEMDESC_GUARD_PAGE;

	if (flags & KGSL_MEMFLAGS_SECURE)
		memdesc->priv |= KGSL_MEMDESC_SECURE;

	memdesc->flags = flags;
	memdesc->pad_to = mmu->va_padding;
	memdesc->dev = device->dev->parent;

	align = max_t(unsigned int,
		(memdesc->flags & KGSL_MEMALIGN_MASK) >> KGSL_MEMALIGN_SHIFT,
		ilog2(PAGE_SIZE));
	kgsl_memdesc_set_align(memdesc, align);
}

int
kgsl_sharedmem_page_alloc_user(struct kgsl_memdesc *memdesc,
			uint64_t size)
{
	int ret = 0;
	unsigned int j, page_size, len_alloc;
	unsigned int pcount = 0;
	size_t len;
	unsigned int align;

	static DEFINE_RATELIMIT_STATE(_rs,
					DEFAULT_RATELIMIT_INTERVAL,
					DEFAULT_RATELIMIT_BURST);

	size = PAGE_ALIGN(size);
	if (size == 0 || size > UINT_MAX)
		return -EINVAL;

	align = (memdesc->flags & KGSL_MEMALIGN_MASK) >> KGSL_MEMALIGN_SHIFT;

	/*
	 * As 1MB is the max supported page size, use the alignment
	 * corresponding to 1MB page to make sure higher order pages
	 * are used if possible for a given memory size. Also, we
	 * don't need to update alignment in memdesc flags in case
	 * higher order page is used, as memdesc flags represent the
	 * virtual alignment specified by the user which is anyways
	 * getting satisfied.
	 */
	if (align < ilog2(SZ_1M))
		align = ilog2(SZ_1M);

	page_size = kgsl_get_page_size(size, align);

	/*
	 * The alignment cannot be less than the intended page size - it can be
	 * larger however to accomodate hardware quirks
	 */

	if (align < ilog2(page_size)) {
		kgsl_memdesc_set_align(memdesc, ilog2(page_size));
		align = ilog2(page_size);
	}

	/*
	 * There needs to be enough room in the page array to be able to
	 * service the allocation entirely with PAGE_SIZE sized chunks
	 */

	len_alloc = PAGE_ALIGN(size) >> PAGE_SHIFT;

	memdesc->ops = &kgsl_page_alloc_ops;

	/*
	 * Allocate space to store the list of pages. This is an array of
	 * pointers so we can track 1024 pages per page of allocation.
	 * Keep this array around for non global non secure buffers that
	 * are allocated by kgsl. This helps with improving the vm fault
	 * routine by finding the faulted page in constant time.
	 */
	if (!(memdesc->flags & KGSL_MEMFLAGS_SECURE))
		atomic_long_add(size, &kgsl_driver.stats.page_alloc_pending);

	memdesc->pages = kgsl_malloc(len_alloc * sizeof(struct page *));

	if (memdesc->pages == NULL) {
		ret = -ENOMEM;
		goto done;
	}

	len = size;

	while (len > 0) {
		int page_count;

		page_count = kgsl_pool_alloc_page(&page_size,
					memdesc->pages + pcount,
					len_alloc - pcount,
					&align);
		if (page_count <= 0) {
			if (page_count == -EAGAIN)
				continue;

			/*
			 * Update sglen and memdesc size,as requested allocation
			 * not served fully. So that they can be correctly freed
			 * in kgsl_sharedmem_free().
			 */
			memdesc->size = (size - len);

			if (sharedmem_noretry_flag != true &&
					__ratelimit(&_rs))
				KGSL_CORE_ERR(
					"Out of memory: only allocated %lldKB of %lldKB requested\n",
					(size - len) >> 10, size >> 10);

			ret = -ENOMEM;
			goto done;
		}

		pcount += page_count;
		len -= page_size;
		memdesc->size += page_size;
		memdesc->page_count += page_count;

		/* Get the needed page size for the next iteration */
		page_size = kgsl_get_page_size(len, align);
	}

	/* Call to the hypervisor to lock any secure buffer allocations */
	if (memdesc->flags & KGSL_MEMFLAGS_SECURE) {
		memdesc->sgt = kmalloc(sizeof(struct sg_table), GFP_KERNEL);
		if (memdesc->sgt == NULL) {
			ret = -ENOMEM;
			goto done;
		}

		ret = sg_alloc_table_from_pages(memdesc->sgt, memdesc->pages,
			memdesc->page_count, 0, memdesc->size, GFP_KERNEL);
		if (ret) {
			kfree(memdesc->sgt);
			goto done;
		}

		ret = kgsl_lock_sgt(memdesc->sgt, memdesc->size);
		if (ret) {
			sg_free_table(memdesc->sgt);
			kfree(memdesc->sgt);
			memdesc->sgt = NULL;

			if (ret == -EADDRNOTAVAIL) {
				kgsl_free(memdesc->pages);
				memset(memdesc, 0, sizeof(*memdesc));
				return ret;
			}

			goto done;
		}

		memdesc->priv |= KGSL_MEMDESC_TZ_LOCKED;

		/* Record statistics */
		KGSL_STATS_ADD(memdesc->size, &kgsl_driver.stats.secure,
			&kgsl_driver.stats.secure_max);

		/*
		 * We don't need the array for secure buffers because they are
		 * not mapped to CPU
		 */
		kgsl_free(memdesc->pages);
		memdesc->pages = NULL;
		memdesc->page_count = 0;

		/* Don't map and zero the locked secure buffer */
		goto done;
	}

	KGSL_STATS_ADD(memdesc->size, &kgsl_driver.stats.page_alloc,
		&kgsl_driver.stats.page_alloc_max);

done:
	if (!(memdesc->flags & KGSL_MEMFLAGS_SECURE))
		atomic_long_sub(size, &kgsl_driver.stats.page_alloc_pending);

	if (ret) {
		if (memdesc->pages) {
			unsigned int count = 1;

			for (j = 0; j < pcount; j += count) {
				count = 1 << compound_order(memdesc->pages[j]);
				kgsl_pool_free_page(memdesc->pages[j]);
			}
		}

		kgsl_free(memdesc->pages);
		memset(memdesc, 0, sizeof(*memdesc));
	}

	return ret;
}

void kgsl_sharedmem_free(struct kgsl_memdesc *memdesc)
{
	if (memdesc == NULL || memdesc->size == 0)
		return;

	/* Make sure the memory object has been unmapped */
	kgsl_mmu_put_gpuaddr(memdesc);

	if (memdesc->ops && memdesc->ops->free)
		memdesc->ops->free(memdesc);

	if (memdesc->sgt) {
		sg_free_table(memdesc->sgt);
		kfree(memdesc->sgt);
		memdesc->sgt = NULL;
	}

	if (memdesc->pages)
		kgsl_free(memdesc->pages);
}
EXPORT_SYMBOL(kgsl_sharedmem_free);

void kgsl_free_secure_page(struct page *page)
{
	struct sg_table sgt;
	struct scatterlist sgl;

	if (!page)
		return;

	sgt.sgl = &sgl;
	sgt.nents = 1;
	sgt.orig_nents = 1;
	sg_init_table(&sgl, 1);
	sg_set_page(&sgl, page, PAGE_SIZE, 0);

	kgsl_unlock_sgt(&sgt);
	__free_page(page);
}

struct page *kgsl_alloc_secure_page(void)
{
	struct page *page;
	struct sg_table sgt;
	struct scatterlist sgl;
	int status;

	page = alloc_page(GFP_KERNEL | __GFP_ZERO |
			__GFP_NORETRY | __GFP_HIGHMEM);
	if (!page)
		return NULL;

	sgt.sgl = &sgl;
	sgt.nents = 1;
	sgt.orig_nents = 1;
	sg_init_table(&sgl, 1);
	sg_set_page(&sgl, page, PAGE_SIZE, 0);

	status = kgsl_lock_sgt(&sgt, PAGE_SIZE);
	if (status) {
		if (status == -EADDRNOTAVAIL)
			return NULL;

		__free_page(page);
		return NULL;
	}
	return page;
}

int
kgsl_sharedmem_readl(const struct kgsl_memdesc *memdesc,
			uint32_t *dst,
			uint64_t offsetbytes)
{
	uint32_t *src;
	BUG_ON(memdesc == NULL || memdesc->hostptr == NULL || dst == NULL);
	WARN_ON(offsetbytes % sizeof(uint32_t) != 0);
	if (offsetbytes % sizeof(uint32_t) != 0)
		return -EINVAL;

	WARN_ON(offsetbytes > (memdesc->size - sizeof(uint32_t)));
	if (offsetbytes > (memdesc->size - sizeof(uint32_t)))
		return -ERANGE;

	rmb();
	src = (uint32_t *)(memdesc->hostptr + offsetbytes);
	*dst = *src;
	return 0;
}
EXPORT_SYMBOL(kgsl_sharedmem_readl);

int
kgsl_sharedmem_writel(struct kgsl_device *device,
			const struct kgsl_memdesc *memdesc,
			uint64_t offsetbytes,
			uint32_t src)
{
	uint32_t *dst;
	BUG_ON(memdesc == NULL || memdesc->hostptr == NULL);
	WARN_ON(offsetbytes % sizeof(uint32_t) != 0);
	if (offsetbytes % sizeof(uint32_t) != 0)
		return -EINVAL;

	WARN_ON(offsetbytes > (memdesc->size - sizeof(uint32_t)));
	if (offsetbytes > (memdesc->size - sizeof(uint32_t)))
		return -ERANGE;
	kgsl_cffdump_write(device,
		memdesc->gpuaddr + offsetbytes,
		src);
	dst = (uint32_t *)(memdesc->hostptr + offsetbytes);
	*dst = src;

	wmb();

	return 0;
}
EXPORT_SYMBOL(kgsl_sharedmem_writel);

int
kgsl_sharedmem_readq(const struct kgsl_memdesc *memdesc,
			uint64_t *dst,
			uint64_t offsetbytes)
{
	uint64_t *src;
	BUG_ON(memdesc == NULL || memdesc->hostptr == NULL || dst == NULL);
	WARN_ON(offsetbytes % sizeof(uint32_t) != 0);
	if (offsetbytes % sizeof(uint32_t) != 0)
		return -EINVAL;

	WARN_ON(offsetbytes > (memdesc->size - sizeof(uint32_t)));
	if (offsetbytes > (memdesc->size - sizeof(uint32_t)))
		return -ERANGE;

	/*
	 * We are reading shared memory between CPU and GPU.
	 * Make sure reads before this are complete
	 */
	rmb();
	src = (uint64_t *)(memdesc->hostptr + offsetbytes);
	*dst = *src;
	return 0;
}
EXPORT_SYMBOL(kgsl_sharedmem_readq);

int
kgsl_sharedmem_writeq(struct kgsl_device *device,
			const struct kgsl_memdesc *memdesc,
			uint64_t offsetbytes,
			uint64_t src)
{
	uint64_t *dst;
	BUG_ON(memdesc == NULL || memdesc->hostptr == NULL);
	WARN_ON(offsetbytes % sizeof(uint32_t) != 0);
	if (offsetbytes % sizeof(uint32_t) != 0)
		return -EINVAL;

	WARN_ON(offsetbytes > (memdesc->size - sizeof(uint32_t)));
	if (offsetbytes > (memdesc->size - sizeof(uint32_t)))
		return -ERANGE;
	kgsl_cffdump_write(device,
		lower_32_bits(memdesc->gpuaddr + offsetbytes), src);
	kgsl_cffdump_write(device,
		upper_32_bits(memdesc->gpuaddr + offsetbytes), src);
	dst = (uint64_t *)(memdesc->hostptr + offsetbytes);
	*dst = src;

	/*
	 * We are writing to shared memory between CPU and GPU.
	 * Make sure write above is posted immediately
	 */
	wmb();

	return 0;
}
EXPORT_SYMBOL(kgsl_sharedmem_writeq);

int
kgsl_sharedmem_set(struct kgsl_device *device,
		const struct kgsl_memdesc *memdesc, uint64_t offsetbytes,
		unsigned int value, uint64_t sizebytes)
{
	BUG_ON(memdesc == NULL || memdesc->hostptr == NULL);
	BUG_ON(offsetbytes + sizebytes > memdesc->size);

	kgsl_cffdump_memset(device,
		memdesc->gpuaddr + offsetbytes, value,
		sizebytes);
	memset(memdesc->hostptr + offsetbytes, value, sizebytes);
	return 0;
}
EXPORT_SYMBOL(kgsl_sharedmem_set);

static const char * const memtype_str[] = {
	[KGSL_MEMTYPE_OBJECTANY] = "any(0)",
	[KGSL_MEMTYPE_FRAMEBUFFER] = "framebuffer",
	[KGSL_MEMTYPE_RENDERBUFFER] = "renderbuffer",
	[KGSL_MEMTYPE_ARRAYBUFFER] = "arraybuffer",
	[KGSL_MEMTYPE_ELEMENTARRAYBUFFER] = "elementarraybuffer",
	[KGSL_MEMTYPE_VERTEXARRAYBUFFER] = "vertexarraybuffer",
	[KGSL_MEMTYPE_TEXTURE] = "texture",
	[KGSL_MEMTYPE_SURFACE] = "surface",
	[KGSL_MEMTYPE_EGL_SURFACE] = "egl_surface",
	[KGSL_MEMTYPE_GL] = "gl",
	[KGSL_MEMTYPE_CL] = "cl",
	[KGSL_MEMTYPE_CL_BUFFER_MAP] = "cl_buffer_map",
	[KGSL_MEMTYPE_CL_BUFFER_NOMAP] = "cl_buffer_nomap",
	[KGSL_MEMTYPE_CL_IMAGE_MAP] = "cl_image_map",
	[KGSL_MEMTYPE_CL_IMAGE_NOMAP] = "cl_image_nomap",
	[KGSL_MEMTYPE_CL_KERNEL_STACK] = "cl_kernel_stack",
	[KGSL_MEMTYPE_COMMAND] = "command",
	[KGSL_MEMTYPE_2D] = "2d",
	[KGSL_MEMTYPE_EGL_IMAGE] = "egl_image",
	[KGSL_MEMTYPE_EGL_SHADOW] = "egl_shadow",
	[KGSL_MEMTYPE_MULTISAMPLE] = "egl_multisample",
	/* KGSL_MEMTYPE_KERNEL handled below, to avoid huge array */
};

void kgsl_get_memory_usage(char *name, size_t name_size, uint64_t memflags)
{
	unsigned int type = MEMFLAGS(memflags, KGSL_MEMTYPE_MASK,
		KGSL_MEMTYPE_SHIFT);

	if (type == KGSL_MEMTYPE_KERNEL)
		strlcpy(name, "kernel", name_size);
	else if (type < ARRAY_SIZE(memtype_str) && memtype_str[type] != NULL)
		strlcpy(name, memtype_str[type], name_size);
	else
		snprintf(name, name_size, "VK/others(%3d)", type);
}
EXPORT_SYMBOL(kgsl_get_memory_usage);

int kgsl_sharedmem_alloc_contig(struct kgsl_device *device,
			struct kgsl_memdesc *memdesc, uint64_t size)
{
	int result = 0;

	size = PAGE_ALIGN(size);
	if (size == 0 || size > SIZE_MAX)
		return -EINVAL;

	memdesc->size = size;
	memdesc->ops = &kgsl_cma_ops;
	memdesc->dev = device->dev->parent;

	memdesc->hostptr = dma_alloc_attrs(memdesc->dev, (size_t) size,
		&memdesc->physaddr, GFP_KERNEL, NULL);

	if (memdesc->hostptr == NULL) {
		result = -ENOMEM;
		goto err;
	}

	result = memdesc_sg_dma(memdesc, memdesc->physaddr, size);
	if (result)
		goto err;

	/* Record statistics */

	if (kgsl_mmu_get_mmutype(device) == KGSL_MMU_TYPE_NONE)
		memdesc->gpuaddr = memdesc->physaddr;

	KGSL_STATS_ADD(size, &kgsl_driver.stats.coherent,
		&kgsl_driver.stats.coherent_max);

err:
	if (result)
		kgsl_sharedmem_free(memdesc);

	return result;
}
EXPORT_SYMBOL(kgsl_sharedmem_alloc_contig);

static int scm_lock_chunk(struct kgsl_memdesc *memdesc, int lock)
{
	struct cp2_lock_req request;
	unsigned int resp;
	unsigned int *chunk_list;
	struct scm_desc desc = {0};
	int result;

	/*
	 * Flush the virt addr range before sending the memory to the
	 * secure environment to ensure the data is actually present
	 * in RAM
	 *
	 * Chunk_list holds the physical address of secure memory.
	 * Pass in the virtual address of chunk_list to flush.
	 * Chunk_list size is 1 because secure memory is physically
	 * contiguous.
	 */
	chunk_list = kzalloc(sizeof(unsigned int), GFP_KERNEL);
	if (!chunk_list)
		return -ENOMEM;

	chunk_list[0] = memdesc->physaddr;
	dmac_flush_range((void *)chunk_list, (void *)chunk_list + 1);

	request.chunks.chunk_list = virt_to_phys(chunk_list);
	/*
	 * virt_to_phys(chunk_list) may be an address > 4GB. It is guaranteed
	 * that when using scm_call (the older interface), the phys addresses
	 * will be restricted to below 4GB.
	 */
	desc.args[0] = virt_to_phys(chunk_list);
	desc.args[1] = request.chunks.chunk_list_size = 1;
	desc.args[2] = request.chunks.chunk_size = (unsigned int) memdesc->size;
	desc.args[3] = request.mem_usage = 0;
	desc.args[4] = request.lock = lock;
	desc.args[5] = 0;
	desc.arginfo = SCM_ARGS(6, SCM_RW, SCM_VAL, SCM_VAL, SCM_VAL, SCM_VAL,
				SCM_VAL);
	kmap_flush_unused();
	kmap_atomic_flush_unused();
	if (!is_scm_armv8()) {
		result = scm_call(SCM_SVC_MP, MEM_PROTECT_LOCK_ID2,
				&request, sizeof(request), &resp, sizeof(resp));
	} else {
		result = scm_call2(SCM_SIP_FNID(SCM_SVC_MP,
				   MEM_PROTECT_LOCK_ID2_FLAT), &desc);
		resp = desc.ret[0];
	}

	kfree(chunk_list);
	return result;
}

static int kgsl_cma_alloc_secure(struct kgsl_device *device,
			struct kgsl_memdesc *memdesc, uint64_t size)
{
	struct kgsl_iommu *iommu = KGSL_IOMMU_PRIV(device);
	int result = 0;
	size_t aligned;

	/* Align size to 1M boundaries */
	aligned = ALIGN(size, SZ_1M);

	/* The SCM call uses an unsigned int for the size */
	if (aligned == 0 || aligned > UINT_MAX)
		return -EINVAL;

	/*
	 * If there is more than a page gap between the requested size and the
	 * aligned size we don't need to add more memory for a guard page. Yay!
	 */

	if (memdesc->priv & KGSL_MEMDESC_GUARD_PAGE)
		if (aligned - size >= SZ_4K)
			memdesc->priv &= ~KGSL_MEMDESC_GUARD_PAGE;

	memdesc->size = aligned;
	memdesc->ops = &kgsl_cma_ops;
	memdesc->dev = iommu->ctx[KGSL_IOMMU_CONTEXT_SECURE].dev;

	init_dma_attrs(&memdesc->attrs);
	dma_set_attr(DMA_ATTR_STRONGLY_ORDERED, &memdesc->attrs);

	memdesc->hostptr = dma_alloc_attrs(memdesc->dev, aligned,
		&memdesc->physaddr, GFP_KERNEL, &memdesc->attrs);

	if (memdesc->hostptr == NULL) {
		result = -ENOMEM;
		goto err;
	}

	result = memdesc_sg_dma(memdesc, memdesc->physaddr, aligned);
	if (result)
		goto err;

	result = scm_lock_chunk(memdesc, 1);

	if (result != 0)
		goto err;

	/* Set the private bit to indicate that we've secured this */
	SetPagePrivate(sg_page(memdesc->sgt->sgl));

	memdesc->priv |= KGSL_MEMDESC_TZ_LOCKED;

	/* Record statistics */
	KGSL_STATS_ADD(aligned, &kgsl_driver.stats.secure,
	       &kgsl_driver.stats.secure_max);
err:
	if (result)
		kgsl_sharedmem_free(memdesc);

	return result;
}

/**
 * kgsl_cma_unlock_secure() - Unlock secure memory by calling TZ
 * @memdesc: memory descriptor
 */
static void kgsl_cma_unlock_secure(struct kgsl_memdesc *memdesc)
{
	if (memdesc->size == 0 || !(memdesc->priv & KGSL_MEMDESC_TZ_LOCKED))
		return;

	if (!scm_lock_chunk(memdesc, 0))
		ClearPagePrivate(sg_page(memdesc->sgt->sgl));
}

void kgsl_sharedmem_set_noretry(bool val)
{
	sharedmem_noretry_flag = val;
}

bool kgsl_sharedmem_get_noretry(void)
{
	return sharedmem_noretry_flag;
}
