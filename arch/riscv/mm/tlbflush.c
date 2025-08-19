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

/*
// Removes cores that don't need flushing from the cpumask
static void smokewagon_filter_cpumask(struct cpumask *cmask, 
			      struct mm_struct *mm,
				  unsigned long start, unsigned long size,
				  unsigned long stride)
{
	cpumask_t smokewagon_filter;
	cpumask_clear(&smokewagon_filter);
	struct vm_area_struct *vma;
	char mask_buf[NR_CPUS+1];   // debug printk buffer
	char filter_buf[NR_CPUS+1]; // debug printk buffer
	unsigned long addr = start;
	unsigned long end = start + size;

	// OR smokewagon cpumasks for all pages in range together to create filter
	while (addr < end) {
		vma = lock_vma_under_rcu(mm, start);
		while (addr < vma->vm_end) {
			cpumask_or(&smokewagon_filter, &smokewagon_filter, &mm->context.smokewagon_masks[addr >> PAGE_SHIFT]);
			addr += stride;
		}

		
	}

	// filter interrupt mask with cpumask_and();

	// TODO clear smokewagon mask for everyone who got flushed, needs VMA synchronization
}
*/
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
		} else {
			sbi_remote_sfence_vma_asid(cmask,
						   start, size, asid);
		}
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
	/* use smokewagon */
	__flush_tlb_range(mm_cpumask(mm), get_mm_asid(mm),
			  start, end - start, page_size);
}

void flush_tlb_page(struct vm_area_struct *vma, unsigned long addr)
{
	/* use smokewagon */
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
	/* use smokewagon */
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
	/* use smokewagon */
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
	/* use smokewagon? */
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
#define SMCIR_TLBWR 1UL << 28
#define SMCIR_TLBP 1UL << 31
#define SMCIR_TLBR 1UL << 30

/*
 * print_tlb() probes and prints the jTLB shared by 4 T-Head harts. It's possible and likely that that
 * other threads will be altering these contents as we read and write.
 */
static inline void print_tlb(void)
{
	for (unsigned long i = 0; i < 1024; i++) {
		csr_write(CSR_SMIR, i);
		csr_write(CSR_SMCIR, SMCIR_TLBR);
		unsigned long smir = csr_read(CSR_SMIR);
		unsigned long smeh = csr_read(CSR_SMEH);
		unsigned long smel = csr_read(CSR_SMEL);

		unsigned long miss = (smir & 1UL << 31) >> 31;
		unsigned long fatal = (smir & 1UL << 30) >> 30;

		unsigned long vpn = smeh >> 19;
		unsigned long pg_size = (smeh & GENMASK(18,16)) >> 16;
		unsigned long asid = smeh & GENMASK(15,0);

		unsigned long pfn = (smel & GENMASK(37,10)) >> 10;
		unsigned long prot = smel & ~GENMASK(37,10);

		printk(KERN_ALERT "smokewagon: print_tlb: cpu: %2d, smir: 0x%lx, miss: %lu, fatal: %lu\n"
						  "                       asid: 0x%lx, vpn: 0x%lx, pg_size: %lu, smeh: 0x%lx\n"
						  "                       pfn: 0x%lx, prot: 0x%lx, smel: 0x%lx\n",
			smp_processor_id(), smir, miss, fatal,
			asid, vpn, pg_size, smeh,
			pfn, prot, smel);
	}
}

inline unsigned long scan_tlb(struct mm_struct *mm, unsigned long vpn_to_find)
{
	unsigned long asid_to_find = get_mm_asid(mm);
	int cpu = get_cpu();
	for (unsigned long i = 0; i < 1024; i++) {
		csr_write(CSR_SMIR, i);
		csr_write(CSR_SMCIR, SMCIR_TLBR);
		unsigned long smir = csr_read(CSR_SMIR);
		unsigned long smeh = csr_read(CSR_SMEH);
		unsigned long smel = csr_read(CSR_SMEL);

		unsigned long miss = (smir & 1UL << 31) >> 31;
		unsigned long fatal = (smir & 1UL << 30) >> 30;

		unsigned long vpn_found = smeh >> 19;
		unsigned long pg_size = (smeh & GENMASK(18,16)) >> 16;
		unsigned long asid_found = smeh & GENMASK(15,0);

		unsigned long pfn = (smel & GENMASK(37,10)) >> 10;
		unsigned long prot = smel & ~GENMASK(37,10);

		if ( (vpn_to_find == vpn_found) && (asid_to_find == asid_found) ) {
			printk( "smokewagon: scan_tlb HIT cpu: %2d, asid_to_find: 0x%lx, vpn_to_find: 0x%lx, smir: 0x%lx, miss: %lu, fatal: %lu\n"
					"                         asid_found: 0x%lx, vpn_found: 0x%lx, pg_size: %lu, smeh: 0x%lx\n"
					"                         pfn: 0x%lx, prot: 0x%lx, smel: 0x%lx\n",
				cpu, asid_to_find, vpn_to_find, smir, miss, fatal,
				asid_found, vpn_found, pg_size, smeh,
				pfn, prot, smel);
			put_cpu();
			return 0;
		}
	}
	printk(KERN_ALERT "smokewagon: scan_tlb MISS: cpu: %2d, asid_to_find: 0x%lx, vpn_to_find: 0x%lx\n",
		smp_processor_id(), asid_to_find, vpn_to_find);
	put_cpu();
	return -ENOMEM;
}

inline unsigned long probe_tlb(struct mm_struct *mm, unsigned long vpn)
{
	unsigned long asid = get_mm_asid(mm);
	int cpu = get_cpu();

	// clear smel and smir for clarity's sake
	csr_write(CSR_SMEL, 0);
	csr_write(CSR_SMIR, 0);

	// write smeh then smcir to probe, which just gets the TLB index in smir, but doesn't read the TLB entry at that index
	// SMEH_4KB_PAGE (or 2MB/1GB) is necessary or the probe will MISS?
	// if that's true, do we need to probe 3 times to make sure we test all possible sizes?
	csr_write(CSR_SMEH, asid | SMEH_4KB_PAGE | (vpn << SMEH_VPN_SHIFT));
	csr_write(CSR_SMCIR, asid | SMCIR_TLBP); // probes the TLB, updating smir but not smel or smeh

	// read and report probe results
	unsigned long smir = csr_read(CSR_SMIR);
	unsigned long miss = (smir & 1UL << 31) >> 31;
	unsigned long fatal = (smir & 1UL << 30) >> 30;

	if ( miss || fatal ) {
		/*printk( "smokewagon: probe_tlb MISS cpu: %2d, asid: 0x%lx, vpn: 0x%lx, miss: %lu, fatal: %lu, smeh: 0x%lx\n"
				"                           smel: 0x%lx, smir: 0x%lx\n",
				cpu, asid, vpn, miss, fatal, csr_read(CSR_SMEH),
				csr_read(CSR_SMEL), smir);*/
		put_cpu();
		return -ENOMEM;
	}

	// use smir to read TLB entry into CSRs.
	// TODO FIXME there's no locking here so other cores can race in and change things on us. horrifying!
	csr_write(CSR_SMCIR, asid | SMCIR_TLBR);

	// read CSRs and report
	unsigned long smeh = csr_read(CSR_SMEH);
	unsigned long smel = csr_read(CSR_SMEL);
	unsigned long pfn = (smel & GENMASK(37,10)) >> 10;
	unsigned long prot = smel & GENMASK(9,0);
	smir = csr_read(CSR_SMIR);
	miss = (smir & 1UL << 31) >> 31;
	fatal = (smir & 1UL << 30) >> 30;
	printk( "smokewagon: probe_tlb HIT cpu: %2d, asid: 0x%lx, vpn: 0x%lx, miss: %lu, fatal: %lu\n"
			"                          pfn: 0x%lx, prot: 0x%lx, smeh: 0x%lx, smel: 0x%lx, smir: 0x%lx\n",
			cpu, asid, vpn, miss, fatal,
			pfn, prot, smeh, smel, smir);
	put_cpu();
	return 0;
}

/* enter with VMA read-locked and PTE locked from do_swap_page()
   we rely on the PTE lock to protect the associated page's cpumask */
inline void smokewagon_load_tlb(struct vm_fault *vmf)
{
	/* construct smeh */
	unsigned long vpn = vmf->address >> PAGE_SHIFT;
	unsigned long asid = get_mm_asid(vmf->vma->vm_mm); // TODO: Does this need better locking?
	unsigned long smeh = asid | SMEH_4KB_PAGE | (vpn << SMEH_VPN_SHIFT);

	/* construct smel */
	unsigned long pfn = swp_offset_pfn(pte_to_swp_entry(ptep_get(vmf->pte)));
	pgprot_t prot = vm_get_page_prot(vmf->vma->vm_flags);
	ALT_THEAD_PMA(prot);
	pte_t smel = pfn_pte(pfn, prot);
	if (vmf->vma->vm_flags & VM_WRITE)
		smel = pte_mkwrite(pte_mkdirty(smel), vmf->vma);
	printk(KERN_ALERT "smokewagon_load_tlb(): cpu: %2d, pte: 0x%lx, orig_pte: 0x%lx\n"
					  "                       smeh: 0x%lx, asid: 0x%lx, vpn: 0x%lx, addr: 0x%lx\n"
					  "                       smel: 0x%lx, pfn: 0x%lx, V: %lx, R: %lx, W: %lx, X: %lx, U: %lx, G: %lx, A: %lx, D: %lx\n"
					  "                       vm_flags: 0x%lx, VM_READ: %lx, VM_WRITE: %lx, VM_EXEC: %lx, VM_SHARED: %lx, VM_SMOKEWAGON: %lx, VM_MAYREAD: %lx, VM_MAYWRITE: %lx, VM_MAYEXEC: %lx, VM_MAYSHARE: %lx\n",
		smp_processor_id(), pte_val(ptep_get(vmf->pte)), pte_val(vmf->orig_pte),
		smeh, asid, vpn, vmf->address,
		smel.pte, pfn, smel.pte & _PAGE_PRESENT, smel.pte & _PAGE_READ, smel.pte & _PAGE_WRITE, smel.pte & _PAGE_EXEC, smel.pte & _PAGE_USER, smel.pte & _PAGE_GLOBAL, smel.pte & _PAGE_ACCESSED, smel.pte & _PAGE_DIRTY,
		vmf->vma->vm_flags, vmf->vma->vm_flags & VM_READ, vmf->vma->vm_flags & VM_WRITE, vmf->vma->vm_flags & VM_EXEC, vmf->vma->vm_flags & VM_SHARED, vmf->vma->vm_flags & VM_SMOKEWAGON, vmf->vma->vm_flags & VM_MAYREAD, vmf->vma->vm_flags & VM_MAYWRITE, vmf->vma->vm_flags & VM_MAYEXEC, vmf->vma->vm_flags & VM_MAYSHARE);

	/* construct smcir */
	unsigned long smcir = asid | SMCIR_TLBWR; // TLBWR overwrites a random entry

	/* debug kprint
	cpumask_t before = vmf->vma->vm_mm->context.smokewagon_masks[vpn];
	char before_buf[NR_CPUS+1];
	for (size_t i=0; i<nr_cpu_ids;i++) {before_buf[i] = cpumask_test_cpu(i,&before) ? '1' : '0';}
	before_buf[nr_cpu_ids] = '\0';
	printk(KERN_ALERT "smokewagon: smokewagon_load_tlb() cpu: %2d, vpn: 0x%lx,  before: %s\n", smp_processor_id(), vpn, before_buf);
	*/

	cpumask_set_cpu(smp_processor_id(), &vmf->vma->vm_mm->context.smokewagon_masks[vpn]);

	/* debug kprint
	cpumask_t after = vmf->vma->vm_mm->context.smokewagon_masks[vpn];
	char after_buf[NR_CPUS+1];
	for (size_t i=0; i<nr_cpu_ids;i++) {after_buf[i] = cpumask_test_cpu(i,&after) ? '1' : '0';}
	after_buf[nr_cpu_ids] = '\0';
	printk(KERN_ALERT "smokewagon: smokewagon_load_tlb() cpu: %2d, vpn: 0x%lx,   after: %s\n", smp_processor_id(), vpn, after_buf);
	*/

	csr_write(CSR_SMEH, smeh);
	csr_write(CSR_SMEL, smel.pte);
	csr_write(CSR_SMCIR, smcir);
	//printk(KERN_ALERT "smokewagon: smokewagon_load_tlb(): cpu: %2d, asid: 0x%lx, vpn: 0x%lx, smir_after: 0x%lx, smcir_after: 0x%lx\n", smp_processor_id(), asid, vpn, csr_read(CSR_SMIR), csr_read(CSR_SMCIR));
	//probe_tlb(vmf->vma->vm_mm, vpn);
}
