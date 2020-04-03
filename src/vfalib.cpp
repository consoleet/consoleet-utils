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

class vectorizer final {
	public:
	vectorizer(const glyph &, int descent = 0);
	std::vector<std::vector<edge>> simple();
	std::vector<std::vector<edge>> n1();
	std::vector<std::vector<edge>> n2();
	static constexpr const unsigned int scale_factor = 2;

	private:
	void make_squares();
	void internal_edge_delete();
	unsigned int neigh_edges(unsigned int dir, const vertex &, std::set<edge>::iterator &, std::set<edge>::iterator &) const;
	std::set<edge>::iterator next_edge(unsigned int dir, const edge &) const;
	std::vector<edge> pop_poly(unsigned int flags);
	void set(int, int);

	const glyph &m_glyph;
	int m_descent = 0;
	std::set<edge> emap;
	static const unsigned int P_SIMPLIFY_LINES = 1 << 0;
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
	auto max = std::min(0xE0U, static_cast<unsigned int>(m_glyph.size()));
	for (unsigned int k = 0xC0; k < max; ++k)
		m_glyph[k].lge();
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

int font::save_bdf(const char *file)
{
	std::unique_ptr<FILE, deleter> filep(fopen(file, "w"));
	if (filep == nullptr)
		return -errno;
	auto fp = filep.get();
	vfsize sz0;
	if (m_glyph.size() > 0)
		sz0 = m_glyph[0].m_size;
	std::string bfd_name = name;
	/* X logical font description (XLFD) does not permit dashes */
	std::replace(bfd_name.begin(), bfd_name.end(), '-', ' ');
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

std::pair<int, int> font::find_ascent_descent() const
{
	std::pair<int, int> asds{0, 0};
	if (m_glyph.size() == 0)
		return asds;
	int base = -1;
	if (m_unicode_map == nullptr || m_unicode_map->m_u2i.size() == 0) {
		for (unsigned int c : {'M', 'X', 'x'})
			if (m_glyph.size() >= c)
				base = std::max(base, m_glyph[c].find_baseline());
	} else {
		for (unsigned int c : {'M', 'X', 'x'}) {
			auto i = m_unicode_map->m_u2i.find(c);
			if (i == m_unicode_map->m_u2i.cend())
				continue;
			base = std::max(base, m_glyph[i->second].find_baseline());
		}
	}
	if (base < 0) {
		asds.first = m_glyph[0].m_size.h;
		return asds;
	}
	asds.first = base;
	asds.second = m_glyph[0].m_size.h - base;
	return asds;
}

int font::save_sfd(const char *file, enum vectoalg vt)
{
	std::unique_ptr<FILE, deleter> filep(fopen(file, "w"));
	if (filep == nullptr)
		return -errno;
	auto fp = filep.get();
	std::string ps_name = name;
	/* PostScript name does not allow spaces */
	std::replace(ps_name.begin(), ps_name.end(), ' ', '-');
	auto asds = find_ascent_descent();
	fprintf(fp, "SplineFontDB: 3.0\n");
	fprintf(fp, "FontName: %s\n", ps_name.c_str());
	fprintf(fp, "FullName: %s\n", name.c_str());
	fprintf(fp, "FamilyName: %s\n", name.c_str());
	fprintf(fp, "Weight: medium\n");
	fprintf(fp, "Version: 001.000\n");
	fprintf(fp, "ItalicAngle: 0\n");
	fprintf(fp, "UnderlinePosition: -3\n");
	fprintf(fp, "UnderlineWidth: 1\n");
	fprintf(fp, "Ascent: %d\n", asds.first * vectorizer::scale_factor);
	fprintf(fp, "Descent: %d\n", asds.second * vectorizer::scale_factor);
	fprintf(fp, "NeedsXUIDChange: 1\n");
	fprintf(fp, "FSType: 0\n");
	fprintf(fp, "PfmFamily: 49\n");
	fprintf(fp, "TTFWeight: 500\n");
	fprintf(fp, "TTFWidth: 5\n");
	fprintf(fp, "Panose: 2 0 6 9 9 0 0 0 0 0\n");
	fprintf(fp, "LineGap: 0\n");
	fprintf(fp, "VLineGap: 0\n");
	fprintf(fp, "OS2TypoAscent: %d\n", asds.first * vectorizer::scale_factor);
	fprintf(fp, "OS2TypoAOffset: 0\n");
	fprintf(fp, "OS2TypoDescent: %d\n", -asds.second * vectorizer::scale_factor);
	fprintf(fp, "OS2TypoDOffset: 0\n");
	fprintf(fp, "OS2TypoLinegap: 0\n");
	fprintf(fp, "OS2WinAscent: %d\n", asds.first * vectorizer::scale_factor);
	fprintf(fp, "OS2WinAOffset: 0\n");
	fprintf(fp, "OS2WinDescent: %d\n", asds.second * vectorizer::scale_factor);
	fprintf(fp, "OS2WinDOffset: 0\n");
	fprintf(fp, "HheadAscent: %d\n", asds.first * vectorizer::scale_factor);
	fprintf(fp, "HheadAOffset: 0\n");
	fprintf(fp, "HheadDescent: %d\n", -asds.second * vectorizer::scale_factor);
	fprintf(fp, "HheadDOffset: 0\n");
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
			save_sfd_glyph(fp, idx, idx, asds.first, asds.second, vt);
	} else {
		for (const auto &pair : m_unicode_map->m_u2i)
			save_sfd_glyph(fp, pair.second, pair.first, asds.first, asds.second, vt);
	}
	fprintf(fp, "EndChars\n");
	fprintf(fp, "EndSplineFont\n");
	return 0;
}

vectorizer::vectorizer(const glyph &g, int desc) :
	m_glyph(g), m_descent(desc)
{}

void vectorizer::set(int x, int y)
{
	/* TTF/OTF spec wants CCW orientation */
	int s = scale_factor;
	x *= s;
	y *= s;
	emap.insert(edge{{x, y}, {x, y + s}});
	emap.insert(edge{{x, y + s}, {x + s, y + s}});
	emap.insert(edge{{x + s, y + s}, {x + s, y}});
	emap.insert(edge{{x + s, y}, {x, y}});
}

void vectorizer::make_squares()
{
	const auto &sz = m_glyph.m_size;
	for (unsigned int y = 0; y < sz.h; ++y) {
		int yy = sz.h - 1 - static_cast<int>(y) - m_descent;
		for (unsigned int x = 0; x < sz.w; ++x) {
			bitpos ipos = y * sz.w + x;
			if (m_glyph.m_data[ipos.byte] & ipos.mask)
				set(x, yy);
		}
	}
}

void vectorizer::internal_edge_delete()
{
	/*
	 * Remove overlaps: As enforced by set(), all the polygons are added
	 * with the same orientation. Polygons at most touch, and never
	 * overlap. Joining polygons therefore simply requires removing shared
	 * contradirectional edges. The remaining set of edges then forms a new
	 * set of polygons, and, as the edges themselves were never reoriented,
	 * these polygons have the correct orientation.
	 */
	for (auto edge = emap.begin(); edge != emap.end(); ) {
		auto twin = emap.find({edge->end_vtx, edge->start_vtx});
		if (twin == edge) {
			printf("Glyph outline description is faulty: edge with startvtx==endvtx (%d,%d)\n",
				edge->start_vtx.x, edge->start_vtx.y);
			break;
		}
		if (twin == emap.cend()) {
			++edge;
			continue;
		}
		emap.erase(twin);
		edge = emap.erase(edge);
	}
}

/**
 * Find the next edges (up to two) for @tail.
 */
unsigned int vectorizer::neigh_edges(unsigned int cur_dir, const vertex &tail,
    std::set<edge>::iterator &inward, std::set<edge>::iterator &outward) const
{
	inward = emap.lower_bound({tail, {INT_MIN, INT_MIN}});
	if (inward == emap.end() || inward->start_vtx != tail) {
		outward = inward = emap.end();
		return 0;
	}
	outward = std::next(inward); /* due to sortedness of @emap */
	if (outward == emap.cend() || outward->start_vtx != tail) {
		outward = emap.end();
		return 1;
	}
	if (cur_dir == 0 || cur_dir == 270)
		std::swap(inward, outward); /* order of @emap */
	return 2;
}

std::set<edge>::iterator vectorizer::next_edge(unsigned int cur_dir,
    const edge &cur_edge) const
{
	const auto &tail = cur_edge.end_vtx;
	std::set<edge>::iterator inward, outward;
	neigh_edges(cur_dir, tail, inward, outward);
	/*
	 * If there are two edges from a given start vertex, prefer the
	 * edge which makes an inward curving. This makes a shape like
	 *
	 *   ######
	 *   ##  ##
	 *     ####
	 *
	 * be emitted a single polygon, rather than two (outer &
	 * enclave). The tradeoff is that fully-enclosed enclaves, e.g.
	 *
	 * ########
	 * ##  ####
	 * ####  ##
	 * ########
	 *
	 * will favor making a single polygon with self-intersection.
	 * The enclave of the number '4', when a 1-px stroke thickness
	 * is used, also ceases to be an enclave.
	 *
	 * (None of all this has an effect on rendering, just the way
	 * font editors see the outline.)
	 */
	return inward;
}

std::vector<edge> vectorizer::pop_poly(unsigned int flags)
{
	std::vector<edge> poly;
	if (emap.size() == 0)
		return poly;
	poly.push_back(*emap.begin());
	emap.erase(emap.begin());
	auto prev_dir = poly[0].trivial_dir();

	while (true) {
		if (emap.size() == 0)
			break;
		auto &tail_vtx = poly.rbegin()->end_vtx;
		if (tail_vtx == poly.cbegin()->start_vtx)
			break;
		auto next = next_edge(prev_dir, *poly.rbegin());
		if (next == emap.cend()) {
			fprintf(stderr, "unclosed poly wtf?!\n");
			break;
		}

		/*
		 * Skip redundant vertices along the way to the next
		 * directional change of the outline. (Vertices are not
		 * deleted, and they are also duplicated, in case another
		 * polygon has a vertex in the same location.)
		 */
		auto next_dir = next->trivial_dir();
		if ((flags & P_SIMPLIFY_LINES) && next_dir == prev_dir)
			tail_vtx = next->end_vtx;
		else
			poly.push_back(*next);
		emap.erase(next);
		prev_dir = next_dir;
	}
	return poly;
}

std::vector<std::vector<edge>> vectorizer::simple()
{
	make_squares();
	internal_edge_delete();
	std::vector<std::vector<edge>> pmap;
	while (true) {
		auto poly = pop_poly(P_SIMPLIFY_LINES);
		if (poly.size() == 0)
			break;
		pmap.push_back(std::move(poly));
	}
	return pmap;
}

static inline bool testbit_c(const glyph &g, int x, int y)
{
	if (x < 0 || y < 0 || x >= static_cast<int>(g.m_size.w) || y >= static_cast<int>(g.m_size.h))
		return false;
	bitpos bp = y * g.m_size.w + x;
	return g.m_data[bp.byte] & bp.mask;
}

static inline bool testbit_u(const glyph &g, int x, int y)
{
	bitpos bp = y * g.m_size.w + x;
	return g.m_data[bp.byte] & bp.mask;
}

std::vector<std::vector<edge>> vectorizer::n1()
{
	auto &g = m_glyph;
	const auto &sz = g.m_size;
	for (unsigned int uy = 0; uy < sz.h; ++uy) {
		int y = sz.h - 1 - static_cast<int>(uy) - m_descent;
		for (unsigned int ux = 0; ux < sz.w; ++ux) {
			bitpos ipos = uy * sz.w + ux;
			int x = ux;

			bool c1 = testbit_c(g, ux - 1, uy + 1);
			bool c2 = testbit_c(g, ux,     uy + 1);
			bool c3 = testbit_c(g, ux + 1, uy + 1);
			bool c4 = testbit_c(g, ux - 1, uy);
			bool c5 = testbit_u(g, ux,     uy);
			bool c6 = testbit_c(g, ux + 1, uy);
			bool c7 = testbit_c(g, ux - 1, uy - 1);
			bool c8 = testbit_c(g, ux,     uy - 1);
			bool c9 = testbit_c(g, ux + 1, uy - 1);

			bool di = c5;
			bool tl = (c4 && ((c8 && ((!c7 && (c1 || c3 || c9)) || (!c1 && !c2) || (!c6 && !c9))) || c5)) || (c5 && ((!c1 && !c9) || c7 || c8));
			bool tr = (((!c7 && !c3) || c9 || c8 || c6) && c5) || (((!c9 && (c1 || c3 || c7)) || (!c2 && !c3) || (!c4 && !c7)) && c8 && c6);
			bool bl = (c5 && (c1 || c2 || (!c3 && !c7) || c4)) || (c2 && c4 && ((!c1 && (c3 || c7 || c9)) || (!c3 && !c6) || (!c7 && !c8)));
			bool br = (c2 && ((c6 && ((!c3 && (c1 || c7 || c9)) || (!c1 && !c4) || (!c8 && !c9))) || c5)) || (c5 && ((!c1 && !c9) || c3 || c6));

			if (tl) {
				emap.insert(edge{{2*x,   2*y+1}, {2*x,   2*y+2}});
				emap.insert(edge{{2*x,   2*y+2}, {2*x+1, 2*y+2}});
				emap.insert(edge{{2*x+1, 2*y+2}, {2*x,   2*y+1}});
			}
			if (tr) {
				emap.insert(edge{{2*x+1, 2*y+2}, {2*x+2, 2*y+2}});
				emap.insert(edge{{2*x+2, 2*y+2}, {2*x+2, 2*y+1}});
				emap.insert(edge{{2*x+2, 2*y+1}, {2*x+1, 2*y+2}});
			}
			if (bl) {
				emap.insert(edge{{2*x,   2*y},   {2*x,   2*y+1}});
				emap.insert(edge{{2*x,   2*y+1}, {2*x+1, 2*y}});
				emap.insert(edge{{2*x+1, 2*y},   {2*x,   2*y}});
			}
			if (br) {
				emap.insert(edge{{2*x+1, 2*y},   {2*x+2, 2*y+1}});
				emap.insert(edge{{2*x+2, 2*y+1}, {2*x+2, 2*y}});
				emap.insert(edge{{2*x+2, 2*y},   {2*x+1, 2*y}});
			}
			if (di) {
				emap.insert(edge{{2*x,   2*y+1}, {2*x+1, 2*y+2}});
				emap.insert(edge{{2*x+1, 2*y+2}, {2*x+2, 2*y+1}});
				emap.insert(edge{{2*x+2, 2*y+1}, {2*x+1, 2*y  }});
				emap.insert(edge{{2*x+1, 2*y  }, {2*x,   2*y+1}});
			}
		}
	}

	internal_edge_delete();
	std::vector<std::vector<edge>> pmap;
	while (true) {
		auto poly = pop_poly(P_SIMPLIFY_LINES);
		if (poly.size() == 0)
			break;
		pmap.push_back(std::move(poly));
	}
	return pmap;
}

static void n2_angle(std::vector<edge> &poly)
{
	static const unsigned int M_HEAD = 0x20, M_TAIL = 0x02,
		M_XHEAD = 0x10, M_XTAIL = 0x01;
	std::vector<unsigned int> flags(poly.size());

	for (size_t xm3 = 0; xm3 < poly.size(); ++xm3) {
		auto xm2 = (xm3 + 1) % poly.size();
		auto xm1 = (xm3 + 2) % poly.size();
		auto x00 = (xm3 + 3) % poly.size();
		auto xp1 = (xm3 + 4) % poly.size();
		auto xp2 = (xm3 + 5) % poly.size();
		auto xp3 = (xm3 + 6) % poly.size();
		auto dm3 = poly[xm3].trivial_dir(), dm2 = poly[xm2].trivial_dir();
		auto dm1 = poly[xm1].trivial_dir(), d00 = poly[x00].trivial_dir();
		auto dp1 = poly[xp1].trivial_dir(), dp2 = poly[xp2].trivial_dir();
		auto dp3 = poly[xp3].trivial_dir();

#if 0
		printf("I%zu dm3:\e[32m%d\e[0m,dm2:\e[32m%d\e[0m,"
			"dm1:\e[32m%d\e[0m,d:\e[32m%d\e[0m,dp1:\e[32m%d\e[0m,"
			"dp2:\e[32m%d\e[0m,dp3:\e[32m%d\e[0m\n",
			x00, dm3, dm2, dm1, d00, dp1, dp2, dp3);
#endif

		if (d00 == dm2 && d00 == dp2) {
			/* _|~|_ or ~|_|~ pattern seen */
			if ((dm3 == d00 || dm3 == dp1) &&
			    (dp3 == d00 || dp3 == dm1) &&
			    dm1 == (dm2 + 270) % 360 && dp1 == (dm2 + 90) % 360) {
				/* pimple __|~|__ ('f', '4'), retain */
				flags[xm2] |= M_XTAIL;
				flags[xm1]  = M_XHEAD | M_XTAIL;
				flags[x00]  = M_XHEAD | M_XTAIL;
				flags[xp1]  = M_XHEAD | M_XTAIL;
				flags[xp2] |= M_XHEAD;
				continue;
			}

			if (dm1 == (dm2 + 90) % 360 && dp1 == (dm2 + 270) % 360) {
				/* dimple ~~|_|~~ ('8'), sink it */
				if (dm3 == dm2) {
					/* with left-side flat zone */
					flags[xm2] |= M_TAIL;
					flags[xm1]  = M_HEAD | M_TAIL;
					flags[x00] |= M_HEAD;
				}
				if (dp3 == dp2) {
					/* with right-side flat zone */
					flags[x00] |= M_TAIL;
					flags[xp1]  = M_HEAD | M_TAIL;
					flags[xp2] |= M_HEAD;
				}
				continue;
			}
		}

		/* Test for chicane */
		if (dm1 != dp1)
			continue;
		if ((d00 + 270) % 360 != dp1 && (d00 + 90) % 360 != dp1)
			continue;

		/* #5: serif (ramp), topleft of 'E' */
		if (dm2 == dm1 && dp2 == (d00 + 180) % 360 && dp3 == dp2)
			continue;
		/* bottomleft 'E' */
		if (dp2 == dp1 && dm2 == (d00 + 180) % 360 && dm3 == dm2)
			continue;

		/*
		 * #1: single step, with or without sump,
		 * #2: bottom of stairs, with or without sump,
		 * #3: stairs midpart,
		 * #4: top of stairs (implies no sump)
		 */
		flags[xm1] |= M_TAIL;
		flags[x00]  = M_HEAD | M_TAIL;
		flags[xp1] |= M_HEAD;

		if (dp2 == d00) {
			flags[xp1] |= M_TAIL;
			flags[xp2] |= M_HEAD;
		}
		if (dm2 == d00) {
			flags[xm2] |= M_TAIL;
			flags[xm1] |= M_HEAD;
		}
	}

	auto p_iter = poly.begin();
	auto f_iter = flags.begin();
	for (size_t ia = 0; ia < poly.size(); ++ia, ++p_iter, ++f_iter) {
		auto ix = ia + 1;
		auto ib = ix % poly.size();
		if (!(flags[ia] & M_TAIL && flags[ib] & M_HEAD))
			continue;
		if ((flags[ia] & M_XTAIL) || (flags[ib] & M_XHEAD))
			continue;

		flags[ia] &= ~M_TAIL;
		flags[ib] &= ~M_HEAD;
		p_iter = poly.insert(std::next(p_iter), edge{{-64, -64}, {-64, -64}});
		f_iter = flags.insert(std::next(f_iter), 0);
		ib = (ia + 2) % poly.size();

		auto da = poly[ia].trivial_dir(), db = poly[ib].trivial_dir();
		if (da == 0)
			--poly[ia].end_vtx.y;
		else if (da == 90)
			--poly[ia].end_vtx.x;
		else if (da == 180)
			++poly[ia].end_vtx.y;
		else if (da == 270)
			++poly[ia].end_vtx.x;
		if (db == 0)
			++poly[ib].start_vtx.y;
		else if (db == 90)
			++poly[ib].start_vtx.x;
		else if (db == 180)
			--poly[ib].start_vtx.y;
		else if (db == 270)
			--poly[ib].start_vtx.x;
		poly[ix].start_vtx = poly[ia].end_vtx;
		poly[ix].end_vtx   = poly[ib].start_vtx;
		++ia;
	}
	poly.erase(std::remove_if(poly.begin(), poly.end(), [](const edge &e) {
		return e.start_vtx == e.end_vtx;
	}), poly.end());

	p_iter = poly.begin();
	while (p_iter != poly.cend()) {
		auto next = std::next(p_iter);
		if (next == poly.end())
			break;
		auto d1 = p_iter->trivial_dir(), d2 = next->trivial_dir();
		if (d1 != d2) {
			++p_iter;
			continue;
		}
		p_iter->end_vtx = next->end_vtx;
		poly.erase(next);
	}
}

std::vector<std::vector<edge>> vectorizer::n2()
{
	make_squares();
	internal_edge_delete();
	std::vector<std::vector<edge>> pmap;
	while (true) {
		/* Have all edges retian length 1 */
		auto poly = pop_poly(0);
		if (poly.size() == 0)
			break;
		n2_angle(poly);
		pmap.push_back(std::move(poly));
	}
	return pmap;
}

void font::save_sfd_glyph(FILE *fp, size_t idx, char32_t cp, int asc, int desc,
    enum vectoalg vt)
{
	unsigned int cpx = cp;
	const auto &g = m_glyph[idx];
	const auto &sz = g.m_size;
	fprintf(fp, "StartChar: %04x\n", cpx);
	fprintf(fp, "Encoding: %u %u %u\n", cpx, cpx, cpx);
	fprintf(fp, "Width: %u\n", sz.w * vectorizer::scale_factor);
	fprintf(fp, "Flags: MW\n");
	fprintf(fp, "Fore\n");
	fprintf(fp, "SplineSet\n");

	std::vector<std::vector<edge>> pmap;
	if (vt == V_SIMPLE)
		pmap = vectorizer(m_glyph[idx], desc).simple();
	else if (vt == V_N1)
		pmap = vectorizer(m_glyph[idx], desc).n1();
	else if (vt == V_N2)
		pmap = vectorizer(m_glyph[idx], desc).n2();
	for (const auto &poly : pmap) {
		const auto &v1 = poly.cbegin()->start_vtx;
		fprintf(fp, "%d %d m 25\n", v1.x, v1.y);
		for (const auto &edge : poly)
			fprintf(fp, " %d %d l 25\n", edge.end_vtx.x, edge.end_vtx.y);
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

int glyph::find_baseline() const
{
	for (int y = m_size.h - 1; y >= 0; --y) {
		for (unsigned int x = 0; x < m_size.w; ++x) {
			bitpos ipos = y * m_size.w + x;
			if (m_data[ipos.byte] & ipos.mask)
				return y + 1;
		}
	}
	return -1;
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

bool vertex::operator<(const struct vertex &o) const
{
	return std::tie(x, y) < std::tie(o.x, o.y);
}

bool vertex::operator==(const struct vertex &o) const
{
	return std::tie(x, y) == std::tie(o.x, o.y);
}

bool edge::operator<(const struct edge &o) const
{
	return std::tie(start_vtx, end_vtx) < std::tie(o.start_vtx, o.end_vtx);
}

bool edge::operator==(const struct edge &o) const
{
	return std::tie(start_vtx, end_vtx) == std::tie(o.start_vtx, o.end_vtx);
}

unsigned int edge::trivial_dir() const
{
	/*
	 * If the glyph has anything but straight lines and diagonals,
	 * you need to switch to atan().
	 */
	if (end_vtx.y > start_vtx.y)
		return end_vtx.x == start_vtx.x ? 0 :
		       end_vtx.x < start_vtx.x ? 315 : 45;
	if (end_vtx.y < start_vtx.y)
		return end_vtx.x == start_vtx.x ? 180 :
		       end_vtx.x < start_vtx.x ? 225 : 135;
	return end_vtx.x < start_vtx.x ? 270 : 90;
}

} /* namespace vfalib */
