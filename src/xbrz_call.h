#ifndef XBRZ_CALL_H
#define XBRZ_CALL_H 1

#ifdef __cplusplus
#	include <cstdint>
#else
#	include <stdint.h>
#endif
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void xbrz_scale(size_t, const uint32_t *, uint32_t *,
	unsigned int, unsigned int);
extern void xbrz_nearest(size_t, const uint32_t *, uint32_t *,
	unsigned int, unsigned int);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* XBRZ_CALL_H */
