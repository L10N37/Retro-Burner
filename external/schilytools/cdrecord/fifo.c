/* @(#)fifo.c	1.66 15/04/22 Copyright 1989,1997-2015 J. Schilling */
#include <retroburner/platform.h>
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#endif
#include <stdint.h>
#include <schily/mconfig.h>
#ifndef lint
static	UConst char sccsid[] =
	"@(#)fifo.c	1.66 15/04/22 Copyright 1989,1997-2015 J. Schilling";
#endif
/*
 *	A "fifo" that uses shared memory between two processes
 *
 *	The actual code is a mixture of borrowed code from star's fifo.c
 *	and a proposal from Finn Arne Gangstad <finnag@guardian.no>
 *	who had the idea to use a ring buffer to handle average size chunks.
 *
 *	Copyright (c) 1989,1997-2015 J. Schilling
 */
/*
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * See the file CDDL.Schily.txt in this distribution for details.
 * A copy of the CDDL is also available via the Internet at
 * http://www.opensource.org/licenses/cddl1.txt
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file CDDL.Schily.txt from this distribution.
 */

#ifndef	DEBUG
#define	DEBUG
#endif
/*#define	XDEBUG*/
#include <schily/mconfig.h>
#if	defined(HAVE_OS_H) && \
	defined(HAVE_CLONE_AREA) && defined(HAVE_CREATE_AREA) && \
	defined(HAVE_DELETE_AREA)
#include <OS.h>
#	define	HAVE_BEOS_AREAS	/* BeOS/Zeta/Haiku */
#endif
/*
 * RB_RETROBURNER_WIN32_FIFO_01
 *
 * Native MinGW has no POSIX fork(), but a producer thread is sufficient for
 * cdrecord's ring buffer because both sides intentionally share the FIFO.
 * Keep every original POSIX/Cygwin path unchanged.
 */
#if	defined(__MINGW32__) && !defined(__CYGWIN__)
#define	USE_WIN32_THREAD_FIFO	1
#endif

#if	!defined(USE_WIN32_THREAD_FIFO)
#if	!defined(HAVE_SMMAP) && !defined(HAVE_USGSHM) && \
	!defined(HAVE_DOSALLOCSHAREDMEM) && !defined(HAVE_BEOS_AREAS)
#undef	FIFO			/* We cannot have a FIFO on this platform */
#endif
#if	!defined(HAVE_FORK)
#undef	FIFO			/* We cannot have a FIFO on this platform */
#endif
#endif

#ifdef	FIFO
#if	!defined(USE_WIN32_THREAD_FIFO)
#if !defined(USE_MMAP) && !defined(USE_USGSHM)
#define	USE_MMAP
#endif
#ifndef	HAVE_SMMAP
#	undef	USE_MMAP
#	define	USE_USGSHM	/* now SYSV shared memory is the default*/
#endif
#ifdef	USE_MMAP		/* Only want to have one implementation */
#	undef	USE_USGSHM	/* mmap() is preferred			*/
#endif
#endif	/* !USE_WIN32_THREAD_FIFO */

#ifdef	HAVE_DOSALLOCSHAREDMEM	/* This is for OS/2 */
#	undef	USE_MMAP
#	undef	USE_USGSHM
#	define	USE_OS2SHM
#endif

#ifdef	HAVE_BEOS_AREAS		/* This is for BeOS/Zeta */
#	undef	USE_MMAP
#	undef	USE_USGSHM
#	undef	USE_OS2SHM
#	define	USE_BEOS_AREAS
#endif

#if	defined(USE_WIN32_THREAD_FIFO)
#include <schily/windows.h>
#include <process.h>
#endif

#include <schily/stdio.h>
#include <schily/stdlib.h>
#include <schily/unistd.h>	/* includes <sys/types.h> */
#include <schily/utypes.h>
#include <schily/fcntl.h>
#if defined(HAVE_SMMAP) && defined(USE_MMAP)
#include <schily/mman.h>
#endif
#include <schily/wait.h>
#include <schily/standard.h>
#include <schily/errno.h>
#include <schily/signal.h>
#include <schily/libport.h>
#include <schily/schily.h>
#include <schily/nlsdefs.h>
#include <schily/vfork.h>

#include "cdrecord.h"
#include "xio.h"

#ifdef DEBUG
#ifdef XDEBUG
FILE	*ef;
#define	USDEBUG1	if (debug) {if (s == owner_reader) fprintf(ef, "r"); else fprintf(ef, "w"); fflush(ef); }
#define	USDEBUG2	if (debug) {if (s == owner_reader) fprintf(ef, "R"); else fprintf(ef, "W"); fflush(ef); }
#else
#define	USDEBUG1
#define	USDEBUG2
#endif
#define	EDEBUG(a)	if (debug) error a
#else
#define	EDEBUG(a)
#define	USDEBUG1
#define	USDEBUG2
#endif

#define	palign(x, a)	(((char *)(x)) + ((a) - 1 - (((uintptr_t)((x)-1))%(a))))

typedef enum faio_owner {
	owner_none,		/* Unused in real life			    */
	owner_writer,		/* owned by process that writes into FIFO   */
	owner_faio,		/* Intermediate state when buf still in use */
	owner_reader		/* owned by process that reads from FIFO    */
} fowner_t;

char	*onames[] = {
	"none",
	"writer",
	"faio",
	"reader",
};

typedef struct faio {
	int	len;
	volatile fowner_t owner;
	volatile int users;
	short	fd;
	short	saved_errno;
	char	*bufp;
} faio_t;

#if	defined(USE_WIN32_THREAD_FIFO)
#define	RB_FIFO_SHARED	volatile
#define	RB_FIFO_MEMORY_BARRIER()	__sync_synchronize()
#else
#define	RB_FIFO_SHARED
#define	RB_FIFO_MEMORY_BARRIER()
#endif

struct faio_stats {
	RB_FIFO_SHARED long	puts;
	RB_FIFO_SHARED long	gets;
	RB_FIFO_SHARED long	empty;
	RB_FIFO_SHARED long	full;
	RB_FIFO_SHARED long	done;
	RB_FIFO_SHARED long	cont_low;
	RB_FIFO_SHARED int	users;
} *sp;

#define	MIN_BUFFERS	3

#define	MSECS	1000
#define	SECS	(1000*MSECS)

/*
 * Note: WRITER_MAXWAIT & READER_MAXWAIT need to be greater than the SCSI
 * timeout for commands that write to the media. This is currently 200s
 * if we are in SAO mode.
 */
/* microsecond delay between each buffer-ready probe by writing process */
#define	WRITER_DELAY	(20*MSECS)
#define	WRITER_MAXWAIT	(240*SECS)	/* 240 seconds max wait for data */

/* microsecond delay between each buffer-ready probe by reading process */
#define	READER_DELAY	(80*MSECS)
#define	READER_MAXWAIT	(240*SECS)	/* 240 seconds max wait for reader */

LOCAL	char	*buf;
LOCAL	char	*bufbase;
LOCAL	char	*bufend;
LOCAL	long	buflen;			/* The size of the FIFO buffer */

extern	int	debug;
extern	int	lverbose;

EXPORT	long	init_fifo	__PR((long));
#if	defined(USE_WIN32_THREAD_FIFO)
LOCAL	char	*mkwinshare	__PR((int size));
LOCAL	unsigned __stdcall rb_fifo_selftest_worker(void *arg);
EXPORT	BOOL	rb_fifo_selftest	__PR((void));
#endif
#ifdef	USE_MMAP
LOCAL	char	*mkshare	__PR((int size));
#endif
#ifdef	USE_USGSHM
LOCAL	char	*mkshm		__PR((int size));
#endif
#ifdef	USE_OS2SHM
LOCAL	char	*mkos2shm	__PR((int size));
#endif
#ifdef	USE_BEOS_AREAS
LOCAL	char	*mkbeosshm	__PR((int size));
LOCAL	void	beosshm_child	__PR((void));
#endif

EXPORT	BOOL	init_faio	__PR((track_t *trackp, int));
EXPORT	BOOL	await_faio	__PR((void));
EXPORT	void	kill_faio	__PR((void));
EXPORT	int	wait_faio	__PR((void));
LOCAL	void	faio_reader	__PR((track_t *trackp));
LOCAL	void	faio_read_track	__PR((track_t *trackp));
LOCAL	void	faio_wait_on_buffer __PR((faio_t *f, fowner_t s,
					unsigned long delay,
					unsigned long max_wait));
LOCAL	int	faio_read_segment __PR((int fd, faio_t *f, track_t *track, long secno, int len));
LOCAL	faio_t	*faio_ref	__PR((int n));
EXPORT	int	faio_read_buf	__PR((int f, char *bp, int size));
EXPORT	int	faio_get_buf	__PR((int f, char **bpp, int size));
EXPORT	void	fifo_stats	__PR((void));
EXPORT	int	fifo_percent	__PR((BOOL addone));


EXPORT long
init_fifo(fs)
	long	fs;
{
	int	pagesize;
#if	defined(USE_WIN32_THREAD_FIFO)
	SYSTEM_INFO	rb_si;
#endif

	if (fs == 0L)
		return (fs);

#if	defined(USE_WIN32_THREAD_FIFO)
	GetSystemInfo(&rb_si);
	pagesize = (int)rb_si.dwPageSize;
#else
#if	defined(USE_WIN32_THREAD_FIFO)
	GetSystemInfo(&rb_si);
	pagesize = (int)rb_si.dwPageSize;
#else
#ifdef	_SC_PAGESIZE
	pagesize = sysconf(_SC_PAGESIZE);
#else
	pagesize = rb_page_size();
#endif
#endif
#endif
	buflen = roundup(fs, pagesize) + pagesize;
	EDEBUG(("fs: %ld buflen: %ld\n", fs, buflen));

#if	defined(USE_WIN32_THREAD_FIFO)
	buf = mkwinshare(buflen);
#endif
#if	defined(USE_MMAP)
	buf = mkshare(buflen);
#endif
#if	defined(USE_USGSHM)
	buf = mkshm(buflen);
#endif
#if	defined(USE_OS2SHM)
	buf = mkos2shm(buflen);
#endif
#if	defined(USE_BEOS_AREAS)
	buf = mkbeosshm(buflen);
#endif

	bufbase = buf;
	bufend = buf + buflen;
	EDEBUG(("buf: %p bufend: %p, buflen: %ld\n", buf, bufend, buflen));
	buf = palign(buf, pagesize);
	buflen -= buf - bufbase;
	EDEBUG(("buf: %p bufend: %p, buflen: %ld (align %ld)\n", buf, bufend, buflen, (long)(buf - bufbase)));

	/*
	 * Dirty the whole buffer. This can die with various signals if
	 * we're trying to lock too much memory
	 */
	fillbytes(buf, buflen, '\0');

#ifdef	XDEBUG
	if (debug)
		ef = fopen("/tmp/ef", "w");
#endif
	return (buflen-pagesize); /* We use one page for administrative data */
}

#if	defined(USE_WIN32_THREAD_FIFO)
LOCAL char *
mkwinshare(size)
	int	size;
{
	char	*addr;

	addr = (char *)VirtualAlloc(NULL, (SIZE_T)size,
	    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (addr == NULL)
		comerr(_("Cannot allocate %d Bytes for Win32 FIFO.\n"), size);

	if (debug)
		errmsgno(EX_BAD,
		    _("Win32 FIFO memory allocated at: %p size %d\n"),
		    (void *)addr, size);

	return (addr);
}

/*
 * RB_RETROBURNER_WIN32_FIFO_SELFTEST_01
 *
 * No optical device is touched. This verifies the two native primitives that
 * replace fork()+shared-memory on MinGW: one 32 MiB shared address space and
 * one CRT-aware worker thread.
 */
LOCAL unsigned __stdcall
rb_fifo_selftest_worker(void *arg)
{
	unsigned char	*p;
	int	i;

	p = (unsigned char *)arg;
	for (i = 0; i < 4096; i++)
		p[i] = (unsigned char)(((i * 37) + 0x5A) & 0xFF);

	RB_FIFO_MEMORY_BARRIER();
	return (0);
}

EXPORT BOOL
rb_fifo_selftest()
{
	HANDLE	th;
	unsigned tid;
	DWORD	waitret;
	DWORD	exitcode;
	long	allocated;
	unsigned char	*p;
	int	i;

	allocated = init_fifo(32L * 1024L * 1024L);
	if (allocated < (32L * 1024L * 1024L))
		return (FALSE);

	p = (unsigned char *)buf;
	fillbytes((char *)p, 4096, '\0');

	th = (HANDLE)_beginthreadex(NULL, 0,
	    rb_fifo_selftest_worker, p, 0, &tid);
	if (th == (HANDLE)0)
		return (FALSE);

	waitret = WaitForSingleObject(th, 10000);
	if (waitret != WAIT_OBJECT_0) {
		CloseHandle(th);
		return (FALSE);
	}

	exitcode = 1;
	if (!GetExitCodeThread(th, &exitcode)) {
		CloseHandle(th);
		return (FALSE);
	}
	CloseHandle(th);

	if (exitcode != 0)
		return (FALSE);

	RB_FIFO_MEMORY_BARRIER();
	for (i = 0; i < 4096; i++) {
		if (p[i] != (unsigned char)(((i * 37) + 0x5A) & 0xFF))
			return (FALSE);
	}

	printf("RetroBeam Win32 FIFO self-test: PASS allocated=%ld bytes thread/shared-memory=PASS\n",
	    allocated);
	return (TRUE);
}
#endif	/* USE_WIN32_THREAD_FIFO */
#ifdef	USE_MMAP
LOCAL char *
mkshare(size)
	int	size;
{
	int	f;
	char	*addr;

#ifdef	MAP_ANONYMOUS	/* HP/UX */
	f = -1;
	addr = mmap(0, mmap_sizeparm(size),
			PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, f, 0);
#else
	if ((f = open("/dev/zero", O_RDWR)) < 0)
		comerr(_("Cannot open '/dev/zero'.\n"));
	addr = mmap(0, mmap_sizeparm(size),
			PROT_READ|PROT_WRITE, MAP_SHARED, f, 0);
#endif
	if (addr == (char *)-1)
		comerr(_("Cannot get mmap for %d Bytes on /dev/zero.\n"), size);
	if (f >= 0)
		close(f);

	if (debug) errmsgno(EX_BAD, _("shared memory segment attached at: %p size %d\n"),
				(void *)addr, size);

	return (addr);
}
#endif

#ifdef	USE_USGSHM
#include <schily/ipc.h>
#include <schily/shm.h>
LOCAL char *
mkshm(size)
	int	size;
{
	int	id;
	char	*addr;
	/*
	 * Unfortunately, a declaration of shmat() is missing in old
	 * implementations such as AT&T SVr0 and SunOS.
	 * We cannot add this definition here because the return-type
	 * changed on newer systems.
	 *
	 * We will get a warning like this:
	 *
	 * warning: assignment of pointer from integer lacks a cast
	 * or
	 * warning: illegal combination of pointer and integer, op =
	 */
/*	extern	char *shmat();*/

	if ((id = shmget(IPC_PRIVATE, size, IPC_CREAT|0600)) == -1)
		comerr(_("shmget failed\n"));

	if (debug) errmsgno(EX_BAD, _("shared memory segment allocated: %d\n"), id);

	if ((addr = shmat(id, (char *)0, 0600)) == (char *)-1)
		comerr(_("shmat failed\n"));

	if (debug) errmsgno(EX_BAD, _("shared memory segment attached at: %p size %d\n"),
				(void *)addr, size);

	if (shmctl(id, IPC_RMID, 0) < 0)
		comerr(_("shmctl failed to detach shared memory segment\n"));

#ifdef	SHM_LOCK
	/*
	 * Although SHM_LOCK is standard, it seems that all versions of AIX
	 * ommit this definition.
	 */
	if (shmctl(id, SHM_LOCK, 0) < 0)
		comerr(_("shmctl failed to lock shared memory segment\n"));
#endif

	return (addr);
}
#endif

#ifdef	USE_OS2SHM
LOCAL char *
mkos2shm(size)
	int	size;
{
	char	*addr;

	/*
	 * The OS/2 implementation of shm (using shm.dll) limits the size of one shared
	 * memory segment to 0x3fa000 (aprox. 4MBytes). Using OS/2 native API we have
	 * no such restriction so I decided to use it allowing fifos of arbitrary size.
	 */
	if (DosAllocSharedMem(&addr, NULL, size, 0X100L | 0x1L | 0x2L | 0x10L))
		comerr(_("DosAllocSharedMem() failed\n"));

	if (debug) errmsgno(EX_BAD, _("shared memory allocated attached at: %p size %d\n"),
				(void *)addr, size);

	return (addr);
}
#endif

#ifdef	USE_BEOS_AREAS
LOCAL	area_id	faio_aid;
LOCAL	void	*faio_addr;
LOCAL	char	faio_name[32];

LOCAL char *
mkbeosshm(size)
	int	size;
{
	snprintf(faio_name, sizeof (faio_name), "cdrecord FIFO %lld",
		(int64_t)getpid());

	faio_aid = create_area(faio_name, &faio_addr,
			B_ANY_ADDRESS,
			size,
			B_NO_LOCK, B_READ_AREA|B_WRITE_AREA);
	if (faio_addr == NULL) {
		comerrno(faio_aid,
			_("Cannot get create_area for %d Bytes FIFO.\n"), size);
	}
	if (debug) errmsgno(EX_BAD, _("shared memory allocated attached at: %p size %d\n"),
				(void *)faio_addr, size);
	return (faio_addr);
}

LOCAL void
beosshm_child()
{
	/*
	 * Delete the area created by fork that is copy-on-write.
	 */
	delete_area(area_for(faio_addr));
	/*
	 * Clone (share) the original one.
	 * The original implementaion used B_ANY_ADDRESS, but newer Haiku
	 * versions implement address randomization that prevents us from
	 * using the pointer in the child. So we noe use B_EXACT_ADDRESS.
	 */
	faio_aid = clone_area(faio_name, &faio_addr,
			B_EXACT_ADDRESS, B_READ_AREA|B_WRITE_AREA,
			faio_aid);
	if (bufbase != faio_addr) {
		comerrno(EX_BAD, _("Panic FIFO addr.\n"));
		/* NOTREACHED */
	}
}
#endif

LOCAL	int	faio_buffers;
LOCAL	int	faio_buf_size;
LOCAL	int	buf_idx = 0;		/* Initialize to fix an Amiga bug   */
LOCAL	int	buf_idx_reader = 0;	/* Separate var to allow vfork()    */
					/* buf_idx_reader is for the process */
					/* that fills the FIFO		    */
LOCAL	pid_t	faio_pid;
LOCAL	BOOL	faio_didwait;
#if	defined(USE_WIN32_THREAD_FIFO)
LOCAL	HANDLE	faio_thread = NULL;
LOCAL	unsigned faio_thread_id;
LOCAL	unsigned __stdcall faio_reader_thread(void *arg);
#endif

#ifdef AMIGA
/*
 * On Amiga fork will be replaced by the speciall vfork() like call ix_vfork,
 * which lets the parent asleep. The child process later wakes up the parent
 * process by calling ix_fork_resume().
 */
#define	fork()		 ix_vfork()
#define	__vfork_resume() ix_vfork_resume()

#else	/* !AMIGA */
#define	__vfork_resume()
#endif


/*#define	faio_ref(n)	(&((faio_t *)buf)[n])*/


#if	defined(USE_WIN32_THREAD_FIFO)
LOCAL unsigned __stdcall
faio_reader_thread(void *arg)
{
	faio_reader((track_t *)arg);
	return (0);
}
#endif
EXPORT BOOL
init_faio(trackp, bufsize)
	track_t	*trackp;
	int	bufsize;	/* The size of a single transfer buffer */
{
	int	n;
	faio_t	*f;
	int	pagesize;
	char	*base;
#if	defined(USE_WIN32_THREAD_FIFO)
	SYSTEM_INFO	rb_si;
#endif

	if (buflen == 0L)
		return (FALSE);

#ifdef	_SC_PAGESIZE
	pagesize = sysconf(_SC_PAGESIZE);
#else
	pagesize = rb_page_size();
#endif

	faio_buf_size = bufsize;
	f = (faio_t *)buf;

	/*
	 * Compute space for buffer headers.
	 * Round bufsize up to pagesize to make each FIFO segment
	 * properly page aligned.
	 */
	bufsize = roundup(bufsize, pagesize);
	faio_buffers = (buflen - sizeof (*sp)) / bufsize;
	EDEBUG(("bufsize: %d buffers: %d hdrsize %ld\n", bufsize, faio_buffers, (long)faio_buffers * sizeof (struct faio)));

	/*
	 * Reduce buffer space by header space.
	 */
	n = sizeof (*sp) + faio_buffers * sizeof (struct faio);
	n = roundup(n, pagesize);
	faio_buffers = (buflen-n) / bufsize;
	EDEBUG(("bufsize: %d buffers: %d hdrsize %ld\n", bufsize, faio_buffers, (long)faio_buffers * sizeof (struct faio)));

	if (faio_buffers < MIN_BUFFERS) {
		errmsgno(EX_BAD,
			_("write-buffer too small, minimum is %dk. Disabling.\n"),
						MIN_BUFFERS*bufsize/1024);
		return (FALSE);
	}

	if (debug)
		printf(_("Using %d buffers of %d bytes.\n"), faio_buffers, faio_buf_size);

	f = (faio_t *)buf;
	base = buf + roundup(sizeof (*sp) + faio_buffers * sizeof (struct faio),
				pagesize);

	for (n = 0; n < faio_buffers; n++, f++, base += bufsize) {
		/* Give all the buffers to the file reader process */
		f->owner = owner_writer;
		f->users = 0;
		f->bufp = base;
		f->fd = -1;
	}
	sp = (struct faio_stats *)f;	/* point past headers */
	sp->gets = sp->puts = sp->done = 0L;
	sp->users = 1;

#if	defined(USE_WIN32_THREAD_FIFO)
	faio_thread = (HANDLE)_beginthreadex(NULL, 0,
	    faio_reader_thread, trackp, 0, &faio_thread_id);
	if (faio_thread == (HANDLE)0)
		comerr(_("_beginthreadex() failed starting Win32 FIFO reader"));

	/*
	 * Match the intent of the original child raisepri(1), but apply the
	 * priority to the background reader thread rather than the whole process.
	 */
	SetThreadPriority(faio_thread, THREAD_PRIORITY_HIGHEST);
	faio_didwait = FALSE;
#else
	faio_pid = fork();
	if (faio_pid < 0)
		comerr(_("fork(2) failed"));

	if (faio_pid == 0) {
		/*
		 * child (background) process that fills the FIFO.
		 */
		raisepri(1);		/* almost max priority */

#ifdef USE_OS2SHM
		DosGetSharedMem(buf, 3); /* PAG_READ|PAG_WRITE */
#endif
#ifdef	USE_BEOS_AREAS
		beosshm_child();
#endif
		/* Ignoring SIGALRM cures the SCO rb_sleep_us() bug */
/*		signal(SIGALRM, SIG_IGN);*/
		__vfork_resume();	/* Needed on some platforms */
		faio_reader(trackp);
		/* NOTREACHED */
	} else {
#ifdef	__needed__
		unsigned int	t;
#endif

		faio_didwait = FALSE;

		/*
		 * XXX We used to close all track files in the foreground
		 * XXX process. This was not correct before we used "xio"
		 * XXX and with "xio" it will start to fail because we need
		 * XXX the fd handles for the faio_get_buf() function.
		 */
#ifdef	__needed__
		/* close all file-descriptors that only the child will use */
		for (t = 1; t <= trackp->tracks; t++) {
			if (trackp[t].xfp != NULL)
				xclose(trackp[t].xfp);
		}
#endif
	}
#endif	/* USE_WIN32_THREAD_FIFO */

	return (TRUE);
}

EXPORT BOOL
await_faio()
{
	int	n;
	int	lastfd = -1;
	faio_t	*f;

	/*
	 * Wait until the reader is active and has filled the buffer.
	 */
	if (lverbose || debug) {
		printf(_("Waiting for reader process to fill input buffer ... "));
		flush();
	}

	faio_wait_on_buffer(faio_ref(faio_buffers - 1), owner_reader,
			    500*MSECS, 0);

	if (lverbose || debug)
		printf(_("input buffer ready.\n"));

	sp->empty = sp->full = 0L;	/* set correct stat state */
	sp->cont_low = faio_buffers;	/* set cont to max value  */

	f = faio_ref(0);
	for (n = 0; n < faio_buffers; n++, f++) {
		if (f->fd != lastfd &&
			f->fd == STDIN_FILENO && f->len == 0) {
			errmsgno(EX_BAD, _("Premature EOF on stdin.\n"));
			kill_faio();
			return (FALSE);
		}
		lastfd = f->fd;
	}
	return (TRUE);
}

EXPORT void
kill_faio()
{
#if	defined(USE_WIN32_THREAD_FIFO)
	if (faio_thread != NULL) {
		/*
		 * Upstream uses SIGKILL here. The Win32 equivalent is confined to
		 * the same exceptional abort path; normal completion uses wait_faio.
		 */
		TerminateThread(faio_thread, 1);
		WaitForSingleObject(faio_thread, 5000);
		CloseHandle(faio_thread);
		faio_thread = NULL;
		faio_didwait = TRUE;
	}
#else
	if (faio_pid > 0)
		kill(faio_pid, SIGKILL);
#endif
}

EXPORT int
wait_faio()
{
#if	defined(USE_WIN32_THREAD_FIFO)
	DWORD	waitret;
	DWORD	exitcode;

	if (faio_thread != NULL && !faio_didwait) {
		waitret = WaitForSingleObject(faio_thread, INFINITE);
		if (waitret != WAIT_OBJECT_0)
			return (-1);

		exitcode = 1;
		if (!GetExitCodeThread(faio_thread, &exitcode))
			exitcode = 1;

		CloseHandle(faio_thread);
		faio_thread = NULL;
		faio_didwait = TRUE;
		return (exitcode == 0 ? 0 : -1);
	}
	faio_didwait = TRUE;
	return (0);
#else
	if (faio_pid > 0 && !faio_didwait)
		return (wait(0));
	faio_didwait = TRUE;
	return (0);
#endif
}

LOCAL void
faio_reader(trackp)
	track_t	*trackp;
{
	/* This function should not return, but _exit. */
	unsigned int	trackno;

	if (debug)
		printf(_("\nfaio_reader starting\n"));

	for (trackno = 0; trackno <= trackp->tracks; trackno++) {
		if (trackno == 0 && trackp[0].xfp == NULL)
			continue;
		if (debug)
			printf(_("\nfaio_reader reading track %u\n"), trackno);
		faio_read_track(&trackp[trackno]);
	}
	sp->done++;
	if (debug)
		printf(_("\nfaio_reader all tracks read, exiting\n"));

	/* Prevent hang if buffer is larger than all the tracks combined */
	if (sp->gets == 0)
		faio_ref(faio_buffers - 1)->owner = owner_reader;

#ifdef	USE_OS2SHM
	DosFreeMem(buf);
	rb_sleep_seconds(30000);	/* XXX If calling _exit() here the parent process seems to be blocked */
			/* XXX This should be fixed soon */
#endif
#if	defined(USE_WIN32_THREAD_FIFO)
	if (debug)
		error(_("\nfaio_reader thread return\n"));
	return;
#else
	if (debug)
		error(_("\nfaio_reader _exit(0)\n"));
	_exit(0);
#endif
}

#ifndef	faio_ref
LOCAL faio_t *
faio_ref(n)
	int	n;
{
	return (&((faio_t *)buf)[n]);
}
#endif


LOCAL void
faio_read_track(trackp)
	track_t *trackp;
{
	int	fd = -1;
	int	bytespt = trackp->secsize * trackp->secspt;
	int	secspt = trackp->secspt;
	int	l;
	long	secno = trackp->trackstart;
	tsize_t	tracksize = trackp->tracksize;
	tsize_t	bytes_read = (tsize_t)0;
	long	bytes_to_read;

	if (trackp->xfp != NULL)
		fd = xfileno(trackp->xfp);

	if (bytespt > faio_buf_size) {
		comerrno(EX_BAD,
		_("faio_read_track fatal: secsize %d secspt %d, bytespt(%d) > %d !!\n"),
			trackp->secsize, trackp->secspt, bytespt,
			faio_buf_size);
	}

	do {
		bytes_to_read = bytespt;
		if (tracksize > 0) {
			if ((tracksize - bytes_read) > bytespt) {
				bytes_to_read = bytespt;
			} else {
				bytes_to_read = tracksize - bytes_read;
			}
		}
		l = faio_read_segment(fd, faio_ref(buf_idx_reader), trackp, secno, bytes_to_read);
		if (++buf_idx_reader >= faio_buffers)
			buf_idx_reader = 0;
		if (l <= 0)
			break;
		bytes_read += l;
		secno += secspt;
	} while (tracksize < 0 || bytes_read < tracksize);

#if	!defined(USE_WIN32_THREAD_FIFO)
	if (trackp->xfp != NULL) {
		xclose(trackp->xfp);	/* Don't keep files open longer than neccesary */
		trackp->xfp = NULL;
	}
#endif
}

LOCAL void
#ifdef	PROTOTYPES
faio_wait_on_buffer(faio_t *f, fowner_t s,
			unsigned long delay,
			unsigned long max_wait)
#else
faio_wait_on_buffer(f, s, delay, max_wait)
	faio_t	*f;
	fowner_t s;
	unsigned long delay;
	unsigned long max_wait;
#endif
{
	unsigned long max_loops;

	if (f->owner == s)
		return;		/* return immediately if the buffer is ours */

	if (s == owner_reader)
		sp->empty++;
	else
		sp->full++;

	max_loops = max_wait / delay + 1;

	while (max_wait == 0 || max_loops--) {
		USDEBUG1;
		rb_sleep_us(delay);
		USDEBUG2;

		if (f->owner == s)
			return;
	}
	if (debug) {
		errmsgno(EX_BAD,
		_("%lu microseconds passed waiting for %d current: %d idx: %ld\n"),
		max_wait, s, f->owner, (long)(f - faio_ref(0))/sizeof (*f));
	}
	comerrno(EX_BAD, _("faio_wait_on_buffer for %s timed out.\n"),
	(s > owner_reader || s < owner_none) ? "bad_owner" : onames[s-owner_none]);
}

LOCAL int
faio_read_segment(fd, f, trackp, secno, len)
	int	fd;
	faio_t	*f;
	track_t	*trackp;
	long	secno;
	int	len;
{
	int l;

	faio_wait_on_buffer(f, owner_writer, WRITER_DELAY, WRITER_MAXWAIT);

	f->fd = fd;
	l = fill_buf(fd, trackp, secno, f->bufp, len);
	f->len = l;
	f->saved_errno = geterrno();
	f->users = sp->users;
	RB_FIFO_MEMORY_BARRIER();
	f->owner = owner_reader;

	sp->puts++;

	return (l);
}

EXPORT int
faio_read_buf(fd, bp, size)
	int fd;
	char *bp;
	int size;
{
	char *bufp;

	int len = faio_get_buf(fd, &bufp, size);
	if (len > 0) {
		movebytes(bufp, bp, len);
	}
	return (len);
}

EXPORT int
faio_get_buf(fd, bpp, size)
	int fd;
	char **bpp;
	int size;
{
	faio_t	*f;
	int	len;

again:
	f = faio_ref(buf_idx);
	if (f->owner == owner_faio) {
		f->owner = owner_writer;
		if (++buf_idx >= faio_buffers)
			buf_idx = 0;
		f = faio_ref(buf_idx);
	}

	if ((sp->puts - sp->gets) < sp->cont_low && sp->done == 0) {
		EDEBUG(("gets: %ld puts: %ld cont: %ld low: %ld\n", sp->gets, sp->puts, sp->puts - sp->gets, sp->cont_low));
		sp->cont_low = sp->puts - sp->gets;
	}
	faio_wait_on_buffer(f, owner_reader, READER_DELAY, READER_MAXWAIT);
	RB_FIFO_MEMORY_BARRIER();
	len = f->len;

	if (f->fd != fd) {
		if (f->len == 0) {
			/*
			 * If the tracksize for this track was known, and
			 * the tracksize is 0 mod bytespt, this happens.
			 */
			goto again;
		}
		comerrno(EX_BAD,
		_("faio_get_buf fatal: fd=%d, f->fd=%d, f->len=%d f->errno=%d\n"),
		fd, f->fd, f->len, f->saved_errno);
	}
	if (size < len) {
		comerrno(EX_BAD,
		_("unexpected short read-attempt in faio_get_buf. size = %d, len = %d\n"),
		size, len);
	}

	if (len < 0)
		seterrno(f->saved_errno);

	sp->gets++;

	*bpp = f->bufp;
	if (--f->users <= 0) {
		RB_FIFO_MEMORY_BARRIER();
		f->owner = owner_faio;
	}
	return (len);
}

EXPORT void
fifo_stats()
{
	if (sp == NULL)	/* We might not use a FIFO */
		return;

	errmsgno(EX_BAD, _("fifo had %ld puts and %ld gets.\n"),
		sp->puts, sp->gets);
	errmsgno(EX_BAD, _("fifo was %ld times empty and %ld times full, min fill was %ld%%.\n"),
		sp->empty, sp->full, (100L*sp->cont_low)/faio_buffers);
}

EXPORT int
fifo_percent(addone)
	BOOL	addone;
{
	int	percent;

	if (sp == NULL)	/* We might not use a FIFO */
		return (-1);

	if (sp->done)
		return (100);
	percent = (100*(sp->puts + 1 - sp->gets)/faio_buffers);
	if (percent > 100)
		return (100);
	return (percent);
}
#else	/* FIFO */

#include <schily/standard.h>
#include <schily/utypes.h>	/* includes sys/types.h */
#include <schily/schily.h>
#include <schily/nlsdefs.h>

#include "cdrecord.h"

EXPORT	long	init_fifo	__PR((long));
EXPORT	BOOL	init_faio	__PR((track_t *track, int));
EXPORT	BOOL	await_faio	__PR((void));
EXPORT	void	kill_faio	__PR((void));
EXPORT	int	wait_faio	__PR((void));
EXPORT	int	faio_read_buf	__PR((int f, char *bp, int size));
EXPORT	int	faio_get_buf	__PR((int f, char **bpp, int size));
EXPORT	void	fifo_stats	__PR((void));
EXPORT	int	fifo_percent	__PR((BOOL addone));


EXPORT long
init_fifo(fs)
	long	fs;
{
	errmsgno(EX_BAD, _("Fifo not supported.\n"));
	return (0L);
}

EXPORT BOOL
init_faio(track, bufsize)
	track_t	*track;
	int	bufsize;
{
	return (FALSE);
}

EXPORT BOOL
await_faio()
{
	return (TRUE);
}

EXPORT void
kill_faio()
{
}

EXPORT int
wait_faio()
{
	return (0);
}

EXPORT int
faio_read_buf(fd, bp, size)
	int fd;
	char *bp;
	int size;
{
	return (0);
}

EXPORT int
faio_get_buf(fd, bpp, size)
	int fd;
	char **bpp;
	int size;
{
	return (0);
}

EXPORT void
fifo_stats()
{
}

EXPORT int
fifo_percent(addone)
	BOOL	addone;
{
	return (-1);
}

#endif	/* FIFO */
