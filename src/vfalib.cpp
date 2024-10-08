// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2019 Jan Engelhardt
/*
 *	I/O and glyph manipulation routines of the "VGA font assembler"
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

using namespace vfalib;

namespace {

enum { /* see libXfont */
	PCF_PROPERTIES       = 0x1U,
	PCF_ACCELERATORS     = 0x2U,
	PCF_METRICS          = 0x4U,
	PCF_BITMAPS          = 0x8U,
	PCF_INK_METRICS      = 0x10U,
	PCF_BDF_ENCODINGS    = 0x20U,
	PCF_SWIDTHS          = 0x40U,
	PCF_GLYPH_NAMES      = 0x80U,
	PCF_BDF_ACCELERATORS = 0x100U,
};

enum {
	PCF_DEFAULT_FORMAT     = 0,
	PCF_INKBOUNDS          = 0x200U,
	PCF_ACCEL_W_INKBOUNDS  = 0x100U,
	PCF_COMPRESSED_METRICS = 0x100U,

	PCF_GLYPH_PAD_MASK     = 0x3U,
	PCF_BYTE_MASK          = 0x4U,
	PCF_FORMAT_MASK        = 0xffffff00U,
};

enum {
	PSF1_MAGIC0 = 0x36,
	PSF1_MAGIC1 = 0x04,
	PSF1_MF_512 = 1 << 0,
	PSF1_MF_TAB = 1 << 1,
	PSF1_MF_SEQ = 1 << 2,
	PSF2_MAGIC0 = 0x72,
	PSF2_MAGIC1 = 0xB5,
	PSF2_MAGIC2 = 0x4A,
	PSF2_MAGIC3 = 0x86,
	PSF2_HAS_UNICODE_TABLE = 0x01,
	VFA_UCS2 = 0x8000,
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

struct pcf_table {
	uint32_t type, format, size, offset;
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
	std::vector<std::vector<edge>> n2(unsigned int flags = 0);

	/*
	 * A distance of one pixel is mapped to this many vector font units.
	 * The EmSize value is also scaled, hence a font generally always looks
	 * the same regardless of the chosen factor.
	 *
	 * When the N2 vectorizer does its work, it generates nodal points at
	 * what would be half a pixel. Because SFD does not use floating point,
	 * we need to increase the base precision first.
	 */
	static constexpr int default_scale_factor = 2;
	int scale_factor_x = default_scale_factor, scale_factor_y = default_scale_factor;
	static const unsigned int P_ISTHMUS = 1 << 1;

	private:
	void make_squares();
	void internal_edge_delete();
	unsigned int neigh_edges(unsigned int dir, const vertex &, std::set<edge>::iterator &, std::set<edge>::iterator &) const;
	std::set<edge>::iterator next_edge(unsigned int dir, const edge &, unsigned int flags) const;
	std::vector<edge> pop_poly(unsigned int flags);
	void set(int, int);

	const glyph &m_glyph;
	int m_descent = 0;
	std::set<edge> emap;
	static const unsigned int P_SIMPLIFY_LINES = 1 << 0;
};

}

static const char vfhex[] = "0123456789abcdef";

static FILE *vfopen(const char *name, const char *mode)
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
	std::unique_ptr<FILE, deleter> fp(vfopen(file, "rb"));
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
		HX_chomp(line);
		int keyfrom = strtol(line, &end, 0), keyto = keyfrom;
		++lnum;
		do {
			if (*end == '-')
				keyto = strtol(end + 1, &end, 0);
			ptr = end;
			while (HX_isspace(*ptr) || *ptr == '\r')
				++ptr;
			if (*ptr == '\0' || *ptr == '\n' || *ptr == '#')
				break;
			if (strcmp(ptr, "idem") == 0) {
				/*
				 * Nothing needed; missing entries in the i2u
				 * map imply idempotency.
				 */
				break;
			} else if (ptr[0] != 'U') {
				fprintf(stderr, "Warning: Unexpected char '%c' in unicode map line %zu.\n", ptr[0], lnum);
				break;
			} else if (ptr[1] != '+') {
				fprintf(stderr, "Warning: Unexpected char '%c' in unicode map line %zu.\n", ptr[1], lnum);
				break;
			}
			if (keyfrom != keyto) {
				fprintf(stderr, "Warning: No support for ranged mappings (0x%x-0x%x here) for anything but \"idem\".\n", keyfrom, keyto);
				break;
			}
			ptr += 2;
			auto val = strtoul(ptr, &end, 16);
			if (end == ptr)
				break;
			add_i2u(keyfrom, val);
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

void unicode_map::swap_idx(unsigned int a, unsigned int b)
{
	decltype(m_i2u) new_i2u;
	decltype(m_u2i) new_u2i;
	for (auto &e : m_u2i) {
		if (e.second == a)
			e.second = b;
		else if (e.second == b)
			e.second = a;
	}
	for (const auto &e : m_i2u)
		new_i2u.emplace(e.first == a ? b :
		                e.first == b ? a :
		                e.first, std::move(e.second));
	m_i2u = std::move(new_i2u);
}

font::font() :
	props{
		{"FontName", "vfontas-output"},
		{"FamilyName", "vfontas output"},
		{"FullName", "vfontas output"},
		{"Weight", "medium"},
		{"TTFWeight", "500"},
	}
{}

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

void font::lgeu()
{
	static constexpr uint16_t cand[] = {
		/*
		 * It looks like cp{737,850,852,865,866} only have subsets
		 * of cp437's graphic characters. Therefore I did not bother
		 * checking cp{855,857,860,861,863,869}.
		 */
		0x2500, 0x250c, 0x2514, 0x2518, 0x251c, 0x252c, 0x2534, 0x253c,
		0x2550, 0x2552, 0x2553, 0x2554, 0x2558, 0x2559, 0x255a, 0x255e,
		0x255f, 0x2560, 0x2564, 0x2565, 0x2566, 0x2567, 0x2568, 0x2569,
		0x256a, 0x256b, 0x256c, 0x2580, 0x2584, 0x2588, 0x258c, 0x2590,
	};
	if (m_unicode_map == nullptr) {
		fprintf(stderr, "This font has no unicode map, can't perform LGEU command.\n");
		return;
	}
	auto &map = *m_unicode_map;
	for (auto uc : cand) {
		auto it = map.m_u2i.find(uc);
		if (it != map.m_u2i.end())
			m_glyph[it->second].lge();
	}
}

void font::lgeuf()
{
	if (m_unicode_map == nullptr) {
		fprintf(stderr, "This font has no unicode map, can't perform LGEU command.\n");
		return;
	}
	auto &map = *m_unicode_map;
	for (auto it = map.m_u2i.lower_bound(0x2500);
	     it != map.m_u2i.upper_bound(0x2591); ++it)
		m_glyph[it->second].lge();
	for (auto it = map.m_u2i.lower_bound(0x2591);
	     it != map.m_u2i.upper_bound(0x2594); ++it)
		m_glyph[it->second].lge(2);
	for (auto it = map.m_u2i.lower_bound(0x2594);
	     it != map.m_u2i.upper_bound(0x2600); ++it)
		m_glyph[it->second].lge();
}

void font::overstrike(unsigned int px)
{
	for (auto &g : m_glyph)
		g = g.overstrike(px);
}

struct bdfglystate {
	int uc = -1, w = 0, h = 0, of_left = 0, of_baseline = 0;
	unsigned int dwidth = 0, lr = 0;
	unsigned int font_ascent = 0, font_descent = 0, font_height = 0;
	std::string name, buf;

	void reset() {
		w = h = of_left = of_baseline = dwidth = lr = 0;
		uc = -1;
		name.clear();
		buf.clear();
	}
};

static size_t hexrunparse(void *vdest, size_t destsize, const char *p)
{
	auto dest = static_cast<uint8_t *>(vdest);
	size_t written = 0;
	while (destsize > 0) {
		auto c = HX_tolower(*p++);
		if (c >= '0' && c <= '9')
			c -= '0';
		else if (c >= 'a' && c <= 'f')
			c = c - 'a' + 10;
		else
			break;
		auto d = HX_tolower(*p++);
		if (d >= '0' && d <= '9')
			d -= '0';
		else if (d >= 'a' && d <= 'f')
			d = d - 'a' + 10;
		else
			break;
		*dest++ = (c << 4) | d;
		++written;
	}
	return written;
}

static void bdfbitparse(bdfglystate &cchar, const char *line)
{
	auto offset = cchar.buf.size();
	auto bpl = (cchar.w + 7) / 8;
	cchar.buf.resize(offset + bpl);
	auto z = hexrunparse(cchar.buf.data() + offset, bpl, line);
	cchar.buf.resize(offset + z);
}

static glyph bdfcomplete(const bdfglystate &cchar)
{
	vfsize bbx_size(cchar.w, cchar.h);
	auto g = glyph::create_from_rpad(bbx_size, cchar.buf.c_str(), bytes_per_glyph(bbx_size));
	vfrect src_rect, dst_rect;
	src_rect.x = cchar.of_left >= 0 ? 0 : -cchar.of_left;
	src_rect.w = cchar.of_left >= 0 ? cchar.w : std::max(0, cchar.w + cchar.of_left);
	src_rect.y = 0;
	src_rect.h = cchar.h;
	dst_rect.x = std::max(0, cchar.of_left);
	dst_rect.y = std::max(0, static_cast<int>(cchar.font_ascent) - cchar.of_baseline - cchar.h);
	dst_rect.w = cchar.dwidth;
	dst_rect.h = cchar.font_height;
	return g.copy_rect_to(src_rect, glyph(dst_rect), dst_rect);
}

#include "glynames.cpp"

static std::string translate_charname(const char *s)
{
	if (*s == 'C') {
		const char *p;
		for (p = s; HX_isdigit(*p) && *p != '\0'; ++p)
			;
		if (*p == '\0')
			return s;
	}
	if (strncmp(s, "uni", 3) == 0) {
		char *end = nullptr;
		auto uc = strtoul(&s[1], &end, 16);
		if (end != nullptr && *end == '\0') {
			char buf[16];
			snprintf(buf, sizeof(buf), "C%lu", uc);
			return buf;
		}
	}
	auto it = std::lower_bound(std::cbegin(ff_glyph_names), std::cend(ff_glyph_names), s,
	          [](const std::pair<const char *, uint32_t> &p, const char *q) {
	          	return strcmp(p.first, q) < 0;
	          });
	if (it == std::cend(ff_glyph_names) || strcmp(it->first, s) != 0)
		return s;
	char buf[16];
	snprintf(buf, sizeof(buf), "C%u", it->second);
	return buf;
}

int font::load_bdf(const char *filename)
{
	enum { BDF_NONE, BDF_FONT, BDF_CHAR, BDF_BITMAP, BDF_PASTBITMAP, BDF_DONE };
	std::unique_ptr<FILE, deleter> fp(vfopen(filename, "r"));
	if (fp == nullptr)
		return -errno;
	if (m_unicode_map == nullptr)
		m_unicode_map = std::make_shared<unicode_map>();

	hxmc_t *line = nullptr;
	auto lineclean = make_scope_success([&]() { HXmc_free(line); });
	unsigned int state = BDF_NONE;
	bdfglystate cchar;

	while (HX_getl(&line, fp.get()) != nullptr) {
		HX_chomp(line);
		if (state == BDF_NONE) {
			if (strcmp(line, "STARTFONT 2.1") == 0) {
				state = BDF_FONT;
				continue;
			}
		} else if (state == BDF_FONT) {
			if (strcmp(line, "ENDFONT") == 0)
				break;
			if (strncmp(line, "STARTCHAR ", 10) == 0) {
				cchar.reset();
				cchar.font_height = cchar.font_ascent + cchar.font_descent;
				cchar.name = translate_charname(line + 10);
				state = BDF_CHAR;
				continue;
			}
			auto fields = sscanf(line, "FONT_ASCENT %u", &cchar.font_ascent);
			if (fields == 1)
				continue;
			fields = sscanf(line, "FONT_DESCENT %u", &cchar.font_descent);
			if (fields == 1)
				continue;
		} else if (state == BDF_CHAR) {
			int tmp = -1;
			auto fields = sscanf(line, "ENCODING %d %d", &tmp, &cchar.uc);
			if (fields == 2 && tmp == -1) {
				continue;
			} else if (fields == 1 && tmp == -1 && cchar.uc == -1 &&
			    cchar.name.size() >= 2 && cchar.name[0] == 'C' &&
			    HX_isdigit(cchar.name[1])) {
				cchar.uc = strtoul(cchar.name.c_str() + 1, nullptr, 10);
				continue;
			} else if (fields == 1 && tmp == -1) {
				state = BDF_PASTBITMAP;
				continue;
			} else if (fields == 1) {
				cchar.uc = tmp;
				continue;
			}
			fields = sscanf(line, "DWIDTH %d", &cchar.dwidth);
			if (fields == 1)
				continue;
			/* only supporting Writing Mode 0 right now */
			fields = sscanf(line, "BBX %d %d %d %d", &cchar.w, &cchar.h, &cchar.of_left, &cchar.of_baseline);
			if (fields == 4) {
				cchar.lr = cchar.h;
				continue;
			}
			if (strcmp(line, "BITMAP") == 0) {
				state = cchar.lr == 0 ? BDF_PASTBITMAP : BDF_BITMAP;
				continue;
			}
		} else if (state == BDF_BITMAP) {
			if (cchar.lr == 0) {
				state = BDF_PASTBITMAP;
				continue;
			}
			if (cchar.lr-- > 0)
				bdfbitparse(cchar, line);
			if (cchar.lr == 0) {
				state = BDF_PASTBITMAP;
				continue;
			}
		} else if (state == BDF_PASTBITMAP) {
			if (strcmp(line, "ENDCHAR") == 0) {
				if (cchar.uc != -1) {
					m_unicode_map->add_i2u(m_glyph.size(), cchar.uc);
					m_glyph.push_back(bdfcomplete(std::move(cchar)));
				}
				state = BDF_FONT;
				continue;
			}
		}
	}
	return 0;
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
		auto last_idx = m_glyph.size() - 1;
		auto repl = m_unicode_map->m_u2i.find(last_idx);
		if (repl != m_unicode_map->m_u2i.end()) {
			/* There is a glyph which would be better for this spot */
			std::swap(m_glyph.back(), m_glyph[repl->second]);
			m_unicode_map->swap_idx(last_idx, repl->second);
		}
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

int font::load_fnt(const char *file, unsigned int width, unsigned int height)
{
	std::unique_ptr<FILE, deleter> fp(vfopen(file, "rb"));
	if (fp == nullptr)
		return -errno;
	if (width == static_cast<unsigned int>(-1))
		width = 8;
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
	std::unique_ptr<FILE, deleter> fp(vfopen(file, "r"));
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

		char gbits[32]{};
		HX_chomp(line);
		auto z = hexrunparse(gbits, ARRAY_SIZE(gbits), end);
		if (z == 16)
			m_glyph.emplace_back(glyph::create_from_rpad(vfsize(8, 16), gbits, z));
		else if (z == 32)
			m_glyph.emplace_back(glyph::create_from_rpad(vfsize(16, 16), gbits, z));
		else
			fprintf(stderr, "load_hex: unrecognized glyph size (%zu bytes) in line %zu\n", z, lnum);
		m_unicode_map->add_i2u(m_glyph.size() - 1, cp);
	}
	HXmc_free(line);
	return 0;
}

static int load_pcf_props(FILE *fp, std::map<std::string, std::string> &map)
{
	uint32_t val = 0;
	if (fread(&val, 4, 1, fp) != 1)
		return -EINVAL;
	val = le32_to_cpu(val);
	if ((val & PCF_FORMAT_MASK) != (PCF_DEFAULT_FORMAT & PCF_FORMAT_MASK))
		return -EINVAL;
	bool be = val & PCF_BYTE_MASK;
	if (fread(&val, 4, 1, fp) != 1)
		return -EINVAL;
	auto numprop = be ? be32_to_cpu(val) : le32_to_cpu(val);
	auto tbl_offset = ftell(fp);
	fseek(fp, numprop * 9, SEEK_CUR);
	fseek(fp, 4 - (ftell(fp) & 3), SEEK_CUR);
	if (fread(&val, 4, 1, fp) != 1)
		return -EINVAL;
	val = be ? be32_to_cpu(val) : le32_to_cpu(val);
	std::string sblk;
	sblk.resize(val);
	if (fread(&sblk[0], sblk.size(), 1, fp) != 1)
		return -EINVAL;
	fseek(fp, tbl_offset, SEEK_SET);
	for (unsigned int i = 0; i < numprop; ++i) {
		if (fread(&val, 4, 1, fp) != 1)
			return -EINVAL;
		auto name_idx = be ? be32_to_cpu(val) : le32_to_cpu(val);
		uint8_t is_string = 0;
		if (fread(&is_string, 1, 1, fp) != 1)
			return -EINVAL;
		if (fread(&val, 4, 1, fp) != 1)
			return -EINVAL;
		auto val_idx = be ? be32_to_cpu(val) : le32_to_cpu(val);
		if (name_idx >= sblk.size() || val_idx >= sblk.size())
			return -EINVAL;
		if (is_string)
			map[&sblk[name_idx]] = &sblk[val_idx];
		else
			map[&sblk[name_idx]] = std::to_string(val_idx);
	}
	return 0;
}

static int load_pcf_bitmaps(FILE *fp)
{
	uint32_t val;
	if (fread(&val, 4, 1, fp) != 1)
		return -EINVAL;
	auto fmt = le32_to_cpu(val);
	if ((fmt & PCF_FORMAT_MASK) != (PCF_DEFAULT_FORMAT & PCF_FORMAT_MASK))
		return -EINVAL;
	bool be = fmt & PCF_BYTE_MASK;
	if (fread(&val, 4, 1, fp) != 1)
		return -EINVAL;
	auto numbitmaps = be ? be32_to_cpu(val) : le32_to_cpu(val);
	static constexpr unsigned int glypadopts = 4;
	std::vector<uint32_t> offsets(numbitmaps);
	uint32_t bmpsize[glypadopts];
	if (fread(&offsets[0], 4, numbitmaps, fp) != numbitmaps ||
	    fread(&bmpsize[0], 4, glypadopts, fp) != glypadopts)
		return -EINVAL;
	for (unsigned int i = 0; i < numbitmaps; ++i) {
		offsets[i] = be ? be32_to_cpu(offsets[i]) : le32_to_cpu(offsets[i]);
		printf("bmp %u offset %u\n",
			i, offsets[i]);
	}
	for (unsigned int i = 0; i < glypadopts; ++i) {
		bmpsize[i] = be ? be32_to_cpu(bmpsize[i]) : le32_to_cpu(bmpsize[i]);
		printf("padopt %u size %u\n", i, bmpsize[i]);
	}
	std::string bmapbuf;
	#define PCF_GLYPH_PAD_INDEX(f) ((f) & PCF_GLYPH_PAD_MASK)
	bmapbuf.resize(bmpsize[PCF_GLYPH_PAD_INDEX(fmt)]);
	fprintf(stderr, "buf %zu\n", bmapbuf.size());
	if (fread(&bmapbuf[0], bmapbuf.size(), 1, fp) != 1)
		return -EINVAL;
	return 0;
}

int font::load_pcf(const char *filename)
{
	std::unique_ptr<FILE, deleter> fp(vfopen(filename, "r"));
	if (fp == nullptr)
		return -errno;

	uint32_t val = 0;
	if (fread(&val, 4, 1, fp.get()) != 1)
		return -EINVAL;
	if (memcmp(&val, "\x01""fcp", 4) != 0)
		return -EINVAL;
	if (fread(&val, 4, 1, fp.get()) != 1)
		return -EINVAL;
	val = le32_to_cpu(val);
	std::vector<pcf_table> table(val);
	const pcf_table *bmp_table = nullptr, *prop_table = nullptr;
	for (size_t i = 0; i < val; ++i) {
		auto &t = table[i];
		if (fread(&t, sizeof(t), 1, fp.get()) != 1)
			return -EINVAL;
		t.type   = le32_to_cpu(t.type);
		t.format = le32_to_cpu(t.format);
		t.size   = le32_to_cpu(t.size);
		t.offset = le32_to_cpu(t.offset);
		if (t.type == PCF_PROPERTIES)
			prop_table = &t;
		else if (t.type == PCF_BITMAPS)
			bmp_table = &t;
		fprintf(stderr, "Table %zu: type %xh format %xh size %u offset %u\n",
			i, t.type, t.format, t.size, t.offset);
	}
	if (prop_table == nullptr || fseek(fp.get(), prop_table->offset, SEEK_SET) != 0) {
		fprintf(stderr, "pcf: no properties\n");
		return -EINVAL;
	}
	std::map<std::string, std::string> propmap;
	auto ret = load_pcf_props(fp.get(), propmap);
	if (ret != 0)
		return ret;
	//y=PIXEL_SIZE,x=QUAD_WIDTH
	if (bmp_table == nullptr || fseek(fp.get(), bmp_table->offset, SEEK_SET) != 0) {
		fprintf(stderr, "pcf: no bitmaps\n");
		return -EINVAL;
	}
	load_pcf_bitmaps(fp.get());
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

static char32_t nextucs2(FILE *fp)
{
	auto x = fgetc(fp);
	if (x == EOF)
		return ~0U;
	auto y = fgetc(fp);
	if (y == EOF)
		return ~0U;
	x |= y << 8;
	return x < 0xffff ? x : ~0U;
}

static unsigned int psf_version(FILE *fp)
{
	uint8_t x = fgetc(fp), y = fgetc(fp);
	if (x == PSF1_MAGIC0 && y == PSF1_MAGIC1)
		return 1;
	if (x != PSF2_MAGIC0 || y != PSF2_MAGIC1)
		return 0;
	x = fgetc(fp);
	y = fgetc(fp);
	return x == PSF2_MAGIC2 && y == PSF2_MAGIC3 ? 2 : 0;
}

int font::load_psf(const char *file)
{
	std::unique_ptr<FILE, deleter> fp(vfopen(file, "rb"));
	if (fp == nullptr)
		return -errno;

	struct psf2_header hdr{};
	switch (psf_version(fp.get())) {
	case 0:
		return -EINVAL;
	case 1: {
		auto mode = fgetc(fp.get()), charsize = fgetc(fp.get());
		if (mode == EOF || charsize == EOF)
			return -EINVAL;
		hdr.length   = (mode & PSF1_MF_512) ? 512 : 256;
		hdr.charsize = charsize;
		hdr.height   = charsize;
		hdr.width    = 8;
		hdr.flags   |= VFA_UCS2;
		if (mode & (PSF1_MF_TAB | PSF1_MF_SEQ))
			hdr.flags |= PSF2_HAS_UNICODE_TABLE;
		break;
	}
	case 2: {
		if (fread(&hdr.version, sizeof(hdr) - offsetof(decltype(hdr), version), 1, fp.get()) != 1 ||
		    le32_to_cpu(hdr.version) != 0)
			return -EINVAL;
		hdr.version    = le32_to_cpu(hdr.version);
		if (hdr.version != 0)
			return -EINVAL;
		hdr.headersize = le32_to_cpu(hdr.headersize);
		hdr.flags      = le32_to_cpu(hdr.flags);
		hdr.length     = le32_to_cpu(hdr.length);
		hdr.charsize   = le32_to_cpu(hdr.charsize);
		hdr.height     = le32_to_cpu(hdr.height);
		hdr.width      = le32_to_cpu(hdr.width);
		break;
	}
	}

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
	for (unsigned int idx = 0; idx < hdr.length; ++idx) {
		do {
			auto uc = hdr.flags & VFA_UCS2 ? nextucs2(fp.get()) : nextutf8(fp.get());
			if (uc == ~0U)
				break;
			m_unicode_map->add_i2u(glyph_start + idx, uc);
		} while (true);
	}
	return 0;
}

int font::save_bdf(const char *file)
{
	std::unique_ptr<FILE, deleter> filep(vfopen(file, "w"));
	if (filep == nullptr)
		return -errno;
	auto fp = filep.get();
	vfsize sz0;
	if (m_glyph.size() > 0)
		sz0 = m_glyph[0].m_size;
	std::string bfd_name = props["FullName"];
	/* X logical font description (XLFD) does not permit dashes */
	std::replace(bfd_name.begin(), bfd_name.end(), '-', ' ');
	fprintf(fp, "STARTFONT 2.1\n");
	fprintf(fp, "FONT -misc-%s-medium-r-normal--%u-%u-75-75-c-%u-iso10646-1\n",
		props["FontName"].c_str(), sz0.h, 10 * sz0.h, 10 * sz0.w);
	fprintf(fp, "SIZE %u 75 75\n", sz0.h);
	fprintf(fp, "FONTBOUNDINGBOX %u %u 0 -%u\n", sz0.w, sz0.h, sz0.h / 4);
	fprintf(fp, "STARTPROPERTIES 24\n");
	fprintf(fp, "FONT_TYPE \"Bitmap\"\n");
	fprintf(fp, "FONTNAME_REGISTRY \"\"\n");
	fprintf(fp, "FOUNDRY \"misc\"\n");
	fprintf(fp, "FAMILY_NAME \"%s\"\n", props["FamilyName"].c_str());
	fprintf(fp, "WEIGHT_NAME \"%s\"\n", props["Weight"].c_str());
	fprintf(fp, "SLANT \"r\"\n");
	fprintf(fp, "SETWIDTH_NAME \"normal\"\n");
	fprintf(fp, "PIXEL_SIZE %u\n", sz0.h);
	fprintf(fp, "POINT_SIZE %u\n", 10 * sz0.h);
	fprintf(fp, "SPACING \"C\"\n");
	fprintf(fp, "AVERAGE_WIDTH %u\n", 10 * sz0.w);
	fprintf(fp, "FONT \"%s\"\n", props["FullName"].c_str());
	fprintf(fp, "WEIGHT %s\n", props["TTFWeight"].c_str());
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
	/* sz.h/4 is just a guess as to the descent of glyphs */
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
	std::unique_ptr<FILE, deleter> fp(vfopen(outpath.c_str(), "w"));
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
	std::unique_ptr<FILE, deleter> fp(vfopen(file, "wb"));
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
	std::unique_ptr<FILE, deleter> fp(vfopen(file, "w"));
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
	std::unique_ptr<FILE, deleter> fp(vfopen(file, "wb"));
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

static unsigned int ttfweight_to_panose(const char *s)
{
	unsigned int z = strtoul(s, nullptr, 0);
	if (z >= 1 && z <= 999)
		return 1 + z / 100;
	return 6;
}

static void name_reminder(font::propmap_t &props)
{
	auto &a = props["FontName"], &b = props["FamilyName"], &c = props["FullName"];
	auto x = a.empty() || a == "vfontas-output";
	auto y = b.empty() || b == "vfontas output";
	auto z = c.empty() || c == "vfontas output";
	if (x && y && z) {
		fprintf(stderr, "Hint: Consider -setname <name>, "
		        "or the detailed version, e.g.\n"
		        "\t-setprop FontName aerial_20  # PostScript name\n"
		        "\t-setprop FamilyName \"Aerial 2.0\"\n"
		        "\t-setprop FullName \"Aerial 2.0 Bold\"\n");
		return;
	}
	if (x)
		fprintf(stderr, "Hint: Consider -setprop FontName <name>. "
		        "This is the PostScript name and "
		        "drives FontForge's default output filename. "
		        "This name should not have spaces.\n");
	if (y)
		fprintf(stderr, "Hint: Consider -setprop FamilyName <name>. "
		        "This is the name without \"Bold\", \"Italic\", etc. suffix.\n");
	if (z)
		fprintf(stderr, "Hint: Consider -setprop FullName <name>. "
		        "This is the name with \"Bold\", \"Italic\", etc. suffix.\n");
}

int font::save_sfd(const char *file, enum vectoalg vt)
{
	std::unique_ptr<FILE, deleter> filep(vfopen(file, "w"));
	if (filep == nullptr)
		return -errno;
	auto fp = filep.get();
	auto asds = find_ascent_descent();
	name_reminder(props);
	auto it = props.find("ssf");
	if (it != props.end()) {
		char *end = nullptr;
		auto a = strtoul(it->second.c_str(), &end, 0);
		if (end == nullptr || end[0] != '/') {
			fprintf(stderr, "What garbage is \"%s\"? Ignored -setprop request.\n", it->second.c_str());
		} else if (end[0] == '/') {
			auto b = strtoul(end + 1, nullptr, 0);
			if (b == 0) {
				fprintf(stderr, "What garbage is \"%s\"? Ignored -setprop request.\n", it->second.c_str());
			} else {
				m_ssfx = 2 * a;
				m_ssfy = 2 * b;
			}
		}
	}
	fprintf(fp, "SplineFontDB: 3.0\n");
	fprintf(fp, "FontName: %s\n", props["FontName"].c_str());
	fprintf(fp, "FullName: %s\n", props["FullName"].c_str());
	fprintf(fp, "FamilyName: %s\n", props["FamilyName"].c_str());
	fprintf(fp, "Weight: %s\n", props["Weight"].c_str());
	fprintf(fp, "Version: 001.000\n");
	fprintf(fp, "ItalicAngle: 0\n");
	fprintf(fp, "UnderlinePosition: -3\n");
	fprintf(fp, "UnderlineWidth: 1\n");
	fprintf(fp, "Ascent: %d\n", asds.first * m_ssfy);
	fprintf(fp, "Descent: %d\n", asds.second * m_ssfy);
	fprintf(fp, "NeedsXUIDChange: 1\n");
	fprintf(fp, "FSType: 0\n");
	fprintf(fp, "PfmFamily: 49\n");
	fprintf(fp, "TTFWeight: %s\n", props["TTFWeight"].c_str());
	fprintf(fp, "TTFWidth: 5\n");
	fprintf(fp, "Panose: 2 0 %u 9 9 0 0 0 0 0\n", ttfweight_to_panose(props["TTFWeight"].c_str()));
	fprintf(fp, "LineGap: 0\n");
	fprintf(fp, "VLineGap: 0\n");
	fprintf(fp, "OS2TypoAscent: %d\n", asds.first * m_ssfy);
	fprintf(fp, "OS2TypoAOffset: 0\n");
	fprintf(fp, "OS2TypoDescent: %d\n", -asds.second * m_ssfy);
	fprintf(fp, "OS2TypoDOffset: 0\n");
	fprintf(fp, "OS2TypoLinegap: 0\n");
	fprintf(fp, "OS2WinAscent: %d\n", asds.first * m_ssfy);
	fprintf(fp, "OS2WinAOffset: 0\n");
	fprintf(fp, "OS2WinDescent: %d\n", asds.second * m_ssfy);
	fprintf(fp, "OS2WinDOffset: 0\n");
	fprintf(fp, "HheadAscent: %d\n", asds.first * m_ssfy);
	fprintf(fp, "HheadAOffset: 0\n");
	fprintf(fp, "HheadDescent: %d\n", -asds.second * m_ssfy);
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

vectorizer::vectorizer(const glyph &g, int desc) :
	m_glyph(g), m_descent(desc)
{}

/**
 * Produce a polygon for a given pixel.
 *
 * The "polygon" association is never stored. Instead, this property
 * is implicit in the graph (emap) and a polygon is defined by the
 * smallest walk with right turns only.
 */
void vectorizer::set(int x, int y)
{
	/* TTF/OTF spec: right side of line to be interior */
	const int &sx = scale_factor_x, &sy = scale_factor_y;
	x *= sx;
	y *= sy;
	emap.insert(edge{{y, x}, {y + sy, x}});
	emap.insert(edge{{y + sy, x}, {y + sy, x + sx}});
	emap.insert(edge{{y + sy, x + sx}, {y, x + sx}});
	emap.insert(edge{{y, x + sx}, {y, x}});
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
	 * Remove overlaps: As enforced by set(), all the abstract polygons are
	 * added with the same orientation. Polygons at most touch, and never
	 * overlap. Joining these abstract polygons simply requires removing
	 * shared contradirectional edges. It follows by induction that the
	 * intrinsic property {{smallest walk with right turns only} forms a
	 * closed polygon} is kept.
	 *
	 * *--->**--->**--->*      *--->*--->**--->*
	 * ^    |^    |^    |  =>  ^         |^    |
	 * |    v|    v|    v      |         v|    v
	 * *<---**<---**<---*      *<---*<---**<---*
	 *
	 * As the edges were never reoriented, polygons also retain their
	 * orientation. In other words, after this edge removal, the remaining
	 * set of edges forms a new set of abstract polygons.
	 */
	for (auto edge = emap.begin(); edge != emap.end(); ) {
		auto twin = emap.find({edge->end_vtx, edge->start_vtx});
		if (twin == emap.cend()) {
			++edge;
			continue;
		} else if (twin == edge) {
			printf("Glyph outline description is faulty: edge with startvtx==endvtx (%d,%d)\n",
				edge->start_vtx.x, edge->start_vtx.y);
			break;
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
    const edge &cur_edge, unsigned int flags) const
{
	const auto &tail = cur_edge.end_vtx;
	std::set<edge>::iterator inward, outward;
	auto ret = neigh_edges(cur_dir, tail, inward, outward);
	if (!(flags & P_ISTHMUS) || ret <= 1)
		return inward;
	/*
	 * If there are two edges with the same vertex, we have
	 * an intersection ahead (illustrative):
	 *
	 *   ##..##..  ..##..##  ####....  ....####
	 *   ..##....  ....##..  ####..##  ##..####
	 *   ##..####  ####..##  ....##..  ..##....
	 *   ....####  ####....  ..##..##  ##..##..
	 *
	 * n2_angle will work with the polygon edge we determine here, so the
	 * choice of walking direction matters.
	 *
	 * We are working with lines rather than pixels, but every edge's right
	 * side corresponds to a pixel, thereby the bitmap could be
	 * reconstructed. But since we have a reference to the bitmap anyway,
	 * it can just be checked directly.
	 *
	 * Antijoinworthy patterns:
	 *   (A1)      (A2)
	 *   ....##..  ..##....
	 *   ..##....  ####....
	 *   ##..####  ....####
	 *   ....####  ....####
	 * Joinworthy:
	 *   (J1)
	 *   ..MM....
	 *   ..MM....
	 *   ....####
	 *   ....####
	 *
	 * Right now, we are only testing for A1+A2. Might be enough...?
	 */
	vertex bmp;
	if (cur_dir == 0)
		bmp = cur_edge.start_vtx;
	else if (cur_dir == 90)
		bmp = {cur_edge.start_vtx.x, cur_edge.start_vtx.y - scale_factor_y};
	else if (cur_dir == 180)
		bmp = {cur_edge.start_vtx.x - scale_factor_x, cur_edge.end_vtx.y};
	else if (cur_dir == 270)
		bmp = cur_edge.end_vtx;
	else
		bmp = {};
	bmp.x /= scale_factor_x;
	bmp.y /= scale_factor_y;
	bmp.y = m_glyph.m_size.h - bmp.y - m_descent - 1;

	/* Test for pattern A1 */
	bool up    = testbit_c(m_glyph, bmp.x, bmp.y - 2);
	bool right = testbit_c(m_glyph, bmp.x + 2, bmp.y);
	bool down  = testbit_c(m_glyph, bmp.x, bmp.y + 2);
	bool left  = testbit_c(m_glyph, bmp.x - 2, bmp.y);
	if (cur_dir == 0 && left && up)
		return inward;
	if (cur_dir == 90 && up && right)
		return inward;
	if (cur_dir == 180 && right && down)
		return inward;
	if (cur_dir == 270 && down && left)
		return inward;

	/* Test for pattern A2 */
	if (cur_dir == 0 && testbit_c(m_glyph, bmp.x - 2, bmp.y - 1) && testbit_c(m_glyph, bmp.x - 1, bmp.y - 2))
		return inward;
	if (cur_dir == 90 && testbit_c(m_glyph, bmp.x + 1, bmp.y - 2) && testbit_c(m_glyph, bmp.x + 2, bmp.y - 1))
		return inward;
	if (cur_dir == 180 && testbit_c(m_glyph, bmp.x + 2, bmp.y + 1) && testbit_c(m_glyph, bmp.x + 1, bmp.y + 2))
		return inward;
	if (cur_dir == 270 && testbit_c(m_glyph, bmp.x - 2, bmp.y + 1) && testbit_c(m_glyph, bmp.x - 1, bmp.y + 2))
		return inward;

	return outward;
}

/**
 * Extract one polygon from the graph.
 *
 * The vectorizer class only keeps a loose set of edge descriptions, but all
 * these edges form valid closed polygons (cf. vectorizer::set,
 * vectorizer::internal_edge_removal). Thus, by starting a walk at an arbitrary
 * edge and following the path with "right turns only" until we see the same
 * edge again, that will be our polygon.
 */
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
		auto next = next_edge(prev_dir, *poly.rbegin(), flags);
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
				emap.insert(edge{{2*y+1, 2*x},   {2*y+2, 2*x}});
				emap.insert(edge{{2*y+2, 2*x},   {2*y+2, 2*x+1}});
				emap.insert(edge{{2*y+2, 2*x+1}, {2*y+1, 2*x}});
			}
			if (tr) {
				emap.insert(edge{{2*y+2, 2*x+1}, {2*y+2, 2*x+2}});
				emap.insert(edge{{2*y+2, 2*x+2}, {2*y+1, 2*x+2}});
				emap.insert(edge{{2*y+1, 2*x+2}, {2*y+2, 2*x+1}});
			}
			if (bl) {
				emap.insert(edge{{2*y,   2*x},   {2*y+1, 2*x}});
				emap.insert(edge{{2*y+1, 2*x},   {2*y,   2*x+1}});
				emap.insert(edge{{2*y,   2*x+1}, {2*y,   2*x}});
			}
			if (br) {
				emap.insert(edge{{2*y,   2*x+1}, {2*y+1, 2*x+2}});
				emap.insert(edge{{2*y+1, 2*x+2}, {2*y,   2*x+2}});
				emap.insert(edge{{2*y,   2*x+2}, {2*y,   2*x+1}});
			}
			if (di) {
				emap.insert(edge{{2*y+1, 2*x},   {2*y+2, 2*x+1}});
				emap.insert(edge{{2*y+2, 2*x+1}, {2*y+1, 2*x+2}});
				emap.insert(edge{{2*y+1, 2*x+2}, {2*y,   2*x+1}});
				emap.insert(edge{{2*y,   2*x+1}, {2*y+1, 2*x}});
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

static void n2_angle(std::vector<edge> &poly, unsigned int sx, unsigned int sy)
{
	static const unsigned int M_HEAD = 0x20, M_TAIL = 0x02,
		M_XHEAD = 0x10, M_XTAIL = 0x01;
	std::vector<unsigned int> flags(poly.size());

	/*
	 * It's a closed polygon and so it does not matter which edge
	 * processing starts at. (xm3 = x minus 3, x00 = current edge, xp3 = x
	 * plus 3, etc.)
	 *
	 * In the loop, edges are marked with bitflags M_HEAD/M_TAIL to
	 * indicate that a particular edge allows modification of the start or
	 * end vertex (or both).
	 *
	 * M_XHEAD/M_XTAIL are used as veto flags. (We cannot just use e.g.
	 * `flags[xm3] & ~M_XHEAD` in one iteration, because a subsequent
	 * iteration may set it again via e.g. `flags[xm2] |= M_HEAD`.)
	 */
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

		/* #5: serif (ramp), topleft of ibmvga916 'E' */
		if (dm2 == dm1 && d00 == (dm1 + 270) % 360 &&
		    dp1 == dm1 && dp2 == (dm1 + 90) % 360 && dp3 == dp2)
			continue;
		/* bottomleft of ibmvga916 'E' */
		if (dm3 == dm2 && dm1 == (dm2 + 90) % 360 &&
		    d00 == (dm2 + 180) % 360 && dp1 == dm1 && dp2 == dp1)
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

		/* Shift nodal points. This actually creates the diagonal visuals. */
		auto da = poly[ia].trivial_dir(), db = poly[ib].trivial_dir();
		if (da == 0)
			poly[ia].end_vtx.y -= sy;
		else if (da == 90)
			poly[ia].end_vtx.x -= sx;
		else if (da == 180)
			poly[ia].end_vtx.y += sy;
		else if (da == 270)
			poly[ia].end_vtx.x += sx;
		if (db == 0)
			poly[ib].start_vtx.y += sy;
		else if (db == 90)
			poly[ib].start_vtx.x += sx;
		else if (db == 180)
			poly[ib].start_vtx.y -= sy;
		else if (db == 270)
			poly[ib].start_vtx.x -= sx;
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

std::vector<std::vector<edge>> vectorizer::n2(unsigned int flags)
{
	flags &= P_ISTHMUS;
	make_squares();
	internal_edge_delete();
	std::vector<std::vector<edge>> pmap;
	while (true) {
		/* Have all edges retain length 1 */
		auto poly = pop_poly(flags);
		if (poly.size() == 0)
			break;
		n2_angle(poly, scale_factor_x / 2, scale_factor_y / 2);
		pmap.push_back(std::move(poly));
	}
	return pmap;
}

void font::save_sfd_glyph(FILE *fp, size_t idx, char32_t cp, int asc, int desc,
    enum vectoalg vt)
{
	unsigned int cpx = cp;
	if (idx >= m_glyph.size())
		return;
	const auto &g = m_glyph[idx];
	const auto &sz = g.m_size;
	fprintf(fp, "StartChar: %04x\n", cpx);
	fprintf(fp, "Encoding: %u %u %u\n", cpx, cpx, cpx);
	fprintf(fp, "Width: %u\n", sz.w * m_ssfx);
	fprintf(fp, "Flags: MW\n");
	fprintf(fp, "Fore\n");
	fprintf(fp, "SplineSet\n");

	std::vector<std::vector<edge>> pmap;
	vectorizer vct(m_glyph[idx], desc);
	vct.scale_factor_x = m_ssfx;
	vct.scale_factor_y = m_ssfy;
	if (vt == V_SIMPLE)
		pmap = vct.simple();
	else if (vt == V_N1)
		pmap = vct.n1();
	else if (vt == V_N2)
		pmap = vct.n2();
	else if (vt == V_N2EV)
		pmap = vct.n2(vectorizer::P_ISTHMUS);
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

/*
 * Create the in-memory representation (which is bitpacked) from a bytepacked
 * ("right-padded") raw representation.
 */
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

glyph glyph::copy_rect_to(const vfrect &sof, const glyph &other,
    const vfrect &pof, bool overwrite) const
{
	glyph out = other;

	for (unsigned int y = sof.y; y < sof.y + sof.h && y < m_size.h; ++y) {
		for (unsigned int x = sof.x; x < sof.x + sof.w && x < m_size.w; ++x) {
			int ox = pof.x + x - sof.x;
			int oy = pof.y + y - sof.y;
			if (ox < 0 || oy < 0 || static_cast<unsigned int>(ox) >= pof.w ||
			    static_cast<unsigned int>(oy) >= pof.h)
				continue;
			bitpos ipos = y * m_size.w + x;
			bitpos opos = oy * out.m_size.w + ox;
			if (m_data[ipos.byte] & ipos.mask)
				out.m_data[opos.byte] |= opos.mask;
			else if (overwrite)
				out.m_data[opos.byte] &= ~opos.mask;
		}
	}
	return out;
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

void glyph::lge(unsigned int adj)
{
	if (m_size.w < adj + 1)
		return;
	for (unsigned int y = 0; y < m_size.h; ++y) {
		bitpos ipos = (y + 1) * m_size.w - 1 - adj;
		bitpos opos = (y + 1) * m_size.w - 1;
		if (m_data[ipos.byte] & ipos.mask)
			m_data[opos.byte] |= opos.mask;
		else
			m_data[opos.byte] &= ~opos.mask;
	}
}

glyph glyph::overstrike(unsigned int px) const
{
	glyph composite(m_size);
	for (unsigned int x = 0; x <= px; ++x)
		composite = copy_rect_to(vfpos(0, 0) | m_size, composite,
		            vfpos(x, 0) | m_size, false);
	return composite;
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

/**
 * Convert from bit-packed representation to row-padded.
 */
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
	return std::tie(y, x) < std::tie(o.y, o.x);
}

bool vertex::operator==(const struct vertex &o) const
{
	return std::tie(y, x) == std::tie(o.y, o.x);
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
