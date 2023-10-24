#include <winsock2.h>
#include <Windows.h>
#include <ws2ipdef.h>
#include <winhttp.h>
#include <shellapi.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>

#include <VersionHelpers.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <cmath>

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

    HANDLE heap = NULL;
    HMENU menu = NULL;
    HWND window = NULL;
    HWND setupdlg = NULL;
    UINT WM_Application = WM_NULL;
    UINT WM_TaskbarCreated = WM_NULL;
    HKEY settings = NULL;   // app settings
    HKEY builds = NULL;     // last versions
    HKEY alerts = NULL;     // reported changes
    HKEY metrics = NULL;    // debug metrics
    HINTERNET internet = NULL;
    HINTERNET connection = NULL;

    const INITCOMMONCONTROLSEX iccEx = {
        sizeof (INITCOMMONCONTROLSEX),
        ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_LINK_CLASS
    };

    static constexpr auto MAX_PLATFORMS = 12u;
    static constexpr auto MAX_PLATFORM_LENGTH = 22u;
    static constexpr auto MAX_CHANNEL_LENGTH = 22u;
    static constexpr auto MAX_RELEASE_LENGTH = 22u;

    enum Mode {
        Idle = 0,
        Failure,
        Checking,
        Signalling,
    } mode;

    bool installed = false;
    DWORD failure = 0;
    char * http_buffer = nullptr;
    char * json_allocator = nullptr;
    std::size_t http_commit = 0;
    std::size_t json_commit = 0;
    std::size_t json_used_bytes = 0;

    INT_PTR CALLBACK SetupProcedure (HWND, UINT, WPARAM, LPARAM);
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

    /*void Print (const wchar_t * format, ...) {
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
                        SetSettingsValue (L"animate", 1); // animated tray icon

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
        if (RegQueryValueEx (settings, name, NULL, NULL, reinterpret_cast <BYTE *> (&value), &size) == ERROR_SUCCESS) {
            if (value)
                return value;
        }
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
        LoadString (reinterpret_cast <HINSTANCE> (&__ImageBase), 0x0A, url, (int) array_size (url));

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
    if (FirstInstance ()) {
        switch (command) {
            case HideCommand: // start hidden
                nid.dwState = NIS_HIDDEN;
                nid.dwStateMask = NIS_HIDDEN;
                break;
            case ShowCommand:
                ExitProcess (ERROR_FILE_NOT_FOUND);
            case SettingCommand:
            case TerminateCommand:
                ExitProcess (ERROR_INVALID_PARAMETER);
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

    heap = GetProcessHeap ();

    http_buffer = (char *) VirtualAlloc (NULL, 16777216, MEM_RESERVE, PAGE_READWRITE);
    if (!http_buffer)
        ExitProcess (GetLastError ());

    json_allocator = (char *) VirtualAlloc (NULL, 16777216, MEM_RESERVE, PAGE_READWRITE);
    if (!json_allocator)
        ExitProcess (GetLastError ());

    if (InitCommonControlsEx (&iccEx) && InitResources () && InitRegistry ()) {
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

    class Animator {
        int         cx = 0;
        int         cy = 0;
        ULONGLONG   start = 0;
        HDC         hScreenDC = NULL;
        HDC         hDC = NULL;
        HBITMAP     hOldBitmap = NULL;
        ICONINFO    iconinfo = {};
        BITMAPINFO  bitmapinfo = {};
        struct RGBA {
            std::uint8_t b;
            std::uint8_t g;
            std::uint8_t r;
            std::uint8_t a;
        };
        RGBA * srcIconData = NULL;
        RGBA * newIconData = NULL;

        bool Start (int cx, int cy) {
            if (GetIconInfo (nid.hIcon, &this->iconinfo)) {

                this->hScreenDC = GetDC (NULL);
                this->hDC = CreateCompatibleDC (this->hScreenDC);
                this->hOldBitmap = (HBITMAP) SelectObject (this->hDC, this->iconinfo.hbmColor);

                this->srcIconData = (RGBA *) HeapAlloc (heap, 0, sizeof (RGBA) * cx * cy);
                this->newIconData = (RGBA *) HeapAlloc (heap, 0, sizeof (RGBA) * cx * cy);

                if (this->srcIconData && this->newIconData) {
                    this->bitmapinfo = { { sizeof (BITMAPINFOHEADER), cx, cy, 1, 32, BI_RGB, 0, 0, 0, 0, 0 }, {{ 0, 0, 0, 0 }} };

                    if (GetDIBits (this->hDC, this->iconinfo.hbmColor, 0, cy, this->srcIconData, &this->bitmapinfo, DIB_RGB_COLORS)) {
                        this->cx = cx;
                        this->cy = cy;
                        return true;
                    }
                }
            }
            this->Cleanup ();
            return false;
        }

        void Cleanup () {
            if (this->newIconData) HeapFree (heap, 0, this->newIconData);
            if (this->srcIconData) HeapFree (heap, 0, this->srcIconData);

            if (this->hOldBitmap) SelectObject (this->hDC, this->hOldBitmap);
            DeleteObject (this->iconinfo.hbmColor);
            DeleteObject (this->iconinfo.hbmMask);

            if (this->hDC) DeleteDC (this->hDC);
            if (this->hScreenDC) ReleaseDC (NULL, this->hScreenDC);

            std::memset (this, 0, sizeof *this);
        }

        void AddLight (RGBA lclr, float lx, float ly, float ld, float a) { // position, diameter, alpha
            auto i = 0u;
            for (auto y = 0; y != this->cy; ++y) {
                for (auto x = 0; x != this->cx; ++x) {
                    auto d = sqrtf ((x - lx) * (x - lx) + (y - ly) * (y - ly));
                    d -= 1.0f;
                    d /= ld;
                    if (d < 1.0f) {
                        d = 1.0f - d;
                        if (d > 1.0f) {
                            d = 1.0f;
                        }
                        d *= a;

                        if (d > 0.0f) {
                            auto c = this->newIconData [i];
                            c.b = (std::uint8_t) (c.b + ((int) lclr.b - (int) c.b) * d);
                            c.g = (std::uint8_t) (c.g + ((int) lclr.g - (int) c.g) * d);
                            c.r = (std::uint8_t) (c.r + ((int) lclr.r - (int) c.r) * d);
                            this->newIconData [i] = c;
                        }
                    }
                    ++i;
                }
            }
        }

    public:
        void set (Mode m) {
            switch (m) {
                case Idle:
                    if (mode != Idle) {
                        mode = Idle;
                        KillTimer (window, 2);
                        this->Cleanup ();
                        UpdateTrayIcon ();
                    }
                    break;
                default:
                    if (mode == Idle) {
                        if (this->Start (GetSystemMetrics (SM_CXSMICON), GetSystemMetrics (SM_CYSMICON))) {
                            SetTimer (window, 2, 50, NULL);
                        }
                    }
                    start = GetTickCount64 ();
                    mode = m;
            }
        }

        HICON frame () {
            if (mode != Idle) {
                std::memcpy (this->newIconData, this->srcIconData, sizeof (COLORREF) * this->cx * this->cy);

                switch (mode) {
                    case Checking: {
                        auto t = (GetTickCount64 () - this->start) / 200.0f;
                        this->AddLight ({ 0xFF, 0xFF, 0xFF },
                                        this->cx / 2.0f + this->cx / 2.5f * sinf (t),
                                        this->cy / 2.0f + this->cy / 2.5f * cosf (t),
                                        this->cx / 1.3f, 1.0f);
                    } break;

                    case Signalling: {
                        auto t = (GetTickCount64 () - this->start) / 400.0f;
                        auto n = 4u; // N unique platforms, max 4
                        float offsets [] = { 0.3125f, 0.6875f };

                        for (auto i = 0u; i != n; ++i) {
                            // TODO: use colors and cycle them
                            this->AddLight ({ 0x3F, 0xFF, 0x3F },
                                            this->cx * offsets [((i + 1) / 2 + 1) % 2], this->cy * offsets [(i + 1) % 2],
                                            this->cx / 1.8f, std::sinf (t + i * 3.14159f / 2.0f));

                        }
                    } break;

                    case Failure:
                        this->AddLight ({ 0x00, 0x00, 0xFF },
                                        this->cx * 0.666f, this->cy * 0.666f, this->cx / 1.3f,
                                        std::abs (std::sinf ((GetTickCount64 () - this->start) / 1500.0f)));
                        break;
                }

                if (auto hNewBitmap = CreateDIBitmap (hDC, &this->bitmapinfo.bmiHeader, CBM_INIT, this->newIconData, &this->bitmapinfo, DIB_RGB_COLORS)) {

                    DeleteObject (this->iconinfo.hbmColor);
                    this->iconinfo.hbmColor = hNewBitmap;

                    if (auto hNewIcon = CreateIconIndirect (&this->iconinfo)) {
                        return hNewIcon;
                    }
                    DeleteObject (hNewBitmap);
                }
            }

            // normal case and fallback

            return (HICON) LoadImage (reinterpret_cast <HINSTANCE> (&__ImageBase), MAKEINTRESOURCE (1), IMAGE_ICON,
                                      GetSystemMetrics (SM_CXSMICON), GetSystemMetrics (SM_CYSMICON), LR_DEFAULTCOLOR);
        }

    } animation;

    void UpdateTrayIcon () {
        nid.szTip [0] = L'\0';

        wchar_t status [128];
        LoadString (reinterpret_cast <HINSTANCE> (&__ImageBase), 0x20 + mode, status, (int) array_size (status));

        switch (mode) {

            case Idle:
            case Checking:
            case Signalling:
                _snwprintf (nid.szTip, array_size (nid.szTip), L"%s\n%s", strings [L"InternalName"], status);
                break;

            case Failure:
                auto offset = _snwprintf (nid.szTip, array_size (nid.szTip), L"%s\n%s %u: ",
                                          strings [L"InternalName"], status, failure);
                if (offset > 0) {
                    GetErrorMessage (failure, nid.szTip + offset, array_size (nid.szTip) - offset);
                }
                break;
        }

        if (nid.hIcon) {
            DestroyIcon (nid.hIcon);
            nid.hIcon = NULL;
        }
        if (!nid.hIcon) {
            nid.hIcon = animation.frame ();
        }
        nid.uFlags &= ~NIF_INFO;
        Shell_NotifyIcon (NIM_MODIFY, &nid);
    }

    void OpenWebsite (HWND hWnd, UINT id) {
        wchar_t url [256];
        std::wcscpy (url, L"https://");

        LoadString (reinterpret_cast <HINSTANCE> (&__ImageBase), id, url + 8, (int) array_size (url) - 8);
        ShellExecute (hWnd, NULL, url, NULL, NULL, SW_SHOWDEFAULT);
    }

    void CALLBACK AboutHelp (LPHELPINFO) {
        OpenWebsite (NULL, 0x0B);
    }

    void AboutDialog () {
        wchar_t text [1024];
        auto n = _snwprintf (text, array_size (text) - 2, L"%s %s\n%s\n\n",
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
        box.lpszCaption = strings [L"ProductName"];
        box.dwStyle = MB_USERICON | MB_HELP;
        box.lpszIcon = MAKEINTRESOURCE (1);
        box.dwLanguageId = LANG_USER_DEFAULT;
        box.dwContextHelpId = 1;
        box.lpfnMsgBoxCallback = AboutHelp;
        MessageBoxIndirect (&box);

        TrimMemoryUsage ();
    }

    void ScheduleCheck () {
        SetTimer (window, 1, GetSettingsValue (L"check", 180) * 60 * 1000, NULL);
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
                    case 3:
                        CheckChanges ();
                        KillTimer (hWnd, wParam);
                        break;

                }
                break;

            case WM_POWERBROADCAST:
                switch (wParam) {
                    case PBT_APMRESUMEAUTOMATIC:
                        SetTimer (hWnd, 3, GetSettingsValue (L"retry", 10000), NULL);
                }
                break;

            case WM_DPICHANGED:
                UpdateTrayIcon ();
                break;

            case WM_APP:
                switch (LOWORD (lParam)) {
                    case NIN_BALLOONUSERCLICK:
                        animation.set (Idle);
                        OpenWebsite (hWnd, 0x0A);
                        break;

                    case WM_LBUTTONDBLCLK:
                        animation.set (Idle);
                        TrayProcedure (hWnd, WM_COMMAND, GetMenuDefaultItem (menu, FALSE, 0) & 0xFFFF, 0);
                        break;

                    case WM_CONTEXTMENU:
                        TrackTrayMenu (hWnd, { (short) LOWORD (wParam), (short) HIWORD (wParam) });
                }
                break;

            case WM_COMMAND:
                switch (wParam) {
                    case 0x1D:
                        if (setupdlg) {
                            BringWindowToTop (setupdlg);
                        } else {
                            DialogBox (reinterpret_cast <HINSTANCE> (&__ImageBase), MAKEINTRESOURCE (1), hWnd, SetupProcedure);
                            setupdlg = NULL;
                            TrimMemoryUsage ();
                        }
                        break;
                    case 0x1C:
                        CheckChanges ();
                        break;
                    case 0x1B:
                        AboutDialog ();
                        break;
                    case 0x1A:
                        OpenWebsite (hWnd, 0x0A);
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
                        case SettingCommand:
                            TrayProcedure (hWnd, WM_COMMAND, 0x1D, 0);
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


    HFONT CreateTitleFont (HWND hWnd, COLORREF * cr) {
        LOGFONT font;
        HANDLE hTheme = OpenThemeData (hWnd, L"TEXTSTYLE");

        if (cr) {
            if (hTheme == NULL
                || GetThemeColor (hTheme, 1, 1, TMT_TEXTCOLOR, cr) != S_OK) {

                *cr = GetSysColor (COLOR_WINDOWTEXT); // fallback
            }
        }

        if (hTheme == NULL
            || GetThemeFont (hTheme, NULL, TEXT_MAININSTRUCTION, 0, TMT_FONT, &font) != S_OK) {

            // fallback
            if (GetObject (GetStockObject (DEFAULT_GUI_FONT), sizeof font, &font)) {

                std::wcsncpy (font.lfFaceName, L"Trebuchet MS", LF_FACESIZE);
                if (font.lfHeight > 0)
                    font.lfHeight += 5;
                else
                    font.lfHeight -= 5;
            }
        }

        if (hTheme) {
            CloseThemeData (hTheme);
        }
        return CreateFontIndirect (&font);
    }

    bool GetDlgItemCoordinates (HWND hWnd, UINT control, RECT * result) {
        if (auto hCtrl = GetDlgItem (hWnd, control)) {
            union {
                RECT r;
                POINT pt [2];
            };
            if (GetWindowRect (hCtrl, &r)) {
                if (ScreenToClient (hWnd, &pt [0]) && ScreenToClient (hWnd, &pt [1])) {
                    *result = r;
                    return true;
                }
            }
        }
        return false;
    }

    bool AppendRsrcString (wchar_t * buffer, std::size_t maximum, UINT id) {
        const wchar_t * string = nullptr;
        if (auto length = LoadString (reinterpret_cast <HINSTANCE> (&__ImageBase), id, (LPWSTR) &string, 0)) {

            auto bufferlen = std::wcslen (buffer);
            if (bufferlen < maximum) {

                _snwprintf (buffer + bufferlen, maximum - bufferlen, L"%.*s", (int) length, string);
            }
        }
        return false;
    }

    std::size_t strings_match (const char * a, const char * b, std::size_t an, std::size_t bn) {
        std::size_t n = 0;
        while (an && bn) {
            if (a [n] != b [n])
                return false;
            if (a [n] == 0)
                return n + 1;

            --an;
            --bn;
            ++n;
        }
        if (an && a [n])
            return false;
        if (bn && b [n])
            return false;

        return n + 1;
    }

    template <std::size_t N, std::size_t M>
    bool strings_match (const char (&a) [N], const char (&b) [M]) { return strings_match (a, b, N, M); }

    template <std::size_t N>
    bool strings_match (const char (&a) [N], const char * b) { return strings_match (a, b, N, ~0); }

    enum class Update : int {
        None = 0,
        Release,
        Build,
        New
    };
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

    // Remembered
    //  - platforms/channels temporary memory for Settings dialog
    //
    class Remembered {
        char platforms [MAX_PLATFORMS][MAX_PLATFORM_LENGTH] = { {} };
        struct {
            std::uint16_t platforms = 0; // MAX_PLATFORMS bits
            char          name [MAX_CHANNEL_LENGTH] = {};
        } channels [64]; // channels and releases mixed together

        void insert_channel (std::size_t p, const char * title) {
            for (auto & channel : this->channels) {
                if (channel.name [0] == '\0') { // empty slot
                    std::strncpy (channel.name, title, sizeof channel.name); // insert
                    channel.platforms |= (1 << p);
                    break;
                } else
                if (strings_match (channel.name, title)) { // exists
                    channel.platforms |= (1 << p); // add bit
                    break;
                }
            }
        }

    public:
        void reset () {
            std::memset (this->platforms, 0, sizeof this->platforms);
            std::memset (this->channels, 0, sizeof this->channels);
        }
        void insert (const BuildInfo & info) {
            bool found = false;
            auto p = 0u;
            for (; p != MAX_PLATFORMS; ++p) {
                if (this->platforms [p][0] == '\0') { // empty slot
                    std::strncpy (this->platforms [p], info.platform, sizeof this->platforms [p]); // insert
                    found = true;
                    break;
                } else
                if (strings_match (this->platforms [p], info.platform)) { // exists
                    found = true;
                    break;
                }
            }
            if (found) {
                this->insert_channel (p, info.channel); // Canary
                this->insert_channel (p, info.name); // 21H2
            }
        }
        void fill_platforms (HWND hComboBox) const {
            SendMessage (hComboBox, CB_RESETCONTENT, 0, 0);
            SendMessage (hComboBox, CB_ADDSTRING, 0, (LPARAM) L"*");
            for (auto & p : this->platforms) {
                if (p [0] == '\0')
                    break;

                wchar_t text [MAX_PLATFORM_LENGTH + 1] = {};
                MultiByteToWideChar (CP_UTF8, 0, p, sizeof p, text, (int) array_size (text));
                SendMessage (hComboBox, CB_ADDSTRING, 0, (LPARAM) text);
            }
        }
        void fill_channels (HWND hComboBox, HWND hPlatform) const {
            int wlength;
            wchar_t wplatform [MAX_PLATFORM_LENGTH + 1];

            auto cursel = (int) SendMessage (hPlatform, CB_GETCURSEL, 0, 0);
            if (cursel != -1) {
                wlength = (int) SendMessage (hPlatform, CB_GETLBTEXT, cursel, (LPARAM) wplatform);
            } else {
                wlength = (int) GetWindowText (hPlatform, wplatform, (int) array_size (wplatform));
            }

            char platform [MAX_PLATFORM_LENGTH + 1] = {};
            if (WideCharToMultiByte (CP_UTF8, 0, wplatform, wlength, platform, (int) sizeof platform, NULL, NULL)) {

                for (auto p = 0u; p != MAX_PLATFORMS; ++p) {
                    if (strings_match (this->platforms [p], platform)) {

                        SendMessage (hComboBox, CB_RESETCONTENT, 0, 0);
                        SendMessage (hComboBox, CB_ADDSTRING, 0, (LPARAM) L"*");

                        for (auto & channel : this->channels) {
                            if (channel.platforms & (1 << p)) {
                                if (channel.name [0] == '\0')
                                    break;

                                wchar_t text [MAX_CHANNEL_LENGTH] = {};
                                MultiByteToWideChar (CP_UTF8, 0, channel.name, -1, text, (int) array_size (text));
                                SendMessage (hComboBox, CB_ADDSTRING, 0, (LPARAM) text);
                            }
                        }
                        break;
                    }
                }
            }
        }

    } remembered;

    void UpdateButtons (HWND hwnd) {
        auto hPlatform = GetDlgItem (hwnd, 201);
        auto hChannels = GetDlgItem (hwnd, 202);
        auto hBuilds = GetDlgItem (hwnd, 203);

        if (IsWindowEnabled (GetDlgItem (hwnd, 299))) {
            SetDlgItemText (hwnd, 210, L"\xE109");
            SetDlgItemText (hwnd, 211, L"\xE107");
        } else {
            SetDlgItemText (hwnd, 210, L"\xE10B");
            SetDlgItemText (hwnd, 211, L"\xE10E");
        }

        EnableWindow (GetDlgItem (hwnd, 210),
                      GetWindowTextLength (hPlatform) || GetWindowTextLength (hChannels) || GetWindowTextLength (hBuilds));
    }

    HFONT hTitleFont = NULL;
    COLORREF crTitleColor = 0;

    INT_PTR CALLBACK SetupProcedure (HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_INITDIALOG:
                setupdlg = hwnd;
                hTitleFont = CreateTitleFont (hwnd, &crTitleColor);

                SendDlgItemMessage (hwnd, 200, WM_SETFONT, (WPARAM) hTitleFont, 0);
                SendDlgItemMessage (hwnd, 300, WM_SETFONT, (WPARAM) hTitleFont, 0);
                SendDlgItemMessage (hwnd, 203, CB_ADDSTRING, 0, (LPARAM) L"*");

                // fix buttons
                for (auto btn = 210; btn != 212; ++btn) {
                    RECT rBtn;
                    COMBOBOXINFO cbInfo = { sizeof cbInfo, 0 };

                    if (GetDlgItemCoordinates (hwnd, btn, &rBtn)
                        && SendDlgItemMessage (hwnd, 201, CB_GETCOMBOBOXINFO, 0, (LPARAM) &cbInfo)) {

                        MoveWindow (GetDlgItem (hwnd, btn), rBtn.left, rBtn.top - 1,
                                    rBtn.right - rBtn.left + 1, cbInfo.rcButton.bottom + 2 * cbInfo.rcButton.top + 1, TRUE);
                    }
                }

                if (HWND hList = GetDlgItem (hwnd, 299)) {
                    ListView_SetExtendedListViewStyle (hList, LVS_EX_COLUMNSNAPPOINTS | LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP);

                    wchar_t buffer [128];
                    std::wcscpy (buffer, strings [L"FileDescription"]);
                    AppendRsrcString (buffer, array_size (buffer), 0x30);
                    SetWindowText (hwnd, buffer);
                    
                    RECT r;
                    for (auto c = 0u; c != 3u; ++c) {
                        GetDlgItemCoordinates (hwnd, 201 + c, &r);
                        LoadString (reinterpret_cast <HINSTANCE> (&__ImageBase), 0x31 + c, buffer, (int) array_size (buffer));
                        LVCOLUMN col = { LVCF_WIDTH | LVCF_TEXT, 0, r.right - r.left + (c ? 8 : 0), buffer, 0, 0, 0, 0 };
                        ListView_InsertColumn (hList, c, &col);
                    }

                    DWORD i = 0;
                    auto cchAlert = (DWORD) array_size (buffer);
                    while (RegEnumValue (alerts, i++, buffer, &cchAlert, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                        cchAlert = (DWORD) array_size (buffer);

                        wchar_t * channel = std::wcschr (buffer, L';');
                        wchar_t * build = nullptr;

                        if (channel) {
                            build = std::wcschr (channel, L'#');
                        } else {
                            build = std::wcschr (buffer, L'#');
                        }
                        
                        if (channel) *channel++ = L'\0';
                        if (build) *build++ = L'\0';

                        LVITEM lvi = { LVIF_TEXT, MAXINT, 0, 0, 0, buffer, 0, 0, 0, 0, 0, 0 };
                        auto item = ListView_InsertItem (hList, &lvi);
                        if (channel) {
                            ListView_SetItemText (hList, item, 1, channel);
                        }
                        if (build) {
                            ListView_SetItemText (hList, item, 2, build);
                        }
                    }
                }

                SetDlgItemInt (hwnd, 311, GetSettingsValue (L"check", 180), false);
                CheckDlgButton (hwnd, 320, GetSettingsValue (L"toast"));
                CheckDlgButton (hwnd, 321, GetSettingsValue (L"animate"));
                CheckDlgButton (hwnd, 322, GetSettingsValue (L"legacy"));

                remembered.fill_platforms (GetDlgItem (hwnd, 201));
                UpdateButtons (hwnd);
                return TRUE;

            case WM_CLOSE:
                DeleteObject (hTitleFont);
                break;

            case WM_COMMAND:
                switch (LOWORD (wParam)) {

                    case IDOK:
                        SetSettingsValue (L"check", GetDlgItemInt (hwnd, 311, NULL, FALSE));
                        SetSettingsValue (L"toast", IsDlgButtonChecked (hwnd, 320));
                        SetSettingsValue (L"legacy", IsDlgButtonChecked (hwnd, 322));
                        SetSettingsValue (L"animate", IsDlgButtonChecked (hwnd, 321));

                        RegDeleteTree (alerts, NULL);

                        if (auto hList = GetDlgItem (hwnd, 299)) {
                            auto n = ListView_GetItemCount (hList);
                            for (int i = 0; i != n; ++i) {
                                wchar_t buffer [MAX_PLATFORM_LENGTH + MAX_CHANNEL_LENGTH + MAX_RELEASE_LENGTH + 4];
                                wchar_t platform [MAX_PLATFORM_LENGTH + 1];
                                wchar_t channel [MAX_CHANNEL_LENGTH + 1];
                                wchar_t build [MAX_RELEASE_LENGTH + 1];

                                platform [0] = L'\0';
                                channel [0] = L'\0';
                                build [0] = L'\0';

                                ListView_GetItemText (hList, i, 0, platform, (int) array_size (platform));
                                ListView_GetItemText (hList, i, 1, channel, (int) array_size (channel));
                                ListView_GetItemText (hList, i, 2, build, (int) array_size (build));

                                _snwprintf (buffer, array_size (buffer), L"%s%s%s%s%s",
                                            platform,
                                            channel [0] ? L";" : (build [0] ? L"" : L";*"), channel,
                                            build [0] ? L"#" : L"", build);

                                RegSetValueEx (alerts, buffer, NULL, REG_NONE, NULL, 0);
                            }
                        }

                        EndDialog (hwnd, TRUE);
                        break;

                    case IDCANCEL:
                        EndDialog (hwnd, FALSE);
                        break;

                    case 201:
                    case 202:
                    case 203:
                        switch (HIWORD (wParam)) {
                            case CBN_SELCHANGE:
                            case CBN_EDITCHANGE:
                                if (LOWORD (wParam) == 201) {
                                    remembered.fill_channels (GetDlgItem (hwnd, 202), GetDlgItem (hwnd, 201));
                                }
                                UpdateButtons (hwnd);
                                break;
                        }
                        break;

                    case 210:
                    case 211:
                        auto hList = GetDlgItem (hwnd, 299);
                        bool editing = !IsWindowEnabled (hList);

                        wchar_t text [128];
                        switch (LOWORD (wParam)) {
                            case 210: // add/save
                                GetDlgItemText (hwnd, 201, text, (int) array_size (text));

                                int item;
                                if (editing) {
                                    item = ListView_GetNextItem (hList, -1, LVNI_SELECTED | LVNI_ALL);
                                    ListView_SetItemText (hList, item, 0, text);
                                } else {
                                    LVITEM lvi = { LVIF_TEXT, MAXINT, 0, 0, 0, text, 0, 0, 0, 0, 0, 0 };
                                    item = ListView_InsertItem (hList, &lvi);
                                }
                                GetDlgItemText (hwnd, 202, text, (int) array_size (text));
                                ListView_SetItemText (hList, item, 1, text);

                                GetDlgItemText (hwnd, 203, text, (int) array_size (text));
                                ListView_SetItemText (hList, item, 2, text);

                                ListView_EnsureVisible (hList, item, false);
                                break;

                            case 211: // delete/cancel
                                if (!editing) {
                                    if (auto n = ListView_GetSelectedCount (GetDlgItem (hwnd, 299))) {
                                        if (n == 1) {
                                            LoadString (reinterpret_cast <HINSTANCE> (&__ImageBase), 0x35, text, (int) array_size (text));
                                        } else {
                                            wchar_t fmt [128];
                                            LoadString (reinterpret_cast <HINSTANCE> (&__ImageBase), (n < 5) ? 0x36 : 0x37, fmt, (int) array_size (fmt));
                                            _snwprintf (text, array_size (text), fmt, n);
                                        }

                                        if (MessageBox (hwnd, text, text, MB_ICONWARNING | MB_YESNO) == IDYES) {
                                            int item;
                                            while ((item = ListView_GetNextItem (hList, -1, LVNI_SELECTED | LVNI_ALL)) != -1) {
                                                ListView_DeleteItem (hList, item);
                                            }
                                        }
                                    }
                                }
                                break;
                        }

                        if (editing) {
                            EnableWindow (hList, TRUE);
                            UpdateButtons (hwnd);
                        }
                        break;
                }
                break;

            case WM_NOTIFY:
                if (auto nm = reinterpret_cast <NMHDR *> (lParam)) {
                    switch (nm->idFrom) {
                        case 299:
                            switch (nm->code) {
                                case LVN_GETEMPTYMARKUP:
                                    reinterpret_cast <NMLVEMPTYMARKUP *> (nm)->dwFlags = EMF_CENTERED;
                                    LoadString (reinterpret_cast <HMODULE> (&__ImageBase), 0x34,
                                                reinterpret_cast <NMLVEMPTYMARKUP *> (nm)->szMarkup,
                                                sizeof (NMLVEMPTYMARKUP::szMarkup) / sizeof (NMLVEMPTYMARKUP::szMarkup [0]));
                                    SetWindowLongPtr (hwnd, DWLP_MSGRESULT, TRUE);
                                    return TRUE;

                                case LVN_ITEMCHANGED:
                                    EnableWindow (GetDlgItem (hwnd, 211), ListView_GetSelectedCount (nm->hwndFrom));
                                    break;

                                case LVN_ITEMACTIVATE:
                                    auto item = reinterpret_cast <NMITEMACTIVATE *> (nm)->iItem;
                                    for (auto c = 0u; c != 3u; ++c) {
                                        wchar_t text [32];
                                        ListView_GetItemText (nm->hwndFrom, item, c, text, (int) array_size (text));
                                        SetDlgItemText (hwnd, 201 + c, text);
                                    }
                                    EnableWindow (nm->hwndFrom, FALSE);
                                    UpdateButtons (hwnd);
                                    break;
                            }
                            break;
                        case IDHELP:
                            switch (nm->code) {
                                case NM_CLICK:
                                case NM_RETURN:
                                    OpenWebsite (hwnd, 0x0B);
                            }
                            break;
                    }
                }
                break;

            case WM_CTLCOLORBTN:
            case WM_CTLCOLORSTATIC: {
                int id = GetDlgCtrlID ((HWND) lParam);
                if (id > 128 || id == -1) {
                    switch (id) {
                        case 200:
                        case 300:
                            SetTextColor ((HDC) wParam, crTitleColor);
                            break;
                        default:
                            SetTextColor ((HDC) wParam, GetSysColor (COLOR_WINDOWTEXT));
                    }
                    SetBkColor ((HDC) wParam, GetSysColor (COLOR_WINDOW));
                    return (INT_PTR) GetSysColorBrush (COLOR_WINDOW);
                }
            } break;

            case WM_PAINT: {
                RECT rLimiter;
                if (GetDlgItemCoordinates (hwnd, 127, &rLimiter)) {

                    PAINTSTRUCT ps;
                    if (BeginPaint (hwnd, &ps)) {

                        if (ps.rcPaint.bottom > rLimiter.top)
                            ps.rcPaint.bottom = rLimiter.top;

                        FillRect (ps.hdc, &ps.rcPaint, GetSysColorBrush (COLOR_WINDOW));
                        EndPaint (hwnd, &ps);
                    }
                }
            } break;
        }
        return FALSE;
    }

    std::uint8_t qursor = 0;
    std::uint8_t queued = 0;
    struct {
        void (* callback) (JsonValue) = nullptr;
        char    platform [MAX_PLATFORM_LENGTH] = {};
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
            remembered.reset ();
        }

        if (auto request = WinHttpOpenRequest (connection, NULL, path, NULL,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) {

            if (WinHttpSendRequest (request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, (DWORD_PTR) callback)) {
                return true;
            }
            WinHttpCloseHandle (request);
        }

        failure = GetLastError ();
        animation.set (Failure);

        SetMetricsValue (L"http error", failure); // store error to report if triggered manually
        SetTimer (window, 3, GetSettingsValue (L"retry", 10000), NULL);

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

    class StringBuilder {
        wchar_t * const buffer;
        std::size_t const N;
        std::size_t size = 0;

    public:
        bool        failed = false;

        template <std::size_t N>
        StringBuilder (wchar_t (&buffer) [N])
            : buffer (buffer)
            , N (N) {
        };

        void reset () {
            this->failed = false;
            this->size = 0;
        }
        bool append (const char * s) {
            if (!this->failed) {
                auto n = std::strlen (s);
                if (this->size + n <= N) {
                    for (std::size_t i = 0; i != n; ++i) {
                        this->buffer [this->size++] = wchar_t (*s++);
                    }
                    return true;
                } else
                    this->failed = true;
            }
            return false;
        }
        bool append_rsrc (unsigned int id) {
            if (!this->failed) {
                const wchar_t * string = nullptr;
                if (auto length = LoadString (reinterpret_cast <HINSTANCE> (&__ImageBase), id, (LPWSTR) &string, 0)) {
                    if (this->size + length <= N) {
                        std::memcpy (this->buffer + this->size, string, length * sizeof (wchar_t));
                        this->size += length;
                        return true;
                    } else {
                        this->failed = true;
                    }
                }
            }
            return false;
        }
        void append (wchar_t c) {
            if (!this->failed) {
                if (this->size < N) {
                    this->buffer [this->size++] = c;
                } else {
                    this->failed = true;
                }
            }
        }
        void append (unsigned int u) {
            if (!this->failed) {
                wchar_t s [12];
                auto n = _snwprintf (s, 12, L"%u", u);
                
                if (this->size + n <= N) {
                    std::memcpy (this->buffer + this->size, s, n * sizeof (wchar_t));
                    this->size += n;
                } else {
                    this->failed = true;
                }
            }
        }
        void finish () {
            if (this->size < N) {
                this->buffer [this->size] = L'\0';
            }
        }
    };

    // Report
    //  - collection of builds accumulated to generate nice toast notification
    //
    class Report {
        struct Release {
            char name [MAX_RELEASE_LENGTH + 1];

            std::uint32_t build;
            std::uint32_t ubr;
        };
        struct Channel {
            char name [MAX_CHANNEL_LENGTH + 1];
            std::uint8_t nreleases;

            Release releases [4u];
        };
        struct Platform {
            char name [MAX_PLATFORM_LENGTH + 1];
            std::uint8_t nchannels;

            Channel channels [8u];
        };
        
        Platform platforms [MAX_PLATFORMS];// */

        std::uint8_t nplatforms = 0;

        Platform * access_platform (const char * name) {
            for (auto & p : this->platforms) {
                if (strings_match (p.name, name)) {
                    return &p;
                }
                if (p.name [0] == '\0') {
                    std::strncpy (p.name, name, sizeof p.name);
                    ++this->nplatforms;
                    return &p;
                }
            }
            return nullptr;
        }
        Channel * access_channel (Platform * platform, const char * name) {
            for (auto & ch : platform->channels) {
                if (strings_match (ch.name, name)) {
                    return &ch;
                }
                if (ch.name [0] == '\0') {
                    std::strncpy (ch.name, name, sizeof ch.name);
                    ++platform->nchannels;
                    return &ch;
                }
            }
            return nullptr;
        }
        Release * access_release (Channel * channel, const char * name) {
            for (auto & r : channel->releases) {
                if (strings_match (r.name, name)) {
                    return &r;
                }
                if (r.name [0] == '\0') {
                    std::strncpy (r.name, name, sizeof r.name);
                    ++channel->nreleases;
                    return &r;
                }
            }
            return nullptr;
        }

    public:
        void insert (Update level, const BuildInfo & info) {
            if (auto platform = this->access_platform (info.platform)) {
                if (auto channel = this->access_channel (platform, info.channel)) {
                    if (auto release = this->access_release (channel, info.name)) {
                        release->build = info.build;
                        release->ubr = info.release;
                    } else {
                        ++channel->nreleases;
                    }
                }
            }
        }
    
        void toast () {
            if (this->nplatforms) {
                animation.set (Signalling);

                nid.dwState = 0;
                nid.dwStateMask = NIS_HIDDEN;

                if (GetSettingsValue (L"toast")) {

                    nid.uFlags |= NIF_INFO;
                    nid.dwInfoFlags = NIIF_RESPECT_QUIET_TIME;

                    // TITLE
                    //  - 0: "PC (Dev, Canary, LTSB, LTSC); ISO: Stable; XBox (Abc, Def, Ghi, Jkp)"
                    //  - 1: "PC (Dev, Canary & 2 more); ISO: Stable; XBox (Abc, Def & 2 more)"
                    //  - 2: "PC (3), ISO (1), XBox (4)"
                    //  - 3: "PC, ISO, XBox"

                    StringBuilder title (nid.szInfoTitle);

                    auto level = 0u;
                    for (; level != 4u; ++level) {
                        title.reset ();

                        bool first_platform = true;
                        for (const auto & platform : this->platforms) {
                            if (!platform.name [0])
                                break;

                            if (!first_platform) {
                                title.append ((level >= 2) ? L',' : L';');
                                title.append (L' ');
                            } else {
                                first_platform = false;
                            }
                             
                            if (!title.append (platform.name)) {
                                title.append (L'\x2026');
                                break;
                            }

                            switch (level) {
                                case 2:
                                    title.append (L' ');
                                    title.append (L'(');
                                    title.append ((unsigned int) platform.nchannels);
                                    title.append (L')');
                                    break;

                                case 0:
                                case 1:
                                    if (platform.nchannels > 1) {
                                        title.append (L' ');
                                        title.append (L'(');

                                        auto nchannel = 0;
                                        for (const auto & channel : platform.channels) {
                                            if (!channel.name [0])
                                                break;

                                            if (nchannel) {
                                                if ((level == 1) && (platform.nchannels > 3) && (nchannel == 2)) {
                                                    title.append (" & ");
                                                    title.append (platform.nchannels - 2u);
                                                    title.append_rsrc (0x2A);
                                                    break;
                                                } else {
                                                    title.append (L',');
                                                    title.append (L' ');
                                                }
                                            }
                                            title.append (channel.name);
                                            ++nchannel;
                                        }
                                        title.append (L')');
                                    } else {
                                        title.append (L':');
                                        title.append (L' ');
                                        title.append (platform.channels [0].name);
                                    }
                            }
                        }

                        if (!title.failed)
                            break;
                    }

                    // CONTENT
                    //  - we have 4 rows available
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
                    //     - when many:
                    //        - PC: 10 new builds in 7 channels
                    //          Server: 20348.2031 (LTSC 21H2), 17763.4974 (LTSC) & 14393.6351 (LTSC)
                    //  - order multiple changes by:
                    //     - user set priority
                    //     - build number highest to lowest
                    
                    //this->append (L"\x272E %hs (%hs) \t%hs %u.%u", info.platform, info.channel, info.name, info.build, info.release);

                    StringBuilder content (nid.szInfo);

                    switch (this->nplatforms) {
                        case 1:
                            // TODO: do not repeat platform
                            break;
                        case 2:
                        case 3:
                            // TODO: compute channels or builds
                            // TODO: count different channels

                            break;
                        case 4:
                            // TODO: per-platform
                            break;
                        default:
                            // TODO: show "& X more..." on the last row
                            break;
                    }

                    title.finish ();
                    content.finish ();

                    nid.uTimeout = 60'000;
                }
                
                Shell_NotifyIcon (NIM_MODIFY, &nid);
                nid.uFlags &= ~NIF_INFO;
            }
        }
    } * report = NULL;

    static const auto xfjhdslkfjs = sizeof (Report);

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

    bool UpdateBuild (const wchar_t * key, const BuildInfo & info, bool do_report) {
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

            if (!installed && do_report) {

                // compare with alerts

                wchar_t alert [256];
                auto cchAlert = (DWORD) array_size (alert);

                DWORD i = 0;
                while (RegEnumValue (alerts, i++, alert, &cchAlert, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                    cchAlert = (DWORD) array_size (alert);

                    if (Windows::MatchFilename (key, alert)) {

                        if (report == nullptr)
                            report = (Report *) HeapAlloc (heap, HEAP_ZERO_MEMORY, sizeof (Report));

                        if (report)
                            report->insert (updated, info);

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
    bool UpdateBuilds (const BuildInfo & info) {
        wchar_t key [MAX_PLATFORM_LENGTH + MAX_CHANNEL_LENGTH + 16];
        auto reported = 0u;

        _snwprintf (key, array_size (key), L"%hs;%hs#%u", info.platform, info.channel, info.build); // "PC;Canary#23456" - for tracking UBRs for build
        reported += UpdateBuild (key, info, !reported);

        _snwprintf (key, array_size (key), L"%hs#%u", info.platform, info.build); // "PC#23456" - for tracking UBRs for build
        reported += UpdateBuild (key, info, !reported);

        _snwprintf (key, array_size (key), L"%hs;%hs", info.platform, info.name); // "PC;21H2"
        reported += UpdateBuild (key, info, !reported);

        _snwprintf (key, array_size (key), L"%hs;%hs", info.platform, info.channel); // "PC;Canary" or "SDK;Unstable"
        reported += UpdateBuild (key, info, !reported);

        remembered.insert (info);
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
        if (mode != Checking) {
            result = SubmitRequest (nullptr, timeline);
            if (result) {
                animation.set (Checking);
            }
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
                            WriteFile (h, json, DWORD (o - json), &n, NULL);
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

            ScheduleCheck ();

            if (failure) {
                animation.set (Failure);
            } else {
                animation.set (Idle);
            }

            if (report) {
                report->toast ();

                HeapFree (heap, 0, report);
                report = nullptr;
            }

            TrimMemoryUsage ();

            if (failure) {
                SetTimer (window, 3, GetSettingsValue (L"retry", 10000), NULL);
            } else {
                EnableMenuItem (menu, 0x1D, MF_ENABLED);
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
#ifndef _M_ARM64
        if (IsWindows8Point1OrGreater ()) {
#endif
            HeapSetInformation (NULL, HeapOptimizeResources, NULL, 0);
#ifndef _M_ARM64
        }
#endif
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
