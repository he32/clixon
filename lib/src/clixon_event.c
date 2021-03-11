/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020-2021 Olof Hagsand and Rubicon Communications, LLC(Netgat)e

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 *
 * Event handling and loop
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/time.h>

#include "clixon_queue.h"
#include "clixon_log.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_err.h"
#include "clixon_sig.h"
#include "clixon_proc.h"
#include "clixon_event.h"

/*
 * Constants
 */
#define EVENT_STRLEN 32

/*
 * Types
 */
struct event_data{
    struct event_data *e_next;     /* next in list */
    int (*e_fn)(int, void*);            /* function */
    enum {EVENT_FD, EVENT_TIME} e_type;        /* type of event */
    int e_fd;                      /* File descriptor */
    struct timeval e_time;         /* Timeout */
    void *e_arg;                   /* function argument */
    char e_string[EVENT_STRLEN];             /* string for debugging */
};

/*
 * Internal variables
 * XXX consider use handle variables instead of global
 */
static struct event_data *ee = NULL;
static struct event_data *ee_timers = NULL;

/* Set if element in ee is deleted (clixon_event_unreg_fd). Check in ee loops */
static int _ee_unreg = 0;

/* If set (eg by signal handler) exit select loop on next run and return 0 */
static int _clicon_exit = 0;

/* If set (eg by signal handler) call waitpid on waiting processes, ignore EINTR, continue select loop */
static int _clicon_sig_child = 0;

/* If set (eg by signal handler) ignore EINTR and continue select loop */
static int _clicon_sig_ignore = 0;

/*! For signal handlers: instead of doing exit, set a global variable to exit
 * Status is then checked in event_loop.
 * Note it maybe would be better to do use on a handle basis, but a signal
 * handler is global
 */
int
clicon_exit_set(void)
{
    _clicon_exit++;
    return 0;
}

/*! Set exit to 0
 */
int
clicon_exit_reset(void)
{
    _clicon_exit = 0;
    return 0;
}

/*! Get the status of global exit variable, usually set by signal handlers
 */
int
clicon_exit_get(void)
{
    return _clicon_exit;
}

int
clicon_sig_child_set(int val)
{
    _clicon_sig_child = val;
    return 0;
}

int
clicon_sig_child_get(void)
{
    return _clicon_sig_child;
}

int
clicon_sig_ignore_set(int val)
{
    _clicon_sig_ignore = val;
    return 0;
}

int
clicon_sig_ignore_get(void)
{
    return _clicon_sig_ignore;
}

/*! Register a callback function to be called on input on a file descriptor.
 *
 * @param[in]  fd  File descriptor
 * @param[in]  fn  Function to call when input available on fd
 * @param[in]  arg Argument to function fn
 * @param[in]  str Describing string for logging
 * @code
 * int fn(int fd, void *arg){
 * }
 * clixon_event_reg_fd(fd, fn, (void*)42, "call fn on input on fd");
 * @endcode 
 */
int
clixon_event_reg_fd(int   fd, 
		    int (*fn)(int, void*), 
		    void *arg, 
		    char *str)
{
    struct event_data *e;

    if ((e = (struct event_data *)malloc(sizeof(struct event_data))) == NULL){
	clicon_err(OE_EVENTS, errno, "malloc");
	return -1;
    }
    memset(e, 0, sizeof(struct event_data));
    strncpy(e->e_string, str, EVENT_STRLEN);
    e->e_fd = fd;
    e->e_fn = fn;
    e->e_arg = arg;
    e->e_type = EVENT_FD;
    e->e_next = ee;
    ee = e;
    clicon_debug(2, "%s, registering %s", __FUNCTION__, e->e_string);
    return 0;
}

/*! Deregister a file descriptor callback
 * @param[in]  s   File descriptor
 * @param[in]  fn  Function to call when input available on fd
 * Note: deregister when exactly function and socket match, not argument
 * @see clixon_event_reg_fd
 * @see clixon_event_unreg_timeout
 */
int
clixon_event_unreg_fd(int   s, 
		      int (*fn)(int, void*))
{
    struct event_data *e, **e_prev;
    int found = 0;

    e_prev = &ee;
    for (e = ee; e; e = e->e_next){
	if (fn == e->e_fn && s == e->e_fd) {
	    found++;
	    *e_prev = e->e_next;
	    _ee_unreg++;
	    free(e);
	    break;
	}
	e_prev = &e->e_next;
    }
    return found?0:-1;
}

/*! Call a callback function at an absolute time
 * @param[in]  t   Absolute (not relative!) timestamp when callback is called
 * @param[in]  fn  Function to call at time t
 * @param[in]  arg Argument to function fn
 * @param[in]  str Describing string for logging
 * @code
 * int fn(int d, void *arg){
 *   struct timeval t, t1;
 *   gettimeofday(&t, NULL);
 *   t1.tv_sec = 1; t1.tv_usec = 0;
 *   timeradd(&t, &t1, &t);
 *   clixon_event_reg_timeout(t, fn, NULL, "call every second");
 * } 
 * @endcode 
 * 
 * Note that the timestamp is an absolute timestamp, not relative.
 * Note also that the callback is not periodic, you need to make a new 
 * registration for each period, see example above.
 * Note also that the first argument to fn is a dummy, just to get the same
 * signature as for file-descriptor callbacks.
 * @see clixon_event_reg_fd
 * @see clixon_event_unreg_timeout
 */
int
clixon_event_reg_timeout(struct timeval t,  
			 int          (*fn)(int, void*), 
			 void          *arg, 
			 char          *str)
{
    struct event_data *e, *e1, **e_prev;

    if ((e = (struct event_data *)malloc(sizeof(struct event_data))) == NULL){
	clicon_err(OE_EVENTS, errno, "malloc");
	return -1;
    }
    memset(e, 0, sizeof(struct event_data));
    strncpy(e->e_string, str, EVENT_STRLEN);
    e->e_fn = fn;
    e->e_arg = arg;
    e->e_type = EVENT_TIME;
    e->e_time = t;
    /* Sort into right place */
    e_prev = &ee_timers;
    for (e1=ee_timers; e1; e1=e1->e_next){
	if (timercmp(&e->e_time, &e1->e_time, <))
	    break;
	e_prev = &e1->e_next;
    }
    e->e_next = e1;
    *e_prev = e;
    clicon_debug(2, "%s: %s", __FUNCTION__, str); 
    return 0;
}

/*! Deregister a timeout callback as previosly registered by clixon_event_reg_timeout()
 * Note: deregister when exactly function and function arguments match, not time. So you
 * cannot have same function and argument callback on different timeouts. This is a little
 * different from clixon_event_unreg_fd.
 * @param[in]  fn  Function to call at time t
 * @param[in]  arg Argument to function fn
 * @see clixon_event_reg_timeout
 * @see clixon_event_unreg_fd
 */
int
clixon_event_unreg_timeout(int (*fn)(int, void*), 
			   void *arg)
{
    struct event_data *e, **e_prev;
    int found = 0;

    e_prev = &ee_timers;
    for (e = ee_timers; e; e = e->e_next){
	if (fn == e->e_fn && arg == e->e_arg) {
	    found++;
	    *e_prev = e->e_next;
	    free(e);
	    break;
	}
	e_prev = &e->e_next;
    }
    return found?0:-1;
}

/*! Poll to see if there is any data available on this file descriptor.
 * @param[in]  fd   File descriptor
 * @retval    -1    Error
 * @retval     0    Nothing to read/empty fd
 * @retval     1    Something to read on fd
 */
int 
clixon_event_poll(int fd)
{
    int            retval = -1;
    fd_set         fdset;
    struct timeval tnull = {0,};

    FD_ZERO(&fdset);
    FD_SET(fd, &fdset);
    if ((retval = select(FD_SETSIZE, &fdset, NULL, NULL, &tnull)) < 0)
	clicon_err(OE_EVENTS, errno, "select");
    return retval;
}

/*! Dispatch file descriptor events (and timeouts) by invoking callbacks.
 * There is an issue with fairness that timeouts may take over all events
 * One could try to poll the file descriptors after a timeout?
 * @retval  0  OK
 * @retval -1  Error: eg select, callback, timer, 
 */
int
clixon_event_loop(clicon_handle h)
{
    struct event_data *e;
    struct event_data *e_next;
    int                n;
    struct timeval     t;
    struct timeval     t0;
    struct timeval     tnull = {0,};
    fd_set             fdset;
    int                retval = -1;

    while (!clicon_exit_get()){
	FD_ZERO(&fdset);
	if (clicon_sig_child_get()){
	    /* Go through processes and wait for child processes */
	    if (clixon_process_waitpid(h) < 0)
		goto err;
	    clicon_sig_child_set(0);
	}
	for (e=ee; e; e=e->e_next)
	    if (e->e_type == EVENT_FD)
		FD_SET(e->e_fd, &fdset);
	if (ee_timers != NULL){
	    gettimeofday(&t0, NULL);
	    timersub(&ee_timers->e_time, &t0, &t); 
	    if (t.tv_sec < 0)
		n = select(FD_SETSIZE, &fdset, NULL, NULL, &tnull); 
	    else
		n = select(FD_SETSIZE, &fdset, NULL, NULL, &t); 
	}
	else
	    n = select(FD_SETSIZE, &fdset, NULL, NULL, NULL);
	if (clicon_exit_get())
	    break;
	if (n == -1) {
	    if (errno == EINTR){
		/* Signals are checked and are in three classes:
		 * (1) Signals that exit gracefully, the function returns 0
		 *     Must be registered such as by set_signal() of SIGTERM,SIGINT, etc with a handler that calls
		 *     clicon_exit_set().
		 * (2) SIGCHILD Childs that exit(), go through clixon_proc list and cal waitpid
		 *     New select loop is called
		 * (2) Signals are ignored, and the select is rerun, ie handler calls clicon_sig_ignore_get
		 *     New select loop is called
		 * (3) Other signals result in an error and return -1.
		 */
		clicon_debug(1, "%s select: %s", __FUNCTION__, strerror(errno));
		if (clicon_exit_get()){
		    clicon_err(OE_EVENTS, errno, "select");
		    retval = 0;
		}
		else if (clicon_sig_child_get()){
		    /* Go through processes and wait for child processes */
		    if (clixon_process_waitpid(h) < 0)
			goto err;
		    clicon_sig_child_set(0);
		    continue;
		}
		else if (clicon_sig_ignore_get()){
		    clicon_sig_ignore_set(0);
		    continue;
		}
		else
		    clicon_err(OE_EVENTS, errno, "select");
	    }
	    else
		clicon_err(OE_EVENTS, errno, "select");
	    goto err;
	}
	if (n==0){ /* Timeout */
	    e = ee_timers;
	    ee_timers = ee_timers->e_next;
	    clicon_debug(2, "%s timeout: %s", __FUNCTION__, e->e_string);
	    if ((*e->e_fn)(0, e->e_arg) < 0){
		free(e);
		goto err;
	    }
	    free(e);
	}
	_ee_unreg = 0;
	for (e=ee; e; e=e_next){
	    if (clicon_exit_get())
		break;
	    e_next = e->e_next;
	    if(e->e_type == EVENT_FD && FD_ISSET(e->e_fd, &fdset)){
		clicon_debug(2, "%s: FD_ISSET: %s", __FUNCTION__, e->e_string);
		if ((*e->e_fn)(e->e_fd, e->e_arg) < 0){
		    clicon_debug(1, "%s Error in: %s", __FUNCTION__, e->e_string);
		    goto err;

		}
		if (_ee_unreg){
		    _ee_unreg = 0;
		    break;
		}
	    }
	}
	continue;
      err:
	break;
    }
    clicon_debug(1, "%s done:%d", __FUNCTION__, retval);
    return retval;
}

int
clixon_event_exit(void)
{
    struct event_data *e, *e_next;
    
    e_next = ee;
    while ((e = e_next) != NULL){
	e_next = e->e_next;
	free(e);
    }
    ee = NULL;
    e_next = ee_timers;
    while ((e = e_next) != NULL){
	e_next = e->e_next;
	free(e);
    }
    ee_timers = NULL;
    return 0;
}
