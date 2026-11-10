#include "system.h"

#include <filesystem>
#include <random>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <dwmapi.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace sys
{
    namespace
    {
#ifdef _WIN32
        using Socket = SOCKET;
        const Socket kNoSocket = INVALID_SOCKET;
        void closeSocket(Socket s) { closesocket(s); }
        HANDLE g_claim = nullptr;

        std::wstring wide(const std::string &text)
        {
            std::wstring out(MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()));
            return out;
        }
#else
        using Socket = int;
        const Socket kNoSocket = -1;
        void closeSocket(Socket s) { close(s); }
        int g_claim = -1;
#endif

        Socket openSocket(int timeoutMs)
        {
#ifdef _WIN32
            static const bool started = [] {
                WSADATA data;
                return WSAStartup(MAKEWORD(2, 2), &data) == 0;
            }();
            const DWORD timeout = static_cast<DWORD>(timeoutMs);
#else
            static const bool started = [] {
                signal(SIGPIPE, SIG_IGN); // a write to a closed connection is an error, not the end of the program
                return true;
            }();
            const timeval timeout{timeoutMs / 1000, timeoutMs % 1000 * 1000};
#endif
            const Socket s = started ? socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) : kNoSocket;
            if (s != kNoSocket)
            {
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
                setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
            }
            return s;
        }

        sockaddr_in loopback(int port)
        {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(static_cast<unsigned short>(port));
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            return address;
        }
    }

    bool exchange(int port, const std::string &request, int timeoutMs, const std::function<bool(const char *, size_t)> &receive)
    {
        const Socket s = openSocket(timeoutMs);
        if (s == kNoSocket)
            return false;
        const sockaddr_in address = loopback(port);
        bool sent = connect(s, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0;
        for (size_t done = 0; sent && done < request.size();)
        {
            const auto count = send(s, request.data() + done, static_cast<int>(request.size() - done), 0);
            sent = count > 0;
            done += sent ? static_cast<size_t>(count) : 0;
        }
        char block[16384];
        for (bool more = sent; more;)
        {
            const auto count = recv(s, block, sizeof(block), 0);
            more = count > 0 && receive(block, static_cast<size_t>(count));
        }
        closeSocket(s);
        return sent;
    }

    int freePort()
    {
        const Socket s = openSocket(1000);
        sockaddr_in address = loopback(0);
        auto size = static_cast<socklen_t>(sizeof(address));
        int port = 0;
        if (s != kNoSocket && bind(s, reinterpret_cast<const sockaddr *>(&address), size) == 0 &&
            getsockname(s, reinterpret_cast<sockaddr *>(&address), &size) == 0)
            port = ntohs(address.sin_port);
        if (s != kNoSocket)
            closeSocket(s);
        return port;
    }

    std::string randomToken()
    {
        std::random_device random; // the system's random source
        std::string token;
        for (int i = 0; i < 32; ++i)
            token += "0123456789abcdef"[random() % 16];
        return token;
    }

#ifdef _WIN32
    std::intptr_t spawn(const std::vector<std::string> &command)
    {
        std::wstring line;
        for (const std::string &part : command) // quoted: none of ours holds a quote or ends with a backslash
            line += (line.empty() ? L"\"" : L" \"") + wide(part) + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
            return 0;
        CloseHandle(process.hThread);
        return reinterpret_cast<std::intptr_t>(process.hProcess);
    }

    int exitCode(std::intptr_t process)
    {
        const HANDLE handle = reinterpret_cast<HANDLE>(process);
        DWORD code = 0;
        if (WaitForSingleObject(handle, 0) != WAIT_OBJECT_0 || !GetExitCodeProcess(handle, &code))
            return -1;
        return static_cast<int>(code);
    }

    bool claim(const std::string &name)
    {
        g_claim = CreateMutexW(nullptr, FALSE, wide("Local\\" + name).c_str());
        return g_claim && GetLastError() != ERROR_ALREADY_EXISTS;
    }

    void release()
    {
        CloseHandle(g_claim);
    }

    void allowForeground()
    {
        AllowSetForegroundWindow(ASFW_ANY);
    }

    void darkTitleBar(void *window)
    {
        const BOOL dark = TRUE;
        const COLORREF caption = RGB(10, 10, 11), text = RGB(236, 236, 238); // the page's background and text
        DwmSetWindowAttribute(static_cast<HWND>(window), DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        DwmSetWindowAttribute(static_cast<HWND>(window), DWMWA_CAPTION_COLOR, &caption, sizeof(caption)); // Windows 11
        DwmSetWindowAttribute(static_cast<HWND>(window), DWMWA_TEXT_COLOR, &text, sizeof(text));
    }

    std::string programPath()
    {
        std::wstring path(32768, L'\0'); // the longest path Windows has
        path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
        std::string out(WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr), '\0');
        WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), out.data(), static_cast<int>(out.size()), nullptr, nullptr);
        return out;
    }
#else
    std::intptr_t spawn(const std::vector<std::string> &command)
    {
        std::vector<char *> arguments;
        for (const std::string &part : command)
            arguments.push_back(const_cast<char *>(part.c_str()));
        arguments.push_back(nullptr);
        pid_t process = 0;
        return posix_spawnp(&process, arguments[0], nullptr, nullptr, arguments.data(), environ) == 0 ? process : 0;
    }

    int exitCode(std::intptr_t process)
    {
        int status = 0;
        if (waitpid(static_cast<pid_t>(process), &status, WNOHANG) != process)
            return -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }

    bool claim(const std::string &name)
    {
        // O_CLOEXEC: the backend this program starts must not inherit the lock and keep it after the window closed.
        const std::string path = (std::filesystem::temp_directory_path() / (name + ".lock")).string();
        g_claim = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        return g_claim >= 0 && flock(g_claim, LOCK_EX | LOCK_NB) == 0;
    }

    void release()
    {
        close(g_claim);
    }

    void allowForeground() {}
    void darkTitleBar(void *) {}

    std::string programPath()
    {
        return "";
    }
#endif
}
