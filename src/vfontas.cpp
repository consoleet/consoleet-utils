// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2019–2025 Jan Engelhardt
/*
 *	Command-line interface of the "VGA font assembler"
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

enum cpi_device_type {
	DEVTYPE_SCREEN = 1,
	DEVTYPE_PRINTER,
};

/**
 * @device_name:	for screens, usually "EGA", or perhaps "LCD".
 * 			for printers, usually "4201", "4208", "5202",
 * 			"1050", "EPS", "PPDS"
 */
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

struct cpi_printfont_header {
	uint16_t printer_type, escape_length;
} __attribute__((packed));

static std::string cpi_separator;

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
		f.copy_to_blank(vfpos() | f.m_glyph[0].m_size, vfpos() | vfsize(x, y));
	return true;
}

static bool vf_clearmap(font &f, char **args)
{
	f.m_unicode_map.reset();
	return true;
}

static bool vf_copy(font &f, char **args)
{
	auto x = strtol(args[0], nullptr, 0);
	auto y = strtol(args[1], nullptr, 0);
	auto w = strtol(args[2], nullptr, 0);
	auto h = strtol(args[3], nullptr, 0);
	auto bx = strtol(args[4], nullptr, 0);
	auto by = strtol(args[5], nullptr, 0);
	if (x < 0 || y < 0) {
		fprintf(stderr, "Error: Crop xpos/ypos must be positive.\n");
		return false;
	}
	if (w <= 0 || h <= 0) {
		fprintf(stderr, "Error: Crop width/height must be positive non-zero.\n");
		return false;
	}
	if (f.m_glyph.size() > 0)
		f.copy_rect(vfpos(x, y) | vfsize(w, h), vfpos(bx, by) | f.m_glyph[0].m_size);
	return true;
}

static bool vf_cpisep(font &f, char **args)
{
	cpi_separator = args[0];
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
		f.copy_to_blank(vfpos(x, y) | f.m_glyph[0].m_size, vfpos() | vfsize(w, h));
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

static bool vf_lgeu(font &f, char **args)
{
	f.lgeu();
	return true;
}

static bool vf_lgeuf(font &f, char **args)
{
	f.lgeuf();
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

static bool vf_loadraw(font &f, char **args)
{
	auto ret = f.load_fnt(args[0], atoi(args[1]), atoi(args[2]));
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

static bool vf_loadpcf(font &f, char **args)
{
	auto ret = f.load_pcf(args[0]);
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

static bool vf_move(font &f, char **args)
{
	auto x = strtol(args[0], nullptr, 0);
	auto y = strtol(args[1], nullptr, 0);
	if (f.m_glyph.size() <= 0)
		return true;
	f.copy_to_blank(vfpos() | f.m_glyph[0].m_size, vfpos(x, y) | f.m_glyph[0].m_size);
	return true;
}

static bool vf_overstrike(font &f, char **args)
{
	f.overstrike(strtoul(args[0], nullptr, 0));
	return true;
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
	if (xf <= 0 || yf <= 0) {
		fprintf(stderr, "Error: scaling factor(s) should be positive and greater than zero.\n");
		return false;
	}
	f.upscale(vfsize(xf, yf));
	return true;
}

static uint32_t xlate_segoff(uint32_t x)
{
	return (x >> 12) + (x & 0xFFFF);
}

static void vf_extract_sfh(const char *sfhblk, unsigned int num_fonts,
    const std::string &tpl_dir, const char *dev, const char *cpg)
{
	for (unsigned int i = 0; i < num_fonts; ++i) {
		struct cpi_screenfont_header sfh;
		memcpy(&sfh, sfhblk, sizeof(sfh));
		sfh.num_chars = le16_to_cpu(sfh.num_chars);
		printf("SFH: %ux%u pixels x %u chars\n", sfh.width, sfh.height,
		       sfh.num_chars);
		if (sfh.width == 0 || sfh.height == 0 || sfh.num_chars == 0)
			/* Avoid producing empty files */
			continue;

		char buf[HXSIZEOF_Z32*3];
		snprintf(buf, sizeof(buf), "%ux%u.fnt", sfh.width, sfh.height);
		auto out_dir = tpl_dir;
		auto out_file = tpl_dir;
		if (cpi_separator.size() > 0) {
			out_file += std::string("/") + dev + cpi_separator +
			            cpg + cpi_separator + buf;
		} else {
			out_dir += std::string("/") + dev + "/" + cpg;
			out_file = out_dir + "/" + buf;
		}
		printf("Writing to %s\n", out_file.c_str());
		HX_mkdir(out_dir.c_str(), S_IRWXUGO);
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

static void vf_extract_pfh(const char *pfhblk)
{
	struct cpi_printfont_header pfh;
	memcpy(&pfh, pfhblk, sizeof(pfh));
	pfh.printer_type = le16_to_cpu(pfh.printer_type);
	pfh.escape_length = le16_to_cpu(pfh.escape_length);
	printf("PFH: printer_type=%u escape_len=%u\n",
	       pfh.printer_type, pfh.escape_length);
}

static int vf_extract_cpi2(const char *vdata, size_t vsize,
    const char *directory, bool seg_mode)
{
	struct cpi_fontfile_header ffh;
	struct cpi_fontinfo_header fih;

	memcpy(&ffh, vdata, sizeof(ffh));
	ffh.pnum = le16_to_cpu(ffh.pnum);
	ffh.fih_offset = le32_to_cpu(ffh.fih_offset);

	if (ffh.id0 != 0xFF || strncmp(ffh.id, "FONT    ", sizeof(ffh.id)) != 0 ||
	    ffh.pnum != 1 || ffh.ptyp != 1)
		return -EINVAL;
	if (ffh.fih_offset + sizeof(fih) >= vsize)
		return -EINVAL;

	memcpy(&fih, vdata + ffh.fih_offset, sizeof(fih));
	fih.num_codepages = le16_to_cpu(fih.num_codepages);

	const char *cpeblk = vdata + ffh.fih_offset + sizeof(fih);
	for (unsigned int i = 0; i < fih.num_codepages; ++i) {
		struct cpi_cpentry_header cpeh;
		struct cpi_cpinfo_header cpih;

		if (cpeblk - vdata + sizeof(cpeh) >= vsize)
			return -EINVAL;
		memcpy(&cpeh, cpeblk, sizeof(cpeh));
		cpeh.cpeh_size        = le16_to_cpu(cpeh.cpeh_size);
		if (cpeh.cpeh_size != sizeof(cpeh))
			return -EINVAL;
		cpeh.next_cpeh_offset = seg_mode ?
		                        xlate_segoff(le32_to_cpu(cpeh.next_cpeh_offset)) :
		                        le32_to_cpu(cpeh.next_cpeh_offset);
		cpeh.device_type      = le16_to_cpu(cpeh.device_type);
		cpeh.codepage         = le16_to_cpu(cpeh.codepage);
		cpeh.cpih_offset      = seg_mode ?
		                        xlate_segoff(le32_to_cpu(cpeh.cpih_offset)) :
		                        le32_to_cpu(cpeh.cpih_offset);
		cpeblk = vdata + cpeh.next_cpeh_offset;

		printf("CPEH #%u: Name: %.*s, Codepage: %u, Device: %.*s, DType: %u\n",
		       i, static_cast<int>(sizeof(cpeh.device_name)),
		       cpeh.device_name, cpeh.codepage,
		       static_cast<int>(std::size(cpeh.device_name)),
		       cpeh.device_name, cpeh.device_type);

		if (cpeh.next_cpeh_offset + sizeof(cpeh) >= vsize)
			return -EINVAL;
		if (cpeh.cpih_offset + sizeof(cpih) >= vsize)
			return -EINVAL;
		memcpy(&cpih, vdata + cpeh.cpih_offset, sizeof(cpih));
		cpih.version   = le16_to_cpu(cpih.version);
		cpih.num_fonts = le16_to_cpu(cpih.num_fonts);
		cpih.size      = le16_to_cpu(cpih.size);
		printf("CPIH: version=%u fonts=%u size=%u\n", cpih.version,
		       cpih.num_fonts, cpih.size);
		if (cpih.version != 1)
			continue;

		char dev[HXSIZEOF_Z32*2], cpg[HXSIZEOF_Z32*2];
		*dev = '\0';
		HX_strlncat(dev, cpeh.device_name, std::size(dev), std::size(cpeh.device_name));
		HX_strrtrim(dev);
		snprintf(cpg, std::size(cpg), "%u", cpeh.codepage);
		if (cpeh.device_type == DEVTYPE_SCREEN)
			vf_extract_sfh(vdata + cpeh.cpih_offset + sizeof(cpih),
				cpih.num_fonts, directory, dev, cpg);
		else if (cpeh.device_type == DEVTYPE_PRINTER)
			vf_extract_pfh(vdata + cpeh.cpih_offset + sizeof(cpih));
	}
	return true;
}

static bool vf_xcpi(font &f, char **args, bool seg_mode)
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
	auto ret = vf_extract_cpi2(static_cast<const char *>(mapping),
	           sb.st_size, args[1], seg_mode);
	if (ret == -EINVAL) {
		fprintf(stderr, "xcpi: file \"%s\" not recognized\n", args[0]);
		return false;
	}
	return ret >= 0;
}

static bool vf_xcpi_flat(font &f, char **args)
{
	return vf_xcpi(f, args, false);
}

static bool vf_xcpi_seg(font &f, char **args)
{
	return vf_xcpi(f, args, true);
}

static bool vf_xlat(font &f, char **args)
{
	auto x = strtol(args[0], nullptr, 0);
	auto y = strtol(args[1], nullptr, 0);
	if (f.m_glyph.size() > 0)
		f.copy_to_blank(vfpos() | f.m_glyph[0].m_size, vfpos(x, y) | f.m_glyph[0].m_size);
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
	{"copy", 6, vf_copy},
	{"cpisep", 1, vf_cpisep},
	{"crop", 4, vf_crop},
	{"fliph", 0, vf_fliph},
	{"flipv", 0, vf_flipv},
	{"invert", 0, vf_invert},
	{"lge", 0, vf_lge},
	{"lgeu", 0, vf_lgeu},
	{"lgeuf", 0, vf_lgeuf},
	{"loadbdf", 1, vf_loadbdf},
	{"loadclt", 1, vf_loadclt},
	{"loadfnt", 1, vf_loadfnt},
	{"loadhex", 1, vf_loadhex},
	{"loadmap", 1, vf_loadmap},
	{"loadpcf", 1, vf_loadpcf},
	{"loadpsf", 1, vf_loadpsf},
	{"loadraw", 3, vf_loadraw},
	{"move", 2, vf_move},
	{"overstrike", 1, vf_overstrike},
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
	{"xcpi", 2, vf_xcpi_flat},
	{"xcpi.ice", 2, vf_xcpi_seg},
	{"xlat", 2, vf_xlat},
};

int main(int argc, char **argv)
{
	font f;
	if (argc > 0) {
		--argc;
		++argv;
	}
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
