/*
 * Copyright (c) 2013 Google Inc. All rights reserved
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <err.h>
#include <trace.h>
#include <kernel/vm.h>
#include <kernel/mutex.h>
#include <kernel/thread.h>
#include <lib/heap.h>
#include <lib/sm.h>
#include <lib/sm/smcall.h>
#include <lib/sm/sm_err.h>
#include <lk/init.h>
#include <sys/types.h>

#define LOCAL_TRACE	0

void sm_set_mon_stack(void *stack);

extern unsigned long monitor_vector_table;
extern ulong lk_boot_args[4];

static void *boot_args;
static int boot_args_refcnt;
static mutex_t boot_args_lock = MUTEX_INITIAL_VALUE(boot_args_lock);
static thread_t *nsthread;
static uint32_t sm_api_version;
static bool sm_api_version_locked;
static spin_lock_t sm_api_version_lock;

extern smc32_handler_t sm_stdcall_table[];

long smc_sm_api_version(smc32_args_t *args)
{
	uint32_t api_version = args->params[0];

	spin_lock(&sm_api_version_lock);
	if (!sm_api_version_locked) {
		LTRACEF("request api version %d\n", api_version);
		if (api_version > TRUSTY_API_VERSION_CURRENT)
			api_version = TRUSTY_API_VERSION_CURRENT;

		sm_api_version = api_version;
	} else {
		TRACEF("ERROR: Tried to select api version %d after use, current version %d\n",
		       api_version, sm_api_version);
		api_version = sm_api_version;
	}
	spin_unlock(&sm_api_version_lock);

	LTRACEF("return api version %d\n", api_version);
	return api_version;
}

static uint32_t sm_get_api_version(void)
{
	if (!sm_api_version_locked) {
		spin_lock_saved_state_t state;
		spin_lock_save(&sm_api_version_lock, &state, SPIN_LOCK_FLAG_IRQ_FIQ);
		sm_api_version_locked = true;
		TRACEF("lock api version %d\n", sm_api_version);
		spin_unlock_restore(&sm_api_version_lock, state, SPIN_LOCK_FLAG_IRQ_FIQ);
	}
	return sm_api_version;
}

static void sm_wait_for_smcall(void)
{
	long ret = 0;
	smc32_args_t args;

	while (true) {
		enter_critical_section();

		thread_yield();
		arch_disable_fiqs();
		sm_sched_nonsecure(ret, &args);

		/* Pull args out before enabling interrupts */
		arch_enable_fiqs();
		exit_critical_section();

		/* Dispatch 'standard call' handler */
		ret = sm_stdcall_table[SMC_ENTITY(args.smc_nr)](&args);
	}
}

void sm_set_mon_stack(void *stack)
{
#if WITH_LIB_SM_MONITOR
	void monitor_init_secondary(void *secure_svc_stack);
	monitor_init_secondary(stack);
#elif WITH_SMP
	extern void *secondary_cpu_allocated_stack;
	secondary_cpu_allocated_stack = stack;
#endif
}

/* per-cpu secure monitor initialization */
static void sm_secondary_init(uint level)
{
	const size_t stack_size = 4096;
	void *mon_stack;

	/* We need to have a thread context in order to use heap_alloc
	 * On primary CPU it is bootstrap. Set it to nsthread on
	 * secondary CPU.
	 */
	if (!get_current_thread() && nsthread) {
		set_current_thread(nsthread);

		mon_stack = heap_alloc(stack_size, 8);
		if (!mon_stack)
			dprintf(CRITICAL, "failed to allocate monitor mode stack!\n");
		else
			sm_set_mon_stack(mon_stack + stack_size);
	}

#if WITH_LIB_SM_MONITOR
	/* let normal world enable SMP, lock TLB, access CP10/11 */
	__asm__ volatile (
		"mrc	p15, 0, r1, c1, c1, 2	\n"
		"orr	r1, r1, #0xC00		\n"
		"orr	r1, r1, #0x60000	\n"
		"mcr	p15, 0, r1, c1, c1, 2	@ NSACR	\n"
		::: "r1"
	);

	__asm__ volatile (
		"mcr	p15, 0, %0, c12, c0, 1	\n"
		: : "r" (&monitor_vector_table)
	);
#endif
}

LK_INIT_HOOK_FLAGS(libsm_cpu, sm_secondary_init, LK_INIT_LEVEL_PLATFORM - 2, LK_INIT_FLAG_ALL_CPUS);

static void sm_init(uint level)
{
	status_t err;

	mutex_acquire(&boot_args_lock);

	/* Map the boot arguments if supplied by the bootloader */
	if (lk_boot_args[1] && lk_boot_args[2]) {
		ulong offset = lk_boot_args[1] & (PAGE_SIZE - 1);
		paddr_t paddr = ROUNDDOWN(lk_boot_args[1], PAGE_SIZE);
		size_t size   = ROUNDUP(lk_boot_args[2] + offset, PAGE_SIZE);
		void  *vptr;

		err = vmm_alloc_physical(vmm_get_kernel_aspace(), "sm",
				 size, &vptr, PAGE_SIZE_SHIFT, paddr,
				 0, ARCH_MMU_FLAG_NS | ARCH_MMU_FLAG_CACHED);
		if (!err) {
			boot_args = (uint8_t *)vptr + offset;
			boot_args_refcnt++;
		} else {
			boot_args = NULL;
			TRACEF("Error mapping boot parameter block: %d\n", err);
		}
	}

	mutex_release(&boot_args_lock);

	nsthread = thread_create("ns-switch",
			(thread_start_routine)sm_wait_for_smcall,
			NULL, LOWEST_PRIORITY + 1, DEFAULT_STACK_SIZE);
	if (!nsthread) {
		panic("failed to create NS switcher thread!\n");
	}
}

LK_INIT_HOOK(libsm, sm_init, LK_INIT_LEVEL_PLATFORM - 1);

enum handler_return sm_handle_irq(void)
{
	bool fiqs_disabled;
	smc32_args_t args;

	fiqs_disabled = arch_fiqs_disabled();
	if (!fiqs_disabled)
		arch_disable_fiqs();
	sm_sched_nonsecure(SM_ERR_INTERRUPTED, &args);
	while (args.smc_nr != SMC_SC_RESTART_LAST)
		sm_sched_nonsecure(SM_ERR_INTERLEAVED_SMC, &args);
	if (!fiqs_disabled)
		arch_enable_fiqs();

	return INT_NO_RESCHEDULE;
}

void sm_handle_fiq(void)
{
	uint32_t expected_return;
	smc32_args_t args = {0};
	if (sm_get_api_version() >= TRUSTY_API_VERSION_RESTART_FIQ) {
		sm_sched_nonsecure(SM_ERR_FIQ_INTERRUPTED, &args);
		expected_return = SMC_SC_RESTART_FIQ;
	} else {
		sm_sched_nonsecure(SM_ERR_INTERRUPTED, &args);
		expected_return = SMC_SC_RESTART_LAST;
	}
	if (args.smc_nr != expected_return) {
		TRACEF("got bad restart smc %x, expected %x\n",
		       args.smc_nr, expected_return);
		while (args.smc_nr != expected_return)
			sm_sched_nonsecure(SM_ERR_INTERLEAVED_SMC, &args);
	}
}

status_t sm_get_boot_args(void **boot_argsp, size_t *args_sizep)
{
	status_t err = NO_ERROR;

	if (!boot_argsp || !args_sizep)
		return ERR_INVALID_ARGS;

	mutex_acquire(&boot_args_lock);

	if (!boot_args) {
		err = ERR_NOT_CONFIGURED;
		goto unlock;
	}

	boot_args_refcnt++;
	*boot_argsp = boot_args;
	*args_sizep = lk_boot_args[2];
unlock:
	mutex_release(&boot_args_lock);
	return err;
}

void sm_put_boot_args(void)
{
	mutex_acquire(&boot_args_lock);

	if (!boot_args) {
		TRACEF("WARNING: caller does not own "
			"a reference to boot parameters\n");
		goto unlock;
	}

	boot_args_refcnt--;
	if (boot_args_refcnt == 0) {
		vmm_free_region(vmm_get_kernel_aspace(), (vaddr_t)boot_args);
		boot_args = NULL;
		thread_resume(nsthread);
	}
unlock:
	mutex_release(&boot_args_lock);
}

static void sm_release_boot_args(uint level)
{
	if (boot_args) {
		sm_put_boot_args();
	} else {
		/* we need to resume the ns-switcher here if
		 * the boot loader didn't pass bootargs
		 */
		thread_resume(nsthread);
	}

	if (boot_args)
		TRACEF("WARNING: outstanding reference to boot args"
				"at the end of initialzation!\n");
}

LK_INIT_HOOK(libsm_bootargs, sm_release_boot_args, LK_INIT_LEVEL_LAST);
