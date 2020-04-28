#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
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


#define LORA_MTU	255


#define MSG(args...) fprintf(stderr, args) /* message that is destined to the user */

int main(int argc, char **argv)
{
	__u8 sf;
	__u8 cr;
	__u16 bw;
	__u64 freq;
	char data[LORA_MTU];
	int len = 0;
	int ret;

	struct pollfd pollfds;

	pollfds.fd = socket(PF_LORA, SOCK_DGRAM, 1);
	if (pollfds.fd == -1) {
		int err = errno;
		fprintf(stderr, "socket failed: %s\n", strerror(err));
		return 1;
	}
	printf("socket %d\n", pollfds.fd);

	struct ifreq ifr;
	strcpy(ifr.ifr_name, "lora0");
	ret = ioctl(pollfds.fd, SIOCGIFINDEX, &ifr);
	if (ret == -1) {
		int err = errno;
		fprintf(stderr, "ioctl failed: %s\n", strerror(err));
		return 1;
	}
	printf("ifindex %d\n", ifr.ifr_ifindex);

	struct sockaddr_lora addr;
	addr.lora_family = AF_LORA;
	addr.lora_ifindex = ifr.ifr_ifindex;
	addr.lora_addr.tx.freq = 868000000;

	ret = bind(pollfds.fd, (struct sockaddr *)&addr, sizeof(addr));
	if (ret == -1) {
		int err = errno;
		fprintf(stderr, "bind failed: %s\n", strerror(err));
		return 1;
	}

	pollfds.events = POLLIN;
	poll(&pollfds, 1, -1);

	int addr_len;
	len = recvfrom(pollfds.fd, (char *)data, LORA_MTU,
                0, (struct sockaddr *)&addr,
                &addr_len);
	if (len <= 0) {
		printf("Error recving\n");
	} else {
		printf("Freq: %llu, BW: %d, SF: %d, CR: %d\n", addr.lora_addr.tx.freq,
							   addr.lora_addr.tx.bw,
							   addr.lora_addr.tx.sf,
							   addr.lora_addr.tx.cr);

		char *buf = data;
		while (len--)
			printf("%02X ", *buf++);
		printf("\n");
	}

	close(pollfds.fd);
	return 0;
}
