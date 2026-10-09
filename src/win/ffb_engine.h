// Telemetry force-feedback engine: a thread that finds the player's car in GP4's memory,
// runs FfbModel at a fixed rate and drives a constant-force effect on the wheel.
#pragma once
#include <atomic>
#include <thread>

namespace gp4u {

class FfbEngine {
public:
    static FfbEngine& get();
    bool start();            // idempotent; false if it cannot run (no FFB wheel, mode off)
    void stop();             // zero force, release the effect, join the thread
    void on_device_reset();  // the game reset the wheel's effects (mix mode)

private:
    FfbEngine() = default;
    void run();
    std::thread th_;
    std::atomic<bool> running_{false};
    std::atomic<bool> reset_{false};
};

}  // namespace gp4u
