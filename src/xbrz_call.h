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

extern void xbrz_scale(size_t, uint32_t *, uint32_t *, int, int);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* XBRZ_CALL_H */
