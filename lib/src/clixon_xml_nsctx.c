/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand and Benny Holmgren

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

 * XML support functions.
 * @see     https://www.w3.org/TR/2009/REC-xml-names-20091208
 * An xml namespace context is a cligen variable vector containing a list of
 * <prefix,namespace> pairs.
 * It is encoded in a cvv as a list of string values, where the c name is the 
 * prefix and the string values are the namespace URI.
 * The default namespace is decoded as having the name NULL
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include <assert.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_err.h"
#include "clixon_string.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_log.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_xml_nsctx.h"

/*! Create and initialize XML namespace context
 * @param[in] prefix    Namespace prefix, or NULL for default
 * @param[in] namespace Set this namespace. If NULL create empty nsctx
 * @retval    nsc       Return namespace context in form of a cvec
 * @retval    NULL      Error
 * @code
 * cvec *nsc = NULL;
 * if ((nsc = xml_nsctx_init(NULL, "urn:example:example")) == NULL)
 *   err;
 * ...
 * xml_nsctx_free(nsc);
 * @endcode
 * @see xml_nsctx_node  Use namespace context of an existing XML node
 * @see xml_nsctx_free  Free the reutned handle
 */
cvec *
xml_nsctx_init(char  *prefix,
	       char  *namespace)
{
    cvec *cvv = NULL;

    if ((cvv = cvec_new(0)) == NULL){
	clicon_err(OE_XML, errno, "cvec_new");
	goto done;
    }
    if (namespace && xml_nsctx_add(cvv, prefix, namespace) < 0)
	goto done;
 done:
    return cvv;
}

/*! Free XML namespace context
 * @param[in] prefix    Namespace prefix, or NULL for default
 * @param[in] namespace Cached namespace to set (assume non-null?)
 * @retval    nsc       Return namespace context in form of a cvec
 * @retval    NULL      Error
 */
int
xml_nsctx_free(cvec *nsc)
{
    cvec *cvv = (cvec*)nsc;

    if (cvv)
	cvec_free(cvv);
    return 0;
}

/*! Get namespace given prefix (or NULL for default) from namespace context
 * @param[in] cvv    Namespace context
 * @param[in] prefix Namespace prefix, or NULL for default
 * @retval    ns     Cached namespace
 * @retval    NULL   No namespace found (not cached or not found)
 */
char*
xml_nsctx_get(cvec *cvv,
	      char *prefix)
{
    cg_var *cv;
    
    if ((cv = cvec_find(cvv, prefix)) != NULL)
	return cv_string_get(cv);
    return NULL;
}

/*! Reverse get prefix given namespace
 * @param[in]  cvv    Namespace context
 * @param[in]  ns     Namespace 
 * @param[out] prefix Prefix (direct pointer)
 * @retval     0      No prefix found
 * @retval     1      Prefix found
 * @note NULL is a valid prefix (default)
 */
int
xml_nsctx_get_prefix(cvec  *cvv,
		     char  *namespace,
		     char **prefix)
{
    cg_var *cv = NULL;
    char   *ns = NULL;

    while ((cv = cvec_each(cvv, cv)) != NULL){
	if ((ns = cv_string_get(cv)) != NULL &&
	    strcmp(ns, namespace) == 0){
	    *prefix = cv_name_get(cv); /* can be NULL */
	    return 1;
	}
    }
    *prefix = NULL;
    return 0;
}

/*! Set or replace namespace in namespace context
 * @param[in] cvv       Namespace context
 * @param[in] prefix    Namespace prefix, or NULL for default
 * @param[in] namespace Cached namespace to set (assume non-null?)
 * @retval    0         OK
 * @retval   -1         Error
 */
int
xml_nsctx_add(cvec  *cvv,
	      char  *prefix,
	      char  *namespace)
{
    int     retval = -1;
    cg_var *cv;
    
    if ((cv = cvec_find(cvv, prefix)) != NULL) /* found, replace that */
	cv_string_set(cv, namespace);
    else /* cvec exists, but not prefix */
	cvec_add_string(cvv, prefix, namespace);
    retval = 0;
    // done:
    return retval;
}

static int
xml_nsctx_node1(cxobj *xn,
		cvec   *nsc)
{
    int    retval = -1;
    cxobj *xa = NULL;
    char  *pf;  /* prefix */
    char  *nm;  /* name */
    char  *val; /* value */
    cxobj *xp;  /* parent */

    /* xmlns:t="<ns1>" prefix:xmlns, name:t
     * xmlns="<ns2>"   prefix:NULL   name:xmlns
     */
    while ((xa = xml_child_each(xn, xa, CX_ATTR)) != NULL){
	pf = xml_prefix(xa);
	nm = xml_name(xa);
	if (pf == NULL){
	    if (strcmp(nm, "xmlns")==0 && /* set default namespace context */
		xml_nsctx_get(nsc, NULL) == NULL){
		val = xml_value(xa);
		if (xml_nsctx_add(nsc, NULL, val) < 0)
		    goto done;
	    }
	}
	else
	    if (strcmp(pf, "xmlns")==0 && /* set prefixed namespace context */
		xml_nsctx_get(nsc, nm) == NULL){
		val = xml_value(xa);
		if (xml_nsctx_add(nsc, nm, val) < 0)
		    goto done;
	    }
    }
    if ((xp = xml_parent(xn)) == NULL){
#ifdef USE_NETCONF_NS_AS_DEFAULT
	/* If not default namespace defined, use the base netconf ns as default */
	if (xml_nsctx_get(nsc, NULL) == NULL)
	    if (xml_nsctx_add(nsc, NULL, NETCONF_BASE_NAMESPACE) < 0)
		goto done;
#endif
    }
    else
	if (xml_nsctx_node1(xp, nsc) < 0)
	    goto done;
    retval = 0;
 done:
    return retval;
}

/*! Create and initialize XML namespace from XML node context
 * Fully explore all prefix:namespace pairs from context of one node
 * @param[in]  xn     XML node
 * @param[out] ncp    XML namespace context
 * @retval     0      OK
 * @retval     -1     Error
 * @code
 * cxobj *x; // must initialize
 * cvec *nsc = NULL;
 * if (xml_nsctx_node(x, &nsc) < 0)
 *   err
 * ...
 * xml_nsctx_free(nsc)
 * @endcode
 * @see xml_nsctx_init
 * @see xml_nsctx_free  Free the returned handle
 */
int
xml_nsctx_node(cxobj *xn,
	       cvec **ncp)
{
    int   retval = -1;
    cvec *nc = NULL;
    
    if ((nc = cvec_new(0)) == NULL){
	clicon_err(OE_XML, errno, "cvec_new");
	goto done;
    }
    if (xml_nsctx_node1(xn, nc) < 0)
	goto done;
    *ncp = nc;
    retval = 0;
 done:
    return retval;
}

/*! Create and initialize XML namespace context from Yang node
 * Primary use is Yang path statements, eg leafrefs and others
 * Fully explore all prefix:namespace pairs from context of one node
 * @param[in]  yn     Yang statement in module tree (or module itself)
 * @param[out] ncp    XML namespace context
 * @retval     0      OK
 * @retval     -1     Error
 * @code
 * yang_stmt *y; // must initialize
 * cvec *nsc = NULL;
 * if (xml_nsctx_yang(y, &nsc) < 0)
 *   err
 * ...
 * xml_nsctx_free(nsc)
 * @endcode
 * @see RFC7950 Sections 6.4.1 (and 9.9.2?)
 */
int
xml_nsctx_yang(yang_stmt *yn,
	       cvec     **ncp)
{
    int        retval = -1;
    cvec      *nc = NULL;
    yang_stmt *yspec;
    yang_stmt *ymod;  /* yang main module/submodule node */
    yang_stmt *yp;    /* yang prefix node */
    yang_stmt *ym;    /* yang imported module */
    yang_stmt *yns;   /* yang namespace */
    yang_stmt *y;
    char      *name;
    char      *namespace;
    char      *prefix;
    char      *mynamespace;
    char      *myprefix;
    
    if ((nc = cvec_new(0)) == NULL){
	clicon_err(OE_XML, errno, "cvec_new");
	goto done;
    }
    if ((myprefix = yang_find_myprefix(yn)) == NULL){
	clicon_err(OE_YANG, ENOENT, "My yang prefix not found");
	goto done;
    }
    if ((mynamespace = yang_find_mynamespace(yn)) == NULL){
	clicon_err(OE_YANG, ENOENT, "My yang namespace not found");
	goto done;
    }
    /* Add my prefix and default namespace (from real module) */
    if (xml_nsctx_add(nc, NULL, mynamespace) < 0)
	goto done;
    if (xml_nsctx_add(nc, myprefix, mynamespace) < 0)
	goto done;
    /* Find top-most module or sub-module and get prefixes from that */
    if ((ymod = ys_module(yn)) == NULL){
	clicon_err(OE_YANG, ENOENT, "My yang module not found");
	goto done;
    }
    yspec = yang_parent_get(ymod); /* Assume yspec exists */

    /* Iterate over module and register all import prefixes
     */
    y = NULL;
    while ((y = yn_each(ymod, y)) != NULL) {
	if (yang_keyword_get(y) == Y_IMPORT){
	    if ((name = yang_argument_get(y)) == NULL)
		continue; /* Just skip - shouldnt happen) */
	    if ((yp = yang_find(y, Y_PREFIX, NULL)) == NULL)
		continue; 
	    if ((prefix = yang_argument_get(yp)) == NULL)
		continue;
	    if ((ym = yang_find(yspec, Y_MODULE, name)) == NULL)
		continue;
	    if ((yns = yang_find(ym, Y_NAMESPACE, NULL)) == NULL)
		continue;
	    if ((namespace = yang_argument_get(yns)) == NULL)
		continue;
	    if (xml_nsctx_add(nc, prefix, namespace) < 0)
		goto done;
	}
    }
    *ncp = nc;
    retval = 0;
 done:
    return retval;
}

/*! Create and initialize XML namespace context from Yang spec
 *
 * That is, create a "canonical" XML namespace mapping from all loaded yang 
 * modules which are children of the yang specification.
 * ALso add netconf base namespace: nc , urn:ietf:params:xml:ns:netconf:base:1.0
 * Fully explore all prefix:namespace pairs of all yang modules
 * @param[in]  yspec  Yang spec
 * @param[out] ncp    XML namespace context
 */
int
xml_nsctx_yangspec(yang_stmt *yspec,
		   cvec     **ncp)
{
    int        retval = -1;
    cvec      *nc = NULL;
    yang_stmt *ymod = NULL;
    yang_stmt *yprefix;
    yang_stmt *ynamespace;

    if ((nc = cvec_new(0)) == NULL){
	clicon_err(OE_XML, errno, "cvec_new");
	goto done;
    }
    ymod = NULL;
    while ((ymod = yn_each(yspec, ymod)) != NULL){
	if (yang_keyword_get(ymod) != Y_MODULE)
	    continue;
	if ((yprefix = yang_find(ymod, Y_PREFIX, NULL)) == NULL)
	    continue;
	if ((ynamespace = yang_find(ymod, Y_NAMESPACE, NULL)) == NULL)
	    continue;
	if (xml_nsctx_add(nc, yang_argument_get(yprefix), yang_argument_get(ynamespace)) < 0)
	    goto done;
    }
    if (xml_nsctx_add(nc,  NETCONF_BASE_PREFIX, NETCONF_BASE_NAMESPACE) < 0)
	goto done;
    *ncp = nc;
    retval = 0;
 done:
    return retval;
}
