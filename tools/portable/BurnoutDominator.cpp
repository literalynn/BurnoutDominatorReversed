// BurnoutDominator.exe: the one file of a portable installation on Windows.
//
// Put it in an empty folder and start it. It brings into that folder a Python and a Git
// (official builds, checked against the SHA-256 below) and the project's latest release, then
// opens the launcher's window (tools/launcher/launcher.cpp), whose program, the project's
// source\tools\launcher.py, does everything else: the ISO, the build, the game, updates.
// Nothing is written outside the folder, so it can be moved.
//
// Build (the release workflow does it), with the window and raylib:
//   cmake -S tools/launcher -B build/launcher && cmake --build build/launcher --config Release
#include <windows.h>
#include <bcrypt.h>
#include <urlmon.h>
#include <wininet.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace fs = std::filesystem;

int launcherMain(int argc, char **argv); // the launcher's window: ../launcher/launcher.cpp

namespace
{
    struct Package
    {
        const wchar_t *name;   // shown while it downloads
        const wchar_t *folder; // under the launcher's folder
        const wchar_t *proof;  // a file that exists once the package is in place
        const wchar_t *url;
        const char *sha256;    // "" for the project's own release, which changes with every version
    };

    const Package kPackages[] = {
        {L"Python", L"python", L"python\\pythonw.exe",
         L"https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip",
         "4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3"},
        {L"Git", L"git", L"git\\cmd\\git.exe",
         L"https://github.com/git-for-windows/git/releases/download/v2.56.0.windows.4/MinGit-2.56.0.4-64-bit.zip",
         "e3bf78351fe777fba47ddee5950082f8a98bb0f02604fdc7ba3ee2cb68ffa877"},
        {L"Burnout Dominator Reversed", L"source", L"source\\tools\\launcher.py",
         L"https://github.com/literalynn/BurnoutDominatorReversed/releases/latest/download/source.zip", ""},
    };

    constexpr UINT kStatus = WM_APP + 1; // g_status changed
    constexpr UINT kDone = WM_APP + 2;   // wParam: 1 on success
    HWND g_window = nullptr;
    std::mutex g_statusLock; // the download thread writes g_status, the window reads it
    std::wstring g_status = L"Starting...";
    std::wstring g_error;

    fs::path program()
    {
        std::wstring path(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (length < path.size())
            {
                path.resize(length);
                return path;
            }
            path.resize(path.size() * 2);
        }
    }

    fs::path folder()
    {
        return program().parent_path();
    }

    std::string utf8(const fs::path &path)
    {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    std::string sha256(const fs::path &file)
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        std::string hex;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
            return hex;
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0)
        {
            std::ifstream input(file, std::ios::binary);
            std::vector<char> block(1 << 16);
            while (input.read(block.data(), static_cast<std::streamsize>(block.size())) || input.gcount() > 0)
                BCryptHashData(hash, reinterpret_cast<PUCHAR>(block.data()), static_cast<ULONG>(input.gcount()), 0);
            unsigned char digest[32];
            if (BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0)
            {
                static const char kDigits[] = "0123456789abcdef";
                for (unsigned char byte : digest)
                {
                    hex += kDigits[byte >> 4];
                    hex += kDigits[byte & 15];
                }
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return hex;
    }

    // Runs a command without a window; returns its exit code, or -1 when it could not start.
    int run(std::wstring command, const fs::path &directory, bool wait)
    {
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                            directory.c_str(), &startup, &process))
            return -1;
        DWORD code = 0;
        if (wait)
        {
            WaitForSingleObject(process.hProcess, INFINITE);
            GetExitCodeProcess(process.hProcess, &code);
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return static_cast<int>(code);
    }

    void status(const std::wstring &text)
    {
        {
            const std::lock_guard<std::mutex> lock(g_statusLock);
            g_status = text;
        }
        if (g_window)
            PostMessageW(g_window, kStatus, 0, 0);
    }

    bool install(const fs::path &root, const Package &package)
    {
        std::error_code ignored;
        const fs::path archive = root / (std::wstring(package.folder) + L".zip.part");
        status(std::wstring(L"Downloading ") + package.name + L"...");
        // BDR_SOURCE_URL names another archive of the project: a fork's release, or a local file to test with.
        std::wstring url = package.url;
        wchar_t other[2048];
        if (package.sha256[0] == '\0' && GetEnvironmentVariableW(L"BDR_SOURCE_URL", other, 2048) > 0)
            url = other;
        DeleteUrlCacheEntryW(url.c_str()); // never an older copy from the system's cache
        if (URLDownloadToFileW(nullptr, url.c_str(), archive.c_str(), 0, nullptr) != S_OK)
        {
            g_error = std::wstring(L"Could not download ") + package.name + L".\nCheck the Internet connection, then start again.\n\n" + url;
            fs::remove(archive, ignored);
            return false;
        }
        if (package.sha256[0] != '\0' && sha256(archive) != package.sha256)
        {
            g_error = std::wstring(L"The download of ") + package.name + L" is not the expected file. Nothing was installed from it.";
            fs::remove(archive, ignored);
            return false;
        }
        status(std::wstring(L"Unpacking ") + package.name + L"...");
        const fs::path target = root / package.folder;
        fs::remove_all(target, ignored); // an interrupted first start
        fs::create_directories(target, ignored);
        // tar.exe ships with Windows 10 and 11 and reads ZIP archives.
        wchar_t system[MAX_PATH];
        GetSystemDirectoryW(system, MAX_PATH);
        const std::wstring command = L"\"" + std::wstring(system) + L"\\tar.exe\" -xf \"" + archive.wstring() + L"\" -C \"" + target.wstring() + L"\"";
        const int code = run(command, root, true);
        fs::remove(archive, ignored);
        if (code != 0 || !fs::exists(root / package.proof))
        {
            g_error = std::wstring(L"Could not unpack ") + package.name + L" into\n" + target.wstring();
            fs::remove_all(target, ignored);
            return false;
        }
        return true;
    }

    DWORD WINAPI prepare(LPVOID)
    {
        const fs::path root = folder();
        bool ok = true;
        for (const Package &package : kPackages)
        {
            if (fs::exists(root / package.proof))
                continue;
            ok = install(root, package);
            if (!ok)
                break;
            if (std::wstring(package.folder) == L"python")
            {
                // The embeddable Python only looks where this file says: its own folder, and the
                // project's tools, which import one another.
                std::ofstream paths(root / L"python" / L"python312._pth", std::ios::app);
                paths << "..\\source\\tools\n";
            }
        }
        PostMessageW(g_window, kDone, ok ? 1 : 0, 0);
        return 0;
    }

    LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case kStatus:
            InvalidateRect(window, nullptr, TRUE);
            return 0;
        case kDone:
            DestroyWindow(window);
            PostQuitMessage(wParam ? 0 : 1);
            return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT paint;
            const HDC dc = BeginPaint(window, &paint);
            RECT area;
            GetClientRect(window, &area);
            // The launcher's look: near black, one red line, plain text.
            const HBRUSH black = CreateSolidBrush(RGB(10, 10, 11)), red = CreateSolidBrush(RGB(229, 32, 46));
            FillRect(dc, &area, black);
            RECT accent = area;
            accent.bottom = 2;
            FillRect(dc, &accent, red);
            SetBkMode(dc, TRANSPARENT);
            const HFONT title = CreateFontW(26, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            const HFONT text = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            RECT line = area;
            line.top = 36;
            SetTextColor(dc, RGB(236, 236, 238));
            HGDIOBJ previous = SelectObject(dc, title);
            DrawTextW(dc, L"Burnout Dominator Reversed", -1, &line, DT_CENTER | DT_SINGLELINE);
            line.top = 84;
            SelectObject(dc, text);
            {
                const std::lock_guard<std::mutex> lock(g_statusLock);
                DrawTextW(dc, g_status.c_str(), -1, &line, DT_CENTER | DT_SINGLELINE);
            }
            line.top = 112;
            SetTextColor(dc, RGB(138, 138, 147));
            DrawTextW(dc, L"First start: the launcher's files are being downloaded into this folder.", -1, &line, DT_CENTER | DT_SINGLELINE);
            SelectObject(dc, previous);
            DeleteObject(title);
            DeleteObject(text);
            DeleteObject(black);
            DeleteObject(red);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_CLOSE:
            return 0; // the downloads are short; closing in the middle would leave half a folder
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    bool prepared(const fs::path &root)
    {
        for (const Package &package : kPackages)
            if (!fs::exists(root / package.proof))
                return false;
        return true;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    const fs::path exe = program(), root = exe.parent_path();
    std::error_code ignored;
    fs::remove(exe.wstring() + L".old", ignored); // the program an update replaced
    if (!prepared(root))
    {
        WNDCLASSW type{};
        type.lpfnWndProc = windowProc;
        type.hInstance = instance;
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        type.lpszClassName = L"BurnoutDominatorFirstStart";
        RegisterClassW(&type);
        const int width = 640, height = 170;
        g_window = CreateWindowExW(WS_EX_TOPMOST, type.lpszClassName, L"Burnout Dominator Reversed", WS_POPUP | WS_VISIBLE,
                                   (GetSystemMetrics(SM_CXSCREEN) - width) / 2, (GetSystemMetrics(SM_CYSCREEN) - height) / 2,
                                   width, height, nullptr, nullptr, instance, nullptr);
        CloseHandle(CreateThread(nullptr, 0, prepare, nullptr, 0, nullptr));
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (message.wParam != 0)
        {
            MessageBoxW(nullptr, g_error.c_str(), L"Burnout Dominator Reversed", MB_ICONERROR);
            return 1;
        }
    }
    // The launcher reads this to keep everything inside the folder. A Python configured on the
    // PC must not leak into ours.
    SetEnvironmentVariableW(L"BDR_PORTABLE", root.c_str());
    SetEnvironmentVariableW(L"BDR_LAUNCHER_EXE", exe.c_str()); // an update puts the release's program in its place
    SetEnvironmentVariableW(L"PYTHONHOME", nullptr);
    SetEnvironmentVariableW(L"PYTHONPATH", nullptr);
    const fs::path python = root / L"python" / L"pythonw.exe", script = root / L"source" / L"tools" / L"launcher.py";
    std::string arguments[] = {"BurnoutDominator", "--python", utf8(python), "--script", utf8(script)};
    char *argv[] = {arguments[0].data(), arguments[1].data(), arguments[2].data(), arguments[3].data(), arguments[4].data()};
    const int code = launcherMain(5, argv);
    if (code == 3) // an update replaced this program: the new one takes over
        run(L"\"" + exe.wstring() + L"\"", root, false);
    if (code == 2) // no window could open (no OpenGL 3.3): the launcher as a page, in an Edge window
        run(L"\"" + python.wstring() + L"\" \"" + script.wstring() + L"\"", root, false);
    if (code == 1)
    {
        MessageBoxW(nullptr, L"The launcher could not be started. Delete the python and source folders next to this file, then start again.",
                    L"Burnout Dominator Reversed", MB_ICONERROR);
        return 1;
    }
    return 0;
}
