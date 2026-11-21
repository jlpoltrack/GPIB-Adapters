#ifndef XDR_H_
#define XDR_H_

/* XDR (RFC 4506) for ONC RPC: big-endian 32-bit words, opaques padded to 4 bytes. A failed read or write sets
 * err and returns zeros, so callers check err once at the end. */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
	const uint8_t *p;
	uint32_t len, pos;
	bool err;
} xdr_in_t;

typedef struct {
	uint8_t *p;
	uint32_t max, len;
	bool err;
} xdr_out_t;

static inline uint32_t xdr_pad(uint32_t n)
{
	return (n + 3u) & ~3u;
}

static inline uint32_t xdr_u32(xdr_in_t *x)
{
	if (x->err || (x->len - x->pos < 4u)) {
		x->err = true;
		return 0;
	}
	const uint8_t *b = &x->p[x->pos];
	x->pos += 4u;
	return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

/* variable-length opaque or string: returns a pointer into the input, NULL on error or if longer than max */
static inline const uint8_t *xdr_opaque(xdr_in_t *x, uint32_t *n, uint32_t max)
{
	uint32_t len = xdr_u32(x);
	if (x->err || (len > max) || (xdr_pad(len) > x->len - x->pos)) {
		x->err = true;
		*n = 0;
		return NULL;
	}
	const uint8_t *p = &x->p[x->pos];
	x->pos += xdr_pad(len);
	*n = len;
	return p;
}

static inline void xdr_put_u32(xdr_out_t *x, uint32_t v)
{
	if (x->err || (x->max - x->len < 4u)) {
		x->err = true;
		return;
	}
	uint8_t *b = &x->p[x->len];
	b[0] = (uint8_t)(v >> 24);
	b[1] = (uint8_t)(v >> 16);
	b[2] = (uint8_t)(v >> 8);
	b[3] = (uint8_t)v;
	x->len += 4u;
}

static inline void xdr_put_opaque(xdr_out_t *x, const void *data, uint32_t n)
{
	xdr_put_u32(x, n);
	if (x->err || (x->max - x->len < xdr_pad(n))) {
		x->err = true;
		return;
	}
	if (n)
		memmove(&x->p[x->len], data, n);
	memset(&x->p[x->len + n], 0, xdr_pad(n) - n);
	x->len += xdr_pad(n);
}

#endif /* XDR_H_ */
