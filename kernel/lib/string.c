#include <stddef.h>
#include <stdint.h>

void *memset(void *dest, int value, size_t count)
{
	uint8_t *out = dest;
	for (size_t i = 0; i < count; ++i) {
		out[i] = (uint8_t)value;
	}
	return dest;
}

void *memcpy(void *dest, const void *src, size_t count)
{
	uint8_t *out = dest;
	const uint8_t *in = src;
	for (size_t i = 0; i < count; ++i) {
		out[i] = in[i];
	}
	return dest;
}

void *memmove(void *dest, const void *src, size_t count)
{
	uint8_t *out = dest;
	const uint8_t *in = src;

	if (out == in || count == 0) {
		return dest;
	}
	if (out < in) {
		for (size_t i = 0; i < count; ++i) {
			out[i] = in[i];
		}
		return dest;
	}

	for (size_t i = count; i != 0; --i) {
		out[i - 1U] = in[i - 1U];
	}
	return dest;
}

int memcmp(const void *lhs, const void *rhs, size_t count)
{
	const uint8_t *a = lhs;
	const uint8_t *b = rhs;
	for (size_t i = 0; i < count; ++i) {
		if (a[i] != b[i]) {
			return (int)a[i] - (int)b[i];
		}
	}
	return 0;
}

size_t strlen(const char *text)
{
	size_t len = 0;
	while (text[len] != '\0') {
		++len;
	}
	return len;
}

