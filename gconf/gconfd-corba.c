/* GConf
 * Copyright (C) 1999, 2000 Red Hat Inc.
 * Copyright (C) 2003       CodeFactory AB
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "gconfd-corba.h"
#include "gconfd.h"
#include "gconf-database-corba.h"
#include "gconf-corba-utils.h"
#include "gconf.h"

#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/*
 * Declarations
 */

static void    add_client            (const ConfigListener  client);
static void    remove_client         (const ConfigListener  client);
static GSList *list_clients          (void);
static void    log_clients_to_string (GString              *str);


/* 
 * CORBA goo
 */

static ConfigServer server = CORBA_OBJECT_NIL;
static PortableServer_POA the_poa;

static ConfigDatabase
gconfd_get_default_database(PortableServer_Servant servant,
                            CORBA_Environment* ev);

static ConfigDatabase
gconfd_get_database(PortableServer_Servant servant,
                    const CORBA_char* address,
                    CORBA_Environment* ev);

static void
gconfd_add_client (PortableServer_Servant servant,
                   const ConfigListener client,
                   CORBA_Environment *ev);

static void
gconfd_remove_client (PortableServer_Servant servant,
                      const ConfigListener client,
                      CORBA_Environment *ev);

static CORBA_long
gconfd_ping(PortableServer_Servant servant, CORBA_Environment *ev);

static void
gconfd_shutdown(PortableServer_Servant servant, CORBA_Environment *ev);

static PortableServer_ServantBase__epv base_epv = {
  NULL,
  NULL,
  NULL
};

static POA_ConfigServer__epv server_epv = { 
  NULL,
  gconfd_get_default_database,
  gconfd_get_database,
  gconfd_add_client,
  gconfd_remove_client,
  gconfd_ping,
  gconfd_shutdown
};

static POA_ConfigServer__vepv poa_server_vepv = { &base_epv, &server_epv };
static POA_ConfigServer poa_server_servant = { NULL, &poa_server_vepv };

static ConfigDatabase
gconfd_get_default_database(PortableServer_Servant servant,
                            CORBA_Environment* ev)
{
  GConfDatabase *db;

  if (gconfd_corba_check_in_shutdown (ev))
    return CORBA_OBJECT_NIL;
  
  db = gconfd_lookup_database (NULL);

  if (db)
    return CORBA_Object_duplicate (gconf_database_corba_get_objref (db), ev);
  else
    return CORBA_OBJECT_NIL;
}

static ConfigDatabase
gconfd_get_database(PortableServer_Servant servant,
                    const CORBA_char* address,
                    CORBA_Environment* ev)
{
  GConfDatabase *db;
  GError* error = NULL;  

  if (gconfd_corba_check_in_shutdown (ev))
    return CORBA_OBJECT_NIL;
  
  db = gconfd_obtain_database (address, &error);

  if (db != NULL)
    return CORBA_Object_duplicate (gconf_database_corba_get_objref (db), ev);
  else if (gconf_corba_set_exception(&error, ev))
    return CORBA_OBJECT_NIL;
  else
    return CORBA_OBJECT_NIL;
}

static void
gconfd_add_client (PortableServer_Servant servant,
                   const ConfigListener client,
                   CORBA_Environment *ev)
{
  if (gconfd_corba_check_in_shutdown (ev))
    return;
  
  add_client (client);
}

static void
gconfd_remove_client (PortableServer_Servant servant,
                      const ConfigListener client,
                      CORBA_Environment *ev)
{
  if (gconfd_corba_check_in_shutdown (ev))
    return;
  
  remove_client (client);
}

static CORBA_long
gconfd_ping(PortableServer_Servant servant, CORBA_Environment *ev)
{
  if (gconfd_corba_check_in_shutdown (ev))
    return 0;
  
  return getpid();
}

static void
gconfd_shutdown(PortableServer_Servant servant, CORBA_Environment *ev)
{
  if (gconfd_corba_check_in_shutdown (ev))
    return;
  
  gconf_log(GCL_DEBUG, _("Shutdown request received"));

  gconf_main_quit();
}

PortableServer_POA
gconf_corba_get_poa ()
{
  return the_poa;
}

gboolean
gconfd_corba_init (void)
{
  gchar* ior;  
  CORBA_Environment ev;
  CORBA_ORB orb;

  CORBA_exception_init(&ev);

  orb = gconf_orb_get ();
  
  POA_ConfigServer__init (&poa_server_servant, &ev);
  
  the_poa = (PortableServer_POA)CORBA_ORB_resolve_initial_references(orb, "RootPOA", &ev);
  PortableServer_POAManager_activate(PortableServer_POA__get_the_POAManager(the_poa, &ev), &ev);

  server = PortableServer_POA_servant_to_reference(the_poa,
                                                   &poa_server_servant,
                                                   &ev);
  if (CORBA_Object_is_nil(server, &ev)) 
    {
      gconf_log(GCL_ERR, _("Failed to get object reference for ConfigServer"));
      return FALSE;
    }

  /* Needs to be done before loading sources */
  ior = CORBA_ORB_object_to_string (orb, server, &ev);
  gconf_set_daemon_ior (ior);
  CORBA_free (ior);

  return TRUE;
}

/*
 * Logging
 */

/*
   The log file records the current listeners we have registered,
   so we can restore them if we exit and restart.

   Basically:

   1) On startup, we parse any logfile and try to restore the
      listeners contained therein. As we restore each listener (give
      clients a new listener ID) we append a removal of the previous
      daemon's listener and the addition of our own listener to the
      logfile; this means that if we crash and have to restore a
      client's listener a second time, we'll have the client's current
      listener ID. If all goes well we then atomically rewrite the
      parsed logfile with the resulting current state, to keep the logfile
      compact.

   2) While running, we keep a FILE* open and whenever we add/remove
      a listener we write a line to the logfile recording it,
      to keep the logfile always up-to-date.

   3) On normal exit, and also periodically (every hour or so, say) we
      atomically write over the running log with our complete current
      state, to keep the running log from growing without bound.
*/

static void
get_log_names (gchar **logdir, gchar **logfile)
{
  *logdir = gconf_concat_dir_and_key (g_get_home_dir (), ".gconfd-dbus");
  *logfile = gconf_concat_dir_and_key (*logdir, "saved_state");
}

static void close_append_handle (void);

static FILE* append_handle = NULL;
static guint append_handle_timeout = 0;

static gboolean
close_append_handle_timeout(gpointer data)
{
  close_append_handle ();

  /* uninstall the timeout */
  append_handle_timeout = 0;
  return FALSE;
}

static gboolean
open_append_handle (GError **err)
{
  if (append_handle == NULL)
    {
      gchar *logdir;
      gchar *logfile;

      get_log_names (&logdir, &logfile);
      
      mkdir (logdir, 0700); /* ignore failure, we'll catch failures
                             * that matter on open()
                             */
      
      append_handle = fopen (logfile, "a");

      if (append_handle == NULL)
        {
          gconf_set_error (err,
                           GCONF_ERROR_FAILED,
                           _("Failed to open gconfd logfile; won't be able to restore listeners after gconfd shutdown (%s)"),
                           strerror (errno));
          
          g_free (logdir);
          g_free (logfile);

          return FALSE;
        }
      
      g_free (logdir);
      g_free (logfile);


      {
        const gulong timeout_len = 1000*60*0.5; /* 1 sec * 60 s/min * 0.5 min */

        if (append_handle_timeout != 0)
          g_source_remove (append_handle_timeout);
        
        append_handle_timeout = g_timeout_add (timeout_len,
                                               close_append_handle_timeout,
                                               NULL);
      }
    }

  return TRUE;
}

static void
close_append_handle (void)
{
  if (append_handle)
    {
      if (fclose (append_handle) < 0)
        gconf_log (GCL_WARNING,
                   _("Failed to close gconfd logfile; data may not have been properly saved (%s)"),
                   strerror (errno));

      append_handle = NULL;

      if (append_handle_timeout != 0)
        {
          g_source_remove (append_handle_timeout);
          append_handle_timeout = 0;
        }
    }
}

/* Atomically save our current state, if possible; otherwise
 * leave the running log in place.
 */
void
gconfd_corba_logfile_save (void)
{
  GList *tmp_list;
  gchar *logdir = NULL;
  gchar *logfile = NULL;
  gchar *tmpfile = NULL;
  gchar *tmpfile2 = NULL;
  GString *saveme = NULL;
  gint fd = -1;
  
  /* Close the running log */
  close_append_handle ();
  
  get_log_names (&logdir, &logfile);

  mkdir (logdir, 0700); /* ignore failure, we'll catch failures
                         * that matter on open()
                         */

  saveme = g_string_new ("");

  /* Clients */
  log_clients_to_string (saveme);
  
  /* Default database */
  gconf_database_log_listeners_to_string (gconfd_lookup_database (NULL),
                                          TRUE,
                                          saveme);

  /* Other databases */
  
  tmp_list = gconfd_get_database_list ();;

  while (tmp_list)
    {
      GConfDatabase *db = tmp_list->data;

      gconf_database_log_listeners_to_string (db,
                                              FALSE,
                                              saveme);
      
      tmp_list = g_list_next (tmp_list);
    }
  
  /* Now try saving the string to a temporary file */
  tmpfile = g_strconcat (logfile, ".tmp", NULL);
  
  fd = open (tmpfile, O_WRONLY | O_CREAT | O_TRUNC, 0700);

  if (fd < 0)
    {
      gconf_log (GCL_WARNING,
                 _("Could not open saved state file '%s' for writing: %s"),
                 tmpfile, strerror (errno));
      
      goto out;
    }

 again:
  
  if (write (fd, saveme->str, saveme->len) < 0)
    {
      if (errno == EINTR)
        goto again;
      
      gconf_log (GCL_WARNING,
                 _("Could not write saved state file '%s' fd: %d: %s"),
                 tmpfile, fd, strerror (errno));

      goto out;
    }

  if (close (fd) < 0)
    {
      gconf_log (GCL_WARNING,
                 _("Failed to close new saved state file '%s': %s"),
                 tmpfile, strerror (errno));
      goto out;
    }

  fd = -1;
  
  /* Move the main saved state file aside, if it exists */
  if (gconf_file_exists (logfile))
    {
      tmpfile2 = g_strconcat (logfile, ".orig", NULL);
      if (rename (logfile, tmpfile2) < 0)
        {
          gconf_log (GCL_WARNING,
                     _("Could not move aside old saved state file '%s': %s"),
                     logfile, strerror (errno));
          goto out;
        }
    }

  /* Move the new saved state file into place */
  if (rename (tmpfile, logfile) < 0)
    {
      gconf_log (GCL_WARNING,
                 _("Failed to move new save state file into place: %s"),
                 strerror (errno));

      /* Try to restore old file */
      if (tmpfile2)
        {
          if (rename (tmpfile2, logfile) < 0)
            {
              gconf_log (GCL_WARNING,
                         _("Failed to restore original saved state file that had been moved to '%s': %s"),
                         tmpfile2, strerror (errno));

            }
        }
      
      goto out;
    }

  /* Get rid of original saved state file if everything succeeded */
  if (tmpfile2)
    unlink (tmpfile2);
  
 out:
  if (saveme)
    g_string_free (saveme, TRUE);
  g_free (logdir);
  g_free (logfile);
  g_free (tmpfile);
  g_free (tmpfile2);

  if (fd >= 0)
    close (fd);
}

typedef struct _ListenerLogEntry ListenerLogEntry;

struct _ListenerLogEntry
{
  guint connection_id;
  gchar *ior;
  gchar *address;
  gchar *location;
};

static guint
listener_logentry_hash (gconstpointer v)
{
  const ListenerLogEntry *lle = v;

  return
    (lle->connection_id         & 0xff000000) |
    (g_str_hash (lle->ior)      & 0x00ff0000) |
    (g_str_hash (lle->address)  & 0x0000ff00) |
    (g_str_hash (lle->location) & 0x000000ff);
}

static gboolean
listener_logentry_equal (gconstpointer ap, gconstpointer bp)
{
  const ListenerLogEntry *a = ap;
  const ListenerLogEntry *b = bp;

  return
    a->connection_id == b->connection_id &&
    strcmp (a->location, b->location) == 0 &&
    strcmp (a->ior, b->ior) == 0 &&
    strcmp (a->address, b->address) == 0;
}

/* Return value indicates whether we "handled" this line of text */
static gboolean
parse_listener_entry (GHashTable *entries,
                      gchar      *text)
{
  gboolean add;
  gchar *p;
  gchar *ior;
  gchar *address;
  gchar *location;
  gchar *end;
  guint connection_id;
  GError *err;
  ListenerLogEntry *lle;
  ListenerLogEntry *old;
  
  if (strncmp (text, "ADD", 3) == 0)
    {
      add = TRUE;
      p = text + 3;
    }
  else if (strncmp (text, "REMOVE", 6) == 0)
    {
      add = FALSE;
      p = text + 6;
    }
  else
    {
      return FALSE;
    }
  
  while (*p && g_ascii_isspace (*p))
    ++p;

  errno = 0;
  end = NULL;
  connection_id = strtoul (p, &end, 10);
  if (end == p || errno != 0)
    {
      gconf_log (GCL_DEBUG,
                 "Failed to parse connection ID in saved state file");
      
      return TRUE;
    }

  if (connection_id == 0)
    {
      gconf_log (GCL_DEBUG,
                 "Connection ID 0 in saved state file is not valid");
      return TRUE;
    }
  
  p = end;

  while (*p && g_ascii_isspace (*p))
    ++p;

  err = NULL;
  end = NULL;
  gconf_unquote_string_inplace (p, &end, &err);
  if (err != NULL)
    {
      gconf_log (GCL_DEBUG,
                 "Failed to unquote config source address from saved state file: %s",
                 err->message);

      g_error_free (err);
      
      return TRUE;
    }

  address = p;
  p = end;

  while (*p && g_ascii_isspace (*p))
    ++p;
  
  err = NULL;
  end = NULL;
  gconf_unquote_string_inplace (p, &end, &err);
  if (err != NULL)
    {
      gconf_log (GCL_DEBUG,
                 "Failed to unquote listener location from saved state file: %s",
                 err->message);

      g_error_free (err);
      
      return TRUE;
    }

  location = p;
  p = end;

  while (*p && g_ascii_isspace (*p))
    ++p;
  
  err = NULL;
  end = NULL;
  gconf_unquote_string_inplace (p, &end, &err);
  if (err != NULL)
    {
      gconf_log (GCL_DEBUG,
                 "Failed to unquote IOR from saved state file: %s",
                 err->message);
      
      g_error_free (err);
      
      return TRUE;
    }
  
  ior = p;
  p = end;    

  lle = g_new (ListenerLogEntry, 1);
  lle->connection_id = connection_id;
  lle->address = address;
  lle->ior = ior;
  lle->location = location;

  if (*(lle->address) == '\0' ||
      *(lle->ior) == '\0' ||
      *(lle->location) == '\0')
    {
      gconf_log (GCL_DEBUG,
                 "Saved state file listener entry didn't contain all the fields; ignoring.");

      g_free (lle);

      return TRUE;
    }
  
  old = g_hash_table_lookup (entries, lle);

  if (old)
    {
      if (add)
        {
          gconf_log (GCL_DEBUG,
                     "Saved state file records the same listener added twice; ignoring the second instance");
          goto quit;
        }
      else
        {
          /* This entry was added, then removed. */
          g_hash_table_remove (entries, lle);
          goto quit;
        }
    }
  else
    {
      if (add)
        {
          g_hash_table_insert (entries, lle, lle);
          
          return TRUE;
        }
      else
        {
          gconf_log (GCL_DEBUG,
                     "Saved state file had a removal of a listener that wasn't added; ignoring the removal.");
          goto quit;
        }
    }
  
 quit:
  g_free (lle);

  return TRUE;
}                

/* Return value indicates whether we "handled" this line of text */
static gboolean
parse_client_entry (GHashTable *clients,
                    gchar      *text)
{
  gboolean add;
  gchar *ior;
  GError *err;
  gchar *old;
  gchar *p;
  gchar *end;
  
  if (strncmp (text, "CLIENTADD", 9) == 0)
    {
      add = TRUE;
      p = text + 9;
    }
  else if (strncmp (text, "CLIENTREMOVE", 12) == 0)
    {
      add = FALSE;
      p = text + 12;
    }
  else
    {
      return FALSE;
    }
  
  while (*p && g_ascii_isspace (*p))
    ++p;
  
  err = NULL;
  end = NULL;
  gconf_unquote_string_inplace (p, &end, &err);
  if (err != NULL)
    {
      gconf_log (GCL_DEBUG,
                 "Failed to unquote IOR from saved state file: %s",
                 err->message);
      
      g_error_free (err);
      
      return TRUE;
    }
  
  ior = p;
  p = end;    
  
  old = g_hash_table_lookup (clients, ior);

  if (old)
    {
      if (add)
        {
          gconf_log (GCL_DEBUG,
                     "Saved state file records the same client added twice; ignoring the second instance");
          goto quit;
        }
      else
        {
          /* This entry was added, then removed. */
          g_hash_table_remove (clients, ior);
          goto quit;
        }
    }
  else
    {
      if (add)
        {
          g_hash_table_insert (clients, ior, ior);
          
          return TRUE;
        }
      else
        {
          gconf_log (GCL_DEBUG,
                     "Saved state file had a removal of a client that wasn't added; ignoring the removal.");
          goto quit;
        }
    }
  
 quit:

  return TRUE;
}

static void
restore_client (const gchar *ior)
{
  ConfigListener cl;
  CORBA_Environment ev;
  
  CORBA_exception_init (&ev);
  
  cl = CORBA_ORB_string_to_object (gconf_orb_get (), (gchar*)ior, &ev);

  CORBA_exception_free (&ev);
  
  if (CORBA_Object_is_nil (cl, &ev))
    {
      CORBA_exception_free (&ev);

      gconf_log (GCL_DEBUG,
                 "Client in saved state file no longer exists, not restoring it as a client");
      
      return;
    }

  ConfigListener_drop_all_caches (cl, &ev);
  
  if (ev._major != CORBA_NO_EXCEPTION)
    {
      gconf_log (GCL_DEBUG, "Failed to update client in saved state file, probably the client no longer exists");

      goto finished;
    }

  /* Add the client, since it still exists. Note that the client still
   * has the wrong server object reference, so next time it tries to
   * contact the server it will re-add itself; we just live with that,
   * it's not a problem.
   */
  add_client (cl);
  
 finished:
  CORBA_Object_release (cl, &ev);

  CORBA_exception_free (&ev);
}

static void
restore_listener (GConfDatabase* db,
                  ListenerLogEntry *lle)
{
  ConfigListener cl;
  CORBA_Environment ev;
  guint new_cnxn;
  GError *err;
  
  CORBA_exception_init (&ev);
  
  cl = CORBA_ORB_string_to_object (gconf_orb_get (), lle->ior, &ev);

  CORBA_exception_free (&ev);
  
  if (CORBA_Object_is_nil (cl, &ev))
    {
      CORBA_exception_free (&ev);

      gconf_log (GCL_DEBUG,
                 "Client in saved state file no longer exists, not updating its listener connections");
      
      return;
    }

  /* "Cancel" the addition of the listener in the saved state file,
   * so that if we reload the saved state file a second time
   * for some reason, we don't try to add this listener that time.
   */

  err = NULL;  
  if (!gconfd_logfile_change_listener (db,
                                       FALSE, /* remove */
                                       lle->connection_id,
                                       cl,
                                       lle->location,
                                       &err))
    {
      gconf_log (GCL_DEBUG,
                 "Failed to cancel previous daemon's listener in saved state file: %s",
                 err->message);
      g_error_free (err);
    }
  
  new_cnxn = gconf_database_corba_readd_listener (db, cl, "from-saved-state", lle->location);

  gconf_log (GCL_DEBUG, "Attempting to update listener from saved state file, old connection %u, new connection %u", (guint) lle->connection_id, (guint) new_cnxn);
  
  ConfigListener_update_listener (cl,
                                  gconf_database_corba_get_objref (db),
                                  lle->address,
                                  lle->connection_id,
                                  lle->location,
                                  new_cnxn,
                                  &ev);
  
  if (ev._major != CORBA_NO_EXCEPTION)
    {
      gconf_log (GCL_DEBUG, "Failed to update listener in saved state file, probably the client no longer exists");

      /* listener will get removed next time we try to notify -
       * we already appended a cancel of the listener to the
       * saved state file.
       */
      goto finished;
    }

  /* Successfully notified client of new connection ID, so put that
   * connection ID in the saved state file.
   */
  err = NULL;  
  if (!gconfd_logfile_change_listener (db,
                                       TRUE, /* add */
                                       new_cnxn,
                                       cl,
                                       lle->location,
                                       &err))
    {
      gconf_log (GCL_DEBUG,
                 "Failed to re-add this daemon's listener ID in saved state file: %s",
                 err->message);
      g_error_free (err);
    }

  /* We updated the listener, and logged that to the saved state
   * file. Yay!
   */
  
 finished:
  
  CORBA_Object_release (cl, &ev);

  CORBA_exception_free (&ev);
}

static void
listener_logentry_restore_and_destroy_foreach (gpointer key,
                                               gpointer value,
                                               gpointer data)
{
  ListenerLogEntry *lle = key;
  GConfDatabase *db;
  
  if (strcmp (lle->address, "def") == 0)
    db = gconfd_lookup_database (NULL);
  else
    db = gconfd_obtain_database (lle->address, NULL);
  
  if (db == NULL)
    {
      gconf_log (GCL_WARNING,
                 _("Unable to restore a listener on address '%s', couldn't resolve the database"),
                 lle->address);
      return;
    }

  restore_listener (db, lle);

  /* We don't need it anymore */
  g_free (lle);
}

static void
restore_client_foreach (gpointer key,
                        gpointer value,
                        gpointer data)
{
  restore_client (key);
}


#ifndef HAVE_FLOCKFILE
#  define flockfile(f) (void)1
#  define funlockfile(f) (void)1
#  define getc_unlocked(f) getc(f)
#endif /* !HAVE_FLOCKFILE */

static gchar*
read_line (FILE *f)
{
  int c;
  GString *str;
  
  str = g_string_new ("");
  
  flockfile (f);

  while (TRUE)
    {
      c = getc_unlocked (f);

      switch (c)
        {
        case EOF:
          if (ferror (f))
            {
              gconf_log (GCL_ERR,
                         _("Error reading saved state file: %s"),
                         g_strerror (errno));
            }

          /* FALL THRU */
        case '\n':
          goto done;
          break;

        default:
          g_string_append_c (str, c);
          break;
        }
    }

 done:
  funlockfile (f);

  if (str->len == 0)
    {
      g_string_free (str, TRUE);
      return NULL;
    }
  else
    {
      gchar *ret;
      
      ret = str->str;
      g_string_free (str, FALSE);
      return ret;
    }
}

void
gconfd_corba_logfile_read (void)
{
  gchar *logfile;
  gchar *logdir;
  GHashTable *entries;
  GHashTable *clients;
  FILE *f;
  gchar *line;
  GSList *lines = NULL;
  
  /* Just for good form */
  close_append_handle ();
  
  get_log_names (&logdir, &logfile);

  f = fopen (logfile, "r");
  
  if (f == NULL)
    {
      gconf_log (GCL_ERR, _("Unable to open saved state file '%s': %s"),
                 logfile, g_strerror (errno));

      goto finished;
    }

  entries = g_hash_table_new (listener_logentry_hash, listener_logentry_equal);
  clients = g_hash_table_new (g_str_hash, g_str_equal);

  line = read_line (f);
  while (line)
    {
      if (!parse_listener_entry (entries, line))
        {
          if (!parse_client_entry (clients, line))
            {
              gconf_log (GCL_DEBUG,
                         "Didn't understand line in saved state file: '%s'", 
                         line);
              g_free (line);
              line = NULL;
            }
        }

      if (line)
        lines = g_slist_prepend (lines, line);
      
      line = read_line (f);
    }
  
  /* Restore clients first */
  g_hash_table_foreach (clients,
                        restore_client_foreach,
                        NULL);
  
  /* Entries that still remain in the listener hash table were added
   * but not removed, so add them in this daemon instantiation and
   * update their listeners with the new connection ID etc.
   */
  g_hash_table_foreach (entries, 
                        listener_logentry_restore_and_destroy_foreach,
                        NULL);

  g_hash_table_destroy (entries);
  g_hash_table_destroy (clients);

  /* Note that we need the strings to remain valid until we are totally
   * finished, because we store pointers to them in the log entry
   * hash.
   */
  g_slist_foreach (lines, (GFunc)g_free, NULL);
  g_slist_free (lines);
  
 finished:
  if (f != NULL)
    fclose (f);
  
  g_free (logfile);
  g_free (logdir);
}

gboolean
gconfd_logfile_change_listener (GConfDatabase *db,
                                gboolean add,
                                guint connection_id,
                                ConfigListener listener,
                                const gchar *where,
                                GError **err)
{
  gchar *ior = NULL;
  gchar *quoted_db_name;
  gchar *quoted_where;
  gchar *quoted_ior;
  
  if (!open_append_handle (err))
    return FALSE;
  
  ior = gconf_object_to_string (listener, err);
  
  if (ior == NULL)
    return FALSE;

  quoted_ior = gconf_quote_string (ior);
  g_free (ior);
  ior = NULL;
  
  if (db == gconfd_lookup_database (NULL))
    quoted_db_name = gconf_quote_string ("def");
  else
    {
      const gchar *db_name;
      
      db_name = gconf_database_get_persistent_name (db);
      
      quoted_db_name = gconf_quote_string (db_name);
    }

  quoted_where = gconf_quote_string (where);

  /* KEEP IN SYNC with gconf-database.c log to string function */
  if (fprintf (append_handle, "%s %u %s %s %s\n",
               add ? "ADD" : "REMOVE", connection_id,
               quoted_db_name, quoted_where, quoted_ior) < 0)
    goto error;

  if (fflush (append_handle) < 0)
    goto error;

  g_free (quoted_db_name);
  g_free (quoted_ior);
  g_free (quoted_where);
  
  return TRUE;

 error:

  if (add)
    gconf_set_error (err,
                     GCONF_ERROR_FAILED,
                     _("Failed to log addition of listener to gconfd logfile; won't be able to re-add the listener if gconfd exits or shuts down (%s)"),
                     g_strerror (errno));
  else
    gconf_set_error (err,
                     GCONF_ERROR_FAILED,
                     _("Failed to log removal of listener to gconfd logfile; might erroneously re-add the listener if gconfd exits or shuts down (%s)"),
                     g_strerror (errno));

  g_free (quoted_db_name);
  g_free (quoted_ior);
  g_free (quoted_where);

  return FALSE;
}

static void
log_client_change (const ConfigListener client,
                   gboolean add)
{
  gchar *ior = NULL;
  gchar *quoted_ior = NULL;
  GError *err;
  
  err = NULL;
  ior = gconf_object_to_string (client, &err);

  if (err != NULL)
    {
      gconf_log (GCL_WARNING, _("Failed to get IOR for client: %s"),
                 err->message);
      g_error_free (err);
      return;
    }
      
  if (ior == NULL)
    return;

  quoted_ior = gconf_quote_string (ior);
  g_free (ior);
  ior = NULL;
  
  if (!open_append_handle (&err))
    {
      gconf_log (GCL_WARNING, _("Failed to open saved state file: %s"),
                 err->message);

      g_error_free (err);
      
      goto error;
    }

  /* KEEP IN SYNC with log to string function */
  if (fprintf (append_handle, "%s %s\n",
               add ? "CLIENTADD" : "CLIENTREMOVE", quoted_ior) < 0)
    {
      gconf_log (GCL_WARNING,
                 _("Failed to write client add to saved state file: %s"),
                 strerror (errno));
      goto error;
    }

  if (fflush (append_handle) < 0)
    {
      gconf_log (GCL_WARNING,
                 _("Failed to flush client add to saved state file: %s"),
                 strerror (errno));
      goto error;
    }

 error:
  g_free (ior);
  g_free (quoted_ior);
}

static void
log_client_add (const ConfigListener client)
{
  log_client_change (client, TRUE);
}

static void
log_client_remove (const ConfigListener client)
{
  log_client_change (client, FALSE);
}

/*
 * Client handling
 */

static GHashTable *client_table = NULL;

static void
add_client (const ConfigListener client)
{
  gconfd_need_log_cleanup ();
  
  if (client_table == NULL)
    client_table = g_hash_table_new ((GHashFunc) gconf_CORBA_Object_hash,
                                     (GCompareFunc) gconf_CORBA_Object_equal);

  if (g_hash_table_lookup (client_table, client))
    {
      /* Ignore this case; it happens normally when we added a client
       * from the logfile, and the client also adds itself
       * when it gets a new server objref.
       */
      return;
    }
  else
    {
      CORBA_Environment ev;
      ConfigListener copy;
      ORBitConnection *connection;
      
      CORBA_exception_init (&ev);
      copy = CORBA_Object_duplicate (client, &ev);
      g_hash_table_insert (client_table, copy, copy);
      CORBA_exception_free (&ev);

      /* Set maximum buffer size, which makes the connection nonblocking
       * if the kernel buffers are full and keeps gconfd from
       * locking up. Set the max to a pretty high number to avoid
       * dropping clients that are just stuck for a while.
       */
      connection = ORBit_small_get_connection (copy);
      ORBit_connection_set_max_buffer (connection, 1024 * 128);
      
      log_client_add (client);

      gconf_log (GCL_DEBUG, "Added a new client");
    }
}

static void
remove_client (const ConfigListener client)
{
  ConfigListener old_client;
  CORBA_Environment ev;

  gconfd_need_log_cleanup ();
  
  if (client_table == NULL)
    goto notfound;
  
  old_client = g_hash_table_lookup (client_table, 
                                    client);

  if (old_client == NULL)
    goto notfound;

  g_hash_table_remove (client_table,
                       old_client);

  log_client_remove (old_client);
  
  CORBA_exception_init (&ev);
  CORBA_Object_release (old_client, &ev);
  CORBA_exception_free (&ev);

  return;
  
 notfound:
  gconf_log (GCL_WARNING, _("Some client removed itself from the GConf server when it hadn't been added."));  
}

static void
hash_listify_func(gpointer key, gpointer value, gpointer user_data)
{
  GSList** list_p = user_data;

  *list_p = g_slist_prepend(*list_p, value);
}

static GSList*
list_clients (void)
{
  GSList *clients = NULL;

  if (client_table == NULL)
    return NULL;

  g_hash_table_foreach (client_table, hash_listify_func, &clients);

  return clients;
}

static void
log_clients_foreach (gpointer key, gpointer value, gpointer data)
{
  ConfigListener client;
  gchar *ior = NULL;
  gchar *quoted_ior = NULL;
  GError *err;

  client = value;
  
  err = NULL;
  ior = gconf_object_to_string (client, &err);

  if (err != NULL)
    {
      gconf_log (GCL_WARNING, _("Failed to get IOR for client: %s"),
                 err->message);
      g_error_free (err);
      return;
    }
      
  if (ior == NULL)
    return;

  quoted_ior = gconf_quote_string (ior);
  g_free (ior);
  ior = NULL;

  g_string_append (data, "CLIENTADD ");
  g_string_append (data, quoted_ior);
  g_string_append_c (data, '\n');
  g_free (quoted_ior);
}

static void
log_clients_to_string (GString *str)
{
  if (client_table == NULL)
    return;

  g_hash_table_foreach (client_table, log_clients_foreach, str);
}

void
gconfd_corba_drop_old_clients (void)
{
  GSList *clients;
  GSList *tmp;
  
  clients = list_clients ();

  if (clients)
    {
      CORBA_Environment ev;

      CORBA_exception_init (&ev);
      
      tmp = clients;
      while (tmp != NULL)
        {
          ConfigListener cl = tmp->data;
          CORBA_boolean result;

          result = CORBA_Object_non_existent (cl, &ev);
          
          if (ev._major != CORBA_NO_EXCEPTION)
            {
              gconf_log (GCL_WARNING, "Exception from CORBA_Object_non_existent(), assuming stale listener");
              CORBA_exception_free (&ev);
              CORBA_exception_init (&ev);
              result = TRUE;
            }

          if (result)
            {
              gconf_log (GCL_DEBUG, "removing stale client in drop_old_clients");
              
              remove_client (cl);
            }
          
          tmp = g_slist_next (tmp);
        }

      g_slist_free (clients);

      CORBA_exception_free (&ev);
    }
}

guint
gconfd_corba_client_count (void)
{
  if (client_table == NULL)
    return 0;
  else
    return g_hash_table_size (client_table);
}

gboolean
gconf_corba_set_exception(GError** error,
			  CORBA_Environment* ev)
{
  GConfError en;

  if (error == NULL)
    return FALSE;

  if (*error == NULL)
    return FALSE;
  
  en = (*error)->code;

  /* success is not supposed to get set */
  g_return_val_if_fail(en != GCONF_ERROR_SUCCESS, FALSE);
  
  {
    ConfigException* ce;

    ce = ConfigException__alloc();
    g_assert(error != NULL);
    g_assert(*error != NULL);
    g_assert((*error)->message != NULL);
    ce->message = CORBA_string_dup((gchar*)(*error)->message); /* cast const */
      
    switch (en)
      {
      case GCONF_ERROR_FAILED:
        ce->err_no = ConfigFailed;
        break;
      case GCONF_ERROR_NO_PERMISSION:
        ce->err_no = ConfigNoPermission;
        break;
      case GCONF_ERROR_BAD_ADDRESS:
        ce->err_no = ConfigBadAddress;
        break;
      case GCONF_ERROR_BAD_KEY:
        ce->err_no = ConfigBadKey;
        break;
      case GCONF_ERROR_PARSE_ERROR:
        ce->err_no = ConfigParseError;
        break;
      case GCONF_ERROR_CORRUPT:
        ce->err_no = ConfigCorrupt;
        break;
      case GCONF_ERROR_TYPE_MISMATCH:
        ce->err_no = ConfigTypeMismatch;
        break;
      case GCONF_ERROR_IS_DIR:
        ce->err_no = ConfigIsDir;
        break;
      case GCONF_ERROR_IS_KEY:
        ce->err_no = ConfigIsKey;
        break;
      case GCONF_ERROR_NO_WRITABLE_DATABASE:
        ce->err_no = ConfigNoWritableDatabase;        
        break;
      case GCONF_ERROR_IN_SHUTDOWN:
        ce->err_no = ConfigInShutdown;
        break;
      case GCONF_ERROR_OVERRIDDEN:
        ce->err_no = ConfigOverridden;
        break;
      case GCONF_ERROR_LOCK_FAILED:
        ce->err_no = ConfigLockFailed;
        break;

      case GCONF_ERROR_OAF_ERROR:
      case GCONF_ERROR_LOCAL_ENGINE:
      case GCONF_ERROR_NO_SERVER:
      case GCONF_ERROR_SUCCESS:
      default:
        gconf_log (GCL_ERR, "Unhandled error code %d", en);
        g_assert_not_reached();
        break;
      }

    CORBA_exception_set(ev, CORBA_USER_EXCEPTION,
                        ex_ConfigException, ce);

    gconf_log(GCL_DEBUG, _("Returning exception: %s"), (*error)->message);
      
    g_error_free(*error);
    *error = NULL;
      
    return TRUE;
  }
}

gboolean
gconfd_corba_check_in_shutdown (CORBA_Environment *ev)
{
  if (gconfd_in_shutdown ())
    {
      ConfigException* ce;
      
      ce = ConfigException__alloc();
      ce->message = CORBA_string_dup("config server is currently shutting down");
      ce->err_no = ConfigInShutdown;

      CORBA_exception_set(ev, CORBA_USER_EXCEPTION,
                          ex_ConfigException, ce);

      return TRUE;
    }
  else
    return FALSE;
}

GConfValue* 
gconf_value_from_corba_value(const ConfigValue* value)
{
  GConfValue* gval;
  GConfValueType type = GCONF_VALUE_INVALID;
  
  switch (value->_d)
    {
    case InvalidVal:
      return NULL;
      break;
    case IntVal:
      type = GCONF_VALUE_INT;
      break;
    case StringVal:
      type = GCONF_VALUE_STRING;
      break;
    case FloatVal:
      type = GCONF_VALUE_FLOAT;
      break;
    case BoolVal:
      type = GCONF_VALUE_BOOL;
      break;
    case SchemaVal:
      type = GCONF_VALUE_SCHEMA;
      break;
    case ListVal:
      type = GCONF_VALUE_LIST;
      break;
    case PairVal:
      type = GCONF_VALUE_PAIR;
      break;
    default:
      gconf_log(GCL_DEBUG, "Invalid type in %s", G_GNUC_FUNCTION);
      return NULL;
    }

  g_assert(GCONF_VALUE_TYPE_VALID(type));
  
  gval = gconf_value_new(type);

  switch (gval->type)
    {
    case GCONF_VALUE_INT:
      gconf_value_set_int(gval, value->_u.int_value);
      break;
    case GCONF_VALUE_STRING:
      if (!g_utf8_validate (value->_u.string_value, -1, NULL))
        {
          gconf_log (GCL_ERR, _("Invalid UTF-8 in string value in '%s'"),
                     value->_u.string_value); 
        }
      else
        {
          gconf_value_set_string(gval, value->_u.string_value);
        }
      break;
    case GCONF_VALUE_FLOAT:
      gconf_value_set_float(gval, value->_u.float_value);
      break;
    case GCONF_VALUE_BOOL:
      gconf_value_set_bool(gval, value->_u.bool_value);
      break;
    case GCONF_VALUE_SCHEMA:
      gconf_value_set_schema_nocopy(gval, 
                                    gconf_schema_from_corba_schema(&(value->_u.schema_value)));
      break;
    case GCONF_VALUE_LIST:
      {
        GSList* list = NULL;
        guint i = 0;
        
        switch (value->_u.list_value.list_type)
          {
          case BIntVal:
            gconf_value_set_list_type(gval, GCONF_VALUE_INT);
            break;
          case BBoolVal:
            gconf_value_set_list_type(gval, GCONF_VALUE_BOOL);
            break;
          case BFloatVal:
            gconf_value_set_list_type(gval, GCONF_VALUE_FLOAT);
            break;
          case BStringVal:
            gconf_value_set_list_type(gval, GCONF_VALUE_STRING);
            break;
          case BInvalidVal:
            break;
          default:
            g_warning("Bizarre list type in %s", G_GNUC_FUNCTION);
            break;
          }

        if (gconf_value_get_list_type(gval) != GCONF_VALUE_INVALID)
          {
            i = 0;
            while (i < value->_u.list_value.seq._length)
              {
                GConfValue* val;
                
                /* This is a bit dubious; we cast a ConfigBasicValue to ConfigValue
                   because they have the same initial members, but by the time
                   the CORBA and C specs kick in, not sure we are guaranteed
                   to be able to do this.
                */
                val = gconf_value_from_corba_value((ConfigValue*)&value->_u.list_value.seq._buffer[i]);
                
                if (val == NULL)
                  gconf_log(GCL_ERR, _("Couldn't interpret CORBA value for list element"));
                else if (val->type != gconf_value_get_list_type(gval))
                  gconf_log(GCL_ERR, _("Incorrect type for list element in %s"), G_GNUC_FUNCTION);
                else
                  list = g_slist_prepend(list, val);
                
                ++i;
              }
        
            list = g_slist_reverse(list);
            
            gconf_value_set_list_nocopy(gval, list);
          }
        else
          {
            gconf_log(GCL_ERR, _("Received list from gconfd with a bad list type"));
          }
      }
      break;
    case GCONF_VALUE_PAIR:
      {
        g_return_val_if_fail(value->_u.pair_value._length == 2, gval);
        
        gconf_value_set_car_nocopy(gval,
                                   gconf_value_from_corba_value((ConfigValue*)&value->_u.list_value.seq._buffer[0]));

        gconf_value_set_cdr_nocopy(gval,
                                   gconf_value_from_corba_value((ConfigValue*)&value->_u.list_value.seq._buffer[1]));
      }
      break;
    default:
      g_assert_not_reached();
      break;
    }
  
  return gval;
}

void          
gconf_fill_corba_value_from_gconf_value(const GConfValue *value, 
                                        ConfigValue      *cv)
{
  if (value == NULL)
    {
      cv->_d = InvalidVal;
      return;
    }

  switch (value->type)
    {
    case GCONF_VALUE_INT:
      cv->_d = IntVal;
      cv->_u.int_value = gconf_value_get_int(value);
      break;
    case GCONF_VALUE_STRING:
      cv->_d = StringVal;
      cv->_u.string_value = CORBA_string_dup((char*)gconf_value_get_string(value));
      break;
    case GCONF_VALUE_FLOAT:
      cv->_d = FloatVal;
      cv->_u.float_value = gconf_value_get_float(value);
      break;
    case GCONF_VALUE_BOOL:
      cv->_d = BoolVal;
      cv->_u.bool_value = gconf_value_get_bool(value);
      break;
    case GCONF_VALUE_SCHEMA:
      cv->_d = SchemaVal;
      gconf_fill_corba_schema_from_gconf_schema (gconf_value_get_schema(value),
                                                 &cv->_u.schema_value);
      break;
    case GCONF_VALUE_LIST:
      {
        guint n, i;
        GSList* list;
        
        cv->_d = ListVal;

        list = gconf_value_get_list(value);

        n = g_slist_length(list);

        cv->_u.list_value.seq._buffer =
          CORBA_sequence_ConfigBasicValue_allocbuf(n);
        cv->_u.list_value.seq._length = n;
        cv->_u.list_value.seq._maximum = n;
        CORBA_sequence_set_release(&cv->_u.list_value.seq, TRUE);
        
        switch (gconf_value_get_list_type(value))
          {
          case GCONF_VALUE_INT:
            cv->_u.list_value.list_type = BIntVal;
            break;

          case GCONF_VALUE_BOOL:
            cv->_u.list_value.list_type = BBoolVal;
            break;
            
          case GCONF_VALUE_STRING:
            cv->_u.list_value.list_type = BStringVal;
            break;

          case GCONF_VALUE_FLOAT:
            cv->_u.list_value.list_type = BFloatVal;
            break;

          case GCONF_VALUE_SCHEMA:
            cv->_u.list_value.list_type = BSchemaVal;
            break;
            
          default:
            cv->_u.list_value.list_type = BInvalidVal;
            gconf_log(GCL_DEBUG, "Invalid list type in %s", G_GNUC_FUNCTION);
            break;
          }
        
        i= 0;
        while (list != NULL)
          {
            /* That dubious ConfigBasicValue->ConfigValue cast again */
            gconf_fill_corba_value_from_gconf_value((GConfValue*)list->data,
                                                    (ConfigValue*)&cv->_u.list_value.seq._buffer[i]);

            list = g_slist_next(list);
            ++i;
          }
      }
      break;
    case GCONF_VALUE_PAIR:
      {
        cv->_d = PairVal;

        cv->_u.pair_value._buffer =
          CORBA_sequence_ConfigBasicValue_allocbuf(2);
        cv->_u.pair_value._length = 2;
        cv->_u.pair_value._maximum = 2;
        CORBA_sequence_set_release(&cv->_u.pair_value, TRUE);
        
        /* dubious cast */
        gconf_fill_corba_value_from_gconf_value (gconf_value_get_car(value),
                                                 (ConfigValue*)&cv->_u.pair_value._buffer[0]);
        gconf_fill_corba_value_from_gconf_value(gconf_value_get_cdr(value),
                                                (ConfigValue*)&cv->_u.pair_value._buffer[1]);
      }
      break;
      
    case GCONF_VALUE_INVALID:
      cv->_d = InvalidVal;
      break;
    default:
      cv->_d = InvalidVal;
      gconf_log(GCL_DEBUG, "Unknown type in %s", G_GNUC_FUNCTION);
      break;
    }
}

ConfigValue*  
gconf_corba_value_from_gconf_value (const GConfValue* value)
{
  ConfigValue* cv;

  cv = ConfigValue__alloc();

  gconf_fill_corba_value_from_gconf_value(value, cv);

  return cv;
}

ConfigValue*  
gconf_invalid_corba_value ()
{
  ConfigValue* cv;

  cv = ConfigValue__alloc();

  cv->_d = InvalidVal;

  return cv;
}

static ConfigValueType
corba_type_from_gconf_type(GConfValueType type)
{
  switch (type)
    {
    case GCONF_VALUE_INT:
      return IntVal;
    case GCONF_VALUE_BOOL:
      return BoolVal;
    case GCONF_VALUE_FLOAT:
      return FloatVal;
    case GCONF_VALUE_INVALID:
      return InvalidVal;
    case GCONF_VALUE_STRING:
      return StringVal;
    case GCONF_VALUE_SCHEMA:
      return SchemaVal;
    case GCONF_VALUE_LIST:
      return ListVal;
    case GCONF_VALUE_PAIR:
      return PairVal;
    default:
      g_assert_not_reached();
      return InvalidVal;
    }
}

static GConfValueType
gconf_type_from_corba_type(ConfigValueType type)
{
  switch (type)
    {
    case InvalidVal:
      return GCONF_VALUE_INVALID;
    case StringVal:
      return GCONF_VALUE_STRING;
    case IntVal:
      return GCONF_VALUE_INT;
    case FloatVal:
      return GCONF_VALUE_FLOAT;
    case SchemaVal:
      return GCONF_VALUE_SCHEMA;
    case BoolVal:
      return GCONF_VALUE_BOOL;
    case ListVal:
      return GCONF_VALUE_LIST;
    case PairVal:
      return GCONF_VALUE_PAIR;
    default:
      g_assert_not_reached();
      return GCONF_VALUE_INVALID;
    }
}

void          
gconf_fill_corba_schema_from_gconf_schema(const GConfSchema *sc, 
                                          ConfigSchema      *cs)
{
  cs->value_type = corba_type_from_gconf_type (gconf_schema_get_type (sc));
  cs->value_list_type = corba_type_from_gconf_type (gconf_schema_get_list_type (sc));
  cs->value_car_type = corba_type_from_gconf_type (gconf_schema_get_car_type (sc));
  cs->value_cdr_type = corba_type_from_gconf_type (gconf_schema_get_cdr_type (sc));

  cs->locale = CORBA_string_dup (gconf_schema_get_locale (sc) ? gconf_schema_get_locale (sc) : "");
  cs->short_desc = CORBA_string_dup (gconf_schema_get_short_desc (sc) ? gconf_schema_get_short_desc (sc) : "");
  cs->long_desc = CORBA_string_dup (gconf_schema_get_long_desc (sc) ? gconf_schema_get_long_desc (sc) : "");
  cs->owner = CORBA_string_dup (gconf_schema_get_owner (sc) ? gconf_schema_get_owner (sc) : "");

  {
    gchar* encoded;
    GConfValue* default_val;

    default_val = gconf_schema_get_default_value (sc);

    if (default_val)
      {
        encoded = gconf_value_encode (default_val);

        g_assert (encoded != NULL);

        cs->encoded_default_value = CORBA_string_dup (encoded);

        g_free (encoded);
      }
    else
      cs->encoded_default_value = CORBA_string_dup ("");
  }
}

ConfigSchema* 
gconf_corba_schema_from_gconf_schema (const GConfSchema* sc)
{
  ConfigSchema* cs;

  cs = ConfigSchema__alloc ();

  gconf_fill_corba_schema_from_gconf_schema (sc, cs);

  return cs;
}

GConfSchema*  
gconf_schema_from_corba_schema(const ConfigSchema* cs)
{
  GConfSchema* sc;
  GConfValueType type = GCONF_VALUE_INVALID;
  GConfValueType list_type = GCONF_VALUE_INVALID;
  GConfValueType car_type = GCONF_VALUE_INVALID;
  GConfValueType cdr_type = GCONF_VALUE_INVALID;

  type = gconf_type_from_corba_type(cs->value_type);
  list_type = gconf_type_from_corba_type(cs->value_list_type);
  car_type = gconf_type_from_corba_type(cs->value_car_type);
  cdr_type = gconf_type_from_corba_type(cs->value_cdr_type);

  sc = gconf_schema_new();

  gconf_schema_set_type(sc, type);
  gconf_schema_set_list_type(sc, list_type);
  gconf_schema_set_car_type(sc, car_type);
  gconf_schema_set_cdr_type(sc, cdr_type);

  if (*cs->locale != '\0')
    {
      if (!g_utf8_validate (cs->locale, -1, NULL))
        gconf_log (GCL_ERR, _("Invalid UTF-8 in locale for schema"));
      else
        gconf_schema_set_locale(sc, cs->locale);
    }

  if (*cs->short_desc != '\0')
    {
      if (!g_utf8_validate (cs->short_desc, -1, NULL))
        gconf_log (GCL_ERR, _("Invalid UTF-8 in short description for schema"));
      else
        gconf_schema_set_short_desc(sc, cs->short_desc);
    }

  if (*cs->long_desc != '\0')
    {
      if (!g_utf8_validate (cs->long_desc, -1, NULL))
        gconf_log (GCL_ERR, _("Invalid UTF-8 in long description for schema"));
      else
        gconf_schema_set_long_desc(sc, cs->long_desc);
    }

  if (*cs->owner != '\0')
    {
      if (!g_utf8_validate (cs->owner, -1, NULL))
        gconf_log (GCL_ERR, _("Invalid UTF-8 in owner for schema"));
      else
        gconf_schema_set_owner(sc, cs->owner);
    }
      
  {
    GConfValue* val;

    val = gconf_value_decode(cs->encoded_default_value);

    if (val)
      gconf_schema_set_default_value_nocopy(sc, val);
  }
  
  return sc;
}