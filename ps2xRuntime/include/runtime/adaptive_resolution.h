#pragma once

#include <algorithm>
#include <atomic>

namespace ps2x
{
    // adaptive_resolution (settings.ini): while the GPU cannot keep up with the player's resolution,
    // the picture steps down (4x to 2x to the PS2's own) and steps back up once the GPU has room.
    // The paraLLEl-GS backend measures, about once a second, the GPU time of its passes over the
    // time they were spread over, and asks update() which step to draw at.
    class AdaptiveResolution
    {
    public:
        // Busy above kLower for kLowerWindows windows in a row: one step down. One step up when the
        // step above, estimated at kStepCost times the busy time of this one (2x costs about 2.1
        // times 1x, 4x 1.3 times 2x, on an RTX 5090 in a race), stays under kRaise for
        // kRaiseWindows windows, and not before a wait that doubles at each step down (30 s, 60 s,
        // up to 10 minutes), so that a scene at the limit does not switch back and forth.
        static constexpr double kLower = 0.9;
        static constexpr int kLowerWindows = 2;
        static constexpr double kStepCost = 2.5;
        static constexpr double kRaise = 0.7;
        static constexpr int kRaiseWindows = 8;
        static constexpr double kFirstWait = 30.0;
        static constexpr double kLongestWait = 600.0;

        // Steps below the player's resolution: 0 draws it.
        int step() const { return m_step; }

        // One window: GPU busy seconds over wall seconds, and how many steps exist below the
        // player's resolution. Returns the step to draw at.
        int update(double gpuSeconds, double wallSeconds, int steps)
        {
            m_step = std::clamp(m_step, 0, std::max(steps, 0));
            if (wallSeconds <= 0.0)
                return m_step;
            m_clock += wallSeconds;
            const double busy = std::clamp(gpuSeconds / wallSeconds, 0.0, 1.0);
            if (busy > kLower)
            {
                m_raiseWindows = 0;
                if (++m_lowerWindows >= kLowerWindows && m_step < steps)
                {
                    ++m_step;
                    m_lowerWindows = 0;
                    m_raiseAt = m_clock + m_wait;
                    m_wait = std::min(m_wait * 2.0, kLongestWait);
                }
                return m_step;
            }
            m_lowerWindows = 0;
            if (m_step > 0 && m_clock >= m_raiseAt && busy * kStepCost < kRaise)
            {
                if (++m_raiseWindows >= kRaiseWindows)
                {
                    --m_step;
                    m_raiseWindows = 0;
                }
            }
            else
                m_raiseWindows = 0;
            return m_step;
        }

    private:
        int m_step = 0;
        int m_lowerWindows = 0;
        int m_raiseWindows = 0;
        double m_clock = 0.0;
        double m_raiseAt = 0.0;
        double m_wait = kFirstWait;
    };

    // The step the backend draws at now, for the F1 menu (0 = the player's resolution).
    inline std::atomic<int> g_adaptiveResolutionStep{0};
}
