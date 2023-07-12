// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2022,2023 Jan Engelhardt
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <iostream>

namespace {
struct srgb888 { uint8_t r, g, b; };
struct srgb { double r, g, b; };
struct lrgb { double r, g, b; };
struct xyz { double x, y, z; };
struct lab { double l, a, b; };
struct lch { double l, c, h; };
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

static unsigned int debug_cvt;

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

static std::vector<lch> to_lch(const std::vector<srgb888> &in)
{
	std::vector<lch> out;
	for (const auto &color : in) {
		if (debug_cvt)
			fprintf(stderr, "to_lch(%s):\n", to_hex(color).c_str());
		auto a = to_srgb(color);
		if (debug_cvt)
			fprintf(stderr, "\tsrgb = {%f, %f, %f}\n", a.r, a.g, a.b);
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
		out.push_back(e);
	}
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
	printf("xterm -fa monospace:size=24");
	for (unsigned int idx = 0; idx < 16; ++idx)
		printf(" -xrm '*VT100*color%u: %s'", idx, to_hex(pal[idx]).c_str());
	printf("\n");
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
		} else if (strcmp(*argv, "debug") == 0) {
			debug_cvt = 1;
		} else if (strcmp(*argv, "lch") == 0) {
			la = to_lch(ra);
		} else if (strcmp(*argv, "rgb") == 0) {
			ra = to_srgb888(la);
		} else if (strncmp(*argv, "litmul=", 7) == 0) {
			for (auto &e : la)
				e.l *= arg1;
		} else if (strncmp(*argv, "satmul=", 7) == 0) {
			std::transform(la.begin(), la.end(), la.begin(), [=](const lch &e) {
				return lch{e.l, e.c * arg1, e.h};
			});
		} else if (strncmp(*argv, "hueadd=", 7) == 0) {
			for (auto &e : la)
				e.h = fmod(e.h + arg1, 360);
		} else if (strncmp(*argv, "hueset=", 5) == 0) {
			arg1 = fmod(arg1, 360);
			for (auto &e : la)
				e.h = arg1;
		} else if (strcmp(*argv, "emit") == 0) {
			emit(ra);
		} else if (strcmp(*argv, "xterm") == 0) {
			xterm(ra);
		} else {
			fprintf(stderr, "Unrecognized command: \"%s\"\n", *argv);
			return EXIT_FAILURE;
		}
	}
	return EXIT_SUCCESS;
}
