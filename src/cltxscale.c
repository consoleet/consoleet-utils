/*
 *	Scale a Consoleet text bitmap with XBRZ
 *	written by Jan Engelhardt, 2014
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the WTF Public License version 2 or
 *	(at your option) any later version.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libHX/ctype_helper.h>
#include <libHX/option.h>
#include <libHX/string.h>
#include "xbrz_call.h"

static unsigned int t_scale_factor = 5;
static char *t_input_file, *t_output_file;

static int clt_convert(FILE *ifh, FILE *ofh)
{
	hxmc_t *line = NULL;
	unsigned int gl_width, gl_height;

	if (HX_getl(&line, ifh) == NULL ||
	    strcmp(line, "PCLT\n") != 0 ||
	    HX_getl(&line, ifh) == NULL ||
	    sscanf(line, "%u %u\n", &gl_width, &gl_height) != 2) {
		printf("Not a CLT file\n");
		HXmc_free(line);
		return 0;
	}

	uint32_t *bm = calloc((gl_width + 2) * (gl_height + 2), sizeof(*bm));
	uint32_t *bmout = calloc((gl_width + 2) * t_scale_factor * (gl_height + 2) * t_scale_factor, sizeof(*bmout));
	if (bm == NULL || bmout == NULL)
		abort();

	unsigned int y = 0;
	while (HX_getl(&line, ifh) != NULL) {
		const char *p = line;
		unsigned int x;
		for (x = 0, p = line;
		     p[0] != '\0' && p[1] != '\0' && x < gl_width;
		     ++x, p += 2)
		{
			bool empty = ((p[0] == '.' || HX_isspace(p[0])) &&
			    (p[1] == '.' || HX_isspace(p[1])));
			bm[y*(gl_width+2)+x+1] = empty ? 0 : ~0U;
		}
		++y;
	}

	xbrz_scale(t_scale_factor, bm, bmout, gl_width + 2, gl_height + 2);
	gl_height *= t_scale_factor;
	gl_width *= t_scale_factor;
	fprintf(ofh, "PCLT\n%u %u\n", gl_width, gl_height);
	for (y = t_scale_factor; y < gl_height + t_scale_factor; ++y) {
		static const char s[][3] = {"..", "##"};
		for (unsigned int x = t_scale_factor; x < gl_width + t_scale_factor; ++x)
			fputs(s[bmout[y*(gl_width+2*t_scale_factor)+x] != 0], ofh);
		fprintf(ofh, "\n");
	}
	free(bm);
	free(bmout);
	return 0;
}

static const struct HXoption clt_options_table[] = {
	{.sh = 'f', .type = HXTYPE_UINT, .ptr = &t_scale_factor,
	 .help = "Scaling factor (2--5)", .htyp = "N"},
	{.sh = 'i', .type = HXTYPE_STRING, .ptr = &t_input_file,
	 .help = "Input file", .htyp = "NAME"},
	{.sh = 'o', .type = HXTYPE_STRING, .ptr = &t_output_file,
	 .help = "Output file", .htyp = "NAME"},
	HXOPT_AUTOHELP,
	HXOPT_TABLEEND,
};

static bool clt_get_options(int *argc, const char ***argv)
{
	int ret;
	ret = HX_getopt(clt_options_table, argc, argv, HXOPT_USAGEONERR);
	if (ret != HXOPT_ERR_SUCCESS)
		return false;
	if (t_scale_factor < 2 || t_scale_factor > 5) {
		fprintf(stderr, "Scaling factor needs to be >= 2 and <= 5\n");
		return false;
	}
	if (t_input_file == NULL) {
		fprintf(stderr, "You have to specify an input file (-i)\n");
		return false;
	}
	if (t_output_file == NULL) {
		fprintf(stderr, "You have to specify an output file (-o)\n");
		return false;
	}
	return true;
}

int main(int argc, const char **argv)
{
	FILE *ifh, *ofh;
	int ret;

	if (!clt_get_options(&argc, &argv))
		return EXIT_FAILURE;
	ifh = fopen(t_input_file, "r");
	if (ifh == NULL) {
		fprintf(stderr, "Could not open %s for reading: %s\n",
		        t_input_file, strerror(errno));
		return EXIT_FAILURE;
	}
	ofh = fopen(t_output_file, "w");
	if (ofh == NULL) {
		fprintf(stderr, "Could not open %s for writing: %s\n",
		        t_output_file, strerror(errno));
		return EXIT_FAILURE;
	}
	ret = clt_convert(ifh, ofh);
	fclose(ifh);
	fclose(ofh);
	return ret;
}
