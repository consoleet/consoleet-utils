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
#include <libHX/proc.h>
#include <libHX/string.h>

enum {
	FT_SFD = 0,
	FT_BDF,
};

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

	void (*emit_header)(struct bdf_handle *);
	void (*close)(struct bdf_handle *);
};

static char *p_output_file;
static unsigned int p_descent, p_xheight = UINT_MAX;
static unsigned int p_output_filetype;
static int p_optimize = -1;

static struct bdf_handle *generic_open(const char *file)
{
	struct bdf_handle *f = malloc(sizeof(*f));

	if (f == NULL) {
		perror("malloc");
		return NULL;
	}
	f->handle = fopen(file, "w");
	if (f->handle == NULL) {
		int ret = errno;
		fprintf(stderr, "ERROR: Could not open %s for writing: %s\n",
		        file, strerror(errno));
		free(f);
		errno = ret;
		return NULL;
	}
	f->bbox.width = f->bbox.height = 0;
	f->descent = p_descent;
	f->ascent = UINT_MAX;
	f->xheight = p_xheight;
	f->capheight = UINT_MAX;
	f->num_glyphs = UINT_MAX;
	return f;
}

static void generic_close(struct bdf_handle *bdf)
{
	long of;

	if (bdf->ascent == UINT_MAX)
		bdf->ascent = bdf->bbox.height - bdf->descent;
	if (bdf->capheight == UINT_MAX)
		bdf->capheight = bdf->ascent;
	if (bdf->xheight == UINT_MAX)
		bdf->xheight = bdf->ascent / 2;
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
	bdf->emit_header(bdf);
	fclose(bdf->handle);
	free(bdf);
}

static void sfd_emit(struct bdf_handle *font)
{
	FILE *f = font->handle;
	fprintf(f,
		"SplineFontDB: 3.0\n"
		"FontName: newfont\n"
		"FullName: New Font\n"
		"FamilyName: newfont\n"
		"Weight: Medium\n"
		"Copyright: created by clt2bdf/clt2sfd\n"
		"UComments: created by clt2bdf/clt2sfd\n"
		"Version: 001.000\n"
		"ItalicAngle: 0\n"
		"UnderlinePosition: -100\n"
		"UnderlineWidth: 40\n"
	);
	fprintf(f,
		"Ascent: %d\n"
		"Descent: %d\n",
		font->ascent, font->descent
	);
	fprintf(f,
		"NeedsXUIDChange: 1\n"
		"FSType: 0\n"
		"PfmFamily: 33\n"
		"TTFWeight: 500\n"
		"TTFWidth: 5\n"
		"Panose: 2 0 6 4 0 0 0 0 0 0\n"
		"LineGap: 72\n"
		"VLineGap: 0\n"
		"OS2WinAscent: 0\n"
		"OS2WinAOffset: 1\n"
		"OS2WinDescent: 0\n"
		"OS2WinDOffset: 1\n"
		"HheadAscent: 0\n"
		"HheadAOffset: 1\n"
		"HheadDescent: 0\n"
		"HheadDOffset: 1\n"
		"Encoding: UnicodeBmp\n"
		"UnicodeInterp: none\n"
		"DisplaySize: -24\n"
		"AntiAlias: 1\n"
		"FitToEm: 1\n"
		"WinInfo: 0 50 22\n"
		"TeXData: 1 0 0 346030 173015 115343 0 1048576 115343 783286 444596 497025 792723 393216 433062 380633 303038 157286 324010 404750 52429 2506097 1059062 262144\n"
	);
	fprintf(f, "BeginChars: 65536 %d\n", font->num_glyphs);
}

static void sfd_close(struct bdf_handle *font)
{
	fprintf(font->handle, "EndChars\nEndSplineFont\n");
	generic_close(font);
}

static struct bdf_handle *sfd_open(const char *file)
{
	struct bdf_handle *fh = generic_open(file);

	if (fh == NULL)
		return NULL;
	fh->emit_header = sfd_emit;
	fh->close = sfd_close;

	fh->emit_header(fh);
	fprintf(fh->handle, "%-*s", 80, "");
	fh->header_length = ftell(fh->handle);
	fh->num_glyphs = 0;
	return fh;
}

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
	struct bdf_handle *bdf = generic_open(file);
	if (bdf == NULL)
		return NULL;
	bdf->emit_header = bdf_emit;
	bdf->close = generic_close;

	bdf->emit_header(bdf);
	bdf->header_length = ftell(bdf->handle);
	bdf->num_glyphs = 0;
	return bdf;
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
	if (glyph_width > bdf->bbox.width)
		bdf->bbox.width = glyph_width;
	if (glyph_height > bdf->bbox.height)
		bdf->bbox.height = glyph_height;

	if (p_output_filetype == FT_SFD) {
		int y = glyph_height - 1 - bdf->descent;

		fprintf(fp, "StartChar: %04x\n", glyph_number);
		fprintf(fp, "Encoding: %d %d %d\n", glyph_number, glyph_number, glyph_number);
		fprintf(fp, "Width: %d\n", glyph_width);
		fprintf(fp, "TeX: 0 0 0 0\n");
		fprintf(fp, "Fore\n");

		while (HX_getl(&line, glyph_handle) != NULL) {
			const char *p = line;
			unsigned int x;
			for (x = 0, p = line; p[0] != '\0' && p[1] != '\0';
			     ++x, p += 2)
			{
				if ((p[0] == '.' || HX_isspace(p[0])) &&
				    (p[1] == '.' || HX_isspace(p[1])))
					continue;
				fprintf(fp, "%d %d m 25\n", x, y);
				fprintf(fp, " %d %d l 25\n", x, y + 1);
				fprintf(fp, " %d %d l 25\n", x + 1, y + 1);
				fprintf(fp, " %d %d l 25\n", x + 1, y);
				fprintf(fp, " %d %d l 25\n", x, y);
			}
			--y;
		}
		fprintf(fp, "EndSplineSet\nEndChar\n");
	} else if (p_output_filetype == FT_BDF) {
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
	}
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

	while ((node = HXmap_traverse(trav)) != NULL) {
		ret = p_process_glyph_file(bdf, node->data);
		if (ret <= 0) {
			ret = EXIT_FAILURE;
			break;
		} else {
			ret = EXIT_SUCCESS;
		}
	}

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

static int p_run_optimizer(const char *file)
{
	const char *q, *args[] = {"fontforge", "-lang=ff", "-c", NULL, NULL};
	hxmc_t *script = HXmc_strinit("");
	int ret = EXIT_FAILURE;
	char *fm = NULL;

	if (script == NULL)
		return EXIT_FAILURE;
	if (HXmc_strcat(&script, "Open(\"") == NULL)
		goto out;
	q = HX_strquote(file, HXQUOTE_DQUOTE, &fm);
	if (q == NULL)
		goto out;
	if (HXmc_strcat(&script, q) == NULL)
		goto out;
	if (HXmc_strcat(&script, "\"); SelectAll(); RemoveOverlap(); "
	   "Simplify(); Save(\"") == NULL)
		goto out;
	if (HXmc_strcat(&script, q) == NULL)
		goto out;
	if (HXmc_strcat(&script, "\");") == NULL)
		goto out;
	args[3] = script;
	fprintf(stderr, "Running optimizer (FontForge)... %s\n", args[3]);
	ret = HXproc_run_sync(args, 0);
 out:
	free(fm);
	HXmc_free(script);
	return ret;
}

static bool p_get_options(int *argc, const char ***argv)
{
	static const struct HXoption options_table[] = {
		{.ln = "bdf", .type = HXTYPE_VAL, .ptr = &p_output_filetype,
		 .val = FT_BDF, .help = "Generate BDF output (for gbdfed, bdftopcf)"},
		{.ln = "sfd", .type = HXTYPE_VAL, .ptr = &p_output_filetype,
		 .val = FT_SFD, .help = "Generate SFD output (for Fontforge)"},
		{.sh = 'O', .type = HXTYPE_NONE | HXOPT_INC, .ptr = &p_optimize,
		 .help = "Optimize: Postprocess generated file using FontForge"},
		{.sh = 'd', .type = HXTYPE_UINT, .ptr = &p_descent,
		 .help = "Set the font's descent"},
		{.sh = 'o', .type = HXTYPE_STRING, .ptr = &p_output_file,
		 .help = "Emit BDF data to this file", .htyp = "NAME"},
		{.sh = 'x', .type = HXTYPE_UINT, .ptr = &p_xheight,
		 .help = "Set the font's x-height"},
		HXOPT_AUTOHELP,
		HXOPT_TABLEEND,
	};

	p_output_filetype = FT_SFD;
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

	if (p_output_filetype == FT_SFD)
		bdf = sfd_open(p_output_file);
	else if (p_output_filetype == FT_BDF)
		bdf = bdf_open(p_output_file);
	if (bdf == NULL)
		goto out;
	ret = p_process_filemap(bdf, filemap);
 out:
	HXmap_free(filemap);
	if (bdf != NULL)
		bdf->close(bdf);
	if (ret == EXIT_SUCCESS && p_output_filetype == FT_SFD)
		ret = p_run_optimizer(p_output_file);
	return ret;
}
