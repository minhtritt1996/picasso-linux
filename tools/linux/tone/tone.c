/* Minimal ALSA playback smoke test: 3 s of a 440 Hz square tone on the first PCM.
 * Uses the kernel uapi directly (no alsa-lib); hw_params are filled by hand the
 * way aplay does for S16_LE stereo 48 kHz, 4 periods of 1024 frames. */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sound/asound.h>

static void set_mask(struct snd_pcm_hw_params *p, int var, unsigned int bit)
{
	p->masks[var].bits[bit / 32] |= 1u << (bit % 32);
}

static void set_int(struct snd_pcm_hw_params *p, int var, unsigned int lo, unsigned int hi)
{
	p->intervals[var - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = lo;
	p->intervals[var - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = hi;
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/snd/pcmC0D0p";
	struct snd_pcm_hw_params hp;
	short buf[4096];
	unsigned int rate = 48000, ch = 2, half, n = 0;
	int fd, i, frames;

	{
		int cfd = open("/dev/snd/controlC0", O_RDWR);
		fprintf(stderr, "control open -> %d (%s)\n", cfd, cfd < 0 ? strerror(errno) : "ok");
		if (cfd >= 0)
			close(cfd);
	}
	{
		static const int flags[] = { O_RDWR, O_WRONLY, O_RDWR | O_NONBLOCK, O_WRONLY | O_NONBLOCK };
		static const char *const nm[] = { "O_RDWR", "O_WRONLY", "O_RDWR|NB", "O_WRONLY|NB" };
		int i;

		for (i = 0; i < 4; i++) {
			fd = open(dev, flags[i]);
			fprintf(stderr, "open %s -> %d (%s)\n", nm[i], fd,
				fd < 0 ? strerror(errno) : "ok");
			if (fd >= 0)
				break;
		}
		if (fd < 0)
			return 1;
	}
	fprintf(stderr, "stage: hw_params\n");

	memset(&hp, 0, sizeof(hp));
	set_mask(&hp, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	set_mask(&hp, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
	set_mask(&hp, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	set_int(&hp, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16, 16);
	set_int(&hp, SNDRV_PCM_HW_PARAM_FRAME_BITS, 32, 32);
	set_int(&hp, SNDRV_PCM_HW_PARAM_CHANNELS, 2, 2);
	set_int(&hp, SNDRV_PCM_HW_PARAM_RATE, rate, rate);
	set_int(&hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, 1024, 1024);
	set_int(&hp, SNDRV_PCM_HW_PARAM_PERIODS, 4, 4);
	set_int(&hp, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, 4096, 4096);
	set_int(&hp, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 4096 * 4, 4096 * 4);
	hp.rmask = ~0U;
	hp.cmask = ~0U;

	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) < 0) {
		perror("HW_PARAMS");
		return 1;
	}
	fprintf(stderr, "stage: write\n");

	half = rate / 880;
	frames = (int)rate * 3;
	while (frames > 0) {
		int nf = frames > 1024 ? 1024 : frames;

		for (i = 0; i < nf; i++, n++) {
			short v = ((n / half) & 1) ? -12000 : 12000;

			buf[2 * i] = v;
			buf[2 * i + 1] = v;
		}
		if (write(fd, buf, (size_t)nf * ch * sizeof(short)) < 0) {
			perror("write");
			break;
		}
		frames -= nf;
	}
	ioctl(fd, SNDRV_PCM_IOCTL_DRAIN, 0);
	close(fd);
	fprintf(stderr, "tone done\n");
	return 0;
}
