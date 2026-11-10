// See test_dovi_rpu_check.sh. Each argument is one of libdovi's test RPUs:
// an Annex B start code and an escaped RPU starting with its 0x19 prefix,
// which this turns into the UNSPEC62 NAL VideoToolbox hands over (7C 01 and
// the escaped RPU).
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AVERROR_INVALIDDATA (-1)

#include "dovi_rpu_check.inc"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static int check(const uint8_t *nal, size_t size, const char **why)
{
    return ff_dovi_p7_rpu_check(nal, size, why);
}

static uint8_t *load_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *data = malloc(1 << 20);
    *size = fread(data, 1, 1 << 20, f);
    fclose(f);
    return data;
}

// libdovi's parse_rpu_file(): an RPU starts at every 00 00 00 01 and runs to
// the next, less one byte when that one is preceded by a zero. Returns the
// RPU count; starts[i]/sizes[i] cover each one's start code too.
static int split_rpus(const uint8_t *data, size_t size, size_t *starts, size_t *sizes, int max)
{
    size_t offsets[256];
    int n = 0;
    for (size_t i = 0; i + 4 <= size && n < 256; i++) {
        if (!data[i] && !data[i + 1] && !data[i + 2] && data[i + 3] == 1)
            offsets[n++] = i;
    }
    int count = 0;
    for (int i = 0; i < n && count < max; i++) {
        size_t len = (i + 1 < n ? offsets[i + 1] : size) - offsets[i];
        if (i + 1 < n && offsets[i] + len + 3 <= size && offsets[i] + len >= 1 &&
            !memcmp(data + offsets[i] + len - 1, "\0\0\0\1", 4))
            len--;
        starts[count] = offsets[i];
        sizes[count] = len;
        count++;
    }
    return count;
}

// One RPU as the UNSPEC62 NAL VideoToolbox hands over: 7C 01, then the
// escaped RPU that followed the start code.
static uint8_t *rpu_nal(const uint8_t *data, size_t start, size_t len, size_t *size)
{
    uint8_t *nal = malloc(len + 16);
    nal[0] = 0x7C;
    nal[1] = 0x01;
    memcpy(nal + 2, data + start + 4, len - 4);
    *size = len - 2;
    return nal;
}

// The NAL with one bit of the unescaped payload flipped, re-escaped, so the
// damage is a plausible corrupt RPU and not an escaping error.
static size_t flip_bit(const uint8_t *nal, size_t size, size_t bit, uint8_t *out)
{
    uint8_t *raw = malloc(size);
    size_t n = 0, o = 0;
    for (size_t i = 2; i < size; i++) {
        if (i >= 4 && !nal[i - 2] && !nal[i - 1] && nal[i] == 3)
            continue;
        raw[n++] = nal[i];
    }
    raw[1 + bit / 8] ^= 0x80 >> (bit % 8);
    out[o++] = 0x7C;
    out[o++] = 0x01;
    for (size_t i = 0, zeros = 0; i < n; i++) {
        if (zeros >= 2 && raw[i] <= 3) {
            out[o++] = 3;
            zeros = 0;
        }
        out[o++] = raw[i];
        zeros = raw[i] ? 0 : zeros + 1;
    }
    free(raw);
    return o;
}

int main(int argc, char **argv)
{
    const char *why;
    int accepted = 0;

    int files = 0;
    for (int a = 1; a < argc; a++) {
        size_t fsize, starts[64], sizes[64];
        uint8_t *file = load_file(argv[a], &fsize);
        int count = file ? split_rpus(file, fsize, starts, sizes, 64) : 0;
        CHECK(count > 0, "%s: no RPU", argv[a]);
        files += count > 0;
        for (int k = 0; k < count; k++) {
        size_t size;
        uint8_t *nal = rpu_nal(file, starts[k], sizes[k], &size);

        // Every RPU libdovi parses in its own tests is taken. (Its one
        // invalid asset, st2094_10_level3, is invalid in content libdovi
        // checks after parsing; its CRC32 is intact, so it is taken too.)
        int r = check(nal, size, &why);
        CHECK(r == 0, "%s #%d: refused (%s)", argv[a], k, why ? why : "?");
        accepted += r == 0;

        // Any single flipped bit in what the CRC32 covers is refused.
        uint8_t *bad = malloc(size * 2 + 16);
        for (size_t bit = 0; bit < 8 * 4; bit++) {
            size_t n = flip_bit(nal, size, bit, bad);
            CHECK(check(bad, n, &why) < 0, "%s #%d: bit %zu flipped was taken", argv[a], k, bit);
        }

        // Truncated anywhere past the minimum: refused (the CRC32 or the
        // final byte no longer line up).
        size_t trimmed = size;
        while (trimmed > 0 && !nal[trimmed - 1])
            trimmed--;
        for (size_t cut = 25; cut < trimmed; cut++) {
            CHECK(check(nal, cut, &why) < 0, "%s #%d: cut to %zu taken", argv[a], k, cut);
        }

        // Trailing zero bytes are trimmed, as libdovi does.
        memcpy(bad, nal, size);
        memset(bad + size, 0, 8);
        CHECK(check(bad, size + 8, &why) == 0, "%s #%d: trailing zeros refused (%s)",
              argv[a], k, why ? why : "?");
        free(bad);
        free(nal);
        }
        free(file);
    }
    CHECK(files > 10, "only %d libdovi assets", files);

    // The shape libdovi 3.3.2 panics on: valid start, zero padding.
    uint8_t pad[37] = { 0x7C, 0x01, 0x19, 0x08, 0x09 };
    CHECK(check(pad, sizeof(pad), &why) < 0 && !strcmp(why, "RPU data too short"),
          "zero-padded RPU: %s", why ? why : "taken");
    // Too short, wrong start, too large.
    CHECK(check(pad, 24, &why) < 0, "24 bytes taken");
    uint8_t wrong[40] = { 0x7C, 0x01, 0x19, 0x08, 0x0A, 1, 2, 3 };
    CHECK(check(wrong, sizeof(wrong), &why) < 0, "wrong prefix taken");
    uint8_t *big = calloc(1, 64 * 1024 + 1);
    memcpy(big, pad, 5);
    CHECK(check(big, 64 * 1024 + 1, &why) < 0 && !strcmp(why, "larger than 64 KiB"),
          "oversized: %s", why ? why : "taken");
    free(big);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("dovi rpu check: %d RPUs from %d libdovi assets taken, damaged and "
           "truncated ones refused\n", accepted, files);
    return 0;
}
