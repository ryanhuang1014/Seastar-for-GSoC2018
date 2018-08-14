# TCP-BBR development in Seastar for GSoC 2018
------

TCP BBR was proposed as a modern TCP congestion control algorithm by Google in 2016. The traditional loss-based TCP algorithms like TCP CUBIC, reduce their sending rates greatly when detecting packet loss, and increase rates conservatively when no packets loss happens. However, it is hard to estimate the network condition just on basis of one metric, i.e, packet loss. As a consequence, loss-based TCP algorithms underutilize network most of the time, but cause severe congestion occasionally.

TCP BBR is a new variation of TCP algorithm which adjusts its sending rate based on the real-time network metric, i.e, packet round trip time (RTT), the delivered rate (goodput). It is optimal that the amount of traffic walking through a network link is just equal to the well-known bandwidth delay production (BDP). Hence, TCP BBR uses RTT and goodput to constrict the amount of traffic in flight is equal to BDP. In this way, TCP BBR can maximize the goodput, and minimize the RTT at the same time.


## The use of TCP BBR


To use the TCP BBR congestion control algorithm, just select the bbr algorithm from command line options. The command line is listed as follows:

```TCP congestion algorithm options:
  --tcp-congestion arg (=tcp_newreno)   select the tcp congestion algorithm 
                                        (tcp_newreno / tcp_bbr)
```

An example is showed as follows:

`sudo build/release/apps/httpd/httpd --network-stack native --dpdk-pmd --dhcp 0 --host-ipv4-addr 192.168.11.1 --netmask-ipv4-addr 255.255.255.0 --port 80 --collectd 0 --smp 2 -m 2G --lro on --tso on --tcp-congestion tcp_bbr`


## The description of TCP BBR module

In general, the implemention in the Seastar is composed of 3 important components: 1) the rate estimation algorithm, 2) TCP BBR state and model transition algorithm, 3) the packet pacing system.

### The rate estimation algorithm 

This component is used to estimation the bottleneck bandwidth which will be used by TCP BBR algorithm to throttle its sending rate. The implementation is developed as TCP-BBR which is described at [https://tools.ietf.org/id/draft-cheng-iccrg-delivery-rate-estimation-00.html](https://tools.ietf.org/id/draft-cheng-iccrg-delivery-rate-estimation-00.html).
The useful link to discuss such method can also be shown at [https://groups.google.com/forum/#!topic/bbr-dev/EamFWz1N_n4](https://groups.google.com/forum/#!topic/bbr-dev/EamFWz1N_n4). 

The method regards the sending and receiving of each packet as a rate sample, the method records the data acknowledged (called `delivered`), and the time spent on it. Later it divides the data by the time interval, which represents the capability of such a link to deliver data, i.e., the bottleneck bandwidth.

Even if the method is clear and simple in theory, a lot of practical issues have to be addressed. For instance, the packet generated from an app-limit application, i.e., the sending rate is not limited by the network but the packet generation rate of applications,  should not be used to generate a valid rate sample, and the packets generated in the `probe rtt` and `probe drain` phase should not be included. In our DPDK implementation, since we cannot disable the `tso` that causes lots of partial acks, partial acks for one large packet generate similar bandwidth, which can easily cheat the bbr in the `startup` phase, and makes it feel like it reaches the bottleneck.

In our implementation, it is also the most vulnerable part, and we refer the  [tcp_rate.cc](https://elixir.bootlin.com/linux/latest/source/net/ipv4/tcp_rate.c) in the Linux kernel. To generate such a rate sample, several packet states have to be recorded in the unacked data, i.e., `_snd.data` queue. These states include
```         
|
            bool app_limit;  
            std::chrono::high_resolution_clock::time_point high_tx_time;
            std::chrono::high_resolution_clock::time_point p_ack_time;
            uint64_t p_delivered_bytes;
            std::chrono::high_resolution_clock::time_point p_send_time;
            std::chrono::high_resolution_clock::time_point p_first_send_time;
            uint64_t send_rate;
```

The `app_limit` is used to determine whether the sending of this packet is constricted by the network or application. Since the bbr needs a high granularity clock, i.e, microseconds, we use a variable `high_tx_time` to record this to generate rtt measurement. The `p_delivered_bytes` records the highest sequence of data delivered when this packet is sent. The `p_ack_time` records the time when the newest packet is received. The `p_send_time` records the sending time of this packet. The `p_first_send_time` records the sending time of the newest received packet when this packet is sent from source. `send_rate` is kept temporarily to record the pacing rate when this packet is sent, and it is useful in the startup phase, though it is not included in the original implementation.

We can easily validate our pacing system by running our program on a link with the capacity of 10Mbps, i.e., (1.25MB/s), and observe the estimation change.
![rate-estimation](rate-estimation.PNG)

### The state and model transition algorithm

This component is just developed as the instruction of TCP-BBR:

```

             |
             V
    +---> STARTUP  ----+
    |        |         |
    |        V         |
    |      DRAIN   ----+
    |        |         |
    |        V         |
    +---> PROBE_BW ----+
    |      ^    |      |
    |      |    |      |
    |      +----+      |
    |                  |
    +---- PROBE_RTT <--+

```
The bbr transfers between the above 4 states. We use `void enter_startup(),void enter_drain(),void enter_probe_bw() and void enter_probe_rtt()` to implement them. In each state, bbr has different `pacing_gain` and `cwnd_gain`, which are used to multiply the `pacing_rate` and `cwnd` to explore more bandwidth or reduce the sending rate and cwnd. Most importantly, the main interface for this component is the `bbr_update_model_and_state` function, each time a packet is received, the bbr will call this function to update its state and model. Since the `btl_bw` (bottleneck bandwidth) and `prop_rtt` (propagation delay) are the two variables bbr concerns, the `update_rtt` and `update_btl_bw` function are responsible for them.

In the `startup` phase, bbr doubles its sending rate and cwnd as other tcp congestion control algorithms. The only difference is that it not only double cwnd, but also controls the sending rate 2.885X as the btl_bw, and packets are paced as the pacing_rate (pacing_gain * btl_bw). After the `startup` phase, usually a queue in the link is built, bbr enters the `drain` phase to drain the queue by slowing down the packet pacing. When the packets in flight are smaller than the estimated BDP (btl * prop_rtt), bbr exits the `drain` state and enter the `probe bw` state. The `prop bw` state is the normal state for bbr, it paces around the `btl_bw` it measured previously, and increases 1.25X by its pacing rate to try to get more bandwidth in such state, and decreases 0.75X its pacing rate to carefully drain the queue caused from the previous attempt. After 10s elapsed, the bbr thinks the smallest rtt it measured previously has expired, so it enters the `prop_rtt` phase. It throttles its cwnd to 4*mss to wait for all packets in flight are received, since it is easier to get the propagation delay in an empty link.

### The packet pacing system

This component is responsible to limit sending rate to the estimated bottleneck bandwidth as TCP BBR. We bypass all the original `output()` function, and instead of sending a packet inmmediately, we use a timer to pace all packets to the bottleneck bandwidth, and a `time_out()` function is triggered to call `output()` function to send packets at the speed of the bottleneck bandwidth.

We can easily validate our pacing system by directly limit the pacing rate to be a fixed number, e.g., 8Mbps (1MB/s), and observe the downloading rate.

![rate-limiting](new-packet-pacing.PNG)




## Reproducing the results 

We can observe the long-term performance of TCP BBR by fetching a large video, which makes the transmission last long enough. In our tests, a 1.3GB video file is placed under the home directory. We just use the same environment as what in BBR paper, i.e., a link with the capacity of 10Mbps, delay of 40ms.

1. use the Seastar as a http server, and run `sudo build/release/apps/httpd/httpd --network-stack native --dpdk-pmd --dhcp 0 --host-ipv4-addr 192.168.11.1 --netmask-ipv4-addr 255.255.255.0 --port 80 --collectd 0 --smp 2 -m 2G --lro on --tso on --tcp-congestion tcp_bbr ` 

2. use a ubuntu PC as a client, and run `curl --local-port 10000  -o video-1.3GB.mkv 192.168.11.1/file/home/hcy/video-1.3GB.mkv`

### The basic state and model transition

We can easily observe the basic state and model transition within a single TCP flow. We captured 3000 packets that send from server to client with the Wireshark. The graph below shows the throughput in different BBR phases. 


![state-and-model-transition](single-flow-behavior.PNG)

We can easily see the bbr starts with a `startup` phase which doubles the sending rate in each round. It reaches the bottleneck bandwidth(1.25 MiB/s) very quickly. Then bbr keeps trying to increase its rate to explore more bandwidth, but since it exceeds the link capacity, even if the server increases the sending rate, it will not get bigger throughput. After 3 rounds, the bbr knows it reaches the bottleneck bandwidth. Then it enters the second phase `drain`. It reduces the sending rate to 0.75X the bottleneck bandwidth until the flight size equals its estimated BDP. It can be shown in the graph, there is an obvious rate drop after the peak. After that, the bbr enters the `probe bw` phase, which is the normal state for bbr. In this phase, bbr paces around its estimated bottleneck bandwidth, and it multiplies its pacing rate by [1.25, 0.75, 1, 1, 1, 1] to try to grab more free bandwidth and not cause queuing delay. Most importantly, the `probe rtt` phase is the distinct phase for bbr, in this phase, bbr suddenly reduces its congestion window to 4*mss, and wait for all the packets in flight are received. By that time, bbr has the best chance to measure the propagation rtt of a link and excludes the impact of queuing delay. bbr enters the `probe rtt` phase 10s later than the time it measured the minimum rtt. And we can see an obvious rate valley every 10s in the graph.  
