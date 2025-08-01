/*-------------------------------------------------------------------------
 *
 * ipci.c
 *	  POSTGRES inter-process communication initialization code.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/ipci.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/clog.h"
#include "access/commit_ts.h"
#include "access/multixact.h"
#include "access/nbtree.h"
#include "access/subtrans.h"
#include "access/syncscan.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xlogprefetcher.h"
#include "access/xlogrecovery.h"
#include "commands/async.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "postmaster/bgworker_internals.h"
#include "postmaster/bgwriter.h"
#include "postmaster/postmaster.h"
#include "postmaster/walsummarizer.h"
#include "replication/logicallauncher.h"
#include "replication/origin.h"
#include "replication/slot.h"
#include "replication/slotsync.h"
#include "replication/walreceiver.h"
#include "replication/walsender.h"
#include "storage/bufmgr.h"
#include "storage/dsm.h"
#include "storage/dsm_registry.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"
#include "storage/pmsignal.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "storage/sinvaladt.h"
#include "storage/spin.h"
#include "utils/guc.h"
#include "utils/injection_point.h"

/* GUCs */
int			shared_memory_type = DEFAULT_SHARED_MEMORY_TYPE;

shmem_startup_hook_type shmem_startup_hook = NULL;

static Size total_addin_request = 0;

static void CreateOrAttachShmemStructs(void);

/*
 * RequestAddinShmemSpace
 *		Request that extra shmem space be allocated for use by
 *		a loadable module.
 *
 * This may only be called via the shmem_request_hook of a library that is
 * loaded into the postmaster via shared_preload_libraries.  Calls from
 * elsewhere will fail.
 */
void
RequestAddinShmemSpace(Size size)
{
	if (!process_shmem_requests_in_progress)
		elog(FATAL, "cannot request additional shared memory outside shmem_request_hook");
	total_addin_request = add_size(total_addin_request, size);
}

/*
 * CalculateShmemSize
 *		Calculates the amount of shared memory and number of semaphores needed.
 *
 * If num_semaphores is not NULL, it will be set to the number of semaphores
 * required.
 */
Size
CalculateShmemSize(int *num_semaphores) /// 
{
	Size		size;
	int			numSemas;

	/* Compute number of semaphores we'll need */
	numSemas = ProcGlobalSemas();
	numSemas += SpinlockSemas();

	/* Return the number of semaphores if requested by the caller */
	if (num_semaphores)
		*num_semaphores = numSemas;

	/*
	 * Size of the Postgres shared-memory block is estimated via moderately-
	 * accurate estimates for the big hogs, plus 100K for the stuff that's too
	 * small to bother with estimating.
	 *
	 * We take some care to ensure that the total size request doesn't
	 * overflow size_t.  If this gets through, we don't need to be so careful
	 * during the actual allocation phase.
	 */
	size = 100000;
	size = add_size(size, PGSemaphoreShmemSize(numSemas));
	size = add_size(size, SpinlockSemaSize());
	size = add_size(size, hash_estimate_size(SHMEM_INDEX_SIZE,
											 sizeof(ShmemIndexEnt)));
	size = add_size(size, dsm_estimate_size());
	size = add_size(size, DSMRegistryShmemSize());
	size = add_size(size, BufferShmemSize());
	size = add_size(size, LockShmemSize());
	size = add_size(size, PredicateLockShmemSize());
	size = add_size(size, ProcGlobalShmemSize());
	size = add_size(size, XLogPrefetchShmemSize());
	size = add_size(size, VarsupShmemSize());
	size = add_size(size, XLOGShmemSize());
	size = add_size(size, XLogRecoveryShmemSize());
	size = add_size(size, CLOGShmemSize());
	size = add_size(size, CommitTsShmemSize());
	size = add_size(size, SUBTRANSShmemSize());
	size = add_size(size, TwoPhaseShmemSize());
	size = add_size(size, BackgroundWorkerShmemSize());
	size = add_size(size, MultiXactShmemSize());
	size = add_size(size, LWLockShmemSize());
	size = add_size(size, ProcArrayShmemSize());
	size = add_size(size, BackendStatusShmemSize());
	size = add_size(size, SInvalShmemSize());
	size = add_size(size, PMSignalShmemSize());
	size = add_size(size, ProcSignalShmemSize());
	size = add_size(size, CheckpointerShmemSize());
	size = add_size(size, AutoVacuumShmemSize());
	size = add_size(size, ReplicationSlotsShmemSize());
	size = add_size(size, ReplicationOriginShmemSize());
	size = add_size(size, WalSndShmemSize());
	size = add_size(size, WalRcvShmemSize());
	size = add_size(size, WalSummarizerShmemSize());
	size = add_size(size, PgArchShmemSize());
	size = add_size(size, ApplyLauncherShmemSize());
	size = add_size(size, BTreeShmemSize());
	size = add_size(size, SyncScanShmemSize());
	size = add_size(size, AsyncShmemSize());
	size = add_size(size, StatsShmemSize());
	size = add_size(size, WaitEventCustomShmemSize());
	size = add_size(size, InjectionPointShmemSize());
	size = add_size(size, SlotSyncShmemSize());
#ifdef EXEC_BACKEND
	size = add_size(size, ShmemBackendArraySize());
#endif

	/* include additional requested shmem from preload libraries */
	size = add_size(size, total_addin_request);

	/* might as well round it off to a multiple of a typical page size */
	size = add_size(size, 8192 - (size % 8192));

	return size;
}

#ifdef EXEC_BACKEND
/*
 * AttachSharedMemoryStructs
 *		Initialize a postmaster child process's access to shared memory
 *      structures.
 *
 * In !EXEC_BACKEND mode, we inherit everything through the fork, and this
 * isn't needed.
 */
void
AttachSharedMemoryStructs(void)
{
	/* InitProcess must've been called already */
	Assert(MyProc != NULL);
	Assert(IsUnderPostmaster);

	CreateOrAttachShmemStructs();

	/*
	 * Now give loadable modules a chance to set up their shmem allocations
	 */
	if (shmem_startup_hook)
		shmem_startup_hook();
}
#endif

/*
 * CreateSharedMemoryAndSemaphores
 *		Creates and initializes shared memory and semaphores.
 */
void
CreateSharedMemoryAndSemaphores(void)
{
	PGShmemHeader *shim;
	PGShmemHeader *seghdr;
	Size		size;  /// typedef size_t Size; 8个字节
	int			numSemas;

	Assert(!IsUnderPostmaster);

	/* Compute the size of the shared-memory block */
	size = CalculateShmemSize(&numSemas);
	elog(DEBUG3, "invoking IpcMemoryCreate(size=%zu)", size);

	/*
	 * Create the shmem segment
	 */
	seghdr = PGSharedMemoryCreate(size, &shim); /// 这个创建共享内存的函数仅仅是主进程启动时执行一次。
	/// 通常情况下，seghdr指向了mmap()分配的共享内存，开始的56个字节是PGShmemHeader结构。

	/*
	 * Make sure that huge pages are never reported as "unknown" while the
	 * server is running.
	 */
	Assert(strcmp("unknown",
				  GetConfigOption("huge_pages_status", false, false)) != 0);

	InitShmemAccess(seghdr); /// 就是设置三个全局的指针，两个指向共享内存的头部，一个指向共享内存的尾部。

	/*
	 * Create semaphores
	 */
	PGReserveSemaphores(numSemas);

	/*
	 * If spinlocks are disabled, initialize emulation layer (which depends on
	 * semaphores, so the order is important here).
	 */
#ifndef HAVE_SPINLOCKS
	SpinlockSemaInit();
#endif

	/*
	 * Set up shared memory allocation mechanism
	 */
	InitShmemAllocation();

	/* Initialize subsystems */
	CreateOrAttachShmemStructs();

#ifdef EXEC_BACKEND

	/*
	 * Alloc the win32 shared backend array
	 */
	ShmemBackendArrayAllocation();
#endif

	/* Initialize dynamic shared memory facilities. */
	dsm_postmaster_startup(shim);

	/*
	 * Now give loadable modules a chance to set up their shmem allocations
	 */
	if (shmem_startup_hook)
		shmem_startup_hook();
}

/*
 * Initialize various subsystems, setting up their data structures in
 * shared memory.
 *
 * This is called by the postmaster or by a standalone backend.
 * It is also called by a backend forked from the postmaster in the
 * EXEC_BACKEND case.  In the latter case, the shared memory segment
 * already exists and has been physically attached to, but we have to
 * initialize pointers in local memory that reference the shared structures,
 * because we didn't inherit the correct pointer values from the postmaster
 * as we do in the fork() scenario.  The easiest way to do that is to run
 * through the same code as before.  (Note that the called routines mostly
 * check IsUnderPostmaster, rather than EXEC_BACKEND, to detect this case.
 * This is a bit code-wasteful and could be cleaned up.)
 */
static void
CreateOrAttachShmemStructs(void)
{
	/*
	 * Now initialize LWLocks, which do shared memory allocation and are
	 * needed for InitShmemIndex.
	 */
	CreateLWLocks();

	/*
	 * Set up shmem.c index hashtable
	 */
	InitShmemIndex();

	dsm_shmem_init();
	DSMRegistryShmemInit();

	/*
	 * Set up xlog, clog, and buffers
	 */
	VarsupShmemInit();
	XLOGShmemInit();
	XLogPrefetchShmemInit();
	XLogRecoveryShmemInit();
	CLOGShmemInit();
	CommitTsShmemInit();
	SUBTRANSShmemInit();
	MultiXactShmemInit();
	InitBufferPool();

	/*
	 * Set up lock manager
	 */
	InitLocks();

	/*
	 * Set up predicate lock manager
	 */
	InitPredicateLocks();

	/*
	 * Set up process table
	 */
	if (!IsUnderPostmaster)
		InitProcGlobal();
	CreateSharedProcArray();
	CreateSharedBackendStatus();
	TwoPhaseShmemInit();
	BackgroundWorkerShmemInit();

	/*
	 * Set up shared-inval messaging
	 */
	CreateSharedInvalidationState();

	/*
	 * Set up interprocess signaling mechanisms
	 */
	PMSignalShmemInit();
	ProcSignalShmemInit();
	CheckpointerShmemInit();
	AutoVacuumShmemInit();
	ReplicationSlotsShmemInit();
	ReplicationOriginShmemInit();
	WalSndShmemInit();
	WalRcvShmemInit();
	WalSummarizerShmemInit();
	PgArchShmemInit();
	ApplyLauncherShmemInit();
	SlotSyncShmemInit();

	/*
	 * Set up other modules that need some shared memory space
	 */
	BTreeShmemInit();
	SyncScanShmemInit();
	AsyncShmemInit();
	StatsShmemInit();
	WaitEventCustomShmemInit();
	InjectionPointShmemInit();
}

/*
 * InitializeShmemGUCs
 *
 * This function initializes runtime-computed GUCs related to the amount of
 * shared memory required for the current configuration.
 */
void
InitializeShmemGUCs(void)
{
	char		buf[64];
	Size		size_b;
	Size		size_mb;
	Size		hp_size;

	/*
	 * Calculate the shared memory size and round up to the nearest megabyte.
	 */
	size_b = CalculateShmemSize(NULL);
	size_mb = add_size(size_b, (1024 * 1024) - 1) / (1024 * 1024);
	sprintf(buf, "%zu", size_mb);
	SetConfigOption("shared_memory_size", buf,
					PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);

	/*
	 * Calculate the number of huge pages required.
	 */
	GetHugePageSize(&hp_size, NULL);
	if (hp_size != 0)
	{
		Size		hp_required;

		hp_required = add_size(size_b / hp_size, 1);
		sprintf(buf, "%zu", hp_required);
		SetConfigOption("shared_memory_size_in_huge_pages", buf,
						PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);
	}
}
