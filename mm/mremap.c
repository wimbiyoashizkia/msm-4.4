/*
 *	mm/mremap.c
 *
 *	(C) Copyright 1996 Linus Torvalds
 *
 *	Address space accounting code	<alan@lxorguk.ukuu.org.uk>
 *	(C) Copyright 2002 Red Hat Inc, All Rights Reserved
 */

#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/shm.h>
#include <linux/ksm.h>
#include <linux/mman.h>
#include <linux/swap.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/swapops.h>
#include <linux/highmem.h>
#include <linux/security.h>
#include <linux/syscalls.h>
#include <linux/mmu_notifier.h>
#include <linux/sched/sysctl.h>
#include <linux/uaccess.h>
#include <linux/mm-arch-hooks.h>

#include <asm/cacheflush.h>
#include <asm/tlbflush.h>

#include "internal.h"

static pmd_t *get_old_pmd(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;

	pgd = pgd_offset(mm, addr);
	if (pgd_none_or_clear_bad(pgd))
		return NULL;

	pud = pud_offset(pgd, addr);
	if (pud_none_or_clear_bad(pud))
		return NULL;

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd))
		return NULL;

	return pmd;
}

static pmd_t *alloc_new_pmd(struct mm_struct *mm, struct vm_area_struct *vma,
			    unsigned long addr)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;

	pgd = pgd_offset(mm, addr);
	pud = pud_alloc(mm, pgd, addr);
	if (!pud)
		return NULL;

	pmd = pmd_alloc(mm, pud, addr);
	if (!pmd)
		return NULL;

	VM_BUG_ON(pmd_trans_huge(*pmd));

	return pmd;
}

static pte_t move_soft_dirty_pte(pte_t pte)
{
	/*
	 * Set soft dirty bit so we can notice
	 * in userspace the ptes were moved.
	 */
#ifdef CONFIG_MEM_SOFT_DIRTY
	if (pte_present(pte))
		pte = pte_mksoft_dirty(pte);
	else if (is_swap_pte(pte))
		pte = pte_swp_mksoft_dirty(pte);
#endif
	return pte;
}

static void move_ptes(struct vm_area_struct *vma, pmd_t *old_pmd,
		unsigned long old_addr, unsigned long old_end,
		struct vm_area_struct *new_vma, pmd_t *new_pmd,
		unsigned long new_addr, bool need_rmap_locks)
{
	struct address_space *mapping = NULL;
	struct anon_vma *anon_vma = NULL;
	struct mm_struct *mm = vma->vm_mm;
	pte_t *old_pte, *new_pte, pte;
	spinlock_t *old_ptl, *new_ptl;
	bool force_flush = false;
	unsigned long len = old_end - old_addr;

	/*
	 * When need_rmap_locks is true, we take the i_mmap_rwsem and anon_vma
	 * locks to ensure that rmap will always observe either the old or the
	 * new ptes. This is the easiest way to avoid races with
	 * truncate_pagecache(), page migration, etc...
	 *
	 * When need_rmap_locks is false, we use other ways to avoid
	 * such races:
	 *
	 * - During exec() shift_arg_pages(), we use a specially tagged vma
	 *   which rmap call sites look for using is_vma_temporary_stack().
	 *
	 * - During mremap(), new_vma is often known to be placed after vma
	 *   in rmap traversal order. This ensures rmap will always observe
	 *   either the old pte, or the new pte, or both (the page table locks
	 *   serialize access to individual ptes, but only rmap traversal
	 *   order guarantees that we won't miss both the old and new ptes).
	 */
	if (need_rmap_locks) {
		if (vma->vm_file) {
			mapping = vma->vm_file->f_mapping;
			i_mmap_lock_write(mapping);
		}
		if (vma->anon_vma) {
			anon_vma = vma->anon_vma;
			anon_vma_lock_write(anon_vma);
		}
	}

	/*
	 * We don't have to worry about the ordering of src and dst
	 * pte locks because exclusive mmap_sem prevents deadlock.
	 */
	old_pte = pte_offset_map_lock(mm, old_pmd, old_addr, &old_ptl);
	new_pte = pte_offset_map(new_pmd, new_addr);
	new_ptl = pte_lockptr(mm, new_pmd);
	if (new_ptl != old_ptl)
		spin_lock_nested(new_ptl, SINGLE_DEPTH_NESTING);
	flush_tlb_batched_pending(vma->vm_mm);
	arch_enter_lazy_mmu_mode();

	for (; old_addr < old_end; old_pte++, old_addr += PAGE_SIZE,
				   new_pte++, new_addr += PAGE_SIZE) {
		if (pte_none(*old_pte))
			continue;
		pte = ptep_get_and_clear(mm, old_addr, old_pte);
		/*
		 * If we are remapping a valid PTE, make sure
		 * to flush TLB before we drop the PTL for the PTE.
		 *
		 * NOTE! Both old and new PTL matter: the old one
		 * for racing with page_mkclean(), the new one to
		 * make sure the physical page stays valid until
		 * the TLB entry for the old mapping has been
		 * flushed.
		 */
		if (pte_present(pte))
			force_flush = true;
		pte = move_pte(pte, new_vma->vm_page_prot, old_addr, new_addr);
		pte = move_soft_dirty_pte(pte);
		set_pte_at(mm, new_addr, new_pte, pte);
	}

	arch_leave_lazy_mmu_mode();
	if (force_flush)
		flush_tlb_range(vma, old_end - len, old_end);
	if (new_ptl != old_ptl)
		spin_unlock(new_ptl);
	pte_unmap(new_pte - 1);
	pte_unmap_unlock(old_pte - 1, old_ptl);
	if (anon_vma)
		anon_vma_unlock_write(anon_vma);
	if (mapping)
		i_mmap_unlock_write(mapping);
}

#ifdef CONFIG_HAVE_MOVE_PMD
static void move_normal_pmd(struct vm_area_struct *vma, struct vm_area_struct *new_vma,
		  unsigned long old_addr,
		  unsigned long new_addr, unsigned long old_end,
		  pmd_t *old_pmd, pmd_t *new_pmd)
{
	spinlock_t *old_ptl, *new_ptl;
	pmd_t pmd;
	struct mm_struct *mm = vma->vm_mm;

	if ((old_addr & PMD_MASK) || (new_addr & PMD_MASK)
	    || old_end - old_addr < PMD_SIZE ||
	    (new_vma->vm_flags & VM_NOHUGEPAGE))
		return;

	/*
	 * The destination pmd shouldn't be established, free_pgtables()
	 * should have released it.
	 *
	 * However, there's a case during execve() where we use mremap
	 * to move the initial stack, and in that case the target area
	 * may overlap the source area (always moving down).
	 *
	 * If everything is PMD-aligned, that works fine, as moving
	 * each pmd down will clear the source pmd. But if we first
	 * have a few 4kB-only pages that get moved down, and then
	 * hit the "now the rest is PMD-aligned, let's do everything
	 * one pmd at a time", we will still have the old (now empty
	 * of any 4kB pages, but still there) PMD in the page table
	 * tree.
	 *
	 * Warn on it once - because we really should try to figure
	 * out how to do this better - but then say "I won't move
	 * this pmd".
	 *
	 * One alternative might be to just unmap the target pmd at
	 * this point, and verify that it really is empty. We'll see.
	 */
	if (WARN_ON_ONCE(!pmd_none(*new_pmd))) {
		return;
	}

	/*
	 * We don't have to worry about the ordering of src and dst
	 * ptlocks because exclusive mmap_sem prevents deadlock.
	 */
	old_ptl = pmd_lock(vma->vm_mm, old_pmd);
	new_ptl = pmd_lockptr(mm, new_pmd);
	if (new_ptl != old_ptl)
		spin_lock_nested(new_ptl, SINGLE_DEPTH_NESTING);

	/* Clear the pmd */
	pmd = *old_pmd;
	pmd_clear(old_pmd);
	
	VM_BUG_ON(!pmd_none(*new_pmd));

	set_pmd_at(mm, new_addr, new_pmd, pmd_mksoft_dirty(pmd));
	flush_tlb_range(vma, old_addr, old_addr + PMD_SIZE);
	if (new_ptl != old_ptl)
		spin_unlock(new_ptl);
	spin_unlock(old_ptl);
	return;
}
#endif

#define LATENCY_LIMIT	(64 * PAGE_SIZE)

unsigned long move_page_tables(struct vm_area_struct *vma,
		unsigned long old_addr, struct vm_area_struct *new_vma,
		unsigned long new_addr, unsigned long len,
		bool need_rmap_locks)
{
	unsigned long extent, next, old_end;
	pmd_t *old_pmd, *new_pmd;
	unsigned long mmun_start;	/* For mmu_notifiers */
	unsigned long mmun_end;		/* For mmu_notifiers */

	old_end = old_addr + len;
	flush_cache_range(vma, old_addr, old_end);

	mmun_start = old_addr;
	mmun_end   = old_end;
	mmu_notifier_invalidate_range_start(vma->vm_mm, mmun_start, mmun_end);

	for (; old_addr < old_end; old_addr += extent, new_addr += extent) {
		cond_resched();
		next = (old_addr + PMD_SIZE) & PMD_MASK;
		/* even if next overflowed, extent below will be ok */
		extent = next - old_addr;
		if (extent > old_end - old_addr)
			extent = old_end - old_addr;
		old_pmd = get_old_pmd(vma->vm_mm, old_addr);
		if (!old_pmd)
			continue;
		new_pmd = alloc_new_pmd(vma->vm_mm, vma, new_addr);
		if (!new_pmd)
			break;
		if (pmd_trans_huge(*old_pmd)) {
			int err = 0;
			if (extent == HPAGE_PMD_SIZE) {
				VM_BUG_ON_VMA(vma->vm_file || !vma->anon_vma,
					      vma);
				/* See comment in move_ptes() */
				if (need_rmap_locks)
					anon_vma_lock_write(vma->anon_vma);
				err = move_huge_pmd(vma, new_vma, old_addr,
						    new_addr, old_end,
						    old_pmd, new_pmd);
				if (need_rmap_locks)
					anon_vma_unlock_write(vma->anon_vma);
			}
			if (err > 0) {
				continue;
			} else if (!err) {
				split_huge_page_pmd(vma, old_addr, old_pmd);
			}
			VM_BUG_ON(pmd_trans_huge(*old_pmd));
		} else if (extent == PMD_SIZE) {
#ifdef CONFIG_HAVE_MOVE_PMD
				VM_BUG_ON_VMA(vma->vm_file || !vma->anon_vma,
					      vma);
				/* See comment in move_ptes() */
				if (need_rmap_locks)
					anon_vma_lock_write(vma->anon_vma);
				move_normal_pmd(vma, new_vma, old_addr,
						    new_addr, old_end,
						    old_pmd, new_pmd);
				if (need_rmap_locks)
					anon_vma_unlock_write(vma->anon_vma);
#endif
		}

		if (pmd_none(*new_pmd) && __pte_alloc(new_vma->vm_mm, new_vma,
						      new_pmd, new_addr))
			break;
		next = (new_addr + PMD_SIZE) & PMD_MASK;
		if (extent > next - new_addr)
			extent = next - new_addr;
		if (extent > LATENCY_LIMIT)
			extent = LATENCY_LIMIT;
		move_ptes(vma, old_pmd, old_addr, old_addr + extent,
			  new_vma, new_pmd, new_addr, need_rmap_locks);
	}

	mmu_notifier_invalidate_range_end(vma->vm_mm, mmun_start, mmun_end);

	return len + old_addr - old_end;	/* how much done */
}

static unsigned long move_vma(struct vm_area_struct *vma,
		unsigned long old_addr, unsigned long old_len,
		unsigned long new_len, unsigned long new_addr, bool *locked)
{
	struct mm_struct *mm = vma->vm_mm;
	struct vm_area_struct *new_vma;
	unsigned long vm_flags = vma->vm_flags;
	unsigned long new_pgoff;
	unsigned long moved_len;
	unsigned long excess = 0;
	unsigned long hiwater_vm;
	int split = 0;
	int err;
	bool need_rmap_locks;

	/*
	 * We'd prefer to avoid failure later on in do_munmap:
	 * which may split one vma into three before unmapping.
	 */
	if (mm->map_count >= sysctl_max_map_count - 3)
		return -ENOMEM;

	/*
	 * Advise KSM to break any KSM pages in the area to be moved:
	 * it would be confusing if they were to turn up at the new
	 * location, where they happen to coincide with different KSM
	 * pages recently unmapped.  But leave vma->vm_flags as it was,
	 * so KSM can come around to merge on vma and new_vma afterwards.
	 */
	err = ksm_madvise(vma, old_addr, old_addr + old_len,
						MADV_UNMERGEABLE, &vm_flags);
	if (err)
		return err;

	new_pgoff = vma->vm_pgoff + ((old_addr - vma->vm_start) >> PAGE_SHIFT);
	new_vma = copy_vma(&vma, new_addr, new_len, new_pgoff,
			   &need_rmap_locks);
	if (!new_vma)
		return -ENOMEM;

	/* new_vma is returned protected by copy_vma, to prevent speculative
	 * page fault to be done in the destination area before we move the pte.
	 * Now, we must also protect the source VMA since we don't want pages
	 * to be mapped in our back while we are copying the PTEs.
	 */
	if (vma != new_vma)
		vm_raw_write_begin(vma);

	moved_len = move_page_tables(vma, old_addr, new_vma, new_addr, old_len,
				     need_rmap_locks);
	if (moved_len < old_len) {
		err = -ENOMEM;
	} else if (vma->vm_ops && vma->vm_ops->mremap) {
		err = vma->vm_ops->mremap(new_vma);
	}

	if (unlikely(err)) {
		/*
		 * On error, move entries back from new area to old,
		 * which will succeed since page tables still there,
		 * and then proceed to unmap new area instead of old.
		 */
		move_page_tables(new_vma, new_addr, vma, old_addr, moved_len,
				 true);
		if (vma != new_vma)
			vm_raw_write_end(vma);

		vma = new_vma;
		old_len = new_len;
		old_addr = new_addr;
		new_addr = err;
	} else {
		arch_remap(mm, old_addr, old_addr + old_len,
			   new_addr, new_addr + new_len);

		if (vma != new_vma)
			vm_raw_write_end(vma);
	}
	vm_raw_write_end(new_vma);

	/* Conceal VM_ACCOUNT so old reservation is not undone */
	if (vm_flags & VM_ACCOUNT) {
		vma->vm_flags &= ~VM_ACCOUNT;
		excess = vma->vm_end - vma->vm_start - old_len;
		if (old_addr > vma->vm_start &&
		    old_addr + old_len < vma->vm_end)
			split = 1;
	}

	/*
	 * If we failed to move page tables we still do total_vm increment
	 * since do_munmap() will decrement it by old_len == new_len.
	 *
	 * Since total_vm is about to be raised artificially high for a
	 * moment, we need to restore high watermark afterwards: if stats
	 * are taken meanwhile, total_vm and hiwater_vm appear too high.
	 * If this were a serious issue, we'd add a flag to do_munmap().
	 */
	hiwater_vm = mm->hiwater_vm;
	vm_stat_account(mm, vma->vm_flags, vma->vm_file, new_len>>PAGE_SHIFT);

	if (do_munmap(mm, old_addr, old_len) < 0) {
		/* OOM: unable to split vma, just get accounts right */
		vm_unacct_memory(excess >> PAGE_SHIFT);
		excess = 0;
	}
	mm->hiwater_vm = hiwater_vm;

	/* Restore VM_ACCOUNT if one or two pieces of vma left */
	if (excess) {
		vma->vm_flags |= VM_ACCOUNT;
		if (split)
			vma->vm_next->vm_flags |= VM_ACCOUNT;
	}

	if (vm_flags & VM_LOCKED) {
		mm->locked_vm += new_len >> PAGE_SHIFT;
		*locked = true;
	}

	return new_addr;
}

static struct vm_area_struct *vma_to_resize(unsigned long addr,
	unsigned long old_len, unsigned long new_len, unsigned long *p)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma = find_vma(mm, addr);
	unsigned long pgoff;

	if (!vma || vma->vm_start > addr)
		return ERR_PTR(-EFAULT);

	if (is_vm_hugetlb_page(vma))
		return ERR_PTR(-EINVAL);

	/* We can't remap across vm area boundaries */
	if (old_len > vma->vm_end - addr)
		return ERR_PTR(-EFAULT);

	if (new_len == old_len)
		return vma;

	/* Need to be careful about a growing mapping */
	pgoff = (addr - vma->vm_start) >> PAGE_SHIFT;
	pgoff += vma->vm_pgoff;
	if (pgoff + (new_len >> PAGE_SHIFT) < pgoff)
		return ERR_PTR(-EINVAL);

	if (vma->vm_flags & (VM_DONTEXPAND | VM_PFNMAP))
		return ERR_PTR(-EFAULT);

	if (vma->vm_flags & VM_LOCKED) {
		unsigned long locked, lock_limit;
		locked = mm->locked_vm << PAGE_SHIFT;
		lock_limit = rlimit(RLIMIT_MEMLOCK);
		locked += new_len - old_len;
		if (locked > lock_limit && !capable(CAP_IPC_LOCK))
			return ERR_PTR(-EAGAIN);
	}

	if (!may_expand_vm(mm, (new_len - old_len) >> PAGE_SHIFT))
		return ERR_PTR(-ENOMEM);

	if (vma->vm_flags & VM_ACCOUNT) {
		unsigned long charged = (new_len - old_len) >> PAGE_SHIFT;
		if (security_vm_enough_memory_mm(mm, charged))
			return ERR_PTR(-ENOMEM);
		*p = charged;
	}

	return vma;
}

static unsigned long mremap_to(unsigned long addr, unsigned long old_len,
		unsigned long new_addr, unsigned long new_len, bool *locked)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	unsigned long ret = -EINVAL;
	unsigned long charged = 0;
	unsigned long map_flags;

	if (offset_in_page(new_addr))
		goto out;

	if (new_len > TASK_SIZE || new_addr > TASK_SIZE - new_len)
		goto out;

	/* Ensure the old/new locations do not overlap */
	if (addr + old_len > new_addr && new_addr + new_len > addr)
		goto out;

	ret = do_munmap(mm, new_addr, new_len);
	if (ret)
		goto out;

	if (old_len >= new_len) {
		ret = do_munmap(mm, addr+new_len, old_len - new_len);
		if (ret && old_len != new_len)
			goto out;
		old_len = new_len;
	}

	vma = vma_to_resize(addr, old_len, new_len, &charged);
	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto out;
	}

	map_flags = MAP_FIXED;
	if (vma->vm_flags & VM_MAYSHARE)
		map_flags |= MAP_SHARED;

	ret = get_unmapped_area(vma->vm_file, new_addr, new_len, vma->vm_pgoff +
				((addr - vma->vm_start) >> PAGE_SHIFT),
				map_flags);
	if (offset_in_page(ret))
		goto out1;

	ret = move_vma(vma, addr, old_len, new_len, new_addr, locked);
	if (!(offset_in_page(ret)))
		goto out;
out1:
	vm_unacct_memory(charged);

out:
	return ret;
}

static int vma_expandable(struct vm_area_struct *vma, unsigned long delta)
{
	unsigned long end = vma->vm_end + delta;
	if (end < vma->vm_end) /* overflow */
		return 0;
	if (vma->vm_next && vma->vm_next->vm_start < end) /* intersection */
		return 0;
	if (get_unmapped_area(NULL, vma->vm_start, end - vma->vm_start,
			      0, MAP_FIXED) & ~PAGE_MASK)
		return 0;
	return 1;
}

/*
 * Expand (or shrink) an existing mapping, potentially moving it at the
 * same time (controlled by the MREMAP_MAYMOVE flag and available VM space)
 *
 * MREMAP_FIXED option added 5-Dec-1999 by Benjamin LaHaise
 * This option implies MREMAP_MAYMOVE.
 */
SYSCALL_DEFINE5(mremap, unsigned long, addr, unsigned long, old_len,
		unsigned long, new_len, unsigned long, flags,
		unsigned long, new_addr)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	unsigned long ret = -EINVAL;
	unsigned long charged = 0;
	bool locked = false;

	if (flags & ~(MREMAP_FIXED | MREMAP_MAYMOVE))
		return ret;

	if (flags & MREMAP_FIXED && !(flags & MREMAP_MAYMOVE))
		return ret;

	if (offset_in_page(addr))
		return ret;

	old_len = PAGE_ALIGN(old_len);
	new_len = PAGE_ALIGN(new_len);

	/*
	 * We allow a zero old-len as a special case
	 * for DOS-emu "duplicate shm area" thing. But
	 * a zero new-len is nonsensical.
	 */
	if (!new_len)
		return ret;

	if (down_write_killable(&current->mm->mmap_sem))
		return -EINTR;

	if (flags & MREMAP_FIXED) {
		ret = mremap_to(addr, old_len, new_addr, new_len,
				&locked);
		goto out;
	}

	/*
	 * Always allow a shrinking remap: that just unmaps
	 * the unnecessary pages..
	 * do_munmap does all the needed commit accounting
	 */
	if (old_len >= new_len) {
		ret = do_munmap(mm, addr+new_len, old_len - new_len);
		if (ret && old_len != new_len)
			goto out;
		ret = addr;
		goto out;
	}

	/*
	 * Ok, we need to grow..
	 */
	vma = vma_to_resize(addr, old_len, new_len, &charged);
	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto out;
	}

	/* old_len exactly to the end of the area..
	 */
	if (old_len == vma->vm_end - addr) {
		/* can we just expand the current mapping? */
		if (vma_expandable(vma, new_len - old_len)) {
			int pages = (new_len - old_len) >> PAGE_SHIFT;

			if (vma_adjust(vma, vma->vm_start, addr + new_len,
				       vma->vm_pgoff, NULL)) {
				ret = -ENOMEM;
				goto out;
			}

			vm_stat_account(mm, vma->vm_flags, vma->vm_file, pages);
			if (vma->vm_flags & VM_LOCKED) {
				mm->locked_vm += pages;
				locked = true;
				new_addr = addr;
			}
			ret = addr;
			goto out;
		}
	}

	/*
	 * We weren't able to just expand or shrink the area,
	 * we need to create a new one and move it..
	 */
	ret = -ENOMEM;
	if (flags & MREMAP_MAYMOVE) {
		unsigned long map_flags = 0;
		if (vma->vm_flags & VM_MAYSHARE)
			map_flags |= MAP_SHARED;

		new_addr = get_unmapped_area(vma->vm_file, 0, new_len,
					vma->vm_pgoff +
					((addr - vma->vm_start) >> PAGE_SHIFT),
					map_flags);
		if (offset_in_page(new_addr)) {
			ret = new_addr;
			goto out;
		}

		ret = move_vma(vma, addr, old_len, new_len, new_addr, &locked);
	}
out:
	if (offset_in_page(ret)) {
		vm_unacct_memory(charged);
		locked = 0;
	}
	up_write(&current->mm->mmap_sem);
	if (locked && new_len > old_len)
		mm_populate(new_addr + old_len, new_len - old_len);
	return ret;
}
