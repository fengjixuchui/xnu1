/*
 * Copyright (c) 2004-2021 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
#include <stdarg.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/event.h>         // for kqueue related stuff
#include <sys/fsevents.h>

#if CONFIG_FSE
#include <sys/namei.h>
#include <sys/filedesc.h>
#include <sys/kernel.h>
#include <sys/file_internal.h>
#include <sys/stat.h>
#include <sys/vnode_internal.h>
#include <sys/mount_internal.h>
#include <sys/proc_internal.h>
#include <sys/kauth.h>
#include <sys/uio.h>
#include <kern/kalloc.h>
#include <sys/dirent.h>
#include <sys/attr.h>
#include <sys/sysctl.h>
#include <sys/ubc.h>
#include <machine/cons.h>
#include <miscfs/specfs/specdev.h>
#include <miscfs/devfs/devfs.h>
#include <sys/filio.h>
#include <kern/locks.h>
#include <libkern/OSAtomic.h>
#include <kern/zalloc.h>
#include <mach/mach_time.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <IOKit/IOBSD.h>

#include <security/audit/audit.h>
#include <bsm/audit_kevents.h>

#include <pexpert/pexpert.h>
#include <libkern/section_keywords.h>

typedef struct kfs_event {
	LIST_ENTRY(kfs_event) kevent_list;
	uint64_t       abstime;    // when this event happened (mach_absolute_time())
	int16_t        type;       // type code of this event
	uint16_t       flags;      // per-event flags
	int32_t        refcount;   // number of clients referencing this
	pid_t          pid;
	int32_t        spare;

	union {
		struct regular_event {
			// This must match the layout of fse_info
			// exactly, except for the "nlink" field is
			// not included here.  See copy_out_kfse()
			// for all of the sordid details, and also
			// the _Static_assert() statements below.
			ino64_t          ino;
			dev_t            dev;
			int32_t          mode;
			uid_t            uid;
			uint32_t         document_id;
			struct kfs_event *dest; // if this is a two-file op
			const char       *str;
			uint16_t         len;
		} regular_event;

		struct {
			ino64_t          src_ino;
			ino64_t          dst_ino;
			uint64_t         docid;
			dev_t            dev;
		} docid_event;

		struct {
			uint32_t         version;
			dev_t            dev;
			ino64_t          ino;
			uint64_t         origin_id;
			uint64_t         age;
			uint32_t         use_state;
			uint32_t         urgency;
			uint64_t         size;
		} activity_event;

		struct {
			audit_token_t    audit_token;
			const char       *str;
			uint16_t         len;
		} access_granted_event;
	};
} kfs_event;

_Static_assert(offsetof(struct regular_event, ino) == offsetof(fse_info, ino),
    "kfs_event and fse_info out-of-sync");
_Static_assert(offsetof(struct regular_event, dev) == offsetof(fse_info, dev),
    "kfs_event and fse_info out-of-sync");
_Static_assert(offsetof(struct regular_event, mode) == offsetof(fse_info, mode),
    "kfs_event and fse_info out-of-sync");
_Static_assert(offsetof(struct regular_event, uid) == offsetof(fse_info, uid),
    "kfs_event and fse_info out-of-sync");
_Static_assert(offsetof(struct regular_event, document_id) == offsetof(fse_info, document_id),
    "kfs_event and fse_info out-of-sync");

#define KFSE_INFO_COPYSIZE offsetof(fse_info, nlink)

// flags for the flags field
#define KFSE_COMBINED_EVENTS          0x0001
#define KFSE_CONTAINS_DROPPED_EVENTS  0x0002
#define KFSE_ON_LIST                  0x0004
#define KFSE_BEING_CREATED            0x0008

LIST_HEAD(kfse_list, kfs_event) kfse_list_head = LIST_HEAD_INITIALIZER(x);
int num_events_outstanding = 0;
int num_pending_rename = 0;


struct fsevent_handle;

typedef struct fs_event_watcher {
	int8_t      *event_list;         // the events we're interested in
	int32_t      num_events;
	dev_t       *devices_not_to_watch;// report events from devices not in this list
	uint32_t     num_devices;
	int32_t      flags;
	kfs_event  **event_queue;
	int32_t      eventq_size;        // number of event pointers in queue
	int32_t      num_readers;
	int32_t      rd;                 // read index into the event_queue
	int32_t      wr;                 // write index into the event_queue
	int32_t      blockers;
	int32_t      my_id;
	uint32_t     num_dropped;
	uint64_t     max_event_id;
	struct fsevent_handle *fseh;
	pid_t        pid;
	char         proc_name[(2 * MAXCOMLEN) + 1];
} fs_event_watcher;

// fs_event_watcher flags
#define WATCHER_DROPPED_EVENTS         0x0001
#define WATCHER_CLOSING                0x0002
#define WATCHER_WANTS_COMPACT_EVENTS   0x0004
#define WATCHER_WANTS_EXTENDED_INFO    0x0008
#define WATCHER_APPLE_SYSTEM_SERVICE   0x0010   // fseventsd, coreservicesd, mds, revisiond

#define MAX_WATCHERS  8
static fs_event_watcher *watcher_table[MAX_WATCHERS];

#define DEFAULT_MAX_KFS_EVENTS   4096
static int max_kfs_events = DEFAULT_MAX_KFS_EVENTS;

// we allocate kfs_event structures out of this zone
static zone_t     event_zone;
static int        fs_event_init = 0;

//
// this array records whether anyone is interested in a
// particular type of event.  if no one is, we bail out
// early from the event delivery
//
static int16_t     fs_event_type_watchers[FSE_MAX_EVENTS];

// the device currently being unmounted:
static dev_t fsevent_unmount_dev = 0;
// how many ACKs are still outstanding:
static int fsevent_unmount_ack_count = 0;

static int  watcher_add_event(fs_event_watcher *watcher, kfs_event *kfse);
static void fsevents_wakeup(fs_event_watcher *watcher);

//
// Locks
//
static LCK_ATTR_DECLARE(fsevent_lock_attr, 0, 0);
static LCK_GRP_DECLARE(fsevent_mutex_group, "fsevent-mutex");
static LCK_GRP_DECLARE(fsevent_rw_group, "fsevent-rw");

static LCK_RW_DECLARE_ATTR(event_handling_lock, // handles locking for event manipulation and recycling
    &fsevent_rw_group, &fsevent_lock_attr);
static LCK_MTX_DECLARE_ATTR(watch_table_lock,
    &fsevent_mutex_group, &fsevent_lock_attr);
static LCK_MTX_DECLARE_ATTR(event_buf_lock,
    &fsevent_mutex_group, &fsevent_lock_attr);
static LCK_MTX_DECLARE_ATTR(event_writer_lock,
    &fsevent_mutex_group, &fsevent_lock_attr);


/* Explicitly declare qsort so compiler doesn't complain */
__private_extern__ void qsort(
	void * array,
	size_t nmembers,
	size_t member_size,
	int (*)(const void *, const void *));

static int
is_ignored_directory(const char *path)
{
	if (!path) {
		return 0;
	}

#define IS_TLD(x) strnstr(__DECONST(char *, path), x, MAXPATHLEN)
	if (IS_TLD("/.Spotlight-V100/") ||
	    IS_TLD("/.MobileBackups/") ||
	    IS_TLD("/Backups.backupdb/")) {
		return 1;
	}
#undef IS_TLD

	return 0;
}

static void
fsevents_internal_init(void)
{
	int i;

	if (fs_event_init++ != 0) {
		return;
	}

	for (i = 0; i < FSE_MAX_EVENTS; i++) {
		fs_event_type_watchers[i] = 0;
	}

	memset(watcher_table, 0, sizeof(watcher_table));

	PE_get_default("kern.maxkfsevents", &max_kfs_events, sizeof(max_kfs_events));

	event_zone = zone_create_ext("fs-event-buf", sizeof(kfs_event),
	    ZC_NOGC | ZC_NOCALLOUT, ZONE_ID_ANY, ^(zone_t z) {
		// mark the zone as exhaustible so that it will not
		// ever grow beyond what we initially filled it with
		zone_set_exhaustible(z, max_kfs_events, /* exhausts */ true);
	});

	zone_fill_initially(event_zone, max_kfs_events);
}

static void
lock_watch_table(void)
{
	lck_mtx_lock(&watch_table_lock);
}

static void
unlock_watch_table(void)
{
	lck_mtx_unlock(&watch_table_lock);
}

static void
lock_fs_event_list(void)
{
	lck_mtx_lock(&event_buf_lock);
}

static void
unlock_fs_event_list(void)
{
	lck_mtx_unlock(&event_buf_lock);
}

// forward prototype
static void release_event_ref(kfs_event *kfse);

static boolean_t
watcher_cares_about_dev(fs_event_watcher *watcher, dev_t dev)
{
	unsigned int i;

	// if devices_not_to_watch is NULL then we care about all
	// events from all devices
	if (watcher->devices_not_to_watch == NULL) {
		return true;
	}

	for (i = 0; i < watcher->num_devices; i++) {
		if (dev == watcher->devices_not_to_watch[i]) {
			// found a match! that means we do not
			// want events from this device.
			return false;
		}
	}

	// if we're here it's not in the devices_not_to_watch[]
	// list so that means we do care about it
	return true;
}


int
need_fsevent(int type, vnode_t vp)
{
	if (type >= 0 && type < FSE_MAX_EVENTS && fs_event_type_watchers[type] == 0) {
		return 0;
	}

	// events in /dev aren't really interesting...
	if (vp->v_tag == VT_DEVFS) {
		return 0;
	}

	return 1;
}


#define is_throw_away(x)  ((x) == FSE_STAT_CHANGED || (x) == FSE_CONTENT_MODIFIED)


int num_dropped         = 0;

static struct timeval last_print;

//
// These variables are used to track coalescing multiple identical
// events for the same vnode/pathname.  If we get the same event
// type and same vnode/pathname as the previous event, we just drop
// the event since it's superfluous.  This improves some micro-
// benchmarks considerably and actually has a real-world impact on
// tests like a Finder copy where multiple stat-changed events can
// get coalesced.
//
static int     last_event_type = -1;
static void   *last_ptr = NULL;
static char    last_str[MAXPATHLEN];
static int     last_nlen = 0;
static int     last_vid = -1;
static uint64_t last_coalesced_time = 0;
static void   *last_event_ptr = NULL;
static pid_t last_pid = -1;
int            last_coalesced = 0;
static mach_timebase_info_data_t    sTimebaseInfo = { 0, 0 };

#define MAX_HARDLINK_NOTIFICATIONS 128

static inline void
kfse_init(kfs_event *kfse, int type, uint64_t time, proc_t p)
{
	memset(kfse, 0, sizeof(*kfse));
	kfse->refcount = 1;
	kfse->type =     (int16_t)type;
	kfse->abstime =  time;
	kfse->pid =      proc_getpid(p);

	OSBitOrAtomic16(KFSE_BEING_CREATED, &kfse->flags);
}

int
add_fsevent(int type, vfs_context_t ctx, ...)
{
	struct proc      *p = vfs_context_proc(ctx);
	int               i, arg_type, ret;
	kfs_event        *kfse, *kfse_dest = NULL, *cur;
	fs_event_watcher *watcher;
	va_list           ap;
	int               error = 0, did_alloc = 0;
	int64_t           orig_linkcount = -1;
	dev_t             dev = 0;
	uint64_t          now, elapsed;
	uint64_t          orig_linkid = 0, next_linkid = 0;
	uint64_t          link_parentid = 0;
	char             *pathbuff = NULL, *path_override = NULL;
	char              *link_name = NULL;
	vnode_t           link_vp = NULL;
	int               pathbuff_len = 0;
	uthread_t         ut = get_bsdthread_info(current_thread());
	bool              do_all_links = true;
	bool              do_cache_reset = false;

	if (type == FSE_CONTENT_MODIFIED_NO_HLINK) {
		do_all_links = false;
		type = FSE_CONTENT_MODIFIED;
	}


restart:
	va_start(ap, ctx);

	// ignore bogus event types..
	if (type < 0 || type >= FSE_MAX_EVENTS) {
		return EINVAL;
	}

	// if no one cares about this type of event, bail out
	if (fs_event_type_watchers[type] == 0) {
		va_end(ap);

		return 0;
	}

	now = mach_absolute_time();

	// find a free event and snag it for our use
	// NOTE: do not do anything that would block until
	//       the lock is dropped.
	lock_fs_event_list();

	//
	// check if this event is identical to the previous one...
	// (as long as it's not an event type that can never be the
	// same as a previous event)
	//
	if (path_override == NULL &&
	    type != FSE_CREATE_FILE &&
	    type != FSE_DELETE &&
	    type != FSE_RENAME &&
	    type != FSE_EXCHANGE &&
	    type != FSE_CHOWN &&
	    type != FSE_DOCID_CHANGED &&
	    type != FSE_DOCID_CREATED &&
	    type != FSE_CLONE &&
	    type != FSE_ACTIVITY &&
	    // don't coalesce FSE_ACCESS_GRANTED because it could
	    // have been granted to a different process.
	    type != FSE_ACCESS_GRANTED) {
		void *ptr = NULL;
		int   vid = 0, was_str = 0, nlen = 0;

		for (arg_type = va_arg(ap, int32_t); arg_type != FSE_ARG_DONE; arg_type = va_arg(ap, int32_t)) {
			switch (arg_type) {
			case FSE_ARG_VNODE: {
				ptr = va_arg(ap, void *);
				vid = vnode_vid((struct vnode *)ptr);
				last_str[0] = '\0';
				break;
			}
			case FSE_ARG_STRING: {
				nlen = va_arg(ap, int32_t);
				ptr = va_arg(ap, void *);
				was_str = 1;
				break;
			}
			}
			if (ptr != NULL) {
				break;
			}
		}

		if (sTimebaseInfo.denom == 0) {
			(void) clock_timebase_info(&sTimebaseInfo);
		}

		elapsed = (now - last_coalesced_time);
		if (sTimebaseInfo.denom != sTimebaseInfo.numer) {
			if (sTimebaseInfo.denom == 1) {
				elapsed *= sTimebaseInfo.numer;
			} else {
				// this could overflow... the worst that will happen is that we'll
				// send (or not send) an extra event so I'm not going to worry about
				// doing the math right like dtrace_abs_to_nano() does.
				elapsed = (elapsed * sTimebaseInfo.numer) / (uint64_t)sTimebaseInfo.denom;
			}
		}

		if (type == last_event_type
		    && (elapsed < 1000000000)
		    && (last_pid == proc_getpid(p))
		    &&
		    ((vid && vid == last_vid && last_ptr == ptr)
		    ||
		    (last_str[0] && last_nlen == nlen && ptr && strcmp(last_str, ptr) == 0))
		    ) {
			last_coalesced++;
			unlock_fs_event_list();
			va_end(ap);

			return 0;
		} else {
			last_ptr = ptr;
			if (ptr && was_str) {
				strlcpy(last_str, ptr, sizeof(last_str));
			}
			last_nlen = nlen;
			last_vid = vid;
			last_event_type = type;
			last_coalesced_time = now;
			last_pid = proc_getpid(p);
		}
	}
	va_start(ap, ctx);


	kfse = zalloc_noblock(event_zone);
	if (kfse && (type == FSE_RENAME || type == FSE_EXCHANGE || type == FSE_CLONE)) {
		kfse_dest = zalloc_noblock(event_zone);
		if (kfse_dest == NULL) {
			did_alloc = 1;
			zfree(event_zone, kfse);
			kfse = NULL;
		}
	}


	if (kfse == NULL) {    // yikes! no free events
		unlock_fs_event_list();
		lock_watch_table();

		for (i = 0; i < MAX_WATCHERS; i++) {
			watcher = watcher_table[i];
			if (watcher == NULL) {
				continue;
			}

			watcher->flags |= WATCHER_DROPPED_EVENTS;
			fsevents_wakeup(watcher);
		}
		unlock_watch_table();

		{
			struct timeval current_tv;

			num_dropped++;

			// only print a message at most once every 5 seconds
			microuptime(&current_tv);
			if ((current_tv.tv_sec - last_print.tv_sec) > 10) {
				int ii;
				void *junkptr = zalloc_noblock(event_zone), *listhead = kfse_list_head.lh_first;

				printf("add_fsevent: event queue is full! dropping events (num dropped events: %d; num events outstanding: %d).\n", num_dropped, num_events_outstanding);
				printf("add_fsevent: kfse_list head %p ; num_pending_rename %d\n", listhead, num_pending_rename);
				printf("add_fsevent: zalloc sez: %p\n", junkptr);
				printf("add_fsevent: event_zone info: %d 0x%x\n", ((int *)event_zone)[0], ((int *)event_zone)[1]);
				lock_watch_table();
				for (ii = 0; ii < MAX_WATCHERS; ii++) {
					if (watcher_table[ii] == NULL) {
						continue;
					}

					printf("add_fsevent: watcher %s %p: rd %4d wr %4d q_size %4d flags 0x%x\n",
					    watcher_table[ii]->proc_name,
					    watcher_table[ii],
					    watcher_table[ii]->rd, watcher_table[ii]->wr,
					    watcher_table[ii]->eventq_size, watcher_table[ii]->flags);
				}
				unlock_watch_table();

				last_print = current_tv;
				if (junkptr) {
					zfree(event_zone, junkptr);
				}
			}
		}

		if (pathbuff) {
			release_pathbuff(pathbuff);
			pathbuff = NULL;
		}
		return ENOSPC;
	}

	kfse_init(kfse, type, now, p);
	last_event_ptr = kfse;
	if (type == FSE_RENAME || type == FSE_EXCHANGE || type == FSE_CLONE) {
		kfse_init(kfse_dest, type, now, p);
		kfse->regular_event.dest = kfse_dest;
	}

	num_events_outstanding++;
	if (kfse->type == FSE_RENAME) {
		num_pending_rename++;
	}
	LIST_INSERT_HEAD(&kfse_list_head, kfse, kevent_list);
	OSBitOrAtomic16(KFSE_ON_LIST, &kfse->flags);

	if (kfse->refcount < 1) {
		panic("add_fsevent: line %d: kfse recount %d but should be at least 1", __LINE__, kfse->refcount);
	}

	unlock_fs_event_list(); // at this point it's safe to unlock

	//
	// now process the arguments passed in and copy them into
	// the kfse
	//

	cur = kfse;

	if (type == FSE_DOCID_CREATED || type == FSE_DOCID_CHANGED) {
		//
		// These events are special and not like the other events.
		// They only have a dev_t, src inode #, dest inode #, and
		// a doc-id (va_arg'd to us in that order).  If we don't
		// get one of them, then the error-check filler will
		// catch it.
		//
		do_all_links = false;
		arg_type = va_arg(ap, int32_t);
		if (arg_type == FSE_ARG_DEV) {
			cur->docid_event.dev = (dev_t)(va_arg(ap, dev_t));
		}

		arg_type = va_arg(ap, int32_t);
		if (arg_type == FSE_ARG_INO) {
			cur->docid_event.src_ino =
			    (ino64_t)(va_arg(ap, ino64_t));
		}

		arg_type = va_arg(ap, int32_t);
		if (arg_type == FSE_ARG_INO) {
			cur->docid_event.dst_ino =
			    (ino64_t)(va_arg(ap, ino64_t));
		}

		arg_type = va_arg(ap, int32_t);
		if (arg_type == FSE_ARG_INT32) {
			cur->docid_event.docid =
			    (uint64_t)va_arg(ap, uint32_t);
		} else if (arg_type == FSE_ARG_INT64) {
			cur->docid_event.docid =
			    (uint64_t)va_arg(ap, uint64_t);
		}

		goto done_with_args;
	}

	if (type == FSE_ACTIVITY) {
		do_all_links = false;

		arg_type = va_arg(ap, int32_t);
		if (arg_type == FSE_ARG_INT32) {
			cur->activity_event.version = (uint32_t)(va_arg(ap, uint32_t));
		}

		arg_type = va_arg(ap, int32_t);
		if (arg_type == FSE_ARG_DEV) {
			cur->activity_event.dev = (dev_t)(va_arg(ap, dev_t));
		}

		arg_type = va_arg(ap, int32_t);
		if (arg_type == FSE_ARG_INO) {
			cur->activity_event.ino = (ino64_t)(va_arg(ap, ino64_t));
		}

		arg_type = va_arg(ap, int32_t);
		if (arg_type == FSE_ARG_INT64) {
			cur->activity_event.origin_id = (uint64_t)(va_arg(ap, uint64_t));
		}

		arg_type = va_arg(ap, int32_t);
		if (arg_type == FSE_ARG_INT64) {
			cur->activity_event.age = (uint64_t)(va_arg(ap, uint64_t));
		}

		arg_type = va_arg(ap, int32_t);
		if (arg_type == FSE_ARG_INT32) {
			cur->activity_event.use_state = (uint32_t)(va_arg(ap, uint32_t));
		}

		arg_type = va_arg(ap, int32_t);
		if (arg_type == FSE_ARG_INT32) {
			cur->activity_event.urgency = (uint32_t)(va_arg(ap, uint32_t));
		}

		arg_type = va_arg(ap, int32_t);
		if (arg_type == FSE_ARG_INT64) {
			cur->activity_event.size = (uint64_t)(va_arg(ap, uint64_t));
		}

		goto done_with_args;
	}
#if CONFIG_FSE_ACCESS_GRANTED
	if (type == FSE_ACCESS_GRANTED) {
		//
		// This one is also different.  We get a path string
		// and (maybe) and audit token.  If we don't get the
		// audit token, we extract is from the vfs_context_t.
		//
		audit_token_t *atokenp = NULL;
		vnode_t vp = NULL;
		char *path_str = NULL;
		size_t path_strlen = 0;
		void *arg;
		int32_t len32;

		do_all_links = false;

		while ((arg_type = va_arg(ap, int32_t)) != FSE_ARG_DONE) {
			switch (arg_type) {
			case FSE_ARG_STRING:
				len32 = va_arg(ap, int32_t);
				arg = va_arg(ap, char *);
				if (path_str == NULL) {
					path_str = arg;
					path_strlen = len32;
				}
				break;

			case FSE_ARG_PATH:
				arg = va_arg(ap, char *);
				if (path_str == NULL) {
					path_str = arg;
				}
				break;

			case FSE_ARG_VNODE:
				arg = va_arg(ap, vnode_t);
				if (vp == NULL) {
					vp = arg;
				}
				break;

			case FSE_ARG_AUDIT_TOKEN:
				arg = va_arg(ap, audit_token_t *);
				if (atokenp == NULL) {
					atokenp = arg;
				}
				break;

			default:
				printf("add_fsevent: FSE_ACCESS_GRANTED unknown type %d\n", arg_type);
				// just skip one 32-bit word and hope we
				// sync up...
				(void)va_arg(ap, int32_t);
			}
		}

		if (atokenp != NULL) {
			memcpy(&cur->access_granted_event.audit_token,
			    atokenp,
			    sizeof(cur->access_granted_event.audit_token));
		} else if (vfs_context_copy_audit_token(ctx,
		    &cur->access_granted_event.audit_token) != 0) {
			OSBitOrAtomic16(KFSE_CONTAINS_DROPPED_EVENTS,
			    &cur->flags);
			goto done_with_args;
		}

		//
		// If we got FSE_ARG_STRING, the length includes the
		// terminating NUL.  If we got FSE_ARG_PATH, all we
		// got was the string pointer, so get the length and
		// adjust.  If we didn't get either, then the caller
		// needs to have provided us with a vnode, and with
		// that we can get the path.
		//
		if (path_str != NULL) {
			if (path_strlen == 0) {
				path_strlen = strlen(path_str) + 1;
			}
		} else if (vp != NULL) {
			pathbuff = get_pathbuff();
			pathbuff_len = MAXPATHLEN;
			pathbuff[0] = '\0';
			if (vn_getpath_no_firmlink(vp, pathbuff,
			    &pathbuff_len) == 0) {
				path_str = pathbuff;
				path_strlen = pathbuff_len;
			}
		}

		if (path_str != NULL) {
			assert(path_strlen <= INT16_MAX);
			cur->access_granted_event.str =
			    vfs_addname(path_str, (uint32_t)path_strlen, 0, 0);
			if (path_str == pathbuff) {
				release_pathbuff(pathbuff);
				pathbuff = NULL;
			}
		}
		if (cur->access_granted_event.str == NULL) {
			OSBitOrAtomic16(KFSE_CONTAINS_DROPPED_EVENTS,
			    &cur->flags);
		}

		goto done_with_args;
	}
#endif
	if (type == FSE_UNMOUNT_PENDING) {
		// Just a dev_t
		// We use the same fields as the regular event, but we
		// don't have all of the data.
		do_all_links = false;

		arg_type = va_arg(ap, int32_t);
		if (arg_type == FSE_ARG_DEV) {
			cur->regular_event.dev = (dev_t)(va_arg(ap, dev_t));
		}

		cur->regular_event.dest = NULL;
		cur->regular_event.str = NULL;
		cur->regular_event.len = 0;

		goto done_with_args;
	}

	for (arg_type = va_arg(ap, int32_t); arg_type != FSE_ARG_DONE; arg_type = va_arg(ap, int32_t)) {
		switch (arg_type) {
		case FSE_ARG_VNODE: {
			// this expands out into multiple arguments to the client
			struct vnode *vp;
			struct vnode_attr va;

			if (kfse->regular_event.str != NULL) {
				cur = kfse_dest;
			}

			vp = va_arg(ap, struct vnode *);
			if (vp == NULL) {
				panic("add_fsevent: you can't pass me a NULL vnode ptr (type %d)!",
				    cur->type);
			}

			VATTR_INIT(&va);
			VATTR_WANTED(&va, va_fsid);
			VATTR_WANTED(&va, va_fileid);
			VATTR_WANTED(&va, va_mode);
			VATTR_WANTED(&va, va_uid);
			VATTR_WANTED(&va, va_document_id);
			VATTR_WANTED(&va, va_nlink);
			if ((ret = vnode_getattr(vp, &va, vfs_context_kernel())) != 0) {
				// printf("add_fsevent: failed to getattr on vp %p (%d)\n", cur->fref.vp, ret);
				cur->regular_event.str = NULL;
				error = EINVAL;
				goto clean_up;
			}

			cur->regular_event.dev  = dev = (dev_t)va.va_fsid;
			cur->regular_event.ino  = (ino64_t)va.va_fileid;
			cur->regular_event.mode = (int32_t)vnode_vttoif(vnode_vtype(vp)) | va.va_mode;
			cur->regular_event.uid  = va.va_uid;
			cur->regular_event.document_id  = va.va_document_id;
			if (vp->v_flag & VISHARDLINK) {
				cur->regular_event.mode |= FSE_MODE_HLINK;
				if ((vp->v_type == VDIR && va.va_dirlinkcount == 0) || (vp->v_type == VREG && va.va_nlink == 0)) {
					cur->regular_event.mode |= FSE_MODE_LAST_HLINK;
				}
				if (orig_linkid == 0) {
					orig_linkid = cur->regular_event.ino;
					orig_linkcount = MIN(va.va_nlink, MAX_HARDLINK_NOTIFICATIONS);
					link_vp = vp;
					if (vp->v_mount->mnt_kern_flag & MNTK_PATH_FROM_ID && !link_name) {
						VATTR_INIT(&va);
						VATTR_WANTED(&va, va_parentid);
						VATTR_WANTED(&va, va_name);
						link_name = zalloc(ZV_NAMEI);
						va.va_name = link_name;
						if ((ret = vnode_getattr(vp, &va, vfs_context_kernel()) != 0) ||
						    !(VATTR_IS_SUPPORTED(&va, va_name)) ||
						    !(VATTR_IS_SUPPORTED(&va, va_parentid))) {
							zfree(ZV_NAMEI, link_name);
							link_name = NULL;
						}
						if (link_name) {
							link_parentid = va.va_parentid;
						}
						va.va_name = NULL;
					}
				}
			}

			// if we haven't gotten the path yet, get it.
			if (pathbuff == NULL && path_override == NULL) {
				pathbuff = get_pathbuff();
				pathbuff_len = MAXPATHLEN;

				pathbuff[0] = '\0';
				if ((ret = vn_getpath_no_firmlink(vp, pathbuff, &pathbuff_len)) != 0 || pathbuff[0] == '\0') {
					OSBitOrAtomic16(KFSE_CONTAINS_DROPPED_EVENTS,
					    &cur->flags);

					do {
						if (vp->v_parent != NULL) {
							vp = vp->v_parent;
						} else if (vp->v_mount) {
							strlcpy(pathbuff, vp->v_mount->mnt_vfsstat.f_mntonname, MAXPATHLEN);
							break;
						} else {
							vp = NULL;
						}

						if (vp == NULL) {
							break;
						}

						pathbuff_len = MAXPATHLEN;
						ret = vn_getpath_no_firmlink(vp, pathbuff, &pathbuff_len);
					} while (ret == ENOSPC);

					if (ret != 0 || vp == NULL) {
						error = ENOENT;
						goto clean_up;
					}
				}
			} else if (path_override) {
				pathbuff = path_override;
				pathbuff_len = (int)strlen(path_override) + 1;
			} else {
				strlcpy(pathbuff, "NOPATH", MAXPATHLEN);
				pathbuff_len = (int)strlen(pathbuff) + 1;
			}

			// store the path by adding it to the global string table
			cur->regular_event.len = (u_int16_t)pathbuff_len;
			cur->regular_event.str =
			    vfs_addname(pathbuff, pathbuff_len, 0, 0);
			if (cur->regular_event.str == NULL ||
			    cur->regular_event.str[0] == '\0') {
				panic("add_fsevent: was not able to add path %s to event %p.", pathbuff, cur);
			}

			if (pathbuff != path_override) {
				release_pathbuff(pathbuff);
			}
			pathbuff = NULL;

			break;
		}

		case FSE_ARG_FINFO: {
			fse_info *fse;

			fse = va_arg(ap, fse_info *);

			cur->regular_event.dev  = dev = (dev_t)fse->dev;
			cur->regular_event.ino  = (ino64_t)fse->ino;
			cur->regular_event.mode = (int32_t)fse->mode;
			cur->regular_event.uid  = (uid_t)fse->uid;
			cur->regular_event.document_id  = (uint32_t)fse->document_id;
			// if it's a hard-link and this is the last link, flag it
			if (fse->mode & FSE_MODE_HLINK) {
				if (fse->nlink == 0) {
					cur->regular_event.mode |= FSE_MODE_LAST_HLINK;
				}
				if (orig_linkid == 0) {
					orig_linkid = cur->regular_event.ino;
					orig_linkcount = MIN(fse->nlink, MAX_HARDLINK_NOTIFICATIONS);
				}
			}
			if (cur->regular_event.mode & FSE_TRUNCATED_PATH) {
				OSBitOrAtomic16(KFSE_CONTAINS_DROPPED_EVENTS,
				    &cur->flags);
				cur->regular_event.mode &= ~FSE_TRUNCATED_PATH;
			}
			break;
		}

		case FSE_ARG_STRING:
			if (kfse->regular_event.str != NULL) {
				cur = kfse_dest;
			}

			cur->regular_event.len =
			    (int16_t)(va_arg(ap, int32_t) & 0x7fff);
			if (cur->regular_event.len >= 1) {
				cur->regular_event.str =
				    vfs_addname(va_arg(ap, char *),
				    cur->regular_event.len, 0, 0);
			} else {
				printf("add_fsevent: funny looking string length: %d\n", (int)cur->regular_event.len);
				cur->regular_event.len = 2;
				cur->regular_event.str = vfs_addname("/",
				    cur->regular_event.len, 0, 0);
			}
			if (cur->regular_event.str[0] == 0) {
				printf("add_fsevent: bogus looking string (len %d)\n", cur->regular_event.len);
			}
			break;

		case FSE_ARG_INT32: {
			uint32_t ival = (uint32_t)va_arg(ap, int32_t);
			kfse->regular_event.uid = ival;
			break;
		}

		default:
			printf("add_fsevent: unknown type %d\n", arg_type);
			// just skip one 32-bit word and hope we sync up...
			(void)va_arg(ap, int32_t);
		}
	}

done_with_args:
	va_end(ap);

	// XXX Memory barrier here?
	if (kfse_dest) {
		OSBitAndAtomic16(~KFSE_BEING_CREATED, &kfse_dest->flags);
	}
	OSBitAndAtomic16(~KFSE_BEING_CREATED, &kfse->flags);

	//
	// now we have to go and let everyone know that
	// is interested in this type of event
	//
	lock_watch_table();

	for (i = 0; i < MAX_WATCHERS; i++) {
		watcher = watcher_table[i];
		if (watcher == NULL) {
			continue;
		}

		if (type < watcher->num_events
		    && watcher->event_list[type] == FSE_REPORT
		    && watcher_cares_about_dev(watcher, dev)) {
			if (watcher_add_event(watcher, kfse) != 0) {
				watcher->num_dropped++;
				continue;
			}
		}

		// if (kfse->refcount < 1) {
		//    panic("add_fsevent: line %d: kfse recount %d but should be at least 1", __LINE__, kfse->refcount);
		// }
	}

	unlock_watch_table();

clean_up:

	if (pathbuff) {
		release_pathbuff(pathbuff);
		pathbuff = NULL;
	}
	// replicate events for sibling hardlinks
	if (do_all_links &&
	    (kfse->regular_event.mode & FSE_MODE_HLINK) &&
	    !(kfse->regular_event.mode & FSE_MODE_LAST_HLINK) &&
	    (type == FSE_STAT_CHANGED ||
	    type == FSE_CONTENT_MODIFIED ||
	    type == FSE_FINDER_INFO_CHANGED ||
	    type == FSE_XATTR_MODIFIED)) {
		if (orig_linkcount > 0 && orig_linkid != 0) {
#ifndef APFSIOC_NEXT_LINK
#define APFSIOC_NEXT_LINK  _IOWR('J', 10, uint64_t)
#endif
			if (path_override == NULL) {
				path_override = get_pathbuff();
			}
			if (next_linkid == 0) {
				next_linkid = orig_linkid;
			}

			if (link_vp) {
				mount_t mp = NULL;
				vnode_t mnt_rootvp = NULL;
				int iret = -1;

				mp = vnode_mount(link_vp);
				if (mp) {
					iret = VFS_ROOT(mp, &mnt_rootvp, vfs_context_kernel());
				}

				if (iret == 0 && mnt_rootvp) {
					iret = VNOP_IOCTL(mnt_rootvp, APFSIOC_NEXT_LINK, (char *)&next_linkid, (int)0, vfs_context_kernel());
					vnode_put(mnt_rootvp);
				}

				int32_t fsid0;
				int path_override_len = MAXPATHLEN;

				// continue resolving hardlink paths if there is a valid next_linkid retrieved
				// file systems not supporting APFSIOC_NEXT_LINK will skip replicating events for sibling hardlinks
				if (iret == 0 && next_linkid != 0) {
					fsid0 = link_vp->v_mount->mnt_vfsstat.f_fsid.val[0];
					ut->uu_flag |= UT_KERN_RAGE_VNODES;
					if (!do_cache_reset) {
						do_cache_reset = true;
					}
					if ((iret = fsgetpath_internal(ctx, fsid0, next_linkid, MAXPATHLEN, path_override, FSOPT_NOFIRMLINKPATH, &path_override_len)) == 0) {
						orig_linkcount--;
						ut->uu_flag &= ~UT_KERN_RAGE_VNODES;

						if (orig_linkcount >= 0) {
							release_event_ref(kfse);
							goto restart;
						}
					} else {
						// failed to get override path
						// encountered a broken link or the linkid has been deleted before retrieving the path
						orig_linkcount--;
						ut->uu_flag &= ~UT_KERN_RAGE_VNODES;

						if (orig_linkcount >= 0) {
							goto clean_up;
						}
					}
				}
			}
		}
	}

	if (link_name) {
		/*
		 * If we call fsgetpath on all the links, it will set the link origin cache
		 * to the last link that the path was obtained for.
		 * To restore the the original link id cache in APFS we need to issue a
		 * lookup on the original directory + name for the link.
		 */
		if (do_cache_reset) {
			vnode_t dvp = NULLVP;

			if ((ret = VFS_VGET(link_vp->v_mount, (ino64_t)link_parentid, &dvp, vfs_context_kernel())) == 0) {
				vnode_t lvp = NULLVP;

				ret = vnode_lookupat(link_name, 0, &lvp, ctx, dvp);
				if (!ret) {
					vnode_put(lvp);
					lvp = NULLVP;
				}
				vnode_put(dvp);
				dvp = NULLVP;
			}
			ret = 0;
		}
		zfree(ZV_NAMEI, link_name);
		link_name = NULL;
	}

	if (path_override) {
		release_pathbuff(path_override);
		path_override = NULL;
	}

	release_event_ref(kfse);

	return error;
}

int
test_fse_access_granted(vnode_t vp, unsigned long type, vfs_context_t ctx)
{
	audit_token_t atoken;
	char *pathbuff;
	int error, pathbuff_len;

	if (type == 0) {
		return add_fsevent(FSE_ACCESS_GRANTED, ctx,
		           FSE_ARG_VNODE, vp, FSE_ARG_DONE);
	}

	if (type == 1) {
		error = vfs_context_copy_audit_token(ctx, &atoken);
		if (error) {
			return error;
		}
		return add_fsevent(FSE_ACCESS_GRANTED, ctx,
		           FSE_ARG_VNODE, vp, FSE_ARG_AUDIT_TOKEN, &atoken,
		           FSE_ARG_DONE);
	}

	if (type == 2 || type == 3) {
		pathbuff = get_pathbuff();
		pathbuff_len = MAXPATHLEN;
		pathbuff[0] = '\0';
		error = vn_getpath_no_firmlink(vp, pathbuff, &pathbuff_len);
		if (error) {
			release_pathbuff(pathbuff);
			return error;
		}
		if (type == 2) {
			error = add_fsevent(FSE_ACCESS_GRANTED, ctx,
			    FSE_ARG_STRING, pathbuff_len, pathbuff,
			    FSE_ARG_DONE);
		} else {
			error = add_fsevent(FSE_ACCESS_GRANTED, ctx,
			    FSE_ARG_PATH, pathbuff, FSE_ARG_DONE);
		}
		release_pathbuff(pathbuff);
		return error;
	}

	return ENOTSUP;
}

static void
release_event_ref(kfs_event *kfse)
{
	int old_refcount;
	kfs_event *dest = NULL;
	const char *path_str = NULL, *dest_path_str = NULL;

	lock_fs_event_list();

	old_refcount = OSAddAtomic(-1, &kfse->refcount);
	if (old_refcount > 1) {
		unlock_fs_event_list();
		return;
	}

	if (last_event_ptr == kfse) {
		last_event_ptr = NULL;
		last_event_type = -1;
		last_coalesced_time = 0;
	}

	if (kfse->refcount < 0) {
		panic("release_event_ref: bogus kfse refcount %d", kfse->refcount);
	}

	assert(kfse->refcount == 0);
	assert(kfse->type != FSE_INVALID);

	//
	// Get pointers to all the things so we can free without
	// holding any locks.
	//
	if (kfse->type != FSE_DOCID_CREATED &&
	    kfse->type != FSE_DOCID_CHANGED &&
	    kfse->type != FSE_ACTIVITY) {
		path_str = kfse->regular_event.str;

		dest = kfse->regular_event.dest;
		if (dest != NULL) {
			assert(dest->type != FSE_INVALID);
			if (OSAddAtomic(-1,
			    &kfse->regular_event.dest->refcount) == 1) {
				dest_path_str = dest->regular_event.str;
			} else {
				dest = NULL;
			}
		}
	}

	if (dest != NULL) {
		if (dest->flags & KFSE_ON_LIST) {
			num_events_outstanding--;
			LIST_REMOVE(dest, kevent_list);
		}
	}

	if (kfse->flags & KFSE_ON_LIST) {
		num_events_outstanding--;
		LIST_REMOVE(kfse, kevent_list);
		if (kfse->type == FSE_RENAME) {
			num_pending_rename--;
		}
	}

	unlock_fs_event_list();

	zfree(event_zone, kfse);
	if (dest != NULL) {
		zfree(event_zone, dest);
	}

	if (path_str != NULL) {
		vfs_removename(path_str);
	}
	if (dest_path_str != NULL) {
		vfs_removename(dest_path_str);
	}
}

#define FSEVENTS_WATCHER_ENTITLEMENT            \
	"com.apple.private.vfs.fsevents-watcher"

#define FSEVENTS_ACTIVITY_WATCHER_ENTITLEMENT \
	"com.apple.private.vfs.fsevents-activity-watcher"

//
// We restrict this for two reasons:
//
// 1- So that naive processes don't get this firehose by default.
//
// 2- Because this event, when delivered to watcheres, includes the
//    audit token of the process granted the access, and we don't
//    want to leak that to random watchers.
//
#define FSEVENTS_ACCESS_GRANTED_WATCHER_ENTITLEMENT \
	"com.apple.private.vfs.fsevents-access-granted-watcher"

static bool
watcher_is_entitled(task_t task)
{
	//
	// We consider a process to be entitled to watch /dev/fsevents
	// if it has either FSEVENTS_WATCHER_ENTITLEMENT or
	// FSEVENTS_ACCESS_GRANTED_WATCHER_ENTITLEMENT.
	//
	return !!(IOTaskHasEntitlement(task, FSEVENTS_WATCHER_ENTITLEMENT) ||
	       IOTaskHasEntitlement(task,
	       FSEVENTS_ACCESS_GRANTED_WATCHER_ENTITLEMENT) ||
	       IOTaskHasEntitlement(task,
	       FSEVENTS_ACTIVITY_WATCHER_ENTITLEMENT));
}
#if CONFIG_FSE_ACCESS_GRANTED
static bool
watcher_is_entitled_for_access_granted(task_t task)
{
	return !!IOTaskHasEntitlement(task,
	           FSEVENTS_ACCESS_GRANTED_WATCHER_ENTITLEMENT);
}
#endif
static bool
watcher_is_entitled_for_activity(task_t task)
{
	return !!IOTaskHasEntitlement(task,
	           FSEVENTS_ACTIVITY_WATCHER_ENTITLEMENT);
}

static int
add_watcher(int8_t *event_list, int32_t num_events, int32_t eventq_size, fs_event_watcher **watcher_out, void *fseh)
{
	int               i;
	fs_event_watcher *watcher;

	if (eventq_size <= 0 || eventq_size > 100 * max_kfs_events) {
		eventq_size = max_kfs_events;
	}
	if (num_events > FSE_ACTIVITY &&
	    event_list[FSE_ACTIVITY] != FSE_IGNORE &&
	    !watcher_is_entitled_for_activity(current_task())) {
		event_list[FSE_ACTIVITY] = FSE_IGNORE;
	}
#if CONFIG_FSE_ACCESS_GRANTED
	// If the watcher wants FSE_ACCESS_GRANTED, ensure it has the
	// correct entitlement.  If not, just silently drop that event.
	if (num_events > FSE_ACCESS_GRANTED &&
	    event_list[FSE_ACCESS_GRANTED] != FSE_IGNORE &&
	    !watcher_is_entitled_for_access_granted(current_task())) {
		event_list[FSE_ACCESS_GRANTED] = FSE_IGNORE;
	}
#endif
	// Note: the event_queue follows the fs_event_watcher struct
	//       in memory so we only have to do one allocation
	watcher = kalloc_type(fs_event_watcher, kfs_event *, eventq_size, Z_WAITOK);
	if (watcher == NULL) {
		return ENOMEM;
	}

	watcher->event_list   = event_list;
	watcher->num_events   = num_events;
	watcher->devices_not_to_watch = NULL;
	watcher->num_devices  = 0;
	watcher->flags        = 0;
	watcher->event_queue  = (kfs_event **)&watcher[1];
	watcher->eventq_size  = eventq_size;
	watcher->rd           = 0;
	watcher->wr           = 0;
	watcher->blockers     = 0;
	watcher->num_readers  = 0;
	watcher->max_event_id = 0;
	watcher->fseh         = fseh;
	watcher->pid          = proc_selfpid();
	proc_selfname(watcher->proc_name, sizeof(watcher->proc_name));

	watcher->num_dropped  = 0;  // XXXdbg - debugging

	if (watcher_is_entitled(current_task())) {
		watcher->flags |= WATCHER_APPLE_SYSTEM_SERVICE;
	} else {
		printf("fsevents: watcher %s (pid: %d) - Using /dev/fsevents directly is unsupported.  Migrate to FSEventsFramework\n",
		    watcher->proc_name, watcher->pid);
	}

	lock_watch_table();

	// find a slot for the new watcher
	for (i = 0; i < MAX_WATCHERS; i++) {
		if (watcher_table[i] == NULL) {
			watcher->my_id   = i;
			watcher_table[i] = watcher;
			break;
		}
	}

	if (i >= MAX_WATCHERS) {
		printf("fsevents: too many watchers!\n");
		unlock_watch_table();
		kfree_type(fs_event_watcher, kfs_event *, watcher->eventq_size, watcher);
		return ENOSPC;
	}

	// now update the global list of who's interested in
	// events of a particular type...
	for (i = 0; i < num_events; i++) {
		if (event_list[i] != FSE_IGNORE && i < FSE_MAX_EVENTS) {
			fs_event_type_watchers[i]++;
		}
	}

	unlock_watch_table();

	*watcher_out = watcher;

	return 0;
}



static void
remove_watcher(fs_event_watcher *target)
{
	int i, j, counter = 0;
	fs_event_watcher *watcher;
	kfs_event *kfse;

	lock_watch_table();

	for (j = 0; j < MAX_WATCHERS; j++) {
		watcher = watcher_table[j];
		if (watcher != target) {
			continue;
		}

		watcher_table[j] = NULL;

		for (i = 0; i < watcher->num_events; i++) {
			if (watcher->event_list[i] != FSE_IGNORE && i < FSE_MAX_EVENTS) {
				fs_event_type_watchers[i]--;
			}
		}

		if (watcher->flags & WATCHER_CLOSING) {
			unlock_watch_table();
			return;
		}

		// printf("fsevents: removing watcher %p (rd %d wr %d num_readers %d flags 0x%x)\n", watcher, watcher->rd, watcher->wr, watcher->num_readers, watcher->flags);
		watcher->flags |= WATCHER_CLOSING;
		OSAddAtomic(1, &watcher->num_readers);

		unlock_watch_table();

		while (watcher->num_readers > 1 && counter++ < 5000) {
			lock_watch_table();
			fsevents_wakeup(watcher); // in case they're asleep
			unlock_watch_table();

			tsleep(watcher, PRIBIO, "fsevents-close", 1);
		}
		if (counter++ >= 5000) {
			// printf("fsevents: close: still have readers! (%d)\n", watcher->num_readers);
			panic("fsevents: close: still have readers! (%d)", watcher->num_readers);
		}

		// drain the event_queue

		lck_rw_lock_exclusive(&event_handling_lock);
		while (watcher->rd != watcher->wr) {
			kfse = watcher->event_queue[watcher->rd];
			watcher->event_queue[watcher->rd] = NULL;
			watcher->rd = (watcher->rd + 1) % watcher->eventq_size;
			OSSynchronizeIO();
			if (kfse != NULL && kfse->type != FSE_INVALID && kfse->refcount >= 1) {
				release_event_ref(kfse);
			}
		}
		lck_rw_unlock_exclusive(&event_handling_lock);

		kfree_data(watcher->event_list, watcher->num_events * sizeof(int8_t));
		kfree_data(watcher->devices_not_to_watch, watcher->num_devices * sizeof(dev_t));
		kfree_type(fs_event_watcher, kfs_event *, watcher->eventq_size, watcher);
		return;
	}

	unlock_watch_table();
}


#define EVENT_DELAY_IN_MS   10
static thread_call_t event_delivery_timer = NULL;
static int timer_set = 0;


static void
delayed_event_delivery(__unused void *param0, __unused void *param1)
{
	int i;

	lock_watch_table();

	for (i = 0; i < MAX_WATCHERS; i++) {
		if (watcher_table[i] != NULL && watcher_table[i]->rd != watcher_table[i]->wr) {
			fsevents_wakeup(watcher_table[i]);
		}
	}

	timer_set = 0;

	unlock_watch_table();
}


//
// The watch table must be locked before calling this function.
//
static void
schedule_event_wakeup(void)
{
	uint64_t deadline;

	if (event_delivery_timer == NULL) {
		event_delivery_timer = thread_call_allocate((thread_call_func_t)delayed_event_delivery, NULL);
	}

	clock_interval_to_deadline(EVENT_DELAY_IN_MS, 1000 * 1000, &deadline);

	thread_call_enter_delayed(event_delivery_timer, deadline);
	timer_set = 1;
}



#define MAX_NUM_PENDING  16

//
// NOTE: the watch table must be locked before calling
//       this routine.
//
static int
watcher_add_event(fs_event_watcher *watcher, kfs_event *kfse)
{
	if (kfse->abstime > watcher->max_event_id) {
		watcher->max_event_id = kfse->abstime;
	}

	if (((watcher->wr + 1) % watcher->eventq_size) == watcher->rd) {
		watcher->flags |= WATCHER_DROPPED_EVENTS;
		fsevents_wakeup(watcher);
		return ENOSPC;
	}

	OSAddAtomic(1, &kfse->refcount);
	watcher->event_queue[watcher->wr] = kfse;
	OSSynchronizeIO();
	watcher->wr = (watcher->wr + 1) % watcher->eventq_size;

	//
	// wake up the watcher if there are more than MAX_NUM_PENDING events.
	// otherwise schedule a timer (if one isn't already set) which will
	// send any pending events if no more are received in the next
	// EVENT_DELAY_IN_MS milli-seconds.
	//
	int32_t num_pending = 0;
	if (watcher->rd < watcher->wr) {
		num_pending = watcher->wr - watcher->rd;
	}

	if (watcher->rd > watcher->wr) {
		num_pending = watcher->wr + watcher->eventq_size - watcher->rd;
	}

	if (num_pending > (watcher->eventq_size * 3 / 4) && !(watcher->flags & WATCHER_APPLE_SYSTEM_SERVICE)) {
		/* Non-Apple Service is falling behind, start dropping events for this process */
		lck_rw_lock_exclusive(&event_handling_lock);
		while (watcher->rd != watcher->wr) {
			kfse = watcher->event_queue[watcher->rd];
			watcher->event_queue[watcher->rd] = NULL;
			watcher->rd = (watcher->rd + 1) % watcher->eventq_size;
			OSSynchronizeIO();
			if (kfse != NULL && kfse->type != FSE_INVALID && kfse->refcount >= 1) {
				release_event_ref(kfse);
			}
		}
		watcher->flags |= WATCHER_DROPPED_EVENTS;
		lck_rw_unlock_exclusive(&event_handling_lock);

		printf("fsevents: watcher falling behind: %s (pid: %d) rd: %4d wr: %4d q_size: %4d flags: 0x%x\n",
		    watcher->proc_name, watcher->pid, watcher->rd, watcher->wr,
		    watcher->eventq_size, watcher->flags);

		fsevents_wakeup(watcher);
	} else if (num_pending > MAX_NUM_PENDING) {
		fsevents_wakeup(watcher);
	} else if (timer_set == 0) {
		schedule_event_wakeup();
	}

	return 0;
}

static int
fill_buff(uint16_t type, int32_t size, const void *data,
    char *buff, int32_t *_buff_idx, int32_t buff_sz,
    struct uio *uio)
{
	int32_t amt, error = 0, buff_idx = *_buff_idx;
	uint16_t tmp;

	//
	// the +1 on the size is to guarantee that the main data
	// copy loop will always copy at least 1 byte
	//
	if ((buff_sz - buff_idx) <= (int)(2 * sizeof(uint16_t) + 1)) {
		if (buff_idx > uio_resid(uio)) {
			error = ENOSPC;
			goto get_out;
		}

		error = uiomove(buff, buff_idx, uio);
		if (error) {
			goto get_out;
		}
		buff_idx = 0;
	}

	// copy out the header (type & size)
	memcpy(&buff[buff_idx], &type, sizeof(uint16_t));
	buff_idx += sizeof(uint16_t);

	tmp = size & 0xffff;
	memcpy(&buff[buff_idx], &tmp, sizeof(uint16_t));
	buff_idx += sizeof(uint16_t);

	// now copy the body of the data, flushing along the way
	// if the buffer fills up.
	//
	while (size > 0) {
		amt = (size < (buff_sz - buff_idx)) ? size : (buff_sz - buff_idx);
		memcpy(&buff[buff_idx], data, amt);

		size -= amt;
		buff_idx += amt;
		data = (const char *)data + amt;
		if (size > (buff_sz - buff_idx)) {
			if (buff_idx > uio_resid(uio)) {
				error = ENOSPC;
				goto get_out;
			}
			error = uiomove(buff, buff_idx, uio);
			if (error) {
				goto get_out;
			}
			buff_idx = 0;
		}

		if (amt == 0) { // just in case...
			break;
		}
	}

get_out:
	*_buff_idx = buff_idx;

	return error;
}


static int copy_out_kfse(fs_event_watcher *watcher, kfs_event *kfse, struct uio *uio)  __attribute__((noinline));

static int
copy_out_kfse(fs_event_watcher *watcher, kfs_event *kfse, struct uio *uio)
{
	int      error;
	uint16_t tmp16;
	int32_t  type;
	kfs_event *cur;
	char     evbuff[512];
	int      evbuff_idx = 0;

	if (kfse->type == FSE_INVALID) {
		panic("fsevents: copy_out_kfse: asked to copy out an invalid event (kfse %p, refcount %d)", kfse, kfse->refcount);
	}

	if (kfse->flags & KFSE_BEING_CREATED) {
		return 0;
	}

	if (((kfse->type == FSE_RENAME) || (kfse->type == FSE_CLONE)) &&
	    kfse->regular_event.dest == NULL) {
		//
		// This can happen if an event gets recycled but we had a
		// pointer to it in our event queue.  The event is the
		// destination of a rename or clone which we'll process
		// separately (that is, another kfse points to this one
		// so it's ok to skip this guy because we'll process it
		// when we process the other one)
		error = 0;
		goto get_out;
	}

	if (watcher->flags & WATCHER_WANTS_EXTENDED_INFO) {
		type = (kfse->type & 0xfff);

		if (kfse->flags & KFSE_CONTAINS_DROPPED_EVENTS) {
			type |= (FSE_CONTAINS_DROPPED_EVENTS << FSE_FLAG_SHIFT);
		} else if (kfse->flags & KFSE_COMBINED_EVENTS) {
			type |= (FSE_COMBINED_EVENTS << FSE_FLAG_SHIFT);
		}
	} else {
		type = (int32_t)kfse->type;
	}

	// copy out the type of the event
	memcpy(evbuff, &type, sizeof(int32_t));
	evbuff_idx += sizeof(int32_t);

	// copy out the pid of the person that generated the event
	memcpy(&evbuff[evbuff_idx], &kfse->pid, sizeof(pid_t));
	evbuff_idx += sizeof(pid_t);

	cur = kfse;

copy_again:

	if (kfse->type == FSE_DOCID_CHANGED ||
	    kfse->type == FSE_DOCID_CREATED) {
		dev_t    dev     = cur->docid_event.dev;
		ino64_t  src_ino = cur->docid_event.src_ino;
		ino64_t  dst_ino = cur->docid_event.dst_ino;
		uint64_t docid   = cur->docid_event.docid;

		error = fill_buff(FSE_ARG_DEV, sizeof(dev_t), &dev, evbuff,
		    &evbuff_idx, sizeof(evbuff), uio);
		if (error != 0) {
			goto get_out;
		}

		error = fill_buff(FSE_ARG_INO, sizeof(ino64_t), &src_ino,
		    evbuff, &evbuff_idx, sizeof(evbuff), uio);
		if (error != 0) {
			goto get_out;
		}

		error = fill_buff(FSE_ARG_INO, sizeof(ino64_t), &dst_ino,
		    evbuff, &evbuff_idx, sizeof(evbuff), uio);
		if (error != 0) {
			goto get_out;
		}

		error = fill_buff(FSE_ARG_INT64, sizeof(uint64_t), &docid,
		    evbuff, &evbuff_idx, sizeof(evbuff), uio);
		if (error != 0) {
			goto get_out;
		}

		goto done;
	}

	if (kfse->type == FSE_UNMOUNT_PENDING) {
		dev_t    dev  = cur->regular_event.dev;

		error = fill_buff(FSE_ARG_DEV, sizeof(dev_t), &dev,
		    evbuff, &evbuff_idx, sizeof(evbuff), uio);
		if (error != 0) {
			goto get_out;
		}

		goto done;
	}

	if (kfse->type == FSE_ACTIVITY) {
		error = fill_buff(FSE_ARG_INT32, sizeof(cur->activity_event.version), &cur->activity_event.version,
		    evbuff, &evbuff_idx, sizeof(evbuff), uio);
		if (error != 0) {
			goto get_out;
		}
		error = fill_buff(FSE_ARG_DEV, sizeof(cur->activity_event.dev), &cur->activity_event.dev, evbuff,
		    &evbuff_idx, sizeof(evbuff), uio);
		if (error != 0) {
			goto get_out;
		}

		error = fill_buff(FSE_ARG_INO, sizeof(cur->activity_event.ino), &cur->activity_event.ino,
		    evbuff, &evbuff_idx, sizeof(evbuff), uio);
		if (error != 0) {
			goto get_out;
		}

		error = fill_buff(FSE_ARG_INT64, sizeof(cur->activity_event.origin_id), &cur->activity_event.origin_id,
		    evbuff, &evbuff_idx, sizeof(evbuff), uio);
		if (error != 0) {
			goto get_out;
		}

		error = fill_buff(FSE_ARG_INT64, sizeof(cur->activity_event.age), &cur->activity_event.age,
		    evbuff, &evbuff_idx, sizeof(evbuff), uio);
		if (error != 0) {
			goto get_out;
		}

		error = fill_buff(FSE_ARG_INT32, sizeof(cur->activity_event.use_state), &cur->activity_event.use_state,
		    evbuff, &evbuff_idx, sizeof(evbuff), uio);
		if (error != 0) {
			goto get_out;
		}

		error = fill_buff(FSE_ARG_INT32, sizeof(cur->activity_event.urgency), &cur->activity_event.urgency,
		    evbuff, &evbuff_idx, sizeof(evbuff), uio);
		if (error != 0) {
			goto get_out;
		}

		error = fill_buff(FSE_ARG_INT64, sizeof(cur->activity_event.size), &cur->activity_event.size,
		    evbuff, &evbuff_idx, sizeof(evbuff), uio);
		if (error != 0) {
			goto get_out;
		}

		goto done;
	}
#if CONFIG_FSE_ACCESS_GRANTED
	if (kfse->type == FSE_ACCESS_GRANTED) {
		//
		// KFSE_CONTAINS_DROPPED_EVENTS will be set if either
		// the path or audit token are bogus; don't copy out
		// either in that case.
		//
		if (cur->flags & KFSE_CONTAINS_DROPPED_EVENTS) {
			goto done;
		}
		error = fill_buff(FSE_ARG_STRING,
		    cur->access_granted_event.len,
		    cur->access_granted_event.str,
		    evbuff, &evbuff_idx, sizeof(evbuff), uio);
		if (error != 0) {
			goto get_out;
		}
		error = fill_buff(FSE_ARG_AUDIT_TOKEN,
		    sizeof(cur->access_granted_event.audit_token),
		    &cur->access_granted_event.audit_token,
		    evbuff, &evbuff_idx, sizeof(evbuff), uio);
		if (error != 0) {
			goto get_out;
		}

		goto done;
	}
#endif
	if (cur->regular_event.str == NULL ||
	    cur->regular_event.str[0] == '\0') {
		printf("copy_out_kfse:2: empty/short path (%s)\n",
		    cur->regular_event.str);
		error = fill_buff(FSE_ARG_STRING, 2, "/", evbuff, &evbuff_idx,
		    sizeof(evbuff), uio);
	} else {
		error = fill_buff(FSE_ARG_STRING, cur->regular_event.len,
		    cur->regular_event.str, evbuff, &evbuff_idx,
		    sizeof(evbuff), uio);
	}
	if (error != 0) {
		goto get_out;
	}

	if (cur->regular_event.dev == 0 && cur->regular_event.ino == 0) {
		// this happens when a rename event happens and the
		// destination of the rename did not previously exist.
		// it thus has no other file info so skip copying out
		// the stuff below since it isn't initialized
		goto done;
	}

	if (watcher->flags & WATCHER_WANTS_COMPACT_EVENTS) {
		// We rely on the layout of the "regular_event"
		// structure being the same as fse_info in order
		// to speed up this copy.  The nlink field in
		// fse_info is not included.
		error = fill_buff(FSE_ARG_FINFO, KFSE_INFO_COPYSIZE,
		    &cur->regular_event, evbuff, &evbuff_idx,
		    sizeof(evbuff), uio);
		if (error != 0) {
			goto get_out;
		}
	} else {
		error = fill_buff(FSE_ARG_DEV, sizeof(dev_t),
		    &cur->regular_event.dev, evbuff, &evbuff_idx,
		    sizeof(evbuff), uio);
		if (error != 0) {
			goto get_out;
		}

		error = fill_buff(FSE_ARG_INO, sizeof(ino64_t),
		    &cur->regular_event.ino, evbuff, &evbuff_idx,
		    sizeof(evbuff), uio);
		if (error != 0) {
			goto get_out;
		}

		error = fill_buff(FSE_ARG_MODE, sizeof(int32_t),
		    &cur->regular_event.mode, evbuff, &evbuff_idx,
		    sizeof(evbuff), uio);
		if (error != 0) {
			goto get_out;
		}

		error = fill_buff(FSE_ARG_UID, sizeof(uid_t),
		    &cur->regular_event.uid, evbuff, &evbuff_idx,
		    sizeof(evbuff), uio);
		if (error != 0) {
			goto get_out;
		}

		error = fill_buff(FSE_ARG_GID, sizeof(gid_t),
		    &cur->regular_event.document_id, evbuff, &evbuff_idx,
		    sizeof(evbuff), uio);
		if (error != 0) {
			goto get_out;
		}
	}

	if (cur->regular_event.dest) {
		cur = cur->regular_event.dest;
		goto copy_again;
	}

done:
	// very last thing: the time stamp
	error = fill_buff(FSE_ARG_INT64, sizeof(uint64_t), &cur->abstime,
	    evbuff, &evbuff_idx, sizeof(evbuff), uio);
	if (error != 0) {
		goto get_out;
	}

	// check if the FSE_ARG_DONE will fit
	if (sizeof(uint16_t) > sizeof(evbuff) - evbuff_idx) {
		if (evbuff_idx > uio_resid(uio)) {
			error = ENOSPC;
			goto get_out;
		}
		error = uiomove(evbuff, evbuff_idx, uio);
		if (error) {
			goto get_out;
		}
		evbuff_idx = 0;
	}

	tmp16 = FSE_ARG_DONE;
	memcpy(&evbuff[evbuff_idx], &tmp16, sizeof(uint16_t));
	evbuff_idx += sizeof(uint16_t);

	// flush any remaining data in the buffer (and hopefully
	// in most cases this is the only uiomove we'll do)
	if (evbuff_idx > uio_resid(uio)) {
		error = ENOSPC;
	} else {
		error = uiomove(evbuff, evbuff_idx, uio);
	}

get_out:

	return error;
}



static int
fmod_watch(fs_event_watcher *watcher, struct uio *uio)
{
	int               error = 0;
	user_ssize_t      last_full_event_resid;
	kfs_event        *kfse;
	uint16_t          tmp16;
	int               skipped;

	last_full_event_resid = uio_resid(uio);

	// need at least 2048 bytes of space (maxpathlen + 1 event buf)
	if (uio_resid(uio) < 2048 || watcher == NULL) {
		return EINVAL;
	}

	if (watcher->flags & WATCHER_CLOSING) {
		return 0;
	}

	if (OSAddAtomic(1, &watcher->num_readers) != 0) {
		// don't allow multiple threads to read from the fd at the same time
		OSAddAtomic(-1, &watcher->num_readers);
		return EAGAIN;
	}

restart_watch:
	if (watcher->rd == watcher->wr) {
		if (watcher->flags & WATCHER_CLOSING) {
			OSAddAtomic(-1, &watcher->num_readers);
			return 0;
		}
		OSAddAtomic(1, &watcher->blockers);

		// there's nothing to do, go to sleep
		error = tsleep((caddr_t)watcher, PUSER | PCATCH, "fsevents_empty", 0);

		OSAddAtomic(-1, &watcher->blockers);

		if (error != 0 || (watcher->flags & WATCHER_CLOSING)) {
			OSAddAtomic(-1, &watcher->num_readers);
			return error;
		}
	}

	// if we dropped events, return that as an event first
	if (watcher->flags & WATCHER_DROPPED_EVENTS) {
		int32_t val = FSE_EVENTS_DROPPED;

		error = uiomove((caddr_t)&val, sizeof(int32_t), uio);
		if (error == 0) {
			val = 0; // a fake pid
			error = uiomove((caddr_t)&val, sizeof(int32_t), uio);

			tmp16 = FSE_ARG_DONE; // makes it a consistent msg
			error = uiomove((caddr_t)&tmp16, sizeof(int16_t), uio);

			last_full_event_resid = uio_resid(uio);
		}

		if (error) {
			OSAddAtomic(-1, &watcher->num_readers);
			return error;
		}

		watcher->flags &= ~WATCHER_DROPPED_EVENTS;
	}

	skipped = 0;

	lck_rw_lock_shared(&event_handling_lock);
	while (uio_resid(uio) > 0 && watcher->rd != watcher->wr) {
		if (watcher->flags & WATCHER_CLOSING) {
			break;
		}

		//
		// check if the event is something of interest to us
		// (since it may have been recycled/reused and changed
		// its type or which device it is for)
		//
		kfse = watcher->event_queue[watcher->rd];
		if (!kfse || kfse->type == FSE_INVALID || kfse->type >= watcher->num_events || kfse->refcount < 1) {
			break;
		}

		if (watcher->event_list[kfse->type] == FSE_REPORT) {
			if (!(watcher->flags & WATCHER_APPLE_SYSTEM_SERVICE) &&
			    kfse->type != FSE_DOCID_CREATED &&
			    kfse->type != FSE_DOCID_CHANGED &&
			    kfse->type != FSE_ACTIVITY &&
			    is_ignored_directory(kfse->regular_event.str)) {
				// If this is not an Apple System Service, skip specified directories
				// radar://12034844
				error = 0;
				skipped = 1;
			} else {
				skipped = 0;
				if (last_event_ptr == kfse) {
					last_event_ptr = NULL;
					last_event_type = -1;
					last_coalesced_time = 0;
				}
				error = copy_out_kfse(watcher, kfse, uio);
				if (error != 0) {
					// if an event won't fit or encountered an error while
					// we were copying it out, then backup to the last full
					// event and just bail out.  if the error was ENOENT
					// then we can continue regular processing, otherwise
					// we should unlock things and return.
					uio_setresid(uio, last_full_event_resid);
					if (error != ENOENT) {
						lck_rw_unlock_shared(&event_handling_lock);
						error = 0;
						goto get_out;
					}
				}

				last_full_event_resid = uio_resid(uio);
			}
		}

		watcher->event_queue[watcher->rd] = NULL;
		watcher->rd = (watcher->rd + 1) % watcher->eventq_size;
		OSSynchronizeIO();
		release_event_ref(kfse);
	}
	lck_rw_unlock_shared(&event_handling_lock);

	if (skipped && error == 0) {
		goto restart_watch;
	}

get_out:
	OSAddAtomic(-1, &watcher->num_readers);

	return error;
}


//
// Shoo watchers away from a volume that's about to be unmounted
// (so that it can be cleanly unmounted).
//
void
fsevent_unmount(__unused struct mount *mp, __unused vfs_context_t ctx)
{
#if !defined(XNU_TARGET_OS_OSX)
	dev_t dev = mp->mnt_vfsstat.f_fsid.val[0];
	int error, waitcount = 0;
	struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};

	// wait for any other pending unmounts to complete
	lock_watch_table();
	while (fsevent_unmount_dev != 0) {
		error = msleep((caddr_t)&fsevent_unmount_dev, &watch_table_lock, PRIBIO, "fsevent_unmount_wait", &ts);
		if (error == EWOULDBLOCK) {
			error = 0;
		}
		if (!error && (++waitcount >= 10)) {
			error = EWOULDBLOCK;
			printf("timeout waiting to signal unmount pending for dev %d (fsevent_unmount_dev %d)\n", dev, fsevent_unmount_dev);
		}
		if (error) {
			// there's a problem, bail out
			unlock_watch_table();
			return;
		}
	}
	if (fs_event_type_watchers[FSE_UNMOUNT_PENDING] == 0) {
		// nobody watching for unmount pending events
		unlock_watch_table();
		return;
	}
	// this is now the current unmount pending
	fsevent_unmount_dev = dev;
	fsevent_unmount_ack_count = fs_event_type_watchers[FSE_UNMOUNT_PENDING];
	unlock_watch_table();

	// send an event to notify the watcher they need to get off the mount
	error = add_fsevent(FSE_UNMOUNT_PENDING, ctx, FSE_ARG_DEV, dev, FSE_ARG_DONE);

	// wait for acknowledgment(s) (give up if it takes too long)
	lock_watch_table();
	waitcount = 0;
	while (fsevent_unmount_dev == dev) {
		error = msleep((caddr_t)&fsevent_unmount_dev, &watch_table_lock, PRIBIO, "fsevent_unmount_pending", &ts);
		if (error == EWOULDBLOCK) {
			error = 0;
		}
		if (!error && (++waitcount >= 10)) {
			error = EWOULDBLOCK;
			printf("unmount pending ack timeout for dev %d\n", dev);
		}
		if (error) {
			// there's a problem, bail out
			if (fsevent_unmount_dev == dev) {
				fsevent_unmount_dev = 0;
				fsevent_unmount_ack_count = 0;
			}
			wakeup((caddr_t)&fsevent_unmount_dev);
			break;
		}
	}
	unlock_watch_table();
#endif /* ! XNU_TARGET_OS_OSX */
}


//
// /dev/fsevents device code
//
static int fsevents_installed = 0;

typedef struct fsevent_handle {
	UInt32            flags;
	SInt32            active;
	fs_event_watcher *watcher;
	struct klist      knotes;
	struct selinfo    si;
} fsevent_handle;

#define FSEH_CLOSING   0x0001

static int
fseventsf_read(struct fileproc *fp, struct uio *uio,
    __unused int flags, __unused vfs_context_t ctx)
{
	fsevent_handle *fseh = (struct fsevent_handle *)fp_get_data(fp);
	int error;

	error = fmod_watch(fseh->watcher, uio);

	return error;
}


#pragma pack(push, 4)
typedef struct fsevent_dev_filter_args32 {
	uint32_t            num_devices;
	user32_addr_t       devices;
} fsevent_dev_filter_args32;
typedef struct fsevent_dev_filter_args64 {
	uint32_t            num_devices;
	user64_addr_t       devices;
} fsevent_dev_filter_args64;
#pragma pack(pop)

#define FSEVENTS_DEVICE_FILTER_32       _IOW('s', 100, fsevent_dev_filter_args32)
#define FSEVENTS_DEVICE_FILTER_64       _IOW('s', 100, fsevent_dev_filter_args64)

static int
fseventsf_ioctl(struct fileproc *fp, u_long cmd, caddr_t data, vfs_context_t ctx)
{
	fsevent_handle *fseh = (struct fsevent_handle *)fp_get_data(fp);
	int ret = 0;
	fsevent_dev_filter_args64 *devfilt_args, _devfilt_args;

	OSAddAtomic(1, &fseh->active);
	if (fseh->flags & FSEH_CLOSING) {
		OSAddAtomic(-1, &fseh->active);
		return 0;
	}

	switch (cmd) {
	case FIONBIO:
	case FIOASYNC:
		break;

	case FSEVENTS_WANT_COMPACT_EVENTS: {
		fseh->watcher->flags |= WATCHER_WANTS_COMPACT_EVENTS;
		break;
	}

	case FSEVENTS_WANT_EXTENDED_INFO: {
		fseh->watcher->flags |= WATCHER_WANTS_EXTENDED_INFO;
		break;
	}

	case FSEVENTS_GET_CURRENT_ID: {
		*(uint64_t *)data = fseh->watcher->max_event_id;
		ret = 0;
		break;
	}

	case FSEVENTS_DEVICE_FILTER_32: {
		if (proc_is64bit(vfs_context_proc(ctx))) {
			ret = EINVAL;
			break;
		}
		fsevent_dev_filter_args32 *devfilt_args32 = (fsevent_dev_filter_args32 *)data;

		devfilt_args = &_devfilt_args;
		memset(devfilt_args, 0, sizeof(fsevent_dev_filter_args64));
		devfilt_args->num_devices = devfilt_args32->num_devices;
		devfilt_args->devices     = CAST_USER_ADDR_T(devfilt_args32->devices);
		goto handle_dev_filter;
	}

	case FSEVENTS_DEVICE_FILTER_64:
		if (!proc_is64bit(vfs_context_proc(ctx))) {
			ret = EINVAL;
			break;
		}
		devfilt_args = (fsevent_dev_filter_args64 *)data;

handle_dev_filter:
		{
			int new_num_devices, old_num_devices = 0;
			dev_t *devices_not_to_watch, *tmp = NULL;

			if (devfilt_args->num_devices > 256) {
				ret = EINVAL;
				break;
			}

			new_num_devices = devfilt_args->num_devices;
			if (new_num_devices == 0) {
				lock_watch_table();

				tmp = fseh->watcher->devices_not_to_watch;
				fseh->watcher->devices_not_to_watch = NULL;
				old_num_devices = fseh->watcher->num_devices;
				fseh->watcher->num_devices = new_num_devices;

				unlock_watch_table();
				kfree_data(tmp, old_num_devices * sizeof(dev_t));
				break;
			}

			devices_not_to_watch = kalloc_data(new_num_devices * sizeof(dev_t), Z_WAITOK);
			if (devices_not_to_watch == NULL) {
				ret = ENOMEM;
				break;
			}

			ret = copyin((user_addr_t)devfilt_args->devices,
			    (void *)devices_not_to_watch,
			    new_num_devices * sizeof(dev_t));
			if (ret) {
				kfree_data(devices_not_to_watch, new_num_devices * sizeof(dev_t));
				break;
			}

			lock_watch_table();
			old_num_devices = fseh->watcher->num_devices;
			fseh->watcher->num_devices = new_num_devices;
			tmp = fseh->watcher->devices_not_to_watch;
			fseh->watcher->devices_not_to_watch = devices_not_to_watch;
			unlock_watch_table();

			kfree_data(tmp, old_num_devices * sizeof(dev_t));

			break;
		}

	case FSEVENTS_UNMOUNT_PENDING_ACK: {
		lock_watch_table();
		dev_t dev = *(dev_t *)data;
		if (fsevent_unmount_dev == dev) {
			if (--fsevent_unmount_ack_count <= 0) {
				fsevent_unmount_dev = 0;
				wakeup((caddr_t)&fsevent_unmount_dev);
			}
		} else {
			printf("unexpected unmount pending ack %d (%d)\n", dev, fsevent_unmount_dev);
			ret = EINVAL;
		}
		unlock_watch_table();
		break;
	}

	default:
		ret = EINVAL;
		break;
	}

	OSAddAtomic(-1, &fseh->active);
	return ret;
}


static int
fseventsf_select(struct fileproc *fp, int which, __unused void *wql, vfs_context_t ctx)
{
	fsevent_handle *fseh = (struct fsevent_handle *)fp_get_data(fp);
	int ready = 0;

	if ((which != FREAD) || (fseh->watcher->flags & WATCHER_CLOSING)) {
		return 0;
	}


	// if there's nothing in the queue, we're not ready
	if (fseh->watcher->rd != fseh->watcher->wr) {
		ready = 1;
	}

	if (!ready) {
		lock_watch_table();
		selrecord(vfs_context_proc(ctx), &fseh->si, wql);
		unlock_watch_table();
	}

	return ready;
}


#if NOTUSED
static int
fseventsf_stat(__unused struct fileproc *fp, __unused struct stat *sb, __unused vfs_context_t ctx)
{
	return ENOTSUP;
}
#endif

static int
fseventsf_close(struct fileglob *fg, __unused vfs_context_t ctx)
{
	fsevent_handle *fseh = (struct fsevent_handle *)fg_get_data(fg);
	fs_event_watcher *watcher;

	OSBitOrAtomic(FSEH_CLOSING, &fseh->flags);
	while (OSAddAtomic(0, &fseh->active) > 0) {
		tsleep((caddr_t)fseh->watcher, PRIBIO, "fsevents-close", 1);
	}

	watcher = fseh->watcher;
	fg_set_data(fg, NULL);
	fseh->watcher = NULL;

	remove_watcher(watcher);
	selthreadclear(&fseh->si);
	kfree_type(fsevent_handle, fseh);

	return 0;
}

static void
filt_fsevent_detach(struct knote *kn)
{
	fsevent_handle *fseh = (struct fsevent_handle *)knote_kn_hook_get_raw(kn);

	lock_watch_table();

	KNOTE_DETACH(&fseh->knotes, kn);

	unlock_watch_table();
}

/*
 * Determine whether this knote should be active
 *
 * This is kind of subtle.
 *      --First, notice if the vnode has been revoked: in so, override hint
 *      --EVFILT_READ knotes are checked no matter what the hint is
 *      --Other knotes activate based on hint.
 *      --If hint is revoke, set special flags and activate
 */
static int
filt_fsevent_common(struct knote *kn, struct kevent_qos_s *kev, long hint)
{
	fsevent_handle *fseh = (struct fsevent_handle *)knote_kn_hook_get_raw(kn);
	int activate = 0;
	int32_t rd, wr, amt;
	int64_t data = 0;

	if (NOTE_REVOKE == hint) {
		kn->kn_flags |= (EV_EOF | EV_ONESHOT);
		activate = 1;
	}

	rd = fseh->watcher->rd;
	wr = fseh->watcher->wr;
	if (rd <= wr) {
		amt = wr - rd;
	} else {
		amt = fseh->watcher->eventq_size - (rd - wr);
	}

	switch (kn->kn_filter) {
	case EVFILT_READ:
		data = amt;
		activate = (data != 0);
		break;
	case EVFILT_VNODE:
		/* Check events this note matches against the hint */
		if (kn->kn_sfflags & hint) {
			kn->kn_fflags |= (uint32_t)hint;     /* Set which event occurred */
		}
		if (kn->kn_fflags != 0) {
			activate = 1;
		}
		break;
	default:
		// nothing to do...
		break;
	}

	if (activate && kev) {
		knote_fill_kevent(kn, kev, data);
	}
	return activate;
}

static int
filt_fsevent(struct knote *kn, long hint)
{
	return filt_fsevent_common(kn, NULL, hint);
}

static int
filt_fsevent_touch(struct knote *kn, struct kevent_qos_s *kev)
{
	int res;

	lock_watch_table();

	/* accept new fflags/data as saved */
	kn->kn_sfflags = kev->fflags;
	kn->kn_sdata = kev->data;

	/* restrict the current results to the (smaller?) set of new interest */
	/*
	 * For compatibility with previous implementations, we leave kn_fflags
	 * as they were before.
	 */
	//kn->kn_fflags &= kev->fflags;

	/* determine if the filter is now fired */
	res = filt_fsevent_common(kn, NULL, 0);

	unlock_watch_table();

	return res;
}

static int
filt_fsevent_process(struct knote *kn, struct kevent_qos_s *kev)
{
	int res;

	lock_watch_table();

	res = filt_fsevent_common(kn, kev, 0);

	unlock_watch_table();

	return res;
}

SECURITY_READ_ONLY_EARLY(struct  filterops) fsevent_filtops = {
	.f_isfd = 1,
	.f_attach = NULL,
	.f_detach = filt_fsevent_detach,
	.f_event = filt_fsevent,
	.f_touch = filt_fsevent_touch,
	.f_process = filt_fsevent_process,
};

static int
fseventsf_kqfilter(struct fileproc *fp, struct knote *kn,
    __unused struct kevent_qos_s *kev)
{
	fsevent_handle *fseh = (struct fsevent_handle *)fp_get_data(fp);
	int res;

	kn->kn_filtid = EVFILTID_FSEVENT;
	knote_kn_hook_set_raw(kn, (void *) fseh);

	lock_watch_table();

	KNOTE_ATTACH(&fseh->knotes, kn);

	/* check to see if it is fired already */
	res = filt_fsevent_common(kn, NULL, 0);

	unlock_watch_table();

	return res;
}


static int
fseventsf_drain(struct fileproc *fp, __unused vfs_context_t ctx)
{
	int counter = 0;
	fsevent_handle *fseh = (struct fsevent_handle *)fp_get_data(fp);

	// if there are people still waiting, sleep for 10ms to
	// let them clean up and get out of there.  however we
	// also don't want to get stuck forever so if they don't
	// exit after 5 seconds we're tearing things down anyway.
	while (fseh->watcher->blockers && counter++ < 500) {
		// issue wakeup in case anyone is blocked waiting for an event
		// do this each time we wakeup in case the blocker missed
		// the wakeup due to the unprotected test of WATCHER_CLOSING
		// and decision to tsleep in fmod_watch... this bit of
		// latency is a decent tradeoff against not having to
		// take and drop a lock in fmod_watch
		lock_watch_table();
		fsevents_wakeup(fseh->watcher);
		unlock_watch_table();

		tsleep((caddr_t)fseh->watcher, PRIBIO, "watcher-close", 1);
	}

	return 0;
}


static int
fseventsopen(__unused dev_t dev, __unused int flag, __unused int mode, __unused struct proc *p)
{
	if (!kauth_cred_issuser(kauth_cred_get())) {
		return EPERM;
	}

	return 0;
}

static int
fseventsclose(__unused dev_t dev, __unused int flag, __unused int mode, __unused struct proc *p)
{
	return 0;
}

static int
fseventsread(__unused dev_t dev, __unused struct uio *uio, __unused int ioflag)
{
	return EIO;
}


static int
parse_buffer_and_add_events(const char *buffer, size_t bufsize, vfs_context_t ctx, size_t *remainder)
{
	const fse_info *finfo, *dest_finfo;
	const char *path, *ptr, *dest_path, *event_start = buffer;
	size_t path_len, dest_path_len;
	int type, err = 0;


	ptr = buffer;
	while ((ptr + sizeof(int) + sizeof(fse_info) + 1) < buffer + bufsize) {
		type = *(const int *)ptr;
		if (type < 0 || type == FSE_ACCESS_GRANTED || type == FSE_ACTIVITY ||
		    type >= FSE_MAX_EVENTS) {
			err = EINVAL;
			break;
		}

		ptr += sizeof(int);

		finfo = (const fse_info *)ptr;
		ptr += sizeof(fse_info);

		path = ptr;
		while (ptr < buffer + bufsize && *ptr != '\0') {
			ptr++;
		}

		if (ptr >= buffer + bufsize) {
			break;
		}

		ptr++; // advance over the trailing '\0'

		path_len = ptr - path;

		if (type != FSE_RENAME && type != FSE_EXCHANGE && type != FSE_CLONE) {
			event_start = ptr; // record where the next event starts

			err = add_fsevent(type, ctx, FSE_ARG_STRING, path_len, path, FSE_ARG_FINFO, finfo, FSE_ARG_DONE);
			if (err) {
				break;
			}
			continue;
		}

		//
		// if we're here we have to slurp up the destination finfo
		// and path so that we can pass them to the add_fsevent()
		// call.  basically it's a copy of the above code.
		//
		dest_finfo = (const fse_info *)ptr;
		ptr += sizeof(fse_info);

		dest_path = ptr;
		while (ptr < buffer + bufsize && *ptr != '\0') {
			ptr++;
		}

		if (ptr >= buffer + bufsize) {
			break;
		}

		ptr++;       // advance over the trailing '\0'
		event_start = ptr; // record where the next event starts

		dest_path_len = ptr - dest_path;
		//
		// If the destination inode number is non-zero, generate a rename
		// with both source and destination FSE_ARG_FINFO. Otherwise generate
		// a rename with only one FSE_ARG_FINFO. If you need to inject an
		// exchange with an inode of zero, just make that inode (and its path)
		// come in as the first one, not the second.
		//
		if (dest_finfo->ino) {
			err = add_fsevent(type, ctx,
			    FSE_ARG_STRING, path_len, path, FSE_ARG_FINFO, finfo,
			    FSE_ARG_STRING, dest_path_len, dest_path, FSE_ARG_FINFO, dest_finfo,
			    FSE_ARG_DONE);
		} else {
			err = add_fsevent(type, ctx,
			    FSE_ARG_STRING, path_len, path, FSE_ARG_FINFO, finfo,
			    FSE_ARG_STRING, dest_path_len, dest_path,
			    FSE_ARG_DONE);
		}

		if (err) {
			break;
		}
	}

	// if the last event wasn't complete, set the remainder
	// to be the last event start boundary.
	//
	*remainder = (long)((buffer + bufsize) - event_start);

	return err;
}


//
// Note: this buffer size can not ever be less than
//       2*MAXPATHLEN + 2*sizeof(fse_info) + sizeof(int)
//       because that is the max size for a single event.
//       I made it 4k to be a "nice" size.  making it
//       smaller is not a good idea.
//
#define WRITE_BUFFER_SIZE  4096
static char *write_buffer = NULL;

static int
fseventswrite(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int error = 0;
	size_t count, offset = 0, remainder = 0;
	vfs_context_t ctx = vfs_context_current();

	lck_mtx_lock(&event_writer_lock);

	if (write_buffer == NULL) {
		write_buffer = zalloc_permanent(WRITE_BUFFER_SIZE, ZALIGN_64);
	}

	//
	// this loop copies in and processes the events written.
	// it takes care to copy in reasonable size chunks and
	// process them.  if there is an event that spans a chunk
	// boundary we're careful to copy those bytes down to the
	// beginning of the buffer and read the next chunk in just
	// after it.
	//
	while (uio_resid(uio)) {
		count = MIN(WRITE_BUFFER_SIZE - offset, (size_t)uio_resid(uio));

		error = uiomove(write_buffer + offset, (int)count, uio);
		if (error) {
			break;
		}

		error = parse_buffer_and_add_events(write_buffer, offset + count, ctx, &remainder);
		if (error) {
			break;
		}

		//
		// if there's any remainder, copy it down to the beginning
		// of the buffer so that it will get processed the next time
		// through the loop.  note that the remainder always starts
		// at an event boundary.
		//
		memmove(write_buffer, (write_buffer + count + offset) - remainder, remainder);
		offset = remainder;
	}

	lck_mtx_unlock(&event_writer_lock);

	return error;
}


static const struct fileops fsevents_fops = {
	.fo_type     = DTYPE_FSEVENTS,
	.fo_read     = fseventsf_read,
	.fo_write    = fo_no_write,
	.fo_ioctl    = fseventsf_ioctl,
	.fo_select   = fseventsf_select,
	.fo_close    = fseventsf_close,
	.fo_kqfilter = fseventsf_kqfilter,
	.fo_drain    = fseventsf_drain,
};

typedef struct fsevent_clone_args32 {
	user32_addr_t       event_list;
	int32_t             num_events;
	int32_t             event_queue_depth;
	user32_addr_t       fd;
} fsevent_clone_args32;

typedef struct fsevent_clone_args64 {
	user64_addr_t       event_list;
	int32_t             num_events;
	int32_t             event_queue_depth;
	user64_addr_t       fd;
} fsevent_clone_args64;

#define FSEVENTS_CLONE_32       _IOW('s', 1, fsevent_clone_args32)
#define FSEVENTS_CLONE_64       _IOW('s', 1, fsevent_clone_args64)

static int
fseventsioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag, struct proc *p)
{
	struct fileproc *f;
	int fd, error;
	fsevent_handle *fseh = NULL;
	fsevent_clone_args64 *fse_clone_args, _fse_clone;
	int8_t *event_list;
	int is64bit = proc_is64bit(p);

	switch (cmd) {
	case FSEVENTS_CLONE_32: {
		if (is64bit) {
			return EINVAL;
		}
		fsevent_clone_args32 *args32 = (fsevent_clone_args32 *)data;

		fse_clone_args = &_fse_clone;
		memset(fse_clone_args, 0, sizeof(fsevent_clone_args64));

		fse_clone_args->event_list        = CAST_USER_ADDR_T(args32->event_list);
		fse_clone_args->num_events        = args32->num_events;
		fse_clone_args->event_queue_depth = args32->event_queue_depth;
		fse_clone_args->fd                = CAST_USER_ADDR_T(args32->fd);
		goto handle_clone;
	}

	case FSEVENTS_CLONE_64:
		if (!is64bit) {
			return EINVAL;
		}
		fse_clone_args = (fsevent_clone_args64 *)data;

handle_clone:
		if (fse_clone_args->num_events <= 0 || fse_clone_args->num_events > 4096) {
			return EINVAL;
		}

		fseh = kalloc_type(fsevent_handle, Z_WAITOK | Z_ZERO | Z_NOFAIL);

		klist_init(&fseh->knotes);

		event_list = kalloc_data(fse_clone_args->num_events * sizeof(int8_t), Z_WAITOK);
		if (event_list == NULL) {
			kfree_type(fsevent_handle, fseh);
			return ENOMEM;
		}

		error = copyin((user_addr_t)fse_clone_args->event_list,
		    (void *)event_list,
		    fse_clone_args->num_events * sizeof(int8_t));
		if (error) {
			kfree_data(event_list, fse_clone_args->num_events * sizeof(int8_t));
			kfree_type(fsevent_handle, fseh);
			return error;
		}

		/*
		 * Lock down the user's "fd" result buffer so it's safe
		 * to hold locks while we copy it out.
		 */
		error = vslock((user_addr_t)fse_clone_args->fd,
		    sizeof(int32_t));
		if (error) {
			kfree_data(event_list, fse_clone_args->num_events * sizeof(int8_t));
			kfree_type(fsevent_handle, fseh);
			return error;
		}

		error = add_watcher(event_list,
		    fse_clone_args->num_events,
		    fse_clone_args->event_queue_depth,
		    &fseh->watcher,
		    fseh);
		if (error) {
			vsunlock((user_addr_t)fse_clone_args->fd,
			    sizeof(int32_t), 0);
			kfree_data(event_list, fse_clone_args->num_events * sizeof(int8_t));
			kfree_type(fsevent_handle, fseh);
			return error;
		}

		fseh->watcher->fseh = fseh;

		error = falloc(p, &f, &fd);
		if (error) {
			remove_watcher(fseh->watcher);
			vsunlock((user_addr_t)fse_clone_args->fd,
			    sizeof(int32_t), 0);
			kfree_data(event_list, fse_clone_args->num_events * sizeof(int8_t));
			kfree_type(fsevent_handle, fseh);
			return error;
		}
		proc_fdlock(p);
		f->fp_glob->fg_flag = FREAD | FWRITE;
		f->fp_glob->fg_ops = &fsevents_fops;
		fp_set_data(f, fseh);

		/*
		 * We can safely hold the proc_fdlock across this copyout()
		 * because of the vslock() call above.  The vslock() call
		 * also ensures that we will never get an error, so assert
		 * this.
		 */
		error = copyout((void *)&fd, (user_addr_t)fse_clone_args->fd, sizeof(int32_t));
		assert(error == 0);

		procfdtbl_releasefd(p, fd, NULL);
		fp_drop(p, fd, f, 1);
		proc_fdunlock(p);

		vsunlock((user_addr_t)fse_clone_args->fd,
		    sizeof(int32_t), 1);
		break;

	default:
		error = EINVAL;
		break;
	}

	return error;
}

static void
fsevents_wakeup(fs_event_watcher *watcher)
{
	selwakeup(&watcher->fseh->si);
	KNOTE(&watcher->fseh->knotes, NOTE_WRITE | NOTE_NONE);
	wakeup((caddr_t)watcher);
}


/*
 * A struct describing which functions will get invoked for certain
 * actions.
 */
static const struct cdevsw fsevents_cdevsw =
{
	.d_open = fseventsopen,
	.d_close = fseventsclose,
	.d_read = fseventsread,
	.d_write = fseventswrite,
	.d_ioctl = fseventsioctl,
	.d_stop = eno_stop,
	.d_reset = eno_reset,
	.d_select = eno_select,
	.d_mmap = eno_mmap,
	.d_strategy = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
};


/*
 * Called to initialize our device,
 * and to register ourselves with devfs
 */

void
fsevents_init(void)
{
	int ret;

	if (fsevents_installed) {
		return;
	}

	fsevents_installed = 1;

	ret = cdevsw_add(-1, &fsevents_cdevsw);
	if (ret < 0) {
		fsevents_installed = 0;
		return;
	}

	devfs_make_node(makedev(ret, 0), DEVFS_CHAR,
	    UID_ROOT, GID_WHEEL, 0644, "fsevents");

	fsevents_internal_init();
}


char *
get_pathbuff(void)
{
	return zalloc(ZV_NAMEI);
}

void
release_pathbuff(char *path)
{
	if (path == NULL) {
		return;
	}
	zfree(ZV_NAMEI, path);
}

int
get_fse_info(struct vnode *vp, fse_info *fse, __unused vfs_context_t ctx)
{
	struct vnode_attr va;

	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_fsid);
	va.va_vaflags |= VA_REALFSID;
	VATTR_WANTED(&va, va_fileid);
	VATTR_WANTED(&va, va_mode);
	VATTR_WANTED(&va, va_uid);
	VATTR_WANTED(&va, va_document_id);
	if (vp->v_flag & VISHARDLINK) {
		if (vp->v_type == VDIR) {
			VATTR_WANTED(&va, va_dirlinkcount);
		} else {
			VATTR_WANTED(&va, va_nlink);
		}
	}

	if (vnode_getattr(vp, &va, vfs_context_kernel()) != 0) {
		memset(fse, 0, sizeof(fse_info));
		return -1;
	}

	return vnode_get_fse_info_from_vap(vp, fse, &va);
}

int
vnode_get_fse_info_from_vap(vnode_t vp, fse_info *fse, struct vnode_attr *vap)
{
	fse->ino  = (ino64_t)vap->va_fileid;
	fse->dev  = (dev_t)vap->va_fsid;
	fse->mode = (int32_t)vnode_vttoif(vnode_vtype(vp)) | vap->va_mode;
	fse->uid  = (uid_t)vap->va_uid;
	fse->document_id  = (uint32_t)vap->va_document_id;
	if (vp->v_flag & VISHARDLINK) {
		fse->mode |= FSE_MODE_HLINK;
		if (vp->v_type == VDIR) {
			fse->nlink = (uint64_t)vap->va_dirlinkcount;
		} else {
			fse->nlink = (uint64_t)vap->va_nlink;
		}
	}

	return 0;
}

void
create_fsevent_from_kevent(vnode_t vp, uint32_t kevents, struct vnode_attr *vap)
{
	int fsevent_type = FSE_CONTENT_MODIFIED, len; // the default is the most pessimistic
	char pathbuf[MAXPATHLEN];
	fse_info fse;


	if (kevents & VNODE_EVENT_DELETE) {
		fsevent_type = FSE_DELETE;
	} else if (kevents & (VNODE_EVENT_EXTEND | VNODE_EVENT_WRITE)) {
		fsevent_type = FSE_CONTENT_MODIFIED;
	} else if (kevents & VNODE_EVENT_LINK) {
		fsevent_type = FSE_CREATE_FILE;
	} else if (kevents & VNODE_EVENT_RENAME) {
		fsevent_type = FSE_CREATE_FILE; // XXXdbg - should use FSE_RENAME but we don't have the destination info;
	} else if (kevents & (VNODE_EVENT_FILE_CREATED | VNODE_EVENT_FILE_REMOVED | VNODE_EVENT_DIR_CREATED | VNODE_EVENT_DIR_REMOVED)) {
		fsevent_type = FSE_STAT_CHANGED; // XXXdbg - because vp is a dir and the thing created/removed lived inside it
	} else { // a catch all for VNODE_EVENT_PERMS, VNODE_EVENT_ATTRIB and anything else
		fsevent_type = FSE_STAT_CHANGED;
	}

	// printf("convert_kevent: kevents 0x%x fsevent type 0x%x (for %s)\n", kevents, fsevent_type, vp->v_name ? vp->v_name : "(no-name)");

	fse.dev = vap->va_fsid;
	fse.ino = vap->va_fileid;
	fse.mode = vnode_vttoif(vnode_vtype(vp)) | (uint32_t)vap->va_mode;
	if (vp->v_flag & VISHARDLINK) {
		fse.mode |= FSE_MODE_HLINK;
		if (vp->v_type == VDIR) {
			fse.nlink = vap->va_dirlinkcount;
		} else {
			fse.nlink = vap->va_nlink;
		}
	}

	if (vp->v_type == VDIR) {
		fse.mode |= FSE_REMOTE_DIR_EVENT;
	}


	fse.uid = vap->va_uid;
	fse.document_id = vap->va_document_id;

	len = sizeof(pathbuf);
	if (vn_getpath_no_firmlink(vp, pathbuf, &len) == 0) {
		add_fsevent(fsevent_type, vfs_context_current(), FSE_ARG_STRING, len, pathbuf, FSE_ARG_FINFO, &fse, FSE_ARG_DONE);
	}
	return;
}

#else /* CONFIG_FSE */

#include <sys/fsevents.h>

/*
 * The get_pathbuff and release_pathbuff routines are used in places not
 * related to fsevents, and it's a handy abstraction, so define trivial
 * versions that don't cache a pool of buffers.  This way, we don't have
 * to conditionalize the callers, and they still get the advantage of the
 * pool of buffers if CONFIG_FSE is turned on.
 */
char *
get_pathbuff(void)
{
	return zalloc(ZV_NAMEI);
}

void
release_pathbuff(char *path)
{
	zfree(ZV_NAMEI, path);
}

int
add_fsevent(__unused int type, __unused vfs_context_t ctx, ...)
{
	return 0;
}

int
need_fsevent(__unused int type, __unused vnode_t vp)
{
	return 0;
}

#endif /* CONFIG_FSE */
