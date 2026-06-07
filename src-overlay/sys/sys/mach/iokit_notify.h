/*
 * iokit_notify.h — kernel->userland IORegistry device-event Mach message
 * (K-PR2, nextbsd#225, part of #211/#218).
 *
 * The Apple-faithful device-arrival/departure notification channel. A userland
 * client (DiskArbitration, the IOKitNotify migration) creates a Mach receive
 * right and registers it via IOREGIOCWATCH on /dev/ioregistry (see
 * <sys/ioregistry.h>) together with a packed-nvlist match bag and an event
 * mask. The kernel copies a send right to that port and, from the newbus
 * device_attach / device_detach eventhandlers, pushes one ioreg_event_msg per
 * matching event to the client's port. This is the kernel-served replacement
 * for hwregd's /dev/devctl watch (which stays inert per the #225 decision).
 *
 * Mirrors the kernel-originated send proven in compat/mach/iokit_kextd.c
 * (ikm_alloc -> fill header -> init trailer -> ipc_mqueue_send_always); the
 * trailer MUST be initialized or a receiver that does not request
 * MACH_RCV_TRAILER gets MACH_RCV_INVALID_DATA (the bug PR#18 fixed).
 */
#ifndef _SYS_MACH_IOKIT_NOTIFY_H_
#define _SYS_MACH_IOKIT_NOTIFY_H_

#include <sys/mach/message.h>
#include <sys/mach/ndr.h>

/*
 * msgh_id for a device-event notification — "IONT", chosen outside the
 * MACH_NOTIFY_* range (0x0100..) and distinct from IOKIT_KEXTD_LOAD_MSGID
 * ("IOKT", 0x494f4b54). The userland receivers (the PR4/PR5 IOKitNotify /
 * DiskArbitration migration) MUST match on this id.
 */
#define	IOKIT_NOTIFY_EVENT_MSGID	0x494f4e54	/* 'I','O','N','T' */

/* Event kinds carried in ioreg_event_msg.kind — value-identical to the
 * IOREG_EVENT_* mask bits in <sys/ioregistry.h>. Re-stated here so a userland
 * receiver of this message need only include this one header. */
#define	IOKIT_NOTIFY_KIND_ARRIVE	0x00000001	/* device attached */
#define	IOKIT_NOTIFY_KIND_DEPART	0x00000002	/* device detached */
#define	IOKIT_NOTIFY_KIND_MATCHED	0x00000004	/* device bound driver */

#define	IOKIT_NOTIFY_NAME_MAX		32		/* == IOREG_NAME_MAX */

/*
 * Wire format. Fixed-size, inline (no out-of-line/complex descriptors) so the
 * kernel send is trivial and the client reads it with a plain mach_msg. The
 * body identifies the device by its stable registry id (see <sys/ioregistry.h>)
 * plus its newbus name/class and PCI ids (0 if not a PCI nub), enough for a
 * client to act or to issue follow-up IOREGIOC* queries.
 */
typedef struct {
	mach_msg_header_t		hdr;
	NDR_record_t			NDR;
	uint32_t			kind;		/* IOKIT_NOTIFY_KIND_* */
	uint32_t			_pad;		/* keep id 8-byte aligned */
	uint64_t			id;		/* stable registry node id */
	char				name[IOKIT_NOTIFY_NAME_MAX];	/* device_get_name() */
	char				classname[IOKIT_NOTIFY_NAME_MAX]; /* devclass name */
	uint32_t			pci_vendor;	/* 0 if not a PCI nub */
	uint32_t			pci_device;	/* 0 if not a PCI nub */
	mach_msg_format_0_trailer_t	trailer;	/* appended on receive */
} ioreg_event_msg;

#ifdef _KERNEL
/*
 * Kernel-side notify channel, implemented in compat/mach/iokit_notify.c (only
 * under COMPAT_MACH; iokit_registry.c calls these via weak/extern decls so the
 * standard build needs no Mach headers). All operate on an opaque send right
 * stored as a void * (an ipc_port_t) so the standard registry file never names
 * a Mach type.
 *
 *   iokit_notify_copyin_port: resolve a caller-supplied port *name* in the
 *       current task's IPC space to a retained naked send right (void *), the
 *       same MACH_MSG_TYPE_COPY_SEND copyin task_set_special_port_trap uses.
 *       Returns 0 and *out on success; an errno on failure (*out untouched).
 *   iokit_notify_port_dead: true if the stored send right is dead/invalid.
 *   iokit_notify_copy_port: return a fresh retained send ref to the same port
 *       (NULL if dead). Used to snapshot a watch's port under the watch lock so
 *       the ref outlives the lock; the snapshot ref is then handed to
 *       iokit_notify_send (which consumes it).
 *   iokit_notify_release_port: drop a retained send right (stored or snapshot).
 *   iokit_notify_send: build and push one ioreg_event_msg, CONSUMING the passed
 *       send ref (the ref becomes the message's destination right). Returns 0 on
 *       send, ENXIO if the port is dead (ref still released), ENOMEM on kmsg
 *       exhaustion (ref released).
 */
int	iokit_notify_copyin_port(uint32_t port_name, void **out);
int	iokit_notify_port_dead(void *port);
void   *iokit_notify_copy_port(void *port);
void	iokit_notify_release_port(void *port);
int	iokit_notify_send(void *port_ref, uint32_t kind, uint64_t id,
	    const char *name, const char *classname,
	    uint32_t pci_vendor, uint32_t pci_device);
#endif /* _KERNEL */

#endif /* _SYS_MACH_IOKIT_NOTIFY_H_ */
