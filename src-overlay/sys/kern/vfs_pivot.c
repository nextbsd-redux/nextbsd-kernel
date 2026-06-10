/*-
 * NextBSD: vfs.pivot -- adopt an already-mounted filesystem as the root /.
 *
 * The FreeBSD analog of Linux pivot_root/switch_root. A tiny mfsroot (initramfs)
 * boots, its /rescue/init assembles a writable overlay in userland --
 *   mdconfig -t vnode the uzip (read-only, on-demand)  -> mount at /rofs
 *   mount -t tmpfs                                      -> /cow  (writable upper)
 *   mount -t unionfs (lower=/rofs, upper=/cow)          -> /newroot
 * -- then `sysctl vfs.pivot=/newroot`. This handler makes that union the real
 * system root, after which /rescue/init `exec`s launchd (PID 1 is preserved
 * across exec, so launchd is PID 1 on the union).
 *
 * The earlier in-kernel overlay hook (reverted, #39/#40) hand-rolled the root
 * swap with pwd_set_rootvnode(), which only repoints CURPROC -- so launchd's
 * forked services kept the OLD root and died (dead Mach ports). The fix is to
 * use the kernel's own mountcheckdirs(), exactly as kern_reroot() does: it walks
 * FOREACH_PROC_IN_SYSTEM and repoints every process's root/cwd, updates prison0,
 * and balances every refcount in one audited function.
 *
 * References (releng/15.0): kern_reroot() sys/kern/kern_shutdown.c:543;
 * set_rootvnode()/vfs_mountroot_shuffle() sys/kern/vfs_mountroot.c:233,304;
 * mountcheckdirs() sys/kern/kern_descrip.c:4380.
 *
 * Carried in nextbsd-kernel (src-overlay + a kernel-build-only files fragment);
 * self-registers a sysctl, so no patch to the freebsd-src fork.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/fcntl.h>			/* AT_FDCWD (used by NDINIT) */
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/jail.h>
#include <sys/priv.h>
#include <sys/filedesc.h>		/* mountcheckdirs() */
#include <sys/vnode.h>

static int
vfs_pivot_to(struct thread *td, const char *path)
{
	struct nameidata nd;
	struct mount *newmp, *oldmp;
	struct vnode *newdp, *olddp;
	int error;

	/*
	 * 1. Resolve the path. The overlay is mounted AT `path`, so namei()
	 *    crosses the mountpoint and returns the union's root vnode
	 *    (referenced + locked).
	 */
	NDINIT(&nd, LOOKUP, FOLLOW | LOCKLEAF, UIO_SYSSPACE, path);
	error = namei(&nd);
	if (error != 0)
		return (error);
	NDFREE_PNBUF(&nd);
	newdp = nd.ni_vp;
	newmp = newdp->v_mount;

	/* Must be the root of its own mount, and not already the rootfs. */
	if ((newdp->v_vflag & VV_ROOT) == 0 ||
	    (newmp->mnt_flag & MNT_ROOTFS) != 0) {
		vput(newdp);
		return (EINVAL);
	}

	/* Pin the target mount across the swap (sleeps past any unmount). */
	error = vfs_busy(newmp, 0);
	if (error != 0) {
		vput(newdp);
		return (error);
	}

	olddp = rootvnode;
	oldmp = olddp->v_mount;

	/*
	 * 2. Establish the "first in mountlist + MNT_ROOTFS" invariant that
	 *    set_rootvnode()/dounmount() rely on: move the union to the head and
	 *    hand MNT_ROOTFS across (cf. vfs_mountroot_shuffle :316-343).
	 */
	mtx_lock(&mountlist_mtx);
	TAILQ_REMOVE(&mountlist, newmp, mnt_list);
	TAILQ_INSERT_HEAD(&mountlist, newmp, mnt_list);
	mtx_unlock(&mountlist_mtx);

	MNT_ILOCK(oldmp);
	oldmp->mnt_flag &= ~MNT_ROOTFS;
	MNT_IUNLOCK(oldmp);
	MNT_ILOCK(newmp);
	newmp->mnt_flag |= MNT_ROOTFS;
	/* Make getfsstat read "<union> on /" -- the mount-table cleanliness that
	 * keeps efibootmgr et al. from ever seeing a /newroot (or /sysroot)
	 * prefix. The component mounts are named /rofs + /cow by /rescue/init. */
	strlcpy(newmp->mnt_stat.f_mntonname, "/", MNAMELEN);
	MNT_IUNLOCK(newmp);

	/*
	 * 3. Assign the global rootvnode the way set_rootvnode() does -- one held
	 *    reference (vfs_mountroot.c:237,240). Set it BEFORE mountcheckdirs so
	 *    its `if (rootvnode == olddp)` self-update branch is skipped (matching
	 *    kern_reroot, where vfs_mountroot set rootvnode first).
	 */
	VOP_UNLOCK(newdp);
	vrefact(newdp);
	rootvnode = newdp;

	/*
	 * 4. THE repoint: migrate every process's root/cwd and prison0/allprison
	 *    roots from olddp -> newdp, with matched refcounts. This is the fix
	 *    for the launchd failure -- forked services now inherit the union root.
	 */
	mountcheckdirs(olddp, newdp);

	/* Drop the OLD global root reference (the boot set_rootvnode ref on the
	 * mfsroot). mountcheckdirs already dropped the per-proc and prison refs. */
	vrele(olddp);

	/* Release the transient namei lookup ref; newdp stays alive via the
	 * global + the per-proc refs mountcheckdirs installed. */
	vrele(newdp);

	/* Purge the namecache for both trees (cf. shuffle :325-327,344). */
	cache_purgevfs(oldmp);
	cache_purgevfs(newmp);

	vfs_unbusy(newmp);

	printf("vfs.pivot: / is now %s (mounted from %s)\n",
	    newmp->mnt_stat.f_fstypename, path);
	return (0);
}

static int
sysctl_vfs_pivot(SYSCTL_HANDLER_ARGS)
{
	struct thread *td = curthread;
	char path[MAXPATHLEN];
	size_t len;
	int error;

	if (req->newptr == NULL)		/* write-only */
		return (EINVAL);
	len = req->newlen;
	if (len == 0 || len >= sizeof(path))
		return (EINVAL);
	error = SYSCTL_IN(req, path, len);
	if (error != 0)
		return (error);
	path[len] = '\0';

	/* Mirror kern_reroot()'s policy: init only, else require privilege. */
	if (curproc != initproc) {
		error = priv_check(td, PRIV_REBOOT);
		if (error != 0)
			return (error);
	}

	return (vfs_pivot_to(td, path));
}

SYSCTL_PROC(_vfs, OID_AUTO, pivot,
    CTLTYPE_STRING | CTLFLAG_WR | CTLFLAG_MPSAFE | CTLFLAG_SECURE,
    NULL, 0, sysctl_vfs_pivot, "A",
    "Adopt the already-mounted filesystem at the given path as the root /");
