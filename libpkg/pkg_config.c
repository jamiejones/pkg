/*-
 * Copyright (c) 2011-2013 Baptiste Daroussin <bapt@FreeBSD.org>
 * Copyright (c) 2011-2012 Julien Laffaye <jlaffaye@FreeBSD.org>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <assert.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <ucl.h>

#include "pkg.h"
#include "private/pkg.h"
#include "private/event.h"

#define ABI_VAR_STRING "${ABI}"
#define REPO_NAME_PREFIX "repo-"

int eventpipe = -1;

struct config_entry {
	uint8_t type;
	const char *key;
	const char *def;
	const char *desc;
};

static char myabi[BUFSIZ];
static struct pkg_repo *repos = NULL;
static struct pkg_config *config = NULL;
static struct pkg_config *config_by_key = NULL;

static struct config_entry c[] = {
	[PKG_CONFIG_REPO] = {
		PKG_CONFIG_STRING,
		"PACKAGESITE",
#ifdef DEFAULT_PACKAGESITE
		DEFAULT_PACKAGESITE,
#else
		NULL,
#endif
		"Repository URL",
	},
	[PKG_CONFIG_DBDIR] = {
		PKG_CONFIG_STRING,
		"PKG_DBDIR",
		"/var/db/pkg",
		"Where the package databases are stored",
	},
	[PKG_CONFIG_CACHEDIR] = {
		PKG_CONFIG_STRING,
		"PKG_CACHEDIR",
		"/var/cache/pkg",
		"Directory containing cache of downloaded packages",
	},
	[PKG_CONFIG_PORTSDIR] = {
		PKG_CONFIG_STRING,
		"PORTSDIR",
#ifdef PORTSDIR
		PORTSDIR,
#else
		"/usr/ports",
#endif
		"Location of the ports collection",
	},
	[PKG_CONFIG_REPOKEY] = {
		PKG_CONFIG_STRING,
		"PUBKEY",
		NULL,
		"Public key for authenticating packages from the chosen repository",
	},
	[PKG_CONFIG_HANDLE_RC_SCRIPTS] = {
		PKG_CONFIG_BOOL,
		"HANDLE_RC_SCRIPTS",
		NULL,
		"Automatically handle restarting services",
	},
	[PKG_CONFIG_ASSUME_ALWAYS_YES] = {
		PKG_CONFIG_BOOL,
		"ASSUME_ALWAYS_YES",
		NULL,
		"Answer 'yes' to all pkg(8) questions",
	},
	[PKG_CONFIG_REPOS_DIR] = {
		PKG_CONFIG_LIST,
		"REPOS_DIR",
		"/etc/pkg/,"PREFIX"/etc/pkg/repos/",
		"Location of the repository configuration files"
	},
	[PKG_CONFIG_PLIST_KEYWORDS_DIR] = {
		PKG_CONFIG_STRING,
		"PLIST_KEYWORDS_DIR",
		NULL,
		"Directory containing definitions of plist keywords",
	},
	[PKG_CONFIG_SYSLOG] = {
		PKG_CONFIG_BOOL,
		"SYSLOG",
		"YES",
		"Log pkg(8) operations via syslog(3)",
	},
	[PKG_CONFIG_AUTODEPS] = {
		PKG_CONFIG_BOOL,
		"AUTODEPS",
		"YES",
		"Automatically append dependencies to fulfil dynamic linking requrements of binaries",
	},
	[PKG_CONFIG_ABI] = {
		PKG_CONFIG_STRING,
		"ABI",
		myabi,
		"Override the automatically detected ABI",
	},
	[PKG_CONFIG_DEVELOPER_MODE] = {
		PKG_CONFIG_BOOL,
		"DEVELOPER_MODE",
		"NO",
		"Add extra strict, pedantic warnings as an aid to package maintainers",
	},
	[PKG_CONFIG_PORTAUDIT_SITE] = {
		PKG_CONFIG_STRING,
		"PORTAUDIT_SITE",
#ifdef DEFAULT_AUDIT_URL
		DEFAULT_AUDIT_URL,
#else
		"http://portaudit.FreeBSD.org/auditfile.tbz",
#endif
		"URL giving location of the audit database",
	},
	[PKG_CONFIG_VULNXML_SITE] = {
		PKG_CONFIG_STRING,
		"VULNXML_SITE",
#ifdef DEFAULT_VULNXML_URL
		DEFAULT_VULNXML_URL,
#else
		"http://www.vuxml.org/freebsd/vuln.xml.bz2",
#endif
		"URL giving location of the vulnxml database",
	},
	[PKG_CONFIG_MIRRORS] = {
		PKG_CONFIG_STRING,
		"MIRROR_TYPE",
#if DEFAULT_MIRROR_TYPE == 1
		"SRV",
#elif DEFAULT_MIRROR_TYPE == 2
		"HTTP",
#else
		NULL,
#endif
		"How to locate alternate mirror sites of a repository (one of: 'SRV', 'HTTP')",
	},
	[PKG_CONFIG_FETCH_RETRY] = {
		PKG_CONFIG_INTEGER,
		"FETCH_RETRY",
		"3",
		"How many times to retry fetching files",
	},
	[PKG_CONFIG_PLUGINS_DIR] = {
		PKG_CONFIG_STRING,
		"PKG_PLUGINS_DIR",
		PREFIX"/lib/pkg/",
		"Directory which pkg(8) will load plugins from",
	},
	[PKG_CONFIG_ENABLE_PLUGINS] = {
		PKG_CONFIG_BOOL,
		"PKG_ENABLE_PLUGINS",
		"YES",
		"Activate plugin support",
	},
	[PKG_CONFIG_PLUGINS] = {
		PKG_CONFIG_LIST,
		"PLUGINS",
		NULL,
		"List of plugins that pkg(8) should load",
	},
	[PKG_CONFIG_DEBUG_SCRIPTS] = {
		PKG_CONFIG_BOOL,
		"DEBUG_SCRIPTS",
		"NO",
		"Run shell scripts in verbose mode to facilitate debugging",
	},
	[PKG_CONFIG_PLUGINS_CONF_DIR] = {
		PKG_CONFIG_STRING,
		"PLUGINS_CONF_DIR",
		PREFIX"/etc/pkg/",
		"Directory containing plugin configuration data",
	},
	[PKG_CONFIG_PERMISSIVE] = {
		PKG_CONFIG_BOOL,
		"PERMISSIVE",
		"NO",
		"Permit package installation despite presence of conflicting packages",
	},
	[PKG_CONFIG_REPO_AUTOUPDATE] = {
		PKG_CONFIG_BOOL,
		"REPO_AUTOUPDATE",
		"YES",
		"Automatically update repository catalogues prior to package updates",
	},
	[PKG_CONFIG_NAMESERVER] = {
		PKG_CONFIG_STRING,
		"NAMESERVER",
		NULL,
		"Use this nameserver when looking up addresses",
	},
	[PKG_CONFIG_EVENT_PIPE] = {
		PKG_CONFIG_STRING,
		"EVENT_PIPE",
		NULL,
		"Send all events to the specified fifo or Unix socket",
	},
	[PKG_CONFIG_FETCH_TIMEOUT] = {
		PKG_CONFIG_INTEGER,
		"FETCH_TIMEOUT",
		"30",
		NULL,
	},
	[PKG_CONFIG_UNSET_TIMESTAMP] = {
		PKG_CONFIG_BOOL,
		"UNSET_TIMESTAMP",
		"NO",
		NULL,
	},
	[PKG_CONFIG_SSH_RESTRICT_DIR] = {
		PKG_CONFIG_STRING,
		"SSH_RESTRICT_DIR",
		NULL,
		"Directory the ssh subsystem will be restricted to",
	},
	[PKG_CONFIG_ENV] = {
		PKG_CONFIG_KVLIST,
		"PKG_ENV",
		NULL,
		"Environment variables pkg will use",
	},
	[PKG_CONFIG_DISABLE_MTREE] = {
		PKG_CONFIG_BOOL,
		"DISABLE_MTREE",
		"NO",
		"Experimental: disable MTREE processing on pkg installation",
	},
	[PKG_CONFIG_SSH_ARGS] = {
		PKG_CONFIG_STRING,
		"PKG_SSH_ARGS",
		NULL,
		"Extras arguments to pass to ssh(1)",
	},
	[PKG_CONFIG_DEBUG_LEVEL] = {
		PKG_CONFIG_INTEGER,
		"DEBUG_LEVEL",
		"0",
		"Level for debug messages",
	},
	[PKG_CONFIG_ALIAS] = {
		PKG_CONFIG_KVLIST,
		"ALIAS",
		NULL,
		"Command aliases",
	},
	[PKG_CONFIG_CUDF_SOLVER] = {
		PKG_CONFIG_STRING,
		"CUDF_SOLVER",
		NULL,
		"Experimental: tells pkg to use an external CUDF solver",
	},
	[PKG_CONFIG_SAT_SOLVER] = {
		PKG_CONFIG_STRING,
		"SAT_SOLVER",
		NULL,
		"Experimental: tells pkg to use an external SAT solver",
	},
};

static bool parsed = false;
static size_t c_size = NELEM(c);

static void		 pkg_config_kv_free(struct pkg_config_kv *);
static void		 pkg_config_value_free(struct pkg_config_value *);
static struct pkg_repo	*pkg_repo_new(const char *name, const char *url);

static void
connect_evpipe(const char *evpipe) {
	struct stat st;
	struct sockaddr_un sock;
	int flag = O_WRONLY;

	if (stat(evpipe, &st) != 0) {
		pkg_emit_error("No such event pipe: %s", evpipe);
		return;
	}

	if (!S_ISFIFO(st.st_mode) && !S_ISSOCK(st.st_mode)) {
		pkg_emit_error("%s is not a fifo or socket", evpipe);
		return;
	}

	if (S_ISFIFO(st.st_mode)) {
		flag |= O_NONBLOCK;
		if ((eventpipe = open(evpipe, flag)) == -1)
			pkg_emit_errno("open event pipe", evpipe);
		return;
	}

	if (S_ISSOCK(st.st_mode)) {
		if ((eventpipe = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
			pkg_emit_errno("Open event pipe", evpipe);
			return;
		}
		memset(&sock, 0, sizeof(struct sockaddr_un));
		sock.sun_family = AF_UNIX;
		if (strlcpy(sock.sun_path, evpipe, sizeof(sock.sun_path)) >=
		    sizeof(sock.sun_path)) {
			pkg_emit_error("Socket path too long: %s", evpipe);
			close(eventpipe);
			eventpipe = -1;
			return;
		}

		if (connect(eventpipe, (struct sockaddr *)&sock, SUN_LEN(&sock)) == -1) {
			pkg_emit_errno("Connect event pipe", evpipe);
			close(eventpipe);
			eventpipe = -1;
			return;
		}
	}

}

static void
obj_walk_array(ucl_object_t *obj, struct pkg_config *conf)
{
	struct pkg_config_value *v;
	ucl_object_t *cur;
	ucl_object_iter_t it = NULL;
	
	while ((cur = ucl_iterate_object(obj, &it, true))) {
		if (cur->type != UCL_STRING)
			continue;
		v = malloc(sizeof(struct pkg_config_value));
		v->value = strdup(ucl_object_tostring(cur));
		HASH_ADD_STR(conf->list, value, v);
	}
}

static void
obj_walk_object(ucl_object_t *obj, struct pkg_config *conf)
{
	struct pkg_config_kv *kv;
	ucl_object_t *cur;
	ucl_object_iter_t it = NULL;
	const char *key;

	while ((cur = ucl_iterate_object(obj, &it, true))) {
		if (cur->type != UCL_STRING)
			continue;
		kv = malloc(sizeof(struct pkg_config_kv));
		key = ucl_object_key(cur);
		if (key == NULL)
			continue;
		kv->key = strdup(key);
		kv->value = strdup(ucl_object_tostring(cur));
		HASH_ADD_STR(conf->kvlist, value, kv);
	}
}

void
pkg_object_walk(ucl_object_t *obj, struct pkg_config *conf_by_key)
{
	ucl_object_t *cur;
	ucl_object_iter_t it = NULL;
	struct sbuf *b = sbuf_new_auto();
	struct pkg_config *conf;
	const char *key;
	size_t i;

	while ((cur = ucl_iterate_object(obj, &it, true))) {
		sbuf_clear(b);
		key = ucl_object_key(cur);
		if (key == NULL)
			continue;
		for (i = 0; i < strlen(key); i++)
			sbuf_putc(b, toupper(key[i]));
		sbuf_finish(b);

		HASH_FIND(hhkey, conf_by_key, sbuf_data(b), (size_t)sbuf_len(b), conf);
		if (conf != NULL) {
			switch (conf->type) {
			case PKG_CONFIG_STRING:
				if (cur->type != UCL_STRING) {
					pkg_emit_error("Expecting a string for key %s,"
					    " ignoring...", key);
					continue;
				}
				if (!conf->fromenv) {
					free(conf->string);
					conf->string = strdup(ucl_object_tostring(cur));
				}
				break;
			case PKG_CONFIG_INTEGER:
				if (cur->type != UCL_INT) {
					pkg_emit_error("Expecting an integer for key %s,"
					    " ignoring...", key);
					continue;
				}
				if (!conf->fromenv)
					conf->integer = ucl_object_toint(cur);
				break;
			case PKG_CONFIG_BOOL:
				if (cur->type != UCL_BOOLEAN) {
					pkg_emit_error("Expecting a boolean for key %s,"
					    " ignoring...", key);
					continue;
				}

				if (!conf->fromenv)
					conf->boolean = ucl_object_toboolean(cur);
				break;
			case PKG_CONFIG_LIST:
				if (cur->type != UCL_ARRAY) {
					pkg_emit_error("Expecting a list for key %s,"
					    " ignoring...", key);
					continue;
				}
				if (!conf->fromenv) {
					HASH_FREE(conf->list, pkg_config_value, pkg_config_value_free);
					conf->list = NULL;
					obj_walk_array(cur, conf);
				}
				break;
			case PKG_CONFIG_KVLIST:
				if (cur->type != UCL_OBJECT) {
					pkg_emit_error("Expecting a mapping for key %s,"
					    " ignoring...", key);
					continue;
				}
				if (!conf->fromenv) {
					HASH_FREE(conf->kvlist, pkg_config_kv, pkg_config_kv_free);
					conf->kvlist = NULL;
					obj_walk_object(cur, conf);
				}
				break;
			}
		}
	}
	sbuf_delete(b);
}

static char *
subst_packagesite_str(const char *oldstr)
{
	const char *myarch;
	struct sbuf *newval;
	const char *variable_string;
	char *res;

	variable_string = strstr(oldstr, ABI_VAR_STRING);
	if (variable_string == NULL)
		return strdup(oldstr);

	newval = sbuf_new_auto();
	sbuf_bcat(newval, oldstr, variable_string - oldstr);
	pkg_config_string(PKG_CONFIG_ABI, &myarch);
	sbuf_cat(newval, myarch);
	sbuf_cat(newval, variable_string + strlen(ABI_VAR_STRING));
	sbuf_finish(newval);

	res = strdup(sbuf_data(newval));
	sbuf_free(newval);

	return res;
}

int
pkg_initialized(void)
{
	return (parsed);
}

int
pkg_config_desc(pkg_config_key key, const char **desc)
{
	struct pkg_config *conf;

	if (parsed != true) {
		pkg_emit_error("pkg_init() must be called before pkg_config_desc()");
		return (EPKG_FATAL);
	}

	HASH_FIND_INT(config, &key, conf);
	if (conf == NULL)
		*desc = NULL;
	else
		*desc = conf->desc;

	return (EPKG_OK);
}

int
pkg_config_string(pkg_config_key key, const char **val)
{
	struct pkg_config *conf;

	if (parsed != true) {
		pkg_emit_error("pkg_init() must be called before pkg_config_string()");
		return (EPKG_FATAL);
	}

	HASH_FIND_INT(config, &key, conf);
	if (conf == NULL)
		*val = NULL;
	else
		*val = conf->string;

	return (EPKG_OK);
}

int
pkg_config_int64(pkg_config_key key, int64_t *val)
{
	struct pkg_config *conf;

	if (parsed != true) {
		pkg_emit_error("pkg_init() must be called before pkg_config_int64()");
		return (EPKG_FATAL);
	}

	HASH_FIND_INT(config, &key, conf);
	if (conf == NULL)
		return (EPKG_FATAL);

	*val = conf->integer;

	return (EPKG_OK);
}

int
pkg_config_bool(pkg_config_key key, bool *val)
{
	struct pkg_config *conf;

	if (parsed != true) {
		pkg_emit_error("pkg_init() must be called before pkg_config_bool()");
		return (EPKG_FATAL);
	}

	HASH_FIND_INT(config, &key, conf);
	if (conf == NULL)
		return (EPKG_FATAL);

	*val = conf->boolean;

	return (EPKG_OK);
}

int
pkg_config_kvlist(pkg_config_key key, struct pkg_config_kv **kv)
{
	struct pkg_config *conf;

	if (parsed != true) {
		pkg_emit_error("pkg_init() must be called before pkg_config_kvlist()");
		return (EPKG_FATAL);
	}

	HASH_FIND_INT(config, &key, conf);
	if (conf == NULL)
		return (EPKG_FATAL);

	if (conf->type != PKG_CONFIG_KVLIST) {
		pkg_emit_error("this config entry is not a \"key: value\" list");
		return (EPKG_FATAL);
	}

	HASH_NEXT(conf->kvlist, (*kv));
}

int
pkg_config_list(pkg_config_key key, struct pkg_config_value **v)
{
	struct pkg_config *conf;

	if (parsed != true) {
		pkg_emit_error("pkg_init() must be called before pkg_config_list()");
		return (EPKG_FATAL);
	}

	HASH_FIND_INT(config, &key, conf);
	if (conf == NULL)
		return (EPKG_FATAL);

	if (conf->type != PKG_CONFIG_LIST) {
		pkg_emit_error("this config entry is not a list");
		return (EPKG_FATAL);
	}

	HASH_NEXT(conf->list, (*v));
}

const char *
pkg_config_value(struct pkg_config_value *v)
{
	assert(v != NULL);

	return (v->value);
}


const char *
pkg_config_kv_get(struct pkg_config_kv *kv, pkg_config_kv_t type)
{
	assert(kv != NULL);

	switch (type) {
	case PKG_CONFIG_KV_KEY:
		return (kv->key);
		break;
	case PKG_CONFIG_KV_VALUE:
		return (kv->value);
		break;
	}
	return (NULL);
}

static void
disable_plugins_if_static(void)
{
	void *dlh;
	struct pkg_config *conf;
	pkg_config_key k = PKG_CONFIG_ENABLE_PLUGINS;

	HASH_FIND_INT(config, &k, conf);

	if (conf == NULL)
		return;

	if (!conf->boolean)
		return;

	dlh = dlopen(0, 0);

	/* if dlh is NULL then we are in static binary */
	if (dlh == NULL)
		conf->boolean = false;
	else
		dlclose(dlh);

	return;
}

static void
add_repo(ucl_object_t *obj, struct pkg_repo *r, const char *rname)
{
	ucl_object_t *cur, *tmp = NULL;
	ucl_object_iter_t it = NULL;
	bool enable = true;
	const char *url = NULL, *pubkey = NULL, *mirror_type = NULL;
	const char *signature_type = NULL, *fingerprints = NULL;
	const char *key;

	pkg_debug(1, "PkgConfig: parsing repository object %s", rname);
	while ((cur = ucl_iterate_object(obj, &it, true))) {
		key = ucl_object_key(cur);
		if (key == NULL)
			continue;

		if (strcasecmp(key, "url") == 0) {
			if (cur->type != UCL_STRING) {
				pkg_emit_error("Expecting a string for the "
				    "'%s' key of the '%s' repo",
				    key, rname);
				return;
			}
			url = ucl_object_tostring(cur);
		} else if (strcasecmp(key, "pubkey") == 0) {
			if (cur->type != UCL_STRING) {
				pkg_emit_error("Expecting a string for the "
				    "'%s' key of the '%s' repo",
				    key, rname);
				return;
			}
			pubkey = ucl_object_tostring(cur);
		} else if (strcasecmp(key, "enabled") == 0) {
			if (cur->type == UCL_STRING)
				tmp = ucl_object_fromstring_common(ucl_object_tostring(cur),
				    strlen(ucl_object_tostring(cur)), UCL_STRING_PARSE_BOOLEAN);
			if (cur->type != UCL_BOOLEAN && (tmp != NULL && tmp->type != UCL_BOOLEAN)) {
				pkg_emit_error("Expecting a boolean for the "
				    "'%s' key of the '%s' repo",
				    key, rname);
				if (tmp != NULL)
					ucl_object_free(tmp);
				return;
			}
			if (tmp != NULL)
				pkg_emit_error("Warning: expecting a boolean for the '%s' key of the '%s' repo, "
				    " the value has been correctly converted, please consider fixing", key, rname);
			enable = ucl_object_toboolean(tmp != NULL ? tmp : cur);
			if (tmp != NULL)
				ucl_object_free(tmp);
		} else if (strcasecmp(key, "mirror_type") == 0) {
			if (cur->type != UCL_STRING) {
				pkg_emit_error("Expecting a string for the "
				    "'%s' key of the '%s' repo",
				    key, rname);
				return;
			}
			mirror_type = ucl_object_tostring(cur);
		} else if (strcasecmp(key, "signature_type") == 0) {
			if (cur->type != UCL_STRING) {
				pkg_emit_error("Expecting a string for the "
				    "'%s' key of the '%s' repo",
				    key, rname);
				return;
			}
			signature_type = ucl_object_tostring(cur);
		} else if (strcasecmp(key, "fingerprints") == 0) {
			if (cur->type != UCL_STRING) {
				pkg_emit_error("Expecting a string for the "
				    "'%s' key of the '%s' repo",
				    key, rname);
				return;
			}
			fingerprints = ucl_object_tostring(cur);
		}
	}

	if (r == NULL && url == NULL) {
		pkg_debug(1, "No repo and no url for %s", rname);
		return;
	}

	if (r == NULL)
		r = pkg_repo_new(rname, url);

	if (signature_type != NULL) {
		if (strcasecmp(signature_type, "pubkey") == 0)
			r->signature_type = SIG_PUBKEY;
		else if (strcasecmp(signature_type, "fingerprints") == 0)
			r->signature_type = SIG_FINGERPRINT;
		else
			r->signature_type = SIG_NONE;
	}


	if (fingerprints != NULL) {
		free(r->fingerprints);
		r->fingerprints = strdup(fingerprints);
	}

	if (pubkey != NULL) {
		free(r->pubkey);
		r->pubkey = strdup(pubkey);
	}

	r->enable = enable;

	if (mirror_type != NULL) {
		if (strcasecmp(mirror_type, "srv") == 0)
			r->mirror_type = SRV;
		else if (strcasecmp(mirror_type, "http") == 0)
			r->mirror_type = HTTP;
		else
			r->mirror_type = NOMIRROR;
	}
}

static void
walk_repo_obj(ucl_object_t *obj, const char *file)
{
	ucl_object_t *cur;
	ucl_object_iter_t it = NULL;
	struct pkg_repo *r;
	const char *key;

	while ((cur = ucl_iterate_object(obj, &it, true))) {
		key = ucl_object_key(cur);
		if (key == NULL)
			continue;
		pkg_debug(1, "PkgConfig: parsing key '%s'", key);
		r = pkg_repo_find_ident(key);
		if (r != NULL)
			pkg_debug(1, "PkgConfig: overwriting repository %s", key);
		if (cur->type == UCL_OBJECT)
			add_repo(cur, r, key);
		else
			pkg_emit_error("Ignoring bad configuration entry in %s: %s",
			    file, ucl_object_emit(cur, UCL_EMIT_YAML));
	}
}

static void
load_repo_file(const char *repofile)
{
	struct ucl_parser *p;
	ucl_object_t *obj = NULL;
	bool fallback = false;
	const char *myarch;

	p = ucl_parser_new(0);

	pkg_config_string(PKG_CONFIG_ABI, &myarch);
	ucl_parser_register_variable (p, "ABI", myarch);

	pkg_debug(1, "PKgConfig: loading %s", repofile);
	if (!ucl_parser_add_file(p, repofile)) {
		pkg_emit_error("Error parsing: %s: %s", repofile,
		    ucl_parser_get_error(p));
		if (errno == ENOENT) {
			ucl_parser_free(p);
			return;
		}
		fallback = true;
	}

	if (fallback) {
		obj = yaml_to_ucl(repofile, NULL, 0);
		if (obj == NULL)
			return;
	}

	if (fallback) {
		pkg_emit_error("%s file is using a deprecated format. "
		    "Please replace it with the following:\n"
		    "====== BEGIN %s ======\n"
		    "%s"
		    "\n====== END %s ======\n",
		    repofile, repofile,
		    ucl_object_emit(obj, UCL_EMIT_YAML),
		    repofile);
	}

	obj = ucl_parser_get_object(p);

	if (obj->type == UCL_OBJECT)
		walk_repo_obj(obj, repofile);

	ucl_object_free(obj);
}

static void
load_repo_files(const char *repodir)
{
	struct dirent *ent;
	DIR *d;
	char *p;
	size_t n;
	char path[MAXPATHLEN];

	if ((d = opendir(repodir)) == NULL)
		return;

	pkg_debug(1, "PkgConfig: loading repositories in %s", repodir);
	while ((ent = readdir(d))) {
		if ((n = strlen(ent->d_name)) <= 5)
			continue;
		p = &ent->d_name[n - 5];
		if (strcmp(p, ".conf") == 0) {
			snprintf(path, sizeof(path), "%s%s%s",
			    repodir,
			    repodir[strlen(repodir) - 1] == '/' ? "" : "/",
			    ent->d_name);
			load_repo_file(path);
		}
	}
	closedir(d);
}

static void
load_repositories(const char *repodir)
{
	struct pkg_repo *r;
	struct pkg_config_value *v;
	const char *url, *pub, *mirror_type;

	pkg_config_string(PKG_CONFIG_REPO, &url);
	pkg_config_string(PKG_CONFIG_REPOKEY, &pub);
	pkg_config_string(PKG_CONFIG_MIRRORS, &mirror_type);

	if (url != NULL) {
		pkg_emit_error("PACKAGESITE in pkg.conf is deprecated. "
		    "Please create a repository configuration file");
		r = pkg_repo_new("packagesite", url);
		if (pub != NULL) {
			r->pubkey = strdup(pub);
			r->signature_type = SIG_PUBKEY;
		}
		if (mirror_type != NULL) {
			if (strcasecmp(mirror_type, "srv") == 0)
				r->mirror_type = SRV;
			else if (strcasecmp(mirror_type, "http") == 0)
				r->mirror_type = HTTP;
		}
	}

	if (repodir != NULL) {
		load_repo_files(repodir);
		return;
	}

	v = NULL;
	while (pkg_config_list(PKG_CONFIG_REPOS_DIR, &v) == EPKG_OK)
		load_repo_files(pkg_config_value(v));
}


int
pkg_init(const char *path, const char *reposdir)
{
	struct ucl_parser *p = NULL;
	size_t i;
	const char *val = NULL;
	const char *buf, *walk, *value, *key;
	const char *errstr = NULL;
	const char *evkey = NULL;
	const char *nsname = NULL;
	const char *evpipe = NULL;
	struct pkg_config *conf;
	struct pkg_config_value *v;
	struct pkg_config_kv *kv;
	ucl_object_t *obj = NULL, *cur;
	ucl_object_iter_t it = NULL;
	bool fallback = false;

	pkg_get_myarch(myabi, BUFSIZ);
	if (parsed != false) {
		pkg_emit_error("pkg_init() must only be called once");
		return (EPKG_FATAL);
	}

	for (i = 0; i < c_size; i++) {
		conf = malloc(sizeof(struct pkg_config));
		conf->id = i;
		conf->key = c[i].key;
		conf->type = c[i].type;
		conf->desc = c[i].desc;
		conf->fromenv = false;
		val = getenv(c[i].key);

		switch (c[i].type) {
		case PKG_CONFIG_STRING:
			if (val != NULL) {
				if (strcmp(c[i].key, "PACKAGESITE") == 0)
					conf->string = subst_packagesite_str(val);
				else
					conf->string = strdup(val);
				conf->fromenv = true;
			}
			else if (c[i].def != NULL)
				conf->string = strdup(c[i].def);
			else
				conf->string = NULL;
			break;
		case PKG_CONFIG_INTEGER:
			if (val == NULL)
				val = c[i].def;
			else
				conf->fromenv = true;
			conf->integer = strtonum(val, 0, INT64_MAX, &errstr);
			if (errstr != NULL) {
				pkg_emit_error("Unable to convert %s to int64: %s",
				    val, errstr);
				free(conf);
				return (EPKG_FATAL);
			}
			break;
		case PKG_CONFIG_BOOL:
			if (val == NULL)
				val = c[i].def;
			else
				conf->fromenv = true;
			if (val != NULL && (
			    strcmp(val, "1") == 0 ||
			    strcasecmp(val, "yes") == 0 ||
			    strcasecmp(val, "true") == 0 ||
			    strcasecmp(val, "on") == 0)) {
				conf->boolean = true;
			} else {
				conf->boolean = false;
			}
			break;
		case PKG_CONFIG_KVLIST:
			conf->kvlist = NULL;
			if (val == NULL)
				val = c[i].def;
			else
				conf->fromenv = false;
			if (val != NULL) {
				walk = buf = val;
				while ((buf = strchr(buf, ',')) != NULL) {
					key = walk;
					value = walk;
					while (*value != ',') {
						if (*value == '=')
							break;
						value++;
					}
					if (value == buf || (value - key) == 0) {
						pkg_emit_error("Malformed Key/Value for %s", c[i].key);
						pkg_config_kv_free(conf->kvlist);
						conf->kvlist = NULL;
						break;
					}
					kv = malloc(sizeof(struct pkg_config_kv));
					kv->key = strndup(key, value - key);
					kv->value = strndup(value + 1, buf - value -1);
					HASH_ADD_STR(conf->kvlist, value, kv);
					buf++;
					walk = buf;
				}
				key = walk;
				value = walk;
				while (*value != '\0') {
					if (*value == '=')
						break;
					value++;
				}
				if (*value == '\0' || (value - key) == 0) {
					pkg_emit_error("Malformed Key/Value for %s: %s", c[i].key, val);
					pkg_config_kv_free(conf->kvlist);
					conf->kvlist = NULL;
					break;
				}
				kv = malloc(sizeof(struct pkg_config_kv));
				kv->key = strndup(key, value - key);
				kv->value = strdup(value + 1);
				HASH_ADD_STR(conf->kvlist, value, kv);
			}
			break;
		case PKG_CONFIG_LIST:
			conf->list = NULL;
			if (val == NULL)
				val = c[i].def;
			else
				conf->fromenv = true;
			if (val != NULL) {
				walk = buf = val;
				while ((buf = strchr(buf, ',')) != NULL) {
					v = malloc(sizeof(struct pkg_config_value));
					v->value = strndup(walk, buf - walk);
					HASH_ADD_STR(conf->list, value, v);
					buf++;
					walk = buf;
				}
				v = malloc(sizeof(struct pkg_config_value));
				v->value = strdup(walk);
				HASH_ADD_STR(conf->list, value, v);
			}
			break;
		}

		HASH_ADD_INT(config, id, conf);
		HASH_ADD_KEYPTR(hhkey, config_by_key, conf->key,
		    strlen(conf->key), conf);
	}

	if (path == NULL)
		path = PREFIX"/etc/pkg.conf";

	p = ucl_parser_new(0);

	errno = 0;
	if (!ucl_parser_add_file(p, path)) {
		if (errno == ENOENT)
			goto parsed;
		fallback = true;
	}

	if (!fallback) {
		/* Validate the first level of the configuration */
		obj = ucl_parser_get_object(p);
		if (obj->type == UCL_OBJECT) {
			while ((cur = ucl_iterate_object(obj, &it, true))) {
				key = ucl_object_key(cur);
				if (key == NULL)
					continue;
				if (strcasecmp(key, "REPOS_DIR") == 0 &&
				    cur->type != UCL_ARRAY)
					fallback = true;
				else if (strcasecmp(key, "PKG_ENV") == 0 &&
				    cur->type != UCL_OBJECT)
					fallback = true;
				else if (strcasecmp(key, "ALIAS") == 0 &&
				    cur->type != UCL_OBJECT)
					fallback = true;
				if (fallback)
					break;
			}
		} else {
			fallback = true;
		}
	}

	if (fallback) {
		if (obj != NULL)
			ucl_object_free(obj);
		obj = yaml_to_ucl(path, NULL, 0);
		if (obj == NULL)
			return (EPKG_FATAL);
	}

	if (fallback) {
		pkg_emit_error("Your pkg.conf file is in deprecated format you "
		    "should convert it to the following format:\n"
		    "====== BEGIN pkg.conf ======\n"
		    "%s"
		    "\n====== END pkg.conf ======\n",
		    ucl_object_emit(obj, UCL_EMIT_YAML));
	}

	if (obj->type == UCL_OBJECT)
		pkg_object_walk(obj, config_by_key);

parsed:
	disable_plugins_if_static();

	parsed = true;
	ucl_object_free(obj);
	ucl_parser_free(p);

	pkg_debug(1, "%s", "pkg initialized");

	/* Start the event pipe */
	pkg_config_string(PKG_CONFIG_EVENT_PIPE, &evpipe);
	if (evpipe != NULL)
		connect_evpipe(evpipe);

	kv = NULL;
	while (pkg_config_kvlist(PKG_CONFIG_ENV, &kv) == EPKG_OK) {
		evkey = pkg_config_kv_get(kv, PKG_CONFIG_KV_KEY);
		if (evkey != NULL && evkey[0] != '\0')
			setenv(evkey, pkg_config_kv_get(kv, PKG_CONFIG_KV_VALUE), 1);
	}

	/* load the repositories */
	load_repositories(reposdir);

	setenv("HTTP_USER_AGENT", "pkg/"PKGVERSION, 1);

	/* bypass resolv.conf with specified NAMESERVER if any */
	pkg_config_string(PKG_CONFIG_NAMESERVER, &nsname);
	if (nsname != NULL)
		set_nameserver(nsname);

	return (EPKG_OK);
}

struct pkg_config *
pkg_config_lookup(const char *name)
{
	struct pkg_config *conf;

	if (name == NULL)
		return (NULL);

	HASH_FIND(hhkey, config_by_key, name, strlen(name), conf);

	return (conf);
}

static void
pkg_config_kv_free(struct pkg_config_kv *k)
{
	if (k == NULL)
		return;

	free(k->key);
	free(k->value);
	free(k);
}

static void
pkg_config_value_free(struct pkg_config_value *v)
{
	if (v == NULL)
		return;

	free(v->value);
	free(v);
}

static void
pkg_config_free(struct pkg_config *conf)
{
	if (conf == NULL)
		return;

	if (conf->type == PKG_CONFIG_STRING)
		free(conf->string);
	else if (conf->type == PKG_CONFIG_KVLIST)
		HASH_FREE(conf->kvlist, pkg_config_kv, pkg_config_kv_free);
	else if (conf->type == PKG_CONFIG_LIST)
		HASH_FREE(conf->list, pkg_config_value, pkg_config_value_free);

	free(conf);
}

int
pkg_config_id(struct pkg_config *conf)
{
	return (conf->id);
}

int
pkg_config_type(struct pkg_config *conf)
{
	return (conf->type);
}

const char *
pkg_config_name(struct pkg_config *conf)
{
	return (conf->key);
}

int
pkg_configs(struct pkg_config **conf)
{
	HASH_NEXT(config, (*conf));
}

static struct pkg_repo *
pkg_repo_new(const char *name, const char *url)
{
	struct pkg_repo *r;

	r = calloc(1, sizeof(struct pkg_repo));
	r->type = REPO_BINARY_PKGS;
	r->update = repo_update_binary_pkgs;
	r->url = subst_packagesite_str(url);
	r->signature_type = SIG_NONE;
	r->mirror_type = NOMIRROR;
	r->enable = true;
	asprintf(&r->name, REPO_NAME_PREFIX"%s", name);
	HASH_ADD_KEYPTR(hh, repos, r->name, strlen(r->name), r);

	return (r);
}

static void
pkg_repo_free(struct pkg_repo *r)
{
	free(r->url);
	free(r->name);
	free(r->pubkey);
	if (r->ssh != NULL) {
		fprintf(r->ssh, "quit\n");
		pclose(r->ssh);
	}
	free(r);
}

void
pkg_shutdown(void)
{
	if (!parsed) {
		pkg_emit_error("pkg_shutdown() must be called after pkg_init()");
		_exit(EX_SOFTWARE);
		/* NOTREACHED */
	}

	HASH_FREE(config, pkg_config, pkg_config_free);
	HASH_FREE(repos, pkg_repo, pkg_repo_free);

	config_by_key = NULL;

	parsed = false;

	return;
}

int
pkg_repos_total_count(void)
{

	return (HASH_COUNT(repos));
}

int
pkg_repos_activated_count(void)
{
	struct pkg_repo *r = NULL;
	int count = 0;

	for (r = repos; r != NULL; r = r->hh.next) {
		if (r->enable)
			count++;
	}

	return (count);
}

int
pkg_repos(struct pkg_repo **r)
{
	HASH_NEXT(repos, (*r));
}

const char *
pkg_repo_url(struct pkg_repo *r)
{
	return (r->url);
}

/* The repo identifier from pkg.conf(5): without the 'repo-' prefix */
const char *
pkg_repo_ident(struct pkg_repo *r)
{
	return (r->name + strlen(REPO_NAME_PREFIX));
}

/* Ditto: The repo identifier from pkg.conf(5): without the 'repo-' prefix */
const char *
pkg_repo_ident_from_name(const char *repo_name)
{
	return (repo_name + strlen(REPO_NAME_PREFIX));
}

/* The basename of the sqlite DB file and the database name */
const char *
pkg_repo_name(struct pkg_repo *r)
{
	return (r->name);
}

const char *
pkg_repo_key(struct pkg_repo *r)
{
	return (r->pubkey);
}

const char *
pkg_repo_fingerprints(struct pkg_repo *r)
{
	return (r->fingerprints);
}

signature_t
pkg_repo_signature_type(struct pkg_repo *r)
{
	return (r->signature_type);
}

bool
pkg_repo_enabled(struct pkg_repo *r)
{
	return (r->enable);
}

mirror_t
pkg_repo_mirror_type(struct pkg_repo *r)
{
	return (r->mirror_type);
}

/* Locate the repo by the identifying tag from pkg.conf(5) */
struct pkg_repo *
pkg_repo_find_ident(const char *repoident)
{
	struct pkg_repo *r;
	char *name;

	asprintf(&name, REPO_NAME_PREFIX"%s", repoident);
	if (name == NULL)
		return (NULL);	/* Out of memory */

	r = pkg_repo_find_name(name);
	free(name);

	return (r);
}


/* Locate the repo by the file basename / database name */
struct pkg_repo *
pkg_repo_find_name(const char *reponame)
{
	struct pkg_repo *r;

	HASH_FIND_STR(repos, reponame, r);
	return (r);
}
