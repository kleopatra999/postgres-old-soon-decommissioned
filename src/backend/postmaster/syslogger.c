/*-------------------------------------------------------------------------
 *
 * syslogger.c
 *
 * The system logger (syslogger) is new in Postgres 8.0. It catches all
 * stderr output from the postmaster, backends, and other subprocesses
 * by redirecting to a pipe, and writes it to a set of logfiles.
 * It's possible to have size and age limits for the logfile configured
 * in postgresql.conf. If these limits are reached or passed, the
 * current logfile is closed and a new one is created (rotated).
 * The logfiles are stored in a subdirectory (configurable in
 * postgresql.conf), using an internal naming scheme that mangles
 * creation time and current postmaster pid.
 *
 * Author: Andreas Pflug <pgadmin@pse-consulting.de>
 *
 * Copyright (c) 2004, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "postmaster/syslogger.h"
#include "pgtime.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"
#include "utils/guc.h"
#include "utils/ps_status.h"


/*
 * We really want line-buffered mode for logfile output, but Windows does
 * not have it, and interprets _IOLBF as _IOFBF (bozos).  So use _IONBF
 * instead on Windows.
 */
#ifdef WIN32
#define LBF_MODE	_IONBF
#else
#define LBF_MODE	_IOLBF
#endif


/*
 * GUC parameters.  Redirect_stderr cannot be changed after postmaster
 * start, but the rest can change at SIGHUP.
 */
bool		Redirect_stderr = false;
int			Log_RotationAge = 24*60;
int			Log_RotationSize  = 10*1024;
char *      Log_directory = "pg_log";
char *      Log_filename_prefix = "postgresql-";

/*
 * Globally visible state (used by elog.c)
 */
bool am_syslogger = false;

/*
 * Private state
 */
static pg_time_t	last_rotation_time = 0;

static bool	redirection_done = false;

static bool	pipe_eof_seen = false;

static FILE *syslogFile = NULL;

/* These must be exported for EXEC_BACKEND case ... annoying */
#ifndef WIN32
int syslogPipe[2] = {-1, -1};
#else
HANDLE syslogPipe[2] = {0, 0};
#endif

#ifdef WIN32
static HANDLE threadHandle=0;
static CRITICAL_SECTION sysfileSection;
#endif

/*
 * Flags set by interrupt handlers for later service in the main loop.
 */
static volatile sig_atomic_t got_SIGHUP = false;


/* Local subroutines */
#ifdef EXEC_BACKEND
static pid_t syslogger_forkexec(void);
static void syslogger_parseArgs(int argc, char *argv[]);
#endif
#ifdef WIN32
static unsigned int __stdcall pipeThread(void *arg);
#endif
static void logfile_rotate(void);
static char* logfile_getname(pg_time_t timestamp);
static void sigHupHandler(SIGNAL_ARGS);


/*
 * Main entry point for syslogger process
 * argc/argv parameters are valid only in EXEC_BACKEND case.
 */
NON_EXEC_STATIC void
SysLoggerMain(int argc, char *argv[])
{
	char         currentLogDir[MAXPGPATH];

	IsUnderPostmaster = true;	/* we are a postmaster subprocess now */

	MyProcPid = getpid();		/* reset MyProcPid */

	/* Lose the postmaster's on-exit routines */
	on_exit_reset();

#ifdef EXEC_BACKEND
	syslogger_parseArgs(argc, argv);
#endif /* EXEC_BACKEND */

	am_syslogger = true;

	init_ps_display("logger process", "", "");
	set_ps_display("");

	/*
	 * If we restarted, our stderr is already redirected into our own
	 * input pipe.  This is of course pretty useless, not to mention that
	 * it interferes with detecting pipe EOF.  Point stderr to /dev/null.
	 * This assumes that all interesting messages generated in the syslogger
	 * will come through elog.c and will be sent to write_syslogger_file.
	 */
	if (redirection_done)
	{
		int	fd = open(NULL_DEV, O_WRONLY);

		/*
		 * The closes might look redundant, but they are not: we want to be
		 * darn sure the pipe gets closed even if the open failed.  We can
		 * survive running with stderr pointing nowhere, but we can't afford
		 * to have extra pipe input descriptors hanging around.
		 */
		close(fileno(stdout));
		close(fileno(stderr));
		dup2(fd, fileno(stdout));
		dup2(fd, fileno(stderr));
		close(fd);
	}

	/*
	 * Also close our copy of the write end of the pipe.  This is needed
	 * to ensure we can detect pipe EOF correctly.  (But note that in the
	 * restart case, the postmaster already did this.)
	 */
#ifndef WIN32
	if (syslogPipe[1] >= 0)
		close(syslogPipe[1]);
	syslogPipe[1] = -1;
#else
	if (syslogPipe[1])
		CloseHandle(syslogPipe[1]);
	syslogPipe[1] = 0;
#endif

	/*
	 * Properly accept or ignore signals the postmaster might send us
	 *
	 * Note: we ignore all termination signals, and instead exit only when
	 * all upstream processes are gone, to ensure we don't miss any dying
	 * gasps of broken backends...
	 */

	pqsignal(SIGHUP, sigHupHandler);	/* set flag to read config file */
	pqsignal(SIGINT,  SIG_IGN);
	pqsignal(SIGTERM, SIG_IGN);
	pqsignal(SIGQUIT, SIG_IGN);
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, SIG_IGN);
	pqsignal(SIGUSR2, SIG_IGN);

	/*
	 * Reset some signals that are accepted by postmaster but not here
	 */
	pqsignal(SIGCHLD, SIG_DFL);
	pqsignal(SIGTTIN, SIG_DFL);
	pqsignal(SIGTTOU, SIG_DFL);
	pqsignal(SIGCONT, SIG_DFL);
	pqsignal(SIGWINCH, SIG_DFL);

	PG_SETMASK(&UnBlockSig);

#ifdef WIN32
	/* Fire up separate data transfer thread */
	InitializeCriticalSection(&sysfileSection);

	{
		unsigned int tid;

		threadHandle = (HANDLE)_beginthreadex(0, 0, pipeThread, 0, 0, &tid);
	}
#endif  /* WIN32 */

	/* remember age of initial logfile */
	last_rotation_time = time(NULL);
	/* remember active logfile directory */
	strncpy(currentLogDir, Log_directory, MAXPGPATH);

	/* main worker loop */
	for (;;)
	{
		bool rotation_requested = false;
#ifndef WIN32
		char        logbuffer[1024];
		int          bytesRead;
		int         rc;
		fd_set		rfds;
		struct timeval timeout;
#endif

		if (got_SIGHUP)
		{
			got_SIGHUP = false;
			ProcessConfigFile(PGC_SIGHUP);

			/*
			 * Check if the log directory changed in postgresql.conf. If so,
			 * force rotation to make sure we're writing the logfiles in the
			 * right place.
			 *
			 * XXX is it worth responding similarly to a change of
			 * Log_filename_prefix?
			 */
			if (strncmp(Log_directory, currentLogDir, MAXPGPATH) != 0)
			{
				strncpy(currentLogDir, Log_directory, MAXPGPATH);
				rotation_requested = true;
			}
		}

		if (!rotation_requested &&
			last_rotation_time != 0 &&
			Log_RotationAge > 0)
		{
			/*
			 * Do a logfile rotation if too much time has elapsed
			 * since the last one.
			 */
			pg_time_t   now = time(NULL);
			int         elapsed_secs = now - last_rotation_time;

			if (elapsed_secs >= Log_RotationAge * 60)
				rotation_requested = true;
		}

		if (!rotation_requested && Log_RotationSize > 0)
		{
			/*
			 * Do a rotation if file is too big
			 */
			if (ftell(syslogFile) >= Log_RotationSize * 1024L)
				rotation_requested = true;
		}

		if (rotation_requested)
			logfile_rotate();

#ifndef WIN32
		/*
		 * Wait for some data, timing out after 1 second
		 */
		FD_ZERO(&rfds);
		FD_SET(syslogPipe[0], &rfds);
		timeout.tv_sec=1;
		timeout.tv_usec=0;

		rc = select(syslogPipe[0]+1, &rfds, NULL, NULL, &timeout);

		if (rc < 0)
		{
			if (errno != EINTR)
				ereport(LOG,
						(errcode_for_socket_access(),
						 errmsg("select() failed in logger process: %m")));
		}
		else if (rc > 0 && FD_ISSET(syslogPipe[0], &rfds))
		{
		    bytesRead = piperead(syslogPipe[0],
								 logbuffer, sizeof(logbuffer));

			if (bytesRead < 0)
			{
				if (errno != EINTR)
					ereport(LOG,
							(errcode_for_socket_access(),
							 errmsg("could not read from logger pipe: %m")));
			}
			else if (bytesRead > 0)
			{
				write_syslogger_file(logbuffer, bytesRead);
				continue;
			}
			else
			{
				/*
				 * Zero bytes read when select() is saying read-ready
				 * means EOF on the pipe: that is, there are no longer
				 * any processes with the pipe write end open.  Therefore,
				 * the postmaster and all backends are shut down, and we
				 * are done.
				 */
				pipe_eof_seen = true;
			}
		}
#else /* WIN32 */
		/*
		 * On Windows we leave it to a separate thread to transfer data and
		 * detect pipe EOF.  The main thread just wakes up once a second to
		 * check for SIGHUP and rotation conditions.
		 */
		pgwin32_backend_usleep(1000000);
#endif /* WIN32 */

		if (pipe_eof_seen)
		{
			ereport(LOG,
					(errmsg("logger shutting down")));
			/*
			 * Normal exit from the syslogger is here.  Note that we
			 * deliberately do not close syslogFile before exiting;
			 * this is to allow for the possibility of elog messages
			 * being generated inside proc_exit.  Regular exit() will
			 * take care of flushing and closing stdio channels.
			 */
			proc_exit(0);
		}
	}
}

/*
 * Postmaster subroutine to start a syslogger subprocess.
 */
int
SysLogger_Start(void)
{
    pid_t sysloggerPid;
	pg_time_t now;
	char *filename;

	if (!Redirect_stderr)
	    return 0;

	/*
	 * If first time through, create the pipe which will receive stderr output.
	 *
	 * If the syslogger crashes and needs to be restarted, we continue to use
	 * the same pipe (indeed must do so, since extant backends will be writing
	 * into that pipe).
	 *
	 * This means the postmaster must continue to hold the read end of the
	 * pipe open, so we can pass it down to the reincarnated syslogger.
	 * This is a bit klugy but we have little choice.
	 */
#ifndef WIN32
    if (syslogPipe[0] < 0)
	{
	    if (pgpipe(syslogPipe) < 0)
		    ereport(FATAL,
					(errcode_for_socket_access(),
					 (errmsg("could not create pipe for syslogging: %m"))));
	}
#else
    if (!syslogPipe[0])
	{
		SECURITY_ATTRIBUTES sa;

		memset(&sa, 0, sizeof(SECURITY_ATTRIBUTES));
		sa.nLength = sizeof(SECURITY_ATTRIBUTES);
		sa.bInheritHandle = TRUE;

		if (!CreatePipe(&syslogPipe[0], &syslogPipe[1], &sa, 32768))
		    ereport(FATAL,
					(errcode_for_file_access(),
					 (errmsg("could not create pipe for syslogging: %m"))));
	}
#endif

	/*
	 * create log directory if not present; ignore errors
	 */
	if (is_absolute_path(Log_directory))
		mkdir(Log_directory, 0700);
	else
	{
		filename = palloc(MAXPGPATH);
		snprintf(filename, MAXPGPATH, "%s/%s", DataDir, Log_directory);
		mkdir(filename, 0700);
		pfree(filename);
	}

	/*
	 * The initial logfile is created right in the postmaster,
	 * to verify that the Log_directory is writable.
	 */
	now = time(NULL);
	filename = logfile_getname(now);

	syslogFile = fopen(filename, "a");

	if (!syslogFile)
	    ereport(FATAL,
				(errcode_for_file_access(),
				 (errmsg("could not create logfile \"%s\": %m",
						 filename))));

	setvbuf(syslogFile, NULL, LBF_MODE, 0);

	pfree(filename);

	/*
	 * Now we can fork off the syslogger subprocess.
	 */
	fflush(stdout);
	fflush(stderr);

#ifdef __BEOS__
	/* Specific beos actions before backend startup */
	beos_before_backend_startup();
#endif

#ifdef EXEC_BACKEND
	switch ((sysloggerPid = syslogger_forkexec()))
#else
	switch ((sysloggerPid = fork()))
#endif
	{
		case -1:
#ifdef __BEOS__
			/* Specific beos actions */
			beos_backend_startup_failed();
#endif
			ereport(LOG,
					(errmsg("could not fork system logger: %m")));
			return 0;

#ifndef EXEC_BACKEND
		case 0:
			/* in postmaster child ... */
#ifdef __BEOS__
			/* Specific beos actions after backend startup */
			beos_backend_startup();
#endif
			/* Close the postmaster's sockets */
			ClosePostmasterPorts(true);

			/* Drop our connection to postmaster's shared memory, as well */
			PGSharedMemoryDetach();

			/* do the work */
			SysLoggerMain(0, NULL);
			break;
#endif

		default:
			/* success, in postmaster */

			/* now we redirect stderr, if not done already */
			if (!redirection_done)
			{
#ifndef WIN32
				fflush(stdout);
				if (dup2(syslogPipe[1], fileno(stdout)) < 0)
					ereport(FATAL,
							(errcode_for_file_access(),
							 errmsg("could not redirect stdout: %m")));
				fflush(stderr);
				if (dup2(syslogPipe[1], fileno(stderr)) < 0)
					ereport(FATAL,
							(errcode_for_file_access(),
							 errmsg("could not redirect stderr: %m")));
				/* Now we are done with the write end of the pipe. */
				close(syslogPipe[1]);
				syslogPipe[1] = -1;
#else
				fflush(stderr);
				if (dup2(_open_osfhandle((long)syslogPipe[1],
										 _O_APPEND | _O_TEXT),
						 _fileno(stderr)) < 0)
					ereport(FATAL,
							(errcode_for_file_access(),
							 errmsg("could not redirect stderr: %m")));
				/* Now we are done with the write end of the pipe. */
				CloseHandle(syslogPipe[1]);
				syslogPipe[1] = 0;
#endif
				redirection_done = true;
			}

			/* postmaster will never write the file; close it */
			fclose(syslogFile);
			syslogFile = NULL;
			return (int) sysloggerPid;
	}

	/* we should never reach here */
	return 0;
}


#ifdef EXEC_BACKEND

/*
 * syslogger_forkexec() -
 *
 * Format up the arglist for, then fork and exec, a syslogger process
 */
static pid_t
syslogger_forkexec(void)
{
	char *av[10];
	int ac = 0, bufc = 0, i;
	char numbuf[2][32];

	av[ac++] = "postgres";
	av[ac++] = "-forklog";
	av[ac++] = NULL;			/* filled in by postmaster_forkexec */

	/* static variables (those not passed by write_backend_variables) */
#ifndef WIN32
	if (syslogFile != NULL)
	    snprintf(numbuf[bufc++], 32, "%d", fileno(syslogFile));
    else
	    strcpy(numbuf[bufc++], "-1");
	snprintf(numbuf[bufc++], 32, "%d", (int) redirection_done);
#else  /* WIN32 */
	if (syslogFile != NULL)
	    snprintf(numbuf[bufc++], 32, "%ld",
				 _get_osfhandle(_fileno(syslogFile)));
    else
	    strcpy(numbuf[bufc++], "0");
	snprintf(numbuf[bufc++], 32, "%d", (int) redirection_done);
#endif /* WIN32 */

	/* Add to the arg list */
	Assert(bufc <= lengthof(numbuf));
	for (i = 0; i < bufc; i++)
		av[ac++] = numbuf[i];

	av[ac] = NULL;
	Assert(ac < lengthof(av));

	return postmaster_forkexec(ac, av);
}

/*
 * syslogger_parseArgs() -
 *
 * Extract data from the arglist for exec'ed syslogger process
 */
static void
syslogger_parseArgs(int argc, char *argv[])
{
    int fd;

	Assert(argc == 5);
	argv += 3;

#ifndef WIN32
	fd = atoi(*argv++);
	if (fd != -1)
	{
	    syslogFile = fdopen(fd, "a");
		setvbuf(syslogFile, NULL, LBF_MODE, 0);
	}
	redirection_done = (bool) atoi(*argv++);
#else  /* WIN32 */
	fd = atoi(*argv++);
	if (fd != 0)
	{
		fd = _open_osfhandle(fd, _O_APPEND);
		if (fd != 0)
		{
			syslogFile = fdopen(fd, "a");
			setvbuf(syslogFile, NULL, LBF_MODE, 0);
		}
	}
	redirection_done = (bool) atoi(*argv++);
#endif /* WIN32 */
}

#endif /* EXEC_BACKEND */


/* --------------------------------
 *		logfile routines
 * --------------------------------
 */

/*
 * Write to the currently open logfile
 *
 * This is exported so that elog.c can call it when am_syslogger is true.
 * This allows the syslogger process to record elog messages of its own,
 * even though its stderr does not point at the syslog pipe.
 */
void
write_syslogger_file(const char *buffer, int count)
{
    int rc;

#ifndef WIN32
    rc = fwrite(buffer, 1, count, syslogFile);
#else
    EnterCriticalSection(&sysfileSection);
    rc = fwrite(buffer, 1, count, syslogFile);
    LeaveCriticalSection(&sysfileSection);
#endif

    if (rc != count)
        ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write to logfile: %m")));
}

#ifdef WIN32

/*
 * Worker thread to transfer data from the pipe to the current logfile.
 *
 * We need this because on Windows, WaitForSingleObject does not work on
 * unnamed pipes: it always reports "signaled", so the blocking ReadFile won't
 * allow for SIGHUP; and select is for sockets only.
 */
static unsigned int __stdcall
pipeThread(void *arg)
{
    DWORD bytesRead;
    char    logbuffer[1024];

    for (;;)
    {
        if (!ReadFile(syslogPipe[0], logbuffer, sizeof(logbuffer),
					  &bytesRead, 0))
		{
			DWORD error = GetLastError();

			if (error == ERROR_HANDLE_EOF ||
				error == ERROR_BROKEN_PIPE)
				break;
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not read from logger pipe: %m")));
		}
        else if (bytesRead > 0)
            write_syslogger_file(logbuffer, bytesRead);
    }

	/* We exit the above loop only upon detecting pipe EOF */
	pipe_eof_seen = true;
    _endthread();
    return 0;
}

#endif /* WIN32 */

/*
 * perform logfile rotation
 */
static void
logfile_rotate(void)
{
	char *filename;
	pg_time_t now;
	FILE *fh;

	now = time(NULL);
	filename = logfile_getname(now);

	fh = fopen(filename, "a");
	if (!fh)
	{
		int	saveerrno = errno;

		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open new logfile \"%s\": %m",
						filename)));

		/*
		 * ENFILE/EMFILE are not too surprising on a busy system; just keep
		 * using the old file till we manage to get a new one.  Otherwise,
		 * assume something's wrong with Log_directory and stop trying to
		 * create files.
		 */
		if (saveerrno != ENFILE && saveerrno != EMFILE)
		{
			ereport(LOG,
				(errmsg("disabling auto rotation (use SIGHUP to reenable)")));
			Log_RotationAge = 0;
			Log_RotationSize = 0;
		}
		pfree(filename);
		return;
	}

	setvbuf(fh, NULL, LBF_MODE, 0);

	/* On Windows, need to interlock against data-transfer thread */
#ifdef WIN32
	EnterCriticalSection(&sysfileSection);
#endif
	fclose(syslogFile);
	syslogFile = fh;
#ifdef WIN32
	LeaveCriticalSection(&sysfileSection);
#endif

	last_rotation_time = now;

	pfree(filename);
}


/*
 * construct logfile name using timestamp information
 *
 * Result is palloc'd.
 */
static char*
logfile_getname(pg_time_t timestamp)
{
	char *filename;
	char stamptext[128];

	pg_strftime(stamptext, sizeof(stamptext), "%Y-%m-%d_%H%M%S",
				pg_localtime(&timestamp));

	filename = palloc(MAXPGPATH);

	if (is_absolute_path(Log_directory))
		snprintf(filename, MAXPGPATH, "%s/%s%05u_%s.log",
				 Log_directory, Log_filename_prefix,
				 (unsigned int) PostmasterPid, stamptext);
	else
		snprintf(filename, MAXPGPATH, "%s/%s/%s%05u_%s.log",
				 DataDir, Log_directory, Log_filename_prefix,
				 (unsigned int) PostmasterPid, stamptext);

	return filename;
}

/* --------------------------------
 *		signal handler routines
 * --------------------------------
 */

/* SIGHUP: set flag to reload config file */
static void
sigHupHandler(SIGNAL_ARGS)
{
    got_SIGHUP = true;
}
