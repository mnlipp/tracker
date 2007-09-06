/* Tracker - indexer and metadata database engine
 * Copyright (C) 2006, Mr Jamie McCracken (jamiemcc@gnome.org)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#define DBUS_API_SUBJECT_TO_CHANGE
#define I_AM_MAIN

#include <signal.h>
#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>

#include "config.h"

#ifdef IOPRIO_SUPPORT
#include "tracker-ioprio.h"
#endif

#ifdef HAVE_INOTIFY
#   include "tracker-inotify.h"
#else
#   ifdef HAVE_FAM
#      include "tracker-fam.h"
#   endif
#endif

#ifndef HAVE_INOTIFY
#   ifndef HAVE_FAM
#      ifndef OS_WIN32
#         define POLL_ONLY
#      endif
#   endif
#endif


#include "tracker-dbus-methods.h"
#include "tracker-dbus-metadata.h"
#include "tracker-dbus-keywords.h"
#include "tracker-dbus-search.h"
#include "tracker-dbus-files.h"
#include "tracker-email.h"
#include "tracker-cache.h"
#include "tracker-indexer.h"

#include "tracker-os-dependant.h"
  
#ifdef OS_WIN32
#include <windows.h>
#include <pthread.h>
#include "mingw-compat.h"
#endif

#include "tracker-apps.h"

typedef struct {
	gchar	*uri;
	gint	mtime;
} IndexDir;

Tracker		       *tracker;
DBConnection	       *main_thread_db_con;
DBConnection	       *main_thread_cache_con;




static DBusConnection  *main_connection;

/*
 *   The workflow to process files and notified file change events are as follows:
 *
 *   1) File scan or file notification change (as reported by FAM/iNotify).
 *   2) File Scheduler (we wait until a file's changes have stabilised (NB not neccesary with inotify))
 *   3) We process a file's basic metadata (stat) and determine what needs doing in a seperate thread.
 *   4) We extract CPU intensive embedded metadata/text/thumbnail in another thread and save changes to the DB
 *
 *
 *  Three threads are used to fully process a file event. Files or events to be processed are placed on
 *  asynchronous queues where another thread takes over the work.
 *
 *  The main thread is very lightweight and no cpu intensive or heavy file I/O (or heavy DB access) is permitted
 *  here after initialisation of the daemon. This ensures the main thread can handle events and DBUS
 *  requests in a timely low latency manner.
 *
 *  The File Process thread is for moderate CPU intensive load and I/O and involves calls to stat()
 *  and simple fast queries on the DB. The main thread queues files to be processed by this thread
 *  via the file_process async queue. As no heavily CPU intensive activity occurs here, we can quickly
 *  keep the DB representation of the watched file system up to date. Once a file has been processed
 *  here it is then placed on the file metadata queue which is handled by the File Metadata thread.
 *
 *  The File Metadata thread is a low priority thread to handle the highly CPU intensive parts.
 *  During this phase, embedded metadata is extracted from files and if a text filter and/or
 *  thumbnailer is available for the mime type of the file then these will be spawned synchronously.
 *  Finally all metadata (including file's text contents and path to thumbnails) is saved to the DB.
 *
 *  All responses including user initiated requests are queued by the main thread onto an
 *  asynchronous queue where potentially multiple threads are waiting to process them.
 */


gchar *type_array[] =   {"index", "string", "numeric", "date", NULL};


static void schedule_file_check (const gchar *uri, DBConnection *db_con);

static void delete_directory (DBConnection *db_con, FileInfo *info);

static void delete_file (DBConnection *db_con, FileInfo *info);

static void scan_directory (const gchar *uri, DBConnection *db_con);


#ifdef POLL_ONLY

 gboolean 	tracker_start_watching 		(void){tracker->watch_limit = 0; return TRUE;}
 void     	tracker_end_watching 		(void){return;}

 gboolean 	tracker_add_watch_dir 			(const gchar *dir, DBConnection *db_con){return FALSE;}
 void     	tracker_remove_watch_dir 		(const gchar *dir, gboolean delete_subdirs, DBConnection *db_con) {return;}
 gboolean 	tracker_is_directory_watched 		(const gchar *dir, DBConnection *db_con) {return FALSE;}
 int		tracker_count_watch_dirs 		(void) {return 0;}


#endif /* POLL_ONLY */

static gchar **no_watch_dirs = NULL;
static gchar **watch_dirs = NULL;
static gchar **crawl_dirs = NULL;
static gchar *language = NULL;
static gboolean disable_indexing = FALSE;
static gboolean reindex = FALSE;
static gboolean fatal_errors = FALSE;
static gboolean low_memory, enable_evolution, enable_thunderbird, enable_kmail;
static gint throttle = 0;
static gint verbosity = 0;
static gint initial_sleep = -1; /* >= 0 is valid and will be set */

static GOptionEntry entries[] = {
	{"exclude-dir", 'e', 0, G_OPTION_ARG_STRING_ARRAY, &no_watch_dirs, N_("Directory to exclude from indexing"), N_("/PATH/DIR")},
	{"include-dir", 'i', 0, G_OPTION_ARG_STRING_ARRAY, &watch_dirs, N_("Directory to include in indexing"), N_("/PATH/DIR")},
	{"crawl-dir", 'c', 0, G_OPTION_ARG_STRING_ARRAY, &crawl_dirs, N_("Directory to crawl for indexing at start up only"), N_("/PATH/DIR")},
	{"no-indexing", 'n', 0, G_OPTION_ARG_NONE, &disable_indexing, N_("Disable any indexing or watching taking place"), NULL },
	{"verbosity", 'v', 0, G_OPTION_ARG_INT, &verbosity, N_("Value that controls the level of logging. Valid values are 0 (displays/logs only errors), 1 (minimal), 2 (detailed), and 3 (debug)"), N_("VALUE") },
	{"throttle", 't', 0, G_OPTION_ARG_INT, &throttle, N_("Value to use for throttling indexing. Value must be in range 0-20 (default 0) with lower values increasing indexing speed"), N_("VALUE") },
	{"low-memory", 'm', 0, G_OPTION_ARG_NONE, &low_memory, N_("Minimizes the use of memory but may slow indexing down"), NULL },
	{"initial-sleep", 's', 0, G_OPTION_ARG_INT, &initial_sleep, N_("Initial sleep time, just before indexing, in seconds"), NULL },
	{"language", 'l', 0, G_OPTION_ARG_STRING, &language, N_("Language to use for stemmer and stop words list (ISO 639-1 2 characters code)"), N_("LANG")},
	{"reindex", 'R', 0, G_OPTION_ARG_NONE, &reindex, N_("Force a re-index of all content"), NULL },
	{"fatal-errors", 'f', 0, G_OPTION_ARG_NONE, &fatal_errors, N_("Make tracker errors fatal"), NULL },
	{NULL}
};


static void
my_yield (void)
{
#ifndef OS_WIN32
	while (g_main_context_iteration (NULL, FALSE)) {
		;
	}
#endif
}


static gint
get_update_count (DBConnection *db_con)
{

	gint  count = 0;
	gchar ***res = tracker_exec_proc (db_con, "GetUpdateCount", 0);

	if (res) {
		if (res[0] && res[0][0]) {
			count = atoi (res[0][0]);
		}
		tracker_db_free_result (res);
	}

	return count;
}


static void
set_update_count (DBConnection *db_con, gint count)
{
	gchar *str_count = tracker_int_to_str (count);
	tracker_exec_proc (db_con, "SetUpdateCount", 1, str_count);
	g_free (str_count);
}


static gboolean
do_cleanup (const gchar *sig_msg)
{
	tracker->status = STATUS_SHUTDOWN;

	if (tracker->log_file && sig_msg) {
		tracker_log ("Received signal '%s' so now shutting down", sig_msg);

		tracker_print_object_allocations ();
	}

	/* stop threads from further processing of events if possible */

	tracker->in_flush = TRUE;

	set_update_count (main_thread_db_con, tracker->update_count);

	/* wait for files thread to sleep */
	while (!g_mutex_trylock (tracker->files_signal_mutex)) {
		g_usleep (100);
	}

	g_mutex_unlock (tracker->files_signal_mutex);

	while (!g_mutex_trylock (tracker->metadata_signal_mutex)) {
		g_usleep (100);
	}

	g_mutex_unlock (tracker->metadata_signal_mutex);

	if (tracker->log_file) {
		tracker_log ("shutting down threads");
	}

	/* send signals to each thread to wake them up and then stop them */

	tracker->shutdown = TRUE;

	g_mutex_lock (tracker->request_signal_mutex);
	g_cond_signal (tracker->request_thread_signal);
	g_mutex_unlock (tracker->request_signal_mutex);

	g_mutex_lock (tracker->metadata_signal_mutex);
	g_cond_signal (tracker->metadata_thread_signal);
	g_mutex_unlock (tracker->metadata_signal_mutex);

	g_mutex_unlock (tracker->files_check_mutex);

	g_mutex_lock (tracker->files_signal_mutex);
	g_cond_signal (tracker->file_thread_signal);
	g_mutex_unlock (tracker->files_signal_mutex);


	/* wait for threads to exit and unlock check mutexes to prevent any deadlocks*/

	g_mutex_unlock (tracker->request_check_mutex);
	g_mutex_lock (tracker->request_stopped_mutex);

	g_mutex_unlock (tracker->metadata_check_mutex);
	g_mutex_lock (tracker->metadata_stopped_mutex);

	g_mutex_unlock (tracker->files_check_mutex);
	g_mutex_lock (tracker->files_stopped_mutex);

	tracker_email_end_email_watching ();

	tracker_db_close (main_thread_db_con);

	/* This must be called after all other db functions */
	tracker_db_finalize ();

	if (tracker->log_file) {
		tracker_debug ("shutting down main thread");
	}

	/* remove sys tmp directory */
	if (tracker->sys_tmp_root_dir) {
		tracker_remove_dirs (tracker->sys_tmp_root_dir);
	}

	g_main_loop_quit (tracker->loop);

	exit (EXIT_SUCCESS);

	return FALSE;
}


static void
check_dir_for_deletions (DBConnection *db_con, const gchar *uri)
{
	gchar **files, **files_p;

	/* check for any deletions*/
	files = tracker_db_get_files_in_folder (db_con, uri);

	for (files_p = files; *files_p; files_p++) {
		gchar *str = *files_p;

		if (!tracker_file_is_valid (str)) {
                        FileInfo *info;
			info = tracker_create_file_info (str, 1, 0, 0);
			info = tracker_db_get_file_info (db_con, info);

			if (!info->is_directory) {
				delete_file (db_con, info);
			} else {
				delete_directory (db_con, info);
			}
			tracker_free_file_info (info);
		}
	}

	g_strfreev (files);
}


static void
schedule_dir_check (const gchar *uri, DBConnection *db_con)
{
	if (!tracker->is_running) {
		return;
	}

	g_return_if_fail (tracker_check_uri (uri));
	g_return_if_fail (db_con);

	tracker_db_insert_pending_file (db_con, 0, uri, "unknown", 0, TRACKER_ACTION_DIRECTORY_REFRESH, TRUE, FALSE, -1);
}


static void
add_dirs_to_list (GSList *my_list, DBConnection *db_con)
{
	if (!tracker->is_running) {
		return;
	}

	GSList *dir_list = NULL, *lst;

	for (lst = my_list; lst; lst = lst->next) {
                gchar *dir = lst->data;
		if (dir) {
			dir_list = g_slist_prepend (dir_list, g_strdup (dir));
		}
	}

	/* add sub directories breadth first recursively to avoid running out of file handles */
	while (dir_list) {
                GSList *file_list = NULL, *tmp;

		for (tmp = dir_list; tmp; tmp = tmp->next) {
                        gchar *str = tmp->data;

			if (str && !tracker_file_is_no_watched (str)) {
				tracker->dir_list = g_slist_prepend (tracker->dir_list, g_strdup (str));
			}
		}

		g_slist_foreach (dir_list, (GFunc) tracker_get_dirs, &file_list);
		g_slist_foreach (dir_list, (GFunc) g_free, NULL);
		g_slist_free (dir_list);

		dir_list = file_list;
	}
}


static void
add_dirs_to_watch_list (GSList *dir_list, gboolean check_dirs, DBConnection *db_con)
{
	if (!tracker->is_running) {
		return;
	}

	g_return_if_fail (dir_list != NULL);

	/* add sub directories breadth first recursively to avoid running out of file handles */
	while (dir_list) {
                GSList *file_list = NULL, *tmp;

		for (tmp = dir_list; tmp; tmp = tmp->next) {
                        gchar *str = tmp->data;

			if (str && !tracker_file_is_no_watched (str)) {

				tracker->dir_list = g_slist_prepend (tracker->dir_list, g_strdup (str));

				if (tracker_file_is_crawled (str) || !tracker->enable_watching) {
                                        continue;
                                }

				if ( ((tracker_count_watch_dirs () + g_slist_length (dir_list)) < tracker->watch_limit)) {

					if (!tracker_add_watch_dir (str, db_con) && tracker_is_directory (str)) {
						tracker_debug ("Watch failed for %s", str);
					}
				}
			}
		}

		g_slist_foreach (dir_list, (GFunc) tracker_get_dirs, &file_list);
		g_slist_foreach (dir_list, (GFunc) g_free, NULL);
		g_slist_free (dir_list);

		dir_list = file_list;
	}
}


static gboolean
watch_dir (const gchar* dir, DBConnection *db_con)
{
	gchar *dir_utf8;

	if (!tracker->is_running) {
		return TRUE;
	}

	if (!dir) {
		return FALSE;
	}

	if (!g_utf8_validate (dir, -1, NULL)) {

		dir_utf8 = g_filename_to_utf8 (dir, -1, NULL,NULL,NULL);
		if (!dir_utf8) {
			tracker_error ("ERROR: watch_dir could not be converted to utf8 format");
			return FALSE;
		}
	} else {
		dir_utf8 = g_strdup (dir);
	}

	if (!tracker_file_is_valid (dir_utf8)) {
		g_free (dir_utf8);
		return FALSE;
	}

	if (!tracker_is_directory (dir_utf8)) {
		g_free (dir_utf8);
		return FALSE;
	}

	if (tracker_file_is_crawled (dir_utf8)) {
		return FALSE;
	}

	if (!tracker_file_is_no_watched (dir_utf8)) {
		GSList *mylist = NULL;

		mylist = g_slist_prepend (mylist, dir_utf8);

		add_dirs_to_watch_list (mylist, TRUE, db_con);
	}

	return TRUE;
}


static void
signal_handler (int signo)
{
	if (!tracker->is_running) {
		return;
	}

	static gboolean in_loop = FALSE;

  	/* avoid re-entrant signals handler calls */
	if (in_loop) {
		return;
	}

  	in_loop = TRUE;

  	switch (signo) {

  		case SIGSEGV:
	  	case SIGBUS:
		case SIGILL:
  		case SIGFPE:
  		case SIGPIPE:
		case SIGABRT:

			tracker->is_running = FALSE;
			tracker_end_watching ();

			g_timeout_add_full (G_PRIORITY_LOW,
			     		    1,
		 	    		    (GSourceFunc) do_cleanup,
			     		    g_strdup (g_strsignal (signo)), NULL
			   		    );

    			break;

		case SIGTERM:
		case SIGINT:

			tracker->is_running = FALSE;
			tracker_end_watching ();

			g_timeout_add_full (G_PRIORITY_LOW,
			     		    1,
		 	    		    (GSourceFunc) do_cleanup,
			     		    g_strdup (g_strsignal (signo)), NULL
			   		    );

			break;

		default:
			if (tracker->log_file && g_strsignal (signo)) {
	   			tracker_log ("Received signal %s ", g_strsignal (signo));
			}
			in_loop = FALSE;
    			break;
  	}
}


static void
delete_file (DBConnection *db_con, FileInfo *info)
{
	/* info struct may have been deleted in transit here so check if still valid and intact */
	g_return_if_fail (tracker_file_info_is_valid (info));

	/* if we dont have an entry in the db for the deleted file, we ignore it */
	if (info->file_id == 0) {
		return;
	}

	tracker_db_delete_file (db_con, info->file_id);

	tracker_log ("deleting file %s", info->uri);
}


static void
delete_directory (DBConnection *db_con, FileInfo *info)
{
	/* info struct may have been deleted in transit here so check if still valid and intact */
	g_return_if_fail (tracker_file_info_is_valid (info));

	/* if we dont have an entry in the db for the deleted directory, we ignore it */
	if (info->file_id == 0) {
		return;
	}

	tracker_db_delete_directory (db_con, info->file_id, info->uri);

	tracker_remove_watch_dir (info->uri, TRUE, db_con);

	tracker_log ("deleting directory %s and subdirs", info->uri);
}


static void
schedule_file_check (const gchar *uri, DBConnection *db_con)
{
	if (!tracker->is_running) {
		return;
	}

	g_return_if_fail (tracker_check_uri (uri));
	g_return_if_fail (db_con);

	/* keep mainloop responsive */
	my_yield ();

	if (!tracker_is_directory (uri)) {
		tracker_db_insert_pending_file (db_con, 0, uri, "unknown", 0, TRACKER_ACTION_CHECK, 0, FALSE, -1);
	} else {
		schedule_dir_check (uri, db_con);
	}
}


static inline void
queue_dir (const gchar *uri)
{
	FileInfo *info = tracker_create_file_info (uri, TRACKER_ACTION_DIRECTORY_CHECK, 0, 0);
	g_async_queue_push (tracker->file_process_queue, info);
}


static inline void
queue_file (const gchar *uri)
{
	FileInfo *info = tracker_create_file_info (uri, TRACKER_ACTION_CHECK, 0, 0);
	g_async_queue_push (tracker->file_process_queue, info);
}


static void
check_directory (const gchar *uri)
{
	GSList *file_list = NULL;

	if (!tracker->is_running) {
		return;
	}

	g_return_if_fail (tracker_check_uri (uri));
	g_return_if_fail (tracker_is_directory (uri));

	file_list = tracker_get_files (uri, FALSE);
	tracker_debug ("checking %s for %d files", uri, g_slist_length (file_list));

	g_slist_foreach (file_list, (GFunc) queue_file, NULL);
	g_slist_foreach (file_list, (GFunc) g_free, NULL);
	g_slist_free (file_list);

	queue_file (uri);
}



static void
scan_directory (const gchar *uri, DBConnection *db_con)
{
	GSList *file_list;

	if (!tracker->is_running) {
		return;
	}

	g_return_if_fail (db_con);
	g_return_if_fail (tracker_check_uri (uri));
	g_return_if_fail (tracker_is_directory (uri));

	/* keep mainloop responsive */
	my_yield ();

	file_list = tracker_get_files (uri, FALSE);
	tracker_debug ("scanning %s for %d files", uri, g_slist_length(file_list));

	g_slist_foreach (file_list, (GFunc) schedule_file_check, db_con);
	g_slist_foreach (file_list, (GFunc) g_free, NULL);
	g_slist_free (file_list);

	/* recheck directory to update its mtime if its changed whilst scanning */
	schedule_dir_check (uri, db_con);
	tracker_debug ("finished scanning");
}


/* determines whether an action applies to a file or a directory */
static void
verify_action (FileInfo *info)
{
	if (info->action == TRACKER_ACTION_CHECK) {
		if (info->is_directory) {
			info->action = TRACKER_ACTION_DIRECTORY_CHECK;
			info->counter = 0;
		} else {
			info->action = TRACKER_ACTION_FILE_CHECK;
		}

	} else {
		if (info->action == TRACKER_ACTION_DELETE || info->action == TRACKER_ACTION_DELETE_SELF) {

			/* we are in trouble if we cant find the deleted uri in the DB - assume its a directory (worst case) */
			if (info->file_id == 0) {
				info->is_directory = TRUE;
			}

			info->counter = 0;
			if (info->is_directory) {
				info->action = TRACKER_ACTION_DIRECTORY_DELETED;
			} else {
				info->action = TRACKER_ACTION_FILE_DELETED;
			}
		} else {
			if (info->action == TRACKER_ACTION_MOVED_FROM) {
				info->counter = 1;
				if (info->is_directory) {
					info->action = TRACKER_ACTION_DIRECTORY_MOVED_FROM;
				} else {
					info->action = TRACKER_ACTION_FILE_MOVED_FROM;
				}

			} else {

				if (info->action == TRACKER_ACTION_CREATE) {
					if (info->is_directory) {
						info->action = TRACKER_ACTION_DIRECTORY_CREATED;
						info->counter = 0; /* do not reschedule a created directory */
					} else {
						info->action = TRACKER_ACTION_FILE_CREATED;
					}

				} else {
					if (info->action == TRACKER_ACTION_FILE_MOVED_TO) {
						info->counter = 0;
						if (info->is_directory) {
							info->action = TRACKER_ACTION_DIRECTORY_MOVED_TO;
						} else {
							info->action = TRACKER_ACTION_FILE_MOVED_TO;
						}
					}
				}
			}
		}
	}
}


static void
index_entity (DBConnection *db_con, FileInfo *info)
{
	gchar *service_info;

	g_return_if_fail (info);
	g_return_if_fail (tracker_check_uri (info->uri));

	if (!tracker_file_is_valid (info->uri)) {
		//tracker_debug ("Warning - file %s in not valid or could not be read - abandoning index on this file", info->uri);
		return;
	}

	if (!info->is_directory) {
		/* sleep to throttle back indexing */
		tracker_throttle (1000);
	}

	service_info = tracker_get_service_for_uri (info->uri);

	if (!service_info) {
		tracker_error ("ERROR: cannot find service for path %s", info->uri);
		return;
	}

//	tracker_debug ("indexing %s with service %s", info->uri, service_info);

	gchar *str = g_utf8_strdown  (service_info, -1);

	ServiceDef *def = g_hash_table_lookup (tracker->service_table, str);

	g_free (str);

	if (!def) {
		if (service_info) {
			tracker_error ("ERROR: unknown service %s", service_info);
		} else {
			tracker_error ("ERROR: unknown service");
		}
		g_free (service_info);
		return;
	}

	if (info->is_directory) {
		info->is_hidden = !def->show_service_directories;
		tracker_db_index_file (db_con, info, NULL, NULL);
		g_free (service_info);
		return;

	} else {
		info->is_hidden = !def->show_service_files;
	}


	if (g_str_has_suffix (service_info, "Emails")) {

		if (!tracker_email_index_file (db_con->emails, info, service_info)) {

			g_free (service_info);
			return;
		}


	} else if (strcmp (service_info, "Files") == 0) {
		tracker_db_index_file (db_con, info, NULL, NULL);

	} else if (g_str_has_suffix (service_info, "Conversations")) {
		tracker_db_index_conversation (db_con, info);

	} else if (strcmp (service_info, "Applications") == 0) {
		tracker_db_index_application (db_con, info);

	} else {
		tracker_db_index_service (db_con, info, NULL, NULL, NULL, FALSE, TRUE, TRUE, TRUE);
	}

	g_free (service_info);
}


static inline void
process_directory_list (DBConnection *db_con, GSList *list, gboolean recurse)
{
	tracker->dir_list = NULL;

	if (!list) {
		return;
	}

	g_slist_foreach (list, (GFunc) watch_dir, db_con);

	g_slist_foreach (list, (GFunc) schedule_dir_check, db_con);

	if (recurse && tracker->dir_list) {
		g_slist_foreach (tracker->dir_list, (GFunc) schedule_dir_check, db_con);
	}

	if (tracker->dir_list) {
		g_slist_foreach (tracker->dir_list, (GFunc) g_free, NULL);
		g_slist_free (tracker->dir_list);
                tracker->dir_list = NULL;
	}
}


static void
process_files_thread (void)
{
	sigset_t     signal_set;

	DBConnection *db_con;

	GSList	     *moved_from_list; /* list to hold moved_from events whilst waiting for a matching moved_to event */
	gboolean pushed_events, first_run;

        /* block all signals in this thread */
        sigfillset (&signal_set);
#ifndef OS_WIN32
        pthread_sigmask (SIG_BLOCK, &signal_set, NULL);
#endif

	g_mutex_lock (tracker->files_signal_mutex);
	g_mutex_lock (tracker->files_stopped_mutex);

	/* set thread safe DB connection */
	tracker_db_thread_init ();

	db_con = tracker_db_connect_all (TRUE);

	tracker->index_status = INDEX_CONFIG;

	pushed_events = FALSE;

	first_run = TRUE;

	moved_from_list = NULL;

	tracker_log ("starting indexing...");
	tracker->status = STATUS_INDEXING;

	while (TRUE) {
		FileInfo *info;
		gboolean need_index;

		need_index = FALSE;

		/* make thread sleep if first part of the shutdown process has been activated */
		if (!tracker->is_running) {

			g_cond_wait (tracker->file_thread_signal, tracker->files_signal_mutex);

			/* determine if wake up call is new stuff or a shutdown signal */
			if (!tracker->shutdown) {
				continue;
			} else {

				if (tracker->dir_list) {
					g_slist_foreach (tracker->dir_list, (GFunc) g_free, NULL);
					g_slist_free (tracker->dir_list);
					tracker->dir_list = NULL;
				}
				break;
			}
		}

		tracker->status = STATUS_INDEXING;

		if (tracker->grace_period > 1) {
			tracker_log ("pausing indexing while non-tracker disk I/O is taking place");
			g_usleep (1000 * 1000);
			tracker->grace_period--;
			if (tracker->grace_period > 3) tracker->grace_period = 3;
			continue;
		}

		tracker_cache_flush (db_con);

		info = g_async_queue_try_pop (tracker->file_process_queue);

		/* check pending table if we haven't got anything */
		if (!info) {
			gchar ***res;
			gint  k;

			/* set mutex to indicate we are in "check" state */
			g_mutex_lock (tracker->files_check_mutex);


			if (tracker_db_has_pending_files (db_con)) {
				g_mutex_unlock (tracker->files_check_mutex);

			} else {

				/* check dir_queue in case there are directories waiting to be indexed */

				if (g_async_queue_length (tracker->dir_queue) > 0) {

					gchar *uri = g_async_queue_try_pop (tracker->dir_queue);

					if (uri) {

						check_directory (uri);

						g_free (uri);

						g_mutex_unlock (tracker->files_check_mutex);

						continue;
					}
				}

				if (tracker->index_status != INDEX_FINISHED) {

					g_mutex_unlock (tracker->files_check_mutex);

					switch (tracker->index_status) {


						case INDEX_CONFIG:
							tracker_log ("Starting config indexing");
							break;

						case INDEX_APPLICATIONS: {
								GSList *list;

								tracker_log ("Starting application indexing");

								tracker_applications_add_service_directories ();

								list = tracker_get_service_dirs ("Applications");

								tracker_add_root_directories (list);

								process_directory_list (db_con, list, FALSE);

								g_slist_free (list);
										
			

							}
							break;

						case INDEX_FILES: {

								/* sleep for N secs before watching/indexing any of the major services */
								if (tracker->initial_sleep > 0) {
									tracker_log ("Sleeping for %d secs...", tracker->initial_sleep);
									g_usleep (tracker->initial_sleep * 1000 * 1000);
								}

								tracker_log ("Starting file indexing...");
								tracker->dir_list = NULL;

								/* delete all stuff in the no watch dirs */

								if (tracker->no_watch_directory_list) {
									GSList *lst;

									tracker_log ("Deleting entities in no watch directories...");

									for (lst = tracker->no_watch_directory_list; lst; lst = lst->next) {
                                                                                gchar *no_watch_uri = lst->data;

										if (no_watch_uri) {
											guint32 f_id = tracker_db_get_file_id (db_con, no_watch_uri);

											if (f_id > 0) {
												tracker_db_delete_directory (db_con, f_id, no_watch_uri);
											}
										}
									}
								}

								if (!tracker->watch_directory_roots_list) {
									break;
								}

								tracker_add_root_directories (tracker->watch_directory_roots_list);

								/* index watched dirs first */
								g_slist_foreach (tracker->watch_directory_roots_list, (GFunc) watch_dir, db_con);

								g_slist_foreach (tracker->dir_list, (GFunc) schedule_dir_check, db_con);

								if (tracker->dir_list) {
									g_slist_foreach (tracker->dir_list, (GFunc) g_free, NULL);
									g_slist_free (tracker->dir_list);
									tracker->dir_list = NULL;
								}

								g_slist_foreach (tracker->watch_directory_roots_list, (GFunc) schedule_dir_check, db_con);

								if (tracker->dir_list) {
									g_slist_foreach (tracker->dir_list, (GFunc) g_free, NULL);
									g_slist_free (tracker->dir_list);
									tracker->dir_list = NULL;
								}
							}
							break;

						case INDEX_CRAWL_FILES: {
								tracker_log ("Indexing directories to be crawled...");
								tracker->dir_list = NULL;

								if (!tracker->crawl_directory_list) {
									break;
								}

								tracker_add_root_directories (tracker->crawl_directory_list);

								add_dirs_to_list (tracker->crawl_directory_list, db_con);

								g_slist_foreach (tracker->dir_list, (GFunc) schedule_dir_check, db_con);

								if (tracker->dir_list) {
									g_slist_foreach (tracker->dir_list, (GFunc) g_free, NULL);
									g_slist_free (tracker->dir_list);
									tracker->dir_list = NULL;
								}

								g_slist_foreach (tracker->crawl_directory_list, (GFunc) schedule_dir_check, db_con);

								if (tracker->dir_list) {
									g_slist_foreach (tracker->dir_list, (GFunc) g_free, NULL);
									g_slist_free (tracker->dir_list);
									tracker->dir_list = NULL;
								}
							}
							break;

						case INDEX_CONVERSATIONS: {
								gchar    *gaim, *purple;
								gboolean has_logs = FALSE;
								GSList   *list    = NULL;

								

								gaim = g_build_filename (g_get_home_dir(), ".gaim", "logs", NULL);
								purple = g_build_filename (g_get_home_dir(), ".purple", "logs", NULL);

								if (tracker_file_is_valid (gaim)) {

									has_logs = TRUE;

									tracker_add_service_path ("GaimConversations", gaim);

									list = g_slist_prepend (NULL, gaim);
								}

								if (tracker_file_is_valid (purple)) {

									has_logs = TRUE;

									tracker_add_service_path ("GaimConversations", purple);

									list = g_slist_prepend (NULL, purple);
								}

								if (has_logs) {
									tracker_log ("Starting chat log indexing...");

									tracker_add_root_directories (list);

									process_directory_list (db_con, list, TRUE);

									g_slist_free (list);
								}

								g_free (gaim);
								g_free (purple);

								
							}
							break;


						
						case INDEX_EXTERNAL:
							break;


						case INDEX_EMAILS:  {
								tracker_cache_flush_all (db_con);

								if (tracker->index_evolution_emails || tracker->index_kmail_emails) {

									tracker_email_add_service_directories (db_con->emails);
									tracker_log ("Starting email indexing...");

									if (tracker->index_evolution_emails) {
										GSList *list = tracker_get_service_dirs ("EvolutionEmails");
										tracker_add_root_directories (list);
										process_directory_list (db_con, list, TRUE);
										g_slist_free (list);
									}

									if (tracker->index_kmail_emails) {
										GSList *list = tracker_get_service_dirs ("KMailEmails");
										tracker_add_root_directories (list);
										process_directory_list (db_con, list, TRUE);
										g_slist_free (list);
									}
							
								}

							}
							break;
				
						case INDEX_FINISHED:
							break;


					}

					tracker->index_status++;

					continue;
				}


				tracker_cache_flush_all (db_con);
			

				if (tracker->is_running && (tracker->first_time_index || tracker->do_optimize || (tracker->update_count > tracker->optimization_count))) {

					tracker->status = STATUS_OPTIMIZING;
					
					tracker->do_optimize = FALSE;
					tracker->first_time_index = FALSE;
					tracker->update_count = 0;

					tracker_log ("Updating database stats...please wait...");

					tracker_db_start_transaction (db_con);
					tracker_db_exec_no_reply (db_con, "ANALYZE");
					tracker_db_end_transaction (db_con);

					tracker_db_start_transaction (db_con->emails);
					tracker_db_exec_no_reply (db_con->emails, "ANALYZE");
					tracker_db_end_transaction (db_con->emails);

					tracker_log ("Finished optimizing. Waiting for new events...");
				}

				/* we have no stuff to process so sleep until awoken by a new signal */

				tracker->status = STATUS_IDLE;

				g_cond_wait (tracker->file_thread_signal, tracker->files_signal_mutex);
				g_mutex_unlock (tracker->files_check_mutex);

				tracker->grace_period = 1;

				/* determine if wake up call is new stuff or a shutdown signal */
				if (!tracker->shutdown) {
				} else {
					break;
				}
			}

			res = tracker_db_get_pending_files (db_con);

			k = 0;
			pushed_events = FALSE;

			if (res) {
				gchar **row;

				tracker->status = STATUS_PENDING;

				while ((row = tracker_db_get_row (res, k))) {
					FileInfo	    *info_tmp;
					TrackerChangeAction tmp_action;

					if (!tracker->is_running) {
						break;
					}

					k++;

					tmp_action = atoi(row[2]);

/*
					if (tmp_action != TRACKER_ACTION_CHECK) {
						tracker_debug ("processing %s with event %s", row[1], tracker_actions[tmp_action]);
					}
*/
					info_tmp = tracker_create_file_info (row[1], tmp_action, 0, WATCH_OTHER);
					g_async_queue_push (tracker->file_process_queue, info_tmp);
					pushed_events = TRUE;
				}

				tracker_db_free_result (res);
			}

			if (!tracker->is_running) {
				continue;
			}

			tracker_db_remove_pending_files (db_con);

			/* pending files are present but not yet ready as we are waiting til they stabilize
                           so we should sleep for 100ms (only occurs when using FAM or inotify move/create) */
			if (!pushed_events && (k == 0)) {
				g_usleep (100000);
			}

			continue;
		}


		tracker->status = STATUS_INDEXING;

		/* info struct may have been deleted in transit here so check if still valid and intact */
		if (!tracker_file_info_is_valid (info)) {
			continue;
		}

 		if (!tracker_check_uri (info->uri)) {
			tracker_free_file_info (info);
			continue;
		}

		/* get file ID and other interesting fields from Database if not previously fetched or is newly created */

		if (info->file_id == 0 && info->action != TRACKER_ACTION_CREATE &&
		    info->action != TRACKER_ACTION_DIRECTORY_CREATED && info->action != TRACKER_ACTION_FILE_CREATED) {

			info = tracker_db_get_file_info (db_con, info);
		}

		/* Get more file info if db retrieval returned nothing */
		if (info->file_id == 0 && info->action != TRACKER_ACTION_DELETE &&
		    info->action != TRACKER_ACTION_DIRECTORY_DELETED && info->action != TRACKER_ACTION_FILE_DELETED) {

			info = tracker_get_file_info (info);

			info->is_new = TRUE;

		} else {
			info->is_new = FALSE;
		}

		/* preprocess ambiguous actions when we need to work out if its a file or a directory that the action relates to */
		verify_action (info);

		//tracker_debug ("processing %s with action %s and counter %d ", info->uri, tracker_actions[info->action], info->counter);

		/* process deletions */

		if (info->action == TRACKER_ACTION_FILE_DELETED || info->action == TRACKER_ACTION_FILE_MOVED_FROM) {

			delete_file (db_con, info);

			if (info->action == TRACKER_ACTION_FILE_MOVED_FROM) {
				moved_from_list = g_slist_prepend (moved_from_list, info);
			} else {
				info = tracker_dec_info_ref (info);
			}

			continue;

		} else {
			if (info->action == TRACKER_ACTION_DIRECTORY_DELETED || info->action ==  TRACKER_ACTION_DIRECTORY_MOVED_FROM) {
				
				delete_file (db_con, info);

				delete_directory (db_con, info);

				if (info->action == TRACKER_ACTION_DIRECTORY_MOVED_FROM) {
					moved_from_list = g_slist_prepend (moved_from_list, info);
				} else {
					info = tracker_dec_info_ref (info);
				}

				continue;
			}
		}

		/* get latest file info from disk */
		if (info->mtime == 0) {
			info = tracker_get_file_info (info);
		}

		/* check if file needs indexing */
		need_index = (info->mtime > info->indextime);

		switch (info->action) {

			case TRACKER_ACTION_FILE_CHECK:

				break;

			case TRACKER_ACTION_FILE_CHANGED:
			case TRACKER_ACTION_FILE_CREATED:
			case TRACKER_ACTION_WRITABLE_FILE_CLOSED:

				need_index = TRUE;

				break;

			case TRACKER_ACTION_FILE_MOVED_TO :

				need_index = FALSE;

				/* to do - look up corresponding info in moved_from_list and update path and name in DB */

				break;

			case TRACKER_ACTION_DIRECTORY_REFRESH:

				if (need_index && !tracker_file_is_no_watched (info->uri)) {
					g_async_queue_push (tracker->dir_queue, g_strdup (info->uri));
				}
				need_index = FALSE;

				break;

			case TRACKER_ACTION_DIRECTORY_CHECK:

				if (need_index && !tracker_file_is_no_watched (info->uri)) {
					g_async_queue_push (tracker->dir_queue, g_strdup (info->uri));

					if (info->indextime > 0) {
						check_dir_for_deletions (db_con, info->uri);
					}
				}

				break;

			case TRACKER_ACTION_DIRECTORY_CREATED:
			case TRACKER_ACTION_DIRECTORY_MOVED_TO:

				need_index = TRUE;
				tracker_debug ("processing created directory %s", info->uri);

				/* schedule a rescan for all files in folder to avoid race conditions */
				if (info->action == TRACKER_ACTION_DIRECTORY_CREATED) {
					if (!tracker_file_is_no_watched (info->uri)) {
						/* add to watch folders (including subfolders) */
						watch_dir (info->uri, db_con);
						scan_directory (info->uri, db_con);
					} else {
						tracker_debug ("blocked scan of directory %s as its in the no watch list", info->uri);
					}
				}

				break;

			default:
				break;
		}

		if (need_index) {
			tracker->index_count++;

			if (tracker->verbosity == 1) {
				if ( (tracker->index_count == 1 || tracker->index_count == 100  || (tracker->index_count >= 500 && tracker->index_count%500 == 0)) && (tracker->verbosity == 1)) {
					tracker_log ("indexing #%d - %s", tracker->index_count, info->uri);
					xdg_mime_shutdown ();
				}
			}
			index_entity (db_con, info);
		}

		tracker_dec_info_ref (info);
	}
	xdg_mime_shutdown ();
	tracker_db_close_all (db_con);


	tracker_db_thread_end ();
	g_mutex_unlock (tracker->files_stopped_mutex);
}


static void
process_user_request_queue_thread (void)
{
	sigset_t     signal_set;
	DBConnection *db_con, *blob_db_con, *cache_db_con;
	DBConnection *emails_db_con = NULL;

        /* block all signals in this thread */
        sigfillset (&signal_set);
#ifndef OS_WIN32
        pthread_sigmask (SIG_BLOCK, &signal_set, NULL);
#endif
	g_mutex_lock (tracker->request_signal_mutex);
	g_mutex_lock (tracker->request_stopped_mutex);

	/* set thread safe DB connection */
	tracker_db_thread_init ();

	db_con = tracker_db_connect_all (FALSE);

	tracker_db_prepare_queries (db_con);

	while (TRUE) {
		DBusRec	    *rec;
		DBusMessage *reply;

		/* make thread sleep if first part of the shutdown process has been activated */
		if (!tracker->is_running) {

			g_cond_wait (tracker->request_thread_signal, tracker->request_signal_mutex);

			/* determine if wake up call is new stuff or a shutdown signal */
			if (!tracker->shutdown) {
				continue;
			} else {
				break;
			}
		}

		/* lock check mutex to prevent race condition when a request is submitted after popping queue but prior to sleeping */
		g_mutex_lock (tracker->request_check_mutex);

		rec = g_async_queue_try_pop (tracker->user_request_queue);

		if (!rec) {
			g_cond_wait (tracker->request_thread_signal, tracker->request_signal_mutex);
			g_mutex_unlock (tracker->request_check_mutex);

			/* determine if wake up call is new stuff or a shutdown signal */
			if (!tracker->shutdown) {
				continue;
			} else {
				break;
			}
		}

		/* thread will not sleep without another iteration so race condition no longer applies */
		g_mutex_unlock (tracker->request_check_mutex);

		rec->user_data = db_con;

		switch (rec->action) {

			case DBUS_ACTION_PING:

				reply = dbus_message_new_method_return (rec->message);

				gboolean result = TRUE;

				dbus_message_append_args (reply,
				  			  DBUS_TYPE_BOOLEAN, &result,
				  			  DBUS_TYPE_INVALID);

				dbus_connection_send (rec->connection, reply, NULL);
				dbus_message_unref (reply);

				break;

			case DBUS_ACTION_GET_STATS:

				tracker_dbus_method_get_stats (rec);

				break;

			case DBUS_ACTION_GET_SERVICES:

				tracker_dbus_method_get_services (rec);

				break;

			case DBUS_ACTION_GET_VERSION:

				tracker_dbus_method_get_version (rec);

				break;

                        case DBUS_ACTION_GET_STATUS:

                                reply = dbus_message_new_method_return (rec->message);
                                gchar *tracker_status[] = {"Initializing","Watching","Indexing","Pending","Optimizing","Idle","Shutdown"};
                         
                                gchar* status = tracker_status[tracker->status];
                                dbus_message_append_args (reply,
                                                          DBUS_TYPE_STRING, &status,
                                                          DBUS_TYPE_INVALID);
                                dbus_connection_send (rec->connection, reply, NULL);
                                dbus_message_unref (reply);

                                break;

			case DBUS_ACTION_METADATA_GET:

				tracker_dbus_method_metadata_get (rec);

				break;

			case DBUS_ACTION_METADATA_SET:

				tracker_dbus_method_metadata_set(rec);

				break;

			case DBUS_ACTION_METADATA_REGISTER_TYPE:

				tracker_dbus_method_metadata_register_type (rec);

				break;

			case DBUS_ACTION_METADATA_GET_TYPE_DETAILS:

				tracker_dbus_method_metadata_get_type_details (rec);

				break;

			case DBUS_ACTION_METADATA_GET_REGISTERED_TYPES:

				tracker_dbus_method_metadata_get_registered_types (rec);

				break;

			case DBUS_ACTION_METADATA_GET_WRITEABLE_TYPES:

				tracker_dbus_method_metadata_get_writeable_types (rec);

				break;

			case DBUS_ACTION_METADATA_GET_REGISTERED_CLASSES:

				tracker_dbus_method_metadata_get_registered_classes (rec);

				break;

			case DBUS_ACTION_KEYWORDS_GET_LIST:

				tracker_dbus_method_keywords_get_list (rec);

				break;

			case DBUS_ACTION_KEYWORDS_GET:

				tracker_dbus_method_keywords_get (rec);

				break;

			case DBUS_ACTION_KEYWORDS_ADD:

				tracker_dbus_method_keywords_add (rec);

				break;

			case DBUS_ACTION_KEYWORDS_REMOVE:

				tracker_dbus_method_keywords_remove (rec);

				break;

			case DBUS_ACTION_KEYWORDS_REMOVE_ALL:

				tracker_dbus_method_keywords_remove_all (rec);

				break;

			case DBUS_ACTION_KEYWORDS_SEARCH:

				tracker_dbus_method_keywords_search (rec);

				break;

			case DBUS_ACTION_SEARCH_GET_HIT_COUNT:

				tracker_dbus_method_search_get_hit_count (rec);

				break;

			case DBUS_ACTION_SEARCH_GET_HIT_COUNT_ALL:

				tracker_dbus_method_search_get_hit_count_all (rec);

				break;

			case DBUS_ACTION_SEARCH_TEXT:

				tracker_dbus_method_search_text (rec);

				break;

			case DBUS_ACTION_SEARCH_TEXT_DETAILED:

				tracker_dbus_method_search_text_detailed (rec);

				break;

			case DBUS_ACTION_SEARCH_GET_SNIPPET:

				tracker_dbus_method_search_get_snippet (rec);

				break;

			case DBUS_ACTION_SEARCH_FILES_BY_TEXT:

				tracker_dbus_method_search_files_by_text (rec);

				break;

			case DBUS_ACTION_SEARCH_METADATA:

				tracker_dbus_method_search_metadata (rec);

				break;

			case DBUS_ACTION_SEARCH_MATCHING_FIELDS:

				tracker_dbus_method_search_matching_fields (rec);

				break;

			case DBUS_ACTION_SEARCH_QUERY:

				tracker_dbus_method_search_query (rec);

				break;

			case DBUS_ACTION_SEARCH_SUGGEST:

				tracker_dbus_method_search_suggest (rec);

				break;

			case DBUS_ACTION_FILES_EXISTS:

				tracker_dbus_method_files_exists (rec);

				break;

			case DBUS_ACTION_FILES_CREATE:

				tracker_dbus_method_files_create (rec);

				break;

			case DBUS_ACTION_FILES_DELETE:

				tracker_dbus_method_files_delete (rec);

				break;

			case DBUS_ACTION_FILES_GET_SERVICE_TYPE:

				tracker_dbus_method_files_get_service_type (rec);

				break;

			case DBUS_ACTION_FILES_GET_TEXT_CONTENTS:

				tracker_dbus_method_files_get_text_contents (rec);

				break;

			case DBUS_ACTION_FILES_SEARCH_TEXT_CONTENTS:

				tracker_dbus_method_files_search_text_contents (rec);

				break;

			case DBUS_ACTION_FILES_GET_BY_SERVICE_TYPE:

				tracker_dbus_method_files_get_by_service_type (rec);

				break;

			case DBUS_ACTION_FILES_GET_BY_MIME_TYPE:

				tracker_dbus_method_files_get_by_mime_type (rec);

				break;

			case DBUS_ACTION_FILES_GET_BY_MIME_TYPE_VFS:

				tracker_dbus_method_files_get_by_mime_type_vfs (rec);

				break;

			case DBUS_ACTION_FILES_GET_MTIME:

				tracker_dbus_method_files_get_mtime (rec);

				break;

			case DBUS_ACTION_FILES_GET_METADATA_FOLDER_FILES:

				tracker_dbus_method_files_get_metadata_for_files_in_folder (rec);

				break;

			case DBUS_ACTION_FILES_SEARCH_BY_TEXT_MIME:

				tracker_dbus_method_files_search_by_text_mime (rec);

				break;

			case DBUS_ACTION_FILES_SEARCH_BY_TEXT_MIME_LOCATION:

				tracker_dbus_method_files_search_by_text_mime_location(rec);

				break;

			case DBUS_ACTION_FILES_SEARCH_BY_TEXT_LOCATION:

				tracker_dbus_method_files_search_by_text_location (rec);

				break;

			default:
				break;
		}

		dbus_message_unref (rec->message);

		g_free (rec);
	}

	tracker_db_close_all (db_con);
	
	tracker_db_thread_end ();

	tracker_debug ("request thread has exited successfully");

	/* unlock mutex so we know thread has exited */
	g_mutex_unlock (tracker->request_check_mutex);
	g_mutex_unlock (tracker->request_stopped_mutex);
}


static void
unregistered_func (DBusConnection *connection, gpointer data)
{
}


static DBusHandlerResult
local_dbus_connection_monitoring_message_func (DBusConnection *connection, DBusMessage *message, gpointer data)
{
	/* DBus connection has been lost! */
	if (dbus_message_is_signal (message, DBUS_INTERFACE_LOCAL, "Disconnected")) {
		dbus_message_ref (message);

		tracker_log ("DBus connection has been lost, trackerd will shutdown");

		tracker->is_running = FALSE;
		tracker_end_watching ();
		do_cleanup ("DBus connection lost");
	}

	return DBUS_HANDLER_RESULT_HANDLED;
}


static gboolean
add_local_dbus_connection_monitoring (DBusConnection *connection)
{
	DBusObjectPathVTable dbus_daemon_vtable = {
		unregistered_func,
		local_dbus_connection_monitoring_message_func,
		NULL,
		NULL,
		NULL,
		NULL
	};

	if (!dbus_connection_register_object_path (connection, DBUS_PATH_LOCAL, &dbus_daemon_vtable, NULL)) {
		tracker_log ("could not register local D-BUS connection handler");
		return FALSE;

	} else {
		return TRUE;
	}
}


static void
set_defaults (void)
{
	tracker->grace_period = 0;

	tracker->index_status = INDEX_CONFIG;

	tracker->watch_directory_roots_list = NULL;
	tracker->no_watch_directory_list = NULL;
	tracker->crawl_directory_list = NULL;
	tracker->use_nfs_safe_locking = FALSE;

	tracker->enable_indexing = TRUE;
	tracker->enable_watching = TRUE;
	tracker->enable_content_indexing = TRUE;
	tracker->enable_thumbnails = FALSE;

	tracker->watch_limit = 0;
	tracker->index_counter = 0;
	tracker->index_count = 0;
	tracker->update_count = 0;

	tracker->max_index_text_length = MAX_INDEX_TEXT_LENGTH;
	tracker->max_process_queue_size = MAX_PROCESS_QUEUE_SIZE;
	tracker->max_extract_queue_size = MAX_EXTRACT_QUEUE_SIZE;
	tracker->optimization_count = OPTIMIZATION_COUNT;
	tracker->max_words_to_index = MAX_WORDS_TO_INDEX;

	tracker->flush_count = 0;

	tracker->index_evolution_emails = TRUE;
	tracker->index_thunderbird_emails = FALSE;
	tracker->index_kmail_emails = TRUE;

	tracker->use_extra_memory = TRUE;

	tracker->throttle = 0;
	tracker->initial_sleep = 45;

	tracker->min_word_length = 3;
	tracker->max_word_length = 30;
	tracker->use_stemmer = TRUE;
	tracker->language = tracker_get_english_lang_code ();
	tracker->stop_words = NULL;

	tracker->index_numbers = FALSE;
	tracker->index_number_min_length = 6;
	tracker->strip_accents = TRUE;

	tracker->first_flush = TRUE;

	tracker->services_dir = g_build_filename (TRACKER_DATADIR, "tracker", "services", NULL);

	tracker->skip_mount_points = FALSE;
	tracker->root_directory_devices = NULL;

	/* battery and ac power checks */
	const gchar *battery_filenames[4] = {
		"/proc/acpi/ac_adapter/AC/state",
		"/proc/acpi/ac_adapter/AC0/state",
		"/proc/acpi/ac_adapter/ADp1/state",
		"/proc/acpi/ac_adapter/ACAD/state"
	};

	gint i;

	tracker->battery_state_file = NULL;

	for (i = 0; i < 4; i++) {

		if (g_file_test (battery_filenames[i], G_FILE_TEST_EXISTS)) {
			tracker->battery_state_file = g_strdup (battery_filenames[i]);
			break;
		}
	}
}


static void
sanity_check_option_values (void)
{
	tracker->index_thunderbird_emails = FALSE;

	if (tracker->max_index_text_length < 0) tracker->max_index_text_length = 0;
	if (tracker->max_words_to_index < 0) tracker->max_words_to_index = 0;
	if (tracker->optimization_count < 1000) tracker->optimization_count = 1000;


	if (tracker->min_word_length < 1) tracker->min_word_length = 3;

	if (!tracker_is_supported_lang (tracker->language)) {
		tracker_set_language ("en", TRUE);
	} else {
		tracker_set_language (tracker->language, TRUE);
	}

	/* dont need this as the default is to watch home dir in the cfg file */
	//if (!tracker->watch_directory_roots_list) {
	//	tracker->watch_directory_roots_list = g_slist_prepend (tracker->watch_directory_roots_list, g_strdup (g_get_home_dir()));
	//}

	if (tracker->throttle > 20) {
		tracker->throttle = 20;
	} else 	if (tracker->throttle < 0) {
		tracker->throttle = 0;
	}

	if (tracker->initial_sleep > 1000) tracker->initial_sleep = 1000; /* g_usleep (X * 1000 * 1000) */

	gchar *bools[] = {"no", "yes"};

	tracker_log ("Tracker configuration options :");
	tracker_log ("Verbosity : ........................  %d", tracker->verbosity);
 	tracker_log ("Low memory mode : ..................  %s", bools[!tracker->use_extra_memory]);
 	tracker_log ("Indexing enabled : .................  %s", bools[tracker->enable_indexing]);
 	tracker_log ("Watching enabled : .................  %s", bools[tracker->enable_watching]);
 	tracker_log ("File content indexing enabled : ....  %s", bools[tracker->enable_content_indexing]);
	tracker_log ("Thumbnailing enabled : .............  %s", bools[tracker->enable_thumbnails]);
 	tracker_log ("Evolution email indexing enabled : .  %s", bools[tracker->index_evolution_emails]);
 	tracker_log ("Thunderbird email indexing enabled :  %s", bools[tracker->index_thunderbird_emails]);
 	tracker_log ("K-Mail indexing enabled : ..........  %s\n", bools[tracker->index_kmail_emails]);

	tracker_log ("Tracker indexer parameters :");
	tracker_log ("Indexer language code : ............  %s", tracker->language);
	tracker_log ("Minimum index word length : ........  %d", tracker->min_word_length);
	tracker_log ("Maximum index word length : ........  %d", tracker->max_word_length);
	tracker_log ("Stemmer enabled : ..................  %s", bools[tracker->use_stemmer]);

	tracker->word_count = 0;
	tracker->word_detail_count = 0;

	if (tracker->use_extra_memory) {
		tracker->max_process_queue_size = 5000;
		tracker->max_extract_queue_size = 5000;

		tracker->cached_table = g_hash_table_new (g_str_hash, g_str_equal);

		tracker->word_detail_limit = 150000;
		tracker->word_detail_min = 75000;
		tracker->word_count_limit = 10000;
		tracker->word_count_min = 500;
	} else {
		tracker->word_detail_limit = 50000;
		tracker->word_detail_min = 25000;
		tracker->word_count_limit = 5000;
		tracker->word_count_min = 100;
	}


	GSList *lst;
	tracker_log ("Setting watch directory roots to:");
	for (lst = tracker->watch_directory_roots_list; lst; lst = lst->next) {
                if (lst->data) {
                        tracker_log (lst->data);
                }
	}
	tracker_log ("\t");

	if (tracker->no_watch_directory_list) {

		tracker_log ("Setting no watch directory roots to:");
		for (lst = tracker->no_watch_directory_list; lst; lst = lst->next) {
                        if (lst->data) {
                                tracker_log (lst->data);
                        }
		}
		tracker_log ("\t");
	}

	if (tracker->crawl_directory_list) {

		tracker_log ("Setting crawl directory roots to:");
		for (lst = tracker->crawl_directory_list; lst; lst = lst->next) {
                        if (lst->data) {
                                tracker_log (lst->data);
                        }
		}
		tracker_log ("\t");
	}

	if (tracker->verbosity < 0) {
		tracker->verbosity = 0;
	} else if (tracker->verbosity > 3) {
		tracker->verbosity = 3;
	}

	g_print ("Throttle level is %d\n", tracker->throttle);

	tracker->metadata_table = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL);
	tracker->service_table = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL);
	tracker->service_id_table = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL);
	tracker->service_directory_table = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL);
}


gint
main (gint argc, gchar *argv[])
{
	gint 		lfp;
#ifndef OS_WIN32
  	struct 	sigaction act;
	sigset_t 	empty_mask;
#endif
	gchar 		*lock_file, *str, *lock_str;
	GOptionContext  *context = NULL;
	GError          *error = NULL;
	gchar           *example;
	gboolean 	need_index, need_data;
	DBConnection 	*db_con;

	if (!g_thread_supported ())
		g_thread_init (NULL);

	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, TRACKER_LOCALEDIR);
        bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
        textdomain (GETTEXT_PACKAGE);

	/* set timezone info */
	tzset ();

        /* Translators: this messagge will apper immediately after the  */
        /* usage string - Usage: COMMAND <THIS_MESSAGE>     */
	context = g_option_context_new (_("- start the tracker daemon"));
        example = g_strconcat ("-i ", _("DIRECTORY"), " -i ", _("DIRECTORY"),
			       " -e ", _("DIRECTORY"), " -e ", _("DIRECTORY"),
			       NULL);

#ifdef HAVE_RECENT_GLIB
        /* Translators: this message will appear after the usage string */
        /* and before the list of options, showing an usage example.    */
        g_option_context_set_summary (context,
                                      g_strconcat(_("To include or exclude multiple directories "
                                                    "at the same time, join multiple options like:"),

                                                  "\n\n\t",
                                                  example, NULL));

#endif /* HAVE_RECENT_GLIB */

	g_option_context_add_main_entries (context, entries, NULL);
	g_option_context_parse (context, &argc, &argv, &error);

	g_option_context_free (context);
	g_free (example);

	g_print ("\n\nTracker version %s Copyright (c) 2005-2006 by Jamie McCracken (jamiemcc@gnome.org)\n\n", TRACKER_VERSION);
	g_print ("This program is free software and comes without any warranty.\nIt is licensed under version 2 or later of the General Public License which can be viewed at http://www.gnu.org/licenses/gpl.txt\n\n");

	g_print ("Initialising tracker...\n");

#ifndef OS_WIN32
	/* trap signals */
	sigemptyset (&empty_mask);
	act.sa_handler = signal_handler;
	act.sa_mask    = empty_mask;
	act.sa_flags   = 0;
	sigaction (SIGTERM, &act, NULL);
	sigaction (SIGILL,  &act, NULL);
	sigaction (SIGBUS,  &act, NULL);
	sigaction (SIGFPE,  &act, NULL);
	sigaction (SIGHUP,  &act, NULL);
	sigaction (SIGSEGV, &act, NULL);
	sigaction (SIGABRT, &act, NULL);
	sigaction (SIGUSR1, &act, NULL);
	sigaction (SIGINT,  &act, NULL);
#endif

	dbus_g_thread_init ();

	tracker = g_new0 (Tracker, 1);

	tracker->status = STATUS_INIT;

 	tracker->is_running = FALSE;

	/* Make a temporary directory for Tracker into g_get_tmp_dir() directory */
	gchar *tmp_dir;

	tmp_dir = g_strdup_printf ("Tracker-%s.%u", g_get_user_name (), getpid());

	tracker->sys_tmp_root_dir = g_build_filename (g_get_tmp_dir (), tmp_dir, NULL);
	tracker->root_dir = g_build_filename (g_get_user_data_dir  (), "tracker", NULL);
	tracker->data_dir = g_build_filename (g_get_user_cache_dir (), "tracker", NULL);
	tracker->config_dir = g_strdup (g_get_user_config_dir ());
	tracker->user_data_dir = g_build_filename (tracker->root_dir, "data", NULL);

	g_free (tmp_dir);

	/* remove an existing one */
	if (g_file_test (tracker->sys_tmp_root_dir, G_FILE_TEST_EXISTS)) {
		tracker_remove_dirs (tracker->sys_tmp_root_dir);
	}

	/* remove old tracker dirs */
	gchar *old = g_build_filename (g_get_home_dir (), ".Tracker", NULL);

	if (g_file_test (old ,G_FILE_TEST_EXISTS)) {
		tracker_remove_dirs (old);
	}

	g_free (old);

	if (!g_file_test (tracker->user_data_dir, G_FILE_TEST_EXISTS)) {
		g_mkdir_with_parents (tracker->user_data_dir, 00755);
	}

	if (!g_file_test (tracker->data_dir, G_FILE_TEST_EXISTS)) {
		g_mkdir_with_parents (tracker->data_dir, 00755);
	}

        tracker->email_attachements_dir = g_build_filename (tracker->sys_tmp_root_dir, "Attachments", NULL);
	g_mkdir_with_parents (tracker->email_attachements_dir, 00700);
	tracker_log ("Made email attachments directory %s\n", tracker->email_attachements_dir);

	need_index = FALSE;
	need_data = FALSE;

	tracker->shutdown = FALSE;

	tracker->files_check_mutex = g_mutex_new ();
	tracker->metadata_check_mutex = g_mutex_new ();
	tracker->request_check_mutex = g_mutex_new ();

	tracker->files_stopped_mutex = g_mutex_new ();
	tracker->metadata_stopped_mutex = g_mutex_new ();
	tracker->request_stopped_mutex = g_mutex_new ();

	tracker->file_metadata_thread = NULL;
	tracker->file_process_thread = NULL;
	tracker->user_request_thread = NULL;;

	tracker->file_thread_signal = g_cond_new ();
	tracker->metadata_thread_signal = g_cond_new ();
	tracker->request_thread_signal = g_cond_new ();

	tracker->metadata_signal_mutex = g_mutex_new ();
	tracker->files_signal_mutex = g_mutex_new ();
	tracker->request_signal_mutex = g_mutex_new ();

	tracker->log_access_mutex = g_mutex_new ();
	tracker->scheduler_mutex = g_mutex_new ();

	tracker->stemmer_mutex = g_mutex_new ();

	//tracker->cached_word_table_mutex = g_mutex_new ();

	tracker->dir_queue = g_async_queue_new ();

	/* delete old stuff if files.db is present in data dir */
	char *old_file = g_build_filename (tracker->data_dir, "files.db", NULL);
	if (reindex || g_file_test (old_file, G_FILE_TEST_EXISTS)) {
		tracker_remove_dirs (tracker->data_dir);
		g_mkdir_with_parents (tracker->data_dir, 00755);
		need_index = TRUE;
	}
	g_free (old_file);


	/* check user data files */
	if (!need_index) {
		need_index = tracker_db_needs_setup ();
	}

	need_data = tracker_db_needs_data ();

	umask (077);

	str = g_strconcat (g_get_user_name (), "_tracker_lock", NULL);

	tracker->log_file = g_build_filename (tracker->root_dir, "tracker.log", NULL);

	/* check if setup for NFS usage (and enable atomic NFS safe locking) */
	//lock_str = tracker_get_config_option ("NFSLocking");
	lock_str = NULL;

	if (lock_str != NULL) {

		tracker->use_nfs_safe_locking = (strcmp (str, "1") == 0);

		/* place lock file in tmp dir to allow multiple sessions on NFS */
		lock_file = g_build_filename (tracker->sys_tmp_root_dir, str, NULL);

		g_free (lock_str);

	} else {

		tracker->use_nfs_safe_locking = FALSE;

		/* place lock file in home dir to prevent multiple sessions on NFS (as standard locking might be broken on NFS) */
		lock_file = g_build_filename (tracker->root_dir, str, NULL);
	}

	g_free (str);

	/* prevent muliple instances  */
	lfp = g_open (lock_file, O_RDWR|O_CREAT, 0640);
	g_free (lock_file);

	if (lfp < 0) {
                g_warning ("Cannot open or create lockfile - exiting");
		exit (1);
	}

	if (lockf (lfp, F_TLOCK, 0) <0) {
		g_warning ("Tracker daemon is already running - exiting");
		exit (0);
	}

	/* Set child's niceness to 19 */
        errno = 0;
        /* nice() uses attribute "warn_unused_result" and so complains if we do not check its
           returned value. But it seems that since glibc 2.2.4, nice() can return -1 on a
           successful call so we have to check value of errno too. Stupid... */
        if (nice (19) == -1 && errno) {
                g_printerr ("ERROR: trying to set nice value\n");
        }

#ifdef IOPRIO_SUPPORT
	ioprio ();
#endif

	/* reset log file */
	tracker_unlink (tracker->log_file);

	/* deal with config options with defaults, config file and option params */
	set_defaults ();
	tracker_load_config_file ();

	if (error) {
		g_printerr ("invalid arguments: %s\n", error->message);
		return 1;
	}

	if (no_watch_dirs) {
		tracker->no_watch_directory_list = tracker_filename_array_to_list (no_watch_dirs);
	}

	if (watch_dirs) {
	 	tracker->watch_directory_roots_list = tracker_filename_array_to_list (watch_dirs);
	}

	if (crawl_dirs) {
	 	tracker->crawl_directory_list = tracker_filename_array_to_list (crawl_dirs);
	}

	if (disable_indexing) {
		tracker->enable_indexing = FALSE;
	}

	if (language) {
		tracker->language = language;
	}

	if (enable_evolution) {
		tracker->index_evolution_emails = TRUE;
	}

	if (enable_thunderbird) {
		tracker->index_thunderbird_emails = TRUE;
	}

	if (enable_kmail) {
		tracker->index_kmail_emails = TRUE;
	}

	if (throttle != 0) {
		tracker->throttle = throttle;
	}

	if (low_memory) {
		tracker->use_extra_memory = FALSE;
	}

	if (verbosity != 0) {
		tracker->verbosity = verbosity;
	}

	if (initial_sleep >= 0) {
		tracker->initial_sleep = initial_sleep;
	}

	tracker->fatal_errors = fatal_errors;

	sanity_check_option_values ();

	/* set thread safe DB connection */
	tracker_db_thread_init ();

	if (!tracker_db_initialize (tracker->data_dir)) {
		tracker_log ("ERROR: failed to initialise database engine - exiting...");
		return 1;
	}

	/* create cache db */
	DBConnection *db2 = tracker_db_connect_cache ();
	tracker_db_close (db2);

	if (need_data) {
		tracker_create_db ();
	}

	/* create database if needed */
	if (need_index) {

		tracker->first_time_index = TRUE;

		/* create files db and emails db */
		DBConnection *db_con_tmp = tracker_db_connect ();

		/* reset stats for embedded services if they are being reindexed */
		if (!need_data) {
			tracker_log ("*** DELETING STATS *** ");
			tracker_db_exec_no_reply (db_con_tmp, "update ServiceTypes set TypeCount = 0 where Embedded = 1");

		}

		tracker_db_close (db_con_tmp);
		g_free (db_con_tmp);

		/* create databases */

		db_con_tmp = tracker_db_connect_file_content ();
		tracker_db_close (db_con_tmp);
		g_free (db_con_tmp);

		db_con_tmp = tracker_indexer_open ("file-index.db");
		tracker_db_close (db_con_tmp);
		g_free (db_con_tmp);

		db_con_tmp = tracker_indexer_open ("email-index.db");
		tracker_db_close (db_con_tmp);
		g_free (db_con_tmp);

		db_con_tmp = tracker_db_connect_email_content ();
		tracker_db_close (db_con_tmp);
		g_free (db_con_tmp);

		db_con_tmp = tracker_db_connect_emails ();
		tracker_db_close (db_con_tmp);
		g_free (db_con_tmp);

	} else {
		tracker->first_time_index = FALSE;
	}

	db_con = tracker_db_connect ();
	db_con->thread = "main";
	DBConnection *blob_db = tracker_db_connect_file_content ();
	db_con->blob = blob_db;

	main_thread_db_con = db_con;

	tracker->update_count = get_update_count (main_thread_db_con);

	tracker_db_get_static_data (db_con);

	tracker->file_metadata_queue = g_async_queue_new ();
	tracker->file_process_queue = g_async_queue_new ();
	tracker->user_request_queue = g_async_queue_new ();

  	tracker->loop = g_main_loop_new (NULL, TRUE);

	/* this var is used to tell the threads when to quit */
	tracker->is_running = TRUE;

	tracker->user_request_thread =  g_thread_create ((GThreadFunc) process_user_request_queue_thread, NULL, FALSE, NULL);

	main_connection = tracker_dbus_init ();

	add_local_dbus_connection_monitoring (main_connection);

	if (!tracker_start_watching ()) {
		tracker_error ("ERROR: file monitoring failed to start");
		do_cleanup ("File watching failure");
		exit (1);
	}

	tracker->file_process_thread =  g_thread_create ((GThreadFunc) process_files_thread, NULL, FALSE, NULL);
	
	g_main_loop_run (tracker->loop);

	return EXIT_SUCCESS;
}
