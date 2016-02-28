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

 * 
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <syslog.h>
#include <errno.h>
#include <assert.h>
#include <dlfcn.h>
#include <dirent.h>
#include <libgen.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

/* clicon_cli */
#include "clixon_cli_api.h"
#include "cli_plugin.h"
#include "cli_handle.h"


/*
 * Name of plugin functions
 * More in clicon_plugin.h
 */
#define PLUGIN_PROMPT_HOOK   "plugin_prompt_hook"
#define PLUGIN_PARSE_HOOK    "plugin_parse_hook"
#define PLUGIN_SUSP_HOOK     "plugin_susp_hook"

/*
 *
 * CLI PLUGIN INTERFACE, INTERNAL SECTION
 *
 */

/*
 * Find syntax mode named 'mode'. Create if specified
 */
static cli_syntaxmode_t *
syntax_mode_find(cli_syntax_t *stx, const char *mode, int create)
{
    cli_syntaxmode_t *m;

    m = stx->stx_modes;
    if (m) {
	do {
	    if (strcmp(m->csm_name, mode) == 0)
		return m;
	    m = NEXTQ(cli_syntaxmode_t *, m);
	} while (m && m != stx->stx_modes);
    }
    
    if (create == 0)
	return  NULL;

    if ((m = chunk(sizeof(cli_syntaxmode_t), stx->stx_cnklbl)) == NULL) {
	perror("chunk");
	return NULL;
    }
    memset (m, 0, sizeof (*m));
    strncpy(m->csm_name, mode, sizeof(m->csm_name)-1);
    strncpy(m->csm_prompt, CLI_DEFAULT_PROMPT, sizeof(m->csm_prompt)-1);
    INSQ(m, stx->stx_modes);
    stx->stx_nmodes++;

    return m;
}

/*
 * Find plugin by name
 */
static struct cli_plugin *
plugin_find_cli(cli_syntax_t *stx, char *plgnam)
{
    struct cli_plugin *p;
    
    if ((p = stx->stx_plugins) != NULL)
      do {
	if (strcmp (p->cp_name, plgnam) == 0)
	  return p;
	p = NEXTQ(struct cli_plugin *, p);
      } while (p && p != stx->stx_plugins);

    return NULL;
}

/*
 * Generate parse tree for syntax mode 
 */
static int
gen_parse_tree(clicon_handle h, cli_syntaxmode_t *m)
{
    cli_tree_add(h, m->csm_name, m->csm_pt);
    return 0;
}


/*
 * Append syntax
 */
static int
syntax_append(clicon_handle h,
	      cli_syntax_t *stx,
	      const char *name, 
	      parse_tree pt)
{
    cli_syntaxmode_t *m;

    if ((m = syntax_mode_find(stx, name, 1)) == NULL) 
	return -1;

    if (cligen_parsetree_merge(&m->csm_pt, NULL, pt) < 0)
	return -1;
    
    return 0;
}

/* 
 * Unload a plugin
 */
static int
plugin_unload(clicon_handle h, void *handle)
{
    int retval = 0;
    char *error;
    plgexit_t *exitfun;

    /* Call exit function is it exists */
    exitfun = dlsym(handle, PLUGIN_EXIT);
    if (dlerror() == NULL)
	exitfun(h);

    dlerror();    /* Clear any existing error */
    if (dlclose(handle) != 0) {
	error = (char*)dlerror();
	cli_output (stderr, "dlclose: %s\n", error ? error : "Unknown error");
	/* Just report */
    }

    return retval;
}

/*
 * Unload all plugins in a group
 */
static int
syntax_unload(clicon_handle h)
{
    struct cli_plugin *p;
    cli_syntax_t *stx = cli_syntax(h);
    
    if (stx == NULL)
	return 0;

    while (stx->stx_nplugins > 0) {
	p = stx->stx_plugins;
	plugin_unload(h, p->cp_handle);
	clicon_debug(1, "DEBUG: Plugin '%s' unloaded.", p->cp_name);
	DELQ(stx->stx_plugins, stx->stx_plugins, struct cli_plugin *);
	stx->stx_nplugins--;
    }
    while (stx->stx_nmodes > 0) {
	DELQ(stx->stx_modes, stx->stx_modes, cli_syntaxmode_t *);
	stx->stx_nmodes--;
    }

    unchunk_group(stx->stx_cnklbl);
    return 0;
}


/*! Dynamic string to function mapper
 *
 * The cli load function uses this function to map from strings to names.
 * handle is the dlopen handle, so it only looks in the current plugin being
 * loaded. It should also look in libraries?
 *
 * Returns a function pointer to the callback. Beware that this pointer
 * can theoretically be NULL depending on where the callback is loaded
 * into memory. Caller must check the error string which is non-NULL is
 * an error occured
 *
 * Compare with expand_str2fn - essentially identical.
 */
cg_fnstype_t *
load_str2fn(char *name, void *handle, char **error)
{
    cg_fnstype_t *fn = NULL;

    /* Reset error */
    *error = NULL;

    /* First check given plugin if any */
    if (handle) {
	dlerror();	/* Clear any existing error */
	fn = dlsym(handle, name);
	if ((*error = (char*)dlerror()) == NULL)
	    return fn;  /* If no error we found the address of the callback */
    }

    /* Now check global namespace which includes any shared object loaded
     * into the global namespace. I.e. all lib*.so as well as the 
     * master plugin if it exists 
     */
    dlerror();	/* Clear any existing error */
    fn = dlsym(NULL, name);
    if ((*error = (char*)dlerror()) == NULL)
	return fn;  /* If no error we found the address of the callback */

    /* Return value not really relevant here as the error string is set to
     * signal an error. However, just checking the function pointer for NULL
     * should work in most cases, although it's not 100% correct. 
     */
   return NULL; 
}

/*
 * expand_str2fn
 * maps strings from the CLI specification file to real funtions using dlopen 
 * mapping. One could do something more elaborate with namespaces and plugins:
 * x::a, x->a, but this is not done yet.
 * Compare with load_str2fn - essentially identical.
 * @param[in] name    Name of function
 * @param[in] handle  Handle to plugin .so module  as returned by dlopen, see cli_plugin_load
 */
expand_cb *
expand_str2fn(char *name, void *handle, char **error)
{
    expand_cb *fn = NULL;

    /* Reset error */
    *error = NULL;

    /* First check given plugin if any */
    if (handle) {
	dlerror();	/* Clear any existing error */
	fn = dlsym(handle, name);
	if ((*error = (char*)dlerror()) == NULL)
	    return fn;  /* If no error we found the address of the callback */
    }

    /* Now check global namespace which includes any shared object loaded
     * into the global namespace. I.e. all lib*.so as well as the 
     * master plugin if it exists 
     */
    dlerror();	/* Clear any existing error */
    fn = dlsym(NULL, name);
    if ((*error = (char*)dlerror()) == NULL)
	return fn;  /* If no error we found the address of the callback */

    /* Return value not really relevant here as the error string is set to
     * signal an error. However, just checking the function pointer for NULL
     * should work in most cases, although it's not 100% correct. 
     */
   return NULL; 
}


/*
 * Load a dynamic plugin object and call it's init-function
 * Note 'file' may be destructively modified
 */
static plghndl_t 
cli_plugin_load (clicon_handle h, char *file, int dlflags, const char *cnklbl)
{
    char              *error;
    char              *name;
    void              *handle = NULL;
    plginit_t         *initfun;
    struct cli_plugin *cp = NULL;

    dlerror();    /* Clear any existing error */
    if ((handle = dlopen (file, dlflags)) == NULL) {
        error = (char*)dlerror();
	cli_output (stderr, "dlopen: %s\n", error ? error : "Unknown error");
	goto quit;
    }
    /* call plugin_init() if defined */
    if ((initfun = dlsym(handle, PLUGIN_INIT)) != NULL) {
	if (initfun(h) != 0) { 
	    cli_output (stderr, "Failed to initiate %s\n", strrchr(file,'/')?strchr(file, '/'):file);
	    goto quit;
	}
    }
    
    if ((cp = chunk(sizeof (struct cli_plugin), cnklbl)) == NULL) {
	perror("chunk");
	goto quit;
    }
    memset (cp, 0, sizeof(*cp));

    name = basename(file);
    snprintf(cp->cp_name, sizeof(cp->cp_name), "%.*s", (int)strlen(name)-3, name);
    cp->cp_handle = handle;

quit:
    if (cp == NULL) {
	if (handle)
	    dlclose(handle);
    }
    return cp;
}

/*
 * Append to syntax mode from file
 * Arguments:
 *   filename	: Name of file where syntax is specified (in syntax-group dir)
 */
static int
cli_load_syntax(clicon_handle h, const char *filename, const char *clispec_dir)
{
    void      *handle = NULL;  /* Handle to plugin .so module */
    char      *mode = NULL;    /* Name of syntax mode to append new syntax */
    parse_tree pt = {0,};
    int        retval = -1;
    FILE      *f;
    char      *filepath;
    cvec      *vr = NULL;
    char      *prompt = NULL;
    char     **vec = NULL;
    int        i, nvec;
    char      *plgnam;
    struct cli_plugin *p;

    if ((filepath = chunk_sprintf(__FUNCTION__, "%s/%s", 
				  clispec_dir,
				  filename)) == NULL){
	clicon_err(OE_PLUGIN, errno, "chunk");
	goto done;
    }
    if ((vr = cvec_new(0)) == NULL){
	clicon_err(OE_PLUGIN, errno, "cvec_new");
	goto done;
    }
    /* Build parse tree from syntax spec. */
    if ((f = fopen(filepath, "r")) == NULL){
	clicon_err(OE_PLUGIN, errno, "fopen %s", filepath);
	goto done;
    }

    /* Assuming this plugin is first in queue */
    if (cli_parse_file(h, f, filepath, &pt, vr) < 0){
	clicon_err(OE_PLUGIN, 0, "failed to parse cli file %s", filepath);
	fclose(f);
	goto done;
    }
    fclose(f);
	
    /* Get CLICON specific global variables */
    prompt = cvec_find_str(vr, "CLICON_PROMPT");
    plgnam = cvec_find_str(vr, "CLICON_PLUGIN");
    mode = cvec_find_str(vr, "CLICON_MODE");

    if (plgnam != NULL) { /* Find plugin for callback resolving */
	if ((p = plugin_find_cli (cli_syntax(h), plgnam)) != NULL)
	    handle = p->cp_handle;
	if (handle == NULL){
	    clicon_err(OE_PLUGIN, 0, "CLICON_PLUGIN set to '%s' in %s but plugin %s.so not found in %s\n", 
		       plgnam, filename, plgnam, 
		       clicon_cli_dir(h));
	    goto done;
	}
    }

    /* Resolve callback names to function pointers */
    if (cligen_callback_str2fn(pt, load_str2fn, handle) < 0){     
	clicon_err(OE_PLUGIN, 0, "Mismatch between CLIgen file '%s' and CLI plugin file '%s'. Some possible errors:\n\t1. A function given in the CLIgen file does not exist in the plugin (ie link error)\n\t2. The CLIgen spec does not point to the correct plugin .so file (CLICON_PLUGIN=\"%s\" is wrong)", 
		   filename, plgnam, plgnam);
	goto done;
    }
    if (cligen_expand_str2fn(pt, expand_str2fn, handle) < 0)     
	goto done;


    /* Make sure we have a syntax mode specified */
    if (mode == NULL || strlen(mode) < 1) { /* may be null if not given in file */
	clicon_err(OE_PLUGIN, 0, "No syntax mode specified in %s", filepath);
	goto done;
    }
    if ((vec = clicon_strsplit(mode, ":", &nvec, __FUNCTION__)) == NULL) {
	goto done;
    }
    for (i = 0; i < nvec; i++) {
	if (syntax_append(h, cli_syntax(h), vec[i], pt) < 0) { 
	    goto done;
	}
	if (prompt)
	    cli_set_prompt(h, vec[i], prompt);
    }

    cligen_parsetree_free(pt, 1);
    retval = 0;
    
done:
    if (vr)
	cvec_free(vr);
    unchunk_group(__FUNCTION__);
    return retval;
}

/*
 * Load plugins within a directory
 */
static int
cli_plugin_load_dir(clicon_handle h, char *dir, cli_syntax_t *stx)
{
    int                i;
    int	               ndp;
    struct dirent     *dp;
    char              *file;
    char              *master_plugin;
    char              *master;
    struct cli_plugin *cp;
    struct stat        st;
    int                retval = -1;


    /* Format master plugin path */
    if ((master_plugin = clicon_master_plugin(h)) == NULL){
	clicon_err(OE_PLUGIN, 0, "clicon_master_plugin option not set");
	goto quit;
    }
    if ((master = chunk_sprintf(__FUNCTION__, "%s.so", master_plugin)) == NULL){
	clicon_err(OE_PLUGIN, errno, "chunk_sprintf master plugin");
	goto quit;
    }
    /* Get plugin objects names from plugin directory */
    ndp = clicon_file_dirent(dir, &dp, "(.so)$", S_IFREG, __FUNCTION__);
    if (ndp < 0)
        goto quit;

    /* Load master plugin first */
    file = chunk_sprintf(__FUNCTION__, "%s/%s", dir, master);
    if (file == NULL) {
	clicon_err(OE_UNIX, errno, "chunk_sprintf dir");
	goto quit;
    }
    if (stat(file, &st) == 0) {
	clicon_debug(1, "DEBUG: Loading master plugin '%s'", master);
	cp = cli_plugin_load(h, file, RTLD_NOW|RTLD_GLOBAL, stx->stx_cnklbl);
	if (cp == NULL)
	    goto quit;
	/* Look up certain call-backs in master plugin */
	stx->stx_prompt_hook = 
	    dlsym(cp->cp_handle, PLUGIN_PROMPT_HOOK);
	stx->stx_parse_hook =
	    dlsym(cp->cp_handle, PLUGIN_PARSE_HOOK);
	stx->stx_susp_hook =
	    dlsym(cp->cp_handle, PLUGIN_SUSP_HOOK);
	INSQ(cp, stx->stx_plugins);
	stx->stx_nplugins++;
    }
    unchunk (file);

    /* Load the rest */
    for (i = 0; i < ndp; i++) {
	if (strcmp (dp[i].d_name, master) == 0)
	    continue; /* Skip master now */
	file = chunk_sprintf(__FUNCTION__, "%s/%s", dir, dp[i].d_name);
	if (file == NULL) {
	    clicon_err(OE_UNIX, errno, "chunk_sprintf dir");
	    goto quit;
	}
	clicon_debug(1, "DEBUG: Loading plugin '%s'", dp[i].d_name);

	if ((cp = cli_plugin_load (h, file, RTLD_NOW, stx->stx_cnklbl)) == NULL)
	    goto quit;
	INSQ(cp, stx->stx_plugins);
	stx->stx_nplugins++;
	unchunk (file);
    }
    if (dp)
	unchunk(dp);

    retval = 0;

 quit:
    unchunk_group(__FUNCTION__);

    return retval;
}


/*
 * Load a syntax group.
 */
int
cli_syntax_load (clicon_handle h)
{
    int                retval = -1;
    char              *plugin_dir = NULL;
    char              *clispec_dir = NULL;
    int                ndp;
    int                i;
    char              *cnklbl = "__CLICON_CLI_SYNTAX_CNK_LABEL__";
    struct dirent     *dp;
    cli_syntax_t      *stx;
    cli_syntaxmode_t  *m;

    /* Syntax already loaded.  XXX should we re-load?? */
    if ((stx = cli_syntax(h)) != NULL)
	return 0;

    /* Format plugin directory path */
    if ((plugin_dir = clicon_cli_dir(h)) == NULL){
	clicon_err(OE_FATAL, 0, "clicon_cli_dir not set");
	goto quit;
    }
    if ((clispec_dir = clicon_clispec_dir(h)) == NULL){
	clicon_err(OE_FATAL, 0, "clicon_clispec_dir not set");
	goto quit;
    }

    /* Allocate plugin group object */
    if ((stx = chunk(sizeof(*stx), cnklbl)) == NULL) {
	clicon_err(OE_UNIX, errno, "chunk");
	goto quit;
    }
    memset (stx, 0, sizeof (*stx));	/* Zero out all */
    /* populate name and chunk label */
    strncpy (stx->stx_cnklbl, cnklbl, sizeof(stx->stx_cnklbl)-1);

    cli_syntax_set(h, stx);

    /* First load CLICON system plugins. CLIXON_CLI_SYSDIR is defined
       in Makefile*/
    if (cli_plugin_load_dir(h, CLIXON_CLI_SYSDIR, stx) < 0)
        goto quit;
    
    /* Then load application plugins */
    if (cli_plugin_load_dir(h, plugin_dir, stx) < 0)
        goto quit;
    
    /* load syntaxfiles */
    if ((ndp = clicon_file_dirent(clispec_dir, &dp, "(.cli)$", S_IFREG, __FUNCTION__)) < 0)
	goto quit;
    /* Load the rest */
    for (i = 0; i < ndp; i++) {
	clicon_debug(1, "DEBUG: Loading syntax '%.*s'", 
		    (int)strlen(dp[i].d_name)-4, dp[i].d_name);
	if (cli_load_syntax(h, dp[i].d_name, clispec_dir) < 0)
	    goto quit;
    }
    if (dp)
	unchunk(dp);


    /* Did we successfully load any syntax modes? */
    if (stx->stx_nmodes <= 0) {
	retval = 0;
	goto quit;
    }	
    /* Parse syntax tree for all modes */
    m = stx->stx_modes;
    do {
	if (gen_parse_tree(h, m) != 0)
	    goto quit;
	m = NEXTQ(cli_syntaxmode_t *, m);
    } while (m && m != stx->stx_modes);


    /* Set callbacks into  CLIgen */
    cli_susp_hook(h, cli_syntax(h)->stx_susp_hook);

    /* All good. We can now proudly return a new group */
    retval = 0;

quit:
    if (retval != 0) {
	syntax_unload(h);
	unchunk_group(cnklbl);
	cli_syntax_set(h, NULL);
    }
    unchunk_group(__FUNCTION__);
    return retval;
}

/*
 * Call plugin_start() in all plugins
 */
int
cli_plugin_start(clicon_handle h, int argc, char **argv)
{
    struct cli_plugin *p;
    cli_syntax_t *stx;
    plgstart_t *startfun;
// XXX    int (*startfun)(clicon_handle, int, char **);
    
    stx = cli_syntax(h);

    if ((p = stx->stx_plugins) != NULL)
	do {
	    startfun = dlsym(p->cp_handle, PLUGIN_START);
	    if (dlerror() == NULL)
		startfun(h, argc, argv);
	    p = NEXTQ(struct cli_plugin *, p);
	} while (p && p != stx->stx_plugins);
    
    return 0;
}

/*

 */
int
cli_plugin_finish(clicon_handle h)
{
    syntax_unload(h);
    cli_syntax_set(h, NULL);
    return 0;
}

/*! Help function to print a meaningful error string. 
 * Sometimes the libraries specify an error string, if so print that.
 * Otherwise just print 'command error'.
 */
int 
cli_handler_err(FILE *f)
{
    if (clicon_errno){
	cli_output(f,  "%s: %s", 
		   clicon_strerror(clicon_errno),
		   clicon_err_reason);
	if (clicon_suberrno)
	    cli_output(f, ": %s", strerror(clicon_suberrno));
	cli_output(f,  "\n");

    }
    else
	cli_output(f, "CLI command error\n");
    return 0;
}


/*
 * Evaluate a matched command
 */
int
clicon_eval(clicon_handle h, char *cmd, cg_obj *match_obj, cvec *vr)
{
    cli_output_reset();
#ifdef notyet
    if (isrecording())
	record_command(cmd);
#endif
    if (!cli_exiting(h)) {	
	clicon_err_reset();
	if (cligen_eval(cli_cligen(h), match_obj, vr) < 0) {
#if 0 /* This is removed since we get two error messages on failure.
	 But maybe only sometime?
	 Both a real log when clicon_err is called, and the  here again.
	 (Before clicon_err was silent)  */
	    cli_handler_err(stdout); 
#endif
	}
    }
    return 0;
}


/*
 * clicon_parse
 * Given a command string, parse and evaluate the string according to
 * the syntax parse tree of the syntax mode specified by *mode.
 * If there is no match in the tree for the command, the parse hook 
 * will be called to see if another mode should be evaluated. If a
 * match is found in another mode, the mode variable is updated to point at 
 * the new mode string.
 *
 * INPUT:
 *   cmd	The command string
 *   match_obj  Pointer to CLIgen match object
 *   mode	A pointer to the mode string pointer
 * OUTPUT:
 *   kr         Keyword vector
 *   vr         Variable vector
 * RETURNS:
 *   -2	      : on eof (shouldnt happen)
 *   -1	      : In parse error
 *   >=0      : Number of matches
 */
int
clicon_parse(clicon_handle h, char *cmd, char **mode, int *result)
{
    char *m, *msav;
    int res = -1;
    int r;
    cli_syntax_t *stx;
    cli_syntaxmode_t *smode;
    char       *treename;
    parse_tree *pt;     /* Orig */
    cg_obj     *match_obj;
    cvec       *vr = NULL;
    
    stx = cli_syntax(h);
    m = *mode;
    if (m == NULL) {
	smode = stx->stx_active_mode;
	m = smode->csm_name;
    }
    else {
	if ((smode = syntax_mode_find(stx, m, 0)) == NULL) {
	    cli_output(stderr, "Can't find syntax mode '%s'\n", m);
	    return -1;
	}
    }
    msav = NULL;
    while(smode) {
	if (cli_tree_active(h))
	    msav = strdup(cli_tree_active(h)); 
	cli_tree_active_set(h, m);
	treename = cli_tree_active(h);
	if ((pt = cli_tree(h, treename)) == NULL){
	    fprintf(stderr, "No such parse-tree registered: %s\n", treename);
	    goto done;;
	}
	if ((vr = cvec_new(0)) == NULL){
	    fprintf(stderr, "%s: cvec_new: %s\n", __FUNCTION__, strerror(errno));
	    goto done;;
	}
	res = cliread_parse(cli_cligen(h), cmd, pt, &match_obj, vr);
	if (res != CG_MATCH)
	    pt_expand_cleanup_1(pt);
	if (msav){
	    cli_tree_active_set(h, msav);
	    free(msav);
	}
	msav = NULL;
	switch (res) {
	case CG_EOF: /* eof */
	case CG_ERROR:
	    cli_output(stderr, "CLI parse error: %s\n", cmd);
	    goto done;
	case CG_NOMATCH: /* no match */
	    smode = NULL;
	    if (stx->stx_parse_hook) {
		/* Try to find a match in upper  modes, a'la IOS. */
		if ((m = stx->stx_parse_hook(h, cmd, m)) != NULL)  {
		    if ((smode = syntax_mode_find(stx, m, 0)) != NULL)
			continue;
		    else
			cli_output(stderr, "Can't find syntax mode '%s'\n", m);
		}
	    }
	    cli_output(stderr, "CLI syntax error: \"%s\": %s\n", 
		       cmd, cli_nomatch(h));
	    break;
	case CG_MATCH:
	    if (m != *mode){	/* Command in different mode */
		*mode = m;
		cli_set_syntax_mode(h, m);
	    }
	    if ((r = clicon_eval(h, cmd, match_obj, vr)) < 0)
		cli_handler_err(stdout);
	    pt_expand_cleanup_1(pt);
	    if (result)
		*result = r;
	    goto done;
	    break;
	default:
	    cli_output(stderr, "CLI syntax error: \"%s\" is ambiguous\n", cmd);
	    goto done;
	    break;
	}
    }
done:
    if (vr)
	cvec_free(vr);
    return res;
}

/*
 * Read command from CLIgen's cliread() using current syntax mode.
 */
char *
clicon_cliread(clicon_handle h)
{
    char *ret;
    char *pfmt = NULL;
    cli_syntaxmode_t *mode;
    cli_syntax_t *stx;

    stx = cli_syntax(h);
    mode = stx->stx_active_mode;

    if (stx->stx_prompt_hook)
	pfmt = stx->stx_prompt_hook(h, mode->csm_name);
    if (clicon_quiet_mode(h))
	cli_prompt_set(h, "");
    else
	cli_prompt_set(h, cli_prompt(pfmt ? pfmt : mode->csm_prompt));
    cli_tree_active_set(h, mode->csm_name);
    ret = cliread(cli_cligen(h));
    if (pfmt)
	free(pfmt);
    return ret;
}

/*
 * cli_find_plugin
 * Find a plugin by name and return the dlsym handl
 * Used by libclicon code to find callback funcctions in plugins.
 */
static void *
cli_find_plugin(clicon_handle h, char *plugin)
{
    struct cli_plugin *p;
    
    p = plugin_find_cli(cli_syntax(h), plugin);
    if (p)
	return p->cp_handle;
    
    return NULL;
}


/*! Initialize plugin code (not the plugins themselves)
 */
int
cli_plugin_init(clicon_handle h)
{
    find_plugin_t *fp = cli_find_plugin;
    clicon_hash_t *data = clicon_data(h);

    /* Register CLICON_FIND_PLUGIN in data hash */
    if (hash_add(data, "CLICON_FIND_PLUGIN", &fp, sizeof(fp)) == NULL) {
	clicon_err(OE_UNIX, errno, "failed to register CLICON_FIND_PLUGIN");
	return -1;
    }
	
    return 0;
}


/*
 *
 * CLI PLUGIN INTERFACE, PUBLIC SECTION
 *
 */


/*
 * Set syntax mode mode for existing current plugin group.
 */
int
cli_set_syntax_mode(clicon_handle h, const char *name)
{
    cli_syntaxmode_t *mode;
    
    if ((mode = syntax_mode_find(cli_syntax(h), name, 1)) == NULL)
	return 0;
    
    cli_syntax(h)->stx_active_mode = mode;
    return 1;
}

/*
 * Get syntax mode name
 */
char *
cli_syntax_mode(clicon_handle h)
{
    cli_syntaxmode_t *csm;

    if ((csm = cli_syntax(h)->stx_active_mode) == NULL)
	return NULL;
    return csm->csm_name;
}


/*
 * Callback from cli_set_prompt(). Set prompt format for syntax mode
 * Arguments:
 *   name	: Name of syntax mode 
 *   prompt	: Prompt format
 */
int
cli_set_prompt(clicon_handle h, const char *name, const char *prompt)
{
    cli_syntaxmode_t *m;

    if ((m = syntax_mode_find(cli_syntax(h), name, 1)) == NULL)
	return -1;
    
    strncpy(m->csm_prompt, prompt, sizeof(m->csm_prompt)-1);
    return 0;
}

/* 
 * Format prompt 
 * XXX: HOST_NAME_MAX from sysconf()
 */
static int
prompt_fmt (char *prompt, size_t plen, char *fmt, ...)
{
  va_list ap;
  char   *s = fmt;
  char    hname[1024];
  char    tty[32];
  char   *new;
  char   *tmp;
  int     ret = -1;
  
  /* Start with empty string */
  if((new = chunk_sprintf(__FUNCTION__, "%s", ""))==NULL)
      goto done;
  
  while(*s) {
      if (*s == '%' && *++s) {
	  switch(*s) {
	      
	  case 'H': /* Hostname */
	      if (gethostname (hname, sizeof (hname)) != 0)
		  strncpy(hname, "unknown", sizeof(hname)-1);
	      if((new = chunk_strncat(new, hname, 0, __FUNCTION__))==NULL)
		  goto done;
	      break;
	      
	  case 'U': /* Username */
	      tmp = getenv("USER");
	      if((new = chunk_strncat(new, (tmp ? tmp : "nobody"), 0, __FUNCTION__))==NULL)
		  goto done;
	      break;
	      
	  case 'T': /* TTY */
	      if(ttyname_r(fileno(stdin), tty, sizeof(tty)-1) < 0)
		  strcpy(tty, "notty");
	      if((new = chunk_strncat(new, tty, strlen(tty), __FUNCTION__))==NULL)
		  goto done;
	      break;
	      
	  default:
	      if((new = chunk_strncat(new, "%", 1, __FUNCTION__))==NULL ||
		 (new = chunk_strncat(new, s, 1, __FUNCTION__)))
		  goto done;
	  }
      } 
      else {
	  if ((new = chunk_strncat(new, s, 1, __FUNCTION__))==NULL)
	      goto done;
      }
      s++;
  }
  
done:
  if (new)
      fmt = new;
  va_start(ap, fmt);
  ret = vsnprintf(prompt, plen, fmt, ap);
  va_end(ap);
  
  unchunk_group(__FUNCTION__);
  
  return ret;
}

/*
 * Return a formatted prompt string
 */
char *
cli_prompt(char *fmt)
{
    static char prompt[CLI_PROMPT_LEN];

    if (prompt_fmt(prompt, sizeof(prompt), fmt) < 0)
	return CLI_DEFAULT_PROMPT;
    
    return prompt;
}


/*
 * Run command in CLI engine
 */
int
cli_exec(clicon_handle h, char *cmd, char **mode, int *result)
{
    return clicon_parse(h, cmd, mode, result);
}


/*
 * push_one
 * nifty code that 'pushes' a syntax one ore more levels
 * op: eg "set"
 * cmd: eg "edit policy-options"
 */
int
cli_ptpush(clicon_handle h, char *mode, char *string, char *op)
{
    cg_obj *co, *co_cmd, *cc;
    parse_tree *pt;
    char **vec = NULL;
    int i, j, nvec;
    int found;
    parse_tree pt_top;
    cli_syntaxmode_t *m;

    if ((m = syntax_mode_find(cli_syntax(h), mode, 0)) == NULL)
	return 0;
    pt_top = m->csm_pt;
    if ((co_cmd = co_find_one(pt_top, op)) == NULL)
	return 0;
    pt = &co_cmd->co_pt;
    /* vec is the command, eg 'edit policy_option' */
    if ((vec = clicon_strsplit(string, " ", &nvec, __FUNCTION__)) == NULL)
	goto catch;
    co = NULL;
    found = 0;
    for (i=0; i<nvec; i++){
	found = 0;
	for (j=0; j<pt->pt_len; j++){
	    co = pt->pt_vec[j];
	    if (co && co->co_type == CO_COMMAND && 
		(strcmp(co->co_command, vec[i])==0)){
		pt = &co->co_pt;
		found++;
		break;
	    }
	}
	if (!found)
	    break;//not found on this level
    }
    if (found){ // match all levels
	if (!co_cmd->co_pushed){
	    co_cmd->co_pt_push = co_cmd->co_pt;
	    co_cmd->co_pushed++;
	}
	co_cmd->co_pt = co->co_pt;
	pt = &co_cmd->co_pt;
	for (i=0; i<pt->pt_len; i++) /* set correct parent */
	    if ((cc = pt->pt_vec[i]) != NULL)
		co_up_set(cc, co_cmd);
    }
  catch:
    unchunk_group(__FUNCTION__) ;
    return 0;
}

int
cli_ptpop(clicon_handle h, char *mode, char *op)
{
    cg_obj  *co_cmd, *cc;
    int i;
    parse_tree *pt;
    parse_tree pt_top;
    cli_syntaxmode_t *m;

    if ((m = syntax_mode_find(cli_syntax(h), mode, 0)) == NULL)
	return 0;
    pt_top = m->csm_pt;
    if ((co_cmd = co_find_one(pt_top, op)) == NULL) //set
	return 0;
    if (!co_cmd->co_pushed)
	return 0;
    co_cmd->co_pushed = 0;
    co_cmd->co_pt = co_cmd->co_pt_push;
    pt = &co_cmd->co_pt;
    for (i=0; i<pt->pt_len; i++) /* set correct parent */	    
	if ((cc = pt->pt_vec[i]) != NULL)
	    co_up_set(cc, co_cmd);
    return 0;
}


/*
 * clicon_valcb
 * Callback from clicon_dbvars_parse() 
 * Find a cli plugin based on name if given and
 * use dlsym to resolve a function pointer in it.
 * Call the resolved function to get the cgv populated
 */
int
clicon_valcb(void *arg, cvec *vars, cg_var *cgv, char *fname, cg_var *funcarg)
{
    char *func;
    char *plgnam = NULL;
    void *handle;
    struct cli_plugin *p;
    cli_valcb_t *cb;
    clicon_handle h = (clicon_handle)arg;

    /* Make copy */
    if ((fname = strdup(fname)) == NULL) {
	clicon_err(OE_UNIX, errno, "strdup");
	return -1;
    }

    /* Extract plugin name if any */
    if ((func = strstr(fname, "::")) != NULL) {
	*func = '\0';
	func += 2;
	plgnam = fname;
    }
    else
	func = fname;
    
    /* If we have specified a plugin name, find the handle to be used
     * with dlsym()
     */
    handle = NULL;
    if (plgnam && (p = plugin_find_cli(cli_syntax(h), plgnam)))
	handle = p->cp_handle;
    
    /* Look up function pointer */
    if ((cb = dlsym(handle, func)) == NULL) {
	clicon_err(OE_UNIX, errno, "unable to find %s()", func);
	free(fname);
	return -1;
    }
    free(fname);

    if (cb(vars, cgv, funcarg) < 0)
	return -1;

    return 0;
}

