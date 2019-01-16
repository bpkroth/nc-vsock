This repo is a fork of the basic nc-vsock tool.

Build it as follows:
```

```

In it we've added a simple tool to help measure the oneway latency of a simple vsock communication from guest down to the host.

To do this we make use of the following assumptions:
- The `rdtscp`, `constant_tsc`, `nonstop_tsc`, `tsc_known_freq`, and `tsc_adjust` instructions are present in both the VM and the host.
  (To get them in the VM one can use the `<cpu mode='host-model'/>` attribute in `libvirt`, which we use with a `qemu+kvm` hypervisor to provide our VMs, though others that support `vsock` (eg: Hyper-V, VMware, etc.) or should also work.)
- `debugfs` is mounted at `/sys/kernel/debug` so that the VM's TSC offset can be read from `/sys/kernel/debug/kvm/*/vcpu[0-9]*/tsc-offset`.
  (Note: there should be only one VM running for that shell glob to work, else substitute the correct path as necessary)
- The TSCs of different cores on a socket are synchronized.
  (See https://github.com/bpkroth/rdtsc-example/blob/master/multicore-readtsc.c for an example of how to check for that)
- `taskset` is used to pin server/client to same/different cores as desired, but on the same socket (see TSC sync issues above).
- Either `taskset` or `virsh vcpupin` is used to pin the guest VMs vCPUs to appropriate CPUs (see TSC sync issues above).

From there the model is as follows:
- A server process is spun up on the host to accept a single client connection:
```
# For measuring VM to host vsock:
# taskset -c 4 ./vsock-oneway-latency-benchmark -m vsock -s $(sudo bash -c 'cat /sys/kernel/debug/kvm/*/vcpu*/tsc-offset | head -n1') | tail

# For measuring local unix socket comms (either on the VM or on the host):
# taskset -c 4 ./vsock-oneway-latency-benchmark -m unix -s 0 | tail

# For measuring remote ip based comms (VM to host):
# taskset -c 4 ./vsock-oneway-latency-benchmark -m inet -s $(sudo bash -c 'cat /sys/kernel/debug/kvm/*/vcpu*/tsc-offset | head -n1') | tail

# For measuring local ip based comms (either on the VM or on the host):
# taskset -c 4 ./vsock-oneway-latency-benchmark -m inet -s 0 | tail

```
- The client process connects to the server:
```
# For measuring VM to host vsock:
# taskset -c 1 ./vsock-oneway-latency-benchmark -m vsock -c 2 | tail

# For measuring local unix socket comms (either on the VM or on the host):
# taskset -c 1 ./vsock-oneway-latency-benchmark -m unix -c  '/tmp/vsock-oneway-latency-benchmark.sock' | tail

# For measuring remote ip based comms (VM to host):
# taskset -c 1 ./vsock-oneway-latency-benchmark -m inet -s $server_ip | tail

# For measuring local ip based comms (either on the VM or on the host):
# taskset -c 1 ./vsock-oneway-latency-benchmark -m inet -s 127.0.0.1 | tail
```
- Now the client/server will perform `ITERATIONS` number of the following sequence:
	- `rdtsc` (with appropriate fencing to prevent CPU reordering)
	- `write()` to send it to the server
	- The server will `read()` the value and, using the provided tsc offset, compute the difference from its own `rdtsc` result.
	- The value is stored in an array and a short ACK is sent back to the client.
	- The client will `read()` that value to perform a new local `rdtsc` and compute the duration of the entire RTT
	  (without any need for offsets, though incorporating the overheads incurred by the extra server/cient `write()/read()` calls.)
- At the end both client and server will emit some stats on the results, though note that since the client's measures a full RTT and the server's only measures a single downcall, they will be roughly double.


Here are some results from some runs done on the following machine:

VM `lscpu` and `uname` output:
```
# uname -a
# lscpu
# lscpu
Architecture:          x86_64
CPU op-mode(s):        32-bit, 64-bit
Byte Order:            Little Endian
CPU(s):                1
On-line CPU(s) list:   0
Thread(s) per core:    1
Core(s) per socket:    1
Socket(s):             1
NUMA node(s):          1
Vendor ID:             GenuineIntel
CPU family:            6
Model:                 94
Model name:            Intel Core Processor (Skylake, IBRS)
Stepping:              3
CPU MHz:               2294.670
BogoMIPS:              4589.34
Hypervisor vendor:     KVM
Virtualization type:   full
L1d cache:             32K
L1i cache:             32K
L2 cache:              4096K
L3 cache:              16384K
NUMA node0 CPU(s):     0
Flags:                 fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush mmx fxsr sse sse2 ss syscall nx pdpe1gb rdtscp lm constant_tsc rep_good nopl xtopology nonstop_tsc cpuid tsc_known_freq pni pclmulqdq ssse3 fma cx16 pcid sse4_1 sse4_2 x2apic movbe popcnt tsc_deadline_timer aes xsave avx f16c rdrand hypervisor lahf_lm abm 3dnowprefetch cpuid_fault invpcid_single pti ssbd ibrs ibpb fsgsbase tsc_adjust bmi1 hle avx2 smep bmi2 erms invpcid rtm rdseed adx smap xsaveopt arat
```

Host `lscpu` and `uname` output:
```
# uname -a
Linux gen5-b3a-14 4.18.0-0.bpo.3-amd64 #1 SMP Debian 4.18.20-2~bpo9+1 (2018-12-08) x86_64 GNU/Linux
# lscpu
Architecture:          x86_64
CPU op-mode(s):        32-bit, 64-bit
Byte Order:            Little Endian
CPU(s):                80
On-line CPU(s) list:   0-79
Thread(s) per core:    2
Core(s) per socket:    20
Socket(s):             2
NUMA node(s):          2
Vendor ID:             GenuineIntel
CPU family:            6
Model:                 79
Model name:            Intel(R) Xeon(R) CPU E5-2673 v4 @ 2.30GHz
Stepping:              1
CPU MHz:               1199.858
CPU max MHz:           2301.0000
CPU min MHz:           1200.0000
BogoMIPS:              4589.30
Virtualization:        VT-x
L1d cache:             32K
L1i cache:             32K
L2 cache:              256K
L3 cache:              51200K
NUMA node0 CPU(s):     0-19,40-59
NUMA node1 CPU(s):     20-39,60-79
Flags:                 fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush dts acpi mmx fxsr sse sse2 ss ht tm pbe syscall nx pdpe1gb rdtscp lm constant_tsc arch_perfmon pebs bts rep_
good nopl xtopology nonstop_tsc cpuid aperfmperf pni pclmulqdq dtes64 monitor ds_cpl vmx smx est tm2 ssse3 sdbg fma cx16 xtpr pdcm pcid dca sse4_1 sse4_2 x2apic movbe popcnt tsc_deadline_timer aes xsave avx f16c
 rdrand lahf_lm abm 3dnowprefetch cpuid_fault epb cat_l3 cdp_l3 invpcid_single pti intel_ppin ssbd ibrs ibpb stibp tpr_shadow vnmi flexpriority ept vpid fsgsbase tsc_adjust bmi1 hle avx2 smep bmi2 erms invpcid r
tm cqm rdt_a rdseed adx smap intel_pt xsaveopt cqm_llc cqm_occup_llc cqm_mbm_total cqm_mbm_local dtherm ida arat pln pts flush_l1d
```

Server (host) side (one-way) results:
```
# (local unix socket one-way communication latencies on the host)
# taskset -c 4 ./vsock-oneway-latency-benchmark -m unix -s 0 | tail
Connection from ...
999996: 13724
999997: 13764
999998: 14232
999999: 14312
Initial connection/send: 308959
min: 9440
max: 1281892
median: 14036
avg: 14146.421570
stddev: 1646.850549

# (local ip loopback one-way communication latencies on the host)
# taskset -c 4 ./vsock-oneway-latency-benchmark -m inet -s 0 | tail
Connection from client address '127.0.0.1' at port 40872 ...
999996: 157648
999997: 492512
999998: 789748
999999: 1728556
Initial connection/send: 410649
min: 37536
max: 1728556
median: 48652
avg: 48717.873978
stddev: 2291.348548

# (remote ip one-way communication latencies between the VM and the host)
# taskset -c 4 ./vsock-oneway-latency-benchmark -m inet -s $(sudo bash -c 'cat /sys/kernel/debug/kvm/*/vcpu*/tsc-offset | head -n1') | tail
Connection from client address '10.191.22.6' at port 50464 ...
999996: 63591
999997: 104586
999998: 62126
999999: 60858
Initial connection/send: 359460
min: 56386
max: 1158162
median: 61796
avg: 65130.601088
stddev: 11178.740056

# (vsock one-way communication latencies between the VM and the host)
# taskset -c 4 ./vsock-oneway-latency-benchmark -m vsock -s $(sudo bash -c 'cat /sys/kernel/debug/kvm/*/vcpu*/tsc-offset | head -n1') | tail
Connection from cid 3 (id: 1, size: 4) port 1115...
999996: 41288
999997: 41673
999998: 40603
999999: 41307
Initial connection/send: 350803
min: 37584
max: 6711109
median: 41071
avg: 42578.326480
stddev: 9810.366642
```

Given the TSC ticks at a constant 100MHz, the values above have been converted to microseconds and compared in the following table:


| socket type | connect (us) | min (us) | max (us) | median (us) | average (us) | stddev (us)   | average + stddev (us) |
|:------------|-------------:|---------:|---------:|------------:|-------------:|--------------:|----------------------:|
| vsock       | 3508.0       | 375.8    | 67111.1  | 410.7       | 425.8        | 98.1 (23%)    | 523.8                 |
| unix local  | 3089.6       | 94.4     | 12818.9  | 140.4       | 141.5        | 16.4 (11%)    | 157.9                 |
| inet local  | 4106.5       | 375.4    | 17285.6  | 486.5       | 487.2        | 22.9 (4.7%)   | 510.1                 |
| inet remote | 3594.6       | 563.9    | 11581.6  | 618.0       | 651.3        | 111.8 (17.2%) | 763.1                 |

And here's the relative differences:

| socket type | connect (%) | min (%) | max (%) | median (%) | average (%) | stddev (%) | average + stddev (%) |
|:------------|------------:|--------:|--------:|-----------:|------------:|-----------:|---------------------:|
| vsock       | 100%        | 100%    | 100%    | 100%       | 100%        | 100%       | 100%                 |
| unix local  | 88.1%       | 25.2%   | 19.1%   | 34.2%      | 33.2%       | 16.7%      | 30.1%                |
| inet local  | 117.1%      | 99.9%   | 25.8%   | 118.5%     | 114.4%      | 23.3%      | 97.4%                |
| inet remote | 102.5%      | 150%    | 17.3%   | 150.4%     | 152.9%      | 113.9%     | 145.7%               |

As can be seen vsock:
- has relatively similar average performance to an ip loopback connection
- but is much slower than a local unix socket
- has higher variability
  we suspect this is due to additional overhead of VMEXIT/VMENTER context switching and scheduling overhead, though have not yet investigated it
- it is possible there are additional optimizations to be made in the guest os and/or host hypervisor vsock handlers to improve this situation further


For completeness here's the same results from the client (eg: VM) side:
```
# (local unix socket round-trip communication latencies on the host)
# taskset -c 3 ./vsock-oneway-latency-benchmark -m unix -c '/tmp/vsock-oneway-latency-benchmark.sock' | tail
999996: 21768
999997: 21404
999998: 22728
999999: 17404
Initial connection/send: 332810
min: 16104
max: 1292132
median: 21868
avg: 21991.649269
stddev: 1911.080622

# (local ip loopback round-trip communication latencies on the host)
# taskset -c 3 ./vsock-oneway-latency-benchmark -m inet -c 127.0.0.1 | tail
999996: 164604
999997: 498840
999998: 795256
999999: 1734308
Initial connection/send: 410005
min: 43572
max: 1734308
median: 54960
avg: 55036.987251
stddev: 2332.692484

# (remote ip round-trip communication latencies from the VM to the host and back)
# ./vsock-oneway-latency-benchmark -m inet -c 10.191.20.188 | tail
999996: 88426
999997: 131218
999998: 86626
999999: 85162
Initial connection/send: 422480
min: 73054
max: 96124620
median: 85664
avg: 103560.520980
stddev: 1013498.526825

# (vsock round-trip communication latencies from the VM to the host and back)
# ./vsock-oneway-latency-benchmark -m vsock -c 2 | tail
999996: 71948
999997: 71919
999998: 71743
999999: 70518
Initial connection/send: 468271
min: 61694
max: 38552412
median: 71984
avg: 73876.022723
stddev: 45690.017097
```
