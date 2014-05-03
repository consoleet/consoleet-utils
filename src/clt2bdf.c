/*
 *	Convert from the Consoleet text bitmap format to BDF
 *	written by Jan Engelhardt, 2014
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the WTF Public License version 2 or
 *	(at your option) any later version.
 */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <libHX/ctype_helper.h>
#include <libHX/io.h>
#include <libHX/map.h>
#include <libHX/option.h>
#include <libHX/string.h>

/**
 * width-height union
 */
union wh {
	struct {
		unsigned int width, height;
	};
	unsigned int v[2];
};

/**
 * @handle:		the stdio handle for the output file
 * @header_length:	maximum length of the text header
 * @bbox:		font's bounding box (maximum size over all glyphs)
 * @descent:		(see your font manual)
 * @ascent:		(see your font manual)
 * @xheight:		(see your font manual)
 * @capheight:		(see your font manual)
 * @num_glyphs:		number of glyphs there will be
 */
struct bdf_handle {
	FILE *handle;
	long header_length;
	union wh bbox;
	unsigned int descent, ascent, xheight, capheight, num_glyphs;
};

static char *p_output_file;

/**
 * @bdf:	in-memory BDF header structure
 *
 * Emit the BDF header stored in @bdf out into the file handle @bdf->handle.
 */
static void bdf_emit(struct bdf_handle *bdf)
{
	FILE *f = bdf->handle;
	fprintf(f,
		"STARTFONT 2.1\n"
		"FONT newname\n"
		"SIZE %u 72 72\n", bdf->bbox.width);
	fprintf(f,
		"FONTBOUNDINGBOX %u %u 0 %u\n",
		bdf->bbox.width, bdf->bbox.height, bdf->descent);
	fprintf(f,
		"STARTPROPERTIES 11\n"
		"CAP_HEIGHT %u\n"
		"DEFAULT_CHAR 0feff\n"
		"FONT_ASCENT %u\n"
		"FONT_DESCENT %u\n",
		bdf->capheight, bdf->ascent, bdf->descent);
	fprintf(f,
		"POINT_SIZE 160\n"
		"QUAD_WIDTH %u\n"
		"RESOLUTION 72\n"
		"RESOLUTION_X 72\n"
		"RESOLUTION_Y 72\n"
		"WEIGHT 5\n"
		"X_HEIGHT %u\n"
		"ENDPROPERTIES\n",
		bdf->bbox.width, bdf->xheight);
	fprintf(f, "CHARS %u\n", bdf->num_glyphs);

}

/**
 * @file:	name of BDF file to create
 *
 * Open @file for writing and initialize the in-memory variables for the
 * BDF header properties.
 */
static struct bdf_handle *bdf_open(const char *file)
{
	struct bdf_handle *bdf = malloc(sizeof(*bdf));

	if (bdf == NULL) {
		perror("malloc");
		assert(bdf != NULL);
	}
	bdf->handle = fopen(file, "w");
	if (bdf->handle == NULL) {
		int ret = errno;
		fprintf(stderr, "ERROR: Could not open %s for writing: %s\n",
		        file, strerror(errno));
		free(bdf);
		errno = ret;
		return NULL;
	}
	bdf->bbox.width = bdf->bbox.height = UINT_MAX;
	bdf->descent = UINT_MAX;
	bdf->ascent = UINT_MAX;
	bdf->xheight = UINT_MAX;
	bdf->capheight = UINT_MAX;
	bdf->num_glyphs = UINT_MAX;
	bdf_emit(bdf);
	bdf->header_length = ftell(bdf->handle);
	bdf->num_glyphs = 0;
	return bdf;
}

static void bdf_close(struct bdf_handle *bdf)
{
	long of;

	if (bdf->ascent == UINT_MAX)
		bdf->ascent = bdf->bbox.height - bdf->descent;
	if (bdf->capheight == UINT_MAX)
		bdf->capheight = bdf->ascent;
	of = fseek(bdf->handle, 0, SEEK_SET);
	if (of < 0) {
		perror("fseek");
		abort();
	}
	while (bdf->header_length-- > 0)
		fputc('\n', bdf->handle);
	of = fseek(bdf->handle, 0, SEEK_SET);
	if (of < 0) {
		perror("fseek");
		abort();
	}
	bdf_emit(bdf);
	fclose(bdf->handle);
	free(bdf);
}

/**
 * @glyph_file:	path to CLT bitmap
 * @outfp:	filehandle to just-generating BDF
 *
 * Read a CLT bitmap file and convert it to a BDF glyph, outputting it on the
 * @outfp stream.
 */
static int p_process_glyph_file(struct bdf_handle *bdf, const char *glyph_file)
{
	unsigned int glyph_width, glyph_height, glyph_number;
	FILE *glyph_handle;
	hxmc_t *line = NULL;
	FILE *fp = bdf->handle;
	char *end;
	int ret;

	glyph_handle = fopen(glyph_file, "r");
	if (glyph_handle == NULL) {
		ret = -errno;
		fprintf(stderr, "%s: %s\n", glyph_file, strerror(errno));
		return ret;
	}
	glyph_file = HX_basename(glyph_file);
	glyph_number = strtoul(glyph_file, &end, 16);
	if (end == glyph_file || (*end != '\0' && *end != '.')) {
		fprintf(stderr, "cannot determine glyph position from filename\n");
		fclose(glyph_handle);
		return 0;
	}
	if (HX_getl(&line, glyph_handle) == NULL ||
	    strcmp(line, "PCLT\n") != 0 ||
	    HX_getl(&line, glyph_handle) == NULL ||
	    sscanf(line, "%u %u\n", &glyph_width, &glyph_height) != 2) {
		fprintf(stderr, "not a CLT file\n");
		HXmc_free(line);
		fclose(glyph_handle);
		return 0;
	}

	++bdf->num_glyphs;
	fprintf(fp, "STARTCHAR U+%04x\n", glyph_number);
	fprintf(fp, "ENCODING %d\n", glyph_number);
	fprintf(fp, "SWIDTH %u 0\n", glyph_width * 1000);
	fprintf(fp, "DWIDTH %u 0\n", glyph_width);
	fprintf(fp, "BBX %u %u 0 %u\n", glyph_width, glyph_height, bdf->descent);
	fprintf(fp, "BITMAP\n");

	while (HX_getl(&line, glyph_handle) != NULL) {
		const char *x = line;
		unsigned int bit = 0, pixel = 0;

		HX_chomp(line);
		for (x = line; x[0] != '\0' && x[1] != '\0'; x += 2) {
			if ((x[0] == '.' || HX_isspace(x[0])) &&
			    (x[1] == '.' || HX_isspace(x[1])))
				/* whitespace */;
			else
				pixel |= 1 << (7 - bit);
			if (bit++ != 8)
				continue;
			fprintf(fp, "%02x", pixel);
			pixel = 0;
			bit = 0;
		}
		if (bit > 0) {
			fprintf(fp, "%02x", pixel);
			pixel = 0;
			bit = 0;
		}
		fprintf(fp, "\n");
	}

	fprintf(fp, "ENDCHAR\n");
	fclose(glyph_handle);
	HXmc_free(line);
	return 1;
}

static int
p_process_filemap(struct bdf_handle *bdf, const struct HXmap *filemap)
{
	struct HXmap_trav *trav = HXmap_travinit(filemap, HXMAP_NOFLAGS);
	const struct HXmap_node *node;
	int ret = EXIT_SUCCESS;

	while ((node = HXmap_traverse(trav)) != NULL)
		ret = p_process_glyph_file(bdf, node->data);

	HXmap_travfree(trav);
	return ret;
}

/**
 * @dir_handle:	directory handle for currently-inspected directory
 */
static int p_collect_files_dh(struct HXmap *filemap,
    const char *dir_path, struct HXdir *dir_handle)
{
	struct stat sb;
	const char *de;
	hxmc_t *filepath = NULL;
	int ret;

	while ((de = HXdir_read(dir_handle)) != NULL) {
		if (HXmc_strcpy(&filepath, dir_path) == NULL ||
		    HXmc_strcat(&filepath, "/") == NULL ||
		    HXmc_strcat(&filepath, de) == NULL)
			abort();
		if (stat(filepath, &sb) < 0 || !S_ISREG(sb.st_mode))
			continue;
		ret = HXmap_add(filemap, de, filepath);
		if (ret < 0) {
			fprintf(stderr, "%s\n", strerror(-ret));
			return EXIT_FAILURE;
		}
	}
	HXmc_free(filepath);
	return EXIT_SUCCESS;
}

static int p_collect_files(struct HXmap *map, const char *dir_name)
{
	struct HXdir *dir_handle = HXdir_open(dir_name);
	int ret;

	if (dir_handle == NULL) {
		fprintf(stderr, "ERROR: Could not read %s: %s\n",
		        dir_name, strerror(errno));
		return EXIT_FAILURE;
	}
	ret = p_collect_files_dh(map, dir_name, dir_handle);
	HXdir_close(dir_handle);
	return ret;
}

static bool p_get_options(int *argc, const char ***argv)
{
	static const struct HXoption options_table[] = {
		{.sh = 'o', .type = HXTYPE_STRING, .ptr = &p_output_file,
		 .help = "Emit BDF data to this file", .htyp = "NAME"},
		HXOPT_AUTOHELP,
		HXOPT_TABLEEND,
	};

	if (HX_getopt(options_table, argc, argv, HXOPT_USAGEONERR) !=
	    HXOPT_ERR_SUCCESS)
		return false;
	if (p_output_file == NULL) {
		fprintf(stderr, "ERROR: You need to specify an "
		        "output file with -o\n");
		return false;
	}
	return true;
}

int main(int argc, const char **argv)
{
	int ret = EXIT_SUCCESS;
	struct bdf_handle *bdf = NULL;
	struct HXmap *filemap;

	if (!p_get_options(&argc, &argv))
		return EXIT_FAILURE;

	/*
	 * gbdfed likes to have all characters ordered by their Unicode point,
	 * so sort-filter it through a sorted map.
	 */
	filemap = HXmap_init(HXMAPT_ORDERED, HXMAP_SCKEY | HXMAP_SCDATA);
	while (*++argv != NULL) {
		ret = p_collect_files(filemap, *argv);
		if (ret != EXIT_SUCCESS)
			goto out;
	}

	bdf = bdf_open(p_output_file);
	if (bdf == NULL)
		goto out;
	ret = p_process_filemap(bdf, filemap);
 out:
	HXmap_free(filemap);
	if (bdf != NULL)
		bdf_close(bdf);
	return ret;
}
