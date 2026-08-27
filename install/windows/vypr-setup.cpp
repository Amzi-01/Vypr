// Vypr - Windows guest installer.
//
// One executable, run inside the Windows VM. It puts the agent in place, gives
// the Linux host a way in over SSH, and installs the two drivers that Vypr
// cannot work without: IVSHMEM, which is how frames leave the VM, and Parsec's
// virtual USB driver, which is what makes the mouse behave in games.
//
// The work runs on a background thread so the window stays responsive, and
// every step reports into the log box rather than throwing up dialogs. A step
// that fails does not stop the others: a machine with no internet can still
// get the agent and SSH, and be told what is missing.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <urlmon.h>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

#define IDR_AGENT        101
#define IDI_VYPR         102
#define ID_LOG           1001
#define ID_INSTALL       1002
#define ID_KEY           1003
#define ID_CHK_DRIVERS   1010
#define ID_CHK_SSH       1011
#define ID_CHK_AUTOLOGIN 1012
#define ID_CHK_PARSEC    1013
#define ID_PASSWORD      1014
#define ID_SOURCE_LINK   1015
#define WM_STEP_DONE     (WM_APP + 1)

static const wchar_t *LG_HOST_URL =
    L"https://looking-glass.io/artifact/stable/host";
static const wchar_t *PARSEC_VUD_URL =
    L"https://builds.parsec.app/vud/parsec-vud-0.3.10.0.exe";
static const wchar_t *SOURCE_URL =
    L"https://github.com/Amzi-01/Vypr/blob/master/install/windows/vypr-setup.cpp";

static HWND g_main, g_log, g_key, g_install, g_progress;
static HWND g_chk_drivers, g_chk_ssh, g_chk_autologin, g_chk_parsec;
static HWND g_password, g_pw_label, g_pw_note, g_link;
static HFONT g_font, g_font_bold;
static bool  g_failed = false;

// ---------------------------------------------------------------- logging

static void logf(const wchar_t *fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);

    int i = (int)SendMessageW(g_log, LB_ADDSTRING, 0, (LPARAM)buf);
    SendMessageW(g_log, LB_SETTOPINDEX, i, 0);
}

// ------------------------------------------------------------ running things

// Runs a command and feeds its output into the log a line at a time. Used for
// the parts of Windows that are only reasonable to drive from PowerShell -
// optional features, services, ACLs - rather than reimplementing them here.
static DWORD run(const std::wstring &cmd, bool quiet = false)
{
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return (DWORD)-1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = si.hStdError = wr;

    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmd;
    BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return (DWORD)-1; }

    std::string acc;
    char chunk[512];
    DWORD n;
    while (ReadFile(rd, chunk, sizeof chunk, &n, nullptr) && n) {
        acc.append(chunk, n);
        size_t nl;
        while ((nl = acc.find('\n')) != std::string::npos) {
            std::string line = acc.substr(0, nl);
            acc.erase(0, nl + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (!line.empty() && !quiet) {
                wchar_t w[1024];
                MultiByteToWideChar(CP_ACP, 0, line.c_str(), -1, w, 1024);
                logf(L"      %s", w);
            }
        }
    }
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
}

// PowerShell is driven through a script file rather than -Command. Passing a
// script as an argument means escaping quotes through both the C++ literal and
// the command line, which is where mistakes hide; a file has to escape nothing.
static DWORD powershell(const std::wstring &script, bool quiet = false)
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"vypr-step.ps1";

    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return (DWORD)-1;

    int need = WideCharToMultiByte(CP_UTF8, 0, script.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(need > 0 ? need - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, script.c_str(), -1, utf8.data(), need, nullptr, nullptr);

    DWORD written = 0;
    const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    WriteFile(f, bom, 3, &written, nullptr);
    WriteFile(f, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    CloseHandle(f);

    DWORD code = run(L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" +
                     path + L"\"", quiet);
    DeleteFileW(path.c_str());
    return code;
}

static bool step_ok(bool cond, const wchar_t *what)
{
    if (cond) { logf(L"  [ok] %s", what); return true; }
    logf(L"  [!!] %s", what);
    g_failed = true;
    return false;
}

// ------------------------------------------------------------------- paths

static std::wstring program_dir()
{
    wchar_t pf[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, 0, pf);
    return std::wstring(pf) + L"\\Vypr";
}

static std::wstring temp_dir()
{
    wchar_t t[MAX_PATH];
    GetTempPathW(MAX_PATH, t);
    return t;
}

// ------------------------------------------------------------- the agent

static bool write_resource(int id, const std::wstring &path)
{
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return false;
    HGLOBAL h = LoadResource(nullptr, res);
    if (!h) return false;
    void *data = LockResource(h);
    DWORD size = SizeofResource(nullptr, res);
    if (!data || !size) return false;

    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, data, size, &written, nullptr);
    CloseHandle(f);
    return ok && written == size;
}

static void install_agent()
{
    logf(L"Installing the agent");
    std::wstring dir = program_dir();
    CreateDirectoryW(dir.c_str(), nullptr);

    std::wstring exe = dir + L"\\vypr-agent.exe";

    // A running agent holds the IVSHMEM device open, and the driver does not
    // release it if the process is killed - that takes a reboot to clear. So
    // ask the task to end and give it a moment, rather than terminating it.
    run(L"schtasks /end /tn vypr-agent", true);
    Sleep(2000);

    step_ok(write_resource(IDR_AGENT, exe), L"vypr-agent.exe unpacked");

    /*
     * The task runs a small script rather than the agent directly, so its
     * output lands in a file. Everything the agent has to say about why it is
     * not working - no display attached, a region it does not recognise, a
     * capture that stopped - goes to stderr, and a task started with no
     * redirection throws all of it away. Diagnosing anything without this
     * means reconstructing the launch by hand first.
     */
    wchar_t progdata[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, progdata);
    const std::wstring logdir = std::wstring(progdata) + L"\\Vypr";
    CreateDirectoryW(logdir.c_str(), nullptr);

    const std::wstring script = dir + L"\\run-agent.cmd";
    {
        std::wstring body = L"@echo off\r\n\"" + exe + L"\" > \"" +
                            logdir + L"\\agent.log\" 2>&1\r\n";
        HANDLE f = CreateFileW(script.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            std::string utf8;
            utf8.reserve(body.size());
            for (wchar_t c : body) utf8.push_back((char)(c & 0x7f));
            DWORD wrote = 0;
            WriteFile(f, utf8.data(), (DWORD)utf8.size(), &wrote, nullptr);
            CloseHandle(f);
        }
    }
    logf(L"  [--] the agent logs to %s\\agent.log", logdir.c_str());

    // Interactive and elevated: a non-interactive task has no desktop to
    // capture, and the agent needs to see windows owned by elevated apps.
    std::wstring task =
        L"schtasks /create /tn vypr-agent /tr \"\\\"" + script + L"\\\"\" "
        L"/sc onlogon /it /rl highest /f";
    step_ok(run(task, true) == 0, L"registered to start when you log in");
}

// ------------------------------------------------------------------ drivers

static bool device_has_driver(const wchar_t *hwid_fragment)
{
    std::wstring q =
        L"$d = Get-PnpDevice | Where-Object { $_.InstanceId -like '*" +
        std::wstring(hwid_fragment) + L"*' -and $_.Status -eq 'OK' }\n"
        L"if ($d) { exit 0 } else { exit 1 }\n";
    return powershell(q, true) == 0;
}

static void install_ivshmem()
{
    logf(L"IVSHMEM driver (this is how frames leave the VM)");

    if (device_has_driver(L"VEN_1AF4&DEV_1110")) {
        logf(L"  [ok] already installed");
        return;
    }

    std::wstring zip = temp_dir() + L"looking-glass-host.zip";
    std::wstring dir = temp_dir() + L"vypr-lg";

    logf(L"  downloading the Looking Glass host package...");
    if (URLDownloadToFileW(nullptr, LG_HOST_URL, zip.c_str(), 0, nullptr) != S_OK) {
        logf(L"  [!!] could not download it. The driver ships with the Looking");
        logf(L"       Glass host package - install that by hand from");
        logf(L"       looking-glass.io and run this again.");
        g_failed = true;
        return;
    }

    // It is the only package that carries a signed ivshmem.inf; its installer
    // lays the files down, and pnputil then binds them to the device.
    powershell(L"Expand-Archive -LiteralPath '" + zip + L"' -DestinationPath '" +
               dir + L"' -Force", true);
    run(L"\"" + dir + L"\\looking-glass-host-setup.exe\" /S", true);

    std::wstring inf = L"C:\\Program Files\\Looking Glass (host)\\ivshmem.inf";
    run(L"pnputil /add-driver \"" + inf + L"\" /install", true);

    step_ok(device_has_driver(L"VEN_1AF4&DEV_1110"), L"IVSHMEM driver installed");
}

static void install_parsec_vud()
{
    logf(L"Parsec virtual USB driver (this is what fixes the mouse in games)");

    // Vypr injects mouse motion with SendInput, which always goes through the
    // Win32 cursor pipeline. A game reading raw input for its camera wants the
    // deltas a physical mouse produces and gets absolute packets instead, so it
    // reads the cursor at a corner and whips the camera there every frame. No
    // userspace API can produce a real HID delta. Parsec's driver injects at
    // the kernel HID level, which is why it is here.
    std::wstring exe = temp_dir() + L"parsec-vud.exe";
    logf(L"  downloading...");
    if (URLDownloadToFileW(nullptr, PARSEC_VUD_URL, exe.c_str(), 0, nullptr) != S_OK) {
        logf(L"  [!!] could not download it. Without this driver the mouse will");
        logf(L"       misbehave in games that read raw input. Get it from");
        logf(L"       builds.parsec.app and run it yourself.");
        g_failed = true;
        return;
    }

    logf(L"  installing (Windows may ask you to trust the driver)...");
    step_ok(run(L"\"" + exe + L"\" /silent", true) == 0 ||
            run(L"\"" + exe + L"\"", true) == 0,
            L"Parsec virtual USB driver installed");
}

// --------------------------------------------------------------------- SSH

static void setup_ssh(const std::wstring &pubkey)
{
    logf(L"SSH access, so the host can start things in here");

    if (pubkey.empty()) {
        logf(L"  [--] skipped: no public key was given");
        return;
    }

    logf(L"  enabling the OpenSSH server...");
    powershell(L"Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0 "
               L"-ErrorAction SilentlyContinue | Out-Null", true);
    powershell(L"Set-Service -Name sshd -StartupType Automatic; Start-Service sshd", true);

    // Where the key goes depends on the account. sshd reads
    // administrators_authorized_keys for anyone in the administrators group and
    // ignores their profile copy entirely - and it refuses that file outright
    // unless only SYSTEM and Administrators can write it. A standard account
    // uses the ordinary path instead.
    //
    // The key is appended if it is not already there, never written over the
    // file: someone re-running this should not lose the keys they already had.
    std::wstring akeys =
        L"$key = '" + pubkey + L"'\n"
        L"$admin = ([Security.Principal.WindowsPrincipal]"
        L"[Security.Principal.WindowsIdentity]::GetCurrent())"
        L".IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)\n"
        L"if ($admin) {\n"
        L"  $p = Join-Path $env:ProgramData 'ssh\\administrators_authorized_keys'\n"
        L"} else {\n"
        L"  $d = Join-Path $env:USERPROFILE '.ssh'\n"
        L"  New-Item -ItemType Directory -Force -Path $d | Out-Null\n"
        L"  $p = Join-Path $d 'authorized_keys'\n"
        L"}\n"
        L"$existing = if (Test-Path $p) { Get-Content $p } else { @() }\n"
        L"if ($existing -notcontains $key) {\n"
        L"  ($existing + $key) | Set-Content -Path $p -Encoding ascii\n"
        L"}\n"
        L"if ($admin) {\n"
        L"  icacls $p /inheritance:r /grant 'SYSTEM:F' "
        L"/grant 'BUILTIN\\Administrators:F' | Out-Null\n"
        L"}\n"
        L"exit 0\n";
    step_ok(powershell(akeys, true) == 0, L"host key authorised");

    powershell(L"New-NetFirewallRule -Name vypr-ssh -DisplayName 'Vypr (SSH)' "
               L"-Enabled True -Direction Inbound -Protocol TCP -LocalPort 22 "
               L"-Action Allow -ErrorAction SilentlyContinue | Out-Null", true);

    step_ok(powershell(L"if ((Get-Service sshd).Status -eq 'Running') "
                       L"{ exit 0 } else { exit 1 }\n", true) == 0,
            L"SSH server running");
}

// -------------------------------------------------------------- auto-login

static void setup_autologin(std::wstring &password)
{
    logf(L"Automatic login");

    // Without a logged-in session there is no desktop to capture and
    // interactive scheduled tasks will not run, so a VM that boots to a lock
    // screen is a VM Vypr cannot use. This is the only reason the password is
    // asked for, and the only thing it is used for.
    wchar_t user[256];
    DWORD n = 256;
    GetUserNameW(user, &n);

    const std::wstring key =
        L"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";

    run(L"reg add \"" + key + L"\" /v AutoAdminLogon /t REG_SZ /d 1 /f", true);
    run(L"reg add \"" + key + L"\" /v DefaultUserName /t REG_SZ /d \"" +
        std::wstring(user) + L"\" /f", true);

    if (password.empty()) {
        logf(L"  [--] no password given, so this is half-done: Windows will try");
        logf(L"       to log %s in and stop at the password prompt.", user);
        SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
        return;
    }

    // Windows itself reads this value at logon; there is no way to enable
    // automatic login without it being here, and no encrypted form of it that
    // Winlogon accepts. Said plainly rather than buried.
    const DWORD rc = run(L"reg add \"" + key + L"\" /v DefaultPassword /t REG_SZ /d \"" +
                         password + L"\" /f", true);

    /* Out of this process's memory as soon as it has been handed over. */
    SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
    password.clear();

    if (rc == 0) {
        logf(L"  [ok] %s will be logged in automatically at boot", user);
        logf(L"  [--] Windows stores this password in the registry in a form it");
        logf(L"       can read back. That is how automatic login works; it is");
        logf(L"       not something Vypr chose. Anyone with administrator access");
        logf(L"       to this VM can read it.");
    } else {
        step_ok(false, L"could not write the automatic-login password");
    }
}

// ------------------------------------------------------------------ driver

static DWORD WINAPI worker(LPVOID)
{
    bool want_drivers   = SendMessageW(g_chk_drivers, BM_GETCHECK, 0, 0) == BST_CHECKED;
    bool want_parsec    = SendMessageW(g_chk_parsec, BM_GETCHECK, 0, 0) == BST_CHECKED;
    bool want_ssh       = SendMessageW(g_chk_ssh, BM_GETCHECK, 0, 0) == BST_CHECKED;
    bool want_autologin = SendMessageW(g_chk_autologin, BM_GETCHECK, 0, 0) == BST_CHECKED;

    int len = GetWindowTextLengthW(g_key);
    std::wstring pubkey(len + 1, L'\0');
    GetWindowTextW(g_key, pubkey.data(), len + 1);
    pubkey.resize(len);
    while (!pubkey.empty() && (pubkey.back() == L'\r' || pubkey.back() == L'\n'))
        pubkey.pop_back();

    std::wstring password;
    if (want_autologin) {
        int plen = GetWindowTextLengthW(g_password);
        password.assign(plen + 1, L'\0');
        GetWindowTextW(g_password, password.data(), plen + 1);
        password.resize(plen);
        /* Cleared from the control too, so it is not sitting in a window that
         * stays open after the work is done. */
        SetWindowTextW(g_password, L"");
    }

    install_agent();
    if (want_drivers)   install_ivshmem();
    if (want_parsec)    install_parsec_vud();
    if (want_ssh)       setup_ssh(pubkey);
    if (want_autologin) setup_autologin(password);

    PostMessageW(g_main, WM_STEP_DONE, 0, 0);
    return 0;
}

// -------------------------------------------------------------------- window

static HWND mk(const wchar_t *cls, const wchar_t *text, DWORD style,
               int x, int y, int w, int h, int id, HWND parent)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_font_bold = CreateFontW(-22, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                                  DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                                  L"Segoe UI");

        HWND title = mk(L"STATIC", L"Vypr", 0, 24, 20, 300, 32, 0, h);
        SendMessageW(title, WM_SETFONT, (WPARAM)g_font_bold, TRUE);

        mk(L"STATIC",
           L"Sets this Windows machine up so your Linux desktop can run its "
           L"applications as if they were local. Everything below is optional "
           L"except the agent.",
           0, 24, 56, 520, 40, 0, h);

        g_chk_drivers = mk(L"BUTTON", L"IVSHMEM driver \x2014 how frames leave the VM",
                           BS_AUTOCHECKBOX, 24, 108, 480, 22, ID_CHK_DRIVERS, h);
        g_chk_parsec = mk(L"BUTTON", L"Parsec virtual USB driver \x2014 fixes the mouse in games",
                          BS_AUTOCHECKBOX, 24, 132, 480, 22, ID_CHK_PARSEC, h);
        g_chk_ssh = mk(L"BUTTON", L"OpenSSH server \x2014 lets the host start things in here",
                       BS_AUTOCHECKBOX, 24, 156, 480, 22, ID_CHK_SSH, h);
        g_chk_autologin = mk(L"BUTTON",
                             L"Log in to Windows automatically \x2014 Vypr needs a desktop to capture",
                             BS_AUTOCHECKBOX, 24, 180, 480, 22, ID_CHK_AUTOLOGIN, h);

        SendMessageW(g_chk_drivers, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(g_chk_parsec, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(g_chk_ssh, BM_SETCHECK, BST_CHECKED, 0);

        g_pw_label = mk(L"STATIC", L"Windows password for this account:",
                        0, 44, 206, 480, 20, 0, h);
        g_password = mk(L"EDIT", L"", WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL,
                        44, 226, 300, 24, ID_PASSWORD, h);
        g_pw_note = mk(L"STATIC",
            L"Used once, to write Windows' own automatic-login setting. It is "
            L"not sent anywhere and Vypr does not keep it. Windows stores it in "
            L"the registry \x2014 that is how automatic login works.",
            0, 44, 256, 490, 72, 0, h);
        g_link = mk(L"STATIC", L"Read exactly what this does \x2014 the source of this installer",
                    SS_NOTIFY, 44, 332, 490, 20, ID_SOURCE_LINK, h);

        /* Off until asked for, so the field cannot be filled in by habit. */
        EnableWindow(g_password, FALSE);
        EnableWindow(g_pw_label, FALSE);

        mk(L"STATIC", L"Host public key (the Linux installer printed this):",
           0, 24, 368, 480, 20, 0, h);
        g_key = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
                   24, 390, 520, 24, ID_KEY, h);

        g_log = mk(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOSEL,
                   24, 428, 520, 186, ID_LOG, h);

        g_install = mk(L"BUTTON", L"Install", BS_DEFPUSHBUTTON,
                       444, 626, 100, 30, ID_INSTALL, h);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(w) == ID_CHK_AUTOLOGIN) {
            const BOOL on = SendMessageW(g_chk_autologin, BM_GETCHECK, 0, 0) == BST_CHECKED;
            EnableWindow(g_password, on);
            EnableWindow(g_pw_label, on);
            if (!on) SetWindowTextW(g_password, L"");
            return 0;
        }
        if (LOWORD(w) == ID_SOURCE_LINK && HIWORD(w) == STN_CLICKED) {
            ShellExecuteW(nullptr, L"open", SOURCE_URL, nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        if (LOWORD(w) == ID_INSTALL) {
            /*
             * Once the work is done this button is Close.
             *
             * That used to be handled in the message loop, which never saw it:
             * a button sends WM_COMMAND straight to its parent's window
             * procedure, so it is never posted to the queue and GetMessage
             * cannot filter for it. The button did nothing at all.
             */
            if (GetWindowLongPtrW(g_install, GWLP_USERDATA) == 1) {
                DestroyWindow(h);
                return 0;
            }
            EnableWindow(g_install, FALSE);
            SendMessageW(g_log, LB_RESETCONTENT, 0, 0);
            g_failed = false;
            CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        }
        return 0;

    case WM_STEP_DONE:
        logf(L"");
        if (g_failed) {
            logf(L"Finished, but some steps did not complete - see the [!!] lines.");
            logf(L"Everything else is installed; fix those and run this again.");
        } else {
            logf(L"Done. Reboot Windows, then launch from Linux with: vypr run <app>");
        }
        SetWindowTextW(g_install, L"Close");
        EnableWindow(g_install, TRUE);
        SetWindowLongPtrW(g_install, GWLP_USERDATA, 1);
        return 0;

    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)w, TRANSPARENT);
        if ((HWND)l == g_link) SetTextColor((HDC)w, RGB(0, 90, 200));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
{
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"VyprSetup";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_VYPR));
    RegisterClassExW(&wc);

    RECT r{0, 0, 570, 676};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME, FALSE);
    g_main = CreateWindowExW(0, L"VyprSetup", L"Vypr Setup",
                             (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX),
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, inst, nullptr);
    if (!g_main) {
        wchar_t err[128];
        _snwprintf_s(err, _TRUNCATE, L"Could not create the window (error %lu).",
                     GetLastError());
        MessageBoxW(nullptr, err, L"Vypr Setup", MB_ICONERROR);
        return 1;
    }

    // Deliberately not the nCmdShow we were handed: launched from a scheduled
    // task that is SW_HIDE, and an installer nobody can see is worse than no
    // installer at all.
    ShowWindow(g_main, SW_SHOWNORMAL);
    SetForegroundWindow(g_main);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_main, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
