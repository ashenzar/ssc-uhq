/* SSC frame header (F34.3, F35). Four bytes, outside the range coder.
 *
 *   byte0 = 0xff
 *   byte1 = 0xee (F-mode) or 0xe0|(frame_counter>>4) (E-mode)
 *   byte2 bits 0-1 = base (init 1, stays 1). Bits 2-3 = idx in E-mode,
 *           or the F-mode 4-slice addend (0/4/8/12). Bits 4-7 = frame
 *           counter low nibble. Byte1's low nibble is the high half.
 *   byte3 = bitrate tag in the low 5 bits. E-mode rewrites the top 3 bits
 *           with the 8-slice code; the tag stays.
 *
 * UHQ-152 (tag 0xe6) is E-mode but skips the scan and always drops 4 bands.
 * F-mode never drops bands (end stays nbEBands). Do not invent a 96 kHz
 * mode paired with an F-mode bitrate (OD22): this parser only reads the header.
 */
#ifndef SSC_HEADER_H
#define SSC_HEADER_H

struct ssc_header {
   int bitrate;
   int e_mode;          /* 1: byte1 is 0xe0..0xef */
   int uhq;             /* no TF symbols, no 3/128 pad: E-mode, 256k and 328k (F46) */
   int end;             /* bands the range coder covers */
   int idx;             /* bands dropped from the top; 0 in F-mode */
   int per_mode_alloc;  /* bitrate > 192000 selects the per-mode alloc table */
   int tag;             /* byte3 & 0x1f */
};

/* Returns 0, or -1 if the four bytes are not a known SSC header. */
static int ssc_header_parse(const unsigned char *p, int len, int nbEBands,
                            struct ssc_header *h)
{
   /* Low 5 bits of byte 3. The top 3 bits are the E-mode slice code and vary per frame. */
   static const struct { unsigned char tag; int bitrate; int e_mode; } tags[] = {
      {0x10,  88000, 0}, {0x11,  96000, 0}, {0x12, 128000, 0}, {0x13, 192000, 0},
      {0x14, 229000, 0}, {0x15, 328000, 0}, {0x16, 256000, 0},
      {0x00, 291000, 1}, {0x01, 308000, 1}, {0x02, 442000, 1}, {0x03, 584000, 1},
      {0x04, 886000, 1}, {0x05, 250000, 1}, {0x06, 152000, 1},
   };
   int i, tag, e_mode;
   if (len < 4 || p[0] != 0xff || nbEBands < 4)
      return -1;
   tag = p[3] & 0x1f;
   e_mode = -1;
   h->bitrate = 0;
   for (i = 0; i < (int)(sizeof tags / sizeof tags[0]); i++) {
      if (tags[i].tag == tag) {
         h->bitrate = tags[i].bitrate;
         e_mode = tags[i].e_mode;
         break;
      }
   }
   if (!h->bitrate)
      return -1;
   /* E-mode byte1 is 0xe0 | (frame_counter >> 4). That collides with the
    * F-mode constant 0xee when the counter nibble is 0xe, so the tag decides. */
   if (e_mode) {
      if ((p[1] & 0xf0) != 0xe0)
         return -1;
   } else if (p[1] != 0xee)
      return -1;
   h->e_mode = e_mode;
   h->uhq = e_mode || h->bitrate == 256000 || h->bitrate == 328000;
   h->tag = tag;
   h->per_mode_alloc = h->bitrate > 192000;
   if (!e_mode) {
      h->idx = 0;
      h->end = nbEBands;
   } else if (tag == 0x06) { /* 0xe6: UHQ-152 forces end = nbEBands-4 */
      h->idx = 4;
      h->end = nbEBands - 4;
   } else {
      h->idx = (p[2] >> 2) & 3;
      h->end = nbEBands - h->idx;
   }
   return 0;
}

/* Bytes ssc_encode returns. fs is the mode's Fs (44100 for the shared
 * 44.1/48 kHz mode, 96000 for the 96 kHz mode), not the create() argument.
 * The uhq bitrates (E-mode plus 256k and 328k, F46) skip the 3/128 pad. */
static int ssc_frame_bytes(int bitrate, int fs, int frame_samples)
{
   int uhq = bitrate == 152000 || bitrate == 250000 || bitrate == 291000 ||
             bitrate == 308000 || bitrate == 442000 || bitrate == 584000 ||
             bitrate == 886000 || bitrate == 256000 || bitrate == 328000;
   int bytes = (int)(((long long)bitrate * frame_samples) / ((long long)fs * 8));
   int n = bytes + (uhq ? 0 : (bytes * 3) >> 7);
   n += (~n) & 1;
   return n + 3;
}

#endif
