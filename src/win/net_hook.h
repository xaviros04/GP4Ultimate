// Network log: wraps the DirectPlay object GP4 creates (CoCreateInstance, patched in
// GP4.exe's import table) and writes what it does to gp4ultimate.log ("net:" lines): session
// setup, connection addresses, players, system messages, and once per second the
// traffic summary from core/net_stats.h. Nothing GP4 sends is changed; in free-running
// races the race packets it receives are paced (core/net_playout.h).
#pragma once

namespace gp4u {

// Installs the CoCreateInstance hook in the main module (the game). dumps = hex dumps
// per packet kind and direction; log = write the net: lines; playout = pace race packets
// through core/net_playout.h (free-running races, win/net_sync.h).
void install_net_hooks(int dumps, bool log, bool playout);

// Who directs online races (clock and AI cars): 0 GP4 decides (the fastest PC), 1 this PC
// if possible, 2 another PC if possible, 3 auto (through the server: the best connection
// to it; with DirectPlay GP4 decides). Applied when the game is created or joined.
void net_set_race_director(int mode);

// Free-running races: a simulation step starts (called by net_sync before its receives).
void net_playout_begin_step();
struct NetPlayoutCounts {
    unsigned delivered, caught_up;
};
NetPlayoutCounts net_playout_counts();

}  // namespace gp4u
