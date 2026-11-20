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
#include <stdlib.h>
#include <string.h>

/* Enable a DAPM mixer control by its exact name ("<widget> <control>"). */
static int set_ctl(const char *name)
{
	struct snd_ctl_elem_value val;
	int fd, ret = -1;

	fd = open("/dev/snd/controlC0", O_RDWR);
	if (fd < 0) { perror("open control"); return -1; }
	memset(&val, 0, sizeof(val));
	val.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	strncpy((char *)val.id.name, name, sizeof(val.id.name) - 1);
	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &val) < 0) {
		fprintf(stderr, "ctl read %s: %s\n", name, strerror(errno));
	} else {
		val.value.integer.value[0] = 1;
		val.value.integer.value[1] = 0;
		if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val) < 0)
			fprintf(stderr, "ctl write %s: %s\n", name, strerror(errno));
		else {
			fprintf(stderr, "route on: %s\n", name);
			ret = 0;
		}
	}
	close(fd);
	return ret;
}

static void enable_routes(void)
{
	set_ctl("SEC_MI2S_RX Audio Mixer MultiMedia1");
}

int main(int argc, char **argv)
{
	enable_routes();
	const char *dev = argc > 1 ? argv[1] : "/dev/snd/pcmC0D0p";
	struct snd_pcm_hw_params hp;
	short buf[4096];
	unsigned int rate = 48000, ch = 2, half, n = 0, c;

	if (argc > 2)
		ch = (unsigned int)atoi(argv[2]);
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
	{
		int k;

		/* "any" masks/intervals: the kernel rejects empty ones */
		for (k = SNDRV_PCM_HW_PARAM_FIRST_MASK; k <= SNDRV_PCM_HW_PARAM_LAST_MASK; k++)
			memset(hp.masks[k - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits, 0xff,
			       sizeof(hp.masks[0].bits));
		for (k = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; k <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; k++) {
			struct snd_interval *iv = &hp.intervals[k - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];

			memset(iv, 0, sizeof(*iv));
			iv->min = 0;
			iv->max = ~0U;
		}
	}
	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &hp) < 0) {
		perror("HW_REFINE");
		return 1;
	}
	/* fix only what we need; the kernel chooses the rest */
	{
		int fi = SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK;
		int ri = SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL;
		int ci = SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL;

		int ai = SNDRV_PCM_HW_PARAM_ACCESS - SNDRV_PCM_HW_PARAM_FIRST_MASK;
		int si = SNDRV_PCM_HW_PARAM_SUBFORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK;

		memset(hp.masks[ai].bits, 0, sizeof(hp.masks[ai].bits));
		hp.masks[ai].bits[SNDRV_PCM_ACCESS_RW_INTERLEAVED / 32] =
			1U << (SNDRV_PCM_ACCESS_RW_INTERLEAVED % 32);
		memset(hp.masks[fi].bits, 0, sizeof(hp.masks[fi].bits));
		hp.masks[fi].bits[SNDRV_PCM_FORMAT_S16_LE / 32] = 1U << (SNDRV_PCM_FORMAT_S16_LE % 32);
		memset(hp.masks[si].bits, 0, sizeof(hp.masks[si].bits));
		hp.masks[si].bits[SNDRV_PCM_SUBFORMAT_STD / 32] =
			1U << (SNDRV_PCM_SUBFORMAT_STD % 32);
		memset(&hp.intervals[ri], 0, sizeof(hp.intervals[ri]));
		hp.intervals[ri].min = rate;
		hp.intervals[ri].max = rate;
		hp.intervals[ri].integer = 1;
		memset(&hp.intervals[ci], 0, sizeof(hp.intervals[ci]));
		hp.intervals[ci].min = ch;
		hp.intervals[ci].max = ch;
		hp.intervals[ci].integer = 1;
		hp.rmask = (1U << ai) | (1U << fi) | (1U << si);
		hp.cmask = (1U << ri) | (1U << ci);
	}

	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) < 0) {
		perror("HW_PARAMS");
		return 1;
	}
	fprintf(stderr, "stage: write (buf=%u per=%u rate=%u ch=%u fmt=%u)\n",
		hp.intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min,
		hp.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min,
		hp.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min,
		hp.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min,
		hp.masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits[0]);

	if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0)
		fprintf(stderr, "prepare: %m\n");

	half = rate / 880;
	frames = (int)rate * 12; /* 12 s so a human can confirm */
	while (frames > 0) {
		int nf = frames > 1024 ? 1024 : frames;

		for (i = 0; i < nf; i++, n++) {
			short v = ((n / half) & 1) ? -12000 : 12000;

			/* every channel carries the tone: with more than one
			 * data line the amp may be listening on either */
			for (c = 0; c < ch; c++)
				buf[ch * i + c] = v;
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
