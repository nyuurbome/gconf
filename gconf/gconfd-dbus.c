/* GConf
 * Copyright (C) 2003  CodeFactory AB
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

#include "gconfd-dbus.h"
#include "gconf-internals.h"
#include "gconf-locale.h"
#include "gconfd.h"
#include "gconf.h"
#include "gconf-dbus-utils.h"
#include <time.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static const char *config_server_messages[] = {
  GCONF_DBUS_CONFIG_SERVER_SHUTDOWN,
  GCONF_DBUS_CONFIG_SERVER_PING
};

static const char *config_database_messages[] = {
  GCONF_DBUS_CONFIG_DATABASE_DIR_EXISTS, 
  GCONF_DBUS_CONFIG_DATABASE_ALL_DIRS,
  GCONF_DBUS_CONFIG_DATABASE_ALL_ENTRIES,
  GCONF_DBUS_CONFIG_DATABASE_LOOKUP,
  GCONF_DBUS_CONFIG_DATABASE_LOOKUP_DEFAULT_VALUE,
  GCONF_DBUS_CONFIG_DATABASE_REMOVE_DIR,
  GCONF_DBUS_CONFIG_DATABASE_ADD_LISTENER,
  GCONF_DBUS_CONFIG_DATABASE_SET,
  GCONF_DBUS_CONFIG_DATABASE_RECURSIVE_UNSET,
  GCONF_DBUS_CONFIG_DATABASE_UNSET,
  GCONF_DBUS_CONFIG_DATABASE_SET_SCHEMA,
  GCONF_DBUS_CONFIG_DATABASE_SYNC,
  GCONF_DBUS_CONFIG_DATABASE_SYNCHRONOUS_SYNC,
  GCONF_DBUS_CONFIG_DATABASE_CLEAR_CACHE,
  GCONF_DBUS_CONFIG_DATABASE_REMOVE_LISTENER
};

static const char *lifecycle_messages[] = {
  DBUS_MESSAGE_SERVICE_DELETED,
};

typedef struct {
  GConfDatabaseListener parent;
  char *who;
} Listener;

static Listener* listener_new     (const char     *who,
				   const char     *name);
static void      listener_destroy (Listener       *l);
static void      add_client       (DBusConnection *connection,
				   const char     *name);


static void
gconfd_shutdown (DBusConnection *connection,
		 DBusMessage    *message)
{
  if (gconfd_dbus_check_in_shutdown (connection, message))
    return;

  gconf_log(GCL_DEBUG, _("Shutdown request received"));

  gconf_main_quit();
}

static void
gconfd_ping (DBusConnection *connection,
	     DBusMessage    *message)
{
  DBusMessage *reply;
  
  if (gconfd_dbus_check_in_shutdown (connection, message))
    return;

  reply = dbus_message_new_reply (message);
  dbus_message_append_args (reply,
			    DBUS_TYPE_UINT32, getpid (),
			    0);
  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (reply);
}

static DBusHandlerResult
gconfd_config_server_handler (DBusMessageHandler *handler,
			      DBusConnection     *connection,
			      DBusMessage        *message,
			      void               *user_data)
{
  if (dbus_message_name_is (message, GCONF_DBUS_CONFIG_SERVER_SHUTDOWN))
    {
      gconfd_shutdown (connection, message);
      
      return DBUS_HANDLER_RESULT_REMOVE_MESSAGE;
    }
  else if (dbus_message_name_is (message, GCONF_DBUS_CONFIG_SERVER_PING))
    {
      add_client (connection, dbus_message_get_sender (message));      
      gconfd_ping (connection, message);
    }
  return DBUS_HANDLER_RESULT_ALLOW_MORE_HANDLERS;
}

static gboolean
gconf_dbus_set_exception (DBusConnection *connection,
			  DBusMessage    *message,
			  GError        **error)
{
  GConfError en;
  const char *name = NULL;
  DBusMessage *reply;
  
  if (error == NULL || *error == NULL)
    return FALSE;

  en = (*error)->code;

  /* success is not supposed to get set */
  g_return_val_if_fail(en != GCONF_ERROR_SUCCESS, FALSE);

  switch (en)
    {
    case GCONF_ERROR_FAILED:
      name = GCONF_DBUS_ERROR_FAILED;
      break;
    case GCONF_ERROR_NO_PERMISSION:
      name = GCONF_DBUS_ERROR_NO_PERMISSION;
      break;
    case GCONF_ERROR_BAD_ADDRESS:
      name = GCONF_DBUS_ERROR_BAD_ADDRESS;
      break;
    case GCONF_ERROR_BAD_KEY:
      name = GCONF_DBUS_ERROR_BAD_KEY;
      break;
    case GCONF_ERROR_PARSE_ERROR:
      name = GCONF_DBUS_ERROR_PARSE_ERROR;
      break;
    case GCONF_ERROR_CORRUPT:
      name = GCONF_DBUS_ERROR_CORRUPT;
      break;
    case GCONF_ERROR_TYPE_MISMATCH:
      name = GCONF_DBUS_ERROR_TYPE_MISMATCH;
      break;
    case GCONF_ERROR_IS_DIR:
      name = GCONF_DBUS_ERROR_IS_DIR;
      break;
    case GCONF_ERROR_IS_KEY:
      name = GCONF_DBUS_ERROR_IS_KEY;
      break;
    case GCONF_ERROR_NO_WRITABLE_DATABASE:
      name = GCONF_DBUS_ERROR_NO_WRITABLE_DATABASE;
      break;
    case GCONF_ERROR_IN_SHUTDOWN:
      name = GCONF_DBUS_ERROR_IN_SHUTDOWN;
      break;
    case GCONF_ERROR_OVERRIDDEN:
      name = GCONF_DBUS_ERROR_OVERRIDDEN;
      break;
    case GCONF_ERROR_LOCK_FAILED:
      name = GCONF_DBUS_ERROR_LOCK_FAILED;
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

  reply = dbus_message_new_error_reply (message, name, (*error)->message);
  dbus_connection_send (connection, reply, NULL);  
  dbus_message_unref (reply);
  
  return TRUE;
}

static GConfDatabase *
gconf_database_from_id (DBusConnection *connection,
			DBusMessage    *message,
			guint           id)
{
  if (id == 0)
    return gconfd_lookup_database (NULL);
  else
    {
      DBusMessage *reply;

      reply = dbus_message_new_error_reply (message, GCONF_DBUS_ERROR_FAILED,
					    _("The database could not be accessed."));
      dbus_connection_send (connection, reply, NULL);
      dbus_message_unref (reply);
      return NULL;
    }
}

/* Convenience function that returns an error if the message is malformed */
static gboolean
gconf_dbus_get_message_args (DBusConnection *connection,
			     DBusMessage    *message,
			     int             first_arg_type,
			     ...)
{
  gboolean retval;
  va_list var_args;

  va_start (var_args, first_arg_type);
  retval = dbus_message_get_args_valist (message, NULL, first_arg_type, var_args);
  va_end (var_args);

  if (!retval)
    {
      DBusMessage *reply;
      
      reply = dbus_message_new_error_reply (message, GCONF_DBUS_ERROR_FAILED,
					    _("Got a malformed message."));
      dbus_connection_send (connection, reply, NULL);
      dbus_message_unref (reply);
      
      return FALSE;      
    }

  return TRUE;
}

static void
gconfd_config_database_dir_exists (DBusConnection *connection,
				   DBusMessage    *message)
{
  gboolean retval;
  int id;
  char *dir;
  GConfDatabase *db;
  DBusMessage *reply;
  GError *error = NULL;
  
  if (gconfd_dbus_check_in_shutdown (connection, message))
    return;

  if (!gconf_dbus_get_message_args (connection, message,
				    DBUS_TYPE_UINT32, &id,
				    DBUS_TYPE_STRING, &dir,
				    0))
      return;

  if (!(db = gconf_database_from_id (connection, message, id)))
    {
      dbus_free (dir);
      return;
    }

  retval =
    gconf_database_dir_exists (db, dir, &error);

  dbus_free (dir);
  
  if (gconf_dbus_set_exception (connection, message, &error))
    return;

  reply = dbus_message_new_reply (message);
  dbus_message_append_boolean (reply, retval);
  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (reply);
}

static void
gconfd_config_database_all_entries (DBusConnection *connection,
				    DBusMessage    *message)
{
  char *dir;
  int id;
  GConfDatabase *db;
  GConfLocaleList* locale_list;  
  DBusMessage *reply;
  GSList *pairs, *tmp;
  int i, len;
  GError* error = NULL;
  char *locale;
  char **keys;
  char **schema_names;
  unsigned char *is_defaults;
  unsigned char *is_writables;
  
  if (gconfd_dbus_check_in_shutdown (connection, message))
    return;
  
  if (!gconf_dbus_get_message_args (connection, message,
				    DBUS_TYPE_UINT32, &id,
				    DBUS_TYPE_STRING, &dir,
				    DBUS_TYPE_STRING, &locale,				    
				    0))
    return;
  
  if (!(db = gconf_database_from_id (connection, message, id)))
    {
      dbus_free (dir);
      return;
    }

  locale_list = gconfd_locale_cache_lookup (locale);
  pairs = gconf_database_all_entries(db, dir, locale_list->list, &error);

  dbus_free (dir);
  if (gconf_dbus_set_exception (connection, message, &error))
    return;

  len = g_slist_length(pairs);

  keys = g_new (char *, len + 1);
  keys[len] = NULL;

  schema_names = g_new (char *, len + 1);
  schema_names[len] = NULL;

  is_defaults = g_new (unsigned char, len);
  is_writables = g_new (unsigned char, len);  
    
  tmp = pairs;
  i = 0;
  while (tmp != NULL)
    {
      GConfEntry *p = tmp->data;

      g_assert(p != NULL);
      g_assert(p->key != NULL);

      schema_names[i] = g_strdup (gconf_entry_get_schema_name (p));
      if (!schema_names[i])
	schema_names[i] = g_strdup ("");
      
      keys[i] = g_strdup (p->key);
      is_defaults[i] = gconf_entry_get_is_default (p);
      is_writables[i] = gconf_entry_get_is_writable (p);
      ++i;

      tmp = tmp->next;
    }
  
  reply = dbus_message_new_reply (message);
  
  dbus_message_append_args (reply,
			    DBUS_TYPE_STRING_ARRAY, (const char **)keys, len,
			    DBUS_TYPE_STRING_ARRAY, (const char **)schema_names, len,
			    DBUS_TYPE_BOOLEAN_ARRAY, (const char **)is_defaults, len,
			    DBUS_TYPE_BOOLEAN_ARRAY, (const char **)is_writables, len,			    
			    0);
  
  g_strfreev (keys);
  g_strfreev (schema_names);
  g_free (is_defaults);
  g_free (is_writables);

  /* Now append the message values */
  tmp = pairs;
  i = 0;
  while (tmp != NULL)
    {
      GConfEntry *p = tmp->data;

      gconf_dbus_fill_message_from_gconf_value (reply, gconf_entry_get_value (p));

      g_assert(p != NULL);
      g_assert(p->key != NULL);

      gconf_entry_free (p);
      
      tmp = tmp->next;
    }
  g_slist_free(pairs);
  
  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (reply);
}

static void
gconfd_config_database_all_dirs (DBusConnection *connection,
				 DBusMessage    *message)
{
  char *dir;
  int id;
  GConfDatabase *db;
  DBusMessage *reply;
  GSList *subdirs, *tmp;
  int i, len;
  char **dirs;
  GError* error = NULL;
  
  if (gconfd_dbus_check_in_shutdown (connection, message))
    return;
  
  if (!gconf_dbus_get_message_args (connection, message,
				    DBUS_TYPE_UINT32, &id,
				    DBUS_TYPE_STRING, &dir,
				    0))
    return;
  
  if (!(db = gconf_database_from_id (connection, message, id)))
    {
      dbus_free (dir);
      return;
    }

  subdirs = gconf_database_all_dirs (db, dir, &error);
  dbus_free (dir);
  if (gconf_dbus_set_exception (connection, message, &error))
    return;  

  len = g_slist_length (subdirs);
  dirs = g_new (char *, len + 1);
  dirs[len] = NULL;
  
  tmp = subdirs;
  i = 0;

  while (tmp != NULL)
    {
      gchar *subdir = tmp->data;

      dirs[i] = subdir;

      ++i;
      tmp = tmp->next;
    }

  g_assert (i == len);
  g_slist_free (subdirs);
  
  reply = dbus_message_new_reply (message);
  dbus_message_append_string_array (reply, (const char **)dirs, len);
  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (reply);
  
  g_strfreev (dirs);
}

static void
gconfd_config_database_lookup (DBusConnection *connection,
			       DBusMessage    *message)
{
  char *key;
  char *locale;
  gboolean use_schema_default;
  int id;
  GConfDatabase *db;
  GConfLocaleList* locale_list;
  GConfValue *val;
  char *s;
  gboolean is_default = FALSE;
  gboolean is_writable = TRUE;
  GError* error = NULL;
  DBusMessage *reply;
  
  if (gconfd_dbus_check_in_shutdown (connection, message))
    return;

  if (!gconf_dbus_get_message_args (connection, message,
				    DBUS_TYPE_UINT32, &id,
				    DBUS_TYPE_STRING, &key,
				    DBUS_TYPE_STRING, &locale,
				    DBUS_TYPE_BOOLEAN, &use_schema_default,
				    0))
    return;

  if (!(db = gconf_database_from_id (connection, message, id)))
    {
      dbus_free (key);
      dbus_free (locale);
      return;
    }

  locale_list = gconfd_locale_cache_lookup (locale);

  s = NULL;  
  val = gconf_database_query_value (db, key, locale_list->list,
				    use_schema_default,
				    &s,
				    &is_default,
				    &is_writable,
				    &error);
  dbus_free (key);
  dbus_free (locale);
  
  gconf_log (GCL_DEBUG, "In lookup_with_schema_name returning schema name '%s' error '%s'",
             s, error ? error->message : "none");
  
  if (gconf_dbus_set_exception (connection, message, &error))
    {
      if (val)
	gconf_value_free (val);
      
      return;  
    }
  
  reply = dbus_message_new_reply (message);
  gconf_dbus_fill_message_from_gconf_value (reply, val);
  if (val)
    gconf_value_free (val);
  
  dbus_message_append_string (reply, s ? s : "");
  dbus_message_append_boolean (reply, is_default);
  dbus_message_append_boolean (reply, is_writable);
  
  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (reply);
}

static void
gconfd_config_database_lookup_default_value (DBusConnection *connection,
					     DBusMessage    *message)
{
  char *key, *locale;
  int id;
  GConfDatabase *db;
  GConfLocaleList* locale_list;
  GConfValue *val;
  GError* error = NULL;
  DBusMessage *reply;
  
  if (gconfd_dbus_check_in_shutdown (connection, message))
    return;

  if (!gconf_dbus_get_message_args (connection, message,
				    DBUS_TYPE_UINT32, &id,
				    DBUS_TYPE_STRING, &key,
				    DBUS_TYPE_STRING, &locale,
				    0))
    return;

  if (!(db = gconf_database_from_id (connection, message, id)))
    {
      dbus_free (key);
      dbus_free (locale);
      return;
    }

  locale_list = gconfd_locale_cache_lookup(locale);
  
  val = gconf_database_query_default_value(db, key,
                                           locale_list->list,
                                           NULL,
                                           &error);

  gconf_locale_list_unref(locale_list);

  if (gconf_dbus_set_exception (connection, message, &error))
    return;  

  reply = dbus_message_new_reply (message);
  gconf_dbus_fill_message_from_gconf_value (reply, val);
  gconf_value_free (val);
  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (reply);
}

static void
gconfd_config_database_remove_dir (DBusConnection *connection,
				   DBusMessage    *message)
{
  GError* error = NULL;
  GConfDatabase *db;
  DBusMessage *reply;
  char *dir;
  int id;

  if (gconfd_dbus_check_in_shutdown (connection, message))
    return;

  if (!gconf_dbus_get_message_args (connection, message,
				    DBUS_TYPE_UINT32, &id,
				    DBUS_TYPE_STRING, &dir,
				    0))
    return;

  if (!(db = gconf_database_from_id (connection, message, id)))
    {
      dbus_free (dir);
      return;
    }
  
  gconf_database_remove_dir(db, dir, &error);
  dbus_free (dir);

  if (gconf_dbus_set_exception (connection, message, &error))
    return;  

  /* This really sucks, but we need to ack that the removal was successful */
  reply = dbus_message_new_reply (message);
  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (reply);

}

static guint
gconf_database_dbus_add_listener (GConfDatabase *db,
				  const char    *who,
				  const char    *name,
				  const char    *where)
{
  guint cnxn;
  Listener *l;
  
  db->last_access = time(NULL);
  
  l = listener_new (who, name);

  cnxn = gconf_listeners_add (db->listeners, where, l,
			      (GFreeFunc)listener_destroy);

  if (l->parent.name == NULL)
    l->parent.name = g_strdup_printf ("%u", cnxn);

  gconf_log (GCL_DEBUG, "Added listener %s (%u)", l->parent.name, cnxn);
  
  return cnxn;
}


static void
gconfd_config_database_add_listener (DBusConnection *connection,
				     DBusMessage    *message)
{
  GConfDatabase *db;
  DBusMessage *reply;
  DBusDict *dict;
  char *dir;
  int id;
  int cnxn;
  const char *name = NULL;
  
  if (gconfd_dbus_check_in_shutdown (connection, message))
    return;

  if (!gconf_dbus_get_message_args (connection, message,
				    DBUS_TYPE_UINT32, &id,
				    DBUS_TYPE_STRING, &dir,
				    DBUS_TYPE_DICT, &dict,
				    0))
    return;
  
  if (!(db = gconf_database_from_id (connection, message, id)))
    {
      dbus_free (dir);
      dbus_dict_unref (dict);
      return;
    }

  dbus_dict_get_string (dict, "name", &name);

  cnxn = gconf_database_dbus_add_listener (db, dbus_message_get_sender (message), name, dir);
  
  reply = dbus_message_new_reply (message);
  dbus_message_append_uint32 (reply, cnxn);
  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (reply);
  dbus_dict_unref (dict);  
}

static void
gconfd_config_database_remove_listener (DBusConnection *connection,
					DBusMessage    *message)
{
  GConfDatabase *db;
  DBusMessage *reply;
  int id;
  int cnxn;
  
  if (gconfd_dbus_check_in_shutdown (connection, message))
    return;

  if (!gconf_dbus_get_message_args (connection, message,
				    DBUS_TYPE_UINT32, &id,
				    DBUS_TYPE_UINT32, &cnxn,
				    0))
    return;
  
  if (!(db = gconf_database_from_id (connection, message, id)))
      return;


  gconf_listeners_remove (db->listeners, cnxn);
  
  reply = dbus_message_new_reply (message);
  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (reply);
}

static void
gconfd_config_database_set (DBusConnection *connection,
			    DBusMessage    *message)
{
  GConfDatabase *db;
  DBusMessage *reply;
  char *key;
  int id;
  GConfValue *value;
  DBusMessageIter *iter;
  GError *error = NULL;
  
  if (gconfd_dbus_check_in_shutdown (connection, message))
    return;

  if (!gconf_dbus_get_message_args (connection, message,
				    DBUS_TYPE_UINT32, &id,
				    DBUS_TYPE_STRING, &key,
				    0))
    return;

  if (!(db = gconf_database_from_id (connection, message, id)))
    {
      dbus_free (key);
      return;
    }  
  
  iter = dbus_message_get_args_iter (message);
  dbus_message_iter_next (iter);
  dbus_message_iter_next (iter);
  
  value = gconf_dbus_create_gconf_value_from_message (iter);
  dbus_message_iter_unref (iter);

  gconf_database_set (db, key, value, &error);
  dbus_free (key);
  gconf_value_free (value);
  
  if (gconf_dbus_set_exception (connection, message, &error))
    return;

  /* This really sucks, but we need to ack that the setting was successful */
  reply = dbus_message_new_reply (message);
  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (reply);
}

static void
gconfd_config_database_recursive_unset (DBusConnection *connection,
					DBusMessage    *message)
{
  guint flags;
  int id;
  gchar *key;
  GConfDatabase *db;
  GConfUnsetFlags gconf_flags;
  DBusMessage *reply;
  GError *error = NULL;
  
  if (gconfd_dbus_check_in_shutdown (connection, message))
    return;

  if (!gconf_dbus_get_message_args (connection, message,
				    DBUS_TYPE_UINT32, &id,
				    DBUS_TYPE_STRING, &key,
				    DBUS_TYPE_UINT32, &flags,
				    0))
    return;

  if (!(db = gconf_database_from_id (connection, message, id)))
    {
      dbus_free (key);
      return;
    }
  
  gconf_flags = 0;
  if (flags & GCONF_DBUS_UNSET_INCLUDING_SCHEMA_NAMES)
    gconf_flags |= GCONF_DBUS_UNSET_INCLUDING_SCHEMA_NAMES;

  error = NULL;
  gconf_database_recursive_unset (db, key, NULL, gconf_flags, &error);

  if (gconf_dbus_set_exception (connection, message, &error))
    return;

  reply = dbus_message_new_reply (message);
  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (reply);
}

static void
gconfd_config_database_unset (DBusConnection *connection,
			      DBusMessage    *message)
{
  int id;
  gchar *key;
  GConfDatabase *db;
  DBusMessage *reply;
  GError *error = NULL;
  
  if (gconfd_dbus_check_in_shutdown (connection, message))
    return;

  if (!gconf_dbus_get_message_args (connection, message,
				    DBUS_TYPE_UINT32, &id,
				    DBUS_TYPE_STRING, &key,
				    0))
    return;

  if (!(db = gconf_database_from_id (connection, message, id)))
    {
      dbus_free (key);
      return;
    }
  
  gconf_database_unset (db, key, NULL, &error);
  
  if (gconf_dbus_set_exception (connection, message, &error))
    return;

  reply = dbus_message_new_reply (message);
  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (reply);
}

static void
gconfd_config_database_set_schema (DBusConnection *connection,
				   DBusMessage    *message)
{
  int id;
  gchar *key, *schema_key;
  GConfDatabase *db;
  DBusMessage *reply;
  GError *error = NULL;

  if (gconfd_dbus_check_in_shutdown (connection, message))
    return;

  if (!gconf_dbus_get_message_args (connection, message,
				    DBUS_TYPE_UINT32, &id,
				    DBUS_TYPE_STRING, &key,
				    DBUS_TYPE_STRING, &schema_key,
				    0))
    return;

  if (!(db = gconf_database_from_id (connection, message, id)))
    {
      dbus_free (key);
      return;
    }

  gconf_database_set_schema (db, key,
                             *schema_key != '\0' ?
                             schema_key : NULL,
                             &error);
  
  if (gconf_dbus_set_exception (connection, message, &error))
    return;

  reply = dbus_message_new_reply (message);
  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (reply);
}

static void
gconfd_config_database_synchronous_sync (DBusConnection *connection,
					 DBusMessage    *message)
{
  int id;
  GConfDatabase *db;
  DBusMessage *reply;
  GError *error = NULL;
  
  if (gconfd_dbus_check_in_shutdown (connection, message))
    return;

  if (!gconf_dbus_get_message_args (connection, message,
				    DBUS_TYPE_UINT32, &id,
				    0))
    return;

  if (!(db = gconf_database_from_id (connection, message, id)))
    {
      return;
    }

  gconf_database_synchronous_sync (db, &error);
  
  if (gconf_dbus_set_exception (connection, message, &error))
    return;

  reply = dbus_message_new_reply (message);
  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (reply);
}

static void
gconfd_config_database_sync (DBusConnection *connection,
			     DBusMessage    *message)
{
  int id;
  GConfDatabase *db;
  DBusMessage *reply;
  GError *error = NULL;

  if (gconfd_dbus_check_in_shutdown (connection, message))
    return;

  if (!gconf_dbus_get_message_args (connection, message,
				    DBUS_TYPE_UINT32, &id,
				    0))
    return;

  if (!(db = gconf_database_from_id (connection, message, id)))
    {
      return;
    }

  gconf_database_sync (db, &error);
  
  if (gconf_dbus_set_exception (connection, message, &error))
    return;

  reply = dbus_message_new_reply (message);
  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (reply);
}

static void
gconfd_config_database_clear_cache (DBusConnection *connection,
				    DBusMessage    *message)
{
  int id;
  GConfDatabase *db;
  DBusMessage *reply;
  GError *error = NULL;

  if (gconfd_dbus_check_in_shutdown (connection, message))
    return;

  if (!gconf_dbus_get_message_args (connection, message,
				    DBUS_TYPE_UINT32, &id,
				    0))
    return;

  if (!(db = gconf_database_from_id (connection, message, id)))
    {
      return;
    }

  gconf_database_clear_cache (db, &error);
  
  if (gconf_dbus_set_exception (connection, message, &error))
    return;

  reply = dbus_message_new_reply (message);
  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (reply);
}

static GHashTable *client_hash = NULL;

static void
add_client (DBusConnection *connection,
	    const char     *name)
{
  char *tmp;
  
  if (!client_hash)
    client_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  /* Check if client is in the hash already */
  if (g_hash_table_lookup (client_hash, name))
    return;

  tmp = g_strdup (name);
  g_hash_table_insert (client_hash, tmp, tmp);

  gconf_log (GCL_DEBUG, "Added a new client");  
}

guint
gconfd_dbus_client_count (void)
{
  return g_hash_table_size (client_hash);
}

static gboolean
remove_listener_predicate (const gchar* location,
			   guint        cnxn_id,
			   gpointer     listener_data,
			   gpointer     user_data)
{
  Listener *l = listener_data;
  const char *name = user_data;
  
  if (l->parent.type != GCONF_DATABASE_LISTENER_DBUS)
    return FALSE;

  if (strcmp (l->who, name) == 0)
    {
      return TRUE;
    }
  else
    return FALSE;
}

static void
remove_listeners (GConfDatabase *db, const char *name)
{
  if (db->listeners)
    {
      gconf_listeners_remove_if (db->listeners,
                                 remove_listener_predicate,
                                 (gpointer)name);
    }
}

static void
remove_client (DBusConnection *connection,
	       DBusMessage    *message)
{
  char *name;
  GList *list;
  GConfDatabase *db;

  if (!client_hash)
    return;
  
  dbus_message_get_args (message, NULL, 
			 DBUS_TYPE_STRING, &name,
			 0);

  /* Check if we know the client */
  if (g_hash_table_lookup (client_hash, name) == NULL)
    {
      dbus_free (name);
      return;
    }

  /* Now clean up after it */
  list = gconfd_get_database_list ();
  while (list)
    {
      db = list->data;
      remove_listeners (db, name);
      list = list->next;
    }

  /* Clean up the default database */
  db = gconfd_lookup_database (NULL);
  remove_listeners (db, name);
  g_hash_table_remove (client_hash, name);
  dbus_free (name);
}

static DBusHandlerResult
gconfd_lifecycle_handler (DBusMessageHandler *handler,
			  DBusConnection     *connection,
			  DBusMessage        *message,
			  void               *user_data)
{
  if (dbus_message_name_is (message, DBUS_MESSAGE_SERVICE_DELETED))
    remove_client (connection, message);

  return DBUS_HANDLER_RESULT_ALLOW_MORE_HANDLERS;
}

static DBusHandlerResult
gconfd_config_database_handler (DBusMessageHandler *handler,
				DBusConnection     *connection,
				DBusMessage        *message,
				void               *user_data)
{
  if (dbus_message_name_is (message, GCONF_DBUS_CONFIG_DATABASE_DIR_EXISTS))
    {
      add_client (connection, dbus_message_get_sender (message));
      gconfd_config_database_dir_exists (connection, message);
      return DBUS_HANDLER_RESULT_REMOVE_MESSAGE;
    }
  else if (dbus_message_name_is (message, GCONF_DBUS_CONFIG_DATABASE_ALL_DIRS))
    {
      add_client (connection, dbus_message_get_sender (message));      
      gconfd_config_database_all_dirs (connection, message);
      return DBUS_HANDLER_RESULT_REMOVE_MESSAGE;
    }
  else if (dbus_message_name_is (message, GCONF_DBUS_CONFIG_DATABASE_ALL_ENTRIES))
    {
      add_client (connection, dbus_message_get_sender (message));      
      gconfd_config_database_all_entries (connection, message);
      return DBUS_HANDLER_RESULT_REMOVE_MESSAGE;
    }
  else if (dbus_message_name_is (message, GCONF_DBUS_CONFIG_DATABASE_LOOKUP))
    {
      add_client (connection, dbus_message_get_sender (message));      
      gconfd_config_database_lookup (connection, message);
      return DBUS_HANDLER_RESULT_REMOVE_MESSAGE;
    }
  else if (dbus_message_name_is (message, GCONF_DBUS_CONFIG_DATABASE_LOOKUP_DEFAULT_VALUE))
    {
      add_client (connection, dbus_message_get_sender (message));      
      gconfd_config_database_lookup_default_value (connection, message);
      return DBUS_HANDLER_RESULT_REMOVE_MESSAGE;
    }
  else if (dbus_message_name_is (message, GCONF_DBUS_CONFIG_DATABASE_REMOVE_DIR))
    {
      add_client (connection, dbus_message_get_sender (message));      
      gconfd_config_database_remove_dir (connection, message);
      return DBUS_HANDLER_RESULT_REMOVE_MESSAGE;
    }
  else if (dbus_message_name_is (message, GCONF_DBUS_CONFIG_DATABASE_ADD_LISTENER))
    {
      add_client (connection, dbus_message_get_sender (message));      
      gconfd_config_database_add_listener (connection, message);
      return DBUS_HANDLER_RESULT_REMOVE_MESSAGE;
    }
  else if (dbus_message_name_is (message, GCONF_DBUS_CONFIG_DATABASE_REMOVE_LISTENER))
    {
      add_client (connection, dbus_message_get_sender (message));      
      gconfd_config_database_remove_listener (connection, message);
      return DBUS_HANDLER_RESULT_REMOVE_MESSAGE;
    }
  else if (dbus_message_name_is (message, GCONF_DBUS_CONFIG_DATABASE_SET))
    {
      add_client (connection, dbus_message_get_sender (message));      
      gconfd_config_database_set (connection, message);
      return DBUS_HANDLER_RESULT_REMOVE_MESSAGE;
    }
  else if (dbus_message_name_is (message, GCONF_DBUS_CONFIG_DATABASE_RECURSIVE_UNSET))
    {
      add_client (connection, dbus_message_get_sender (message));      
      gconfd_config_database_recursive_unset (connection, message);
      return DBUS_HANDLER_RESULT_REMOVE_MESSAGE;
    }
  else if (dbus_message_name_is (message, GCONF_DBUS_CONFIG_DATABASE_UNSET))
    {
      add_client (connection, dbus_message_get_sender (message));      
      gconfd_config_database_unset (connection, message);
      return DBUS_HANDLER_RESULT_REMOVE_MESSAGE;
    }
  else if (dbus_message_name_is (message, GCONF_DBUS_CONFIG_DATABASE_SET_SCHEMA))
    {
      add_client (connection, dbus_message_get_sender (message));      
      gconfd_config_database_set_schema (connection, message);
      return DBUS_HANDLER_RESULT_REMOVE_MESSAGE;
    }
  else if (dbus_message_name_is (message, GCONF_DBUS_CONFIG_DATABASE_SYNC))
    {
      add_client (connection, dbus_message_get_sender (message));      
      gconfd_config_database_sync (connection, message);
      return DBUS_HANDLER_RESULT_REMOVE_MESSAGE;
    }
  else if (dbus_message_name_is (message, GCONF_DBUS_CONFIG_DATABASE_SYNCHRONOUS_SYNC))
    {
      add_client (connection, dbus_message_get_sender (message));      
      gconfd_config_database_synchronous_sync (connection, message);
      return DBUS_HANDLER_RESULT_REMOVE_MESSAGE;
    }
  else if (dbus_message_name_is (message, GCONF_DBUS_CONFIG_DATABASE_CLEAR_CACHE))
    {
      add_client (connection, dbus_message_get_sender (message));      
      gconfd_config_database_clear_cache (connection, message);
      return DBUS_HANDLER_RESULT_REMOVE_MESSAGE;
    }
  
  return DBUS_HANDLER_RESULT_ALLOW_MORE_HANDLERS;
}

static const char *
get_dbus_address (void)
{
  /* FIXME: Change this when we know how to find the message bus. */
  return g_getenv ("DBUS_ADDRESS");
}

static DBusConnection *dbus_conn = NULL;

gboolean
gconfd_dbus_init (void)
{
  const char *dbus_address;
  DBusResultCode result;
  DBusMessageHandler *handler;
  GError *gerror = NULL;
  DBusError error;

  /* dbus_error_init (&error);*/

  dbus_conn = dbus_bus_get_with_g_main (DBUS_BUS_SESSION, &gerror);
  if (dbus_conn == NULL) {
    gconf_log (GCL_ERR, _("Failed to connect to the D-BUS session bus: %s",
			  gerror->message));
    return FALSE;
  }

  if (!dbus_connection_register_object_path (dbus_conn,
					     server_path,
					     &server_vtable,
					     NULL)) {
    gconf_log (GCL_ERR, _("Failed to register server object"));
    return FALSE;
  }

  dbus_error_init (&error);
  result = dbus_bus_acquire_service (dbus_conn,
				     GCONF_SERVICE_NAME,
				     0, &error);
  if (dbus_error_is_set (&error)) {
    gconf_log (GCL_ERR, _("Failed to acquire resource"));
    dbus_error_free (&error);
    return FALSE;
  }

  /* FIXME: Work from here! */
#if 0
  /* Add the config server handler */
  handler = dbus_message_handler_new (gconfd_config_server_handler, NULL, NULL);
  dbus_connection_register_handler (dbus_conn, handler, config_server_messages,
				    G_N_ELEMENTS (config_server_messages));

  /* Add the config database handler */
  handler = dbus_message_handler_new (gconfd_config_database_handler, NULL, NULL);
  dbus_connection_register_handler (dbus_conn, handler, config_database_messages,
				    G_N_ELEMENTS (config_database_messages));

  /* Add the lifecycle handler */
  handler = dbus_message_handler_new (gconfd_lifecycle_handler, NULL, NULL);
  dbus_connection_register_handler (dbus_conn, handler, lifecycle_messages,
				    G_N_ELEMENTS (lifecycle_messages));
#endif
  
  return TRUE;
}

gboolean
gconfd_dbus_check_in_shutdown (DBusConnection *connection,
			       DBusMessage    *message)
{
  if (gconfd_in_shutdown ())
    {
      DBusMessage *reply;
      
      reply = dbus_message_new_error_reply (message, GCONF_DBUS_ERROR_IN_SHUTDOWN,
					    _("The GConf daemon is currently shutting down."));
      dbus_connection_send (connection, reply, NULL);  
      dbus_message_unref (reply);
      
      return TRUE;
    }
  else
    return FALSE;
}

typedef struct {
  GConfDatabase* db;
  const GConfValue *value;
  gboolean is_default;
  gboolean is_writable;
} ListenerNotifyClosure;

static void
notify_listeners_cb(GConfListeners* listeners,
                    const gchar* all_above_key,
                    guint cnxn_id,
                    gpointer listener_data,
                    gpointer user_data)
{
  Listener* l = listener_data;
  ListenerNotifyClosure* closure = user_data;
  DBusMessage *message;
  
  if (l->parent.type != GCONF_DATABASE_LISTENER_DBUS)
    return;

  message = dbus_message_new (l->who, GCONF_DBUS_CONFIG_LISTENER_NOTIFY);

  dbus_message_append_args (message,
			    DBUS_TYPE_UINT32, 0, /* We only support the default database for now */
			    DBUS_TYPE_UINT32, cnxn_id,
			    DBUS_TYPE_STRING, all_above_key,
			    DBUS_TYPE_BOOLEAN, closure->is_default,
			    DBUS_TYPE_BOOLEAN, closure->is_writable,
			    0);

  gconf_dbus_fill_message_from_gconf_value (message, closure->value);

  dbus_connection_send (dbus_conn, message, NULL);
  dbus_message_unref (message);
}

void
gconf_database_dbus_notify_listeners (GConfDatabase    *db,
				      const gchar      *key,
				      const GConfValue *value,
				      gboolean          is_default,
				      gboolean          is_writable)
{
  ListenerNotifyClosure closure;

  closure.db = db;
  closure.value = value;
  closure.is_default = is_default;
  closure.is_writable = is_writable;

  gconf_listeners_notify (db->listeners, key, notify_listeners_cb, &closure);
}

/*
 * The listener object
 */

static Listener* 
listener_new (const char *who,
              const char *name)
{
  Listener* l;
  l = g_new0 (Listener, 1);

  l->who = g_strdup (who);
  l->parent.name = g_strdup (name);
  l->parent.type = GCONF_DATABASE_LISTENER_DBUS;
  
  return l;
}

static void      
listener_destroy (Listener* l)
{  
  g_free (l->parent.name);
  g_free (l->who);
  g_free (l);
}

