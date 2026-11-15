// update.h - self-update from the project's GitHub releases.
//
// The utility asks the releases API for the latest tag, compares it with its
// own version, downloads the published RazerLite.exe and swaps itself for it.
// A running executable cannot be overwritten, but it can be renamed, which is
// what makes the swap possible without a separate updater program.
//
// Everything here runs on its own thread: network calls must never sit in the
// message loop, and they must never sit in the HID loop either.

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#define UPDATE_OWNER   L"stepanovmaxim"
#define UPDATE_REPO    L"RazerLite"
#define UPDATE_ASSET   "RazerLite.exe"
#define UPDATE_API     L"https://api.github.com/repos/" UPDATE_OWNER L"/" UPDATE_REPO \
                       L"/releases/latest"

typedef struct {
    BOOL  available;            // a newer version is published
    int   major, minor, patch;
    WCHAR version[32];
    WCHAR assetUrl[640];
} UpdateInfo;

// ------------------------------------------------------------ tiny helpers

static int UpStrLenA(const char *s) {
    int n = 0;
    while (s[n]) ++n;
    return n;
}

// Substring search over a buffer that is not null terminated.
static const char *UpFind(const char *haystack, int length, const char *needle) {
    int n = UpStrLenA(needle);
    if (n <= 0 || length < n) return NULL;
    for (int i = 0; i <= length - n; ++i) {
        int k = 0;
        while (k < n && haystack[i + k] == needle[k]) ++k;
        if (k == n) return haystack + i;
    }
    return NULL;
}

// Reads the JSON string value that follows the given key.
static BOOL UpJsonString(const char *json, int length, const char *key,
                         char *out, int outSize) {
    const char *at = UpFind(json, length, key);
    if (!at) return FALSE;
    at += UpStrLenA(key);
    const char *end = json + length;
    while (at < end && (*at == ' ' || *at == ':' || *at == '"')) ++at;

    int n = 0;
    while (at < end && *at != '"' && n < outSize - 1) {
        if (*at == '\\' && at + 1 < end) ++at;        // url escapes, e.g. \/
        out[n++] = *at++;
    }
    out[n] = 0;
    return n > 0;
}

// "v1.2.3" or "1.2.3" into three numbers; anything missing counts as zero.
static void UpParseVersion(const char *text, int *major, int *minor, int *patch) {
    int part[3] = { 0, 0, 0 }, index = 0;
    const char *p = text;
    if (*p == 'v' || *p == 'V') ++p;
    for (; *p && index < 3; ++p) {
        if (*p >= '0' && *p <= '9') part[index] = part[index] * 10 + (*p - '0');
        else if (*p == '.') ++index;
        else break;
    }
    *major = part[0];
    *minor = part[1];
    *patch = part[2];
}

// ------------------------------------------------------------------ fetch

// One GET, redirects followed, body returned in a heap buffer the caller frees.
static char *UpHttpGet(const WCHAR *url, DWORD maxBytes, DWORD *sizeOut) {
    static WCHAR host[256], path[1024];           // static: no 4 KB stack frames
    URL_COMPONENTS parts;
    ZeroMemory(&parts, sizeof(parts));
    parts.dwStructSize     = sizeof(parts);
    parts.lpszHostName     = host;
    parts.dwHostNameLength = ARRAYSIZE(host);
    parts.lpszUrlPath      = path;
    parts.dwUrlPathLength  = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(url, 0, 0, &parts)) return NULL;

    HINTERNET session = WinHttpOpen(L"RazerLite", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return NULL;
    WinHttpSetTimeouts(session, 10000, 10000, 20000, 30000);

    char *buffer = NULL;
    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    if (connection) {
        DWORD flags = (parts.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET request = WinHttpOpenRequest(connection, L"GET", path, NULL,
                                               WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (request) {
            // GitHub turns away requests that carry no user agent.
            WinHttpAddRequestHeaders(request,
                L"User-Agent: RazerLite\r\nAccept: application/vnd.github+json\r\n",
                (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

            if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(request, NULL)) {

                DWORD status = 0, statusSize = sizeof(status);
                WinHttpQueryHeaders(request,
                                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                    WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    DWORD capacity = 64 * 1024, used = 0;
                    buffer = (char *)HeapAlloc(GetProcessHeap(), 0, capacity);
                    while (buffer) {
                        DWORD chunk = 0;
                        if (!WinHttpReadData(request, buffer + used,
                                             capacity - used, &chunk) || chunk == 0)
                            break;
                        used += chunk;
                        if (used > maxBytes) { used = 0; break; }
                        if (capacity - used < 8192) {
                            DWORD grown = capacity * 2;
                            char *bigger = (char *)HeapReAlloc(GetProcessHeap(), 0,
                                                               buffer, grown);
                            if (!bigger) break;
                            buffer = bigger;
                            capacity = grown;
                        }
                    }
                    if (buffer && used == 0) {
                        HeapFree(GetProcessHeap(), 0, buffer);
                        buffer = NULL;
                    } else if (buffer && sizeOut) {
                        *sizeOut = used;
                    }
                }
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connection);
    }
    WinHttpCloseHandle(session);
    return buffer;
}

// Asks GitHub for the newest release and reports whether it beats ours.
static BOOL UpdateQuery(int ourMajor, int ourMinor, int ourPatch, UpdateInfo *out) {
    ZeroMemory(out, sizeof(*out));

    DWORD size = 0;
    char *json = UpHttpGet(UPDATE_API, 1024 * 1024, &size);
    if (!json) return FALSE;

    static char tag[64], url[640];
    BOOL ok = UpJsonString(json, (int)size, "\"tag_name\"", tag, sizeof(tag));
    if (ok) {
        UpParseVersion(tag, &out->major, &out->minor, &out->patch);
        MultiByteToWideChar(CP_UTF8, 0, tag, -1, out->version, ARRAYSIZE(out->version));

        // Take the download url belonging to our own asset: find the file name
        // in the asset list, then read the browser_download_url after it.
        const char *asset = UpFind(json, (int)size, "\"name\":\"" UPDATE_ASSET "\"");
        const char *search = asset ? asset : json;
        int remaining = (int)size - (int)(search - json);
        if (!UpJsonString(search, remaining, "\"browser_download_url\"", url, sizeof(url)))
            ok = FALSE;
        else
            MultiByteToWideChar(CP_UTF8, 0, url, -1, out->assetUrl,
                                ARRAYSIZE(out->assetUrl));
    }
    HeapFree(GetProcessHeap(), 0, json);
    if (!ok) return FALSE;

    out->available = (out->major > ourMajor) ||
                     (out->major == ourMajor && out->minor > ourMinor) ||
                     (out->major == ourMajor && out->minor == ourMinor &&
                      out->patch > ourPatch);
    return TRUE;
}

// --------------------------------------------------------------- download

// Fetches the asset into `destPath`. Refuses anything that is not a Windows
// executable, so an error page never ends up being launched.
static BOOL UpdateDownload(const WCHAR *url, const WCHAR *destPath) {
    DWORD size = 0;
    char *data = UpHttpGet(url, 16 * 1024 * 1024, &size);
    if (!data) return FALSE;

    BOOL ok = (size > 16 * 1024 && data[0] == 'M' && data[1] == 'Z');
    if (ok) {
        HANDLE file = CreateFileW(destPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, NULL);
        ok = (file != INVALID_HANDLE_VALUE);
        if (ok) {
            DWORD written = 0;
            ok = WriteFile(file, data, size, &written, NULL) && written == size;
            CloseHandle(file);
            if (!ok) DeleteFileW(destPath);
        }
    }
    HeapFree(GetProcessHeap(), 0, data);
    return ok;
}

// ------------------------------------------------------------------ swap

// Moves the new build into place and starts it. The running image cannot be
// overwritten, but Windows does allow renaming it, so the old build steps
// aside as RazerLite.exe.old and the replacement takes its path - which keeps
// the Run key, any shortcuts and the single-instance mutex working unchanged.
//
// The new process is told which process to wait for, because the old one still
// holds that mutex for the moment it takes to exit.
static BOOL UpdateApply(const WCHAR *stagedExe) {
    static WCHAR current[MAX_PATH], retired[MAX_PATH + 8], args[MAX_PATH + 64];
    DWORD len = GetModuleFileNameW(NULL, current, MAX_PATH);
    if (!len || len >= MAX_PATH) return FALSE;

    lstrcpyW(retired, current);
    lstrcatW(retired, L".old");
    DeleteFileW(retired);                               // left over from last time
    if (!MoveFileExW(current, retired, MOVEFILE_REPLACE_EXISTING)) return FALSE;

    if (!CopyFileW(stagedExe, current, FALSE)) {
        MoveFileExW(retired, current, MOVEFILE_REPLACE_EXISTING);   // put it back
        return FALSE;
    }
    DeleteFileW(stagedExe);

    wsprintfW(args, L"\"%s\" --updated=%u", current, GetCurrentProcessId());

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    if (!CreateProcessW(current, args, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        MoveFileExW(retired, current, MOVEFILE_REPLACE_EXISTING);
        return FALSE;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

// Called at startup: clears the previous build once it is no longer in use.
static void UpdateCleanup(void) {
    WCHAR current[MAX_PATH], retired[MAX_PATH + 8];
    DWORD len = GetModuleFileNameW(NULL, current, MAX_PATH);
    if (!len || len >= MAX_PATH) return;
    lstrcpyW(retired, current);
    lstrcatW(retired, L".old");
    DeleteFileW(retired);
}
