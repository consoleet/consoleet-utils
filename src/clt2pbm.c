/*
 *	Convert from the Consoleet text bitmap format to PBM Portable Bitmap
 *	written by Jan Engelhardt, 2014
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the WTF Public License version 2 or
 *	(at your option) any later version.
 */
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <libHX/ctype_helper.h>
#include <libHX/defs.h>
#include <libHX/option.h>
#include <libHX/string.h>

static unsigned int x_scale_factor = 1, y_scale_factor = 1;
static unsigned int xy_scale_factor = 0;

static int p_process_handle(const char *inname, FILE *infile, FILE *outfile)
{
	hxmc_t *line = NULL;
	unsigned int width = 0, height = 0;
	unsigned int t, x, y;
	const char *p;

	fprintf(stderr, "Converting %s\n", inname);
	assert(HX_getl(&line, infile) != NULL);
	assert(strcmp(line, "PCLT\n") == 0);
	assert(HX_getl(&line, infile) != NULL);
	sscanf(line, "%u %u\n", &width, &height);
	fprintf(outfile, "P1\n%u %u\n",
	        width * x_scale_factor, height * y_scale_factor);

	while (HX_getl(&line, infile) != NULL) {
		HX_chomp(line);
		for (y = 0; y < y_scale_factor; ++y) {
			for (p = line, t = 0; *p != '\0' && t < width;
			     p += 2, ++t)
				for (x = 0; x < x_scale_factor; ++x) {
					fputc((*p == '.' || HX_isspace(*p)) ?
					      '0' : '1', outfile);
					fputc(' ', outfile);
				}
			fputc('\n', outfile);
		}
	}
	return 1;
}

static int p_process_file(const char *file)
{
	FILE *infile, *outfile;
	char *buf, *p;
	int ret = EXIT_FAILURE;

	buf = malloc(strlen(file) + 5);
	if (buf == NULL) {
		perror("malloc");
		return -errno;
	}
	strcpy(buf, file);
	if ((p = strrchr(buf, '.')) != NULL)
		*p = '\0';
	strcat(buf, ".ppm");
	infile = fopen(file, "r");
	if (infile == NULL) {
		fprintf(stderr, "ERROR: Could not open %s for reading: %s\n",
		        file, strerror(errno));
		goto out;
	}
	outfile = fopen(buf, "w");
	if (outfile == NULL) {
		fprintf(stderr, "ERROR: Could not open %s for writing: %s\n",
		        buf, strerror(errno));
		goto out;
	}
	ret = p_process_handle(file, infile, outfile);

 out:
	if (outfile != NULL)
		fclose(outfile);
	if (infile != NULL)
		fclose(infile);
	free(buf);
	return ret;
}

static bool p_get_options(int *argc, const char ***argv)
{
	static const struct HXoption option_table[] = {
		{.sh = 'x', .type = HXTYPE_UINT, .ptr = &x_scale_factor,
		 .help = "Scale output horizontally by given factor",
		 .htyp = "factor"},
		{.sh = 'y', .type = HXTYPE_UINT, .ptr = &y_scale_factor,
		 .help = "Scale output vertically by given factor",
		 .htyp = "factor"},
		{.sh = 's', .type = HXTYPE_UINT, .ptr = &xy_scale_factor,
		 .help = "Scale output (in both directions) by given factor",
		 .htyp = "factor"},
		HXOPT_AUTOHELP,
		HXOPT_TABLEEND,
	};
	int ret = HX_getopt(option_table, argc, argv, HXOPT_USAGEONERR);
	if (xy_scale_factor != 0)
		x_scale_factor = y_scale_factor = xy_scale_factor;
	return ret == HXOPT_ERR_SUCCESS;
}

int main(int argc, const char **argv)
{
	int ret;

	if (!p_get_options(&argc, &argv))
		return EXIT_FAILURE;
	if (argc == 1)
		ret = p_process_handle("(stdin)", stdin, stdout);
	else
		while (*++argv != NULL) {
			ret = p_process_file(*argv);
			if (ret <= 0) {
				ret = EXIT_FAILURE;
				break;
			}
		}
	return ret;
}
