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
#include <sys/proc.h>
#include <sys/sbuf.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sx.h>

#include <sys/mach/mach_types.h>
#include <sys/mach/task.h>
#include <sys/mach/thread_pool.h>
#include <sys/mach/ipc/ipc_space.h>
#include <sys/mach/ipc/ipc_entry.h>
#include <sys/mach/ipc/ipc_port.h>
#include <sys/mach/ipc/ipc_pset.h>
#include <sys/selinfo.h>
#include <sys/event.h>




int mach_debug_enable;

SYSCTL_ROOT_NODE(OID_AUTO,  mach, CTLFLAG_RW, 0,
	"mach subsystem parameters");

SYSCTL_INT(_mach, OID_AUTO, debug_enable, CTLFLAG_RWTUN,
		   &mach_debug_enable, 0, "enable mach debug logging");

/*
 * Regression detector for nextbsd-userland#135.
 *
 * rcvlarge_notify counts "a message is waiting on port N" events handed to
 * userland by filt_machport(); these do NOT dequeue anything -- the
 * MACH_RCV_LARGE branch returns with the message still queued and userland
 * issues its own mach_msg(). psetport_drained counts messages actually taken
 * off a port-set member port.
 *
 * A persistently growing gap means daemons are being told about messages they
 * never come back for. That was the wedge: libmach registered the
 * EVFILT_MACHPORT readiness knote EV_CLEAR, so a skipped readiness event was
 * never re-armed. Before the fix the gap ran 10/12/11/6 per 150 sends; after
 * it, delivery went 22/80 -> 80/80 on the original harness.
 *
 * NOTE the gap is not zero in healthy operation and is not meant to be. The
 * knote is level-triggered now, so the same readiness is re-reported until
 * drained and notify legitimately exceeds drained. Watch the TREND against a
 * known-good baseline, not the absolute value.
 */
unsigned long mach_rcvlarge_notify;
SYSCTL_ULONG(_mach, OID_AUTO, rcvlarge_notify, CTLFLAG_RD,
		   &mach_rcvlarge_notify, 0,
		   "kqueue message-waiting notifications handed to userland");
unsigned long mach_psetport_drained;
SYSCTL_ULONG(_mach, OID_AUTO, psetport_drained, CTLFLAG_RD,
		   &mach_psetport_drained, 0,
		   "messages actually dequeued from port-set member ports");

/*
 * Resolve the space holding a port's receive right back to the process that
 * owns it. Caller holds allproc_lock shared; no PROC_LOCK is taken on the
 * processes being compared, which would invert the order against the
 * PROC_LOCK already held by the caller. Diagnostic read only.
 */
static struct proc *
mach_space_to_proc(ipc_space_t space)
{
	struct proc *q;
	task_t t;

	if (space == NULL)
		return (NULL);
	FOREACH_PROC_IN_SYSTEM(q) {
		t = (task_t)q->p_machdata;
		if (t != NULL && t->itk_space == space)
			return (q);
	}
	return (NULL);
}

static int
mach_port_backlog_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sb;
	struct proc *p;
	ipc_space_t space;
	ipc_entry_t entry;
	ipc_port_t port;
	task_t task;
	struct proc *owner;
	char rights[4];
	int error, found, ri;

	error = sysctl_wire_old_buffer(req, 0);
	if (error != 0)
		return (error);
	sbuf_new_for_sysctl(&sb, NULL, 512, req);
	sbuf_printf(&sb, "%-6s %-16s %-6s %-18s %8s %4s %7s %6s %-9s %s\n",
	    "pid", "comm", "name", "port", "msgcount", "pset", "waiters",
	    "rights", "ip_rcvname", "recv-right-owner");

	found = 0;
	sx_slock(&allproc_lock);
	FOREACH_PROC_IN_SYSTEM(p) {
		task = (task_t)p->p_machdata;
		if (task == NULL)
			continue;
		space = task->itk_space;
		if (space == NULL)
			continue;
		PROC_LOCK(p);
		LIST_FOREACH(entry, &space->is_entry_list, ie_space_link) {
			if (entry->ie_bits & MACH_PORT_TYPE_PORT_SET)
				continue;
			port = (ipc_port_t)entry->ie_object;
			if (port == NULL || port->ip_msgcount == 0)
				continue;
			found++;
			ri = 0;
			if (entry->ie_bits & MACH_PORT_TYPE_RECEIVE)
				rights[ri++] = 'R';
			if (entry->ie_bits & MACH_PORT_TYPE_SEND)
				rights[ri++] = 'S';
			if (entry->ie_bits & MACH_PORT_TYPE_SEND_ONCE)
				rights[ri++] = 'O';
			rights[ri] = '\0';
			/*
			 * An inactive port's union holds a destination or a
			 * timestamp, not a receiver -- reading it as a space
			 * there would print garbage.
			 */
			owner = ip_active(port) ?
			    mach_space_to_proc(port->ip_receiver) : NULL;
			sbuf_printf(&sb,
			    "%-6d %-16s %-6u %-18p %8d %4s %7s %6s %-9u %s[%d]\n",
			    p->p_pid, p->p_comm, entry->ie_name, port,
			    port->ip_msgcount,
			    port->ip_pset != NULL ? "yes" : "no",
			    port->port_comm.rcd_thread_pool.thr_acts != NULL ?
			    "YES" : "no",
			    rights[0] != '\0' ? rights : "-",
			    /*
			     * The name the kqueue notification hands to
			     * userland. For the row holding the receive right
			     * this must equal the "name" column; anything else
			     * sends the daemon to the wrong port.
			     */
			    ip_active(port) ? port->ip_receiver_name : 0,
			    owner != NULL ? owner->p_comm : "?",
			    owner != NULL ? owner->p_pid : -1);

			/*
			 * Knote state for the set this stranded port belongs
			 * to. This is the measurement that separates kernel
			 * from userland:
			 *
			 *   ACTIVE or QUEUED set -- the kernel told userland a
			 *     message was waiting and userland did not drain
			 *     it. Nothing further to find on this side.
			 *   neither set -- userland was never told, and the
			 *     activation was lost despite every counter on the
			 *     delivery path reading clean.
			 *
			 * Read WITHOUT ips_note_lock: that is an sx, this runs
			 * under PROC_LOCK (a mutex), and sleeping there would
			 * panic. Same trade-off already taken for the port
			 * fields above, and sound for the same reason -- a
			 * wedge persists for minutes. The iteration is bounded
			 * so a torn or corrupt list cannot spin the sysctl.
			 */
			if (port->ip_pset != NULL) {
				struct knote *kn;
				int kncount = 0;

				SLIST_FOREACH(kn,
				    &port->ip_pset->ips_note.kl_list,
				    kn_selnext) {
					if (++kncount > 8) {
						sbuf_printf(&sb,
						    "         knote: ...more\n");
						break;
					}
					sbuf_printf(&sb,
					    "         knote kq=%p status=0x%x%s%s%s "
					    "influx=%d flags=0x%x\n",
					    kn->kn_kq, kn->kn_status,
					    (kn->kn_status & KN_ACTIVE) ? " ACTIVE" : "",
					    (kn->kn_status & KN_QUEUED) ? " QUEUED" : "",
					    (kn->kn_status & KN_DISABLED) ? " DISABLED" : "",
					    kn->kn_influx, kn->kn_flags);
				}
				if (kncount == 0)
					sbuf_printf(&sb,
					    "         knote: NONE registered on this pset\n");
			}
		}
		PROC_UNLOCK(p);
	}
	sx_sunlock(&allproc_lock);

	if (found == 0)
		sbuf_printf(&sb, "(no port is holding an undelivered message)\n");
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}
SYSCTL_PROC(_mach, OID_AUTO, port_backlog,
	    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE,
	    NULL, 0, mach_port_backlog_sysctl, "A",
	    "ports currently holding undelivered messages");


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



