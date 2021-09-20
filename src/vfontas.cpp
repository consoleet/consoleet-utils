/*
 *	Command-line interface of the "VGA font assembler"
 *	written by Jan Engelhardt, 2019
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	3 of the License, or (at your option) any later version.
 *	For details, see the file named "LICENSE.GPL3".
 */
#include "config.h"
#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <libHX/defs.h>
#include <libHX/io.h>
#include <libHX/string.h>
#include "vfalib.hpp"
#define MMAP_NONE reinterpret_cast<void *>(-1)

using namespace vfalib;

/* CPI: see http://www.seasip.info/DOS/CPI/cpi.html */
struct cpi_fontfile_header {
	uint8_t id0;
	char id[7], reserved[8];
	uint16_t pnum;
	uint8_t ptyp;
	uint32_t fih_offset;
} __attribute__((packed));

struct cpi_fontinfo_header {
	uint16_t num_codepages;
} __attribute__((packed));

struct cpi_cpentry_header {
	uint16_t cpeh_size;
	uint32_t next_cpeh_offset;
	uint16_t device_type;
	char device_name[8];
	uint16_t codepage;
	char reserved[6];
	uint32_t cpih_offset;
} __attribute__((packed));

struct cpi_cpinfo_header {
	uint16_t version;
	uint16_t num_fonts;
	uint16_t size;
} __attribute__((packed));

struct cpi_screenfont_header {
	uint8_t height, width, yaspect, xaspect;
	uint16_t num_chars;
} __attribute__((packed));

static bool vf_blankfnt(font &f, char **args)
{
	f.init_256_blanks();
	return true;
}

static bool vf_canvas(font &f, char **args)
{
	auto x = strtol(args[0], nullptr, 0);
	auto y = strtol(args[1], nullptr, 0);
	if (x < 0 || y < 0) {
		fprintf(stderr, "Error: Canvas size should be positive.\n");
		return false;
	}
	if (f.m_glyph.size() > 0)
		f.blit(vfpos() | f.m_glyph[0].m_size, vfpos() | vfsize(x, y));
	return true;
}

static bool vf_clearmap(font &f, char **args)
{
	f.m_unicode_map.reset();
	return true;
}

static bool vf_crop(font &f, char **args)
{
	auto x = strtol(args[0], nullptr, 0);
	auto y = strtol(args[1], nullptr, 0);
	auto w = strtol(args[2], nullptr, 0);
	auto h = strtol(args[3], nullptr, 0);
	if (x < 0 || y < 0) {
		fprintf(stderr, "Error: Crop xpos/ypos must be positive.\n");
		return false;
	}
	if (w <= 0 || h <= 0) {
		fprintf(stderr, "Error: Crop width/height must be positive non-zero.\n");
		return false;
	}
	if (f.m_glyph.size() > 0)
		f.blit(vfpos(x, y) | f.m_glyph[0].m_size, vfpos() | vfsize(w, h));
	return true;
}

static bool vf_fliph(font &f, char **args)
{
	f.flip(true, false);
	return true;
}

static bool vf_flipv(font &f, char **args)
{
	f.flip(false, true);
	return true;
}

static bool vf_invert(font &f, char **args)
{
	f.invert();
	return true;
}

static bool vf_lge(font &f, char **args)
{
	f.lge();
	return true;
}

static bool vf_loadbdf(font &f, char **args)
{
	auto ret = f.load_bdf(args[0]);
	if (ret >= 0)
		return true;
	fprintf(stderr, "Error loading %s: %s\n", args[0], strerror(-ret));
	return false;
}

static bool vf_loadclt(font &f, char **args)
{
	auto ret = f.load_clt(args[0]);
	if (ret >= 0)
		return true;
	fprintf(stderr, "Error loading %s: %s\n", args[0], strerror(-ret));
	return false;
}

static bool vf_loadfnt(font &f, char **args)
{
	auto ret = f.load_fnt(args[0]);
	if (ret >= 0)
		return true;
	fprintf(stderr, "Error loading %s: %s\n", args[0], strerror(-ret));
	return false;
}

static bool vf_loadfnth(font &f, char **args)
{
	auto ret = f.load_fnt(args[0], atoi(args[1]));
	if (ret >= 0)
		return true;
	fprintf(stderr, "Error loading %s: %s\n", args[0], strerror(-ret));
	return false;
}

static bool vf_loadhex(font &f, char **args)
{
	auto ret = f.load_hex(args[0]);
	if (ret >= 0)
		return true;
	fprintf(stderr, "Error loading %s: %s\n", args[0], strerror(-ret));
	return false;
}

static bool vf_loadmap(font &f, char **args)
{
	if (f.m_unicode_map == nullptr)
		f.m_unicode_map = std::make_shared<unicode_map>();
	auto ret = f.m_unicode_map->load(args[0]);
	if (ret >= 0)
		return true;
	fprintf(stderr, "Error loading %s: %s\n", args[0], strerror(-ret));
	return false;
}

static bool vf_loadpsf(font &f, char **args)
{
	auto ret = f.load_psf(args[0]);
	if (ret >= 0)
		return true;
	fprintf(stderr, "Error loading %s: %s\n", args[0], strerror(-ret));
	return false;
}

static bool vf_savebdf(font &f, char **args)
{
	auto ret = f.save_bdf(args[0]);
	if (ret >= 0)
		return true;
	fprintf(stderr, "Error saving %s: %s\n", args[0], strerror(-ret));
	return false;
}

static bool vf_saveclt(font &f, char **args)
{
	auto ret = f.save_clt(args[0]);
	if (ret >= 0)
		return true;
	fprintf(stderr, "Error saving %s/: %s\n", args[0], strerror(-ret));
	return false;
}

static bool vf_savefnt(font &f, char **args)
{
	auto ret = f.save_fnt(args[0]);
	if (ret >= 0)
		return true;
	fprintf(stderr, "Error saving %s: %s\n", args[0], strerror(-ret));
	return false;
}

static bool vf_savemap(font &f, char **args)
{
	auto ret = f.save_map(args[0]);
	if (ret >= 0)
		return true;
	fprintf(stderr, "Error saving %s: %s\n", args[0], strerror(-ret));
	return false;
}

static bool vf_savepbm(font &f, char **args)
{
	auto ret = f.save_pbm(args[0]);
	if (ret >= 0)
		return true;
	fprintf(stderr, "Error saving %s: %s\n", args[0], strerror(-ret));
	return false;
}

static bool vf_savepsf(font &f, char **args)
{
	auto ret = f.save_psf(args[0]);
	if (ret >= 0)
		return true;
	fprintf(stderr, "Error saving %s: %s\n", args[0], strerror(-ret));
	return false;
}

static bool vf_savesfd(font &f, char **args)
{
	auto ret = f.save_sfd(args[0], vectoalg::V_SIMPLE);
	if (ret >= 0)
		return true;
	fprintf(stderr, "Error saving %s: %s\n", args[0], strerror(-ret));
	return false;
}

static bool vf_saven1(font &f, char **args)
{
	auto ret = f.save_sfd(args[0], vectoalg::V_N1);
	if (ret >= 0)
		return true;
	fprintf(stderr, "Error saving %s: %s\n", args[0], strerror(-ret));
	return false;
}

static bool vf_saven2(font &f, char **args)
{
	auto ret = f.save_sfd(args[0], vectoalg::V_N2);
	if (ret >= 0)
		return true;
	fprintf(stderr, "Error saving %s: %s\n", args[0], strerror(-ret));
	return false;
}

static bool vf_saven2ev(font &f, char **args)
{
	auto ret = f.save_sfd(args[0], vectoalg::V_N2EV);
	if (ret >= 0)
		return true;
	fprintf(stderr, "Error saving %s: %s\n", args[0], strerror(-ret));
	return false;
}

static bool vf_setbold(font &f, char **args)
{
	f.props.insert_or_assign("TTFWeight", "700");
	f.props.insert_or_assign("StyleMap", "0x0020");
	f.props.insert_or_assign("Weight", "bold");
	return true;
}

static bool vf_setname(font &f, char **args)
{
	std::string ps_name = args[0];
	/* PostScript name does not allow spaces */
	std::replace(ps_name.begin(), ps_name.end(), ' ', '-');
	f.props.insert_or_assign("FontName", std::move(ps_name));
	f.props.insert_or_assign("FullName", args[0]);
	f.props.insert_or_assign("FamilyName", args[0]);
	f.props.emplace("Weight", "medium");
	return true;
}

static bool vf_setprop(font &f, char **args)
{
	f.props.insert_or_assign(args[0], args[1]);
	return true;
}

static bool vf_upscale(font &f, char **args)
{
	auto xf = strtol(args[0], nullptr, 0);
	auto yf = strtol(args[1], nullptr, 0);
	if (xf <= 0 || xf <= 0) {
		fprintf(stderr, "Error: scaling factor(s) should be positive and not zero.\n");
		return false;
	}
	f.upscale(vfsize(xf, yf));
	return true;
}

static void vf_extract_cpi3(const char *sfhblk, unsigned int num_fonts,
    const std::string &directory)
{
	for (unsigned int i = 0; i < num_fonts; ++i) {
		struct cpi_screenfont_header sfh;
		memcpy(&sfh, sfhblk, sizeof(sfh));
		sfh.num_chars = le16_to_cpu(sfh.num_chars);

		char buf[HXSIZEOF_Z32*3];
		snprintf(buf, sizeof(buf), "%ux%u.fnt", sfh.width, sfh.height);
		auto out_file = directory + "/" + buf;
		printf("Writing to %s\n", out_file.c_str());
		HX_mkdir(directory.c_str(), S_IRWXUGO);
		auto out_fd = open(out_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUGO | S_IWUGO);
		if (out_fd < 0) {
			fprintf(stderr, "Error writing to %s: %s\n", out_file.c_str(), strerror(errno));
			continue;
		}

		auto length = sfh.width * sfh.height / 8 * sfh.num_chars;
		write(out_fd, sfhblk + sizeof(sfh), length);
		close(out_fd);
		sfhblk += sizeof(sfh) + length;
	}
}

static int vf_extract_cpi2(const char *vdata, const char *directory)
{
	struct cpi_fontfile_header ffh;
	memcpy(&ffh, vdata, sizeof(ffh));
	ffh.pnum = le16_to_cpu(ffh.pnum);
	ffh.fih_offset = le32_to_cpu(ffh.fih_offset);

	if (ffh.id0 != 0xFF || strncmp(ffh.id, "FONT    ", sizeof(ffh.id)) != 0 ||
	    ffh.pnum != 1 || ffh.ptyp != 1)
		return -EINVAL;

	struct cpi_fontinfo_header fih;
	memcpy(&fih, vdata + ffh.fih_offset, sizeof(fih));
	fih.num_codepages = le16_to_cpu(fih.num_codepages);

	const char *cpeblk = vdata + ffh.fih_offset + sizeof(fih);
	for (unsigned int i = 0; i < fih.num_codepages; ++i) {
		struct cpi_cpentry_header cpeh;
		memcpy(&cpeh, cpeblk, sizeof(cpeh));
		cpeh.cpeh_size        = le16_to_cpu(cpeh.cpeh_size);
		cpeh.next_cpeh_offset = le32_to_cpu(cpeh.next_cpeh_offset);
		cpeh.device_type      = le16_to_cpu(cpeh.device_type);
		cpeh.codepage         = le16_to_cpu(cpeh.codepage);
		cpeh.cpih_offset      = le32_to_cpu(cpeh.cpih_offset);
		cpeblk                = vdata + cpeh.next_cpeh_offset;

		printf("CPEH #%u: Name: %.*s, Codepage: %u\n",
		       i, static_cast<int>(sizeof(cpeh.device_name)),
		       cpeh.device_name, cpeh.codepage);
		if (cpeh.device_type != 1)
			/* non-screen */
			continue;

		struct cpi_cpinfo_header cpih;
		memcpy(&cpih, vdata + cpeh.cpih_offset, sizeof(cpih));
		cpih.version   = le16_to_cpu(cpih.version);
		cpih.num_fonts = le16_to_cpu(cpih.num_fonts);
		cpih.size      = le16_to_cpu(cpih.size);
		if (cpih.version != 1)
			continue;

		char buf[HXSIZEOF_Z32*2];
		*buf = '\0';
		HX_strlncat(buf, cpeh.device_name, sizeof(buf), sizeof(cpeh.device_name));
		HX_strrtrim(buf);
		auto out_dir = std::string(directory) + "/" + buf + "/";
		snprintf(buf, sizeof(buf), "%u", cpeh.codepage);
		out_dir += buf;
		vf_extract_cpi3(vdata + cpeh.cpih_offset + sizeof(cpih), cpih.num_fonts, out_dir);
	}
	return true;
}

static bool vf_xcpi(font &f, char **args)
{
	auto in_fd = open(args[0], O_RDONLY);
	if (in_fd < 0) {
		fprintf(stderr, "Could not open %s: %s\n", args[0], strerror(errno));
		return false;
	}
	auto fdclean = make_scope_success([&]() { close(in_fd); });
	struct stat sb;
	if (fstat(in_fd, &sb) < 0) {
		fprintf(stderr, "fstat: %s\n", strerror(errno));
		return false;
	}

	auto mapping = mmap(nullptr, sb.st_size, PROT_READ, MAP_SHARED, in_fd, 0);
	if (mapping == MMAP_NONE) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		return false;
	}
	auto mapclean = make_scope_success([&]() { munmap(mapping, sb.st_size); });
	auto ret = vf_extract_cpi2(static_cast<const char *>(mapping), args[1]);
	if (ret == -EINVAL) {
		fprintf(stderr, "xcpi: file \"%s\" not recognized\n", args[0]);
		return false;
	}
	return ret >= 0;
}

static bool vf_xlat(font &f, char **args)
{
	auto x = strtol(args[0], nullptr, 0);
	auto y = strtol(args[1], nullptr, 0);
	if (f.m_glyph.size() > 0)
		f.blit(vfpos() | f.m_glyph[0].m_size, vfpos(x, y) | f.m_glyph[0].m_size);
	return true;
}

static const struct vf_command {
	const char *cmd;
	unsigned int nargs;
	bool (*func)(font &f, char **args);
} vf_commlist[] = {
	{"blankfnt", 0, vf_blankfnt},
	{"canvas", 2, vf_canvas},
	{"clearmap", 0, vf_clearmap},
	{"crop", 4, vf_crop},
	{"fliph", 0, vf_fliph},
	{"flipv", 0, vf_flipv},
	{"invert", 0, vf_invert},
	{"lge", 0, vf_lge},
	{"loadbdf", 1, vf_loadbdf},
	{"loadclt", 1, vf_loadclt},
	{"loadfnt", 1, vf_loadfnt},
	{"loadfnth", 2, vf_loadfnth},
	{"loadhex", 1, vf_loadhex},
	{"loadmap", 1, vf_loadmap},
	{"loadpsf", 1, vf_loadpsf},
	{"savebdf", 1, vf_savebdf},
	{"saveclt", 1, vf_saveclt},
	{"savefnt", 1, vf_savefnt},
	{"savemap", 1, vf_savemap},
	{"saven1", 1, vf_saven1},
	{"saven2", 1, vf_saven2},
	{"saven2ev", 1, vf_saven2ev},
	{"savepbm", 1, vf_savepbm},
	{"savepsf", 1, vf_savepsf},
	{"savesfd", 1, vf_savesfd},
	{"setbold", 0, vf_setbold},
	{"setname", 1, vf_setname},
	{"setprop", 2, vf_setprop},
	{"upscale", 2, vf_upscale},
	{"xcpi", 2, vf_xcpi},
	{"xlat", 2, vf_xlat},
};

int main(int argc, char **argv)
{
	--argc;
	++argv;
	if (argc == 0) {
		fprintf(stderr, "You should specify some commlist.\n");
		return EXIT_FAILURE;
	}
	font f;
	while (argc > 0) {
		if (argv[0][0] == '-')
			++argv[0];
		auto ce = static_cast<const vf_command *>(bsearch(argv[0], vf_commlist, ARRAY_SIZE(vf_commlist), sizeof(*vf_commlist),
			[](const void *cmd, const void *arr) -> int {
				return strcmp(static_cast<const char *>(cmd), static_cast<const vf_command *>(arr)->cmd);
			}));
		if (ce == nullptr) {
			fprintf(stderr, "Error: Unknown command \"%s\"\n", argv[0]);
			return EXIT_FAILURE;
		}
		--argc;
		if (static_cast<unsigned int>(argc) < ce->nargs) {
			fprintf(stderr, "Error: Command \"%s\" requires %u arguments.\n", argv[0], ce->nargs);
			return EXIT_FAILURE;
		}
		if (!ce->func(f, ++argv))
			return EXIT_FAILURE;
		argc -= ce->nargs;
		argv += ce->nargs;
	}
	return EXIT_SUCCESS;
}
