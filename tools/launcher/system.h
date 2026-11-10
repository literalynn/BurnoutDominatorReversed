// What the launcher's window asks of the operating system besides raylib, in a file of its own: raylib.h and
// windows.h cannot be included together.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sys
{
    // Sends `request` to 127.0.0.1:port and hands what comes back to receive() until the server closes the
    // connection, receive() returns false, or nothing comes for timeoutMs. False when it could not connect.
    bool exchange(int port, const std::string &request, int timeoutMs, const std::function<bool(const char *, size_t)> &receive);
    int freePort(); // free on 127.0.0.1 now; 0 when none is
    std::string randomToken();
    // Starts a program without a console window and without waiting: its process, 0 when it could not start.
    std::intptr_t spawn(const std::vector<std::string> &command);
    int exitCode(std::intptr_t process); // -1 while it runs
    // One holder of `name` at a time, until it exits or releases it: false when another process holds it.
    bool claim(const std::string &name);
    void release();
    void allowForeground(); // lets the launcher already open come to the front when asked
    void darkTitleBar(void *window);
    std::string programPath(); // this program's file on Windows, "" elsewhere
}
