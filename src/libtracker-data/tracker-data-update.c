/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2006, Mr Jamie McCracken (jamiemcc@gnome.org)
 * Copyright (C) 2008, Nokia (urho.konttori@nokia.com)
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

#include "config.h"

#include <stdlib.h>

#include <libtracker-common/tracker-type-utils.h>
#include <libtracker-common/tracker-file-utils.h>

#include <libtracker-db/tracker-db-manager.h>
#include <libtracker-db/tracker-db-dbus.h>

#include "tracker-data-manager.h"
#include "tracker-data-update.h"

guint32
tracker_data_update_get_new_service_id (TrackerDBInterface *iface)
{
	guint32		    files_max;
	TrackerDBResultSet *result_set;
	TrackerDBInterface *temp_iface;
	static guint32	    max = 0;

	if (G_LIKELY (max != 0)) {
		return ++max;
	}

	temp_iface = tracker_db_manager_get_db_interface (TRACKER_DB_FILE_METADATA);

	result_set = tracker_db_interface_execute_query (temp_iface, NULL,
							 "SELECT MAX(ID) AS A FROM Services");

	if (result_set) {
		GValue val = {0, };
		_tracker_db_result_set_get_value (result_set, 0, &val);
		if (G_VALUE_TYPE (&val) == G_TYPE_INT) {
			max = g_value_get_int (&val);
		}
		g_value_unset (&val);
		g_object_unref (result_set);
	}

	temp_iface = tracker_db_manager_get_db_interface (TRACKER_DB_EMAIL_METADATA);

	result_set = tracker_db_interface_execute_query (temp_iface, NULL,
							 "SELECT MAX(ID) AS A FROM Services");

	if (result_set) {
		GValue val = {0, };
		_tracker_db_result_set_get_value (result_set, 0, &val);
		if (G_VALUE_TYPE (&val) == G_TYPE_INT) {
			files_max = g_value_get_int (&val);
			max = MAX (files_max, max);
		}
		g_value_unset (&val);
		g_object_unref (result_set);
	}

	return ++max;
}

void
tracker_data_update_increment_stats (TrackerDBInterface *iface,
				     TrackerService     *service)
{
	const gchar *service_type, *parent;

	service_type = tracker_service_get_name (service);
	parent = tracker_service_get_parent (service);

	tracker_db_interface_execute_procedure (iface,
						NULL,
						"IncStat",
						service_type,
						NULL);

	if (parent) {
		tracker_db_interface_execute_procedure (iface,
							NULL,
							"IncStat",
							parent,
							NULL);
	}
}

void
tracker_data_update_decrement_stats (TrackerDBInterface *iface,
				     TrackerService     *service)
{
	const gchar *service_type, *parent;

	service_type = tracker_service_get_name (service);
	parent = tracker_service_get_parent (service);

	tracker_db_interface_execute_procedure (iface,
						NULL,
						"DecStat",
						service_type,
						NULL);

	if (parent) {
		tracker_db_interface_execute_procedure (iface,
							NULL,
							"DecStat",
							parent,
							NULL);
	}
}

void
tracker_data_update_create_event (TrackerDBInterface *iface,
				  guint32 service_id,
				  const gchar *type)
{
	gchar *service_id_str;

	service_id_str = tracker_guint32_to_string (service_id);

	tracker_db_interface_execute_procedure (iface, NULL, "CreateEvent",
						service_id_str,
						type,
						NULL);

	g_free (service_id_str);
}

gboolean
tracker_data_update_create_service (TrackerService *service,
				    guint32	    service_id,
				    const gchar	   *dirname,
				    const gchar	   *basename,
				    GHashTable     *metadata)
{
	TrackerDBInterface *iface;
	TrackerDBResultSet *result_set;
	guint32	volume_id = 0;
	gchar *id_str, *service_type_id_str, *path, *volume_id_str;
	gboolean is_dir, is_symlink, enabled;

	if (!service) {
		return FALSE;
	}

	/* retrieve VolumeID */
	iface = tracker_db_manager_get_db_interface (TRACKER_DB_COMMON);
	result_set = tracker_db_interface_execute_procedure (iface, NULL,
							     "GetVolumeByPath",
							     dirname,
							     dirname,
							     NULL);
	if (result_set) {
		tracker_db_result_set_get (result_set, 0, &volume_id, -1);
		g_object_unref (result_set);
	}
	volume_id_str = tracker_guint32_to_string (volume_id);

	iface = tracker_db_manager_get_db_interface_by_type (tracker_service_get_name (service),
							     TRACKER_DB_CONTENT_TYPE_METADATA);

	id_str = tracker_guint32_to_string (service_id);
	service_type_id_str = tracker_gint_to_string (tracker_service_get_id (service));

	path = g_build_filename (dirname, basename, NULL);

	is_dir = g_file_test (path, G_FILE_TEST_IS_DIR);
	is_symlink = g_file_test (path, G_FILE_TEST_IS_SYMLINK);

	tracker_db_interface_execute_procedure (iface, NULL, "CreateService",
						id_str,
						dirname,
						basename,
						service_type_id_str,
						is_dir ? "Folder" : g_hash_table_lookup (metadata, "File:Mime"),
						g_hash_table_lookup (metadata, "File:Size"),
						is_dir ? "1" : "0",
						is_symlink ? "1" : "0",
						"0", /* Offset */
						g_hash_table_lookup (metadata, "File:Modified"),
						volume_id_str, /* Aux ID */
						NULL);

	enabled = is_dir ?
		tracker_service_get_show_service_directories (service) :
		tracker_service_get_show_service_files (service);

	if (!enabled) {
		tracker_db_interface_execute_query (iface, NULL,
						    "Update services set Enabled = 0 where ID = %d",
						    service_id);
	}

	g_free (id_str);
	g_free (service_type_id_str);
	g_free (volume_id_str);
	g_free (path);

	return TRUE;
}

void
tracker_data_update_delete_service (TrackerService *service,
				    guint32	    service_id)
{

	TrackerDBInterface *iface;
	gchar *service_id_str;

	g_return_if_fail (TRACKER_IS_SERVICE (service));
	g_return_if_fail (service_id >= 1);

	iface = tracker_db_manager_get_db_interface_by_type (tracker_service_get_name (service),
							     TRACKER_DB_CONTENT_TYPE_METADATA);

	service_id_str = tracker_guint32_to_string (service_id);

	/* Delete from services table */
	tracker_db_interface_execute_procedure (iface,
						NULL,
						"DeleteService1",
						service_id_str,
						NULL);

	g_free (service_id_str);
}

void
tracker_data_update_delete_service_recursively (TrackerService *service,
						gchar          *service_path)
{
	TrackerDBInterface *iface;
	gchar              *str;

	g_return_if_fail (TRACKER_IS_SERVICE (service));
	g_return_if_fail (service_path != NULL);

	iface = tracker_db_manager_get_db_interface_by_type (tracker_service_get_name (service),
							     TRACKER_DB_CONTENT_TYPE_METADATA);

	/* Delete from services table recursively */
	str = g_strconcat (service_path, "/%", NULL);

	/* We have to give two arguments. One is the actual path and
	 * the second is a string representing the likeness to match
	 * sub paths. Not sure how to do this in the .sql file
	 * instead.
	 */
	tracker_db_interface_execute_procedure (iface,
						NULL,
						"DeleteServiceRecursively",
						service_path,
						str,
						NULL);

	g_free (str);
}

void
tracker_data_update_move_service (TrackerService *service,
				  const gchar	*from,
				  const gchar	*to)
{
	TrackerDBInterface *iface;
	GError *error = NULL;
	gchar *from_dirname;
	gchar *from_basename;
	gchar *to_dirname;
	gchar *to_basename;

	g_return_if_fail (TRACKER_IS_SERVICE (service));
	g_return_if_fail (from != NULL);
	g_return_if_fail (to != NULL);

	iface = tracker_db_manager_get_db_interface_by_type (tracker_service_get_name (service),
							     TRACKER_DB_CONTENT_TYPE_METADATA);

	tracker_file_get_path_and_name (from,
					&from_dirname,
					&from_basename);
	tracker_file_get_path_and_name (to,
					&to_dirname,
					&to_basename);

	tracker_db_interface_execute_procedure (iface,
						NULL,
						"MoveService",
						to_dirname, to_basename,
						from_dirname, from_basename,
						NULL);

	/* FIXME: This procedure should use LIKE statement */
	tracker_db_interface_execute_procedure (iface,
						&error,
						"MoveServiceChildren",
						from,
						to,
						from,
						NULL);

	g_free (to_dirname);
	g_free (to_basename);
	g_free (from_dirname);
	g_free (from_basename);
}

void
tracker_data_update_delete_all_metadata (TrackerService *service,
					 guint32	 service_id)
{
	TrackerDBInterface *iface;
	gchar *service_id_str;

	iface = tracker_db_manager_get_db_interface_by_type (tracker_service_get_name (service),
							     TRACKER_DB_CONTENT_TYPE_METADATA);

	service_id_str = tracker_guint32_to_string (service_id);

	/* Delete from ServiceMetadata, ServiceKeywordMetadata,
	 * ServiceNumberMetadata.
	 */
	tracker_db_interface_execute_procedure (iface,
						NULL,
						"DeleteServiceMetadata",
						service_id_str,
						NULL);
	tracker_db_interface_execute_procedure (iface,
						NULL,
						"DeleteServiceKeywordMetadata",
						service_id_str,
						NULL);
	tracker_db_interface_execute_procedure (iface,
						NULL,
						"DeleteServiceNumericMetadata",
						service_id_str,
						NULL);
	g_free (service_id_str);
}

void
tracker_data_update_set_metadata (TrackerService *service,
				  guint32	  service_id,
				  TrackerField	 *field,
				  const gchar	 *value,
				  const gchar	 *parsed_value)
{
	TrackerDBInterface *iface;
	gint metadata_key;
	gchar *id_str;
	gchar *time_string;

	id_str = tracker_guint32_to_string (service_id);
	iface = tracker_db_manager_get_db_interface_by_type (tracker_service_get_name (service),
							     TRACKER_DB_CONTENT_TYPE_METADATA);

	switch (tracker_field_get_data_type (field)) {
	case TRACKER_FIELD_TYPE_KEYWORD:
		tracker_db_interface_execute_procedure (iface, NULL,
							"SetMetadataKeyword",
							id_str,
							tracker_field_get_id (field),
							value,
							NULL);
		break;

	case TRACKER_FIELD_TYPE_INDEX:
	case TRACKER_FIELD_TYPE_STRING:
	case TRACKER_FIELD_TYPE_DOUBLE:
		tracker_db_interface_execute_procedure (iface, NULL,
							"SetMetadata",
							id_str,
							tracker_field_get_id (field),
							parsed_value,
							value,
							NULL);
		break;

	case TRACKER_FIELD_TYPE_INTEGER:
		tracker_db_interface_execute_procedure (iface, NULL,
							"SetMetadataNumeric",
							id_str,
							tracker_field_get_id (field),
							value,
							NULL);
		break;

	case TRACKER_FIELD_TYPE_DATE:

		time_string = tracker_date_to_time_string (value);

		if (time_string) {
			tracker_db_interface_execute_procedure (iface, NULL,
								"SetMetadataNumeric",
								id_str,
								tracker_field_get_id (field),
								time_string,
								NULL);
			g_free (time_string);
		}
		break;

	case TRACKER_FIELD_TYPE_FULLTEXT:
		tracker_data_update_set_content (service, service_id, value);
		break;

	case TRACKER_FIELD_TYPE_BLOB:
	case TRACKER_FIELD_TYPE_STRUCT:
	case TRACKER_FIELD_TYPE_LINK:
		/* not handled */
	default:
		break;
	}

	metadata_key = tracker_ontology_service_get_key_metadata (tracker_service_get_name (service),
								  tracker_field_get_name (field));
	if (metadata_key > 0) {
		tracker_db_interface_execute_query (iface, NULL,
						    "update Services set KeyMetadata%d = '%s' where id = %d",
						    metadata_key,
						    value,
						    service_id);
	}

	g_free (id_str);
}

void
tracker_data_update_delete_metadata (TrackerService *service,
				     guint32	     service_id,
				     TrackerField   *field,
				     const gchar    *value)
{
	TrackerDBInterface *iface;
	gint metadata_key;
	gchar *id_str;

	id_str = tracker_guint32_to_string (service_id);
	iface = tracker_db_manager_get_db_interface_by_type (tracker_service_get_name (service),
							     TRACKER_DB_CONTENT_TYPE_METADATA);

	switch (tracker_field_get_data_type (field)) {
	case TRACKER_FIELD_TYPE_KEYWORD:
		if (!value) {
			g_debug ("Trying to remove keyword field with no specific value");
			tracker_db_interface_execute_procedure (iface, NULL,
								"DeleteMetadataKeyword",
								id_str,
								tracker_field_get_id (field),
								NULL);
		} else {
			tracker_db_interface_execute_procedure (iface, NULL,
								"DeleteMetadataKeywordValue",
								id_str,
								tracker_field_get_id (field),
								value,
								NULL);
		}
		break;

	case TRACKER_FIELD_TYPE_INDEX:
	case TRACKER_FIELD_TYPE_STRING:
	case TRACKER_FIELD_TYPE_DOUBLE:
		tracker_db_interface_execute_procedure (iface, NULL,
							"DeleteMetadata",
							id_str,
							tracker_field_get_id (field),
							NULL);
		break;

	case TRACKER_FIELD_TYPE_INTEGER:
	case TRACKER_FIELD_TYPE_DATE:
		tracker_db_interface_execute_procedure (iface, NULL,
							"DeleteMetadataNumeric",
							id_str,
							tracker_field_get_id (field),
							NULL);
		break;

	case TRACKER_FIELD_TYPE_FULLTEXT:
		tracker_data_update_delete_content (service, service_id);
		break;

	case TRACKER_FIELD_TYPE_BLOB:
	case TRACKER_FIELD_TYPE_STRUCT:
	case TRACKER_FIELD_TYPE_LINK:
		/* not handled */
	default:
		break;
	}

	metadata_key = tracker_ontology_service_get_key_metadata (tracker_service_get_name (service),
								  tracker_field_get_name (field));
	if (metadata_key > 0) {
		tracker_db_interface_execute_query (iface, NULL,
						    "update Services set KeyMetadata%d = '%s' where id = %d",
						    metadata_key, "", service_id);
	}

	g_free (id_str);
}

void
tracker_data_update_set_content (TrackerService *service,
				  guint32	 service_id,
				  const gchar   *text)
{
	TrackerDBInterface *iface;
	TrackerField *field;
	gchar *id_str;

	id_str = tracker_guint32_to_string (service_id);
	field = tracker_ontology_get_field_by_name ("File:Contents");
	iface = tracker_db_manager_get_db_interface_by_type (tracker_service_get_name (service),
							     TRACKER_DB_CONTENT_TYPE_CONTENTS);

	tracker_db_interface_execute_procedure (iface, NULL,
						"SaveServiceContents",
						id_str,
						tracker_field_get_id (field),
						text,
						NULL);
	g_free (id_str);
}

void
tracker_data_update_delete_content (TrackerService *service,
				     guint32	    service_id)
{
	TrackerDBInterface *iface;
	TrackerField *field;
	gchar *service_id_str;

	service_id_str = tracker_guint32_to_string (service_id);
	field = tracker_ontology_get_field_by_name ("File:Contents");
	iface = tracker_db_manager_get_db_interface_by_type (tracker_service_get_name (service),
							     TRACKER_DB_CONTENT_TYPE_CONTENTS);

	/* Delete contents if it has! */
	tracker_db_interface_execute_procedure (iface, NULL,
						"DeleteContent",
						service_id_str,
						tracker_field_get_id (field),
						NULL);

	g_free (service_id_str);
}

void
tracker_data_update_delete_handled_events (TrackerDBInterface *iface)
{
	g_return_if_fail (TRACKER_IS_DB_INTERFACE (iface));

	tracker_data_manager_exec (iface, "DELETE FROM Events WHERE BeingHandled = 1");
}

void
tracker_data_update_enable_volume (const gchar *udi,
                                   const gchar *mount_path)
{
	TrackerDBInterface *iface;
	TrackerDBResultSet *result_set;
	guint32		    id = 0;

	g_return_if_fail (udi != NULL);
	g_return_if_fail (mount_path != NULL);

	iface = tracker_db_manager_get_db_interface (TRACKER_DB_COMMON);

	result_set = tracker_db_interface_execute_procedure (iface, NULL,
					   "GetVolumeID",
					   udi,
					   NULL);


	if (result_set) {
		tracker_db_result_set_get (result_set, 0, &id, -1);
		g_object_unref (result_set);
	}

	if (id == 0) {
		tracker_db_interface_execute_procedure (iface, NULL,
							"InsertVolume",
							mount_path,
							udi,
							NULL);
	} else {
		tracker_db_interface_execute_procedure (iface, NULL,
							"EnableVolume",
							mount_path,
							udi,
							NULL);
	}
}

void
tracker_data_update_disable_volume (const gchar *udi)
{
	TrackerDBInterface *iface;

	g_return_if_fail (udi != NULL);

	iface = tracker_db_manager_get_db_interface (TRACKER_DB_COMMON);
	
	tracker_db_interface_execute_procedure (iface, NULL,
						"DisableVolume",
						udi,
						NULL);
}

void
tracker_data_update_disable_all_volumes (void)
{
	TrackerDBInterface *iface;

	iface = tracker_db_manager_get_db_interface (TRACKER_DB_COMMON);
	
	tracker_db_interface_execute_procedure (iface, NULL,
						"DisableAllVolumes",
						NULL);
}

