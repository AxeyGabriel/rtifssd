/*-
 * Realtime Interface Statistics Sender Daemon
 * rtifssd
 * 
 * Written by Axey Gabriel Muller Endres
 * 19 Jul 2021
 *
 */

#include <sys/types.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/queue.h>
#include <net/if.h>
#include <net/if_mib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <signal.h>
#include <inttypes.h>
#include <syslog.h>
#include <zmq.h>
#include <fcntl.h>

#define DESCRLEN 64
#define VERSION 1
#define MESSAGESIZE 1024 * 1024

char message[MESSAGESIZE];
char myhostname[_POSIX_HOST_NAME_MAX + 1];
SLIST_HEAD(, if_stat) curlist;

char *ifacepattern = NULL;

int signal_fd;

struct if_stat
{
	SLIST_ENTRY(if_stat) link;
	struct timeval tv;
	char dev_name[IF_NAMESIZE];
	char descr[DESCRLEN];
	int ifrow;
	struct ifmibdata mibdata;
	uint64_t if_in_curtraffic;
	uint64_t if_out_curtraffic;
	uint64_t if_in_traffic_peak;
	uint64_t if_out_traffic_peak;
	uint64_t if_in_curpps;
	uint64_t if_out_curpps;
	uint64_t if_in_pps_peak;
	uint64_t if_out_pps_peak;
	bool first;
};

int get_ifmib_ifcount(int *value);
int get_ifmib_general(int row, struct ifmibdata *ifmd);
void update_interface_index(void);
void update_interface_data(void);

void sighandler(int signal)
{
	int rc = write(signal_fd, " ", 1);
	if (rc != 1)
	{
		syslog(LOG_ERR, "Signal handler: Error writing to pipe");
		exit(1);
	}
}

int main(int argc, char **argv)
{
	char *server = NULL;
	int ch;
	int sockoptval = 1;
	int pipefds[2];
	int rc;
	int i;
	struct timeval time_before, time_after;
	uint64_t elapsed_us;

	openlog("rtifssd", LOG_PID | LOG_NDELAY, LOG_LOCAL0);
	syslog(LOG_INFO, "System started, initializing....");

	/* Arguments parsing */
	while ((ch = getopt(argc, argv, "s:i:")) != -1)
	{
		switch (ch)
		{
			case 's':
				server = optarg;
				break;
			case 'i':
				ifacepattern = optarg;
				break;
			case '?':
			default:
				syslog(LOG_ERR, "Error: please specify -s for server address");
				return 1;
		}
	}
	if (server == NULL || ifacepattern == NULL)
	{
		syslog(LOG_ERR, "Error: wrong or missing arguments.");
		return 1;
	}
		
	/* Signal handling */
	rc = pipe(pipefds);
	if (rc != 0)
	{
		syslog(LOG_ERR, "Error creating pipe: %m");
		return 1;
	}
	
	for (i = 0; i < 2; i++)
	{
		int flags = fcntl(pipefds[i], F_GETFL, 0);
		if (flags < 0)
		{
			syslog(LOG_ERR, "Error: fcntl: %m");
			return 1;
		}
		rc = fcntl(pipefds[i], F_SETFL, flags | O_NONBLOCK);
		if (rc != 0)
		{
			syslog(LOG_ERR, "Error: fcntl: %m");
			return 1;
		}
	}
	
	signal_fd = pipefds[1];

	zmq_pollitem_t items[] =
	{
		{ 0, pipefds[0], ZMQ_POLLIN, 0 }
	};

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	gethostname(myhostname, _POSIX_HOST_NAME_MAX + 1);

	/* Set a low process priority */
	if (nice(15))
	{
		syslog(LOG_ERR, "Error: nice: %m");
		return 1;
	}

	/* ZeroMQ Connection Handling */
	void *zmqcontext = zmq_ctx_new();
	if (zmqcontext == NULL)
	{
		syslog(LOG_ERR, "zmq_ctx_new error: %s", zmq_strerror(errno));
		return 1;
	}

	void *zmqpublisher = zmq_socket(zmqcontext, ZMQ_PUB);
	if (zmqpublisher == NULL)
	{
		syslog(LOG_ERR, "zmq_socket error: %s", zmq_strerror(errno));
		return 1;
	}

	sockoptval = 1;
	zmq_setsockopt(zmqpublisher, ZMQ_SNDHWM, &sockoptval, sizeof(sockoptval));
	
	if ((rc = zmq_connect(zmqpublisher, server)) == -1)
	{
		syslog(LOG_ERR, "Error: zmq_connect: %s, server=%s", zmq_strerror(errno), server);
	}
	
	syslog(LOG_INFO, "Sending statistics to %s", server);

	/* Initialize our linked list containing the interfaces */
	SLIST_INIT(&curlist);

	while (1)
	{
		/* TODO 29-JUL-2021:
		 * Optimize this, it's ugly
		 */
		gettimeofday(&time_before, NULL);
		update_interface_index();
		update_interface_data();
		gettimeofday(&time_after, NULL);
		elapsed_us = ((time_after.tv_sec - time_before.tv_sec) * 1000000L + time_after.tv_usec) - time_before.tv_usec;

			
		if (zmq_send(zmqpublisher, message, strlen(message), ZMQ_DONTWAIT) == -1)
		{
			syslog(LOG_ERR, "zmq_send error: %s", zmq_strerror(errno));
		}

		/* Event handling */
		rc = zmq_poll(items, 1, 0);
		if (rc < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}

			syslog(LOG_ERR, "zmq_poll: %s", zmq_strerror(errno));
			return 1;
		}

		/* Received an interrupt */
		if (items[0].revents & ZMQ_POLLIN)
		{
			char buffer[1];
			read(pipefds[0], buffer, 1);
			syslog(LOG_NOTICE, "Interrupt received, stopping system.");
			break;
		}

		usleep(1000000 - elapsed_us);
		printf("elapsed microseconds = %ld, sleep seconds = %ld\n", elapsed_us, 1000000 - elapsed_us);
	}

	/* Reached the end of the life cycle */
	struct if_stat *node = NULL;
	while (!SLIST_EMPTY(&curlist))
	{
		node = SLIST_FIRST(&curlist);
		SLIST_REMOVE_HEAD(&curlist, link);
		free(node);
	}

	zmq_close(zmqpublisher);
	zmq_ctx_destroy(zmqcontext);

	syslog(LOG_ALERT, "Everything cleaned up. Exiting.");	
	closelog();

	return 0;
}

int get_ifmib_ifcount(int *value)
{
	int name[6];
	size_t len;

	name[0] = CTL_NET;
	name[1] = PF_LINK;
	name[2] = NETLINK_GENERIC;
	name[3] = IFMIB_SYSTEM;
	name[4] = IFMIB_IFCOUNT;

	len = sizeof(value);
	
	return sysctl(name, 5, value, &len, (void *)0, 0);
}

int get_ifmib_general(int row, struct ifmibdata *ifmd)
{
	int name[6];
	size_t len;

	name[0] = CTL_NET;
	name[1] = PF_LINK;
	name[2] = NETLINK_GENERIC;
	name[3] = IFMIB_IFDATA;
	name[4] = row;
	name[5] = IFDATA_GENERAL;

	len = sizeof(*ifmd);
	
	return sysctl(name, 6, ifmd, &len, (void *)0, 0);
}

void update_interface_index(void)
{
	struct if_stat *p = NULL;
	int ifindex;
	bool alreadyhas = false;

	/* Get interface count */
	if (get_ifmib_ifcount(&ifindex))
	{
		syslog(LOG_ERR, "Error: get_ifmib_count: %m");
		return;
	}
	
	while (ifindex)
	{
		SLIST_FOREACH(p, &curlist, link)
		{
			if (p->ifrow == ifindex)
			{
				alreadyhas = true;
			}
		}
					
		if (!alreadyhas)
		{
			p = calloc(1, sizeof(struct if_stat));
			if (p == NULL)
			{
				syslog(LOG_ERR, "Error: calloc: %m");
				continue;
			}
			
			p->ifrow = ifindex;	
			p->first = true;
			SLIST_INSERT_HEAD(&curlist, p, link);
		
		}

		alreadyhas = false;
		ifindex--;
	}
}

void update_interface_data(void)
{
	struct ifreq ifr;
	struct if_stat *p = NULL, *temp_var;
	struct timeval oldtv, newtv, tv;
	int s;
	char descr[DESCRLEN];
	uint64_t new_inb, new_outb, old_inb, old_outb;
	uint64_t new_inp, new_outp, old_inp, old_outp;
	double elapsed;
	unsigned long curtime;
	int count = 0;
	
	curtime = time(NULL);
	sprintf(message, "{\"apiversion\": %d ,\"hostname\": \"%s\", \"time\": %lu, \"csv\":[", VERSION, myhostname, curtime);
	
	SLIST_FOREACH_SAFE(p, &curlist, link, temp_var)
	{
		/* Save old counter values before querying new ones */
		old_inb = p->mibdata.ifmd_data.ifi_ibytes;
		old_outb = p->mibdata.ifmd_data.ifi_obytes;
		old_inp = p->mibdata.ifmd_data.ifi_ipackets;
		old_outp = p->mibdata.ifmd_data.ifi_opackets;

		gettimeofday(&newtv, NULL);
		
		/* Get interface mib data 
		 * and if it does no longer exist
		 * delete from the list
		 */
		if (get_ifmib_general(p->ifrow, &p->mibdata) != 0)
		{
			SLIST_REMOVE(&curlist, p, if_stat, link);
			free(p);
			continue;
		}
		
		/* Filter interfaces */
		if (strncmp(p->mibdata.ifmd_name, ifacepattern, strlen(ifacepattern)) != 0)
		{
			SLIST_REMOVE(&curlist, p, if_stat, link);
			free(p);
			continue;
		}

		/* Get interface description */
		strlcpy(ifr.ifr_name, p->mibdata.ifmd_name, sizeof(ifr.ifr_name));
		ifr.ifr_addr.sa_family = AF_LOCAL;
		s = socket(ifr.ifr_addr.sa_family, SOCK_DGRAM, 0);
		if (s < 0)
		{
			syslog(LOG_ERR, "Error: socket: %m");
		}
			
		ifr.ifr_buffer.buffer = &descr;
		ifr.ifr_buffer.length = DESCRLEN;
	
		if (ioctl(s, SIOCGIFDESCR, &ifr) == -1)
		{
			SLIST_REMOVE(&curlist, p, if_stat, link);
			free(p);
			continue;
		}	
		close(s);

		if (strlen(descr) == 0)
		{
			SLIST_REMOVE(&curlist, p, if_stat, link);
			free(p);
			continue;
		}
			
		/* Check if this interface has changed
		 * e.g. when the current client disconnects
		 * and a new one connect with the same interface
		 * */
		if (strncmp(p->descr, descr, DESCRLEN) != 0)
		{
			/* Update the interface name and description
			 * and clear our counters
			 */
			strncpy(p->descr, descr, DESCRLEN);
			strncpy(p->dev_name, p->mibdata.ifmd_name, IF_NAMESIZE);
			old_inb = p->mibdata.ifmd_data.ifi_ibytes;
			old_outb = p->mibdata.ifmd_data.ifi_obytes;
			old_inp = p->mibdata.ifmd_data.ifi_ipackets;
			old_outp = p->mibdata.ifmd_data.ifi_opackets;
		}

		/* Get current counter values */
		new_inb = p->mibdata.ifmd_data.ifi_ibytes;
		new_outb = p->mibdata.ifmd_data.ifi_obytes;
		new_inp = p->mibdata.ifmd_data.ifi_ipackets;
		new_outp = p->mibdata.ifmd_data.ifi_opackets;	
	
		/* Calculate elapsed time */
		oldtv = p->tv;
		timersub(&newtv, &oldtv, &tv);
		elapsed = tv.tv_sec + (tv.tv_usec * 1e-6);
	
		/* Calculate counter difference */
		p->if_in_curtraffic = new_inb - old_inb;
		p->if_out_curtraffic = new_outb - old_outb;
		p->if_in_curpps = new_inp - old_inp;
		p->if_out_curpps = new_outp - old_outp;
	
		/* Divide by the time */
		p->if_in_curtraffic /= elapsed;
		p->if_out_curtraffic /= elapsed;
		p->if_in_curpps /= elapsed;
		p->if_out_curpps /= elapsed;
	
		if (p->if_in_curtraffic > p->if_in_traffic_peak)
		{
			p->if_in_traffic_peak = p->if_in_curtraffic;
		}
		if (p->if_out_curtraffic > p->if_out_traffic_peak)
		{
			p->if_out_traffic_peak = p->if_out_curtraffic;
		}
		if (p->if_in_curpps > p->if_in_pps_peak)
		{
			p->if_in_pps_peak = p->if_in_curpps;
		}
		if (p->if_out_curpps > p->if_out_pps_peak)
		{
			p->if_out_pps_peak = p->if_out_curpps;
		}
	
		p->tv.tv_sec = newtv.tv_sec;
		p->tv.tv_usec = newtv.tv_usec;

		if (p->first)
		{
			p->first = false;
			continue;
		}

		count++;
			
		/*
		 * CSV format
		 * Columns:
		 *	1: Description
		 *	2: Traffic in
		 *	3: Traffic out
		 *	4: Peak Traffic In
		 *	5: Peak Traffic Out
		 *	6: Total inbound octets
		 *	7: Total outbound octets
		 *	8: Packets per second in
		 *	9: Packets per second out
		 *	10: Peak pps in
		 *	11: Peak pps out
		 */
		
		sprintf(message + strlen(message), "\"%s,%"PRId64",%"PRId64",%"PRId64",%"PRId64",%"PRId64",%"PRId64",%"PRId64",%"PRId64",%"PRId64",%"PRId64"\",", \
			p->descr,							\
			p->if_in_curtraffic,				\
			p->if_out_curtraffic,				\
			p->if_in_traffic_peak,				\
			p->if_out_traffic_peak,				\
			p->mibdata.ifmd_data.ifi_ibytes,	\
			p->mibdata.ifmd_data.ifi_obytes,	\
			p->if_in_curpps,					\
			p->if_out_curpps,					\
			p->if_in_pps_peak,					\
			p->if_out_pps_peak);
	}

	/* Delete last comma if there is any interface in the list */
	if (message[strlen(message) - 1] == ',')
	{
		message[strlen(message) - 1] = '\0'; 
	}
	
	/* Terminate JSON */
	sprintf(message + strlen(message), "], \"count\": %d}", count);
}
