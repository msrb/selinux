/* Author: Jason Tang	  <jtang@tresys.com>
 *         Christopher Ashworth <cashworth@tresys.com>
 *
 * Copyright (C) 2004-2006 Tresys Technology, LLC
 * Copyright (C) 2005 Red Hat, Inc.
 * 
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <sepol/module.h>
#include <sepol/handle.h>
#include <sepol/cil/cil.h>
#include <selinux/selinux.h>

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdio_ext.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>
#include <errno.h>
#include <dirent.h>

#include "user_internal.h"
#include "seuser_internal.h"
#include "port_internal.h"
#include "iface_internal.h"
#include "boolean_internal.h"
#include "fcontext_internal.h"
#include "node_internal.h"
#include "genhomedircon.h"

#include "debug.h"
#include "handle.h"
#include "modules.h"
#include "direct_api.h"
#include "semanage_store.h"
#include "database_policydb.h"
#include "policy.h"
#include <sys/mman.h>
#include <sys/wait.h>

#define PIPE_READ 0
#define PIPE_WRITE 1

static void semanage_direct_destroy(semanage_handle_t * sh);
static int semanage_direct_disconnect(semanage_handle_t * sh);
static int semanage_direct_begintrans(semanage_handle_t * sh);
static int semanage_direct_commit(semanage_handle_t * sh);
static int semanage_direct_install(semanage_handle_t * sh, char *data,
				   size_t data_len, const char *module_name, const char *lang_ext);
static int semanage_direct_install_file(semanage_handle_t * sh, const char *module_name);
static int semanage_direct_remove(semanage_handle_t * sh, char *module_name);
static int semanage_direct_list(semanage_handle_t * sh,
				semanage_module_info_t ** modinfo,
				int *num_modules);
static int semanage_direct_get_enabled(semanage_handle_t *sh,
				       const semanage_module_key_t *modkey,
				       int *enabled);
static int semanage_direct_set_enabled(semanage_handle_t *sh,
				       const semanage_module_key_t *modkey,
				       int enabled);

static int semanage_direct_get_module_info(semanage_handle_t *sh,
					   const semanage_module_key_t *modkey,
					   semanage_module_info_t **modinfo);

static int semanage_direct_list_all(semanage_handle_t *sh,
				    semanage_module_info_t **modinfo,
				    int *num_modules);

static int semanage_direct_install_info(semanage_handle_t *sh,
					const semanage_module_info_t *modinfo,
					char *data,
					size_t data_len);

static int semanage_direct_remove_key(semanage_handle_t *sh,
				      const semanage_module_key_t *modkey);

static struct semanage_policy_table direct_funcs = {
	.get_serial = semanage_direct_get_serial,
	.destroy = semanage_direct_destroy,
	.disconnect = semanage_direct_disconnect,
	.begin_trans = semanage_direct_begintrans,
	.commit = semanage_direct_commit,
	.install = semanage_direct_install,
	.install_file = semanage_direct_install_file,
	.remove = semanage_direct_remove,
	.list = semanage_direct_list,
	.get_enabled = semanage_direct_get_enabled,
	.set_enabled = semanage_direct_set_enabled,
	.get_module_info = semanage_direct_get_module_info,
	.list_all = semanage_direct_list_all,
	.install_info = semanage_direct_install_info,
	.remove_key = semanage_direct_remove_key,
};

int semanage_direct_is_managed(semanage_handle_t * sh)
{
	if (semanage_check_init(sh, sh->conf->store_root_path))
		goto err;

	if (semanage_access_check(sh) < 0)
		return 0;

	return 1;

      err:
	ERR(sh, "could not check whether policy is managed");
	return STATUS_ERR;
}

/* Check that the module store exists, creating it if necessary.
 */
int semanage_direct_connect(semanage_handle_t * sh)
{
	const char *path;

	if (semanage_check_init(sh, sh->conf->store_root_path))
		goto err;

	if (sh->create_store)
		if (semanage_create_store(sh, 1))
			goto err;

	if (semanage_access_check(sh) < SEMANAGE_CAN_READ)
		goto err;

	sh->u.direct.translock_file_fd = -1;
	sh->u.direct.activelock_file_fd = -1;

	/* set up function pointers */
	sh->funcs = &direct_funcs;

	/* Object databases: local modifications */
	if (user_base_file_dbase_init(sh,
				      semanage_path(SEMANAGE_ACTIVE,
						    SEMANAGE_USERS_BASE_LOCAL),
				      semanage_path(SEMANAGE_TMP,
						    SEMANAGE_USERS_BASE_LOCAL),
				      semanage_user_base_dbase_local(sh)) < 0)
		goto err;

	if (user_extra_file_dbase_init(sh,
				       semanage_path(SEMANAGE_ACTIVE,
						     SEMANAGE_USERS_EXTRA_LOCAL),
				       semanage_path(SEMANAGE_TMP,
						     SEMANAGE_USERS_EXTRA_LOCAL),
				       semanage_user_extra_dbase_local(sh)) < 0)
		goto err;

	if (user_join_dbase_init(sh,
				 semanage_user_base_dbase_local(sh),
				 semanage_user_extra_dbase_local(sh),
				 semanage_user_dbase_local(sh)) < 0)
		goto err;

	if (port_file_dbase_init(sh,
				 semanage_path(SEMANAGE_ACTIVE,
					       SEMANAGE_PORTS_LOCAL),
				 semanage_path(SEMANAGE_TMP,
					       SEMANAGE_PORTS_LOCAL),
				 semanage_port_dbase_local(sh)) < 0)
		goto err;

	if (iface_file_dbase_init(sh,
				  semanage_path(SEMANAGE_ACTIVE,
						SEMANAGE_INTERFACES_LOCAL),
				  semanage_path(SEMANAGE_TMP,
						SEMANAGE_INTERFACES_LOCAL),
				  semanage_iface_dbase_local(sh)) < 0)
		goto err;

	if (bool_file_dbase_init(sh,
				 semanage_path(SEMANAGE_ACTIVE,
					       SEMANAGE_BOOLEANS_LOCAL),
				 semanage_path(SEMANAGE_TMP,
					       SEMANAGE_BOOLEANS_LOCAL),
				 semanage_bool_dbase_local(sh)) < 0)
		goto err;

	if (fcontext_file_dbase_init(sh,
				     semanage_path(SEMANAGE_ACTIVE, SEMANAGE_STORE_FC_LOCAL),
				     semanage_path(SEMANAGE_TMP, SEMANAGE_STORE_FC_LOCAL),
				     semanage_fcontext_dbase_local(sh)) < 0)
		goto err;

	if (seuser_file_dbase_init(sh,
				   semanage_path(SEMANAGE_ACTIVE,
						 SEMANAGE_SEUSERS_LOCAL),
				   semanage_path(SEMANAGE_TMP,
						 SEMANAGE_SEUSERS_LOCAL),
				   semanage_seuser_dbase_local(sh)) < 0)
		goto err;

	if (node_file_dbase_init(sh,
				 semanage_path(SEMANAGE_ACTIVE,
					       SEMANAGE_NODES_LOCAL),
				 semanage_path(SEMANAGE_TMP,
					       SEMANAGE_NODES_LOCAL),
				 semanage_node_dbase_local(sh)) < 0)
		goto err;

	/* Object databases: local modifications + policy */
	if (user_base_policydb_dbase_init(sh,
					  semanage_user_base_dbase_policy(sh)) <
	    0)
		goto err;

	if (user_extra_file_dbase_init(sh,
				       semanage_path(SEMANAGE_ACTIVE,
						     SEMANAGE_USERS_EXTRA),
				       semanage_path(SEMANAGE_TMP,
						     SEMANAGE_USERS_EXTRA),
				       semanage_user_extra_dbase_policy(sh)) <
	    0)
		goto err;

	if (user_join_dbase_init(sh,
				 semanage_user_base_dbase_policy(sh),
				 semanage_user_extra_dbase_policy(sh),
				 semanage_user_dbase_policy(sh)) < 0)
		goto err;

	if (port_policydb_dbase_init(sh, semanage_port_dbase_policy(sh)) < 0)
		goto err;

	if (iface_policydb_dbase_init(sh, semanage_iface_dbase_policy(sh)) < 0)
		goto err;

	if (bool_policydb_dbase_init(sh, semanage_bool_dbase_policy(sh)) < 0)
		goto err;

	if (fcontext_file_dbase_init(sh,
				     semanage_path(SEMANAGE_ACTIVE, SEMANAGE_STORE_FC),
				     semanage_path(SEMANAGE_TMP, SEMANAGE_STORE_FC),
				     semanage_fcontext_dbase_policy(sh)) < 0)
		goto err;

	if (seuser_file_dbase_init(sh,
				   semanage_path(SEMANAGE_ACTIVE, SEMANAGE_STORE_SEUSERS),
				   semanage_path(SEMANAGE_TMP, SEMANAGE_STORE_SEUSERS),
				   semanage_seuser_dbase_policy(sh)) < 0)
		goto err;

	if (node_policydb_dbase_init(sh, semanage_node_dbase_policy(sh)) < 0)
		goto err;

	/* Active kernel policy */
	if (bool_activedb_dbase_init(sh, semanage_bool_dbase_active(sh)) < 0)
		goto err;

	/* set the disable dontaudit value */
	path = semanage_path(SEMANAGE_ACTIVE, SEMANAGE_DISABLE_DONTAUDIT);
	if (access(path, F_OK) == 0)
		sepol_set_disable_dontaudit(sh->sepolh, 1);
	else
		sepol_set_disable_dontaudit(sh->sepolh, 0);

	return STATUS_SUCCESS;

      err:
	ERR(sh, "could not establish direct connection");
	return STATUS_ERR;
}

static void semanage_direct_destroy(semanage_handle_t * sh
					__attribute__ ((unused)))
{
	/* do nothing */
	sh = NULL;
}

static int semanage_direct_disconnect(semanage_handle_t * sh)
{
	/* destroy transaction */
	if (sh->is_in_transaction) {
		/* destroy sandbox */
		if (semanage_remove_directory
		    (semanage_path(SEMANAGE_TMP, SEMANAGE_TOPLEVEL)) < 0) {
			ERR(sh, "Could not cleanly remove sandbox %s.",
			    semanage_path(SEMANAGE_TMP, SEMANAGE_TOPLEVEL));
			return -1;
		}
		if (semanage_remove_directory
		    (semanage_final_path(SEMANAGE_FINAL_TMP,
					 SEMANAGE_FINAL_TOPLEVEL)) < 0) {
			ERR(sh, "Could not cleanly remove tmp %s.",
			    semanage_final_path(SEMANAGE_FINAL_TMP,
						SEMANAGE_FINAL_TOPLEVEL));
			return -1;
		}
		semanage_release_trans_lock(sh);
	}

	/* Release object databases: local modifications */
	user_base_file_dbase_release(semanage_user_base_dbase_local(sh));
	user_extra_file_dbase_release(semanage_user_extra_dbase_local(sh));
	user_join_dbase_release(semanage_user_dbase_local(sh));
	port_file_dbase_release(semanage_port_dbase_local(sh));
	iface_file_dbase_release(semanage_iface_dbase_local(sh));
	bool_file_dbase_release(semanage_bool_dbase_local(sh));
	fcontext_file_dbase_release(semanage_fcontext_dbase_local(sh));
	seuser_file_dbase_release(semanage_seuser_dbase_local(sh));
	node_file_dbase_release(semanage_node_dbase_local(sh));

	/* Release object databases: local modifications + policy */
	user_base_policydb_dbase_release(semanage_user_base_dbase_policy(sh));
	user_extra_file_dbase_release(semanage_user_extra_dbase_policy(sh));
	user_join_dbase_release(semanage_user_dbase_policy(sh));
	port_policydb_dbase_release(semanage_port_dbase_policy(sh));
	iface_policydb_dbase_release(semanage_iface_dbase_policy(sh));
	bool_policydb_dbase_release(semanage_bool_dbase_policy(sh));
	fcontext_file_dbase_release(semanage_fcontext_dbase_policy(sh));
	seuser_file_dbase_release(semanage_seuser_dbase_policy(sh));
	node_policydb_dbase_release(semanage_node_dbase_policy(sh));

	/* Release object databases: active kernel policy */
	bool_activedb_dbase_release(semanage_bool_dbase_active(sh));

	return 0;
}

static int semanage_direct_begintrans(semanage_handle_t * sh)
{

	if (semanage_access_check(sh) != SEMANAGE_CAN_WRITE) {
		return -1;
	}
	if (semanage_get_trans_lock(sh) < 0) {
		return -1;
	}
	if ((semanage_make_sandbox(sh)) < 0) {
		return -1;
	}
	if ((semanage_make_final(sh)) < 0) {
		return -1;
	}
	return 0;
}

/********************* utility functions *********************/

#include <stdlib.h>
#include <bzlib.h>
#include <string.h>
#include <sys/sendfile.h>

/* bzip() a data to a file, returning the total number of compressed bytes
 * in the file.  Returns -1 if file could not be compressed. */
static ssize_t bzip(semanage_handle_t *sh, const char *filename, char *data,
			size_t num_bytes)
{
	BZFILE* b;
	size_t  size = 1<<16;
	int     bzerror;
	size_t  total = 0;
	size_t len = 0;
	FILE *f;

	if ((f = fopen(filename, "wb")) == NULL) {
		return -1;
	}

	if (!sh->conf->bzip_blocksize) {
		if (fwrite(data, 1, num_bytes, f) < num_bytes) {
			fclose(f);
			return -1;
		}
		fclose(f);
		return num_bytes;
	}

	b = BZ2_bzWriteOpen( &bzerror, f, sh->conf->bzip_blocksize, 0, 0);
	if (bzerror != BZ_OK) {
		BZ2_bzWriteClose ( &bzerror, b, 1, 0, 0 );
		return -1;
	}
	
	while ( num_bytes > total ) {
		if (num_bytes - total > size) {
			len = size;
		} else {
			len = num_bytes - total;
		}
		BZ2_bzWrite ( &bzerror, b, &data[total], len );
		if (bzerror == BZ_IO_ERROR) { 
			BZ2_bzWriteClose ( &bzerror, b, 1, 0, 0 );
			return -1;
		}
		total += len;
	}

	BZ2_bzWriteClose ( &bzerror, b, 0, 0, 0 );
	fclose(f);
	if (bzerror == BZ_IO_ERROR) {
		return -1;
	}
	return total;
}

#define BZ2_MAGICSTR "BZh"
#define BZ2_MAGICLEN (sizeof(BZ2_MAGICSTR)-1)

/* bunzip() a file to '*data', returning the total number of uncompressed bytes
 * in the file.  Returns -1 if file could not be decompressed. */
ssize_t bunzip(semanage_handle_t *sh, FILE *f, char **data)
{
	BZFILE* b = NULL;
	size_t  nBuf;
	char*   buf = NULL;
	size_t  size = 1<<18;
	size_t  bufsize = size;
	int     bzerror;
	size_t  total=0;
	char*   uncompress = NULL;
	char*   tmpalloc = NULL;
	int     ret = -1;

	buf = malloc(bufsize);
	if (buf == NULL) {
		ERR(sh, "Failure allocating memory.");
		goto exit;
	}

	/* Check if the file is bzipped */
	bzerror = fread(buf, 1, BZ2_MAGICLEN, f);
	rewind(f);
	if ((bzerror != BZ2_MAGICLEN) || memcmp(buf, BZ2_MAGICSTR, BZ2_MAGICLEN)) {
		goto exit;
	}

	b = BZ2_bzReadOpen ( &bzerror, f, 0, sh->conf->bzip_small, NULL, 0 );
	if ( bzerror != BZ_OK ) {
		ERR(sh, "Failure opening bz2 archive.");
		goto exit;
	}

	uncompress = malloc(size);
	if (uncompress == NULL) {
		ERR(sh, "Failure allocating memory.");
		goto exit;
	}

	while ( bzerror == BZ_OK) {
		nBuf = BZ2_bzRead ( &bzerror, b, buf, bufsize);
		if (( bzerror == BZ_OK ) || ( bzerror == BZ_STREAM_END )) {
			if (total + nBuf > size) {
				size *= 2;
				tmpalloc = realloc(uncompress, size);
				if (tmpalloc == NULL) {
					ERR(sh, "Failure allocating memory.");
					goto exit;
				}
				uncompress = tmpalloc;
			}
			memcpy(&uncompress[total], buf, nBuf);
			total += nBuf;
		}
	}
	if ( bzerror != BZ_STREAM_END ) {
		ERR(sh, "Failure reading bz2 archive.");
		goto exit;
	}

	ret = total;
	*data = uncompress;

exit:
	BZ2_bzReadClose ( &bzerror, b );
	free(buf);
	if ( ret < 0 ) {
		free(uncompress);
	}
	return ret;
}

/* mmap() a file to '*data',
 *  If the file is bzip compressed map_file will uncompress 
 * the file into '*data'.
 * Returns the total number of bytes in memory .
 * Returns -1 if file could not be opened or mapped. */
static ssize_t map_file(semanage_handle_t *sh, int fd, char **data,
			int *compressed)
{
	ssize_t size = -1;
	char *uncompress;
	if ((size = bunzip(sh, fdopen(fd, "r"), &uncompress)) > 0) {
		*data = mmap(0, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, 0, 0);
		if (*data == MAP_FAILED) {
			free(uncompress);
			return -1;
		} else {
			memcpy(*data, uncompress, size);
		}
		free(uncompress);
		*compressed = 1;
	} else {
		struct stat sb;
		if (fstat(fd, &sb) == -1 ||
		    (*data = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0)) ==
		    MAP_FAILED) {
			size = -1;
		} else {
			size = sb.st_size;
		}
		*compressed = 0;
	} 

	return size;
}

/* Writes a block of data to a file.  Returns 0 on success, -1 on
 * error. */
static int write_file(semanage_handle_t * sh,
		      const char *filename, char *data, size_t num_bytes)
{
	int out;

	if ((out =
	     open(filename, O_WRONLY | O_CREAT | O_TRUNC,
		  S_IRUSR | S_IWUSR)) == -1) {
		ERR(sh, "Could not open %s for writing.", filename);
		return -1;
	}
	if (write(out, data, num_bytes) == -1) {
		ERR(sh, "Error while writing to %s.", filename);
		close(out);
		return -1;
	}
	close(out);
	return 0;
}

static int semanage_direct_update_user_extra(semanage_handle_t * sh, cil_db_t *cildb)
{
	const char *ofilename = NULL;
	int retval = -1;
	char *data = NULL;
	size_t size = 0;

	dbase_config_t *pusers_extra = semanage_user_extra_dbase_policy(sh);

	retval = cil_userprefixes_to_string(cildb, &data, &size);
	if (retval != SEPOL_OK) {
		goto cleanup;
	}

	if (size > 0) {
		ofilename = semanage_path(SEMANAGE_TMP, SEMANAGE_USERS_EXTRA);
		if (ofilename == NULL) {
			return retval;
		}
		retval = write_file(sh, ofilename, data, size);
		if (retval < 0)
			return retval;

		pusers_extra->dtable->drop_cache(pusers_extra->dbase);
		
	} else {
		retval =  pusers_extra->dtable->clear(sh, pusers_extra->dbase);
	}

cleanup:
	free(data);

	return retval;
}

static int semanage_direct_update_seuser(semanage_handle_t * sh, cil_db_t *cildb)
{
	const char *ofilename = NULL;
	int retval = -1;
	char *data = NULL;
	size_t size = 0;

	dbase_config_t *pseusers = semanage_seuser_dbase_policy(sh);

	retval = cil_selinuxusers_to_string(cildb, &data, &size);
	if (retval != SEPOL_OK) {
		goto cleanup;
	}

	if (size > 0) {
		ofilename = semanage_path(SEMANAGE_TMP, SEMANAGE_STORE_SEUSERS);
		if (ofilename == NULL) {
			return -1;
		}
		retval = write_file(sh, ofilename, data, size);

		pseusers->dtable->drop_cache(pseusers->dbase);
	} else {
		retval = pseusers->dtable->clear(sh, pseusers->dbase);
	}

cleanup:
	free(data);

	return retval;
}

static int read_from_pipe_to_data(semanage_handle_t *sh, size_t initial_len, int fd, char **out_data_read, size_t *out_read_len)
{
	size_t max_len = initial_len;
	size_t read_len = 0;
	size_t data_read_len = 0;
	char *data_read = NULL;

	if (max_len <= 0) {
		max_len = 1;
	}
	data_read = malloc(max_len * sizeof(*data_read));
	if (data_read == NULL) {
		ERR(sh, "Failed to malloc, out of memory.\n");
		return -1;
	}

	while ((read_len = read(fd, data_read + data_read_len, max_len - data_read_len)) > 0) {
		data_read_len += read_len;
		if (data_read_len == max_len) {
			max_len *= 2;
			data_read = realloc(data_read, max_len);
			if (data_read == NULL) {
				ERR(sh, "Failed to realloc, out of memory.\n");
				return -1;
			}
		}
	}

	*out_read_len = data_read_len;
	*out_data_read = data_read;

	return 0;
}

static int semanage_pipe_data(semanage_handle_t *sh, char *path, char *in_data, size_t in_data_len, char **out_data, size_t *out_data_len, char **err_data, size_t *err_data_len)
{
	int input_fd[2];
	int output_fd[2];
	int err_fd[2];
	pid_t pid;
	char *data_read = NULL;
	char *err_data_read = NULL;
	int retval;
	int status = 0;
	size_t initial_len;
	size_t data_read_len = 0;
	size_t err_data_read_len = 0;
	struct sigaction old_signal;
	struct sigaction new_signal;
	new_signal.sa_handler = SIG_IGN;
	sigemptyset(&new_signal.sa_mask);
	new_signal.sa_flags = 0;
	/* This is needed in case the read end of input_fd is closed causing a SIGPIPE signal to be sent.
	 * If SIGPIPE is not caught, the signal will cause semanage to terminate immediately. The sigaction below
	 * creates a new_signal that ignores SIGPIPE allowing the write to exit cleanly.
	 *
	 * Another sigaction is called in cleanup to restore the original behavior when a SIGPIPE is received.
	 */
	sigaction(SIGPIPE, &new_signal, &old_signal);

	retval = pipe(input_fd);
	if (retval == -1) {
		ERR(sh, "Unable to create pipe for input pipe: %s\n", strerror(errno));
		goto cleanup;
	}
	retval = pipe(output_fd);
	if (retval == -1) {
		ERR(sh, "Unable to create pipe for output pipe: %s\n", strerror(errno));
		goto cleanup;
	}
	retval = pipe(err_fd);
	if (retval == -1) {
		ERR(sh, "Unable to create pipe for error pipe: %s\n", strerror(errno));
		goto cleanup;
	}

	pid = fork();
	if (pid == -1) {
		ERR(sh, "Unable to fork from parent: %s.", strerror(errno));
		retval = -1;
		goto cleanup;
	} else if (pid == 0) {
		retval = dup2(input_fd[PIPE_READ], STDIN_FILENO);
		if (retval == -1) {
			ERR(sh, "Unable to dup2 input pipe: %s\n", strerror(errno));
			goto cleanup;
		}
		retval = dup2(output_fd[PIPE_WRITE], STDOUT_FILENO);
		if (retval == -1) {
			ERR(sh, "Unable to dup2 output pipe: %s\n", strerror(errno));
			goto cleanup;
		}
		retval = dup2(err_fd[PIPE_WRITE], STDERR_FILENO);
		if (retval == -1) {
			ERR(sh, "Unable to dup2 error pipe: %s\n", strerror(errno));
			goto cleanup;
		}

		retval = close(input_fd[PIPE_WRITE]);
		if (retval == -1) {
			ERR(sh, "Unable to close input pipe: %s\n", strerror(errno));
			goto cleanup;
		}
		retval = close(output_fd[PIPE_READ]);
		if (retval == -1) {
			ERR(sh, "Unable to close output pipe: %s\n", strerror(errno));
			goto cleanup;
		}
		retval = close(err_fd[PIPE_READ]);
		if (retval == -1) {
			ERR(sh, "Unable to close error pipe: %s\n", strerror(errno));
			goto cleanup;
		}
		retval = execl(path, path, NULL);
		if (retval == -1) {
			ERR(sh, "Unable to execute %s : %s\n", path, strerror(errno));
			_exit(EXIT_FAILURE);
		}
	} else {
		retval = close(input_fd[PIPE_READ]);
		input_fd[PIPE_READ] = -1;
		if (retval == -1) {
			ERR(sh, "Unable to close read end of input pipe: %s\n", strerror(errno));
			goto cleanup;
		}

		retval = close(output_fd[PIPE_WRITE]);
		output_fd[PIPE_WRITE] = -1;
		if (retval == -1) {
			ERR(sh, "Unable to close write end of output pipe: %s\n", strerror(errno));
			goto cleanup;
		}

		retval = close(err_fd[PIPE_WRITE]);
		err_fd[PIPE_WRITE] = -1;
		if (retval == -1) {
			ERR(sh, "Unable to close write end of error pipe: %s\n", strerror(errno));
			goto cleanup;
		}

		retval = write(input_fd[PIPE_WRITE], in_data, in_data_len);
		if (retval == -1) {
			ERR(sh, "Failed to write data to input pipe: %s\n", strerror(errno));
			goto cleanup;
		}
		retval = close(input_fd[PIPE_WRITE]);
		input_fd[PIPE_WRITE] = -1;
		if (retval == -1) {
			ERR(sh, "Unable to close write end of input pipe: %s\n", strerror(errno));
			goto cleanup;
		}

		initial_len = 1 << 17;
		retval = read_from_pipe_to_data(sh, initial_len, output_fd[PIPE_READ], &data_read, &data_read_len);
		if (retval != 0) {
			goto cleanup;
		}
		retval = close(output_fd[PIPE_READ]);
		output_fd[PIPE_READ] = -1;
		if (retval == -1) {
			ERR(sh, "Unable to close read end of output pipe: %s\n", strerror(errno));
			goto cleanup;
		}

		initial_len = 1 << 9;
		retval = read_from_pipe_to_data(sh, initial_len, err_fd[PIPE_READ], &err_data_read, &err_data_read_len);
		if (retval != 0) {
			goto cleanup;
		}
		retval = close(err_fd[PIPE_READ]);
		err_fd[PIPE_READ] = -1;
		if (retval == -1) {
			ERR(sh, "Unable to close read end of error pipe: %s\n", strerror(errno));
			goto cleanup;
		}

		if (waitpid(pid, &status, 0) == -1 || !WIFEXITED(status)) {
			ERR(sh, "Child process %s did not exit cleanly.", path);
			retval = -1;
			goto cleanup;
		}
		if (WEXITSTATUS(status) != 0) {
			ERR(sh, "Child process %s failed with code: %d.", path, WEXITSTATUS(status));
			retval = -1;
			goto cleanup;
		}
	}

	retval = 0;

cleanup:
	sigaction(SIGPIPE, &old_signal, NULL);

	if (data_read != NULL) {
		*out_data = data_read;
		*out_data_len = data_read_len;
	}

	if (err_data_read != NULL) {
		*err_data = err_data_read;
		*err_data_len = err_data_read_len;
	}

	if (output_fd[PIPE_READ] != -1) {
		close(output_fd[PIPE_READ]);
	}
	if (output_fd[PIPE_WRITE] != -1) {
		close(output_fd[PIPE_WRITE]);
	}
	if (err_fd[PIPE_READ] != -1) {
		close(err_fd[PIPE_READ]);
	}
	if (err_fd[PIPE_WRITE] != -1) {
		close(err_fd[PIPE_WRITE]);
	}
	if (input_fd[PIPE_READ] != -1) {
		close(input_fd[PIPE_READ]);
	}
	if (input_fd[PIPE_WRITE] != -1) {
		close(input_fd[PIPE_WRITE]);
	}

	return retval;
}

static int semanage_direct_write_langext(semanage_handle_t *sh,
				char *lang_ext,
				const semanage_module_info_t *modinfo)
{
	int ret = -1;
	char fn[PATH_MAX];
	FILE *fp = NULL;

	ret = semanage_module_get_path(sh,
			modinfo,
			SEMANAGE_MODULE_PATH_LANG_EXT,
			fn,
			sizeof(fn));
	if (ret != 0) {
		goto cleanup;
	}

	fp = fopen(fn, "w");
	if (fp == NULL) {
		ERR(sh, "Unable to open %s module ext file.", modinfo->name);
		ret = -1;
		goto cleanup;
	}

	if (fputs(lang_ext, fp) < 0) {
		ERR(sh, "Unable to write %s module ext file.", modinfo->name);
		ret = -1;
		goto cleanup;
	}

	if (fclose(fp) != 0) {
		ERR(sh, "Unable to close %s module ext file.", modinfo->name);
		ret = -1;
		goto cleanup;
	}

	fp = NULL;

	ret = 0;

cleanup:
	if (fp != NULL) fclose(fp);

	return ret;
}

static int semanage_compile_hll(semanage_handle_t *sh,
				semanage_module_info_t *modinfos,
				int num_modinfos)
{
	char cil_path[PATH_MAX];
	char hll_path[PATH_MAX];
	char *compiler_path = NULL;
	char *cil_data = NULL;
	char *err_data = NULL;
	char *hll_data = NULL;
	char *start = NULL;
	char *end = NULL;
	ssize_t hll_data_len = 0;
	ssize_t bzip_status;
	int status = 0;
	int i, compressed;
	int in_fd = -1;
	size_t cil_data_len;
	size_t err_data_len;

	assert(sh);
	assert(modinfos);

	for (i = 0; i < num_modinfos; i++) {
		if (!strcasecmp(modinfos[i].lang_ext, "cil")) {
			continue;
		}

		status = semanage_module_get_path(
				sh,
				&modinfos[i],
				SEMANAGE_MODULE_PATH_CIL,
				cil_path,
				sizeof(cil_path));
		if (status != 0) {
			goto cleanup;
		}

		if (semanage_get_ignore_module_cache(sh) == 0 &&
			access(cil_path, F_OK) == 0) {
			continue;
		}

		status = semanage_get_hll_compiler_path(sh, modinfos[i].lang_ext, &compiler_path);
		if (status != 0) {
			goto cleanup;
		}

		status = semanage_module_get_path(
				sh,
				&modinfos[i],
				SEMANAGE_MODULE_PATH_HLL,
				hll_path,
				sizeof(hll_path));
		if (status != 0) {
			goto cleanup;
		}

		if ((in_fd = open(hll_path, O_RDONLY)) == -1) {
			ERR(sh, "Unable to open %s\n", hll_path);
			status = -1;
			goto cleanup;
		}

		if ((hll_data_len = map_file(sh, in_fd, &hll_data, &compressed)) <= 0) {
			ERR(sh, "Unable to read file %s\n", hll_path);
			status = -1;
			goto cleanup;
		}

		if (in_fd >= 0) close(in_fd);
		in_fd = -1;

		status = semanage_pipe_data(sh, compiler_path, hll_data, (size_t)hll_data_len, &cil_data, &cil_data_len, &err_data, &err_data_len);
		if (err_data_len > 0) {
			for (start = end = err_data; end < err_data + err_data_len; end++) {
				if (*end == '\n') {
					fprintf(stderr, "%s: ", modinfos[i].name);
					fwrite(start, 1, end - start + 1, stderr);
					start = end + 1;
				}
			}

			if (end != start) {
				fprintf(stderr, "%s: ", modinfos[i].name);
				fwrite(start, 1, end - start, stderr);
				fprintf(stderr, "\n");
			}
		}
		if (status != 0) {
			goto cleanup;
		}

		if (sh->conf->remove_hll == 1) {
			status = unlink(hll_path);
			if (status != 0) {
				ERR(sh, "Error while removing HLL file %s: %s", hll_path, strerror(errno));
				goto cleanup;
			}

			status = semanage_direct_write_langext(sh, "cil", &modinfos[i]);
			if (status != 0) {
				goto cleanup;
			}
		}

		bzip_status = bzip(sh, cil_path, cil_data, cil_data_len);
		if (bzip_status == -1) {
			ERR(sh, "Failed to bzip %s\n", cil_path);
			status = -1;
			goto cleanup;
		}

		if (hll_data_len > 0) munmap(hll_data, hll_data_len);
		hll_data_len = 0;

		free(cil_data);
		free(err_data);
		free(compiler_path);
		cil_data = NULL;
		err_data = NULL;
		compiler_path = NULL;
		cil_data_len = 0;
		err_data_len = 0;
	}

	status = 0;

cleanup:
	if (hll_data_len > 0) munmap(hll_data, hll_data_len);
	if (in_fd >= 0) close(in_fd);
	free(cil_data);
	free(err_data);
	free(compiler_path);
	return status;
}

/********************* direct API functions ********************/

/* Commits all changes in sandbox to the actual kernel policy.
 * Returns commit number on success, -1 on error.
 */
static int semanage_direct_commit(semanage_handle_t * sh)
{
	char **mod_filenames = NULL;
	char *fc_buffer = NULL;
	size_t fc_buffer_len = 0;
	const char *ofilename = NULL;
	const char *path;
	int retval = -1, num_modinfos = 0, i, missing_policy_kern = 0,
		missing_seusers = 0, missing_fc = 0, missing = 0;
	sepol_policydb_t *out = NULL;
	struct cil_db *cildb = NULL;
	semanage_module_info_t *modinfos = NULL;

	/* Declare some variables */
	int modified = 0, fcontexts_modified, ports_modified,
	    seusers_modified, users_extra_modified, dontaudit_modified,
	    preserve_tunables_modified, bools_modified,
		disable_dontaudit, preserve_tunables;
	dbase_config_t *users = semanage_user_dbase_local(sh);
	dbase_config_t *users_base = semanage_user_base_dbase_local(sh);
	dbase_config_t *pusers_base = semanage_user_base_dbase_policy(sh);
	dbase_config_t *users_extra = semanage_user_extra_dbase_local(sh);
	dbase_config_t *ports = semanage_port_dbase_local(sh);
	dbase_config_t *pports = semanage_port_dbase_policy(sh);
	dbase_config_t *bools = semanage_bool_dbase_local(sh);
	dbase_config_t *pbools = semanage_bool_dbase_policy(sh);
	dbase_config_t *ifaces = semanage_iface_dbase_local(sh);
	dbase_config_t *pifaces = semanage_iface_dbase_policy(sh);
	dbase_config_t *nodes = semanage_node_dbase_local(sh);
	dbase_config_t *pnodes = semanage_node_dbase_policy(sh);
	dbase_config_t *fcontexts = semanage_fcontext_dbase_local(sh);
	dbase_config_t *pfcontexts = semanage_fcontext_dbase_policy(sh);
	dbase_config_t *seusers = semanage_seuser_dbase_local(sh);

	/* Create or remove the disable_dontaudit flag file. */
	path = semanage_path(SEMANAGE_TMP, SEMANAGE_DISABLE_DONTAUDIT);
	if (access(path, F_OK) == 0)
		dontaudit_modified = !(sepol_get_disable_dontaudit(sh->sepolh) == 1);
	else
		dontaudit_modified = (sepol_get_disable_dontaudit(sh->sepolh) == 1);
	if (sepol_get_disable_dontaudit(sh->sepolh) == 1) {
		FILE *touch;
		touch = fopen(path, "w");
		if (touch != NULL) {
			if (fclose(touch) != 0) {
				ERR(sh, "Error attempting to create disable_dontaudit flag.");
				goto cleanup;
			}
		} else {
			ERR(sh, "Error attempting to create disable_dontaudit flag.");
			goto cleanup;
		}
	} else {
		if (remove(path) == -1 && errno != ENOENT) {
			ERR(sh, "Error removing the disable_dontaudit flag.");
			goto cleanup;
		}
	}

	/* Create or remove the preserve_tunables flag file. */
	path = semanage_path(SEMANAGE_TMP, SEMANAGE_PRESERVE_TUNABLES);
	if (access(path, F_OK) == 0)
		preserve_tunables_modified = !(sepol_get_preserve_tunables(sh->sepolh) == 1);
	else
		preserve_tunables_modified = (sepol_get_preserve_tunables(sh->sepolh) == 1);
	if (sepol_get_preserve_tunables(sh->sepolh) == 1) {
		FILE *touch;
		touch = fopen(path, "w");
		if (touch != NULL) {
			if (fclose(touch) != 0) {
				ERR(sh, "Error attempting to create preserve_tunable flag.");
				goto cleanup;
			}
		} else {
			ERR(sh, "Error attempting to create preserve_tunable flag.");
			goto cleanup;
		}
	} else {
		if (remove(path) == -1 && errno != ENOENT) {
			ERR(sh, "Error removing the preserve_tunables flag.");
			goto cleanup;
		}
	}

	/* Before we do anything else, flush the join to its component parts.
	 * This *does not* flush to disk automatically */
	if (users->dtable->is_modified(users->dbase)) {
		retval = users->dtable->flush(sh, users->dbase);
		if (retval < 0)
			goto cleanup;
	}

	/* Decide if anything was modified */
	fcontexts_modified = fcontexts->dtable->is_modified(fcontexts->dbase);
	seusers_modified = seusers->dtable->is_modified(seusers->dbase);
	users_extra_modified =
	    users_extra->dtable->is_modified(users_extra->dbase);
	ports_modified = ports->dtable->is_modified(ports->dbase);
	bools_modified = bools->dtable->is_modified(bools->dbase);

	modified = sh->modules_modified;
	modified |= seusers_modified;
	modified |= users_extra_modified;
	modified |= ports_modified;
	modified |= users->dtable->is_modified(users_base->dbase);
	modified |= ifaces->dtable->is_modified(ifaces->dbase);
	modified |= nodes->dtable->is_modified(nodes->dbase);
	modified |= dontaudit_modified;
	modified |= preserve_tunables_modified;

	/* This is for systems that have already migrated with an older version
	 * of semanage_migrate_store. The older version did not copy policy.kern so
	 * the policy binary must be rebuilt here.
	 */
	if (!sh->do_rebuild && !modified) {
		path = semanage_path(SEMANAGE_TMP, SEMANAGE_STORE_KERNEL);

		if (access(path, F_OK) != 0) {
			missing_policy_kern = 1;
		}

		path = semanage_path(SEMANAGE_TMP, SEMANAGE_STORE_FC);

		if (access(path, F_OK) != 0) {
			missing_fc = 1;
		}

		path = semanage_path(SEMANAGE_TMP, SEMANAGE_STORE_SEUSERS);

		if (access(path, F_OK) != 0) {
			missing_seusers = 1;
		}
	}

	missing |= missing_policy_kern;
	missing |= missing_fc;
	missing |= missing_seusers;

	/* If there were policy changes, or explicitly requested, rebuild the policy */
	if (sh->do_rebuild || modified || missing) {
		/* =================== Module expansion =============== */

		retval = semanage_get_active_modules(sh, &modinfos, &num_modinfos);
		if (retval < 0) {
			goto cleanup;
		}

		if (num_modinfos == 0) {
			goto cleanup;
		}

		retval = semanage_compile_hll(sh, modinfos, num_modinfos);
		if (retval < 0) {
			ERR(sh, "Failed to compile hll files into cil files.\n");
			goto cleanup;
		}

		retval = semanage_get_cil_paths(sh, modinfos, num_modinfos, &mod_filenames);
		if (retval < 0)
			goto cleanup;

		retval = semanage_verify_modules(sh, mod_filenames, num_modinfos);
		if (retval < 0)
			goto cleanup;

		cil_db_init(&cildb);

		disable_dontaudit = sepol_get_disable_dontaudit(sh->sepolh);
		preserve_tunables = sepol_get_preserve_tunables(sh->sepolh);
		cil_set_disable_dontaudit(cildb, disable_dontaudit);
		cil_set_disable_neverallow(cildb, !(sh->conf->expand_check));
		cil_set_preserve_tunables(cildb, preserve_tunables);
		cil_set_target_platform(cildb, sh->conf->target_platform);
		cil_set_policy_version(cildb, sh->conf->policyvers);

		if (sh->conf->handle_unknown != -1) {
			cil_set_handle_unknown(cildb, sh->conf->handle_unknown);
		}

		retval = semanage_load_files(sh, cildb, mod_filenames, num_modinfos);
		if (retval < 0) {
			goto cleanup;
		}

		retval = cil_compile(cildb);
		if (retval < 0)
			goto cleanup;

		retval = cil_build_policydb(cildb, &out);
		if (retval < 0)
			goto cleanup;

		/* File Contexts */
		retval = cil_filecons_to_string(cildb, &fc_buffer, &fc_buffer_len);
		if (retval < 0)
			goto cleanup;

		/* Write the contexts (including template contexts) to a single file. */
		ofilename = semanage_path(SEMANAGE_TMP, SEMANAGE_FC_TMPL);
		if (ofilename == NULL) {
			retval = -1;
			goto cleanup;
		}
		retval = write_file(sh, ofilename, fc_buffer, fc_buffer_len);
		if (retval < 0)
			goto cleanup;

		/* Split complete and template file contexts into their separate files. */
		retval = semanage_split_fc(sh);
		if (retval < 0)
			goto cleanup;

		pfcontexts->dtable->drop_cache(pfcontexts->dbase);

		/* SEUsers */
		retval = semanage_direct_update_seuser(sh, cildb);
		if (retval < 0)
			goto cleanup;

		/* User Extra */
		retval = semanage_direct_update_user_extra(sh, cildb);
		if (retval < 0)
			goto cleanup;

		cil_db_destroy(&cildb);
	
	} else {
		/* Load already linked policy */
		retval = sepol_policydb_create(&out);
		if (retval < 0)
			goto cleanup;

		retval = semanage_read_policydb(sh, out);
		if (retval < 0)
			goto cleanup;
	}

	if (sh->do_rebuild || modified || bools_modified) {
		/* Attach to policy databases that work with a policydb. */
		dbase_policydb_attach((dbase_policydb_t *) pusers_base->dbase, out);
		dbase_policydb_attach((dbase_policydb_t *) pports->dbase, out);
		dbase_policydb_attach((dbase_policydb_t *) pifaces->dbase, out);
		dbase_policydb_attach((dbase_policydb_t *) pbools->dbase, out);
		dbase_policydb_attach((dbase_policydb_t *) pnodes->dbase, out);

		/* ============= Apply changes, and verify  =============== */

		retval = semanage_base_merge_components(sh);
		if (retval < 0)
			goto cleanup;

		retval = semanage_write_policydb(sh, out);
		if (retval < 0)
			goto cleanup;

		retval = semanage_verify_kernel(sh);
		if (retval < 0)
			goto cleanup;
	} else {
		retval = semanage_base_merge_components(sh);
		if (retval < 0)
			goto cleanup;
	}

	/* ======= Post-process: Validate non-policydb components ===== */

	/* Validate local modifications to file contexts.
	 * Note: those are still cached, even though they've been 
	 * merged into the main file_contexts. We won't check the 
	 * large file_contexts - checked at compile time */
	if (sh->do_rebuild || modified || fcontexts_modified) {
		retval = semanage_fcontext_validate_local(sh, out);
		if (retval < 0)
			goto cleanup;
	}

	/* Validate local seusers against policy */
	if (sh->do_rebuild || modified || seusers_modified) {
		retval = semanage_seuser_validate_local(sh, out);
		if (retval < 0)
			goto cleanup;
	}

	/* Validate local ports for overlap */
	if (sh->do_rebuild || modified || ports_modified) {
		retval = semanage_port_validate_local(sh);
		if (retval < 0)
			goto cleanup;
	}

	/* ================== Write non-policydb components ========= */

	/* Commit changes to components */
	retval = semanage_commit_components(sh);
	if (retval < 0)
		goto cleanup;

	retval = semanage_copy_file(semanage_path(SEMANAGE_TMP, SEMANAGE_STORE_KERNEL),
			semanage_final_path(SEMANAGE_FINAL_TMP, SEMANAGE_KERNEL),
			sh->conf->file_mode);
	if (retval < 0) {
		goto cleanup;
	}

	path = semanage_path(SEMANAGE_TMP, SEMANAGE_STORE_FC_LOCAL);
	if (access(path, F_OK) == 0) {
		retval = semanage_copy_file(semanage_path(SEMANAGE_TMP, SEMANAGE_STORE_FC_LOCAL),
							semanage_final_path(SEMANAGE_FINAL_TMP, SEMANAGE_FC_LOCAL),
							sh->conf->file_mode);
		if (retval < 0) {
			goto cleanup;
		}
	}

	path = semanage_path(SEMANAGE_TMP, SEMANAGE_STORE_FC);
	if (access(path, F_OK) == 0) {
		retval = semanage_copy_file(semanage_path(SEMANAGE_TMP, SEMANAGE_STORE_FC),
							semanage_final_path(SEMANAGE_FINAL_TMP, SEMANAGE_FC),
							sh->conf->file_mode);
		if (retval < 0) {
			goto cleanup;
		}
	}

	path = semanage_path(SEMANAGE_TMP, SEMANAGE_STORE_SEUSERS);
	if (access(path, F_OK) == 0) {
		retval = semanage_copy_file(semanage_path(SEMANAGE_TMP, SEMANAGE_STORE_SEUSERS),
							semanage_final_path(SEMANAGE_FINAL_TMP, SEMANAGE_SEUSERS),
							sh->conf->file_mode);
		if (retval < 0) {
			goto cleanup;
		}
	}

	/* run genhomedircon if its enabled, this should be the last operation
	 * which requires the out policydb */
	if (!sh->conf->disable_genhomedircon) {
		if (out && (retval =
			semanage_genhomedircon(sh, out, sh->conf->usepasswd, sh->conf->ignoredirs)) != 0) {
			ERR(sh, "semanage_genhomedircon returned error code %d.",
			    retval);
			goto cleanup;
		}
	} else {
		WARN(sh, "WARNING: genhomedircon is disabled. \
                               See /etc/selinux/semanage.conf if you need to enable it.");
        }

	/* free out, if we don't free it before calling semanage_install_sandbox 
	 * then fork() may fail on low memory machines */
	sepol_policydb_free(out);
	out = NULL;

	/* remove files that are automatically generated and no longer needed */
	unlink(semanage_path(SEMANAGE_TMP, SEMANAGE_FC_TMPL));
	unlink(semanage_path(SEMANAGE_TMP, SEMANAGE_HOMEDIR_TMPL));
	unlink(semanage_path(SEMANAGE_TMP, SEMANAGE_USERS_EXTRA));

	if (sh->do_rebuild || modified || bools_modified || fcontexts_modified) {
		retval = semanage_install_sandbox(sh);
	}

cleanup:
	for (i = 0; i < num_modinfos; i++) {
		semanage_module_info_destroy(sh, &modinfos[i]);
	}
	free(modinfos);

	for (i = 0; mod_filenames != NULL && i < num_modinfos; i++) {
		free(mod_filenames[i]);
	}

	if (modified || bools_modified) {
		/* Detach from policydb, so it can be freed */
		dbase_policydb_detach((dbase_policydb_t *) pusers_base->dbase);
		dbase_policydb_detach((dbase_policydb_t *) pports->dbase);
		dbase_policydb_detach((dbase_policydb_t *) pifaces->dbase);
		dbase_policydb_detach((dbase_policydb_t *) pnodes->dbase);
		dbase_policydb_detach((dbase_policydb_t *) pbools->dbase);
	}

	free(mod_filenames);
	sepol_policydb_free(out);
	cil_db_destroy(&cildb);
	semanage_release_trans_lock(sh);

	free(fc_buffer);

	/* regardless if the commit was successful or not, remove the
	   sandbox if it is still there */
	semanage_remove_directory(semanage_path
				  (SEMANAGE_TMP, SEMANAGE_TOPLEVEL));
	semanage_remove_directory(semanage_final_path
				  (SEMANAGE_FINAL_TMP,
				   SEMANAGE_FINAL_TOPLEVEL));
	return retval;
}

/* Writes a module to the sandbox's module directory, overwriting any
 * previous module stored within.  Note that module data are not
 * free()d by this function; caller is responsible for deallocating it
 * if necessary.  Returns 0 on success, -1 if out of memory, -2 if the
 * data does not represent a valid module file, -3 if error while
 * writing file. */
static int semanage_direct_install(semanage_handle_t * sh,
				   char *data, size_t data_len,
				   const char *module_name, const char *lang_ext)
{
	int status = 0;
	int ret = 0;

	semanage_module_info_t modinfo;
	ret = semanage_module_info_init(sh, &modinfo);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	ret = semanage_module_info_set_priority(sh, &modinfo, sh->priority);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	ret = semanage_module_info_set_name(sh, &modinfo, module_name);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	ret = semanage_module_info_set_lang_ext(sh, &modinfo, lang_ext);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	ret = semanage_module_info_set_enabled(sh, &modinfo, -1);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	status = semanage_direct_install_info(sh, &modinfo, data, data_len);

cleanup:

	semanage_module_info_destroy(sh, &modinfo);

	return status;
}

/* Attempts to link a module to the sandbox's module directory, unlinking any
 * previous module stored within.  Returns 0 on success, -1 if out of memory, -2 if the
 * data does not represent a valid module file, -3 if error while
 * writing file. */

static int semanage_direct_install_file(semanage_handle_t * sh,
					const char *install_filename)
{

	int retval = -1;
	char *data = NULL;
	ssize_t data_len = 0;
	int compressed = 0;
	int in_fd = -1;
	char *path = NULL;
	char *filename;
	char *lang_ext = NULL;
	char *separator;

	if ((in_fd = open(install_filename, O_RDONLY)) == -1) {
		ERR(sh, "Unable to open %s: %s\n", install_filename, strerror(errno));
		retval = -1;
		goto cleanup;
	}

	if ((data_len = map_file(sh, in_fd, &data, &compressed)) <= 0) {
		ERR(sh, "Unable to read file %s\n", install_filename);
		retval = -1;
		goto cleanup;
	}

	path = strdup(install_filename);
	if (path == NULL) {
		ERR(sh, "No memory available for strdup.\n");
		retval = -1;
		goto cleanup;
	}

	filename = basename(path);

	if (compressed) {
		separator = strrchr(filename, '.');
		if (separator == NULL) {
			ERR(sh, "Compressed module does not have a valid extension.");
			retval = -1;
			goto cleanup;
		}
		*separator = '\0';
		lang_ext = separator + 1;
	}

	separator = strrchr(filename, '.');
	if (separator == NULL) {
		if (lang_ext == NULL) {
			ERR(sh, "Module does not have a valid extension.");
			retval = -1;
			goto cleanup;
		}
	} else {
		*separator = '\0';
		lang_ext = separator + 1;
	}

	retval = semanage_direct_install(sh, data, data_len, filename, lang_ext);

cleanup:
	if (in_fd != -1) {
		close(in_fd);
	}
	if (data_len > 0) munmap(data, data_len);
	free(path);

	return retval;
}

/* Removes a module from the sandbox.  Returns 0 on success, -1 if out
 * of memory, -2 if module not found or could not be removed. */
static int semanage_direct_remove(semanage_handle_t * sh, char *module_name)
{
	int status = 0;
	int ret = 0;

	semanage_module_key_t modkey;
	ret = semanage_module_key_init(sh, &modkey);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	ret = semanage_module_key_set_priority(sh, &modkey, sh->priority);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	ret = semanage_module_key_set_name(sh, &modkey, module_name);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	status = semanage_direct_remove_key(sh, &modkey);

cleanup:
	return status;
}

/* Allocate an array of module_info structures for each readable
 * module within the store.  Note that if the calling program has
 * already begun a transaction then this function will get a list of
 * modules within the sandbox.	The caller is responsible for calling
 * semanage_module_info_datum_destroy() on each element of the array
 * as well as free()ing the entire list.
 */
static int semanage_direct_list(semanage_handle_t * sh,
				semanage_module_info_t ** modinfo,
				int *num_modules)
{
	int i, retval = -1;
	*modinfo = NULL;
	*num_modules = 0;

	/* get the read lock when reading from the active
	   (non-transaction) directory */
	if (!sh->is_in_transaction)
		if (semanage_get_active_lock(sh) < 0)
			return -1;

	if (semanage_get_active_modules(sh, modinfo, num_modules) == -1) {
		goto cleanup;
	}

	if (num_modules == 0) {
		retval = semanage_direct_get_serial(sh);
		goto cleanup;
	}

	retval = semanage_direct_get_serial(sh);

      cleanup:
	if (retval < 0) {
		for (i = 0; i < *num_modules; i++) {
			semanage_module_info_destroy(sh, &(*modinfo[i]));
			modinfo[i] = NULL;
		}
		free(*modinfo);
		*modinfo = NULL;
	}

	if (!sh->is_in_transaction) {
		semanage_release_active_lock(sh);
	}
	return retval;
}

static int semanage_direct_get_enabled(semanage_handle_t *sh,
				       const semanage_module_key_t *modkey,
				       int *enabled)
{
	assert(sh);
	assert(modkey);
	assert(enabled);

	int status = 0;
	int ret = 0;

	char path[PATH_MAX];
	struct stat sb;
	semanage_module_info_t *modinfo = NULL;

	/* get module info */
	ret = semanage_module_get_module_info(
			sh,
			modkey,
			&modinfo);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	/* get disabled file path */
	ret = semanage_module_get_path(
			sh,
			modinfo,
			SEMANAGE_MODULE_PATH_DISABLED,
			path,
			sizeof(path));
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	if (stat(path, &sb) < 0) {
		*enabled = 1;
	}
	else {
		*enabled = 0;
	}

cleanup:
	semanage_module_info_destroy(sh, modinfo);
	free(modinfo);

	return status;
}

static int semanage_direct_set_enabled(semanage_handle_t *sh,
				       const semanage_module_key_t *modkey,
				       int enabled)
{
	assert(sh);
	assert(modkey);

	int status = 0;
	int ret = 0;

	char fn[PATH_MAX];
	const char *path = NULL;
	FILE *fp = NULL;
	semanage_module_info_t *modinfo = NULL;

	/* check transaction */
	if (!sh->is_in_transaction) {
		if (semanage_begin_transaction(sh) < 0) {
			status = -1;
			goto cleanup;
		}
	}

	/* validate name */
	ret = semanage_module_validate_name(modkey->name);
	if (ret != 0) {
		errno = 0;
		ERR(sh, "Name %s is invalid.", modkey->name);
		status = -1;
		goto cleanup;
	}

	/* validate enabled */
	ret = semanage_module_validate_enabled(enabled);
	if (ret != 0) {
		errno = 0;
		ERR(sh, "Enabled status %d is invalid.", enabled);
		status = -1;
		goto cleanup;
	}

	/* check for disabled path, create if missing */
	path = semanage_path(SEMANAGE_TMP, SEMANAGE_MODULES_DISABLED);

	ret = semanage_mkdir(sh, path);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	/* get module info */
	ret = semanage_module_get_module_info(
			sh,
			modkey,
			&modinfo);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	/* get module disabled file */
	ret = semanage_module_get_path(
			sh,
			modinfo,
			SEMANAGE_MODULE_PATH_DISABLED,
			fn,
			sizeof(fn));
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	switch (enabled) {
		case 0: /* disable the module */
			fp = fopen(fn, "w");

			if (fp == NULL) {
				ERR(sh,
				    "Unable to disable module %s",
				    modkey->name);
				status = -1;
				goto cleanup;
			}

			if (fclose(fp) != 0) {
				ERR(sh,
				    "Unable to close disabled file for module %s",
				    modkey->name);
				status = -1;
				goto cleanup;
			}

			fp = NULL;

			break;
		case 1: /* enable the module */
			if (unlink(fn) < 0) {
				if (errno != ENOENT) {
					ERR(sh,
					    "Unable to enable module %s",
					    modkey->name);
					status = -1;
					goto cleanup;
				}
				else {
					/* module already enabled */
					errno = 0;
				}
			}

			break;
		case -1: /* warn about ignored setting to default */
			WARN(sh,
			     "Setting module %s to 'default' state has no effect",
			     modkey->name);
			break;
	}

cleanup:
	semanage_module_info_destroy(sh, modinfo);
	free(modinfo);

	if (fp != NULL) fclose(fp);
	return status;
}

int semanage_direct_access_check(semanage_handle_t * sh)
{
	if (semanage_check_init(sh, sh->conf->store_root_path))
		return -1;

	return semanage_store_access_check();
}

int semanage_direct_mls_enabled(semanage_handle_t * sh)
{
	sepol_policydb_t *p = NULL;
	int retval;

	retval = sepol_policydb_create(&p);
	if (retval < 0)
		goto cleanup;

	retval = semanage_read_policydb(sh, p);
	if (retval < 0)
		goto cleanup;

	retval = sepol_policydb_mls_enabled(p);
cleanup:
	sepol_policydb_free(p);
	return retval;
}

static int semanage_direct_get_module_info(semanage_handle_t *sh,
					   const semanage_module_key_t *modkey,
					   semanage_module_info_t **modinfo)
{
	assert(sh);
	assert(modkey);
	assert(modinfo);

	int status = 0;
	int ret = 0;

	char fn[PATH_MAX];
	FILE *fp = NULL;
	size_t size = 0;
	struct stat sb;
	char *tmp = NULL;

	int i = 0;

	semanage_module_info_t *modinfos = NULL;
	int modinfos_len = 0;
	semanage_module_info_t *highest = NULL;

	/* check module name */
	ret = semanage_module_validate_name(modkey->name);
	if (ret < 0) {
		errno = 0;
		ERR(sh, "Name %s is invalid.", modkey->name);
		status = -1;
		goto cleanup;
	}

	/* if priority == 0, then find the highest priority available */
	if (modkey->priority == 0) {
		ret = semanage_direct_list_all(sh, &modinfos, &modinfos_len);
		if (ret != 0) {
			status = -1;
			goto cleanup;
		}

		for (i = 0; i < modinfos_len; i++) {
			ret = strcmp(modinfos[i].name, modkey->name);
			if (ret == 0) {
				highest = &modinfos[i];
				break;
			}
		}

		if (highest == NULL) {
			status = -1;
			goto cleanup;
		}

		ret = semanage_module_info_create(sh, modinfo);
		if (ret != 0) {
			status = -1;
			goto cleanup;
		}

		ret = semanage_module_info_clone(sh, highest, *modinfo);
		if (ret != 0) {
			status = -1;
		}

		/* skip to cleanup, module was found */
		goto cleanup;
	}

	/* check module priority */
	ret = semanage_module_validate_priority(modkey->priority);
	if (ret != 0) {
		errno = 0;
		ERR(sh, "Priority %d is invalid.", modkey->priority);
		status = -1;
		goto cleanup;
	}

	/* copy in key values */
	ret = semanage_module_info_create(sh, modinfo);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	ret = semanage_module_info_set_priority(sh, *modinfo, modkey->priority);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	ret = semanage_module_info_set_name(sh, *modinfo, modkey->name);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	/* lookup module ext */
	ret = semanage_module_get_path(sh,
				       *modinfo,
				       SEMANAGE_MODULE_PATH_LANG_EXT,
				       fn,
				       sizeof(fn));
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	fp = fopen(fn, "r");

	if (fp == NULL) {
		ERR(sh,
		    "Unable to open %s module lang ext file at %s.",
		    (*modinfo)->name, fn);
		status = -1;
		goto cleanup;
	}

	/* set module ext */
	if (getline(&tmp, &size, fp) < 0) {
		ERR(sh,
		    "Unable to read %s module lang ext file.",
		    (*modinfo)->name);
		status = -1;
		goto cleanup;
	}

	ret = semanage_module_info_set_lang_ext(sh, *modinfo, tmp);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}
	free(tmp);
	tmp = NULL;

	if (fclose(fp) != 0) {
		ERR(sh,
		    "Unable to close %s module lang ext file.",
		    (*modinfo)->name);
		status = -1;
		goto cleanup;
	}

	fp = NULL;

	/* lookup enabled/disabled status */
	ret = semanage_module_get_path(sh,
				       *modinfo,
				       SEMANAGE_MODULE_PATH_DISABLED,
				       fn,
				       sizeof(fn));
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	/* set enabled/disabled status */
	if (stat(fn, &sb) < 0) {
		ret = semanage_module_info_set_enabled(sh, *modinfo, 1);
		if (ret != 0) {
			status = -1;
			goto cleanup;
		}
	}
	else {
		ret = semanage_module_info_set_enabled(sh, *modinfo, 0);
		if (ret != 0) {
			status = -1;
			goto cleanup;
		}
	}

cleanup:
	free(tmp);

	if (modinfos != NULL) {
		for (i = 0; i < modinfos_len; i++) {
			semanage_module_info_destroy(sh, &modinfos[i]);
		}
		free(modinfos);
	}

	if (fp != NULL) fclose(fp);
	return status;
}

static int semanage_direct_set_module_info(semanage_handle_t *sh,
					   const semanage_module_info_t *modinfo)
{
	int status = 0;
	int ret = 0;

	char fn[PATH_MAX];
	const char *path = NULL;
	int enabled = 0;

	semanage_module_key_t modkey;
	ret = semanage_module_key_init(sh, &modkey);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	semanage_module_info_t *modinfo_tmp = NULL;

	/* check transaction */
	if (!sh->is_in_transaction) {
		if (semanage_begin_transaction(sh) < 0) {
			status = -1;
			goto cleanup;
		}
	}

	/* validate module */
	ret = semanage_module_info_validate(modinfo);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	sh->modules_modified = 1;

	/* check for modules path, create if missing */
	path = semanage_path(SEMANAGE_TMP, SEMANAGE_MODULES);

	ret = semanage_mkdir(sh, path);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	/* write priority */
	ret = semanage_module_get_path(sh,
				       modinfo,
				       SEMANAGE_MODULE_PATH_PRIORITY,
				       fn,
				       sizeof(fn));
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	ret = semanage_mkdir(sh, fn);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	/* write name */
	ret = semanage_module_get_path(sh,
				       modinfo,
				       SEMANAGE_MODULE_PATH_NAME,
				       fn,
				       sizeof(fn));
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	ret = semanage_mkdir(sh, fn);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	/* write ext */
	ret = semanage_direct_write_langext(sh, modinfo->lang_ext, modinfo);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	/* write enabled/disabled status */

	/* check for disabled path, create if missing */
	path = semanage_path(SEMANAGE_TMP, SEMANAGE_MODULES_DISABLED);

	ret = semanage_mkdir(sh, path);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	ret = semanage_module_get_path(sh,
				       modinfo,
				       SEMANAGE_MODULE_PATH_DISABLED,
				       fn,
				       sizeof(fn));
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	ret = semanage_module_key_set_name(sh, &modkey, modinfo->name);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	if (modinfo->enabled == -1) {
		/* default to enabled */
		enabled = 1;

		/* check if a module is already installed */
		ret = semanage_module_get_module_info(sh,
						      &modkey,
						      &modinfo_tmp);
		if (ret == 0) {
			/* set enabled status to current one */
			enabled = modinfo_tmp->enabled;
		}
	}
	else {
		enabled = modinfo->enabled;
	}

	ret = semanage_module_set_enabled(sh, &modkey, enabled);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

cleanup:
	semanage_module_key_destroy(sh, &modkey);

	semanage_module_info_destroy(sh, modinfo_tmp);
	free(modinfo_tmp);

	return status;
}

static int semanage_priorities_filename_select(const struct dirent *d)
{
	if (d->d_name[0] == '.' ||
	    strcmp(d->d_name, "disabled") == 0)
		return 0;
	return 1;
}

static int semanage_modules_filename_select(const struct dirent *d)
{
	if (d->d_name[0] == '.')
		return 0;
	return 1;
}

static int semanage_direct_list_all(semanage_handle_t *sh,
				    semanage_module_info_t **modinfos,
				    int *modinfos_len)
{
	assert(sh);
	assert(modinfos);
	assert(modinfos_len);

	int status = 0;
	int ret = 0;

	int i = 0;
	int j = 0;

	*modinfos = NULL;
	*modinfos_len = 0;
	void *tmp = NULL;

	const char *toplevel = NULL;

	struct dirent **priorities = NULL;
	int priorities_len = 0;
	char priority_path[PATH_MAX];

	struct dirent **modules = NULL;
	int modules_len = 0;

	uint16_t priority = 0;

	semanage_module_info_t modinfo;
	ret = semanage_module_info_init(sh, &modinfo);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	semanage_module_info_t *modinfo_tmp = NULL;

	if (sh->is_in_transaction) {
		toplevel = semanage_path(SEMANAGE_TMP, SEMANAGE_MODULES);
	} else {
		toplevel = semanage_path(SEMANAGE_ACTIVE, SEMANAGE_MODULES);
	}

	/* find priorities */
	priorities_len = scandir(toplevel,
				 &priorities,
				 semanage_priorities_filename_select,
				 versionsort);
	if (priorities_len == -1) {
		ERR(sh, "Error while scanning directory %s.", toplevel);
		status = -1;
		goto cleanup;
	}

	/* for each priority directory */
	/* loop through in reverse so that highest priority is first */
	for (i = priorities_len - 1; i >= 0; i--) {
		/* convert priority string to uint16_t */
		ret = semanage_string_to_priority(priorities[i]->d_name,
						  &priority);
		if (ret != 0) {
			status = -1;
			goto cleanup;
		}

		/* set our priority */
		ret = semanage_module_info_set_priority(sh,
							&modinfo,
							priority);
		if (ret != 0) {
			status = -1;
			goto cleanup;
		}

		/* get the priority path */
		ret = semanage_module_get_path(sh,
					       &modinfo,
					       SEMANAGE_MODULE_PATH_PRIORITY,
					       priority_path,
					       sizeof(priority_path));
		if (ret != 0) {
			status = -1;
			goto cleanup;
		}

		/* cleanup old modules */
		if (modules != NULL) {
			for (j = 0; j < modules_len; j++) {
				free(modules[j]);
				modules[j] = NULL;
			}
			free(modules);
			modules = NULL;
			modules_len = 0;
		}

		/* find modules at this priority */
		modules_len = scandir(priority_path,
				      &modules,
				      semanage_modules_filename_select,
				      versionsort);
		if (modules_len == -1) {
			ERR(sh,
			    "Error while scanning directory %s.",
			    priority_path);
			status = -1;
			goto cleanup;
		}

		if (modules_len == 0) continue;

		/* add space for modules */
		tmp = realloc(*modinfos,
			      sizeof(semanage_module_info_t) *
				(*modinfos_len + modules_len));
		if (tmp == NULL) {
			ERR(sh, "Error allocating memory for module array.");
			status = -1;
			goto cleanup;
		}
		*modinfos = tmp;

		/* for each module directory */
		for(j = 0; j < modules_len; j++) {
			/* set module name */
			ret = semanage_module_info_set_name(
					sh,
					&modinfo,
					modules[j]->d_name);
			if (ret != 0) {
				status = -1;
				goto cleanup;
			}

			/* get module values */
			ret = semanage_direct_get_module_info(
					sh,
					(const semanage_module_key_t *)
						(&modinfo),
					&modinfo_tmp);
			if (ret != 0) {
				status = -1;
				goto cleanup;
			}

			/* copy into array */
			ret = semanage_module_info_init(
					sh,
					&((*modinfos)[*modinfos_len]));
			if (ret != 0) {
				status = -1;
				goto cleanup;
			}

			ret = semanage_module_info_clone(
					sh,
					modinfo_tmp,
					&((*modinfos)[*modinfos_len]));
			if (ret != 0) {
				status = -1;
				goto cleanup;
			}

			ret = semanage_module_info_destroy(sh, modinfo_tmp);
			if (ret != 0) {
				status = -1;
				goto cleanup;
			}
			free(modinfo_tmp);
			modinfo_tmp = NULL;

			*modinfos_len += 1;
		}
	}

cleanup:
	semanage_module_info_destroy(sh, &modinfo);

	if (priorities != NULL) {
		for (i = 0; i < priorities_len; i++) {
			free(priorities[i]);
		}
		free(priorities);
	}

	if (modules != NULL) {
		for (i = 0; i < modules_len; i++) {
			free(modules[i]);
		}
		free(modules);
	}

	ret = semanage_module_info_destroy(sh, modinfo_tmp);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}
	free(modinfo_tmp);
	modinfo_tmp = NULL;

	if (status != 0) {
		if (modinfos != NULL) {
			for (i = 0; i < *modinfos_len; i++) {
				semanage_module_info_destroy(
						sh, 
						&(*modinfos)[i]);
			}
			free(*modinfos);
			*modinfos = NULL;
			*modinfos_len = 0;
		}
	}

	return status;
}

static int semanage_direct_install_info(semanage_handle_t *sh,
					const semanage_module_info_t *modinfo,
					char *data,
					size_t data_len)
{
	assert(sh);
	assert(modinfo);
	assert(data);

	int status = 0;
	int ret = 0;
	int type;

	char path[PATH_MAX];

	semanage_module_info_t *higher_info = NULL;
	semanage_module_key_t higher_key;
	ret = semanage_module_key_init(sh, &higher_key);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	/* validate module info */
	ret = semanage_module_info_validate(modinfo);
	if (ret != 0) {
		ERR(sh, "%s failed module validation.\n", modinfo->name);
		status = -2;
		goto cleanup;
	}

	/* Check for higher priority module and warn if there is one as
	 * it will override the module currently being installed.
	 */
	ret = semanage_module_key_set_name(sh, &higher_key, modinfo->name);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	ret = semanage_direct_get_module_info(sh, &higher_key, &higher_info);
	if (ret == 0) {
		if (higher_info->priority > modinfo->priority) {
			errno = 0;
			WARN(sh,
			     "A higher priority %s module exists at priority %d and will override the module currently being installed at priority %d.",
			     modinfo->name,
			     higher_info->priority,
			     modinfo->priority);
		}
		else if (higher_info->priority < modinfo->priority) {
			errno = 0;
			INFO(sh,
			     "Overriding %s module at lower priority %d with module at priority %d.",
			     modinfo->name,
			     higher_info->priority,
			     modinfo->priority);
		}

		if (higher_info->enabled == 0 && modinfo->enabled == -1) {
			errno = 0;
			WARN(sh,
			     "%s module will be disabled after install due to default enabled status.",
			     modinfo->name);
		}
	}

	/* set module meta data */
	ret = semanage_direct_set_module_info(sh, modinfo);
	if (ret != 0) {
		status = -2;
		goto cleanup;
	}

	/* install module source file */
	if (!strcasecmp(modinfo->lang_ext, "cil")) {
		type = SEMANAGE_MODULE_PATH_CIL;
	} else {
		type = SEMANAGE_MODULE_PATH_HLL;
	}
	ret = semanage_module_get_path(
			sh,
			modinfo,
			type,
			path,
			sizeof(path));
	if (ret != 0) {
		status = -3;
		goto cleanup;
	}

	ret = bzip(sh, path, data, data_len);
	if (ret <= 0) {
		ERR(sh, "Error while writing to %s.", path);
		status = -3;
		goto cleanup;
	}
	
	/* if this is an HLL, delete the CIL cache if it exists so it will get recompiled */
	if (type == SEMANAGE_MODULE_PATH_HLL) {
		ret = semanage_module_get_path(
				sh,
				modinfo,
				SEMANAGE_MODULE_PATH_CIL,
				path,
				sizeof(path));
		if (ret != 0) {
			status = -3;
			goto cleanup;
		}

		if (access(path, F_OK) == 0) {
			ret = unlink(path);
			if (ret != 0) {
				ERR(sh, "Error while removing cached CIL file %s: %s", path, strerror(errno));
				status = -3;
				goto cleanup;
			}
		}
	}

cleanup:
	semanage_module_key_destroy(sh, &higher_key);
	semanage_module_info_destroy(sh, higher_info);
	free(higher_info);

	return status;
}

static int semanage_direct_remove_key(semanage_handle_t *sh,
				      const semanage_module_key_t *modkey)
{
	assert(sh);
	assert(modkey);

	int status = 0;
	int ret = 0;

	char path[PATH_MAX];
	semanage_module_info_t *modinfo = NULL;

	semanage_module_key_t modkey_tmp;
	ret = semanage_module_key_init(sh, &modkey_tmp);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	/* validate module key */
	ret = semanage_module_validate_priority(modkey->priority);
	if (ret != 0) {
		errno = 0;
		ERR(sh, "Priority %d is invalid.", modkey->priority);
		status = -1;
		goto cleanup;
	}

	ret = semanage_module_validate_name(modkey->name);
	if (ret != 0) {
		errno = 0;
		ERR(sh, "Name %s is invalid.", modkey->name);
		status = -1;
		goto cleanup;
	}

	ret = semanage_module_key_set_name(sh, &modkey_tmp, modkey->name);
	if (ret != 0) {
		status = -1;
		goto cleanup;
	}

	/* get module path */
	ret = semanage_module_get_path(
			sh,
			(const semanage_module_info_t *)modkey,
			SEMANAGE_MODULE_PATH_NAME,
			path,
			sizeof(path));
	if (ret != 0) {
		status = -2;
		goto cleanup;
	}

	/* remove directory */
	ret = semanage_remove_directory(path);
	if (ret != 0) {
		ERR(sh, "Unable to remove module %s at priority %d.", modkey->name, modkey->priority);
		status = -2;
		goto cleanup;
	}

	/* check if its the last module at any priority */
	ret = semanage_module_get_module_info(sh, &modkey_tmp, &modinfo);
	if (ret != 0) {
		/* info that no other module will override */
		errno = 0;
		INFO(sh,
		     "Removing last %s module (no other %s module exists at another priority).",
		     modkey->name,
		     modkey->name);

		/* remove disabled status file */
		ret = semanage_module_get_path(
				sh,
				(const semanage_module_info_t *)modkey,
				SEMANAGE_MODULE_PATH_DISABLED,
				path,
				sizeof(path));
		if (ret != 0) {
			status = -1;
			goto cleanup;
		}

		struct stat sb;
		if (stat(path, &sb) == 0) {
			ret = unlink(path);
			if (ret != 0) {
				status = -1;
				goto cleanup;
			}
		}
	}
	else {
		/* if a lower priority module is going to become active */
		if (modkey->priority > modinfo->priority) {
			/* inform what the new active module will be */
			errno = 0;
			INFO(sh,
			     "%s module at priority %d is now active.",
			     modinfo->name,
			     modinfo->priority);
		}
	}

cleanup:
	semanage_module_key_destroy(sh, &modkey_tmp);

	semanage_module_info_destroy(sh, modinfo);
	free(modinfo);

	return status;
}

