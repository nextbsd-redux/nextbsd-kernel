/*-
 * NextBSD: writable overlay for a read-only live root.
 *
 * When the live image's loader.conf sets the kenv `vfs.root.overlay`, stack a
 * writable tmpfs over the read-only root via unionfs so `/` becomes read-write
 * BEFORE init (PID 1 / launchd) is exec'd. The installed system does not set the
 * kenv, so this is a no-op there and `/` stays a plain rw-UFS.
 *
 * Mechanism: register a `mountroot` event handler. EVENTHANDLER_INVOKE(mountroot)
 * fires at the end of vfs_mountroot() (sys/kern/vfs_mountroot.c), which is called
 * synchronously by start_init() (sys/kern/init_main.c:732) -- AFTER the real root
 * is mounted (read-only, MNT_ROOTFS, devfs shuffled to /dev) and BEFORE
 * start_init() execs PID 1. So the overlay exists before any userland runs.
 *
 * We use the high-level kernel_mount() API (as the root mount itself does), NOT
 * the low-level vfs_mount_alloc()/VFS_MOUNT() devfs path: tmpfs and unionfs both
 * dereference mnt_vnodecovered during mount, which the devfs path leaves NULL.
 * Mounting unionfs over "/" makes the covered vnode (the real root) the union's
 * LOWER automatically (sys/fs/unionfs/union_vfsops.c), with the tmpfs as the
 * upper -- exactly the rw-over-ro semantics we want, no `below` option. unionfs
 * refuses to BE MNT_ROOTFS, so we mount it with flags 0; the real root keeps
 * MNT_ROOTFS and the union sits on top.
 *
 * Carried entirely in nextbsd-kernel (src-overlay + a kernel-build-only files
 * fragment); no edit to the freebsd-src fork.  (nextbsd #70 live-root series)
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/eventhandler.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/jail.h>
#include <sys/filedesc.h>
#include <sys/vnode.h>

/*
 * Scratch mountpoint for the tmpfs upper. MUST already exist (empty) in the
 * read-only root image: the kernel cannot mkdir on a RO fs, and tmpfs derives
 * its default owner/mode from VOP_GETATTR(mnt_vnodecovered) at mount time. The
 * NextBSD image build is responsible for creating it.
 */
#define	OVL_MNT	"/.overlay"

static struct mount *
overlay_find_mp(const char *onname, const char *fstype)
{
	struct mount *mp;

	/* kernel_mount() does not hand back the mp; find it by name+type. */
	mtx_lock(&mountlist_mtx);
	TAILQ_FOREACH_REVERSE(mp, &mountlist, mntlist, mnt_list) {
		if (strcmp(mp->mnt_stat.f_mntonname, onname) == 0 &&
		    strcmp(mp->mnt_stat.f_fstypename, fstype) == 0)
			break;
	}
	mtx_unlock(&mountlist_mtx);
	return (mp);
}

static void
overlay_set_ignore(struct mount *mp)
{

	if (mp == NULL)
		return;
	/* MNT_IGNORE => hidden from df(1); leaves one clean "/" line. */
	MNT_ILOCK(mp);
	mp->mnt_flag |= MNT_IGNORE;
	MNT_IUNLOCK(mp);
}

static void
vfs_overlay_mountroot(void *arg __unused)
{
	struct mntarg *ma;
	struct mount *lower_mp, *tmpfs_mp, *union_mp;
	struct vnode *uvp, *oldrootvp;
	struct thread *td = curthread;
	char *val;
	int error;

	val = kern_getenv("vfs.root.overlay");
	if (val == NULL)
		return;			/* not a live image: plain root, no-op */
	freeenv(val);

	/* The real read-only root currently at / (holds MNT_ROOTFS). */
	lower_mp = rootvnode->v_mount;

	/* 1. Writable tmpfs upper at OVL_MNT (must pre-exist in the RO image). */
	ma = mount_arg(NULL, "fstype", "tmpfs", -1);
	ma = mount_arg(ma, "fspath", OVL_MNT, -1);
	ma = mount_arg(ma, "from", "tmpfs", -1);
	ma = mount_arg(ma, "mode", "0755", -1);
	/* size omitted => unlimited (capped by available RAM/swap). */
	error = kernel_mount(ma, 0);
	if (error != 0) {
		printf("vfs.root.overlay: tmpfs mount on %s failed (%d); "
		    "leaving root read-only\n", OVL_MNT, error);
		return;
	}
	tmpfs_mp = overlay_find_mp(OVL_MNT, "tmpfs");

	/* 2. unionfs over /: covered(/) = RO root = lower; target = tmpfs = upper. */
	ma = mount_arg(NULL, "fstype", "unionfs", -1);
	ma = mount_arg(ma, "fspath", "/", -1);
	ma = mount_arg(ma, "target", OVL_MNT, -1);
	error = kernel_mount(ma, 0);		/* flags 0: never MNT_ROOTFS */
	if (error != 0) {
		printf("vfs.root.overlay: unionfs mount over / failed (%d); "
		    "leaving root read-only\n", error);
		(void)kern_unmount(td, OVL_MNT, 0);
		return;
	}
	union_mp = overlay_find_mp("/", "unionfs");
	if (union_mp == NULL) {
		printf("vfs.root.overlay: union mount not found after mount; "
		    "leaving root as-is\n");
		return;
	}

	/* 3. Re-point the global rootvnode at the union root. set_rootvnode()
	 *    would pick TAILQ_FIRST (the lower), so do it explicitly. */
	error = VFS_ROOT(union_mp, LK_EXCLUSIVE, &uvp);
	if (error != 0) {
		printf("vfs.root.overlay: VFS_ROOT(union) failed (%d)\n", error);
		(void)kern_unmount(td, "/", 0);	/* union is not MNT_ROOTFS */
		(void)kern_unmount(td, OVL_MNT, 0);
		return;
	}
	VOP_UNLOCK(uvp);

	oldrootvp = rootvnode;
	rootvnode = uvp;		/* the VFS_ROOT ref becomes the global ref */
	pwd_set_rootvnode();		/* refresh curproc's root from rootvnode */
	cache_purge(uvp);

	/*
	 * vfs_mountroot() already set prison0.pr_root to the OLD root (it runs
	 * before this event handler). Re-sync it to the union, or every process
	 * (all are in prison0) would resolve / through the RO lower.
	 */
	mtx_lock(&prison0.pr_mtx);
	if (prison0.pr_root != NULL)
		vrele(prison0.pr_root);
	prison0.pr_root = uvp;
	vref(uvp);
	mtx_unlock(&prison0.pr_mtx);

	if (oldrootvp != NULL)
		vrele(oldrootvp);

	/* 4. Hide the RO lower + tmpfs from df; lower keeps MNT_ROOTFS. */
	overlay_set_ignore(lower_mp);
	overlay_set_ignore(tmpfs_mp);

	printf("vfs.root.overlay: / is now read-write "
	    "(unionfs: tmpfs upper over %s lower)\n",
	    lower_mp->mnt_stat.f_fstypename);
}

/*
 * Register the handler before vfs_mountroot() runs. SI_SUB_VFS is well before
 * start_init()/SI_SUB_KTHREAD_INIT, and the eventhandler subsystem is up by
 * then. EVENTHANDLER_PRI_FIRST so we run ahead of any other mountroot consumer.
 */
static void
vfs_overlay_register(void *arg __unused)
{

	EVENTHANDLER_REGISTER(mountroot, vfs_overlay_mountroot, NULL,
	    EVENTHANDLER_PRI_FIRST);
}
SYSINIT(vfs_root_overlay, SI_SUB_VFS, SI_ORDER_ANY, vfs_overlay_register, NULL);
