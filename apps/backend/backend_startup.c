/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <pwd.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include <clixon/clixon.h>

#include "clixon_backend_transaction.h"
#include "clixon_backend_plugin.h"
#include "backend_handle.h"
#include "clixon_backend_commit.h"
#include "backend_startup.h"

/*! Merge db1 into db2 without commit 
 *
 * @retval    1       Validation OK       
 * @retval    0       Validation failed (with cbret set)
 * @retval   -1       Error
 */
static int
db_merge(clixon_handle h,
         const char   *db1,
         const char   *db2,
         cbuf         *cbret)
{
    int    retval = -1;
    cxobj *xt = NULL;

    /* Get data as xml from db1 */
    if (xmldb_get0(h, (char*)db1, YB_MODULE, NULL, NULL, 1, WITHDEFAULTS_EXPLICIT, &xt, NULL, NULL) < 0)
        goto done;
    xml_name_set(xt, NETCONF_INPUT_CONFIG);
    /* Merge xml into db2. Without commit */
    retval = xmldb_put(h, (char*)db2, OP_MERGE, xt, clicon_username_get(h), cbret);
 done:
    if (xt)
        xml_free(xt);
    return retval;
}

/*! Clixon startup startup mode: Commit startup configuration into running state
 *
 * @param[in]  h       Clixon handle
 * @param[in]  db      tmp or startup
 * @param[out] cbret   If status is invalid contains error message
 * @retval     1       OK
 * @retval     0       Validation failed
 * @retval    -1       Error

OK:
                              reset     
running                         |--------+------------> RUNNING
                parse validate OK       / commit 
startup -------+--+-------+------------+          


INVALID (requires manual edit of candidate)
failsafe      ----------------------+
                            reset    \ commit
running                       |-------+---------------> RUNNING FAILSAFE
              parse validate fail 
startup      ---+-------------------------------------> INVALID XML

ERR: (requires repair of startup) NYI
failsafe      ----------------------+
                            reset    \ commit
running                       |-------+---------------> RUNNING FAILSAFE
              parse fail
startup       --+-------------------------------------> BROKEN XML

 * @note: if commit fails, copy factory to running
 */
int
startup_mode_startup(clixon_handle        h,
                     char                *db,
                     cbuf                *cbret)
{
    int        retval = -1;
    int        ret = 0;
    int        rollback_exists;
    yang_stmt *yspec = clicon_dbspec_yang(h);

    if (strcmp(db, "running")==0){
        clixon_err(OE_FATAL, 0, "Invalid startup db: %s", db);
        goto done;
    }
    /* If startup does not exist, create it empty */
    if (xmldb_exists(h, db) != 1){ /* diff */
        if (xmldb_create(h, db) < 0) /* diff */
            return -1;
    }

    /* When a confirming-commit is issued, the confirmed-commit timeout
     * callback is removed and then the rollback database is deleted.
     *
     * The presence of a rollback database means that before the rollback
     * database was deleted, either clixon_backend crashed or the machine
     * rebooted.
     */
    if (if_feature(yspec, "ietf-netconf", "confirmed-commit")) {
        if ((rollback_exists = xmldb_exists(h, "rollback")) < 0) {
            clixon_err(OE_DAEMON, 0, "Error checking for the existence of the rollback database");
            goto done;
        }
        if (rollback_exists == 1) {
            ret = startup_commit(h, "rollback", cbret);
            switch(ret) {
                case -1:
                case 0:
                    /* validation failed, cbret set */
                    if ((ret = startup_commit(h, "failsafe", cbret)) < 0)
                        goto fail;

                    /* Rename the errored rollback database so that it is not tried on a subsequent startup */
                    xmldb_rename(h, db, NULL, ".error");
                    goto ok;
                case 1:
                    /* validation ok */
                    xmldb_delete(h, "rollback");
                    goto ok;
                default:
                    /* Unexpected response */
                    goto fail;
            }
        }
        else {
            if ((ret = startup_commit(h, db, cbret)) < 0)
                goto done;
            if (ret == 0)
                goto fail;
        }
    }
    else {
        if ((ret = startup_commit(h, db, cbret)) < 0)
            goto done;
        if (ret == 0)
            goto fail;
    }
 ok:
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Merge xml in filename into database
 *
 * @retval    1       Validation OK       
 * @retval    0       Validation failed (with cbret set)
 * @retval   -1       Error
 */
static int
load_extraxml(clixon_handle h,
              char         *filename,
              const char   *db,
              cbuf         *cbret)
{
    int        retval =  -1;
    cxobj     *xt = NULL;
    cxobj     *xerr = NULL;
    FILE      *fp = NULL;
    yang_stmt *yspec = NULL;
    int        ret;

    if (filename == NULL)
        return 1;
    if ((fp = fopen(filename, "r")) == NULL){
        clixon_err(OE_UNIX, errno, "open(%s)", filename);
        goto done;
    }
    yspec = clicon_dbspec_yang(h);
    /* No yang check yet because it has <config> as top symbol, do it later after that is removed */
    if (clixon_xml_parse_file(fp, YB_NONE, yspec, &xt, &xerr) < 0)
        goto done;
    /* Replace parent w first child */
    if (xml_rootchild(xt, 0, &xt) < 0)
        goto done;
    /* Ensure edit-config "config" statement */
    if (xt)
        xml_name_set(xt, NETCONF_INPUT_CONFIG);
    /* Now we can yang bind */
    if ((ret = xml_bind_yang(h, xt, YB_MODULE, yspec, &xerr)) < 0)
        goto done;
    if (ret == 0){
        if (netconf_err2cb(h, xerr, cbret) < 0)
            goto done;
        retval = 0;
        goto done;
    }
    /* Merge user reset state */
    retval = xmldb_put(h, (char*)db, OP_MERGE, xt, clicon_username_get(h), cbret);
 done:
    if (fp)
        fclose(fp);
    if (xt)
        xml_free(xt);
    return retval;
}

/*! Load extra XML via file and/or reset callback, and merge with current
 *
 * An application can add extra XML either via the -c <file> option or
 * via the .ca_reset callback. This XML is "merged" into running, that is,
 * it does not trigger validation calbacks.
 * The function uses an extra "tmp" database, loads the file to it, and calls
 * the reset function on it.
 * @param[in]  h       Clixon handle
 * @param[in]  file    (Optional) extra xml file
 * @param[out] status  Startup status
 * @param[out] cbret   If status is invalid contains error message
 * @retval     1       OK
 * @retval     0       Validation failed
 * @retval    -1       Error
                
running -----------------+----+------>
           reset  loadfile   / merge
tmp     |-------+-----+-----+
             reset   extrafile
 */
int
startup_extraxml(clixon_handle h,
                 char         *file,
                 cbuf         *cbret)
{
    int         retval = -1;
    char       *tmp_db = "tmp";
    int         ret;
    cxobj       *xt0 = NULL;
    cxobj       *xt = NULL;

    /* Clear tmp db */
    if (xmldb_db_reset(h, tmp_db) < 0)
        goto done;
    /* Application may define extra xml in its reset function */
    if (clixon_plugin_reset_all(h, tmp_db) < 0)
        goto done;
    /* Extra XML can also be added via file */
    if (file){
        /* Parse and load file into tmp db */
        if ((ret = load_extraxml(h, file, tmp_db, cbret)) < 0)
            goto done;
        if (ret == 0)
            goto fail;
    }
    /*
     * Check if tmp db is empty. 
     * It should be empty if extra-xml is null and reset plugins did nothing
     * then skip validation.
     */
    if ((ret = xmldb_get0(h, tmp_db, YB_MODULE, NULL, NULL, 1, 0, &xt0, NULL, NULL)) < 0)
        goto done;
    if (ret == 0){
        clixon_err(OE_DB, 0, "Error when reading from %s, unknown error", tmp_db);
        goto done;
    }
    if ((ret = xmldb_empty_get(h, tmp_db)) < 0)
        goto done;
    if (ret == 1)
        goto ok;
    xt = NULL;
    /* Clear db cache so that it can be read by startup */
    xmldb_clear(h, tmp_db);
    /* Validate the tmp db and return possibly upgraded xml in xt */
    if ((ret = startup_validate(h, tmp_db, &xt, cbret)) < 0)
        goto done;
    if (ret == 0)
        goto fail;
    if (xt==NULL || xml_child_nr(xt)==0)
        goto ok;
    /* Ensure yang bindings and defaults that were scratched in startup_validate */
    if (xmldb_populate(h, tmp_db) < 0)
        goto done;
    /* Merge tmp into running (no commit) */
    if ((ret = db_merge(h, tmp_db, "running", cbret)) < 0)
        goto fail;
    if (ret == 0)
        goto fail;
 ok:
    retval = 1;
 done:
    if (xt)
        xml_free(xt);
    if (xt0)
        xml_free(xt0);
    if (xmldb_delete(h, tmp_db) != 0 && errno != ENOENT)
        return -1;
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Init modules state of the backend (server). To compare with startup XML
 *
 * Set the modules state as setopt to the datastore module.
 * Only if CLICON_XMLDB_MODSTATE is enabled
 * @retval  0 OK
 * @retval -1 Error
 */
int
startup_module_state(clixon_handle h,
                     yang_stmt    *yspec)
{
    int    retval = -1;
    cxobj *x = NULL;
    int    ret;

    if (!clicon_option_bool(h, "CLICON_XMLDB_MODSTATE"))
        goto ok;
    /* Set up cache
     * Now, access brief module cache with clicon_modst_cache_get(h, 1) */
    if ((ret = yang_modules_state_get(h, yspec, NULL, NULL, 1, &x)) < 0)
        goto done;
    if (ret == 0)
        goto fail;
 ok:
    retval = 1;
 done:
    if (x)
        xml_free(x);
    return retval;
 fail:
    retval = 0;
    goto done;
}
