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
#include "Windows_MatchFilename.hpp"
#include "gason.h"

#pragma warning (disable:6262) // stack usage warning
#pragma warning (disable:6053) // _snwprintf may not NUL-terminate
#pragma warning (disable:6250) // calling VirtualFree without release
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
    HKEY metrics = NULL;    // debug metrics
    HINTERNET internet = NULL;
    HINTERNET connection = NULL;

    static constexpr auto MAX_PLATFORMS = 16u;

    bool installed = false;
    bool signalled = false;
    bool checking = false;
    bool retry = false;
    DWORD failure = 0;
    char * http_buffer = nullptr;
    char * json_allocator = nullptr;
    std::size_t http_commit = 0;
    std::size_t json_commit = 0;
    std::size_t json_used_bytes = 0;

    LRESULT CALLBACK TrayProcedure (HWND, UINT, WPARAM, LPARAM);
    WNDCLASS wndclass = {
        0, TrayProcedure, 0, 0,
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
        if (CommandLineEndsWith (cmdline, L"settings")) return SettingCommand;
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

                DWORD disposition = 0;
                if (RegCreateKeyEx (hKeyTRIMCORE, strings [L"InternalName"], 0, NULL, 0,
                                    KEY_ALL_ACCESS, NULL, &settings, &disposition) == ERROR_SUCCESS) {
                    if (disposition == REG_CREATED_NEW_KEY) {
                        // defaults
                        SetSettingsValue (L"check", 180); // minutes
                        SetSettingsValue (L"toast", 1); // balloon notifications
                        SetSettingsValue (L"legacy", 0); // report on legacy platforms

                        // do not report first check
                        installed = true;
                    }

                    RegCreateKeyEx (settings, L"builds", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &builds, NULL);
                    RegCreateKeyEx (settings, L"alerts", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &alerts, NULL);
                    RegCreateKeyEx (settings, L"metrics", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &metrics, NULL);
                }
                RegCloseKey (hKeyTRIMCORE);
            }
            RegCloseKey (hKeySoftware);
        }
        return builds && alerts && settings;
    }

    DWORD GetSettingsValue (const wchar_t * name, DWORD default_ = 0) {
        DWORD size = sizeof (DWORD);
        DWORD value = 0;
        if (RegQueryValueEx (settings, name, NULL, NULL, reinterpret_cast <BYTE *> (&value), &size) == ERROR_SUCCESS)
            return value;
        else
            return default_;
    }

    void SetMetricsValue (const wchar_t * name, DWORD value) {
        RegSetValueEx (metrics, name, 0, REG_DWORD, reinterpret_cast <const BYTE *> (&value), sizeof value);
    }
    void UpdateMetricsMaximum (const wchar_t * name, DWORD value) {
        DWORD size = sizeof (DWORD);
        DWORD previous = 0;
        if (RegQueryValueEx (metrics, name, NULL, NULL, reinterpret_cast <BYTE *> (&previous), &size) != ERROR_SUCCESS) {
            previous = 0;
        }
        if (value > previous) {
            RegSetValueEx (metrics, name, 0, REG_DWORD, reinterpret_cast <const BYTE *> (&value), sizeof value);
        }
    }

    template <typename T, std::size_t N>
    constexpr std::size_t array_size (T (&) [N]) { return N; };

    void WINAPI InternetHandler (HINTERNET request, DWORD_PTR context, DWORD code, LPVOID data, DWORD size);

    void InitInternet () {
        wchar_t url [64];
        LoadString (reinterpret_cast <HINSTANCE> (&__ImageBase), 0x10, url, (int) array_size (url));

        wchar_t agent [128];
        _snwprintf (agent, array_size (agent), L"%s/%u.%u (https://%s)",
                    strings [L"InternalName"], HIWORD (version->dwProductVersionMS), LOWORD (version->dwProductVersionMS), url);
        
        internet = WinHttpOpen (agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
        if (internet) {
            WinHttpSetStatusCallback (internet, InternetHandler, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS, NULL);

            connection = WinHttpConnect (internet, url, 443, 0);
        }
    }

    void UpdateTrayIcon ();
    void AboutDialog ();
    bool CheckChanges ();
    void TrimMemoryUsage ();
}

void Main () {
    if (IsDebuggerPresent ()) {
        if (!AttachConsole (ATTACH_PARENT_PROCESS)) {
            AllocConsole ();
        }
    }

    auto command = ParseCommandLine ();
    if (command == SettingCommand) {
        // settings dialog
        ExitProcess (ERROR_SUCCESS);
    }
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

    http_buffer = (char *) VirtualAlloc (NULL, 16777216, MEM_RESERVE, PAGE_READWRITE);
    if (!http_buffer)
        ExitProcess (GetLastError ());

    json_allocator = (char *) VirtualAlloc (NULL, 16777216, MEM_RESERVE, PAGE_READWRITE);
    if (!json_allocator)
        ExitProcess (GetLastError ());

    if (InitResources () && InitRegistry ()) {
        LoadLibrary (L"NTDLL");
        LoadLibrary (L"WININET");

        if (auto atom = RegisterClass (&wndclass)) {

            menu = GetSubMenu (LoadMenu (reinterpret_cast <HINSTANCE> (&__ImageBase), MAKEINTRESOURCE (1)), 0);
            if (menu) {
                SetMenuDefaultItem (menu, 0x1A, FALSE);

                WM_Application = RegisterWindowMessage (name);
                WM_TaskbarCreated = RegisterWindowMessage (L"TaskbarCreated");

                window = CreateWindow ((LPCTSTR) (std::intptr_t) atom, L"", 0, 0, 0, 0, 0, HWND_DESKTOP,
                                       NULL, reinterpret_cast <HINSTANCE> (&__ImageBase), NULL);
                if (window) {
                    ChangeWindowMessageFilterEx (window, WM_Application, MSGFLT_ALLOW, NULL);
                    ChangeWindowMessageFilterEx (window, WM_TaskbarCreated,  MSGFLT_ALLOW, NULL);

                    InitInternet ();
                    CheckChanges ();

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
    DWORD GetErrorMessage (DWORD code, wchar_t * buffer, std::size_t length) {
        if (auto n = FormatMessage (FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK |
                                    FORMAT_MESSAGE_FROM_SYSTEM, NULL, code, 0, buffer, (DWORD) length, NULL)) {
            return n;
        }

        static const wchar_t * const modules [] = {
            L"WININET",
            L"NTDLL" // ???
        };
        for (auto i = 0u; i < sizeof modules / sizeof modules [0]; ++i) {
            if (auto module = GetModuleHandle (modules [i])) {
                if (auto n = FormatMessage (FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK |
                                            FORMAT_MESSAGE_FROM_HMODULE, module, code, 0, buffer, (DWORD) length, NULL)) {
                    return n;
                }
            }
        }
        return 0;
    }

    void UpdateTrayIcon () {

        nid.szTip [0] = L'\0';
        if (nid.hIcon) {
            DestroyIcon (nid.hIcon);
            nid.hIcon = NULL;
        }

        wchar_t status [128];
        if (checking) {
            LoadString (reinterpret_cast <HINSTANCE> (&__ImageBase), 0x21, status, (int) array_size (status));

            // TODO: animate icon

        } else
        if (signalled) {

            // add latest versons
            // TODO: use badge on icon (allow disable)
            status [0] = 0;

        } else
        if (failure) {
            LoadString (reinterpret_cast <HINSTANCE> (&__ImageBase), 0x22, status, (int) array_size (status));
            auto offset = _snwprintf (nid.szTip, array_size (nid.szTip), L"%s\n%s %u: ",
                                      strings [L"InternalName"], status, failure);
            if (offset > 0) {
                GetErrorMessage (failure, nid.szTip + offset, array_size (nid.szTip) - offset);
            }

        } else {
            std::wcsncpy (nid.szTip, strings [L"ProductName"], array_size (nid.szTip));
            // TODO: No new builds or releases
        }

        if (!nid.szTip [0]) {
            _snwprintf (nid.szTip, array_size (nid.szTip), L"%s\n%s",
                        strings [L"InternalName"] /*or ProductName, but we have only 128 chars*/,
                        // HIWORD (version->dwProductVersionMS), LOWORD (version->dwProductVersionMS),
                        status);
        }

        if (!nid.hIcon) {
            const auto cx = GetSystemMetrics (SM_CXSMICON);
            const auto cy = GetSystemMetrics (SM_CYSMICON);

            nid.hIcon = (HICON) LoadImage (reinterpret_cast <HINSTANCE> (&__ImageBase), MAKEINTRESOURCE (1), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);

            // colorization test

            /*ICONINFO info;
            if (GetIconInfo (nid.hIcon, &info)) {

                HDC hScreenDC = GetDC (NULL);
                HDC hDC = CreateCompatibleDC (hScreenDC);
                HBITMAP hOldBitmap = (HBITMAP) SelectObject (hDC, info.hbmColor);

                auto data = (COLORREF *) HeapAlloc (GetProcessHeap (), 0, sizeof (COLORREF) * cx * cy);
                if (data) {
                    BITMAPINFO bi = { { sizeof (BITMAPINFOHEADER), cx, cy, 1, 32, BI_RGB, 0, 0, 0, 0, 0 }, {{ 0, 0, 0, 0 }} };

                    if (GetDIBits (hDC, info.hbmColor, 0, cy, data, &bi, DIB_RGB_COLORS)) {

                        for (auto i = 0u; i < cx * cy; ++i) {
                            auto r = GetRValue (data [i]);
                            auto b = GetBValue (data [i]);

                            data [i] = RGB (b, GetGValue (data [i]), r);
                        }

                        if (auto hNewBitmap = CreateDIBitmap (hDC, &bi.bmiHeader, CBM_INIT, data, &bi, DIB_RGB_COLORS)) {

                            DeleteObject (info.hbmColor);
                            info.hbmColor = hNewBitmap;
                            
                            if (auto hNewIcon = CreateIconIndirect (&info)) {
                                DestroyIcon (nid.hIcon);
                                nid.hIcon = hNewIcon;
                            }
                        }
                    }
                    HeapFree (GetProcessHeap (), 0, data);
                }


                //CreateIconIndirect (

                SelectObject (hDC, hOldBitmap);
                DeleteObject (info.hbmColor);
                DeleteObject (info.hbmMask);
                DeleteDC (hDC);
                ReleaseDC (NULL, hScreenDC);
            }
            // */
        }
        nid.uFlags &= ~NIF_INFO;
        Shell_NotifyIcon (NIM_MODIFY, &nid);
    }

    void AboutDialog () {
        // TODO: TaskDialogIndirect, add link and check for updates?

        // wchar_t caption [256];
        // _snwprintf (caption, array_size (caption), L"%s - %s",
        //            strings [L"ProductName"], strings [L"ProductVersion"]);

        wchar_t text [4096];
        auto n = _snwprintf (text, array_size (text), L"%s %s\n%s\n\n",
                             strings [L"FileDescription"], strings [L"ProductVersion"], strings [L"LegalCopyright"]);
        int i = 1;
        int m = 0;
        while (m = LoadString (reinterpret_cast <HINSTANCE> (&__ImageBase), i, &text [n], (int) (array_size (text) - n))) {
            n += m;
            i++;
        }

        MSGBOXPARAMS box = {};
        box.cbSize = sizeof box;
        box.hwndOwner = HWND_DESKTOP;
        box.hInstance = reinterpret_cast <HINSTANCE> (&__ImageBase);
        box.lpszText = text;
        box.lpszCaption = strings [L"ProductName"]; //caption;
        box.dwStyle = MB_USERICON;
        box.lpszIcon = MAKEINTRESOURCE (1);
        box.dwLanguageId = LANG_USER_DEFAULT;
        MessageBoxIndirect (&box);
    }

    void ScheduleCheck () {
        auto minutes = GetSettingsValue (L"check");
        if (minutes == 0) {
            minutes = 180;
        }
        SetTimer (window, 1, minutes * 60 * 1000, NULL);
    }

    void TrackTrayMenu (HWND hWnd, POINT pt) {
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

    void OpenWebsite (HWND hWnd) {
        wchar_t url [64];
        std::wcscpy (url, L"https://");

        LoadString (reinterpret_cast <HINSTANCE> (&__ImageBase), 0x10, url + 8, (int) array_size (url) - 8);
        ShellExecute (hWnd, NULL, url, NULL, NULL, SW_SHOWDEFAULT);
    }

    LRESULT CALLBACK TrayProcedure (HWND hWnd, UINT message,
                              WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_CREATE:
                nid.hWnd = hWnd;
                nid.uVersion = NOTIFYICON_VERSION_4;
                Shell_NotifyIcon (NIM_ADD, &nid);
                Shell_NotifyIcon (NIM_SETVERSION, &nid);

                UpdateTrayIcon ();
                return 0;

            case WM_TIMER:
                switch (wParam) {
                    case 1:
                        CheckChanges ();
                        break;
                    case 2:
                        UpdateTrayIcon ();
                        break;
                }
                break;

            case WM_POWERBROADCAST:
                switch (wParam) {
                    case PBT_APMRESUMEAUTOMATIC:
                        retry = true;
                        CheckChanges ();
                }
                break;

            case WM_DPICHANGED:
                UpdateTrayIcon ();
                break;

            case WM_APP:
                switch (LOWORD (lParam)) {
                    case NIN_BALLOONUSERCLICK:
                        signalled = false;
                        OpenWebsite (hWnd);
                        break;

                    case WM_LBUTTONDBLCLK:
                        signalled = false;
                        TrayProcedure (hWnd, WM_COMMAND, GetMenuDefaultItem (menu, FALSE, 0) & 0xFFFF, 0);
                        break;

                    case WM_CONTEXTMENU:
                        TrackTrayMenu (hWnd, { (short) LOWORD (wParam), (short) HIWORD (wParam) });
                }
                break;

            case WM_COMMAND:
                switch (wParam) {
                    case 0x10:
                        // run the program itself, to open config dialog
                        break;
                    case 0x11:
                        CheckChanges ();
                        break;
                    case 0x1B:
                        AboutDialog ();
                        break;
                    case 0x1A:
                        OpenWebsite (hWnd);
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
                            CheckChanges ();
                            break;
                        case TerminateCommand:
                            SendMessage (hWnd, WM_CLOSE, ERROR_SUCCESS, 0);
                    }
                    return 0;
                } else
                    if (message == WM_TaskbarCreated) {
                        nid.uVersion = NOTIFYICON_VERSION_4;
                        Shell_NotifyIcon (NIM_ADD, &nid);
                        Shell_NotifyIcon (NIM_SETVERSION, &nid);
                        UpdateTrayIcon ();
                        return 0;
                    } else
                        return DefWindowProc (hWnd, message, wParam, lParam);
        }
        return 0;
    }

    std::uint8_t qursor = 0;
    std::uint8_t queued = 0;
    struct {
        void (* callback) (JsonValue) = nullptr;
        char    platform [30] = {};
        bool    tool = false;
        bool    legacy = false;
    } current, queue [MAX_PLATFORMS];

    bool SubmitRequest (const char * suffix, void (*callback)(JsonValue)) {
        wchar_t path [48];
        if (suffix) {
            _snwprintf (path, array_size (path), L"/timeline/%hs", suffix);
        } else {
            _snwprintf (path, array_size (path), L"/timeline");
            failure = 0;
        }

        if (auto request = WinHttpOpenRequest (connection, NULL, path, NULL,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) {

            if (WinHttpSendRequest (request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, (DWORD_PTR) callback)) {
                return true;
            }
            WinHttpCloseHandle (request);
        }
        checking = false;
        KillTimer (window, 2);

        failure = GetLastError ();
        SetMetricsValue (L"http error", failure); // store error to report if triggered manually

        if (retry) {
            retry = false;
            return CheckChanges ();
        } else
            return false;
    }

    bool EnqueueRequest (const char * path, void (*callback)(JsonValue), bool tool, bool legacy) {
        if (queued < array_size (queue)) {
            queue [queued].callback = callback;
            queue [queued].tool = tool;
            queue [queued].legacy = legacy;
            std::strncpy (queue [queued].platform, path, sizeof queue [queued].platform);
            ++queued;
            return true;
        } else
            return false;
    }

    template <typename Callback>
    void process (JsonValue json, Callback callback) {
        callback (json);
    }

    template <typename Callback, typename... Args>
    void process (JsonValue json, Callback callback, std::nullptr_t, Args &&... remaining) {
        switch (json.getTag ()) {
            case JSON_ARRAY:
            case JSON_OBJECT:
                for (auto i : json) {
                    process (i->value, callback, remaining...);
                }
        }
    }

    template <typename Callback, typename... Args>
    void process (JsonValue json, Callback callback, const char * text, Args &&... remaining) {
        switch (json.getTag ()) {
            case JSON_OBJECT:
                for (auto i : json) {
                    if (std::strcmp (i->key, text) == 0) {
                        process (i->value, callback, remaining...);
                    }
                }
        }
    }

    enum class Update : int {
        None = 0,
        Release,
        Build,
        New
    };

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

    struct BuildData {
        std::uint32_t date;
        std::uint32_t build;
        std::uint32_t release; // UBR
    };
    struct BuildInfo : BuildData {
        const char * platform = nullptr;  // PC, XBox, Server, ...
        const char * channel = nullptr;   // Canary, Dev, Unstable, ...
        const char * name = nullptr;      // 21H2
    };

    // TODO:
    //  - caption: 
    //     - try generate: "Platform: Channel, Channel, Channel; Platform: Channel, Channel"
    //        - e.g.: "PC: Dev, Canary, LTSB; ISO: Stable; 
    //     - if too long, generate: "Platform: # channels"...
    //     - 
    //  - rows:
    //     - if more than 4, append "& # more..." to the last one
    //     - postupne ohodnocovat reportovane entries podle priority
    //     - merge same build numbers if possible
    //        - 25348.1001: PC (Dev)
    //        - 25348.1001: PC (Dev, Beta)
    //        - 25348.1001: PC (Dev, Beta, ...)
    //        - 20349.1787: PC 21H2 (LTSC), Azure 22H2 (Production)
    //     - alternatively
    //        - PC 25348.1001 (Dev)
    //        - PC 25348.1001 (Dev & Beta) - if version numbers are equal
    //        - PC 25348.1001 (Dev), 22123.1001 (Beta) - if not
    //        - PC 25348.1001 (Dev, Beta, ...) - if more than would fit
    //        - 25348.1001: PC (Dev), ISO (Stable)
    //  - order multiple changes by:
    //     - user set priority
    //     - build number highest to lowest

    struct Report {
        /*struct {
            char name [32];
            struct {
                char name [32];


            } channels [16u];
        } platform [MAX_PLATFORMS];*/

        std::size_t tempi = 0;
        wchar_t temp [8192];

        void append (const wchar_t * format, ...) {
            va_list args;
            va_start (args, format);
            this->tempi += wvsprintf (this->temp + this->tempi, format, args);
            va_end (args);
        }// */

        void insert (Update level, const BuildInfo & info) {
            if (tempi) {
                this->append (L"\n");
            }
            this->append (L"\x272E %hs (%hs) \t%hs %u.%u", info.platform, info.channel, info.name, info.build, info.release);
        }
        bool toast () {
            // auto changes = 1; // TODO: count

            // TODO: update tray icon and nid.szTip?

            if (tempi) {
                signalled = true;

                nid.dwState = 0;
                nid.dwStateMask = NIS_HIDDEN;

                if (GetSettingsValue (L"toast")) {

                    nid.uFlags |= NIF_INFO;
                    nid.dwInfoFlags = 0;

                    if (IsWindows7OrGreater ()) {
                        nid.dwInfoFlags |= NIIF_RESPECT_QUIET_TIME;
                    }

                    // TODO: title text? strings [L"InternalName"]
                    std::wcsncpy (nid.szInfoTitle, L"ChangeWindows: PC, XBox, SDK, ISO, ...", array_size (nid.szInfoTitle));
                    std::wcsncpy (nid.szInfo, temp, array_size (nid.szInfo));

                    nid.uTimeout = 60'000;

                    Shell_NotifyIcon (NIM_MODIFY, &nid);

                    nid.uFlags &= ~NIF_INFO;
                } else {
                    // set signalling icon, play sound and set nid.szTip?
                }
            }

            /*
                // 8.1 celý tip
                // 1507 cca 164 chars
                // 1607 cca 176 chars v balonu a 215 v seznamu
                // 2021 cca 205 chars v balonu a 215 v seznamu
                // Win 11 cca 219 chars v balonu a 200 v seznamu

                // // místo "New" symbol \xE113 (star) or \xE115 (sprocket) or \E154 (windows)
                std::wcsncpy (nid.szInfo, L"\x272E \xE113 \xEA8A \xE115 \xE154 \x2665 \x2661 \x2026 \x2116" // 17 chars
                              " 9 0 20 x3 4 5 6 7 8 9 0 302 3 4 5 6 7 8 9 0 402 3 4 5 6 7 8 9 0 502 3 4 5 6 7 8 9 0 602 3 4 5 6 7 8 9a 0 702 3 4 5 6 7 8b 9 0" // 126 chars
                              " 802 3 4 5 6 7c 8 9 0 902 3 4 5 6d 7 8 9 0 100 3 4 5e 6 7 8 9 0 110 3 4f 5 6. 7 8 9 0 1 2 3g 4 5 6 7 8 9 0 1 2h..",
                              256);
                std::wcsncpy (nid.szInfoTitle, L"\x272E \xE113 \xEA8A \xE115 \xE154 \x2665 \x2661 \x2026 \x2116 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 X.", 64);
            */

            // and reset

            std::memset (this, 0, sizeof *this);
            return true;
        }
    } report;

    std::uint32_t timestamp_to_date (const char * timestamp) {
        char date [9] = {};
        date [0] = timestamp [0];
        date [1] = timestamp [1];
        date [2] = timestamp [2];
        date [3] = timestamp [3];
        date [4] = timestamp [5];
        date [5] = timestamp [6];
        date [6] = timestamp [8];
        date [7] = timestamp [9];
        date [8] = '\0';

        return (std::uint32_t) std::strtoul (date, nullptr, 10);
    }

    std::uint32_t release_from_flight (const char * flight) {
        if (auto release = std::strchr (flight, '.')) {
            ++release;
            return (std::uint32_t) std::strtoul (release, nullptr, 10);
        } else
            return 0;
    }

    bool UpdateBuild (const wchar_t * key, const BuildInfo & info, bool report) {
        Update updated = Update::None;

        // read stored value

        BuildData data = {};
        DWORD size = sizeof data;
        if (RegQueryValueEx (builds, key, NULL, NULL, reinterpret_cast <BYTE *> (&data), &size) == ERROR_SUCCESS) {

            // compare
            if (data.build < info.build) {
                data = (BuildData) info;
                updated = Update::Build;
            } else
            if (data.build == info.build) {
                if (data.release < info.release) {
                    data = (BuildData) info;
                    updated = Update::Release;
                }
            }
        } else {
            data = (BuildData) info;
            updated = Update::New;
        }

        // update in registry

        if (updated != Update::None) {
            RegSetValueEx (builds, key, NULL, REG_BINARY, reinterpret_cast <BYTE *> (&data), sizeof data);
            // Print (L"UPDATE(%d) %s %u.%u\n", (int) updated, key, info.build, info.release);

            if (!installed && report) {

                // compare with alerts

                wchar_t alert [256];
                auto cchAlert = (DWORD) array_size (alert);

                DWORD i = 0;
                while (RegEnumValue (alerts, i++, alert, &cchAlert, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                    cchAlert = (DWORD) array_size (alert);

                    if (Windows::MatchFilename (key, alert)) {
                        ::report.insert (updated, info);
                        return true;
                    }
                }
            }
        }

        return false;
    }

    // UpdateBuilds
    //  - platform = PC, XBox, Server, ...
    //  - channel = Canary, Dev, Unstable, ...
    //  - date = YYYY-MM-DD HH:MM:SS
    //  - name = 21H2
    //  - flight = 23456.1001
    //
    bool UpdateBuilds (BuildInfo & info) {
        wchar_t key [256];
        auto reported = 0u;

        _snwprintf (key, array_size (key), L"%hs;%hs#%u", info.platform, info.channel, info.build); // "PC;Canary#23456" - for tracking UBRs for build
        reported += UpdateBuild (key, info, !reported);

        _snwprintf (key, array_size (key), L"%hs#%u", info.platform, info.build); // "PC#23456" - for tracking UBRs for build
        reported += UpdateBuild (key, info, !reported);

        _snwprintf (key, array_size (key), L"%hs;%hs", info.platform, info.name); // "PC;21H2"
        reported += UpdateBuild (key, info, !reported);

        _snwprintf (key, array_size (key), L"%hs;%hs", info.platform, info.channel); // "PC;Canary" or "SDK;Unstable"
        reported += UpdateBuild (key, info, !reported);

        return reported;
    }

    template <typename... Args>
    const char * get (JsonValue json, Args &&... args) {
        const char * value = nullptr;
        process (json,
                 [&value] (JsonValue x) {
                     if (x.getTag () == JSON_STRING) {
                         value = x.toString ();
                     }
                 }, args...);
        return value;
    }

    template <typename... Args>
    double getNumber (JsonValue json, Args &&... args) {
        double value = 0;
        process (json,
                 [&value] (JsonValue x) {
                     if (x.getTag () == JSON_NUMBER) {
                         value = x.toNumber ();
                     }
                 }, args...);
        return value;
    }

    void platform (JsonValue json) {
        process (json,
                 [] (JsonValue release) {
                    auto flight = get (release, "flight"); // "23456.1001"

                    BuildInfo info;

                    info.platform = get (release, "platform", "name"); // "PC";
                    info.channel = nullptr;
                    info.name = get (release, "release", "version"); // "21H2";

                    info.date = timestamp_to_date (get (release, "date"));
                    info.build = (std::uint32_t) std::strtoul (flight, nullptr, 10);
                    info.release = release_from_flight (flight);

                    process (release,
                             [&info] (JsonValue channel) {
                                 if (channel.getTag () == JSON_STRING) {
                                     info.channel = channel.toString ();
                                     UpdateBuilds (info);
                                 }
                             }, "release_channel", nullptr, "name");
                 }, "props", "timeline", nullptr, "flights", nullptr, nullptr);
    }

    void timeline (JsonValue json) {
        process (json,
                 [] (JsonValue platform) {

                    auto slug = get (platform, "slug"); // "pc" or "iso"
                    auto tool = getNumber (platform, "tool"); // 0 or 1
                    auto legacy = getNumber (platform, "legacy"); // "0 or 1

                    if (!legacy || GetSettingsValue (L"legacy")) {
                        // allocate slug
                        EnqueueRequest (slug, ::platform, tool, legacy);
                    }
                 }, "props", "platforms", nullptr);
    }

    bool CheckChanges () {
        bool result = false;
        if (!checking) {
            checking = true;
            result = SubmitRequest (nullptr, timeline);
            SetTimer (window, 2, 100, NULL);
        }
        UpdateTrayIcon ();
        return result;
    }

    std::size_t received = 0;
    wchar_t statusbuffer [6];

    void WINAPI InternetHandler (HINTERNET request, DWORD_PTR context, DWORD code, LPVOID data_, DWORD size) {
        auto data = static_cast <char *> (data_);
        switch (code) {

            case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            case WINHTTP_CALLBACK_STATUS_CLOSING_CONNECTION:
                failure = GetLastError ();
                SetMetricsValue (L"http error", failure);
                break;

            case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
                if (WinHttpReceiveResponse (request, NULL))
                    return;

                failure = GetLastError ();
                SetMetricsValue (L"http error", failure);
                break;

            case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
                size = sizeof statusbuffer;
                if (WinHttpQueryHeaders (request, WINHTTP_QUERY_STATUS_CODE, NULL, statusbuffer, &size, WINHTTP_NO_HEADER_INDEX)) {

                    auto status = std::wcstoul (statusbuffer, nullptr, 10);
                    if (status == 200) {
                        if (WinHttpQueryDataAvailable (request, NULL))
                            return;

                        failure = GetLastError ();
                        SetMetricsValue (L"http error", failure);
                    } else {
                        failure = ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
                        SetMetricsValue (L"http error", status);
                    }
                } else {
                    failure = GetLastError ();
                    SetMetricsValue (L"http error", failure);
                }

                break;

            case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
                if (size) {
                    received += size;
                    http_buffer [received] = '\0';

                    [[ fallthrough ]];

            case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
                    if (received + 4096 >= http_commit) {
                        if (VirtualAlloc (http_buffer, http_commit + 4096, MEM_COMMIT, PAGE_READWRITE)) {
                            http_commit += 4096;
                        } else {
                            failure = GetLastError ();
                            break;
                        }
                    }

                    if (WinHttpReadData (request, http_buffer + received, (DWORD) (http_commit - received - 1), NULL))
                        return;
                }
        }

        WinHttpCloseHandle (request);
        UpdateMetricsMaximum (L"http downloaded maximum", (DWORD) received);

        if (received) {

            // find json content
            if (auto json = std::strstr (http_buffer, "<div id=\"app\" data-page=\"")) {
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

                    if (GetSettingsValue (L"dump json")) {
                        wchar_t filename [24 + sizeof current.platform];
                        _snwprintf (filename, array_size (filename), L"ChangeWindows-%.*hs.json", (unsigned int) sizeof current.platform, current.platform);

                        auto h = CreateFile (filename, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                        if (h != INVALID_HANDLE_VALUE) {
                            DWORD n;
                            WriteFile (h, json, o - json, &n, NULL);
                            CloseHandle (h);
                        }
                    }

                    // parse

                    JsonValue tree;
                    auto jsonStatus = jsonParse (json, &end, &tree);
                    if (jsonStatus == JSON_OK) {
                        ((void (*)(JsonValue)) context) (tree);
                    } else {
                        failure = ERROR_INVALID_DATA;
                        SetMetricsValue (L"json error", jsonStatus);
                    }

                    // free bump allocator

                    UpdateMetricsMaximum (L"json allocated maximum", (DWORD) json_used_bytes);
                    json_used_bytes = 0;
                } else {
                    failure = ERROR_RECEIVE_PARTIAL;
                }
            } else {
                failure = ERROR_NOT_FOUND;
            }

            received = 0;
            Sleep (100);
        }

        if (queued && (qursor < queued)) {
            
            current = queue [qursor];
            SubmitRequest (queue [qursor].platform, queue [qursor].callback);
            ++qursor;

        } else {
            qursor = 0;
            queued = 0;
            installed = false;

            checking = false;
            KillTimer (window, 2);

            ScheduleCheck ();
            UpdateTrayIcon ();
            TrimMemoryUsage ();
            report.toast ();

            if (retry) {
                retry = false;
                CheckChanges ();
            }
        }
    }

    void TrimMemoryUsage () {
        if (VirtualFree (http_buffer, http_commit, MEM_DECOMMIT)) {
            http_commit = 0;
        }
        if (VirtualFree (json_allocator, json_commit, MEM_DECOMMIT)) {
            json_commit = 0;
        }
        if (IsWindows8Point1OrGreater ()) {
            HeapSetInformation (NULL, HeapOptimizeResources, NULL, 0);
        }
        SetProcessWorkingSetSize (GetCurrentProcess (), (SIZE_T) -1, (SIZE_T) -1);
    }
}

JsonNode * jsonAllocate (std::size_t n) {
    if (json_used_bytes + n + 4096 >= json_commit) {
        if (VirtualAlloc (json_allocator, json_commit + 4096, MEM_COMMIT, PAGE_READWRITE)) {
            json_commit += 4096;
        } else {
            failure = GetLastError ();
            return nullptr;
        }
    }

    json_used_bytes += (DWORD) n;
    return (JsonNode *) &json_allocator [json_used_bytes - (DWORD) n];
}
