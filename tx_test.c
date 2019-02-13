#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/socket.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <linux/types.h>

#include "include/linux/lora.h"

#ifndef AF_LORA
#define AF_LORA 45
#endif

#ifndef PF_LORA
#define PF_LORA AF_LORA
#endif

#define MSG(args...) fprintf(stderr, args) /* message that is destined to the user */

/* describe command line options */
void usage(void) {
	printf("Available options:\n");
	printf(" -h                 print this help\n");
	printf(" -f         <float> target frequency in MHz\n");
	printf(" -b         <uint>  LoRa bandwidth in kHz [125, 250, 500]\n");
	printf(" -s         <uint>  LoRa Spreading Factor [7-12]\n");
	printf(" -c         <uint>  LoRa Coding Rate 4/x [5-8]\n");
	printf(" -p         <int>   RF power (dBm)\n");
}

int main(int argc, char **argv)
{
	/* user entry parameters */
	int xi = 0;
	unsigned int xu = 0;
	double xd = 0.0;
	float xf = 0.0;

	__u8 sf;
	__u8 cr;
	__u16 bw;
	__u64 freq;
	__s8 pow;
	char data[LORA_MTU];
	int len = 0;
	int ret;

	while ((ret = getopt(argc, argv, "hf:b:s:c:p:")) != -1) {
		switch (ret) {
			case 'f':
				ret = sscanf(optarg, "%lf", &xd);
				if ((ret != 1) || (xd < 30.0) || (xd > 3000.0)) {
					MSG("ERROR: invalid TX frequency\n");
					usage();
					return EXIT_FAILURE;
				} else {
					freq = (uint32_t)((xd*1e6) + 0.5); /* .5 Hz offset to get rounding instead of truncating */
				}
				break;
			case 'b':
				ret = sscanf(optarg, "%i", &xi);
				if ((ret != 1) || ((xi != 125) && (xi != 250) && (xi != 500))) {
					MSG("ERROR: invalid LoRa bandwidth\n");
					usage();
					return EXIT_FAILURE;
				} else {
					bw = xi;
				}
				break;
			case 's':
				ret = sscanf(optarg, "%i", &xi);
				if ((ret != 1) || (xi < 7) || (xi > 12)) {
					MSG("ERROR: invalid spreading factor\n");
					usage();
					return EXIT_FAILURE;
				} else {
					sf = xi;
				}
				break;
			case 'c':
				ret = sscanf(optarg, "%i", &xi);
				if ((ret != 1) || (xi < 5) || (xi > 8)) {
					MSG("ERROR: invalid coding rate\n");
					usage();
					return EXIT_FAILURE;
				} else {
					cr = xi;
				}
				break;
			case 'p':
				ret = sscanf(optarg, "%i", &xi);
				if ((ret != 1) || (xi < -60) || (xi > 60)) {
					MSG("ERROR: invalid RF power\n");
					usage();
					return EXIT_FAILURE;
				} else {
					pow = xi;
				}
				break;
			case 'h':
			default:
				usage();
				return EXIT_FAILURE;
		}
	}
	while ((optind < argc) && (len < LORA_MTU)) {
		len += snprintf(data + len, LORA_MTU - len, "%s ", argv[optind++]);
	}

	int skt = socket(PF_LORA, SOCK_DGRAM, 1);
	if (skt == -1) {
		int err = errno;
		fprintf(stderr, "socket failed: %s\n", strerror(err));
		return 1;
	}
	printf("socket %d\n", skt);

	struct ifreq ifr;
	strcpy(ifr.ifr_name, "lora0");
	ret = ioctl(skt, SIOCGIFINDEX, &ifr);
	if (ret == -1) {
		int err = errno;
		fprintf(stderr, "ioctl failed: %s\n", strerror(err));
		return 1;
	}
	printf("ifindex %d\n", ifr.ifr_ifindex);

	struct sockaddr_lora addr;
	addr.lora_family = AF_LORA;
	addr.lora_ifindex = ifr.ifr_ifindex;
	addr.lora_addr.tx.sf = 7;
	addr.lora_addr.tx.cr = 5;
	addr.lora_addr.tx.bw = 125;
	addr.lora_addr.tx.freq = 868000000;

	if (sf)
		addr.lora_addr.tx.sf = sf;
	if (cr)
		addr.lora_addr.tx.cr = cr;
	if (bw)
		addr.lora_addr.tx.bw = bw;
	if (freq)
		addr.lora_addr.tx.freq = freq;
	if (pow)
		addr.lora_addr.tx.power = pow;

	ret = bind(skt, (struct sockaddr *)&addr, sizeof(addr));
	if (ret == -1) {
		int err = errno;
		fprintf(stderr, "bind failed: %s\n", strerror(err));
		return 1;
	}

	int bytes_sent = write(skt, &data, len);
	if (bytes_sent == -1) {
		int err = errno;
		fprintf(stderr, "write failed: %s\n", strerror(err));
		return 1;
	}
	printf("bytes_sent %d\n", bytes_sent);

	return 0;
}
