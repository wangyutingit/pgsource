/*-------------------------------------------------------------------------
 *
 * postmaster.c
 *	  This program acts as a clearing house for requests to the
 *	  POSTGRES system.  Frontend programs connect to the Postmaster,
 *	  and postmaster forks a new backend process to handle the
 *	  connection.
 *
 *	  The postmaster also manages system-wide operations such as
 *	  startup and shutdown. The postmaster itself doesn't do those
 *	  operations, mind you --- it just forks off a subprocess to do them
 *	  at the right times.  It also takes care of resetting the system
 *	  if a backend crashes.
 *
 *	  The postmaster process creates the shared memory and semaphore
 *	  pools during startup, but as a rule does not touch them itself.
 *	  In particular, it is not a member of the PGPROC array of backends
 *	  and so it cannot participate in lock-manager operations.  Keeping
 *	  the postmaster away from shared memory operations makes it simpler
 *	  and more reliable.  The postmaster is almost always able to recover
 *	  from crashes of individual backends by resetting shared memory;
 *	  if it did much with shared memory then it would be prone to crashing
 *	  along with the backends.
 *
 *	  When a request message is received, we now fork() immediately.
 *	  The child process performs authentication of the request, and
 *	  then becomes a backend if successful.  This allows the auth code
 *	  to be written in a simple single-threaded style (as opposed to the
 *	  crufty "poor man's multitasking" code that used to be needed).
 *	  More importantly, it ensures that blockages in non-multithreaded
 *	  libraries like SSL or PAM cannot cause denial of service to other
 *	  clients.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/postmaster.c
 *
 * NOTES
 *
 * Initialization:
 *		The Postmaster sets up shared memory data structures
 *		for the backends.
 *
 * Synchronization:
 *		The Postmaster shares memory with the backends but should avoid
 *		touching shared memory, so as not to become stuck if a crashing
 *		backend screws up locks or shared memory.  Likewise, the Postmaster
 *		should never block on messages from frontend clients.
 *
 * Garbage Collection:
 *		The Postmaster cleans up after backends if they have an emergency
 *		exit and/or core dump.
 *
 * Error Reporting:
 *		Use write_stderr() only for reporting "interactive" errors
 *		(essentially, bogus arguments on the command line).  Once the
 *		postmaster is launched, use ereport().
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/param.h>
#include <netdb.h>
#include <limits.h>

#ifdef USE_BONJOUR
#include <dns_sd.h>
#endif

#ifdef USE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#ifdef HAVE_PTHREAD_IS_THREADED_NP
#include <pthread.h>
#endif

#include "access/xlog.h"
#include "access/xlogrecovery.h"
#include "common/file_perm.h"
#include "common/file_utils.h"
#include "common/ip.h"
#include "common/pg_prng.h"
#include "lib/ilist.h"
#include "libpq/libpq.h"
#include "libpq/pqsignal.h"
#include "pg_getopt.h"
#include "pgstat.h"
#include "port/pg_bswap.h"
#include "postmaster/autovacuum.h"
#include "postmaster/auxprocess.h"
#include "postmaster/bgworker_internals.h"
#include "postmaster/pgarch.h"
#include "postmaster/postmaster.h"
#include "postmaster/syslogger.h"
#include "postmaster/walsummarizer.h"
#include "replication/logicallauncher.h"
#include "replication/slotsync.h"
#include "replication/walsender.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "tcop/backend_startup.h"
#include "tcop/tcopprot.h"
#include "utils/datetime.h"
#include "utils/memutils.h"
#include "utils/pidfile.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"

#ifdef EXEC_BACKEND
#include "storage/pg_shmem.h"
#endif


/*
 * Possible types of a backend. Beyond being the possible bkend_type values in
 * struct bkend, these are OR-able request flag bits for SignalSomeChildren()
 * and CountChildren().
 */
#define BACKEND_TYPE_NORMAL		0x0001	/* normal backend */
#define BACKEND_TYPE_AUTOVAC	0x0002	/* autovacuum worker process */
#define BACKEND_TYPE_WALSND		0x0004	/* walsender process */
#define BACKEND_TYPE_BGWORKER	0x0008	/* bgworker process */
#define BACKEND_TYPE_ALL		0x000F	/* OR of all the above */

/*
 * List of active backends (or child processes anyway; we don't actually
 * know whether a given child has become a backend or is still in the
 * authorization phase).  This is used mainly to keep track of how many
 * children we have and send them appropriate signals when necessary.
 *
 * As shown in the above set of backend types, this list includes not only
 * "normal" client sessions, but also autovacuum workers, walsenders, and
 * background workers.  (Note that at the time of launch, walsenders are
 * labeled BACKEND_TYPE_NORMAL; we relabel them to BACKEND_TYPE_WALSND
 * upon noticing they've changed their PMChildFlags entry.  Hence that check
 * must be done before any operation that needs to distinguish walsenders
 * from normal backends.)
 *
 * Also, "dead_end" children are in it: these are children launched just for
 * the purpose of sending a friendly rejection message to a would-be client.
 * We must track them because they are attached to shared memory, but we know
 * they will never become live backends.  dead_end children are not assigned a
 * PMChildSlot.  dead_end children have bkend_type NORMAL.
 *
 * "Special" children such as the startup, bgwriter, autovacuum launcher, and
 * slot sync worker tasks are not in this list.  They are tracked via StartupPID
 * and other pid_t variables below.  (Thus, there can't be more than one of any
 * given "special" child process type.  We use BackendList entries for any
 * child process there can be more than one of.)
 */
typedef struct bkend
{
	pid_t		pid;			/* process id of backend */
	int32		cancel_key;		/* cancel key for cancels for this backend */
	int			child_slot;		/* PMChildSlot for this backend, if any */
	int			bkend_type;		/* child process flavor, see above */
	bool		dead_end;		/* is it going to send an error and quit? */
	bool		bgworker_notify;	/* gets bgworker start/stop notifications */
	dlist_node	elem;			/* list link in BackendList */
} Backend;

static dlist_head BackendList = DLIST_STATIC_INIT(BackendList);

#ifdef EXEC_BACKEND
Backend    *ShmemBackendArray;
#endif

BackgroundWorker *MyBgworkerEntry = NULL;



/* The socket number we are listening for connections on */
int			PostPortNumber = DEF_PGPORT; ///定义在这里：include/pg_config.h:#define DEF_PGPORT 5432

/* The directory names for Unix socket(s) */
char	   *Unix_socket_directories;

/* The TCP listen address(es) */
char	   *ListenAddresses;

/*
 * SuperuserReservedConnections is the number of backends reserved for
 * superuser use, and ReservedConnections is the number of backends reserved
 * for use by roles with privileges of the pg_use_reserved_connections
 * predefined role.  These are taken out of the pool of MaxConnections backend
 * slots, so the number of backend slots available for roles that are neither
 * superuser nor have privileges of pg_use_reserved_connections is
 * (MaxConnections - SuperuserReservedConnections - ReservedConnections).
 *
 * If the number of remaining slots is less than or equal to
 * SuperuserReservedConnections, only superusers can make new connections.  If
 * the number of remaining slots is greater than SuperuserReservedConnections
 * but less than or equal to
 * (SuperuserReservedConnections + ReservedConnections), only superusers and
 * roles with privileges of pg_use_reserved_connections can make new
 * connections.  Note that pre-existing superuser and
 * pg_use_reserved_connections connections don't count against the limits.
 */
int			SuperuserReservedConnections;
int			ReservedConnections;

/* The socket(s) we're listening to. */
#define MAXLISTEN	64
static int	NumListenSockets = 0;
static pgsocket *ListenSockets = NULL;

/* still more option variables */
bool		EnableSSL = false;

int			PreAuthDelay = 0;
int			AuthenticationTimeout = 60;

bool		log_hostname;		/* for ps display and logging */
bool		Log_connections = false;

bool		enable_bonjour = false;
char	   *bonjour_name;
bool		restart_after_crash = true;
bool		remove_temp_files_after_crash = true;
bool		send_abort_for_crash = false;
bool		send_abort_for_kill = false;

/* PIDs of special child processes; 0 when not running */
static pid_t StartupPID = 0,
			BgWriterPID = 0,
			CheckpointerPID = 0,
			WalWriterPID = 0,
			WalReceiverPID = 0,
			WalSummarizerPID = 0,
			AutoVacPID = 0,
			PgArchPID = 0,
			SysLoggerPID = 0,
			SlotSyncWorkerPID = 0;

/* Startup process's status */
typedef enum
{
	STARTUP_NOT_RUNNING,
	STARTUP_RUNNING,
	STARTUP_SIGNALED,			/* we sent it a SIGQUIT or SIGKILL */
	STARTUP_CRASHED,
} StartupStatusEnum;

static StartupStatusEnum StartupStatus = STARTUP_NOT_RUNNING;

/* Startup/shutdown state */
#define			NoShutdown		0
#define			SmartShutdown	1
#define			FastShutdown	2
#define			ImmediateShutdown	3

static int	Shutdown = NoShutdown;

static bool FatalError = false; /* T if recovering from backend crash */

/*
 * We use a simple state machine to control startup, shutdown, and
 * crash recovery (which is rather like shutdown followed by startup).
 *
 * After doing all the postmaster initialization work, we enter PM_STARTUP
 * state and the startup process is launched. The startup process begins by
 * reading the control file and other preliminary initialization steps.
 * In a normal startup, or after crash recovery, the startup process exits
 * with exit code 0 and we switch to PM_RUN state.  However, archive recovery
 * is handled specially since it takes much longer and we would like to support
 * hot standby during archive recovery.
 *
 * When the startup process is ready to start archive recovery, it signals the
 * postmaster, and we switch to PM_RECOVERY state. The background writer and
 * checkpointer are launched, while the startup process continues applying WAL.
 * If Hot Standby is enabled, then, after reaching a consistent point in WAL
 * redo, startup process signals us again, and we switch to PM_HOT_STANDBY
 * state and begin accepting connections to perform read-only queries.  When
 * archive recovery is finished, the startup process exits with exit code 0
 * and we switch to PM_RUN state.
 *
 * Normal child backends can only be launched when we are in PM_RUN or
 * PM_HOT_STANDBY state.  (connsAllowed can also restrict launching.)
 * In other states we handle connection requests by launching "dead_end"
 * child processes, which will simply send the client an error message and
 * quit.  (We track these in the BackendList so that we can know when they
 * are all gone; this is important because they're still connected to shared
 * memory, and would interfere with an attempt to destroy the shmem segment,
 * possibly leading to SHMALL failure when we try to make a new one.)
 * In PM_WAIT_DEAD_END state we are waiting for all the dead_end children
 * to drain out of the system, and therefore stop accepting connection
 * requests at all until the last existing child has quit (which hopefully
 * will not be very long).
 *
 * Notice that this state variable does not distinguish *why* we entered
 * states later than PM_RUN --- Shutdown and FatalError must be consulted
 * to find that out.  FatalError is never true in PM_RECOVERY, PM_HOT_STANDBY,
 * or PM_RUN states, nor in PM_SHUTDOWN states (because we don't enter those
 * states when trying to recover from a crash).  It can be true in PM_STARTUP
 * state, because we don't clear it until we've successfully started WAL redo.
 */
typedef enum
{
	PM_INIT,					/* postmaster starting */
	PM_STARTUP,					/* waiting for startup subprocess */
	PM_RECOVERY,				/* in archive recovery mode */
	PM_HOT_STANDBY,				/* in hot standby mode */
	PM_RUN,						/* normal "database is alive" state */
	PM_STOP_BACKENDS,			/* need to stop remaining backends */
	PM_WAIT_BACKENDS,			/* waiting for live backends to exit */
	PM_SHUTDOWN,				/* waiting for checkpointer to do shutdown
								 * ckpt */
	PM_SHUTDOWN_2,				/* waiting for archiver and walsenders to
								 * finish */
	PM_WAIT_DEAD_END,			/* waiting for dead_end children to exit */
	PM_NO_CHILDREN,				/* all important children have exited */
} PMState;

static PMState pmState = PM_INIT;

/*
 * While performing a "smart shutdown", we restrict new connections but stay
 * in PM_RUN or PM_HOT_STANDBY state until all the client backends are gone.
 * connsAllowed is a sub-state indicator showing the active restriction.
 * It is of no interest unless pmState is PM_RUN or PM_HOT_STANDBY.
 */
static bool connsAllowed = true;

/* Start time of SIGKILL timeout during immediate shutdown or child crash */
/* Zero means timeout is not running */
static time_t AbortStartTime = 0;

/* Length of said timeout */
#define SIGKILL_CHILDREN_AFTER_SECS		5

static bool ReachedNormalRunning = false;	/* T if we've reached PM_RUN */

bool		ClientAuthInProgress = false;	/* T during new-client
											 * authentication */

bool		redirection_done = false;	/* stderr redirected for syslogger? */

/* received START_AUTOVAC_LAUNCHER signal */
static bool start_autovac_launcher = false;

/* the launcher needs to be signaled to communicate some condition */
static bool avlauncher_needs_signal = false;

/* received START_WALRECEIVER signal */
static bool WalReceiverRequested = false;

/* set when there's a worker that needs to be started up */
static bool StartWorkerNeeded = true;
static bool HaveCrashedWorker = false;

/* set when signals arrive */
static volatile sig_atomic_t pending_pm_pmsignal;
static volatile sig_atomic_t pending_pm_child_exit;
static volatile sig_atomic_t pending_pm_reload_request;
static volatile sig_atomic_t pending_pm_shutdown_request;
static volatile sig_atomic_t pending_pm_fast_shutdown_request;
static volatile sig_atomic_t pending_pm_immediate_shutdown_request;

/* event multiplexing object */
static WaitEventSet *pm_wait_set;

#ifdef USE_SSL
/* Set when and if SSL has been initialized properly */
bool		LoadedSSL = false;
#endif

#ifdef USE_BONJOUR
static DNSServiceRef bonjour_sdref = NULL;
#endif

/*
 * postmaster.c - function prototypes
 */
static void CloseServerPorts(int status, Datum arg);
static void unlink_external_pid_file(int status, Datum arg);
static void getInstallationPaths(const char *argv0);
static void checkControlFile(void);
static void handle_pm_pmsignal_signal(SIGNAL_ARGS);
static void handle_pm_child_exit_signal(SIGNAL_ARGS);
static void handle_pm_reload_request_signal(SIGNAL_ARGS);
static void handle_pm_shutdown_request_signal(SIGNAL_ARGS);
static void process_pm_pmsignal(void);
static void process_pm_child_exit(void);
static void process_pm_reload_request(void);
static void process_pm_shutdown_request(void);
static void dummy_handler(SIGNAL_ARGS);
static void CleanupBackend(int pid, int exitstatus);
static bool CleanupBackgroundWorker(int pid, int exitstatus);
static void HandleChildCrash(int pid, int exitstatus, const char *procname);
static void LogChildExit(int lev, const char *procname,
						 int pid, int exitstatus);
static void PostmasterStateMachine(void);

static void ExitPostmaster(int status) pg_attribute_noreturn();
static int	ServerLoop(void);
static int	BackendStartup(ClientSocket *client_sock);
static void report_fork_failure_to_client(ClientSocket *client_sock, int errnum);
static CAC_state canAcceptConnections(int backend_type);
static bool RandomCancelKey(int32 *cancel_key);
static void signal_child(pid_t pid, int signal);
static void sigquit_child(pid_t pid);
static bool SignalSomeChildren(int signal, int target);
static void TerminateChildren(int signal);

#define SignalChildren(sig)			   SignalSomeChildren(sig, BACKEND_TYPE_ALL)

static int	CountChildren(int target);
static bool assign_backendlist_entry(RegisteredBgWorker *rw);
static void maybe_start_bgworkers(void);
static bool CreateOptsFile(int argc, char *argv[], char *fullprogname);
static pid_t StartChildProcess(BackendType type);
static void StartAutovacuumWorker(void);
static void MaybeStartWalReceiver(void);
static void MaybeStartWalSummarizer(void);
static void InitPostmasterDeathWatchHandle(void);
static void MaybeStartSlotSyncWorker(void);

/*
 * Archiver is allowed to start up at the current postmaster state?
 *
 * If WAL archiving is enabled always, we are allowed to start archiver
 * even during recovery.
 */
#define PgArchStartupAllowed()	\
	(((XLogArchivingActive() && pmState == PM_RUN) ||			\
	  (XLogArchivingAlways() &&									  \
	   (pmState == PM_RECOVERY || pmState == PM_HOT_STANDBY))) && \
	 PgArchCanRestart())

#ifdef EXEC_BACKEND

#ifdef WIN32
#define WNOHANG 0				/* ignored, so any integer value will do */

static pid_t waitpid(pid_t pid, int *exitstatus, int options);
static void WINAPI pgwin32_deadchild_callback(PVOID lpParameter, BOOLEAN TimerOrWaitFired);

static HANDLE win32ChildQueue;

typedef struct
{
	HANDLE		waitHandle;
	HANDLE		procHandle;
	DWORD		procId;
} win32_deadchild_waitinfo;
#endif							/* WIN32 */

static void ShmemBackendArrayAdd(Backend *bn);
static void ShmemBackendArrayRemove(Backend *bn);
#endif							/* EXEC_BACKEND */

/* Macros to check exit status of a child process */
#define EXIT_STATUS_0(st)  ((st) == 0)
#define EXIT_STATUS_1(st)  (WIFEXITED(st) && WEXITSTATUS(st) == 1)
#define EXIT_STATUS_3(st)  (WIFEXITED(st) && WEXITSTATUS(st) == 3)

#ifndef WIN32
/*
 * File descriptors for pipe used to monitor if postmaster is alive.
 * First is POSTMASTER_FD_WATCH, second is POSTMASTER_FD_OWN.
 */
int			postmaster_alive_fds[2] = {-1, -1};
#else
/* Process handle of postmaster used for the same purpose on Windows */
HANDLE		PostmasterHandle;
#endif

/*
 * Postmaster main entry point
 */
void
PostmasterMain(int argc, char *argv[]) /// 这里是真正的主进程入口函数，由main()进行调用，进入到这里。
{
	int			opt;
	int			status;
	char	   *userDoption = NULL;
	bool		listen_addr_saved = false;
	char	   *output_config_variable = NULL;

	InitProcessGlobals(); /// 做一些简单的初始化的工作，主要是记录本进程启动的时间戳，产生一个随机值。

	PostmasterPid = MyProcPid; /// 在main()函数中执行过MyProcPid = getpid();所以MyProcPid包含本进程的进程号。

	IsPostmasterEnvironment = true; /// 表明我们在postmaster进程当中

	/*
	 * Start our win32 signal implementation
	 */
#ifdef WIN32
	pgwin32_signal_initialize();
#endif

	/*
	 * We should not be creating any files or directories before we check the
	 * data directory (see checkDataDir()), but just in case set the umask to
	 * the most restrictive (owner-only) permissions.
	 *
	 * checkDataDir() will reset the umask based on the data directory
	 * permissions.
	 */
	umask(PG_MODE_MASK_OWNER);

	/*
	 * By default, palloc() requests in the postmaster will be allocated in
	 * the PostmasterContext, which is space that can be recycled by backends.
	 * Allocated data that needs to be available to backends should be
	 * allocated in TopMemoryContext.
	 */
	PostmasterContext = AllocSetContextCreate(TopMemoryContext,
											  "Postmaster",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(PostmasterContext); /// 单独产生一个内存池PostmasterContext，放在TopMemoryContext下面
	/// 由主进程派生的子进程，根据自己的需要，可能会删除掉PostmasterContext。

	/* Initialize paths to installation files */
	getInstallationPaths(argv[0]); /// argv[0]是带路径的，如/xxxx/xxxxx/postgres

	/*
	 * Set up signal handlers for the postmaster process.
	 *
	 * CAUTION: when changing this list, check for side-effects on the signal
	 * handling setup of child processes.  See tcop/postgres.c,
	 * bootstrap/bootstrap.c, postmaster/bgwriter.c, postmaster/walwriter.c,
	 * postmaster/autovacuum.c, postmaster/pgarch.c, postmaster/syslogger.c,
	 * postmaster/bgworker.c and postmaster/checkpointer.c.
	 */
	pqinitmask();
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);

	pqsignal(SIGHUP, handle_pm_reload_request_signal);
	pqsignal(SIGINT, handle_pm_shutdown_request_signal);
	pqsignal(SIGQUIT, handle_pm_shutdown_request_signal);
	pqsignal(SIGTERM, handle_pm_shutdown_request_signal);
	pqsignal(SIGALRM, SIG_IGN); /* ignored */
	pqsignal(SIGPIPE, SIG_IGN); /* ignored */
	pqsignal(SIGUSR1, handle_pm_pmsignal_signal);
	pqsignal(SIGUSR2, dummy_handler);	/* unused, reserve for children */
	pqsignal(SIGCHLD, handle_pm_child_exit_signal);

	/* This may configure SIGURG, depending on platform. */
	InitializeLatchSupport();
	InitProcessLocalLatch();

	/*
	 * No other place in Postgres should touch SIGTTIN/SIGTTOU handling.  We
	 * ignore those signals in a postmaster environment, so that there is no
	 * risk of a child process freezing up due to writing to stderr.  But for
	 * a standalone backend, their default handling is reasonable.  Hence, all
	 * child processes should just allow the inherited settings to stand.
	 */
#ifdef SIGTTIN
	pqsignal(SIGTTIN, SIG_IGN); /* ignored */
#endif
#ifdef SIGTTOU
	pqsignal(SIGTTOU, SIG_IGN); /* ignored */
#endif

	/* ignore SIGXFSZ, so that ulimit violations work like disk full */
#ifdef SIGXFSZ
	pqsignal(SIGXFSZ, SIG_IGN); /* ignored */
#endif

	/* Begin accepting signals. */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * Options setup
	 */
	InitializeGUCOptions();

	opterr = 1;

	/*
	 * Parse command-line options.  CAUTION: keep this in sync with
	 * tcop/postgres.c (the option sets should not conflict) and with the
	 * common help() function in main/main.c.
	 */
	while ((opt = getopt(argc, argv, "B:bC:c:D:d:EeFf:h:ijk:lN:OPp:r:S:sTt:W:-:")) != -1)
	{
		switch (opt)
		{
			case 'B':
				SetConfigOption("shared_buffers", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'b':
				/* Undocumented flag used for binary upgrades */
				IsBinaryUpgrade = true;
				break;

			case 'C':
				output_config_variable = strdup(optarg);
				break;

			case 'c':
			case '-':
				{
					char	   *name,
							   *value;

					ParseLongOption(optarg, &name, &value);
					if (!value)
					{
						if (opt == '-')
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("--%s requires a value",
											optarg)));
						else
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("-c %s requires a value",
											optarg)));
					}

					SetConfigOption(name, value, PGC_POSTMASTER, PGC_S_ARGV);
					pfree(name);
					pfree(value);
					break;
				}

			case 'D':
				userDoption = strdup(optarg); /// 如果指定 -D /xxx/xxx，则optarg和userDoption的值都是/xxx/xxx
				break;

			case 'd':
				set_debug_options(atoi(optarg), PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'E':
				SetConfigOption("log_statement", "all", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'e':
				SetConfigOption("datestyle", "euro", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'F':
				SetConfigOption("fsync", "false", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'f':
				if (!set_plan_disabling_options(optarg, PGC_POSTMASTER, PGC_S_ARGV))
				{
					write_stderr("%s: invalid argument for option -f: \"%s\"\n",
								 progname, optarg);
					ExitPostmaster(1);
				}
				break;

			case 'h':
				SetConfigOption("listen_addresses", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'i':
				SetConfigOption("listen_addresses", "*", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'j':
				/* only used by interactive backend */
				break;

			case 'k':
				SetConfigOption("unix_socket_directories", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'l':
				SetConfigOption("ssl", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'N':
				SetConfigOption("max_connections", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'O':
				SetConfigOption("allow_system_table_mods", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'P':
				SetConfigOption("ignore_system_indexes", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'p':
				SetConfigOption("port", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'r':
				/* only used by single-user backend */
				break;

			case 'S':
				SetConfigOption("work_mem", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 's':
				SetConfigOption("log_statement_stats", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'T':

				/*
				 * This option used to be defined as sending SIGSTOP after a
				 * backend crash, but sending SIGABRT seems more useful.
				 */
				SetConfigOption("send_abort_for_crash", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 't':
				{
					const char *tmp = get_stats_option_name(optarg);

					if (tmp)
					{
						SetConfigOption(tmp, "true", PGC_POSTMASTER, PGC_S_ARGV);
					}
					else
					{
						write_stderr("%s: invalid argument for option -t: \"%s\"\n",
									 progname, optarg);
						ExitPostmaster(1);
					}
					break;
				}

			case 'W':
				SetConfigOption("post_auth_delay", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			default:
				write_stderr("Try \"%s --help\" for more information.\n",
							 progname);
				ExitPostmaster(1);
		}
	}

	/*
	 * Postmaster accepts no non-option switch arguments.
	 */
	if (optind < argc)
	{
		write_stderr("%s: invalid argument: \"%s\"\n",
					 progname, argv[optind]);
		write_stderr("Try \"%s --help\" for more information.\n",
					 progname);
		ExitPostmaster(1);
	}

	/*
	 * Locate the proper configuration files and data directory, and read
	 * postgresql.conf for the first time.
	 */
	if (!SelectConfigFiles(userDoption, progname)) 
		ExitPostmaster(2);

	if (output_config_variable != NULL)
	{
		/*
		 * If this is a runtime-computed GUC, it hasn't yet been initialized,
		 * and the present value is not useful.  However, this is a convenient
		 * place to print the value for most GUCs because it is safe to run
		 * postmaster startup to this point even if the server is already
		 * running.  For the handful of runtime-computed GUCs that we cannot
		 * provide meaningful values for yet, we wait until later in
		 * postmaster startup to print the value.  We won't be able to use -C
		 * on running servers for those GUCs, but using this option now would
		 * lead to incorrect results for them.
		 */
		int			flags = GetConfigOptionFlags(output_config_variable, true);

		if ((flags & GUC_RUNTIME_COMPUTED) == 0)
		{
			/*
			 * "-C guc" was specified, so print GUC's value and exit.  No
			 * extra permission check is needed because the user is reading
			 * inside the data dir.
			 */
			const char *config_val = GetConfigOption(output_config_variable,
													 false, false);

			puts(config_val ? config_val : "");
			ExitPostmaster(0);
		}

		/*
		 * A runtime-computed GUC will be printed later on.  As we initialize
		 * a server startup sequence, silence any log messages that may show
		 * up in the output generated.  FATAL and more severe messages are
		 * useful to show, even if one would only expect at least PANIC.  LOG
		 * entries are hidden.
		 */
		SetConfigOption("log_min_messages", "FATAL", PGC_SUSET,
						PGC_S_OVERRIDE);
	}

	/* Verify that DataDir looks reasonable */
	checkDataDir(); /// 检查数据库集群的目录，大版本等信息。如果发现错误，就拒绝启动

	/* Check that pg_control exists */
	checkControlFile(); /// 对控制文件进行初步检查，并不读取其中的内容

	/* And switch working directory into it */
	ChangeToDataDir(); /// 把$PGDATA 当做本进程的当前目录，Current Work Directory

	/*
	 * Check for invalid combinations of GUC settings.
	 */
	/// 缺省情况下SuperuserReservedConnections = 3， ReservedConnections=0， MaxConnections = 100
	if (SuperuserReservedConnections + ReservedConnections >= MaxConnections) /// 检查关于连接数量的相关参数的设置。
	{
		write_stderr("%s: \"superuser_reserved_connections\" (%d) plus \"reserved_connections\" (%d) must be less than \"max_connections\" (%d)\n",
					 progname,
					 SuperuserReservedConnections, ReservedConnections,
					 MaxConnections);
		ExitPostmaster(1);
	}
	/// 缺省情况下，XLogArchiveMode=0, wal_level=WAL_LEVEL_REPLICA，max_wal_senders = 10
	/// summarize_wal = false
	if (XLogArchiveMode > ARCHIVE_MODE_OFF && wal_level == WAL_LEVEL_MINIMAL) /// 归档模式和 wal_level 的关系检查
		ereport(ERROR,
				(errmsg("WAL archival cannot be enabled when \"wal_level\" is \"minimal\"")));
	if (max_wal_senders > 0 && wal_level == WAL_LEVEL_MINIMAL)
		ereport(ERROR,
				(errmsg("WAL streaming (\"max_wal_senders\" > 0) requires \"wal_level\" to be \"replica\" or \"logical\"")));
	if (summarize_wal && wal_level == WAL_LEVEL_MINIMAL)
		ereport(ERROR,
				(errmsg("WAL cannot be summarized when \"wal_level\" is \"minimal\"")));

	/*
	 * Other one-time internal sanity checks can go here, if they are fast.
	 * (Put any slow processing further down, after postmaster.pid creation.)
	 */
	if (!CheckDateTokenTables())
	{
		write_stderr("%s: invalid datetoken tables, please fix\n", progname);
		ExitPostmaster(1);
	}

	/*
	 * Now that we are done processing the postmaster arguments, reset
	 * getopt(3) library so that it will work correctly in subprocesses.
	 */
	optind = 1;
#ifdef HAVE_INT_OPTRESET
	optreset = 1;				/* some systems need this too */
#endif

	/* For debugging: display postmaster environment */
	{
		extern char **environ; /// 只有日志级别设置为 DEBUG3 或者更高，才显示本进程的环境变量，供排错使用。
		char	  **p;

		ereport(DEBUG3,
				(errmsg_internal("%s: PostmasterMain: initial environment dump:",
								 progname)));
		ereport(DEBUG3,
				(errmsg_internal("-----------------------------------------")));
		for (p = environ; *p; ++p)
			ereport(DEBUG3,
					(errmsg_internal("\t%s", *p)));
		ereport(DEBUG3,
				(errmsg_internal("-----------------------------------------")));
	}

	/*
	 * Create lockfile for data directory.
	 *
	 * We want to do this before we try to grab the input sockets, because the
	 * data directory interlock is more reliable than the socket-file
	 * interlock (thanks to whoever decided to put socket files in /tmp :-().
	 * For the same reason, it's best to grab the TCP socket(s) before the
	 * Unix socket(s).
	 *
	 * Also note that this internally sets up the on_proc_exit function that
	 * is responsible for removing both data directory and socket lockfiles;
	 * so it must happen before opening sockets so that at exit, the socket
	 * lockfiles go away after CloseServerPorts runs.
	 */
	CreateDataDirLockFile(true); /// 锁文件就是postmaster.pid ： define DIRECTORY_LOCK_FILE		"postmaster.pid"

	/*
	 * Read the control file (for error checking and config info).
	 *
	 * Since we verify the control file's CRC, this has a useful side effect
	 * on machines where we need a run-time test for CRC support instructions.
	 * The postmaster will do the test once at startup, and then its child
	 * processes will inherit the correct function pointer and not need to
	 * repeat the test.
	 */
	LocalProcessControlFile(false); /// 读取控制文件的内容到一个指针指向的内容中。如果控制文件中的内容不合法，就报错退出了。

	/*
	 * Register the apply launcher.  It's probably a good idea to call this
	 * before any modules had a chance to take the background worker slots.
	 */
	ApplyLauncherRegister();

	/*
	 * process any libraries that should be preloaded at postmaster start
	 */
	process_shared_preload_libraries();

	/*
	 * Initialize SSL library, if specified.
	 */
#ifdef USE_SSL
	if (EnableSSL)
	{
		(void) secure_initialize(true);
		LoadedSSL = true;
	}
#endif

	/*
	 * Now that loadable modules have had their chance to alter any GUCs,
	 * calculate MaxBackends.
	 */
	InitializeMaxBackends();

	/*
	 * Give preloaded libraries a chance to request additional shared memory.
	 */
	process_shmem_requests();

	/*
	 * Now that loadable modules have had their chance to request additional
	 * shared memory, determine the value of any runtime-computed GUCs that
	 * depend on the amount of shared memory required.
	 */
	InitializeShmemGUCs();

	/*
	 * Now that modules have been loaded, we can process any custom resource
	 * managers specified in the wal_consistency_checking GUC.
	 */
	InitializeWalConsistencyChecking();

	/*
	 * If -C was specified with a runtime-computed GUC, we held off printing
	 * the value earlier, as the GUC was not yet initialized.  We handle -C
	 * for most GUCs before we lock the data directory so that the option may
	 * be used on a running server.  However, a handful of GUCs are runtime-
	 * computed and do not have meaningful values until after locking the data
	 * directory, and we cannot safely calculate their values earlier on a
	 * running server.  At this point, such GUCs should be properly
	 * initialized, and we haven't yet set up shared memory, so this is a good
	 * time to handle the -C option for these special GUCs.
	 */
	if (output_config_variable != NULL)
	{
		const char *config_val = GetConfigOption(output_config_variable,
												 false, false);

		puts(config_val ? config_val : "");
		ExitPostmaster(0);
	}

	/*
	 * Set up shared memory and semaphores.
	 *
	 * Note: if using SysV shmem and/or semas, each postmaster startup will
	 * normally choose the same IPC keys.  This helps ensure that we will
	 * clean up dead IPC objects if the postmaster crashes and is restarted.
	 */
	CreateSharedMemoryAndSemaphores(); /// 整个数据库集群运行过程中，这个函数只调用了一次。

	/*
	 * Estimate number of openable files.  This must happen after setting up
	 * semaphores, because on some platforms semaphores count as open files.
	 */
	set_max_safe_fds();

	/*
	 * Set reference point for stack-depth checking.
	 */
	(void) set_stack_base();

	/*
	 * Initialize pipe (or process handle on Windows) that allows children to
	 * wake up from sleep on postmaster death.
	 */
	InitPostmasterDeathWatchHandle();

#ifdef WIN32

	/*
	 * Initialize I/O completion port used to deliver list of dead children.
	 */
	win32ChildQueue = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
	if (win32ChildQueue == NULL)
		ereport(FATAL,
				(errmsg("could not create I/O completion port for child queue")));
#endif

#ifdef EXEC_BACKEND
	/* Write out nondefault GUC settings for child processes to use */
	write_nondefault_variables(PGC_POSTMASTER);

	/*
	 * Clean out the temp directory used to transmit parameters to child
	 * processes (see internal_forkexec).  We must do this before launching
	 * any child processes, else we have a race condition: we could remove a
	 * parameter file before the child can read it.  It should be safe to do
	 * so now, because we verified earlier that there are no conflicting
	 * Postgres processes in this data directory.
	 */
	RemovePgTempFilesInDir(PG_TEMP_FILES_DIR, true, false);
#endif

	/*
	 * Forcibly remove the files signaling a standby promotion request.
	 * Otherwise, the existence of those files triggers a promotion too early,
	 * whether a user wants that or not.
	 *
	 * This removal of files is usually unnecessary because they can exist
	 * only during a few moments during a standby promotion. However there is
	 * a race condition: if pg_ctl promote is executed and creates the files
	 * during a promotion, the files can stay around even after the server is
	 * brought up to be the primary.  Then, if a new standby starts by using
	 * the backup taken from the new primary, the files can exist at server
	 * startup and must be removed in order to avoid an unexpected promotion.
	 *
	 * Note that promotion signal files need to be removed before the startup
	 * process is invoked. Because, after that, they can be used by
	 * postmaster's SIGUSR1 signal handler.
	 */
	RemovePromoteSignalFiles(); /// 这个函数比较简单，就是调用unlink系统函数删除promote文件

	/* Do the same for logrotate signal file */
	RemoveLogrotateSignalFiles();

	/* Remove any outdated file holding the current log filenames. */
	if (unlink(LOG_METAINFO_DATAFILE) < 0 && errno != ENOENT)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m",
						LOG_METAINFO_DATAFILE)));

	/*
	 * If enabled, start up syslogger collection subprocess
	 */
	SysLoggerPID = SysLogger_Start(); /// 根据参数配置来决定是否启动日志搜集进程，缺省情况下是不启动这个进程。

	/*
	 * Reset whereToSendOutput from DestDebug (its starting state) to
	 * DestNone. This stops ereport from sending log messages to stderr unless
	 * Log_destination permits.  We don't do this until the postmaster is
	 * fully launched, since startup failures may as well be reported to
	 * stderr.
	 *
	 * If we are in fact disabling logging to stderr, first emit a log message
	 * saying so, to provide a breadcrumb trail for users who may not remember
	 * that their logging is configured to go somewhere else.
	 */
	if (!(Log_destination & LOG_DESTINATION_STDERR))
		ereport(LOG,
				(errmsg("ending log output to stderr"),
				 errhint("Future log output will go to log destination \"%s\".",
						 Log_destination_string)));

	whereToSendOutput = DestNone;

	/*
	 * Report server startup in log.  While we could emit this much earlier,
	 * it seems best to do so after starting the log collector, if we intend
	 * to use one.
	 */
	ereport(LOG,
			(errmsg("starting %s", PG_VERSION_STR)));

	/*
	 * Establish input sockets.
	 *
	 * First set up an on_proc_exit function that's charged with closing the
	 * sockets again at postmaster shutdown.
	 */
	ListenSockets = palloc(MAXLISTEN * sizeof(pgsocket));
	on_proc_exit(CloseServerPorts, 0);

	if (ListenAddresses)
	{
		char	   *rawstring;
		List	   *elemlist;
		ListCell   *l;
		int			success = 0;

		/* Need a modifiable copy of ListenAddresses */
		rawstring = pstrdup(ListenAddresses);

		/* Parse string into list of hostnames */
		if (!SplitGUCList(rawstring, ',', &elemlist))
		{
			/* syntax error in list */
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid list syntax in parameter \"%s\"",
							"listen_addresses")));
		}

		foreach(l, elemlist)
		{
			char	   *curhost = (char *) lfirst(l);

			if (strcmp(curhost, "*") == 0)
				status = ListenServerPort(AF_UNSPEC, NULL,
										  (unsigned short) PostPortNumber,
										  NULL,
										  ListenSockets,
										  &NumListenSockets,
										  MAXLISTEN);
			else
				status = ListenServerPort(AF_UNSPEC, curhost,
										  (unsigned short) PostPortNumber,
										  NULL,
										  ListenSockets,
										  &NumListenSockets,
										  MAXLISTEN);

			if (status == STATUS_OK)
			{
				success++;
				/* record the first successful host addr in lockfile */
				if (!listen_addr_saved)
				{
					AddToDataDirLockFile(LOCK_FILE_LINE_LISTEN_ADDR, curhost);
					listen_addr_saved = true;
				}
			}
			else
				ereport(WARNING,
						(errmsg("could not create listen socket for \"%s\"",
								curhost)));
		}

		if (!success && elemlist != NIL)
			ereport(FATAL,
					(errmsg("could not create any TCP/IP sockets")));

		list_free(elemlist);
		pfree(rawstring);
	}

#ifdef USE_BONJOUR
	/* Register for Bonjour only if we opened TCP socket(s) */
	if (enable_bonjour && NumListenSockets > 0)
	{
		DNSServiceErrorType err;

		/*
		 * We pass 0 for interface_index, which will result in registering on
		 * all "applicable" interfaces.  It's not entirely clear from the
		 * DNS-SD docs whether this would be appropriate if we have bound to
		 * just a subset of the available network interfaces.
		 */
		err = DNSServiceRegister(&bonjour_sdref,
								 0,
								 0,
								 bonjour_name,
								 "_postgresql._tcp.",
								 NULL,
								 NULL,
								 pg_hton16(PostPortNumber),
								 0,
								 NULL,
								 NULL,
								 NULL);
		if (err != kDNSServiceErr_NoError)
			ereport(LOG,
					(errmsg("DNSServiceRegister() failed: error code %ld",
							(long) err)));

		/*
		 * We don't bother to read the mDNS daemon's reply, and we expect that
		 * it will automatically terminate our registration when the socket is
		 * closed at postmaster termination.  So there's nothing more to be
		 * done here.  However, the bonjour_sdref is kept around so that
		 * forked children can close their copies of the socket.
		 */
	}
#endif

	if (Unix_socket_directories)
	{
		char	   *rawstring;
		List	   *elemlist;
		ListCell   *l;
		int			success = 0;

		/* Need a modifiable copy of Unix_socket_directories */
		rawstring = pstrdup(Unix_socket_directories);

		/* Parse string into list of directories */
		if (!SplitDirectoriesString(rawstring, ',', &elemlist))
		{
			/* syntax error in list */
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid list syntax in parameter \"%s\"",
							"unix_socket_directories")));
		}

		foreach(l, elemlist)
		{
			char	   *socketdir = (char *) lfirst(l);

			status = ListenServerPort(AF_UNIX, NULL,
									  (unsigned short) PostPortNumber,
									  socketdir,
									  ListenSockets,
									  &NumListenSockets,
									  MAXLISTEN);

			if (status == STATUS_OK)
			{
				success++;
				/* record the first successful Unix socket in lockfile */
				if (success == 1)
					AddToDataDirLockFile(LOCK_FILE_LINE_SOCKET_DIR, socketdir);
			}
			else
				ereport(WARNING,
						(errmsg("could not create Unix-domain socket in directory \"%s\"",
								socketdir)));
		}

		if (!success && elemlist != NIL)
			ereport(FATAL,
					(errmsg("could not create any Unix-domain sockets")));

		list_free_deep(elemlist);
		pfree(rawstring);
	}

	/*
	 * check that we have some socket to listen on
	 */
	if (NumListenSockets == 0)
		ereport(FATAL,
				(errmsg("no socket created for listening")));

	/*
	 * If no valid TCP ports, write an empty line for listen address,
	 * indicating the Unix socket must be used.  Note that this line is not
	 * added to the lock file until there is a socket backing it.
	 */
	if (!listen_addr_saved)
		AddToDataDirLockFile(LOCK_FILE_LINE_LISTEN_ADDR, "");

	/*
	 * Record postmaster options.  We delay this till now to avoid recording
	 * bogus options (eg, unusable port number).
	 */
	if (!CreateOptsFile(argc, argv, my_exec_path)) /// 创建postmaster.opts文件。如果失败就退出整个进程。
		ExitPostmaster(1);

	/*
	 * Write the external PID file if requested
	 */
	if (external_pid_file)
	{
		FILE	   *fpidfile = fopen(external_pid_file, "w");

		if (fpidfile)
		{
			fprintf(fpidfile, "%d\n", MyProcPid);
			fclose(fpidfile);

			/* Make PID file world readable */
			if (chmod(external_pid_file, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) != 0)
				write_stderr("%s: could not change permissions of external PID file \"%s\": %m\n",
							 progname, external_pid_file);
		}
		else
			write_stderr("%s: could not write external PID file \"%s\": %m\n",
						 progname, external_pid_file);

		on_proc_exit(unlink_external_pid_file, 0);
	}

	/*
	 * Remove old temporary files.  At this point there can be no other
	 * Postgres processes running in this directory, so this should be safe.
	 */
	RemovePgTempFiles();

	/*
	 * Initialize the autovacuum subsystem (again, no process start yet)
	 */
	autovac_init();

	/*
	 * Load configuration files for client authentication.
	 */
	if (!load_hba()) /// 读取 pg_hba.conf 配置文件中的信息，如果出错，整个进程就退出了。
	{
		/*
		 * It makes no sense to continue if we fail to load the HBA file,
		 * since there is no way to connect to the database in this case.
		 */
		ereport(FATAL,
		/* translator: %s is a configuration file */
				(errmsg("could not load %s", HbaFileName))); /// extern PGDLLIMPORT char *HbaFileName;
	}
	if (!load_ident()) /// 如果 pg_ident.conf不存在，也无所谓
	{
		/*
		 * We can start up without the IDENT file, although it means that you
		 * cannot log in using any of the authentication methods that need a
		 * user name mapping. load_ident() already logged the details of error
		 * to the log.
		 */
	}

#ifdef HAVE_PTHREAD_IS_THREADED_NP

	/*
	 * On macOS, libintl replaces setlocale() with a version that calls
	 * CFLocaleCopyCurrent() when its second argument is "" and every relevant
	 * environment variable is unset or empty.  CFLocaleCopyCurrent() makes
	 * the process multithreaded.  The postmaster calls sigprocmask() and
	 * calls fork() without an immediate exec(), both of which have undefined
	 * behavior in a multithreaded program.  A multithreaded postmaster is the
	 * normal case on Windows, which offers neither fork() nor sigprocmask().
	 * Currently, macOS is the only platform having pthread_is_threaded_np(),
	 * so we need not worry whether this HINT is appropriate elsewhere.
	 */
	if (pthread_is_threaded_np() != 0)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("postmaster became multithreaded during startup"),
				 errhint("Set the LC_ALL environment variable to a valid locale.")));
#endif

	/*
	 * Remember postmaster startup time
	 */
	PgStartTime = GetCurrentTimestamp(); /// 获取当前的系统时间，保存在全局变量中，记录主进程的启动时间。

	/*
	 * Report postmaster status in the postmaster.pid file, to allow pg_ctl to
	 * see what's happening.
	 */
	AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STARTING);

	/* Start bgwriter and checkpointer so they can help with recovery */
	if (CheckpointerPID == 0) /// 先启动 CheckPoint 进程和 Bgwriter 进程，确保 StartUp 进程能够正常工作。
		CheckpointerPID = StartChildProcess(B_CHECKPOINTER);
	if (BgWriterPID == 0)
		BgWriterPID = StartChildProcess(B_BG_WRITER);

	/*
	 * We're ready to rock and roll...
	 */
	StartupPID = StartChildProcess(B_STARTUP); /// 启动 Startup 进程，如果失败，整个数据库集群无法启动。
	Assert(StartupPID != 0);
	StartupStatus = STARTUP_RUNNING;
	pmState = PM_STARTUP;

	/* Some workers may be scheduled to start now */
	maybe_start_bgworkers();

	status = ServerLoop(); /// 这个是postmaster主进程的主要循环逻辑。

	/*
	 * ServerLoop probably shouldn't ever return, but if it does, close down.
	 */
	ExitPostmaster(status != STATUS_OK);

	abort();					/* not reached */
}


/*
 * on_proc_exit callback to close server's listen sockets
 */
static void
CloseServerPorts(int status, Datum arg)
{
	int			i;

	/*
	 * First, explicitly close all the socket FDs.  We used to just let this
	 * happen implicitly at postmaster exit, but it's better to close them
	 * before we remove the postmaster.pid lockfile; otherwise there's a race
	 * condition if a new postmaster wants to re-use the TCP port number.
	 */
	for (i = 0; i < NumListenSockets; i++)
	{
		if (closesocket(ListenSockets[i]) != 0)
			elog(LOG, "could not close listen socket: %m");
	}
	NumListenSockets = 0;

	/*
	 * Next, remove any filesystem entries for Unix sockets.  To avoid race
	 * conditions against incoming postmasters, this must happen after closing
	 * the sockets and before removing lock files.
	 */
	RemoveSocketFiles();

	/*
	 * We don't do anything about socket lock files here; those will be
	 * removed in a later on_proc_exit callback.
	 */
}

/*
 * on_proc_exit callback to delete external_pid_file
 */
static void
unlink_external_pid_file(int status, Datum arg)
{
	if (external_pid_file)
		unlink(external_pid_file);
}


/*
 * Compute and check the directory paths to files that are part of the
 * installation (as deduced from the postgres executable's own location)
 */
static void
getInstallationPaths(const char *argv0)
{
	DIR		   *pdir;

	/* Locate the postgres executable itself */
	if (find_my_exec(argv0, my_exec_path) < 0)
		ereport(FATAL,
				(errmsg("%s: could not locate my own executable path", argv0)));

#ifdef EXEC_BACKEND
	/* Locate executable backend before we change working directory */
	if (find_other_exec(argv0, "postgres", PG_BACKEND_VERSIONSTR,
						postgres_exec_path) < 0)
		ereport(FATAL,
				(errmsg("%s: could not locate matching postgres executable",
						argv0)));
#endif

	/*
	 * Locate the pkglib directory --- this has to be set early in case we try
	 * to load any modules from it in response to postgresql.conf entries.
	 */
	/// 根据调试结果，my_exec_path="/home/postgres/pg175/bin/postgres"
	/// 这个函数执行完毕后，pkglib_path = "/home/postgres/pg175/lib"
	get_pkglib_path(my_exec_path, pkglib_path);

	/*
	 * Verify that there's a readable directory there; otherwise the Postgres
	 * installation is incomplete or corrupt.  (A typical cause of this
	 * failure is that the postgres executable has been moved or hardlinked to
	 * some directory that's not a sibling of the installation lib/
	 * directory.)
	 */
	pdir = AllocateDir(pkglib_path);
	if (pdir == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m",
						pkglib_path),
				 errhint("This may indicate an incomplete PostgreSQL installation, or that the file \"%s\" has been moved away from its proper location.",
						 my_exec_path)));
	FreeDir(pdir);

	/*
	 * It's not worth checking the share/ directory.  If the lib/ directory is
	 * there, then share/ probably is too.
	 */
}

/*
 * Check that pg_control exists in the correct location in the data directory.
 *
 * No attempt is made to validate the contents of pg_control here.  This is
 * just a sanity check to see if we are looking at a real data directory.
 */
static void
checkControlFile(void)
{
	char		path[MAXPGPATH];
	FILE	   *fp;

	snprintf(path, sizeof(path), "%s/global/pg_control", DataDir); /// $PGDATA/global/pg_control 这个是控制文件

	fp = AllocateFile(path, PG_BINARY_R);
	if (fp == NULL)
	{
		write_stderr("%s: could not find the database system\n"
					 "Expected to find it in the directory \"%s\",\n"
					 "but could not open file \"%s\": %m\n",
					 progname, DataDir, path);
		ExitPostmaster(2);
	}
	FreeFile(fp);
}

/*
 * Determine how long should we let ServerLoop sleep, in milliseconds.
 *
 * In normal conditions we wait at most one minute, to ensure that the other
 * background tasks handled by ServerLoop get done even when no requests are
 * arriving.  However, if there are background workers waiting to be started,
 * we don't actually sleep so that they are quickly serviced.  Other exception
 * cases are as shown in the code.
 */
static int
DetermineSleepTime(void)
{
	TimestampTz next_wakeup = 0;

	/*
	 * Normal case: either there are no background workers at all, or we're in
	 * a shutdown sequence (during which we ignore bgworkers altogether).
	 */
	if (Shutdown > NoShutdown ||
		(!StartWorkerNeeded && !HaveCrashedWorker))
	{
		if (AbortStartTime != 0)
		{
			int			seconds;

			/* time left to abort; clamp to 0 in case it already expired */
			seconds = SIGKILL_CHILDREN_AFTER_SECS -
				(time(NULL) - AbortStartTime);

			return Max(seconds * 1000, 0);
		}
		else
			return 60 * 1000;
	}

	if (StartWorkerNeeded)
		return 0;

	if (HaveCrashedWorker)
	{
		slist_mutable_iter siter;

		/*
		 * When there are crashed bgworkers, we sleep just long enough that
		 * they are restarted when they request to be.  Scan the list to
		 * determine the minimum of all wakeup times according to most recent
		 * crash time and requested restart interval.
		 */
		slist_foreach_modify(siter, &BackgroundWorkerList)
		{
			RegisteredBgWorker *rw;
			TimestampTz this_wakeup;

			rw = slist_container(RegisteredBgWorker, rw_lnode, siter.cur);

			if (rw->rw_crashed_at == 0)
				continue;

			if (rw->rw_worker.bgw_restart_time == BGW_NEVER_RESTART
				|| rw->rw_terminate)
			{
				ForgetBackgroundWorker(&siter);
				continue;
			}

			this_wakeup = TimestampTzPlusMilliseconds(rw->rw_crashed_at,
													  1000L * rw->rw_worker.bgw_restart_time);
			if (next_wakeup == 0 || this_wakeup < next_wakeup)
				next_wakeup = this_wakeup;
		}
	}

	if (next_wakeup != 0)
	{
		int			ms;

		/* result of TimestampDifferenceMilliseconds is in [0, INT_MAX] */
		ms = (int) TimestampDifferenceMilliseconds(GetCurrentTimestamp(),
												   next_wakeup);
		return Min(60 * 1000, ms);
	}

	return 60 * 1000;
}

/*
 * Activate or deactivate notifications of server socket events.  Since we
 * don't currently have a way to remove events from an existing WaitEventSet,
 * we'll just destroy and recreate the whole thing.  This is called during
 * shutdown so we can wait for backends to exit without accepting new
 * connections, and during crash reinitialization when we need to start
 * listening for new connections again.  The WaitEventSet will be freed in fork
 * children by ClosePostmasterPorts().
 */
static void
ConfigurePostmasterWaitSet(bool accept_connections)
{
	if (pm_wait_set)
		FreeWaitEventSet(pm_wait_set);
	pm_wait_set = NULL;

	pm_wait_set = CreateWaitEventSet(NULL,
									 accept_connections ? (1 + NumListenSockets) : 1);
	AddWaitEventToSet(pm_wait_set, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch,
					  NULL);

	if (accept_connections)
	{
		for (int i = 0; i < NumListenSockets; i++)
			AddWaitEventToSet(pm_wait_set, WL_SOCKET_ACCEPT, ListenSockets[i],
							  NULL, NULL);
	}
}

/*
 * Main idle loop of postmaster
 */
static int
ServerLoop(void)
{
	time_t		last_lockfile_recheck_time,
				last_touch_time;
	WaitEvent	events[MAXLISTEN];
	int			nevents;

	ConfigurePostmasterWaitSet(true);
	last_lockfile_recheck_time = last_touch_time = time(NULL); /// 获取当前时间

	for (;;) /// 无限循环
	{
		time_t		now;

		nevents = WaitEventSetWait(pm_wait_set,
								   DetermineSleepTime(),
								   events,
								   lengthof(events),
								   0 /* postmaster posts no wait_events */ );

		/*
		 * Latch set by signal handler, or new connection pending on any of
		 * our sockets? If the latter, fork a child process to deal with it.
		 */
		for (int i = 0; i < nevents; i++) /// 扫描事件数组
		{
			if (events[i].events & WL_LATCH_SET)
				ResetLatch(MyLatch); /// 重置latch

			/*
			 * The following requests are handled unconditionally, even if we
			 * didn't see WL_LATCH_SET.  This gives high priority to shutdown
			 * and reload requests where the latch happens to appear later in
			 * events[] or will be reported by a later call to
			 * WaitEventSetWait().
			 */
			if (pending_pm_shutdown_request)
				process_pm_shutdown_request();
			if (pending_pm_reload_request)
				process_pm_reload_request();
			if (pending_pm_child_exit)
				process_pm_child_exit();
			if (pending_pm_pmsignal)
				process_pm_pmsignal();

			if (events[i].events & WL_SOCKET_ACCEPT)
			{
				ClientSocket s;

				if (AcceptConnection(events[i].fd, &s) == STATUS_OK)
					BackendStartup(&s); /// 在这里启动postgres进程？

				/* We no longer need the open socket in this process */
				if (s.sock != PGINVALID_SOCKET)
				{
					if (closesocket(s.sock) != 0)
						elog(LOG, "could not close client socket: %m");
				}
			}
		}

		/* If we have lost the log collector, try to start a new one */
		if (SysLoggerPID == 0 && Logging_collector)
			SysLoggerPID = SysLogger_Start();

		/*
		 * If no background writer process is running, and we are not in a
		 * state that prevents it, start one.  It doesn't matter if this
		 * fails, we'll just try again later.  Likewise for the checkpointer.
		 */
		if (pmState == PM_RUN || pmState == PM_RECOVERY ||
			pmState == PM_HOT_STANDBY || pmState == PM_STARTUP) /// 处于初始状态时尝试启动checkpointer和bgwriter进程。
		{
			if (CheckpointerPID == 0)
				CheckpointerPID = StartChildProcess(B_CHECKPOINTER);
			if (BgWriterPID == 0)
				BgWriterPID = StartChildProcess(B_BG_WRITER);
		}

		/*
		 * Likewise, if we have lost the walwriter process, try to start a new
		 * one.  But this is needed only in normal operation (else we cannot
		 * be writing any new WAL).
		 */
		if (WalWriterPID == 0 && pmState == PM_RUN)
			WalWriterPID = StartChildProcess(B_WAL_WRITER);

		/*
		 * If we have lost the autovacuum launcher, try to start a new one. We
		 * don't want autovacuum to run in binary upgrade mode because
		 * autovacuum might update relfrozenxid for empty tables before the
		 * physical files are put in place.
		 */
		if (!IsBinaryUpgrade && AutoVacPID == 0 &&
			(AutoVacuumingActive() || start_autovac_launcher) &&
			pmState == PM_RUN)
		{
			AutoVacPID = StartChildProcess(B_AUTOVAC_LAUNCHER);
			if (AutoVacPID != 0)
				start_autovac_launcher = false; /* signal processed */
		}

		/* If we have lost the archiver, try to start a new one. */
		if (PgArchPID == 0 && PgArchStartupAllowed())
			PgArchPID = StartChildProcess(B_ARCHIVER);

		/* If we need to start a slot sync worker, try to do that now */
		MaybeStartSlotSyncWorker();

		/* If we need to signal the autovacuum launcher, do so now */
		if (avlauncher_needs_signal)
		{
			avlauncher_needs_signal = false;
			if (AutoVacPID != 0)
				kill(AutoVacPID, SIGUSR2);
		}

		/* If we need to start a WAL receiver, try to do that now */
		if (WalReceiverRequested)
			MaybeStartWalReceiver();

		/* If we need to start a WAL summarizer, try to do that now */
		MaybeStartWalSummarizer();

		/* Get other worker processes running, if needed */
		if (StartWorkerNeeded || HaveCrashedWorker)
			maybe_start_bgworkers();

#ifdef HAVE_PTHREAD_IS_THREADED_NP

		/*
		 * With assertions enabled, check regularly for appearance of
		 * additional threads.  All builds check at start and exit.
		 */
		Assert(pthread_is_threaded_np() == 0);
#endif

		/*
		 * Lastly, check to see if it's time to do some things that we don't
		 * want to do every single time through the loop, because they're a
		 * bit expensive.  Note that there's up to a minute of slop in when
		 * these tasks will be performed, since DetermineSleepTime() will let
		 * us sleep at most that long; except for SIGKILL timeout which has
		 * special-case logic there.
		 */
		now = time(NULL);

		/*
		 * If we already sent SIGQUIT to children and they are slow to shut
		 * down, it's time to send them SIGKILL (or SIGABRT if requested).
		 * This doesn't happen normally, but under certain conditions backends
		 * can get stuck while shutting down.  This is a last measure to get
		 * them unwedged.
		 *
		 * Note we also do this during recovery from a process crash.
		 */
		if ((Shutdown >= ImmediateShutdown || FatalError) &&
			AbortStartTime != 0 &&
			(now - AbortStartTime) >= SIGKILL_CHILDREN_AFTER_SECS)
		{
			/* We were gentle with them before. Not anymore */
			ereport(LOG,
			/* translator: %s is SIGKILL or SIGABRT */
					(errmsg("issuing %s to recalcitrant children",
							send_abort_for_kill ? "SIGABRT" : "SIGKILL")));
			TerminateChildren(send_abort_for_kill ? SIGABRT : SIGKILL);
			/* reset flag so we don't SIGKILL again */
			AbortStartTime = 0;
		}

		/*
		 * Once a minute, verify that postmaster.pid hasn't been removed or
		 * overwritten.  If it has, we force a shutdown.  This avoids having
		 * postmasters and child processes hanging around after their database
		 * is gone, and maybe causing problems if a new database cluster is
		 * created in the same place.  It also provides some protection
		 * against a DBA foolishly removing postmaster.pid and manually
		 * starting a new postmaster.  Data corruption is likely to ensue from
		 * that anyway, but we can minimize the damage by aborting ASAP.
		 */
		if (now - last_lockfile_recheck_time >= 1 * SECS_PER_MINUTE)
		{
			if (!RecheckDataDirLockFile())
			{
				ereport(LOG,
						(errmsg("performing immediate shutdown because data directory lock file is invalid")));
				kill(MyProcPid, SIGQUIT);
			}
			last_lockfile_recheck_time = now;
		}

		/*
		 * Touch Unix socket and lock files every 58 minutes, to ensure that
		 * they are not removed by overzealous /tmp-cleaning tasks.  We assume
		 * no one runs cleaners with cutoff times of less than an hour ...
		 */
		if (now - last_touch_time >= 58 * SECS_PER_MINUTE)
		{
			TouchSocketFiles();
			TouchSocketLockFiles();
			last_touch_time = now;
		}
	}
}

/*
 * The client has sent a cancel request packet, not a normal
 * start-a-new-connection packet.  Perform the necessary processing.
 * Nothing is sent back to the client.
 */
void
processCancelRequest(int backendPID, int32 cancelAuthCode)
{
	Backend    *bp;

#ifndef EXEC_BACKEND
	dlist_iter	iter;
#else
	int			i;
#endif

	/*
	 * See if we have a matching backend.  In the EXEC_BACKEND case, we can no
	 * longer access the postmaster's own backend list, and must rely on the
	 * duplicate array in shared memory.
	 */
#ifndef EXEC_BACKEND
	dlist_foreach(iter, &BackendList)
	{
		bp = dlist_container(Backend, elem, iter.cur);
#else
	for (i = MaxLivePostmasterChildren() - 1; i >= 0; i--)
	{
		bp = (Backend *) &ShmemBackendArray[i];
#endif
		if (bp->pid == backendPID)
		{
			if (bp->cancel_key == cancelAuthCode)
			{
				/* Found a match; signal that backend to cancel current op */
				ereport(DEBUG2,
						(errmsg_internal("processing cancel request: sending SIGINT to process %d",
										 backendPID)));
				signal_child(bp->pid, SIGINT);
			}
			else
				/* Right PID, wrong key: no way, Jose */
				ereport(LOG,
						(errmsg("wrong key in cancel request for process %d",
								backendPID)));
			return;
		}
#ifndef EXEC_BACKEND			/* make GNU Emacs 26.1 see brace balance */
	}
#else
	}
#endif

	/* No matching backend */
	ereport(LOG,
			(errmsg("PID %d in cancel request did not match any process",
					backendPID)));
}

/*
 * canAcceptConnections --- check to see if database state allows connections
 * of the specified type.  backend_type can be BACKEND_TYPE_NORMAL,
 * BACKEND_TYPE_AUTOVAC, or BACKEND_TYPE_BGWORKER.  (Note that we don't yet
 * know whether a NORMAL connection might turn into a walsender.)
 */
static CAC_state
canAcceptConnections(int backend_type)
{
	CAC_state	result = CAC_OK;

	/*
	 * Can't start backends when in startup/shutdown/inconsistent recovery
	 * state.  We treat autovac workers the same as user backends for this
	 * purpose.  However, bgworkers are excluded from this test; we expect
	 * bgworker_should_start_now() decided whether the DB state allows them.
	 */
	if (pmState != PM_RUN && pmState != PM_HOT_STANDBY &&
		backend_type != BACKEND_TYPE_BGWORKER)
	{
		if (Shutdown > NoShutdown)
			return CAC_SHUTDOWN;	/* shutdown is pending */
		else if (!FatalError && pmState == PM_STARTUP)
			return CAC_STARTUP; /* normal startup */
		else if (!FatalError && pmState == PM_RECOVERY)
			return CAC_NOTCONSISTENT;	/* not yet at consistent recovery
										 * state */
		else
			return CAC_RECOVERY;	/* else must be crash recovery */
	}

	/*
	 * "Smart shutdown" restrictions are applied only to normal connections,
	 * not to autovac workers or bgworkers.
	 */
	if (!connsAllowed && backend_type == BACKEND_TYPE_NORMAL)
		return CAC_SHUTDOWN;	/* shutdown is pending */

	/*
	 * Don't start too many children.
	 *
	 * We allow more connections here than we can have backends because some
	 * might still be authenticating; they might fail auth, or some existing
	 * backend might exit before the auth cycle is completed.  The exact
	 * MaxBackends limit is enforced when a new backend tries to join the
	 * shared-inval backend array.
	 *
	 * The limit here must match the sizes of the per-child-process arrays;
	 * see comments for MaxLivePostmasterChildren().
	 */
	if (CountChildren(BACKEND_TYPE_ALL) >= MaxLivePostmasterChildren())
		result = CAC_TOOMANY;

	return result;
}

/*
 * ClosePostmasterPorts -- close all the postmaster's open sockets
 *
 * This is called during child process startup to release file descriptors
 * that are not needed by that child process.  The postmaster still has
 * them open, of course.
 *
 * Note: we pass am_syslogger as a boolean because we don't want to set
 * the global variable yet when this is called.
 */
void
ClosePostmasterPorts(bool am_syslogger)
{
	/* Release resources held by the postmaster's WaitEventSet. */
	if (pm_wait_set)
	{
		FreeWaitEventSetAfterFork(pm_wait_set);
		pm_wait_set = NULL;
	}

#ifndef WIN32

	/*
	 * Close the write end of postmaster death watch pipe. It's important to
	 * do this as early as possible, so that if postmaster dies, others won't
	 * think that it's still running because we're holding the pipe open.
	 */
	if (close(postmaster_alive_fds[POSTMASTER_FD_OWN]) != 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg_internal("could not close postmaster death monitoring pipe in child process: %m")));
	postmaster_alive_fds[POSTMASTER_FD_OWN] = -1;
	/* Notify fd.c that we released one pipe FD. */
	ReleaseExternalFD();
#endif

	/*
	 * Close the postmaster's listen sockets.  These aren't tracked by fd.c,
	 * so we don't call ReleaseExternalFD() here.
	 *
	 * The listen sockets are marked as FD_CLOEXEC, so this isn't needed in
	 * EXEC_BACKEND mode.
	 */
#ifndef EXEC_BACKEND
	if (ListenSockets)
	{
		for (int i = 0; i < NumListenSockets; i++)
		{
			if (closesocket(ListenSockets[i]) != 0)
				elog(LOG, "could not close listen socket: %m");
		}
		pfree(ListenSockets);
	}
	NumListenSockets = 0;
	ListenSockets = NULL;
#endif

	/*
	 * If using syslogger, close the read side of the pipe.  We don't bother
	 * tracking this in fd.c, either.
	 */
	if (!am_syslogger)
	{
#ifndef WIN32
		if (syslogPipe[0] >= 0)
			close(syslogPipe[0]);
		syslogPipe[0] = -1;
#else
		if (syslogPipe[0])
			CloseHandle(syslogPipe[0]);
		syslogPipe[0] = 0;
#endif
	}

#ifdef USE_BONJOUR
	/* If using Bonjour, close the connection to the mDNS daemon */
	if (bonjour_sdref)
		close(DNSServiceRefSockFD(bonjour_sdref));
#endif
}


/*
 * InitProcessGlobals -- set MyStartTime[stamp], random seeds
 *
 * Called early in the postmaster and every backend.
 */
void
InitProcessGlobals(void) /// 记录启动时间，产生随机数
{
	/// 调用gettimeofday()获得时间信息
	MyStartTimestamp = GetCurrentTimestamp(); /// backend/utils/init/globals.c:TimestampTz MyStartTimestamp;
	MyStartTime = timestamptz_to_time_t(MyStartTimestamp);

	/*
	 * Set a different global seed in every process.  We want something
	 * unpredictable, so if possible, use high-quality random bits for the
	 * seed.  Otherwise, fall back to a seed based on timestamp and PID.
	 */
	if (unlikely(!pg_prng_strong_seed(&pg_global_prng_state)))
	{
		uint64		rseed;

		/*
		 * Since PIDs and timestamps tend to change more frequently in their
		 * least significant bits, shift the timestamp left to allow a larger
		 * total number of seeds in a given time period.  Since that would
		 * leave only 20 bits of the timestamp that cycle every ~1 second,
		 * also mix in some higher bits.
		 */
		rseed = ((uint64) MyProcPid) ^
			((uint64) MyStartTimestamp << 12) ^
			((uint64) MyStartTimestamp >> 20);

		pg_prng_seed(&pg_global_prng_state, rseed);
	}

	/*
	 * Also make sure that we've set a good seed for random(3).  Use of that
	 * is deprecated in core Postgres, but extensions might use it.
	 */
#ifndef WIN32
	srandom(pg_prng_uint32(&pg_global_prng_state));
#endif
}

/*
 * Child processes use SIGUSR1 to notify us of 'pmsignals'.  pg_ctl uses
 * SIGUSR1 to ask postmaster to check for logrotate and promote files.
 */
static void
handle_pm_pmsignal_signal(SIGNAL_ARGS)
{
	pending_pm_pmsignal = true;
	SetLatch(MyLatch);
}

/*
 * pg_ctl uses SIGHUP to request a reload of the configuration files.
 */
static void
handle_pm_reload_request_signal(SIGNAL_ARGS)
{
	pending_pm_reload_request = true;
	SetLatch(MyLatch);
}

/*
 * Re-read config files, and tell children to do same.
 */
static void
process_pm_reload_request(void)
{
	pending_pm_reload_request = false;

	ereport(DEBUG2,
			(errmsg_internal("postmaster received reload request signal")));

	if (Shutdown <= SmartShutdown)
	{
		ereport(LOG,
				(errmsg("received SIGHUP, reloading configuration files")));
		ProcessConfigFile(PGC_SIGHUP);
		SignalChildren(SIGHUP);
		if (StartupPID != 0)
			signal_child(StartupPID, SIGHUP);
		if (BgWriterPID != 0)
			signal_child(BgWriterPID, SIGHUP);
		if (CheckpointerPID != 0)
			signal_child(CheckpointerPID, SIGHUP);
		if (WalWriterPID != 0)
			signal_child(WalWriterPID, SIGHUP);
		if (WalReceiverPID != 0)
			signal_child(WalReceiverPID, SIGHUP);
		if (WalSummarizerPID != 0)
			signal_child(WalSummarizerPID, SIGHUP);
		if (AutoVacPID != 0)
			signal_child(AutoVacPID, SIGHUP);
		if (PgArchPID != 0)
			signal_child(PgArchPID, SIGHUP);
		if (SysLoggerPID != 0)
			signal_child(SysLoggerPID, SIGHUP);
		if (SlotSyncWorkerPID != 0)
			signal_child(SlotSyncWorkerPID, SIGHUP);

		/* Reload authentication config files too */
		if (!load_hba())
			ereport(LOG,
			/* translator: %s is a configuration file */
					(errmsg("%s was not reloaded", HbaFileName)));

		if (!load_ident())
			ereport(LOG,
					(errmsg("%s was not reloaded", IdentFileName)));

#ifdef USE_SSL
		/* Reload SSL configuration as well */
		if (EnableSSL)
		{
			if (secure_initialize(false) == 0)
				LoadedSSL = true;
			else
				ereport(LOG,
						(errmsg("SSL configuration was not reloaded")));
		}
		else
		{
			secure_destroy();
			LoadedSSL = false;
		}
#endif

#ifdef EXEC_BACKEND
		/* Update the starting-point file for future children */
		write_nondefault_variables(PGC_SIGHUP);
#endif
	}
}

/*
 * pg_ctl uses SIGTERM, SIGINT and SIGQUIT to request different types of
 * shutdown.
 */
static void
handle_pm_shutdown_request_signal(SIGNAL_ARGS)
{
	switch (postgres_signal_arg)
	{
		case SIGTERM:
			/* smart is implied if the other two flags aren't set */
			pending_pm_shutdown_request = true;
			break;
		case SIGINT:
			pending_pm_fast_shutdown_request = true;
			pending_pm_shutdown_request = true;
			break;
		case SIGQUIT:
			pending_pm_immediate_shutdown_request = true;
			pending_pm_shutdown_request = true;
			break;
	}
	SetLatch(MyLatch);
}

/*
 * Process shutdown request.
 */
static void
process_pm_shutdown_request(void)
{
	int			mode;

	ereport(DEBUG2,
			(errmsg_internal("postmaster received shutdown request signal")));

	pending_pm_shutdown_request = false;

	/*
	 * If more than one shutdown request signal arrived since the last server
	 * loop, take the one that is the most immediate.  That matches the
	 * priority that would apply if we processed them one by one in any order.
	 */
	if (pending_pm_immediate_shutdown_request)
	{
		pending_pm_immediate_shutdown_request = false;
		pending_pm_fast_shutdown_request = false;
		mode = ImmediateShutdown;
	}
	else if (pending_pm_fast_shutdown_request)
	{
		pending_pm_fast_shutdown_request = false;
		mode = FastShutdown;
	}
	else
		mode = SmartShutdown;

	switch (mode)
	{
		case SmartShutdown:

			/*
			 * Smart Shutdown:
			 *
			 * Wait for children to end their work, then shut down.
			 */
			if (Shutdown >= SmartShutdown)
				break;
			Shutdown = SmartShutdown;
			ereport(LOG,
					(errmsg("received smart shutdown request")));

			/* Report status */
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STOPPING);
#ifdef USE_SYSTEMD
			sd_notify(0, "STOPPING=1");
#endif

			/*
			 * If we reached normal running, we go straight to waiting for
			 * client backends to exit.  If already in PM_STOP_BACKENDS or a
			 * later state, do not change it.
			 */
			if (pmState == PM_RUN || pmState == PM_HOT_STANDBY)
				connsAllowed = false;
			else if (pmState == PM_STARTUP || pmState == PM_RECOVERY)
			{
				/* There should be no clients, so proceed to stop children */
				pmState = PM_STOP_BACKENDS;
			}

			/*
			 * Now wait for online backup mode to end and backends to exit. If
			 * that is already the case, PostmasterStateMachine will take the
			 * next step.
			 */
			PostmasterStateMachine();
			break;

		case FastShutdown:

			/*
			 * Fast Shutdown:
			 *
			 * Abort all children with SIGTERM (rollback active transactions
			 * and exit) and shut down when they are gone.
			 */
			if (Shutdown >= FastShutdown)
				break;
			Shutdown = FastShutdown;
			ereport(LOG,
					(errmsg("received fast shutdown request")));

			/* Report status */
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STOPPING);
#ifdef USE_SYSTEMD
			sd_notify(0, "STOPPING=1");
#endif

			if (pmState == PM_STARTUP || pmState == PM_RECOVERY)
			{
				/* Just shut down background processes silently */
				pmState = PM_STOP_BACKENDS;
			}
			else if (pmState == PM_RUN ||
					 pmState == PM_HOT_STANDBY)
			{
				/* Report that we're about to zap live client sessions */
				ereport(LOG,
						(errmsg("aborting any active transactions")));
				pmState = PM_STOP_BACKENDS;
			}

			/*
			 * PostmasterStateMachine will issue any necessary signals, or
			 * take the next step if no child processes need to be killed.
			 */
			PostmasterStateMachine();
			break;

		case ImmediateShutdown:

			/*
			 * Immediate Shutdown:
			 *
			 * abort all children with SIGQUIT, wait for them to exit,
			 * terminate remaining ones with SIGKILL, then exit without
			 * attempt to properly shut down the data base system.
			 */
			if (Shutdown >= ImmediateShutdown)
				break;
			Shutdown = ImmediateShutdown;
			ereport(LOG,
					(errmsg("received immediate shutdown request")));

			/* Report status */
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STOPPING);
#ifdef USE_SYSTEMD
			sd_notify(0, "STOPPING=1");
#endif

			/* tell children to shut down ASAP */
			/* (note we don't apply send_abort_for_crash here) */
			SetQuitSignalReason(PMQUIT_FOR_STOP);
			TerminateChildren(SIGQUIT);
			pmState = PM_WAIT_BACKENDS;

			/* set stopwatch for them to die */
			AbortStartTime = time(NULL);

			/*
			 * Now wait for backends to exit.  If there are none,
			 * PostmasterStateMachine will take the next step.
			 */
			PostmasterStateMachine();
			break;
	}
}

static void
handle_pm_child_exit_signal(SIGNAL_ARGS)
{
	pending_pm_child_exit = true;
	SetLatch(MyLatch);
}

/*
 * Cleanup after a child process dies.
 */
static void
process_pm_child_exit(void)
{
	int			pid;			/* process id of dead child process */
	int			exitstatus;		/* its exit status */

	pending_pm_child_exit = false;

	ereport(DEBUG4,
			(errmsg_internal("reaping dead processes")));

	while ((pid = waitpid(-1, &exitstatus, WNOHANG)) > 0)
	{
		/*
		 * Check if this child was a startup process.
		 */
		if (pid == StartupPID)
		{
			StartupPID = 0;

			/*
			 * Startup process exited in response to a shutdown request (or it
			 * completed normally regardless of the shutdown request).
			 */
			if (Shutdown > NoShutdown &&
				(EXIT_STATUS_0(exitstatus) || EXIT_STATUS_1(exitstatus)))
			{
				StartupStatus = STARTUP_NOT_RUNNING;
				pmState = PM_WAIT_BACKENDS;
				/* PostmasterStateMachine logic does the rest */
				continue;
			}

			if (EXIT_STATUS_3(exitstatus))
			{
				ereport(LOG,
						(errmsg("shutdown at recovery target")));
				StartupStatus = STARTUP_NOT_RUNNING;
				Shutdown = Max(Shutdown, SmartShutdown);
				TerminateChildren(SIGTERM);
				pmState = PM_WAIT_BACKENDS;
				/* PostmasterStateMachine logic does the rest */
				continue;
			}

			/*
			 * Unexpected exit of startup process (including FATAL exit)
			 * during PM_STARTUP is treated as catastrophic. There are no
			 * other processes running yet, so we can just exit.
			 */
			if (pmState == PM_STARTUP &&
				StartupStatus != STARTUP_SIGNALED &&
				!EXIT_STATUS_0(exitstatus))
			{
				LogChildExit(LOG, _("startup process"),
							 pid, exitstatus);
				ereport(LOG,
						(errmsg("aborting startup due to startup process failure")));
				ExitPostmaster(1);
			}

			/*
			 * After PM_STARTUP, any unexpected exit (including FATAL exit) of
			 * the startup process is catastrophic, so kill other children,
			 * and set StartupStatus so we don't try to reinitialize after
			 * they're gone.  Exception: if StartupStatus is STARTUP_SIGNALED,
			 * then we previously sent the startup process a SIGQUIT; so
			 * that's probably the reason it died, and we do want to try to
			 * restart in that case.
			 *
			 * This stanza also handles the case where we sent a SIGQUIT
			 * during PM_STARTUP due to some dead_end child crashing: in that
			 * situation, if the startup process dies on the SIGQUIT, we need
			 * to transition to PM_WAIT_BACKENDS state which will allow
			 * PostmasterStateMachine to restart the startup process.  (On the
			 * other hand, the startup process might complete normally, if we
			 * were too late with the SIGQUIT.  In that case we'll fall
			 * through and commence normal operations.)
			 */
			if (!EXIT_STATUS_0(exitstatus))
			{
				if (StartupStatus == STARTUP_SIGNALED)
				{
					StartupStatus = STARTUP_NOT_RUNNING;
					if (pmState == PM_STARTUP)
						pmState = PM_WAIT_BACKENDS;
				}
				else
					StartupStatus = STARTUP_CRASHED;
				HandleChildCrash(pid, exitstatus,
								 _("startup process"));
				continue;
			}

			/*
			 * Startup succeeded, commence normal operations
			 */
			StartupStatus = STARTUP_NOT_RUNNING;
			FatalError = false;
			AbortStartTime = 0;
			ReachedNormalRunning = true;
			pmState = PM_RUN;
			connsAllowed = true;

			/*
			 * Crank up the background tasks, if we didn't do that already
			 * when we entered consistent recovery state.  It doesn't matter
			 * if this fails, we'll just try again later.
			 */
			if (CheckpointerPID == 0)
				CheckpointerPID = StartChildProcess(B_CHECKPOINTER);
			if (BgWriterPID == 0)
				BgWriterPID = StartChildProcess(B_BG_WRITER);
			if (WalWriterPID == 0)
				WalWriterPID = StartChildProcess(B_WAL_WRITER);
			MaybeStartWalSummarizer();

			/*
			 * Likewise, start other special children as needed.  In a restart
			 * situation, some of them may be alive already.
			 */
			if (!IsBinaryUpgrade && AutoVacuumingActive() && AutoVacPID == 0)
				AutoVacPID = StartChildProcess(B_AUTOVAC_LAUNCHER);
			if (PgArchStartupAllowed() && PgArchPID == 0)
				PgArchPID = StartChildProcess(B_ARCHIVER);
			MaybeStartSlotSyncWorker();

			/* workers may be scheduled to start now */
			maybe_start_bgworkers();

			/* at this point we are really open for business */
			ereport(LOG,
					(errmsg("database system is ready to accept connections")));

			/* Report status */
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_READY);
#ifdef USE_SYSTEMD
			sd_notify(0, "READY=1");
#endif

			continue;
		}

		/*
		 * Was it the bgwriter?  Normal exit can be ignored; we'll start a new
		 * one at the next iteration of the postmaster's main loop, if
		 * necessary.  Any other exit condition is treated as a crash.
		 */
		if (pid == BgWriterPID)
		{
			BgWriterPID = 0;
			if (!EXIT_STATUS_0(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("background writer process"));
			continue;
		}

		/*
		 * Was it the checkpointer?
		 */
		if (pid == CheckpointerPID)
		{
			CheckpointerPID = 0;
			if (EXIT_STATUS_0(exitstatus) && pmState == PM_SHUTDOWN)
			{
				/*
				 * OK, we saw normal exit of the checkpointer after it's been
				 * told to shut down.  We expect that it wrote a shutdown
				 * checkpoint.  (If for some reason it didn't, recovery will
				 * occur on next postmaster start.)
				 *
				 * At this point we should have no normal backend children
				 * left (else we'd not be in PM_SHUTDOWN state) but we might
				 * have dead_end children to wait for.
				 *
				 * If we have an archiver subprocess, tell it to do a last
				 * archive cycle and quit. Likewise, if we have walsender
				 * processes, tell them to send any remaining WAL and quit.
				 */
				Assert(Shutdown > NoShutdown);

				/* Waken archiver for the last time */
				if (PgArchPID != 0)
					signal_child(PgArchPID, SIGUSR2);

				/*
				 * Waken walsenders for the last time. No regular backends
				 * should be around anymore.
				 */
				SignalChildren(SIGUSR2);

				pmState = PM_SHUTDOWN_2;
			}
			else
			{
				/*
				 * Any unexpected exit of the checkpointer (including FATAL
				 * exit) is treated as a crash.
				 */
				HandleChildCrash(pid, exitstatus,
								 _("checkpointer process"));
			}

			continue;
		}

		/*
		 * Was it the wal writer?  Normal exit can be ignored; we'll start a
		 * new one at the next iteration of the postmaster's main loop, if
		 * necessary.  Any other exit condition is treated as a crash.
		 */
		if (pid == WalWriterPID)
		{
			WalWriterPID = 0;
			if (!EXIT_STATUS_0(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("WAL writer process"));
			continue;
		}

		/*
		 * Was it the wal receiver?  If exit status is zero (normal) or one
		 * (FATAL exit), we assume everything is all right just like normal
		 * backends.  (If we need a new wal receiver, we'll start one at the
		 * next iteration of the postmaster's main loop.)
		 */
		if (pid == WalReceiverPID)
		{
			WalReceiverPID = 0;
			if (!EXIT_STATUS_0(exitstatus) && !EXIT_STATUS_1(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("WAL receiver process"));
			continue;
		}

		/*
		 * Was it the wal summarizer? Normal exit can be ignored; we'll start
		 * a new one at the next iteration of the postmaster's main loop, if
		 * necessary.  Any other exit condition is treated as a crash.
		 */
		if (pid == WalSummarizerPID)
		{
			WalSummarizerPID = 0;
			if (!EXIT_STATUS_0(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("WAL summarizer process"));
			continue;
		}

		/*
		 * Was it the autovacuum launcher?	Normal exit can be ignored; we'll
		 * start a new one at the next iteration of the postmaster's main
		 * loop, if necessary.  Any other exit condition is treated as a
		 * crash.
		 */
		if (pid == AutoVacPID)
		{
			AutoVacPID = 0;
			if (!EXIT_STATUS_0(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("autovacuum launcher process"));
			continue;
		}

		/*
		 * Was it the archiver?  If exit status is zero (normal) or one (FATAL
		 * exit), we assume everything is all right just like normal backends
		 * and just try to restart a new one so that we immediately retry
		 * archiving remaining files. (If fail, we'll try again in future
		 * cycles of the postmaster's main loop.) Unless we were waiting for
		 * it to shut down; don't restart it in that case, and
		 * PostmasterStateMachine() will advance to the next shutdown step.
		 */
		if (pid == PgArchPID)
		{
			PgArchPID = 0;
			if (!EXIT_STATUS_0(exitstatus) && !EXIT_STATUS_1(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("archiver process"));
			if (PgArchStartupAllowed())
				PgArchPID = StartChildProcess(B_ARCHIVER);
			continue;
		}

		/* Was it the system logger?  If so, try to start a new one */
		if (pid == SysLoggerPID)
		{
			SysLoggerPID = 0;
			/* for safety's sake, launch new logger *first* */
			SysLoggerPID = SysLogger_Start();
			if (!EXIT_STATUS_0(exitstatus))
				LogChildExit(LOG, _("system logger process"),
							 pid, exitstatus);
			continue;
		}

		/*
		 * Was it the slot sync worker? Normal exit or FATAL exit can be
		 * ignored (FATAL can be caused by libpqwalreceiver on receiving
		 * shutdown request by the startup process during promotion); we'll
		 * start a new one at the next iteration of the postmaster's main
		 * loop, if necessary. Any other exit condition is treated as a crash.
		 */
		if (pid == SlotSyncWorkerPID)
		{
			SlotSyncWorkerPID = 0;
			if (!EXIT_STATUS_0(exitstatus) && !EXIT_STATUS_1(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("slot sync worker process"));
			continue;
		}

		/* Was it one of our background workers? */
		if (CleanupBackgroundWorker(pid, exitstatus))
		{
			/* have it be restarted */
			HaveCrashedWorker = true;
			continue;
		}

		/*
		 * Else do standard backend child cleanup.
		 */
		CleanupBackend(pid, exitstatus);
	}							/* loop over pending child-death reports */

	/*
	 * After cleaning out the SIGCHLD queue, see if we have any state changes
	 * or actions to make.
	 */
	PostmasterStateMachine();
}

/*
 * Scan the bgworkers list and see if the given PID (which has just stopped
 * or crashed) is in it.  Handle its shutdown if so, and return true.  If not a
 * bgworker, return false.
 *
 * This is heavily based on CleanupBackend.  One important difference is that
 * we don't know yet that the dying process is a bgworker, so we must be silent
 * until we're sure it is.
 */
static bool
CleanupBackgroundWorker(int pid,
						int exitstatus) /* child's exit status */
{
	char		namebuf[MAXPGPATH];
	slist_mutable_iter iter;

	slist_foreach_modify(iter, &BackgroundWorkerList)
	{
		RegisteredBgWorker *rw;

		rw = slist_container(RegisteredBgWorker, rw_lnode, iter.cur);

		if (rw->rw_pid != pid)
			continue;

#ifdef WIN32
		/* see CleanupBackend */
		if (exitstatus == ERROR_WAIT_NO_CHILDREN)
			exitstatus = 0;
#endif

		snprintf(namebuf, MAXPGPATH, _("background worker \"%s\""),
				 rw->rw_worker.bgw_type);


		if (!EXIT_STATUS_0(exitstatus))
		{
			/* Record timestamp, so we know when to restart the worker. */
			rw->rw_crashed_at = GetCurrentTimestamp();
		}
		else
		{
			/* Zero exit status means terminate */
			rw->rw_crashed_at = 0;
			rw->rw_terminate = true;
		}

		/*
		 * Additionally, just like a backend, any exit status other than 0 or
		 * 1 is considered a crash and causes a system-wide restart.
		 */
		if (!EXIT_STATUS_0(exitstatus) && !EXIT_STATUS_1(exitstatus))
		{
			HandleChildCrash(pid, exitstatus, namebuf);
			return true;
		}

		/*
		 * We must release the postmaster child slot. If the worker failed to
		 * do so, it did not clean up after itself, requiring a crash-restart
		 * cycle.
		 */
		if (!ReleasePostmasterChildSlot(rw->rw_child_slot))
		{
			HandleChildCrash(pid, exitstatus, namebuf);
			return true;
		}

		/* Get it out of the BackendList and clear out remaining data */
		dlist_delete(&rw->rw_backend->elem);
#ifdef EXEC_BACKEND
		ShmemBackendArrayRemove(rw->rw_backend);
#endif

		/*
		 * It's possible that this background worker started some OTHER
		 * background worker and asked to be notified when that worker started
		 * or stopped.  If so, cancel any notifications destined for the
		 * now-dead backend.
		 */
		if (rw->rw_backend->bgworker_notify)
			BackgroundWorkerStopNotifications(rw->rw_pid);
		pfree(rw->rw_backend);
		rw->rw_backend = NULL;
		rw->rw_pid = 0;
		rw->rw_child_slot = 0;
		ReportBackgroundWorkerExit(&iter);	/* report child death */

		LogChildExit(EXIT_STATUS_0(exitstatus) ? DEBUG1 : LOG,
					 namebuf, pid, exitstatus);

		return true;
	}

	return false;
}

/*
 * CleanupBackend -- cleanup after terminated backend.
 *
 * Remove all local state associated with backend.
 *
 * If you change this, see also CleanupBackgroundWorker.
 */
static void
CleanupBackend(int pid,
			   int exitstatus)	/* child's exit status. */
{
	dlist_mutable_iter iter;

	LogChildExit(DEBUG2, _("server process"), pid, exitstatus);

	/*
	 * If a backend dies in an ugly way then we must signal all other backends
	 * to quickdie.  If exit status is zero (normal) or one (FATAL exit), we
	 * assume everything is all right and proceed to remove the backend from
	 * the active backend list.
	 */

#ifdef WIN32

	/*
	 * On win32, also treat ERROR_WAIT_NO_CHILDREN (128) as nonfatal case,
	 * since that sometimes happens under load when the process fails to start
	 * properly (long before it starts using shared memory). Microsoft reports
	 * it is related to mutex failure:
	 * http://archives.postgresql.org/pgsql-hackers/2010-09/msg00790.php
	 */
	if (exitstatus == ERROR_WAIT_NO_CHILDREN)
	{
		LogChildExit(LOG, _("server process"), pid, exitstatus);
		exitstatus = 0;
	}
#endif

	if (!EXIT_STATUS_0(exitstatus) && !EXIT_STATUS_1(exitstatus))
	{
		HandleChildCrash(pid, exitstatus, _("server process"));
		return;
	}

	dlist_foreach_modify(iter, &BackendList)
	{
		Backend    *bp = dlist_container(Backend, elem, iter.cur);

		if (bp->pid == pid)
		{
			if (!bp->dead_end)
			{
				if (!ReleasePostmasterChildSlot(bp->child_slot))
				{
					/*
					 * Uh-oh, the child failed to clean itself up.  Treat as a
					 * crash after all.
					 */
					HandleChildCrash(pid, exitstatus, _("server process"));
					return;
				}
#ifdef EXEC_BACKEND
				ShmemBackendArrayRemove(bp);
#endif
			}
			if (bp->bgworker_notify)
			{
				/*
				 * This backend may have been slated to receive SIGUSR1 when
				 * some background worker started or stopped.  Cancel those
				 * notifications, as we don't want to signal PIDs that are not
				 * PostgreSQL backends.  This gets skipped in the (probably
				 * very common) case where the backend has never requested any
				 * such notifications.
				 */
				BackgroundWorkerStopNotifications(bp->pid);
			}
			dlist_delete(iter.cur);
			pfree(bp);
			break;
		}
	}
}

/*
 * HandleChildCrash -- cleanup after failed backend, bgwriter, checkpointer,
 * walwriter, autovacuum, archiver, slot sync worker, or background worker.
 *
 * The objectives here are to clean up our local state about the child
 * process, and to signal all other remaining children to quickdie.
 */
static void
HandleChildCrash(int pid, int exitstatus, const char *procname)
{
	dlist_mutable_iter iter;
	slist_iter	siter;
	Backend    *bp;
	bool		take_action;

	/*
	 * We only log messages and send signals if this is the first process
	 * crash and we're not doing an immediate shutdown; otherwise, we're only
	 * here to update postmaster's idea of live processes.  If we have already
	 * signaled children, nonzero exit status is to be expected, so don't
	 * clutter log.
	 */
	take_action = !FatalError && Shutdown != ImmediateShutdown;

	if (take_action)
	{
		LogChildExit(LOG, procname, pid, exitstatus);
		ereport(LOG,
				(errmsg("terminating any other active server processes")));
		SetQuitSignalReason(PMQUIT_FOR_CRASH);
	}

	/* Process background workers. */
	slist_foreach(siter, &BackgroundWorkerList)
	{
		RegisteredBgWorker *rw;

		rw = slist_container(RegisteredBgWorker, rw_lnode, siter.cur);
		if (rw->rw_pid == 0)
			continue;			/* not running */
		if (rw->rw_pid == pid)
		{
			/*
			 * Found entry for freshly-dead worker, so remove it.
			 */
			(void) ReleasePostmasterChildSlot(rw->rw_child_slot);
			dlist_delete(&rw->rw_backend->elem);
#ifdef EXEC_BACKEND
			ShmemBackendArrayRemove(rw->rw_backend);
#endif
			pfree(rw->rw_backend);
			rw->rw_backend = NULL;
			rw->rw_pid = 0;
			rw->rw_child_slot = 0;
			/* don't reset crashed_at */
			/* don't report child stop, either */
			/* Keep looping so we can signal remaining workers */
		}
		else
		{
			/*
			 * This worker is still alive.  Unless we did so already, tell it
			 * to commit hara-kiri.
			 */
			if (take_action)
				sigquit_child(rw->rw_pid);
		}
	}

	/* Process regular backends */
	dlist_foreach_modify(iter, &BackendList)
	{
		bp = dlist_container(Backend, elem, iter.cur);

		if (bp->pid == pid)
		{
			/*
			 * Found entry for freshly-dead backend, so remove it.
			 */
			if (!bp->dead_end)
			{
				(void) ReleasePostmasterChildSlot(bp->child_slot);
#ifdef EXEC_BACKEND
				ShmemBackendArrayRemove(bp);
#endif
			}
			dlist_delete(iter.cur);
			pfree(bp);
			/* Keep looping so we can signal remaining backends */
		}
		else
		{
			/*
			 * This backend is still alive.  Unless we did so already, tell it
			 * to commit hara-kiri.
			 *
			 * We could exclude dead_end children here, but at least when
			 * sending SIGABRT it seems better to include them.
			 *
			 * Background workers were already processed above; ignore them
			 * here.
			 */
			if (bp->bkend_type == BACKEND_TYPE_BGWORKER)
				continue;

			if (take_action)
				sigquit_child(bp->pid);
		}
	}

	/* Take care of the startup process too */
	if (pid == StartupPID)
	{
		StartupPID = 0;
		/* Caller adjusts StartupStatus, so don't touch it here */
	}
	else if (StartupPID != 0 && take_action)
	{
		sigquit_child(StartupPID);
		StartupStatus = STARTUP_SIGNALED;
	}

	/* Take care of the bgwriter too */
	if (pid == BgWriterPID)
		BgWriterPID = 0;
	else if (BgWriterPID != 0 && take_action)
		sigquit_child(BgWriterPID);

	/* Take care of the checkpointer too */
	if (pid == CheckpointerPID)
		CheckpointerPID = 0;
	else if (CheckpointerPID != 0 && take_action)
		sigquit_child(CheckpointerPID);

	/* Take care of the walwriter too */
	if (pid == WalWriterPID)
		WalWriterPID = 0;
	else if (WalWriterPID != 0 && take_action)
		sigquit_child(WalWriterPID);

	/* Take care of the walreceiver too */
	if (pid == WalReceiverPID)
		WalReceiverPID = 0;
	else if (WalReceiverPID != 0 && take_action)
		sigquit_child(WalReceiverPID);

	/* Take care of the walsummarizer too */
	if (pid == WalSummarizerPID)
		WalSummarizerPID = 0;
	else if (WalSummarizerPID != 0 && take_action)
		sigquit_child(WalSummarizerPID);

	/* Take care of the autovacuum launcher too */
	if (pid == AutoVacPID)
		AutoVacPID = 0;
	else if (AutoVacPID != 0 && take_action)
		sigquit_child(AutoVacPID);

	/* Take care of the archiver too */
	if (pid == PgArchPID)
		PgArchPID = 0;
	else if (PgArchPID != 0 && take_action)
		sigquit_child(PgArchPID);

	/* Take care of the slot sync worker too */
	if (pid == SlotSyncWorkerPID)
		SlotSyncWorkerPID = 0;
	else if (SlotSyncWorkerPID != 0 && take_action)
		sigquit_child(SlotSyncWorkerPID);

	/* We do NOT restart the syslogger */

	if (Shutdown != ImmediateShutdown)
		FatalError = true;

	/* We now transit into a state of waiting for children to die */
	if (pmState == PM_RECOVERY ||
		pmState == PM_HOT_STANDBY ||
		pmState == PM_RUN ||
		pmState == PM_STOP_BACKENDS ||
		pmState == PM_SHUTDOWN)
		pmState = PM_WAIT_BACKENDS;

	/*
	 * .. and if this doesn't happen quickly enough, now the clock is ticking
	 * for us to kill them without mercy.
	 */
	if (AbortStartTime == 0)
		AbortStartTime = time(NULL);
}

/*
 * Log the death of a child process.
 */
static void
LogChildExit(int lev, const char *procname, int pid, int exitstatus)
{
	/*
	 * size of activity_buffer is arbitrary, but set equal to default
	 * track_activity_query_size
	 */
	char		activity_buffer[1024];
	const char *activity = NULL;

	if (!EXIT_STATUS_0(exitstatus))
		activity = pgstat_get_crashed_backend_activity(pid,
													   activity_buffer,
													   sizeof(activity_buffer));

	if (WIFEXITED(exitstatus))
		ereport(lev,

		/*------
		  translator: %s is a noun phrase describing a child process, such as
		  "server process" */
				(errmsg("%s (PID %d) exited with exit code %d",
						procname, pid, WEXITSTATUS(exitstatus)),
				 activity ? errdetail("Failed process was running: %s", activity) : 0));
	else if (WIFSIGNALED(exitstatus))
	{
#if defined(WIN32)
		ereport(lev,

		/*------
		  translator: %s is a noun phrase describing a child process, such as
		  "server process" */
				(errmsg("%s (PID %d) was terminated by exception 0x%X",
						procname, pid, WTERMSIG(exitstatus)),
				 errhint("See C include file \"ntstatus.h\" for a description of the hexadecimal value."),
				 activity ? errdetail("Failed process was running: %s", activity) : 0));
#else
		ereport(lev,

		/*------
		  translator: %s is a noun phrase describing a child process, such as
		  "server process" */
				(errmsg("%s (PID %d) was terminated by signal %d: %s",
						procname, pid, WTERMSIG(exitstatus),
						pg_strsignal(WTERMSIG(exitstatus))),
				 activity ? errdetail("Failed process was running: %s", activity) : 0));
#endif
	}
	else
		ereport(lev,

		/*------
		  translator: %s is a noun phrase describing a child process, such as
		  "server process" */
				(errmsg("%s (PID %d) exited with unrecognized status %d",
						procname, pid, exitstatus),
				 activity ? errdetail("Failed process was running: %s", activity) : 0));
}

/*
 * Advance the postmaster's state machine and take actions as appropriate
 *
 * This is common code for process_pm_shutdown_request(),
 * process_pm_child_exit() and process_pm_pmsignal(), which process the signals
 * that might mean we need to change state.
 */
static void
PostmasterStateMachine(void)
{
	/* If we're doing a smart shutdown, try to advance that state. */
	if (pmState == PM_RUN || pmState == PM_HOT_STANDBY)
	{
		if (!connsAllowed)
		{
			/*
			 * This state ends when we have no normal client backends running.
			 * Then we're ready to stop other children.
			 */
			if (CountChildren(BACKEND_TYPE_NORMAL) == 0)
				pmState = PM_STOP_BACKENDS;
		}
	}

	/*
	 * If we're ready to do so, signal child processes to shut down.  (This
	 * isn't a persistent state, but treating it as a distinct pmState allows
	 * us to share this code across multiple shutdown code paths.)
	 */
	if (pmState == PM_STOP_BACKENDS)
	{
		/*
		 * Forget any pending requests for background workers, since we're no
		 * longer willing to launch any new workers.  (If additional requests
		 * arrive, BackgroundWorkerStateChange will reject them.)
		 */
		ForgetUnstartedBackgroundWorkers();

		/* Signal all backend children except walsenders */
		SignalSomeChildren(SIGTERM,
						   BACKEND_TYPE_ALL - BACKEND_TYPE_WALSND);
		/* and the autovac launcher too */
		if (AutoVacPID != 0)
			signal_child(AutoVacPID, SIGTERM);
		/* and the bgwriter too */
		if (BgWriterPID != 0)
			signal_child(BgWriterPID, SIGTERM);
		/* and the walwriter too */
		if (WalWriterPID != 0)
			signal_child(WalWriterPID, SIGTERM);
		/* If we're in recovery, also stop startup and walreceiver procs */
		if (StartupPID != 0)
			signal_child(StartupPID, SIGTERM);
		if (WalReceiverPID != 0)
			signal_child(WalReceiverPID, SIGTERM);
		if (WalSummarizerPID != 0)
			signal_child(WalSummarizerPID, SIGTERM);
		if (SlotSyncWorkerPID != 0)
			signal_child(SlotSyncWorkerPID, SIGTERM);
		/* checkpointer, archiver, stats, and syslogger may continue for now */

		/* Now transition to PM_WAIT_BACKENDS state to wait for them to die */
		pmState = PM_WAIT_BACKENDS;
	}

	/*
	 * If we are in a state-machine state that implies waiting for backends to
	 * exit, see if they're all gone, and change state if so.
	 */
	if (pmState == PM_WAIT_BACKENDS)
	{
		/*
		 * PM_WAIT_BACKENDS state ends when we have no regular backends
		 * (including autovac workers), no bgworkers (including unconnected
		 * ones), and no walwriter, autovac launcher, bgwriter or slot sync
		 * worker.  If we are doing crash recovery or an immediate shutdown
		 * then we expect the checkpointer to exit as well, otherwise not. The
		 * stats and syslogger processes are disregarded since they are not
		 * connected to shared memory; we also disregard dead_end children
		 * here. Walsenders and archiver are also disregarded, they will be
		 * terminated later after writing the checkpoint record.
		 */
		if (CountChildren(BACKEND_TYPE_ALL - BACKEND_TYPE_WALSND) == 0 &&
			StartupPID == 0 &&
			WalReceiverPID == 0 &&
			WalSummarizerPID == 0 &&
			BgWriterPID == 0 &&
			(CheckpointerPID == 0 ||
			 (!FatalError && Shutdown < ImmediateShutdown)) &&
			WalWriterPID == 0 &&
			AutoVacPID == 0 &&
			SlotSyncWorkerPID == 0)
		{
			if (Shutdown >= ImmediateShutdown || FatalError)
			{
				/*
				 * Start waiting for dead_end children to die.  This state
				 * change causes ServerLoop to stop creating new ones.
				 */
				pmState = PM_WAIT_DEAD_END;

				/*
				 * We already SIGQUIT'd the archiver and stats processes, if
				 * any, when we started immediate shutdown or entered
				 * FatalError state.
				 */
			}
			else
			{
				/*
				 * If we get here, we are proceeding with normal shutdown. All
				 * the regular children are gone, and it's time to tell the
				 * checkpointer to do a shutdown checkpoint.
				 */
				Assert(Shutdown > NoShutdown);
				/* Start the checkpointer if not running */
				if (CheckpointerPID == 0)
					CheckpointerPID = StartChildProcess(B_CHECKPOINTER);
				/* And tell it to shut down */
				if (CheckpointerPID != 0)
				{
					signal_child(CheckpointerPID, SIGUSR2);
					pmState = PM_SHUTDOWN;
				}
				else
				{
					/*
					 * If we failed to fork a checkpointer, just shut down.
					 * Any required cleanup will happen at next restart. We
					 * set FatalError so that an "abnormal shutdown" message
					 * gets logged when we exit.
					 *
					 * We don't consult send_abort_for_crash here, as it's
					 * unlikely that dumping cores would illuminate the reason
					 * for checkpointer fork failure.
					 */
					FatalError = true;
					pmState = PM_WAIT_DEAD_END;

					/* Kill the walsenders and archiver too */
					SignalChildren(SIGQUIT);
					if (PgArchPID != 0)
						signal_child(PgArchPID, SIGQUIT);
				}
			}
		}
	}

	if (pmState == PM_SHUTDOWN_2)
	{
		/*
		 * PM_SHUTDOWN_2 state ends when there's no other children than
		 * dead_end children left. There shouldn't be any regular backends
		 * left by now anyway; what we're really waiting for is walsenders and
		 * archiver.
		 */
		if (PgArchPID == 0 && CountChildren(BACKEND_TYPE_ALL) == 0)
		{
			pmState = PM_WAIT_DEAD_END;
		}
	}

	if (pmState == PM_WAIT_DEAD_END)
	{
		/* Don't allow any new socket connection events. */
		ConfigurePostmasterWaitSet(false);

		/*
		 * PM_WAIT_DEAD_END state ends when the BackendList is entirely empty
		 * (ie, no dead_end children remain), and the archiver is gone too.
		 *
		 * The reason we wait for those two is to protect them against a new
		 * postmaster starting conflicting subprocesses; this isn't an
		 * ironclad protection, but it at least helps in the
		 * shutdown-and-immediately-restart scenario.  Note that they have
		 * already been sent appropriate shutdown signals, either during a
		 * normal state transition leading up to PM_WAIT_DEAD_END, or during
		 * FatalError processing.
		 */
		if (dlist_is_empty(&BackendList) && PgArchPID == 0)
		{
			/* These other guys should be dead already */
			Assert(StartupPID == 0);
			Assert(WalReceiverPID == 0);
			Assert(WalSummarizerPID == 0);
			Assert(BgWriterPID == 0);
			Assert(CheckpointerPID == 0);
			Assert(WalWriterPID == 0);
			Assert(AutoVacPID == 0);
			Assert(SlotSyncWorkerPID == 0);
			/* syslogger is not considered here */
			pmState = PM_NO_CHILDREN;
		}
	}

	/*
	 * If we've been told to shut down, we exit as soon as there are no
	 * remaining children.  If there was a crash, cleanup will occur at the
	 * next startup.  (Before PostgreSQL 8.3, we tried to recover from the
	 * crash before exiting, but that seems unwise if we are quitting because
	 * we got SIGTERM from init --- there may well not be time for recovery
	 * before init decides to SIGKILL us.)
	 *
	 * Note that the syslogger continues to run.  It will exit when it sees
	 * EOF on its input pipe, which happens when there are no more upstream
	 * processes.
	 */
	if (Shutdown > NoShutdown && pmState == PM_NO_CHILDREN)
	{
		if (FatalError)
		{
			ereport(LOG, (errmsg("abnormal database system shutdown")));
			ExitPostmaster(1);
		}
		else
		{
			/*
			 * Normal exit from the postmaster is here.  We don't need to log
			 * anything here, since the UnlinkLockFiles proc_exit callback
			 * will do so, and that should be the last user-visible action.
			 */
			ExitPostmaster(0);
		}
	}

	/*
	 * If the startup process failed, or the user does not want an automatic
	 * restart after backend crashes, wait for all non-syslogger children to
	 * exit, and then exit postmaster.  We don't try to reinitialize when the
	 * startup process fails, because more than likely it will just fail again
	 * and we will keep trying forever.
	 */
	if (pmState == PM_NO_CHILDREN)
	{
		if (StartupStatus == STARTUP_CRASHED)
		{
			ereport(LOG,
					(errmsg("shutting down due to startup process failure")));
			ExitPostmaster(1);
		}
		if (!restart_after_crash)
		{
			ereport(LOG,
					(errmsg("shutting down because \"restart_after_crash\" is off")));
			ExitPostmaster(1);
		}
	}

	/*
	 * If we need to recover from a crash, wait for all non-syslogger children
	 * to exit, then reset shmem and start the startup process.
	 */
	if (FatalError && pmState == PM_NO_CHILDREN)
	{
		ereport(LOG,
				(errmsg("all server processes terminated; reinitializing")));

		/* remove leftover temporary files after a crash */
		if (remove_temp_files_after_crash)
			RemovePgTempFiles();

		/* allow background workers to immediately restart */
		ResetBackgroundWorkerCrashTimes();

		shmem_exit(1);

		/* re-read control file into local memory */
		LocalProcessControlFile(true);

		/* re-create shared memory and semaphores */
		CreateSharedMemoryAndSemaphores();

		StartupPID = StartChildProcess(B_STARTUP);
		Assert(StartupPID != 0);
		StartupStatus = STARTUP_RUNNING;
		pmState = PM_STARTUP;
		/* crash recovery started, reset SIGKILL flag */
		AbortStartTime = 0;

		/* start accepting server socket connection events again */
		ConfigurePostmasterWaitSet(true);
	}
}


/*
 * Send a signal to a postmaster child process
 *
 * On systems that have setsid(), each child process sets itself up as a
 * process group leader.  For signals that are generally interpreted in the
 * appropriate fashion, we signal the entire process group not just the
 * direct child process.  This allows us to, for example, SIGQUIT a blocked
 * archive_recovery script, or SIGINT a script being run by a backend via
 * system().
 *
 * There is a race condition for recently-forked children: they might not
 * have executed setsid() yet.  So we signal the child directly as well as
 * the group.  We assume such a child will handle the signal before trying
 * to spawn any grandchild processes.  We also assume that signaling the
 * child twice will not cause any problems.
 */
static void
signal_child(pid_t pid, int signal)
{
	if (kill(pid, signal) < 0)
		elog(DEBUG3, "kill(%ld,%d) failed: %m", (long) pid, signal);
#ifdef HAVE_SETSID
	switch (signal)
	{
		case SIGINT:
		case SIGTERM:
		case SIGQUIT:
		case SIGKILL:
		case SIGABRT:
			if (kill(-pid, signal) < 0)
				elog(DEBUG3, "kill(%ld,%d) failed: %m", (long) (-pid), signal);
			break;
		default:
			break;
	}
#endif
}

/*
 * Convenience function for killing a child process after a crash of some
 * other child process.  We log the action at a higher level than we would
 * otherwise do, and we apply send_abort_for_crash to decide which signal
 * to send.  Normally it's SIGQUIT -- and most other comments in this file
 * are written on the assumption that it is -- but developers might prefer
 * to use SIGABRT to collect per-child core dumps.
 */
static void
sigquit_child(pid_t pid)
{
	ereport(DEBUG2,
			(errmsg_internal("sending %s to process %d",
							 (send_abort_for_crash ? "SIGABRT" : "SIGQUIT"),
							 (int) pid)));
	signal_child(pid, (send_abort_for_crash ? SIGABRT : SIGQUIT));
}

/*
 * Send a signal to the targeted children (but NOT special children;
 * dead_end children are never signaled, either).
 */
static bool
SignalSomeChildren(int signal, int target)
{
	dlist_iter	iter;
	bool		signaled = false;

	dlist_foreach(iter, &BackendList)
	{
		Backend    *bp = dlist_container(Backend, elem, iter.cur);

		if (bp->dead_end)
			continue;

		/*
		 * Since target == BACKEND_TYPE_ALL is the most common case, we test
		 * it first and avoid touching shared memory for every child.
		 */
		if (target != BACKEND_TYPE_ALL)
		{
			/*
			 * Assign bkend_type for any recently announced WAL Sender
			 * processes.
			 */
			if (bp->bkend_type == BACKEND_TYPE_NORMAL &&
				IsPostmasterChildWalSender(bp->child_slot))
				bp->bkend_type = BACKEND_TYPE_WALSND;

			if (!(target & bp->bkend_type))
				continue;
		}

		ereport(DEBUG4,
				(errmsg_internal("sending signal %d to process %d",
								 signal, (int) bp->pid)));
		signal_child(bp->pid, signal);
		signaled = true;
	}
	return signaled;
}

/*
 * Send a termination signal to children.  This considers all of our children
 * processes, except syslogger and dead_end backends.
 */
static void
TerminateChildren(int signal)
{
	SignalChildren(signal);
	if (StartupPID != 0)
	{
		signal_child(StartupPID, signal);
		if (signal == SIGQUIT || signal == SIGKILL || signal == SIGABRT)
			StartupStatus = STARTUP_SIGNALED;
	}
	if (BgWriterPID != 0)
		signal_child(BgWriterPID, signal);
	if (CheckpointerPID != 0)
		signal_child(CheckpointerPID, signal);
	if (WalWriterPID != 0)
		signal_child(WalWriterPID, signal);
	if (WalReceiverPID != 0)
		signal_child(WalReceiverPID, signal);
	if (WalSummarizerPID != 0)
		signal_child(WalSummarizerPID, signal);
	if (AutoVacPID != 0)
		signal_child(AutoVacPID, signal);
	if (PgArchPID != 0)
		signal_child(PgArchPID, signal);
	if (SlotSyncWorkerPID != 0)
		signal_child(SlotSyncWorkerPID, signal);
}

/*
 * BackendStartup -- start backend process
 *
 * returns: STATUS_ERROR if the fork failed, STATUS_OK otherwise.
 *
 * Note: if you change this code, also consider StartAutovacuumWorker.
 */
static int
BackendStartup(ClientSocket *client_sock)
{
	Backend    *bn;				/* for backend cleanup */
	pid_t		pid;
	BackendStartupData startup_data;

	/*
	 * Create backend data structure.  Better before the fork() so we can
	 * handle failure cleanly.
	 */
	bn = (Backend *) palloc_extended(sizeof(Backend), MCXT_ALLOC_NO_OOM);
	if (!bn)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return STATUS_ERROR;
	}

	/*
	 * Compute the cancel key that will be assigned to this backend. The
	 * backend will have its own copy in the forked-off process' value of
	 * MyCancelKey, so that it can transmit the key to the frontend.
	 */
	if (!RandomCancelKey(&MyCancelKey))
	{
		pfree(bn);
		ereport(LOG,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not generate random cancel key")));
		return STATUS_ERROR;
	}

	/* Pass down canAcceptConnections state */
	startup_data.canAcceptConnections = canAcceptConnections(BACKEND_TYPE_NORMAL);
	bn->dead_end = (startup_data.canAcceptConnections != CAC_OK);
	bn->cancel_key = MyCancelKey;

	/*
	 * Unless it's a dead_end child, assign it a child slot number
	 */
	if (!bn->dead_end)
		bn->child_slot = MyPMChildSlot = AssignPostmasterChildSlot();
	else
		bn->child_slot = 0;

	/* Hasn't asked to be notified about any bgworkers yet */
	bn->bgworker_notify = false;

	pid = postmaster_child_launch(B_BACKEND, /// [B_BACKEND] = {"backend", BackendMain, true}, 所以子进程的入口函数是BackendMain
								  (char *) &startup_data, sizeof(startup_data),
								  client_sock);
	/// 子进程的BackendMain会调用PostgresMain
	if (pid < 0)
	{
		/* in parent, fork failed */
		int			save_errno = errno;

		if (!bn->dead_end)
			(void) ReleasePostmasterChildSlot(bn->child_slot);
		pfree(bn);
		errno = save_errno;
		ereport(LOG,
				(errmsg("could not fork new process for connection: %m")));
		report_fork_failure_to_client(client_sock, save_errno);
		return STATUS_ERROR;
	}

	/* in parent, successful fork */
	ereport(DEBUG2,
			(errmsg_internal("forked new backend, pid=%d socket=%d",
							 (int) pid, (int) client_sock->sock)));

	/*
	 * Everything's been successful, it's safe to add this backend to our list
	 * of backends.
	 */
	bn->pid = pid;
	bn->bkend_type = BACKEND_TYPE_NORMAL;	/* Can change later to WALSND */ /// walsender和postgres进程本质上就是一回事。
	dlist_push_head(&BackendList, &bn->elem);

#ifdef EXEC_BACKEND
	if (!bn->dead_end)
		ShmemBackendArrayAdd(bn);
#endif

	return STATUS_OK;
}

/*
 * Try to report backend fork() failure to client before we close the
 * connection.  Since we do not care to risk blocking the postmaster on
 * this connection, we set the connection to non-blocking and try only once.
 *
 * This is grungy special-purpose code; we cannot use backend libpq since
 * it's not up and running.
 */
static void
report_fork_failure_to_client(ClientSocket *client_sock, int errnum)
{
	char		buffer[1000];
	int			rc;

	/* Format the error message packet (always V2 protocol) */
	snprintf(buffer, sizeof(buffer), "E%s%s\n",
			 _("could not fork new process for connection: "),
			 strerror(errnum));

	/* Set port to non-blocking.  Don't do send() if this fails */
	if (!pg_set_noblock(client_sock->sock))
		return;

	/* We'll retry after EINTR, but ignore all other failures */
	do
	{
		rc = send(client_sock->sock, buffer, strlen(buffer) + 1, 0);
	} while (rc < 0 && errno == EINTR);
}

/*
 * ExitPostmaster -- cleanup
 *
 * Do NOT call exit() directly --- always go through here!
 */
static void
ExitPostmaster(int status) /// 退出postmaster主进程，会在本进程退出之前执行很多已经注册的函数。
{
#ifdef HAVE_PTHREAD_IS_THREADED_NP

	/*
	 * There is no known cause for a postmaster to become multithreaded after
	 * startup.  However, we might reach here via an error exit before
	 * reaching the test in PostmasterMain, so provide the same hint as there.
	 * This message uses LOG level, because an unclean shutdown at this point
	 * would usually not look much different from a clean shutdown.
	 */
	if (pthread_is_threaded_np() != 0)
		ereport(LOG,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("postmaster became multithreaded"),
				 errhint("Set the LC_ALL environment variable to a valid locale.")));
#endif

	/* should cleanup shared memory and kill all backends */

	/*
	 * Not sure of the semantics here.  When the Postmaster dies, should the
	 * backends all be killed? probably not.
	 *
	 * MUST		-- vadim 05-10-1999
	 */

	proc_exit(status);
}

/*
 * Handle pmsignal conditions representing requests from backends,
 * and check for promote and logrotate requests from pg_ctl.
 */
static void
process_pm_pmsignal(void)
{
	pending_pm_pmsignal = false;

	ereport(DEBUG2,
			(errmsg_internal("postmaster received pmsignal signal")));

	/*
	 * RECOVERY_STARTED and BEGIN_HOT_STANDBY signals are ignored in
	 * unexpected states. If the startup process quickly starts up, completes
	 * recovery, exits, we might process the death of the startup process
	 * first. We don't want to go back to recovery in that case.
	 */
	if (CheckPostmasterSignal(PMSIGNAL_RECOVERY_STARTED) &&
		pmState == PM_STARTUP && Shutdown == NoShutdown)
	{
		/* WAL redo has started. We're out of reinitialization. */
		FatalError = false;
		AbortStartTime = 0;

		/*
		 * Start the archiver if we're responsible for (re-)archiving received
		 * files.
		 */
		Assert(PgArchPID == 0);
		if (XLogArchivingAlways())
			PgArchPID = StartChildProcess(B_ARCHIVER);

		/*
		 * If we aren't planning to enter hot standby mode later, treat
		 * RECOVERY_STARTED as meaning we're out of startup, and report status
		 * accordingly.
		 */
		if (!EnableHotStandby)
		{
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STANDBY);
#ifdef USE_SYSTEMD
			sd_notify(0, "READY=1");
#endif
		}

		pmState = PM_RECOVERY;
	}

	if (CheckPostmasterSignal(PMSIGNAL_BEGIN_HOT_STANDBY) &&
		pmState == PM_RECOVERY && Shutdown == NoShutdown)
	{
		ereport(LOG,
				(errmsg("database system is ready to accept read-only connections")));

		/* Report status */
		AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_READY);
#ifdef USE_SYSTEMD
		sd_notify(0, "READY=1");
#endif

		pmState = PM_HOT_STANDBY;
		connsAllowed = true;

		/* Some workers may be scheduled to start now */
		StartWorkerNeeded = true;
	}

	/* Process background worker state changes. */
	if (CheckPostmasterSignal(PMSIGNAL_BACKGROUND_WORKER_CHANGE))
	{
		/* Accept new worker requests only if not stopping. */
		BackgroundWorkerStateChange(pmState < PM_STOP_BACKENDS);
		StartWorkerNeeded = true;
	}

	if (StartWorkerNeeded || HaveCrashedWorker)
		maybe_start_bgworkers();

	/* Tell syslogger to rotate logfile if requested */
	if (SysLoggerPID != 0)
	{
		if (CheckLogrotateSignal())
		{
			signal_child(SysLoggerPID, SIGUSR1);
			RemoveLogrotateSignalFiles();
		}
		else if (CheckPostmasterSignal(PMSIGNAL_ROTATE_LOGFILE))
		{
			signal_child(SysLoggerPID, SIGUSR1);
		}
	}

	if (CheckPostmasterSignal(PMSIGNAL_START_AUTOVAC_LAUNCHER) &&
		Shutdown <= SmartShutdown && pmState < PM_STOP_BACKENDS)
	{
		/*
		 * Start one iteration of the autovacuum daemon, even if autovacuuming
		 * is nominally not enabled.  This is so we can have an active defense
		 * against transaction ID wraparound.  We set a flag for the main loop
		 * to do it rather than trying to do it here --- this is because the
		 * autovac process itself may send the signal, and we want to handle
		 * that by launching another iteration as soon as the current one
		 * completes.
		 */
		start_autovac_launcher = true;
	}

	if (CheckPostmasterSignal(PMSIGNAL_START_AUTOVAC_WORKER) &&
		Shutdown <= SmartShutdown && pmState < PM_STOP_BACKENDS)
	{
		/* The autovacuum launcher wants us to start a worker process. */
		StartAutovacuumWorker();
	}

	if (CheckPostmasterSignal(PMSIGNAL_START_WALRECEIVER))
	{
		/* Startup Process wants us to start the walreceiver process. */
		/* Start immediately if possible, else remember request for later. */
		WalReceiverRequested = true;
		MaybeStartWalReceiver();
	}

	/*
	 * Try to advance postmaster's state machine, if a child requests it.
	 *
	 * Be careful about the order of this action relative to this function's
	 * other actions.  Generally, this should be after other actions, in case
	 * they have effects PostmasterStateMachine would need to know about.
	 * However, we should do it before the CheckPromoteSignal step, which
	 * cannot have any (immediate) effect on the state machine, but does
	 * depend on what state we're in now.
	 */
	if (CheckPostmasterSignal(PMSIGNAL_ADVANCE_STATE_MACHINE))
	{
		PostmasterStateMachine();
	}

	if (StartupPID != 0 &&
		(pmState == PM_STARTUP || pmState == PM_RECOVERY ||
		 pmState == PM_HOT_STANDBY) &&
		CheckPromoteSignal())
	{
		/*
		 * Tell startup process to finish recovery.
		 *
		 * Leave the promote signal file in place and let the Startup process
		 * do the unlink.
		 */
		signal_child(StartupPID, SIGUSR2);
	}
}

/*
 * Dummy signal handler
 *
 * We use this for signals that we don't actually use in the postmaster,
 * but we do use in backends.  If we were to SIG_IGN such signals in the
 * postmaster, then a newly started backend might drop a signal that arrives
 * before it's able to reconfigure its signal processing.  (See notes in
 * tcop/postgres.c.)
 */
static void
dummy_handler(SIGNAL_ARGS)
{
}

/*
 * Generate a random cancel key.
 */
static bool
RandomCancelKey(int32 *cancel_key)
{
	return pg_strong_random(cancel_key, sizeof(int32));
}

/*
 * Count up number of child processes of specified types (dead_end children
 * are always excluded).
 */
static int
CountChildren(int target)
{
	dlist_iter	iter;
	int			cnt = 0;

	dlist_foreach(iter, &BackendList)
	{
		Backend    *bp = dlist_container(Backend, elem, iter.cur);

		if (bp->dead_end)
			continue;

		/*
		 * Since target == BACKEND_TYPE_ALL is the most common case, we test
		 * it first and avoid touching shared memory for every child.
		 */
		if (target != BACKEND_TYPE_ALL)
		{
			/*
			 * Assign bkend_type for any recently announced WAL Sender
			 * processes.
			 */
			if (bp->bkend_type == BACKEND_TYPE_NORMAL &&
				IsPostmasterChildWalSender(bp->child_slot))
				bp->bkend_type = BACKEND_TYPE_WALSND;

			if (!(target & bp->bkend_type))
				continue;
		}

		cnt++;
	}
	return cnt;
}


/*
 * StartChildProcess -- start an auxiliary process for the postmaster
 *
 * "type" determines what kind of child will be started.  All child types
 * initially go to AuxiliaryProcessMain, which will handle common setup.
 *
 * Return value of StartChildProcess is subprocess' PID, or 0 if failed
 * to start subprocess.
 */
static pid_t
StartChildProcess(BackendType type)
{
	pid_t		pid;

	pid = postmaster_child_launch(type, NULL, 0, NULL);
	if (pid < 0)
	{
		/* in parent, fork failed */
		ereport(LOG,
				(errmsg("could not fork \"%s\" process: %m", PostmasterChildName(type))));

		/*
		 * fork failure is fatal during startup, but there's no need to choke
		 * immediately if starting other child types fails.
		 */
		if (type == B_STARTUP)
			ExitPostmaster(1); /// 如果 startup 进程无法启动，就退出本进程。
		return 0;
	}

	/*
	 * in parent, successful fork
	 */
	return pid;
}

/*
 * StartAutovacuumWorker
 *		Start an autovac worker process.
 *
 * This function is here because it enters the resulting PID into the
 * postmaster's private backends list.
 *
 * NB -- this code very roughly matches BackendStartup.
 */
static void
StartAutovacuumWorker(void)
{
	Backend    *bn;

	/*
	 * If not in condition to run a process, don't try, but handle it like a
	 * fork failure.  This does not normally happen, since the signal is only
	 * supposed to be sent by autovacuum launcher when it's OK to do it, but
	 * we have to check to avoid race-condition problems during DB state
	 * changes.
	 */
	if (canAcceptConnections(BACKEND_TYPE_AUTOVAC) == CAC_OK)
	{
		/*
		 * Compute the cancel key that will be assigned to this session. We
		 * probably don't need cancel keys for autovac workers, but we'd
		 * better have something random in the field to prevent unfriendly
		 * people from sending cancels to them.
		 */
		if (!RandomCancelKey(&MyCancelKey))
		{
			ereport(LOG,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not generate random cancel key")));
			return;
		}

		bn = (Backend *) palloc_extended(sizeof(Backend), MCXT_ALLOC_NO_OOM);
		if (bn)
		{
			bn->cancel_key = MyCancelKey;

			/* Autovac workers are not dead_end and need a child slot */
			bn->dead_end = false;
			bn->child_slot = MyPMChildSlot = AssignPostmasterChildSlot();
			bn->bgworker_notify = false;

			bn->pid = StartChildProcess(B_AUTOVAC_WORKER);
			if (bn->pid > 0)
			{
				bn->bkend_type = BACKEND_TYPE_AUTOVAC;
				dlist_push_head(&BackendList, &bn->elem);
#ifdef EXEC_BACKEND
				ShmemBackendArrayAdd(bn);
#endif
				/* all OK */
				return;
			}

			/*
			 * fork failed, fall through to report -- actual error message was
			 * logged by StartChildProcess
			 */
			(void) ReleasePostmasterChildSlot(bn->child_slot);
			pfree(bn);
		}
		else
			ereport(LOG,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
	}

	/*
	 * Report the failure to the launcher, if it's running.  (If it's not, we
	 * might not even be connected to shared memory, so don't try to call
	 * AutoVacWorkerFailed.)  Note that we also need to signal it so that it
	 * responds to the condition, but we don't do that here, instead waiting
	 * for ServerLoop to do it.  This way we avoid a ping-pong signaling in
	 * quick succession between the autovac launcher and postmaster in case
	 * things get ugly.
	 */
	if (AutoVacPID != 0)
	{
		AutoVacWorkerFailed();
		avlauncher_needs_signal = true;
	}
}

/*
 * MaybeStartWalReceiver
 *		Start the WAL receiver process, if not running and our state allows.
 *
 * Note: if WalReceiverPID is already nonzero, it might seem that we should
 * clear WalReceiverRequested.  However, there's a race condition if the
 * walreceiver terminates and the startup process immediately requests a new
 * one: it's quite possible to get the signal for the request before reaping
 * the dead walreceiver process.  Better to risk launching an extra
 * walreceiver than to miss launching one we need.  (The walreceiver code
 * has logic to recognize that it should go away if not needed.)
 */
static void
MaybeStartWalReceiver(void)
{
	if (WalReceiverPID == 0 &&
		(pmState == PM_STARTUP || pmState == PM_RECOVERY ||
		 pmState == PM_HOT_STANDBY) &&
		Shutdown <= SmartShutdown)
	{
		WalReceiverPID = StartChildProcess(B_WAL_RECEIVER);
		if (WalReceiverPID != 0)
			WalReceiverRequested = false;
		/* else leave the flag set, so we'll try again later */
	}
}

/*
 * MaybeStartWalSummarizer
 *		Start the WAL summarizer process, if not running and our state allows.
 */
static void
MaybeStartWalSummarizer(void)
{
	if (summarize_wal && WalSummarizerPID == 0 &&
		(pmState == PM_RUN || pmState == PM_HOT_STANDBY) &&
		Shutdown <= SmartShutdown)
		WalSummarizerPID = StartChildProcess(B_WAL_SUMMARIZER); /// WalSumarizePID只在这里赋值。
}


/*
 * MaybeStartSlotSyncWorker
 * 		Start the slot sync worker, if not running and our state allows.
 *
 * We allow to start the slot sync worker when we are on a hot standby,
 * fast or immediate shutdown is not in progress, slot sync parameters
 * are configured correctly, and it is the first time of worker's launch,
 * or enough time has passed since the worker was launched last.
 */
static void
MaybeStartSlotSyncWorker(void)
{
	if (SlotSyncWorkerPID == 0 && pmState == PM_HOT_STANDBY &&
		Shutdown <= SmartShutdown && sync_replication_slots &&
		ValidateSlotSyncParams(LOG) && SlotSyncWorkerCanRestart())
		SlotSyncWorkerPID = StartChildProcess(B_SLOTSYNC_WORKER);
}

/*
 * Create the opts file /// 创建postmaster.opts文件
 */
static bool
CreateOptsFile(int argc, char *argv[], char *fullprogname)
{
	FILE	   *fp;
	int			i;

#define OPTS_FILE	"postmaster.opts"

	if ((fp = fopen(OPTS_FILE, "w")) == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", OPTS_FILE)));
		return false;
	}

	fprintf(fp, "%s", fullprogname);
	for (i = 1; i < argc; i++)
		fprintf(fp, " \"%s\"", argv[i]);
	fputs("\n", fp);

	if (fclose(fp))
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", OPTS_FILE)));
		return false;
	}

	return true;
}


/*
 * MaxLivePostmasterChildren
 *
 * This reports the number of entries needed in per-child-process arrays
 * (the PMChildFlags array, and if EXEC_BACKEND the ShmemBackendArray).
 * These arrays include regular backends, autovac workers, walsenders
 * and background workers, but not special children nor dead_end children.
 * This allows the arrays to have a fixed maximum size, to wit the same
 * too-many-children limit enforced by canAcceptConnections().  The exact value
 * isn't too critical as long as it's more than MaxBackends.
 */
int
MaxLivePostmasterChildren(void)
{
	return 2 * (MaxConnections + autovacuum_max_workers + 1 +
				max_wal_senders + max_worker_processes);
}

/*
 * Connect background worker to a database.
 */
void
BackgroundWorkerInitializeConnection(const char *dbname, const char *username, uint32 flags)
{
	BackgroundWorker *worker = MyBgworkerEntry;
	bits32		init_flags = 0; /* never honor session_preload_libraries */

	/* ignore datallowconn and ACL_CONNECT? */
	if (flags & BGWORKER_BYPASS_ALLOWCONN)
		init_flags |= INIT_PG_OVERRIDE_ALLOW_CONNS;
	/* ignore rolcanlogin? */
	if (flags & BGWORKER_BYPASS_ROLELOGINCHECK)
		init_flags |= INIT_PG_OVERRIDE_ROLE_LOGIN;

	/* XXX is this the right errcode? */
	if (!(worker->bgw_flags & BGWORKER_BACKEND_DATABASE_CONNECTION))
		ereport(FATAL,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("database connection requirement not indicated during registration")));

	InitPostgres(dbname, InvalidOid,	/* database to connect to */
				 username, InvalidOid,	/* role to connect as */
				 init_flags,
				 NULL);			/* no out_dbname */

	/* it had better not gotten out of "init" mode yet */
	if (!IsInitProcessingMode())
		ereport(ERROR,
				(errmsg("invalid processing mode in background worker")));
	SetProcessingMode(NormalProcessing);
}

/*
 * Connect background worker to a database using OIDs.
 */
void
BackgroundWorkerInitializeConnectionByOid(Oid dboid, Oid useroid, uint32 flags)
{
	BackgroundWorker *worker = MyBgworkerEntry;
	bits32		init_flags = 0; /* never honor session_preload_libraries */

	/* ignore datallowconn and ACL_CONNECT? */
	if (flags & BGWORKER_BYPASS_ALLOWCONN)
		init_flags |= INIT_PG_OVERRIDE_ALLOW_CONNS;
	/* ignore rolcanlogin? */
	if (flags & BGWORKER_BYPASS_ROLELOGINCHECK)
		init_flags |= INIT_PG_OVERRIDE_ROLE_LOGIN;

	/* XXX is this the right errcode? */
	if (!(worker->bgw_flags & BGWORKER_BACKEND_DATABASE_CONNECTION))
		ereport(FATAL,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("database connection requirement not indicated during registration")));

	InitPostgres(NULL, dboid,	/* database to connect to */
				 NULL, useroid, /* role to connect as */
				 init_flags,
				 NULL);			/* no out_dbname */

	/* it had better not gotten out of "init" mode yet */
	if (!IsInitProcessingMode())
		ereport(ERROR,
				(errmsg("invalid processing mode in background worker")));
	SetProcessingMode(NormalProcessing);
}

/*
 * Block/unblock signals in a background worker
 */
void
BackgroundWorkerBlockSignals(void)
{
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);
}

void
BackgroundWorkerUnblockSignals(void)
{
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);
}

/*
 * Start a new bgworker.
 * Starting time conditions must have been checked already.
 *
 * Returns true on success, false on failure.
 * In either case, update the RegisteredBgWorker's state appropriately.
 *
 * This code is heavily based on autovacuum.c, q.v.
 */
static bool
do_start_bgworker(RegisteredBgWorker *rw)
{
	pid_t		worker_pid;

	Assert(rw->rw_pid == 0);

	/*
	 * Allocate and assign the Backend element.  Note we must do this before
	 * forking, so that we can handle failures (out of memory or child-process
	 * slots) cleanly.
	 *
	 * Treat failure as though the worker had crashed.  That way, the
	 * postmaster will wait a bit before attempting to start it again; if we
	 * tried again right away, most likely we'd find ourselves hitting the
	 * same resource-exhaustion condition.
	 */
	if (!assign_backendlist_entry(rw))
	{
		rw->rw_crashed_at = GetCurrentTimestamp();
		return false;
	}

	ereport(DEBUG1,
			(errmsg_internal("starting background worker process \"%s\"",
							 rw->rw_worker.bgw_name)));

	worker_pid = postmaster_child_launch(B_BG_WORKER, (char *) &rw->rw_worker, sizeof(BackgroundWorker), NULL);
	if (worker_pid == -1)
	{
		/* in postmaster, fork failed ... */
		ereport(LOG,
				(errmsg("could not fork background worker process: %m")));
		/* undo what assign_backendlist_entry did */
		ReleasePostmasterChildSlot(rw->rw_child_slot);
		rw->rw_child_slot = 0;
		pfree(rw->rw_backend);
		rw->rw_backend = NULL;
		/* mark entry as crashed, so we'll try again later */
		rw->rw_crashed_at = GetCurrentTimestamp();
		return false;
	}

	/* in postmaster, fork successful ... */
	rw->rw_pid = worker_pid;
	rw->rw_backend->pid = rw->rw_pid;
	ReportBackgroundWorkerPID(rw);
	/* add new worker to lists of backends */
	dlist_push_head(&BackendList, &rw->rw_backend->elem);
#ifdef EXEC_BACKEND
	ShmemBackendArrayAdd(rw->rw_backend);
#endif
	return true;
}

/*
 * Does the current postmaster state require starting a worker with the
 * specified start_time?
 */
static bool
bgworker_should_start_now(BgWorkerStartTime start_time)
{
	switch (pmState)
	{
		case PM_NO_CHILDREN:
		case PM_WAIT_DEAD_END:
		case PM_SHUTDOWN_2:
		case PM_SHUTDOWN:
		case PM_WAIT_BACKENDS:
		case PM_STOP_BACKENDS:
			break;

		case PM_RUN:
			if (start_time == BgWorkerStart_RecoveryFinished)
				return true;
			/* fall through */

		case PM_HOT_STANDBY:
			if (start_time == BgWorkerStart_ConsistentState)
				return true;
			/* fall through */

		case PM_RECOVERY:
		case PM_STARTUP:
		case PM_INIT:
			if (start_time == BgWorkerStart_PostmasterStart)
				return true;
			/* fall through */
	}

	return false;
}

/*
 * Allocate the Backend struct for a connected background worker, but don't
 * add it to the list of backends just yet.
 *
 * On failure, return false without changing any worker state.
 *
 * Some info from the Backend is copied into the passed rw.
 */
static bool
assign_backendlist_entry(RegisteredBgWorker *rw)
{
	Backend    *bn;

	/*
	 * Check that database state allows another connection.  Currently the
	 * only possible failure is CAC_TOOMANY, so we just log an error message
	 * based on that rather than checking the error code precisely.
	 */
	if (canAcceptConnections(BACKEND_TYPE_BGWORKER) != CAC_OK)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("no slot available for new background worker process")));
		return false;
	}

	/*
	 * Compute the cancel key that will be assigned to this session. We
	 * probably don't need cancel keys for background workers, but we'd better
	 * have something random in the field to prevent unfriendly people from
	 * sending cancels to them.
	 */
	if (!RandomCancelKey(&MyCancelKey))
	{
		ereport(LOG,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not generate random cancel key")));
		return false;
	}

	bn = palloc_extended(sizeof(Backend), MCXT_ALLOC_NO_OOM);
	if (bn == NULL)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return false;
	}

	bn->cancel_key = MyCancelKey;
	bn->child_slot = MyPMChildSlot = AssignPostmasterChildSlot();
	bn->bkend_type = BACKEND_TYPE_BGWORKER;
	bn->dead_end = false;
	bn->bgworker_notify = false;

	rw->rw_backend = bn;
	rw->rw_child_slot = bn->child_slot;

	return true;
}

/*
 * If the time is right, start background worker(s).
 *
 * As a side effect, the bgworker control variables are set or reset
 * depending on whether more workers may need to be started.
 *
 * We limit the number of workers started per call, to avoid consuming the
 * postmaster's attention for too long when many such requests are pending.
 * As long as StartWorkerNeeded is true, ServerLoop will not block and will
 * call this function again after dealing with any other issues.
 */
static void
maybe_start_bgworkers(void)
{
#define MAX_BGWORKERS_TO_LAUNCH 100
	int			num_launched = 0;
	TimestampTz now = 0;
	slist_mutable_iter iter;

	/*
	 * During crash recovery, we have no need to be called until the state
	 * transition out of recovery.
	 */
	if (FatalError)
	{
		StartWorkerNeeded = false;
		HaveCrashedWorker = false;
		return;
	}

	/* Don't need to be called again unless we find a reason for it below */
	StartWorkerNeeded = false;
	HaveCrashedWorker = false;

	slist_foreach_modify(iter, &BackgroundWorkerList)
	{
		RegisteredBgWorker *rw;

		rw = slist_container(RegisteredBgWorker, rw_lnode, iter.cur);

		/* ignore if already running */
		if (rw->rw_pid != 0)
			continue;

		/* if marked for death, clean up and remove from list */
		if (rw->rw_terminate)
		{
			ForgetBackgroundWorker(&iter);
			continue;
		}

		/*
		 * If this worker has crashed previously, maybe it needs to be
		 * restarted (unless on registration it specified it doesn't want to
		 * be restarted at all).  Check how long ago did a crash last happen.
		 * If the last crash is too recent, don't start it right away; let it
		 * be restarted once enough time has passed.
		 */
		if (rw->rw_crashed_at != 0)
		{
			if (rw->rw_worker.bgw_restart_time == BGW_NEVER_RESTART)
			{
				int			notify_pid;

				notify_pid = rw->rw_worker.bgw_notify_pid;

				ForgetBackgroundWorker(&iter);

				/* Report worker is gone now. */
				if (notify_pid != 0)
					kill(notify_pid, SIGUSR1);

				continue;
			}

			/* read system time only when needed */
			if (now == 0)
				now = GetCurrentTimestamp();

			if (!TimestampDifferenceExceeds(rw->rw_crashed_at, now,
											rw->rw_worker.bgw_restart_time * 1000))
			{
				/* Set flag to remember that we have workers to start later */
				HaveCrashedWorker = true;
				continue;
			}
		}

		if (bgworker_should_start_now(rw->rw_worker.bgw_start_time))
		{
			/* reset crash time before trying to start worker */
			rw->rw_crashed_at = 0;

			/*
			 * Try to start the worker.
			 *
			 * On failure, give up processing workers for now, but set
			 * StartWorkerNeeded so we'll come back here on the next iteration
			 * of ServerLoop to try again.  (We don't want to wait, because
			 * there might be additional ready-to-run workers.)  We could set
			 * HaveCrashedWorker as well, since this worker is now marked
			 * crashed, but there's no need because the next run of this
			 * function will do that.
			 */
			if (!do_start_bgworker(rw))
			{
				StartWorkerNeeded = true;
				return;
			}

			/*
			 * If we've launched as many workers as allowed, quit, but have
			 * ServerLoop call us again to look for additional ready-to-run
			 * workers.  There might not be any, but we'll find out the next
			 * time we run.
			 */
			if (++num_launched >= MAX_BGWORKERS_TO_LAUNCH)
			{
				StartWorkerNeeded = true;
				return;
			}
		}
	}
}

/*
 * When a backend asks to be notified about worker state changes, we
 * set a flag in its backend entry.  The background worker machinery needs
 * to know when such backends exit.
 */
bool
PostmasterMarkPIDForWorkerNotify(int pid)
{
	dlist_iter	iter;
	Backend    *bp;

	dlist_foreach(iter, &BackendList)
	{
		bp = dlist_container(Backend, elem, iter.cur);
		if (bp->pid == pid)
		{
			bp->bgworker_notify = true;
			return true;
		}
	}
	return false;
}

#ifdef EXEC_BACKEND

Size
ShmemBackendArraySize(void)
{
	return mul_size(MaxLivePostmasterChildren(), sizeof(Backend));
}

void
ShmemBackendArrayAllocation(void)
{
	Size		size = ShmemBackendArraySize();

	ShmemBackendArray = (Backend *) ShmemAlloc(size);
	/* Mark all slots as empty */
	memset(ShmemBackendArray, 0, size);
}

static void
ShmemBackendArrayAdd(Backend *bn)
{
	/* The array slot corresponding to my PMChildSlot should be free */
	int			i = bn->child_slot - 1;

	Assert(ShmemBackendArray[i].pid == 0);
	ShmemBackendArray[i] = *bn;
}

static void
ShmemBackendArrayRemove(Backend *bn)
{
	int			i = bn->child_slot - 1;

	Assert(ShmemBackendArray[i].pid == bn->pid);
	/* Mark the slot as empty */
	ShmemBackendArray[i].pid = 0;
}
#endif							/* EXEC_BACKEND */


#ifdef WIN32

/*
 * Subset implementation of waitpid() for Windows.  We assume pid is -1
 * (that is, check all child processes) and options is WNOHANG (don't wait).
 */
static pid_t
waitpid(pid_t pid, int *exitstatus, int options)
{
	win32_deadchild_waitinfo *childinfo;
	DWORD		exitcode;
	DWORD		dwd;
	ULONG_PTR	key;
	OVERLAPPED *ovl;

	/* Try to consume one win32_deadchild_waitinfo from the queue. */
	if (!GetQueuedCompletionStatus(win32ChildQueue, &dwd, &key, &ovl, 0))
	{
		errno = EAGAIN;
		return -1;
	}

	childinfo = (win32_deadchild_waitinfo *) key;
	pid = childinfo->procId;

	/*
	 * Remove handle from wait - required even though it's set to wait only
	 * once
	 */
	UnregisterWaitEx(childinfo->waitHandle, NULL);

	if (!GetExitCodeProcess(childinfo->procHandle, &exitcode))
	{
		/*
		 * Should never happen. Inform user and set a fixed exitcode.
		 */
		write_stderr("could not read exit code for process\n");
		exitcode = 255;
	}
	*exitstatus = exitcode;

	/*
	 * Close the process handle.  Only after this point can the PID can be
	 * recycled by the kernel.
	 */
	CloseHandle(childinfo->procHandle);

	/*
	 * Free struct that was allocated before the call to
	 * RegisterWaitForSingleObject()
	 */
	pfree(childinfo);

	return pid;
}

/*
 * Note! Code below executes on a thread pool! All operations must
 * be thread safe! Note that elog() and friends must *not* be used.
 */
static void WINAPI
pgwin32_deadchild_callback(PVOID lpParameter, BOOLEAN TimerOrWaitFired)
{
	/* Should never happen, since we use INFINITE as timeout value. */
	if (TimerOrWaitFired)
		return;

	/*
	 * Post the win32_deadchild_waitinfo object for waitpid() to deal with. If
	 * that fails, we leak the object, but we also leak a whole process and
	 * get into an unrecoverable state, so there's not much point in worrying
	 * about that.  We'd like to panic, but we can't use that infrastructure
	 * from this thread.
	 */
	if (!PostQueuedCompletionStatus(win32ChildQueue,
									0,
									(ULONG_PTR) lpParameter,
									NULL))
		write_stderr("could not post child completion status\n");

	/* Queue SIGCHLD signal. */
	pg_queue_signal(SIGCHLD);
}

/*
 * Queue a waiter to signal when this child dies.  The wait will be handled
 * automatically by an operating system thread pool.  The memory and the
 * process handle will be freed by a later call to waitpid().
 */
void
pgwin32_register_deadchild_callback(HANDLE procHandle, DWORD procId)
{
	win32_deadchild_waitinfo *childinfo;

	childinfo = palloc(sizeof(win32_deadchild_waitinfo));
	childinfo->procHandle = procHandle;
	childinfo->procId = procId;

	if (!RegisterWaitForSingleObject(&childinfo->waitHandle,
									 procHandle,
									 pgwin32_deadchild_callback,
									 childinfo,
									 INFINITE,
									 WT_EXECUTEONLYONCE | WT_EXECUTEINWAITTHREAD))
		ereport(FATAL,
				(errmsg_internal("could not register process for wait: error code %lu",
								 GetLastError())));
}

#endif							/* WIN32 */

/*
 * Initialize one and only handle for monitoring postmaster death.
 *
 * Called once in the postmaster, so that child processes can subsequently
 * monitor if their parent is dead.
 */
static void
InitPostmasterDeathWatchHandle(void)
{
#ifndef WIN32

	/*
	 * Create a pipe. Postmaster holds the write end of the pipe open
	 * (POSTMASTER_FD_OWN), and children hold the read end. Children can pass
	 * the read file descriptor to select() to wake up in case postmaster
	 * dies, or check for postmaster death with a (read() == 0). Children must
	 * close the write end as soon as possible after forking, because EOF
	 * won't be signaled in the read end until all processes have closed the
	 * write fd. That is taken care of in ClosePostmasterPorts().
	 */
	Assert(MyProcPid == PostmasterPid);
	if (pipe(postmaster_alive_fds) < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg_internal("could not create pipe to monitor postmaster death: %m")));

	/* Notify fd.c that we've eaten two FDs for the pipe. */
	ReserveExternalFD();
	ReserveExternalFD();

	/*
	 * Set O_NONBLOCK to allow testing for the fd's presence with a read()
	 * call.
	 */
	if (fcntl(postmaster_alive_fds[POSTMASTER_FD_WATCH], F_SETFL, O_NONBLOCK) == -1)
		ereport(FATAL,
				(errcode_for_socket_access(),
				 errmsg_internal("could not set postmaster death monitoring pipe to nonblocking mode: %m")));
#else

	/*
	 * On Windows, we use a process handle for the same purpose.
	 */
	if (DuplicateHandle(GetCurrentProcess(),
						GetCurrentProcess(),
						GetCurrentProcess(),
						&PostmasterHandle,
						0,
						TRUE,
						DUPLICATE_SAME_ACCESS) == 0)
		ereport(FATAL,
				(errmsg_internal("could not duplicate postmaster handle: error code %lu",
								 GetLastError())));
#endif							/* WIN32 */
}
