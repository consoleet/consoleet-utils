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
#include <babl/babl.h>
#include <libHX/ctype_helper.h>
#include <libHX/misc.h>

namespace {
struct srgb888 { uint8_t r = 0, g = 0, b = 0; };
struct srgb { double r = 0, g = 0, b = 0; };
struct lrgb { double r = 0, g = 0, b = 0; };
struct lch { double l = 0, c = 0, h = 0; };
struct hsl { double h = 0, s = 0, l = 0; };

/**
 * Statistics for one grid view (e.g. 8x8 / 8x16 / ...).
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

static unsigned int xterm_fg, xterm_bg;
static double g_continuous_gamma;
static const Babl *lch_space, *srgb_space, *srgb888_space;

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

static void emit_xterm(const std::vector<srgb888> &pal)
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
	gs.avg = gs.sum / gs.pairs;
	gs.adj_avg = gs.adj_sum / (gs.pairs - gs.penalized);
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

static lrgb to_lrgb(const srgb &e)
{
	return {gamma_expand(e.r), gamma_expand(e.g), gamma_expand(e.b)};
}

static double trivial_lightness(const lrgb &k)
{
	return 0.2126729 * k.r + 0.7151522 * k.g + 0.0721750 * k.b; // D65 luminance
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
	cx_report(o.x816, " 8x16");
	cx_report(o.x88, " 8x8 ");
}

static void cxl_command(const std::vector<lch> &lch_pal)
{
	printf("\e[1m════ Difference of the L components ════\e[0m\n");
	auto sb = cxl_compute(lch_pal);
	colortable_16([&](int bg, int fg, int special) {
		if (special || fg >= 16 || bg >= 16 || fg == bg)
			printf("   ");
		else
			printf("%3.0f", sb.delta[bg][fg]);
	});
	cx_report(sb);
}

static void cxa_command(const std::vector<srgb888> &pal)
{
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

	fprintf(stderr, "equalize(%zu) in: ", sbl.size());
	for (auto z : sbl)
		fprintf(stderr, "%f(\e[%u;3%um%x\e[0m) ", la[z].l, !!(z & 0x8), z & 0x7, z);
	fprintf(stderr, "\nequalize out: ");
	for (unsigned int idx = 1; idx < sbl.size(); ++idx) {
		unsigned int z = sbl[idx];
		la[z].l = (gray - blue) * (idx - 1) / (sbl.size() - 2) + blue + la[sbl[0]].l;
		fprintf(stderr, "%f(\e[%u;3%um%x\e[0m) ", la[z].l, !!(z & 0x8), z & 0x7, z);
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

int main(int argc, const char **argv)
{
	std::vector<srgb888> ra;
	std::vector<lch> la;
	struct bb_guard {
		bb_guard() { ::babl_init(); }
		~bb_guard() { ::babl_exit(); }
	};
	bb_guard bbg;

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
		} else if (strncmp(*argv, "loadpal=", 8) == 0) {
			if (loadpal(&argv[0][8], ra) != 0)
				return EXIT_FAILURE;
			mod_ra = true;
		} else if (strcmp(*argv, "lch") == 0) {
			printf("#L,c,h\n");
			unsigned int cnt = 0;
			for (auto &e : la) {
				printf("\e[%u;3%um%x\e[0m: {%10.6f, %10.6f, %10.6f}\n",
					!!(cnt & 0x8), cnt & 0x7,
					cnt, e.l, e.c, e.h);
				++cnt;
			}
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
				e.h = HX_flpr(e.h + arg1, 360);
			mod_la = true;
		} else if (strncmp(*argv, "hueset=", 7) == 0) {
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
		} else if (strcmp(*argv, "emit") == 0 || strcmp(*argv, "xfce") == 0) {
			emit_xfce(ra);
		} else if (strcmp(*argv, "xterm") == 0) {
			emit_xterm(ra);
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
		} else if (strcmp(*argv, "cxa") == 0) {
			cxa_command(ra);
		} else if (strncmp(*argv, "cfgamma=", 8) == 0) {
			g_continuous_gamma = arg1;
		} else if (strcmp(*argv, "loeq") == 0) {
			la = equalize(la, 9, 100 / 9.0, 100 * 8 / 9.0);
			mod_la = true;
		} else if (strncmp(*argv, "loeq=", 5) == 0) {
			char *end = nullptr;
			arg1 = strtod(&argv[0][5], &end);
			double arg2 = *end == ',' ? strtod(end + 1, &end) : 100 / 9.0 * 8;
			la = equalize(la, 9, arg1, arg2);
			mod_la = true;
		} else if (strcmp(*argv, "eq") == 0) {
			la = equalize(la, 16, 100 / 16.0, 100);
			mod_la = true;
		} else if (strncmp(*argv, "eq=", 3) == 0) {
			la = equalize(la, 16, arg1, 100);
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
