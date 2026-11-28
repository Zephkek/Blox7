#include "studio7.h"

#define CLASSES L"Software\\Classes\\"
#define PARKED L"Software\\Studio7\\Replaced links\\"

static const WCHAR *const g_schemes[] = {L"roblox-studio", L"roblox-studio-auth"};

static void own_exe(WCHAR *out) {
    GetModuleFileNameW(NULL, out, MAX_PATH);
    out[MAX_PATH - 1] = 0;
}

static void command_for(const WCHAR *exe, WCHAR *out) {
    wide_format(out, MAX_PATH + 16, L"\"%ls\" \"%%1\"", exe);
}

static BOOL key_exists(const WCHAR *path) {
    HKEY key;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &key)) return FALSE;
    RegCloseKey(key);
    return TRUE;
}

static BOOL links_to(const WCHAR *scheme, const WCHAR *exe) {
    WCHAR key[160], value[MAX_PATH + 64], command[MAX_PATH + 16];
    DWORD size = sizeof(value);

    if (!exe[0]) return FALSE;
    command_for(exe, command);
    wide_format(key, 160, CLASSES L"%ls\\shell\\open\\command", scheme);
    return !RegGetValueW(HKEY_CURRENT_USER, key, NULL, RRF_RT_REG_SZ, NULL, value, &size) && !_wcsicmp(value, command);
}

static LSTATUS copy_key(const WCHAR *from, const WCHAR *to) {
    HKEY source = NULL, target = NULL;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, from, 0, KEY_READ, &source);

    if (!status) status = RegCreateKeyExW(HKEY_CURRENT_USER, to, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &target, NULL);
    if (!status) status = RegCopyTreeW(source, NULL, target);
    if (target) RegCloseKey(target);
    if (source) RegCloseKey(source);
    if (status) RegDeleteTreeW(HKEY_CURRENT_USER, to);
    return status;
}

static LSTATUS set_value(const WCHAR *key, const WCHAR *sub, const WCHAR *name, const WCHAR *value) {
    WCHAR path[200];

    wide_format(path, 200, L"%ls%ls", key, sub);
    return RegSetKeyValueW(HKEY_CURRENT_USER, path, name, REG_SZ, value, (DWORD)((wcslen(value) + 1) * sizeof(WCHAR)));
}

static LSTATUS take(const WCHAR *scheme, const WCHAR *exe, BOOL ours) {
    WCHAR key[160], parked[160], command[MAX_PATH + 16], icon[MAX_PATH + 8];
    BOOL parking;
    LSTATUS status;

    wide_format(key, 160, CLASSES L"%ls", scheme);
    wide_format(parked, 160, PARKED L"%ls", scheme);
    parking = !ours && key_exists(key);
    if (!ours) RegDeleteTreeW(HKEY_CURRENT_USER, parked);
    if (parking) {
        status = copy_key(key, parked);
        if (status) return status;
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, key);
    command_for(exe, command);
    wide_format(icon, MAX_PATH + 8, L"%ls,0", exe);
    status = set_value(key, L"\\shell\\open\\command", NULL, command);
    if (!status) status = set_value(key, L"\\DefaultIcon", NULL, icon);
    if (!status) status = set_value(key, L"", NULL, L"URL: Roblox Protocol");
    if (!status) status = set_value(key, L"", L"URL Protocol", L"");
    if (status) {
        RegDeleteTreeW(HKEY_CURRENT_USER, key);
        if (parking && !copy_key(parked, key)) RegDeleteTreeW(HKEY_CURRENT_USER, parked);
    }
    return status;
}

static void give_back(const WCHAR *scheme, BOOL ours) {
    WCHAR key[160], parked[160];

    wide_format(key, 160, CLASSES L"%ls", scheme);
    wide_format(parked, 160, PARKED L"%ls", scheme);
    if (ours) {
        RegDeleteTreeW(HKEY_CURRENT_USER, key);
        if (key_exists(parked) && copy_key(parked, key)) return;
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, parked);
}

static void drop_if_empty(const WCHAR *path) {
    HKEY key;
    DWORD subkeys = 1, values = 1;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &key)) return;
    RegQueryInfoKeyW(key, NULL, NULL, NULL, &subkeys, NULL, NULL, &values, NULL, NULL, NULL, NULL);
    RegCloseKey(key);
    if (!subkeys && !values) RegDeleteKeyW(HKEY_CURRENT_USER, path);
}

static void linked(const Paths *paths, WCHAR *out) {
    GetPrivateProfileStringW(L"studio", L"links", L"", out, MAX_PATH, paths->state);
}

BOOL links_wanted(const Paths *paths) {
    WCHAR exe[MAX_PATH];

    linked(paths, exe);
    return exe[0] != 0;
}

BOOL links_set(const Paths *paths, BOOL on) {
    WCHAR exe[MAX_PATH], before[MAX_PATH], parked[160];
    BOOL ok = TRUE;
    size_t i;

    if (paths->custom) return TRUE;
    own_exe(exe);
    linked(paths, before);
    for (i = 0; i < sizeof(g_schemes) / sizeof(g_schemes[0]); i++) {
        BOOL current = links_to(g_schemes[i], exe), ours = current || links_to(g_schemes[i], before);

        wide_format(parked, 160, PARKED L"%ls", g_schemes[i]);
        if (!on) {
            if (ours || key_exists(parked)) give_back(g_schemes[i], ours);
        } else if (!current && take(g_schemes[i], exe, ours)) {
            ok = FALSE;
        }
    }
    drop_if_empty(L"Software\\Studio7\\Replaced links");
    drop_if_empty(L"Software\\Studio7");
    WritePrivateProfileStringW(L"studio", L"links", on ? exe : NULL, paths->state);
    return ok;
}

void links_refresh(const Paths *paths) {
    WCHAR exe[MAX_PATH], before[MAX_PATH];
    BOOL current;
    size_t i;

    if (paths->custom) return;
    linked(paths, before);
    if (!before[0]) return;
    own_exe(exe);
    current = !_wcsicmp(before, exe);
    for (i = 0; i < sizeof(g_schemes) / sizeof(g_schemes[0]); i++) current = current && links_to(g_schemes[i], exe);
    if (!current) links_set(paths, TRUE);
}

BOOL is_link(const WCHAR *text) {
    size_t i, length = wcslen(text);

    if (_wcsnicmp(text, L"roblox-studio:", 14) && _wcsnicmp(text, L"roblox-studio-auth:", 19)) return FALSE;
    if (length > 4000) return FALSE;
    for (i = 0; i < length; i++)
        if (text[i] <= L' ' || text[i] == L'"' || text[i] >= 0x7F) return FALSE;
    return TRUE;
}

BOOL webview_version(WCHAR *out, DWORD room) {
    static const WCHAR *const keys[] = {
        L"SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",
        L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",
    };
    static const HKEY roots[] = {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER};
    BOOL installed = wv2_system_usable();
    int root, key;

    for (root = 0; installed && root < 2; root++) {
        for (key = 0; key < 2; key++) {
            DWORD size = room * sizeof(WCHAR);

            if (!RegGetValueW(roots[root], keys[key], L"pv", RRF_RT_REG_SZ, NULL, out, &size) && out[0] &&
                wcscmp(out, L"0.0.0.0"))
                return TRUE;
        }
    }
    if (wv2_windows_major() < 10 && wv2_folder_complete() && room > wcslen(WV2_VERSION)) {
        wcscpy(out, WV2_VERSION);
        return TRUE;
    }
    out[0] = 0;
    return FALSE;
}

static BOOL own_webview(void) {
    WCHAR value[4];

    return GetEnvironmentVariableW(S7_WEBVIEW_FOLDER, value, 4) != 0;
}

BOOL webview_wanted(void) {
    return !own_webview() && wv2_needs_setup();
}

BOOL webview_lend(void) {
    WCHAR folder[MAX_PATH];

    if (own_webview() || wv2_windows_major() >= 10 || wv2_system_usable() || !wv2_folder_complete() ||
        !wv2_folder(folder, MAX_PATH))
        return FALSE;
    return SetEnvironmentVariableW(S7_WEBVIEW_FOLDER, folder);
}
