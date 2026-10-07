// Sampling profiler for the guest thread (EE code, IOP, GS and host stubs all run
// on it). Opt-in with BDR_PROFILE=1; Windows only, and it names functions only
// when the executable has a PDB next to it (link with /DEBUG). Observation only.
#pragma once

#include <memory>

namespace bdr_profile {

class Sampler {
public:
    Sampler();
    ~Sampler();
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    // Prints the report now (also done by the destructor when sampling is on).
    void report();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace bdr_profile
