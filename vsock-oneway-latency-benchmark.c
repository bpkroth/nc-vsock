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
#include <linux/vm_sockets.h>
#include <x86intrin.h>
#include <math.h>
#include <limits.h>

typedef unsigned long long tsc_t;

#define ITERATIONS 1000
#define SERVER_LISTEN_PORT 12345
// variable for testing with different lengths (up to 4096)
//#define CLIENT_MESSAGE_LENGTH 32
#define CLIENT_MESSAGE_LENGTH 4096

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
	fprintf(stderr, "%s\n", "usage: vsock-latency-benchmark <-s client-tsc-offset|-c server-cid>");
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

	DEBUG_PRINT("%s\n", "Listening ...");

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

void run_server(long long client_tsc_offset)
{
	DEBUG_PRINT("Server using tsc-offset of %lld.\n", client_tsc_offset);

	int client = vsock_listen_and_accept_single_client_connection();

	for (int i=0; i<ITERATIONS; i++)
	{
		// TODO? Use select()/epoll() and change the timer to only
		// measure the time it takes to read/write after we get a
		// notice that there's data available?

		tsc_t client_send_tsc;
		size_t bytes_read = read(client, &client_send_tsc, sizeof(client_send_tsc));
		if (bytes_read <= 0 || bytes_read != sizeof(client_send_tsc))
		{
			perror("read");
			exit(EXIT_FAILURE);
		}

		DEBUG_PRINT("Server received %lu bytes ('%llu') at iteration %d.\n", bytes_read, client_send_tsc, i);

		if (write(client, SERVER_RESPONSE_MESSAGE, SERVER_RESPONSE_LENGTH) != SERVER_RESPONSE_LENGTH)
		{
			perror("write");
			exit(EXIT_FAILURE);
		}

		ticks[i] = end_rdtsc() - client_send_tsc + client_tsc_offset;
	}
}

int vsock_connect(int cid)
{
	DEBUG_PRINT("Client connecting to cid %d on port %d.\n", cid, SERVER_LISTEN_PORT);

	int fd;
	struct sockaddr_vm sa = {
		.svm_family = AF_VSOCK,
		.svm_port = SERVER_LISTEN_PORT,
		.svm_cid = cid,
	};

	if (cid < 0) {
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

void run_client(int server_cid)
{
	int server = vsock_connect(server_cid);

	for (int i=0; i<ITERATIONS; i++)
	{
		tsc_t begin_ts = begin_rdtsc();
		if (write(server, &begin_ts, sizeof(begin_ts)) != sizeof(begin_ts))
		{
			perror("write");
			exit(EXIT_FAILURE);
		}

		char buf[SERVER_RESPONSE_LENGTH+1];
		memset(buf, '\0', SERVER_RESPONSE_LENGTH + 1);
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
	tsc_t min = ULONG_MAX;
	tsc_t max = 0;
	tsc_t sum = 0;
	for (int i=0; i<ITERATIONS; i++)
	{
		fprintf(stdout, "%4d: %llu\n", i, ticks[i]);

		min = (ticks[i] < min) ? ticks[i] : min;
		max = (ticks[i] > max) ? ticks[i] : max;
		sum += ticks[i];
	}

	long double avg = sum / (long double) ITERATIONS;
	long double stddev = 0;
	for (int i=0; i<ITERATIONS; i++)
	{
		stddev += pow(ticks[i] - avg, 2);
	}
	stddev = sqrt(stddev / ITERATIONS);

	fprintf(stdout, "min: %llu\n", min);
	fprintf(stdout, "max: %llu\n", max);
	fprintf(stdout, "avg: %Lf\n", avg);
	fprintf(stdout, "stddev: %Lf\n", stddev);
}

int main(int argc, char** argv)
{
	if (argc == 3 && strcmp(argv[1], "-s") == 0)
	{
		run_server(parse_client_tsc_offset(argv[2]));
	}
	else if (argc == 3 && strcmp(argv[1], "-c") == 0)
	{
		run_client(parse_cid(argv[2]));
	}
	else
	{
		print_usage();
		return EXIT_FAILURE;
	}

	print_results();

	return EXIT_SUCCESS;
}
