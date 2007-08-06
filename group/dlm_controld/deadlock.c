/******************************************************************************
*******************************************************************************
**
**  Copyright (C) 2007 Red Hat, Inc.  All rights reserved.
**
**  This copyrighted material is made available to anyone wishing to use,
**  modify, copy, or redistribute it subject to the terms and conditions
**  of the GNU General Public License v.2.
**
*******************************************************************************
******************************************************************************/

#include "dlm_daemon.h"
#include "libdlm.h"

int deadlock_enabled = 0;

extern struct list_head lockspaces;
extern int our_nodeid;

static SaCkptHandleT global_ckpt_h;
static SaCkptCallbacksT callbacks = { 0, 0 };
static SaVersionT version = { 'B', 1, 1 };
static char section_buf[10 * 1024 * 1024];  /* 10MB of pack_lock's enough? */
static uint32_t section_len;
static uint32_t section_max;

struct node {
	struct list_head	list;
	int			nodeid;
	int			checkpoint_ready;
};

/* from linux/fs/dlm/dlm_internal.h */
#define DLM_LKSTS_WAITING       1
#define DLM_LKSTS_GRANTED       2
#define DLM_LKSTS_CONVERT       3

struct pack_lock {
	uint64_t		xid;
	uint32_t		id;
	int			nodeid;
	uint32_t		remid;
	int			ownpid;
	uint32_t		exflags;
	uint32_t		flags;
	int8_t			status;
	int8_t			grmode;
	int8_t			rqmode;
	int8_t			pad;
};

struct dlm_rsb {
	struct list_head	list;
	struct list_head	locks;
	char			name[DLM_RESNAME_MAXLEN];
	int			len;
};

struct dlm_lkb {
	struct list_head        list;       /* r->locks */
	struct pack_lock	lock;       /* data from debugfs/checkpoint */
	unsigned int		time;       /* waiting time read from debugfs */
	int			from;       /* node that checkpointed the lock*/
	struct dlm_rsb		*rsb;       /* lock against this resource */
	struct trans		*trans;     /* lock owned by this transaction */
	struct list_head	trans_list; /* tr->locks */
};

/* waitfor pointers alloc'ed 4 at at time */
#define TR_NALLOC		4

struct trans {
	struct list_head	list;
	struct list_head	locks;
	uint64_t		xid;
	int			others_waiting_on_us; /* count of trans's
							 pointing to us in
							 waitfor */
	int			waitfor_alloc;
	int			waitfor_count;        /* count of in-use
							 waitfor slots */
	struct trans		**waitfor;	      /* waitfor_alloc trans
							 pointers */
};

#define DLM_HEADER_MAJOR	1
#define DLM_HEADER_MINOR	0
#define DLM_HEADER_PATCH	0

#define DLM_MSG_CYCLE_START	 1
#define DLM_MSG_CHECKPOINT_READY 2
#define DLM_MSG_CANCEL_LOCK	 3

struct dlm_header {
	uint16_t		version[3];
	uint16_t		type; /* MSG_ */
	uint32_t		nodeid; /* sender */
	uint32_t		to_nodeid; /* 0 if to all */
	uint32_t		global_id;
	uint32_t		lkid;
	uint32_t		pad;
	char			name[MAXNAME];
};

static const int __dlm_compat_matrix[8][8] = {
      /* UN NL CR CW PR PW EX PD */
        {1, 1, 1, 1, 1, 1, 1, 0},       /* UN */
        {1, 1, 1, 1, 1, 1, 1, 0},       /* NL */
        {1, 1, 1, 1, 1, 1, 0, 0},       /* CR */
        {1, 1, 1, 1, 0, 0, 0, 0},       /* CW */
        {1, 1, 1, 0, 1, 0, 0, 0},       /* PR */
        {1, 1, 1, 0, 0, 0, 0, 0},       /* PW */
        {1, 1, 0, 0, 0, 0, 0, 0},       /* EX */
        {0, 0, 0, 0, 0, 0, 0, 0}        /* PD */
};

static inline int dlm_modes_compat(int mode1, int mode2)
{
	return __dlm_compat_matrix[mode1 + 1][mode2 + 1];
}

static void free_resources(struct lockspace *ls)
{
	struct dlm_rsb *r, *r_safe;
	struct dlm_lkb *lkb, *lkb_safe;

	list_for_each_entry_safe(r, r_safe, &ls->resources, list) {
		list_for_each_entry_safe(lkb, lkb_safe, &r->locks, list) {
			list_del(&lkb->list);
			free(lkb);
		}
		list_del(&r->list);
		free(r);
	}
}

static void disable_deadlock(void)
{
	log_error("FIXME: deadlock detection disabled");
}

void setup_deadlock(void)
{
	SaAisErrorT rv;

	if (!deadlock_enabled)
		return;

	rv = saCkptInitialize(&global_ckpt_h, &callbacks, &version);
	if (rv != SA_AIS_OK)
		log_error("ckpt init error %d", rv);
}

/* FIXME: use private data hooks into libcpg to save ls */

static struct lockspace *find_ls_by_handle(cpg_handle_t h)
{
	struct lockspace *ls;

	list_for_each_entry(ls, &lockspaces, list) {
		if (ls->cpg_h == h)
			return ls;
	}
	return NULL;
}

static struct dlm_rsb *get_resource(struct lockspace *ls, char *name, int len)
{
	struct dlm_rsb *r;

	list_for_each_entry(r, &ls->resources, list) {
		if (r->len == len && !strncmp(r->name, name, len))
			return r;
	}

	r = malloc(sizeof(struct dlm_rsb));
	if (!r) {
		log_error("get_resource: no memory");
		disable_deadlock();
		return NULL;
	}
	memset(r, 0, sizeof(struct dlm_rsb));
	memcpy(r->name, name, len);
	r->len = len;
	INIT_LIST_HEAD(&r->locks);
	list_add(&r->list, &ls->resources);
	return r;
}

static struct dlm_lkb *create_lkb(void)
{
	struct dlm_lkb *lkb;

	lkb = malloc(sizeof(struct dlm_lkb));
	if (!lkb) {
		log_error("create_lkb: no memory");
		disable_deadlock();
	} else {
		memset(lkb, 0, sizeof(struct dlm_lkb));
		INIT_LIST_HEAD(&lkb->trans_list);
	}
	return lkb;
}

static void add_lkb(struct dlm_rsb *r, struct dlm_lkb *lkb)
{
	list_add(&lkb->list, &r->locks);
	lkb->rsb = r;
}

#define LOCK_LINE_MAX 1024

static void parse_r_name(char *line, char *name)
{
	char *p;
	int i = 0;
	int begin = 0;

	for (p = line; ; p++) {
		if (*p == '"') {
			if (begin)
				break;
			begin = 1;
			continue;
		}
		if (begin)
			name[i++] = *p;
	}
}

/* old/original way of dumping (only master state) in 5.1 kernel */

static int read_debugfs_master(struct lockspace *ls)
{
	FILE *file;
	char path[PATH_MAX];
	char line[LOCK_LINE_MAX];
	struct dlm_rsb *r;
	struct dlm_lkb *lkb;
	char r_name[65];
	unsigned long long xid;
	int r_len;
	int rv;

	snprintf(path, PATH_MAX, "/sys/kernel/debug/dlm/%s_master", ls->name);

	file = fopen(path, "r");
	if (!file)
		return -1;

	/* skip the header on the first line */
	fgets(line, LOCK_LINE_MAX, file);

	while (fgets(line, LOCK_LINE_MAX, file)) {
		lkb = create_lkb();
		if (!lkb)
			break;

		rv = sscanf(line, "%x %d %x %u %llu %x %hhd %hhd %hhd %u %d",
			    &lkb->lock.id,
			    &lkb->lock.nodeid,
			    &lkb->lock.remid,
			    &lkb->lock.ownpid,
			    &xid,
			    &lkb->lock.exflags,
			    &lkb->lock.status,
			    &lkb->lock.grmode,
			    &lkb->lock.rqmode,
			    &lkb->time,
			    &r_len);

		lkb->lock.xid = xid; /* hack to avoid warning */

		log_debug("%s", line);

		if (rv != 11) {
			log_error("invalid debugfs line %d: %s", rv, line);
			free(lkb);
			goto out;
		}

		memset(r_name, 0, sizeof(r_name));
		parse_r_name(line, r_name);

		r = get_resource(ls, r_name, r_len);
		if (!r)
			break;
		add_lkb(r, lkb);
	}
 out:
	fclose(file);
	return 0;
}

static int read_debugfs_locks(struct lockspace *ls)
{
	FILE *file;
	char path[PATH_MAX];
	char line[LOCK_LINE_MAX];
	struct dlm_rsb *r;
	struct dlm_lkb *lkb;
	char r_name[65];
	unsigned long long xid;
	int r_nodeid;
	int r_len;
	int rv;

	snprintf(path, PATH_MAX, "/sys/kernel/debug/dlm/%s_locks", ls->name);

	file = fopen(path, "r");
	if (!file)
		return -1;

	/* skip the header on the first line */
	fgets(line, LOCK_LINE_MAX, file);

	while (fgets(line, LOCK_LINE_MAX, file)) {
		lkb = create_lkb();
		if (!lkb)
			break;

		rv = sscanf(line, "%x %d %x %u %llu %x %x %hhd %hhd %hhd %u %d %d",
			    &lkb->lock.id,
			    &lkb->lock.nodeid,
			    &lkb->lock.remid,
			    &lkb->lock.ownpid,
			    &xid,
			    &lkb->lock.exflags,
			    &lkb->lock.flags,
			    &lkb->lock.status,
			    &lkb->lock.grmode,
			    &lkb->lock.rqmode,
			    &lkb->time,
			    &r_nodeid,
			    &r_len);

		lkb->lock.xid = xid; /* hack to avoid warning */

		if (rv != 13) {
			log_error("invalid debugfs line %d: %s", rv, line);
			free(lkb);
			goto out;
		}

		memset(r_name, 0, sizeof(r_name));
		parse_r_name(line, r_name);

		/* only collecting master lock state */
		if (r_nodeid)
			continue;

		r = get_resource(ls, r_name, r_len);
		if (!r)
			break;
		add_lkb(r, lkb);
	}
 out:
	fclose(file);
	return 0;
}

static int pack_lkb_list(struct list_head *q, struct pack_lock **lockp)
{
	struct dlm_lkb *lkb;
	struct pack_lock *lock = *lockp;
	int count = 0;

	list_for_each_entry(lkb, q, list) {
		if (count + 1 > section_max) {
			log_error("too many locks %d for ckpt buf", count);
			break;
		}

		lock->xid     = cpu_to_le64(lkb->lock.xid);
		lock->id      = cpu_to_le32(lkb->lock.id);
		lock->nodeid  = cpu_to_le32(lkb->lock.nodeid);
		lock->remid   = cpu_to_le32(lkb->lock.remid);
		lock->ownpid  = cpu_to_le32(lkb->lock.ownpid);
		lock->exflags = cpu_to_le32(lkb->lock.exflags);
		lock->flags   = cpu_to_le32(lkb->lock.flags);
		lock->status  = lkb->lock.status;
		lock->grmode  = lkb->lock.grmode;
		lock->rqmode  = lkb->lock.rqmode;
		lock->pad     = lkb->lock.pad;

		lock++;
		count++;
	}
	return count;
}

static void pack_section_buf(struct lockspace *ls, struct dlm_rsb *r)
{
	struct pack_lock *lock;
	int count;

	memset(&section_buf, 0, sizeof(section_buf));
	section_max = sizeof(section_buf) / sizeof(struct pack_lock);

	lock = (struct pack_lock *) &section_buf;

	count = pack_lkb_list(&r->locks, &lock);

	section_len = count * sizeof(struct pack_lock);
}

static int unpack_section_buf(struct lockspace *ls, int nodeid,
			      char *numbuf, int buflen)
{
	struct dlm_rsb *r;
	struct dlm_lkb *lkb;
	struct pack_lock *lock;
	int count = section_len / sizeof(struct pack_lock);
	int i;

	r = get_resource(ls, numbuf, buflen);
	if (!r)
		return -1;

	lock = (struct pack_lock *) &section_buf;

	for (i = 0; i < count; i++) {
		lkb = create_lkb();
		if (!lkb)
			break;

		lkb->lock.xid     = le64_to_cpu(lock->xid);
		lkb->lock.id      = le32_to_cpu(lock->id);
		lkb->lock.nodeid  = le32_to_cpu(lock->nodeid);
		lkb->lock.remid   = le32_to_cpu(lock->remid);
		lkb->lock.ownpid  = le32_to_cpu(lock->ownpid);
		lkb->lock.exflags = le32_to_cpu(lock->exflags);
		lkb->lock.flags   = le32_to_cpu(lock->flags);
		lkb->lock.status  = lock->status;
		lkb->lock.grmode  = lock->grmode;
		lkb->lock.rqmode  = lock->rqmode;
		lkb->lock.pad     = lock->pad;

		lkb->from = nodeid;
		add_lkb(r, lkb);
		lock++;
	}
	return 0;
}

static int _unlink_checkpoint(struct lockspace *ls, SaNameT *name)
{
	SaCkptCheckpointHandleT h;
	SaCkptCheckpointDescriptorT s;
	SaAisErrorT rv;
	int ret = 0;
	int retries;

	h = (SaCkptCheckpointHandleT) ls->lock_ckpt_handle;
	log_group(ls, "unlink ckpt %llx", (unsigned long long)h);

	retries = 0;
 unlink_retry:
	rv = saCkptCheckpointUnlink(global_ckpt_h, name);
	if (rv == SA_AIS_ERR_TRY_AGAIN) {
		log_group(ls, "unlink ckpt retry");
		sleep(1);
		if (retries++ < 10)
			goto unlink_retry;
	}
	if (rv == SA_AIS_OK)
		goto out_close;
	if (!h)
		goto out;

	log_error("unlink ckpt error %d %s", rv, ls->name);
	ret = -1;

	retries = 0;
 status_retry:
	rv = saCkptCheckpointStatusGet(h, &s);
	if (rv == SA_AIS_ERR_TRY_AGAIN) {
		log_group(ls, "unlink ckpt status retry");
		sleep(1);
		if (retries++ < 10)
			goto status_retry;
	}
	if (rv != SA_AIS_OK) {
		log_error("unlink ckpt status error %d %s", rv, ls->name);
		goto out_close;
	}

	log_group(ls, "unlink ckpt status: size %llu, max sections %u, "
		      "max section size %llu, section count %u, mem %u",
		 (unsigned long long)s.checkpointCreationAttributes.checkpointSize,
		 s.checkpointCreationAttributes.maxSections,
		 (unsigned long long)s.checkpointCreationAttributes.maxSectionSize,
		 s.numberOfSections, s.memoryUsed);

 out_close:
	retries = 0;
 close_retry:
	rv = saCkptCheckpointClose(h);
	if (rv == SA_AIS_ERR_TRY_AGAIN) {
		log_group(ls, "unlink ckpt close retry");
		sleep(1);
		if (retries++ < 10)
			goto close_retry;
	}
	if (rv != SA_AIS_OK) {
		log_error("unlink ckpt %llx close err %d %s",
			  (unsigned long long)h, rv, ls->name);
	}
 out:
	ls->lock_ckpt_handle = 0;
	return ret;
}

static int unlink_checkpoint(struct lockspace *ls)
{
	SaNameT name;
	int len;

	len = snprintf((char *)name.value, SA_MAX_NAME_LENGTH, "dlmdeadlk.%s.%d",
		       ls->name, our_nodeid);
	name.length = len;

	return _unlink_checkpoint(ls, &name);
}

static void read_checkpoint(struct lockspace *ls, int nodeid)
{
	SaCkptCheckpointHandleT h;
	SaCkptSectionIterationHandleT itr;
	SaCkptSectionDescriptorT desc;
	SaCkptIOVectorElementT iov;
	SaNameT name;
	SaAisErrorT rv;
	char buf[DLM_RESNAME_MAXLEN];
	int len;
	int retries;

	log_group(ls, "read_checkpoint %d", nodeid);

	if (nodeid == our_nodeid)
		return;

	len = snprintf((char *)name.value, SA_MAX_NAME_LENGTH, "dlmdeadlk.%s.%d",
		       ls->name, nodeid);
	name.length = len;

	retries = 0;
 open_retry:
	rv = saCkptCheckpointOpen(global_ckpt_h, &name, NULL,
				  SA_CKPT_CHECKPOINT_READ, 0, &h);
	if (rv == SA_AIS_ERR_TRY_AGAIN) {
		log_group(ls, "read_checkpoint: %d ckpt open retry", nodeid);
		sleep(1);
		if (retries++ < 10)
			goto open_retry;
	}
	if (rv != SA_AIS_OK) {
		log_error("read_checkpoint: %d ckpt open error %d", nodeid, rv);
		return;
	}

	retries = 0;
 init_retry:
	rv = saCkptSectionIterationInitialize(h, SA_CKPT_SECTIONS_ANY, 0, &itr);
	if (rv == SA_AIS_ERR_TRY_AGAIN) {
		log_group(ls, "read_checkpoint: ckpt iterinit retry");
		sleep(1);
		if (retries++ < 10)
			goto init_retry;
	}
	if (rv != SA_AIS_OK) {
		log_error("read_checkpoint: %d ckpt iterinit error %d", nodeid, rv);
		goto out;
	}

	while (1) {
		retries = 0;
	 next_retry:
		rv = saCkptSectionIterationNext(itr, &desc);
		if (rv == SA_AIS_ERR_NO_SECTIONS)
			break;
		if (rv == SA_AIS_ERR_TRY_AGAIN) {
			log_group(ls, "read_checkpoint: ckpt iternext retry");
			sleep(1);
			if (retries++ < 10)
				goto next_retry;
		}
		if (rv != SA_AIS_OK) {
			log_error("read_checkpoint: %d ckpt iternext error %d",
				  nodeid, rv);
			goto out_it;
		}

		if (!desc.sectionSize)
			continue;

		iov.sectionId = desc.sectionId;
		iov.dataBuffer = &section_buf;
		iov.dataSize = desc.sectionSize;
		iov.dataOffset = 0;

		memset(&buf, 0, sizeof(buf));
		snprintf(buf, sizeof(buf), "%s", desc.sectionId.id);

		log_group(ls, "read_checkpoint: section size %llu id %u \"%s\"",
			  (unsigned long long)iov.dataSize,
			  iov.sectionId.idLen, buf);

		retries = 0;
	 read_retry:
		rv = saCkptCheckpointRead(h, &iov, 1, NULL);
		if (rv == SA_AIS_ERR_TRY_AGAIN) {
			log_group(ls, "read_checkpoint: ckpt read retry");
			sleep(1);
			if (retries++ < 10)
				goto read_retry;
		}
		if (rv != SA_AIS_OK) {
			log_error("read_checkpoint: %d ckpt read error %d",
				  nodeid, rv);
			goto out_it;
		}

		log_group(ls, "read_checkpoint: ckpt read %llu bytes",
			  (unsigned long long)iov.readSize);
		section_len = iov.readSize;

		if (!section_len)
		       continue;

		if (section_len % sizeof(struct pack_lock)) {
			log_error("read_checkpoint: %d bad section len %d",
				  nodeid, section_len);
			continue;
		}

		unpack_section_buf(ls, nodeid, (char *)desc.sectionId.id,
				   desc.sectionId.idLen);
	}

 out_it:
	saCkptSectionIterationFinalize(itr);
	retries = 0;
 out:
	rv = saCkptCheckpointClose(h);
	if (rv == SA_AIS_ERR_TRY_AGAIN) {
		log_group(ls, "read_checkpoint: unlink ckpt close retry");
		sleep(1);
		if (retries++ < 10)
			goto out;
	}
	if (rv != SA_AIS_OK)
		log_error("read_checkpoint: %d close error %d", nodeid, rv);
}

static void write_checkpoint(struct lockspace *ls)
{
	SaCkptCheckpointCreationAttributesT attr;
	SaCkptCheckpointHandleT h;
	SaCkptSectionIdT section_id;
	SaCkptSectionCreationAttributesT section_attr;
	SaCkptCheckpointOpenFlagsT flags;
	SaNameT name;
	SaAisErrorT rv;
	char buf[DLM_RESNAME_MAXLEN];
	struct dlm_rsb *r;
	struct dlm_lkb *lkb;
	int r_count, lock_count, total_size, section_size, max_section_size;
	int len;

	len = snprintf((char *)name.value, SA_MAX_NAME_LENGTH, "dlmdeadlk.%s.%d",
		      ls->name, our_nodeid);
	name.length = len;

	/* unlink an old checkpoint before we create a new one */
	if (ls->lock_ckpt_handle) {
		if (_unlink_checkpoint(ls, &name))
			return;
	}

	/* loop through all locks to figure out sizes to set in
	   the attr fields */

	r_count = 0;
	lock_count = 0;
	total_size = 0;
	max_section_size = 0;

	list_for_each_entry(r, &ls->resources, list) {
		r_count++;
		section_size = 0;
		list_for_each_entry(lkb, &r->locks, list) {
			section_size += sizeof(struct pack_lock);
			lock_count++;
		}
		total_size += section_size;
		if (section_size > max_section_size)
			max_section_size = section_size;
	}

	log_group(ls, "write_checkpoint: r_count %d, lock_count %d",
		  r_count, lock_count);

	log_group(ls, "write_checkpoint: total %d bytes, max_section %d bytes",
		  total_size, max_section_size);

	attr.creationFlags = SA_CKPT_WR_ALL_REPLICAS;
	attr.checkpointSize = total_size;
	attr.retentionDuration = SA_TIME_MAX;
	attr.maxSections = r_count + 1;      /* don't know why we need +1 */
	attr.maxSectionSize = max_section_size;
	attr.maxSectionIdSize = DLM_RESNAME_MAXLEN;

	flags = SA_CKPT_CHECKPOINT_READ |
		SA_CKPT_CHECKPOINT_WRITE |
		SA_CKPT_CHECKPOINT_CREATE;

 open_retry:
	rv = saCkptCheckpointOpen(global_ckpt_h, &name, &attr, flags, 0, &h);
	if (rv == SA_AIS_ERR_TRY_AGAIN) {
		log_group(ls, "write_checkpoint: ckpt open retry");
		sleep(1);
		goto open_retry;
	}
	if (rv == SA_AIS_ERR_EXIST) {
		log_group(ls, "write_checkpoint: ckpt already exists");
		return;
	}
	if (rv != SA_AIS_OK) {
		log_group(ls, "write_checkpoint: ckpt open error %d", rv);
		return;
	}

	log_group(ls, "write_checkpoint: open ckpt handle %llx", (long long)h);
	ls->lock_ckpt_handle = (uint64_t) h;

	list_for_each_entry(r, &ls->resources, list) {
		memset(buf, 0, sizeof(buf));
		len = snprintf(buf, sizeof(buf), "%s", r->name);

		section_id.id = (void *)buf;
		section_id.idLen = len + 1;
		section_attr.sectionId = &section_id;
		section_attr.expirationTime = SA_TIME_END;

		pack_section_buf(ls, r);

		log_group(ls, "write_checkpoint: section size %u id %u \"%s\"",
			  section_len, section_id.idLen, buf);

	 create_retry:
		rv = saCkptSectionCreate(h, &section_attr, &section_buf,
					 section_len);
		if (rv == SA_AIS_ERR_TRY_AGAIN) {
			log_group(ls, "write_checkpoint: ckpt create retry");
			sleep(1);
			goto create_retry;
		}
		if (rv == SA_AIS_ERR_EXIST) {
			/* this shouldn't happen in general */
			log_error("write_checkpoint: clearing old ckpt");
			saCkptCheckpointClose(h);
			_unlink_checkpoint(ls, &name);
			goto open_retry;
		}
		if (rv != SA_AIS_OK) {
			log_error("write_checkpoint: section create %d", rv);
			break;
		}
	}
}

static int _send_message(cpg_handle_t h, void *buf, int len, int type)
{
	struct iovec iov;
	cpg_error_t error;
	int retries = 0;

	iov.iov_base = buf;
	iov.iov_len = len;
 retry:
	error = cpg_mcast_joined(h, CPG_TYPE_AGREED, &iov, 1);
	if (error == CPG_ERR_TRY_AGAIN) {
		retries++;
		usleep(1000);
		if (!(retries % 100))
			log_error("cpg_mcast_joined retry %d", retries);
		if (retries < 1000)
			goto retry;
	}
	if (error != CPG_OK) {
		log_error("cpg_mcast_joined error %d handle %llx",
			  (int)error, (long long)h);
		disable_deadlock();
		return -1;
	}
	return 0;
}

static void send_message(struct lockspace *ls, int type)
{
	struct dlm_header *hd;
	int len;
	char *buf;

	len = sizeof(struct dlm_header);
	buf = malloc(len);
	if (!buf) {
		log_error("send_message: no memory");
		disable_deadlock();
		return;
	}
	memset(buf, 0, len);

	hd = (struct dlm_header *)buf;
	hd->version[0]  = cpu_to_le16(DLM_HEADER_MAJOR);
	hd->version[1]  = cpu_to_le16(DLM_HEADER_MINOR);
	hd->version[2]  = cpu_to_le16(DLM_HEADER_PATCH);
	hd->type	= cpu_to_le16(type);
	hd->nodeid      = cpu_to_le32(our_nodeid);
	hd->to_nodeid   = 0;
	hd->global_id   = cpu_to_le32(ls->global_id);
	memcpy(hd->name, ls->name, strlen(ls->name));

	_send_message(ls->cpg_h, buf, len, type);

	free(buf);
}

static void send_checkpoint_ready(struct lockspace *ls)
{
	log_group(ls, "send_checkpoint_ready");
	send_message(ls, DLM_MSG_CHECKPOINT_READY);
}

void send_cycle_start(struct lockspace *ls)
{
	if (!deadlock_enabled)
		return;
	log_group(ls, "send_cycle_start");
	send_message(ls, DLM_MSG_CYCLE_START);
}

static void send_cancel_lock(struct lockspace *ls, struct trans *tr,
			     struct dlm_lkb *lkb)
{
	struct dlm_header *hd;
	int len;
	char *buf;
	int to_nodeid;
	uint32_t lkid;

	len = sizeof(struct dlm_header);
	buf = malloc(len);
	if (!buf) {
		log_error("send_message: no memory");
		disable_deadlock();
		return;
	}
	memset(buf, 0, len);

	if (lkb->lock.nodeid) {
		/* this was MSTCPY lkb from master node */
		to_nodeid = lkb->lock.nodeid;
		lkid = lkb->lock.remid;
	} else {
		/* process-copy lkb from node where lock is held */
		to_nodeid = lkb->from;
		lkid = lkb->lock.id;
	}

	log_group(ls, "send_cancel_lock to %x lkid %d, id %x remid %x "
		  "nodeid %d from %d", to_nodeid, lkid,
		  lkb->lock.id, lkb->lock.remid, lkb->lock.nodeid, lkb->from);

	hd = (struct dlm_header *)buf;
	hd->version[0]  = cpu_to_le16(DLM_HEADER_MAJOR);
	hd->version[1]  = cpu_to_le16(DLM_HEADER_MINOR);
	hd->version[2]  = cpu_to_le16(DLM_HEADER_PATCH);
	hd->type	= cpu_to_le16(DLM_MSG_CANCEL_LOCK);
	hd->nodeid      = cpu_to_le32(our_nodeid);
	hd->to_nodeid   = cpu_to_le32(to_nodeid);
	hd->lkid        = cpu_to_le32(lkid);
	hd->global_id   = cpu_to_le32(ls->global_id);
	memcpy(hd->name, ls->name, strlen(ls->name));

	_send_message(ls->cpg_h, buf, len, DLM_MSG_CANCEL_LOCK);

	free(buf);
}

static void find_deadlock(struct lockspace *ls);

static void receive_checkpoint_ready(struct lockspace *ls, int nodeid)
{
	struct node *node;
	int not_ready = 0;

	log_group(ls, "receive_checkpoint_ready %d", nodeid);

	read_checkpoint(ls, nodeid);

	/* when locks are read from all nodes, then search_deadlock()
	   to do detection */

	list_for_each_entry(node, &ls->nodes, list) {
		if (node->nodeid == nodeid)
			node->checkpoint_ready = 1;
		if (!node->checkpoint_ready)
			not_ready++;
	}

	if (not_ready) {
		log_group(ls, "not_ready %d", not_ready);
		return;
	}

	find_deadlock(ls);
}

static void receive_cycle_start(struct lockspace *ls, int nodeid)
{
	int rv;

	log_group(ls, "receive_cycle_start %d", nodeid);

	gettimeofday(&ls->last_deadlock_check, NULL);

	rv = read_debugfs_locks(ls);
	if (rv < 0) {
		/* compat for RHEL5.1 kernels */
		rv = read_debugfs_master(ls);
		if (rv < 0) {
			log_error("can't read dlm debugfs file: %s",
				  strerror(errno));
			return;
		}
	}

	write_checkpoint(ls);
	send_checkpoint_ready(ls);
}

static void receive_cancel_lock(struct lockspace *ls, int nodeid, uint32_t lkid)
{
	dlm_lshandle_t h;
	int rv;

	if (nodeid != our_nodeid)
		return;

	h = dlm_open_lockspace(ls->name);
	if (!h) {
		log_error("deadlock cancel %x from %d can't open lockspace %s",
			  lkid, nodeid, ls->name);
		return;
	}

	log_group(ls, "receive_cancel_lock %x from %d", lkid, nodeid);

	rv = dlm_ls_deadlock_cancel(h, lkid, 0);
	if (rv < 0) {
		log_error("deadlock cancel %x from %x lib cancel error %d",
			  lkid, nodeid, rv);
	}

	dlm_close_lockspace(h);
}

static void deliver_cb(cpg_handle_t handle, struct cpg_name *group_name,
		uint32_t nodeid, uint32_t pid, void *data, int data_len)
{
	struct lockspace *ls;
	struct dlm_header *hd;

	ls = find_ls_by_handle(handle);
	if (!ls)
		return;

	hd = (struct dlm_header *) data;

	hd->version[0]  = le16_to_cpu(hd->version[0]);
	hd->version[1]  = le16_to_cpu(hd->version[1]);
	hd->version[2]  = le16_to_cpu(hd->version[2]);
	hd->type	= le16_to_cpu(hd->type);
	hd->nodeid      = le32_to_cpu(hd->nodeid);
	hd->to_nodeid   = le32_to_cpu(hd->to_nodeid);
	hd->global_id   = le32_to_cpu(hd->global_id);

	if (hd->version[0] != DLM_HEADER_MAJOR) {
		log_error("reject message version %u.%u.%u",
			  hd->version[0], hd->version[1], hd->version[2]);
		return;
	}

	switch (hd->type) {
	case DLM_MSG_CYCLE_START:
		receive_cycle_start(ls, hd->nodeid);
		break;
	case DLM_MSG_CHECKPOINT_READY:
		receive_checkpoint_ready(ls, hd->nodeid);
		break;
	case DLM_MSG_CANCEL_LOCK:
		receive_cancel_lock(ls, hd->nodeid, hd->lkid);
		break;
	default:
		log_error("unknown message type %d from %d",
			  hd->type, hd->nodeid);
	}
}

static void node_joined(struct lockspace *ls, int nodeid)
{
	struct node *node;

	node = malloc(sizeof(struct node));
	if (!node) {
		log_error("node_joined: no memory");
		disable_deadlock();
		return;
	}
	memset(node, 0, sizeof(struct node));
	node->nodeid = nodeid;
	list_add_tail(&node->list, &ls->nodes);
	log_group(ls, "node %d joined deadlock cpg", nodeid);
}

static void node_left(struct lockspace *ls, int nodeid, int reason)
{
	struct node *node, *safe;

	list_for_each_entry_safe(node, safe, &ls->nodes, list) {
		if (node->nodeid != nodeid)
			continue;

		/* TODO: purge locks from this node if we're in a cycle */

		list_del(&node->list);
		free(node);
		log_group(ls, "node %d left deadlock cpg", nodeid);
	}
}

static void confchg_cb(cpg_handle_t handle, struct cpg_name *group_name,
		struct cpg_address *member_list, int member_list_entries,
		struct cpg_address *left_list, int left_list_entries,
		struct cpg_address *joined_list, int joined_list_entries)
{
	struct lockspace *ls;
	int i;

	ls = find_ls_by_handle(handle);
	if (!ls)
		return;

	for (i = 0; i < joined_list_entries; i++)
		node_joined(ls, joined_list[i].nodeid);

	for (i = 0; i < left_list_entries; i++)
		node_left(ls, left_list[i].nodeid, left_list[i].reason);
}

static void process_deadlock_cpg(int ci)
{
	struct lockspace *ls;
	cpg_error_t error;

	ls = get_client_lockspace(ci);
	if (!ls)
		return;

	error = cpg_dispatch(ls->cpg_h, CPG_DISPATCH_ONE);
	if (error != CPG_OK)
		log_error("cpg_dispatch error %d", error);
}

cpg_callbacks_t ls_callbacks = {
	.cpg_deliver_fn = deliver_cb,
	.cpg_confchg_fn = confchg_cb,
};

static void make_cpgname(struct lockspace *ls, struct cpg_name *cn)
{
	char name[MAXNAME+8];

	memset(name, 0, sizeof(name));
	strncpy(name, ls->name, sizeof(name));
	strncat(name, "_deadlk", 7);
	memset(cn, 0, sizeof(struct cpg_name));
	strncpy(cn->value, name, strlen(name) + 1);
	cn->length = strlen(name) + 1;
}

void join_deadlock_cpg(struct lockspace *ls)
{
	cpg_handle_t h;
	struct cpg_name cpgname;
	cpg_error_t error;
	int retries = 0;
	int fd, ci;

	if (!deadlock_enabled)
		return;

	unlink_checkpoint(ls); /* not sure about this */

	error = cpg_initialize(&h, &ls_callbacks);
	if (error != CPG_OK) {
		log_error("cpg_initialize error %d", error);
		return;
	}

	cpg_fd_get(h, &fd);
	if (fd < 0) {
		log_error("cpg_fd_get error %d", error);
		return;
	}

	ci = client_add(fd, process_deadlock_cpg, NULL);

	make_cpgname(ls, &cpgname);

 retry:
	error = cpg_join(h, &cpgname);
	if (error == CPG_ERR_TRY_AGAIN) {
		sleep(1);
		if (retries++ < 10)
			goto retry;
	}
	if (error != CPG_OK) {
		log_error("deadlk cpg join error %d", error);
		goto fail;
	}

	ls->cpg_h = h;
	ls->cpg_ci = ci;
	set_client_lockspace(ci, ls);
	log_group(ls, "deadlk cpg ci %d fd %d", ci, fd);
	return;
 fail:
	cpg_finalize(h);
	client_dead(ci);
}

void leave_deadlock_cpg(struct lockspace *ls)
{
	struct cpg_name cpgname;
	cpg_error_t error;
	int retries = 0;

	if (!deadlock_enabled)
		return;

	make_cpgname(ls, &cpgname);
 retry:
	error = cpg_leave(ls->cpg_h, &cpgname);
	if (error == CPG_ERR_TRY_AGAIN) {
		sleep(1);
		if (retries++ < 10)
			goto retry;
	}
	if (error != CPG_OK)
		log_error("deadlk cpg leave error %d", error);

	cpg_finalize(ls->cpg_h);
	client_dead(ls->cpg_ci);
}

static void add_lkb_trans(struct trans *tr, struct dlm_lkb *lkb)
{
	list_add(&lkb->trans_list, &tr->locks);
	lkb->trans = tr;
}

static struct trans *get_trans(struct lockspace *ls, uint64_t xid)
{
	struct trans *tr;

	list_for_each_entry(tr, &ls->transactions, list) {
		if (tr->xid == xid)
			return tr;
	}

	tr = malloc(sizeof(struct trans));
	if (!tr) {
		log_error("get_trans: no memory");
		disable_deadlock();
		return NULL;
	}
	memset(tr, 0, sizeof(struct trans));
	tr->xid = xid;
	tr->waitfor = NULL;
	tr->waitfor_alloc = 0;
	tr->waitfor_count = 0;
	INIT_LIST_HEAD(&tr->locks);
	list_add(&tr->list, &ls->transactions);
	return tr;
}

/* for each rsb, for each lock, find/create trans, add lkb to the trans list */

static void create_trans_list(struct lockspace *ls)
{
	struct dlm_rsb *r;
	struct dlm_lkb *lkb;
	struct trans *tr;
	int r_count = 0, lkb_count = 0;

	list_for_each_entry(r, &ls->resources, list) {
		r_count++;
		list_for_each_entry(lkb, &r->locks, list) {
			lkb_count++;
			tr = get_trans(ls, lkb->lock.xid);
			if (!tr)
				goto out;
			add_lkb_trans(tr, lkb);
		}
	}
 out:
	log_group(ls, "create_trans_list: r_count %d lkb_count %d",
		  r_count, lkb_count);
}

static int locks_compat(struct dlm_lkb *waiting_lkb,
			struct dlm_lkb *granted_lkb)
{
	if (waiting_lkb == granted_lkb) {
		log_debug("waiting and granted same lock");
		return 0;
	}

	if (waiting_lkb->trans->xid == granted_lkb->trans->xid) {
		log_debug("waiting and granted same trans %llx",
			  (long long)waiting_lkb->trans->xid);
		return 0;
	}

	return dlm_modes_compat(granted_lkb->lock.grmode,
				waiting_lkb->lock.rqmode);
}

/* TODO: don't add new waitfor trans if we're already waiting for the same
   trans for another lock */

static void add_waitfor(struct dlm_lkb *waiting_lkb,
			struct dlm_lkb *granted_lkb)
{
	struct trans *tr;
	int old_alloc, i;

	if (locks_compat(waiting_lkb, granted_lkb))
		return;

	tr = waiting_lkb->trans;

	if (tr->waitfor_count == tr->waitfor_alloc) {
		old_alloc = tr->waitfor_alloc;
		tr->waitfor_alloc += TR_NALLOC;
		tr->waitfor = realloc(tr->waitfor,
				      tr->waitfor_alloc * sizeof(tr));
		for (i = old_alloc; i < tr->waitfor_alloc; i++)
			tr->waitfor[i] = NULL;
	}

	tr->waitfor[tr->waitfor_count++] = granted_lkb->trans;
	granted_lkb->trans->others_waiting_on_us++;
}

/* for each trans, for each waiting lock, go to rsb of the lock,
   find granted locks on that rsb, then find the trans the
   granted lock belongs to, add that trans to our waitfor list */

static void create_waitfor_graph(struct lockspace *ls)
{
	struct dlm_lkb *waiting_lkb, *granted_lkb;
	struct dlm_rsb *r;
	struct trans *tr;

	list_for_each_entry(tr, &ls->transactions, list) {
		list_for_each_entry(waiting_lkb, &tr->locks, trans_list) {
			if (waiting_lkb->lock.status == DLM_LKSTS_GRANTED)
				continue;
			/* waiting_lkb status is CONVERT or WAITING */

			r = waiting_lkb->rsb;

			list_for_each_entry(granted_lkb, &r->locks, list) {
				if (granted_lkb->lock.status==DLM_LKSTS_WAITING)
					continue;
				/* granted_lkb status is GRANTED or CONVERT */
				add_waitfor(waiting_lkb, granted_lkb);
			}
		}
	}
}

/* Assume a transaction that's not waiting on any locks will complete, release
   all the locks it currently holds, and exit.  Other transactions that were
   blocked waiting on the removed transaction's now-released locks may now be
   unblocked, complete, release all held locks and exit.  Repeat this until
   no more transactions can be removed.  If there are transactions remaining,
   then they are deadlocked. */

static void remove_waitfor(struct trans *tr, struct trans *remove_tr)
{
	int i;

	for (i = 0; i < tr->waitfor_alloc; i++) {
		if (!tr->waitfor_count)
			break;

		if (!tr->waitfor[i])
			continue;

		if (tr->waitfor[i] == remove_tr) {
			tr->waitfor[i] = NULL;
			tr->waitfor_count--;
			remove_tr->others_waiting_on_us--;
		}
	}
}

/* remove_tr is not waiting for anything, assume it completes and goes away
   and remove it from any other transaction's waitfor list */

static void remove_trans(struct lockspace *ls, struct trans *remove_tr)
{
	struct trans *tr;

	list_for_each_entry(tr, &ls->transactions, list) {
		if (tr == remove_tr)
			continue;
		if (!remove_tr->others_waiting_on_us)
			break;
		remove_waitfor(tr, remove_tr);
	}

	if (remove_tr->others_waiting_on_us)
		log_debug("trans %llx removed others waiting %d",
			  (unsigned long long)remove_tr->xid,
			  remove_tr->others_waiting_on_us);
}

static int reduce_waitfor_graph(struct lockspace *ls)
{
	struct trans *tr, *safe;
	int blocked = 0;
	int removed = 0;

	list_for_each_entry_safe(tr, safe, &ls->transactions, list) {
		if (tr->waitfor_count) {
			blocked++;
			continue;
		}
		remove_trans(ls, tr);
		list_del(&tr->list);
		if (tr->waitfor)
			free(tr->waitfor);
		free(tr);
		removed++;
	}

	if (removed)
		log_group(ls, "reduce_waitfor_graph: %d blocked, %d removed",
			  blocked, removed);
	return removed;
}

static void reduce_waitfor_graph_loop(struct lockspace *ls)
{
	int removed;

	while (1) {
		removed = reduce_waitfor_graph(ls);
		if (!removed)
			break;
	}
}

static struct trans *find_trans_to_cancel(struct lockspace *ls)
{
	struct trans *tr;

	list_for_each_entry(tr, &ls->transactions, list) {
		if (!tr->others_waiting_on_us)
			continue;
		return tr;
	}
	return NULL;
}

static void cancel_trans(struct lockspace *ls)
{
	struct trans *tr;
	struct dlm_lkb *lkb;
	int removed;

	tr = find_trans_to_cancel(ls);
	if (!tr) {
		log_group(ls, "cancel_trans: no trans found");
		return;
	}

	list_for_each_entry(lkb, &tr->locks, trans_list) {
		if (lkb->lock.status == DLM_LKSTS_GRANTED)
			continue;
		send_cancel_lock(ls, tr, lkb);
		tr->waitfor_count--;
	}

	if (tr->waitfor_count)
		log_group(ls, "canceled trans has non-zero waitfor_count %d",
			  tr->waitfor_count);

	/* this should now remove the canceled trans */
	removed = reduce_waitfor_graph(ls);

	if (!removed)
		log_group(ls, "canceled trans not removed from graph");

	/* now call reduce_waitfor_graph() in another loop and it
	   should completely reduce */
}

static char *status_str(int lksts)
{
	switch (lksts) {
	case DLM_LKSTS_WAITING:
		return "W";
	case DLM_LKSTS_GRANTED:
		return "G";
	case DLM_LKSTS_CONVERT:
		return "C";
	}
	return "?";
}

static void dump_trans(struct lockspace *ls, struct trans *tr)
{
	struct dlm_lkb *lkb;
	struct trans *wf;
	int i;

	log_group(ls, "trans %llx waitfor_count %d others_waiting_on_us %d",
		  (unsigned long long)tr->xid, tr->waitfor_count,
		  tr->others_waiting_on_us);

	log_group(ls, "locks:");
	
	list_for_each_entry(lkb, &tr->locks, trans_list) {
		log_group(ls, "  %s: id %08x gr %s rq %s pid %u \"%s\"",
			  status_str(lkb->lock.status),
			  lkb->lock.id,
			  dlm_mode_str(lkb->lock.grmode),
			  dlm_mode_str(lkb->lock.rqmode),
			  lkb->lock.ownpid,
			  lkb->rsb->name);
	}

	if (!tr->waitfor_count)
		return;

	log_group(ls, "waitfor:");

	for (i = 0; i < tr->waitfor_alloc; i++) {
		if (!tr->waitfor[i])
			continue;
		wf = tr->waitfor[i];
		log_group(ls, "  xid %llx", (unsigned long long)wf->xid);
	}
}

static void dump_all_trans(struct lockspace *ls)
{
	struct trans *tr;

	list_for_each_entry(tr, &ls->transactions, list)
		dump_trans(ls, tr);
}

static void find_deadlock(struct lockspace *ls)
{
	if (list_empty(&ls->resources)) {
		log_group(ls, "no resources no deadlock");
		return;
	}

	create_trans_list(ls);
	create_waitfor_graph(ls);

	log_group(ls, "created waitfor graph:");
	dump_all_trans(ls);

	reduce_waitfor_graph_loop(ls);

	if (list_empty(&ls->transactions)) {
		log_group(ls, "no deadlock");
		goto out;
	}

	log_group(ls, "found deadlock");
	dump_all_trans(ls);

	cancel_trans(ls);

	reduce_waitfor_graph_loop(ls);

	if (list_empty(&ls->transactions)) {
		log_group(ls, "deadlock resolved with cancel");
		goto out;
	}

	log_error("deadlock resolution failed");
	dump_all_trans(ls);

 out:
	free_resources(ls);
}
