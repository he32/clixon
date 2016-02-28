/*
 *
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren

  This file is part of CLIXON.

  CLIXON is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 3 of the License, or
  (at your option) any later version.

  CLIXON is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with CLIXON; see the file LICENSE.  If not, see
  <http://www.gnu.org/licenses/>.

 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <syslog.h>
#include <grp.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/param.h>
#ifdef HAVE_SYS_UCRED_H
#include <sys/types.h>
#include <sys/ucred.h>
#endif
#define __USE_GNU
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/types.h>

#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "backend_socket.h"
#include "backend_client.h"
#include "backend_handle.h"

static int
config_socket_init_ipv4(clicon_handle h, char *dst)
{
    int                s;
    struct sockaddr_in addr;
    uint16_t           port;

    port = clicon_sock_port(h);

    /* create inet socket */
    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
	clicon_err(OE_UNIX, errno, "socket");
	return -1;
    }
//    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (void*)&one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(addr.sin_family, dst, &addr.sin_addr) != 1){
	clicon_err(OE_UNIX, errno, "inet_pton: %s (Expected IPv4 address. Check settings of CLICON_SOCK_FAMILY and CLICON_SOCK)", dst);
	goto err; /* Could check getaddrinfo */
    }
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0){
	clicon_err(OE_UNIX, errno, "%s: bind", __FUNCTION__);
	goto err;
    }
    clicon_debug(1, "Listen on server socket at %s:%hu", dst, port);
    if (listen(s, 5) < 0){
	clicon_err(OE_UNIX, errno, "%s: listen", __FUNCTION__);
	goto err;
    }
    return s;
  err:
    close(s);
    return -1;
}

/*! Open a socket and bind it to a file descriptor
 *
 * The socket is accessed via CLICON_SOCK option, has 770 permissions
 * and group according to CLICON_SOCK_GROUP option.
 */
static int
config_socket_init_unix(clicon_handle h, char *sock)
{
    int                s;
    struct sockaddr_un addr;
    mode_t             old_mask;
    char              *config_group;
    gid_t              gid;
    struct stat        st;

    if (lstat(sock, &st) == 0 && unlink(sock) < 0){
	clicon_err(OE_UNIX, errno, "%s: unlink(%s)", __FUNCTION__, sock);
	return -1;
    }
    /* then find configuration group (for clients) and find its groupid */
    if ((config_group = clicon_sock_group(h)) == NULL){
	clicon_err(OE_FATAL, 0, "clicon_sock_group option not set");
	return -1;
    }
    if (group_name2gid(config_group, &gid) < 0)
	return -1;
#if 0
    if (gid == 0) 
	clicon_log(LOG_WARNING, "%s: No such group: %s\n", __FUNCTION__, config_group);
#endif
    /* create unix socket */
    if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
	clicon_err(OE_UNIX, errno, "%s: socket", __FUNCTION__);
	return -1;
    }
//    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (void*)&one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock, sizeof(addr.sun_path)-1);
    old_mask = umask(S_IRWXO | S_IXGRP | S_IXUSR);
    if (bind(s, (struct sockaddr *)&addr, SUN_LEN(&addr)) < 0){
	clicon_err(OE_UNIX, errno, "%s: bind", __FUNCTION__);
	umask(old_mask); 
	goto err;
    }
    umask(old_mask); 
    /* change socket path file group */
    if (lchown(sock, -1, gid) < 0){
	clicon_err(OE_UNIX, errno, "%s: lchown(%s, %s)", __FUNCTION__, 
		sock, config_group);
	goto err;
    }
    clicon_debug(1, "Listen on server socket at %s", addr.sun_path);
    if (listen(s, 5) < 0){
	clicon_err(OE_UNIX, errno, "%s: listen", __FUNCTION__);
	goto err;
    }
    return s;
  err:
    close(s);
    return -1;
}

int
config_socket_init(clicon_handle h)
{
    char *sock;

    if ((sock = clicon_sock(h)) == NULL){
	clicon_err(OE_FATAL, 0, "CLICON_SOCK option not set");
	return -1;
    }
    switch (clicon_sock_family(h)){
    case AF_UNIX:
	return config_socket_init_unix(h, sock);
	break;
    case AF_INET:
	return config_socket_init_ipv4(h, sock);
	break;
    }
    return 0;
}

/*
 * config_accept_client
 * XXX: credentials not properly implemented
 */
int
config_accept_client(int fd, void *arg)
{
    int           retval = -1;
    clicon_handle h = (clicon_handle)arg;
    int           s;
    struct sockaddr_un from;
    socklen_t     len;
    struct client_entry *ce;
#ifdef DONT_WORK /* XXX HAVE_SYS_UCRED_H */
    struct xucred credentials; 	/* FreeBSD. */
    socklen_t     clen;
#elif defined(SO_PEERCRED)
    struct ucred credentials; 	/* Linux. */
    socklen_t     clen;
#endif
    char         *config_group;
    struct group *gr;
    char         *mem;
    int           i;

    clicon_debug(1, "%s", __FUNCTION__);
    len = sizeof(from);
    if ((s = accept(fd, (struct sockaddr*)&from, &len)) < 0){
	clicon_err(OE_UNIX, errno, "%s: accept", __FUNCTION__);
	goto done;
    }
#if defined(SO_PEERCRED)
    /* fill in the user data structure */
    clen =  sizeof(credentials);
    if(getsockopt(s, SOL_SOCKET, SO_PEERCRED/* XXX finns ej i freebsd*/, &credentials, &clen)){
	clicon_err(OE_UNIX, errno, "%s: getsockopt", __FUNCTION__);
	goto done;
    }
#endif   
    if ((ce = backend_client_add(h, (struct sockaddr*)&from)) == NULL)
	goto done;
#if defined(SO_PEERCRED)
    ce->ce_pid = credentials.pid;
    ce->ce_uid = credentials.uid;
#endif
    ce->ce_handle = h;

    /* check credentials of caller (not properly implemented yet) */
    if ((config_group = clicon_sock_group(h)) == NULL){
	clicon_err(OE_FATAL, 0, "clicon_sock_group option not set");
	goto done;
    }
    if ((gr = getgrnam(config_group)) != NULL){
	i = 0; /* one of mem should correspond to ce->ce_uid */
	while ((mem = gr->gr_mem[i++]) != NULL)
	    ;
    }

#if 0
    { /* XXX */
	int ii;
	struct client_entry *c;
	for (c = ce_list, ii=0; c; c = c->ce_next, ii++);
	clicon_debug(1, "Open client socket (nr:%d pid:%d [Total: %d])",
		ce->ce_nr, ce->ce_pid, ii);
    }
#endif
    ce->ce_s = s;

    /*
     * Here we register callbacks for actual data socket 
     */
    if (event_reg_fd(s, from_client, (void*)ce, "client socket") < 0)
	goto done;
    retval = 0;
 done:
    return retval;
}


