/*
 * main.c: Subversion server administration tool.
 *
 * ====================================================================
 * Copyright (c) 2000-2001 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 * ====================================================================
 */

#include <apr_general.h>
#include <apr_pools.h>

#define APR_WANT_STDIO
#define APR_WANT_STRFUNC
#include <apr_want.h>

#include "svn_types.h"
#include "svn_pools.h"
#include "svn_error.h"
#include "svn_fs.h"
#include "svn_repos.h"

#include "db.h"


/*** Tree printing. ***/

/* Print the tree at ROOT:PATH, indenting by INDENTATION spaces.
   Use POOL for any allocation.  */
static svn_error_t *
print_tree (svn_fs_root_t *root,
            const char *path,
            int indentation,
            apr_pool_t *pool)
{
  apr_hash_t *entries;
  apr_hash_index_t *hi;
  
  SVN_ERR (svn_fs_dir_entries (&entries, root, path, pool));

  for (hi = apr_hash_first (pool, entries); hi; hi = apr_hash_next (hi))
    {
      const void *key;
      apr_size_t keylen;
      void *val;
      svn_fs_dirent_t *this_entry;
      const char *this_full_path;
      int is_dir;
      int i;
      svn_fs_id_t *id;
      svn_stringbuf_t *id_str;

      apr_hash_this (hi, &key, &keylen, &val);
      this_entry = val;

      this_full_path = apr_psprintf (pool, "%s/%s", path, this_entry->name);

      /* Indent. */
      for (i = 0; i < indentation; i++)
        printf (" ");

      printf ("%s", this_entry->name);
      
      SVN_ERR (svn_fs_node_id (&id, root, this_full_path, pool));
      id_str = svn_fs_unparse_id (id, pool);

      SVN_ERR (svn_fs_is_dir (&is_dir, root, this_full_path, pool));
      if (is_dir)
        {

          printf ("/ <%s>\n", id_str->data);  /* trailing slash for dirs */
          print_tree (root, this_full_path, indentation + 1, pool);
        }
      else   /* assume it's a file */
        {
          apr_off_t len;
          SVN_ERR (svn_fs_file_length (&len, root, this_full_path, pool));
          printf (" <%s> [%ld]\n", id_str->data, (long int) len);
        }
    }

  return SVN_NO_ERROR;
}




/*** Argument parsing and usage. ***/
static void
usage (const char *progname, int exit_code)
{
  fprintf
    (exit_code ? stderr : stdout,
     "usage: %s SUBCOMMAND REPOS_PATH [ARGS...]\n"
     "\n"
     "Subcommands are: \n"
     "\n"
     "  create    REPOS_PATH\n"
     "                Create a new, empty repository at REPOS_PATH."
     "\n"
     "  youngest  REPOS_PATH\n"
     "                Print the latest revision number."
     "\n"
     "  rmtxn     REPOS_PATH TXN_NAME\n"
     "                Delete the transaction named TXN_NAME."
     "\n"
     "  createtxn REPOS_PATH BASE_REV\n"
     "                Create a new transaction based on BASE_REV."
     "\n"
     "  lstxns    REPOS_PATH\n"
     "                Print all txns and their trees.\n"
     "\n"
     "  lsrevs    REPOS_PATH [LOWER_REV [UPPER_REV]]\n"
     "      If no revision is given, all revision trees are printed.\n"
     "      If just LOWER_REV is given, that revision tree is printed.\n"
     "      If two revisions are given, that range is printed, inclusive.\n"
     "\n"
     "  recover   REPOS_PATH\n"
     "      Run the Berkeley DB recovery procedure on a repository.  Do\n"
     "      this if you've been getting errors indicating that recovery\n"
     "      ought to be run.\n"
     "\n"
     "Printing a tree shows its structure, node ids, and file sizes.\n"
     "\n",
     progname);

  exit (exit_code);
}



/*** Main. ***/

int
main (int argc, const char * const *argv)
{
  apr_pool_t *pool;
  svn_fs_t *fs;
  svn_error_t *err;
  int               /* commands */
    is_create = 0,
    is_youngest = 0,
    is_lstxn = 0,
    is_lsrevs = 0,
    is_rmtxn = 0,
    is_createtxn = 0,
    is_recover = 0;
  const char *path = NULL;

  /* ### this whole thing needs to be cleaned up once client/main.c
     ### is refactored. for now, let's just get the tool up and
     ### running. */

  if (argc < 3)
    {
      usage (argv[0], 1);
      return EXIT_FAILURE;
    }

  path = argv[2];

  if (! ((is_create = strcmp(argv[1], "create") == 0)
         || (is_youngest = strcmp(argv[1], "youngest") == 0)
         || (is_lstxn = strcmp(argv[1], "lstxns") == 0)
         || (is_lsrevs = strcmp(argv[1], "lsrevs") == 0)
         || (is_rmtxn = strcmp(argv[1], "rmtxn") == 0)
         || (is_createtxn = strcmp(argv[1], "createtxn") == 0)
         || (is_recover = strcmp(argv[1], "recover") == 0)))
    {
      usage (argv[0], 1);
      return EXIT_FAILURE;
    }

  apr_initialize ();
  pool = svn_pool_create (NULL);

  if (is_create)
    {
      fs = svn_fs_new(pool);
      err = svn_fs_create_berkeley(fs, path);
      if (err) goto error;
    }
  else if (is_youngest)
    {
      svn_revnum_t youngest_rev;

      err = svn_repos_open (&fs, path, pool);
      if (err) goto error;

      svn_fs_youngest_rev (&youngest_rev, fs, pool);
      printf ("%ld\n", (long int) youngest_rev);
    }
  else if (is_lstxn)
    {
      char **txns;
      char *txn_name;

      err = svn_repos_open (&fs, path, pool);
      if (err) goto error;

      err = svn_fs_list_transactions(&txns, fs, pool);
      if (err) goto error;

      /* Loop, printing revisions. */
      while ((txn_name = *txns++))
        {
          svn_fs_txn_t *txn;
          svn_fs_root_t *this_root;
          svn_stringbuf_t *datestamp;
          svn_stringbuf_t *author;
          svn_stringbuf_t *log;
          svn_string_t date_prop = {SVN_PROP_REVISION_DATE,
                                    strlen(SVN_PROP_REVISION_DATE)};
          svn_string_t auth_prop = {SVN_PROP_REVISION_AUTHOR,
                                    strlen(SVN_PROP_REVISION_AUTHOR)};
          svn_string_t log_prop = {SVN_PROP_REVISION_LOG,
                                   strlen(SVN_PROP_REVISION_LOG)};
          apr_pool_t *this_pool = svn_pool_create (pool);

          err = svn_fs_open_txn (&txn, fs, txn_name, this_pool);
          if (err) goto error;

          err = svn_fs_txn_root (&this_root, txn, this_pool);
          if (err) goto error;

          err = svn_fs_txn_prop (&datestamp, txn, &date_prop, this_pool);
          if (err) goto error;
          err = svn_fs_txn_prop (&author, txn, &auth_prop, this_pool);
          if (err) goto error;
          if (! author)
            author = svn_stringbuf_create ("", this_pool);
          err = svn_fs_txn_prop (&log, txn, &log_prop, this_pool);
          if (err) goto error;
          if (! log)
            log = svn_stringbuf_create ("", this_pool);
          
          printf ("Txn %s:\n", txn_name);
          printf ("Created: %s\n", datestamp->data);
          printf ("Author: %s\n", author->data);
          printf ("Log (%lu bytes):\n%s\n",
                  (unsigned long int) log->len, log->data);
          printf ("==========================================\n");
          print_tree (this_root, "", 1, this_pool);
          printf ("\n");

          svn_pool_destroy (this_pool);
        }
    }
  else if (is_lsrevs)
    {
      svn_revnum_t
        lower = SVN_INVALID_REVNUM,
        upper = SVN_INVALID_REVNUM,
        this;

      err = svn_repos_open (&fs, path, pool);
      if (err) goto error;

      /* Do the args tell us what revisions to inspect? */
      if (argv[3])
        {
          lower = (svn_revnum_t) atoi (argv[3]);
          if (argv[4])
            upper = (svn_revnum_t) atoi (argv[4]);
        }

      /* Fill in for implied args. */
      if (lower == SVN_INVALID_REVNUM)
        {
          lower = 0;
          svn_fs_youngest_rev (&upper, fs, pool);
        }
      else if (upper == SVN_INVALID_REVNUM)
        upper = lower;

      /* Loop, printing revisions. */
      for (this = lower; this <= upper; this++)
        {
          svn_fs_root_t *this_root;
          svn_stringbuf_t *datestamp;
          svn_stringbuf_t *author;
          svn_stringbuf_t *log;
          apr_pool_t *this_pool = svn_pool_create (pool);
          svn_string_t date_prop = {SVN_PROP_REVISION_DATE,
                                    strlen(SVN_PROP_REVISION_DATE)};
          svn_string_t auth_prop = {SVN_PROP_REVISION_AUTHOR,
                                    strlen(SVN_PROP_REVISION_AUTHOR)};
          svn_string_t log_prop = {SVN_PROP_REVISION_LOG,
                                   strlen(SVN_PROP_REVISION_LOG)};
           
          err = svn_fs_revision_root (&this_root, fs, this, this_pool);
          if (err) goto error;

          err = svn_fs_revision_prop (&datestamp, fs, this,
                                      &date_prop, this_pool);
          if (err) goto error;

          err = svn_fs_revision_prop (&author, fs, this,
                                      &auth_prop, this_pool);
          if (err) goto error;
          if (! author)
            author = svn_stringbuf_create ("", this_pool);

          err = svn_fs_revision_prop (&log, fs, this,
                                      &log_prop, this_pool);
          if (err) goto error;
          if (! log)
            log = svn_stringbuf_create ("", this_pool);


          printf ("Revision %ld\n", (long int) this);
          printf ("Created: %s\n", datestamp->data);
          printf ("Author: %s\n", author->data);
          printf ("Log (%lu bytes):\n%s\n",
                  (unsigned long int) log->len, log->data);
          printf ("==========================================\n");
          print_tree (this_root, "", 1, this_pool);
          printf ("\n");

          svn_pool_destroy (this_pool);
        }
    }
  else if (is_rmtxn)
    {
      svn_fs_txn_t *txn;

      if (! argv[3])
        {
          usage (argv[0], 1);
          return EXIT_FAILURE;
        }

      err = svn_repos_open (&fs, path, pool);
      if (err) goto error;
      
      err = svn_fs_open_txn (&txn, fs, argv[3], pool);
      if (err) goto error;

      err = svn_fs_abort_txn (txn);
      if (err) goto error;
    }
  else if (is_createtxn)
    {
      svn_fs_txn_t *txn;

      if (! argv[3])
        {
          usage (argv[0], 1);
          return EXIT_FAILURE;
        }

      err = svn_repos_open (&fs, path, pool);
      if (err) goto error;

      err = svn_fs_begin_txn (&txn, fs, (svn_revnum_t) atoi(argv[3]), pool);
      if (err) goto error;

      err = svn_fs_close_txn (txn);
      if (err) goto error;
    }
  else if (is_recover)
    {
      apr_status_t apr_err;
      const char *lockfile_path, *env_path;
      apr_file_t *lockfile_handle = NULL;

      /* Don't use svn_repos_open() here, because we don't want the
         usual locking behavior. */
      fs = svn_fs_new (pool);
      err = svn_fs_open_berkeley (fs, path);
      if (err && (err->src_err != DB_RUNRECOVERY))
        goto error;

      /* Exclusively lock the repository.  This blocks on other locks,
         including shared locks. */
      lockfile_path = svn_fs_db_lockfile (fs, pool);
      apr_err = apr_file_open (&lockfile_handle, lockfile_path,
                               (APR_WRITE | APR_APPEND), APR_OS_DEFAULT, pool);
      if (! APR_STATUS_IS_SUCCESS (apr_err))
        {
          err = svn_error_createf
            (apr_err, 0, NULL, pool,
             "%s: error opening db lockfile `%s'", argv[0], lockfile_path);
          goto error;
        }

      apr_err = apr_file_lock (lockfile_handle, APR_FLOCK_EXCLUSIVE);
      if (! APR_STATUS_IS_SUCCESS (apr_err))
        {
          err = svn_error_createf
            (apr_err, 0, NULL, pool,
             "%s: exclusive lock on `%s' failed", argv[0], lockfile_path);
          goto error;
        }

      /* Run recovery on the Berkeley environment, using FS to get the
         path to said environment. */ 
      env_path = svn_fs_db_env (fs, pool);
      /* ### todo: this usually seems to get an error -- namely, that
         the DB needs recovery!  Why would that be, when we just
         recovered it?  Is it an error to recover a DB that doesn't
         need recovery, perhaps?  See issue #430. */
      err = svn_fs_berkeley_recover (env_path, pool);
      if (err) goto error;

      /* Release the exclusive lock. */
      apr_err = apr_file_unlock (lockfile_handle);
      if (! APR_STATUS_IS_SUCCESS (apr_err))
        {
          err = svn_error_createf
            (apr_err, 0, NULL, pool,
             "%s: error unlocking `%s'", argv[0], lockfile_path);
          goto error;
        }

      apr_err = apr_file_close (lockfile_handle);
      if (! APR_STATUS_IS_SUCCESS (apr_err))
        {
          err = svn_error_createf
            (apr_err, 0, NULL, pool,
             "%s: error closing `%s'", argv[0], lockfile_path);
          goto error;
        }
    }

  err = svn_fs_close_fs(fs);
  if (err) goto error;

  svn_pool_destroy (pool);
  apr_terminate();

  return EXIT_SUCCESS;

 error:
  svn_handle_error(err, stderr, FALSE);
  return EXIT_FAILURE;
}




/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
