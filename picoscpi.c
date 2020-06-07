#include <libps3000a/ps3000aApi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

int16_t g_picoHandle;
int g_dataclientfd = 0;
int g_wakeupfd = 0;

void fail(char *msg, int pico_err) {
	if (pico_err) {
		fprintf(stderr, "%s: %x", msg, pico_err);
	} else {
		perror(msg);
	}

	if (g_picoHandle > 0)
		ps3000aCloseUnit(g_picoHandle);
	exit(1);
}

struct pico_channel {
	int enabled;
	int coupling;
	int range;
	float offset;
} g_channels[4];

void updatePicoChannel(int channel) {
	int err = ps3000aSetChannel(g_picoHandle, channel, g_channels[channel].enabled,
		g_channels[channel].coupling, g_channels[channel].range, g_channels[channel].offset);
	if (err != PICO_OK)
		fail("SetChannel", err);
}

void initPicoscope() {
	int err;

	/* connect to the first picoscope */
	err = ps3000aOpenUnit(&g_picoHandle, NULL);
	if (err == PICO_POWER_SUPPLY_NOT_CONNECTED) {
		/* switch to USB power */
		/* FIXME: maybe require the user to specify this is ok */
		err = ps3000aChangePowerSource(g_picoHandle, PICO_POWER_SUPPLY_NOT_CONNECTED);
		if (err != PICO_OK)
			fail("ChangePowerSource", err);
	} else if (err != PICO_OK)
		fail("OpenUnit", err);

	/* say hello */
	ps3000aFlashLed(g_picoHandle, 2);

	for (int n = 0; n < 4; ++n) {
		g_channels[n].enabled = 0;
		g_channels[n].coupling = PS3000A_DC;
		g_channels[n].range = PS3000A_5V;
		g_channels[n].offset = 0.0f;
	}

	/* for now: hardcode a fixed signal for the generator: +/-2V square wave @ 1mhz */
	ps3000aSetSigGenBuiltInV2(g_picoHandle, 0, 4000000, PS3000A_SQUARE, 1000000.f, 1000000.f, 0, 0, 0, 0, 0, 0, 0, 0, 0);

	/* hardcode trigger: wait forever */
	struct tPS3000ATriggerChannelProperties channelProps = {
		1.0f / 5.0f * 0x7f00,
		1.0f / 5.0f * 0x7f00,
		1.0f / 5.0f * 0x7f00,
		1.0f / 5.0f * 0x7f00,
		0, // channel A
		PS3000A_LEVEL
	};
	struct tPS3000ATriggerConditionsV2 conditions = {
		PS3000A_CONDITION_TRUE,
		PS3000A_CONDITION_DONT_CARE,
		PS3000A_CONDITION_DONT_CARE,
		PS3000A_CONDITION_DONT_CARE,
		PS3000A_CONDITION_DONT_CARE,
		PS3000A_CONDITION_DONT_CARE,
		PS3000A_CONDITION_DONT_CARE,
		PS3000A_CONDITION_DONT_CARE
	};
	err = ps3000aSetTriggerChannelProperties(g_picoHandle, &channelProps, 1, 0, 0);
	if (err) fail("set trigger prop", err);
	err = ps3000aSetTriggerChannelConditionsV2(g_picoHandle, &conditions, 1);
	if (err) fail("set trigger cond", err);
	err = ps3000aSetTriggerChannelDirections(g_picoHandle, PS3000A_RISING, PS3000A_NONE, PS3000A_NONE, PS3000A_NONE, PS3000A_NONE, PS3000A_NONE);
	if (err) fail("set trigger dir", err);
	err = ps3000aSetTriggerDelay(g_picoHandle, 0);
	if (err) fail("set trigger delay", err);

	/* force chan 1 on */
	g_channels[0].enabled = 1;
	updatePicoChannel(0);

	/* more hardcoding: one capture at once, in rapid block mode */
	int32_t maxSamples;
	ps3000aMemorySegments(g_picoHandle, 1, &maxSamples);
	ps3000aSetNoOfCaptures(g_picoHandle, 1);
}

#define BUF_LEN 1024

void getPicoInfo(char *buf) {
	int err = 0;
	buf[0] = 0;
	int16_t tmp;
	for (int n = 0; n < 11; ++n) {
		err |= ps3000aGetUnitInfo(g_picoHandle, buf + strlen(buf), BUF_LEN - strlen(buf), &tmp, n);
		if (n != 10) strcat(buf, " ");
	}
	if (err)
		fail("GetUnitInfo", err);
}

int16_t sampbuf[1024 * 1024];

void blockCallback(int16_t handle, PICO_STATUS status, void *p) {
	if (status != PICO_OK) {
		fprintf(stderr, "blockCallback: got %x\n", status);
		return;
	}

	int err;

	printf("blockCallback\n");
	err = ps3000aSetDataBuffer(g_picoHandle, 0, sampbuf, sizeof(sampbuf), 0, PS3000A_RATIO_MODE_NONE);
	if (err != PICO_OK)
		fail("SetDataBuffer", err);
	uint32_t numSamples = sizeof(sampbuf);
	int16_t overflow = 0; // bitfield for overvoltage notification
	err = ps3000aGetValuesBulk(g_picoHandle, &numSamples, 0, 0, 0, PS3000A_RATIO_MODE_NONE, &overflow);
	if (err != PICO_OK)
		fail("GetValuesBulk", err);

	printf("first val is %d\n", sampbuf[0]);
	send(g_dataclientfd, sampbuf, numSamples * sizeof(uint16_t), 0);

	char meh = 0;
	write(g_wakeupfd, &meh, 1);
}

void goPicoGo() {
	/* woohoo let's go */
	int err = ps3000aRunBlock(g_picoHandle,
		0,     /* pre-trigger samples */
		10000, /* post-trigger samples */
		256,   /* timebase */
		1,     /* ??? */
		NULL,
		0,     /* segment idx */
		&blockCallback,
		NULL   /* callback data */
		);
	if (err)
		fail("RunBlock", err);
}

void handleSCPI(int fd, char *input) {
	char buf[BUF_LEN];
	/* strip line endings */
	while (strlen(input) > 0 && (input[strlen(input)-1] == '\r' || input[strlen(input)-1] == '\n'))
		input[strlen(input)-1] = 0;

	printf("received: %s\n", input);

	char *postcolon = strchr(input, ':');
	if (postcolon) postcolon++;

	// *IDN?
	if (!strcmp(input, "*IDN?")) {
		getPicoInfo(buf);
		buf[strlen(buf)+1] = 0;
		buf[strlen(buf)] = '\n';
		write(fd, buf, strlen(buf));
		return;
	}

	if (!strncmp(input, "CH", 2) && input[3] == ':') {
		// CH[1-4]
		int channel = input[2] - '1';
		if (channel < 0 || channel > 3)
			return;
		if (!strcmp(postcolon, "EN")) {
			g_channels[channel].enabled = 1;
			updatePicoChannel(channel);
		} else if (!strcmp(postcolon, "DIS")) {
			g_channels[channel].enabled = 0;
			updatePicoChannel(channel);
		} else {
			fprintf(stderr, "unknown channel command: %s\n", postcolon);
			return;
		}
		return;
	}

	fprintf(stderr, "unknown command: %s\n", input);
}

int main(int argc, char **argv) {
	uint16_t portno = 5025;

	initPicoscope();

	int pipes[2];
	if (pipe(pipes) < 0)
		fail("pipe", 0);
	g_wakeupfd = pipes[1];
	fcntl(pipes[0], F_SETFL, O_NONBLOCK);

	int mainfd = socket(AF_INET, SOCK_STREAM, 0);
	int datafd = socket(AF_INET, SOCK_STREAM, 0);

	/* set SO_REUSEADDR */
	int optval = 1;
	setsockopt(mainfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int));
	setsockopt(datafd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int));

	/* setup listening addr struct */
	struct sockaddr_in serveraddr;
	memset(&serveraddr, 0, sizeof(serveraddr));
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
	serveraddr.sin_port = htons(portno);

	/* bind */
	if (bind(mainfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0)
		fail("bind", 0);
	/* only allow one connection */
	if (listen(mainfd, 1) < 0)
		fail("listen", 0);

	/* repeat for data socket */
	serveraddr.sin_port = htons(50101);
	if (bind(datafd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0)
		fail("bind(d)", 0);
	if (listen(datafd, 1) < 0)
		fail("listen(d)", 0);

	int maxfd = datafd;
	int mainclientfd = 0;
	int dataclientfd = 0;
	while (1) {
		/* have to wakeup pico from this thread.. */
		int runPico = 0;

		fd_set readfds;

		FD_ZERO(&readfds);
		FD_SET(mainfd, &readfds);
		FD_SET(datafd, &readfds);
		FD_SET(pipes[0], &readfds);
		if (mainclientfd)
			FD_SET(mainclientfd, &readfds);
		if (select(maxfd + 1, &readfds, 0, 0, 0) < 0)
			fail("select", 0);

		/* accept incoming connections */
		if (FD_ISSET(mainfd, &readfds)) {
			struct sockaddr_in clientaddr;
			int clientlen = sizeof(clientaddr);
			mainclientfd = accept(mainfd, (struct sockaddr *)&clientaddr, &clientlen);
			if (mainclientfd < 0)
				fail("accept", 0);
			if (fcntl(mainclientfd, F_SETFL, O_NONBLOCK) < 0)
				fail("fcntl", 0);
			if (mainclientfd > maxfd)
				maxfd = mainclientfd;
		}
		if (FD_ISSET(datafd, &readfds)) {
			struct sockaddr_in clientaddr;
			int clientlen = sizeof(clientaddr);
			dataclientfd = accept(datafd, (struct sockaddr *)&clientaddr, &clientlen);
			if (dataclientfd < 0)
				fail("accept", 0);
			if (dataclientfd > maxfd)
				maxfd = dataclientfd;
			g_dataclientfd = dataclientfd;

			runPico = 1;
		}

		/* incoming SCPI traffic */
		if (mainclientfd && FD_ISSET(mainclientfd, &readfds)) {
			char buf[1024];

			/* we assume we can grab the entire command in one packet, for now */
			int n = read(mainclientfd, buf, sizeof(buf)-1);
			if (n >= 1) {
				buf[n] = 0;
				handleSCPI(mainclientfd, buf);
			}
		}

		/* drain wakeup pipe */
		if (FD_ISSET(pipes[0], &readfds)) {
			char buf[256];
			printf("wake\n");
			int n = read(pipes[0], buf, sizeof(buf));
			if (n >= 1)
				runPico = 1;
		}

		if (runPico) {
			printf("whoomp\n");
			goPicoGo();
		}
	}
}

