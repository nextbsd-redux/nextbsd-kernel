/*
 * iokit_kextd.c — kernel->kextd Mach load-request send (K3b, #216).
 *
 * Builds and sends an iokit_kextd_load_msg_t to the kext daemon's receive
 * right (registered as HOST_KEXTD_PORT). Modeled directly on the proven
 * kernel-originated send in ipc/ipc_notify.c (ikm_alloc -> fill header ->
 * ipc_mqueue_send_always); the destination send right is copied out of
 * realhost.special[] the same way kern/ipc_tt.c copies HOST_BOOTSTRAP_PORT.
 *
 * See sys/sys/mach/iokit_kextd.h and
 * pkgdemon.github.io/nextbsd-inkernel-iokit-feasibility.html §9 (K3b).
 */

#include <sys/cdefs.h>
#include <sys/param.h>		/* proven to coexist with the ipc internals
				 * (see ipc/ipc_kmsg.c); systm.h is NOT used by
				 * any ipc file, so avoid it here. */
#include <sys/errno.h>		/* ENXIO, ENOMEM */

#include <sys/mach/port.h>
#include <sys/mach/message.h>
#include <sys/mach/ndr.h>
#include <sys/mach/host.h>
#include <sys/mach/host_special_ports.h>

#include <sys/mach/ipc/ipc_kmsg.h>
#include <sys/mach/ipc/ipc_mqueue.h>
#include <sys/mach/ipc/ipc_port.h>

#include <sys/mach/iokit_kextd.h>

/* realhost is declared in <sys/mach/host.h>. */

/* Local bounded, NUL-terminating copy — avoids pulling <sys/systm.h>/libkern
 * (strlcpy) into a file that already includes the deep Mach ipc internals. */
static void
kextd_strcopy(char *dst, const char *src, size_t n)
{
	size_t i = 0;

	if (n == 0)
		return;
	for (; i + 1 < n && src != NULL && src[i] != '\0'; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

int
iokit_kextd_send(const char *bundle, const char *device, uint32_t match_word)
{
	ipc_port_t port;
	ipc_kmsg_t kmsg;
	iokit_kextd_load_msg_t *m;

	/* Copy a fresh send-right ref to kextd's port (same pattern as the
	 * HOST_BOOTSTRAP_PORT fallback in kern/ipc_tt.c). */
	host_lock(&realhost);
	if (!IP_VALID(realhost.special[HOST_KEXTD_PORT])) {
		host_unlock(&realhost);
		return (ENXIO);		/* no kextd registered yet */
	}
	port = ipc_port_copy_send(realhost.special[HOST_KEXTD_PORT]);
	host_unlock(&realhost);
	if (!IP_VALID(port))
		return (ENXIO);

	kmsg = ikm_alloc(sizeof *m);
	if (kmsg == IKM_NULL) {
		ipc_port_release_send(port);
		return (ENOMEM);
	}
	ikm_init(kmsg, sizeof *m);
	m = (iokit_kextd_load_msg_t *) kmsg->ikm_header;

	m->hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_PORT_SEND, 0);
	m->hdr.msgh_local_port = MACH_PORT_NULL;
	m->hdr.msgh_remote_port = (mach_port_t) port;	/* ref consumed by send */
	m->hdr.msgh_id = IOKIT_KEXTD_LOAD_MSGID;
	m->hdr.msgh_size = (mach_msg_size_t)
	    (sizeof(*m) - sizeof(mach_msg_format_0_trailer_t));
	m->NDR = NDR_record;
	kextd_strcopy(m->bundle_id, bundle, sizeof m->bundle_id);
	kextd_strcopy(m->device, device, sizeof m->device);
	m->match_word = match_word;

	ipc_mqueue_send_always(kmsg);
	return (0);
}
