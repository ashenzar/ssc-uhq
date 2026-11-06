/* Decode SSC frames on the host with the patched v1.1 decoder (ssc-uhq T3.3).
 * usage: ssc_native_decode FRAMES MODE OUT.wav
 * FRAMES is `u32 len | bytes` per frame (header included), as the blob helper writes them
 * and as ~/src/ssc-capture/uhq584_getlucky.frames is stored. MODE is 96000 for the 96 kHz
 * mode, anything else for the 44.1/48 kHz one; OUT is written at that rate (48000 for the
 * basic mode, the buds' DAC rate, F20), 16-bit stereo, the decoder's delay removed.
 * Checks each frame as the harness's decode test does: a known header, no range-decoder
 * error, and the range coder ending inside the payload, either within 1 bit of its end or
 * where the encoder's zero padding starts. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ssc_native_dec.h"

static void put32(unsigned char *p, uint32_t v)
{
   p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

int main(int argc, char **argv)
{
   if (argc < 4) {
      fprintf(stderr, "usage: %s FRAMES MODE OUT.wav\n", argv[0]);
      return 2;
   }
   int mode96 = atoi(argv[2]) == 96000, rate = mode96 ? 96000 : 48000;
   FILE *f = fopen(argv[1], "rb");
   if (!f) {
      perror(argv[1]);
      return 1;
   }
   struct ssc_dec d;
   int err = ssc_dec_init(&d, mode96);
   if (err) {
      fprintf(stderr, "decoder init failed %d\n", err);
      return 1;
   }
   size_t cap = 1 << 16, frames = 0;
   int16_t *out = malloc(cap * 1728 * sizeof *out);
   unsigned char pkt[4096];
   uint32_t len;
   int hdr_bad = 0, dec_bad = 0, sync_bad = 0, gap_max = 0, padded = 0, end_n[23] = {0};
   int rates[16] = {0}, nrates = 0, rate_n[16] = {0};
   while (fread(&len, 4, 1, f) == 1) {
      if (len > sizeof pkt || fread(pkt, 1, len, f) != len) {
         fprintf(stderr, "frame %zu: bad length %u\n", frames, len);
         return 1;
      }
      if (frames == cap)
         out = realloc(out, (cap *= 2) * 1728 * sizeof *out);
      struct ssc_header h;
      int ret = ssc_dec_frame(&d, pkt, (int)len, out + frames * 1728, &h);
      if (ret == -1) {
         if (hdr_bad++ < 5)
            fprintf(stderr, "frame %zu: unknown header %02x %02x %02x %02x\n", frames, pkt[0], pkt[1], pkt[2], pkt[3]);
         memset(out + frames * 1728, 0, 1728 * sizeof *out);
         frames++;
         continue;
      }
      int k = 0;
      while (k < nrates && rates[k] != h.bitrate)
         k++;
      if (k == nrates && nrates < 16)
         rates[nrates++] = h.bitrate;
      rate_n[k]++;
      end_n[h.end]++;
      if (ret < 0 && dec_bad++ < 5)
         fprintf(stderr, "frame %zu: range error or short decode\n", frames);
      /* Where the range coder stopped, against the payload's trailing zeros. */
      int gap = 8 * ((int)len - 4) - ssc_dec_tell(&d), z = 0, zmax = 0;
      for (uint32_t i = 4; i < len; i++) {
         z = pkt[i] ? 0 : z + 1;
         zmax = z > zmax ? z : zmax;
      }
      if (gap > 1)
         padded++;
      else if (gap > gap_max)
         gap_max = gap;
      if (gap < 0 || (gap > 1 && zmax < gap / 8 - 1)) {
         if (sync_bad++ < 5)
            fprintf(stderr, "frame %zu: range coder ends %d bits before the payload end, zero run %d bytes\n",
                    frames, gap, zmax);
      }
      frames++;
   }
   fclose(f);
   ssc_dec_destroy(&d);

   printf("%zu frames:", frames);
   for (int k = 0; k < nrates; k++)
      printf(" %d at %d bit/s;", rate_n[k], rates[k]);
   printf(" end");
   for (int e = 0; e <= 22; e++)
      if (end_n[e])
         printf(" %d:%d", e, end_n[e]);
   printf("\n%d unknown headers, %d decode errors, %d out of sync; %d padded, tell gap max %d bits otherwise\n",
          hdr_bad, dec_bad, sync_bad, padded, gap_max);

   long total = frames > 0 ? (long)frames * 864 - SSC_DEC_DELAY : 0;
   unsigned char h[44] = "RIFF....WAVEfmt \x10\0\0\0\1\0\2\0........\4\0\x10\0data....";
   put32(h + 4, 36 + (uint32_t)total * 4);
   put32(h + 24, rate);
   put32(h + 28, rate * 4);
   put32(h + 40, (uint32_t)total * 4);
   FILE *o = fopen(argv[3], "wb");
   if (!o || fwrite(h, 44, 1, o) != 1 ||
       fwrite(out + 2 * SSC_DEC_DELAY, 4, (size_t)total, o) != (size_t)total || fclose(o)) {
      perror(argv[3]);
      return 1;
   }
   free(out);
   return hdr_bad || dec_bad || sync_bad;
}
