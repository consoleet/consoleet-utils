// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2022,2023 Jan Engelhardt
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {
struct srgb888 { uint8_t r = 0, g = 0, b = 0; };
struct srgb { double r = 0, g = 0, b = 0; };
struct lrgb { double r = 0, g = 0, b = 0; };
struct xyz { double x = 0, y = 0, z = 0; };
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

static unsigned int debug_cvt, xterm_fg, xterm_bg;

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
	if (strlen(str) < 7) {
		fprintf(stderr, "Illegal RGB(,L) value: \"%s\"\n", str);
		return c;
	}
	c = to_hsl(to_srgb(srgb888{fromhex(&str[1]), fromhex(&str[3]), fromhex(&str[5])}));
	str += 7;
	if (*str == ',')
		c.l = strtod(&str[1], nullptr);
	return c;
}

static xyz white_point = {0.9504492182750991, 1, 1.0889166484304715};

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

static void colortable_16(void (*pr)(int, int) = nullptr)
{
	if (pr == nullptr)
		pr = [](int bg, int fg) {
			if (bg == -1)
				printf("0%x", fg);
			else
				printf("%x%x", bg, fg);
		};

	for (int bg = -1; bg < 8; ++bg) {
		for (auto bit : {0, 1, 7}) {
			for (int fg = 30; fg <= 39; ++fg) {
				if (fg == 38)
					continue;
				int lo_fg = fg - 30, lo_bg = bg;
				if (bit == 1)
					lo_fg |= 8;
				else if (bit == 7)
					lo_bg |= 8;
				if (bg == -1)
					printf("\e[0;%d;%dm", bit, fg);
				else
					printf("\e[0;%d;%d;4%dm", bit, fg, bg);
				pr(lo_bg, lo_fg);
			}
		}
		printf("\e[0m\n");
	}
	printf("\e[0mdefault \e[37mgray \e[0;1mbold\e[0m \e[2mdim\e[0m "
	       "\e[3mitalic\e[0m \e[4munderscore\e[0m \e[5mblink\e[0m "
	       "\e[6mrapidblink\e[0m \e[7mreverse\e[0m\n");
}

int main(int argc, const char **argv)
{
	std::vector<srgb888> ra;
	std::vector<lch> la;

	while (*++argv != nullptr) {
		auto ptr = strchr(*argv, '=');
		auto arg1 = ptr != nullptr ? strtod(ptr + 1, nullptr) : 0;

		if (strcmp(*argv, "vga") == 0) {
			ra = {std::begin(vga_palette), std::end(vga_palette)};
		} else if (strcmp(*argv, "vgs") == 0) {
			ra = {std::begin(vgasat_palette), std::end(vgasat_palette)};
		} else if (strcmp(*argv, "win") == 0) {
			ra = {std::begin(win_palette), std::end(win_palette)};
		} else if (strcmp(*argv, "debug") == 0) {
			debug_cvt = 1;
		} else if (strcmp(*argv, "lch") == 0) {
			la = to_lch(ra);
		} else if (strcmp(*argv, "rgb") == 0) {
			ra = to_srgb888(la);
		} else if (strcmp(*argv, "stat") == 0) {
			printf("#L,c,h\n");
			for (auto &e : la)
				printf("{%f,%f,%f}\n", e.l, e.c, e.h);
		} else if (strncmp(*argv, "litadd=", 7) == 0) {
			for (auto &e : la)
				e.l += arg1;
		} else if (strncmp(*argv, "litmul=", 7) == 0) {
			for (auto &e : la)
				e.l *= arg1;
		} else if (strncmp(*argv, "litset=", 7) == 0) {
			for (auto &e : la)
				e.l = arg1;
		} else if (strncmp(*argv, "satadd=", 7) == 0) {
			for (auto &e : la)
				e.c += arg1;
		} else if (strncmp(*argv, "satmul=", 7) == 0) {
			for (auto &e : la)
				e.c *= arg1;
		} else if (strncmp(*argv, "satset=", 7) == 0) {
			for (auto &e : la)
				e.c = arg1;
		} else if (strncmp(*argv, "hueadd=", 7) == 0) {
			for (auto &e : la)
				e.h = fmod(e.h + arg1, 360);
		} else if (strncmp(*argv, "hueset=", 5) == 0) {
			arg1 = fmod(arg1, 360);
			for (auto &e : la)
				e.h = arg1;
		} else if (strncmp(*argv, "hsltint=", 8) == 0) {
			ra = hsltint(parse_hsl(&argv[0][8]), la);
		} else if (strncmp(*argv, "lchtint=", 8) == 0) {
			auto base = parse_hsl(&argv[0][8]);
			la = lchtint(to_lch(to_srgb(base)), la);
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
		} else {
			fprintf(stderr, "Unrecognized command: \"%s\"\n", *argv);
		}
	}
	return EXIT_SUCCESS;
}
