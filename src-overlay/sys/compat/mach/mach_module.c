/*-
 * Copyright (c) 2014-2015, Matthew Macy <mmacy@nextbsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *  2. Neither the name of Matthew Macy nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/event.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/sysent.h>
#include <sys/sysproto.h>
#include <sys/types.h>
#include <sys/systm.h>


int mach_debug_enable;

SYSCTL_ROOT_NODE(OID_AUTO,  mach, CTLFLAG_RW, 0,
	"mach subsystem parameters");

SYSCTL_INT(_mach, OID_AUTO, debug_enable, CTLFLAG_RWTUN,
		   &mach_debug_enable, 0, "enable mach debug logging");

/*
 * Park balance. ENTER is bumped immediately before thread_block(), EXIT
 * immediately after it returns. A thread that parks and is never resumed --
 * killed at its timeout while still asleep -- leaves ENTER > EXIT forever.
 *
 * This exists because the reply-port probe in ipc_mqueue_receive_error()
 * produced nothing across a run that reproduced the failure twice. That
 * probe is unconditional and the function has exactly one caller, reached
 * only when ith_state != MACH_MSG_SUCCESS, so a wedged client that never
 * reports there never came back from thread_block() at all. These counters
 * make that visible without assuming which of the two blocking sites it is,
 * and they survive the SIGKILL that hid it from the previous instrument.
 */
unsigned long mach_rcv_park_enter;
SYSCTL_ULONG(_mach, OID_AUTO, rcv_park_enter, CTLFLAG_RD,
		   &mach_rcv_park_enter, 0, "threads entering receive block");
unsigned long mach_rcv_park_exit;
SYSCTL_ULONG(_mach, OID_AUTO, rcv_park_exit, CTLFLAG_RD,
		   &mach_rcv_park_exit, 0, "threads resuming from receive block");
unsigned long mach_snd_park_enter;
SYSCTL_ULONG(_mach, OID_AUTO, snd_park_enter, CTLFLAG_RD,
		   &mach_snd_park_enter, 0, "threads entering send block");
unsigned long mach_snd_park_exit;
SYSCTL_ULONG(_mach, OID_AUTO, snd_park_exit, CTLFLAG_RD,
		   &mach_snd_park_exit, 0, "threads resuming from send block");

/*
 * Post-hoc discriminator, read at port teardown rather than from the wedged
 * thread, which cannot report. A synchronous client that dies waiting for a
 * reply has its reply port destroyed on exit:
 *
 *   queued != 0 -- the reply DID arrive and was never consumed. Delivery and
 *                  routing are fine; the wakeup was lost.
 *   queued == 0 -- nothing was ever aimed at that port. The reply was
 *                  misrouted or never sent.
 */
unsigned long mach_destroy_calls;
SYSCTL_ULONG(_mach, OID_AUTO, destroy_calls, CTLFLAG_RD,
		   &mach_destroy_calls, 0, "ipc_port_destroy calls");
unsigned long mach_destroy_queued;
SYSCTL_ULONG(_mach, OID_AUTO, destroy_queued, CTLFLAG_RD,
		   &mach_destroy_queued, 0, "ports destroyed with messages still queued");


extern struct filterops machport_filtops;

static struct syscall_helper_data osx_syscalls[] = {
	SYSCALL_INIT_HELPER(__proc_info),
	SYSCALL_INIT_HELPER(__iopolicysys),
	SYSCALL_INIT_HELPER(_kernelrpc_mach_vm_allocate_trap),
	SYSCALL_INIT_HELPER(_kernelrpc_mach_vm_deallocate_trap),
	SYSCALL_INIT_HELPER(_kernelrpc_mach_vm_protect_trap),
	SYSCALL_INIT_HELPER(_kernelrpc_mach_vm_map_trap),
	SYSCALL_INIT_HELPER(_kernelrpc_mach_port_allocate_trap),
	SYSCALL_INIT_HELPER(_kernelrpc_mach_port_destroy_trap),
	SYSCALL_INIT_HELPER(_kernelrpc_mach_port_deallocate_trap),
	SYSCALL_INIT_HELPER(_kernelrpc_mach_port_mod_refs_trap),
	SYSCALL_INIT_HELPER(_kernelrpc_mach_port_move_member_trap),
	SYSCALL_INIT_HELPER(_kernelrpc_mach_port_insert_right_trap),
	SYSCALL_INIT_HELPER(_kernelrpc_mach_port_insert_member_trap),
	SYSCALL_INIT_HELPER(_kernelrpc_mach_port_extract_member_trap),
	SYSCALL_INIT_HELPER(_kernelrpc_mach_port_construct_trap),
	SYSCALL_INIT_HELPER(_kernelrpc_mach_port_destruct_trap),
	SYSCALL_INIT_HELPER(mach_reply_port),
	SYSCALL_INIT_HELPER(thread_self_trap),
	SYSCALL_INIT_HELPER(task_self_trap),
	SYSCALL_INIT_HELPER(host_self_trap),
	SYSCALL_INIT_HELPER(mach_msg_trap),
	SYSCALL_INIT_HELPER(mach_msg_overwrite_trap),
	SYSCALL_INIT_HELPER(semaphore_signal_trap),
	SYSCALL_INIT_HELPER(semaphore_signal_all_trap),
	SYSCALL_INIT_HELPER(semaphore_signal_thread_trap),
	SYSCALL_INIT_HELPER(semaphore_wait_trap),
	SYSCALL_INIT_HELPER(semaphore_wait_signal_trap),
	SYSCALL_INIT_HELPER(semaphore_timedwait_signal_trap),
	SYSCALL_INIT_HELPER(_kernelrpc_mach_port_guard_trap),
	SYSCALL_INIT_HELPER(_kernelrpc_mach_port_unguard_trap),
	SYSCALL_INIT_HELPER(task_for_pid),
	SYSCALL_INIT_HELPER(pid_for_task),
	SYSCALL_INIT_HELPER(macx_swapon),
	SYSCALL_INIT_HELPER(macx_swapoff),
	SYSCALL_INIT_HELPER(macx_triggers),
	SYSCALL_INIT_HELPER(macx_backing_store_suspend),
	SYSCALL_INIT_HELPER(macx_backing_store_recovery),
	SYSCALL_INIT_HELPER(swtch_pri),
	SYSCALL_INIT_HELPER(swtch),
	SYSCALL_INIT_HELPER(thread_switch),
	SYSCALL_INIT_HELPER(clock_sleep_trap),
	SYSCALL_INIT_HELPER(mach_timebase_info),
	SYSCALL_INIT_HELPER(mach_wait_until),
	SYSCALL_INIT_HELPER(mk_timer_create),
	SYSCALL_INIT_HELPER(mk_timer_destroy),
	SYSCALL_INIT_HELPER(mk_timer_arm),
	SYSCALL_INIT_HELPER(mk_timer_cancel),
	SYSCALL_INIT_LAST
};

static int
mach_mod_init(void)
{
	int err;

	/*
	 * Out-of-tree fix: the original code rejected post-boot kldload
	 * (`if (!cold) return EINVAL`) because ravynOS expects Mach to be
	 * compiled into the kernel and brought up during boot. Our use
	 * case is the opposite — an explicitly-loadable kernel module —
	 * so allow loading at any time.
	 *
	 * Importantly, this also closes a panic path discovered during
	 * Phase B local testing: when mach_mod_init returned EINVAL after
	 * the SYSINIT functions had already registered eventhandlers,
	 * those handlers stayed live in the kernel after the linker rolled
	 * the failed load back, and the next process exit page-faulted
	 * dereferencing uninitialized Mach state. Letting mach_mod_init
	 * succeed (and the eventhandlers stay paired with a properly-
	 * loaded module) avoids the dangling-handler scenario entirely.
	 */

	if ((err = syscall_helper_register(osx_syscalls, SY_THR_STATIC_KLD))) {
		printf("failed to register osx calls: %d\n", err);
		return (EINVAL);
	}
	/*
	 * Out-of-tree workaround: stock FreeBSD's kqueue_add_filteropts
	 * rejects EVFILT_MACHPORT (-16) because it's out of bounds —
	 * sysfilt_ops[] is sized EVFILT_SYSCOUNT (15), so only filter IDs
	 * -1..-15 are valid. ravynOS patches <sys/event.h> to bump
	 * EVFILT_SYSCOUNT and allocate a -16 slot. We can't make that
	 * kernel-side patch.
	 *
	 * Make the failure non-fatal so mach.ko still loads. Mach kqueue
	 * filter registration is a Phase-B-and-beyond concern; today's
	 * smoke test only requires kldstat -m mach to succeed. Any
	 * downstream code attempting EV_ADD with EVFILT_MACHPORT will
	 * just get EINVAL from kqueue itself.
	 */
	if (kqueue_add_filteropts(EVFILT_MACHPORT, &machport_filtops)) {
		printf("mach: kqueue_add_filteropts(EVFILT_MACHPORT) failed; "
		    "filter unavailable (expected on stock FreeBSD without "
		    "the EVFILT_SYSCOUNT bump)\n");
		/* fall through — module still useful for non-kqueue Mach IPC */
	}
	return (0);
}

static int
mach_module_event_handler(module_t mod, int what, void *arg)
{
	int err;

	switch (what) {
	case MOD_LOAD:
		if ((err = mach_mod_init()) != 0) {
			printf("mach services failed to load - mach system calls will not be available\n");
			return (err);
		}
		break;
	case MOD_UNLOAD:
		return (EBUSY);
	default:
		return (EOPNOTSUPP);
	}
	printf("mach services loaded - mach system calls available\n");
	return (0);
}

static moduledata_t mach_moduledata = {
	"mach",
	mach_module_event_handler,
	NULL
};

DECLARE_MODULE(mach, mach_moduledata, SI_SUB_KLD, SI_ORDER_ANY);



