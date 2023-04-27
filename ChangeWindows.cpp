#include <winsock2.h>
#include <Windows.h>
#include <ws2ipdef.h>
#include <winhttp.h>
#include <shellapi.h>
#include <commdlg.h>

#include <VersionHelpers.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>

#include "Windows_Symbol.hpp"
#include "gason.h"

#pragma warning (disable:6262) // stack usage warning
#pragma warning (disable:26812) // unscoped enum warning

extern "C" IMAGE_DOS_HEADER __ImageBase;
extern "C" int _fltused = 0;

namespace {
    const wchar_t name [] = L"TRIMCORE.ChangeWindows";
    const VS_FIXEDFILEINFO * version = nullptr;

    HMENU menu = NULL;
    HWND window = NULL;
    UINT WM_Application = WM_NULL;
    UINT WM_TaskbarCreated = WM_NULL;
    HKEY settings = NULL;   // app settings
    HKEY builds = NULL;     // last versions
    HKEY alerts = NULL;     // reported changes
    HINTERNET internet = NULL;
    HINTERNET connection = NULL;

    BOOL (WINAPI * pfnChangeWindowMessageFilterEx) (HWND, UINT, DWORD, LPVOID) = NULL;

    auto checking = 0u;
    char buffer [512 * 1024];
    char allocator [sizeof buffer];
    std::size_t allocated = 0;

    LRESULT CALLBACK wndproc (HWND, UINT, WPARAM, LPARAM);
    WNDCLASS wndclass = {
        0, wndproc, 0, 0,
        reinterpret_cast <HINSTANCE> (&__ImageBase),
        NULL, NULL, NULL, NULL, name
    };
    NOTIFYICONDATA nid = {
        sizeof (NOTIFYICONDATA), NULL, 1u, NIF_MESSAGE | NIF_STATE | NIF_ICON | NIF_TIP | NIF_SHOWTIP,
        WM_APP, NULL, { 0 }, 0u, 0u, { 0 }, { NOTIFYICON_VERSION_4 }, { 0 }, 0, { 0,0,0,{0} }, NULL
    };
    bool EnablePrivilege (LPCTSTR lpszPrivilege) {
        HANDLE hToken;
        if (OpenProcessToken (GetCurrentProcess (), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
            LUID luid;
            if (LookupPrivilegeValue (NULL, lpszPrivilege, &luid)) {
                TOKEN_PRIVILEGES tp = { 1, { luid, SE_PRIVILEGE_ENABLED } };
                if (AdjustTokenPrivileges (hToken, FALSE, &tp, sizeof (TOKEN_PRIVILEGES), NULL, NULL)) {
                    return GetLastError () != ERROR_NOT_ALL_ASSIGNED;
                }
            }
        }
        return false;
    }

    enum Command : WPARAM {
        NoCommand = 0,
        TerminateCommand = 1,
        ShowCommand = 2,
        HideCommand = 3,
        CheckCommand = 4,
        SettingCommand = 5,
    };

    struct VS_HEADER {
        WORD wLength;
        WORD wValueLength;
        WORD wType;
    };
    struct StringSet {
        const wchar_t * data;
        std::uint16_t   size;

        const wchar_t * operator [] (const wchar_t * name) const {
            if (this->data) {

                const VS_HEADER * header;
                auto p = this->data;
                auto e = this->data + this->size;

                while ((p < e) && ((header = reinterpret_cast <const VS_HEADER *> (p))->wLength != 0)) {

                    auto length = header->wLength / 2;
                    if (std::wcscmp (p + 3, name) == 0) {
                        return p + length - header->wValueLength;
                    }

                    p += length;
                    if (length % 2) {
                        ++p;
                    }
                }
            }
            return nullptr;
        }
    } strings;


    void Print (const wchar_t * format, ...) {
        va_list args;
        va_start (args, format);

        wchar_t buffer [1024];
        DWORD length = wvsprintf (buffer, format, args);

        DWORD written;
        WriteConsole (GetStdHandle (STD_OUTPUT_HANDLE), buffer, length, &written, NULL);

        va_end (args);
    }// */

    bool FirstInstance () {
        return CreateMutex (NULL, FALSE, name)
            && GetLastError () != ERROR_ALREADY_EXISTS;
    }

    bool AllowIsolatedMessage (HWND hWnd, UINT message, bool allow) {
        if (hWnd && pfnChangeWindowMessageFilterEx) // Win7+
            return pfnChangeWindowMessageFilterEx (hWnd, message, allow ? MSGFLT_ALLOW : MSGFLT_DISALLOW, NULL);

        if (allow || !hWnd)
            return ChangeWindowMessageFilter (message, allow ? MSGFLT_ADD : MSGFLT_REMOVE);

        return allow;
    }

    bool CommandLineEndsWith (const wchar_t * cmdline, const wchar_t * value) {
        auto llen = std::wcslen (cmdline);
        auto vlen = std::wcslen (value);

        if (vlen < llen) {
            if (std::wcscmp (cmdline + llen - vlen, value) == 0) {
                switch (cmdline [llen - vlen - 1]) {
                    case L' ':
                        return true;
                    case L'/':
                        return cmdline [llen - vlen - 2] == L' ';

                    case L'-':
                        std::size_t i = 1;
                        while (cmdline [llen - vlen - i] == L'-') {
                            ++i;
                        }
                        return cmdline [llen - vlen - i] == L' ';
                }
            }
        }
        return false;
    }
        
    Command ParseCommandLine () {
        auto cmdline = GetCommandLine ();

        if (CommandLineEndsWith (cmdline, L"terminate")) return TerminateCommand;
        if (CommandLineEndsWith (cmdline, L"check")) return CheckCommand;
        if (CommandLineEndsWith (cmdline, L"hide")) return HideCommand;
        if (CommandLineEndsWith (cmdline, L"show")) return ShowCommand;

        return NoCommand;
    }

    bool InitResources () {
        auto hInstance = reinterpret_cast <HINSTANCE> (&__ImageBase);

        if (HRSRC hRsrc = FindResource (hInstance, MAKEINTRESOURCE (1), RT_VERSION)) {
            if (HGLOBAL hGlobal = LoadResource (hInstance, hRsrc)) {
                auto data = LockResource (hGlobal);
                auto size = SizeofResource (hInstance, hRsrc);

                struct VS_VERSIONINFO : public VS_HEADER {
                    WCHAR szKey [sizeof "VS_VERSION_INFO"]; // 15 characters
                    WORD  Padding1 [1];
                    VS_FIXEDFILEINFO Value;
                };

                if (size >= sizeof (VS_VERSIONINFO)) {
                    const auto * vi = static_cast <const VS_HEADER *> (data);
                    const auto * vp = static_cast <const unsigned char *> (data)
                        + sizeof (VS_VERSIONINFO) + sizeof (VS_HEADER) - sizeof (VS_FIXEDFILEINFO)
                        + vi->wValueLength;

                    if (!std::wcscmp (reinterpret_cast <const wchar_t *> (vp), L"StringFileInfo")) {
                        vp += sizeof (L"StringFileInfo");

                        strings.size = reinterpret_cast <const VS_HEADER *> (vp)->wLength / 2 - std::size_t (12);
                        strings.data = reinterpret_cast <const wchar_t *> (vp) + 12;
                    }

                    if (vi->wValueLength) {
                        auto p = reinterpret_cast <const DWORD *> (LockResource (hGlobal));
                        auto e = p + (size - sizeof (VS_FIXEDFILEINFO)) / sizeof (DWORD);

                        for (; p != e; ++p)
                            if (*p == 0xFEEF04BDu)
                                break;

                        if (p != e)
                            version = reinterpret_cast <const VS_FIXEDFILEINFO *> (p);
                    }
                }
            }
        }
        return version && strings.data;
    }

    void SetSettingsValue (const wchar_t * name, DWORD value) {
        RegSetValueEx (settings, name, 0, REG_DWORD, reinterpret_cast <const BYTE *> (&value), sizeof value);
    }

    bool InitRegistry () {
        HKEY hKeySoftware = NULL;
        if (RegCreateKeyEx (HKEY_CURRENT_USER, L"SOFTWARE", 0, NULL, 0,
                            KEY_CREATE_SUB_KEY, NULL, &hKeySoftware, NULL) == ERROR_SUCCESS) {

            HKEY hKeyTRIMCORE = NULL;
            if (RegCreateKeyEx (hKeySoftware, strings [L"CompanyName"], 0, NULL, 0, // TRIM CORE SOFTWARE s.r.o.
                                KEY_ALL_ACCESS, NULL, &hKeyTRIMCORE, NULL) == ERROR_SUCCESS) {

                DWORD disp = 0;
                if (RegCreateKeyEx (hKeyTRIMCORE, strings [L"InternalName"], 0, NULL, 0,
                                    KEY_ALL_ACCESS, NULL, &settings, &disp) == ERROR_SUCCESS) {
                    if (disp == REG_CREATED_NEW_KEY) {
                        // defaults
                        SetSettingsValue (L"check", 180); // minutes
                    }

                    RegCreateKeyEx (settings, L"builds", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &builds, NULL);
                    RegCreateKeyEx (settings, L"alerts", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &alerts, NULL);
                }
                RegCloseKey (hKeyTRIMCORE);
            }
            RegCloseKey (hKeySoftware);
        }
        return alerts && settings;
    }

    DWORD GetSettingsValue (const wchar_t * name, DWORD default_) {
        DWORD size = sizeof (DWORD);
        DWORD value = 0;
        if (RegQueryValueEx (settings, name, NULL, NULL, reinterpret_cast <BYTE *> (&value), &size) == ERROR_SUCCESS)
            return value;
        else
            return default_;
    }

    void WINAPI InternetHandler (HINTERNET request, DWORD_PTR context, DWORD code, LPVOID data, DWORD size);

    void InitInternet () {
        wchar_t agent [64];
        _snwprintf (agent, 64, L"%s/%u.%u (https://changewindows.org)",
                    strings [L"InternalName"], HIWORD (version->dwProductVersionMS), LOWORD (version->dwProductVersionMS));
        
        Print (L"User-agent: %s\n", agent);

        internet = WinHttpOpen (agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);

        if (internet) {
            WinHttpSetStatusCallback (internet, InternetHandler, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS, NULL);

            connection = WinHttpConnect (internet, L"changewindows.org", 443, 0);
        }
    }

    void update ();
    void about ();
    bool check ();
    void trim ();
}

#ifndef NDEBUG
int WinMain (HINSTANCE, HINSTANCE, LPSTR, int) {
#else
void WinMainCRTStartup () {
#endif
    if (!AttachConsole (ATTACH_PARENT_PROCESS)) {
        AllocConsole ();
    }

    auto command = ParseCommandLine ();
    if (FirstInstance ()) {
        switch (command) {
            case HideCommand: // start hidden
                nid.dwState = NIS_HIDDEN;
                nid.dwStateMask = NIS_HIDDEN;
                break;
            case TerminateCommand:
                ExitProcess (ERROR_INVALID_PARAMETER);
            case ShowCommand:
                ExitProcess (ERROR_FILE_NOT_FOUND);
        }
    } else {
        SetLastError (0);
        DWORD recipients = BSM_APPLICATIONS;
        switch (command) {
            case TerminateCommand:
                if (EnablePrivilege (SE_TCB_NAME)) {
                    recipients |= BSM_ALLDESKTOPS;
                }
                break;
            case NoCommand:
                command = ShowCommand;
        }
        BroadcastSystemMessage (BSF_FORCEIFHUNG | BSF_IGNORECURRENTTASK, &recipients, RegisterWindowMessage (name), command, 0);
        ExitProcess (GetLastError ());
    }

    if (InitResources () && InitRegistry ()) {
        if (auto atom = RegisterClass (&wndclass)) {

            menu = GetSubMenu (LoadMenu (reinterpret_cast <HINSTANCE> (&__ImageBase), MAKEINTRESOURCE (1)), 0);
            if (menu) {
                SetMenuDefaultItem (menu, 0x10, FALSE);

                WM_Application = RegisterWindowMessage (name);
                WM_TaskbarCreated = RegisterWindowMessage (L"TaskbarCreated");

                window = CreateWindow ((LPCTSTR) (std::intptr_t) atom, L"", 0, 0, 0, 0, 0, HWND_DESKTOP,
                                       NULL, reinterpret_cast <HINSTANCE> (&__ImageBase), NULL);
                if (window) {

                    Windows::Symbol (L"USER32", pfnChangeWindowMessageFilterEx, "ChangeWindowMessageFilterEx");
                    AllowIsolatedMessage (window, WM_Application, true);
                    AllowIsolatedMessage (window, WM_TaskbarCreated, true);

                    InitInternet ();
                    check ();

                    MSG message;
                    while (GetMessage (&message, NULL, 0, 0) > 0) {
                        DispatchMessage (&message);
                    }

                    if (internet) {
                        WinHttpSetStatusCallback (internet, NULL, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, NULL);
                        WinHttpCloseHandle (connection);
                        WinHttpCloseHandle (internet);
                    }

                    if (message.message == WM_QUIT) {
                        ExitProcess ((int) message.wParam);
                    }
                }
            }
        }
    }
    ExitProcess (GetLastError ());
}

namespace {
    void update () {
        _snwprintf (nid.szTip, sizeof nid.szTip / sizeof nid.szTip [0], L"%s %u.%u\n...",
                    strings [L"InternalName"] /*or ProductName, but we have only 128 chars*/,
                    HIWORD (version->dwProductVersionMS), LOWORD (version->dwProductVersionMS));

        if (checking) {
            // add "Checking..."
        } else {
            // add latest versons
        }

        // TODO: add badge to icon (if enabled)

        if (nid.hIcon) {
            DestroyIcon (nid.hIcon);
        }
        nid.hIcon = (HICON) LoadImage (reinterpret_cast <HINSTANCE> (&__ImageBase), MAKEINTRESOURCE (1), IMAGE_ICON,
                                       GetSystemMetrics (SM_CXSMICON), GetSystemMetrics (SM_CYSMICON), LR_DEFAULTCOLOR);

        Shell_NotifyIcon (NIM_MODIFY, &nid);
    }

    void about () {
        wchar_t text [4096];
        auto n = _snwprintf (text, sizeof text / sizeof text [0], L"%s - %s\n%s\n\n",
                             strings [L"ProductName"], strings [L"ProductVersion"], strings [L"LegalCopyright"]);
        int i = 1;
        int m = 0;
        while (m = LoadString (reinterpret_cast <HINSTANCE> (&__ImageBase), i, &text [n], sizeof text / sizeof text [0] - n)) {
            n += m;
            i++;
        }

        MSGBOXPARAMS box = {};
        box.cbSize = sizeof box;
        box.hwndOwner = HWND_DESKTOP;
        box.hInstance = reinterpret_cast <HINSTANCE> (&__ImageBase);
        box.lpszText = text;
        box.lpszCaption = strings [L"ProductName"];
        box.dwStyle = MB_USERICON;
        box.lpszIcon = MAKEINTRESOURCE (1);
        box.dwLanguageId = LANG_USER_DEFAULT;
        MessageBoxIndirect (&box);
    }

    bool check ();
    void track (HWND hWnd, POINT pt) {
        UINT style = TPM_RIGHTBUTTON | TPM_RETURNCMD;
        BOOL align = FALSE;

        if (SystemParametersInfo (SPI_GETMENUDROPALIGNMENT, 0, &align, 0)) {
            if (align) {
                style |= TPM_RIGHTALIGN;
            }
        }

        SetForegroundWindow (hWnd);
        if (auto command = TrackPopupMenu (menu, style, pt.x, pt.y, 0, hWnd, NULL)) {
            PostMessage (hWnd, WM_COMMAND, command, 0);
            Shell_NotifyIcon (NIM_SETFOCUS, &nid);
        }
        PostMessage (hWnd, WM_NULL, 0, 0);
    }

    LRESULT CALLBACK wndproc (HWND hWnd, UINT message,
                              WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_CREATE:
                nid.hWnd = hWnd;
                Shell_NotifyIcon (NIM_ADD, &nid);
                Shell_NotifyIcon (NIM_SETVERSION, &nid);

                update ();
                //SetTimer (hWnd, 1, 5000, NULL);
                return 0;

            case WM_TIMER:
                switch (wParam) {
                    case 1:

                        // TODO: option to enable/disable balloons in registry
                        if (nid.uFlags & NIF_INFO) {
                            nid.uFlags &= ~NIF_INFO;
                            nid.dwInfoFlags = 0;
                            nid.szInfo [0] = L'\0';
                        } else {
                            nid.dwState = 0;
                            nid.dwStateMask = NIS_HIDDEN;
                            nid.uFlags |= NIF_INFO;
                            nid.dwInfoFlags = 0;
                            std::wcsncpy (nid.szInfo, L"1 2 3 4 5 6 7 8 9 0 20 x3 4 5 6 7 8 9 0 302 3 4 5 6 7 8 9 0 402 3 4 5 6 7 8 9 0 502 3 4 5 6 7 8 9 0 602 3 4 5 6 7 8 9 0 702 3 4 5 6 7 8 9 0 802 3 4 5 6 7 8 9 0 902 3 4 5 6 7 8 9 0 100 3 4 5 6 7 8 9 0 110 3 4 5 6 7 8 9 0 ", 256);
                        }

                        nid.hBalloonIcon = nid.hIcon;
                        nid.uTimeout = 60000;

                        if (IsWindows7OrGreater ()) {
                            nid.dwInfoFlags |= NIIF_RESPECT_QUIET_TIME;
                        }

                        std::wcsncpy (nid.szInfoTitle, L"Title 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0", 64);
                        Shell_NotifyIcon (NIM_MODIFY, &nid);

                        break;
                }
                break;

            case WM_DPICHANGED:
                update ();
                break;

            case WM_APP:
                switch (LOWORD (lParam)) {
                    case WM_LBUTTONDBLCLK:
                        wndproc (hWnd, WM_COMMAND, GetMenuDefaultItem (menu, FALSE, 0) & 0xFFFF, 0);
                        break;

                    case WM_CONTEXTMENU:
                        track (hWnd, { (short) LOWORD (wParam), (short) HIWORD (wParam) });
                }
                break;

            case WM_COMMAND:
                switch (wParam) {
                    case 0x10:
                        // run the program itself, to open config dialog
                        break;
                    case 0x11:
                        check ();
                        update ();
                        // TODO: reset timer
                        break;
                    case 0x1B:
                        about ();
                        break;
                    case 0x1A:
                        ShellExecute (hWnd, NULL, L"https://www.changewindows.org", NULL, NULL, SW_SHOWDEFAULT);
                        break;
                    case 0x1E:
                        nid.dwState = NIS_HIDDEN;
                        nid.dwStateMask = NIS_HIDDEN;
                        Shell_NotifyIcon (NIM_MODIFY, &nid);
                        break;
                    case 0x1F:
                        PostMessage (hWnd, WM_CLOSE, ERROR_SUCCESS, 0);
                }
                break;

            case WM_QUERYENDSESSION:
                SendMessage (hWnd, WM_CLOSE, ERROR_SHUTDOWN_IN_PROGRESS, 0);
                return TRUE;

            case WM_CLOSE:
                Shell_NotifyIcon (NIM_DELETE, &nid);
                DestroyWindow (hWnd);
                PostQuitMessage ((int) wParam);
                break;

            default:
                if (message == WM_Application) {
                    switch (wParam) {
                        case ShowCommand:
                            nid.dwState = 0;
                            nid.dwStateMask = NIS_HIDDEN;
                            Shell_NotifyIcon (NIM_MODIFY, &nid);
                            break;
                        case HideCommand:
                            nid.dwState = NIS_HIDDEN;
                            nid.dwStateMask = NIS_HIDDEN;
                            Shell_NotifyIcon (NIM_MODIFY, &nid);
                            break;
                        case CheckCommand:
                            check ();
                            update ();
                            // TODO: reset timer
                            break;
                        case TerminateCommand:
                            SendMessage (hWnd, WM_CLOSE, ERROR_SUCCESS, 0);
                    }
                    return 0;
                } else
                    if (message == WM_TaskbarCreated) {
                        Shell_NotifyIcon (NIM_ADD, &nid);
                        Shell_NotifyIcon (NIM_SETVERSION, &nid);
                        update ();
                        return 0;
                    } else
                        return DefWindowProc (hWnd, message, wParam, lParam);
        }
        return 0;
    }

    auto queued = 0u;
    struct {
        wchar_t platform [32];
        void (*callback)(JsonValue) = nullptr;
    } queue [64];


    bool submit (const wchar_t * path, void (*callback)(JsonValue)) {
        if (auto request = WinHttpOpenRequest (connection, NULL, path, NULL,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) {

            if (WinHttpSendRequest (request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, (DWORD_PTR) callback)) {
                Print (L"submitted %s...\n", path);
                return true;
            }
            WinHttpCloseHandle (request);
        }
        Print (L"submit error %u\n", GetLastError ());
        InterlockedDecrement (&checking);
        return false;
    }

    template <typename T, std::size_t N>
    constexpr std::size_t array_size (T (&) [N]) { return N; };

    void enqueue (const wchar_t * path, void (*callback)(JsonValue)) {
        if (queued < array_size (queue)) {
            queue [queued].callback = callback;
            std::wcsncpy (queue [queued].platform, path, array_size (queue [queued].platform));
            ++queued;
        }
    }

    void platform (JsonValue json) {

        Print (L"got platform\n");
    }

    struct tie {
        const tie *  link = nullptr;
        char *       key = nullptr;
        unsigned int index = ~0u;

        void display () const {
            if (this->link)
                this->link->display ();

            if (this->key) {
                Print (L"[%hs] ", this->key);
            } else
            if (this->index != ~0u) {
                Print (L"[#%u] ", this->index);
            }
        }
    };
    void display (JsonValue o, const tie & stack = tie {});

    template <typename T>
    void get () {

    }

    void timeline (JsonValue json) {
        Print (L"InternetHandler json parsed\n");
        display (json, {});
        // extract data, compare with currently stored, update data, generate changelist

        // list (json, callback, "props", "platforms", nullptr, "slug");
        // list (json, callback, "props", "channel_platforms", nullptr, "channels");



        // request next paths
        /*enqueue (L"/timeline/pc", platform);
        enqueue (L"/timeline/xbox", platform);
        enqueue (L"/timeline/server", platform);
        enqueue (L"/timeline/holographic", platform);
        enqueue (L"/timeline/team", platform);
        enqueue (L"/timeline/azure", platform);
        enqueue (L"/timeline/sdk", platform);
        enqueue (L"/timeline/iso", platform);
        // */
    }

    bool check () {
        if (InterlockedCompareExchange (&checking, 1u, 0u) == 0u) {
            if (submit (L"/timeline", timeline))
                return true;
        }
        return false;
    }

    bool empty (JsonValue o) {
        for (auto i : o) {
            return false;
        }
        return true;
    }


    bool display (const tie & stack, const wchar_t * format, ...) {
        va_list args;
        va_start (args, format);

        stack.display ();

        wchar_t buffer [1024];
        DWORD length = wvsprintf (buffer, format, args);

        DWORD written;
        WriteConsole (GetStdHandle (STD_OUTPUT_HANDLE), buffer, length, &written, NULL);

        va_end (args);
        return false;
    }

    void display (JsonValue o, const tie & stack) {
        switch (o.getTag ()) {
            case JSON_NUMBER:
                display (stack, L"= %f\n", o.toNumber ());
                break;
            case JSON_STRING:
                display (stack, L"= \"%hs\"\n", o.toString ());
                break;
            case JSON_ARRAY:
                if (!empty (o)) {
                    auto n = 0u;
                    for (auto i : o) {
                        display (i->value, { &stack, nullptr, n++ });
                    }
                }
                break;
            case JSON_OBJECT:
                if (!empty (o)) {
                    auto n = 0u;
                    for (auto i : o) {
                        display (i->value, { &stack, i->key, n++ });
                    }
                }
                break;
            case JSON_TRUE:
                display (stack, L"= true\n");
                break;
            case JSON_FALSE:
                display (stack, L"= false\n");
                break;
            case JSON_NULL:
                display (stack, L"= null\n");
                break;
        }
    }

    // Data: simply array
    //  - list view groups: PC, Xbox, Server, Holo, Team, Azure, SDK
    //  - list view items (3 lines):
    //      Preview     LTSC 2021
    //      25217.1000  20348.1668
    //      2022/08/11  2023/04/11
    //  - report:
    //     - UBR in channel increased (updates)
    //     - new build number in channel
    //     - new channel added
    
    // Registry name: "pc|Canary" or "pc|17763"
    /*struct RegistryValue {
        std::uint32_t date;
        std::uint16_t build;
        std::uint16_t release; // UBR
    };*/
    // When set alert: "sdk|23***" or "PC" (means all new)

    // Mapping slugs to texts

    std::size_t received = 0;

    void WINAPI InternetHandler (HINTERNET request, DWORD_PTR context, DWORD code, LPVOID data_, DWORD size) {
        auto data = static_cast <char *> (data_);
        // Print (L"InternetHandler @ %p %08X\n", &data, code);
        switch (code) {

            case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            case WINHTTP_CALLBACK_STATUS_CLOSING_CONNECTION:
                Print (L"InternetHandler @ %p error %u\n", &data, GetLastError ());
                break;

            case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
                if (WinHttpReceiveResponse (request, NULL))
                    return;

                Print (L"InternetHandler @ %p request error %u\n", &data, GetLastError ());
                break;

            case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
                size = sizeof buffer;
                if (WinHttpQueryHeaders (request, WINHTTP_QUERY_STATUS_CODE, NULL, buffer, &size, WINHTTP_NO_HEADER_INDEX)) {

                    auto status = std::wcstoul (reinterpret_cast <const wchar_t *> (buffer), nullptr, 10);
                    if (status == 200) {
                        if (WinHttpQueryDataAvailable (request, NULL))
                            return;

                        //Print (L"InternetHandler @ WINHTTP_QUERY_STATUS_CODE: %u\n", status);
                        Print (L"InternetHandler @ %p WinHttpQueryDataAvailable error %u\n", &data, GetLastError ());
                    } else
                        Print (L"InternetHandler @ %p WINHTTP_QUERY_STATUS_CODE status %u\n", &data, status);
                } else
                    Print (L"InternetHandler @ %p WinHttpQueryHeaders error %u\n", &data, GetLastError ());

                break;

            case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
                if (size) {
                    received += size;
                    buffer [received] = '\0';

                    [[ fallthrough ]];

            case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
                    if (WinHttpReadData (request, buffer + received, (DWORD) (sizeof buffer - received - 1), NULL))
                        return;
                }
        }

        WinHttpCloseHandle (request);
        Print (L"request %p end with %u bytes\n", request, received);

        if (received) {

            // find json content
            if (auto json = std::strstr (buffer, "<div id=\"app\" data-page=\"")) {
                json += 25;

                auto end = std::strchr (json, '"');
                if (end) {
                    *end = '\0';
                

                    // transcode html entities
                    auto p = json;
                    auto o = json;

                    while (p != end) {
                        switch (*p) {
                            case '&':
                                     if (std::strncmp (p + 1, "amp;", 4) == 0) { p += 4; *p = '&'; }
                                else if (std::strncmp (p + 1, "quot;", 5) == 0) { p += 5; *p = '"'; }
                                else if (std::strncmp (p + 1, "raquo;", 6) == 0) { p += 6; *p = '>'; } // good enough
                                else if (std::strncmp (p + 1, "laquo;", 6) == 0) { p += 6; *p = '<'; }

                                break;
                            case '\\':
                                ++p;
                                break;
                        }
                        *o++ = *p++;
                    }
                    *o = '\0';

                    /*wchar_t filename [64];
                    _snwprintf (filename, 64, L"json %p err.txt", request);
                    auto h = CreateFile (filename, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
                    if (h != INVALID_HANDLE_VALUE) {
                        DWORD red;
                        WriteFile (h, json, o - json, &red, NULL);
                        CloseHandle (h);
                    }// */

                    // parse

                    JsonValue tree;
                    if (jsonParse (json, &end, &tree) == JSON_OK) {
                        ((void (*)(JsonValue)) context) (tree);
                    } else {
                        Print (L"request %p JSON error\n", request);
                    }

                    // drop bump allocator

                    allocated = 0;
                } else {
                    Print (L"request %p no end!\n", request);
                }
            }

            received = 0;
            Sleep (500);
        }

        if (queued) {
            queued--;
            submit (queue [queued].platform, queue [queued].callback);

        } else {
            // done
            InterlockedDecrement (&checking);
            update ();
            trim ();
        }
    }

    void trim () {
        if (IsWindows8Point1OrGreater ()) {
            HeapSetInformation (NULL, HeapOptimizeResources, NULL, 0);
        }
        SetProcessWorkingSetSize (GetCurrentProcess (), (SIZE_T) -1, (SIZE_T) -1);
    }
}

JsonNode * jsonAllocate (std::size_t n) {
    JsonNode * node = nullptr;
    if (allocated <= sizeof allocator - n) {
        node = (JsonNode *) &allocator [allocated];
        allocated += n;
    }
    return node;
}
