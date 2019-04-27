#include <cstdint>
#include "xbrz_call.h"
#include "xbrz.h"

void xbrz_scale(size_t factor, const uint32_t *src, uint32_t *trg,
    unsigned int src_width, unsigned int src_height)
{
	xbrz::scale(factor, src, trg, src_width, src_height, xbrz::ColorFormat::RGB);
}

void xbrz_nearest(size_t factor, const uint32_t *src, uint32_t *trg,
    unsigned int src_width, unsigned int src_height)
{
	xbrz::nearestNeighborScale(src, src_width, src_height,
		trg, src_width * factor, src_height * factor);
}
