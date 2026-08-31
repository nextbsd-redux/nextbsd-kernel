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
/*
 * ipc_pset_signal() accounting -- the enqueue fallback that is supposed to
 * wake an EVFILT_MACHPORT daemon when no pooled receiver took the message.
 *
 *   calls    - how often the fallback ran
 *   empty    - how often it hit KNLIST_EMPTY and returned doing NOTHING
 *   knotes   - knotes it walked
 *   enqueued - knotes it actually queued (a real wakeup)
 *   already  - knotes already queued (harmless)
 *   disabled - knotes marked KN_DISABLED (activation dropped)
 *
 * empty dominating means the fallback is dead code in practice and delivery
 * depends entirely on the thread-pool handoff in ipc_mqueue_deliver().
 * enqueued healthy means the wakeup IS delivered and is being destroyed
 * downstream, which puts the defect in the kqueue core rather than here.
 */
unsigned long mach_pset_signal_calls;
SYSCTL_ULONG(_mach, OID_AUTO, pset_signal_calls, CTLFLAG_RD,
		   &mach_pset_signal_calls, 0, "ipc_pset_signal calls");
unsigned long mach_pset_signal_empty;
SYSCTL_ULONG(_mach, OID_AUTO, pset_signal_empty, CTLFLAG_RD,
		   &mach_pset_signal_empty, 0, "calls that found an empty knlist");
unsigned long mach_pset_signal_knotes;
SYSCTL_ULONG(_mach, OID_AUTO, pset_signal_knotes, CTLFLAG_RD,
		   &mach_pset_signal_knotes, 0, "knotes walked");
unsigned long mach_pset_signal_enqueued;
SYSCTL_ULONG(_mach, OID_AUTO, pset_signal_enqueued, CTLFLAG_RD,
		   &mach_pset_signal_enqueued, 0, "knotes actually queued");
unsigned long mach_pset_signal_already;
SYSCTL_ULONG(_mach, OID_AUTO, pset_signal_already, CTLFLAG_RD,
		   &mach_pset_signal_already, 0, "knotes already queued");
unsigned long mach_pset_signal_disabled;
SYSCTL_ULONG(_mach, OID_AUTO, pset_signal_disabled, CTLFLAG_RD,
		   &mach_pset_signal_disabled, 0, "knotes disabled, activation dropped");
/*
 * Sampled BEFORE mach_knote_enqueue(), which the previous probe did not do.
 *
 *   kqasleep  - a thread was waiting in this kq when we queued the knote
 *   kqawake   - nobody was waiting in it
 *   kqcleared - KQ_SLEEP was cleared by the enqueue (it does its own wakeup)
 *
 * kqasleep ~ 0 across hundreds of enqueues, while notifyd provably sleeps in
 * kqueue_scan() holding an undelivered message, means the knote is queued on
 * a different kqueue than the one the daemon sleeps in.
 */
unsigned long mach_pset_signal_kqasleep;
SYSCTL_ULONG(_mach, OID_AUTO, pset_signal_kqasleep, CTLFLAG_RD,
		   &mach_pset_signal_kqasleep, 0, "enqueues onto a kq with a sleeper");
unsigned long mach_pset_signal_kqawake;
SYSCTL_ULONG(_mach, OID_AUTO, pset_signal_kqawake, CTLFLAG_RD,
		   &mach_pset_signal_kqawake, 0, "enqueues onto a kq with no sleeper");
unsigned long mach_pset_signal_kqcleared;
SYSCTL_ULONG(_mach, OID_AUTO, pset_signal_kqcleared, CTLFLAG_RD,
		   &mach_pset_signal_kqcleared, 0, "enqueues that cleared KQ_SLEEP themselves");
/*
 * The in-flux protocol ipc_pset_signal() does not implement (#166).
 *
 *   influx_skip - knote was in flux WITHOUT KN_SCAN: exactly the case
 *                 FreeBSD's knote() refuses to touch. Includes knotes on
 *                 kqueue_scan()'s EV_ONESHOT path that are about to be freed.
 *   influx_scan - in flux WITH KN_SCAN: the case knote() deliberately allows,
 *                 safe because kqueue_scan() holds the knlist lock across
 *                 f_event and cannot proceed until this releases it.
 */
unsigned long mach_pset_signal_influx_skip;
SYSCTL_ULONG(_mach, OID_AUTO, pset_signal_influx_skip, CTLFLAG_RD,
		   &mach_pset_signal_influx_skip, 0,
		   "activations knote() would have skipped (in flux, no KN_SCAN)");
unsigned long mach_pset_signal_influx_scan;
SYSCTL_ULONG(_mach, OID_AUTO, pset_signal_influx_scan, CTLFLAG_RD,
		   &mach_pset_signal_influx_scan, 0,
		   "activations onto a knote in flux from kqueue_scan");
/*
 * filt_machport() accounting -- the consumer side.
 *
 *   calls      - evaluations with hint 0 (a real event check)
 *   mismatch   - the set resolved BY NAME differs from the set the knote
 *                attached to. Non-zero means a knote is scanning the wrong
 *                port set, which is the only shape that fits a delivered
 *                wakeup plus a scan that provably misses nothing plus a
 *                message that stays queued.
 *   xlate_fail - the name no longer resolves to a live port set at all
 *   timedout   - returned 0 (no event); kqueue answers this by clearing
 *                KN_ACTIVE and KN_QUEUED, discarding the activation
 *   event      - returned 1 with a message actually received
 */
unsigned long mach_filt_calls;
SYSCTL_ULONG(_mach, OID_AUTO, filt_calls, CTLFLAG_RD,
		   &mach_filt_calls, 0, "filt_machport event evaluations");
unsigned long mach_filt_mismatch;
SYSCTL_ULONG(_mach, OID_AUTO, filt_mismatch, CTLFLAG_RD,
		   &mach_filt_mismatch, 0, "knote scanning a different pset than it attached to");
unsigned long mach_filt_xlate_fail;
SYSCTL_ULONG(_mach, OID_AUTO, filt_xlate_fail, CTLFLAG_RD,
		   &mach_filt_xlate_fail, 0, "name no longer resolves to a live pset");
unsigned long mach_filt_timedout;
SYSCTL_ULONG(_mach, OID_AUTO, filt_timedout, CTLFLAG_RD,
		   &mach_filt_timedout, 0, "filt_machport returned no event");
unsigned long mach_filt_event;
SYSCTL_ULONG(_mach, OID_AUTO, filt_event, CTLFLAG_RD,
		   &mach_filt_event, 0, "filt_machport returned a message");

/*
 * The kqueue notification path (ipc_mqueue_post_on_thread, MACH_RCV_LARGE).
 * notify counts notifications handed out; name_null counts those that carried
 * MACH_PORT_NAME_NULL, which sends the daemon to no port at all.
 */
/*
 * ipc_mqueue_pset_receive() reports ONE port per evaluation (it breaks at the
 * first non-empty member), and the EVFILT_MACHPORT knote is registered
 * EV_CLEAR, so kqueue clears it after that single report. Any other member
 * port that was ready at the same moment gets no notification and nothing
 * re-arms the knote.
 *
 *   multi_ready - evaluations where at least one OTHER port was also ready
 *   extra_ready - total number of such ports, i.e. notifications not sent
 *
 * Structural, not a race: it needs two ready ports, not a timing window.
 */
/*
 * Real dequeues on port-set member ports, to be read against
 * mach.rcvlarge_notify. A notification does not dequeue; it only tells
 * userland which port to read. The gap between the two is the number of
 * messages userland was told about and did not come back for.
 */
unsigned long mach_psetport_drained;
SYSCTL_ULONG(_mach, OID_AUTO, psetport_drained, CTLFLAG_RD,
		   &mach_psetport_drained, 0, "messages actually dequeued from pset member ports");

unsigned long mach_pset_multi_ready;
SYSCTL_ULONG(_mach, OID_AUTO, pset_multi_ready, CTLFLAG_RD,
		   &mach_pset_multi_ready, 0, "scans finding more than one ready port");
unsigned long mach_pset_extra_ready;
SYSCTL_ULONG(_mach, OID_AUTO, pset_extra_ready, CTLFLAG_RD,
		   &mach_pset_extra_ready, 0, "ready ports that got no notification");

unsigned long mach_rcvlarge_notify;
SYSCTL_ULONG(_mach, OID_AUTO, rcvlarge_notify, CTLFLAG_RD,
		   &mach_rcvlarge_notify, 0, "kqueue message-waiting notifications");
unsigned long mach_rcvlarge_name_null;
SYSCTL_ULONG(_mach, OID_AUTO, rcvlarge_name_null, CTLFLAG_RD,
		   &mach_rcvlarge_name_null, 0, "notifications carrying a null port name");

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

/*
 * mach.port_backlog -- every port in the system currently holding at least
 * one undelivered message, with the process that owns the receive right.
 *
 * A client was observed parked in ipc_mqueue_receive for 226 seconds and
 * completed the instant one unrelated request was sent to the same service.
 * That means its request was sitting on the SERVER's port, unconsumed, and
 * the server was never woken; the later message re-armed the wakeup and
 * drained both. Every counter before this one instrumented the client's
 * reply port, which is the wrong end of the RPC and is why they all read
 * clean.
 *
 * This names the stranded port and its owner during a live wedge, which
 * discriminates the two candidate wakeup paths: syslogd parks a worker in
 * ipc_mqueue_receive, while notifyd has no thread there at all and takes
 * its Mach traffic through EVFILT_MACHPORT/kqueue instead.
 *
 * waiters is rcd_thread_pool.thr_acts: non-NULL means a thread is parked on
 * this port and available to take the message. A port with msgcount != 0 AND
 * a waiter present is a lost wakeup outright -- the message and the thread
 * to run it are both sitting there.
 *
 * Port fields are read WITHOUT the port lock. Taking io_lock under PROC_LOCK
 * would invert the established order, and this is a snapshot of a condition
 * that persists for minutes, so a torn read costs nothing an observer of a
 * live wedge cares about. Nothing here writes.
 */
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



