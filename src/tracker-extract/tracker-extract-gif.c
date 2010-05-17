/*
 * Copyright (C) 2006, Jamie McCracken <jamiemcc@gnome.org>
 * Copyright (C) 2008, Nokia <ivan.frade@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <gif_lib.h>

#include <libtracker-common/tracker-file-utils.h>
#include <libtracker-client/tracker-client.h>
#include <libtracker-extract/tracker-extract.h>

#define XMP_MAGIC_TRAILER_LENGTH 256

typedef struct {
	const gchar *title;
	const gchar *date;
	const gchar *artist;
	gchar *camera;
} MergeData;

typedef struct {
	const gchar *width;
	const gchar *height;
	const gchar *comment;

} GifData;

static void extract_gif (const gchar          *filename,
                         TrackerSparqlBuilder *preupdate,
                         TrackerSparqlBuilder *metadata);

static TrackerExtractData data[] = {
	{ "image/gif", extract_gif },
	{ NULL, NULL }
};

typedef struct {
    unsigned int   byteCount;
    char          *bytes;
} ExtBlock;



static int
ext_block_append(ExtBlock *extBlock,
		 unsigned int len,
		 unsigned char extData[])
{
	extBlock->bytes = realloc(extBlock->bytes,extBlock->byteCount+len);
	if (extBlock->bytes == NULL)
		return (GIF_ERROR);

	memcpy(&(extBlock->bytes[extBlock->byteCount]), &extData[0], len);
	extBlock->byteCount += len;

	return (GIF_OK);
}

static void
insert_keywords (TrackerSparqlBuilder *metadata,
                 const gchar          *uri,
                 gchar                *keywords)
{
	char *lasts, *keyw;
	size_t len;

	keyw = keywords;
	keywords = strchr (keywords, '"');
	if (keywords) {
		keywords++;
	} else {
		keywords = keyw;
	}

	len = strlen (keywords);
	if (keywords[len - 1] == '"') {
		keywords[len - 1] = '\0';
	}

	for (keyw = strtok_r (keywords, ",;", &lasts);
	     keyw;
	     keyw = strtok_r (NULL, ",;", &lasts)) {
		tracker_sparql_builder_predicate (metadata, "nao:hasTag");

		tracker_sparql_builder_object_blank_open (metadata);
		tracker_sparql_builder_predicate (metadata, "a");
		tracker_sparql_builder_object (metadata, "nao:Tag");

		tracker_sparql_builder_predicate (metadata, "nao:prefLabel");
		tracker_sparql_builder_object_unvalidated (metadata, keyw);

		tracker_sparql_builder_object_blank_close (metadata);
	}
}

static void
read_metadata (TrackerSparqlBuilder *preupdate,
               TrackerSparqlBuilder *metadata,
	       GifFileType          *gifFile,
               const gchar          *uri)
{
	GifRecordType  RecordType;
	int            frameheight;
	int            framewidth;
	unsigned char *framedata = NULL;

	int            status;

	MergeData md = { 0 };
	GifData   gd = { 0 };
	TrackerXmpData *xd = NULL;

	do {
		if (DGifGetRecordType(gifFile, &RecordType) == GIF_ERROR) {
			PrintGifError();
			return;
		}

		switch (RecordType) {
		case IMAGE_DESC_RECORD_TYPE:
			if (DGifGetImageDesc(gifFile) == GIF_ERROR) {
				PrintGifError();
				return;
			}

			framewidth  = gifFile->Image.Width;
			frameheight = gifFile->Image.Height;

			framedata = g_malloc (framewidth*frameheight);

			if (DGifGetLine(gifFile, framedata, framewidth*frameheight)==GIF_ERROR) {
				PrintGifError();
				return;
			}

			gd.width  = g_strdup_printf ("%d", framewidth);
			gd.height = g_strdup_printf ("%d", frameheight);


			g_free (framedata);

			break;
		case EXTENSION_RECORD_TYPE:
			{
				GifByteType   *ExtData;
				int            ExtCode;
				ExtBlock       extBlock;

				extBlock.bytes = NULL;
				extBlock.byteCount = 0;

				if ((status = DGifGetExtension(gifFile, &ExtCode, &ExtData)) != GIF_OK) {
					g_warning ("Problem getting the extension");
					return;
				}
#if defined(HAVE_EXEMPI)
				if (strncmp(&ExtData[1],"XMP Data",8) == 0) {
					while (ExtData != NULL && status == GIF_OK ) {
						if ((status = DGifGetExtensionNext(gifFile, &ExtData)) == GIF_OK) {
							if (ExtData != NULL) {
								if (ext_block_append(&extBlock, ExtData[0]+1, (char*)&(ExtData[0])) != GIF_OK) {
									g_warning ("Problem with extension data");
									return;
								}
							}
						}
					}

					xd = tracker_xmp_new (extBlock.bytes,
							      extBlock.byteCount-XMP_MAGIC_TRAILER_LENGTH,
							      uri);

					g_free (extBlock.bytes);
				} else {
#else
				{
#endif
					do {
						status = DGifGetExtensionNext(gifFile, &ExtData);
					} while ( status == GIF_OK && ExtData != NULL);
				}
			}
			break;
		case TERMINATE_RECORD_TYPE:
			break;
		default:
			break;
		}
	} while (RecordType != TERMINATE_RECORD_TYPE);


	if (!xd) {
		xd = g_new0 (TrackerXmpData, 1);
	}

	/* Don't merge if the make is in the model */
	if ((xd->make == NULL || xd->model == NULL) ||
	    (xd->make && xd->model && strstr (xd->model, xd->make) == NULL)) {
		md.camera = tracker_merge_const (" ", 2, xd->make, xd->model);
	} else {
		md.camera = g_strdup (xd->model);
	}

	md.title = tracker_coalesce_strip (3, xd->title, xd->title2, xd->pdf_title);
	md.date = tracker_coalesce_strip (2, xd->date, xd->time_original);
	md.artist = tracker_coalesce_strip (2, xd->artist, xd->contributor);

	/* if (gd.comment) { */
	/* 	tracker_sparql_builder_predicate (metadata, "nie:comment"); */
	/* 	tracker_sparql_builder_object_unvalidated (metadata, md.comment); */
	/* } */

	if (xd->license) {
		tracker_sparql_builder_predicate (metadata, "nie:license");
		tracker_sparql_builder_object_unvalidated (metadata, xd->license);
	}

	if (xd->creator) {
		gchar *uri = tracker_uri_printf_escaped ("urn:contact:%s", xd->creator);

		tracker_sparql_builder_insert_open (preupdate, NULL);
		tracker_sparql_builder_subject_iri (preupdate, uri);
		tracker_sparql_builder_predicate (preupdate, "a");
		tracker_sparql_builder_object (preupdate, "nco:Contact");
		tracker_sparql_builder_predicate (preupdate, "nco:fullname");
		tracker_sparql_builder_object_unvalidated (preupdate, xd->creator);
		tracker_sparql_builder_insert_close (preupdate);

		tracker_sparql_builder_predicate (metadata, "nco:creator");
		tracker_sparql_builder_object_iri (metadata, uri);
		g_free (uri);
	}

	if (md.date) {
		tracker_sparql_builder_predicate (metadata, "nie:contentCreated");
		tracker_sparql_builder_object_unvalidated (metadata, md.date);
	}

	if (xd->description) {
		tracker_sparql_builder_predicate (metadata, "nie:description");
		tracker_sparql_builder_object_unvalidated (metadata, xd->description);
	}

	if (xd->copyright) {
		tracker_sparql_builder_predicate (metadata, "nie:copyright");
		tracker_sparql_builder_object_unvalidated (metadata, xd->copyright);
	}

	if (md.title) {
		tracker_sparql_builder_predicate (metadata, "nie:title");
		tracker_sparql_builder_object_unvalidated (metadata, md.title);
	}

	if (md.camera) {
		tracker_sparql_builder_predicate (metadata, "nmm:camera");
		tracker_sparql_builder_object_unvalidated (metadata, md.camera);
	}

	if (md.artist) {
		gchar *uri = tracker_uri_printf_escaped ("urn:contact:%s", md.artist);

		tracker_sparql_builder_insert_open (preupdate, NULL);
		tracker_sparql_builder_subject_iri (preupdate, uri);
		tracker_sparql_builder_predicate (preupdate, "a");
		tracker_sparql_builder_object (preupdate, "nco:Contact");
		tracker_sparql_builder_predicate (preupdate, "nco:fullname");
		tracker_sparql_builder_object_unvalidated (preupdate, md.artist);
		tracker_sparql_builder_insert_close (preupdate);

		tracker_sparql_builder_predicate (metadata, "nco:contributor");
		tracker_sparql_builder_object_iri (metadata, uri);
		g_free (uri);
	}

	if (xd->orientation) {
		tracker_sparql_builder_predicate (metadata, "nfo:orientation");
		tracker_sparql_builder_object_unvalidated (metadata, xd->orientation);
	}

	if (xd->exposure_time) {
		tracker_sparql_builder_predicate (metadata, "nmm:exposureTime");
		tracker_sparql_builder_object_unvalidated (metadata, xd->exposure_time);
	}

	if (xd->iso_speed_ratings) {
		tracker_sparql_builder_predicate (metadata, "nmm:isoSpeed");
		tracker_sparql_builder_object_unvalidated (metadata, xd->iso_speed_ratings);
	}

	if (xd->white_balance) {
		tracker_sparql_builder_predicate (metadata, "nmm:whiteBalance");
		tracker_sparql_builder_object_unvalidated (metadata, xd->white_balance);
	}

	if (xd->fnumber) {
		tracker_sparql_builder_predicate (metadata, "nmm:fnumber");
		tracker_sparql_builder_object_unvalidated (metadata, xd->fnumber);
	}

	if (xd->flash) {
		tracker_sparql_builder_predicate (metadata, "nmm:flash");
		tracker_sparql_builder_object_unvalidated (metadata, xd->flash);
	}

	if (xd->focal_length) {
		tracker_sparql_builder_predicate (metadata, "nmm:focalLength");
		tracker_sparql_builder_object_unvalidated (metadata, xd->focal_length);
	}

	if (xd->metering_mode) {
		tracker_sparql_builder_predicate (metadata, "nmm:meteringMode");
		tracker_sparql_builder_object_unvalidated (metadata, xd->metering_mode);
	}


	if (xd->keywords) {
		insert_keywords (metadata, uri, xd->keywords);
	}

	if (xd->pdf_keywords) {
		insert_keywords (metadata, uri, xd->pdf_keywords);
	}

	if (xd->rating) {
		tracker_sparql_builder_predicate (metadata, "nao:numericRating");
		tracker_sparql_builder_object_unvalidated (metadata, xd->rating);
	}

	if (xd->subject) {
		insert_keywords (metadata, uri, xd->subject);
	}

	if (xd->publisher) {
		gchar *uri = tracker_uri_printf_escaped ("urn:contact:%s", xd->publisher);

		tracker_sparql_builder_insert_open (preupdate, NULL);
		tracker_sparql_builder_subject_iri (preupdate, uri);
		tracker_sparql_builder_predicate (preupdate, "a");
		tracker_sparql_builder_object (preupdate, "nco:Contact");
		tracker_sparql_builder_predicate (preupdate, "nco:fullname");
		tracker_sparql_builder_object_unvalidated (preupdate, xd->publisher);
		tracker_sparql_builder_insert_close (preupdate);

		tracker_sparql_builder_predicate (metadata, "nco:creator");
		tracker_sparql_builder_object_iri (metadata, uri);
		g_free (uri);
	}

	if (xd->type) {
		tracker_sparql_builder_predicate (metadata, "dc:type");
		tracker_sparql_builder_object_unvalidated (metadata, xd->type);
	}

	if (xd->format) {
		tracker_sparql_builder_predicate (metadata, "dc:format");
		tracker_sparql_builder_object_unvalidated (metadata, xd->format);
	}

	if (xd->identifier) {
		tracker_sparql_builder_predicate (metadata, "dc:identifier");
		tracker_sparql_builder_object_unvalidated (metadata, xd->identifier);
	}

	if (xd->source) {
		tracker_sparql_builder_predicate (metadata, "dc:source");
		tracker_sparql_builder_object_unvalidated (metadata, xd->source);
	}

	if (xd->language) {
		tracker_sparql_builder_predicate (metadata, "dc:language");
		tracker_sparql_builder_object_unvalidated (metadata, xd->language);
	}

	if (xd->relation) {
		tracker_sparql_builder_predicate (metadata, "dc:relation");
		tracker_sparql_builder_object_unvalidated (metadata, xd->relation);
	}

	if (xd->coverage) {
		tracker_sparql_builder_predicate (metadata, "dc:coverage");
		tracker_sparql_builder_object_unvalidated (metadata, xd->coverage);
	}

	if (xd->address || xd->country || xd->city) {
		tracker_sparql_builder_predicate (metadata, "mlo:location");

		tracker_sparql_builder_object_blank_open (metadata);
		tracker_sparql_builder_predicate (metadata, "a");
		tracker_sparql_builder_object (metadata, "mlo:GeoPoint");

		if (xd->address) {
			tracker_sparql_builder_predicate (metadata, "mlo:address");
			tracker_sparql_builder_object_unvalidated (metadata, xd->address);
		}

		if (xd->state) {
			tracker_sparql_builder_predicate (metadata, "mlo:state");
			tracker_sparql_builder_object_unvalidated (metadata, xd->state);
		}

		if (xd->city) {
			tracker_sparql_builder_predicate (metadata, "mlo:city");
			tracker_sparql_builder_object_unvalidated (metadata, xd->city);
		}

		if (xd->country) {
			tracker_sparql_builder_predicate (metadata, "mlo:country");
			tracker_sparql_builder_object_unvalidated (metadata, xd->country);
		}

		tracker_sparql_builder_object_blank_close (metadata);
	}

	if (gd.width) {
		tracker_sparql_builder_predicate (metadata, "nfo:width");
		tracker_sparql_builder_object_unvalidated (metadata, gd.width);
	}

	if (gd.height) {
		tracker_sparql_builder_predicate (metadata, "nfo:height");
		tracker_sparql_builder_object_unvalidated (metadata, gd.height);
	}

	tracker_xmp_free (xd);
	g_free (md.camera);
}


static void
extract_gif (const gchar          *uri,
             TrackerSparqlBuilder *preupdate,
             TrackerSparqlBuilder *metadata)
{
	goffset        size;

	GifFileType   *gifFile = NULL;

	gchar         *filename;

	filename = g_filename_from_uri (uri, NULL, NULL);
	size = tracker_file_get_size (filename);

	if (size < 64) {
		g_free (filename);
		return;
	}

	if ( ( gifFile = DGifOpenFileName(filename) ) == NULL) {
		PrintGifError();
		return;
	}

	g_free (filename);

	tracker_sparql_builder_predicate (metadata, "a");
	tracker_sparql_builder_object (metadata, "nfo:Image");
	tracker_sparql_builder_object (metadata, "nmm:Photo");

	read_metadata (preupdate, metadata, gifFile, uri);

	if(DGifCloseFile(gifFile) != GIF_OK) {
		PrintGifError();
	}

}

TrackerExtractData *
tracker_extract_get_data (void)
{
	return data;
}