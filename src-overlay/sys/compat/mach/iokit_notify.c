/*
 * iokit_notify.c — kernel->userland IORegistry device-event Mach send
 * (K-PR2, nextbsd#225, part of #211/#218).
 *
 * The Mach half of the /dev/ioregistry IOREGIOCWATCH notification channel. The
 * standard (non-Mach) registry file dev/iokit/iokit_registry.c owns the watch
 * list, the eventhandlers and the cdev; it calls into here (extern decls under
 * #ifdef COMPAT_MACH, exactly as iokit_catalogue.c calls iokit_kextd_send) for
 * everything that touches a Mach port. The send right is stored opaquely as a
 * void * so the registry file never names an ipc_port_t.
 *
 * Modeled directly on the proven kernel-originated send in
 * compat/mach/iokit_kextd.c (ikm_alloc -> fill header -> init trailer ->
 * ipc_mqueue_send_always). The caller-supplied port name is resolved to a
 * naked send right with the same MACH_MSG_TYPE_COPY_SEND copyin
 * sys_task_set_special_port_trap uses (compat/mach/mach_traps.c).
 *
 * See sys/sys/mach/iokit_notify.h and sys/sys/ioregistry.h.
 */

#include <sys/cdefs.h>
#include <sys/param.h>		/* coexists with the ipc internals (see
				 * ipc/ipc_kmsg.c); systm.h is NOT used by any
				 * ipc file, so it is avoided here too. */
#include <sys/errno.h>		/* ENXIO, ENOMEM, EINVAL */

#include <sys/mach/port.h>
#include <sys/mach/message.h>
#include <sys/mach/ndr.h>

/* IPC internals first: sys/mach/thread.h references struct ipc_kmsg_queue
 * and ipc_object_t (incomplete/unknown otherwise), so the ipc/* headers
 * that define them must precede it — the order mach_traps.c uses. */
#include <sys/mach/ipc/ipc_kmsg.h>
#include <sys/mach/ipc/ipc_mqueue.h>
#include <sys/mach/ipc/ipc_port.h>
#include <sys/mach/ipc/ipc_object.h>
#include <sys/mach/thread.h>		/* current_task(); pulls in task.h (itk_space) */

#include <sys/mach/iokit_notify.h>

/* DEBUG (#218): the kernel printf, declared locally so this file need not pull
 * in <sys/systm.h> (which coexists poorly with the deep Mach ipc internals
 * included above — see the param.h note). The symbol is always linked. */
extern int printf(const char *, ...) __printflike(1, 2);

/* Local bounded, NUL-terminating copy — avoids pulling <sys/systm.h>/libkern
 * (strlcpy) into a file that includes the deep Mach ipc internals (same reason
 * as iokit_kextd.c's kextd_strcopy). */
static void
notify_strcopy(char *dst, const char *src, size_t n)
{
	size_t i = 0;

	if (n == 0)
		return;
	for (; i + 1 < n && src != NULL && src[i] != '\0'; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

/*
 * Resolve a caller-supplied port *name* (in the current task's IPC space) to a
 * retained naked send right, returned opaquely as a void *. Same copyin the
 * task/host set_special_port traps use. Returns 0 + *out on success; a Mach
 * KERN_* mapped to an errno on failure.
 */
int
iokit_notify_copyin_port(uint32_t port_name, void **out)
{
	task_t task = current_task();
	ipc_port_t port = IP_NULL;
	kern_return_t kr;

	if (out == NULL)
		return (EINVAL);
	if (port_name == 0 || task == NULL)
		return (EINVAL);

	kr = ipc_object_copyin(task->itk_space, port_name,
	    MACH_MSG_TYPE_COPY_SEND, (ipc_object_t *)&port);
	if (kr != KERN_SUCCESS)
		return (EINVAL);
	if (!IP_VALID(port))
		return (EINVAL);

	*out = (void *)port;
	return (0);
}

/* True if the stored send right is dead/invalid (client closed its receive
 * right). Cheap, lock-free liveness probe used to prune watches. */
int
iokit_notify_port_dead(void *vport)
{
	ipc_port_t port = (ipc_port_t)vport;

	if (!IP_VALID(port))
		return (1);
	return (ip_active(port) ? 0 : 1);
}

/*
 * Return a fresh retained send ref to the same port (NULL if dead). Lets the
 * registry snapshot a watch's port under its sx so the ref outlives the lock.
 */
void *
iokit_notify_copy_port(void *vport)
{
	ipc_port_t stored = (ipc_port_t)vport;
	ipc_port_t port;

	if (!IP_VALID(stored) || !ip_active(stored))
		return (NULL);
	port = ipc_port_copy_send(stored);
	if (!IP_VALID(port))
		return (NULL);
	return ((void *)port);
}

/* Drop a retained send right (the stored watch ref or a snapshot copy). */
void
iokit_notify_release_port(void *vport)
{
	ipc_port_t port = (ipc_port_t)vport;

	if (IP_VALID(port))
		ipc_port_release_send(port);
}

/*
 * Build and push one ioreg_event_msg, CONSUMING the passed send ref `vport_ref`
 * (it becomes the message's destination right and is freed by the send). Pass a
 * ref obtained from iokit_notify_copy_port(). Returns ENXIO if the port is dead
 * (ref released), ENOMEM on kmsg exhaustion (ref released), 0 on send.
 *
 * The trailer is initialized exactly as iokit_kextd_send does — a receiver that
 * does not request MACH_RCV_TRAILER otherwise copies garbage trailer bytes and
 * fails with MACH_RCV_INVALID_DATA (PR#18).
 */
int
iokit_notify_send(void *vport_ref, uint32_t kind, uint64_t id,
    const char *name, const char *classname,
    uint32_t pci_vendor, uint32_t pci_device)
{
	ipc_port_t port = (ipc_port_t)vport_ref;
	ipc_kmsg_t kmsg;
	ioreg_event_msg *m;

	if (!IP_VALID(port)) {
		printf("iokit_notify: send kind=0x%x id=%ju: port INVALID "
		    "-> ENXIO\n", kind, (uintmax_t)id);
		return (ENXIO);
	}
	if (!ip_active(port)) {
		printf("iokit_notify: send kind=0x%x id=%ju: port INACTIVE "
		    "(client dropped recv right) -> ENXIO\n", kind,
		    (uintmax_t)id);
		ipc_port_release_send(port);
		return (ENXIO);
	}

	kmsg = ikm_alloc(sizeof *m);
	if (kmsg == IKM_NULL) {
		printf("iokit_notify: send kind=0x%x id=%ju: ikm_alloc failed "
		    "-> ENOMEM\n", kind, (uintmax_t)id);
		ipc_port_release_send(port);
		return (ENOMEM);
	}
	ikm_init(kmsg, sizeof *m);
	m = (ioreg_event_msg *) kmsg->ikm_header;

	m->hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_PORT_SEND, 0);
	m->hdr.msgh_local_port = MACH_PORT_NULL;
	m->hdr.msgh_remote_port = (mach_port_t) port;	/* ref consumed by send */
	m->hdr.msgh_voucher_port = 0;
	m->hdr.msgh_id = IOKIT_NOTIFY_EVENT_MSGID;
	m->hdr.msgh_size = (mach_msg_size_t)
	    (sizeof(*m) - sizeof(mach_msg_format_0_trailer_t));
	m->NDR = NDR_record;

	m->kind = kind;
	m->_pad = 0;
	m->id = id;
	notify_strcopy(m->name, name, sizeof m->name);
	notify_strcopy(m->classname, classname, sizeof m->classname);
	m->pci_vendor = pci_vendor;
	m->pci_device = pci_device;

	/* Initialize the trailer (see file header / PR#18). */
	m->trailer.msgh_seqno = 0;
	m->trailer.msgh_sender = KERNEL_SECURITY_TOKEN;
	m->trailer.msgh_trailer_type = MACH_MSG_TRAILER_FORMAT_0;
	m->trailer.msgh_trailer_size = MACH_MSG_TRAILER_MINIMUM_SIZE;

	ipc_mqueue_send_always(kmsg);
	printf("iokit_notify: send kind=0x%x id=%ju name='%s' class='%s' "
	    "-> queued to client port\n", kind, (uintmax_t)id,
	    name != NULL ? name : "", classname != NULL ? classname : "");
	return (0);
}
