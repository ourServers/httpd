/* ====================================================================
 * Copyright (c) 1995-1997 The Apache Group.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the Apache Group
 *    for use in the Apache HTTP server project (http://www.apache.org/)."
 *
 * 4. The names "Apache Server" and "Apache Group" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission.
 *
 * 5. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the Apache Group
 *    for use in the Apache HTTP server project (http://www.apache.org/)."
 *
 * THIS SOFTWARE IS PROVIDED BY THE APACHE GROUP ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE APACHE GROUP OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Group and was originally based
 * on public domain software written at the National Center for
 * Supercomputing Applications, University of Illinois, Urbana-Champaign.
 * For more information on the Apache Group and the Apache HTTP server
 * project, please see <http://www.apache.org/>.
 *
 */

/*
 * httpd.c: simple http daemon for answering WWW file requests
 *
 * 
 * 03-21-93  Rob McCool wrote original code (up to NCSA HTTPd 1.3)
 * 
 * 03-06-95  blong
 *  changed server number for child-alone processes to 0 and changed name
 *   of processes
 *
 * 03-10-95  blong
 * 	Added numerous speed hacks proposed by Robert S. Thau (rst@ai.mit.edu) 
 *	including set group before fork, and call gettime before to fork
 * 	to set up libraries.
 *
 * 04-14-95  rst / rh
 *      Brandon's code snarfed from NCSA 1.4, but tinkered to work with the
 *      Apache server, and also to have child processes do accept() directly.
 *
 * April-July '95 rst
 *      Extensive rework for Apache.
 */


#define CORE_PRIVATE

#include "httpd.h"
#include "http_main.h"
#include "http_log.h"
#include "http_config.h"	/* for read_config */
#include "http_protocol.h"	/* for read_request */
#include "http_request.h"	/* for process_request */
#include "http_conf_globals.h"
#include "http_core.h"          /* for get_remote_host */
#include "scoreboard.h"
#include <setjmp.h>
#include <assert.h>
#include <sys/stat.h>
#ifdef HAVE_SHMGET
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#if defined(AUX)		/* Aren't defined anyplace */
extern char *shmat(int, char *, int);
extern int  shmctl(int, int, struct shmid_ds *);
extern int  shmget(key_t, int, int);
extern char *sbrk(int);
#include <sys/time.h>
#endif
#endif
#ifdef SecureWare
#include <sys/security.h>
#include <sys/audit.h>
#include <prot.h>
#endif
#include <netinet/tcp.h>

#include "explain.h"

#if !defined(max)
#define max(a,b)        (a > b ? a : b)
#endif

#ifdef __EMX__
    /* Add MMAP style functionality to OS/2 */
    #ifdef HAVE_MMAP
        #define INCL_DOSMEMMGR
        #include <os2.h>
        #include <umalloc.h>
        #include <stdio.h>
        caddr_t create_shared_heap (const char *, size_t);
        caddr_t get_shared_heap (const char *);
    #endif
#endif


DEF_Explain

/*
 * Actual definitions of config globals... here because this is
 * for the most part the only code that acts on 'em.  (Hmmm... mod_main.c?)
 */

int standalone;
uid_t user_id;
char *user_name;
gid_t group_id;
#ifdef MULTIPLE_GROUPS
gid_t group_id_list[NGROUPS_MAX];
#endif
int max_requests_per_child;
char *pid_fname;
char *scoreboard_fname;
char *server_argv0;
struct in_addr bind_address;
listen_rec *listeners;
int daemons_to_start;
int daemons_min_free;
int daemons_max_free;
int daemons_limit;
time_t restart_time;
int suexec_enabled = 0;

char server_root[MAX_STRING_LEN];
char server_confname[MAX_STRING_LEN];

/* *Non*-shared http_main globals... */

server_rec *server_conf;
JMP_BUF jmpbuffer;
JMP_BUF restart_buffer;
int sd;
static fd_set listenfds;
static int listenmaxfd;
pid_t pgrp;

/* one_process --- debugging mode variable; can be set from the command line
 * with the -X flag.  If set, this gets you the child_main loop running
 * in the process which originally started up (no detach, no make_child),
 * which is a pretty nice debugging environment.  (You'll get a SIGHUP
 * early in standalone_main; just continue through.  This is the server
 * trying to kill off any child processes which it might have lying
 * around --- Apache doesn't keep track of their pids, it just sends
 * SIGHUP to the process group, ignoring it in the root process.
 * Continue through and you'll be fine.).
 */

int one_process = 0;

/* The address 255.255.255.255, when used as a virtualhost address,
 * will become the "default" server when the ip doesn't match other vhosts.
 */
#define DEFAULT_VHOST_ADDR 0xfffffffful

#if defined(USE_FCNTL_SERIALIZED_ACCEPT)
static struct flock lock_it = { F_WRLCK, 0, 0, 0 };
static struct flock unlock_it = { F_UNLCK, 0, 0, 0 };

static int lock_fd=-1;

/*
 * Initialize mutex lock.
 * Must be safe to call this on a restart.
 */
void
accept_mutex_init(pool *p)
    {
    char lock_fname[256];

#ifdef __MACHTEN__
    strncpy(lock_fname, "/var/tmp/htlock.XXXXXX", sizeof(lock_fname)-1);
#else
    strncpy(lock_fname, "/usr/tmp/htlock.XXXXXX", sizeof(lock_fname)-1);
#endif
    lock_fname[sizeof(lock_fname)-1] = '\0';

    if (mktemp(lock_fname) == NULL || lock_fname[0] == '\0')
    {
	fprintf (stderr, "Cannot assign name to lock file!\n");
	exit (1);
    }

    lock_fd = popenf(p, lock_fname, O_CREAT | O_WRONLY | O_EXCL, 0644);
    if (lock_fd == -1)
    {
	perror ("open");
	fprintf (stderr, "Cannot open lock file: %s\n", lock_fname);
	exit (1);
    }
    unlink(lock_fname);
}

void accept_mutex_on()
{
    int ret;
    
    while ((ret = fcntl(lock_fd, F_SETLKW, &lock_it)) < 0 && errno == EINTR)
	continue;

    if (ret < 0) {
	log_unixerr("fcntl", "F_SETLKW", "Error getting accept lock. Exiting!",
		    server_conf);
	exit(1);
    }
}

void accept_mutex_off()
{
    if (fcntl (lock_fd, F_SETLKW, &unlock_it) < 0)
    {
	log_unixerr("fcntl", "F_SETLKW", "Error freeing accept lock. Exiting!",
		    server_conf);
	exit(1);
    }
}
#elif defined(USE_FLOCK_SERIALIZED_ACCEPT)

static int lock_fd=-1;

/*
 * Initialize mutex lock.
 * Must be safe to call this on a restart.
 */
void
accept_mutex_init(pool *p)
{
    char lock_fname[256];

    strncpy(lock_fname, "/usr/tmp/htlock.XXXXXX", sizeof(lock_fname)-1);
    lock_fname[sizeof(lock_fname)-1] = '\0';
    
    if (mktemp(lock_fname) == NULL || lock_fname[0] == '\0')
    {
	fprintf (stderr, "Cannot assign name to lock file!\n");
	exit (1);
    }

    lock_fd = popenf(p, lock_fname, O_CREAT | O_WRONLY | O_EXCL, 0644);
    if (lock_fd == -1)
    {
	perror ("open");
	fprintf (stderr, "Cannot open lock file\n");
	exit (1);
    }
    unlink(lock_fname);
}

void accept_mutex_on()
{
    int ret;
    
    while ((ret = flock(lock_fd, LOCK_EX)) < 0 && errno == EINTR)
	continue;

    if (ret < 0) {
	log_unixerr("flock", "LOCK_EX", "Error getting accept lock. Exiting!",
		    server_conf);
	exit(1);
    }
}

void accept_mutex_off()
{
    if (flock (lock_fd, LOCK_UN) < 0)
    {
	log_unixerr("flock", "LOCK_UN", "Error freeing accept lock. Exiting!",
		    server_conf);
	exit(1);
    }
}
#else
/* Default --- no serialization.  Other methods *could* go here,
 * as #elifs...
 */
#define accept_mutex_init(x)
#define accept_mutex_on()
#define accept_mutex_off()
#endif

/*
 * More machine-dependant networking gooo... on some systems,
 * you've got to be *really* sure that all the packets are acknowledged
 * before closing the connection.
 */

#ifndef NO_LINGCLOSE
static void lingering_close (request_rec *r)
{
    int dummybuf[512];
    struct timeval tv;
    fd_set fds, fds_read, fds_err;
    int select_rv = 0, read_rv = 0;
    int sd = r->connection->client->fd;

    if (sd < 0)          /* Don't do anything if the socket is invalid */
        return;

    kill_timeout(r);     /* Remove any leftover timeouts */

    /* Close our half of the connection --- send client a FIN */

    if ((shutdown(sd, 1)) != 0) {
	/* if it fails, no need to go through the rest of the routine */
	log_unixerr("shutdown", NULL, "lingering_close", r->server);
	close(sd);
	return;
    }

    /* Set up to wait for readable data on socket... */

    FD_ZERO(&fds);
    FD_SET(sd, &fds);

    /* Wait for readable data or error condition on socket;
     * slurp up any data that arrives...  We exit when we go for 
     * an interval of tv length without getting any more data, get an
     * error from select(), get an exception on sd, get an error or EOF
     * on a read, or the timer expires.
     */

    /* Prevent a slow-drip client from holding us here indefinitely */

    hard_timeout("lingering_close", r);

    do {
	/* If keep_alive_timeout is too low, using it as a timeout
	 * can cause undesirable behavior so pick some pseudo-arbitrary
	 * minimum value, currently 10 seconds.  These parameters are
         * reset on each pass, since they may be changed by select.
	 */
        tv.tv_sec  = max(r->server->keep_alive_timeout, 10);
        tv.tv_usec = 0;
        read_rv    = 0;
        fds_read   = fds;
        fds_err    = fds;
    
#ifdef HPUX
        select_rv = select(sd + 1, (int*)&fds_read, NULL, (int*)&fds_err, &tv);
#else
        select_rv = select(sd + 1, &fds_read, NULL, &fds_err, &tv);
#endif
    } while ((select_rv > 0) &&           /* Something to see on socket    */
             !FD_ISSET(sd, &fds_err) &&   /* that isn't an error condition */
             FD_ISSET(sd, &fds_read) &&   /* and is worth trying to read   */
             ((read_rv = read(sd, dummybuf, sizeof dummybuf)) > 0));

    /* Log any errors that occurred (client close or reset is not an error) */
    
    if (select_rv < 0)
        log_unixerr("select", NULL, "lingering_close", r->server);
    else if (read_rv < 0 && errno != ECONNRESET)
        log_unixerr("read", NULL, "lingering_close", r->server);

    /* Should now have seen final ack.  Safe to finally kill socket */

    if (close(sd) < 0)
        log_unixerr("close", NULL, "lingering_close", r->server);

    kill_timeout(r);
}
#endif /* ndef NO_LINGCLOSE */

void usage(char *bin)
{
    fprintf(stderr,"Usage: %s [-d directory] [-f file] [-v] [-h] [-l]\n",bin);
    fprintf(stderr,"-d directory : specify an alternate initial ServerRoot\n");
    fprintf(stderr,"-f file : specify an alternate ServerConfigFile\n");
    fprintf(stderr,"-v : show version number\n");
    fprintf(stderr,"-h : list directives\n");
    fprintf(stderr,"-l : list modules\n");
    exit(1);
}

/*****************************************************************
 *
 * Timeout handling.  DISTINCTLY not thread-safe, but all this stuff
 * has to change for threads anyway.  Note that this code allows only
 * one timeout in progress at a time...
 */

static conn_rec *current_conn;
static request_rec *timeout_req;
static char *timeout_name = NULL;
static int alarms_blocked = 0;
static int alarm_pending = 0;

void abort_connection (conn_rec *);

void timeout(sig)			/* Also called on SIGPIPE */
int sig;
{
    char errstr[MAX_STRING_LEN];
    void *dirconf;

    signal(SIGPIPE, SIG_IGN);		/* Block SIGPIPE */
    if (alarms_blocked) {
	alarm_pending = 1;
	return;
    }
    
    if (!current_conn) {
#if defined(NEXT) || defined(USE_LONGJMP)
	longjmp(jmpbuffer,1);
#else
	siglongjmp(jmpbuffer,1);
#endif
    }
    
    if (timeout_req != NULL) dirconf = timeout_req->per_dir_config;
    else dirconf = current_conn->server->lookup_defaults;
    if (sig == SIGPIPE) {
        ap_snprintf(errstr, sizeof(errstr), "%s lost connection to client %s",
	    timeout_name ? timeout_name : "request",
	    get_remote_host(current_conn, dirconf, REMOTE_NAME));
    } else {
        ap_snprintf(errstr, sizeof(errstr), "%s timed out for %s",
	    timeout_name ? timeout_name : "request",
	    get_remote_host(current_conn, dirconf, REMOTE_NAME));
    }
    
    if (!current_conn->keptalive) 
       log_error(errstr, current_conn->server);
      
    if (timeout_req) {
	/* Someone has asked for this transaction to just be aborted
	 * if it times out...
	 */
	
	request_rec *log_req = timeout_req;
	
	while (log_req->main || log_req->prev) {
	    /* Get back to original request... */
	    if (log_req->main) log_req = log_req->main;
	    else log_req = log_req->prev;
	}
	
	if (!current_conn->keptalive) 
            log_transaction(log_req);

	bclose(timeout_req->connection->client);
    
	if (!standalone) exit(0);
#if defined(NEXT) || defined(USE_LONGJMP)
	longjmp(jmpbuffer,1);
#else
	siglongjmp(jmpbuffer,1);
#endif
    }
    else {
	abort_connection (current_conn);
    }
}

/*
 * These two called from alloc.c to protect its critical sections...
 * Note that they can nest (as when destroying the sub_pools of a pool
 * which is itself being cleared); we have to support that here.
 */

void block_alarms() {
    ++alarms_blocked;
}

void unblock_alarms() {
    --alarms_blocked;
    if (alarms_blocked == 0 && alarm_pending) {
	alarm_pending = 0;
	timeout(0);
    }
}

void keepalive_timeout (char *name, request_rec *r)
{
    timeout_req = r;
    timeout_name = name;
    
    signal(SIGALRM,(void (*)())timeout);
    if (r->connection->keptalive) 
       alarm (r->server->keep_alive_timeout);
    else
       alarm (r->server->timeout);
}

void hard_timeout (char *name, request_rec *r)
{
    timeout_req = r;
    timeout_name = name;
    
    signal(SIGALRM,(void (*)())timeout);
    alarm (r->server->timeout);
}

void soft_timeout (char *name, request_rec *r)
{
    timeout_name = name;
    
    signal(SIGALRM,(void (*)())timeout);
    alarm (r->server->timeout);
}

void kill_timeout (request_rec *dummy) {
    alarm (0);
    timeout_req = NULL;
    timeout_name = NULL;
}

/* reset_timeout (request_rec *) resets the timeout in effect,
 * as long as it hasn't expired already.
 */

void reset_timeout (request_rec *r) {
    int i;

    if (timeout_name) { /* timeout has been set */
        i = alarm(r->server->timeout);
        if (i == 0) /* timeout already expired, so set it back to 0 */
	    alarm(0);
    }
}

/*****************************************************************
 *
 * Dealing with the scoreboard... a lot of these variables are global
 * only to avoid getting clobbered by the longjmp() that happens when
 * a hard timeout expires...
 *
 * We begin with routines which deal with the file itself... 
 */

#if defined(HAVE_MMAP)
static scoreboard *scoreboard_image=NULL;

static void setup_shared_mem(void)
{
    caddr_t m;

#ifdef __EMX__
    char errstr[MAX_STRING_LEN];
    int rc;

    m = (caddr_t)create_shared_heap("\\SHAREMEM\\SCOREBOARD", HARD_SERVER_LIMIT*sizeof(short_score));
    if(m == 0) {
       fprintf(stderr, "httpd: Could not create OS/2 Shared memory pool.\n");
       exit(1);
    }

    rc = _uopen((Heap_t)m);
    if(rc != 0) {
       fprintf(stderr, "httpd: Could not uopen() newly created OS/2 Shared memory pool.\n");
    }

#elif defined(MAP_ANON) || defined(MAP_FILE)
/* BSD style */
    m = mmap((caddr_t)0, SCOREBOARD_SIZE,
	     PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
    if (m == (caddr_t)-1)
    {
	perror("mmap");
	fprintf(stderr, "httpd: Could not mmap memory\n");
	exit(1);
    }
#else
/* Sun style */
    int fd;

    fd = open("/dev/zero", O_RDWR);
    if (fd == -1)
    {
	perror("open");
	fprintf(stderr, "httpd: Could not open /dev/zero\n");
	exit(1);
    }
    m = mmap((caddr_t)0, SCOREBOARD_SIZE,
	     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == (caddr_t)-1)
    {
	perror("mmap");
	fprintf(stderr, "httpd: Could not mmap /dev/zero\n");
	exit(1);
    }
    close(fd);
#endif
    scoreboard_image = (scoreboard *)m;
    scoreboard_image->global.exit_generation=0;
}

#elif defined(HAVE_SHMGET)
static scoreboard *scoreboard_image=NULL;
static key_t shmkey = IPC_PRIVATE;
static int shmid = -1;

static void setup_shared_mem(void)
{
    char errstr[MAX_STRING_LEN];
    struct shmid_ds shmbuf;
#ifdef MOVEBREAK
    char *obrk;
#endif

    if ((shmid = shmget(shmkey, SCOREBOARD_SIZE, IPC_CREAT|SHM_R|SHM_W)) == -1)
    {
	perror("shmget");
	fprintf(stderr, "httpd: Could not call shmget\n");
	exit(1);
    }

    ap_snprintf(errstr, sizeof(errstr), "created shared memory segment #%d", shmid);
    log_error(errstr, server_conf);

#ifdef MOVEBREAK
    /*
     * Some SysV systems place the shared segment WAY too close
     * to the dynamic memory break point (sbrk(0)). This severely
     * limits the use of malloc/sbrk in the program since sbrk will
     * refuse to move past that point.
     *
     * To get around this, we move the break point "way up there",
     * attach the segment and then move break back down. Ugly
     */
    if ((obrk=sbrk(MOVEBREAK)) == (char *)-1)
    {
	perror("sbrk");
	fprintf(stderr, "httpd: Could not move break\n");
    }
#endif

#define BADSHMAT	((scoreboard *)(-1))
    if ((scoreboard_image = (scoreboard *)shmat(shmid, 0, 0)) == BADSHMAT)
    {
	perror("shmat");
	fprintf(stderr, "httpd: Could not call shmat\n");
	/*
	 * We exit below, after we try to remove the segment
	 */
    }
    else	/* only worry about permissions if we attached the segment */
    {
	if (shmctl(shmid, IPC_STAT, &shmbuf) != 0) {
	    perror("shmctl");
	    fprintf(stderr, "httpd: Could not stat segment #%d\n", shmid);
	}
	else
	{
	    shmbuf.shm_perm.uid = user_id;
	    shmbuf.shm_perm.gid = group_id;
	    if (shmctl(shmid, IPC_SET, &shmbuf) != 0) {
		perror("shmctl");
		fprintf(stderr, "httpd: Could not set segment #%d\n", shmid);
	    }
	}
    }
    /*
     * We must avoid leaving segments in the kernel's
     * (small) tables.
     */
    if (shmctl(shmid, IPC_RMID, NULL) != 0) {
	perror("shmctl");
	fprintf(stderr, "httpd: Could not delete segment #%d\n", shmid);
	ap_snprintf(errstr, sizeof(errstr), "could not remove shared memory segment #%d", shmid);
	log_unixerr("shmctl","IPC_RMID",errstr, server_conf);
    }
    if (scoreboard_image == BADSHMAT)	/* now bailout */
	exit(1);

#ifdef MOVEBREAK
    if (obrk == (char *)-1)
	return;		/* nothing else to do */
    if (sbrk(-(MOVEBREAK)) == (char *)-1)
    {
	perror("sbrk");
	fprintf(stderr, "httpd: Could not move break back\n");
    }
#endif
    scoreboard_image->global.exit_generation=0;
}

#else
static scoreboard _scoreboard_image;
static scoreboard *scoreboard_image=&_scoreboard_image;
static int have_scoreboard_fname = 0;
static int scoreboard_fd;

static int force_write (int fd, char *buffer, int bufsz)
{
    int rv, orig_sz = bufsz;
    
    do {
	rv = write (fd, buffer, bufsz);
	if (rv > 0) {
	    buffer += rv;
	    bufsz -= rv;
	}
    } while (rv > 0 && bufsz > 0);

    return rv < 0? rv : orig_sz - bufsz;
}

static int force_read (int fd, char *buffer, int bufsz)
{
    int rv, orig_sz = bufsz;
    
    do {
	rv = read (fd, buffer, bufsz);
	if (rv > 0) {
	    buffer += rv;
	    bufsz -= rv;
	}
    } while (rv > 0 && bufsz > 0);
    
    return rv < 0? rv : orig_sz - bufsz;
}
#endif

/* Called by parent process */
void reinit_scoreboard (pool *p)
{
    int exit_gen=0;
    if(scoreboard_image)
	exit_gen=scoreboard_image->global.exit_generation;
	
#if defined(HAVE_SHMGET) || defined(HAVE_MMAP)
    if (scoreboard_image == NULL)
    {
	setup_shared_mem();
    }
    memset(scoreboard_image, 0, SCOREBOARD_SIZE);
    scoreboard_image->global.exit_generation=exit_gen;
#else
    scoreboard_fname = server_root_relative (p, scoreboard_fname);

    have_scoreboard_fname = 1;
    
#ifdef __EMX__
    /* OS/2 needs binary mode set. */
    scoreboard_fd = popenf(p, scoreboard_fname, O_CREAT|O_BINARY|O_RDWR, 0644);
#else
    scoreboard_fd = popenf(p, scoreboard_fname, O_CREAT|O_RDWR, 0644);
#endif
    if (scoreboard_fd == -1)
    {
	fprintf (stderr, "Cannot open scoreboard file:\n");
	perror (scoreboard_fname);
	exit (1);
    }

    memset ((char*)scoreboard_image, 0, sizeof(*scoreboard_image));
    scoreboard_image->global.exit_generation=exit_gen;
    force_write (scoreboard_fd, (char*)scoreboard_image,
		 sizeof(*scoreboard_image));
#endif
}

/* called by child */
void reopen_scoreboard (pool *p)
{
#if !defined(HAVE_MMAP) && !defined(HAVE_SHMGET)
    if (scoreboard_fd != -1) pclosef (p, scoreboard_fd);
    
#ifdef __EMX__    
    /* OS/2 needs binary mode set. */
    scoreboard_fd = popenf(p, scoreboard_fname, O_CREAT|O_BINARY|O_RDWR, 0666);
#else
    scoreboard_fd = popenf(p, scoreboard_fname, O_CREAT|O_RDWR, 0666);
#endif
    if (scoreboard_fd == -1)
    {
	fprintf (stderr, "Cannot open scoreboard file:\n");
	perror (scoreboard_fname);
	exit (1);
    }
#else
#ifdef __EMX__
#ifdef HAVE_MMAP
    caddr_t m;
    int rc;

    m = (caddr_t)get_shared_heap("\\SHAREMEM\\SCOREBOARD");
    if(m == 0) {
        fprintf(stderr, "httpd: Could not find existing OS/2 Shared memory pool.\n");
        exit(1);
    }

    rc = _uopen((Heap_t)m);
    scoreboard_image = (scoreboard *)m;
#endif
#endif
#endif
}

void cleanup_scoreboard ()
{
#if !defined(HAVE_MMAP) && !defined(HAVE_SHMGET)
    unlink (scoreboard_fname);
#endif
}

/* Routines called to deal with the scoreboard image
 * --- note that we do *not* need write locks, since update_child_status
 * only updates a *single* record in place, and only one process writes to
 * a given scoreboard slot at a time (either the child process owning that
 * slot, or the parent, noting that the child has died).
 *
 * As a final note --- setting the score entry to getpid() is always safe,
 * since when the parent is writing an entry, it's only noting SERVER_DEAD
 * anyway.
 */

void sync_scoreboard_image ()
{
#if !defined(HAVE_MMAP) && !defined(HAVE_SHMGET)
    lseek (scoreboard_fd, 0L, 0);
    force_read (scoreboard_fd, (char*)scoreboard_image,
		sizeof(*scoreboard_image));
#endif
}

int update_child_status (int child_num, int status, request_rec *r)
{
    int old_status;
    short_score new_score_rec;

    if (child_num < 0)
	return -1;
    
    sync_scoreboard_image();
    new_score_rec = scoreboard_image->servers[child_num];
    new_score_rec.pid = getpid();
    old_status = new_score_rec.status;
    new_score_rec.status = status;

#if defined(STATUS)
    new_score_rec.last_used=time(NULL);
    if (status == SERVER_DEAD) {
	/*
	 * Reset individual counters
	 */
	new_score_rec.my_access_count = 0L;
	new_score_rec.my_bytes_served = 0L;
	new_score_rec.conn_count = (unsigned short)0;
	new_score_rec.conn_bytes = (unsigned long)0;
    }
    if (r) {
	int slot_size;
	conn_rec *c = r->connection;
	slot_size = sizeof(new_score_rec.client) - 1;
	strncpy(new_score_rec.client, get_remote_host(c, r->per_dir_config,
	 REMOTE_NOLOOKUP), slot_size);
	new_score_rec.client[slot_size] = '\0';
	slot_size = sizeof(new_score_rec.request) - 1;
	strncpy(new_score_rec.request, (r->the_request ? r->the_request :
	 "NULL"), slot_size);
	new_score_rec.request[slot_size] = '\0';
	slot_size = sizeof(new_score_rec.vhost) - 1;
	strncpy(new_score_rec.vhost,r->server->server_hostname, slot_size);
	new_score_rec.vhost[slot_size] = '\0';
    }
#endif

#if defined(HAVE_MMAP) || defined(HAVE_SHMGET)
    memcpy(&scoreboard_image->servers[child_num], &new_score_rec, sizeof new_score_rec);
#else
    lseek (scoreboard_fd, (long)child_num * sizeof(short_score), 0);
    force_write (scoreboard_fd, (char*)&new_score_rec, sizeof(short_score));
#endif

    return old_status;
}

void update_scoreboard_global()
    {
#if !defined(HAVE_MMAP) && !defined(HAVE_SHMGET)
    lseek(scoreboard_fd,
	  (char *)&scoreboard_image->global-(char *)scoreboard_image,0);
    force_write(scoreboard_fd,(char *)&scoreboard_image->global,
		sizeof scoreboard_image->global);
#endif
    }

int get_child_status (int child_num)
{
    if (child_num<0 || child_num>=HARD_SERVER_LIMIT)
    	return -1;
    else
	return scoreboard_image->servers[child_num].status;
}

int count_busy_servers ()
{
    int i;
    int res = 0;

    for (i = 0; i < HARD_SERVER_LIMIT; ++i)
      if (scoreboard_image->servers[i].status == SERVER_BUSY_READ ||
              scoreboard_image->servers[i].status == SERVER_BUSY_WRITE ||
              scoreboard_image->servers[i].status == SERVER_BUSY_KEEPALIVE ||
              scoreboard_image->servers[i].status == SERVER_BUSY_LOG ||
              scoreboard_image->servers[i].status == SERVER_BUSY_DNS)
          ++res;
    return res;
}

int count_live_servers()
    {
    int i;
    int res = 0;

    for (i = 0; i < HARD_SERVER_LIMIT; ++i)
      if (scoreboard_image->servers[i].status != SERVER_DEAD)
	  ++res;
    return res;
    }

short_score get_scoreboard_info(int i)
{
    return (scoreboard_image->servers[i]);
}

#if defined(STATUS)
void increment_counts (int child_num, request_rec *r, int flag)
{
    long int bs=0;
    short_score new_score_rec;

    sync_scoreboard_image();
    new_score_rec = scoreboard_image->servers[child_num];
    if (r->sent_bodyct)
        bgetopt(r->connection->client, BO_BYTECT, &bs);

    if (flag) {
	new_score_rec.conn_count = (unsigned short)0;
	new_score_rec.conn_bytes = (unsigned long)0;
    }
    new_score_rec.access_count ++;
    new_score_rec.my_access_count ++;
    new_score_rec.conn_count ++;
    new_score_rec.bytes_served += (unsigned long)bs;
    new_score_rec.my_bytes_served += (unsigned long)bs;
    new_score_rec.conn_bytes += (unsigned long)bs;

    times(&new_score_rec.times);


#if defined(HAVE_MMAP) || defined(HAVE_SHMGET)
    memcpy(&scoreboard_image->servers[child_num], &new_score_rec, sizeof(short_score));
#else
    lseek (scoreboard_fd, (long)child_num * sizeof(short_score), 0);
    force_write (scoreboard_fd, (char*)&new_score_rec, sizeof(short_score));
#endif
}
#endif

int count_idle_servers ()
{
    int i;
    int res = 0;

    for (i = 0; i < HARD_SERVER_LIMIT; ++i)
	if (scoreboard_image->servers[i].status == SERVER_READY
	  || scoreboard_image->servers[i].status == SERVER_STARTING)
	    ++res;

    return res;
}

int find_free_child_num ()
{
    int i;

    for (i = 0; i < HARD_SERVER_LIMIT; ++i)
	if (scoreboard_image->servers[i].status == SERVER_DEAD)
	    return i;

    return -1;
}

int find_child_by_pid (int pid)
{
    int i;

    for (i = 0; i < HARD_SERVER_LIMIT; ++i)
	if (scoreboard_image->servers[i].pid == pid)
	    return i;

    return -1;
}

void reclaim_child_processes ()
{
    int i, status;
    int my_pid = getpid();

    sync_scoreboard_image();
    for (i = 0; i < HARD_SERVER_LIMIT; ++i) {
	int pid = scoreboard_image->servers[i].pid;

	if (pid != my_pid && pid != 0)
	    waitpid (scoreboard_image->servers[i].pid, &status, 0);
    }
}

#if defined(BROKEN_WAIT) || defined(NEED_WAITPID)
/*
Some systems appear to fail to deliver dead children to wait() at times.
This sorts them out. In fact, this may have been caused by a race condition
in wait_or_timeout(). But this routine is still useful for systems with no
waitpid().
*/
int reap_children()
    {
    int status,n;
    int ret=0;

    for(n=0 ; n < HARD_SERVER_LIMIT ; ++n)
	if(scoreboard_image->servers[n].status != SERVER_DEAD
	   && waitpid(scoreboard_image->servers[n].pid,&status,WNOHANG) == -1
	   && errno == ECHILD)
	    {
	    sync_scoreboard_image();
	    update_child_status(n,SERVER_DEAD,NULL);
	    ret=1;
	    }
    return ret;
    }
#endif

/* Finally, this routine is used by the caretaker process to wait for
 * a while...
 */

#if 1

static int wait_or_timeout(int *status)
    {
#ifndef NEED_WAITPID
    int ret;

    ret=waitpid(-1,status,WNOHANG);
    if(ret <= 0)
	{
	sleep(1);
	return -1;
	}
    return ret;
#else
    if(!reap_children())
	sleep(1);
    return -1;
#endif
    }

#else

static JMP_BUF wait_timeout_buf;

static void longjmp_out_of_alarm (int sig) {
#if defined(NEXT) || defined(USE_LONGJMP)
    longjmp (wait_timeout_buf, 1);
#else
    siglongjmp (wait_timeout_buf, 1);
#endif
}

int wait_or_timeout (int *status)
{
    int wait_or_timeout_retval = -1;
#ifdef BROKEN_WAIT
    static int ntimes;
#endif

#if defined(NEXT)
    if (setjmp(wait_timeout_buf) != 0) {
#else 
    if (sigsetjmp(wait_timeout_buf, 1) != 0) {
#endif
	errno = ETIMEDOUT;
	return wait_or_timeout_retval;
    }
#ifdef BROKEN_WAIT
    if(++ntimes == 60)
	{
	reap_children();
	ntimes=0;
	}
#endif
    signal (SIGALRM, longjmp_out_of_alarm);
    alarm(1);
#if defined(NEXT)
    wait_or_timeout_retval = wait((union wait *)status);
#else
    wait_or_timeout_retval = wait(status);
#endif
    alarm(0);
    return wait_or_timeout_retval;
}

#endif

/*****************************************************************
 * Here follows a long bunch of generic server bookkeeping stuff...
 */

void detach()
{
    int x;

    chdir("/");
    if((x = fork()) > 0)
        exit(0);
    else if(x == -1) {
        fprintf(stderr,"httpd: unable to fork new process\n");
        perror("fork");
        exit(1);
    }
#ifndef NO_SETSID
    if((pgrp=setsid()) == -1) {
        fprintf(stderr,"httpd: setsid failed\n");
        perror("setsid");
        exit(1);
    }
#else
#if defined(NEXT)
    if(setpgrp(0,getpid()) == -1 || (pgrp = getpgrp(0)) == -1) {
        fprintf(stderr,"httpd: setpgrp or getpgrp failed\n");
        perror("setpgrp");
        exit(1);
    }
#else
#ifdef __EMX__
    /* OS/2 doesn't support process group IDs */
    pgrp=getpid();
#else
    if((pgrp=setpgrp(getpid(),0)) == -1) {
        fprintf(stderr,"httpd: setpgrp failed\n");
        perror("setpgrp");
        exit(1);
    }
#endif    
#endif
#endif
}

void sig_term() {
    log_error("httpd: caught SIGTERM, shutting down", server_conf);
    cleanup_scoreboard();
#ifndef NO_KILLPG
    killpg(pgrp,SIGKILL);
#else
    kill(-pgrp,SIGKILL);
#endif
    shutdown(sd,2);
    close(sd);
    exit(1);
}

void bus_error(void) {
    log_error("httpd: caught SIGBUS, dumping core", server_conf);
    chdir(server_root);
    abort();         
    exit(1);
}

void seg_fault() {
    log_error("httpd: caught SIGSEGV, dumping core", server_conf);
    chdir(server_root);
    abort();
    exit(1);
}

void just_die()			/* SIGHUP to child process??? */
{
    exit (0);
}

/* Reset group privileges, after rereading the config files
 * (our uid may have changed, and if so, we want the new perms).
 *
 * Don't reset the uid yet --- we do that only in the child process,
 * so as not to lose any root privs.  But we can set the group stuff
 * now, once, as opposed to once per each new child.
 *
 * Note that we use the username as set in the config files, rather than
 * the lookup of to uid --- the same uid may have multiple passwd entries,
 * with different sets of groups for each.
 */
  
static void set_group_privs()
{
  if(!geteuid()) {
    char *name;
  
    /* Get username if passed as a uid */
    
    if (user_name[0] == '#') {
      struct passwd* ent;
      uid_t uid=atoi(&user_name[1]);

      if ((ent = getpwuid(uid)) == NULL) {
	 log_unixerr("getpwuid",NULL,"couldn't determine user name from uid", server_conf);
	 exit(1);
      }
      
      name = ent->pw_name;
    } else name = user_name;

#ifndef __EMX__ 
    /* OS/2 dosen't support groups. */

    /* Reset `groups' attributes. */
    
    if (initgroups(name, group_id) == -1) {
	log_unixerr("initgroups", NULL, "unable to set groups", server_conf);
	exit (1);
    }
#ifdef MULTIPLE_GROUPS
    if (getgroups(NGROUPS_MAX, group_id_list) == -1) {
	log_unixerr("getgroups", NULL, "unable to get group list", server_conf);
	exit (1);
    }
#endif
    if (setgid(group_id) == -1) {
	log_unixerr("setgid", NULL, "unable to set group id", server_conf);
	exit (1);
    }
#endif 
  }
}

/* check to see if we have the 'suexec' setuid wrapper installed */
int init_suexec ()
{
    struct stat wrapper;
    
    if ((stat(SUEXEC_BIN, &wrapper)) != 0)
      return (suexec_enabled);
    
    if ((wrapper.st_mode & S_ISUID) && wrapper.st_uid == 0) {
      suexec_enabled = 1;
      fprintf(stderr, "Configuring Apache for use with suexec wrapper.\n");
    }

    return (suexec_enabled);
}

static int is_graceful;
static int generation;

void restart() {
    signal (SIGALRM, SIG_IGN);
    alarm (0);
    is_graceful=0;
#if defined(NEXT)  || defined(USE_LONGJMP)
    longjmp(restart_buffer,1);
#else
    siglongjmp(restart_buffer,1);
#endif
}

void graceful_restart()
    {
    scoreboard_image->global.exit_generation=generation;
    is_graceful=1;
    update_scoreboard_global();
#if defined(NEXT) || defined(USE_LONGJMP)
    longjmp(restart_buffer,1);
#else
    siglongjmp(restart_buffer,1);
#endif
    }

void set_signals()
{
#ifndef NO_USE_SIGACTION
    struct sigaction sa;
    memset(&sa,0,sizeof sa);

    if (!one_process) {
	sa.sa_handler = (void (*)())seg_fault;
	if (sigaction(SIGSEGV, &sa, NULL) < 0)
	    log_unixerr("sigaction(SIGSEGV)", NULL, NULL, server_conf);
	sa.sa_handler = (void (*)())bus_error;
	if (sigaction(SIGBUS, &sa, NULL) < 0)
	    log_unixerr("sigaction(SIGBUS)", NULL, NULL, server_conf);
    }
    /* USE WITH EXTREME CAUTION. Graceful restarts are known to break */
    /*  problems will be dealt with in a future release */
    sa.sa_handler=(void (*)())sig_term;
    if(sigaction(SIGTERM,&sa,NULL) < 0)
	log_unixerr("sigaction(SIGTERM)", NULL, NULL, server_conf);
    sa.sa_handler=(void (*)())restart;
    if(sigaction(SIGHUP,&sa,NULL) < 0)
	log_unixerr("sigaction(SIGHUP)", NULL, NULL, server_conf);
    sa.sa_handler=(void (*)())graceful_restart;
    if(sigaction(SIGUSR1,&sa,NULL) < 0)
	log_unixerr("sigaction(SIGUSR1)", NULL, NULL, server_conf);
#else
    if(!one_process) {
	signal(SIGSEGV,(void (*)())seg_fault);
    	signal(SIGBUS,(void (*)())bus_error);
    }

    signal(SIGTERM,(void (*)())sig_term);
    signal(SIGHUP,(void (*)())restart);
    signal(SIGUSR1,(void (*)())graceful_restart);
#endif
}

/*****************************************************************
 * Connection structures and accounting...
 * Should these be global?  Only to this file, at least...
 */

pool *pconf;			/* Pool for config stuff */
pool *ptrans;			/* Pool for per-transaction stuff */

server_rec *find_virtual_server (struct in_addr server_ip, int port,
				 server_rec *server)
{
    server_rec *virt;
    server_addr_rec *sar;
    server_rec *def;

    def = server;
    for (virt = server->next; virt; virt = virt->next) {
	for (sar = virt->addrs; sar; sar = sar->next) {
	    if ((virt->is_virtual == 1) &&	/* VirtualHost */
		(sar->host_addr.s_addr == htonl(INADDR_ANY) ||
		sar->host_addr.s_addr == server_ip.s_addr) &&
		(sar->host_port == 0 || sar->host_port == port)) {
		return virt;
	    } else if ( sar->host_addr.s_addr == DEFAULT_VHOST_ADDR ) {
		/* this is so that you can build a server that is the
		    "default" for any interface which isn't explicitly
		    specified.  So that you can implement "deny anything
		    which isn't expressly permitted" -djg */
		def = virt;
	    }
	}
    }

    return def;
}

void default_server_hostnames(server_rec *s)
{
    struct hostent *h;
    struct in_addr *main_addr;
    int num_addr;
    char *def_hostname;
    int n;
    server_addr_rec *sar;
    int has_inaddr_any;
    int mainport = s->port;
    int from_local=0;  

    /* Main host first */
    
    if (!s->server_hostname) {
	s->server_hostname = get_local_host(pconf);
	from_local = 1;
    }

    def_hostname = s->server_hostname;
    h = gethostbyname(def_hostname);
    if( h == NULL ) {
	fprintf(stderr,"httpd: cannot determine the IP address of ");
	if (from_local) {
	   fprintf(stderr,"the local host (%s). Use ServerName to set it manually.\n",
		s->server_hostname ? s->server_hostname : "<NULL>");
	} else {
	   fprintf(stderr,"the specified ServerName (%s).\n",
		s->server_hostname ? s->server_hostname : "<NULL>");
	};
	exit(1);
    }
    /* we need to use gethostbyaddr below... and since it shares a static
    	area with gethostbyname it'd clobber the value we just got.  So
    	we need to make a copy.  -djg */
    for (num_addr = 0; h->h_addr_list[num_addr] != NULL; num_addr++) {
    	/* nop */
    }
    main_addr = palloc( pconf, sizeof( *main_addr ) * num_addr );
    for (n = 0; n < num_addr; n++) {
    	main_addr[n] = *(struct in_addr *)h->h_addr_list[n];
    }

    /* Then virtual hosts */
    
    for (s = s->next; s; s = s->next) {
	/* Check to see if we might be a HTTP/1.1 virtual host - same IP */
	has_inaddr_any = 0;
	for (n = 0; n < num_addr; n++) {
	    for(sar = s->addrs; sar; sar = sar->next) {
		if (sar->host_addr.s_addr == main_addr[n].s_addr &&
		    s->port == mainport)
		    s->is_virtual = 2;
		if( sar->host_addr.s_addr == htonl(INADDR_ANY) ) {
		    has_inaddr_any = 1;
		}
	    }
	}

	/* FIXME: some of this decision doesn't make a lot of sense in
	    the presence of multiple addresses on the <VirtualHost>
	    directive.  It should issue warnings here perhaps. -djg */
        if (!s->server_hostname) {
	    if (s->is_virtual == 2)
	        s->server_hostname = s->addrs->virthost;
	    else if (has_inaddr_any)
		s->server_hostname = def_hostname;
	    else
	    {
		h = gethostbyaddr ((char *)&(s->addrs->host_addr),
				   sizeof (struct in_addr), AF_INET);
		if (h != NULL)
		    s->server_hostname = pstrdup (pconf, (char *)h->h_name);
		else
		    {
		    fprintf(stderr,"Failed to resolve server name for %s (check DNS)\n",inet_ntoa(s->addrs->host_addr));
		    exit(0);
		    }
	    }
	}
    }
}

void abort_connection (conn_rec *c)
{
    /* Make sure further I/O DOES NOT HAPPEN */
    shutdown (c->client->fd, 2);
    signal (SIGPIPE, SIG_IGN);	/* Ignore further complaints */
    c->aborted = 1;
}

conn_rec *new_connection (pool *p, server_rec *server, BUFF *inout,
			  const struct sockaddr_in *remaddr,
			  const struct sockaddr_in *saddr,
			  int child_num)
{
    conn_rec *conn = (conn_rec *)pcalloc (p, sizeof(conn_rec));
    
    /* Got a connection structure, so initialize what fields we can
     * (the rest are zeroed out by pcalloc).
     */
    
    conn->child_num = child_num;
    
    conn->pool = p;
    conn->local_addr = *saddr;
    conn->server = find_virtual_server(saddr->sin_addr, ntohs(saddr->sin_port),
				       server);
    conn->client = inout;
    
    conn->remote_addr = *remaddr;
    conn->remote_ip = pstrdup (conn->pool,
			       inet_ntoa(conn->remote_addr.sin_addr));

    return conn;
}

void sock_disable_nagle (int s)
{
    /*
     * The Nagle algorithm says that we should delay sending partial
     * packets in hopes of getting more data.  We don't want to do
     * this; we are not telnet.  There are bad interactions between
     * P-HTTP and Nagle's algorithm that have very severe performance
     * penalties.  (Failing to do disable Nagle is not much of a
     * problem with simple HTTP.)  A better description of these
     * problems is in preparation; contact me for details.
     * -John Heidemann <johnh@isi.edu>.
     *
     * In spite of these problems, failure here is not a shooting offense.
     */
    const int just_say_no = 1;
    if (0 != setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&just_say_no,
			sizeof(just_say_no)))
	fprintf(stderr, "httpd: could not set socket option TCP_NODELAY\n");
}

/*****************************************************************
 * Child process main loop.
 * The following vars are static to avoid getting clobbered by longjmp();
 * they are really private to child_main.
 */

static int csd;
static int dupped_csd;
static int requests_this_child;
static int child_num;

void child_main(int child_num_arg)
{
#if defined(UW)
    size_t clen;
#else
    int clen;
#endif
    struct sockaddr sa_server;
    struct sockaddr sa_client;

    csd = -1;
    dupped_csd = -1;
    child_num = child_num_arg;
    requests_this_child = 0;
    reopen_scoreboard (pconf);
    (void)update_child_status (child_num, SERVER_READY, (request_rec*)NULL);

    /* Only try to switch if we're running as root */
    if(!geteuid() && setuid(user_id) == -1) {
        log_unixerr("setuid", NULL, "unable to change uid", server_conf);
	exit (1);
    }

#ifdef NEXT
    setjmp(jmpbuffer);
#else
    sigsetjmp(jmpbuffer,1);
#endif
#ifndef __EMX__
    signal(SIGURG, timeout);
#endif    

    while (1) {
	BUFF *conn_io;
	request_rec *r;
      
        alarm(0);		/* Cancel any outstanding alarms. */
        timeout_req = NULL;	/* No request in progress */
	current_conn = NULL;
        signal(SIGPIPE, timeout);  
    
	clear_pool (ptrans);
	
	sync_scoreboard_image();

	/*fprintf(stderr,"%d check %d %d\n",getpid(),scoreboard_image->global.exit_generation,generation);*/
	if(scoreboard_image->global.exit_generation >= generation)
	    exit(0);
	
	if ((count_idle_servers() >= daemons_max_free)
	    || (max_requests_per_child > 0
	        && ++requests_this_child >= max_requests_per_child))
	{
	    exit(0);
	}

	(void)update_child_status (child_num, SERVER_READY, (request_rec*)NULL);
	
	accept_mutex_on();  /* Lock around "accept", if necessary */

	if (listeners != NULL)
	{
	    fd_set fds;

	    for (;;) {
		memcpy(&fds, &listenfds, sizeof(fd_set));
#ifdef HPUX
		csd = select(listenmaxfd+1, (int*)&fds, NULL, NULL, NULL);
#else
                csd = select(listenmaxfd+1, &fds, NULL, NULL, NULL);
#endif
		if (csd < 0 && errno != EINTR)
		    log_unixerr("select",NULL,"select error", server_conf);

		/*fprintf(stderr,"%d check(2a) %d %d\n",getpid(),scoreboard_image->global.exit_generation,generation);*/
		if(scoreboard_image->global.exit_generation >= generation)
		    exit(0);

		if (csd <= 0) continue;
		for (sd=listenmaxfd; sd >= 0; sd--)
		    if (FD_ISSET(sd, &fds)) break;
		if (sd < 0) continue;

                do {
                    clen = sizeof(sa_client);
                    csd  = accept(sd, &sa_client, &clen);
                } while (csd < 0 && errno == EINTR);
                if (csd > 0) break;
		log_unixerr("accept", "(client socket)", NULL, server_conf);
	    }
	}
	else {
	    fd_set fds;

	    do {
                FD_ZERO(&fds);
                FD_SET(sd,&fds);
#ifdef HPUX
		csd = select(sd+1, (int*)&fds, NULL, NULL, NULL);
#else
		csd = select(sd+1, &fds, NULL, NULL, NULL);
#endif
            } while (csd < 0 && errno == EINTR);

            if (csd < 0) {
		log_unixerr("select","(listen)",NULL,server_conf);
		exit(0);
            }
	    /*fprintf(stderr,"%d check(2a) %d %d\n",getpid(),scoreboard_image->global.exit_generation,generation);*/
	    sync_scoreboard_image();
	    if(scoreboard_image->global.exit_generation >= generation)
		exit(0);

            do {
                clen = sizeof(sa_client);
                csd  = accept(sd, &sa_client, &clen);
            } while (csd < 0 && errno == EINTR);
            if (csd < 0)
                log_unixerr("accept", NULL, "socket error: accept failed",
                            server_conf);
	}

	accept_mutex_off(); /* unlock after "accept" */

	clen = sizeof(sa_server);
	if(getsockname(csd, &sa_server, &clen) < 0) {
	    log_unixerr("getsockname", NULL, NULL, server_conf);
	    continue;
	}

	sock_disable_nagle(csd);

	(void)update_child_status (child_num, SERVER_BUSY_READ, (request_rec*)NULL);
	conn_io = bcreate(ptrans, B_RDWR);
	dupped_csd = csd;
#if defined(NEED_DUPPED_CSD)
	if ((dupped_csd = dup(csd)) < 0) {
	    log_unixerr("dup", NULL, "couldn't duplicate csd", server_conf);
	    dupped_csd = csd;   /* Oh well... */
	}
#endif
	bpushfd(conn_io, csd, dupped_csd);

	current_conn = new_connection (ptrans, server_conf, conn_io,
				       (struct sockaddr_in *)&sa_client,
				       (struct sockaddr_in *)&sa_server,
				       child_num);

	r = read_request (current_conn);
	(void)update_child_status (child_num, SERVER_BUSY_WRITE, r);
	if (r) process_request (r); /* else premature EOF --- ignore */
#if defined(STATUS)
        if (r) increment_counts(child_num,r,1);
#endif
	while (r && current_conn->keepalive) {
	    destroy_pool(r->pool);
	    (void)update_child_status (child_num, SERVER_BUSY_KEEPALIVE,
	     (request_rec*)NULL);
	    r = read_request (current_conn);
	    (void)update_child_status (child_num, SERVER_BUSY_WRITE, r);
	    if (r) process_request (r);
#if defined(STATUS)
	    if (r) increment_counts(child_num,r,0);
#endif
	  sync_scoreboard_image();
	  if(scoreboard_image->global.exit_generation >= generation)
	      exit(0);
	  /*fprintf(stderr,"%d check(3) %d %d\n",getpid(),scoreboard_image->global.exit_generation,generation);*/

	}
#if 0	
	if (bytes_in_pool (ptrans) > 80000)
	    log_printf(r->server,
		       "Memory hog alert: allocated %ld bytes for %s",
		       bytes_in_pool (ptrans), r->the_request);
#endif		
	bflush(conn_io);

#ifdef NO_LINGCLOSE
	bclose(conn_io);	/* just close it */
#else
        if (r && !r->connection->aborted) {
	    /* if the connection was aborted by a soft_timeout, it has
	     * already been shutdown() so we don't need to go through
	     * lingering_close
	     */
	    lingering_close(r);
	} else {
	    close(conn_io->fd);
	}
#endif	
    }    
}

int make_child(server_rec *server_conf, int child_num)
{
    int pid;

    if (one_process) {
	signal (SIGHUP, (void (*)())just_die);
	signal (SIGTERM, (void (*)())just_die);
	child_main (child_num);
    }

    if ((pid = fork()) == -1) {
	log_unixerr("fork", NULL, "Unable to fork new process", server_conf);

	/* In case system resources are maxxed out, we don't want
           Apache running away with the CPU trying to fork over and
           over and over again. */
	sleep(10);

	return -1;
    } 
    
    if (!pid) {
	signal (SIGHUP, (void (*)())just_die);
	signal (SIGTERM, (void (*)())just_die);
	child_main (child_num);
    }
    return 0;
}

static int
make_sock(pool *pconf, const struct sockaddr_in *server)
{
    int s;
    const int one = 1;
    const int keepalive_value = 1;  

    if ((s = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP)) == -1) {
        perror("socket");
        fprintf(stderr,"httpd: could not get socket\n");
        exit(1);
    }

    note_cleanups_for_fd (pconf, s); /* arrange to close on exec or restart */
    
    if((setsockopt(s, SOL_SOCKET,SO_REUSEADDR,(char *)&one,sizeof(one)))
       == -1) {
	perror("setsockopt(SO_REUSEADDR)");
	fprintf(stderr,"httpd: could not set socket option SO_REUSEADDR\n");
        exit(1);
    }
    if((setsockopt(s, SOL_SOCKET,SO_KEEPALIVE,(char *)&keepalive_value,
        sizeof(keepalive_value))) == -1) {
	perror("setsockopt(SO_KEEPALIVE)"); 
        fprintf(stderr,"httpd: could not set socket option SO_KEEPALIVE\n"); 
        exit(1); 
    }

    sock_disable_nagle(s);
    
#ifdef USE_SO_LINGER   /* If puts don't complete, you could try this. */
    {
	/* Unfortunately, SO_LINGER causes problems as severe as it
	 * cures on many of the affected systems; now trying the
	 * lingering_close trick (see routine by that name above)
	 * instead...
	 */
	struct linger li;
	li.l_onoff = 1;
	li.l_linger = 900;

	if (setsockopt(s, SOL_SOCKET, SO_LINGER,
	    (char *)&li, sizeof(struct linger)) < 0) {
	    perror("setsockopt(SO_LINGER)");
	    fprintf(stderr,"httpd: could not set socket option SO_LINGER\n");
	    exit(1);
	}
    }
#endif  /* USE_SO_LINGER */

    /*
     * To send data over high bandwidth-delay connections at full
     * speed we must the TCP window to open wide enough to keep the
     * pipe full.  Default the default window size on many systems
     * is only 4kB.  Cross-country WAN connections of 100ms
     * at 1Mb/s are not impossible for well connected sites in 1995.
     * If we assume 100ms cross-country latency,
     * a 4kB buffer limits throughput to 40kB/s.
     *
     * To avoid this problem I've added the SendBufferSize directive
     * to allow the web master to configure send buffer size.
     *
     * The trade-off of larger buffers is that more kernel memory
     * is consumed.  YMMV, know your customers and your network!
     *
     * -John Heidemann <johnh@isi.edu> 25-Oct-96
     *
     *
     * If no size is specified, use the kernel default.
     */
    if (server_conf->send_buffer_size) {
        if((setsockopt(s, SOL_SOCKET, SO_SNDBUF, (char*)&server_conf->send_buffer_size, sizeof(int))) < 0) {
	    perror("setsockopt(SO_SNDBUF), using default buffer size"); 
	    /* Fail soft. */
	}
    }

    if(bind(s, (struct sockaddr *)server,sizeof(struct sockaddr_in)) == -1)
    {
        perror("bind");
	if (server->sin_addr.s_addr != htonl(INADDR_ANY))
	    fprintf(stderr,"httpd: could not bind to address %s port %d\n",
		    inet_ntoa(server->sin_addr), ntohs(server->sin_port));
	else
	    fprintf(stderr,"httpd: could not bind to port %d\n",
		    ntohs(server->sin_port));
        exit(1);
    }
    listen(s, 512);
    return s;
}

static listen_rec *old_listeners;

static void copy_listeners(pool *p)
    {
    listen_rec *lr;

    assert(old_listeners == NULL);
    for(lr=listeners ; lr ; lr=lr->next)
	{
	listen_rec *nr=malloc(sizeof *nr);
	if (nr == NULL) {
	  fprintf (stderr, "Ouch!  malloc failed in copy_listeners()\n");
	  exit (1);
	}
	*nr=*lr;
	kill_cleanups_for_fd(p,nr->fd);
	nr->next=old_listeners;
	assert(!nr->used);
	old_listeners=nr;
	}
    }

static int find_listener(listen_rec *lr)
    {
    listen_rec *or;

    for(or=old_listeners ; or ; or=or->next)
	if(!memcmp(&or->local_addr,&lr->local_addr,sizeof or->local_addr))
	    {
	    or->used=1;
	    return or->fd;
	    }
    return -1;
    }

static void close_unused_listeners()
    {
    listen_rec *or,*next;

    for(or=old_listeners ; or ; or=next)
	{
	next=or->next;
	if(!or->used)
	    close(or->fd);
	free(or);
	}
    old_listeners=NULL;
    }

/*****************************************************************
 * Executive routines.
 */

static int num_children = 0;

void standalone_main(int argc, char **argv)
{
    struct sockaddr_in sa_server;
    int saved_sd;

    standalone = 1;
    sd = listenmaxfd = -1;
    
    if (!one_process) detach(); 
    
#ifdef NEXT
    setjmp(restart_buffer);
#else
    sigsetjmp(restart_buffer,1);
#endif

    ++generation;

    signal (SIGHUP, SIG_IGN);	/* Until we're done (re)reading config */
    
    if(!one_process && !is_graceful)
    {
#ifndef NO_KILLPG
      if (killpg(pgrp,SIGHUP) < 0)    /* Kill 'em off */
#else
      if (kill(-pgrp,SIGHUP) < 0)
#endif
        log_unixerr ("killpg SIGHUP", NULL, NULL, server_conf);
    }
    
    if(is_graceful)
	{
	/* USE WITH EXTREME CAUTION. Graceful restarts are known to break */
	/*  problems will be dealt with in a future release */
	log_error("SIGUSR1 received.  Doing graceful restart",server_conf);
	kill_cleanups_for_fd(pconf,sd);
	}
    else if (sd != -1 || listenmaxfd != -1) {
	reclaim_child_processes(); /* Not when just starting up */
	log_error ("SIGHUP received.  Attempting to restart", server_conf);
    }
    
    copy_listeners(pconf);
    saved_sd=sd;
    restart_time = time(NULL);
    clear_pool (pconf);
    ptrans = make_sub_pool (pconf);
    
    server_conf = read_config(pconf, ptrans, server_confname); 
    open_logs(server_conf, pconf);
    set_group_privs();
    accept_mutex_init(pconf);
    reinit_scoreboard(pconf);
    
    default_server_hostnames (server_conf);

    if (listeners == NULL) {
        if(!is_graceful) {
	    memset((char *) &sa_server, 0, sizeof(sa_server));
	    sa_server.sin_family=AF_INET;
	    sa_server.sin_addr=bind_address;
	    sa_server.sin_port=htons(server_conf->port);

	    sd = make_sock(pconf, &sa_server);
	}
	else {
	    sd = saved_sd;
	    note_cleanups_for_fd(pconf, sd);
	}
    }
    else {
	listen_rec *lr;
	int fd;

	listenmaxfd = -1;
	FD_ZERO(&listenfds);
	for (lr=listeners; lr != NULL; lr=lr->next)
	{
	    fd=find_listener(lr);
	    if(fd < 0)
		fd = make_sock(pconf, &lr->local_addr);
	    FD_SET(fd, &listenfds);
	    if (fd > listenmaxfd) listenmaxfd = fd;
	    lr->fd=fd;
	}
	close_unused_listeners();
	sd = -1;
    }

    set_signals();
    log_pid(pconf, pid_fname);

    num_children = 0;
    
    if (daemons_max_free < daemons_min_free + 1) /* Don't thrash... */
	daemons_max_free = daemons_min_free + 1;

    while (num_children < daemons_to_start && num_children < daemons_limit) {
	make_child(server_conf, num_children++);
    }

    log_error ("Server configured -- resuming normal operations", server_conf);
    
    while (1) {
	int status, child_slot;
	int pid = wait_or_timeout(&status);
	
	if (pid >= 0) {
	    /* Child died... note that it's gone in the scoreboard. */
	    sync_scoreboard_image();
	    child_slot = find_child_by_pid (pid);
	    Explain2("Reaping child %d slot %d",pid,child_slot);
	    if (child_slot >= 0)
		(void)update_child_status (child_slot, SERVER_DEAD,
		 (request_rec*)NULL);
        }

	sync_scoreboard_image();
	if ((count_idle_servers() < daemons_min_free)
	 && (child_slot = find_free_child_num()) >= 0
	 && child_slot < daemons_limit) {
	    Explain1("Starting new child in slot %d",child_slot);
	    (void)update_child_status(child_slot,SERVER_STARTING,
	     (request_rec*)NULL);
	    if (make_child(server_conf, child_slot) < 0) {
		/* fork didn't succeed. Fix the scoreboard or else
		   it will say SERVER_STARTING forever and ever */
	        (void)update_child_status(child_slot,SERVER_DEAD,
	             (request_rec*)NULL);
	    }

	}

	/*
	if(scoreboard_image->global.please_exit && !count_live_servers())
#if defined(NEXT)  || defined(USE_LONGJMP)
	    longjmp(restart_buffer,1);
#else
	    siglongjmp(restart_buffer,1);
#endif
	*/
    }

} /* standalone_main */

extern char *optarg;
extern int optind;

int
main(int argc, char *argv[])
{
    int c;

#ifdef AUX
    (void)set42sig();
#endif

#ifdef SecureWare
    if(set_auth_parameters(argc,argv) < 0)
    	perror("set_auth_parameters");
    if(getluid() < 0)
	if(setluid(getuid()) < 0)
	    perror("setluid");
    if(setreuid(0, 0) < 0)
	perror("setreuid");
#endif

    init_alloc();
    pconf = permanent_pool;
    ptrans = make_sub_pool(pconf);

    server_argv0 = argv[0];
    strncpy (server_root, HTTPD_ROOT, sizeof(server_root)-1);
    server_root[sizeof(server_root)-1] = '\0';
    strncpy (server_confname, SERVER_CONFIG_FILE, sizeof(server_root)-1);
    server_confname[sizeof(server_confname)-1] = '\0';

    while((c = getopt(argc,argv,"Xd:f:vhl")) != -1) {
        switch(c) {
          case 'd':
            strncpy (server_root, optarg, sizeof(server_root)-1);
            server_root[sizeof(server_root)-1] = '\0';
            break;
          case 'f':
            strncpy (server_confname, optarg, sizeof(server_confname)-1);
            server_confname[sizeof(server_confname)-1] = '\0';
            break;
          case 'v':
            printf("Server version %s.\n",SERVER_VERSION);
            exit(0);
          case 'h':
	    show_directives();
	    exit(0);
	  case 'l':
	    show_modules();
	    exit(0);
	  case 'X':
	    ++one_process;	/* Weird debugging mode. */
	    break;
          case '?':
            usage(argv[0]);
        }
    }

#ifdef __EMX__
    printf("%s \n",SERVER_VERSION);
    printf("OS/2 port by Garey Smiley <garey@slink.com> \n");
#endif

    setup_prelinked_modules();
    
    suexec_enabled = init_suexec();
    server_conf = read_config (pconf, ptrans, server_confname);
    
    if(standalone) {
        clear_pool (pconf);	/* standalone_main rereads... */
        standalone_main(argc, argv);
    }
    else {
        conn_rec *conn;
	request_rec *r;
	struct sockaddr sa_server, sa_client;
	BUFF *cio;
      
	open_logs(server_conf, pconf);
	set_group_privs();
	default_server_hostnames (server_conf);

      /* Only try to switch if we're running as root */
      if(!geteuid() && setuid(user_id) == -1) {
          log_unixerr("setuid", NULL, "unable to change uid", server_conf);
          exit (1);
      }

	c = sizeof(sa_client);
	if ((getpeername(fileno(stdin), &sa_client, &c)) < 0)
	{
/* get peername will fail if the input isn't a socket */
	    perror("getpeername");
	    memset(&sa_client, '\0', sizeof(sa_client));
	}

	c = sizeof(sa_server);
	if(getsockname(fileno(stdin), &sa_server, &c) < 0) {
	    perror("getsockname");
	    fprintf(stderr, "Error getting local address\n");
	    exit(1);
	}
	server_conf->port =ntohs(((struct sockaddr_in *)&sa_server)->sin_port);
	cio = bcreate(ptrans, B_RDWR);
	cio->fd = fileno(stdout);
	cio->fd_in = fileno(stdin);
	conn = new_connection (ptrans, server_conf, cio,
			       (struct sockaddr_in *)&sa_client,
			       (struct sockaddr_in *)&sa_server,-1);
	r = read_request (conn);
	if (r) process_request (r); /* else premature EOF (ignore) */

        while (r && conn->keepalive) {
	    destroy_pool(r->pool);
            r = read_request (conn);
            if (r) process_request (r);
        }

	bflush(cio);
	bclose(cio);
    }
    exit (0);
}

#ifdef __EMX__
#ifdef HAVE_MMAP
/* The next two routines are used to access shared memory under OS/2.  */
/* This requires EMX v09c to be installed.                           */

caddr_t create_shared_heap (const char *name, size_t size)
{
    ULONG rc;
    void *mem;
    Heap_t h;

    rc = DosAllocSharedMem (&mem, name, size,
                          PAG_COMMIT | PAG_READ | PAG_WRITE);
    if (rc != 0)
        return NULL;
    h = _ucreate (mem, size, !_BLOCK_CLEAN, _HEAP_REGULAR | _HEAP_SHARED,
                NULL, NULL);
    if (h == NULL)
        DosFreeMem (mem);
    return (caddr_t)h;
}

caddr_t get_shared_heap (const char *Name)
{

    PVOID    BaseAddress;     /* Pointer to the base address of
                              the shared memory object */
    ULONG    AttributeFlags;  /* Flags describing characteristics
                              of the shared memory object */
    APIRET   rc;              /* Return code */

    /* Request read and write access to */
    /*   the shared memory object       */
    AttributeFlags = PAG_WRITE | PAG_READ;

    rc = DosGetNamedSharedMem(&BaseAddress, Name, AttributeFlags);

    if(rc != 0) {
        printf("DosGetNamedSharedMem error: return code = %ld", rc);
        return 0;
    }

    return BaseAddress;
}
#endif
#endif

