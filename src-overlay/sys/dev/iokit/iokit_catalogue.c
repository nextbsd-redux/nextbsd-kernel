/*
 * NextBSD in-kernel IOKit catalogue (K2, nextbsd#215).
 *
 * Flat in-kernel store of IOKit driver personalities, pushed from userland
 * (kextd) via ioctl on /dev/iocatalogue — mechanism (a): the kernel never
 * parses XML. The K3 matcher (nextbsd#216) calls iocat_lookup_pci() from the
 * device_nomatch path to find the driver bundle that claims an unmatched device.
 *
 * See sys/sys/iocatalogue.h and the design in
 * pkgdemon.github.io/nextbsd-inkernel-iokit-feasibility.html §9.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/sx.h>
#include <sys/conf.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>
#include <sys/iocatalogue.h>

static MALLOC_DEFINE(M_IOCAT, "iocatalogue", "in-kernel IOKit catalogue");

static TAILQ_HEAD(, iocat_record) iocat_list =
    TAILQ_HEAD_INITIALIZER(iocat_list);
static struct sx iocat_lock;
SX_SYSINIT(iocat_lock, &iocat_lock, "iocatalogue");
static u_int iocat_count;	/* # records; mutated under iocat_lock */

static void
iocat_free_record(struct iocat_record *r)
{
	if (r->match != NULL)
		free(r->match, M_IOCAT);
	free(r, M_IOCAT);
}

static void
iocat_flush(void)
{
	struct iocat_record *r;

	sx_xlock(&iocat_lock);
	while ((r = TAILQ_FIRST(&iocat_list)) != NULL) {
		TAILQ_REMOVE(&iocat_list, r, link);
		iocat_free_record(r);
	}
	iocat_count = 0;
	sx_xunlock(&iocat_lock);
}

static int
iocat_add(struct iocat_add *ua)
{
	struct iocat_record *r;
	uint32_t *match;
	size_t sz;
	int error;

	if (ua->nmatch == 0 || ua->nmatch > IOCAT_MAX_MATCH)
		return (EINVAL);
	/* Force a NUL terminator, then reject an empty bundle id. */
	ua->bundle_id[IOCAT_BUNDLE_ID_MAX - 1] = '\0';
	if (ua->bundle_id[0] == '\0')
		return (EINVAL);

	sz = (size_t)ua->nmatch * sizeof(uint32_t);
	match = malloc(sz, M_IOCAT, M_WAITOK);
	error = copyin((const void *)(uintptr_t)ua->match, match, sz);
	if (error != 0) {
		free(match, M_IOCAT);
		return (error);
	}

	r = malloc(sizeof(*r), M_IOCAT, M_WAITOK | M_ZERO);
	strlcpy(r->bundle_id, ua->bundle_id, sizeof(r->bundle_id));
	r->provider_class = ua->provider_class;
	r->probe_score = ua->probe_score;
	r->nmatch = ua->nmatch;
	r->match = match;

	sx_xlock(&iocat_lock);
	TAILQ_INSERT_TAIL(&iocat_list, r, link);
	iocat_count++;
	sx_xunlock(&iocat_lock);
	return (0);
}

int
iocat_lookup_pci(uint32_t match_word, char *buf, size_t buflen,
    int32_t *score_out)
{
	struct iocat_record *r, *best;
	uint32_t i;

	best = NULL;
	sx_slock(&iocat_lock);
	TAILQ_FOREACH(r, &iocat_list, link) {
		if (r->provider_class != IOCAT_PROVIDER_IOPCIDEVICE)
			continue;
		for (i = 0; i < r->nmatch; i++) {
			if (r->match[i] != match_word)
				continue;
			if (best == NULL || r->probe_score > best->probe_score)
				best = r;
			break;
		}
	}
	if (best != NULL) {
		strlcpy(buf, best->bundle_id, buflen);
		if (score_out != NULL)
			*score_out = best->probe_score;
	}
	sx_sunlock(&iocat_lock);
	return (best != NULL ? 0 : ENOENT);
}

/* ---- /dev/iocatalogue: ioctl ingestion from userland (kextd) ---- */

static d_ioctl_t iocat_ioctl;
static struct cdev *iocat_dev;
static struct cdevsw iocat_cdevsw = {
	.d_version = D_VERSION,
	.d_ioctl   = iocat_ioctl,
	.d_name    = "iocatalogue",
};

static int
iocat_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t data,
    int fflag __unused, struct thread *td __unused)
{
	switch (cmd) {
	case IOCATIOCADD:
		return (iocat_add((struct iocat_add *)data));
	case IOCATIOCFLUSH:
		iocat_flush();
		return (0);
	default:
		return (ENOTTY);
	}
}

/* ---- hw.iokit.catalogue: read-only text dump (debug + hwregd transition) ---- */

static int
iocat_sysctl_dump(SYSCTL_HANDLER_ARGS)
{
	struct iocat_record *r;
	struct sbuf sb;
	uint32_t i;
	int error;

	/* Build into auto-extending memory under the lock, copy out after. */
	sbuf_new(&sb, NULL, 256, SBUF_AUTOEXTEND);
	sx_slock(&iocat_lock);
	TAILQ_FOREACH(r, &iocat_list, link) {
		sbuf_printf(&sb, "%s provider=%u score=%d match=",
		    r->bundle_id, r->provider_class, r->probe_score);
		for (i = 0; i < r->nmatch; i++)
			sbuf_printf(&sb, "%s0x%08x", i ? "," : "", r->match[i]);
		sbuf_cat(&sb, "\n");
	}
	sx_sunlock(&iocat_lock);
	error = sbuf_finish(&sb);
	if (error == 0)
		error = SYSCTL_OUT(req, sbuf_data(&sb), sbuf_len(&sb) + 1);
	sbuf_delete(&sb);
	return (error);
}

static SYSCTL_NODE(_hw, OID_AUTO, iokit, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "NextBSD in-kernel IOKit");
SYSCTL_PROC(_hw_iokit, OID_AUTO, catalogue,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0,
    iocat_sysctl_dump, "A", "IOKit catalogue (registered driver personalities)");
SYSCTL_UINT(_hw_iokit, OID_AUTO, catalogue_count, CTLFLAG_RD,
    &iocat_count, 0, "number of registered personalities");

static int
iocat_modevent(module_t mod __unused, int type, void *data __unused)
{
	switch (type) {
	case MOD_LOAD:
		iocat_dev = make_dev(&iocat_cdevsw, 0, UID_ROOT, GID_WHEEL,
		    0600, "iocatalogue");
		return (iocat_dev != NULL ? 0 : ENXIO);
	case MOD_UNLOAD:
		if (iocat_dev != NULL)
			destroy_dev(iocat_dev);
		iocat_flush();
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t iocat_mod = { "iocatalogue", iocat_modevent, NULL };
DECLARE_MODULE(iocatalogue, iocat_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(iocatalogue, 1);
