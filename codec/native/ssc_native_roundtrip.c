/* Encode with the native encoder and decode with the patched v1.1 decoder (patches 0010,
 * 0013), both on the host: the round trip for listening, spectrograms and measurements
 * (ssc-uhq T3.3, T6.1). The decoder setup is the harness's decode_one().
 * usage: ssc_native_roundtrip PCM.s32 RATE BITRATE OUT.wav [BITS [SHIFT]]
 * PCM is S32LE stereo at 2^31, shifted right by SHIFT (default: the plugin's level, as in
 * ssc_native_bench). OUT is 16-bit stereo WAV at RATE with the decoder's 108-sample delay
 * removed; the 24-bit path comes out 8 bits down, the 16-bit path at its own scale. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ssc_native_dec.h"

#define DELAY SSC_DEC_DELAY

static void put32(unsigned char *p, uint32_t v)
{
   p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static int write_wav(const char *path, const int16_t *pcm, long frames, int rate)
{
   unsigned char h[44] = "RIFF....WAVEfmt \x10\0\0\0\1\0\2\0........\4\0\x10\0data....";
   uint32_t bytes = (uint32_t)frames * 4;
   put32(h + 4, 36 + bytes);
   put32(h + 24, rate);
   put32(h + 28, rate * 4);
   put32(h + 40, bytes);
   FILE *f = fopen(path, "wb");
   if (!f || fwrite(h, 44, 1, f) != 1 || fwrite(pcm, 4, frames, f) != (size_t)frames) {
      perror(path);
      return 1;
   }
   return fclose(f) != 0;
}

int main(int argc, char **argv)
{
   if (argc < 5) {
      fprintf(stderr, "usage: %s PCM.s32 RATE BITRATE OUT.wav [BITS [SHIFT]]\n", argv[0]);
      return 2;
   }
   int rate = atoi(argv[2]), bitrate = atoi(argv[3]), bits = argc > 5 ? atoi(argv[5]) : 34;
   int wide = rate == 96000 || bitrate == 256000 || bitrate == 328000 || bits == 24;
   int shift = argc > 6 ? atoi(argv[6]) : wide ? 8 : 16;
   FILE *f = fopen(argv[1], "rb");
   if (!f) {
      perror(argv[1]);
      return 1;
   }
   fseek(f, 0, SEEK_END);
   long frames = ftell(f) / (864 * 8);
   fseek(f, 0, SEEK_SET);
   int32_t *in = malloc((size_t)frames * 864 * 8);
   if (!in || fread(in, 864 * 8, (size_t)frames, f) != (size_t)frames)
      return 1;
   fclose(f);
   for (long i = 0; i < frames * 1728; i++)
      in[i] >>= shift;

   int err;
   struct ssc_encoder *e = ssc_native_create(rate, 2, bits, &err);
   if (!e) {
      fprintf(stderr, "create failed %d\n", err);
      return 1;
   }
   struct ssc_dec d;
   if ((err = ssc_dec_init(&d, rate >= 96000))) {
      fprintf(stderr, "decoder init failed %d\n", err);
      return 1;
   }

   int16_t *out = calloc((size_t)(frames + 1) * 864 * 2, sizeof *out);
   int32_t pcm[1728];
   unsigned char pkt[4096];
   int end_min = 22, end_max = 0, bad = 0;
   long bytes = 0;
   for (long n = 0; n < frames; n++) {
      memcpy(pcm, in + n * 1728, sizeof pcm);
      int len = ssc_native_encode(e, pcm, 864, pkt, bitrate);
      struct ssc_header h;
      int ret = len < 4 ? -1 : ssc_dec_frame(&d, pkt, len, out + n * 1728, &h);
      if (ret == -1 || h.bitrate != bitrate) {
         fprintf(stderr, "frame %ld: encode returned %d, header bitrate %d\n", n, len, ret == -1 ? 0 : h.bitrate);
         return 1;
      }
      bytes += len;
      end_min = h.end < end_min ? h.end : end_min;
      end_max = h.end > end_max ? h.end : end_max;
      if (ret < 0 && bad++ < 10)
         fprintf(stderr, "frame %ld: range error or short decode\n", n);
   }
   ssc_dec_destroy(&d);
   ssc_native_destroy(e);

   /* Fit at the decoder delay, both channels, from frame 2 (the encoder's start-up). */
   long total = (frames - 1) * 864;
   double io = wide ? 1. / 256 : 1., xx = 0, xy = 0, yy = 0, ee = 0;
   for (long s = 2 * 864; s < total; s++)
      for (int c = 0; c < 2; c++) {
         double x = in[2 * s + c] * io, y = out[2 * (s + DELAY) + c];
         xx += x * x; xy += x * y; yy += y * y; ee += (y - x) * (y - x);
      }
   double g = xx > 0 ? xy / xx : 0, res = yy - g * xy;
   printf("%d/%d bits=%d: %ld frames, %ld bytes, end %d..%d, %d bad frames; "
          "snr %.2f dB, fitted gain %.4f (snr %.2f dB)\n",
          rate, bitrate, bits, frames, bytes, end_min, end_max, bad,
          10 * log10(xx / (ee > 1e-9 ? ee : 1e-9)), g, 10 * log10(g * xy / (res > 1e-9 ? res : 1e-9)));
   int r = write_wav(argv[4], out + 2 * DELAY, total, rate);
   free(out);
   free(in);
   return r || bad;
}
