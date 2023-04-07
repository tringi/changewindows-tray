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

#pragma warning (disable:6262) // stack usage warning
#pragma warning (disable:26812) // unscoped enum warning

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {
    const wchar_t name [] = L"TRIMCORE.ChangeWindows";
    const VS_FIXEDFILEINFO * version = nullptr;

    HMENU menu = NULL;
    HWND window = NULL;
    UINT WM_Application = WM_NULL;
    UINT WM_TaskbarCreated = WM_NULL;
    HKEY data = NULL;       // last versions
    HKEY settings = NULL;   // app settings
    HINTERNET internet = NULL;

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
        SettingCommand = 4,
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

    BOOL (WINAPI * pfnChangeWindowMessageFilterEx) (HWND, UINT, DWORD, LPVOID) = NULL;

    bool AllowIsolatedMessage (HWND hWnd, UINT message, bool allow) {
        if (hWnd && pfnChangeWindowMessageFilterEx) // Win7+
            return pfnChangeWindowMessageFilterEx (hWnd, message, allow ? MSGFLT_ALLOW : MSGFLT_DISALLOW, NULL);

        if (allow || !hWnd)
            return ChangeWindowMessageFilter (message, allow ? MSGFLT_ADD : MSGFLT_REMOVE);

        return allow;
    }

    bool ends_with (const wchar_t * cmdline, const wchar_t * value) {
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

        if (ends_with (cmdline, L"terminate")) return TerminateCommand;
        if (ends_with (cmdline, L"hide")) return HideCommand;
        if (ends_with (cmdline, L"show")) return ShowCommand;

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
                        // RegSetSettingsValue (L"color", 1); // white
                    }

                    RegCreateKeyEx (settings, L"data", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &data, NULL);
                }
                RegCloseKey (hKeyTRIMCORE);
            }
            RegCloseKey (hKeySoftware);
        }
        return data && settings;
    }

    void WINAPI InternetHandler (HINTERNET request, DWORD_PTR context, DWORD code, LPVOID data, DWORD size);
    void InitInternet () {
        wchar_t agent [64];
        _snwprintf (agent, 64, L"%s/%u.%u (https://changewindows.org)",
                    strings [L"InternalName"], HIWORD (version->dwProductVersionMS), LOWORD (version->dwProductVersionMS));
        
        internet = WinHttpOpen (agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);

        if (internet) {
            WinHttpSetStatusCallback (internet, InternetHandler, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS, NULL);
        }
    }
}

int WinMainCRTStartup () {
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

                window = CreateWindow ((LPCTSTR) (std::intptr_t) atom, L"", 0,0,0,0,0, HWND_DESKTOP,
                                       NULL, reinterpret_cast <HINSTANCE> (&__ImageBase), NULL);
                if (window) {

                    Windows::Symbol (L"USER32", pfnChangeWindowMessageFilterEx, "ChangeWindowMessageFilterEx");
                    AllowIsolatedMessage (window, WM_Application, true);
                    AllowIsolatedMessage (window, WM_TaskbarCreated, true);

                    InitInternet ();

                    MSG message;
                    while (GetMessage (&message, NULL, 0, 0) > 0) {
                        DispatchMessage (&message);
                    }

                    if (internet) {
                        WinHttpSetStatusCallback (internet, NULL, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, NULL);
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
        _snwprintf (nid.szTip, sizeof nid.szTip / sizeof nid.szTip [0], L"%s - %s\n%s",
                    strings [L"ProductName"], strings [L"ProductVersion"], strings [L"CompanyName"]);
        
        // TODO: latest versons in tooltip, instead of company name

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

                        if (nid.uFlags & NIF_INFO) {
                            Print (L"Remove\n");
                            nid.uFlags &= ~NIF_INFO;
                            nid.dwInfoFlags = 0;
                            nid.szInfo [0] = L'\0';
                        } else {
                            Print (L"Set\n");
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
                        // check now: start download, reset timer
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

    bool check () {
        if (auto connection = WinHttpConnect (internet, L"changewindows.org", 443, 0)) {
            if (auto request = WinHttpOpenRequest (connection, NULL, L"/timeline", NULL,
                                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) {

                if (WinHttpSendRequest (request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, (DWORD_PTR) connection)) {
                    return true;

                } else {
                    WinHttpCloseHandle (request);
                    WinHttpCloseHandle (connection);
                }
            } else {
                WinHttpCloseHandle (connection);
            }
            return false;
        }
    }

    char buffer [8193];

    void WINAPI InternetHandler (HINTERNET request, DWORD_PTR connection, DWORD code, LPVOID data_, DWORD size) {
        auto data = static_cast <char *> (data_);
        Print (L"InternetHandler @ %p %08X: %p %p %u\n", &data, code, buffer, data, size);
        switch (code) {

            case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            case WINHTTP_CALLBACK_STATUS_CLOSING_CONNECTION:
                // this->report (raddi::log::level::error, 0x26);
                break;

                // successes return early here
            case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
                if (WinHttpReceiveResponse (request, NULL))
                    return;

                // this->report (raddi::log::level::error, 0x25, "start");
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

                // NOTE: I really need to stop writing code like this

            case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
                Print (L"InternetHandler @ %p WINHTTP_CALLBACK_STATUS_READ_COMPLETE: %u\n", &data, size);
                if (size) {
                    data [size] = '\0';

                    // data...

                    [[ fallthrough ]];

            case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
                Print (L"InternetHandler @ %p WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE...\n", &data);
                if (WinHttpReadData (request, buffer, sizeof buffer - 1, NULL)) {
                    Print (L"InternetHandler @ %p WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE: true\n", &data);
                    return;
                } else {
                    Print (L"InternetHandler @ %p WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE: error %u\n", &data, GetLastError ());
                }
                }
        }

//cancelled:
        WinHttpCloseHandle (request);
        WinHttpCloseHandle ((HINTERNET) connection);
        Print (L"InternetHandler closed\n");
    }
}
