/* Time the native encoder in-process (ssc-uhq AC5: < 5 % of one core at uhq-584).
 * usage: ssc_native_bench PCM.s32 RATE BITRATE [BITS [SHIFT]]
 * PCM is S32LE stereo at 2^31, shifted right by SHIFT (default: the plugin's level). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "ssc_native.h"

static int cmp(const void *a, const void *b)
{
   double x = *(const double *)a, y = *(const double *)b;
   return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
   if (argc < 4) {
      fprintf(stderr, "usage: %s PCM.s32 RATE BITRATE [BITS [SHIFT]]\n", argv[0]);
      return 2;
   }
   int rate = atoi(argv[2]), bitrate = atoi(argv[3]), bits = argc > 4 ? atoi(argv[4]) : 34;
   int wide = rate == 96000 || bitrate == 256000 || bitrate == 328000 || bits == 24;
   int shift = argc > 5 ? atoi(argv[5]) : wide ? 8 : 16;
   FILE *f = fopen(argv[1], "rb");
   if (!f) {
      perror(argv[1]);
      return 1;
   }
   fseek(f, 0, SEEK_END);
   long frames = ftell(f) / (864 * 8);
   fseek(f, 0, SEEK_SET);
   int32_t *pcm = malloc((size_t)frames * 864 * 8);
   if (!pcm || fread(pcm, 864 * 8, (size_t)frames, f) != (size_t)frames)
      return 1;
   fclose(f);
   for (long i = 0; i < frames * 1728; i++)
      pcm[i] >>= shift;
   int err;
   struct ssc_encoder *e = ssc_native_create(rate, 2, bits, &err);
   if (!e) {
      fprintf(stderr, "create failed %d\n", err);
      return 1;
   }
   double *t = malloc(sizeof *t * (size_t)frames), total = 0;
   unsigned char out[4096];
   long bytes = 0;
   for (long n = 0; n < frames; n++) {
      struct timespec a, b;
      clock_gettime(CLOCK_MONOTONIC, &a);
      int len = ssc_native_encode(e, pcm + n * 1728, 864, out, bitrate);
      clock_gettime(CLOCK_MONOTONIC, &b);
      if (len <= 0) {
         fprintf(stderr, "frame %ld: encode returned %d\n", n, len);
         return 1;
      }
      bytes += len;
      t[n] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6;
      total += t[n];
   }
   qsort(t, (size_t)frames, sizeof *t, cmp);
   double budget = 864.0 * 1e3 / rate;
   printf("%d/%d bits=%d: %ld frames, %ld bytes; encode p50 %.3f ms p99 %.3f ms max %.3f ms; "
          "%.2f %% of one core at real time (budget %.2f ms/frame)\n",
          rate, bitrate, bits, frames, bytes, t[frames / 2], t[frames * 99 / 100], t[frames - 1],
          100 * total / (frames * budget), budget);
   ssc_native_destroy(e);
   return 0;
}
