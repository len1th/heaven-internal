#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <fstream>
#include <thread>
#include <chrono>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")

namespace {

constexpr wchar_t kUpdateHost[] = L"raw.githubusercontent.com";
constexpr INTERNET_PORT kUpdatePort = 443;
constexpr wchar_t kManifestPath[] = L"/len1th/heaven-internal/main/update.json";
constexpr wchar_t kDllPath[] = L"/len1th/heaven-internal/main/HeavenInternal.dll";

struct Reply {
    DWORD status = 0;
    std::string body;
};

std::wstring GetExeDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(path, L'\\');
    if (lastSlash) {
        *lastSlash = L'\0';
    }
    return std::wstring(path);
}

std::string ExtractJsonField(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return "";

    size_t colonPos = json.find(':', keyPos);
    if (colonPos == std::string::npos) return "";

    size_t valueStart = json.find_first_not_of(" \t\n\r", colonPos + 1);
    if (valueStart == std::string::npos) return "";

    if (json[valueStart] == '\"') {
        size_t valueEnd = json.find('\"', valueStart + 1);
        if (valueEnd != std::string::npos) {
            return json.substr(valueStart + 1, valueEnd - valueStart - 1);
        }
    } else {
        size_t valueEnd = json.find_first_of(",}\n\r", valueStart);
        if (valueEnd != std::string::npos) {
            return json.substr(valueStart, valueEnd - valueStart);
        }
    }
    return "";
}

Reply HttpGet(const std::wstring& path) {
    Reply out;
    HINTERNET session = WinHttpOpen(L"HeavenLauncher/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return out;

    WinHttpSetTimeouts(session, 5000, 5000, 10000, 10000);
    HINTERNET connect = WinHttpConnect(session, kUpdateHost, kUpdatePort, 0);
    if (!connect) { WinHttpCloseHandle(session); return out; }

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!request) { WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return out; }

    DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                     SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));

    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0)
        && WinHttpReceiveResponse(request, nullptr)) {
        DWORD size = sizeof(out.status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &out.status, &size, WINHTTP_NO_HEADER_INDEX);
        for (;;) {
            DWORD available = 0, read = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || !available) break;
            std::vector<char> buffer(available);
            if (!WinHttpReadData(request, buffer.data(), available, &read) || !read) break;
            out.body.append(buffer.data(), read);
        }
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return out;
}

bool ComputeSha256(const std::wstring& filePath, std::string& result) {
    std::ifstream input(filePath, std::ios::binary);
    if (!input) return false;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, resultSize = 0;
    bool ok = (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) &&
              (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &resultSize, 0) == 0);
    std::vector<UCHAR> object(objectSize), digest(32);
    if (ok) ok = (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) == 0);

    std::vector<char> buffer(64 * 1024);
    while (ok && input) {
        input.read(buffer.data(), buffer.size());
        auto n = input.gcount();
        if (n > 0) {
            ok = (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(n), 0) == 0);
        }
    }
    if (ok) ok = (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0);
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return false;

    static constexpr char hex[] = "0123456789abcdef";
    result.clear();
    for (UCHAR b : digest) {
        result += hex[b >> 4];
        result += hex[b & 15];
    }
    return true;
}

struct LauncherUI {
    HWND hWnd = nullptr;
    HFONT hFontTitle = nullptr;
    HFONT hFontBold = nullptr;
    HFONT hFontRegular = nullptr;
    HBRUSH hBgBrush = nullptr;
    HBRUSH hCardBrush = nullptr;
    HBRUSH hAccentBrush = nullptr;
    std::wstring statusText = L"Guncellemeler denetleniyor...";
    COLORREF statusColor = RGB(160, 160, 200);
    float progress = 0.15f;
};

LauncherUI g_ui;

void UpdateUIStatus(const std::wstring& text, COLORREF color, float progress) {
    g_ui.statusText = text;
    g_ui.statusColor = color;
    g_ui.progress = progress;
    if (g_ui.hWnd) {
        InvalidateRect(g_ui.hWnd, nullptr, FALSE);
        UpdateWindow(g_ui.hWnd);
    }
}

void LaunchTarget() {
    std::wstring dir = GetExeDir();
    
    // Look for shortcut first
    std::wstring lnk1 = dir + L"\\HeavenİnternalBaslat.lnk";
    std::wstring lnk2 = dir + L"\\HeavenInternalBaslat.lnk";
    
    std::wstring chosenLnk;
    if (GetFileAttributesW(lnk1.c_str()) != INVALID_FILE_ATTRIBUTES) {
        chosenLnk = lnk1;
    } else if (GetFileAttributesW(lnk2.c_str()) != INVALID_FILE_ATTRIBUTES) {
        chosenLnk = lnk2;
    }

    if (!chosenLnk.empty()) {
        HINSTANCE hInst = ShellExecuteW(nullptr, L"open", chosenLnk.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(hInst) > 32) {
            return;
        }
    }

    // Direct fallback: Injector-x64.exe lenith
    std::wstring injectorPath = dir + L"\\Injector-x64.exe";
    if (GetFileAttributesW(injectorPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        ShellExecuteW(nullptr, L"open", injectorPath.c_str(), L"lenith", dir.c_str(), SW_SHOWNORMAL);
    }
}

void WorkerThread() {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::wstring dir = GetExeDir();
    std::wstring dllTarget = dir + L"\\HeavenInternal.dll";
    std::wstring dllTemp = dir + L"\\HeavenInternal.dll.tmp";

    UpdateUIStatus(L"Guncellemeler denetleniyor...", RGB(160, 160, 220), 0.30f);

    Reply manifest = HttpGet(kManifestPath);
    if (manifest.status == 200 && !manifest.body.empty()) {
        std::string remoteVer = ExtractJsonField(manifest.body, "version");
        std::string remoteSha = ExtractJsonField(manifest.body, "sha256");
        std::string remoteUrl = ExtractJsonField(manifest.body, "url");

        if (!remoteSha.empty() && remoteSha.size() == 64) {
            std::string localSha;
            bool hasLocal = ComputeSha256(dllTarget, localSha);
            bool needDownload = !hasLocal || (_stricmp(localSha.c_str(), remoteSha.c_str()) != 0);

            if (needDownload) {
                std::wstring statusMsg = L"Yeni guncelleme indiriliyor (v" + std::wstring(remoteVer.begin(), remoteVer.end()) + L")...";
                UpdateUIStatus(statusMsg, RGB(254, 202, 87), 0.60f);

                std::wstring downloadPath = remoteUrl.empty() ? kDllPath : std::wstring(remoteUrl.begin(), remoteUrl.end());
                Reply dllReply = HttpGet(downloadPath);

                if (dllReply.status == 200 && !dllReply.body.empty()) {
                    std::ofstream out(dllTemp, std::ios::binary | std::ios::trunc);
                    if (out) {
                        out.write(dllReply.body.data(), static_cast<std::streamsize>(dllReply.body.size()));
                        out.close();

                        std::string downloadedSha;
                        if (ComputeSha256(dllTemp, downloadedSha) && _stricmp(downloadedSha.c_str(), remoteSha.c_str()) == 0) {
                            MoveFileExW(dllTemp.c_str(), dllTarget.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
                            UpdateUIStatus(L"Guncelleme tamamlandi! (v" + std::wstring(remoteVer.begin(), remoteVer.end()) + L")", RGB(46, 213, 115), 0.95f);
                        } else {
                            DeleteFileW(dllTemp.c_str());
                            UpdateUIStatus(L"Dogrulama hatasi! Mevcut surumle baslatiliyor...", RGB(255, 71, 87), 0.80f);
                        }
                    }
                } else {
                    UpdateUIStatus(L"Indirme basarisiz, mevcut surumle baslatiliyor...", RGB(255, 71, 87), 0.80f);
                }
            } else {
                UpdateUIStatus(L"Sisteminiz guncel (v" + std::wstring(remoteVer.begin(), remoteVer.end()) + L")", RGB(46, 213, 115), 0.95f);
            }
        } else {
            UpdateUIStatus(L"Guncelleme bilgisi alinamadi, baslatiliyor...", RGB(254, 202, 87), 0.80f);
        }
    } else {
        UpdateUIStatus(L"Baglanti saglanamadi, mevcut surumle baslatiliyor...", RGB(200, 200, 210), 0.80f);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    UpdateUIStatus(L"Heaven baslatiliyor...", RGB(162, 140, 255), 1.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    LaunchTarget();

    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    if (g_ui.hWnd) {
        PostMessageW(g_ui.hWnd, WM_CLOSE, 0, 0);
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE: {
        g_ui.hWnd = hWnd;
        g_ui.hFontTitle   = CreateFontW(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_ui.hFontBold    = CreateFontW(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_ui.hFontRegular = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

        g_ui.hBgBrush     = CreateSolidBrush(RGB(17, 18, 24));      // #111218 Koyu Arka Plan
        g_ui.hCardBrush   = CreateSolidBrush(RGB(24, 25, 34));      // #181922 Kart Arka Plan
        g_ui.hAccentBrush = CreateSolidBrush(RGB(108, 92, 231));    // #6C5CE7 Vurgu Moru

        std::thread(WorkerThread).detach();
        return 0;
    }

    case WM_NCHITTEST: {
        LRESULT hit = DefWindowProcW(hWnd, uMsg, wParam, lParam);
        if (hit == HTCLIENT) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hWnd, &pt);
            if (pt.y < 45) return HTCAPTION;
        }
        return hit;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT clientRc;
        GetClientRect(hWnd, &clientRc);

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBitmap = CreateCompatibleBitmap(hdc, clientRc.right, clientRc.bottom);
        HGDIOBJ oldBitmap = SelectObject(memDC, memBitmap);

        // 1. Ana Arka Plan
        FillRect(memDC, &clientRc, g_ui.hBgBrush);

        // 2. Dis Cerceve (Border)
        HPEN hBorderPen = CreatePen(PS_SOLID, 1, RGB(50, 52, 70));
        HGDIOBJ oldPen = SelectObject(memDC, hBorderPen);
        SelectObject(memDC, GetStockObject(NULL_BRUSH));
        Rectangle(memDC, 0, 0, clientRc.right, clientRc.bottom);
        SelectObject(memDC, oldPen);
        DeleteObject(hBorderPen);

        // 3. Ust Baslik Seridi
        RECT headerRc = { 0, 0, clientRc.right, 45 };
        HBRUSH hHeaderBrush = CreateSolidBrush(RGB(22, 24, 33));
        FillRect(memDC, &headerRc, hHeaderBrush);
        DeleteObject(hHeaderBrush);

        // Ust Vurgu Moru
        RECT accentLine = { 0, 44, clientRc.right, 46 };
        FillRect(memDC, &accentLine, g_ui.hAccentBrush);

        // Baslik Metni
        SetBkMode(memDC, TRANSPARENT);
        SelectObject(memDC, g_ui.hFontTitle);
        SetTextColor(memDC, RGB(162, 140, 255));
        TextOutW(memDC, 20, 12, L"HEAVEN", 6);

        SelectObject(memDC, g_ui.hFontRegular);
        SetTextColor(memDC, RGB(140, 140, 160));
        TextOutW(memDC, 104, 16, L"|  Launcher & Otomatik Guncelleyici", 35);

        // 4. Durum Kutusu
        RECT cardRc = { 20, 62, clientRc.right - 20, 148 };
        HBRUSH hCard = CreateSolidBrush(RGB(22, 23, 31));
        HPEN hCardPen = CreatePen(PS_SOLID, 1, RGB(40, 42, 56));
        SelectObject(memDC, hCard);
        SelectObject(memDC, hCardPen);
        RoundRect(memDC, cardRc.left, cardRc.top, cardRc.right, cardRc.bottom, 6, 6);
        DeleteObject(hCard);
        DeleteObject(hCardPen);

        // Durum Metni
        SelectObject(memDC, g_ui.hFontBold);
        SetTextColor(memDC, g_ui.statusColor);
        RECT textRc = { 32, 76, clientRc.right - 32, 106 };
        DrawTextW(memDC, g_ui.statusText.c_str(), -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // 5. Ilerleme Cubugu (Progress Bar)
        RECT barBgRc = { 32, 114, clientRc.right - 32, 126 };
        HBRUSH hBarBg = CreateSolidBrush(RGB(32, 34, 45));
        FillRect(memDC, &barBgRc, hBarBg);
        DeleteObject(hBarBg);

        int barWidth = static_cast<int>((barBgRc.right - barBgRc.left) * g_ui.progress);
        if (barWidth > 0) {
            RECT barFillRc = { barBgRc.left, barBgRc.top, barBgRc.left + barWidth, barBgRc.bottom };
            FillRect(memDC, &barFillRc, g_ui.hAccentBrush);
        }

        BitBlt(hdc, 0, 0, clientRc.right, clientRc.bottom, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBitmap);
        DeleteObject(memBitmap);
        DeleteDC(memDC);

        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_DESTROY: {
        if (g_ui.hFontTitle)   DeleteObject(g_ui.hFontTitle);
        if (g_ui.hFontBold)    DeleteObject(g_ui.hFontBold);
        if (g_ui.hFontRegular) DeleteObject(g_ui.hFontRegular);
        if (g_ui.hBgBrush)     DeleteObject(g_ui.hBgBrush);
        if (g_ui.hCardBrush)   DeleteObject(g_ui.hCardBrush);
        if (g_ui.hAccentBrush) DeleteObject(g_ui.hAccentBrush);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"HeavenLauncherWnd";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    int width = 460;
    int height = 168;
    int screenX = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int screenY = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    HWND hWnd = CreateWindowExW(
        WS_EX_TOPMOST,
        wc.lpszClassName,
        L"Heaven Launcher",
        WS_POPUP | WS_VISIBLE,
        screenX, screenY, width, height,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hWnd) return 1;

    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterClassW(wc.lpszClassName, hInstance);
    return 0;
}
