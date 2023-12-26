// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2022,2023 Jan Engelhardt
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
#include <utility>
#include <vector>
#include <libHX/ctype_helper.h>

namespace {
struct srgb888 { uint8_t r = 0, g = 0, b = 0; };
struct srgb { double r = 0, g = 0, b = 0; };
struct lrgb { double r = 0, g = 0, b = 0; };
struct xy0 { double x = 0, y = 0; }; // xyY (but without Y)
struct xyz { double x = 0, y = 0, z = 0; }; // XYZ
struct lab { double l = 0, a = 0, b = 0; };
struct lch { double l = 0, c = 0, h = 0; };
struct hsl { double h = 0, s = 0, l = 0; };
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
static constexpr srgb888 xpal_palette[] = {
	/* experimental maximum-contrast palette (not good for rendition) */
	{0x00,0x00,0x00}, {0x34,0x34,0x34}, {0x4e,0x4e,0x4e}, {0x67,0x67,0x67},
	{0x83,0x83,0x83}, {0xa0,0xa0,0xa0}, {0xbf,0xbf,0xbf}, {0xdc,0xdc,0xdc},
	{0x1d,0x1d,0x1d}, {0xff,0xff,0xff}, {0xff,0xff,0xff}, {0xff,0xff,0xff},
	{0xff,0xff,0xff}, {0xff,0xff,0xff}, {0xff,0xff,0xff}, {0xff,0xff,0xff},
};

static unsigned int debug_cvt, xterm_fg, xterm_bg;

static double flpr(double x, double y)
{
	return fmod(fmod(x, y) + y, y);
}

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

static double gamma_expand(double c)
{
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
	return pow((c + 0.055) / 1.055, 12 / 5.0);
}

static double gamma_compress(double c)
{
	return c <= (0.04045 / 12.92) ? c * 12.92 :
	       pow(c, 5 / 12.0) * 1.055 - 0.055;
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

static srgb to_srgb(const lrgb &e)
{
	return {gamma_compress(e.r), gamma_compress(e.g), gamma_compress(e.b)};
}

static lrgb to_lrgb(const srgb &e)
{
	return {gamma_expand(e.r), gamma_expand(e.g), gamma_expand(e.b)};
}

static hsl parse_hsl(const char *str)
{
	hsl c;
	if (*str != '#') {
		sscanf(str, "%lf,%lf,%lf", &c.h, &c.s, &c.l);
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

static constexpr xyz to_xyz(const xy0 &e)
{
	return {e.x / e.y, 1, (1 - e.x - e.y) / e.y};
}

static constexpr xyz white_point = to_xyz(xy0{0.312713, 0.329016});

static lrgb to_lrgb(const xyz &e)
{
	return {
		e.x * 4277208 / 1319795    + e.y * -2028932 / 1319795   + e.z * -658032 / 1319795,
		e.x * -70985202 / 73237775 + e.y * 137391598 / 73237775 + e.z * 3043398 / 73237775,
		e.x * 164508 / 2956735     + e.y * -603196 / 2956735    + e.z * 3125652 / 2956735,
	};
}

static xyz to_xyz(const lrgb &e)
{
	/* https://mina86.com/2019/srgb-xyz-matrix/ */
	return {
		e.r * 33786752 / 81924984 + e.g * 29295110 / 81924984  + e.b * 14783675 / 81924984,
	        e.r * 8710647 / 40962492  + e.g * 29295110 / 40962492  + e.b * 2956735 / 40962492,
	        e.r * 4751262 / 245774952 + e.g * 29295110 / 245774952 + e.b * 233582065 / 245774952,
	};
}

static constexpr double epsilon = 216 / 24389.0, epsilon_inverse = 6 / 29.0;
static constexpr double kappa = 24389 / 27.0;

static double lab_fwd(double v)
{
	return v > epsilon ? pow(v, 1 / 3.0) : (kappa * v + 16) / 116;
}

static double lab_inv(double v)
{
	return v > epsilon_inverse ? pow(v, 3) : (v * 116 - 16) / kappa;
}

static xyz to_xyz(const lab &e)
{
	auto y = (e.l + 16) / 116;
	auto x = (e.a / 500) + y;
	auto z = y - (e.b / 200);
	return {lab_inv(x) * white_point.x, e.l > 8 ? pow(y, 3) : e.l / kappa,
	        lab_inv(z) * white_point.z};
}

static lab to_lab(const xyz &e)
{
	auto x = lab_fwd(e.x / white_point.x);
	auto y = lab_fwd(e.y / white_point.y);
	auto z = lab_fwd(e.z / white_point.z);
	return lab{116 * y - 16.0, 500 * (x - y), 200 * (y - z)};
}

static lab to_lab(const lch &e)
{
	auto rad = e.h * 2 * M_PI / 360;
	return {e.l, e.c * cos(rad), e.c * sin(rad)};
}

static lch to_lch(const lab &e)
{
	auto c = sqrt(pow(e.a, 2) + pow(e.b, 2));
	auto h = atan2(e.b, e.a) * 360 / M_PI / 2;
	if (h < 0)
		h += 360;
	return {e.l, c, h};
}

static lch to_lch(const srgb &a)
{
	auto b = to_lrgb(a);
	if (debug_cvt)
		fprintf(stderr, "\tlrgb = {%f, %f, %f}\n", b.r, b.g, b.b);
	auto c = to_xyz(b);
	if (debug_cvt)
		fprintf(stderr, "\txyz = {%f, %f, %f}\n", c.x, c.y, c.z);
	auto d = to_lab(c);
	if (debug_cvt)
		fprintf(stderr, "\tlab = {%f, %f, %f}\n", d.l, d.a, d.b);
	auto e = to_lch(d);
	if (debug_cvt)
		fprintf(stderr, "\tlch = {%f, %f, %f}\n", e.l, e.c, e.h);
	return e;
}

static lch to_lch(const srgb888 &color)
{
	if (debug_cvt)
		fprintf(stderr, "to_lch(%s):\n", to_hex(color).c_str());
	auto a = to_srgb(color);
	if (debug_cvt)
		fprintf(stderr, "\tsrgb = {%f, %f, %f}\n", a.r, a.g, a.b);
	return to_lch(a);
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
	for (const auto &color : in) {
		if (debug_cvt)
			fprintf(stderr, "to_srgb888(lch{%f, %f, %f}):\n", color.l, color.c, color.h);
		auto a = to_lab(color);
		if (debug_cvt)
			fprintf(stderr, "\tlab = {%f, %f, %f}\n", a.l, a.a, a.b);
		auto b = to_xyz(a);
		if (debug_cvt)
			fprintf(stderr, "\txyz = {%f, %f, %f}\n", b.x, b.y, b.z);
		auto c = to_lrgb(b);
		if (debug_cvt)
			fprintf(stderr, "\tlrgb = {%f, %f, %f}\n", c.r, c.g, c.b);
		auto d = to_srgb(c);
		if (debug_cvt)
			fprintf(stderr, "\tsrgb = {%f, %f, %f}\n", d.r, d.g, d.b);
		auto e = to_srgb888(d);
		if (debug_cvt)
			fprintf(stderr, "\thex = %s\n", to_hex(e).c_str());
		out.push_back(e);
	}
	return out;
}

static void emit(const std::vector<srgb888> &pal)
{
	printf("ColorPalette=");
	for (const auto &e : pal)
		printf("%s;", to_hex(e).c_str());
	printf("\n");
}

static void xterm(const std::vector<srgb888> &pal)
{
	for (unsigned int idx = 0; idx < 16; ++idx)
		printf(" -xrm *VT100*color%u:%s", idx, to_hex(pal[idx]).c_str());
	if (xterm_fg)
		printf(" -fg %s", to_hex(pal[7]).c_str());
	if (xterm_bg)
		printf(" -bg %s", to_hex(pal[0]).c_str());
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
	for (unsigned int b = 0; b < 256; b += 32) {
		for (unsigned int g = 0; g < 256; g += 32) {
			for (unsigned int r = 0; r < 256; r += 16)
				printf("\e[30;48;2;%u;%u;%um.", r, g, b);
			printf("\e[0m\n");
		}
	}
	for (unsigned int c = 0x0; c <= 0xFF; ++c) {
		printf("\e[30;48;5;%um-%02x-", c, c);
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

template<typename F> static void analyze(const double (&delta)[16][16],
    F &&penalize, unsigned int xlim, unsigned int ylim, const char *desc)
{
	double c = 0;
	unsigned int u = 0;
	for (unsigned int y = 0; y < ylim; ++y) {
		for (unsigned int x = 0; x < xlim; ++x) {
			if (x == y)
				continue;
			c += delta[y][x];
			if (penalize(delta[y][x]))
				++u;
		}
	}
	printf("[%-5s] contrast Σ %.0f ø %.1f", desc, c, c / xlim / ylim);
	c -= 100 * u;
	printf(" // minus %u penalties:\tΣ %.0f ø %.1f\n", u, c, c / xlim / ylim);
}

static void cxl_command(const std::vector<lch> &lch_pal)
{
	printf("\e[1m════ Difference of the L components ════\e[0m\n");
	double delta[16][16]{};
	colortable_16([&](int bg, int fg, int special) {
		if (special || fg >= 16 || bg >= 16 || fg == bg) {
			printf("   ");
			return;
		}
		delta[bg][fg] = fabs(lch_pal[fg].l - lch_pal[bg].l);
		printf("%3.0f", delta[bg][fg]);
	});
	auto pf = [](double x) { return x < 7.0; };
	analyze(delta, pf, 16, 16, "16x16");
	analyze(delta, pf, 16,  8, "16x8 ");
	analyze(delta, pf,  8,  8, " 8x8 ");
}

static void cxr_command(const std::vector<srgb888> &srgb_pal)
{
	printf("\e[1m════ L component of the radiosity difference ════\e[0m\n");
	double delta[16][16]{};
	colortable_16([&](int bg, int fg, int special) {
		if (special || fg >= 16 || bg >= 16 || fg == bg) {
			printf("   ");
			return;
		}
		srgb888 xr;
		xr.r = abs(srgb_pal[fg].r - srgb_pal[bg].r);
		xr.g = abs(srgb_pal[fg].g - srgb_pal[bg].g);
		xr.b = abs(srgb_pal[fg].b - srgb_pal[bg].b);
		delta[bg][fg] = to_lch(xr).l;
		printf("%3.0f", delta[bg][fg]);
	});
	auto pf = [](double x) { return x <= 20.0; };
	analyze(delta, pf, 16, 16, "16x16");
	analyze(delta, pf, 16,  8, "16x8 ");
	analyze(delta, pf,  8,  8, " 8x8 ");
}

static std::vector<lch> loeq(std::vector<lch> la, double blue, double gray)
{
	unsigned int sbl[9];
	for (unsigned int idx = 0; idx < std::size(sbl); ++idx)
		sbl[idx] = idx;
	std::sort(std::begin(sbl), std::end(sbl),
		[&](unsigned int x, unsigned int y) { return la[x].l < la[y].l; });

	fprintf(stderr, "loeq in: ");
	for (auto z : sbl)
		fprintf(stderr, "%u(%f) ", z, la[z].l);
	fprintf(stderr, "\nloeq out: ");
	for (unsigned int idx = 1; idx < std::size(sbl); ++idx) {
		la[sbl[idx]].l = (gray - blue) * (idx - 1) / 7 + blue + la[sbl[0]].l;
		fprintf(stderr, "%u(%f) ", sbl[idx], la[sbl[idx]].l);
	}
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
	if (p == nullptr || p[0] != '=')
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

int main(int argc, const char **argv)
{
	std::vector<srgb888> ra;
	std::vector<lch> la;

	while (*++argv != nullptr) {
		auto ptr = strchr(*argv, '=');
		auto arg1 = ptr != nullptr ? strtod(ptr + 1, nullptr) : 0;
		bool mod_ra = false, mod_la = false;

		if (strcmp(*argv, "vga") == 0) {
			ra = {std::begin(vga_palette), std::end(vga_palette)};
			mod_ra = true;
		} else if (strcmp(*argv, "vgs") == 0) {
			ra = {std::begin(vgasat_palette), std::end(vgasat_palette)};
			mod_ra = true;
		} else if (strcmp(*argv, "win") == 0) {
			ra = {std::begin(win_palette), std::end(win_palette)};
			mod_ra = true;
		} else if (strcmp(*argv, "xpal") == 0) {
			ra = {std::begin(xpal_palette), std::end(xpal_palette)};
			mod_ra = true;
		} else if (strncmp(*argv, "loadpal=", 8) == 0) {
			if (loadpal(&argv[0][8], ra) != 0)
				return EXIT_FAILURE;
			mod_ra = true;
		} else if (strcmp(*argv, "debug") == 0) {
			debug_cvt = 1;
		} else if (strcmp(*argv, "stat") == 0) {
			printf("#L,c,h\n");
			for (auto &e : la)
				printf("{%f,%f,%f}\n", e.l, e.c, e.h);
		} else if (strncmp(*argv, "litadd=", 7) == 0) {
			for (auto &e : la)
				e.l += arg1;
			mod_la = true;
		} else if (strncmp(*argv, "litmul=", 7) == 0) {
			for (auto &e : la)
				e.l *= arg1;
			mod_la = true;
		} else if (strncmp(*argv, "litset=", 7) == 0) {
			for (auto &e : la)
				e.l = arg1;
			mod_la = true;
		} else if (strncmp(*argv, "satadd=", 7) == 0) {
			for (auto &e : la)
				e.c += arg1;
			mod_la = true;
		} else if (strncmp(*argv, "satmul=", 7) == 0) {
			for (auto &e : la)
				e.c *= arg1;
			mod_la = true;
		} else if (strncmp(*argv, "satset=", 7) == 0) {
			for (auto &e : la)
				e.c = arg1;
			mod_la = true;
		} else if (strncmp(*argv, "hueadd=", 7) == 0) {
			for (auto &e : la)
				e.h = flpr(e.h + arg1, 360);
			mod_la = true;
		} else if (strncmp(*argv, "hueset=", 5) == 0) {
			arg1 = fmod(arg1, 360);
			for (auto &e : la)
				e.h = arg1;
			mod_la = true;
		} else if (strncmp(*argv, "hsltint=", 8) == 0) {
			ra = hsltint(parse_hsl(&argv[0][8]), la);
			mod_ra = true;
		} else if (strncmp(*argv, "lchtint=", 8) == 0) {
			auto base = parse_hsl(&argv[0][8]);
			la = lchtint(to_lch(to_srgb(base)), la);
			mod_la = true;
		} else if (strcmp(*argv, "emit") == 0) {
			emit(ra);
		} else if (strcmp(*argv, "xterm") == 0) {
			xterm(ra);
		} else if (strcmp(*argv, "fg") == 0) {
			xterm_fg = 1;
		} else if (strcmp(*argv, "bg") == 0) {
			xterm_bg = 1;
		} else if (strcmp(*argv, "b0") == 0) {
			la[0] = {0,0,0};
			ra[0] = {0,0,0};
		} else if (strcmp(*argv, "inv16") == 0) {
			decltype(ra) new_ra(ra.size());
			for (size_t i = 0; i < ra.size(); ++i)
				new_ra[i] = std::move(ra[~i % ra.size()]);
			ra = std::move(new_ra);
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
		} else if (strcmp(*argv, "ct256") == 0) {
			colortable_256();
			colortable_16();
		} else if (strcmp(*argv, "ct") == 0) {
			colortable_16();
		} else if (strcmp(*argv, "cxl") == 0) {
			cxl_command(la);
		} else if (strcmp(*argv, "cxr") == 0) {
			cxr_command(ra);
		} else if (strcmp(*argv, "loeq") == 0) {
			double z = 100 / 9.0;
			la = loeq(la, z, z * 8);
			mod_la = true;
		} else if (strncmp(*argv, "loeq=", 5) == 0) {
			char *end = nullptr;
			arg1 = strtod(&argv[0][5], &end);
			double arg2 = *end == ',' ? strtod(end + 1, &end) : 100 / 9.0 * 8;
			la = loeq(la, arg1, arg2);
			mod_la = true;
		} else {
			fprintf(stderr, "Unrecognized command: \"%s\"\n", *argv);
		}
		if (mod_ra)
			la = to_lch(ra);
		else if (mod_la)
			ra = to_srgb888(la);
	}
	return EXIT_SUCCESS;
}
