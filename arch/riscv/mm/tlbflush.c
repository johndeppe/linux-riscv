// SPDX-License-Identifier: GPL-2.0

#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/sched.h>
#include <linux/hugetlb.h>
#include <asm/sbi.h>
#include <asm/mmu_context.h>

static inline void local_flush_tlb_all_asid(unsigned long asid)
{
	if (asid != FLUSH_TLB_NO_ASID)
		__asm__ __volatile__ ("sfence.vma x0, %0"
				:
				: "r" (asid)
				: "memory");
	else
		local_flush_tlb_all();
}

static inline void local_flush_tlb_page_asid(unsigned long addr,
		unsigned long asid)
{
	if (asid != FLUSH_TLB_NO_ASID)
		__asm__ __volatile__ ("sfence.vma %0, %1"
				:
				: "r" (addr), "r" (asid)
				: "memory");
	else
		local_flush_tlb_page(addr);
}

/*
 * Flush entire TLB if number of entries to be flushed is greater
 * than the threshold below.
 */
static unsigned long tlb_flush_all_threshold __read_mostly = 64;

static void local_flush_tlb_range_threshold_asid(unsigned long start,
						 unsigned long size,
						 unsigned long stride,
						 unsigned long asid)
{
	unsigned long nr_ptes_in_range = DIV_ROUND_UP(size, stride);
	int i;

	if (nr_ptes_in_range > tlb_flush_all_threshold) {
		local_flush_tlb_all_asid(asid);
		return;
	}

	for (i = 0; i < nr_ptes_in_range; ++i) {
		local_flush_tlb_page_asid(start, asid);
		start += stride;
	}
}

static inline void local_flush_tlb_range_asid(unsigned long start,
		unsigned long size, unsigned long stride, unsigned long asid)
{
	if (size <= stride)
		local_flush_tlb_page_asid(start, asid);
	else if (size == FLUSH_TLB_MAX_SIZE)
		local_flush_tlb_all_asid(asid);
	else
		local_flush_tlb_range_threshold_asid(start, size, stride, asid);
}

/* Flush a range of kernel pages without broadcasting */
void local_flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	local_flush_tlb_range_asid(start, end - start, PAGE_SIZE, FLUSH_TLB_NO_ASID);
}

static void __ipi_flush_tlb_all(void *info)
{
	local_flush_tlb_all();
}

void flush_tlb_all(void)
{
	if (riscv_use_ipi_for_rfence())
		on_each_cpu(__ipi_flush_tlb_all, NULL, 1);
	else
		sbi_remote_sfence_vma_asid(NULL, 0, FLUSH_TLB_MAX_SIZE, FLUSH_TLB_NO_ASID);
}

struct flush_tlb_range_data {
	unsigned long asid;
	unsigned long start;
	unsigned long size;
	unsigned long stride;
};

static void __ipi_flush_tlb_range_asid(void *info)
{
	struct flush_tlb_range_data *d = info;

	local_flush_tlb_range_asid(d->start, d->size, d->stride, d->asid);
}

static void __flush_tlb_range(struct cpumask *cmask, unsigned long asid,
			      unsigned long start, unsigned long size,
			      unsigned long stride)
{
	struct flush_tlb_range_data ftd;
	bool broadcast;

	if (cpumask_empty(cmask))
		return;

	if (cmask != cpu_online_mask) {
		unsigned int cpuid;

		cpuid = get_cpu();
		/* check if the tlbflush needs to be sent to other CPUs */
		broadcast = cpumask_any_but(cmask, cpuid) < nr_cpu_ids;
	} else {
		broadcast = true;
	}

	if (broadcast) {
		if (riscv_use_ipi_for_rfence()) {
			ftd.asid = asid;
			ftd.start = start;
			ftd.size = size;
			ftd.stride = stride;
			on_each_cpu_mask(cmask,
					 __ipi_flush_tlb_range_asid,
					 &ftd, 1);
		} else
			sbi_remote_sfence_vma_asid(cmask,
						   start, size, asid);
	} else {
		local_flush_tlb_range_asid(start, size, stride, asid);
	}

	if (cmask != cpu_online_mask)
		put_cpu();
}

static inline unsigned long get_mm_asid(struct mm_struct *mm)
{
	return static_branch_unlikely(&use_asid_allocator) ?
			atomic_long_read(&mm->context.id) & asid_mask : FLUSH_TLB_NO_ASID;
}

void flush_tlb_mm(struct mm_struct *mm)
{
	__flush_tlb_range(mm_cpumask(mm), get_mm_asid(mm),
			  0, FLUSH_TLB_MAX_SIZE, PAGE_SIZE);
}

void flush_tlb_mm_range(struct mm_struct *mm,
			unsigned long start, unsigned long end,
			unsigned int page_size)
{
	__flush_tlb_range(mm_cpumask(mm), get_mm_asid(mm),
			  start, end - start, page_size);
}

void flush_tlb_page(struct vm_area_struct *vma, unsigned long addr)
{
	__flush_tlb_range(mm_cpumask(vma->vm_mm), get_mm_asid(vma->vm_mm),
			  addr, PAGE_SIZE, PAGE_SIZE);
}

void flush_tlb_range(struct vm_area_struct *vma, unsigned long start,
		     unsigned long end)
{
	unsigned long stride_size;

	if (!is_vm_hugetlb_page(vma)) {
		stride_size = PAGE_SIZE;
	} else {
		stride_size = huge_page_size(hstate_vma(vma));

		/*
		 * As stated in the privileged specification, every PTE in a
		 * NAPOT region must be invalidated, so reset the stride in that
		 * case.
		 */
		if (has_svnapot()) {
			if (stride_size >= PGDIR_SIZE)
				stride_size = PGDIR_SIZE;
			else if (stride_size >= P4D_SIZE)
				stride_size = P4D_SIZE;
			else if (stride_size >= PUD_SIZE)
				stride_size = PUD_SIZE;
			else if (stride_size >= PMD_SIZE)
				stride_size = PMD_SIZE;
			else
				stride_size = PAGE_SIZE;
		}
	}

	__flush_tlb_range(mm_cpumask(vma->vm_mm), get_mm_asid(vma->vm_mm),
			  start, end - start, stride_size);
}

void flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	__flush_tlb_range((struct cpumask *)cpu_online_mask, FLUSH_TLB_NO_ASID,
			  start, end - start, PAGE_SIZE);
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
void flush_pmd_tlb_range(struct vm_area_struct *vma, unsigned long start,
			unsigned long end)
{
	__flush_tlb_range(mm_cpumask(vma->vm_mm), get_mm_asid(vma->vm_mm),
			  start, end - start, PMD_SIZE);
}
#endif

bool arch_tlbbatch_should_defer(struct mm_struct *mm)
{
	return true;
}

void arch_tlbbatch_add_pending(struct arch_tlbflush_unmap_batch *batch,
			       struct mm_struct *mm,
			       unsigned long uaddr)
{
	cpumask_or(&batch->cpumask, &batch->cpumask, mm_cpumask(mm));
}

void arch_flush_tlb_batched_pending(struct mm_struct *mm)
{
	flush_tlb_mm(mm);
}

void arch_tlbbatch_flush(struct arch_tlbflush_unmap_batch *batch)
{
	__flush_tlb_range(&batch->cpumask, FLUSH_TLB_NO_ASID, 0,
			  FLUSH_TLB_MAX_SIZE, PAGE_SIZE);
	cpumask_clear(&batch->cpumask);
}

/*
 * These constants and the smokewagon_load_tlb() function below load a specific
 * Smokewagon entry into the TLB  by twiddling CSRs.
 *
 * See page 52 of https://github.com/sophgo/sophgo-doc/blob/e416164a90ab761ab2a6815244e09a06a1c0113c/SG2042/T-Head/XuanTie-C910-C920-UserManual.pdf
 */

#define SMEH_VPN_SHIFT 19
#define SMEH_4KB_PAGE 1UL << 16

// SMEL has a Strongly Ordered bit at 63, but we want it to be 0.
#define SMEL_CACHEABLE 1UL << 62
#define SMEL_BUFFERABLE 1UL << 61
#define SMEL_SHAREABLE 1UL << 60
#define SMEL_TRUSTABLE 1UL << 59
#define SMEL_PFN_SHIFT 10
#define SMEL_VALID 1UL << 0

#define SMCIR_TLBWR 1UL << 28

inline void smokewagon_load_tlb(struct vm_fault *vmf)
{
	unsigned long vpn = vmf->address >> PAGE_SHIFT;
	unsigned long asid = get_mm_asid(vmf->vma->vm_mm); // TODO: Does this need better locking?
	unsigned long smeh = asid | SMEH_4KB_PAGE | (vpn << SMEH_VPN_SHIFT);
	csr_write(CSR_SMEH, smeh);

	unsigned long fixed = SMEL_CACHEABLE | SMEL_BUFFERABLE | SMEL_SHAREABLE | SMEL_TRUSTABLE | SMEL_VALID; // TODO check trustable by probing TLB and printing SMEH and SMEL at madvise time? 
	unsigned long RWXUGADR = (GENMASK(9,1) & vmf->orig_pte.pte); // slightly worried about SMEL_VALID not matching the in-memory PTE but what can I do?
	unsigned long pfn = swp_offset_pfn(__pte_to_swp_entry(vmf->orig_pte)) ;
	unsigned long smel = fixed | (pfn << SMEL_PFN_SHIFT) | RWXUGADR;
	csr_write(CSR_SMEL, smel);

	unsigned long smcir = SMCIR_TLBWR; // might need ASID, or might only be needed for TLBIASID?

	printk(KERN_ALERT "smokewagon_load_tlb(): asid: %ld , address: 0x%lx, vpn: 0x%lx pfn: 0x%lx\n", asid, vmf->address, vpn, pfn);
	csr_write(CSR_SMCIR, smcir);
}