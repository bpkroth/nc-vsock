/**
 * vsock-oneway-latency-benchmark.c
 * 2019-01-09
 * bpkroth
 *
 * A simple tool for measuring the latency of a vsock connection between a VM
 * and its host.
 *
 * This is meant as a more simplified and controlled version of a system built
 * in golang we have for communicating resource allocation adjustments
 * concerning performance SLO addition/subtractions for an application (perf-comms).
 *
 * In that system, clients (in VMs) communicate downward to a host arbitrator
 * (with relatively small protobufs), but it (typically) doesn't respond back
 * (ie: no message ACKs).
 *
 * To measure the latency on such a situation ideally we'd rdtsc in the VM,
 * send it in the packet, rdtsc in the host and compare the results.
 * Unfortunately, there are a number of issues with this approach:
 * a) rdtsc in the VM is at the very least offset from the host (handled in HW
 *    for modern systems), if not full on emulated, though in the first case we
 *    can compensate for this by reading the offset of the vCPU in the host via
 *    debugfs (eg: /sys/kernel/debug/kvm/{vm_id}/vcpu[0-9]/tsc-offset -
 *    originally exported for the purposes of reconstructing traces across a
 *    host/VM via trace-cmd).
 * b) tsc values for different cores/numa nodes may or may not be synchronized
 *    at boot (either by HW (eg: BIOS) or the OS).  However, various CPU/BIOS
 *    implementations have bugs so this value may not be trustworth.  Our tests
 *    show O(1000) ticks difference when reading the value from different cores
 *    at the "same time".
 *
 * ...
 *
 * But with those caveats (which we've separately tested for on this machine),
 * we're going to go ahead and try that approach anyways!
 */

#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/vm_sockets.h>
#include <x86intrin.h>
#include <math.h>
#include <limits.h>

typedef unsigned long long tsc_t;

#define ITERATIONS 1000
#define SERVER_LISTEN_PORT 12345
#define SERVER_UNIX_PATH "/tmp/vsock-oneway-latency-benchmark.sock"

// TODO: variable for testing with different lengths (up to 4096)
#define CLIENT_MESSAGE_LENGTH 32
//#define CLIENT_MESSAGE_LENGTH 4096

const char* SERVER_RESPONSE_MESSAGE = "s";
const int SERVER_RESPONSE_LENGTH = 1;
tsc_t ticks[ITERATIONS] = {0};

#ifdef DEBUG
#define DEBUG_PRINT(...) do{ fprintf( stderr, __VA_ARGS__ ); } while( false )
#else
#define DEBUG_PRINT(...) do{ } while ( false )
#endif

inline tsc_t begin_rdtsc()
{
	// use lfence to prevent CPU reordering of the rdtsc instruction
	// (instead of cpuid which is more heavyweight)
	_mm_lfence();
	return __rdtsc();
}

inline tsc_t end_rdtsc()
{
	// use rdtscp as well for similar reasons (plus it also tells us which
	// core it was read from) we don't care about it in this case, since we
	// expect this benchmark to be run with taskset to affinitize it to a core

	unsigned int cpuNum;
	unsigned long long tsc = __rdtscp(&cpuNum);
	_mm_lfence();
	return tsc;
}

long long parse_client_tsc_offset(const char *client_tsc_offset_str)
{
	char *end = NULL;
	long long client_tsc_offset = strtoll(client_tsc_offset_str, &end, 10);
	if (client_tsc_offset_str != end && *end == '\0') {
		return client_tsc_offset;
	} else {
		fprintf(stderr, "invalid client tsc-offset: %s\n", client_tsc_offset_str);
		return -1;
	}
}

int parse_cid(const char *cid_str)
{
	char *end = NULL;
	long cid = strtol(cid_str, &end, 10);
	if (cid_str != end && *end == '\0') {
		return cid;
	} else {
		fprintf(stderr, "invalid cid: %s\n", cid_str);
		return -1;
	}
}

int vsock_listen_and_accept_single_client_connection()
{
	int listen_fd;
	int client_fd;

	struct sockaddr_vm sa_listen = {
		.svm_family = AF_VSOCK,
		.svm_cid = VMADDR_CID_ANY,
		.svm_port = SERVER_LISTEN_PORT,
	};
	struct sockaddr_vm sa_client;
	socklen_t socklen_client = sizeof(sa_client);

	listen_fd = socket(AF_VSOCK, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		perror("socket");
		return -1;
	}

	if (bind(listen_fd, (struct sockaddr*)&sa_listen, sizeof(sa_listen)) != 0) {
		perror("bind");
		close(listen_fd);
		return -1;
	}

	if (listen(listen_fd, 1) != 0) {
		perror("listen");
		close(listen_fd);
		return -1;
	}

	DEBUG_PRINT("%s\n", "Listening on vsock VMADDR_CID_ANY (2 for the host) ...");

	client_fd = accept(listen_fd, (struct sockaddr*)&sa_client, &socklen_client);
	if (client_fd < 0) {
		perror("accept");
		close(listen_fd);
		return -1;
	}

	int host_vm_id;
	socklen_t host_vm_id_size = sizeof(host_vm_id);
	if (getsockopt(client_fd, SOL_SOCKET, SO_VM_SOCKETS_PEER_HOST_VM_ID, (void *)&host_vm_id, &host_vm_id_size) != 0)
	{
		perror("getsockopt");
		close(client_fd);
		close(listen_fd);
		return -1;
	}

	fprintf(stderr, "Connection from cid %u (id: %d, size: %u) port %u...\n", sa_client.svm_cid, host_vm_id, host_vm_id_size, sa_client.svm_port);

	close(listen_fd);
	return client_fd;
}

int unix_listen_and_accept_single_client_connection()
{
	int listen_fd;
	int client_fd;

	struct sockaddr_un sa_listen = {
		.sun_family = AF_UNIX,
	};
	strncpy(sa_listen.sun_path, SERVER_UNIX_PATH, sizeof(sa_listen.sun_path)-1);
	struct sockaddr_vm sa_client;
	socklen_t socklen_client = sizeof(sa_client);

	listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		perror("socket");
		return -1;
	}

	if (bind(listen_fd, (struct sockaddr*)&sa_listen, sizeof(sa_listen)) != 0) {
		perror("bind");
		close(listen_fd);
		return -1;
	}

	if (listen(listen_fd, 1) != 0) {
		perror("listen");
		close(listen_fd);
		return -1;
	}

	DEBUG_PRINT("Listening at '%s' ...", SERVER_UNIX_PATH);

	client_fd = accept(listen_fd, (struct sockaddr*)&sa_client, &socklen_client);
	if (client_fd < 0) {
		perror("accept");
		close(listen_fd);
		return -1;
	}

	fprintf(stderr, "%s\n", "Connection from ... (not sure how to get the remote peer details offhand ...");

	close(listen_fd);
	return client_fd;
}

int inet_listen_and_accept_single_client_connection()
{
	int listen_fd;
	int client_fd;

	struct sockaddr_in sa_listen = {
		.sin_family = AF_INET,
		// Listen on all addrs so we can test both from loopback and across the network.
		.sin_addr.s_addr = INADDR_ANY,
		.sin_port = htons(SERVER_LISTEN_PORT),
	};
	struct sockaddr_in sa_client;
	socklen_t socklen_client = sizeof(sa_client);

	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		perror("socket");
		return -1;
	}

	// Forcefully attaching socket to port
	int opt = 1;
	if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)))
	{
		perror("setsockopt");
		exit(EXIT_FAILURE);
	}

	if (bind(listen_fd, (struct sockaddr*)&sa_listen, sizeof(sa_listen)) != 0) {
		perror("bind");
		close(listen_fd);
		return -1;
	}

	if (listen(listen_fd, 1) != 0) {
		perror("listen");
		close(listen_fd);
		return -1;
	}

	DEBUG_PRINT("Listening on inet '0.0.0.0' at port '%d' ...", SERVER_LISTEN_PORT);

	client_fd = accept(listen_fd, (struct sockaddr*)&sa_client, &socklen_client);
	if (client_fd < 0) {
		perror("accept");
		close(listen_fd);
		return -1;
	}

	fprintf(stderr, "Connection from client address '%s' at port %u ...\n", inet_ntoa(sa_client.sin_addr), ntohs(sa_client.sin_port));

	close(listen_fd);
	return client_fd;
}


void run_server(int client_sock_fd, long long client_tsc_offset)
{
	DEBUG_PRINT("Server using tsc-offset of %lld.\n", client_tsc_offset);

	for (int i=0; i<ITERATIONS; i++)
	{
		// TODO? Use select()/epoll() and change the timer to only
		// measure the time it takes to read/write after we get a
		// notice that there's data available?

		tsc_t client_send_tsc;
		size_t bytes_read = read(client_sock_fd, &client_send_tsc, sizeof(client_send_tsc));
		if (bytes_read <= 0 || bytes_read != sizeof(client_send_tsc))
		{
			perror("read");
			exit(EXIT_FAILURE);
		}

		DEBUG_PRINT("Server received %lu bytes ('%llu') at iteration %d.\n", bytes_read, client_send_tsc, i);

		if (write(client_sock_fd, SERVER_RESPONSE_MESSAGE, SERVER_RESPONSE_LENGTH) != SERVER_RESPONSE_LENGTH)
		{
			perror("write");
			exit(EXIT_FAILURE);
		}

		ticks[i] = end_rdtsc() - client_send_tsc + client_tsc_offset;
	}
}

int vsock_connect(int server_cid)
{
	DEBUG_PRINT("Client connecting to cid %d on port %d.\n", server_cid, SERVER_LISTEN_PORT);

	int fd;
	struct sockaddr_vm sa = {
		.svm_family = AF_VSOCK,
		.svm_port = SERVER_LISTEN_PORT,
		.svm_cid = server_cid,
	};

	if (server_cid < 0) {
		return -1;
	}

	fd = socket(AF_VSOCK, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}

	if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
		perror("connect");
		close(fd);
		return -1;
	}

	return fd;
}

int unix_connect(const char *server_unix_path)
{
	DEBUG_PRINT("Client connecting to unix path '%s'.\n", server_unix_path);

	int fd;
	struct sockaddr_un sa = {
		.sun_family = AF_UNIX,
	};
	strncpy(sa.sun_path, server_unix_path, sizeof(sa.sun_path)-1);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}

	if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
		perror("connect");
		close(fd);
		return -1;
	}

	return fd;
}

int inet_connect(const char *server_ip)
{
	DEBUG_PRINT("Client connecting to server ip '%s' on port %u.\n", server_ip, SERVER_LISTEN_PORT);

	int fd;
	struct sockaddr_in sa = {
		.sin_family = AF_INET,
		.sin_port = htons(SERVER_LISTEN_PORT),
	};
	if (inet_pton(AF_INET, server_ip, &sa.sin_addr) != 1)
	{
		perror("inet_pton");
		return -1;
	}

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}

	if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
		perror("connect");
		close(fd);
		return -1;
	}

	return fd;
}

void run_client(int server_sock_fd)
{
	for (int i=0; i<ITERATIONS; i++)
	{
		tsc_t begin_ts = begin_rdtsc();
		if (write(server_sock_fd, &begin_ts, sizeof(begin_ts)) != sizeof(begin_ts))
		{
			perror("write");
			exit(EXIT_FAILURE);
		}

		char buf[SERVER_RESPONSE_LENGTH+1];
		memset(buf, '\0', SERVER_RESPONSE_LENGTH + 1);
		size_t bytes_read = read(server_sock_fd, buf, SERVER_RESPONSE_LENGTH);
		if (bytes_read <= 0)
		{
			perror("read");
			exit(EXIT_FAILURE);
		}

		DEBUG_PRINT("Client received %lu bytes ('%s') at iteration %d.\n", bytes_read, buf, i);

		ticks[i] = end_rdtsc() - begin_ts;
	}
}

int cmp_tsc_t(const void *a, const void *b)
{
	return (*(tsc_t*)a - *(tsc_t*)b);
}

void print_results()
{
	// exclude the first timing result from the rest of the stats

	tsc_t min = ULONG_MAX;
	tsc_t max = 0;
	tsc_t sum = 0;
	for (int i=1; i<ITERATIONS; i++)
	{
		fprintf(stdout, "%4d: %llu\n", i, ticks[i]);

		min = (ticks[i] < min) ? ticks[i] : min;
		max = (ticks[i] > max) ? ticks[i] : max;
		sum += ticks[i];
	}

	long double avg = sum / (long double) ITERATIONS;
	long double stddev = 0;
	for (int i=1; i<ITERATIONS; i++)
	{
		stddev += pow(ticks[i] - avg, 2);
	}
	stddev = sqrt(stddev / ITERATIONS);

	// sort them to get the median
	qsort(ticks+1, ITERATIONS-1, sizeof(tsc_t), cmp_tsc_t);

	fprintf(stdout, "Initial connection/send: %llu\n", ticks[0]);
	fprintf(stdout, "min: %llu\n", min);
	fprintf(stdout, "max: %llu\n", max);
	fprintf(stdout, "median: %llu\n", ticks[ITERATIONS/2]);
	fprintf(stdout, "avg: %Lf\n", avg);
	fprintf(stdout, "stddev: %Lf\n", stddev);
}

void print_usage(const char * msg)
{
	fprintf(stderr, "%s\n%s\n", msg, "usage: vsock-latency-benchmark -m <vsock|unix|inet> <-s client-tsc-offset|-c <server-cid|unix-sock-path|ipaddr>>");
}

int main(int argc, char** argv)
{
	enum MODE
	{
		VSOCK,
		UNIX,
		INET,
	} mode;

	if (argc >= 3 && strcmp(argv[1], "-m") == 0)
	{
		if (strcmp(argv[2], "vsock") == 0)
		{
			mode = VSOCK;
		}
		else if (strcmp(argv[2], "unix") == 0)
		{
			mode = UNIX;
		}
		else if (strcmp(argv[2], "inet") == 0)
		{
			mode = INET;
		}
		else
		{
			print_usage("Unhandled mode argument.");
			return EXIT_FAILURE;
		}
	}
	else
	{
		print_usage("Invalid number/type/order of arguments.");
		return EXIT_FAILURE;
	}

	if (argc == 5 && strcmp(argv[3], "-s") == 0)
	{
		int client_sock_fd = -1;
		switch (mode)
		{
			case VSOCK:
				client_sock_fd = vsock_listen_and_accept_single_client_connection();
				break;
			case UNIX:
				client_sock_fd = unix_listen_and_accept_single_client_connection();
				break;
			case INET:
				client_sock_fd = inet_listen_and_accept_single_client_connection();
				break;
			default:
				print_usage("Unhandled mode.");
				return EXIT_FAILURE;
		}

		long long client_tsc_offset = parse_client_tsc_offset(argv[4]);
		if (client_tsc_offset == -1)
		{
			print_usage("Failed to parse client_tsc_offset argument.");
			return EXIT_FAILURE;
		}

		run_server(client_sock_fd, client_tsc_offset);

		if (mode == UNIX)
		{
			unlink(SERVER_UNIX_PATH);
		}
	}
	else if (argc == 5 && strcmp(argv[3], "-c") == 0)
	{
		int server_sock_fd = -1;
		switch (mode)
		{
			case VSOCK:
				// arg is expected to typically be 2 for the host cid constant
				server_sock_fd = vsock_connect(parse_cid(argv[4]));
				break;
			case UNIX:
				// arg is expected to typically be SERVER_UNIX_PATH
				server_sock_fd = unix_connect(argv[4]);
				break;
			case INET:
				// arg is expected to typically be 127.0.0.1
				server_sock_fd = inet_connect(argv[4]);
				break;
			default:
				print_usage("Unhandled mode.");
				return EXIT_FAILURE;
		}

		run_client(server_sock_fd);
	}
	else
	{
		print_usage("Invalid number/type/order of arguments.");
		return EXIT_FAILURE;
	}

	print_results();

	return EXIT_SUCCESS;
}
