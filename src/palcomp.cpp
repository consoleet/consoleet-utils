// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2022–2025 Jan Engelhardt
#include <algorithm>
#include <functional>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>
#include <babl/babl.h>
#include <Eigen/LU>
#include <libHX/ctype_helper.h>
#include <libHX/misc.h>
#include <libHX/option.h>

namespace {
struct srgb888 { uint8_t r = 0, g = 0, b = 0; };
struct srgb { double r = 0, g = 0, b = 0; };
struct lrgb { double r = 0, g = 0, b = 0; };
struct xy0 { double x = 0, y = 0; }; // CIE1391 xyY colorspace (but without Y) [chromaticity plane]
struct xyz { double x = 0, y = 0, z = 0; }; // CIE1391 XYZ colorspace [tristimulus]
struct lch { double l = 0, c = 0, h = 0; };
struct hsl { double h = 0, s = 0, l = 0; };

/***
 * Keep multiple numeric representations of the palette, to reduce accumulation
 * of conversion errors.
 */
struct mpalette {
	std::vector<srgb888> ra;
	std::vector<lch> la;
	double x = 0, y = 0, z = 0;

	void mod_la();
	void mod_ra();
};

/**
 * Statistics for one grid view (e.g. 8x8 / 16x8 / ...).
 *
 * @pairs:      pairs that have contributed to @sum
 * @penalized:  number of penalized pairs
 * @sum:        sum of deltas
 * @avg:        @sum divided by @pairs
 * @adj_sum:    @sum adjusted for penalized pairs
 * @adj_avg:    adjusted average
 */
struct gvstat {
	unsigned int pairs = 0, penalized = 0;
	double sum = 0, avg = 0, adj_sum = 0, adj_avg = 0;
};

struct palstat {
	public:
	bool (*penalize)(double) = nullptr;
	double delta[16][16]{};
	gvstat x1616{}, x816{}, x88{};

	void compute_sums();

	protected:
	void compute_sums(unsigned int xlim, unsigned int ylim, gvstat &);
};

enum class token_type { none, reg, imm, grp, op };
struct token_entry;
using token_value = std::variant<char, double, std::vector<token_entry>>;
struct token_entry {
	token_type type = token_type::none;
	token_value val{};
	std::string repr() const;
};
using token_vector = std::vector<token_entry>;

}

static constexpr srgb888 vga_palette[] = {
	{0x00,0x00,0x00}, {0xaa,0x00,0x00}, {0x00,0xaa,0x00}, {0xaa,0x55,0x00},
	{0x00,0x00,0xaa}, {0xaa,0x00,0xaa}, {0x00,0xaa,0xaa}, {0xaa,0xaa,0xaa},
	{0x55,0x55,0x55}, {0xff,0x55,0x55}, {0x55,0xff,0x55}, {0xff,0xff,0x55},
	{0x55,0x55,0xff}, {0xff,0x55,0xff}, {0x55,0xff,0xff}, {0xff,0xff,0xff},
};
static constexpr srgb888 vgasat_palette[] = {
	{0x00,0x00,0x00}, {0xaa,0x00,0x00}, {0x00,0xaa,0x00}, {0xaa,0x55,0x00},
	{0x00,0x00,0xaa}, {0xaa,0x00,0xaa}, {0x00,0xaa,0xaa}, {0xaa,0xaa,0xaa},
	{0x55,0x55,0x55}, {0xff,0x00,0x00}, {0x00,0xff,0x00}, {0xff,0xff,0x00},
	{0x00,0x00,0xff}, {0xff,0x00,0xff}, {0x00,0xff,0xff}, {0xff,0xff,0xff},
};
static constexpr srgb888 win_palette[] = {
	{0x00,0x00,0x00}, {0x80,0x00,0x00}, {0x00,0x80,0x00}, {0x80,0x80,0x00},
	{0x00,0x00,0x80}, {0x80,0x00,0x80}, {0x00,0x80,0x80}, {0xc0,0xc0,0xc0},
	{0x80,0x80,0x80}, {0xff,0x00,0x00}, {0x00,0xff,0x00}, {0xff,0xff,0x00},
	{0x00,0x00,0xff}, {0xff,0x00,0xff}, {0x00,0xff,0xff}, {0xff,0xff,0xff},
};

static unsigned int xterm_fg, xterm_bg, xterm_bd, g_verbose = 1;
static double g_continuous_gamma;
static const Babl *lch_space, *srgb_space, *srgb888_space;
static Eigen::Matrix3d xyz_to_lrgb_matrix;

static constexpr HXoption g_options_table[] = {
	{{}, 'q', HXTYPE_NONE | HXOPT_DEC, &g_verbose, {}, {}, {}, "Reduce noise"},
	{{}, 'v', HXTYPE_NONE | HXOPT_INC, &g_verbose, {}, {}, {}, "Debugging"},
	HXOPT_AUTOHELP,
	HXOPT_TABLEEND,
};

static uint8_t fromhex(const char *s)
{
	auto a = tolower(s[0]), b = tolower(s[1]);
	unsigned int v = 0;
	if (a >= '0' && a <= '9')
		v += (a - '0') << 4;
	else if (a >= 'a' && a <= 'f')
		v += (a - 'a' + 10) << 4;
	if (b >= '0' && b <= '9')
		v += b - '0';
	else if (b >= 'a' && b <= 'f')
		v += b - 'a' + 10;
	return v;
}

static int hexcolor_split(const char *p, srgb888 &o)
{
	bool zaun = *p == '#';
	if (zaun)
		++p;
	if (!HX_isxdigit(p[0]) || !HX_isxdigit(p[1]) ||
	    !HX_isxdigit(p[2]) || !HX_isxdigit(p[3]) ||
	    !HX_isxdigit(p[4]) || !HX_isxdigit(p[5]))
		return -1;
	o.r = fromhex(&p[0]);
	o.g = fromhex(&p[2]);
	o.b = fromhex(&p[4]);
	return 6 + zaun;
}

static hsl to_hsl(const srgb &i)
{
	hsl c;
	double vmin = std::min({i.r, i.g, i.b}), vmax = std::max({i.r, i.g, i.b});
	c.l = (vmin + vmax) / 2;
	if (vmax == vmin)
		return c;
	auto d = vmax - vmin;
	c.s = c.l > 0.5 ? d / (2 - vmax - vmin) : d / (vmax + vmin);
	if (vmax == i.r) c.h = (i.g - i.b) / d + (i.g < i.b ? 6 : 0);
	if (vmax == i.g) c.h = (i.b - i.r) / d + 2;
	if (vmax == i.b) c.h = (i.r - i.g) / d + 4;
	c.h *= 60;
	return c;
}

static std::string to_hex(const srgb888 &e)
{
	char t[8];
	snprintf(t, std::size(t), "#%02x%02x%02x", e.r, e.g, e.b);
	return t;
}

static srgb888 to_srgb888(const srgb &e)
{
	auto r = std::max(std::min(round(e.r * 255.0), 255.0), 0.0);
	auto g = std::max(std::min(round(e.g * 255.0), 255.0), 0.0);
	auto b = std::max(std::min(round(e.b * 255.0), 255.0), 0.0);
	return {static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)};
}

static double huetorgb(double p, double q, double t)
{
	if (t < 0)
		t += 360;
	if (t > 360)
		t -= 360;
	if (t < 60)
		return p + (q - p) * t / 60;
	if (t < 180)
		return q;
	if (t < 240)
		return p + (q - p) * (4 - t / 60);
	return p;
}

static srgb to_srgb(const hsl &in)
{
	if (in.s <= 0.0)
		return {in.l, in.l, in.l};
	auto q = in.l < 0.5 ? in.l * (1 + in.s) : in.l + in.s - in.l * in.s;
	auto p = 2 * in.l - q;
	return {huetorgb(p, q, in.h + 120),
		huetorgb(p, q, in.h), huetorgb(p, q, in.h - 120)};
}

static srgb to_srgb(const srgb888 &e)
{
	return {e.r / 255.0, e.g / 255.0, e.b / 255.0};
}

static hsl parse_hsl(const char *str)
{
	hsl c;
	if (*str != '#') {
		double h, s, l;
		if (sscanf(str, "%lf,%lf,%lf", &h, &s, &l) != 3) {
			fprintf(stderr, "Illegal HSL input: \"%s\"\n", str);
			return c;
		}
		c = {h, s, l};
		return c;
	}
	srgb888 r;
	if (hexcolor_split(str, r) < 0) {
		fprintf(stderr, "Illegal RGB(,L) value: \"%s\"\n", str);
		return c;
	}
	c = to_hsl(to_srgb(std::move(r)));
	str += 7;
	if (*str == ',')
		c.l = strtod(&str[1], nullptr);
	return c;
}

static srgb888 to_srgb888(const lch &i)
{
	srgb888 o{};
	babl_process(babl_fish(lch_space, srgb888_space), &i, &o, 1);
	return o;
}

static lch to_lch(const srgb888 &i)
{
	lch o{};
	babl_process(babl_fish(srgb888_space, lch_space), &i, &o, 1);
	return o;
}

static lch to_lch(const srgb &i)
{
	lch o{};
	babl_process(babl_fish(srgb_space, lch_space), &i, &o, 1);
	return o;
}

static std::vector<lch> to_lch(const std::vector<srgb888> &in)
{
	std::vector<lch> out;
	for (const auto &color : in)
		out.push_back(to_lch(color));
	return out;
}

static std::vector<srgb888> to_srgb888(const std::vector<lch> &in)
{
	std::vector<srgb888> out;
	for (const auto &color : in)
		out.push_back(to_srgb888(color));
	return out;
}

static void emit_xfce(const std::vector<srgb888> &pal)
{
	printf("ColorPalette=");
	for (const auto &e : pal)
		printf("%s;", to_hex(e).c_str());
	printf("\n");
}

void mpalette::mod_la() { ra = to_srgb888(la); }
void mpalette::mod_ra() { la = to_lch(ra); }

static void emit_xterm(const std::vector<srgb888> &pal)
{
	for (unsigned int idx = 0; idx < 16; ++idx)
		printf(" -xrm *VT100*color%u:%s", idx, to_hex(pal[idx]).c_str());
	if (xterm_fg)
		printf(" -fg %s", to_hex(pal[7]).c_str());
	if (xterm_bg)
		printf(" -bg %s", to_hex(pal[0]).c_str());
	if (xterm_bd)
		printf(" -xrm *VT100*colorBD:%s", to_hex(pal[15]).c_str());
	printf("\n");
}

static std::vector<srgb888> hsltint(const hsl &base, const std::vector<lch> &light)
{
	std::vector<srgb888> out;
	for (const auto &e : light) {
		auto color = base;
		color.l *= e.l / 100.0;
		out.push_back(to_srgb888(to_srgb(color)));
	}
	return out;
}

static std::vector<lch> lchtint(const lch &base, const std::vector<lch> &light)
{
	std::vector<lch> out;
	for (const auto &e : light)
		out.push_back(lch{e.l, base.c, base.h});
	return out;
}

static void colortable_256()
{
	char v = g_verbose >= 1 ? '.' : ' ';
	for (unsigned int b = 0; b < 256; b += 32) {
		for (unsigned int g = 0; g < 256; g += 32) {
			for (unsigned int r = 0; r < 256; r += 16)
				printf("\e[30;48;2;%u;%u;%um%c", r, g, b, v);
			printf("\e[0m\n");
		}
	}
	for (unsigned int c = 0x0; c <= 0xFF; ++c) {
		if (g_verbose >= 1)
			printf("\e[30;48;5;%um-%02x-", c, c);
		else
			printf("\e[30;48;5;%um  ", c);
		if ((c - 3) % 6 == 0)
			printf("\e[0m\n");
	}
}

static void colortable_16(std::function<void(int, int, int)> pr = nullptr)
{
	std::vector<int> modes = {0, 90};
	if (pr == nullptr) {
		printf("                  ┌─ bright ───────┐┌─ bold ─────────┐┌─ reverse ──────┐\n");
		pr = [](int bg, int fg, int sp) {
			if (g_verbose == 0)
				printf("  ");
			else
				printf("%x%c", bg >= 0 ? bg : 0, fg < 10 ? '0' + fg : 'a' + fg - 10);
		};
		modes = {0, 90, 1, 7};
	}

	for (int bg = -1; bg < 16; ++bg) {
		for (auto mode : modes) {
			for (int fg = 0; fg <= 9; ++fg) {
				if (fg == 8)
					continue;
				int report_fg = fg, report_bg = bg;
				std::string emit_str = "\e[";
				if (mode == 0) {
					emit_str += "0;" + std::to_string(30 + fg);
				} else if (mode == 1) {
					emit_str += "0;1;" + std::to_string(30 + fg);
					report_fg += 16;
				} else if (mode == 7) {
					emit_str += "0;7;" + std::to_string(30 + fg);
					report_bg ^= 0x8;
				} else if (mode == 90) {
					emit_str += "0;" + std::to_string(90 + fg);
					report_fg += 8;
				}
				if (fg == 9)
					report_fg = 9;
				if (bg >= 8)
					emit_str += ";" + std::to_string(100 + bg - 8);
				else if (bg >= 0)
					emit_str += ";" + std::to_string(40 + bg);
				emit_str += "m";
				printf("%s", emit_str.c_str());
				auto sp = bg == -1 || fg == 9 || mode == 7;
				pr(report_bg, report_fg, sp);
			}
		}
		printf("\e[0m\n");
	}
	printf("\e[0mdefault \e[37mgray \e[0;1mbold\e[0m \e[2mdim\e[0m "
	       "\e[3mitalic\e[0m \e[4munderscore\e[0m \e[5mblink\e[0m "
	       "\e[6mrapidblink\e[0m \e[7mreverse\e[0m "
	       "\e[8mhidden\e[0m \e[9mstrikethrough\e[0m\n");
}

void palstat::compute_sums(unsigned int xlim, unsigned int ylim, gvstat &gs)
{
	gs.pairs = gs.penalized = 0;
	gs.sum = gs.avg = 0;
	for (unsigned int y = 0; y < ylim; ++y) {
		for (unsigned int x = 0; x < xlim; ++x) {
			if (x == y)
				continue;
			++gs.pairs;
			gs.sum += delta[y][x];
			if (penalize != nullptr && penalize(delta[y][x]))
				++gs.penalized;
			else
				gs.adj_sum += delta[y][x];
		}
	}
	gs.avg = gs.pairs > 0 ? gs.sum / gs.pairs : 0;
	gs.adj_avg = gs.pairs - gs.penalized > 0 ? gs.adj_sum / (gs.pairs - gs.penalized) : 0;
}

void palstat::compute_sums()
{
	compute_sums(16, 16, x1616);
	compute_sums(8, 16, x816);
	compute_sums(8, 8, x88);
}

static palstat cxl_compute(const std::vector<lch> &pal)
{
	palstat o;
	o.penalize = [](double x) { return x < 7.0; };
	for (unsigned int bg = 0; bg < 16; ++bg)
		for (unsigned int fg = 0; fg < 16; ++fg)
			o.delta[bg][fg] = fabs(pal[fg].l - pal[bg].l);
	o.compute_sums();
	return o;
}

static double gamma_expand(double c)
{
	if (g_continuous_gamma != 0)
		return pow(c, g_continuous_gamma);
	/*
	 * To avoid zero slope, part of the range gets a linear mapping /
	 * gamma of 1.0.
	 */
	if (c <= 0.04045)
		return c / 12.92;
	/*
	 * The rest of the curve is a 2.4 gamma (instead of 2.2) to compensate
	 * for the prior linear section. The 2.4 curve approximates the 2.2
	 * curve in the input value range that is of interest.
	 */
	return std::min(1.0, pow((c + 0.055) / 1.055, 12 / 5.0));
}

static double gamma_compress(double c)
{
	return c <= (0.04045 / 12.92) ? c * 12.92 :
	       pow(c, 5 / 12.0) * 1.055 - 0.055;
}

/* This function only makes sense for white */
static constexpr xyz to_xyz(const xy0 &e)
{
	return {e.x / e.y, 1, (1 - e.x - e.y) / e.y};
}

/**
 * Cf. https://en.wikipedia.org/wiki/Standard_illuminant#Computation
 *
 * @t: black-body temperature in Kelvin (e.g. 5000, 5500, 6500)
 */
static constexpr xy0 illuminant_d(double t)
{
	double x = t <= 7000 ?
	           0.244063 + 0.09911 * 1000 / t + 2.9678 * 1000000 / (t * t) -
	           4.6070 * 1000000000 / (t * t * t) :
	           0.237040 + 0.24748 * 1000 / t + 1.9018 * 1000000 / (t * t) -
	           2.0064 * 1000000000 / (t * t * t);
	return {x, -3.0 * x * x + 2.87 * x - 0.275};
}

static lrgb to_lrgb(const srgb &e)
{
	return {gamma_expand(e.r), gamma_expand(e.g), gamma_expand(e.b)};
}

static double trivial_lightness(const lrgb &k)
{
	const auto &dm = xyz_to_lrgb_matrix;
	return dm(1, 0) * k.r + dm(1, 1) * k.g + dm(1, 2) * k.b;
}

static Eigen::Matrix3d make_lrgb_matrix(const xyz &white_raw)
{
	/* https://mina86.com/2019/srgb-xyz-matrix/ */
	static constexpr xy0 red = {0.64, 0.33}, green = {0.30, 0.60}, blue = {0.15, 0.06};
	const Eigen::Matrix3d M_prime{
		{red.x / red.y, green.x / green.y, blue.x / blue.y},
		{1, 1, 1},
		{(1 - red.x - red.y) / red.y, (1 - green.x - green.y) / green.y, (1 - blue.x - blue.y) / blue.y},
	};
	const Eigen::Vector3d W{white_raw.x, white_raw.y, white_raw.z};
	return M_prime * (M_prime.inverse() * W).asDiagonal();
}

static constexpr struct {
	double normbg = 0.56, normtxt = 0.57, revtxt = 0.62, revbg = 0.65,
		black_thresh = 0.022, black_clamp = 1.414,
		scale_bow = 1.14, scale_wob = 1.14,
		lo_offset = 0.027,
		delta_y_min = 0.0005, lo_clip = 0.1;
} sa_param; /* SAPC/APCA ver 0.0.98G */

static double apca_contrast(double ytx, double ybg)
{
	if (ytx <= sa_param.black_thresh)
		ytx += pow(sa_param.black_thresh - ytx, sa_param.black_clamp);
	if (ybg <= sa_param.black_thresh)
		ybg += pow(sa_param.black_thresh - ybg, sa_param.black_clamp);
	if (fabs(ybg - ytx) < sa_param.delta_y_min)
		return 0;
	double oc;
	/* SAPC = S-LUV Advanced Predictive Colour */
	if (ybg > ytx) {
		auto sapc = (pow(ybg, sa_param.normbg) - pow(ytx, sa_param.normtxt)) * sa_param.scale_bow;
		oc = std::max(sapc - sa_param.lo_offset, 0.0);
	} else {
		auto sapc = (pow(ybg, sa_param.revbg) - pow(ytx, sa_param.revtxt)) * sa_param.scale_wob;
		oc = std::min(sapc + sa_param.lo_offset, 0.0);
	}
	return 100 * fabs(oc);
}

static palstat cxa_compute(const std::vector<srgb888> &pal)
{
	/* APCA W3 contrast calculation */
	/* History: https://github.com/w3c/wcag/issues/695 */
	/* Implementation: https://git.apcacontrast.com/documentation/README */
	palstat o;
	o.penalize = [](double d) { return d < 7.3; };
	std::vector<double> ell(pal.size());
	for (unsigned int i = 0; i < pal.size(); ++i)
		ell[i] = trivial_lightness(to_lrgb(to_srgb(pal[i])));
	for (unsigned int bg = 0; bg < 16; ++bg)
		for (unsigned int fg = 0; fg < 16; ++fg)
			o.delta[bg][fg] = apca_contrast(ell[fg], ell[bg]);
	o.compute_sums();
	return o;
}

static void cx_report(const gvstat &o, const char *desc)
{
	printf("[%-5s] contrast Σ %.0f", desc, o.sum);
	printf(" // minus %u penalties:\tΣ %.0f\n",
		o.penalized, o.adj_sum);
}

static void cx_report(const palstat &o)
{
	cx_report(o.x1616, "16x16");
	cx_report(o.x816, "16x8 ");
	cx_report(o.x88, " 8x8 ");
}

static void cxl_command(const std::vector<lch> &lch_pal) try
{
	if (lch_pal.size() < 16) {
		fprintf(stderr, "cxl_compute: LCh palette must have 16 entries\n");
		return;
	}
	auto sb = cxl_compute(lch_pal);
	printf("\e[1m════ Difference of the L components ════\e[0m\n");
	colortable_16([&](int bg, int fg, int special) {
		if (special || fg >= 16 || bg >= 16 || fg == bg)
			printf("   ");
		else
			printf("%3.0f", sb.delta[bg][fg]);
	});
	cx_report(sb);
} catch (int) {
}

static void cxa_command(const std::vector<srgb888> &pal)
{
	if (pal.size() < 16) {
		fprintf(stderr, "cxl_compute: RGB palette must have 16 entries\n");
		return;
	}
	printf("\e[1m════ APCA lightness contrast ════\e[0m\n");
	auto sb = cxa_compute(pal);
	colortable_16([&](int bg, int fg, int special) {
		if (special || fg >= 16 || bg >= 16 || fg == bg) {
			printf("    ");
			return;
		}
		printf("%3.0f ", sb.delta[bg][fg]);
	});
	cx_report(sb);
}

static std::vector<lch> equalize(std::vector<lch> la, unsigned int sbl_size,
    double blue, double gray)
{
	std::vector<unsigned int> sbl(sbl_size);
	for (unsigned int idx = 0; idx < sbl.size(); ++idx)
		sbl[idx] = idx;
	std::sort(sbl.begin(), sbl.end(),
		[&](unsigned int x, unsigned int y) { return la[x].l < la[y].l; });

	if (g_verbose >= 2) {
		fprintf(stderr, "equalize(%zu) in: ", sbl.size());
		for (auto z : sbl)
			fprintf(stderr, "%f(\e[%u;3%um%x\e[0m) ", la[z].l, !!(z & 0x8), z & 0x7, z);
		fprintf(stderr, "\nequalize out: ");
	}
	for (unsigned int idx = 1; idx < sbl.size(); ++idx) {
		unsigned int z = sbl[idx];
		la[z].l = (gray - blue) * (idx - 1) / (sbl.size() - 2) + blue + la[sbl[0]].l;
		if (g_verbose >= 2)
			fprintf(stderr, "%f(\e[%u;3%um%x\e[0m) ", la[z].l, !!(z & 0x8), z & 0x7, z);
	}
	if (g_verbose >= 2)
		fprintf(stderr, "\n");
	return la;
}

template<typename T> static inline void advspace(T *&p)
{
	while (HX_isspace(*p))
		++p;
}

static int loadpal_xf4(const char *p, std::vector<srgb888> &ra)
{
	auto orig = p;
	for (unsigned int n = 0; n < ra.size(); ++n) {
		advspace(p);
		if (*p == '\0')
			break;
		auto len = hexcolor_split(p, ra[n]);
		if (len < 0) {
			fprintf(stderr, "Error in ColorPalette=\"%s\" line near \"%s\"\n", orig, p);
			return EXIT_FAILURE;
		}
		p += len;
		if (*p == ';')
			++p;
	}
	return EXIT_SUCCESS;
}

static int loadpal_sc(const char *frag, std::vector<srgb888> &ra)
{
	char *p = nullptr;
	unsigned int n = strtoul(frag, &p, 0);
	if (n >= 16)
		return 0;
	advspace(p);
	if (*p != '=' && *p != ':')
		return -EINVAL;
	++p;
	advspace(p);
	return hexcolor_split(p, ra[n]);
}

static int loadpal(const char *file, std::vector<srgb888> &ra)
{
	std::ifstream strm;
	strm.open(file != nullptr ? file : "");
	if (!strm.is_open()) {
		fprintf(stderr, "Could not load %s: %s\n", file, strerror(errno));
		return EXIT_FAILURE;
	}
	ra = std::vector<srgb888>(16);
	std::string line;
	while (std::getline(strm, line)) {
		if (strncasecmp(line.c_str(), "ColorPalette=", 13) == 0)
			loadpal_xf4(&line[13], ra);
		else if (strncasecmp(line.c_str(), "color", 5) == 0)
			loadpal_sc(&line[5], ra);
	}
	return EXIT_SUCCESS;
}

template<typename T> T do_blend(const T &a, double amult, const T &b, double bmult)
{
	auto max = std::max(a.size(), b.size());
	T out(max);
	for (size_t x = 0; x < max; ++x) {
		out[x].r = a[x].r * amult + b[x].r * bmult;
		out[x].g = a[x].g * amult + b[x].g * bmult;
		out[x].b = a[x].b * amult + b[x].b * bmult;
	}
	return out;
}

std::string repr(const token_vector &tokens)
{
	std::string out = "(";
	for (const auto &e : tokens)
		out += e.repr();
	return out += ")";
}

std::string token_entry::repr() const
{
	switch (type) {
	case token_type::op:
	case token_type::reg: { char x = std::get<char>(val); return std::string(&x, 1); }
	case token_type::imm: return std::to_string(std::get<double>(val));
	case token_type::grp: return ::repr(std::get<token_vector>(val));
	default: return "?";
	}
}

static int eval_help(const char *expr, const char *ptr, const char *reason)
{
	fprintf(stderr, "Evaluation of expression/subexpression failed at\n\t%s\n\t%-*s^\n%s\n",
		expr, static_cast<int>(ptr - expr), "", reason);
	return 1;
}

static int eval_help(const char *complaint, const token_vector &tokens)
{
	fprintf(stderr, "%s:\n\t%s\n", complaint, repr(tokens).c_str());
	return 1;
}

static constexpr char EVAL_REGS[] = "bcghlrsxyz";
static int eval_tokenize(const char *ptr, char **super_end, token_vector &tokens)
{
	auto cmd = ptr;
	/* Section 1 */
	token_type last_type = token_type::none;
	while (true) {
		while (HX_isspace(*ptr))
			++ptr;
		if (*ptr == '\0' || *ptr == ')')
			break;
		char *end = nullptr;
		auto imm = strtod(ptr, &end);
		if (end == nullptr) {
			return eval_help(cmd, ptr, "strtod failed hard");
		} else if (strchr("*/+,-=^", *ptr) != nullptr) {
			if (last_type == token_type::none || last_type == token_type::op)
				return eval_help(cmd, ptr, "Cannot use operator here (note: no unary operators supported)");
			tokens.push_back(token_entry{token_type::op, token_value{*ptr}});
			++ptr;
		} else if (strchr(EVAL_REGS, *ptr) != nullptr) {
			if (last_type != token_type::none && last_type != token_type::op)
				return eval_help(cmd, ptr, "Cannot use identifier here");
			auto reg = *ptr;
			if (reg == 's')
				reg = 'c';
			tokens.push_back(token_entry{token_type::reg, token_value{reg}});
			++ptr;
		} else if (*ptr == '(') {
			if (last_type != token_type::none && last_type != token_type::op)
				return eval_help(cmd, ptr, "Cannot use opening parenthesis here");
			++ptr;
			token_vector newgrp;
			auto ret = eval_tokenize(ptr, &end, newgrp);
			if (ret != 0)
				return ret;
			ptr = end;
			if (*end != ')')
				return eval_help(cmd, ptr, "Expected closing parenthesis");
			++ptr;
			tokens.push_back(token_entry{token_type::grp, token_value{std::move(newgrp)}});
		} else if (end != ptr) {
			if (last_type != token_type::none && last_type != token_type::op)
				return eval_help(cmd, ptr, "Cannot use immediate value here");
			tokens.push_back(token_entry{token_type::imm, token_value{imm}});
			ptr = end;
		} else {
			return eval_help(cmd, ptr, "Unexpected character");
		}
		last_type = tokens.back().type;
	}

	/* Section 2 */
	if (tokens.empty())
		return eval_help(cmd, ptr, "No tokens were parsed -- empty parenthesis?");
	assert(tokens.front().type != token_type::op);
	if (tokens.back().type == token_type::op)
		return eval_help(cmd, ptr, "Last token cannot be an operator");
	if (super_end != nullptr)
		*super_end = const_cast<char *>(ptr);

	/* Section 3: Precedence maker */
	static constexpr const char *op_prec[] = {"^", "*/", "+-", "=", ","};
	for (auto op_group : op_prec) {
		bool right_assoc = *op_group == '=';
		if (right_assoc)
			std::reverse(tokens.begin(), tokens.end());
		for (size_t i = 1; i < tokens.size(); ) {
			if (tokens[i].type != token_type::op) {
				++i;
				continue;
			}
			auto op = std::get<char>(tokens[i].val);
			if (strchr(op_group, op) == nullptr) {
				++i;
				continue;
			}
			assert(i < tokens.size() - 1);
			token_vector newgrp;
			newgrp.emplace_back(std::move(tokens[i-1]));
			newgrp.emplace_back(std::move(tokens[i]));
			newgrp.emplace_back(std::move(tokens[i+1]));
			if (right_assoc)
				std::reverse(newgrp.begin(), newgrp.end());
			tokens[i-1].type = token_type::grp;
			tokens[i-1].val  = std::move(newgrp);
			tokens.erase(tokens.begin() + i, tokens.begin() + i + 2);
			/* Redo at current position i */
		}
		if (right_assoc)
			std::reverse(tokens.begin(), tokens.end());
	}
	return 0;
}

static double eval_rd(mpalette &mpal, size_t idx, char reg)
{
	switch (reg) {
	case 'r': return mpal.ra[idx].r;
	case 'g': return mpal.ra[idx].g;
	case 'b': return mpal.ra[idx].b;
	case 'l': return mpal.la[idx].l;
	case 'c': return mpal.la[idx].c;
	case 'h': return mpal.la[idx].h;
	case 'x': return mpal.x;
	case 'y': return mpal.y;
	case 'z': return mpal.z;
	default: throw "Illegal register";
	}
}

static token_entry eval_grp(const token_vector &tokens, mpalette &mpal, size_t idx);

static std::pair<token_entry, double>
eval_arg(const token_entry &token, mpalette &mpal, size_t idx)
{
	if (token.type == token_type::imm) {
		return {token, std::get<double>(token.val)};
	} else if (token.type == token_type::reg) {
		return {token, eval_rd(mpal, idx, std::get<char>(token.val))};
	} else if (token.type == token_type::grp) {
		auto categ = eval_grp(std::get<token_vector>(token.val), mpal, idx);
		if (categ.type == token_type::imm) {
			return {categ, std::get<double>(categ.val)};
		} else if (categ.type == token_type::reg) {
			return {categ, eval_rd(mpal, idx, std::get<char>(categ.val))};
		}
	}
	throw "Unhandled subexpr";
}

static token_entry eval_grp(const token_vector &tokens, mpalette &mpal, size_t idx)
{
	if (tokens.size() == 1) {
		if (tokens[0].type != token_type::grp)
			return tokens[0];
		return eval_grp(std::get<token_vector>(tokens[0].val), mpal, idx);
	} else if (tokens.size() != 3) {
		eval_help("Expected a group with 3 tokens", tokens);
		return {};
	} else if (tokens[1].type != token_type::op) {
		eval_help("Expected middle token to be an operator", tokens);
		return {};
	}

	auto op = std::get<char>(tokens[1].val);
	/*
	 * Evaluation order! Take notes from https://en.cppreference.com/w/cpp/language/eval_order .
	 * For ',', we need lhs before rhs.
	 */
	auto [lhs, lhv] = eval_arg(tokens[0], mpal, idx);
	auto [rhs, rhv] = eval_arg(tokens[2], mpal, idx);

	switch (op) {
	case '+': return {token_type::imm, lhv + rhv};
	case '-': return {token_type::imm, lhv - rhv};
	case '*': return {token_type::imm, lhv * rhv};
	case '/': return {token_type::imm, lhv / rhv};
	case '^': return {token_type::imm, pow(std::max(0.0, lhv), rhv)};
	case ',': return rhs;
	case '=': break;
	default:
		fprintf(stderr, "Unhandled op '%c' in subexpr: %s\n", op, repr(tokens).c_str());
		return {};
	}

	if (lhs.type != token_type::reg) {
		fprintf(stderr, "Left-hand side of subexpr needs to be a register: %s\n", repr(tokens).c_str());
		return {};
	}
	bool mod_la = false, mod_ra = false;
	auto reg = std::get<char>(lhs.val);
	switch (reg) {
	case 'r': mpal.ra[idx].r = rhv; mod_ra = true; break;
	case 'g': mpal.ra[idx].g = rhv; mod_ra = true; break;
	case 'b': mpal.ra[idx].b = rhv; mod_ra = true; break;
	case 'l': mpal.la[idx].l = rhv; mod_la = true; break;
	case 'c': mpal.la[idx].c = rhv; mod_la = true; break;
	case 'h': mpal.la[idx].h = HX_flpr(rhv, 360); mod_la = true; break;
	case 'x': mpal.x = rhv; break;
	case 'y': mpal.y = rhv; break;
	case 'z': mpal.z = rhv; break;
	default:
		fprintf(stderr, "Left-hand side of subexpr needs to be a register: %s\n", repr(tokens).c_str());
		return {};
	}
	if (mod_la)
		mpal.mod_la();
	if (mod_ra)
		mpal.mod_ra();
	return lhs;
}

static int do_eval(const char *cmd, mpalette &mpal,
    const std::vector<size_t> &indices = {}) try
{
	token_vector tokens;
	auto ret = eval_tokenize(cmd, nullptr, tokens);
	if (ret != 0)
		return ret;
	if (g_verbose >= 2)
		fprintf(stderr, "# expr parsed as: %s\n", repr(tokens).c_str());
	if (mpal.la.size() != mpal.ra.size()) {
		fprintf(stderr, "Programming error / Debug me here\n");
		return -1;
	}
	if (indices.empty()) {
		for (size_t i = 0; i < mpal.la.size(); ++i) {
			auto d = eval_grp(tokens, mpal, i);
			if (d.type == token_type::none)
				return -1;
		}
	} else {
		for (auto i : indices) {
			if (i >= mpal.la.size())
				continue;
			auto d = eval_grp(tokens, mpal, i);
			if (d.type == token_type::none)
				return -1;
		}
	}
	return 0;
} catch (const char *e) {
	fprintf(stderr, "%s\n", e);
	return -1;
}

std::vector<size_t> parse_range(const char *s_input)
{
	auto s = s_input;
	std::vector<size_t> vec;
	while (*s != '\0' && *s != '=') {
		char *end = nullptr;
		auto val = strtoull(s, &end, 0);
		if (end == s) {
			fprintf(stderr, "Failed parsing range \"%s\" at ...\"%s\"\n", s_input, s);
			break;
		}
		s = end;
		if (*s == '-') {
			++s;
			auto val2 = strtoull(s, &end, 0);
			if (end == s) {
				fprintf(stderr, "Failed parsing range \"%s\" at ...\"%s\"\n", s_input, s);
				break;
			}
			s = end;
			for (auto j = val; j <= val2; ++j)
				vec.push_back(j);
		} else {
			vec.push_back(val);
		}
		if (*s == ',') {
			++s;
			continue;
		}
	}
	return vec;
}

int main(int argc, char **argv)
{
	std::unordered_map<std::string, mpalette> allpal;
	auto xter = allpal.emplace("0", mpalette{});
	mpalette &mpal = xter.first->second;
	struct bb_guard {
		bb_guard() { ::babl_init(); }
		~bb_guard() { ::babl_exit(); }
	};
	bb_guard bbg;
	HXopt6_auto_result argp;

	if (HX_getopt6(g_options_table, argc, argv, &argp,
	    HXOPT_RQ_ORDER | HXOPT_USAGEONERR | HXOPT_ITER_ARGS) != HXOPT_ERR_SUCCESS)
		return EXIT_FAILURE;

	xyz_to_lrgb_matrix = make_lrgb_matrix(to_xyz(illuminant_d(6500)));
	srgb888_space = babl_format_with_space("R'G'B' u8", babl_space("sRGB"));
	if (srgb888_space == nullptr) {
		fprintf(stderr, "BABL does not know sRGB888\n");
		return EXIT_FAILURE;
	}
	srgb_space = babl_format_with_space("R'G'B' double", babl_space("sRGB"));
	if (srgb_space == nullptr) {
		fprintf(stderr, "BABL does not know sRGB\n");
		return EXIT_FAILURE;
	}
	lch_space = babl_format_with_space("CIE LCH(ab) double", babl_space("sRGB"));
	if (lch_space == nullptr) {
		fprintf(stderr, "BABL does not know LCh\n");
		return EXIT_FAILURE;
	}

	for (int ni = 0; ni < argp.nargs; ++ni) {
		auto le_arg = argp.uarg[ni];
		auto ptr = strchr(le_arg, '=');
		auto arg1 = ptr != nullptr ? strtod(ptr + 1, nullptr) : 0;
		bool mod_ra = false, mod_la = false;

		if (strcmp(le_arg, "vga") == 0) {
			mpal.ra = {std::begin(vga_palette), std::end(vga_palette)};
			mod_ra = true;
		} else if (strcmp(le_arg, "vgs") == 0) {
			mpal.ra = {std::begin(vgasat_palette), std::end(vgasat_palette)};
			mod_ra = true;
		} else if (strcmp(le_arg, "win") == 0) {
			mpal.ra = {std::begin(win_palette), std::end(win_palette)};
			mod_ra = true;
		} else if (strncmp(le_arg, "loadpal=", 8) == 0) {
			if (loadpal(&le_arg[8], mpal.ra) != 0)
				return EXIT_FAILURE;
			mod_ra = true;
		} else if (strncmp(le_arg, "loadreg=", 8) == 0) {
			mpal = allpal[&le_arg[8]];
		} else if (strncmp(le_arg, "savereg=", 8) == 0) {
			allpal[&le_arg[8]] = mpal;
		} else if (strncmp(le_arg, "blend=", 6) == 0) {
			char *end = nullptr;
			auto pct = strtod(&le_arg[6], &end);
			if (*end == ',') {
				++end;
				auto bi = allpal.find(end);
				if (bi == allpal.cend()) {
					fprintf(stderr, "Register \"%s\" not defined yet\n", end);
				} else {
					mpal.ra = do_blend(mpal.ra, 1-pct/100, allpal[end].ra, pct/100);
					mod_ra = true;
				}
			}

		} else if (strncmp(le_arg, "eval@", 5) == 0) {
			auto eqsign = strchr(&le_arg[5], '=');
			if (eqsign != nullptr) {
				*eqsign++ = '\0';
				auto indices = parse_range(&le_arg[5]);
				if (do_eval(eqsign, mpal, indices) != 0)
					return EXIT_FAILURE;
			}
		} else if (strncmp(le_arg, "eval=", 5) == 0) {
			if (do_eval(&le_arg[5], mpal) != 0)
				return EXIT_FAILURE;
		} else if (*le_arg == '(' || (strchr(EVAL_REGS, *le_arg) && le_arg[1] == '=')) {
			if (do_eval(le_arg, mpal) != 0)
				return EXIT_FAILURE;
		} else if (strncmp(le_arg, "ild=", 4) == 0) {
			fprintf(stderr, "New white_point D_%.2f:\n", arg1 / 100);
			auto a = illuminant_d(arg1);
			fprintf(stderr, "{x=%.15f, y=%.15f}\n", a.x, a.y);
			auto b = to_xyz(a);
			fprintf(stderr, "{X=%.15f, Y=%.15f, Z=%.15f}\n", b.x, b.y, b.z);
			xyz_to_lrgb_matrix = make_lrgb_matrix(b);
			std::stringstream ss;
			ss << xyz_to_lrgb_matrix;
			fprintf(stderr, "XYZ-to-LRGB matrix:\n%s\n", ss.str().c_str());
		} else if (strcmp(le_arg, "lch") == 0) {
			printf("#L,c,h\n");
			unsigned int cnt = 0;
			for (auto &e : mpal.la) {
				printf("\e[%u;3%um%x\e[0m: {%10.6f, %10.6f, %10.6f}\n",
					!!(cnt & 0x8), cnt & 0x7,
					cnt, e.l, e.c, e.h);
				++cnt;
			}
		} else if (strncmp(le_arg, "hsltint=", 8) == 0) {
			mpal.ra = hsltint(parse_hsl(&le_arg[8]), mpal.la);
			mod_ra = true;
		} else if (strncmp(le_arg, "lchtint=", 8) == 0) {
			auto base = parse_hsl(&le_arg[8]);
			auto v = to_lch(to_srgb(base));
			if (g_verbose >= 2)
				fprintf(stderr, "# converted %s to LCh(%f,%f,%f)\n", &le_arg[8], v.l, v.c, v.h);
			mpal.la = lchtint(v, mpal.la);
			mod_la = true;
		} else if (strcmp(le_arg, "emit") == 0 || strcmp(le_arg, "xfce") == 0) {
			emit_xfce(mpal.ra);
		} else if (strcmp(le_arg, "xterm") == 0) {
			emit_xterm(mpal.ra);
		} else if (strcmp(le_arg, "fg") == 0) {
			xterm_fg = 1;
		} else if (strcmp(le_arg, "bg") == 0) {
			xterm_bg = 1;
		} else if (strcmp(le_arg, "bd") == 0) {
			xterm_bd = 1;
		} else if (strcmp(le_arg, "b0") == 0) {
			mpal.la[0] = {0,0,0};
			mpal.ra[0] = {0,0,0};
		} else if (strcmp(le_arg, "inv16") == 0) {
			decltype(mpal.ra) new_ra(mpal.ra.size());
			for (size_t i = 0; i < mpal.ra.size(); ++i)
				new_ra[i] = std::move(mpal.ra[~i % mpal.ra.size()]);
			mpal.ra = std::move(new_ra);
			mod_ra = true;
			/*
			 * A computational method (only produces exact results
			 * for the "win" palette):
			 *
			 * auto h = to_hsl(to_srgb(e));
			 * h.h += 180;
			 * h.l = 1 - 0.25 * h.s - h.l;
			 * e = to_srgb888(to_srgb(h));
			 */
		} else if (strcmp(le_arg, "ct256") == 0) {
			colortable_256();
			colortable_16();
		} else if (strcmp(le_arg, "ct") == 0) {
			colortable_16();
		} else if (strcmp(le_arg, "cxl") == 0) {
			cxl_command(mpal.la);
		} else if (strcmp(le_arg, "cxa") == 0) {
			cxa_command(mpal.ra);
		} else if (strncmp(le_arg, "cfgamma=", 8) == 0) {
			g_continuous_gamma = arg1;
		} else if (strcmp(le_arg, "loeq") == 0) {
			mpal.la = equalize(mpal.la, 9, 100 / 9.0, 100 * 8 / 9.0);
			mod_la = true;
		} else if (strncmp(le_arg, "loeq=", 5) == 0) {
			char *end = nullptr;
			arg1 = strtod(&le_arg[5], &end);
			double arg2 = *end == ',' ? strtod(end + 1, &end) : 100 / 9.0 * 8;
			mpal.la = equalize(mpal.la, 9, arg1, arg2);
			mod_la = true;
		} else if (strcmp(le_arg, "eq") == 0) {
			mpal.la = equalize(mpal.la, 16, 100 / 16.0, 100);
			mod_la = true;
		} else if (strncmp(le_arg, "eq=", 3) == 0) {
			mpal.la = equalize(mpal.la, 16, arg1, 100);
			mod_la = true;
		} else if (strcmp(le_arg, "syncfromrgb") == 0) {
			mpal.mod_ra();
		} else if (strcmp(le_arg, "syncfromlch") == 0) {
			mpal.mod_la();
		} else {
			fprintf(stderr, "Unrecognized command: \"%s\"\n", le_arg);
		}
		if (mod_ra)
			mpal.mod_ra();
		else if (mod_la)
			mpal.mod_la();
	}
	return EXIT_SUCCESS;
}
