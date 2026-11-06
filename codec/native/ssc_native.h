/* Native SSC encoder API, the blob's create/encode/destroy (ssc-uhq F9, F10, F46). One
 * encoder per process: pitch_search's xcorr and alg_quant's scratch pointer are still
 * globals in the patched libopus (OD8). */
#ifndef SSC_NATIVE_H
#define SSC_NATIVE_H

#include <stdint.h>

struct ssc_encoder;

/* rate 96000, 48000 or 44100; channels 2; bits as ssc_encoder_create (34 on the phone). */
struct ssc_encoder *ssc_native_create(int rate, int channels, int bits, int *err);
/* frame_size 864; pcm interleaved int32, limited in place; out >= 1279 bytes. Returns the
 * frame length including the 4-byte header, or < 0. */
int ssc_native_encode(struct ssc_encoder *e, int32_t *pcm, int frame_size, unsigned char *out, int bitrate);
void ssc_native_destroy(struct ssc_encoder *e);
/* The tables the encoder runs on (modes, shared alloc table), for tools such as the decoder. */
struct ssc_tables;
const struct ssc_tables *ssc_native_tables(void);

#endif
