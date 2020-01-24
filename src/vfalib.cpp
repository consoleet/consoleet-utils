/*
 *	I/O and glyph manipulation routines of the "VGA font assembler"
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
#include <iomanip>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <fcntl.h>
#include <getopt.h>
#include <iconv.h>
#include <unistd.h>
#include <libHX/ctype_helper.h>
#include <libHX/defs.h>
#include <libHX/io.h>
#include <libHX/string.h>
#ifdef HAVE_XBRZ_H
#	include <xbrz.h>
#endif
#include "vfalib.hpp"

namespace vfalib {

enum {
	PSF2_MAGIC0 = 0x72,
	PSF2_MAGIC1 = 0xB5,
	PSF2_MAGIC2 = 0x4A,
	PSF2_MAGIC3 = 0x86,
	PSF2_HAS_UNICODE_TABLE = 0x01,
	PSF2_SEPARATOR = 0xFF,
	PSF2_STARTSEQ = 0xFE,
};

struct bitpos {
	size_t byte;
	unsigned char bybit, mask;
	bitpos(size_t n) : byte(n / CHAR_BIT), bybit(CHAR_BIT - 1 - n % CHAR_BIT), mask(1 << bybit) {}
};

struct deleter {
	void operator()(FILE *f) { fclose(f); }
	void operator()(HXdir *d) { HXdir_close(d); }
};

struct psf2_header {
	uint8_t magic[4];
	uint32_t version, headersize, flags, length, charsize, height, width;
};

static const char vfhex[] = "0123456789abcdef";

static FILE *fopen(const char *name, const char *mode)
{
	if (strcmp(name, "-") != 0)
		return ::fopen(name, mode);
	if (strchr(mode, '+') != nullptr)
		return nullptr;
	if (strpbrk(mode, "wa") != nullptr)
		return stdout;
	if (strchr(mode, 'r') != nullptr)
		return stdin;
	return nullptr;
}

static unsigned int bytes_per_glyph(const vfsize &size)
{
	/* A 9x16 glyph occupy 18 chars in our internal representation */
	return (size.w * size.h + CHAR_BIT - 1) / CHAR_BIT;
}

static unsigned int bytes_per_glyph_rpad(const vfsize &size)
{
	/* A 9x16 glyph occupies 32 chars in PSF2 */
	return size.h * ((size.w + 7) / 8);
}

void unicode_map::add_i2u(unsigned int idx, char32_t uc)
{
	auto &set = m_i2u.emplace(idx, decltype(m_i2u)::mapped_type{}).first->second;
	set.emplace(uc);
	auto r = m_u2i.emplace(uc, idx);
	if (!r.second)
		r.first->second = idx;
}

int unicode_map::load(const char *file)
{
	std::unique_ptr<FILE, deleter> fp(fopen(file, "rb"));
	if (fp == nullptr) {
		fprintf(stderr, "Could not open %s: %s", file, strerror(errno));
		return -errno;
	}
	size_t lnum = 0;
	hxmc_t *line = nullptr;
	auto lineclean = make_scope_success([&]() { HXmc_free(line); });
	while (HX_getl(&line, fp.get()) != nullptr) {
		char *ptr = line, *end = nullptr;
		while (HX_isspace(*ptr))
			++ptr;
		if (*ptr == '#')
			continue;
		auto key = strtoul(line, &end, 0);
		++lnum;
		do {
			ptr = end;
			while (HX_isspace(*ptr) || *ptr == '\r')
				++ptr;
			if (*ptr == '\0' || *ptr == '\n' || *ptr == '#')
				break;
			if (ptr[0] != 'U') {
				fprintf(stderr, "Warning: Unexpected char '%c' in unicode map line %zu.\n", ptr[0], lnum);
				break;
			} else if (ptr[1] != '+') {
				fprintf(stderr, "Warning: Unexpected char '%c' in unicode map line %zu.\n", ptr[1], lnum);
				break;
			}
			ptr += 2;
			auto val = strtoul(ptr, &end, 16);
			if (end == ptr)
				break;
			add_i2u(key, val);
		} while (true);
	}
	return true;
}

std::set<char32_t> unicode_map::to_unicode(unsigned int idx) const
{
	auto j = m_i2u.find(idx);
	if (j == m_i2u.cend())
		return {idx};
	return j->second;
}

ssize_t unicode_map::to_index(char32_t uc) const
{
	auto j = m_u2i.find(uc);
	if (j == m_u2i.cend())
		return -1;
	return j->second;
}

void font::init_256_blanks()
{
	m_glyph = std::vector<glyph>(256, glyph(vfsize(8, 16)));
}

void font::lge()
{
	for (unsigned int k = 0xC0; k <= 0xDF; ++k) {
		if (m_glyph.size() < k)
			break;
		m_glyph[k].lge();
	}
}

void font::xbrz(unsigned int factor, unsigned int mode)
{
	for (auto &g : m_glyph)
		g = g.xbrz(factor, mode);
}

int font::load_clt(const char *dirname)
{
	std::unique_ptr<HXdir, deleter> dh(HXdir_open(dirname));
	if (dh == nullptr)
		return -errno;
	if (m_unicode_map == nullptr)
		m_unicode_map = std::make_shared<unicode_map>();

	const char *de;
	glyph ng;
	while ((de = HXdir_read(dh.get())) != nullptr) {
		if (*de == '.')
			continue;
		char *end;
		char32_t uc = strtoul(de, &end, 16);
		if (*end != '.' || end == de)
			continue;
		auto fn = dirname + std::string("/") + de;
		std::unique_ptr<FILE, deleter> fp(::fopen(fn.c_str(), "r"));
		if (fp == nullptr) {
			fprintf(stderr, "Error opening %s: %s\n", fn.c_str(), strerror(errno));
			return -errno;
		}
		auto ret = load_clt_glyph(fp.get(), ng);
		if (ret == -EINVAL) {
			fprintf(stderr, "%s not recognized as a CLT file\n", fn.c_str());
			continue;
		}
		if (ret < 0)
			return ret;
		m_unicode_map->add_i2u(m_glyph.size(), uc);
		m_glyph.emplace_back(std::move(ng));
	}
	return 0;
}

int font::load_clt_glyph(FILE *fp, glyph &ng)
{
	hxmc_t *line = nullptr;
	auto lineclean = make_scope_success([&]() { HXmc_free(line); });

	if (HX_getl(&line, fp) == nullptr)
		return -EINVAL;
	HX_chomp(line);
	if (strcmp(line, "PCLT") != 0)
		return -EINVAL;
	if (HX_getl(&line, fp) == nullptr)
		return -EINVAL;
	unsigned int width = 0, height = 0, y = 0;
	if (sscanf(line, "%u %u", &width, &height) != 2)
		return -EINVAL;

	for (ng = glyph(vfsize(width, height)); HX_getl(&line, fp) != nullptr; ++y) {
		unsigned int x = 0;
		for (auto p = line; *p != '\0'; ++x) {
			bitpos opos = y * width + x;
			if (*p == '#')
				ng.m_data[opos.byte] |= opos.mask;
			++p;
			if (*p != '\0')
				++p;
		}
	}
	return 0;
}

int font::load_fnt(const char *file, unsigned int height)
{
	std::unique_ptr<FILE, deleter> fp(fopen(file, "rb"));
	if (fp == nullptr)
		return -errno;
	unsigned int width = 8;
	if (height == static_cast<unsigned int>(-1)) {
		height = 16;
		struct stat sb;
		if (fstat(fileno(fp.get()), &sb) == 0) {
			if (sb.st_size > 0 && sb.st_size < 8192)
				height = sb.st_size / 256;
			else if (sb.st_size == 8192)
				/* could be either 8x16x512 or 8x32x256, but this is a common heuristic, so use 8x16x512 */
				height = 16;
		}
	}
	auto bpc = bytes_per_glyph(vfsize(width, height));
	std::unique_ptr<char[]> buf(new char[bpc]);
	do {
		auto ret = fread(buf.get(), bpc, 1, fp.get());
		if (ret < 1)
			break;
		m_glyph.emplace_back(glyph::create_from_rpad(vfsize(width, height), buf.get(), bpc));
	} while (true);
	return 0;
}

int font::load_hex(const char *file)
{
	std::unique_ptr<FILE, deleter> fp(fopen(file, "r"));
	if (fp == nullptr)
		return -errno;
	if (m_unicode_map == nullptr)
		m_unicode_map = std::make_shared<unicode_map>();

	hxmc_t *line = nullptr;
	size_t lnum = 0;
	while (HX_getl(&line, fp.get()) != nullptr) {
		++lnum;
		char *end;
		auto cp = strtoul(line, &end, 16);
		if (*end != ':')
			continue;
		++end;

		unsigned int z;
		char gbits[32]{};
		HX_chomp(line);
		for (z = 0; z < sizeof(gbits) && end[0] != '\0' && end[1] != '\0';
		     ++z, end += 2) {
			gbits[z] = 0;
			auto c = HX_tolower(end[0]);
			if (c >= '0' && c <= '9')
				gbits[z] = c - '0';
			else if (HX_tolower(end[0]) >= 'a' && HX_tolower(end[0]) <= 'f')
				gbits[z] = c - 'a' + 10;
			gbits[z] <<= 4;
			c = HX_tolower(end[1]);
			if (c >= '0' && c <= '9')
				gbits[z] |= c - '0';
			else if (HX_tolower(end[1]) >= 'a' && HX_tolower(end[1]) <= 'f')
				gbits[z] |= c - 'a' + 10;
		}

		if (z == 16)
			m_glyph.emplace_back(glyph::create_from_rpad(vfsize(8, 16), gbits, z));
		else if (z == 32)
			m_glyph.emplace_back(glyph::create_from_rpad(vfsize(16, 16), gbits, z));
		else
			fprintf(stderr, "load_hex: unrecognized glyph size (%u bytes) in line %zu\n", z, lnum);
		m_unicode_map->add_i2u(m_glyph.size() - 1, cp);
		m_unicode_map->add_u2i(cp, m_glyph.size() - 1);
	}
	HXmc_free(line);
	return 0;
}

static char32_t nextutf8(FILE *fp)
{
	unsigned int nbyte = 0;
	auto ret = fgetc(fp);
	if (ret == EOF || ret == 0xFF)
		return ~0U;
	if (ret >= 0x00 && ret < 0xC0)
		return ret;

	if (ret >= 0xC0 && ret < 0xE0) nbyte = 2;
	else if (ret >= 0xE0 && ret < 0xF0) nbyte = 3;
	else if (ret >= 0xF0 && ret < 0xF8) nbyte = 4;
	else if (ret >= 0xF8 && ret < 0xFC) nbyte = 5;
	else if (ret >= 0xFC && ret < 0xFE) nbyte = 6;

	char32_t uc = ret & ~(~0U << (7 - nbyte));
	for (unsigned int z = 1; z < nbyte; ++z) {
		ret = fgetc(fp);
		if (ret == EOF || ret == 0xFF || ((ret & 0xC0) != 0x80))
			return ~0U;
		uc <<= 6;
		uc |= static_cast<unsigned char>(ret & 0x3F);
	}
	return uc;
}

int font::load_psf(const char *file)
{
	std::unique_ptr<FILE, deleter> fp(fopen(file, "rb"));
	if (fp == nullptr)
		return -errno;
	struct psf2_header hdr;
	if (fread(&hdr, sizeof(hdr), 1, fp.get()) != 1)
		return -errno;
	hdr.version    = le32_to_cpu(hdr.version);
	hdr.headersize = le32_to_cpu(hdr.headersize);
	hdr.flags      = le32_to_cpu(hdr.flags);
	hdr.length     = le32_to_cpu(hdr.length);
	hdr.charsize   = le32_to_cpu(hdr.charsize);
	hdr.height     = le32_to_cpu(hdr.height);
	hdr.width      = le32_to_cpu(hdr.width);
	if (hdr.magic[0] != PSF2_MAGIC0 || hdr.magic[1] != PSF2_MAGIC1 ||
	    hdr.magic[2] != PSF2_MAGIC2 || hdr.magic[3] != PSF2_MAGIC3 ||
	    hdr.version != 0)
		return -EINVAL;

	std::unique_ptr<char[]> buf(new char[hdr.charsize]);
	size_t glyph_start = m_glyph.size();
	for (size_t idx = 0; idx < hdr.length; ++idx) {
		if (fread(buf.get(), hdr.charsize, 1, fp.get()) != 1)
			break;
		m_glyph.push_back(glyph::create_from_rpad(vfsize(hdr.width, hdr.height), buf.get(), hdr.charsize));
	}

	if (!(hdr.flags & PSF2_HAS_UNICODE_TABLE))
		return 0;
	m_unicode_map = std::make_shared<unicode_map>();
	auto cd = iconv_open("UTF-32", "UTF-8");
	if (cd == nullptr) {
		fprintf(stderr, "iconv_open: %s\n", strerror(errno));
		return -errno;
	}
	auto cdclean = make_scope_success([&]() { iconv_close(cd); });
	for (unsigned int idx = 0; idx < hdr.length; ++idx) {
		do {
			auto uc = nextutf8(fp.get());
			if (uc == ~0U)
				break;
			m_unicode_map->add_i2u(glyph_start + idx, uc);
		} while (true);
	}
	return 0;
}

int font::save_bdf(const char *file, const char *aname)
{
	std::unique_ptr<FILE, deleter> filep(fopen(file, "w"));
	if (filep == nullptr)
		return -errno;
	auto fp = filep.get();
	vfsize sz0;
	if (m_glyph.size() > 0)
		sz0 = m_glyph[0].m_size;
	std::string name;
	if (aname != nullptr) {
		name = aname;
		/* X logical font description (XLFD) does not permit dashes */
		std::replace(name.begin(), name.end(), '-', ' ');
	} else {
		name = "vfontas output";
	}
	fprintf(fp, "STARTFONT 2.1\n");
	fprintf(fp, "FONT -misc-%s-medium-r-normal--%u-%u-75-75-c-%u-iso10646-1\n",
		name.c_str(), sz0.h, 10 * sz0.h, 10 * sz0.w);
	fprintf(fp, "SIZE %u 75 75\n", sz0.h);
	fprintf(fp, "FONTBOUNDINGBOX %u %u 0 -%u\n", sz0.w, sz0.h, sz0.h / 4);
	fprintf(fp, "STARTPROPERTIES 24\n");
	fprintf(fp, "FONT_TYPE \"Bitmap\"\n");
	fprintf(fp, "FONTNAME_REGISTRY \"\"\n");
	fprintf(fp, "FOUNDRY \"misc\"\n");
	fprintf(fp, "FAMILY_NAME \"%s\"\n", name.c_str());
	fprintf(fp, "WEIGHT_NAME \"medium\"\n");
	fprintf(fp, "SLANT \"r\"\n");
	fprintf(fp, "SETWIDTH_NAME \"normal\"\n");
	fprintf(fp, "PIXEL_SIZE %u\n", sz0.h);
	fprintf(fp, "POINT_SIZE %u\n", 10 * sz0.h);
	fprintf(fp, "SPACING \"C\"\n");
	fprintf(fp, "AVERAGE_WIDTH %u\n", 10 * sz0.w);
	fprintf(fp, "FONT \"%s\"\n", name.c_str());
	fprintf(fp, "WEIGHT 10\n");
	fprintf(fp, "RESOLUTION 75\n");
	fprintf(fp, "RESOLUTION_X 75\n");
	fprintf(fp, "RESOLUTION_Y 75\n");
	fprintf(fp, "CHARSET_REGISTRY \"ISO10646\"\n");
	fprintf(fp, "CHARSET_ENCODING \"1\"\n");
	fprintf(fp, "QUAD_WIDTH %u\n", sz0.w);
	if (m_unicode_map != nullptr && m_unicode_map->m_u2i.find(65533) != m_unicode_map->m_u2i.cend())
		fprintf(fp, "DEFAULT_CHAR 65533\n");
	else
		fprintf(fp, "DEFAULT_CHAR 0\n");
	fprintf(fp, "FONT_ASCENT %u\n", sz0.h * 12 / 16);
	fprintf(fp, "FONT_DESCENT %u\n", sz0.h * 4 / 16);
	fprintf(fp, "CAP_HEIGHT %u\n", sz0.h);
	fprintf(fp, "X_HEIGHT %u\n", sz0.h * 7 / 16);
	fprintf(fp, "ENDPROPERTIES\n");

	if (m_unicode_map == nullptr) {
		fprintf(fp, "CHARS %zu\n", m_glyph.size());
		for (size_t idx = 0; idx < m_glyph.size(); ++idx)
			save_bdf_glyph(fp, idx, idx);
	} else {
		fprintf(fp, "CHARS %zu\n", m_unicode_map->m_u2i.size());
		for (const auto &pair : m_unicode_map->m_u2i)
			save_bdf_glyph(fp, pair.second, pair.first);
	}
	fprintf(fp, "ENDFONT\n");
	return 0;
}

void font::save_bdf_glyph(FILE *fp, size_t idx, char32_t cp)
{
	auto sz = m_glyph[idx].m_size;
	fprintf(fp, "STARTCHAR U+%04x\n" "ENCODING %u\n",
		static_cast<unsigned int>(cp), static_cast<unsigned int>(cp));
	fprintf(fp, "SWIDTH 1000 0\n");
	fprintf(fp, "DWIDTH %u 0\n", sz.w);
	fprintf(fp, "BBX %u %u 0 -%u\n", sz.w, sz.h, sz.h / 4);
	fprintf(fp, "BITMAP\n");

	auto byteperline = (sz.w + 7) / 8;
	unsigned int ctr = 0;
	for (auto c : m_glyph[idx].as_rowpad()) {
		fputc(vfhex[(c&0xF0)>>4], fp);
		fputc(vfhex[c&0x0F], fp);
		if (++ctr % byteperline == 0)
			fprintf(fp, "\n");
	}
	fprintf(fp, "ENDCHAR\n");
}

int font::save_clt(const char *dir)
{
	if (m_unicode_map == nullptr) {
		for (size_t idx = 0; idx < m_glyph.size(); ++idx) {
			auto ret = save_clt_glyph(dir, idx, idx);
			if (ret < 0)
				return ret;
		}
		return 0;
	}
	for (size_t idx = 0; idx < m_glyph.size(); ++idx)
		for (auto codepoint : m_unicode_map->to_unicode(idx)) {
			auto ret = save_clt_glyph(dir, idx, codepoint);
			if (ret < 0)
				return ret;
		}
	return 0;
}

int font::save_clt_glyph(const char *dir, size_t idx, char32_t codepoint)
{
	std::stringstream ss;
	ss << dir << "/" << std::setfill('0') << std::setw(4) << std::hex << codepoint << ".txt";
	auto outpath = ss.str();
	std::unique_ptr<FILE, deleter> fp(fopen(outpath.c_str(), "w"));
	if (fp == nullptr) {
		fprintf(stderr, "Could not open %s for writing: %s\n", outpath.c_str(), strerror(errno));
		return -errno;
	}
	auto data = m_glyph[idx].as_pclt();
	auto ret = fwrite(data.c_str(), data.size(), 1, fp.get());
	if (ret < 0 || (data.size() > 0 && ret != 1)) {
		fprintf(stderr, "fwrite %s: %s\n", outpath.c_str(), strerror(-errno));
		return -errno;
	}
	return 0;
}

int font::save_fnt(const char *file)
{
	std::unique_ptr<FILE, deleter> fp(fopen(file, "wb"));
	if (fp == nullptr)
		return -errno;
	for (const auto &glyph : m_glyph) {
		auto ret = fwrite(glyph.m_data.c_str(), glyph.m_data.size(), 1, fp.get());
		if (ret < 1)
			break;
	}
	return 0;
}

int font::save_map(const char *file)
{
	std::unique_ptr<FILE, deleter> fp(fopen(file, "w"));
	if (fp == nullptr)
		return -errno;
	if (m_unicode_map == nullptr)
		return 0;
	for (const auto &e : m_unicode_map->m_i2u) {
		fprintf(fp.get(), "0x%02x\t", e.first);
		for (auto uc : e.second)
			fprintf(fp.get(), "U+%04x ", uc);
		fprintf(fp.get(), "\n");
	}
	return 0;
}

int font::save_pbm(const char *dir)
{
	if (m_unicode_map == nullptr) {
		for (size_t idx = 0; idx < m_glyph.size(); ++idx) {
			auto ret = save_pbm_glyph(dir, idx, idx);
			if (ret < 0)
				return ret;
		}
		return 0;
	}
	for (size_t idx = 0; idx < m_glyph.size(); ++idx)
		for (auto codepoint : m_unicode_map->to_unicode(idx)) {
			auto ret = save_pbm_glyph(dir, idx, codepoint);
			if (ret < 0)
				return ret;
		}
	return 0;
}

int font::save_pbm_glyph(const char *dir, size_t idx, char32_t codepoint)
{
	std::stringstream ss;
	ss << dir << "/" << std::setfill('0') << std::setw(4) << std::hex << codepoint << ".pbm";
	auto outpath = ss.str();
	std::unique_ptr<FILE, deleter> fp(::fopen(outpath.c_str(), "w"));
	if (fp == nullptr) {
		fprintf(stderr, "Could not open %s for writing: %s\n", outpath.c_str(), strerror(errno));
		return -errno;
	}
	auto data = m_glyph[idx].as_pbm();
	auto ret = fwrite(data.c_str(), data.size(), 1, fp.get());
	if (ret < 0 || (data.size() > 0 && ret != 1)) {
		fprintf(stderr, "fwrite %s: %s\n", outpath.c_str(), strerror(-errno));
		return -errno;
	}
	return 0;
}

int font::save_psf(const char *file)
{
	std::unique_ptr<FILE, deleter> fp(fopen(file, "wb"));
	if (fp == nullptr)
		return -errno;
	struct psf2_header hdr = {{PSF2_MAGIC0, PSF2_MAGIC1, PSF2_MAGIC2, PSF2_MAGIC3}, 0, sizeof(hdr)};
	hdr.version    = cpu_to_le32(hdr.version);
	hdr.headersize = cpu_to_le32(hdr.headersize);
	hdr.flags      = m_unicode_map != nullptr ? PSF2_HAS_UNICODE_TABLE : 0;
	hdr.flags      = cpu_to_le32(hdr.flags);
	hdr.length     = cpu_to_le32(m_glyph.size());
	if (m_glyph.size() > 0) {
		hdr.charsize = cpu_to_le32(bytes_per_glyph_rpad(m_glyph[0].m_size));
		hdr.height   = cpu_to_le32(m_glyph[0].m_size.h);
		hdr.width    = cpu_to_le32(m_glyph[0].m_size.w);
	}
	fwrite(&hdr, sizeof(hdr), 1, fp.get());
	for (size_t idx = 0; idx < m_glyph.size(); ++idx) {
		const auto &pat = m_glyph[idx].as_rowpad();
		fwrite(pat.c_str(), pat.size(), 1, fp.get());
	}
	if (m_unicode_map == nullptr)
		return 0;
	auto cd = iconv_open("UTF-8", "UTF-32");
	if (cd == nullptr) {
		fprintf(stderr, "iconv_open: %s\n", strerror(errno));
		return -errno;
	}
	auto cdclean = make_scope_success([&]() { iconv_close(cd); });
	for (const auto &epair : m_unicode_map->m_i2u) {
		for (auto cp : epair.second) {
			char ob[8];
			char *inbuf = reinterpret_cast<char *>(&cp), *outbuf = ob;
			size_t iblen = sizeof(cp), oblen = ARRAY_SIZE(ob);
			iconv(cd, &inbuf, &iblen, &outbuf, &oblen);
			fwrite(ob, ARRAY_SIZE(ob) - oblen, 1, fp.get());
		}
		fwrite("\xff", 1, 1, fp.get());
	}
	return 0;
}

int font::save_sfd(const char *file, const char *aname)
{
	std::unique_ptr<FILE, deleter> filep(fopen(file, "w"));
	if (filep == nullptr)
		return -errno;
	auto fp = filep.get();
	vfsize sz0;
	if (m_glyph.size() > 0)
		sz0 = m_glyph[0].m_size;
	std::string name;
	if (aname != nullptr) {
		name = aname;
		/* X logical font description (XLFD) does not permit dashes */
		std::replace(name.begin(), name.end(), '-', ' ');
	} else {
		name = "vfontas output";
	}
	int ascent = sz0.h, descent = 0;
	fprintf(fp, "SplineFontDB: 3.0\n");
	fprintf(fp, "FontName: %s\n", name.c_str());
	fprintf(fp, "FullName: %s\n", name.c_str());
	fprintf(fp, "FamilyName: %s\n", name.c_str());
	fprintf(fp, "Weight: medium\n");
	fprintf(fp, "Version: 001.000\n");
	fprintf(fp, "ItalicAngle: 0\n");
	fprintf(fp, "UnderlinePosition: -100\n");
	fprintf(fp, "UnderlineWidth: 40\n");
	fprintf(fp, "Ascent: %u\n", ascent);
	fprintf(fp, "Descent: %u\n", descent);
	fprintf(fp, "NeedsXUIDChange: 1\n");
	fprintf(fp, "FSType: 0\n");
	fprintf(fp, "PfmFamily: 32\n");
	fprintf(fp, "TTFWeight: 500\n");
	fprintf(fp, "TTFWidth: 5\n");
	fprintf(fp, "Panose: 2 0 6 4 0 0 0 0 0 0\n");
	fprintf(fp, "LineGap: 72\n");
	fprintf(fp, "VLineGap: 0\n");
	fprintf(fp, "OS2WinAscent: %u\n", ascent);
	fprintf(fp, "OS2WinAOffset: 1\n");
	fprintf(fp, "OS2WinDescent: %u\n", descent);
	fprintf(fp, "OS2WinDOffset: 1\n");
	fprintf(fp, "HheadAscent: %u\n", ascent);
	fprintf(fp, "HheadAOffset: 1\n");
	fprintf(fp, "HheadDescent: %u\n", descent);
	fprintf(fp, "HheadDOffset: 1\n");
	fprintf(fp, "Encoding: UnicodeBmp\n");
	fprintf(fp, "UnicodeInterp: none\n");
	fprintf(fp, "DisplaySize: -24\n");
	fprintf(fp, "AntiAlias: 1\n");
	fprintf(fp, "FitToEm: 1\n");
	fprintf(fp, "WinInfo: 0 50 22\n");
	fprintf(fp, "TeXData: 1 0 0 346030 173015 115343 0 1048576 115343 783286 444596 497025 792723 393216 433062 380633 303038 157286 324010 404750 52429 2506097 1059062 262144\n");
	fprintf(fp, "BeginChars: 65536 %zu\n\n", m_glyph.size());

	if (m_unicode_map == nullptr) {
		for (size_t idx = 0; idx < m_glyph.size(); ++idx)
			save_sfd_glyph(fp, idx, idx, ascent, descent);
	} else {
		for (const auto &pair : m_unicode_map->m_u2i)
			save_sfd_glyph(fp, pair.second, pair.first, ascent, descent);
	}
	fprintf(fp, "EndChars\n");
	fprintf(fp, "EndSplineFont\n");
	return 0;
}

class vectorizer final {
	public:
	using vertex = std::pair<unsigned int, unsigned int>;
	using edge = std::pair<vertex, vertex>;
	void set(unsigned int, unsigned int);
	void finalize();
	std::vector<edge> pop_poly();

	private:
	void add_edge(edge &&e) { emap.insert(std::move(e)); }
	std::set<edge> emap;
};

void vectorizer::set(unsigned int x, unsigned int y)
{
	/* TTF/OTF spec wants CCW orientation */
	add_edge({{x, y}, {x, y + 1}});
	add_edge({{x, y + 1}, {x + 1, y + 1}});
	add_edge({{x + 1, y + 1}, {x + 1, y}});
	add_edge({{x + 1, y}, {x, y}});
}

void vectorizer::finalize()
{
	/*
	 * Remove overlaps: As enforced by set(), all the polygons are added
	 * with the same orientation. Polygons at most touch, and never
	 * overlap. Joining polygons therefore simply requires removing shared
	 * contradirectional edges. The remaining set of edges then forms a new
	 * set of polygons, and, as the edges themselves were never reoriented,
	 * these polygons have the correct orientation.
	 */
	for (auto i = emap.begin(); i != emap.end(); ) {
		auto j = emap.find({i->second, i->first});
		if (j == emap.cend()) {
			++i;
			continue;
		}
		emap.erase(j);
		i = emap.erase(i);
	}
}

static inline unsigned int dir(const vectorizer::edge &e)
{
	/* We have no diagonal lines, so this is fine */
	const auto &v1 = e.first, &v2 = e.second;
	if (v2.first == v1.first)
		return v2.second < v1.second ? 180 : 0;
	return v2.first < v1.first ? 270 : 90;
}

std::vector<vectorizer::edge> vectorizer::pop_poly()
{
	std::vector<edge> poly;
	if (emap.size() == 0)
		return poly;
	poly.push_back(*emap.begin());
	emap.erase(emap.begin());
	auto prev_dir = dir(poly[0]);

	while (true) {
		if (emap.size() == 0)
			break;
		auto &tail_vtx = poly.rbegin()->second;
		if (tail_vtx == poly.cbegin()->first)
			break;
		auto next = emap.lower_bound({tail_vtx, {}});
		if (next == emap.cend()) {
			fprintf(stderr, "unclosed poly wtf?!\n");
			break;
		}

		/* Squash redundant vertices along the way */
		auto next_dir = dir(*next);
		if (next_dir == prev_dir)
			tail_vtx = next->second;
		else
			poly.push_back(*next);
		emap.erase(next);
		prev_dir = next_dir;
	}
	return poly;
}

void font::save_sfd_glyph(FILE *fp, size_t idx, char32_t cp, int asc, int desc)
{
	unsigned int cpx = cp;
	const auto &g = m_glyph[idx];
	const auto &sz = g.m_size;
	fprintf(fp, "StartChar: %04x\n", cpx);
	fprintf(fp, "Encoding: %u %u %u\n", cpx, cpx, cpx);
	fprintf(fp, "Width: %u\n", sz.w);
	fprintf(fp, "TeX: 0 0 0 0\n");
	fprintf(fp, "Flags: MW\n");
	fprintf(fp, "Fore\n");
	vectorizer vk;

	for (unsigned int y = 0; y < sz.h; ++y) {
		unsigned int yy = sz.h - 1 - y - desc;
		for (unsigned int x = 0; x < sz.w; ++x) {
			bitpos ipos = y * sz.w + x;
			if (g.m_data[ipos.byte] & ipos.mask)
				vk.set(x, yy);
		}
	}
	vk.finalize();
	while (true) {
		auto poly = vk.pop_poly();
		if (poly.size() == 0)
			break;
		const auto &v1 = poly.cbegin()->first;
		fprintf(fp, "%d %d m 25\n", v1.first, v1.second);
		for (const auto &edge : poly)
			fprintf(fp, " %d %d l 25\n", edge.second.first, edge.second.second);
	}
	fprintf(fp, "EndSplineSet\n");
	fprintf(fp, "EndChar\n");
}

glyph::glyph(const vfsize &size) :
	m_size(size)
{
	m_data.resize(bytes_per_glyph(m_size));
}

glyph glyph::create_from_rpad(const vfsize &size, const char *buf, size_t z)
{
	glyph ng(size);
	auto byteperline = (size.w + 7) / 8;
	for (unsigned int y = 0; y < size.h; ++y) {
		for (unsigned int x = 0; x < size.w; ++x) {
			bitpos qpos = x;
			bitpos opos = y * size.w + x;
			if (buf[y*byteperline+qpos.byte] & qpos.mask)
				ng.m_data[opos.byte] |= opos.mask;
		}
	}
	return ng;
}

glyph glyph::blit(const vfrect &sof, const vfrect &pof) const
{
	glyph ng(pof);

	for (unsigned int y = sof.y; y < sof.y + sof.h && y < m_size.h; ++y) {
		for (unsigned int x = sof.x; x < sof.x + sof.w && x < m_size.w; ++x) {
			int ox = pof.x + x - sof.x;
			int oy = pof.y + y - sof.y;
			if (ox < 0 || oy < 0 || static_cast<unsigned int>(ox) >= pof.w ||
			    static_cast<unsigned int>(oy) >= pof.h)
				continue;
			bitpos ipos = y * m_size.w + x;
			bitpos opos = oy * ng.m_size.w + ox;
			if (m_data[ipos.byte] & ipos.mask)
				ng.m_data[opos.byte] |= opos.mask;
		}
	}
	return ng;
}

glyph glyph::flip(bool flipx, bool flipy) const
{
	glyph ng(m_size);
	for (unsigned int y = 0; y < m_size.h; ++y) {
		for (unsigned int x = 0; x < m_size.w; ++x) {
			bitpos ipos = y * m_size.w + x;
			bitpos opos = (flipy ? m_size.h - y - 1 : y) * m_size.w + (flipx ? m_size.w - x - 1 : x);
			if (m_data[ipos.byte] & ipos.mask)
				ng.m_data[opos.byte] |= opos.mask;
		}
	}
	return ng;
}

glyph glyph::upscale(const vfsize &factor) const
{
	glyph ng(vfsize(m_size.w * factor.w, m_size.h * factor.h));
	for (unsigned int y = 0; y < ng.m_size.h; ++y) {
		for (unsigned int x = 0; x < ng.m_size.w; ++x) {
			bitpos opos = y * ng.m_size.w + x;
			bitpos ipos = y / factor.h * m_size.w + x / factor.w;
			if (m_data[ipos.byte] & ipos.mask)
				ng.m_data[opos.byte] |= opos.mask;
		}
	}
	return ng;
}

void glyph::invert()
{
	std::transform(m_data.begin(), m_data.end(), m_data.begin(), [](char c) { return ~c; });
}

void glyph::lge()
{
	if (m_size.w < 2)
		return;
	for (unsigned int y = 0; y < m_size.h; ++y) {
		bitpos ipos = (y + 1) * m_size.w - 2;
		bitpos opos = (y + 1) * m_size.w - 1;
		if (m_data[ipos.byte] & ipos.mask)
			m_data[opos.byte] |= opos.mask;
		else
			m_data[opos.byte] &= ~opos.mask;
	}
}

glyph glyph::xbrz(unsigned int factor, unsigned int mode) const
{
#ifndef HAVE_XBRZ_H
	return {};
#else
	auto ng = blit(vfpos() | m_size, vfpos(1, 1) | vfsize(m_size.w + 2, m_size.h + 2));
	auto src = ng.as_rgba();
	vfsize xs(ng.m_size.w * factor, ng.m_size.h * factor);
	std::vector<uint32_t> dst(xs.w * xs.h);
	if (mode == 0)
		xbrz::scale(factor, &src[0], &dst[0], ng.m_size.w, ng.m_size.h, xbrz::ColorFormat::RGB);
	else if (mode == 1)
		xbrz::nearestNeighborScale(&src[0], ng.m_size.w, ng.m_size.h, &dst[0], xs.w, xs.h);
	else
		return {};

	src.clear();
	ng = glyph(vfsize(m_size.w * factor, m_size.h * factor));
	for (unsigned int y = 0; y < ng.m_size.h; ++y)
		for (unsigned int x = 0; x < ng.m_size.w; ++x) {
			size_t ipos = (y + factor) * xs.w + x + factor;
			bitpos opos = y * ng.m_size.w + x;
			if (dst[ipos] & 0xFF000000)
				ng.m_data[opos.byte] |= opos.mask;
		}
	return ng;
#endif
}

std::string glyph::as_pbm() const
{
	auto bpg = bytes_per_glyph(m_size);
	if (m_data.size() < bpg)
		return {};

	std::stringstream ss;
	ss << "P1\n" << m_size.w << " " << m_size.h << "\n";
	for (unsigned int y = 0; y < m_size.h; ++y) {
		for (unsigned int x = 0; x < m_size.w; ++x) {
			bitpos pos = y * m_size.w + x;
			ss << ((m_data[pos.byte] & pos.mask) ? "1" : "0");
		}
		ss << "\n";
	}
	return ss.str();
}

std::string glyph::as_pclt() const
{
	auto bpc = bytes_per_glyph(m_size);
	if (m_data.size() < bpc)
		return {};

	std::stringstream ss;
	ss << "PCLT\n" << m_size.w << " " << m_size.h << "\n";
	for (unsigned int y = 0; y < m_size.h; ++y) {
		for (unsigned int x = 0; x < m_size.w; ++x) {
			bitpos pos = y * m_size.w + x;
			ss << ((m_data[pos.byte] & pos.mask) ? "##" : "..");
		}
		ss << "\n";
	}
	return ss.str();
}

std::vector<uint32_t> glyph::as_rgba() const
{
	std::vector<uint32_t> vec(m_size.w * m_size.h);
	for (unsigned int y = 0; y < m_size.h; ++y)
		for (unsigned int x = 0; x < m_size.w; ++x) {
			size_t rpos = y * m_size.w + x;
			bitpos ipos = rpos;
			vec[rpos] = (m_data[ipos.byte] & ipos.mask) ? 0xFFFFFFFF : 0;
		}
	return vec;
}

std::string glyph::as_rowpad() const
{
	std::string ret;
	auto byteperline = (m_size.w + 7) / 8;
	ret.resize(bytes_per_glyph_rpad(m_size));
	for (unsigned int y = 0; y < m_size.h; ++y) {
		for (unsigned int x = 0; x < m_size.w; ++x) {
			bitpos ipos = y * m_size.w + x;
			bitpos qpos = x;
			if (m_data[ipos.byte] & ipos.mask)
				ret[y*byteperline+qpos.byte] |= qpos.mask;
		}
	}
	return ret;
}

} /* namespace vfalib */
