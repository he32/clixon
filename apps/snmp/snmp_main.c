/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2022 Olof Hagsand and Kristofer Hallin

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
#include <unistd.h>
#include <pwd.h>
#include <syslog.h>
#include <errno.h>
#include <sys/types.h>
#include <signal.h>

/* net-snmp */
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

/* Command line options to be passed to getopt(3) */
#define SNMP_OPTS "hD:f:l:o:"

#if 1 // XXX hardcoded from https://github.com/net-snmp/net-snmp/blob/master/agent/mibgroup/testhandler.c

Netsnmp_Node_Handler my_test_handler;
Netsnmp_Node_Handler my_test_table_handler;
Netsnmp_Node_Handler my_data_table_handler;
Netsnmp_Node_Handler my_test_instance_handler;

static oid      my_test_oid[4] = { 1, 2, 3, 4 };
static oid      my_table_oid[4] = { 1, 2, 3, 5 };
static oid      my_instance_oid[5] = { 1, 2, 3, 6, 1 };
static oid      my_data_table_oid[4] = { 1, 2, 3, 7 };
static oid      my_data_ulong_instance[4] = { 1, 2, 3, 9 };

u_long          my_ulong = 42;

static netsnmp_table_data_set *table_set;

/*
 * https://net-snmp.sourceforge.io/dev/agent/data_set_8c-example.html#_a0
 */
void
init_testtable(void)
{
    netsnmp_table_row *row;

    /*
     * the OID we want to register our integer at.  This should be the
     * * OID node for the entire table.  In our case this is the
     * * netSnmpIETFWGTable oid definition
     */
    oid             my_registration_oid[] =
        { 1, 3, 6, 1, 4, 1, 8072, 2, 2, 1 };

    /*
     * a debugging statement.  Run the agent with -Dexample_data_set to see
     * * the output of this debugging statement.
     */
    DEBUGMSGTL(("example_data_set",
                "Initalizing example dataset table\n"));

    /*
     * It's going to be the "working group chairs" table, since I'm
     * * sitting at an IETF convention while I'm writing this.
     * *
     * *  column 1 = index = string = WG name
     * *  column 2 = string = chair #1
     * *  column 3 = string = chair #2  (most WGs have 2 chairs now)
     */

    table_set = netsnmp_create_table_data_set("netSnmpIETFWGTable");

    /*
     * allow the creation of new rows via SNMP SETs
     */
    table_set->allow_creation = 1;

    /*
     * set up what a row "should" look like, starting with the index
     */
    netsnmp_table_dataset_add_index(table_set, ASN_OCTET_STR);

    /*
     * define what the columns should look like.  both are octet strings here
     */
    netsnmp_table_set_multi_add_default_row(table_set,
                                            /*
                                             * column 2 = OCTET STRING,
                                             * writable = 1,
                                             * default value = NULL,
                                             * default value len = 0
                                             */
                                            2, ASN_OCTET_STR, 1, NULL, 0,
                                            /*
                                             * similar
                                             */
                                            3, ASN_OCTET_STR, 1, NULL, 0,
                                            0 /* done */ );

    /*
     * register the table
     */
    /*
     * if we wanted to handle specific data in a specific way, or note
     * * when requests came in we could change the NULL below to a valid
     * * handler method in which we could over ride the default
     * * behaviour of the table_dataset helper
     */
    netsnmp_register_table_data_set(netsnmp_create_handler_registration
                                    ("netSnmpIETFWGTable", NULL,
                                     my_registration_oid,
                                     OID_LENGTH(my_registration_oid),
                                     HANDLER_CAN_RWRITE), table_set, NULL);


    /*
     * create the a row for the table, and add the data
     */
    row = netsnmp_create_table_data_row();
    /*
     * set the index to the IETF WG name "snmpv3"
     */
    netsnmp_table_row_add_index(row, ASN_OCTET_STR, "snmpv3",
                                strlen("snmpv3"));


    /*
     * set column 2 to be the WG chair name "Russ Mundy"
     */
    netsnmp_set_row_column(row, 2, ASN_OCTET_STR,
                           "Russ Mundy", strlen("Russ Mundy"));
    netsnmp_mark_row_column_writable(row, 2, 1);        /* make writable via SETs */

    /*
     * set column 3 to be the WG chair name "David Harrington"
     */
    netsnmp_set_row_column(row, 3, ASN_OCTET_STR, "David Harrington",
                           strlen("David Harrington"));
    netsnmp_mark_row_column_writable(row, 3, 1);        /* make writable via SETs */

    /*
     * add the row to the table
     */
    netsnmp_table_dataset_add_row(table_set, row);

    /*
     * add the data, for the second row
     */
    row = netsnmp_create_table_data_row();
    netsnmp_table_row_add_index(row, ASN_OCTET_STR, "snmpconf",
                                strlen("snmpconf"));
    netsnmp_set_row_column(row, 2, ASN_OCTET_STR, "David Partain",
                           strlen("David Partain"));
    netsnmp_mark_row_column_writable(row, 2, 1);        /* make writable */
    netsnmp_set_row_column(row, 3, ASN_OCTET_STR, "Jon Saperia",
                           strlen("Jon Saperia"));
    netsnmp_mark_row_column_writable(row, 3, 1);        /* make writable */
    netsnmp_table_dataset_add_row(table_set, row);

    /*
     * Finally, this actually allows the "add_row" token it the
     * * snmpd.conf file to add rows to this table.
     * * Example snmpd.conf line:
     * *   add_row netSnmpIETFWGTable eos "Glenn Waters" "Dale Francisco"
     */
    netsnmp_register_auto_data_table(table_set, NULL);

    DEBUGMSGTL(("example_data_set", "Done initializing.\n"));
}

void
init_testhandler(void)
{
    /*
     * we're registering at .1.2.3.4 
     */
    netsnmp_handler_registration *my_test;
    netsnmp_table_registration_info *table_info;
    u_long          ind1;
    netsnmp_table_data *table;
    netsnmp_table_row *row;

    clicon_debug(1, "%s", __FUNCTION__);

    /*
     * basic handler test
     */
    netsnmp_register_handler(netsnmp_create_handler_registration
                             ("myTest", my_test_handler, my_test_oid, 4,
                              HANDLER_CAN_RONLY));

    /*
     * instance handler test
     */

    netsnmp_register_instance(netsnmp_create_handler_registration
                              ("myInstance", my_test_instance_handler,
                               my_instance_oid, 5, HANDLER_CAN_RWRITE));

    netsnmp_register_ulong_instance("myulong",
                                    my_data_ulong_instance, 4,
                                    &my_ulong, NULL);

    /*
     * table helper test
     */

    my_test = netsnmp_create_handler_registration("myTable",
                                                  my_test_table_handler,
                                                  my_table_oid, 4,
                                                  HANDLER_CAN_RONLY);
    if (!my_test)
        return;

    table_info = SNMP_MALLOC_TYPEDEF(netsnmp_table_registration_info);
    if (table_info == NULL)
        return;

    netsnmp_table_helper_add_indexes(table_info, ASN_INTEGER, ASN_INTEGER,
                                     0);
    table_info->min_column = 3;
    table_info->max_column = 3;
    netsnmp_register_table(my_test, table_info);

    /*
     * data table helper test
     */
    /*
     * we'll construct a simple table here with two indexes: an
     * integer and a string (why not).  It'll contain only one
     * column so the data pointer is merely the data in that
     * column. 
     */

    table = netsnmp_create_table_data("data_table_test");

    netsnmp_table_data_add_index(table, ASN_INTEGER);
    netsnmp_table_data_add_index(table, ASN_OCTET_STR);

    /*
     * 1 partridge in a pear tree 
     */
    row = netsnmp_create_table_data_row();
    ind1 = 1;
    netsnmp_table_row_add_index(row, ASN_INTEGER, &ind1, sizeof(ind1));
    netsnmp_table_row_add_index(row, ASN_OCTET_STR, "partridge",
                                strlen("partridge"));
    row->data = NETSNMP_REMOVE_CONST(void *, "pear tree");
    netsnmp_table_data_add_row(table, row);

    /*
     * 2 turtle doves 
     */
    row = netsnmp_create_table_data_row();
    ind1 = 2;
    netsnmp_table_row_add_index(row, ASN_INTEGER, &ind1, sizeof(ind1));
    netsnmp_table_row_add_index(row, ASN_OCTET_STR, "turtle",
                                strlen("turtle"));
    row->data = NETSNMP_REMOVE_CONST(void *, "doves");
    netsnmp_table_data_add_row(table, row);

    /*
     * we're going to register it as a normal table too, so we get the
     * automatically parsed column and index information 
     */
    table_info = SNMP_MALLOC_TYPEDEF(netsnmp_table_registration_info);
    if (table_info == NULL)
        return;

    netsnmp_table_helper_add_indexes(table_info, ASN_INTEGER,
                                     ASN_OCTET_STR, 0);
    table_info->min_column = 3;
    table_info->max_column = 3;

    netsnmp_register_read_only_table_data(
	  netsnmp_create_handler_registration("12days", my_data_table_handler, my_data_table_oid, 4,
					   HANDLER_CAN_RONLY),
	  table, table_info);

}

int
my_test_handler(netsnmp_mib_handler *handler,
                netsnmp_handler_registration *reginfo,
                netsnmp_agent_request_info *reqinfo,
                netsnmp_request_info *requests)
{

    oid             myoid1[] = { 1, 2, 3, 4, 5, 6 };
    static u_long   accesses = 0;

    clicon_debug(1, "%s", __FUNCTION__);
    /*
     * loop through requests 
     */
    while (requests) {
        netsnmp_variable_list *var = requests->requestvb;

        DEBUGMSGTL(("testhandler", "  oid:"));
        DEBUGMSGOID(("testhandler", var->name, var->name_length));
        DEBUGMSG(("testhandler", "\n"));

        switch (reqinfo->mode) {
        case MODE_GET:
            if (netsnmp_oid_equals(var->name, var->name_length, myoid1, 6)
                == 0) {
                snmp_set_var_typed_value(var, ASN_INTEGER,
                                         (u_char *) & accesses,
                                         sizeof(accesses));
                return SNMP_ERR_NOERROR;
            }
            break;

        case MODE_GETNEXT:
            if (snmp_oid_compare(var->name, var->name_length, myoid1, 6)
                < 0) {
                snmp_set_var_objid(var, myoid1, 6);
                snmp_set_var_typed_value(var, ASN_INTEGER,
                                         (u_char *) & accesses,
                                         sizeof(accesses));
                return SNMP_ERR_NOERROR;
            }
            break;

        default:
            netsnmp_set_request_error(reqinfo, requests, SNMP_ERR_GENERR);
            break;
        }

        requests = requests->next;
    }
    return SNMP_ERR_NOERROR;
}

/*
 * functionally this is a simply a multiplication table for 12x12
 */

#define MAX_COLONE 12
#define MAX_COLTWO 12
#define RESULT_COLUMN 3
int
my_test_table_handler(netsnmp_mib_handler *handler,
                      netsnmp_handler_registration *reginfo,
                      netsnmp_agent_request_info *reqinfo,
                      netsnmp_request_info *requests)
{

    netsnmp_table_registration_info
        *handler_reg_info =
        (netsnmp_table_registration_info *) handler->prev->myvoid;

    netsnmp_table_request_info *table_info;
    u_long          result;
    int             x, y;

    DEBUGMSGTL(("testhandler", "Got request:\n"));

    while (requests) {
        netsnmp_variable_list *var = requests->requestvb;

        if (requests->processed != 0)
            continue;

        DEBUGMSGTL(("testhandler_table", "Got request:\n"));
        DEBUGMSGTL(("testhandler_table", "  oid:"));
        DEBUGMSGOID(("testhandler_table", var->name, var->name_length));
        DEBUGMSG(("testhandler_table", "\n"));

        table_info = netsnmp_extract_table_info(requests);
        if (table_info == NULL) {
            requests = requests->next;
            continue;
        }

        switch (reqinfo->mode) {
        case MODE_GETNEXT:
            /*
             * beyond our search range? 
             */
            if (table_info->colnum > RESULT_COLUMN)
                break;

            /*
             * below our minimum column? 
             */
            if (table_info->colnum < RESULT_COLUMN ||
                /*
                 * or no index specified 
                 */
                table_info->indexes->val.integer == NULL) {
                table_info->colnum = RESULT_COLUMN;
                x = 0;
                y = 0;
            } else {
                x = *(table_info->indexes->val.integer);
                y = *(table_info->indexes->next_variable->val.integer);
            }

            if (table_info->number_indexes ==
                handler_reg_info->number_indexes) {
                y++;            /* GETNEXT is basically just y+1 for this table */
                if (y > MAX_COLTWO) {   /* (with wrapping) */
                    y = 0;
                    x++;
                }
            }
            if (x <= MAX_COLONE) {
                result = x * y;

                *(table_info->indexes->val.integer) = x;
                *(table_info->indexes->next_variable->val.integer) = y;
                netsnmp_table_build_result(reginfo, requests,
                                           table_info, ASN_INTEGER,
                                           (u_char *) & result,
                                           sizeof(result));
            }

            break;

        case MODE_GET:
            if (var->type == ASN_NULL) {        /* valid request if ASN_NULL */
                /*
                 * is it the right column? 
                 */
                if (table_info->colnum == RESULT_COLUMN &&
                    /*
                     * and within the max boundries? 
                     */
                    *(table_info->indexes->val.integer) <= MAX_COLONE &&
                    *(table_info->indexes->next_variable->val.integer)
                    <= MAX_COLTWO) {

                    /*
                     * then, the result is column1 * column2 
                     */
                    result = *(table_info->indexes->val.integer) *
                        *(table_info->indexes->next_variable->val.integer);
                    snmp_set_var_typed_value(var, ASN_INTEGER,
                                             (u_char *) & result,
                                             sizeof(result));
                }
            }
            break;

        }

        requests = requests->next;
    }

    return SNMP_ERR_NOERROR;
}

#define TESTHANDLER_SET_NAME "my_test"
int
my_test_instance_handler(netsnmp_mib_handler *handler,
                         netsnmp_handler_registration *reginfo,
                         netsnmp_agent_request_info *reqinfo,
                         netsnmp_request_info *requests)
{

    static u_long   accesses = 42;
    u_long         *accesses_cache = NULL;

    clicon_debug(1, "%s", __FUNCTION__);

    switch (reqinfo->mode) {
    case MODE_GET:
        snmp_set_var_typed_value(requests->requestvb, ASN_UNSIGNED,
                                 (u_char *) & accesses, sizeof(accesses));
        break;

#ifndef NETSNMP_NO_WRITE_SUPPORT
    case MODE_SET_RESERVE1:
        if (requests->requestvb->type != ASN_UNSIGNED)
            netsnmp_set_request_error(reqinfo, requests,
                                      SNMP_ERR_WRONGTYPE);
        break;

    case MODE_SET_RESERVE2:
        /*
         * store old info for undo later 
         */
        accesses_cache = netsnmp_memdup(&accesses, sizeof(accesses));
        if (accesses_cache == NULL) {
            netsnmp_set_request_error(reqinfo, requests,
                                      SNMP_ERR_RESOURCEUNAVAILABLE);
            return SNMP_ERR_NOERROR;
        }
        netsnmp_request_add_list_data(requests,
                                      netsnmp_create_data_list
                                      (TESTHANDLER_SET_NAME,
                                       accesses_cache, free));
        break;

    case MODE_SET_ACTION:
        /*
         * update current 
         */
        accesses = *(requests->requestvb->val.integer);
        DEBUGMSGTL(("testhandler", "updated accesses -> %lu\n", accesses));
        break;

    case MODE_SET_UNDO:
        accesses =
            *((u_long *) netsnmp_request_get_list_data(requests,
                                                       TESTHANDLER_SET_NAME));
        break;

    case MODE_SET_COMMIT:
    case MODE_SET_FREE:
        /*
         * nothing to do 
         */
        break;
#endif /* NETSNMP_NO_WRITE_SUPPORT */
    }

    return SNMP_ERR_NOERROR;
}

int
my_data_table_handler(netsnmp_mib_handler *handler,
                      netsnmp_handler_registration *reginfo,
                      netsnmp_agent_request_info *reqinfo,
                      netsnmp_request_info *requests)
{

    char           *column3;
    netsnmp_table_request_info *table_info;
    netsnmp_table_row *row;

    clicon_debug(1, "%s", __FUNCTION__);

    while (requests) {
        if (requests->processed) {
            requests = requests->next;
            continue;
        }

        /*
         * extract our stored data and table info 
         */
        row = netsnmp_extract_table_row(requests);
        table_info = netsnmp_extract_table_info(requests);
        if (!table_info || !row || !row->data)
            continue;
        column3 = (char *) row->data;

        /*
         * there's only one column, we don't need to check if it's right 
         */
        netsnmp_table_data_build_result(reginfo, reqinfo, requests, row,
                                        table_info->colnum,
                                        ASN_OCTET_STR, (u_char*)column3,
                                        strlen(column3));
        requests = requests->next;
    }
    return SNMP_ERR_NOERROR;
}

#endif

/*! Signal terminates process
 * Just set exit flag for proper exit in event loop
 */
static void
clixon_snmp_sig_term(int arg)
{
    clicon_log(LOG_NOTICE, "%s: %s: pid: %u Signal %d", 
	       __PROGRAM__, __FUNCTION__, getpid(), arg);
    /* This should ensure no more accepts or incoming packets are processed because next time eventloop
     * is entered, it will terminate.
     * However there may be a case of sockets closing rather abruptly for clients
     */
    clixon_exit_set(1); 
}

/*! Callback for single socket 
 * This is a workaround for netsnmps API usiing fdset:s, instead an fdset is created before calling
 * the snmp api
 * @param[in]  s   Read socket
 * @param[in]  arg Clixon handle
 */
static int
clixon_snmp_input_cb(int   s, 
		     void *arg)
{
    int    retval = -1;
    fd_set readfds;
    //    clicon_handle h = (clicon_handle)arg;

    clicon_debug(1, "%s", __FUNCTION__);
    FD_ZERO(&readfds);
    FD_SET(s, &readfds);
    snmp_read(&readfds);
    retval = 0;
    // done:
    return retval;
}

/*! Get which sockets are used from SNMP API, the register single sockets into clixon event system
 *
 * This is a workaround for netsnmps API usiing fdset:s, instead an fdset is created before calling
 * the snmp api
 * if you use select(), see snmp_select_info() in snmp_api(3) 
 * snmp_select_info(int *numfds, fd_set *fdset, struct timeval *timeout, int *block)
 * @see clixon_snmp_input_cb
 */
static int
clixon_snmp_fdset_register(clicon_handle h)
{
    int             retval = -1;
    int             numfds = 0;
    fd_set          readfds;
    struct timeval  timeout = { LONG_MAX, 0 };
    int             block = 0;
    int             nr;
    int             i;

    FD_ZERO(&readfds);
    if ((nr = snmp_sess_select_info(NULL, &numfds, &readfds, &timeout, &block)) < 0){
	clicon_err(OE_SNMP, errno, "snmp_select_error");
	goto done;
    }
    for (i=0; i<numfds; i++){
	if (FD_ISSET(i, &readfds)){
	    if (clixon_event_reg_fd(i, clixon_snmp_input_cb, h, "snmp socket") < 0)
		goto done;
	}
    }
    retval = 0;
 done:
    return retval;
}

/*! Init netsnmp agent connection
 * @param[in]  h      Clixon handle
 * @param[in]  logdst Log destination, see clixon_log.h
 * @see snmp_terminate
 */
static int
clixon_snmp_init(clicon_handle h,
		 int           logdst)
{
    int   retval = -1;
    char *sockpath = NULL;

    clicon_debug(1, "%s", __FUNCTION__);
    if (logdst == CLICON_LOG_SYSLOG)
	snmp_enable_calllog();
    else
	snmp_enable_stderrlog();
    /* make a agentx client. */
    netsnmp_ds_set_boolean(NETSNMP_DS_APPLICATION_ID, NETSNMP_DS_AGENT_ROLE, 1);    

    if ((sockpath = clicon_option_str(h, "CLICON_SNMP_AGENT_SOCK")) == NULL){
	clicon_err(OE_SNMP, 0, "CLICON_SNMP_AGENT_SOCK not set");
	goto done;
    }
    /* XXX: This should be configurable. */
    netsnmp_ds_set_string(NETSNMP_DS_APPLICATION_ID, NETSNMP_DS_AGENT_X_SOCKET, sockpath);

    /* initialize the agent library */
    init_agent(__PROGRAM__);

    /* XXX Hardcoded, replace this with generic MIB */
#if 1
    init_testhandler();
    init_testtable();
#else
    init_nstAgentSubagentObject(h);
#endif

    /* example-demon will be used to read example-demon.conf files. */
    init_snmp(__PROGRAM__);

    if (set_signal(SIGTERM, clixon_snmp_sig_term, NULL) < 0){
	clicon_err(OE_DAEMON, errno, "Setting signal");
	goto done;
    }
    if (set_signal(SIGINT, clixon_snmp_sig_term, NULL) < 0){
	clicon_err(OE_DAEMON, errno, "Setting signal");
	goto done;
    }
    if (set_signal(SIGPIPE, SIG_IGN, NULL) < 0){
	clicon_err(OE_UNIX, errno, "Setting DIGPIPE signal");
	goto done;
    }
    /* Workaround for netsnmps API use of fdset:s instead of sockets */
    if (clixon_snmp_fdset_register(h) < 0)
	goto done;
    retval = 0;
 done:
    return retval;
}


/*! Clean and close all state of netconf process (but dont exit). 
 * Cannot use h after this 
 * @param[in]  h  Clixon handle
 */
static int
snmp_terminate(clicon_handle h)
{
    yang_stmt  *yspec;
    cvec       *nsctx;
    cxobj      *x;
    
    shutdown_agent();
    clicon_rpc_close_session(h);
    if ((yspec = clicon_dbspec_yang(h)) != NULL)
	ys_free(yspec);
    if ((yspec = clicon_config_yang(h)) != NULL)
	ys_free(yspec);
    if ((nsctx = clicon_nsctx_global_get(h)) != NULL)
	cvec_free(nsctx);
    if ((x = clicon_conf_xml(h)) != NULL)
	xml_free(x);
    xpath_optimize_exit();
    clixon_event_exit();
    clicon_handle_exit(h);
    clixon_err_exit();
    clicon_log_exit();
    return 0;
}

/*! Usage help routine
 * @param[in]  h      Clixon handle
 * @param[in]  argv0  command line
 */
static void
usage(clicon_handle h,
      char         *argv0)
{
    fprintf(stderr, "usage:%s\n"
	    "where options are\n"
            "\t-h\t\tHelp\n"
	    "\t-D <level>\tDebug level\n"
    	    "\t-f <file>\tConfiguration file (mandatory)\n"
	    "\t-l (e|o|s|f<file>) Log on std(e)rr, std(o)ut, (s)yslog(default), (f)ile\n"
	    "\t-o \"<option>=<value>\"\tGive configuration option overriding config file (see clixon-config.yang)\n",
	    argv0
	    );
    exit(0);
}

int
main(int    argc,
     char **argv)
{
    int              retval = -1;
    int              c;
    char            *argv0 = argv[0];
    clicon_handle    h;
    int              logdst = CLICON_LOG_STDERR;
    struct passwd   *pw;
    yang_stmt       *yspec = NULL;
    char            *str;
    uint32_t         id;
    cvec            *nsctx_global = NULL; /* Global namespace context */
    size_t           cligen_buflen;
    size_t           cligen_bufthreshold;
    int              dbg = 0;
    size_t           sz;
    
    /* Create handle */
    if ((h = clicon_handle_init()) == NULL)
	return -1;
    /* In the startup, logs to stderr & debug flag set later */
    clicon_log_init(__PROGRAM__, LOG_INFO, logdst); 

    /* Set username to clixon handle. Use in all communication to backend */
    if ((pw = getpwuid(getuid())) == NULL){
	clicon_err(OE_UNIX, errno, "getpwuid");
	goto done;
    }
    if (clicon_username_set(h, pw->pw_name) < 0)
	goto done;
    while ((c = getopt(argc, argv, SNMP_OPTS)) != -1)
	switch (c) {
	case 'h' : /* help */
	    usage(h, argv[0]);
	    break;
	case 'D' : /* debug */
	    if (sscanf(optarg, "%d", &dbg) != 1)
		usage(h, argv[0]);
	    break;
	 case 'f': /* override config file */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    clicon_option_str_set(h, "CLICON_CONFIGFILE", optarg);
	    break;
	 case 'l': /* Log destination: s|e|o */
	    if ((logdst = clicon_log_opt(optarg[0])) < 0)
		usage(h, argv[0]);
	    if (logdst == CLICON_LOG_FILE &&
		strlen(optarg)>1 &&
		clicon_log_file(optarg+1) < 0)
		goto done;
	     break;
	}

    /* 
     * Logs, error and debug to stderr or syslog, set debug level
     */
    clicon_log_init(__PROGRAM__, dbg?LOG_DEBUG:LOG_INFO, logdst); 
    clicon_debug_init(dbg, NULL); 

    yang_init(h);
    
    /* Find, read and parse configfile */
    if (clicon_options_main(h) < 0)
	goto done;
    
    /* Now rest of options */
    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, SNMP_OPTS)) != -1)
	switch (c) {
	case 'h' : /* help */
	case 'D' : /* debug */
	case 'f':  /* config file */
	case 'l':  /* log  */
	    break; /* see above */
	case 'o':{ /* Configuration option */
	    char          *val;
	    if ((val = index(optarg, '=')) == NULL)
		usage(h, argv0);
	    *val++ = '\0';
	    if (clicon_option_add(h, optarg, val) < 0)
		goto done;
	    break;
	}
	default:
	    usage(h, argv[0]);
	    break;
	}
    argc -= optind;
    argv += optind;

    /* Access the remaining argv/argc options (after --) w clicon-argv_get() */
    clicon_argv_set(h, argv0, argc, argv);

    /* Init cligen buffers */
    cligen_buflen = clicon_option_int(h, "CLICON_CLI_BUF_START");
    cligen_bufthreshold = clicon_option_int(h, "CLICON_CLI_BUF_THRESHOLD");
    cbuf_alloc_set(cligen_buflen, cligen_bufthreshold);

    if ((sz = clicon_option_int(h, "CLICON_LOG_STRING_LIMIT")) != 0)
	clicon_log_string_limit_set(sz);

    /* Set default namespace according to CLICON_NAMESPACE_NETCONF_DEFAULT */
    xml_nsctx_namespace_netconf_default(h);

    /* Add (hardcoded) netconf features in case ietf-netconf loaded here
     * Otherwise it is loaded in netconf_module_load below
     */
    if (netconf_module_features(h) < 0)
	goto done;

    /* In case ietf-yang-metadata is loaded by application, handle annotation extension */
#if 0
    if (yang_metadata_init(h) < 0)
	goto done;    
#endif
    /* Create top-level yang spec and store as option */
    if ((yspec = yspec_new()) == NULL)
	goto done;
    clicon_dbspec_yang_set(h, yspec);	

    /* Load Yang modules
     * 1. Load a yang module as a specific absolute filename */
    if ((str = clicon_yang_main_file(h)) != NULL){
	if (yang_spec_parse_file(h, str, yspec) < 0)
	    goto done;
    }
    /* 2. Load a (single) main module */
    if ((str = clicon_yang_module_main(h)) != NULL){
	if (yang_spec_parse_module(h, str, clicon_yang_module_revision(h),
				   yspec) < 0)
	    goto done;
    }
    /* 3. Load all modules in a directory */
    if ((str = clicon_yang_main_dir(h)) != NULL){
	if (yang_spec_load_dir(h, str, yspec) < 0)
	    goto done;
    }
    /* Load clixon lib yang module */
    if (yang_spec_parse_module(h, "clixon-lib", NULL, yspec) < 0)
	goto done;
     /* Load yang module library, RFC7895 */
    if (yang_modules_init(h) < 0)
	goto done;
    /* Add netconf yang spec, used by netconf client and as internal protocol */
    if (netconf_module_load(h) < 0)
	goto done;
    /* Here all modules are loaded 
     * Compute and set canonical namespace context
     */
    if (xml_nsctx_yangspec(yspec, &nsctx_global) < 0)
	goto done;
    if (clicon_nsctx_global_set(h, nsctx_global) < 0)
	goto done;

#if 0
    /* Call start function is all plugins before we go interactive */
    if (clixon_plugin_start_all(h) < 0)
	goto done;
#endif
    /* Get session id from backend hello */
    clicon_session_id_set(h, getpid()); 

    /* Send hello request to backend to get session-id back
     * This is done once at the beginning of the session and then this is
     * used by the client, even though new TCP sessions are created for
     * each message sent to the backend.
     */
    if (clicon_hello_req(h, &id) < 0)
	goto done;
    clicon_session_id_set(h, id);
    
    /* Init snmp as subagent */
    if (clixon_snmp_init(h, logdst) < 0)
	goto done;
    
    if (dbg)
	clicon_option_dump(h, dbg);
    /* main event loop */
    if (clixon_event_loop(h) < 0)
	goto done;
    retval = 0;
  done:
    snmp_terminate(h);
    clicon_log_init(__PROGRAM__, LOG_INFO, 0); /* Log on syslog no stderr */
    clicon_log(LOG_NOTICE, "%s: %u Terminated", __PROGRAM__, getpid());
    return retval;
}
