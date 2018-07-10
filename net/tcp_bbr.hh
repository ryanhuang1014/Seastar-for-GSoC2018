//
// Created by ryan on 6/7/18.
//

#ifndef SEASTAR_TCP_BBR_HH
#define SEASTAR_TCP_BBR_HH

/* Here is a state transition diagram for BBR 
 *             |
 *             V
 *    +---> STARTUP  ----+
 *    |        |         |
 *    |        V         |
 *    |      DRAIN   ----+
 *    |        |         |
 *    |        V         |
 *    +---> PROBE_BW ----+
 *    |      ^    |      |
 *    |      |    |      |
 *    |      +----+      |
 *    |                  |
 *    +---- PROBE_RTT <--+
 *
*/

#include <tcp.hh>

class tcp_bbr{
    enum class tcp_bbr_state : uint8_t {
        BBR_STARTUP        = (1 << 0),
        BBR_DRAIN          = (1 << 1),
        BBR_PROBE_BW       = (1 << 2),
        BBR_PROBE_RTT      = (1 << 3)
    }; 
   
    tcp_bbr_state _state;

    //constants for tcp bbr
    // the maximum time for probe_bw state
    static constexpr uint8_t BBR_PROBE_BW_TIME = 10;
    // the minimum time for probe_rtt state
    static constexpr double BBR_PROBE_RTT = 0.2;
    // the slope for inflating cwnd to test whether can grab additional bw
    static constexpr double BBR_GROWTH_SLOPE = 1.25;
    // the threshold for testing whether achieving full pipe
    static constexpr uint8_t BBR_FULL_PIPE_CNT = 3;
    // the minimum cwnd for probe_rtt state 
    static constexpr uint8_t MIN_CWND_PROBE_RTT = 4;
    // the cwnd gain used in startup state 
    static constexpr double BBR_STARTUP_GAIN = 2.885;
    // the cwnd gain used in drain state to drain packet queuing caused in startup state 
    static constexpr double BBR_DRAIN_GAIN = 0.347;
    // the paragram used to make up the delayed ack in receivers 
    static constexpr uint8_t BBR_MAKE_UP_CWND_GAIN = 2;
    // the pacing gain array used in probe bw state 
    static constexpr double BBR_PACING_GAIN[] = {
      1.25, 0.75,
      1, 1, 1, 1, 1, 1
    };
    
    //signal showing achieving full pipe 
    bool is_full_pipe;

    //state transition func 
    void enter_startup();
    void enter_drain();
    void enter_probe_bw();
    void enter_probe_rtt();

    // check state transition condition and proceed state transition 
    void check_full_bw();
    void check_probe_rtt();
    void check_drain();
    void check_advance_cycle_phase();
    
    //cycle phase transition func used in probe bw state
    void advance_cycle_phase();
    bool is_next_cycle_phase();

    // save and restore cwnd because of state transition or packet loss 
    void save_cwnd();
    void restore_cwnd();

    // pacing related func 
    uint32_t init_pacing_rate();
    uint32_t set_pacing_rate_with_gain(uint8_t gain);

    // cwnd update func 
    void update_bw();
    // rtt update func 
    void update_rtt();

    // universal interface for updating cwnd in a bbr way
    uint32_t bbr_update_cwnd();


};

#endif //SEASTAR_TCP_BBR_HH
