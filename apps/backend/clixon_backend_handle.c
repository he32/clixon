/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2017 Olof Hagsand and Benny Holmgren

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

 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <fnmatch.h>
#include <sys/types.h>
#include <sys/time.h>
#include <regex.h>
#include <syslog.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_backend_handle.h"
#include "backend_client.h"
#include "backend_handle.h"

/* header part is copied from struct clicon_handle in lib/src/clicon_handle.c */

#define CLICON_MAGIC 0x99aafabe

#define handle(h) (assert(clicon_handle_check(h)==0),(struct backend_handle *)(h))

/* Clicon_handle for backends.
 * First part of this is header, same for clicon_handle and cli_handle.
 * Access functions for common fields are found in clicon lib: clicon_options.[ch]
 * This file should only contain access functions for the _specific_
 * entries in the struct below.
 */
/*! Backend specific handle added to header CLICON handle
 * This file should only contain access functions for the _specific_
 * entries in the struct below.
 * @note The top part must be equivalent to struct clicon_handle in clixon_handle.c
 * @see struct clicon_handle, struct cli_handle
 */
struct backend_handle {
    int                      bh_magic;     /* magic (HDR)*/
    clicon_hash_t           *bh_copt;      /* clicon option list (HDR) */
    clicon_hash_t           *bh_data;      /* internal clicon data (HDR) */
    /* ------ end of common handle ------ */
    struct client_entry     *bh_ce_list;   /* The client list */
    int                      bh_ce_nr;     /* Number of clients, just increment */
    struct handle_subscription *bh_subscription; /* Event subscription list */
};

/*! Creates and returns a clicon config handle for other CLICON API calls
 */
clicon_handle
backend_handle_init(void)
{
    return clicon_handle_init0(sizeof(struct backend_handle));
}

/*! Deallocates a backend handle, including all client structs
 * @Note: handle 'h' cannot be used in calls after this
 */
int
backend_handle_exit(clicon_handle h)
{
    struct client_entry   *ce;

    /* only delete client structs, not close sockets, etc, see backend_client_rm */
    while ((ce = backend_client_list(h)) != NULL)
	backend_client_delete(h, ce);
    clicon_handle_exit(h); /* frees h and options */
    return 0;
}

/*! Notify event and distribute to all registered clients
 * 
 * @param[in]  h       Clicon handle
 * @param[in]  stream  Name of event stream. CLICON is predefined as LOG stream
 * @param[in]  level   Event level (not used yet)
 * @param[in]  event   Actual message as text format
 *
 * Stream is a string used to qualify the event-stream. Distribute the
 * event to all clients registered to this backend.  
 * XXX: event-log NYI.  
 * @see also subscription_add()
 * @see also backend_notify_xml()
 */
int
backend_notify(clicon_handle h, 
	       char         *stream, 
	       int           level, 
	       char         *event)
{
    struct client_entry        *ce;
    struct client_entry        *ce_next;
    struct client_subscription *su;
    struct handle_subscription *hs;
    int                  retval = -1;

    clicon_debug(2, "%s %s", __FUNCTION__, stream);
    /* First thru all clients(sessions), and all subscriptions and find matches */
    for (ce = backend_client_list(h); ce; ce = ce_next){
	ce_next = ce->ce_next;
	for (su = ce->ce_subscription; su; su = su->su_next)
	    if (strcmp(su->su_stream, stream) == 0){
		if (strlen(su->su_filter)==0 || fnmatch(su->su_filter, event, 0) == 0){
		    if (send_msg_notify(ce->ce_s, level, event) < 0){
			if (errno == ECONNRESET || errno == EPIPE){
			    clicon_log(LOG_WARNING, "client %d reset", ce->ce_nr);
#if 0
			    /* We should remove here but removal is not possible
			       from a client since backend_client is not linked.
			       Maybe we should add it to the plugin, but it feels
			       "safe" that you cant remove a client.
			       Instead, the client is (hopefully) removed elsewhere?
			    */
			    backend_client_rm(h, ce);
#endif
			    break;
			}
			goto done;
		    }
		}
	    }
    }
    /* Then go thru all global (handle) subscriptions and find matches */
    hs = NULL;
    while ((hs = subscription_each(h, hs)) != NULL){
	if (hs->hs_format != FORMAT_TEXT)
	    continue;
	if (strcmp(hs->hs_stream, stream))
	    continue;
	if (hs->hs_filter==NULL ||
	    strlen(hs->hs_filter)==0 || 
	    fnmatch(hs->hs_filter, event, 0) == 0)
	    if ((*hs->hs_fn)(h, event, hs->hs_arg) < 0)
		goto done;
    }
    retval = 0;
  done:
    return retval;
}

/*! Notify event and distribute to all registered clients
 * 
 * @param[in]  h       Clicon handle
 * @param[in]  stream  Name of event stream. CLICON is predefined as LOG stream
 * @param[in]  level   Event level (not used yet)
 * @param[in]  x       Actual message as xml tree
 *
 * Stream is a string used to qualify the event-stream. Distribute the
 * event to all clients registered to this backend.  
 * XXX: event-log NYI.  
 * @see also subscription_add()
 * @see also backend_notify()
 */
int
backend_notify_xml(clicon_handle h, 
		   char         *stream, 
		   int           level, 
		   cxobj        *x)
{
    struct client_entry *ce;
    struct client_entry *ce_next;
    struct client_subscription *su;
    int                  retval = -1;
    cbuf                *cb = NULL;
    struct handle_subscription *hs;

    clicon_debug(1, "%s %s", __FUNCTION__, stream);
    /* Now go thru all clients(sessions), and all subscriptions and find matches */
    for (ce = backend_client_list(h); ce; ce = ce_next){
	ce_next = ce->ce_next;
	for (su = ce->ce_subscription; su; su = su->su_next)
	    if (strcmp(su->su_stream, stream) == 0){
		if (strlen(su->su_filter)==0 || xpath_first(x, su->su_filter) != NULL){
		    if (cb==NULL){
			if ((cb = cbuf_new()) == NULL){
			    clicon_err(OE_PLUGIN, errno, "cbuf_new");
			    goto done;
			}
			if (clicon_xml2cbuf(cb, x, 0, 0) < 0)
			    goto done;
		    }
		    if (send_msg_notify(ce->ce_s, level, cbuf_get(cb)) < 0){
			if (errno == ECONNRESET || errno == EPIPE){
			    clicon_log(LOG_WARNING, "client %d reset", ce->ce_nr);
#if 0
			    /* We should remove here but removal is not possible
			       from a client since backend_client is not linked.
			       Maybe we should add it to the plugin, but it feels
			       "safe" that you cant remove a client.
			       Instead, the client is (hopefully) removed elsewhere?
			    */
			    backend_client_rm(h, ce);
#endif
			    break;
			}
			goto done;
		    }
		}
	    }
    }
    /* Then go thru all global (handle) subscriptions and find matches */
    hs = NULL;
    while ((hs = subscription_each(h, hs)) != NULL){
	if (hs->hs_format != FORMAT_XML)
	    continue;
	if (strcmp(hs->hs_stream, stream))
	    continue;
	if (strlen(hs->hs_filter)==0 || xpath_first(x, hs->hs_filter) != NULL){
	    if ((*hs->hs_fn)(h, x, hs->hs_arg) < 0)
		goto done;
	}
    }
    retval = 0;
  done:
    if (cb)
	cbuf_free(cb);
    return retval;

}

/*! Add new client, typically frontend such as cli, netconf, restconf
 * @param[in]  h        Clicon handle
 * @param[in]  addr     Address of client
 * @retval     ce       Client entry
 * @retval     NULL     Error
 */
struct client_entry *
backend_client_add(clicon_handle    h, 
		   struct sockaddr *addr)
{
    struct backend_handle *bh = handle(h);
    struct client_entry   *ce;

    if ((ce = (struct client_entry *)malloc(sizeof(*ce))) == NULL){
	clicon_err(OE_PLUGIN, errno, "malloc");
	return NULL;
    }
    memset(ce, 0, sizeof(*ce));
    ce->ce_nr = bh->bh_ce_nr++;
    memcpy(&ce->ce_addr, addr, sizeof(*addr));
    ce->ce_next = bh->bh_ce_list;
    bh->bh_ce_list = ce;
    return ce;
}

/*! Return client list
 * @param[in]  h        Clicon handle
 * @retval     ce_list  Client entry list (all sessions)
 */
struct client_entry *
backend_client_list(clicon_handle h)
{
    struct backend_handle *bh = handle(h);

    return bh->bh_ce_list;
}

/*! Actually remove client from client list
 * @param[in]  h   Clicon handle
 * @param[in]  ce  Client handle
 * @see backend_client_rm which is more high-level
 */
int
backend_client_delete(clicon_handle        h,
		      struct client_entry *ce)
{
    struct client_entry   *c;
    struct client_entry  **ce_prev;
    struct backend_handle *bh = handle(h);

    ce_prev = &bh->bh_ce_list;
    for (c = *ce_prev; c; c = c->ce_next){
	if (c == ce){
	    *ce_prev = c->ce_next;
	    free(ce);
	    break;
	}
	ce_prev = &c->ce_next;
    }
    return 0;
}

/*! Add subscription given stream name, callback and argument 
 * @param[in]  h      Clicon handle
 * @param[in]  stream Name of event stream
 * @param[in]  format Expected format of event, eg text or xml
 * @param[in]  filter Filter to match event, depends on format, eg xpath for xml
 * @param[in]  fn     Callback when event occurs
 * @param[in]  arg    Argument to use with callback. Also handle when deleting
 * Note that arg is not a real handle.
 * @see subscription_delete
 * @see subscription_each
 */
struct handle_subscription *
subscription_add(clicon_handle        h,
		 char                *stream, 
		 enum format_enum     format,
		 char                *filter,
		 subscription_fn_t    fn,
		 void                *arg)
{
    struct backend_handle *bh = handle(h);
    struct handle_subscription *hs = NULL;

    if ((hs = malloc(sizeof(*hs))) == NULL){
	clicon_err(OE_PLUGIN, errno, "malloc");
	goto done;
    }
    memset(hs, 0, sizeof(*hs));
    hs->hs_stream = strdup(stream);
    hs->hs_format = format;
    hs->hs_filter = filter?strdup(filter):NULL;
    hs->hs_next   = bh->bh_subscription;
    hs->hs_fn     = fn;
    hs->hs_arg    = arg;
    bh->bh_subscription = hs;
  done:
    return hs;
}

/*! Delete subscription given stream name, callback and argument 
 * @param[in]  h      Clicon handle
 * @param[in]  stream Name of event stream
 * @param[in]  fn     Callback when event occurs
 * @param[in]  arg    Argument to use with callback and handle
 * Note that arg is not a real handle.
 * @see subscription_add
 * @see subscription_each
 */
int
subscription_delete(clicon_handle     h,
		    char             *stream, 
		    subscription_fn_t fn,
		    void             *arg)
{
    struct backend_handle *bh = handle(h);
    struct handle_subscription   *hs;
    struct handle_subscription  **hs_prev;

    hs_prev = &bh->bh_subscription; /* this points to stack and is not real backpointer */
    for (hs = *hs_prev; hs; hs = hs->hs_next){
	/* XXX arg == hs->hs_arg */
	if (strcmp(hs->hs_stream, stream)==0 && hs->hs_fn == fn){
	    *hs_prev = hs->hs_next;
	    free(hs->hs_stream);
	    if (hs->hs_filter)
		free(hs->hs_filter);
	    if (hs->hs_arg)
		free(hs->hs_arg);
	    free(hs);
	    break;
	}
	hs_prev = &hs->hs_next;
    }
    return 0;
}

/*! Iterator over subscriptions
 *
 * NOTE: Never manipulate the child-list during operation or using the
 * same object recursively, the function uses an internal field to remember the
 * index used. It works as long as the same object is not iterated concurrently. 
 *
 * @param[in] h     clicon handle 
 * @param[in] hprev iterator, initialize with NULL
 * @code
 *   clicon_handle h;
 *   struct handle_subscription *hs = NULL;
 *   while ((hs = subscription_each(h, hs)) != NULL) {
 *     ...
 *   }
 * @endcode
 */
struct handle_subscription *
subscription_each(clicon_handle               h,
		  struct handle_subscription *hprev)
{
    struct backend_handle      *bh = handle(h);
    struct handle_subscription *hs = NULL;

    if (hprev)
	hs = hprev->hs_next;
    else
	hs = bh->bh_subscription;
    return hs;
}

/* Database dependency description */
struct backend_netconf_reg {
    qelem_t 	 nr_qelem;	/* List header */
    backend_netconf_cb_t nr_callback;	/* Validation/Commit Callback */
    void	*nr_arg;	/* Application specific argument to cb */
    char        *nr_tag;	/* Xml tag when matched, callback called */
};
typedef struct backend_netconf_reg backend_netconf_reg_t;

static backend_netconf_reg_t *deps = NULL;
/*! Register netconf callback
 * Called from plugin to register a callback for a specific netconf XML tag.
 */
int
backend_netconf_register_callback(clicon_handle h,
				  backend_netconf_cb_t cb,      /* Callback called */
				  void *arg,        /* Arg to send to callback */
				  char *tag)        /* Xml tag when callback is made */
{
    backend_netconf_reg_t *nr;

    if ((nr = malloc(sizeof(backend_netconf_reg_t))) == NULL) {
	clicon_err(OE_DB, errno, "malloc: %s", strerror(errno));
	goto catch;
    }
    memset (nr, 0, sizeof (*nr));
    nr->nr_callback = cb;
    nr->nr_arg  = arg;
    nr->nr_tag  = strdup(tag); /* XXX strdup memleak */
    INSQ(nr, deps);
    return 0;
catch:
    if (nr){
	if (nr->nr_tag)
	    free(nr->nr_tag);
	free(nr);
    }
    return -1;
}

/*! See if there is any callback registered for this tag
 *
 * @param[in]  h       clicon handle
 * @param[in]  xe      Sub-tree (under xorig) at child of rpc: <rpc><xn></rpc>.
 * @param[in]  ce      Client (session) entry
 * @param[out] cbret   Return XML, error or OK as cbuf
 *
 * @retval -1   Error
 * @retval  0   OK, not found handler.
 * @retval  1   OK, handler called
 */
int
backend_netconf_plugin_callbacks(clicon_handle        h,
				 cxobj               *xe,
				 struct client_entry *ce,
				 cbuf                *cbret)
{
    backend_netconf_reg_t *nreg;
    int            retval;

    if (deps == NULL)
	return 0;
    nreg = deps;
    do {
	if (strcmp(nreg->nr_tag, xml_name(xe)) == 0){
	    if ((retval = nreg->nr_callback(h, xe, ce, cbret, nreg->nr_arg)) < 0)
		return -1;
	    else
		return 1; /* handled */
	}
	nreg = NEXTQ(backend_netconf_reg_t *, nreg);
    } while (nreg != deps);
    return 0;
}

