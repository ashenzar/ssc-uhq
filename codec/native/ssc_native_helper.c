/* Drop-in for openssc's ssc_blob_helper with the native encoder (ssc-uhq O5, T5.1). Same
 * protocol: argv [blob.so] rate channels bits bitrate (a leading .so path is ignored, so
 * the blob launcher's argument list works); writes an int32 0 when ready; then per frame
 * reads u32 n and int32 pcm[n * channels], and writes int32 len and len bytes.
 *
 * Tee mode (T5.2): with SSCENC_TEE_CMD set to a blob helper command (run by /bin/sh with
 * the same four arguments appended), every frame also goes through the blob. The blob's
 * frame is what the plugin gets; a differing native frame is reported on stderr. */
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ssc_native.h"

#define FRAME 864u
#define MAX_ENCODE_BYTES 4096u

static int read_full(int fd, void *buf, size_t n)
{
   uint8_t *p = buf;
   while (n) {
      ssize_t r = read(fd, p, n);
      if (r == 0)
         return 0;
      if (r < 0) {
         if (errno == EINTR)
            continue;
         return -1;
      }
      p += (size_t)r;
      n -= (size_t)r;
   }
   return 1;
}

static int write_full(int fd, const void *buf, size_t n)
{
   const uint8_t *p = buf;
   while (n) {
      ssize_t w = write(fd, p, n);
      if (w < 0) {
         if (errno == EINTR)
            continue;
         return -1;
      }
      p += (size_t)w;
      n -= (size_t)w;
   }
   return 0;
}

static long parse_long(const char *s)
{
   char *end = NULL;
   errno = 0;
   long v = strtol(s, &end, 10);
   if (errno || !end || *end) {
      fprintf(stderr, "invalid integer argument: %s\n", s);
      exit(2);
   }
   return v;
}

/* Start the blob helper for tee mode: sh -c "$SSCENC_TEE_CMD \"$@\"" sh rate ch bits bitrate. */
static pid_t start_tee(const char *cmd, char **args, int *to, int *from)
{
   int in[2], out[2];
   if (pipe(in) < 0 || pipe(out) < 0)
      return -1;
   pid_t pid = fork();
   if (pid < 0)
      return -1;
   if (pid == 0) {
      dup2(in[0], STDIN_FILENO);
      dup2(out[1], STDOUT_FILENO);
      close(in[0]);
      close(in[1]);
      close(out[0]);
      close(out[1]);
      char script[4096];
      snprintf(script, sizeof script, "%s \"$@\"", cmd);
      unsetenv("SSCENC_TEE_CMD"); /* the blob side must not tee again */
      execl("/bin/sh", "sh", "-c", script, "sh", args[1], args[2], args[3], args[4], (char *)NULL);
      _exit(127);
   }
   close(in[0]);
   close(out[1]);
   *to = in[1];
   *from = out[0];
   int32_t ready;
   if (read_full(*from, &ready, sizeof ready) <= 0 || ready != 0) {
      fprintf(stderr, "tee: blob helper did not start\n");
      return -1;
   }
   return pid;
}

int main(int argc, char **argv)
{
   signal(SIGPIPE, SIG_IGN);
   if (argc == 6) {
      argv++;
      argc--;
   }
   if (argc != 5) {
      fprintf(stderr, "usage: %s [blob.so] sample-rate channels bit-depth bitrate\n", argv[0]);
      return 2;
   }
   int rate = (int)parse_long(argv[1]), channels = (int)parse_long(argv[2]);
   int bits = (int)parse_long(argv[3]), bitrate = (int)parse_long(argv[4]);
   int err = -1;
   struct ssc_encoder *e = ssc_native_create(rate, channels, bits, &err);
   if (!e) {
      fprintf(stderr, "ssc_native_create(%d,%d,%d) failed err=%d\n", rate, channels, bits, err);
      return 4;
   }
   static int32_t pcm[FRAME * 2], pcm_blob[FRAME * 2];
   static uint8_t out[MAX_ENCODE_BYTES], out_blob[MAX_ENCODE_BYTES];
   const char *tee = getenv("SSCENC_TEE_CMD");
   int tee_to = -1, tee_from = -1;
   long frames = 0, mismatches = 0;
   if (tee && *tee && start_tee(tee, argv, &tee_to, &tee_from) < 0)
      return 4;
   int32_t ready = 0;
   if (write_full(STDOUT_FILENO, &ready, sizeof ready) < 0)
      return 9;
   for (;;) {
      uint32_t n = 0;
      int rr = read_full(STDIN_FILENO, &n, sizeof n);
      if (rr == 0)
         break;
      if (rr < 0) {
         perror("read frame_samples");
         return 6;
      }
      if (n != FRAME) {
         fprintf(stderr, "frame_samples=%u, the native encoder takes %u\n", n, FRAME);
         return 7;
      }
      if (read_full(STDIN_FILENO, pcm, (size_t)n * (size_t)channels * sizeof *pcm) <= 0) {
         fprintf(stderr, "short pcm read\n");
         return 8;
      }
      size_t pcm_bytes = (size_t)n * (size_t)channels * sizeof *pcm;
      if (tee_to >= 0) {
         memcpy(pcm_blob, pcm, pcm_bytes); /* both encoders limit the input in place */
         if (write_full(tee_to, &n, sizeof n) < 0 || write_full(tee_to, pcm_blob, pcm_bytes) < 0)
            return 9;
      }
      memset(out, 0, sizeof out);
      int ret = ssc_native_encode(e, pcm, (int)n, out, bitrate);
      if (tee_to >= 0) {
         int32_t bret;
         if (read_full(tee_from, &bret, sizeof bret) <= 0 ||
             (bret > 0 && (bret > (int)MAX_ENCODE_BYTES || read_full(tee_from, out_blob, (size_t)bret) <= 0))) {
            fprintf(stderr, "tee: blob helper died\n");
            return 9;
         }
         if (bret != ret || (ret > 0 && memcmp(out, out_blob, (size_t)ret))) {
            if (mismatches++ < 10 || mismatches % 1000 == 0)
               fprintf(stderr, "tee: frame %ld differs (blob %d bytes, native %d), %ld so far\n", frames,
                       bret, ret, mismatches);
         }
         ret = bret;
         memcpy(out, out_blob, ret > 0 ? (size_t)ret : 0);
         if (++frames % 100000 == 0)
            fprintf(stderr, "tee: %ld frames, %ld differ\n", frames, mismatches);
      }
      if (write_full(STDOUT_FILENO, &ret, sizeof ret) < 0)
         return 9;
      if (ret > 0 && write_full(STDOUT_FILENO, out, (size_t)ret) < 0)
         return 9;
   }
   if (tee_to >= 0) {
      fprintf(stderr, "tee: done, %ld frames, %ld differ\n", frames, mismatches);
      close(tee_to);
      close(tee_from);
      wait(NULL);
   }
   ssc_native_destroy(e);
   return 0;
}
