#include <cstdint>
#include "xbrz_call.h"
#include "xbrz.h"

void xbrz_scale(size_t factor, uint32_t *src, uint32_t *trg, int src_width,
    int src_height)
{
	xbrz::scale(factor, src, trg, src_width, src_height);
}
