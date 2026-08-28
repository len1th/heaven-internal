#pragma once
#include <windows.h>
#include <windowsx.h>
#include <winhttp.h>
#include <string>
#include <sstream>
#include <fstream>
#include <iostream>
#include "hwid.hpp"

#pragma comment(lib, "winhttp.lib")

namespace Auth {
    // ------------------------------------------------------------------------
    // SUNUCU VE ENDPOINT YAPILANDIRMASI
    // ------------------------------------------------------------------------
    inline std::wstring SERVER_HOST    = L"heavenex.com.tr";                 // Domain veya IP: L"45.143.11.4"
    inline INTERNET_PORT SERVER_PORT   = 443;                                // HTTPS portu
    inline std::wstring SERVER_PATH    = L"/api/launcher/internal/verify";   // Lisans doğrulama endpoint'i
    inline bool USE_HTTPS              = true;                               // SSL aktif
    inline bool IGNORE_SSL_ERRORS      = true;                               // Test / Self-signed sertifikalar için

    // ------------------------------------------------------------------------
    // YANIT VE VERİ YAPILARI
    // ------------------------------------------------------------------------
    struct Response {
        bool success = false;
        std::string message = "";
        std::string expiry = "";
        std::string rawResponse = "";
    };

    // ------------------------------------------------------------------------
    // BASİT JSON AYIKLAYICI
    // ------------------------------------------------------------------------
    inline std::string ExtractJsonField(const std::string& json, const std::string& key) {
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

    // ------------------------------------------------------------------------
    // LİSANS DOSYASI YOLLARI VE İŞLEMLERİ
    // ------------------------------------------------------------------------
    inline std::string GetAppDataKeyPath() {
        char appData[MAX_PATH] = { 0 };
        if (GetEnvironmentVariableA("APPDATA", appData, MAX_PATH)) {
            std::string dir = std::string(appData) + "\\Heaven";
            CreateDirectoryA(dir.c_str(), nullptr);
            return dir + "\\license.key";
        }
        return "license.key";
    }

    inline std::string ReadSavedKeyFromFile(const std::string& filepath) {
        std::ifstream file(filepath);
        if (file.is_open()) {
            std::string key;
            std::getline(file, key);
            size_t first = key.find_first_not_of(" \t\r\n");
            size_t last = key.find_last_not_of(" \t\r\n");
            if (first != std::string::npos && last != std::string::npos) {
                return key.substr(first, (last - first + 1));
            }
        }
        return "";
    }

    inline std::string ReadSavedKey() {
        std::string key = ReadSavedKeyFromFile(GetAppDataKeyPath());
        if (!key.empty()) return key;
        return ReadSavedKeyFromFile("license.key");
    }

    inline bool SaveKeyToFile(const std::string& key, const std::string& filepath) {
        std::ofstream file(filepath, std::ios::trunc);
        if (file.is_open()) {
            file << key;
            return true;
        }
        return false;
    }

    inline void SaveKey(const std::string& key) {
        SaveKeyToFile(key, GetAppDataKeyPath());
        SaveKeyToFile(key, "license.key");
    }

    inline void DeleteSavedKey() {
        std::string p1 = GetAppDataKeyPath();
        DeleteFileA(p1.c_str());
        DeleteFileA("license.key");
    }

    inline void CopyToClipboard(const std::string& text) {
        if (OpenClipboard(nullptr)) {
            EmptyClipboard();
            HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
            if (hGlob) {
                char* pMem = static_cast<char*>(GlobalLock(hGlob));
                if (pMem) {
                    memcpy(pMem, text.c_str(), text.size() + 1);
                    GlobalUnlock(hGlob);
                    SetClipboardData(CF_TEXT, hGlob);
                }
            }
            CloseClipboard();
        }
    }

    // ------------------------------------------------------------------------
    // WINHTTP İLE POST İSTEĞİ VE DOĞRULAMA
    // ------------------------------------------------------------------------
    inline Response SendRequest(const std::string& licenseKey) {
        Response resp;
        std::string hwid = HWID::GetHWID();

        std::ostringstream jsonStream;
        jsonStream << "{\"key\":\"" << licenseKey << "\",\"hwid\":\"" << hwid << "\"}";
        std::string payload = jsonStream.str();

        HINTERNET hSession = WinHttpOpen(
            L"HeavenAuthClient/2.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0
        );

        if (!hSession) {
            resp.message = "WinHTTP oturumu baslatilamadi. Hata: " + std::to_string(GetLastError());
            return resp;
        }

        WinHttpSetTimeouts(hSession, 6000, 6000, 6000, 6000);

        HINTERNET hConnect = WinHttpConnect(hSession, SERVER_HOST.c_str(), SERVER_PORT, 0);
        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            resp.message = "Sunucuya baglanilamadi. Hata: " + std::to_string(GetLastError());
            return resp;
        }

        DWORD requestFlags = USE_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(
            hConnect,
            L"POST",
            SERVER_PATH.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            requestFlags
        );

        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            resp.message = "HTTP istegi hazirlanamadi. Hata: " + std::to_string(GetLastError());
            return resp;
        }

        if (USE_HTTPS && IGNORE_SSL_ERRORS) {
            DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                             SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                             SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                             SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
        }

        LPCWSTR headers = L"Content-Type: application/json\r\nAccept: application/json\r\n";
        BOOL bResults = WinHttpSendRequest(
            hRequest,
            headers,
            (DWORD)-1L,
            (LPVOID)payload.c_str(),
            (DWORD)payload.length(),
            (DWORD)payload.length(),
            0
        );

        if (bResults) {
            bResults = WinHttpReceiveResponse(hRequest, nullptr);
        }

        if (bResults) {
            DWORD bytesToRead = 0;
            DWORD bytesRead = 0;
            std::string fullResponse = "";

            do {
                bytesToRead = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &bytesToRead)) break;
                if (bytesToRead == 0) break;

                char* buffer = new char[bytesToRead + 1];
                ZeroMemory(buffer, bytesToRead + 1);

                if (WinHttpReadData(hRequest, buffer, bytesToRead, &bytesRead)) {
                    fullResponse.append(buffer, bytesRead);
                }
                delete[] buffer;
            } while (bytesToRead > 0);

            resp.rawResponse = fullResponse;

            DWORD statusCode = 0;
            DWORD statusCodeSize = sizeof(statusCode);
            WinHttpQueryHeaders(
                hRequest,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &statusCode,
                &statusCodeSize,
                WINHTTP_NO_HEADER_INDEX
            );

            if (statusCode == 502) {
                resp.success = false;
                resp.message = "Sunucu Hatasi: 502 Bad Gateway (Backend API servisi kapali veya yanit vermiyor)";
            } else if (statusCode == 500) {
                resp.success = false;
                resp.message = "Sunucu Hatasi: 500 Dahili Sunucu Hatasi";
            } else if (statusCode == 404) {
                resp.success = false;
                resp.message = "Sunucu Hatasi: 404 Sayfa Bulunamadi (Endpoint yolu hatali)";
            } else {
                std::string successVal = ExtractJsonField(fullResponse, "success");
                resp.success = (successVal == "true" || successVal == "1");
                resp.message = ExtractJsonField(fullResponse, "message");
                resp.expiry  = ExtractJsonField(fullResponse, "expiry");

                if (!resp.success && resp.message.empty()) {
                    resp.message = "Gecersiz Lisans Anahtari veya HWID uyusmazligi! (HTTP " + std::to_string(statusCode) + ")";
                }
            }
        } else {
            resp.message = "Sunucudan yanit alinamadi. Hata Kodu: " + std::to_string(GetLastError());
        }

        if (hRequest) WinHttpCloseHandle(hRequest);
        if (hConnect) WinHttpCloseHandle(hConnect);
        if (hSession) WinHttpCloseHandle(hSession);

        return resp;
    }

    // ------------------------------------------------------------------------
    // MODERN DARK-THEME WIN32 GUI DIALOG (TURKCE)
    // ------------------------------------------------------------------------
    struct ModernUIData {
        std::string enteredKey;
        bool confirmed = false;
        HWND hEdit = nullptr;
        HWND hStatus = nullptr;
        HWND hHwidEdit = nullptr;
        HWND chkRemember = nullptr;
        HWND btnVerify = nullptr;
        HWND btnCopy = nullptr;
        HWND btnClose = nullptr;
        HWND btnCancel = nullptr;
        HFONT hFontTitle = nullptr;
        HFONT hFontBold = nullptr;
        HFONT hFontRegular = nullptr;
        HFONT hFontMono = nullptr;
        HBRUSH hBgBrush = nullptr;
        HBRUSH hCardBrush = nullptr;
        HBRUSH hEditBrush = nullptr;
        HBRUSH hAccentBrush = nullptr;
        std::wstring statusText = L"Lisans anahtarinizi girip 'Giris Yap' butonuna basiniz.";
        COLORREF statusColor = RGB(160, 160, 180);
        bool isChecking = false;
        Response lastResponse;
    };

    constexpr int IDC_AUTH_KEY_EDIT     = 2001;
    constexpr int IDC_AUTH_HWID_EDIT    = 2002;
    constexpr int IDC_AUTH_BTN_VERIFY   = 2003;
    constexpr int IDC_AUTH_BTN_COPY     = 2004;
    constexpr int IDC_AUTH_BTN_CANCEL   = 2005;
    constexpr int IDC_AUTH_BTN_CLOSE    = 2006;
    constexpr int IDC_AUTH_CHK_REMEMBER = 2007;

    inline void DrawCustomButton(LPDRAWITEMSTRUCT dis, COLORREF bgColor, COLORREF hoverColor, COLORREF textColor, HFONT hFont, const wchar_t* text, bool rounded = true) {
        HDC hdc = dis->hDC;
        RECT rc = dis->rcItem;
        bool isPressed = (dis->itemState & ODS_SELECTED);
        bool isDisabled = (dis->itemState & ODS_DISABLED);

        COLORREF fillCol = isDisabled ? RGB(45, 45, 55) : (isPressed ? RGB(70, 50, 140) : bgColor);

        HBRUSH hBrush = CreateSolidBrush(fillCol);
        HPEN hPen = CreatePen(PS_SOLID, 1, isDisabled ? RGB(60, 60, 70) : RGB(90, 75, 170));
        HGDIOBJ oldBrush = SelectObject(hdc, hBrush);
        HGDIOBJ oldPen = SelectObject(hdc, hPen);

        if (rounded) {
            RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 6, 6);
        } else {
            Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
        }

        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(hBrush);
        DeleteObject(hPen);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, isDisabled ? RGB(120, 120, 130) : textColor);
        HGDIOBJ oldFont = SelectObject(hdc, hFont);
        DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
    }

    inline LRESULT CALLBACK ModernUIMessageProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        ModernUIData* data = reinterpret_cast<ModernUIData*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

        switch (uMsg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            data = reinterpret_cast<ModernUIData*>(cs->lpCreateParams);
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));

            // Modern Fontlar
            data->hFontTitle   = CreateFontW(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            data->hFontBold    = CreateFontW(15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            data->hFontRegular = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            data->hFontMono    = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Consolas");

            // Renk Fircalari
            data->hBgBrush     = CreateSolidBrush(RGB(17, 18, 24));      // #111218 Koyu Arka Plan
            data->hCardBrush   = CreateSolidBrush(RGB(24, 25, 34));      // #181922 Kart Arka Plan
            data->hEditBrush   = CreateSolidBrush(RGB(28, 29, 40));      // #1C1D28 Input Arka Plan
            data->hAccentBrush = CreateSolidBrush(RGB(108, 92, 231));    // #6C5CE7 Vurgu Moru

            // Pencere Kapatma 'X' Butonu (Sag ust)
            data->btnClose = CreateWindowExW(0, L"BUTTON", L"X",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                444, 10, 26, 24, hWnd, (HMENU)(UINT_PTR)IDC_AUTH_BTN_CLOSE, nullptr, nullptr);

            // HWID Kutusu (ReadOnly)
            std::string hwid = HWID::GetHWID();
            std::wstring hwidW(hwid.begin(), hwid.end());
            data->hHwidEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", hwidW.c_str(),
                WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
                24, 76, 336, 26, hWnd, (HMENU)(UINT_PTR)IDC_AUTH_HWID_EDIT, nullptr, nullptr);
            SendMessageW(data->hHwidEdit, WM_SETFONT, (WPARAM)data->hFontMono, TRUE);

            // HWID Kopyala Butonu
            data->btnCopy = CreateWindowExW(0, L"BUTTON", L"Kopyala",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                368, 76, 88, 26, hWnd, (HMENU)(UINT_PTR)IDC_AUTH_BTN_COPY, nullptr, nullptr);

            // Lisans Key Input Kutusu
            data->hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                24, 134, 432, 28, hWnd, (HMENU)(UINT_PTR)IDC_AUTH_KEY_EDIT, nullptr, nullptr);
            SendMessageW(data->hEdit, WM_SETFONT, (WPARAM)data->hFontBold, TRUE);

            // Beni Hatirla Checkbox
            data->chkRemember = CreateWindowExW(0, L"BUTTON", L" Beni Hatirla",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                24, 168, 140, 20, hWnd, (HMENU)(UINT_PTR)IDC_AUTH_CHK_REMEMBER, nullptr, nullptr);
            SendMessageW(data->chkRemember, WM_SETFONT, (WPARAM)data->hFontRegular, TRUE);

            // Kaydedilmis anahtar varsa getir ve Beni Hatirla'yi secili yap
            std::string savedKey = ReadSavedKey();
            if (!savedKey.empty()) {
                std::wstring savedKeyW(savedKey.begin(), savedKey.end());
                SetWindowTextW(data->hEdit, savedKeyW.c_str());
                SendMessageW(data->chkRemember, BM_SETCHECK, BST_CHECKED, 0);
            } else {
                SendMessageW(data->chkRemember, BM_SETCHECK, BST_CHECKED, 0);
            }

            // Dogrula & Giris Butonu
            data->btnVerify = CreateWindowExW(0, L"BUTTON", L"Giris Yap",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                24, 252, 308, 38, hWnd, (HMENU)(UINT_PTR)IDC_AUTH_BTN_VERIFY, nullptr, nullptr);

            // Iptal Butonu
            data->btnCancel = CreateWindowExW(0, L"BUTTON", L"Iptal",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                348, 252, 108, 38, hWnd, (HMENU)(UINT_PTR)IDC_AUTH_BTN_CANCEL, nullptr, nullptr);

            SetFocus(data->hEdit);
            return 0;
        }

        case WM_NCHITTEST: {
            // Pencerenin ust 45px baslik kismindan suruklenmesini sagla
            LRESULT hit = DefWindowProcW(hWnd, uMsg, wParam, lParam);
            if (hit == HTCLIENT) {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hWnd, &pt);
                if (pt.y < 45 && pt.x < 430) {
                    return HTCAPTION;
                }
            }
            return hit;
        }

        case WM_ERASEBKGND:
            return 1; // Flicker onleme

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            RECT clientRc;
            GetClientRect(hWnd, &clientRc);

            // Double Buffering (Cift Tamponlama)
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBitmap = CreateCompatibleBitmap(hdc, clientRc.right, clientRc.bottom);
            HGDIOBJ oldBitmap = SelectObject(memDC, memBitmap);

            // 1. Ana Arka Plan
            FillRect(memDC, &clientRc, data->hBgBrush);

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

            // Ust Vurgu Cizgisi (Mor Serit)
            RECT accentLine = { 0, 44, clientRc.right, 46 };
            FillRect(memDC, &accentLine, data->hAccentBrush);

            // Baslik Metni
            SetBkMode(memDC, TRANSPARENT);
            SelectObject(memDC, data->hFontTitle);
            SetTextColor(memDC, RGB(162, 140, 255)); // Canli Mor
            TextOutW(memDC, 20, 12, L"HEAVEN", 6);

            SelectObject(memDC, data->hFontRegular);
            SetTextColor(memDC, RGB(140, 140, 160));
            TextOutW(memDC, 104, 16, L"|  Lisans Dogrulama", 19);

            // 4. HWID Bolumu Basligi
            SelectObject(memDC, data->hFontBold);
            SetTextColor(memDC, RGB(180, 180, 200));
            TextOutW(memDC, 24, 56, L"SISTEM DONANIM KIMLIGI (HWID)", 29);

            // 5. Lisans Key Bolumu Basligi
            TextOutW(memDC, 24, 114, L"LISANS ANAHTARI (KEY)", 21);

            // 6. Durum / Hata Mesaj Alani
            RECT statusRc = { 24, 192, 456, 242 };
            HBRUSH hStatusCard = CreateSolidBrush(RGB(22, 23, 31));
            HPEN hStatusPen = CreatePen(PS_SOLID, 1, RGB(40, 42, 56));
            SelectObject(memDC, hStatusCard);
            SelectObject(memDC, hStatusPen);
            RoundRect(memDC, statusRc.left, statusRc.top, statusRc.right, statusRc.bottom, 6, 6);
            DeleteObject(hStatusCard);
            DeleteObject(hStatusPen);

            SelectObject(memDC, data->hFontRegular);
            SetTextColor(memDC, data->statusColor);
            RECT statusTextRc = { 34, 196, 446, 238 };
            DrawTextW(memDC, data->statusText.c_str(), -1, &statusTextRc, DT_LEFT | DT_WORDBREAK);

            // Ekrana aktar
            BitBlt(hdc, 0, 0, clientRc.right, clientRc.bottom, memDC, 0, 0, SRCCOPY);

            SelectObject(memDC, oldBitmap);
            DeleteObject(memBitmap);
            DeleteDC(memDC);

            EndPaint(hWnd, &ps);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            HWND hCtrl = (HWND)lParam;
            if (data && hCtrl == data->chkRemember) {
                SetTextColor(hdc, RGB(200, 200, 220));
                SetBkColor(hdc, RGB(17, 18, 24));
                return (LRESULT)data->hBgBrush;
            }
            break;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam;
            HWND hCtrl = (HWND)lParam;
            if (hCtrl == data->hHwidEdit) {
                SetTextColor(hdc, RGB(160, 220, 255)); // Acik Cyan HWID
                SetBkColor(hdc, RGB(22, 23, 32));
                return (LRESULT)data->hCardBrush;
            }
            SetTextColor(hdc, RGB(255, 255, 255)); // Beyaz Key Metni
            SetBkColor(hdc, RGB(28, 29, 40));
            return (LRESULT)data->hEditBrush;
        }

        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
            if (dis->CtlID == IDC_AUTH_BTN_VERIFY) {
                const wchar_t* btnText = data->isChecking ? L"Dogrulaniyor..." : L"Giris Yap ve Baslat";
                DrawCustomButton(dis, RGB(108, 92, 231), RGB(125, 110, 245), RGB(255, 255, 255), data->hFontBold, btnText);
                return TRUE;
            } else if (dis->CtlID == IDC_AUTH_BTN_CANCEL) {
                DrawCustomButton(dis, RGB(36, 38, 50), RGB(48, 50, 65), RGB(190, 190, 205), data->hFontBold, L"Iptal");
                return TRUE;
            } else if (dis->CtlID == IDC_AUTH_BTN_COPY) {
                DrawCustomButton(dis, RGB(42, 44, 58), RGB(55, 58, 75), RGB(160, 220, 255), data->hFontRegular, L"Kopyala");
                return TRUE;
            } else if (dis->CtlID == IDC_AUTH_BTN_CLOSE) {
                DrawCustomButton(dis, RGB(22, 24, 33), RGB(214, 48, 49), RGB(200, 200, 210), data->hFontBold, L"X", false);
                return TRUE;
            }
            break;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);

            if (wmId == IDC_AUTH_BTN_COPY) {
                std::string hwid = HWID::GetHWID();
                CopyToClipboard(hwid);
                data->statusText = L"[BASARILI] HWID panoya kopyalandi!";
                data->statusColor = RGB(46, 213, 115); // Acik Yesil
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }

            if (wmId == IDC_AUTH_BTN_CLOSE || wmId == IDC_AUTH_BTN_CANCEL) {
                data->confirmed = false;
                DestroyWindow(hWnd);
                return 0;
            }

            if (wmId == IDC_AUTH_BTN_VERIFY) {
                if (data->isChecking) return 0;

                wchar_t buffer[256] = { 0 };
                GetWindowTextW(data->hEdit, buffer, 255);
                char keyA[256] = { 0 };
                WideCharToMultiByte(CP_UTF8, 0, buffer, -1, keyA, 255, nullptr, nullptr);
                std::string userKey = keyA;

                size_t first = userKey.find_first_not_of(" \t\r\n");
                size_t last = userKey.find_last_not_of(" \t\r\n");
                if (first != std::string::npos && last != std::string::npos) {
                    userKey = userKey.substr(first, (last - first + 1));
                } else {
                    userKey.clear();
                }

                if (userKey.empty()) {
                    data->statusText = L"Hata: Lutfen lisans anahtarinizi yaziniz veya yapistiriniz!";
                    data->statusColor = RGB(255, 71, 87); // Kirmizi
                    InvalidateRect(hWnd, nullptr, FALSE);
                    return 0;
                }

                // Dogrulama Basladi
                data->isChecking = true;
                data->statusText = L"Sunucuya baglaniliyor ve lisans dogrulaniyor...";
                data->statusColor = RGB(254, 202, 87); // Sari
                EnableWindow(data->btnVerify, FALSE);
                InvalidateRect(hWnd, nullptr, FALSE);
                UpdateWindow(hWnd);

                // Istek gonder
                Response resp = SendRequest(userKey);
                data->lastResponse = resp;
                data->isChecking = false;
                EnableWindow(data->btnVerify, TRUE);

                if (resp.success) {
                    data->statusText = L"[BASARILI] Lisans onaylandi! Sistem baslatiliyor...";
                    data->statusColor = RGB(46, 213, 115); // Yesil
                    InvalidateRect(hWnd, nullptr, FALSE);
                    UpdateWindow(hWnd);

                    data->enteredKey = userKey;
                    data->confirmed = true;

                    LRESULT isChecked = SendMessageW(data->chkRemember, BM_GETCHECK, 0, 0);
                    if (isChecked == BST_CHECKED) {
                        SaveKey(userKey);
                    } else {
                        DeleteSavedKey();
                    }

                    Sleep(500); // Kullaniciya basari hissini goster
                    DestroyWindow(hWnd);
                } else {
                    std::string msg = resp.message.empty() ? "Gecersiz Lisans Anahtari veya HWID uyusmazligi!" : resp.message;
                    std::wstring msgW(msg.begin(), msg.end());
                    data->statusText = L"Hata: " + msgW;
                    data->statusColor = RGB(255, 71, 87); // Kirmizi
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
                return 0;
            }
            break;
        }

        case WM_CLOSE: {
            data->confirmed = false;
            DestroyWindow(hWnd);
            return 0;
        }

        case WM_DESTROY: {
            if (data->hFontTitle)   DeleteObject(data->hFontTitle);
            if (data->hFontBold)    DeleteObject(data->hFontBold);
            if (data->hFontRegular) DeleteObject(data->hFontRegular);
            if (data->hFontMono)    DeleteObject(data->hFontMono);
            if (data->hBgBrush)     DeleteObject(data->hBgBrush);
            if (data->hCardBrush)   DeleteObject(data->hCardBrush);
            if (data->hEditBrush)   DeleteObject(data->hEditBrush);
            if (data->hAccentBrush) DeleteObject(data->hAccentBrush);
            PostQuitMessage(0);
            return 0;
        }
        }
        return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }

    inline Response PromptKeyDialogModern() {
        WNDCLASSEXW wc = { 0 };
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = ModernUIMessageProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"HeavenAuthModernWnd";
        wc.hbrBackground = nullptr;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);

        int width = 480;
        int height = 312;
        int screenX = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
        int screenY = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

        ModernUIData data;

        HWND hWnd = CreateWindowExW(
            WS_EX_TOPMOST,
            wc.lpszClassName,
            L"Heaven - Lisans Dogrulama",
            WS_POPUP | WS_VISIBLE,
            screenX, screenY, width, height,
            nullptr, nullptr, wc.hInstance, &data
        );

        if (!hWnd) {
            Response errResp;
            errResp.success = false;
            errResp.message = "Arayuz penceresi olusturulamadi.";
            return errResp;
        }

        ShowWindow(hWnd, SW_SHOW);
        UpdateWindow(hWnd);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
                SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(IDC_AUTH_BTN_VERIFY, 0), (LPARAM)data.btnVerify);
                continue;
            }
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
                SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(IDC_AUTH_BTN_CANCEL, 0), (LPARAM)data.btnCancel);
                continue;
            }
            if (!IsDialogMessageW(hWnd, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        UnregisterClassW(wc.lpszClassName, wc.hInstance);

        if (data.confirmed && data.lastResponse.success) {
            return data.lastResponse;
        }

        Response cancelResp;
        cancelResp.success = false;
        cancelResp.message = "Lisans dogrulamasi iptal edildi.";
        return cancelResp;
    }

    // ------------------------------------------------------------------------
    // GENEL DOĞRULAMA FONKSİYONU
    // ------------------------------------------------------------------------
    inline Response Verify() {
        // Her inject edildiginde pencere gosterilir
        return PromptKeyDialogModern();
    }
}
