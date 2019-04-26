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
#include <algorithm>
#include <iomanip>
#include <map>
#include <memory>
#include <numeric>
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
#include <libHX/string.h>
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
	return (size.x * size.y + CHAR_BIT - 1) / CHAR_BIT;
}

static unsigned int bytes_per_glyph_rpad(const vfsize &size)
{
	/* A 9x16 glyph occupies 32 chars in PSF2 */
	return size.y * ((size.x + 7) / 8);
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
	char buf[16]{};
	m_glyph.clear();
	m_glyph.resize(256, glyph::create_from_rpad(vfsize(8, 16), buf, sizeof(buf)));
}

void font::lge()
{
	for (unsigned int k = 0xC0; k <= 0xDF; ++k) {
		if (m_glyph.size() < k)
			break;
		m_glyph[k].lge();
	}
}

ssize_t font::load_fnt(const char *file, unsigned int height)
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
	size_t count = 0;
	do {
		auto ret = fread(buf.get(), bpc, 1, fp.get());
		if (ret < 1)
			break;
		m_glyph.emplace_back(glyph::create_from_rpad(vfsize(width, height), buf.get(), bpc));
		++count;
	} while (true);
	return count;
}

ssize_t font::load_hex(const char *file)
{
	std::unique_ptr<FILE, deleter> fp(fopen(file, "r"));
	if (fp == nullptr)
		return -errno;
	if (m_unicode_map == nullptr)
		m_unicode_map = std::make_shared<unicode_map>();

	char line[80], gbits[16];
	size_t lnum = 0;
	while (fgets(line, sizeof(line), fp.get()) != nullptr) {
		++lnum;
		char *end;
		auto cp = strtoul(line, &end, 0);
		if (*end != ':')
			continue;
		++end;

		unsigned int z;
		for (z = 0; z < sizeof(glyph) && end[0] != '\0' && end[1] != '\0'; ++z) {
			gbits[z] = 0;
			if (end[0] >= '0' && end[0] <= '9')
				gbits[z] = end[0] - '0';
			else if (HX_tolower(end[0]) >= 'A' && HX_tolower(end[0]) <= 'F')
				gbits[z] = end[0] - HX_tolower(end[0]);
			gbits[z] <<= 4;
			if (end[1] >= '0' && end[1] <= '9')
				gbits[z] |= end[1] - '0';
			else if (HX_tolower(end[1]) >= 'A' && HX_tolower(end[1]) <= 'F')
				gbits[z] |= end[1] - HX_tolower(end[1]);
		}

		if (z == 16)
			m_glyph.emplace_back(glyph::create_from_rpad(vfsize(8, 16), gbits, z));
		else if (z == 32)
			m_glyph.emplace_back(glyph::create_from_rpad(vfsize(16, 16), gbits, z));
		else
			fprintf(stderr, "load_hex: unrecognized glyph size (%u bytes) in line %zu\n", z, lnum);
		m_unicode_map->add_i2u(m_glyph.size() - 1, cp);
	}
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

ssize_t font::load_psf(const char *file)
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

ssize_t font::save_bdf(const char *file, const char *aname)
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
	fprintf(fp, "FONT -misc-%s-medium-r-normal--%u-%u-75-75-c-%u-iso10646-1\n", name.c_str(), sz0.y, 10 * sz0.y, 10 * sz0.x);
	fprintf(fp, "SIZE %u 75 75\n", sz0.y);
	fprintf(fp, "FONTBOUNDINGBOX %u %u 0 -%u\n", sz0.x, sz0.y, sz0.y / 4);
	fprintf(fp, "STARTPROPERTIES 24\n");
	fprintf(fp, "FONT_TYPE \"Bitmap\"\n");
	fprintf(fp, "FONTNAME_REGISTRY \"\"\n");
	fprintf(fp, "FOUNDRY \"misc\"\n");
	fprintf(fp, "FAMILY_NAME \"%s\"\n", name.c_str());
	fprintf(fp, "WEIGHT_NAME \"medium\"\n");
	fprintf(fp, "SLANT \"r\"\n");
	fprintf(fp, "SETWIDTH_NAME \"normal\"\n");
	fprintf(fp, "PIXEL_SIZE %u\n", sz0.y);
	fprintf(fp, "POINT_SIZE %u\n", 10 * sz0.y);
	fprintf(fp, "SPACING \"C\"\n");
	fprintf(fp, "AVERAGE_WIDTH %u\n", 10 * sz0.x);
	fprintf(fp, "FONT \"%s\"\n", name.c_str());
	fprintf(fp, "WEIGHT 10\n");
	fprintf(fp, "RESOLUTION 75\n");
	fprintf(fp, "RESOLUTION_X 75\n");
	fprintf(fp, "RESOLUTION_Y 75\n");
	fprintf(fp, "CHARSET_REGISTRY \"ISO10646\"\n");
	fprintf(fp, "CHARSET_ENCODING \"1\"\n");
	fprintf(fp, "QUAD_WIDTH %u\n", sz0.x);
	if (m_unicode_map != nullptr && m_unicode_map->m_u2i.find(65533) != m_unicode_map->m_u2i.cend())
		fprintf(fp, "DEFAULT_CHAR 65533\n");
	else
		fprintf(fp, "DEFAULT_CHAR 0\n");
	fprintf(fp, "FONT_ASCENT %u\n", sz0.y * 12 / 16);
	fprintf(fp, "FONT_DESCENT %u\n", sz0.y * 4 / 16);
	fprintf(fp, "CAP_HEIGHT %u\n", sz0.y);
	fprintf(fp, "X_HEIGHT %u\n", sz0.y * 7 / 16);
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
	return m_glyph.size();
}

void font::save_bdf_glyph(FILE *fp, size_t idx, char32_t cp)
{
	auto sz = m_glyph[idx].m_size;
	fprintf(fp, "STARTCHAR U+%04x\n" "ENCODING %u\n",
		static_cast<unsigned int>(cp), static_cast<unsigned int>(cp));
	fprintf(fp, "SWIDTH 1000 0\n");
	fprintf(fp, "DWIDTH %u 0\n", sz.x);
	fprintf(fp, "BBX %u %u 0 -%u\n", sz.x, sz.y, sz.y / 4);
	fprintf(fp, "BITMAP\n");

	auto byteperline = (sz.x + 7) / 8;
	unsigned int ctr = 0;
	for (auto c : m_glyph[idx].as_rowpad()) {
		fputc(vfhex[(c&0xF0)>>4], fp);
		fputc(vfhex[c&0x0F], fp);
		if (++ctr % byteperline == 0)
			fprintf(fp, "\n");
	}
	fprintf(fp, "ENDCHAR\n");
}

ssize_t font::save_clt(const char *dir)
{
	size_t count = 0;
	if (m_unicode_map == nullptr) {
		for (size_t idx = 0; idx < m_glyph.size(); ++idx, ++count) {
			auto ret = save_clt_glyph(dir, idx, idx);
			if (ret < 0)
				return ret;
		}
		return count;
	}
	for (size_t idx = 0; idx < m_glyph.size(); ++idx)
		for (auto codepoint : m_unicode_map->to_unicode(idx)) {
			auto ret = save_clt_glyph(dir, idx, codepoint);
			if (ret < 0)
				return ret;
			++count;
		}
	return count;
}

ssize_t font::save_clt_glyph(const char *dir, size_t idx, char32_t codepoint)
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

ssize_t font::save_fnt(const char *file)
{
	std::unique_ptr<FILE, deleter> fp(fopen(file, "wb"));
	if (fp == nullptr)
		return -errno;
	size_t count = 0;
	for (const auto &glyph : m_glyph) {
		auto ret = fwrite(glyph.m_data.c_str(), glyph.m_data.size(), 1, fp.get());
		if (ret < 1)
			break;
	}
	return count;
}

ssize_t font::save_map(const char *file)
{
	std::unique_ptr<FILE, deleter> fp(fopen(file, "w"));
	if (fp == nullptr)
		return -errno;
	if (m_unicode_map == nullptr)
		return 0;
	size_t count = 0;
	for (const auto &e : m_unicode_map->m_i2u) {
		fprintf(fp.get(), "0x%02x\t", e.first);
		for (auto uc : e.second) {
			fprintf(fp.get(), "U+%04x ", uc);
			++count;
		}
		fprintf(fp.get(), "\n");
	}
	return count;
}

ssize_t font::save_psf(const char *file)
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
		hdr.height   = cpu_to_le32(m_glyph[0].m_size.y);
		hdr.width    = cpu_to_le32(m_glyph[0].m_size.x);
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

glyph::glyph(const vfsize &size) :
	m_size(size)
{
	m_data.resize(bytes_per_glyph(m_size));
}

glyph glyph::create_from_rpad(const vfsize &size, const char *buf, size_t z)
{
	glyph ng(size);
	auto byteperline = (size.x + 7) / 8;
	for (unsigned int y = 0; y < size.y; ++y) {
		for (unsigned int x = 0; x < size.x; ++x) {
			bitpos qpos = x;
			bitpos opos = y * size.x + x;
			if (buf[y*byteperline+qpos.byte] & qpos.mask)
				ng.m_data[opos.byte] |= opos.mask;
		}
	}
	return ng;
}

glyph glyph::blit(const vfsize &sel, const vfpos &sof, const vfsize &cvs, const vfpos &pof) const
{
	glyph ng(cvs);

	for (unsigned int y = sof.y; y < sof.y + sel.y && y < m_size.y; ++y) {
		for (unsigned int x = sof.x; x < sof.x + sel.x && x < m_size.x; ++x) {
			int ox = pof.x + x - sof.x;
			int oy = pof.y + y - sof.y;
			if (ox < 0 || oy < 0 || static_cast<unsigned int>(ox) > cvs.x ||
			    static_cast<unsigned int>(oy) > cvs.y)
				continue;
			bitpos ipos = y * m_size.x + x;
			bitpos opos = oy * ng.m_size.x + ox;
			if (m_data[ipos.byte] & ipos.mask)
				ng.m_data[opos.byte] |= opos.mask;
		}
	}
	return ng;
}

glyph glyph::flip(bool flipx, bool flipy) const
{
	glyph ng(m_size);
	ng.m_data.resize(m_data.size());

	for (unsigned int y = 0; y < m_size.y; ++y) {
		for (unsigned int x = 0; x < m_size.x; ++x) {
			bitpos ipos = y * m_size.x + x;
			bitpos opos = (flipy ? m_size.y - y - 1 : y) * m_size.x + (flipx ? m_size.x - x - 1 : x);
			if (m_data[ipos.byte] & ipos.mask)
				ng.m_data[opos.byte] |= opos.mask;
		}
	}
	return ng;
}

glyph glyph::upscale(const vfsize &factor) const
{
	glyph ng(vfsize(m_size.x * factor.x, m_size.y * factor.y));
	ng.m_data.resize(bytes_per_glyph(ng.m_size));

	for (unsigned int y = 0; y < ng.m_size.y; ++y) {
		for (unsigned int x = 0; x < ng.m_size.x; ++x) {
			bitpos opos = y * ng.m_size.x + x;
			bitpos ipos = y / factor.y * m_size.x + x / factor.x;
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
	if (m_size.x < 2)
		return;
	for (unsigned int y = 0; y < m_size.y; ++y) {
		bitpos ipos = (y + 1) * m_size.x - 2;
		bitpos opos = (y + 1) * m_size.x - 1;
		if (m_data[ipos.byte] & ipos.mask)
			m_data[opos.byte] |= opos.mask;
		else
			m_data[opos.byte] &= ~opos.mask;
	}
}

std::string glyph::as_pclt() const
{
	auto bpc = bytes_per_glyph(m_size);
	if (m_data.size() < bpc)
		return {};

	std::stringstream ss;
	ss << "PCLT\n" << m_size.x << " " << m_size.y << "\n";
	for (unsigned int y = 0; y < m_size.y; ++y) {
		for (unsigned int x = 0; x < m_size.x; ++x) {
			bitpos pos = y * m_size.x + x;
			ss << ((m_data[pos.byte] & pos.mask) ? "##" : "..");
		}
		ss << "\n";
	}
	return ss.str();
}

std::string glyph::as_rowpad() const
{
	std::string ret;
	auto byteperline = (m_size.x + 7) / 8;
	ret.resize(bytes_per_glyph_rpad(m_size));
	for (unsigned int y = 0; y < m_size.y; ++y) {
		for (unsigned int x = 0; x < m_size.x; ++x) {
			bitpos ipos = y * m_size.x + x;
			bitpos qpos = x;
			if (m_data[ipos.byte] & ipos.mask)
				ret[y*byteperline+qpos.byte] |= qpos.mask;
		}
	}
	return ret;
}

} /* namespace vfalib */
