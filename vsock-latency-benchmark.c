/**
 * vsock-latency-benchmark.c
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
 * Instead, we do the following:
 *
 * Implement this simple (C-send, S-recieve, S-respond, C-receive) protocol and
 * do local timing on each end to discover the overall time for the client to
 * send a message and receive a response from the server (and the overhead of
 * the server processing the result).
 */

#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/vm_sockets.h>
#include <x86intrin.h>

typedef unsigned long long tsc_t;

#define ITERATIONS 1000
#define SERVER_LISTEN_PORT 12345
// variable for testing with different lengths (up to 4096)
#define CLIENT_MESSAGE_LENGTH 32

const char CLIENT_MESSAGE_BUFFER[CLIENT_MESSAGE_LENGTH] = {'c'};
const char* SERVER_RESPONSE_MESSAGE = "s";
const int SERVER_RESPONSE_LENGTH = 1;
tsc_t ticks[ITERATIONS] = {0};

#ifdef DEBUG
#define DEBUG_PRINT(...) do{ fprintf( stderr, __VA_ARGS__ ); } while( false )
#else
#define DEBUG_PRINT(...) do{ } while ( false )
#endif

void print_usage()
{
	fprintf(stderr, "%s\n", "usage: vsock-latency-benchmark <-s|-c server-cid>");
}

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

	DEBUG_PRINT("%s\n", "Listening ...");

	client_fd = accept(listen_fd, (struct sockaddr*)&sa_client, &socklen_client);
	if (client_fd < 0) {
		perror("accept");
		close(listen_fd);
		return -1;
	}

	fprintf(stderr, "Connection from cid %u port %u...\n", sa_client.svm_cid, sa_client.svm_port);

	close(listen_fd);
	return client_fd;
}

void run_server()
{
	int client = vsock_listen_and_accept_single_client_connection();

	for (int i=0; i < ITERATIONS; i++)
	{
		// TODO? Use select()/epoll() and change the timer to only
		// measure the time it takes to read/write after we get a
		// notice that there's data available?
		tsc_t begin_ts = begin_rdtsc();

		char buf[CLIENT_MESSAGE_LENGTH];
		size_t bytes_read = read(client, buf, CLIENT_MESSAGE_LENGTH);
		if (bytes_read <= 0)
		{
			perror("read");
			exit(EXIT_FAILURE);
		}

		DEBUG_PRINT("Server received %lu bytes ('%s') at iteration %d.\n", bytes_read, buf, i);

		if (write(client, SERVER_RESPONSE_MESSAGE, SERVER_RESPONSE_LENGTH) <= 0)
		{
			perror("write");
			exit(EXIT_FAILURE);
		}

		ticks[i] = end_rdtsc() - begin_ts;
	}
}

int vsock_connect(const char *cid_str)
{
	int fd;
	int cid;
	struct sockaddr_vm sa = {
		.svm_family = AF_VSOCK,
		.svm_port = SERVER_LISTEN_PORT,
	};

	cid = parse_cid(cid_str);
	if (cid < 0) {
		return -1;
	}
	sa.svm_cid = cid;

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

void run_client(const char* server_cid)
{
	int server = vsock_connect(server_cid);

	for (int i=0; i<ITERATIONS; i++)
	{
		tsc_t begin_ts = begin_rdtsc();

		if (write(server, CLIENT_MESSAGE_BUFFER, CLIENT_MESSAGE_LENGTH) != CLIENT_MESSAGE_LENGTH)
		{
			perror("write");
			exit(EXIT_FAILURE);
		}

		char buf[SERVER_RESPONSE_LENGTH];
		size_t bytes_read = read(server, buf, SERVER_RESPONSE_LENGTH);
		if (bytes_read <= 0)
		{
			perror("read");
			exit(EXIT_FAILURE);
		}

		DEBUG_PRINT("Client received %lu bytes ('%s') at iteration %d.\n", bytes_read, buf, i);

		ticks[i] = end_rdtsc() - begin_ts;
	}
}

void print_results()
{
	for (int i=0; i<ITERATIONS; i++)
	{
		fprintf(stdout, "%4d: %llu\n", i, ticks[i]);
	}
}

int main(int argc, char** argv)
{
	if (argc == 2 && strcmp(argv[1], "-s") == 0)
	{
		run_server();
	}
	else if (argc == 3 && strcmp(argv[1], "-c") == 0)
	{
		run_client(argv[2]);
	}
	else
	{
		print_usage();
		return EXIT_FAILURE;
	}

	print_results();

	return EXIT_SUCCESS;
}
