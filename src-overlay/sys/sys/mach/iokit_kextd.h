/*
 * iokit_kextd.h — kernel->kextd "load this bundle" Mach message (K3b, #216).
 *
 * The Apple-faithful kext load-request channel: the in-kernel IOKit matcher,
 * on a device match, sends this message to the kext daemon's receive right,
 * which it registered as the HOST_KEXTD_PORT host special port. kextd loads the
 * named bundle (OSKext -> kld); newbus then re-probes and attaches the device.
 * This replaces the K3a placeholder devctl_notify and retires devd from the
 * match path. Mirrors XNU's HOST_KEXTD_PORT / kext request mechanism.
 */
#ifndef _SYS_MACH_IOKIT_KEXTD_H_
#define _SYS_MACH_IOKIT_KEXTD_H_

#include <sys/mach/message.h>
#include <sys/mach/ndr.h>

/* msgh_id for a load request — outside the MACH_NOTIFY_* range ("IOKT"). */
#define	IOKIT_KEXTD_LOAD_MSGID	0x494f4b54

#define	IOKIT_KEXTD_BUNDLE_MAX	128

/*
 * Wire format. Fixed-size, inline (no out-of-line/complex descriptors), so the
 * kernel build is trivial and kextd reads it with a plain mach_msg. bundle_id
 * is the CFBundleIdentifier the kernel decided; device/match are informational.
 */
typedef struct {
	mach_msg_header_t		hdr;
	NDR_record_t			NDR;
	char				bundle_id[IOKIT_KEXTD_BUNDLE_MAX];
	char				device[64];
	uint32_t			match_word;	/* 0x<device><vendor> */
	mach_msg_format_0_trailer_t	trailer;	/* appended on receive */
} iokit_kextd_load_msg_t;

#ifdef _KERNEL
/*
 * Send a load request for `bundle` to the registered HOST_KEXTD_PORT. Returns
 * 0 on send, ENXIO if no kextd has registered, ENOMEM on kmsg exhaustion.
 * Safe from kernel thread context (e.g. the matcher's taskqueue worker).
 */
int	iokit_kextd_send(const char *bundle, const char *device,
	    uint32_t match_word);
#endif

#endif /* _SYS_MACH_IOKIT_KEXTD_H_ */
