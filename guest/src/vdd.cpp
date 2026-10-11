// vypr-vdd - configure the Virtual Display Driver's monitors for immersive mode.
//
// Immersive mode gives the guest one virtual monitor per host output, each at
// that output's resolution and laid out the same way. This small tool is what
// the launcher calls, inside the guest, to make that happen without any
// fragile PowerShell:
//
//   vypr-vdd count N
//       Set how many virtual monitors the driver creates (vdd_settings.xml)
//       and reload it, so the count takes effect without a reboot.
//
//   vypr-vdd layout WxH@X,Y [WxH@X,Y ...]
//       Set each Virtual Display Driver monitor, in sorted order, to the given
//       resolution and desktop position. One argument per monitor. Monitors
//       past the end of the list are left alone.
//
//   vypr-vdd list
//       Print every display: name, whether it is a VDD monitor, and its
//       current mode and position. For debugging and for the launcher to learn
//       the mapping.
//
// Display settings are global, not per session, so this works run over SSH
// (session 0) and still changes the console desktop.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// vdd_settings.xml lives here; the driver reads it at load and on reload.
const wchar_t *kSettingsDir  = L"C:\\VirtualDisplayDriver";
const wchar_t *kSettingsPath = L"C:\\VirtualDisplayDriver\\vdd_settings.xml";
// The driver watches this pipe for a reload request.
const wchar_t *kPipe         = L"\\\\.\\pipe\\MTTVirtualDisplayPipe";

struct Display {
    std::wstring name;     // \\.\DISPLAYn
    std::wstring desc;     // adapter string
    bool         vdd;
    bool         attached;
    int          w, h, x, y;
};

std::vector<Display> enum_displays()
{
    std::vector<Display> out;
    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof dd;
        if (!EnumDisplayDevicesW(nullptr, i, &dd, 0)) break;

        Display d{};
        d.name     = dd.DeviceName;
        d.desc     = dd.DeviceString;
        d.vdd      = wcsstr(dd.DeviceString, L"Virtual Display") != nullptr;
        d.attached = (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;

        DEVMODEW dm{};
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
            d.w = (int)dm.dmPelsWidth;
            d.h = (int)dm.dmPelsHeight;
            d.x = dm.dmPosition.x;
            d.y = dm.dmPosition.y;
        }
        out.push_back(std::move(d));
    }
    return out;
}

// VDD monitors in the launcher's order: attached, sorted by (x, y).
std::vector<Display> vdd_monitors_sorted()
{
    std::vector<Display> v;
    for (auto &d : enum_displays())
        if (d.vdd && d.attached) v.push_back(d);
    std::sort(v.begin(), v.end(), [](const Display &a, const Display &b) {
        if (a.x != b.x) return a.x < b.x;
        return a.y < b.y;
    });
    return v;
}

int cmd_list()
{
    for (auto &d : enum_displays())
        std::wprintf(L"%ls '%ls' vdd=%d attached=%d %dx%d @%d,%d\n",
                     d.name.c_str(), d.desc.c_str(), d.vdd ? 1 : 0,
                     d.attached ? 1 : 0, d.w, d.h, d.x, d.y);
    return 0;
}

int cmd_count(int n)
{
    if (n < 1 || n > 16) { std::fprintf(stderr, "vypr-vdd: count 1..16\n"); return 2; }

    CreateDirectoryW(kSettingsDir, nullptr);

    // A minimal settings file: the count, a default GPU, and the handful of
    // resolutions immersive mode uses. The exact modes matter less than the
    // count - layout sets the real per-monitor mode afterwards - but listing
    // them makes those modes selectable.
    std::string xml =
        "<?xml version='1.0' encoding='utf-8'?>\n<vdd_settings>\n"
        "    <monitors><count>" + std::to_string(n) + "</count></monitors>\n"
        "    <gpu><friendlyname>default</friendlyname></gpu>\n"
        "    <global><g_refresh_rate>60</g_refresh_rate></global>\n"
        "    <resolutions>\n"
        "        <resolution><width>3840</width><height>2160</height><refresh_rate>60</refresh_rate></resolution>\n"
        "        <resolution><width>3440</width><height>1440</height><refresh_rate>60</refresh_rate></resolution>\n"
        "        <resolution><width>2560</width><height>1440</height><refresh_rate>60</refresh_rate></resolution>\n"
        "        <resolution><width>1920</width><height>1080</height><refresh_rate>60</refresh_rate></resolution>\n"
        "    </resolutions>\n"
        "    <options><HardwareCursor>true</HardwareCursor><logging>false</logging></options>\n"
        "</vdd_settings>\n";

    HANDLE f = CreateFileW(kSettingsPath, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "vypr-vdd: cannot write %ls (err %lu)\n",
                     kSettingsPath, GetLastError());
        return 1;
    }
    DWORD wrote = 0;
    WriteFile(f, xml.data(), (DWORD)xml.size(), &wrote, nullptr);
    CloseHandle(f);

    // Nudge the driver to re-read. Best effort: if the pipe is not there the
    // file is still read next load, and setting the count usually coincides
    // with a (re)install that loads it anyway.
    HANDLE pipe = CreateFileW(kPipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
        const char *msg = "RELOAD_DRIVER";
        DWORD n2 = 0;
        WriteFile(pipe, msg, (DWORD)std::strlen(msg), &n2, nullptr);
        CloseHandle(pipe);
        std::wprintf(L"count=%d, driver asked to reload\n", n);
    } else {
        std::wprintf(L"count=%d written (reload pipe not open; takes effect on next load)\n", n);
    }
    return 0;
}

bool set_mode(const std::wstring &name, int w, int h, int x, int y)
{
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsW(name.c_str(), ENUM_CURRENT_SETTINGS, &dm)) return false;
    dm.dmPelsWidth  = w;
    dm.dmPelsHeight = h;
    dm.dmPosition.x = x;
    dm.dmPosition.y = y;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION;
    // Into the registry now; a final NULL apply commits every monitor at once.
    LONG r = ChangeDisplaySettingsExW(name.c_str(), &dm, nullptr,
                                      CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    return r == DISP_CHANGE_SUCCESSFUL;
}

int cmd_layout(int argc, wchar_t **argv)
{
    std::vector<std::tuple<int, int, int, int>> want;  // w,h,x,y
    for (int i = 0; i < argc; ++i) {
        int w, h, x, y;
        if (swscanf_s(argv[i], L"%dx%d@%d,%d", &w, &h, &x, &y) != 4) {
            std::fprintf(stderr, "vypr-vdd: layout wants WxH@X,Y, got '%ls'\n", argv[i]);
            return 2;
        }
        want.emplace_back(w, h, x, y);
    }

    auto mons = vdd_monitors_sorted();
    if (mons.size() < want.size())
        std::fprintf(stderr, "vypr-vdd: %zu monitor(s) asked for, %zu present\n",
                     want.size(), mons.size());

    size_t n = std::min(mons.size(), want.size());
    bool ok = true;
    for (size_t i = 0; i < n; ++i) {
        auto [w, h, x, y] = want[i];
        if (set_mode(mons[i].name, w, h, x, y))
            std::wprintf(L"%ls -> %dx%d @%d,%d\n", mons[i].name.c_str(), w, h, x, y);
        else {
            std::fprintf(stderr, "vypr-vdd: %ls refused %dx%d @%d,%d\n",
                         mons[i].name.c_str(), w, h, x, y);
            ok = false;
        }
    }

    // Commit all the staged changes together.
    LONG r = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    if (r != DISP_CHANGE_SUCCESSFUL) {
        std::fprintf(stderr, "vypr-vdd: apply failed (%ld)\n", r);
        ok = false;
    }
    return ok ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc >= 2 && !wcscmp(argv[1], L"list"))   return cmd_list();
    if (argc >= 3 && !wcscmp(argv[1], L"count"))  return cmd_count(_wtoi(argv[2]));
    if (argc >= 2 && !wcscmp(argv[1], L"layout")) return cmd_layout(argc - 2, argv + 2);

    std::fprintf(stderr,
        "usage: vypr-vdd count N\n"
        "       vypr-vdd layout WxH@X,Y [WxH@X,Y ...]\n"
        "       vypr-vdd list\n");
    return 2;
}
