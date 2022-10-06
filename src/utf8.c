#line 26 "utf8.unuc"

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#define UNICODE_MAX 0x10FFFF
#line 51 "utf8.unuc"

static const uint8_t utf8_length[256] = {
     /* 0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
/* 0 */ 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* 1 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* 2 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* 3 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* 4 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* 5 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* 6 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* 7 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* 8 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* 9 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* A */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* B */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* C */ 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
/* D */ 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
/* E */ 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
/* F */ 4, 4, 4, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const uint8_t utf8_mask[6] = {
	0x7F, 0x1F, 0x0F, 0x07, 0x03, 0x01
};

static uint8_t
utf8_sequence_length(char c)
{
	return utf8_length[(uint8_t)c];
}

static ssize_t
utf8_decode(uint32_t *out, char *c, size_t sz)
{
	if (c[0] == 0 || sz == 0)
		return -1;

	uint8_t len = utf8_sequence_length(*c);

	if (len == 0 || len > sz)
		return -1;

	uint32_t result = c[0] & utf8_mask[len-1];

	for (size_t i = 1; i < len; ++i) {
		if ((c[i] & 0xc0) != 0x80)
			return -1; /* not a continuation byte */
		result <<= 6;
		result |= c[i] & 0x3f;
	}

	if (result > UNICODE_MAX)
		return -1; /* value beyond unicode's 21-bit max */
	if (result >= 0xD800 && result <= 0xDFFF)
		return -1; /* surrogate chars */
	if (result >= 0xFDD0 && result <= 0xFDEF)
		return -1; /* non-character range */
	if ((result & 0xFFFE) == 0xFFFE)
		return -1; /* non-character at plane end */

	*out = result;
	return (size_t)len;
}

static ssize_t
utf8_encode(char *out, uint32_t c)
{
	size_t len = 0, first, i;

	if (c < 0x80) {
		first = 0;
		len = 1;
	} else if (c < 0x800) {
		/* XXX: we allow encoding surrogate chars, even
		 * though that's invalid UTF8 */
		first = 0xc0;
		len = 2;
	} else if (c < 0x10000) {
		first = 0xe0;
		len = 3;
	} else if (c < 0x110000) {
		first = 0xf0;
		len = 4;
	} else {
		return -1;
	}

	for (i = len - 1; i > 0; --i) {
		out[i] = (c & 0x3f) | 0x80;
		c >>= 6;
	}
	out[0] = c | first;

	return len;
}
