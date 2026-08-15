#include "mem/vmm.h"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/tlb.h"
#include "core/panic.h"
#include "core/serial.h"
#include "lib/string.h"
#include "mem/pmm.h"
#include "sync/spinlock.h"

/* Serializes every mutation of a page table, in any address space.
 *
 * There was no lock here at all.  Two CPUs mapping addresses that share a PDPT
 * or PD -- two kernel threads growing the heap, say -- could both observe the
 * intermediate entry absent, both allocate a table page, and both store: the
 * loser's table leaks and every PTE already installed through it is silently
 * lost.  heap_lock does not cover this, because elf_load reaches vmm_space_map
 * holding nothing at all.
 *
 * IRQ-safe, because schedule() calls vmm_space_destroy straight from the timer
 * ISR.  Holding it with interrupts off also means the scheduler cannot preempt
 * a CPU that owns the lock, so it can never deadlock against itself.
 *
 * Lock order: vmm_lock -> pmm_lock (these paths allocate and free page-table
 * pages).  Nothing takes them the other way round; the PMM never calls the VMM.
 */
static spinlock_t vmm_lock;

#define PAGE_PRESENT  0x001ULL
#define PAGE_WRITABLE 0x002ULL
#define PAGE_LARGE    0x080ULL
#define PAGE_MASK     0x000ffffffffff000ULL
#define INDEX_MASK    0x1ffULL
#define TWO_MIB       0x200000ULL

static uint64_t active_pml4_phys;
static uint64_t direct_map;
static uint64_t direct_map_size;

static uint64_t read_cr3(void)
{
	uint64_t value;
	__asm__ volatile("mov %%cr3, %0" : "=r"(value));
	return value;
}

static uint64_t *phys_to_virt(uint64_t physical_address)
{
	if (physical_address >= direct_map_size) {
		panic("page table outside early direct map");
	}
	return (uint64_t *)(uintptr_t)(direct_map + physical_address);
}

void vmm_init(const struct os_boot_info *boot_info)
{
	direct_map = boot_info->direct_map_base;
	direct_map_size = boot_info->direct_map_size;
	active_pml4_phys = read_cr3() & PAGE_MASK;

	serial_write("vmm: cr3=");
	serial_write_hex(active_pml4_phys);
	serial_write(" direct=");
	serial_write_hex(direct_map);
	serial_write("\n");

	if (active_pml4_phys != boot_info->page_table_root) {
		panic("bootloader page-table handoff mismatch");
	}
}

/* Mark the whole direct map non-executable.  It aliases every byte of physical
   memory, so leaving it executable means any page the kernel can write — a DMA
   buffer, the heap, a user page — is reachable as code by a stray jump, which
   is most of what SMEP is trying to prevent.  The kernel executes from its own
   higher-half mapping at 0xffffffff80000000 and only ever reads and writes
   through the direct map.

   The identity map at virtual 0 is deliberately left executable: the AP
   trampoline runs there during the real-to-long-mode transition.

   EFER.NXE must already be set on every CPU that can walk these tables — the
   BSP does it in cpu_enable_protections before this runs, and the AP
   trampoline sets it alongside LME before enabling paging. With NXE clear,
   bit 63 is reserved and faults on any access through the entry. */
void vmm_harden_direct_map(void)
{
	uint64_t *pml4 = phys_to_virt(active_pml4_phys);
	uint64_t marked = 0;

	for (uint64_t off = 0; off < direct_map_size; off += TWO_MIB) {
		uint64_t virt  = direct_map + off;
		uint64_t pml4i = (virt >> 39U) & INDEX_MASK;
		uint64_t pdpti = (virt >> 30U) & INDEX_MASK;
		uint64_t pdi   = (virt >> 21U) & INDEX_MASK;

		uint64_t pml4e = pml4[pml4i];
		if ((pml4e & PAGE_PRESENT) == 0) {
			continue;
		}
		uint64_t *pdpt = phys_to_virt(pml4e & PAGE_MASK);
		uint64_t pdpte = pdpt[pdpti];
		if ((pdpte & PAGE_PRESENT) == 0) {
			continue;
		}
		if ((pdpte & PAGE_LARGE) != 0) {          /* 1 GiB mapping */
			if ((pdpte & VMM_NX) == 0) {
				pdpt[pdpti] = pdpte | VMM_NX;
				marked++;
			}
			continue;
		}
		uint64_t *pd = phys_to_virt(pdpte & PAGE_MASK);
		uint64_t pde = pd[pdi];
		if ((pde & PAGE_PRESENT) != 0 && (pde & VMM_NX) == 0) {
			pd[pdi] = pde | VMM_NX;
			marked++;
		}
	}

	/* Reload CR3 to drop stale TLB entries for everything just changed. */
	__asm__ volatile("mov %%cr3, %%rax\n\tmov %%rax, %%cr3"
			 : : : "rax", "memory");

	serial_write("vmm: direct map marked NX (");
	serial_write_dec(marked);
	serial_write(" entries)\n");
}

bool vmm_virt_to_phys(uint64_t virtual_address, uint64_t *physical_address)
{
	uint64_t pml4i = (virtual_address >> 39U) & INDEX_MASK;
	uint64_t pdpti = (virtual_address >> 30U) & INDEX_MASK;
	uint64_t pdi = (virtual_address >> 21U) & INDEX_MASK;
	uint64_t pti = (virtual_address >> 12U) & INDEX_MASK;
	uint64_t offset_4k = virtual_address & 0xfffULL;

	uint64_t *pml4 = phys_to_virt(active_pml4_phys);
	uint64_t pml4e = pml4[pml4i];
	if ((pml4e & PAGE_PRESENT) == 0) {
		return false;
	}

	uint64_t *pdpt = phys_to_virt(pml4e & PAGE_MASK);
	uint64_t pdpte = pdpt[pdpti];
	if ((pdpte & PAGE_PRESENT) == 0) {
		return false;
	}
	if ((pdpte & PAGE_LARGE) != 0) {
		*physical_address = (pdpte & PAGE_MASK) +
			(virtual_address & 0x3fffffffULL);
		return true;
	}

	uint64_t *pd = phys_to_virt(pdpte & PAGE_MASK);
	uint64_t pde = pd[pdi];
	if ((pde & PAGE_PRESENT) == 0) {
		return false;
	}
	if ((pde & PAGE_LARGE) != 0) {
		*physical_address = (pde & PAGE_MASK) +
			(virtual_address & 0x1fffffULL);
		return true;
	}

	uint64_t *pt = phys_to_virt(pde & PAGE_MASK);
	uint64_t pte = pt[pti];
	if ((pte & PAGE_PRESENT) == 0) {
		return false;
	}

	*physical_address = (pte & PAGE_MASK) + offset_4k;
	return true;
}

uint64_t vmm_direct_map_base(void)
{
	return direct_map;
}

uint64_t vmm_pml4_phys(void)
{
	return active_pml4_phys;
}

/* The address space actually installed right now, which is only the kernel's
   while a kernel thread runs.  vmm_pml4_phys() is a boot-time snapshot: anyone
   who switches CR3 temporarily and means to put back what was there must save
   this, or a user thread resumes on the kernel PML4. */
uint64_t vmm_current_cr3(void)
{
	return read_cr3() & PAGE_MASK;
}

static void invlpg(uint64_t virt)
{
	__asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

/* Returns the child table pointer for table[index], allocating a new page
   table page if the entry is absent. Returns NULL on OOM or if the entry
   is already a large page (cannot descend through it for 4K mappings). */
static uint64_t *ensure_table(uint64_t *table, uint64_t index)
{
	if ((table[index] & PAGE_PRESENT) == 0) {
		uint64_t page = pmm_alloc_page();
		if (page == PMM_ALLOC_FAILED) {
			return NULL;
		}
		uint64_t *child = phys_to_virt(page);
		for (uint64_t i = 0; i < 512U; ++i) {
			child[i] = 0;
		}
		table[index] = (page & PAGE_MASK) | PAGE_PRESENT | PAGE_WRITABLE;
	} else if ((table[index] & PAGE_LARGE) != 0) {
		return NULL;
	}
	return phys_to_virt(table[index] & PAGE_MASK);
}

/* Walks existing page tables to the PTE for virt (4K granularity).
   Returns a pointer to the PTE slot, or NULL if any intermediate table
   is absent or is a large page. The returned PTE may itself be 0. */
static uint64_t *pte_for(uint64_t virt)
{
	uint64_t pml4i = (virt >> 39U) & INDEX_MASK;
	uint64_t pdpti = (virt >> 30U) & INDEX_MASK;
	uint64_t pdi   = (virt >> 21U) & INDEX_MASK;
	uint64_t pti   = (virt >> 12U) & INDEX_MASK;

	uint64_t *pml4 = phys_to_virt(active_pml4_phys);
	uint64_t pml4e = pml4[pml4i];
	if ((pml4e & PAGE_PRESENT) == 0 || (pml4e & PAGE_LARGE) != 0) {
		return NULL;
	}

	uint64_t *pdpt = phys_to_virt(pml4e & PAGE_MASK);
	uint64_t pdpte = pdpt[pdpti];
	if ((pdpte & PAGE_PRESENT) == 0 || (pdpte & PAGE_LARGE) != 0) {
		return NULL;
	}

	uint64_t *pd = phys_to_virt(pdpte & PAGE_MASK);
	uint64_t pde = pd[pdi];
	if ((pde & PAGE_PRESENT) == 0 || (pde & PAGE_LARGE) != 0) {
		return NULL;
	}

	uint64_t *pt = phys_to_virt(pde & PAGE_MASK);
	return &pt[pti];
}

/* Read back the raw leaf PTE for `virt`. Lets callers (and the tests) verify
   what actually reached the hardware rather than trusting the mapping call. */
bool vmm_pte_peek(uint64_t virt, uint64_t *out)
{
	uint64_t *pte = pte_for(virt);
	if (pte == NULL || (*pte & PAGE_PRESENT) == 0) {
		return false;
	}
	*out = *pte;
	return true;
}

/* Read back the entry that actually maps `virt` at large-page granularity —
   the PDPT entry if it is a 1 GiB page, otherwise the PD entry. Needed to
   inspect the direct map, which is built from large pages and so has no PTE. */
bool vmm_pde_peek(uint64_t virt, uint64_t *out)
{
	uint64_t pml4i = (virt >> 39U) & INDEX_MASK;
	uint64_t pdpti = (virt >> 30U) & INDEX_MASK;
	uint64_t pdi   = (virt >> 21U) & INDEX_MASK;

	uint64_t *pml4 = phys_to_virt(active_pml4_phys);
	if ((pml4[pml4i] & PAGE_PRESENT) == 0) {
		return false;
	}
	uint64_t *pdpt = phys_to_virt(pml4[pml4i] & PAGE_MASK);
	uint64_t pdpte = pdpt[pdpti];
	if ((pdpte & PAGE_PRESENT) == 0) {
		return false;
	}
	if ((pdpte & PAGE_LARGE) != 0) {
		*out = pdpte;
		return true;
	}
	uint64_t *pd = phys_to_virt(pdpte & PAGE_MASK);
	if ((pd[pdi] & PAGE_PRESENT) == 0) {
		return false;
	}
	*out = pd[pdi];
	return true;
}

static bool vmm_map_locked(uint64_t virt, uint64_t phys, uint64_t flags)
{
	uint64_t pml4i = (virt >> 39U) & INDEX_MASK;
	uint64_t pdpti = (virt >> 30U) & INDEX_MASK;
	uint64_t pdi   = (virt >> 21U) & INDEX_MASK;
	uint64_t pti   = (virt >> 12U) & INDEX_MASK;

	uint64_t *pml4 = phys_to_virt(active_pml4_phys);
	uint64_t *pdpt = ensure_table(pml4, pml4i);
	if (pdpt == NULL) {
		return false;
	}
	uint64_t *pd = ensure_table(pdpt, pdpti);
	if (pd == NULL) {
		return false;
	}
	uint64_t *pt = ensure_table(pd, pdi);
	if (pt == NULL) {
		return false;
	}

	pt[pti] = (phys & PAGE_MASK) | flags | PAGE_PRESENT;
	invlpg(virt);
	return true;
}

bool vmm_map(uint64_t virt, uint64_t phys, uint64_t flags)
{
	uint64_t f = spinlock_acquire_irqsave(&vmm_lock);
	bool ok = vmm_map_locked(virt, phys, flags);
	spinlock_release_irqrestore(&vmm_lock, f);
	/* A new mapping only ever replaces a not-present entry here, so no other
	   CPU can hold a stale TLB entry for it. */
	return ok;
}

bool vmm_unmap(uint64_t virt)
{
	uint64_t f = spinlock_acquire_irqsave(&vmm_lock);
	uint64_t *pte = pte_for(virt);
	if (pte == NULL || (*pte & PAGE_PRESENT) == 0) {
		spinlock_release_irqrestore(&vmm_lock, f);
		return false;
	}
	*pte = 0;
	invlpg(virt);
	spinlock_release_irqrestore(&vmm_lock, f);
	/* Kernel-half address: every CPU shares these tables, so every CPU may hold
	   a stale entry.  Must happen after the PTE is cleared and the lock is
	   dropped -- the IPI handler does not need the lock, but the target CPUs
	   must not observe the old PTE. */
	tlb_shootdown_page(virt);
	return true;
}

bool vmm_protect(uint64_t virt, uint64_t flags)
{
	uint64_t f = spinlock_acquire_irqsave(&vmm_lock);
	uint64_t *pte = pte_for(virt);
	if (pte == NULL || (*pte & PAGE_PRESENT) == 0) {
		spinlock_release_irqrestore(&vmm_lock, f);
		return false;
	}
	*pte = (*pte & PAGE_MASK) | flags | PAGE_PRESENT;
	invlpg(virt);
	spinlock_release_irqrestore(&vmm_lock, f);
	/* Narrowing permissions must be visible everywhere immediately, or another
	   CPU keeps writing through a cached writable entry. */
	tlb_shootdown_page(virt);
	return true;
}

/* ---- Per-process address space API ------------------------------------ */

/* ensure_table variant that operates on an explicitly provided pml4. */
static uint64_t *ensure_table_in(uint64_t *table, uint64_t index, uint64_t extra_flags)
{
	if ((table[index] & PAGE_PRESENT) == 0) {
		uint64_t page = pmm_alloc_page();
		if (page == PMM_ALLOC_FAILED) {
			return NULL;
		}
		uint64_t *child = phys_to_virt(page);
		for (uint64_t i = 0; i < 512U; ++i) {
			child[i] = 0;
		}
		table[index] = (page & PAGE_MASK) | PAGE_PRESENT | PAGE_WRITABLE | extra_flags;
	} else if ((table[index] & PAGE_LARGE) != 0) {
		return NULL;
	}
	return phys_to_virt(table[index] & PAGE_MASK);
}

uint64_t vmm_space_create(void)
{
	/* Reads the kernel half of the live PML4, which vmm_map may be extending on
	   another CPU: without the lock a new address space could capture a
	   half-installed entry. */
	uint64_t f = spinlock_acquire_irqsave(&vmm_lock);
	uint64_t pml4_page = pmm_alloc_page();
	if (pml4_page == PMM_ALLOC_FAILED) {
		spinlock_release_irqrestore(&vmm_lock, f);
		return 0;
	}

	uint64_t *new_pml4  = phys_to_virt(pml4_page);
	uint64_t *kern_pml4 = phys_to_virt(active_pml4_phys);

	/* Zero user half, copy kernel half (entries 256-511). */
	memset(new_pml4, 0, 256U * sizeof(uint64_t));
	memcpy(&new_pml4[256], &kern_pml4[256], 256U * sizeof(uint64_t));

	spinlock_release_irqrestore(&vmm_lock, f);
	return pml4_page;
}

/* No TLB shootdown here, and none is needed: a process has exactly one thread,
   so only one CPU ever loaded this CR3, and schedule() switches that CPU away
   from it before handing the space to the reaper.  Writing CR3 flushes every
   non-global entry for the old space on that CPU, so no stale translation to
   these pages can survive anywhere.  Revisit if a process ever gains a second
   thread. */
void vmm_space_destroy(uint64_t pml4_phys)
{
	uint64_t lockf = spinlock_acquire_irqsave(&vmm_lock);
	if (pml4_phys == 0 || pml4_phys == active_pml4_phys) {
		return;
	}

	uint64_t *pml4 = phys_to_virt(pml4_phys);

	/* Walk user half only (PML4 indices 0-255). */
	for (uint64_t pml4i = 0; pml4i < 256U; ++pml4i) {
		if ((pml4[pml4i] & PAGE_PRESENT) == 0) {
			continue;
		}
		uint64_t pdpt_phys = pml4[pml4i] & PAGE_MASK;
		uint64_t *pdpt = phys_to_virt(pdpt_phys);

		for (uint64_t pdpti = 0; pdpti < 512U; ++pdpti) {
			if ((pdpt[pdpti] & PAGE_PRESENT) == 0) {
				continue;
			}
			if ((pdpt[pdpti] & PAGE_LARGE) != 0) {
				pmm_free_page(pdpt[pdpti] & PAGE_MASK);
				continue;
			}
			uint64_t pd_phys = pdpt[pdpti] & PAGE_MASK;
			uint64_t *pd = phys_to_virt(pd_phys);

			for (uint64_t pdi = 0; pdi < 512U; ++pdi) {
				if ((pd[pdi] & PAGE_PRESENT) == 0) {
					continue;
				}
				if ((pd[pdi] & PAGE_LARGE) != 0) {
					pmm_free_page(pd[pdi] & PAGE_MASK);
					continue;
				}
				uint64_t pt_phys = pd[pdi] & PAGE_MASK;
				uint64_t *pt = phys_to_virt(pt_phys);

				for (uint64_t pti = 0; pti < 512U; ++pti) {
					if ((pt[pti] & PAGE_PRESENT) != 0) {
						pmm_free_page(pt[pti] & PAGE_MASK);
					}
				}
				pmm_free_page(pt_phys);
			}
			pmm_free_page(pd_phys);
		}
		pmm_free_page(pdpt_phys);
	}
	pmm_free_page(pml4_phys);
	spinlock_release_irqrestore(&vmm_lock, lockf);
}

static bool vmm_space_map_locked(uint64_t pml4_phys, uint64_t virt,
				 uint64_t phys, uint64_t flags)
{
	uint64_t pml4i = (virt >> 39U) & INDEX_MASK;
	uint64_t pdpti = (virt >> 30U) & INDEX_MASK;
	uint64_t pdi   = (virt >> 21U) & INDEX_MASK;
	uint64_t pti   = (virt >> 12U) & INDEX_MASK;

	/* Refuse to map into the kernel half (PML4 indices 256-511).  Those
	   entries are copied into every address space and share the kernel's
	   page tables, so a per-process mapping there would corrupt global
	   kernel state.  Per-process mappings must stay in the user half. */
	if (pml4i >= 256U) {
		return false;
	}

	/* User-accessible intermediate tables get the USER flag. */
	uint64_t user_flag = (flags & VMM_USER) ? VMM_USER : 0;

	uint64_t *pml4 = phys_to_virt(pml4_phys);
	uint64_t *pdpt = ensure_table_in(pml4, pml4i, user_flag);
	if (pdpt == NULL) {
		return false;
	}
	uint64_t *pd = ensure_table_in(pdpt, pdpti, user_flag);
	if (pd == NULL) {
		return false;
	}
	uint64_t *pt = ensure_table_in(pd, pdi, user_flag);
	if (pt == NULL) {
		return false;
	}

	pt[pti] = (phys & PAGE_MASK) | flags | PAGE_PRESENT;
	return true;
}

bool vmm_space_map(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags)
{
	uint64_t f = spinlock_acquire_irqsave(&vmm_lock);
	bool ok = vmm_space_map_locked(pml4_phys, virt, phys, flags);
	spinlock_release_irqrestore(&vmm_lock, f);
	/* No shootdown: this only ever populates a user-half entry of an address
	   space that is not yet running anywhere (elf_load builds it before the
	   thread is queued), so no CPU can hold a stale translation for it. */
	return ok;
}

void vmm_space_switch(uint64_t pml4_phys)
{
	__asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

