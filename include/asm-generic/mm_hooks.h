/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Define generic no-op hooks for arch_dup_mmap, arch_exit_mmap
 * and arch_unmap to be included in asm-FOO/mmu_context.h for any
 * arch FOO which doesn't need to hook these.
 */
#ifndef _ASM_GENERIC_MM_HOOKS_H
#define _ASM_GENERIC_MM_HOOKS_H

static inline int arch_dup_mmap(struct mm_struct *oldmm,
				struct mm_struct *mm)
{
	return 0;
}

static inline void arch_exit_mmap(struct mm_struct *mm)
{
	// unpublish smokewagon_xa for teardown (otherwise tlb flushes from kfree() jump in and it's hairy)
	// would be nicer to do the teardown under smokewagon, FIXME
	struct xarray *xa = xchg(&mm->context.smokewagon_xa, NULL);
	if (xa) {
		printk(KERN_ALERT "smokewagon: arch_exit_mmap()\n");
		unsigned long index;
		cpumask_t * mask;
		xa_for_each(xa, index, mask) {
			printk(KERN_ALERT "smokewagon: arch_exit_mmap(): xa_for_each(): index: 0x%lx, mask_ptr: 0x%p\n", index, mask);
			if (xa_err(mask)) {
				printk("smokewagon: arch_exit_mmap(): xa_err(mask_ptr): %d\n", xa_err(mask));
			} else if (mask) {
				printk("smokewagon: arch_exit_mmap(): freeing mask_ptr: %p\n", mask);
				kfree(mask);
			} else {
				printk("smokewagon: arch_exit_mmap(): NULL mask_ptr: %p\n", mask);
			}
		}
		printk(KERN_ALERT "smokewagon: arch_exit_mmap() before xa_destroy()\n");
		xa_destroy(xa);
		kfree(xa);
	}
}

static inline void arch_unmap(struct mm_struct *mm,
			unsigned long start, unsigned long end)
{
}

static inline bool arch_vma_access_permitted(struct vm_area_struct *vma,
		bool write, bool execute, bool foreign)
{
	/* by default, allow everything */
	return true;
}
#endif	/* _ASM_GENERIC_MM_HOOKS_H */
