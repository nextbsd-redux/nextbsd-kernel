/*
 * NextBSD in-kernel IORegistry (K1, nextbsd#214).
 *
 * A read-only, on-demand view of the live newbus device_t tree, presented as
 * IOKit-style nodes + property bags and queryable from userland via ioctl on
 * /dev/ioregistry (and dumped as text via hw.iokit.registry). This is the
 * kernel-served replacement for hwregd's tree: where hwregd caches hw.bus
 * sysctls + /dev/devctl in userland, this walks the real device_t topology.
 *
 * There is NO shadow tree. Every query walks the live newbus tree (safe in the
 * ioctl/sysctl process context). The only persistent state is a small,
 * sx-guarded device_t -> uint64 id map so userland gets stable node ids: ids
 * are minted lazily during a walk, maintained by the device_attach/device_detach
 * eventhandlers, and an id LINGERS (marked detached) after its device goes away
 * so an in-flight userland iterator never dangles (mirrors hwregd's
 * linger-until-unreferenced semantics).
 *
 * See sys/sys/ioregistry.h and the design in
 * pkgdemon.github.io/nextbsd-inkernel-iokit-feasibility.html.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/sx.h>
#include <sys/lock.h>
#include <sys/conf.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>
#include <sys/bus.h>
#include <sys/eventhandler.h>
#include <sys/nv.h>
#include <sys/ioregistry.h>
#include <sys/iocatalogue.h>		/* SYSCTL_DECL(_hw_iokit) */

#include <dev/pci/pcivar.h>

static MALLOC_DEFINE(M_IOREG, "ioregistry", "in-kernel IOKit registry id map");

/* Bound recursion depth so a pathological tree can't blow the kernel stack.
 * Real newbus trees are <10 deep; ioreg_build_path() caps at 32 likewise. */
#define	IOREG_MAX_DEPTH	64

/*
 * The device_t -> stable id map. Entries live as long as the device, then
 * linger (state == IOREG_STATE_DETACHED) so userland ids don't dangle. They are
 * reclaimed only at MOD_UNLOAD; the live device population bounds the table, and
 * detached entries are rare and small, so an unbounded linger is acceptable for
 * a read-only debug/introspection facility (a later PR can age them out).
 */
struct ioreg_ent {
	LIST_ENTRY(ioreg_ent) link;
	device_t	dev;		/* NULL once detached */
	uint64_t	id;		/* stable, never reused */
	int		state;		/* IOREG_STATE_* */
};
static LIST_HEAD(, ioreg_ent) ioreg_map = LIST_HEAD_INITIALIZER(ioreg_map);
static struct sx ioreg_lock;
SX_SYSINIT(ioreg_lock, &ioreg_lock, "ioregistry");
static uint64_t ioreg_next_id = 1;	/* 0 is reserved as "invalid" */

/* ---- id map helpers (all callers hold ioreg_lock) ---- */

static struct ioreg_ent *
ioreg_find_dev_locked(device_t dev)
{
	struct ioreg_ent *e;

	sx_assert(&ioreg_lock, SX_LOCKED);
	LIST_FOREACH(e, &ioreg_map, link)
		if (e->dev == dev)
			return (e);
	return (NULL);
}

static struct ioreg_ent *
ioreg_find_id_locked(uint64_t id)
{
	struct ioreg_ent *e;

	sx_assert(&ioreg_lock, SX_LOCKED);
	if (id == 0)
		return (NULL);
	LIST_FOREACH(e, &ioreg_map, link)
		if (e->id == id)
			return (e);
	return (NULL);
}

/*
 * Return the stable id for dev, minting one (and recording it live) on first
 * sighting. Caller holds the xlock. dev must be non-NULL.
 */
static uint64_t
ioreg_id_for_locked(device_t dev)
{
	struct ioreg_ent *e;

	sx_assert(&ioreg_lock, SX_XLOCKED);
	e = ioreg_find_dev_locked(dev);
	if (e != NULL) {
		/* A reused device_t (re-attach into the same slot) is live again. */
		e->state = IOREG_STATE_LIVE;
		return (e->id);
	}
	e = malloc(sizeof(*e), M_IOREG, M_WAITOK | M_ZERO);
	e->dev = dev;
	e->id = ioreg_next_id++;
	e->state = IOREG_STATE_LIVE;
	LIST_INSERT_HEAD(&ioreg_map, e, link);
	return (e->id);
}

/* ---- newbus accessors -> ioreg_node ---- */

/* True if dev is a PCI nub (parent bus is "pci"); pci_get_* is then valid. */
static bool
ioreg_is_pci(device_t dev)
{
	device_t parent = device_get_parent(dev);

	return (parent != NULL && strcmp(device_get_name(parent), "pci") == 0);
}

/*
 * Build dev's full nameunit path from the root, e.g. "/nexus0/pcib0/pci0/em0".
 * Bounded by IOREG_PATH_MAX; deep trees are simply truncated (NUL-terminated).
 */
static void
ioreg_build_path(device_t dev, char *buf, size_t buflen)
{
	device_t chain[32];
	int n = 0;
	size_t off = 0;
	device_t d;

	for (d = dev; d != NULL && n < (int)nitems(chain); d = device_get_parent(d))
		chain[n++] = d;
	buf[0] = '\0';
	/* Walk from root (last collected) down to dev (first collected). */
	for (int i = n - 1; i >= 0; i--) {
		const char *nu = device_get_nameunit(chain[i]);
		int r;

		if (nu == NULL || nu[0] == '\0')
			continue;
		r = snprintf(buf + off, buflen - off, "/%s", nu);
		if (r < 0 || (size_t)r >= buflen - off) {
			buf[buflen - 1] = '\0';
			return;
		}
		off += r;
	}
	if (off == 0)
		strlcpy(buf, "/", buflen);
}

/* Fill out a node descriptor for dev with stable id `id`. Caller is in process
 * context (no locks held that forbid the newbus reads here). */
static void
ioreg_fill_node(device_t dev, uint64_t id, uint64_t parent_id, int state,
    struct ioreg_node *n)
{
	driver_t *drv;
	devclass_t dc;
	const char *s;

	bzero(n, sizeof(*n));
	n->id = id;
	n->parent_id = parent_id;
	n->state = state;
	n->devstate = (int32_t)device_get_state(dev);

	s = device_get_name(dev);
	if (s != NULL)
		strlcpy(n->name, s, sizeof(n->name));

	dc = device_get_devclass(dev);
	if (dc != NULL && (s = devclass_get_name(dc)) != NULL)
		strlcpy(n->classname, s, sizeof(n->classname));

	drv = device_get_driver(dev);
	if (drv != NULL && drv->name != NULL)
		strlcpy(n->driver, drv->name, sizeof(n->driver));

	ioreg_build_path(dev, n->path, sizeof(n->path));

	if (ioreg_is_pci(dev)) {
		n->pci_vendor = pci_get_vendor(dev);
		n->pci_device = pci_get_device(dev);
		n->pci_subvendor = pci_get_subvendor(dev);
		n->pci_class = pci_get_class(dev);
	}
}

/* ---- property bag (packed nvlist) ---- */

/*
 * Build a node's property bag. Synthesised from the device_get_* / pci_get_*
 * accessors (releng/15.0 exposes device_get_property()/device_has_property() but
 * no key enumeration, so we cannot iterate arbitrary device properties; we
 * publish the well-known scalars). Returns a freshly created nvlist the caller
 * must nvlist_destroy(); NULL on allocation failure.
 */
static nvlist_t *
ioreg_build_bag(device_t dev, uint64_t id, uint64_t parent_id, int state)
{
	struct ioreg_node n;
	nvlist_t *nvl;

	ioreg_fill_node(dev, id, parent_id, state, &n);

	nvl = nvlist_create(0);
	if (nvl == NULL)
		return (NULL);

	nvlist_add_number(nvl, "id", n.id);
	nvlist_add_number(nvl, "parent_id", n.parent_id);
	nvlist_add_number(nvl, "state", (uint64_t)(uint32_t)n.state);
	nvlist_add_number(nvl, "devstate", (uint64_t)(uint32_t)n.devstate);
	nvlist_add_string(nvl, "name", n.name);
	nvlist_add_string(nvl, "class", n.classname);
	nvlist_add_string(nvl, "driver", n.driver);
	nvlist_add_string(nvl, "path", n.path);
	if (n.pci_vendor != 0 || n.pci_device != 0) {
		nvlist_add_number(nvl, "pci_vendor", n.pci_vendor);
		nvlist_add_number(nvl, "pci_device", n.pci_device);
		nvlist_add_number(nvl, "pci_subvendor", n.pci_subvendor);
		nvlist_add_number(nvl, "pci_class", n.pci_class);
	}
	{
		const char *desc = device_get_desc(dev);

		if (desc != NULL && desc[0] != '\0')
			nvlist_add_string(nvl, "description", desc);
	}
	return (nvl);
}

/* ---- ioctl handlers ---- */

/* IOREGIOCROOT: the root is the top of dev0's ancestry (the "root0" nexus). */
static int
ioreg_get_root(uint64_t *out)
{
	device_t root, d;

	root = devclass_get_device(devclass_find("root"), 0);
	if (root == NULL) {
		/* Fall back: climb from any known device to its topmost parent. */
		sx_xlock(&ioreg_lock);
		struct ioreg_ent *e = LIST_FIRST(&ioreg_map);
		device_t any = (e != NULL) ? e->dev : NULL;
		sx_xunlock(&ioreg_lock);
		if (any == NULL) {
			*out = 0;
			return (0);
		}
		for (d = any; device_get_parent(d) != NULL; d = device_get_parent(d))
			;
		root = d;
	}
	sx_xlock(&ioreg_lock);
	*out = ioreg_id_for_locked(root);
	sx_xunlock(&ioreg_lock);
	return (0);
}

/*
 * IOREGIOCCHILDREN: enumerate the immediate children of node `c->id`. Walks the
 * live tree, mints ids for any newly seen children, copies up to c->max ids out
 * to the user array, and reports the true child count. The id xlock is dropped
 * before copyout (copyout may fault/sleep); the child id list is captured into a
 * small kernel array first.
 */
static int
ioreg_children(struct ioreg_children *c)
{
	device_t parent;
	device_t *kids;
	uint64_t *ids;
	int nkids, i, error;
	uint32_t ncopy;

	/* Resolve the parent device_t under the lock; reject lingering/unknown. */
	sx_xlock(&ioreg_lock);
	{
		struct ioreg_ent *e = ioreg_find_id_locked(c->id);

		if (e == NULL || e->dev == NULL) {
			sx_xunlock(&ioreg_lock);
			return (ENOENT);
		}
		parent = e->dev;
	}
	sx_xunlock(&ioreg_lock);

	error = device_get_children(parent, &kids, &nkids);
	if (error != 0)
		return (error);
	if (nkids < 0)
		nkids = 0;

	ids = malloc((size_t)nkids * sizeof(uint64_t) + 1, M_IOREG, M_WAITOK);
	sx_xlock(&ioreg_lock);
	for (i = 0; i < nkids; i++)
		ids[i] = ioreg_id_for_locked(kids[i]);
	sx_xunlock(&ioreg_lock);
	free(kids, M_TEMP);

	c->count = (uint32_t)nkids;
	ncopy = (c->max < (uint32_t)nkids) ? c->max : (uint32_t)nkids;
	error = 0;
	if (ncopy > 0 && c->children != 0)
		error = copyout(ids, (void *)(uintptr_t)c->children,
		    (size_t)ncopy * sizeof(uint64_t));
	free(ids, M_IOREG);
	return (error);
}

/* IOREGIOCNODE: fill the node descriptor for n->id (live or lingering). */
static int
ioreg_node(struct ioreg_node *n)
{
	device_t dev;
	uint64_t id, parent_id;
	int state;

	id = n->id;
	sx_xlock(&ioreg_lock);
	{
		struct ioreg_ent *e = ioreg_find_id_locked(id);
		struct ioreg_ent *pe;

		if (e == NULL) {
			sx_xunlock(&ioreg_lock);
			return (ENOENT);
		}
		state = e->state;
		dev = e->dev;
		parent_id = 0;
		if (dev != NULL) {
			device_t p = device_get_parent(dev);

			if (p != NULL) {
				pe = ioreg_find_dev_locked(p);
				parent_id = (pe != NULL) ? pe->id :
				    ioreg_id_for_locked(p);
			}
		}
	}
	sx_xunlock(&ioreg_lock);

	if (dev == NULL) {
		/* Lingering: device gone, report what the id remembers. */
		bzero(n, sizeof(*n));
		n->id = id;
		n->state = IOREG_STATE_DETACHED;
		return (0);
	}
	ioreg_fill_node(dev, id, parent_id, state, n);
	return (0);
}

/*
 * IOREGIOCPROPS: pack the node's property bag (nvlist) and copy it out. Sizes
 * the bag, copies up to p->len bytes, and always reports the full packed size so
 * userland can size a retry. The nvlist is built and packed in process context
 * with the id lock dropped (nvlist allocs are M_WAITOK).
 */
static int
ioreg_props(struct ioreg_props *p)
{
	device_t dev;
	uint64_t id, parent_id;
	int state, error;
	nvlist_t *nvl;
	void *packed;
	size_t plen;
	uint32_t outlen;

	id = p->id;
	sx_xlock(&ioreg_lock);
	{
		struct ioreg_ent *e = ioreg_find_id_locked(id);
		struct ioreg_ent *pe;

		if (e == NULL || e->dev == NULL) {
			sx_xunlock(&ioreg_lock);
			return (ENOENT);
		}
		state = e->state;
		dev = e->dev;
		parent_id = 0;
		{
			device_t pp = device_get_parent(dev);

			if (pp != NULL) {
				pe = ioreg_find_dev_locked(pp);
				parent_id = (pe != NULL) ? pe->id :
				    ioreg_id_for_locked(pp);
			}
		}
	}
	sx_xunlock(&ioreg_lock);

	nvl = ioreg_build_bag(dev, id, parent_id, state);
	if (nvl == NULL)
		return (ENOMEM);
	if (nvlist_error(nvl) != 0) {
		error = nvlist_error(nvl);
		nvlist_destroy(nvl);
		return (error);
	}

	packed = nvlist_pack(nvl, &plen);
	nvlist_destroy(nvl);
	if (packed == NULL)
		return (ENOMEM);

	outlen = (plen > p->len) ? p->len : (uint32_t)plen;
	error = 0;
	if (outlen > 0 && p->buf != 0)
		error = copyout(packed, (void *)(uintptr_t)p->buf, outlen);
	p->len = (uint32_t)plen;	/* always report full size */
	free(packed, M_NVLIST);
	return (error);
}

/*
 * AND-match a node against a criteria nvlist: every scalar key present in the
 * criteria must be present and equal in the node's bag. String keys match by
 * value; number keys match numerically. Unknown criteria keys never match.
 */
static bool
ioreg_node_matches(device_t dev, uint64_t id, uint64_t parent_id, int state,
    const nvlist_t *crit)
{
	nvlist_t *bag;
	const char *key;
	int type;
	void *cookie;
	bool ok = true;

	if (crit == NULL || nvlist_empty(crit))
		return (true);
	bag = ioreg_build_bag(dev, id, parent_id, state);
	if (bag == NULL)
		return (false);

	cookie = NULL;
	while (ok && (key = nvlist_next(crit, &type, &cookie)) != NULL) {
		switch (type) {
		case NV_TYPE_STRING:
			if (!nvlist_exists_string(bag, key) ||
			    strcmp(nvlist_get_string(bag, key),
			    nvlist_get_string(crit, key)) != 0)
				ok = false;
			break;
		case NV_TYPE_NUMBER:
			if (!nvlist_exists_number(bag, key) ||
			    nvlist_get_number(bag, key) !=
			    nvlist_get_number(crit, key))
				ok = false;
			break;
		default:
			ok = false;	/* unsupported criteria type */
			break;
		}
	}
	nvlist_destroy(bag);
	return (ok);
}

/*
 * Recursively collect matching node ids into `ids` (capacity in *cap entries,
 * which may be 0 to count-only). *count is the running total of matches found
 * (incremented even past capacity, so the caller learns the true count). Walks
 * the live tree from `dev`. Mints ids as needed (own xlock per device).
 */
static void
ioreg_lookup_walk(device_t dev, const nvlist_t *crit, uint64_t *ids,
    uint32_t cap, uint32_t *count, int depth)
{
	device_t *kids;
	int nkids, i;
	uint64_t id, parent_id;
	int state;

	if (depth > IOREG_MAX_DEPTH)
		return;

	sx_xlock(&ioreg_lock);
	id = ioreg_id_for_locked(dev);
	{
		device_t p = device_get_parent(dev);
		struct ioreg_ent *pe;

		parent_id = 0;
		if (p != NULL) {
			pe = ioreg_find_dev_locked(p);
			parent_id = (pe != NULL) ? pe->id : ioreg_id_for_locked(p);
		}
	}
	state = IOREG_STATE_LIVE;
	sx_xunlock(&ioreg_lock);

	if (ioreg_node_matches(dev, id, parent_id, state, crit)) {
		if (*count < cap && ids != NULL)
			ids[*count] = id;
		(*count)++;
	}

	if (device_get_children(dev, &kids, &nkids) == 0) {
		for (i = 0; i < nkids; i++)
			ioreg_lookup_walk(kids[i], crit, ids, cap, count,
			    depth + 1);
		free(kids, M_TEMP);
	}
}

/*
 * IOREGIOCLOOKUP: copy in the packed criteria nvlist, walk the whole tree from
 * the root, and copy out up to lu->max matching ids (reporting the true count).
 */
static int
ioreg_lookup(struct ioreg_lookup *lu)
{
	nvlist_t *crit = NULL;
	void *cbuf = NULL;
	uint64_t *ids = NULL;
	device_t root, d;
	uint32_t count, ncopy;
	int error;

	if (lu->crit_len > IOREG_CRIT_MAX) /* sanity bound on criteria size */
		return (EINVAL);
	if (lu->crit_len > 0 && lu->buf_criteria != 0) {
		cbuf = malloc(lu->crit_len, M_IOREG, M_WAITOK);
		error = copyin((const void *)(uintptr_t)lu->buf_criteria, cbuf,
		    lu->crit_len);
		if (error != 0) {
			free(cbuf, M_IOREG);
			return (error);
		}
		crit = nvlist_unpack(cbuf, lu->crit_len, 0);
		free(cbuf, M_IOREG);
		if (crit == NULL)
			return (EINVAL);
	}

	/* Find the root device_t (same logic as IOREGIOCROOT). */
	root = devclass_get_device(devclass_find("root"), 0);
	if (root == NULL) {
		sx_xlock(&ioreg_lock);
		struct ioreg_ent *e = LIST_FIRST(&ioreg_map);
		d = (e != NULL) ? e->dev : NULL;
		sx_xunlock(&ioreg_lock);
		if (d != NULL) {
			for (; device_get_parent(d) != NULL; d = device_get_parent(d))
				;
			root = d;
		}
	}
	if (root == NULL) {
		if (crit != NULL)
			nvlist_destroy(crit);
		lu->count = 0;
		return (0);
	}

	ids = (lu->max > 0) ? malloc((size_t)lu->max * sizeof(uint64_t),
	    M_IOREG, M_WAITOK) : NULL;
	count = 0;
	ioreg_lookup_walk(root, crit, ids, lu->max, &count, 0);
	if (crit != NULL)
		nvlist_destroy(crit);

	lu->count = count;
	ncopy = (lu->max < count) ? lu->max : count;
	error = 0;
	if (ncopy > 0 && lu->matches != 0)
		error = copyout(ids, (void *)(uintptr_t)lu->matches,
		    (size_t)ncopy * sizeof(uint64_t));
	if (ids != NULL)
		free(ids, M_IOREG);
	return (error);
}

/* ---- /dev/ioregistry ---- */

static d_ioctl_t ioreg_dev_ioctl;
static struct cdev *ioreg_dev;
static struct cdevsw ioreg_cdevsw = {
	.d_version = D_VERSION,
	.d_ioctl   = ioreg_dev_ioctl,
	.d_name    = "ioregistry",
};

static int
ioreg_dev_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t data,
    int fflag __unused, struct thread *td __unused)
{
	switch (cmd) {
	case IOREGIOCROOT:
		return (ioreg_get_root((uint64_t *)data));
	case IOREGIOCCHILDREN:
		return (ioreg_children((struct ioreg_children *)data));
	case IOREGIOCNODE:
		return (ioreg_node((struct ioreg_node *)data));
	case IOREGIOCPROPS:
		return (ioreg_props((struct ioreg_props *)data));
	case IOREGIOCLOOKUP:
		return (ioreg_lookup((struct ioreg_lookup *)data));
	default:
		return (ENOTTY);
	}
}

/* ---- hw.iokit.registry: read-only text dump (debug + hwregd transition) ---- */

static void
ioreg_sysctl_walk(struct sbuf *sb, device_t dev, int depth)
{
	device_t *kids;
	int nkids, i;
	uint64_t id;
	struct ioreg_node n;

	if (depth > IOREG_MAX_DEPTH)
		return;

	sx_xlock(&ioreg_lock);
	id = ioreg_id_for_locked(dev);
	sx_xunlock(&ioreg_lock);

	ioreg_fill_node(dev, id, 0, IOREG_STATE_LIVE, &n);
	for (i = 0; i < depth; i++)
		sbuf_cat(sb, "  ");
	sbuf_printf(sb, "[%ju] %s class=%s driver=%s",
	    (uintmax_t)id, n.name[0] ? n.name : "?",
	    n.classname[0] ? n.classname : "-",
	    n.driver[0] ? n.driver : "-");
	if (n.pci_vendor != 0 || n.pci_device != 0)
		sbuf_printf(sb, " pci=%04x:%04x", n.pci_vendor, n.pci_device);
	sbuf_cat(sb, "\n");

	if (device_get_children(dev, &kids, &nkids) == 0) {
		for (i = 0; i < nkids; i++)
			ioreg_sysctl_walk(sb, kids[i], depth + 1);
		free(kids, M_TEMP);
	}
}

static int
ioreg_sysctl_dump(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sb;
	device_t root, d;
	int error;

	root = devclass_get_device(devclass_find("root"), 0);
	if (root == NULL) {
		sx_xlock(&ioreg_lock);
		struct ioreg_ent *e = LIST_FIRST(&ioreg_map);
		d = (e != NULL) ? e->dev : NULL;
		sx_xunlock(&ioreg_lock);
		if (d != NULL) {
			for (; device_get_parent(d) != NULL; d = device_get_parent(d))
				;
			root = d;
		}
	}

	sbuf_new(&sb, NULL, 1024, SBUF_AUTOEXTEND);
	if (root != NULL)
		ioreg_sysctl_walk(&sb, root, 0);
	else
		sbuf_cat(&sb, "(no root device)\n");
	error = sbuf_finish(&sb);
	if (error == 0)
		error = SYSCTL_OUT(req, sbuf_data(&sb), sbuf_len(&sb) + 1);
	sbuf_delete(&sb);
	return (error);
}

SYSCTL_PROC(_hw_iokit, OID_AUTO, registry,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0,
    ioreg_sysctl_dump, "A",
    "IOKit registry (live newbus device tree)");

/* ---- id-map maintenance via newbus eventhandlers ---- */

/*
 * device_attach: mint (or revive) the id eagerly so the id space tracks the live
 * tree even before the first walk. Runs in the (sleepable) newbus config
 * context, so the sx xlock is fine.
 */
static eventhandler_tag ioreg_attach_tag;
static eventhandler_tag ioreg_detach_tag;

static void
ioreg_on_attach(void *arg __unused, device_t dev)
{
	sx_xlock(&ioreg_lock);
	(void)ioreg_id_for_locked(dev);
	sx_xunlock(&ioreg_lock);
}

/*
 * device_detach: once the device is actually gone (EVHDEV_DETACH_COMPLETE),
 * keep its id but mark it detached and drop the device_t pointer so it lingers
 * for in-flight userland iterators rather than dangling. BEGIN/FAILED are
 * ignored (the device is still present).
 */
static void
ioreg_on_detach(void *arg __unused, device_t dev, enum evhdev_detach state)
{
	struct ioreg_ent *e;

	if (state != EVHDEV_DETACH_COMPLETE)
		return;
	sx_xlock(&ioreg_lock);
	e = ioreg_find_dev_locked(dev);
	if (e != NULL) {
		e->dev = NULL;
		e->state = IOREG_STATE_DETACHED;
	}
	sx_xunlock(&ioreg_lock);
}

static int
ioreg_modevent(module_t mod __unused, int type, void *data __unused)
{
	struct ioreg_ent *e;

	switch (type) {
	case MOD_LOAD:
		ioreg_dev = make_dev(&ioreg_cdevsw, 0, UID_ROOT, GID_WHEEL,
		    0644, "ioregistry");
		if (ioreg_dev == NULL)
			return (ENXIO);
		ioreg_attach_tag = EVENTHANDLER_REGISTER(device_attach,
		    ioreg_on_attach, NULL, EVENTHANDLER_PRI_ANY);
		ioreg_detach_tag = EVENTHANDLER_REGISTER(device_detach,
		    ioreg_on_detach, NULL, EVENTHANDLER_PRI_ANY);
		return (0);
	case MOD_UNLOAD:
		if (ioreg_attach_tag != NULL)
			EVENTHANDLER_DEREGISTER(device_attach, ioreg_attach_tag);
		if (ioreg_detach_tag != NULL)
			EVENTHANDLER_DEREGISTER(device_detach, ioreg_detach_tag);
		if (ioreg_dev != NULL)
			destroy_dev(ioreg_dev);
		sx_xlock(&ioreg_lock);
		while ((e = LIST_FIRST(&ioreg_map)) != NULL) {
			LIST_REMOVE(e, link);
			free(e, M_IOREG);
		}
		sx_xunlock(&ioreg_lock);
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t ioreg_mod = { "ioregistry", ioreg_modevent, NULL };
DECLARE_MODULE(ioregistry, ioreg_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(ioregistry, 1);
MODULE_DEPEND(ioregistry, iocatalogue, 1, 1, 1);	/* shares hw.iokit node */
