#pragma once

// Set when an EE checkpoint asks generated code to return to the scheduler
// (an event, an interrupt or the end of a time slice is due). Every guest call
// on the way out must then return as well: after such a return ctx->pc holds
// the resume point, which can equal the entry of the function that was being
// called (mutual recursion A -> B -> A -> B), so it cannot tell a finished
// call apart from an interrupted one. The scheduler clears it before each
// dispatch. Game thread only.
namespace ps2_ee_unwind
{
    inline thread_local bool g_returningToScheduler = false;
}
