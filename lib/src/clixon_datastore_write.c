/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand
  Copyright (C) 2020-2021 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <syslog.h>       
#include <fcntl.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_err.h"
#include "clixon_string.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_log.h"
#include "clixon_file.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_xml_sort.h"
#include "clixon_options.h"
#include "clixon_data.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_json.h"
#include "clixon_nacm.h"
#include "clixon_netconf_lib.h"
#include "clixon_yang_type.h"
#include "clixon_yang_module.h"
#include "clixon_xml_nsctx.h"
#include "clixon_xml_io.h"
#include "clixon_xml_map.h"
#include "clixon_datastore.h"
#include "clixon_datastore_write.h"
#include "clixon_datastore_read.h"

/*! Given an attribute name and its expected namespace, find its value
 * 
 * An attribute may have a prefix(or NULL). The routine finds the associated
 * xmlns binding to find the namespace: <namespace>:<name>.
 * If such an attribute is not found, failure is returned with cbret set,
 * If such an attribute its found, its string value is returned.
 * @param[in]  x         XML node (where to look for attribute)
 * @param[in]  name      Attribute name
 * @param[in]  ns	     (Expected)Namespace of attribute
 * @param[out] cbret     Error message (if retval=0)
 * @param[out] valp      Pointer to value (if retval=1)
 * @retval    -1         Error
 * @retval     0         Failed (cbret set)
 * @retval     1         OK
 */
static int
attr_ns_value(cxobj *x,
	      char  *name,
	      char  *ns,
	      cbuf  *cbret,
	      char **valp)
{
    int    retval = -1;
    cxobj *xa;
    char  *ans = NULL; /* attribute namespace */
    char  *val = NULL;

    /* prefix=NULL since we do not know the prefix */
    if ((xa = xml_find_type(x, NULL, name, CX_ATTR)) != NULL){ 
	if (xml2ns(xa, xml_prefix(xa), &ans) < 0)
	    goto done;
	if (ans == NULL){ /* the attribute exists, but no namespace */
	    if (netconf_bad_attribute(cbret, "application", name, "Unresolved attribute prefix (no namespace?)") < 0)
		goto done;
	    goto fail;
	}
	/* the attribute exists, but not w expected namespace */
	if (ns == NULL ||
	    strcmp(ans, ns) == 0)
	    val = xml_value(xa);
    }
    *valp = val;
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! When new body is added, some needs type lookup is made and namespace checked
 * This includes identityrefs, paths
 * This code identifies x0 as an identityref, looks at the _body_ string and ensures the right
 * namespace is inserted in x1.
 */
static int
check_body_namespace(cxobj     *x0,
		     cxobj     *x1,
		     cxobj     *x1p,		  
		     char      *x1bstr,
		     yang_stmt *y)
{
    int        retval = -1;
    char      *prefix = NULL;
    char      *ns0 = NULL;
    char      *ns1 = NULL;
    cxobj     *xa;
    cxobj     *x;
    int        isroot;

    /* XXX: need to identify root better than hiereustics and strcmp,... */
    isroot = xml_parent(x1p)==NULL &&
	strcmp(xml_name(x1p), "config") == 0 &&
	xml_prefix(x1p)==NULL;
    if (nodeid_split(x1bstr, &prefix, NULL) < 0)
	goto done;
    if (prefix == NULL)
	goto ok; /* skip */
    if (xml2ns(x0, prefix, &ns0) < 0)
	goto done;
    if (xml2ns(x1, prefix, &ns1) < 0)
	goto done;
    if (ns0 != NULL){
	if (ns1){
	    if (strcmp(ns0, ns1)){
		clicon_err(OE_YANG, EFAULT, "identity namespace collision: %s: %s vs %s", x1bstr, ns0, ns1);
		goto done;
	    }
	}
	else{
	    if (isroot)
		x = x1;
	    else
		x = x1p;
	    if (nscache_set(x, prefix, ns0) < 0)
		goto done;
	    /* Create xmlns attribute to x1 XXX same code ^*/
	    if (prefix){
		if ((xa = xml_new(prefix, x, CX_ATTR)) == NULL)
		    goto done;
		if (xml_prefix_set(xa, "xmlns") < 0)
		    goto done;

	    }
	    else{
		if ((xa = xml_new("xmlns", x, CX_ATTR)) == NULL)
		    goto done;
	    }
	    if (xml_value_set(xa, ns0) < 0)
		goto done;
	    xml_sort(x); /* Ensure attr is first / XXX xml_insert? */
	}
    }
 ok:
    retval = 0;
 done:
    if (prefix)
	free(prefix);
    return retval;
}

/*! Modify a base tree x0 with x1 with yang spec y according to operation op
 * @param[in]  h        Clicon handle
 * @param[in]  x0       Base xml tree (can be NULL in add scenarios)
 * @param[in]  y0       Yang spec corresponding to xml-node x0. NULL if x0 is NULL
 * @param[in]  x0p      Parent of x0
 * @param[in]  x1       XML tree which modifies base
 * @param[in]  x1t      Request root node (nacm needs this)
 * @param[in]  op       OP_MERGE, OP_REPLACE, OP_REMOVE, etc 
 * @param[in]  username User name of requestor for nacm
 * @param[in]  xnacm    NACM XML tree (only if !permit)
 * @param[in]  permit   If set, no NACM tests using xnacm required
 * @param[out] cbret    Initialized cligen buffer. Contains return XML if retval is 0.
 * @retval    -1        Error
 * @retval     0        Failed (cbret set)
 * @retval     1        OK
 * Assume x0 and x1 are same on entry and that y is the spec
 * @see text_modify_top
 * RFC 7950 Sec 7.7.9(leaf-list), 7.8.6(lists)
 * In an "ordered-by user" list, the attributes "insert" and "key" in
 * the YANG XML namespace can be used to control where
 * in the list the entry is inserted.
 */
static int
text_modify(clicon_handle       h,
	    cxobj              *x0,
	    cxobj              *x0p,
	    cxobj              *x0t,
	    cxobj              *x1,
	    cxobj              *x1t,
	    yang_stmt          *y0,
	    enum operation_type op,
	    char               *username,
	    cxobj              *xnacm,
	    int                 permit,
	    cbuf               *cbret)
{
    int        retval = -1;
    char      *opstr = NULL;
    char      *x1name;
    char      *x1cname; /* child name */
    cxobj     *x0c;     /* base child */
    cxobj     *x0b;     /* base body */
    cxobj     *x1c;     /* mod child */
    char      *x0bstr;  /* mod body string */
    char      *x1bstr;  /* mod body string */
    yang_stmt *yc;      /* yang child */
    cxobj    **x0vec = NULL;
    int        i;
    int        ret;
    char      *instr = NULL;
    char      *keystr = NULL;
    char      *valstr = NULL;
    enum insert_type insert = INS_LAST;
    int        changed = 0; /* Only if x0p's children have changed-> sort necessary */
    cvec      *nscx1 = NULL;
    char      *createstr = NULL;	
    
    if (x1 == NULL){
	clicon_err(OE_XML, EINVAL, "x1 is missing");
	goto done;
    }
    /* Check for operations embedded in tree according to netconf */
    if ((ret = attr_ns_value(x1, "operation", NETCONF_BASE_NAMESPACE,
			     cbret, &opstr)) < 0)
	goto done;
    if (ret == 0)
	goto fail;
    if (opstr != NULL)
	if (xml_operation(opstr, &op) < 0)
	    goto done;
    if ((ret = attr_ns_value(x1, "objectcreate", NULL, cbret, &createstr)) < 0)
	goto done;
    if (ret == 0)
	goto fail;
    if (createstr != NULL &&
	(op == OP_REPLACE || op == OP_MERGE || op == OP_CREATE)){
	if (x0 == NULL || xml_nopresence_default(x0)){ /* does not exist or is default */
	    if (strcmp(createstr, "false")==0){
		/* RFC 8040 4.6 PATCH:
		 * If the target resource instance does not exist, the server MUST NOT create it. 
		 */
		if (netconf_data_missing(cbret, NULL,
	  "RFC 8040 4.6. PATCH: If the target resource instance does not exist, the server MUST NOT create it") < 0)
		    goto done;
		goto fail;
	    }
	    clicon_data_set(h, "objectexisted", "false");
	}
	else{  /* exists */
	    clicon_data_set(h, "objectexisted", "true");
	}
    }
    x1name = xml_name(x1);
    if (yang_keyword_get(y0) == Y_LEAF_LIST ||
	yang_keyword_get(y0) == Y_LEAF){
	/* This is a check that a leaf does not have sub-elements 
	 * such as: <leaf>a <leaf>b</leaf> </leaf> 
	 */	    
	if (xml_child_nr_type(x1, CX_ELMNT)){
	    if (netconf_unknown_element(cbret, "application", x1name, "Leaf contains sub-element") < 0)
		goto done;
	    goto fail;
	}
	/* If leaf-list and ordered-by user, then get yang:insert attribute
	 * See RFC 7950 Sec 7.7.9
	 */
	if (yang_keyword_get(y0) == Y_LEAF_LIST &&
	    yang_find(y0, Y_ORDERED_BY, "user") != NULL){
	    if ((ret = attr_ns_value(x1,
				     "insert", YANG_XML_NAMESPACE,
				     cbret, &instr)) < 0)
		goto done;
	    if (ret == 0)
		goto fail;
	    if (instr != NULL &&
		xml_attr_insert2val(instr, &insert) < 0)
		goto done;
	    if ((ret = attr_ns_value(x1,
				     "value", YANG_XML_NAMESPACE,
				     cbret, &valstr)) < 0)
		goto done;
	    /* if insert/before, value attribute must be there */
	    if ((insert == INS_AFTER || insert == INS_BEFORE) &&
		valstr == NULL){
		if (netconf_missing_attribute(cbret, "application", "<bad-attribute>value</bad-attribute>", "Missing value attribute when insert is before or after") < 0)
		    goto done;
		goto fail;
	    }
	}
	x1bstr = xml_body(x1);
	switch(op){ 
	case OP_CREATE:
	    if (x0){
		if (netconf_data_exists(cbret, "Data already exists; cannot create new resource") < 0)
		    goto done;
		goto fail;
	    }
	case OP_REPLACE: /* fall thru */
	case OP_MERGE:
	    if (!(op == OP_MERGE && instr==NULL)){
		/* Remove existing, also applies to merge in the special case
		 * of ordered-by user and (changed) insert attribute.
		 */
		if (!permit && xnacm){
		    if ((ret = nacm_datanode_write(h, x1, x1t, x0?NACM_UPDATE:NACM_CREATE, username, xnacm, cbret)) < 0) 
			goto done;
		    if (ret == 0)
			goto fail;
		    permit = 1;
		}
		/* XXX: Note, if there is an error in adding the object later, the
		 * original object is not reverted.
		 */
		if (x0){
		    xml_purge(x0);
		    x0 = NULL;
		}
	    } /* OP_MERGE & insert */
	case OP_NONE: /* fall thru */
	    if (x0==NULL){
		if ((op != OP_NONE) && !permit && xnacm){
		    if ((ret = nacm_datanode_write(h, x1, x1t, NACM_CREATE, username, xnacm, cbret)) < 0) 
			goto done;
		    if (ret == 0)
			goto fail;
		    permit = 1;
		}
		/* Add new xml node but without parent - insert when node fully
		   copied (see changed conditional below) */
		if ((x0 = xml_new(x1name, NULL, CX_ELMNT)) == NULL)
		    goto done;
		xml_spec_set(x0, y0);

		/* Get namespace from x1
		 * Check if namespace exists in x0 parent
		 * if not add new binding and replace in x0.
		 * See also xmlns copying of attributes in the body section below
		 */
		if (assign_namespace_element(x1, x0, x0p) < 0)
		    goto done;
		changed++;
		if (op==OP_NONE)
		    xml_flag_set(x0, XML_FLAG_NONE); /* Mark for potential deletion */
		if (x1bstr){ /* empty type does not have body */
		    if ((x0b = xml_new("body", x0, CX_BODY)) == NULL)
			goto done; 
		}
	    }
	    if (x1bstr){
		/* Some bodies (eg identityref) requires proper namespace setup, so a type lookup is
		 * necessary.
		 */
		yang_stmt *yrestype = NULL;
		char      *restype;

		if (yang_type_get(y0, NULL, &yrestype, NULL, NULL, NULL, NULL, NULL) < 0)
		    goto done;
		if (yrestype == NULL){
		    clicon_err(OE_CFG, EFAULT, "No restype (internal error)");
		    goto done;
		}
		restype = yang_argument_get(yrestype);
		if (strcmp(restype, "identityref") == 0){
		    x1bstr = clixon_trim2(x1bstr, " \t\n"); 
		    if (check_body_namespace(x1, x0, x0p, x1bstr, y0) < 0)
			goto done;
		}
		else{
		    /* Some bodies strip pretty-printed here, unsure where to do this,.. */
		    if (strcmp(restype, "enumeration") == 0 ||
			strcmp(restype, "bits") == 0)
			x1bstr = clixon_trim2(x1bstr, " \t\n"); 

		    /* If origin body has namespace definitions, copy them. The reason is that
		     * some bodies rely on namespace prefixes, such as NACM path, but there is 
		     * no way we can now this here.
		     * However, this may lead to namespace collisions if these prefixes are not
		     * canonical, and may collide with the assign_namespace_element() above (but that 
		     * is for element sysmbols)
		     * Oh well.
		     */
		    if (assign_namespace_body(x1, x1bstr, x0) < 0)
			goto done;
		}
		if ((x0b = xml_body_get(x0)) != NULL){
		    x0bstr = xml_value(x0b);
		    if (x0bstr==NULL || strcmp(x0bstr, x1bstr)){
			if ((op != OP_NONE) && !permit && xnacm){
			    if ((ret = nacm_datanode_write(h, x1, x1t,
							   x0bstr==NULL?NACM_CREATE:NACM_UPDATE,
							   username, xnacm, cbret)) < 0)
				goto done;
			    if (ret == 0)
				goto fail;
			}
			if (xml_value_set(x0b, x1bstr) < 0)
			    goto done;
			/* If a default value ies replaced, then reset default flag */
			if (xml_flag(x0, XML_FLAG_DEFAULT))
			    xml_flag_reset(x0, XML_FLAG_DEFAULT);
		    }
		}
	    }
	    if (changed){ 
		if (xml_insert(x0p, x0, insert, valstr, NULL) < 0) 
		    goto done;
	    }
	    break;
	case OP_DELETE:
	    if (x0==NULL){
		if (netconf_data_missing(cbret, NULL, "Data does not exist; cannot delete resource") < 0)
		    goto done;
		goto fail;
	    }
	case OP_REMOVE: /* fall thru */
	    if (x0){
		if ((op != OP_NONE) && !permit && xnacm){
		    if ((ret = nacm_datanode_write(h, x0, x0t, NACM_DELETE, username, xnacm, cbret)) < 0)
			goto done;
		    if (ret == 0)
			goto fail;
		}
		x0bstr = xml_body(x0); 
		/* Purge if x1 value is NULL(match-all) or both values are equal */
		if ((x1bstr == NULL) ||
		    ((x0bstr=xml_body(x0)) != NULL && strcmp(x0bstr, x1bstr)==0)){
		    if (xml_purge(x0) < 0)
			goto done;
		}
		else {
		    if (op == OP_DELETE){
			if (netconf_data_missing(cbret, NULL, "Data does not exist; cannot delete resource") < 0)
			    goto done;
			goto fail;
		    }
		}
	    }
	    break;
	default:
	    break;
	} /* switch op */
    } /* if LEAF|LEAF_LIST */
    else { /* eg Y_CONTAINER, Y_LIST, Y_ANYXML  */
	/* If list and ordered-by user, then get insert attribute
             <user nc:operation="create"
                   yang:insert="after"
                   yang:key="[ex:first-name='fred']
                             [ex:surname='flintstone']">
	 * See RFC 7950 Sec 7.8.6
	 */
	if (yang_keyword_get(y0) == Y_LIST &&
	    yang_find(y0, Y_ORDERED_BY, "user") != NULL){
	    if ((ret = attr_ns_value(x1,
				     "insert", YANG_XML_NAMESPACE,
				     cbret, &instr)) < 0)
		goto done;
	    if (ret == 0)
		goto fail;
	    if (instr != NULL &&
		xml_attr_insert2val(instr, &insert) < 0)
		goto done;
	    if ((ret = attr_ns_value(x1,
				     "key", YANG_XML_NAMESPACE,
				     cbret, &keystr)) < 0)
		goto done;

	    /* if insert/before, key attribute must be there */
	    if ((insert == INS_AFTER || insert == INS_BEFORE) &&
		keystr == NULL){
		if (netconf_missing_attribute(cbret, "application", "<bad-attribute>key</bad-attribute>", "Missing key attribute when insert is before or after") < 0)
		    goto done;
		goto fail;
	    }
	    /* If keystr is set, need a full namespace context */
	    if (keystr && xml_nsctx_node(x1, &nscx1) < 0)
		goto done;
	}
	switch(op){ 
	case OP_CREATE:
	    if (x0){
		if (xml_nopresence_default(x0) == 0){
		    if (netconf_data_exists(cbret, "Data already exists; cannot create new resource") < 0)
			goto done;
		    goto fail;
		}
	    }
	case OP_REPLACE: /* fall thru */
	case OP_MERGE:  
	    if (!(op == OP_MERGE && instr==NULL)){
		/* Remove existing, also applies to merge in the special case
		 * of ordered-by user and (changed) insert attribute.
		 */
		if (!permit && xnacm){
		    if ((ret = nacm_datanode_write(h, x1, x1t, x0?NACM_UPDATE:NACM_CREATE, username, xnacm, cbret)) < 0) 
			goto done;
		    if (ret == 0)
			goto fail;
		    permit = 1;
		}
		/* XXX: Note, if there is an error in adding the object later, the
		 * original object is not reverted.
		 */
		if (x0){
		    xml_purge(x0);
		    x0 = NULL;
		}
	    } /* OP_MERGE & insert */
	case OP_NONE: /* fall thru */
	    /* Special case: anyxml, just replace tree, 
	       See rfc6020 7.10.3
	       An anyxml node is treated as an opaque chunk of data.  This data
	       can be modified in its entirety only.
	       Any "operation" attributes present on subelements of an anyxml 
	       node are ignored by the NETCONF server.*/
	    if (yang_keyword_get(y0) == Y_ANYXML ||
		yang_keyword_get(y0) == Y_ANYDATA){
		if (op == OP_NONE)
		    break;
		if (op==OP_MERGE && !permit && xnacm){
		    if ((ret = nacm_datanode_write(h, x1, x1t, x0?NACM_UPDATE:NACM_CREATE, username, xnacm, cbret)) < 0) 
			goto done;
		    if (ret == 0)
			goto fail;
		    permit = 1;
		}
		if (x0){
		    xml_purge(x0);
		}
		if ((x0 = xml_new(x1name, x0p, CX_ELMNT)) == NULL)
		    goto done;
		if (xml_copy(x1, x0) < 0)
		    goto done;
		break;
	    } /* anyxml, anydata */
	    if (x0==NULL){
		if (op==OP_MERGE && !permit && xnacm){
		    if ((ret = nacm_datanode_write(h, x1, x1t, NACM_CREATE, username, xnacm, cbret)) < 0) 
			goto done;
		    if (ret == 0)
			goto fail;
		    permit = 1;
		}
		/* Add new xml node but without parent - insert when node fully
		 * copied (see changed conditional below) 
		 * Note x0 may dangle cases if exit before changed conditional
		 */
		if ((x0 = xml_new(x1name, NULL, CX_ELMNT)) == NULL)
		    goto done;
		xml_spec_set(x0, y0);

		changed++;
		/* Get namespace from x1
		 * Check if namespace exists in x0 parent
		 * if not add new binding and replace in x0.
		 */
		if (assign_namespace_element(x1, x0, x0p) < 0)
		    goto done;
		if (op==OP_NONE)
		    xml_flag_set(x0, XML_FLAG_NONE); /* Mark for potential deletion */
	    }
	    /* First pass: Loop through children of the x1 modification tree 
	     * collect matching nodes from x0 in x0vec (no changes to x0 children)
	     */
	    if ((x0vec = calloc(xml_child_nr(x1), sizeof(x1))) == NULL){
		clicon_err(OE_UNIX, errno, "calloc");
		goto done;
	    }
	    x1c = NULL; 
	    i = 0;
	    while ((x1c = xml_child_each(x1, x1c, CX_ELMNT)) != NULL) {
		x1cname = xml_name(x1c);
		/* Get yang spec of the child by child matching */
		yc = yang_find_datanode(y0, x1cname);
		if (yc == NULL){
		    if (clicon_option_bool(h, "CLICON_YANG_UNKNOWN_ANYDATA") == 1){
			/* Add dummy Y_ANYDATA yang stmt, see ysp_add */
			if ((yc = yang_anydata_add(y0, x1cname)) < 0)
			    goto done;
			xml_spec_set(x1c, yc);
			clicon_log(LOG_WARNING,
				   "%s: %d: No YANG spec for %s, anydata used",
				   __FUNCTION__, __LINE__, x1cname);
		    }
		    else{
			if (netconf_unknown_element(cbret, "application", x1cname, "Unassigned yang spec") < 0)
			    goto done;
			goto fail;
		    }
		}
		/* There is a cornercase (eg augment) of multi-namespace trees where
		 * the yang child has a different namespace.
		 * As an alternative, return in populate where this is detected first time.
		 */
		if (yc != xml_spec(x1c)){
		    clicon_err(OE_YANG, errno, "XML node %s not in namespace %s",
			       x1cname, yang_find_mynamespace(y0));
		    goto done;
		}
		/* See if there is a corresponding node in the base tree */
		x0c = NULL;
		if (match_base_child(x0, x1c, yc, &x0c) < 0)
		    goto done;
		if (x0c && (yc != xml_spec(x0c))){
		    /* There is a match but is should be replaced (choice)*/
		    if (xml_purge(x0c) < 0)
			goto done;
		    x0c = NULL;
		}
		x0vec[i++] = x0c; /* != NULL if x0c is matching x1c */
	    }
	    /* Second pass: Loop through children of the x1 modification tree again
	     * Now potentially modify x0:s children 
	     * Here x0vec contains one-to-one matching nodes of x1:s children.
	     */
	    x1c = NULL;
	    i = 0;
	    while ((x1c = xml_child_each(x1, x1c, CX_ELMNT)) != NULL) {
		x1cname = xml_name(x1c);
		x0c = x0vec[i++];
		yc = yang_find_datanode(y0, x1cname);
		if ((ret = text_modify(h, x0c, x0, x0t, x1c, x1t,
				       yc, op,
				       username, xnacm, permit, cbret)) < 0)
		    goto done;
		/* If xml return - ie netconf error xml tree, then stop and return OK */
		if (ret == 0)
		    goto fail;
	    }
	    if (changed){
		if (xml_insert(x0p, x0, insert, keystr, nscx1) < 0)
		    goto done;
	    }
	    break;
	case OP_DELETE:
	    if (x0==NULL){
		if (netconf_data_missing(cbret, NULL, "Data does not exist; cannot delete resource") < 0)
		    goto done;
		goto fail;
	    }
	case OP_REMOVE: /* fall thru */
	    if (x0){
		if (!permit && xnacm){
		    if ((ret = nacm_datanode_write(h, x0, x0t, NACM_DELETE, username, xnacm, cbret)) < 0) 
			goto done;
		    if (ret == 0)
			goto fail;
		}
		if (xml_purge(x0) < 0)
		    goto done;
	    }
	    break;
	default:
	    break;
	} /* CONTAINER switch op */
    } /* else Y_CONTAINER  */
    retval = 1;
 done:
    if (nscx1)
	xml_nsctx_free(nscx1);
    /* Remove dangling added objects */
    if (changed && x0 && xml_parent(x0)==NULL)
	xml_purge(x0);
    if (x0vec)
	free(x0vec);
    return retval;
 fail: /* cbret set */
    retval = 0;
    goto done;
} /* text_modify */

/*! Modify a top-level base tree x0 with modification tree x1
 * @param[in]  h        Clicon handle
 * @param[in]  x0       Base xml tree (can be NULL in add scenarios)
 * @param[in]  x0t
 * @param[in]  x1       XML tree which modifies base
 * @param[in]  x1t      Request root node (nacm needs this)
 * @param[in]  yspec    Top-level yang spec (if y is NULL)
 * @param[in]  op       OP_MERGE, OP_REPLACE, OP_REMOVE, etc 
 * @param[in]  username User name of requestor for nacm
 * @param[in]  xnacm    NACM XML tree (only if !permit)
 * @param[in]  permit   If set, no NACM tests using xnacm required
 * @param[out] cbret    Initialized cligen buffer. Contains return XML if retval is 0.
 * @retval    -1        Error
 * @retval     0        Failed (cbret set)
 * @retval     1        OK
 * @see text_modify
 */
static int
text_modify_top(clicon_handle       h,
		cxobj              *x0,
		cxobj              *x0t,
		cxobj              *x1,
		cxobj              *x1t,
		yang_stmt          *yspec,
		enum operation_type op,
		char               *username,
		cxobj              *xnacm,
		int                 permit,
		cbuf               *cbret)
{
    int        retval = -1;
    char      *x1cname; /* child name */
    cxobj     *x0c; /* base child */
    cxobj     *x1c; /* mod child */
    yang_stmt *yc;  /* yang child */
    yang_stmt *ymod;/* yang module */
    char      *opstr;
    int        ret;
    char      *createstr = NULL;
    
    /* Check for operations embedded in tree according to netconf */
    if ((ret = attr_ns_value(x1,
			     "operation", NETCONF_BASE_NAMESPACE,
			     cbret, &opstr)) < 0)
	goto done;
    if (ret == 0)
	goto fail;
    if (opstr != NULL)
	if (xml_operation(opstr, &op) < 0)
	    goto done;
    if ((ret = attr_ns_value(x1, "objectcreate", NULL, cbret, &createstr)) < 0)
	goto done;
    if (ret == 0)
	goto fail;
    /* Special case if incoming x1 is empty, top-level only <config/> */
    if (xml_child_nr_type(x1, CX_ELMNT) == 0){ 
	if (xml_child_nr_type(x0, CX_ELMNT)){ /* base tree not empty */
	    switch(op){ 
	    case OP_DELETE:
	    case OP_REMOVE:
	    case OP_REPLACE:
		if (!permit && xnacm){
		    if ((ret = nacm_datanode_write(h, x0, x0t, NACM_DELETE, username, xnacm, cbret)) < 0)
			goto done;
		    if (ret == 0)
			goto fail;
		    permit = 1;
		}
		while ((x0c = xml_child_i(x0, 0)) != 0)
		    if (xml_purge(x0c) < 0)
			goto done;
		break;
	    default:
		break;
	    }
	}
	else /* base tree empty */
	    switch(op){ 
#if 0 /* According to RFC6020 7.5.8 you cant delete a non-existing object.
	 On the other hand, the top-level cannot be removed anyway.
	 Additionally, I think this is irritating so I disable it.
	 I.e., curl -u andy:bar -sS -X DELETE http://localhost/restconf/data
      */
	    case OP_DELETE:
		if (netconf_data_missing(cbret, NULL, "Data does not exist; cannot delete resource") < 0)
		    goto done;
		goto fail;
		break;
#endif
	    default:
		break;
	    }
    }
    /* Special case top-level replace */
    else if (op == OP_REPLACE || op == OP_DELETE){
	if (createstr != NULL){
	    if (xml_child_nr_type(x0, CX_ELMNT)) /* base tree not empty */
		clicon_data_set(h, "objectexisted", "true");
	    else
		clicon_data_set(h, "objectexisted", "false");
	}
	if (!permit && xnacm){
	    if ((ret = nacm_datanode_write(h, x1, x1t, NACM_UPDATE, username, xnacm, cbret)) < 0) 
		goto done;
	    if (ret == 0)
		goto fail;
	    permit = 1;
	}
	while ((x0c = xml_child_i(x0, 0)) != 0)
	    if (xml_purge(x0c) < 0)
		goto done;
    }
    /* Loop through children of the modification tree */
    x1c = NULL;
    while ((x1c = xml_child_each(x1, x1c, CX_ELMNT)) != NULL) {
	x1cname = xml_name(x1c);
	/* Get yang spec of the child */
	yc = NULL;
	if (ys_module_by_xml(yspec, x1c, &ymod) <0)
	    goto done;
	if (ymod != NULL)
	    yc = yang_find_datanode(ymod, x1cname);
	if (yc == NULL){
	    if (ymod != NULL &&
		clicon_option_bool(h, "CLICON_YANG_UNKNOWN_ANYDATA") == 1){
		/* Add dummy Y_ANYDATA yang stmt, see ysp_add */
		if ((yc = yang_anydata_add(ymod, x1cname)) < 0)
		    goto done;
		xml_spec_set(x1c, yc);
		clicon_log(LOG_WARNING,
			   "%s: %d: No YANG spec for %s, anydata used",
			   __FUNCTION__, __LINE__, x1cname);
	    }
	    else{
		if (netconf_unknown_element(cbret, "application", x1cname, "Unassigned yang spec") < 0)
		    goto done;
		goto fail;
	    }
	}
	/* See if there is a corresponding node in the base tree */
	if (match_base_child(x0, x1c, yc, &x0c) < 0)
	    goto done;
	if (x0c && (yc != xml_spec(x0c))){
	    /* There is a match but is should be replaced (choice)*/
	    if (xml_purge(x0c) < 0)
		goto done;
	    x0c = NULL;
	}
	if ((ret = text_modify(h, x0c, x0, x0t, x1c, x1t,
			       yc, op,
			       username, xnacm, permit, cbret)) < 0)
	    goto done;
	/* If xml return - ie netconf error xml tree, then stop and return OK */
	if (ret == 0)
	    goto fail;
    }
    // ok:
    retval = 1;
 done:
    return retval;
 fail: /* cbret set */
    retval = 0;
    goto done;
} /* text_modify_top */

/*! Modify database given an xml tree and an operation
 *
 * @param[in]  h      CLICON handle
 * @param[in]  db     running or candidate
 * @param[in]  op     Top-level operation, can be superceded by other op in tree
 * @param[in]  xt     xml-tree. Top-level symbol is dummy
 * @param[in]  username User name for nacm
 * @param[out] cbret  Initialized cligen buffer. On exit contains XML if retval == 0
 * @retval     1      OK
 * @retval     0      Failed, cbret contains error xml message
 * @retval     -1     Error
 * The xml may contain the "operation" attribute which defines the operation.
 * @code
 *   cxobj     *xt;
 *   cxobj     *xret = NULL;
 *   if (clixon_xml_parse_string("<a>17</a>", YB_NONE, NULL, &xt, NULL) < 0)
 *     err;
 *   if ((ret = xmldb_put(h, "running", OP_MERGE, xt, username, cbret)) < 0)
 *     err;
 *   if (ret==0)
 *     cbret contains netconf error message
 * @endcode
 * @note if xret is non-null, it may contain error message
 */
int
xmldb_put(clicon_handle       h,
	  const char         *db, 
	  enum operation_type op,
	  cxobj              *x1,
	  char               *username,
	  cbuf               *cbret)
{
    int                 retval = -1;
    char               *dbfile = NULL;
    FILE               *f = NULL;
    cbuf               *cb = NULL;
    yang_stmt          *yspec;
    cxobj              *x0 = NULL;
    db_elmnt           *de = NULL;
    int                 ret;
    cxobj              *xnacm = NULL;
    cxobj              *xmodst = NULL;
    cxobj              *x;
    int                 permit = 0; /* nacm permit all */
    char               *format;
    cvec               *nsc = NULL; /* nacm namespace context */
    int                 firsttime = 0;
    int                 pretty;

    if (cbret == NULL){
	clicon_err(OE_XML, EINVAL, "cbret is NULL");
	goto done;
    }
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_YANG, ENOENT, "No yang spec");
	goto done;
    }
    if (x1 && strcmp(xml_name(x1), "config") != 0){
	clicon_err(OE_XML, 0, "Top-level symbol of modification tree is %s, expected \"config\"",
		   xml_name(x1));
	goto done;
    }
    if ((de = clicon_db_elmnt_get(h, db)) != NULL){
	if (clicon_datastore_cache(h) != DATASTORE_NOCACHE)
	    x0 = de->de_xml; 
    }
    /* If there is no xml x0 tree (in cache), then read it from file */
    if (x0 == NULL){
	firsttime++; /* to avoid leakage on error, see fail from text_modify */
	if ((ret = xmldb_readfile(h, db, YB_MODULE, yspec, &x0, de, NULL)) < 0)
	    goto done;
	if (ret == 0)
	    goto fail;
    }
    if (strcmp(xml_name(x0), "config")!=0){
	clicon_err(OE_XML, 0, "Top-level symbol is %s, expected \"config\"",
		   xml_name(x0));
	goto done;
    }
    /* Here x0 looks like: <config>...</config> */

#if 0 /* debug */
    if (xml_apply0(x1, -1, xml_sort_verify, NULL) < 0)
	clicon_log(LOG_NOTICE, "%s: verify failed #1", __FUNCTION__);
#endif

    xnacm = clicon_nacm_cache(h);
    permit = (xnacm==NULL);

    /* Here assume if xnacm is set and !permit do NACM */
    clicon_data_del(h, "objectexisted");
    /* 
     * Modify base tree x with modification x1. This is where the
     * new tree is made.
     */
    if ((ret = text_modify_top(h, x0, x0, x1, x1, yspec, op, username, xnacm, permit, cbret)) < 0)
	goto done;
    /* If xml return - ie netconf error xml tree, then stop and return OK */
    if (ret == 0){
	/* If first time and quit here, x0 is not written back into cache and leaks */
	if (firsttime && x0){
	    xml_free(x0);
	    x0 = NULL;
	}
	goto fail;
    }

    /* Remove NONE nodes if all subs recursively are also NONE */
    if (xml_tree_prune_flagged_sub(x0, XML_FLAG_NONE, 0, NULL) <0)
	goto done;
    if (xml_apply(x0, CX_ELMNT, (xml_applyfn_t*)xml_flag_reset, 
		  (void*)(XML_FLAG_NONE|XML_FLAG_MARK)) < 0)
	goto done;
    /* Mark non-presence containers as XML_FLAG_DEFAULT */
    if (xml_apply(x0, CX_ELMNT, xml_nopresence_default_mark, (void*)XML_FLAG_DEFAULT) < 0)
	goto done;
    /* Clear XML tree of defaults */
    if (xml_tree_prune_flagged(x0, XML_FLAG_DEFAULT, 1) < 0)
	goto done;
#if 0 /* debug */
    if (xml_apply0(x0, -1, xml_sort_verify, NULL) < 0)
	clicon_log(LOG_NOTICE, "%s: verify failed #3", __FUNCTION__);
#endif

    /* Write back to datastore cache if first time */
    if (clicon_datastore_cache(h) != DATASTORE_NOCACHE){
	db_elmnt de0 = {0,};
	if (de != NULL)
	    de0 = *de;
	if (de0.de_xml == NULL)
	    de0.de_xml = x0;
	de0.de_empty = (xml_child_nr(de0.de_xml) == 0);
	clicon_db_elmnt_set(h, db, &de0);
    }
    if (xmldb_db2file(h, db, &dbfile) < 0)
	goto done;
    if (dbfile==NULL){
	clicon_err(OE_XML, 0, "dbfile NULL");
	goto done;
    }
    /* Add module revision info before writing to file)
     * Only if CLICON_XMLDB_MODSTATE is set
     */
    if ((x = clicon_modst_cache_get(h, 1)) != NULL){
	if ((xmodst = xml_dup(x)) == NULL)
	    goto done;
	if (xml_addsub(x0, xmodst) < 0)
	    goto done;
    }
    if ((format = clicon_option_str(h, "CLICON_XMLDB_FORMAT")) == NULL){
	clicon_err(OE_CFG, ENOENT, "No CLICON_XMLDB_FORMAT");
	goto done;
    }
    if ((f = fopen(dbfile, "w")) == NULL){
	clicon_err(OE_CFG, errno, "Creating file %s", dbfile);
	goto done;
    } 
    pretty = clicon_option_bool(h, "CLICON_XMLDB_PRETTY");
    if (strcmp(format,"json")==0){
	if (xml2json(f, x0, pretty) < 0)
	    goto done;
    }
    else if (clicon_xml2file(f, x0, 0, pretty) < 0)
	goto done;
    /* Remove modules state after writing to file
     */
    if (xmodst && xml_purge(xmodst) < 0)
	goto done;
    retval = 1;
 done:
    if (f != NULL)
	fclose(f);
    if (nsc)
	xml_nsctx_free(nsc);
    if (dbfile)
	free(dbfile);
    if (cb)
	cbuf_free(cb);
    if (x0 && clicon_datastore_cache(h) == DATASTORE_NOCACHE)
	xml_free(x0);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/* Dump a datastore to file including modstate
 */
int
xmldb_dump(clicon_handle   h,
	   FILE           *f,
	   cxobj          *xt)
{
    int    retval = -1;
    cxobj *x;
    cxobj *xmodst = NULL;
    char  *format;
    int    pretty;
    
    /* clear XML tree of defaults */
    if (xml_tree_prune_flagged(xt, XML_FLAG_DEFAULT, 1) < 0)
	goto done;
    /* Add modstate first */
    if ((x = clicon_modst_cache_get(h, 1)) != NULL){
	if ((xmodst = xml_dup(x)) == NULL)
	    goto done;
	if (xml_child_insert_pos(xt, xmodst, 0) < 0)
	    goto done;
    }
    if ((format = clicon_option_str(h, "CLICON_XMLDB_FORMAT")) == NULL){
	clicon_err(OE_CFG, ENOENT, "No CLICON_XMLDB_FORMAT");
	goto done;
    }
    pretty = clicon_option_bool(h, "CLICON_XMLDB_PRETTY");
    if (strcmp(format,"json")==0){
	if (xml2json(f, xt, pretty) < 0)
	    goto done;
    }
    else if (clicon_xml2file(f, xt, 0, pretty) < 0)
	goto done;
    retval = 0;
 done:
    return retval;
}

