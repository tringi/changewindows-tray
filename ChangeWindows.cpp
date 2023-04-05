#include <Windows.h>
#include <VersionHelpers.h>
#include <shellapi.h>
#include <commdlg.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>

#include "Windows_Symbol.hpp"
#include "Windows_AllowIsolatedMessage.hpp"

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
    HHOOK hook1 = NULL;
    HHOOK hook2 = NULL;

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

    bool FirstInstance () {
        return CreateMutex (NULL, FALSE, name)
            && GetLastError () != ERROR_ALREADY_EXISTS;
    }

    Command ParseCommand (const wchar_t * argument) {
        while (*argument == L'/' || *argument == L'-') {
            ++argument;
        }
        if (std::wcscmp (argument, L"terminate") == 0) return TerminateCommand;
        if (std::wcscmp (argument, L"hide") == 0) return HideCommand;
        if (std::wcscmp (argument, L"show") == 0) return ShowCommand;

        // if (std::wcscmp (argument, L"query") == 0) return QueryCommand;
        // if (std::wcscmp (argument, L"delete") == 0) return DeleteCommand;

        return NoCommand;
    }

    LPWSTR * argw = NULL;

    Command ParseCommandLine (LPWSTR lpCmdLine) {
        int argc;
        argw = CommandLineToArgvW (lpCmdLine, &argc);

        for (auto i = 0; i < argc; ++i) {
            auto cmd = ParseCommand (argw [i]);
            if (cmd != NoCommand)
                return cmd;
        }
        return NoCommand;
    }
}

int APIENTRY wWinMain (HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {

    auto command = ParseCommandLine (lpCmdLine);
    if (FirstInstance ()) {
        switch (command) {
            case HideCommand: // start hidden
                nid.dwState = NIS_HIDDEN;
                nid.dwStateMask = NIS_HIDDEN;
                break;
            case TerminateCommand:
                return ERROR_INVALID_PARAMETER;
            case ShowCommand:
                return ERROR_FILE_NOT_FOUND;
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
        return GetLastError ();
    }
   
    LocalFree (argw);

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

                    p = std::find (p, e, 0xFEEF04BDu);
                    if (p != e)
                        version = reinterpret_cast <const VS_FIXEDFILEINFO *> (p);
                }
            }
        }
    }

    if (!version || !strings.data) {
        return GetLastError ();
    }

    if (auto atom = RegisterClass (&wndclass)) {
        menu = GetSubMenu (LoadMenu (hInstance, MAKEINTRESOURCE (1)), 0);
        if (menu) {
            SetMenuDefaultItem (menu, 0x10, FALSE);

            WM_Application = RegisterWindowMessage (name);
            WM_TaskbarCreated = RegisterWindowMessage (L"TaskbarCreated");

            window = CreateWindow ((LPCTSTR) (std::intptr_t) atom, L"", 0, 0, 0, 0, 0, HWND_DESKTOP, NULL, hInstance, NULL);
            if (window) {
                Windows::AllowIsolatedMessage (window, WM_Application, true);
                Windows::AllowIsolatedMessage (window, WM_TaskbarCreated, true);

                MSG message;
                while (GetMessage (&message, NULL, 0, 0) > 0) {
                    DispatchMessage (&message);
                }
                if (message.message == WM_QUIT) {
                    return (int) message.wParam;
                }
            }
        }
    }
    return GetLastError ();
}

namespace {
    DWORD tray = ERROR_IO_PENDING;

    void update () {
        if (tray != ERROR_SUCCESS) {
            _snwprintf (nid.szTip, sizeof nid.szTip / sizeof nid.szTip [0], L"%s - %s\nERROR %u",
                        strings [L"ProductName"], strings [L"ProductVersion"], tray);
        } else {
            _snwprintf (nid.szTip, sizeof nid.szTip / sizeof nid.szTip [0], L"%s - %s\n%s",
                        strings [L"ProductName"], strings [L"ProductVersion"], strings [L"CompanyName"]);
        }

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

    void track (HWND hWnd, POINT pt) {
        UINT style = TPM_RIGHTBUTTON;
        BOOL align = FALSE;

        if (SystemParametersInfo (SPI_GETMENUDROPALIGNMENT, 0, &align, 0)) {
            if (align) {
                style |= TPM_RIGHTALIGN;
            }
        }

        SetForegroundWindow (hWnd);
        TrackPopupMenu (menu, style, pt.x, pt.y, 0, hWnd, NULL);
        Shell_NotifyIcon (NIM_SETFOCUS, &nid);
        PostMessage (hWnd, WM_NULL, 0, 0);
    }

    LRESULT CALLBACK wndproc (HWND hWnd, UINT message,
                              WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_CREATE:
                nid.hWnd = hWnd;
                nid.hIcon = (HICON) LoadImage (reinterpret_cast <HINSTANCE> (&__ImageBase), MAKEINTRESOURCE (1), IMAGE_ICON,
                                               GetSystemMetrics (SM_CXSMICON), GetSystemMetrics (SM_CYSMICON), LR_DEFAULTCOLOR);

                Shell_NotifyIcon (NIM_ADD, &nid);
                Shell_NotifyIcon (NIM_SETVERSION, &nid);
                    
                update ();
                return 0;
            
            case WM_DPICHANGED:
                nid.hIcon = (HICON) LoadImage (reinterpret_cast <HINSTANCE> (&__ImageBase), MAKEINTRESOURCE (1), IMAGE_ICON,
                                               GetSystemMetrics (SM_CXSMICON), GetSystemMetrics (SM_CYSMICON), LR_DEFAULTCOLOR);
                Shell_NotifyIcon (NIM_MODIFY, &nid);
                break;

            case WM_APP:
                switch (LOWORD (lParam)) {
                    case WM_LBUTTONDBLCLK:
                        wndproc (hWnd, WM_COMMAND, GetMenuDefaultItem (menu, FALSE, 0) & 0xFFFF, 0);
                        break;

                    case WM_RBUTTONUP:
                        if (!IsWindowsVistaOrGreater ()) { // XP
                            POINT pt = { 0, 0 };
                            GetCursorPos (&pt);
                            track (hWnd, pt);
                        }
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
}
