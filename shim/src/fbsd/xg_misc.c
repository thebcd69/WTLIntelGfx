/*
 * XeGfx: the FreeBSD kernel odds and ends drm-kmod and LinuxKPI need on XNU: a private sysctl tree with tunables
 * from boot-args, sbuf, qsort, counters, sglists, group taskqueues, modules, firmware from the kext's Resources,
 * select/knlist plumbing, and no-op debugfs, vt and vnode leftovers.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sx.h>
#include <sys/proc.h>
#include <sys/smp.h>
#include <sys/pcpu.h>
#include <sys/limits.h>
#include <sys/sysctl.h>
#include <sys/sbuf.h>
#include <sys/selinfo.h>
#include <sys/event.h>
#include <sys/sglist.h>
#include <sys/counter.h>
#include <sys/bus.h>
#include <sys/firmware.h>
#include <sys/domainset.h>
#include <sys/taskqueue.h>
#include <sys/gtaskqueue.h>
#include <sys/linker.h>
#include <sys/module.h>
#include <sys/conf.h>
#include <sys/file.h>
#include <sys/filio.h>
#include <sys/sigio.h>
#include <sys/vnode.h>
#include <sys/time.h>
#include <sys/priority.h>
#include <sys/reboot.h>
#include <sys/fbio.h>
#include <machine/atomic.h>
#include <machine/stdarg.h>
#include <vm/vm.h>

#include <dev/pci/pcivar.h>
#include <dev/vt/vt.h>
#include <dev/vt/hw/fb/vt_fb.h>
#include <dev/vt/colors/vt_termcolors.h>
#include "xg_xnu.h"
#include "xg_misc.h"
#include "xg_sched.h"
#include "xg_fd.h"

void xg_unimpl(const char *name) __attribute__((__noreturn__));

static MALLOC_DEFINE(M_XGMISC, "xgmisc", "XeGfx misc shim");

/* ---- one-time setup ---- */

static struct sx xg_sysctl_lock;
static struct sx xg_mod_lock;
static struct sx xg_fw_lock;
static struct mtx xg_dev_mtx;
static struct mtx xg_knlist_mtx;
static struct mtx xg_gtq_mtx;
static volatile u_int xg_misc_state;	/* 0 not started, 1 running, 2 done */

static void xg_sysctl_register_static(void);

void
xg_misc_init(void)
{
	if (atomic_load_acq_int(&xg_misc_state) == 2)
		return;
	if (!atomic_cmpset_int(&xg_misc_state, 0, 1)) {
		/* someone else is setting up; it may sleep in malloc, so yield instead of spinning hard */
		while (atomic_load_acq_int(&xg_misc_state) != 2)
			thread_block(NULL);
		return;
	}
	sx_init(&xg_sysctl_lock, "sysctl lock");
	sx_init(&xg_mod_lock, "module lock");
	sx_init(&xg_fw_lock, "firmware lock");
	mtx_init(&xg_dev_mtx, "cdev", NULL, MTX_DEF);
	mtx_init(&xg_knlist_mtx, "knlist lock for lockless objects", NULL, MTX_DEF);
	mtx_init(&xg_gtq_mtx, "taskqgroup", NULL, MTX_DEF);
	xg_sysctl_register_static();
	atomic_store_rel_int(&xg_misc_state, 2);
}

/* ---- sysctl: a private in-memory tree, no syscall reaches it ---- */

/*
 * XNU exports sysctl__children and sysctl_handle_{int,long,string,opaque} with its own struct layouts; defining the
 * FreeBSD versions here makes the kext link bind to these instead.
 */
struct sysctl_oid_list sysctl__children = RB_INITIALIZER(&sysctl__children);
/* sys/tree.h trips -Wconditional-uninitialized in its remove path; the code is FreeBSD's own */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconditional-uninitialized"
RB_GENERATE(sysctl_oid_list, sysctl_oid, oid_link, cmp_sysctl_oid);
#pragma clang diagnostic pop

SYSCTL_ROOT_NODE(CTL_HW, hw, CTLFLAG_RW | CTLFLAG_MPSAFE, 0, "hardware");
SYSCTL_ROOT_NODE(OID_AUTO, dev, CTLFLAG_RW | CTLFLAG_MPSAFE, 0, "devices");
SYSCTL_ROOT_NODE(OID_AUTO, compat, CTLFLAG_RW | CTLFLAG_MPSAFE, 0, "compatibility code");

#define	SYSCTL_WLOCK()		sx_xlock(&xg_sysctl_lock)
#define	SYSCTL_WUNLOCK()	sx_xunlock(&xg_sysctl_lock)

static struct sysctl_oid *
sysctl_find_oidname(const char *name, struct sysctl_oid_list *list)
{
	struct sysctl_oid *oidp;

	RB_FOREACH(oidp, sysctl_oid_list, list)
		if (strcmp(oidp->oid_name, name) == 0)
			return (oidp);
	return (NULL);
}

static int
sysctl_old_kernel(struct sysctl_req *req, const void *p, size_t l)
{
	size_t i = 0;

	if (req->oldptr) {
		i = l;
		if (req->oldlen <= req->oldidx)
			i = 0;
		else if (i > req->oldlen - req->oldidx)
			i = req->oldlen - req->oldidx;
		if (i > 0)
			memcpy((char *)req->oldptr + req->oldidx, p, i);
	}
	req->oldidx += l;
	if (req->oldptr && i != l)
		return (ENOMEM);
	return (0);
}

static int
sysctl_new_kernel(struct sysctl_req *req, void *p, size_t l)
{
	if (!req->newptr)
		return (0);
	if (req->newlen - req->newidx < l)
		return (EINVAL);
	memcpy(p, (const char *)req->newptr + req->newidx, l);
	req->newidx += l;
	return (0);
}

/*
 * A tunable is read from boot-args under the oid's full dotted name ("hw." dropped, see xg_kern.c), so
 * compat.linuxkpi.i915_enable_guc=3 sets i915's enable_guc. Numbers go through getenv_long because
 * PE_parse_boot_argn stores numeric values in binary.
 */
static void
sysctl_load_tunable_by_oid_locked(struct sysctl_oid *oidp)
{
	struct sysctl_req req;
	struct sysctl_oid *curr;
	char path[96];
	ssize_t rem = sizeof(path), len;
	uint64_t data;
	char *penv = NULL;
	long v;
	int size, error;

	path[--rem] = 0;
	for (curr = oidp; curr != NULL; curr = SYSCTL_PARENT(curr)) {
		len = strlen(curr->oid_name);
		rem -= len;
		if (curr != oidp)
			rem -= 1;
		if (rem < 0) {
			printf("OID path exceeds %d bytes\n", (int)sizeof(path));
			return;
		}
		memcpy(path + rem, curr->oid_name, len);
		if (curr != oidp)
			path[rem + len] = '.';
	}
	memset(&req, 0, sizeof(req));
	req.td = curthread;
	req.oldfunc = sysctl_old_kernel;
	req.newfunc = sysctl_new_kernel;
	req.lock = REQ_UNWIRED;
	data = 0;
	switch (oidp->oid_kind & CTLTYPE) {
	case CTLTYPE_INT: case CTLTYPE_UINT: case CTLTYPE_S32: case CTLTYPE_U32:
		size = sizeof(int32_t);
		break;
	case CTLTYPE_LONG: case CTLTYPE_ULONG: case CTLTYPE_S64: case CTLTYPE_U64:
		size = sizeof(int64_t);
		break;
	case CTLTYPE_S16: case CTLTYPE_U16:
		size = sizeof(int16_t);
		break;
	case CTLTYPE_S8: case CTLTYPE_U8:
		size = sizeof(int8_t);
		break;
	case CTLTYPE_STRING:
		size = 0;
		break;
	default:
		return;
	}
	if (size != 0) {
		if (!getenv_long(path + rem, &v))
			return;
		/* little endian: the low bytes of the 64-bit value are the narrow value */
		data = (uint64_t)v;
		req.newptr = &data;
		req.newlen = size;
	} else {
		penv = kern_getenv(path + rem);
		if (penv == NULL)
			return;
		req.newptr = penv;
		req.newlen = strlen(penv);
	}
	error = oidp->oid_handler(oidp, oidp->oid_arg1, oidp->oid_arg2, &req);
	if (error != 0)
		printf("Setting sysctl %s failed: %d\n", path + rem, error);
	if (penv != NULL)
		freeenv(penv);
}

static void
sysctl_register_oid_locked(struct sysctl_oid *oidp)
{
	struct sysctl_oid_list *parent = oidp->oid_parent;
	struct sysctl_oid *p, key;
	static int newoid;
	int oid_number, timeout = 2;

	p = sysctl_find_oidname(oidp->oid_name, parent);
	if (p != NULL) {
		if ((p->oid_kind & CTLTYPE) == CTLTYPE_NODE)
			p->oid_refcnt++;
		else
			printf("can't re-use a leaf (%s)!\n", p->oid_name);
		return;
	}
	oid_number = oidp->oid_number;
	if (oid_number < 0) {
		/* counting down keeps auto numbers at the end of the sorted list */
		if (--newoid < CTL_AUTO_START)
			newoid = 0x7fffffff;
		oid_number = newoid;
	}
	key.oid_number = oid_number;
	p = RB_NFIND(sysctl_oid_list, parent, &key);
	while (p != NULL && oid_number == p->oid_number) {
		if (oid_number < CTL_AUTO_START || oid_number == 0x7fffffff) {
			oid_number = CTL_AUTO_START;
			if (!timeout--)
				panic("sysctl: Out of OID numbers\n");
			key.oid_number = oid_number;
			p = RB_NFIND(sysctl_oid_list, parent, &key);
			continue;
		}
		p = RB_NEXT(sysctl_oid_list, NULL, p);
		oid_number++;
	}
	if (oidp->oid_number >= 0 && oidp->oid_number < CTL_AUTO_START && oid_number >= CTL_AUTO_START)
		panic("sysctl: OID number(%d) is already in use for '%s'\n", oidp->oid_number, oidp->oid_name);
	oidp->oid_number = oid_number;
	RB_INSERT(sysctl_oid_list, parent, oidp);
	if ((oidp->oid_kind & CTLTYPE) != CTLTYPE_NODE && (oidp->oid_kind & CTLFLAG_TUN) != 0 &&
	    (oidp->oid_kind & CTLFLAG_NOFETCH) == 0) {
		oidp->oid_kind |= CTLFLAG_NOFETCH;
		sysctl_load_tunable_by_oid_locked(oidp);
	}
}

static void
sysctl_unregister_oid_locked(struct sysctl_oid *oidp)
{
	int error;

	if (oidp->oid_number == OID_AUTO)
		error = EINVAL;
	else
		error = RB_REMOVE(sysctl_oid_list, oidp->oid_parent, oidp) != NULL ? 0 : ENOENT;
	if (error)
		printf("%s: failed(%d) to unregister sysctl(%s)\n", __func__, error, oidp->oid_name);
}

void
sysctl_register_oid(struct sysctl_oid *oidp)
{
	xg_misc_init();
	SYSCTL_WLOCK();
	sysctl_register_oid_locked(oidp);
	SYSCTL_WUNLOCK();
}

void
sysctl_unregister_oid(struct sysctl_oid *oidp)
{
	xg_misc_init();
	SYSCTL_WLOCK();
	sysctl_unregister_oid_locked(oidp);
	SYSCTL_WUNLOCK();
}

/*
 * Static oids sit in the linker set walked here (sys/linker_set.h puts every set in __DATA,__xg_set). ld64 defines
 * the section bounds for us.
 */
extern const struct xg_set_entry xg_misc_set_start[] __asm("section$start$__DATA$__xg_set");
extern const struct xg_set_entry xg_misc_set_end[] __asm("section$end$__DATA$__xg_set");

static void
xg_sysctl_register_static(void)
{
	const struct xg_set_entry *e;

	SYSCTL_WLOCK();
	for (e = xg_misc_set_start; e < xg_misc_set_end; e++)
		if (strcmp(e->set, "sysctl_set") == 0)
			sysctl_register_oid_locked(__DECONST(struct sysctl_oid *, e->sym));
	SYSCTL_WUNLOCK();
}

/*
 * The static tunables again, from the environment as it is now. They are fetched once, when stage 1 registers the
 * static oids; the GU605 display path changes debug.xegfx_env at stage 4 (xg_kms_decide: i915_disable_display=0,
 * i915_enable_psr=0, ...), and without this i915 never saw those values: on 2026-11-10 its display code ran with
 * disable_display=1 (every connector "disconnected", no EDID) and with PSR, FBC and the DC states at their defaults,
 * so the panel froze on its last frame a second after the takeover while macOS kept drawing into the framebuffer.
 * A tunable with no value in the environment or the boot-args keeps what it has. Returns how many were looked at.
 */
int
xg_sysctl_reload_tunables(void)
{
	const struct xg_set_entry *e;
	int n = 0;

	xg_misc_init();
	SYSCTL_WLOCK();
	for (e = xg_misc_set_start; e < xg_misc_set_end; e++) {
		struct sysctl_oid *oidp;

		if (strcmp(e->set, "sysctl_set") != 0)
			continue;
		oidp = __DECONST(struct sysctl_oid *, e->sym);
		if ((oidp->oid_kind & CTLTYPE) == CTLTYPE_NODE || (oidp->oid_kind & CTLFLAG_TUN) == 0)
			continue;
		sysctl_load_tunable_by_oid_locked(oidp);
		n++;
	}
	SYSCTL_WUNLOCK();
	return (n);
}

int
sysctl_ctx_init(struct sysctl_ctx_list *c)
{
	if (c == NULL)
		return (EINVAL);
	TAILQ_INIT(c);
	return (0);
}

static struct sysctl_ctx_entry *
sysctl_ctx_entry_add_locked(struct sysctl_ctx_list *clist, struct sysctl_oid *oidp)
{
	struct sysctl_ctx_entry *e;

	if (clist == NULL || oidp == NULL)
		return (NULL);
	e = malloc(sizeof(*e), M_XGMISC, M_WAITOK);
	e->entry = oidp;
	TAILQ_INSERT_HEAD(clist, e, link);
	return (e);
}

static int
sysctl_remove_oid_locked(struct sysctl_oid *oidp, int del, int recurse)
{
	struct sysctl_oid *p, *tmp;
	int error;

	if (oidp == NULL)
		return (EINVAL);
	if ((oidp->oid_kind & CTLFLAG_DYN) == 0) {
		printf("Warning: can't remove non-dynamic nodes (%s)!\n", oidp->oid_name);
		return (EINVAL);
	}
	if ((oidp->oid_kind & CTLTYPE) == CTLTYPE_NODE && oidp->oid_refcnt == 1) {
		for (p = RB_MIN(sysctl_oid_list, &oidp->oid_children); p != NULL; p = tmp) {
			if (!recurse) {
				printf("Warning: failed attempt to remove oid %s with child %s\n", oidp->oid_name,
				    p->oid_name);
				return (ENOTEMPTY);
			}
			tmp = RB_NEXT(sysctl_oid_list, &oidp->oid_children, p);
			error = sysctl_remove_oid_locked(p, del, recurse);
			if (error)
				return (error);
		}
	}
	if (oidp->oid_refcnt > 1) {
		oidp->oid_refcnt--;
		return (0);
	}
	if (oidp->oid_refcnt == 0) {
		printf("Warning: bad oid_refcnt=%u (%s)!\n", oidp->oid_refcnt, oidp->oid_name);
		return (EINVAL);
	}
	sysctl_unregister_oid_locked(oidp);
	if (del) {
		/* oid_running never rises: nothing calls handlers from outside, so there is nobody to drain */
		if (oidp->oid_descr)
			free(__DECONST(char *, oidp->oid_descr), M_XGMISC);
		if (oidp->oid_label)
			free(__DECONST(char *, oidp->oid_label), M_XGMISC);
		free(__DECONST(char *, oidp->oid_name), M_XGMISC);
		free(oidp, M_XGMISC);
	}
	return (0);
}

int
sysctl_ctx_free(struct sysctl_ctx_list *clist)
{
	struct sysctl_ctx_entry *e, *e1;
	int error = 0;

	xg_misc_init();
	/* dry run first, as FreeBSD does: deregister everything, put it all back if one refuses */
	SYSCTL_WLOCK();
	TAILQ_FOREACH(e, clist, link) {
		error = sysctl_remove_oid_locked(e->entry, 0, 0);
		if (error)
			break;
	}
	if (error)
		e1 = TAILQ_PREV(e, sysctl_ctx_list, link);
	else
		e1 = TAILQ_LAST(clist, sysctl_ctx_list);
	while (e1 != NULL) {
		sysctl_register_oid_locked(e1->entry);
		e1 = TAILQ_PREV(e1, sysctl_ctx_list, link);
	}
	if (error) {
		SYSCTL_WUNLOCK();
		return (EBUSY);
	}
	TAILQ_FOREACH_SAFE(e, clist, link, e1) {
		error = sysctl_remove_oid_locked(e->entry, 1, 0);
		if (error)
			panic("sysctl_remove_oid: corrupt tree, entry: %s", e->entry->oid_name);
		free(e, M_XGMISC);
	}
	SYSCTL_WUNLOCK();
	TAILQ_INIT(clist);
	return (error);
}

int
sysctl_remove_oid(struct sysctl_oid *oidp, int del, int recurse)
{
	int error;

	xg_misc_init();
	SYSCTL_WLOCK();
	error = sysctl_remove_oid_locked(oidp, del, recurse);
	SYSCTL_WUNLOCK();
	return (error);
}

int
sysctl_remove_name(struct sysctl_oid *parent, const char *name, int del, int recurse)
{
	struct sysctl_oid *p;
	int error = ENOENT;

	xg_misc_init();
	SYSCTL_WLOCK();
	p = sysctl_find_oidname(name, &parent->oid_children);
	if (p)
		error = sysctl_remove_oid_locked(p, del, recurse);
	SYSCTL_WUNLOCK();
	return (error);
}

/* '.' separates levels, so a name containing one is stored with it escaped as "%25" */
static char *
sysctl_escape_name(const char *orig)
{
	int i, s = 0, d = 0, nillegals = 0;
	char *new;

	for (i = 0; orig[i] != '\0'; i++)
		if (orig[i] == '.')
			nillegals++;
	new = malloc(i + 2 * nillegals + 1, M_XGMISC, M_WAITOK);
	while (orig[s] != '\0') {
		if (orig[s] == '.') {
			new[d++] = '%';
			new[d++] = '2';
			new[d++] = '5';
			s++;
		} else
			new[d++] = orig[s++];
	}
	new[d] = '\0';
	return (new);
}

struct sysctl_oid *
sysctl_add_oid(struct sysctl_ctx_list *clist, struct sysctl_oid_list *parent, int number, const char *name,
    int kind, void *arg1, intmax_t arg2, int (*handler)(SYSCTL_HANDLER_ARGS), const char *fmt, const char *descr,
    const char *label)
{
	struct sysctl_oid *oidp;
	char *escaped;

	if (parent == NULL)
		return (NULL);
	xg_misc_init();
	escaped = sysctl_escape_name(name);
	SYSCTL_WLOCK();
	oidp = sysctl_find_oidname(escaped, parent);
	if (oidp != NULL) {
		free(escaped, M_XGMISC);
		if ((oidp->oid_kind & CTLTYPE) == CTLTYPE_NODE) {
			oidp->oid_refcnt++;
			if (clist != NULL)
				sysctl_ctx_entry_add_locked(clist, oidp);
			SYSCTL_WUNLOCK();
			return (oidp);
		}
		printf("%s: can't re-use a leaf (%s)!\n", __func__, oidp->oid_name);
		SYSCTL_WUNLOCK();
		return (NULL);
	}
	oidp = malloc(sizeof(*oidp), M_XGMISC, M_WAITOK | M_ZERO);
	oidp->oid_parent = parent;
	RB_INIT(&oidp->oid_children);
	oidp->oid_number = number;
	oidp->oid_refcnt = 1;
	oidp->oid_name = escaped;
	oidp->oid_handler = handler;
	oidp->oid_kind = CTLFLAG_DYN | kind;
	oidp->oid_arg1 = arg1;
	oidp->oid_arg2 = arg2;
	oidp->oid_fmt = fmt;
	if (descr != NULL)
		oidp->oid_descr = strdup(descr, M_XGMISC);
	if (label != NULL)
		oidp->oid_label = strdup(label, M_XGMISC);
	if (clist != NULL)
		sysctl_ctx_entry_add_locked(clist, oidp);
	sysctl_register_oid_locked(oidp);
	SYSCTL_WUNLOCK();
	return (oidp);
}

int
sysctl_handle_bool(SYSCTL_HANDLER_ARGS)
{
	uint8_t temp;
	int error, temp_int;

	temp = arg1 ? (*(bool *)arg1 ? 1 : 0) : (arg2 ? 1 : 0);
	/* exactly 4 bytes left means the caller expects an int, kept for old integer knobs */
	if (req->oldlen - req->oldidx == sizeof(int)) {
		temp_int = temp;
		error = SYSCTL_OUT(req, &temp_int, sizeof(temp_int));
	} else
		error = SYSCTL_OUT(req, &temp, sizeof(temp));
	if (error || !req->newptr)
		return (error);
	if (!arg1)
		return (EPERM);
	if (req->newlen - req->newidx == sizeof(int)) {
		error = SYSCTL_IN(req, &temp_int, sizeof(temp_int));
		if (error == 0)
			*(bool *)arg1 = temp_int != 0;
	} else {
		error = SYSCTL_IN(req, &temp, sizeof(temp));
		if (error == 0)
			*(bool *)arg1 = temp != 0;
	}
	return (error);
}

int
sysctl_handle_int(SYSCTL_HANDLER_ARGS)
{
	int tmpout, error;

	tmpout = arg1 ? *(int *)arg1 : (int)arg2;
	error = SYSCTL_OUT(req, &tmpout, sizeof(int));
	if (error || !req->newptr)
		return (error);
	if (!arg1)
		return (EPERM);
	return (SYSCTL_IN(req, arg1, sizeof(int)));
}

int
sysctl_handle_long(SYSCTL_HANDLER_ARGS)
{
	long tmplong;
	int error;

	tmplong = arg1 ? *(long *)arg1 : (long)arg2;
	error = SYSCTL_OUT(req, &tmplong, sizeof(long));
	if (error || !req->newptr)
		return (error);
	if (!arg1)
		return (EPERM);
	return (SYSCTL_IN(req, arg1, sizeof(long)));
}

int
sysctl_handle_64(SYSCTL_HANDLER_ARGS)
{
	uint64_t tmpout;
	int error;

	tmpout = arg1 ? *(uint64_t *)arg1 : (uint64_t)arg2;
	error = SYSCTL_OUT(req, &tmpout, sizeof(uint64_t));
	if (error || !req->newptr)
		return (error);
	if (!arg1)
		return (EPERM);
	return (SYSCTL_IN(req, arg1, sizeof(uint64_t)));
}

/* no sysctl syscall reaches this tree, so the string lock FreeBSD takes against concurrent readers is not needed */
int
sysctl_handle_string(SYSCTL_HANDLER_ARGS)
{
	size_t outlen;
	int error;

	if (arg2 == 0)
		outlen = arg2 = strlen(arg1) + 1;
	else
		outlen = strnlen(arg1, arg2 - 1) + 1;
	error = SYSCTL_OUT(req, req->oldptr != NULL ? arg1 : NULL, outlen);
	if (error || !req->newptr)
		return (error);
	if (req->newlen - req->newidx >= (size_t)arg2)
		return (EINVAL);
	arg2 = req->newlen - req->newidx;
	if (arg2 == 0) {
		((char *)arg1)[0] = '\0';
		return (0);
	}
	error = SYSCTL_IN(req, arg1, arg2);
	if (error == 0)
		((char *)arg1)[arg2] = '\0';
	return (error);
}

int
sysctl_handle_opaque(SYSCTL_HANDLER_ARGS)
{
	int error;

	error = SYSCTL_OUT(req, arg1, arg2);
	if (error)
		return (error);
	return (SYSCTL_IN(req, arg1, arg2));
}

/* ---- counter(9): one 64-bit cell per counter, see docs/xg_misc.md for the header change it needs ---- */

counter_u64_t
counter_u64_alloc(int flags)
{
	return (malloc(sizeof(uint64_t), M_XGMISC, (flags & (M_WAITOK | M_NOWAIT)) | M_ZERO));
}

void
counter_u64_free(counter_u64_t c)
{
	free(c, M_XGMISC);
}

int
sysctl_handle_counter_u64(SYSCTL_HANDLER_ARGS)
{
	volatile uint64_t *c = *(counter_u64_t *)arg1;
	uint64_t out;
	int error;

	out = atomic_load_64(c);
	error = SYSCTL_OUT(req, &out, sizeof(uint64_t));
	if (error || !req->newptr)
		return (error);
	/* any write zeroes it, as on FreeBSD */
	atomic_store_64(c, 0);
	return (0);
}

/* ---- sbuf(9): XNU exports an sbuf with a different layout, so the FreeBSD one lives here ---- */

#define	SBUF_MINSIZE		2
#define	SBUF_MINEXTENDSIZE	16
#define	SBUF_MAXEXTENDSIZE	PAGE_SIZE
#define	SBUF_MAXEXTENDINCR	PAGE_SIZE
#define	SBUF_FREESPACE(s)	((s)->s_size - ((s)->s_len + 1))

static int
sbuf_extendsize(int size)
{
	int newsize = SBUF_MINEXTENDSIZE;

	while (newsize < size) {
		if (newsize < (int)SBUF_MAXEXTENDSIZE)
			newsize *= 2;
		else
			newsize += SBUF_MAXEXTENDINCR;
	}
	return (newsize);
}

static int
sbuf_extend(struct sbuf *s, int addlen)
{
	char *newbuf;
	int newsize;

	if ((s->s_flags & SBUF_AUTOEXTEND) == 0)
		return (-1);
	newsize = sbuf_extendsize(s->s_size + addlen);
	newbuf = malloc(newsize, M_XGMISC, (s->s_flags & SBUF_NOWAIT) ? M_NOWAIT : M_WAITOK);
	if (newbuf == NULL)
		return (-1);
	memcpy(newbuf, s->s_buf, s->s_size);
	if (s->s_flags & SBUF_DYNAMIC)
		free(s->s_buf, M_XGMISC);
	else
		s->s_flags |= SBUF_DYNAMIC;
	s->s_buf = newbuf;
	s->s_size = newsize;
	return (0);
}

struct sbuf *
sbuf_new(struct sbuf *s, char *buf, int length, int flags)
{
	flags &= SBUF_USRFLAGMSK;
	if (s == NULL) {
		s = malloc(sizeof(*s), M_XGMISC, ((flags & SBUF_NOWAIT) ? M_NOWAIT : M_WAITOK) | M_ZERO);
		if (s == NULL)
			return (NULL);
		s->s_flags = SBUF_DYNSTRUCT;
	} else
		memset(s, 0, sizeof(*s));
	s->s_flags |= flags;
	s->s_size = length;
	s->s_buf = buf;
	if (s->s_buf != NULL)
		return (s);
	if (flags & SBUF_AUTOEXTEND)
		s->s_size = sbuf_extendsize(s->s_size);
	if (s->s_size < SBUF_MINSIZE)
		s->s_size = SBUF_MINSIZE;
	s->s_buf = malloc(s->s_size, M_XGMISC, (flags & SBUF_NOWAIT) ? M_NOWAIT : M_WAITOK);
	if (s->s_buf == NULL) {
		if (s->s_flags & SBUF_DYNSTRUCT)
			free(s, M_XGMISC);
		return (NULL);
	}
	s->s_flags |= SBUF_DYNAMIC;
	return (s);
}

void
sbuf_clear(struct sbuf *s)
{
	s->s_flags &= ~SBUF_FINISHED;
	s->s_error = 0;
	s->s_len = 0;
	s->s_rec_off = 0;
	s->s_sect_len = 0;
}

int
sbuf_setpos(struct sbuf *s, ssize_t pos)
{
	if (pos < 0 || pos > s->s_len)
		return (-1);
	s->s_len = pos;
	s->s_flags &= ~SBUF_FINISHED;
	return (0);
}

static void
sbuf_put_bytes(struct sbuf *s, const char *buf, size_t len)
{
	while (len > 0 && s->s_error == 0) {
		if (SBUF_FREESPACE(s) <= 0 && sbuf_extend(s, len) < 0) {
			s->s_error = ENOMEM;
			break;
		}
		ssize_t n = MIN((ssize_t)len, SBUF_FREESPACE(s));
		memcpy(s->s_buf + s->s_len, buf, n);
		s->s_len += n;
		if (s->s_flags & SBUF_INSECTION)
			s->s_sect_len += n;
		buf += n;
		len -= n;
	}
}

int
sbuf_bcat(struct sbuf *s, const void *buf, size_t len)
{
	if (s->s_error != 0)
		return (-1);
	sbuf_put_bytes(s, buf, len);
	return (s->s_error != 0 ? -1 : 0);
}

int
sbuf_bcpy(struct sbuf *s, const void *buf, size_t len)
{
	sbuf_clear(s);
	return (sbuf_bcat(s, buf, len));
}

int
sbuf_cat(struct sbuf *s, const char *str)
{
	return (sbuf_bcat(s, str, strlen(str)));
}

int
sbuf_cpy(struct sbuf *s, const char *str)
{
	sbuf_clear(s);
	return (sbuf_cat(s, str));
}

int
sbuf_putc(struct sbuf *s, int c)
{
	char ch = (char)c;

	return (sbuf_bcat(s, &ch, 1));
}

int
sbuf_vprintf(struct sbuf *s, const char *fmt, va_list ap)
{
	va_list ap2;
	int len;

	if (s->s_error != 0)
		return (-1);
	if (SBUF_FREESPACE(s) < 0) {
		s->s_error = ENOMEM;
		return (-1);
	}
	/* XNU's vsnprintf returns the full length, so measure first and grow once */
	va_copy(ap2, ap);
	len = vsnprintf(NULL, 0, fmt, ap2);
	va_end(ap2);
	if (len < 0) {
		s->s_error = EINVAL;
		return (-1);
	}
	if (SBUF_FREESPACE(s) < len && sbuf_extend(s, len - SBUF_FREESPACE(s)) < 0) {
		/* fixed buffer: keep what fits, as FreeBSD does, and flag the overflow */
		vsnprintf(s->s_buf + s->s_len, SBUF_FREESPACE(s) + 1, fmt, ap);
		s->s_len += SBUF_FREESPACE(s);
		s->s_error = ENOMEM;
		return (-1);
	}
	vsnprintf(s->s_buf + s->s_len, SBUF_FREESPACE(s) + 1, fmt, ap);
	s->s_len += len;
	if (s->s_flags & SBUF_INSECTION)
		s->s_sect_len += len;
	return (0);
}

int
sbuf_printf(struct sbuf *s, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = sbuf_vprintf(s, fmt, ap);
	va_end(ap);
	return (r);
}

int
sbuf_finish(struct sbuf *s)
{
	s->s_buf[s->s_len] = '\0';
	if (s->s_flags & SBUF_INCLUDENUL)
		s->s_len++;
	s->s_flags |= SBUF_FINISHED;
	return (s->s_error);
}

char *
sbuf_data(struct sbuf *s)
{
	return (s->s_buf);
}

ssize_t
sbuf_len(struct sbuf *s)
{
	if (s->s_error != 0)
		return (-1);
	return (s->s_len);
}

int
sbuf_error(const struct sbuf *s)
{
	return (s->s_error);
}

int
sbuf_done(const struct sbuf *s)
{
	return ((s->s_flags & SBUF_FINISHED) != 0);
}

void
sbuf_delete(struct sbuf *s)
{
	int isdyn;

	if (s->s_flags & SBUF_DYNAMIC)
		free(s->s_buf, M_XGMISC);
	isdyn = s->s_flags & SBUF_DYNSTRUCT;
	memset(s, 0, sizeof(*s));
	if (isdyn)
		free(s, M_XGMISC);
}

/* ---- libkern leftovers ---- */

void
explicit_bzero(void *b, size_t len)
{
	memset(b, 0, len);
	/* keeps the compiler from dropping the store as dead */
	__asm __volatile("" : : "r"(b) : "memory");
}

static void
xg_swap(char *a, char *b, size_t size)
{
	char t;

	while (size-- > 0) {
		t = *a;
		*a++ = *b;
		*b++ = t;
	}
}

/* heapsort: O(n log n), in place, no recursion on a 16 KB kernel stack */
static void
xg_heapsort(void *base, size_t nmemb, size_t size, int (*cmp)(const void *, const void *, void *), void *thunk)
{
	char *a = base;
	size_t start, end, root, child;

	if (nmemb < 2 || size == 0)
		return;
	for (start = nmemb / 2; start-- > 0;) {
		for (root = start; (child = 2 * root + 1) < nmemb; root = child) {
			if (child + 1 < nmemb && cmp(a + child * size, a + (child + 1) * size, thunk) < 0)
				child++;
			if (cmp(a + root * size, a + child * size, thunk) >= 0)
				break;
			xg_swap(a + root * size, a + child * size, size);
		}
	}
	for (end = nmemb - 1; end > 0; end--) {
		xg_swap(a, a + end * size, size);
		for (root = 0; (child = 2 * root + 1) < end; root = child) {
			if (child + 1 < end && cmp(a + child * size, a + (child + 1) * size, thunk) < 0)
				child++;
			if (cmp(a + root * size, a + child * size, thunk) >= 0)
				break;
			xg_swap(a + root * size, a + child * size, size);
		}
	}
}

static int
xg_qsort_plain(const void *x, const void *y, void *thunk)
{
	return (((int (*)(const void *, const void *))thunk)(x, y));
}

void
qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *))
{
	xg_heapsort(base, nmemb, size, xg_qsort_plain, (void *)compar);
}

void
qsort_r(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *, void *), void *thunk)
{
	xg_heapsort(base, nmemb, size, compar, thunk);
}

int
eventratecheck(struct timeval *lasttime, int *cureps, int maxeps)
{
	int now = ticks;

	/* tv_sec holds a tick count here, as on FreeBSD */
	if (lasttime->tv_sec == 0 || (u_int)(now - lasttime->tv_sec) >= (u_int)hz) {
		lasttime->tv_sec = now;
		*cureps = 1;
		return (maxeps != 0);
	}
	(*cureps)++;
	return (maxeps < 0 || *cureps <= maxeps);
}

int
kcmp_cmp(uintptr_t a, uintptr_t b)
{
	if (a == b)
		return (0);
	return (a < b ? 1 : 2);
}

int
doadump(boolean_t textdump)
{
	printf("Cannot dump: no dump device specified.\n");
	return (ENXIO);
}

/* ---- sglist(9) ---- */

struct sglist *
sglist_alloc(int nsegs, int mflags)
{
	struct sglist *sg;

	sg = malloc(sizeof(*sg) + nsegs * sizeof(struct sglist_seg), M_XGMISC, mflags);
	if (sg == NULL)
		return (NULL);
	sglist_init(sg, (u_short)nsegs, (struct sglist_seg *)(sg + 1));
	return (sg);
}

void
sglist_free(struct sglist *sg)
{
	if (sg == NULL)
		return;
	if (atomic_fetchadd_int(&sg->sg_refs, -1) == 1)
		free(sg, M_XGMISC);
}

int
sglist_append_phys(struct sglist *sg, vm_paddr_t paddr, size_t len)
{
	struct sglist_seg *ss;

	if (sg->sg_maxseg == 0)
		return (EINVAL);
	if (len == 0)
		return (0);
	if (sg->sg_nseg == 0) {
		sg->sg_segs[0].ss_paddr = paddr;
		sg->sg_segs[0].ss_len = len;
		sg->sg_nseg = 1;
		return (0);
	}
	/* a single range either merges into the last segment or takes a new one, so nothing to roll back */
	ss = &sg->sg_segs[sg->sg_nseg - 1];
	if (ss->ss_paddr + ss->ss_len == paddr) {
		ss->ss_len += len;
		return (0);
	}
	if (sg->sg_nseg == sg->sg_maxseg)
		return (EFBIG);
	ss++;
	ss->ss_paddr = paddr;
	ss->ss_len = len;
	sg->sg_nseg++;
	return (0);
}

/* ---- SMP: XNU has no cross-call KPI in the SDK, so a rendezvous runs on the calling CPU only ---- */

cpuset_t all_cpus;

void
smp_no_rendezvous_barrier(void *dummy)
{
}

void
smp_rendezvous(void (*setup)(void *), void (*action)(void *), void (*teardown)(void *), void *arg)
{
	static int warned;

	if (mp_ncpus > 1 && !warned) {
		warned = 1;
		printf("xegfx: smp_rendezvous runs on the calling CPU only\n");
	}
	spinlock_enter();
	if (setup != NULL && setup != smp_no_rendezvous_barrier)
		setup(arg);
	if (action != NULL && action != smp_no_rendezvous_barrier)
		action(arg);
	if (teardown != NULL && teardown != smp_no_rendezvous_barrier)
		teardown(arg);
	spinlock_exit();
}

/* ---- scheduling ---- */

/*
 * A relinquish sleeps a tick instead of only yielding: a kernel thread that just yields can keep a lower-priority
 * thread holding an RCU read section off its CPU (docs/xg_sched.md).
 */
static void
xg_relinquish(void)
{
	pause("yield", 1);
}

/* the caller holds its thread lock (a spin mutex), which mi_switch drops before switching */
void
mi_switch(int flags)
{
	struct thread *td = curthread;

	thread_unlock(td);
	if (SCHEDULER_STOPPED())
		return;
	if ((flags & SW_TYPE_MASK) == SWT_RELINQUISH)
		xg_relinquish();
	else
		thread_block(NULL);
}

/* XNU picks priorities itself; this is FreeBSD's mi_switch(SW_VOL | SWT_RELINQUISH) part only */
void
kern_yield(int prio)
{
	xg_relinquish();
}

/* every CPU is in NUMA domain 0 on the laptops XeGfx targets */
int
XG_PCPU_domain(void)
{
	return (0);
}

struct domainset domainset_prefer[MAXMEMDOM];
struct domainset domainset_roundrobin = {
	.ds_policy = DOMAINSET_POLICY_ROUNDROBIN,
	.ds_prefer = -1,
	.ds_cnt = 1,
};

/* ---- group taskqueues: the queues are the sched area's (xg_sched_gtq_*), this is the group around them ---- */

struct taskqgroup {
	const char		*tqg_name;
	int			tqg_cnt;
	struct gtaskqueue	**tqg_queue;	/* made on first attach to that slot */
	char			(*tqg_qname)[24];
	int			*tqg_ntasks;
};

struct taskqgroup *qgroup_softirq;

static struct taskqgroup *
xg_tqg_create(const char *name)
{
	struct taskqgroup *g;

	g = malloc(sizeof(*g), M_XGMISC, M_WAITOK | M_ZERO);
	g->tqg_name = name;
	g->tqg_cnt = mp_ncpus;
	g->tqg_queue = malloc(sizeof(*g->tqg_queue) * g->tqg_cnt, M_XGMISC, M_WAITOK | M_ZERO);
	g->tqg_qname = malloc(sizeof(*g->tqg_qname) * g->tqg_cnt, M_XGMISC, M_WAITOK | M_ZERO);
	g->tqg_ntasks = malloc(sizeof(*g->tqg_ntasks) * g->tqg_cnt, M_XGMISC, M_WAITOK | M_ZERO);
	return (g);
}

static void
xg_tqg_destroy(struct taskqgroup *g)
{
	free(g->tqg_ntasks, M_XGMISC);
	free(g->tqg_qname, M_XGMISC);
	free(g->tqg_queue, M_XGMISC);
	free(g, M_XGMISC);
}

/* TASKQGROUP_DEFINE's SYSINIT does not run on XNU, so the one group LinuxKPI uses is made on first use */
static struct taskqgroup *
xg_tqg_get(struct taskqgroup *g)
{
	struct taskqgroup *n;

	if (g != NULL)
		return (g);
	g = (struct taskqgroup *)atomic_load_acq_ptr((volatile uintptr_t *)&qgroup_softirq);
	if (g != NULL)
		return (g);
	n = xg_tqg_create("softirq");
	if (!atomic_cmpset_ptr((volatile uintptr_t *)&qgroup_softirq, 0, (uintptr_t)n))
		xg_tqg_destroy(n);
	return (qgroup_softirq);
}

static struct gtaskqueue *
xg_tqg_slot(struct taskqgroup *g, int i)
{
	struct gtaskqueue *q;

	mtx_lock(&xg_gtq_mtx);
	q = g->tqg_queue[i];
	if (q == NULL) {
		/* xg_gtq_mtx is a sleep mutex, so creating the worker under it is fine and avoids a second queue */
		snprintf(g->tqg_qname[i], sizeof(g->tqg_qname[i]), "%s_%d", g->tqg_name, i);
		q = g->tqg_queue[i] = xg_sched_gtq_create(g->tqg_qname[i]);
	}
	mtx_unlock(&xg_gtq_mtx);
	return (q);
}

int
taskqgroup_attach_cpu(struct taskqgroup *qgroup, struct grouptask *gtask, void *uniq, int cpu, device_t dev,
    struct resource *irq, const char *name)
{
	struct gtaskqueue *q;

	xg_misc_init();
	qgroup = xg_tqg_get(qgroup);
	gtask->gt_uniq = uniq;
	snprintf(gtask->gt_name, GROUPTASK_NAMELEN, "%s", name ? name : "grouptask");
	gtask->gt_dev = dev;
	gtask->gt_irq = irq;
	gtask->gt_cpu = cpu;
	if (cpu < 0 || cpu >= qgroup->tqg_cnt) {
		printf("%s: qid not found for %s cpu=%d\n", __func__, gtask->gt_name, cpu);
		return (EINVAL);
	}
	q = xg_tqg_slot(qgroup, cpu);
	if (q == NULL)
		return (ENOMEM);
	mtx_lock(&xg_gtq_mtx);
	qgroup->tqg_ntasks[cpu]++;
	gtask->gt_taskqueue = q;
	mtx_unlock(&xg_gtq_mtx);
	return (0);
}

void
taskqgroup_detach(struct taskqgroup *qgroup, struct grouptask *gtask)
{
	struct gtaskqueue *q = gtask->gt_taskqueue;
	struct gtask *t = &gtask->gt_task;
	int i;

	xg_misc_init();
	qgroup = xg_tqg_get(qgroup);
	if (q == NULL)
		return;
	/*
	 * grouptask_block() without the queue's lock: an enqueue racing with the flag write can drop it, but the drain
	 * takes that lock after the write, so the check below sees the loss and goes round again.
	 */
	for (;;) {
		t->ta_flags |= TASK_NOENQUEUE;
		xg_sched_gtq_drain(q, t);
		if ((t->ta_flags & (TASK_NOENQUEUE | TASK_ENQUEUED)) == TASK_NOENQUEUE)
			break;
	}
	mtx_lock(&xg_gtq_mtx);
	for (i = 0; i < qgroup->tqg_cnt; i++)
		if (qgroup->tqg_queue[i] == q)
			break;
	if (i == qgroup->tqg_cnt)
		panic("%s: task %s not in group", __func__, gtask->gt_name);
	qgroup->tqg_ntasks[i]--;
	mtx_unlock(&xg_gtq_mtx);
	gtask->gt_taskqueue = NULL;
	t->ta_flags &= ~TASK_NOENQUEUE;
}

void
taskqgroup_drain_all(struct taskqgroup *qgroup)
{
	struct gtaskqueue *q;
	int i;

	xg_misc_init();
	qgroup = xg_tqg_get(qgroup);
	for (i = 0; i < qgroup->tqg_cnt; i++) {
		mtx_lock(&xg_gtq_mtx);
		q = qgroup->tqg_queue[i];
		mtx_unlock(&xg_gtq_mtx);
		if (q != NULL)
			xg_sched_gtq_drain_all(q);
	}
}

/* ---- modules: DECLARE_MODULE's SYSINIT lands here ---- */

struct module {
	TAILQ_ENTRY(module)	link;
	int			id;
	char			*name;
	modeventhand_t		handler;
	void			*arg;
};

static TAILQ_HEAD(xg_module_head, module) xg_modules = TAILQ_HEAD_INITIALIZER(xg_modules);
static int xg_next_modid = 1;
linker_file_t __this_linker_file;

static int
xg_mod_event(struct module *mod, int what)
{
	return (mod->handler != NULL ? mod->handler(mod, what, mod->arg) : 0);
}

void
module_register_init(const void *arg)
{
	const moduledata_t *data = arg;
	struct module *mod;
	int error;

	/* tunables must be loaded before any module's MOD_LOAD reads its parameters */
	xg_misc_init();
	mod = malloc(sizeof(*mod) + strlen(data->name) + 1, M_XGMISC, M_WAITOK | M_ZERO);
	mod->name = (char *)(mod + 1);
	strcpy(mod->name, data->name);
	mod->handler = data->evhand;
	mod->arg = data->priv;
	sx_xlock(&xg_mod_lock);
	mod->id = xg_next_modid++;
	sx_xunlock(&xg_mod_lock);
	error = xg_mod_event(mod, MOD_LOAD);
	if (error) {
		xg_mod_event(mod, MOD_UNLOAD);
		printf("module_register_init: MOD_LOAD (%s, %p, %p) error %d\n", data->name, (void *)data->evhand,
		    data->priv, error);
		free(mod, M_XGMISC);
		return;
	}
	sx_xlock(&xg_mod_lock);
	TAILQ_INSERT_TAIL(&xg_modules, mod, link);
	sx_xunlock(&xg_mod_lock);
}

int
xg_misc_module_unload_all(void)
{
	struct module *mod;
	int error;

	xg_misc_init();
	sx_xlock(&xg_mod_lock);
	while ((mod = TAILQ_LAST(&xg_modules, xg_module_head)) != NULL) {
		error = xg_mod_event(mod, MOD_QUIESCE);
		if (error == EOPNOTSUPP || error == EINVAL)
			error = 0;
		if (error == 0)
			error = xg_mod_event(mod, MOD_UNLOAD);
		if (error) {
			sx_xunlock(&xg_mod_lock);
			printf("xegfx: module %s refused to unload: %d\n", mod->name, error);
			return (error);
		}
		TAILQ_REMOVE(&xg_modules, mod, link);
		free(mod, M_XGMISC);
	}
	sx_xunlock(&xg_mod_lock);
	return (0);
}

/* ---- firmware(9): images from the kext's Contents/Resources, cached until the last firmware_put ---- */

#define	XG_FW_TIMEOUT_MS	30000

struct xg_fw {
	struct firmware		fw;		/* first, so a firmware pointer is an xg_fw pointer */
	LIST_ENTRY(xg_fw)	link;
	int			refs;
	unsigned long		len;
};

static LIST_HEAD(, xg_fw) xg_fws = LIST_HEAD_INITIALIZER(xg_fws);

const struct firmware *
firmware_get_flags(const char *imagename, uint32_t flags)
{
	struct xg_fw *f;
	void *data;
	unsigned long len;
	char *name;
	int error;

	xg_misc_init();
	/* Resources is flat; LinuxKPI retries a path as its basename and with '/' flattened, so skip the round trip */
	if (strchr(imagename, '/') != NULL)
		return (NULL);
	sx_xlock(&xg_fw_lock);
	LIST_FOREACH(f, &xg_fws, link)
		if (strcmp(f->fw.name, imagename) == 0) {
			f->refs++;
			sx_xunlock(&xg_fw_lock);
			return (&f->fw);
		}
	/* the lock is held across the request so two callers never fetch the same image twice */
	error = xg_misc_fw_request(imagename, &data, &len, XG_FW_TIMEOUT_MS);
	if (error) {
		sx_xunlock(&xg_fw_lock);
		/* LinuxKPI always asks with NOWARN, which hid N-228's failure; one line per name is cheap */
		printf("xegfx: could not load firmware image %s, error %d\n", imagename, error);
		return (NULL);
	}
	f = malloc(sizeof(*f) + strlen(imagename) + 1, M_XGMISC, M_WAITOK | M_ZERO);
	name = (char *)(f + 1);
	strcpy(name, imagename);
	f->fw.name = name;
	f->fw.data = data;
	f->fw.datasize = len;
	f->len = len;
	f->refs = 1;
	LIST_INSERT_HEAD(&xg_fws, f, link);
	sx_xunlock(&xg_fw_lock);
	return (&f->fw);
}

const struct firmware *
firmware_get(const char *imagename)
{
	return (firmware_get_flags(imagename, 0));
}

void
firmware_put(const struct firmware *p, int flags)
{
	struct xg_fw *f;

	if (p == NULL)
		return;
	f = __containerof(p, struct xg_fw, fw);
	xg_misc_init();
	sx_xlock(&xg_fw_lock);
	if (--f->refs > 0 || (flags & FIRMWARE_UNLOAD) == 0) {
		/* without FIRMWARE_UNLOAD an unreferenced image stays cached, as on FreeBSD */
		sx_xunlock(&xg_fw_lock);
		return;
	}
	LIST_REMOVE(f, link);
	sx_xunlock(&xg_fw_lock);
	IOFree(__DECONST(void *, f->fw.data), f->len);
	free(f, M_XGMISC);
}

/* ---- select and knlist: FreeBSD's kqueue does not exist here ---- */

/*
 * selrecord and selwakeup are not defined here: xg_fd.cpp calls XNU's functions of those names, and a FreeBSD
 * definition in the kext would capture those calls. FreeBSD-side callers reach xg_fd_selrecord/xg_fd_selwakeup
 * once sys/selinfo.h maps the names (see docs/xg_fd.md).
 */
void
seldrain(struct selinfo *sip)
{
	xg_fd_selwakeup(sip);
}

static void
xg_knlist_mtx_lock(void *arg)
{
	mtx_lock((struct mtx *)arg);
}

static void
xg_knlist_mtx_unlock(void *arg)
{
	mtx_unlock((struct mtx *)arg);
}

static void
xg_knlist_mtx_assert(void *arg, int what)
{
}

void
knlist_init(struct knlist *knl, void *lock, void (*kl_lock)(void *), void (*kl_unlock)(void *),
    void (*kl_assert_lock)(void *, int))
{
	xg_misc_init();
	knl->kl_lockarg = lock != NULL ? lock : &xg_knlist_mtx;
	knl->kl_lock = kl_lock != NULL ? kl_lock : xg_knlist_mtx_lock;
	knl->kl_unlock = kl_unlock != NULL ? kl_unlock : xg_knlist_mtx_unlock;
	knl->kl_assert_lock = kl_assert_lock != NULL ? kl_assert_lock : xg_knlist_mtx_assert;
	knl->kl_autodestroy = 0;
	SLIST_INIT(&knl->kl_list);
}

void
knlist_add(struct knlist *knl, struct knote *kn, int islocked)
{
	if (!islocked)
		knl->kl_lock(knl->kl_lockarg);
	SLIST_INSERT_HEAD(&knl->kl_list, kn, kn_selnext);
	if (!islocked)
		knl->kl_unlock(knl->kl_lockarg);
	kn->kn_knlist = knl;
	kn->kn_status &= ~KN_DETACHED;
}

void
knlist_remove(struct knlist *knl, struct knote *kn, int islocked)
{
	if (!islocked)
		knl->kl_lock(knl->kl_lockarg);
	SLIST_REMOVE(&knl->kl_list, kn, knote, kn_selnext);
	kn->kn_knlist = NULL;
	if (!islocked)
		knl->kl_unlock(knl->kl_lockarg);
	kn->kn_status |= KN_DETACHED;
}

/* shadows XNU's knote(); the filters still run so their state stays right, activation has no kqueue to go to */
void
knote(struct knlist *list, long hint, int lockflags)
{
	struct knote *kn, *tkn;

	if (list == NULL)
		return;
	if ((lockflags & KNF_LISTLOCKED) == 0)
		list->kl_lock(list->kl_lockarg);
	SLIST_FOREACH_SAFE(kn, &list->kl_list, kn_selnext, tkn)
		(void)kn->kn_fop->f_event(kn, hint);
	if ((lockflags & KNF_LISTLOCKED) == 0)
		list->kl_unlock(list->kl_lockarg);
}

int
knote_triv_copy(struct knote *kn, struct proc *p1)
{
	return (0);
}

/* ---- devfs and file leftovers ---- */

void
dev_lock(void)
{
	xg_misc_init();
	mtx_lock(&xg_dev_mtx);
}

void
dev_unlock(void)
{
	mtx_unlock(&xg_dev_mtx);
}

size_t
devfs_iosize_max(void)
{
	/* FreeBSD's default clamp (vfs.devfs.iosize_max_clamp=1) */
	return (INT_MAX);
}

void *
fiodgname_buf_get_ptr(void *fgnp, u_long com)
{
	return (((struct fiodgname_arg *)fgnp)->buf);
}

/* no FreeBSD signals reach XNU processes, so only clearing the owner succeeds */
int
fsetown(pid_t pgid, struct sigio **sigiop)
{
	if (pgid == 0) {
		funsetown(sigiop);
		return (0);
	}
	return (EOPNOTSUPP);
}

void
funsetown(struct sigio **sigiop)
{
	*sigiop = NULL;
}

/* ---- vnode leftovers: XeGfx files have no FreeBSD vnode, these only keep the counts honest ---- */

static int xg_vop_stat_offsets[] = { __offsetof(struct vop_stat_args, a_vp), VDESC_NO_OFFSET };
static int xg_vop_unlock_offsets[] = { __offsetof(struct vop_unlock_args, a_vp), VDESC_NO_OFFSET };

struct vnodeop_desc vop_stat_desc = {
	.vdesc_name = "vop_stat",
	.vdesc_vop_offset = __offsetof(struct vop_vector, vop_stat),
	.vdesc_vp_offsets = xg_vop_stat_offsets,
	.vdesc_cred_offset = VDESC_NO_OFFSET,
	.vdesc_thread_offset = VDESC_NO_OFFSET,
	.vdesc_vpp_offset = VDESC_NO_OFFSET,
	.vdesc_componentname_offset = VDESC_NO_OFFSET,
};

struct vnodeop_desc vop_unlock_desc = {
	.vdesc_name = "vop_unlock",
	.vdesc_vop_offset = __offsetof(struct vop_vector, vop_unlock),
	.vdesc_vp_offsets = xg_vop_unlock_offsets,
	.vdesc_cred_offset = VDESC_NO_OFFSET,
	.vdesc_thread_offset = VDESC_NO_OFFSET,
	.vdesc_vpp_offset = VDESC_NO_OFFSET,
	.vdesc_componentname_offset = VDESC_NO_OFFSET,
};

void
vref(struct vnode *vp)
{
	if (vp != NULL)
		atomic_add_int(&vp->v_usecount, 1);
}

void
vrele(struct vnode *vp)
{
	if (vp != NULL)
		atomic_subtract_int(&vp->v_usecount, 1);
}

int
_vn_lock(struct vnode *vp, int flags, const char *file, int line)
{
	return (0);
}

int
vn_fill_kinfo_vnode(struct vnode *vp, struct kinfo_file *kif)
{
	return (EOPNOTSUPP);
}

/* ---- newbus and PCI leftovers ---- */

/* the PCI area sets this when it builds its device tree */
device_t root_bus;

struct resource_list_entry *
resource_list_find(struct resource_list *rl, int type, int rid)
{
	struct resource_list_entry *rle;

	STAILQ_FOREACH(rle, rl, link)
		if (rle->type == type && rle->rid == rid)
			return (rle);
	return (NULL);
}

/* the VBT comes from the ACPI OpRegion on UEFI laptops; i915 copes with "no option ROM" */
void *
vga_pci_map_bios(device_t dev, size_t *size)
{
	*size = 0;
	return (NULL);
}

void
vga_pci_unmap_bios(device_t dev, void *bios)
{
}

/* from vm/vm_pager.h, which needs the full vm_object and vm_page definitions */
void cdev_mgtdev_pager_free_pages(vm_object_t object);

/* needs the vm area's device pager objects (mmap of GEM objects to userspace, X5) */
void
cdev_mgtdev_pager_free_pages(vm_object_t object)
{
	xg_unimpl("cdev_mgtdev_pager_free_pages");
}

/* ---- vt(4): there is no FreeBSD console on XNU ---- */

int
vt_allocate(const struct vt_driver *drv, void *softc)
{
	return (0);
}

int
vt_deallocate(const struct vt_driver *drv, void *softc)
{
	return (0);
}

void
vt_suspend(struct vt_device *vd)
{
}

void
vt_resume(struct vt_device *vd)
{
}

int
vt_config_cons_colors(struct fb_info *info, int format, uint32_t rmax, int roffset, uint32_t gmax, int goffset,
    uint32_t bmax, int boffset)
{
	return (0);
}

int
vt_fb_ioctl(struct vt_device *vd, u_long cmd, caddr_t data, struct thread *td)
{
	return (ENOTTY);
}

int
vt_fb_mmap(struct vt_device *vd, vm_ooffset_t offset, vm_paddr_t *paddr, int prot, vm_memattr_t *memattr)
{
	return (EINVAL);
}

/* ---- LinuxKPI debugfs, i2c and i915 debugfs leftovers, declared here without the LinuxKPI headers ---- */

/* ABI of linux/debugfs.h: umode_t is uint16_t, atomic_t and file_operations are only pointed to */
struct dentry;
struct file_operations;
struct i2c_adapter;
struct i2c_msg;
struct intel_gt;
struct intel_gt_debugfs_file;

struct dentry *debugfs_create_dir(const char *name, struct dentry *parent);
struct dentry *debugfs_create_file(const char *name, uint16_t mode, struct dentry *parent, void *data,
    const struct file_operations *fops);
struct dentry *debugfs_create_file_unsafe(const char *name, uint16_t mode, struct dentry *parent, void *data,
    const struct file_operations *fops);
void debugfs_create_bool(const char *name, uint16_t mode, struct dentry *parent, bool *value);
void debugfs_create_atomic_t(const char *name, uint16_t mode, struct dentry *parent, void *value);
void debugfs_remove(struct dentry *dentry);
void intel_gt_debugfs_register(struct intel_gt *gt);
void intel_gt_debugfs_register_files(struct dentry *root, const struct intel_gt_debugfs_file *files,
    unsigned long count, void *data);
int lkpi_i2c_add_adapter(struct i2c_adapter *adapter);
int lkpi_i2c_del_adapter(struct i2c_adapter *adapter);
int lkpi_i2cbb_transfer(struct i2c_adapter *adapter, struct i2c_msg *msgs, int nmsgs);

/* what Linux returns with CONFIG_DEBUG_FS off; callers are written to ignore it and pass it back in */
#define	XG_DEBUGFS_OFF	((struct dentry *)(intptr_t)-ENODEV)

struct dentry *
debugfs_create_dir(const char *name, struct dentry *parent)
{
	return (XG_DEBUGFS_OFF);
}

struct dentry *
debugfs_create_file(const char *name, uint16_t mode, struct dentry *parent, void *data,
    const struct file_operations *fops)
{
	return (XG_DEBUGFS_OFF);
}

struct dentry *
debugfs_create_file_unsafe(const char *name, uint16_t mode, struct dentry *parent, void *data,
    const struct file_operations *fops)
{
	return (XG_DEBUGFS_OFF);
}

void
debugfs_create_bool(const char *name, uint16_t mode, struct dentry *parent, bool *value)
{
}

void
debugfs_create_atomic_t(const char *name, uint16_t mode, struct dentry *parent, void *value)
{
}

void
debugfs_remove(struct dentry *dentry)
{
}

void
intel_gt_debugfs_register(struct intel_gt *gt)
{
}

void
intel_gt_debugfs_register_files(struct dentry *root, const struct intel_gt_debugfs_file *files,
    unsigned long count, void *data)
{
}

/* adapters register without a FreeBSD iicbus; i2c_transfer() calls an adapter's own master_xfer directly */
int
lkpi_i2c_add_adapter(struct i2c_adapter *adapter)
{
	return (0);
}

int
lkpi_i2c_del_adapter(struct i2c_adapter *adapter)
{
	return (0);
}

/* bit-banged adapters (DDC on some outputs) need iicbb, which comes with external displays (X6) */
int
lkpi_i2cbb_transfer(struct i2c_adapter *adapter, struct i2c_msg *msgs, int nmsgs)
{
	return (-ENXIO);
}

/* not in the kext KPI sets: our own bsearch, and suword64 as a copyout */
void *
bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *))
{
	const char *lo = base;
	size_t n = nmemb;

	while (n > 0) {
		const char *mid = lo + (n / 2) * size;
		int c = compar(key, mid);

		if (c == 0)
			return ((void *)(uintptr_t)mid);
		if (c > 0) {
			lo = mid + size;
			n = n - n / 2 - 1;
		} else
			n /= 2;
	}
	return (NULL);
}

int
suword64(volatile void *base, int64_t word)
{
	return (copyout(&word, (void *)(uintptr_t)base, sizeof(word)) == 0 ? 0 : -1);
}
