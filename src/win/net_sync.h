// Free-running online races: replaces GP4's per-step network exchange.
//
// GP4 (0x445d60, called once per simulation step at 0x43637f) sends its car and then
// waits until the other machines' packets for that step have arrived. One round trip per
// step: at 40 ms of ping the race runs at ~24 steps/s instead of 60 (slow motion), and
// every extra player adds to it. The mod redirects that call: each machine sends the same
// packet GP4 would (the race master: its car, one AI car, race time, random seed and
// events; the others: their car and race time), processes whatever has arrived, and goes
// on. Remote cars are overwritten by GP4's own code (0x44580c) as packets come in.
// The race clocks are kept together by core/net_pacer.h. See docs/GP4-FINDINGS.md "Red".
#pragma once
#include <cstdint>
#include <string>

#include "core/net_pacer.h"

namespace gp4u {

struct NetSyncTargets {   // GP4.exe addresses; overridable for the e2e test
    uint32_t call_site;   // call to the per-step exchange (E8 rel32)
    uint32_t step_fn;     // GP4's per-step exchange (its target)
    uint32_t recv_fn;     // receive one packet: CF = race packet processed
    uint32_t recv_post;   // per-packet bookkeeping after recv_fn (events from clients)
    uint32_t send_fn;     // send a type-1 packet: esi = car record or 0
    uint32_t next_ai_fn;  // master: pick the next AI car to send
    uint32_t is_master;   // byte, 0xFF on the race master
    uint32_t recv_state;  // byte, 0 = nothing was waiting
    uint32_t player_car;  // dword, local human car record or 0
    uint32_t ai_car;      // dword, AI car to send or -1
    uint32_t pkt_flags32; // dword, flags of the packet to send / just received
    uint32_t events;      // dword, master's pending event bits
    uint32_t got_mask;    // dword, players heard from this step
    uint32_t my_index;    // byte, local player index
    uint32_t pkt_from;    // byte, sender of the packet just received
    uint32_t pkt_type;    // byte, type of the packet just received
    uint32_t pkt_hdr;     // byte, header flags of the packet just received (0x80 = master)
    uint32_t pkt_time;    // dword, race time in the packet just received
    uint32_t catchup;     // byte, 0xFF = GP4 runs another step before drawing
    uint32_t race_time;   // dword, local race time (ms)
    // Prediction of remote cars (core/net_predict.h); 0 = not available.
    uint32_t car_base = 0;    // first car record (22 x 0x41C)
    uint32_t car_update = 0;  // GP4's per-car update: esi = car record
    // Players that stopped sending (their GP4 crashed); 0 = not available.
    uint32_t active_mask = 0; // dword, players still in the race (bit = player index)
    uint32_t gone_mask = 0;   // dword, players to take out at the end of the step
};
NetSyncTargets gp4_net_sync_targets();

bool install_net_sync(const NetSyncTargets& t, const NetPacerConfig& cfg, std::string* why);
bool net_sync_installed();

// Remote cars carried on by GP4's own physics while their packet is late
// (core/net_predict.h); at most max_steps steps in a row.
void net_sync_set_prediction(bool on, int max_steps);

// A player whose GP4 crashed sends nothing and its car stays on the track, frozen. After
// `ms` without a packet from it, it is taken out as if it had left the game: the race
// director announces it (GP4's own path, 0x4459b7), or, if the silent one is the director,
// every PC takes it out itself and GP4 picks a new director. 0 = never.
void net_sync_set_drop_after(double ms);

// Called at the start of every redirected step, before any receive (the playout buffer
// in win/net_hook.cpp gives each sender one race packet per step).
void net_sync_set_step_callback(void (*on_step)());

// Test hook: real-time source in ms (default QueryPerformanceCounter) and sleep.
void net_sync_set_clock(double (*now_ms)(), void (*sleep_ms)(int));

struct NetSyncStats {
    unsigned steps = 0, sent = 0, received = 0, waits = 0, catchups = 0, predicted = 0, dropped = 0;
    double wait_ms = 0;
    bool lead_known = false;
    double lead_min = 0, lead_max = 0, lead_sum = 0;
    unsigned lead_n = 0;
};
NetSyncStats net_sync_stats();   // since install (the log gets per-second summaries)

}  // namespace gp4u
