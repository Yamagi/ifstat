/*-
 * Copyright (c) 2014 Yamagi Burmeister <yamagi@yamagi.org>
 * Copyright (c) 2003 Trent Nelson <trent@arpa.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <ctype.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <unistd.h>
#include <net/if.h>
#include <net/if_mib.h>
#include <sys/sysctl.h>

typedef struct ifmibdata ifmibdata; // Interface data struct
typedef struct timeval timeval; // Time struct
typedef struct tm tm; // Calendar struct

int8_t quit = 0; // Quit gracefull when 1

// ----------------------------

static void
error(const char *func,  int line, char *msg, int hasmsg)
{
	char errormsg[64];

    snprintf(errormsg, sizeof(errormsg), "%s, line %i", func, line);

	if (hasmsg)
		fprintf(stderr, "%s: %s\n", errormsg , msg);
	else
		perror(errormsg);

	exit(1);
}

static void
usage(const char *binname)
{
	fprintf(stderr, "Usage: %s outfile interval interface\n", binname);
	fprintf(stderr, "  - outfile: File to write data to.\n");
	fprintf(stderr, "  - intervall: Intervall of data retrival in seconds.\n");
	fprintf(stderr, "  - interface: Network interface to retrive data from.\n");
	exit(1);
}

static void
signalhandler(int signal)
{
	quit = 1;
}

// ----------------------------

static void
getifdata(int row, ifmibdata *data)
{
	static int32_t name[] = { CTL_NET, PF_LINK, NETLINK_GENERIC, IFMIB_IFDATA, 0, IFDATA_GENERAL };
	size_t len = sizeof(*data);

	name[4] = row;

	if (sysctl(name, 6, data, &len, NULL, 0) != 0)
		error(__func__, __LINE__, NULL, 0);
}

static uint32_t
getifnumber(void)
{
	uint32_t data = 0;
	size_t len = sizeof(data);
	static int name[] = { CTL_NET, PF_LINK, NETLINK_GENERIC, IFMIB_SYSTEM, IFMIB_IFCOUNT };

	if (sysctl(name, 5, &data, &len, NULL, 0) != 0)
		error(__func__, __LINE__, NULL, 0);

	return data;
}

static int32_t
getifrow(const char *ifname)
{
	uint32_t num = getifnumber();

	ifmibdata *data;
	if ((data = malloc(sizeof(ifmibdata))) == NULL)
		error(__func__, __LINE__, NULL, 0);

	for (int i = 1; i <= num; i++)
	{
		getifdata(i, data);

		if (!strcmp(data->ifmd_name, ifname))
		{
			free(data);
			return i;
		}
	}

	free(data);
	return -1;
}

// ----------------------------

/*
 * This small program retrives the every "interval"
 * seconds the bytes send over "interface" and
 * breaks them down to a bytes per second basis.
 *
 * Usage: :/program file interval interface
 *  - file:      File to save the data to.
 *  - interval:  Interval of data retrival in seconds.
 *  - interface: Network interface to retrive data from.
 *
 * Beware: The program is FreeBSD specific.
 */
int
main(int argc, char **argv)
{
	if (argc != 4)
	{
		usage(argv[0]);
	}

	// First argument is the output file
	const char *outfile = argv[1];

	// Second argument is the interval
	for (int i = 0; i < strlen(argv[2]); i++)
	{
		if (!isdigit(*(argv[2] + i)))
			usage(argv[0]);
	}

	int32_t interval = (int)strtol(argv[2], NULL, 10);

	// Third argument is the interface
	char *inter = argv[3];
	int32_t row = getifrow(inter);

	if (row < 1)
		error(__func__, __LINE__, "Couldn't get interface", 1);

	// ----------------------------

    signal(SIGINT, signalhandler);
    signal(SIGTERM, signalhandler);

	// ----------------------------

	FILE *csv;
	if ((csv = fopen(outfile, "w")) == NULL)
		error(__func__, __LINE__, NULL, 0);

	char *header = "date,input in bytes per second,output in bytes per second\n";
	fwrite(header, strlen(header), 1, csv);

	ifmibdata *data;
	if ((data = malloc(sizeof(ifmibdata))) == NULL)
		error(__func__, __LINE__, NULL, 0);

	uint32_t cur_inb = 0, cur_outb = 0;
	uint32_t old_inb = 0, old_outb = 0;
	timeval tv, old_tv, new_tv;

	while (1)
	{
		// Retrive data
        getifdata(row, data);
		gettimeofday(&new_tv, NULL);
		timersub(&new_tv, &old_tv, &tv);

		// Calculate current throughput
		cur_inb = data->ifmd_data.ifi_ibytes - old_inb;
		cur_outb = data->ifmd_data.ifi_obytes - old_outb;

		// Normalize over time
		cur_inb /= tv.tv_sec + (tv.tv_usec * 1e-6);
		cur_outb /= tv.tv_sec + (tv.tv_usec * 1e-6);

		// Backup values for next iteration
 		old_inb = data->ifmd_data.ifi_ibytes;
		old_outb = data->ifmd_data.ifi_obytes;
		old_tv = new_tv;

		// Write to file
		char t[64] = { 0 };
		tm *time = localtime(&new_tv.tv_sec);
		strftime(t, sizeof(t), "%Y.%m.%d %H:%M:%S", time);

		char s[128] = { 0 };
		snprintf(s, sizeof(s), "%s,%i,%i\n", t, cur_inb, cur_outb);
		fwrite(s, strlen(s), 1, csv);

		// Exit when quit is requested
		if (quit)
			break;

		// Wait for next iteration
		usleep(interval * 1000 * 1000);
	}

	fflush(csv);
	fclose(csv);

	return 0;
}

