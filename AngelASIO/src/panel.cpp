#include "panel.h"
#include "common.h"
#include "engine.h"
#include <commctrl.h>
#include <vector>
#include <cstdio>
#include <cwchar>
#include <cmath>

namespace {

enum {
    ID_IN = 100, ID_OUT, ID_BUF, ID_PER, ID_EXCL, ID_MON, ID_RECFX, ID_HPF, ID_COMP,
    ID_GAIN, ID_REV, ID_THR, ID_LBL_GAIN, ID_LBL_REV, ID_LBL_THR, ID_APPLY, ID_CLOSE
};
const int kBufs[] = { 32, 64, 128, 256, 512, 1024, 2048 };
const float kPers[] = { 0.f, 2.f, 3.f, 5.f, 8.f, 10.f };

struct State {
    Config cfg;
    std::vector<EndpointInfo> ins, outs;
    std::string status;
    bool applied = false, done = false;
    HWND owner = nullptr;
};

HWND mk(HWND p, const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h, int id) {
    return CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h, p, (HMENU)(INT_PTR)id, g_hInst, nullptr);
}
void fillDevices(HWND cb, const std::vector<EndpointInfo>& v, const std::wstring& sel) {
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Predeterminado del sistema");
    int cur = 0;
    for (size_t i = 0; i < v.size(); i++) {
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)v[i].name.c_str());
        if (v[i].id == sel) cur = (int)i + 1;
    }
    SendMessageW(cb, CB_SETCURSEL, cur, 0);
}
void setChk(HWND h, int id, bool v) { CheckDlgButton(h, id, v ? BST_CHECKED : BST_UNCHECKED); }
bool getChk(HWND h, int id) { return IsDlgButtonChecked(h, id) == BST_CHECKED; }
int tbGet(HWND h, int id) { return (int)SendDlgItemMessageW(h, id, TBM_GETPOS, 0, 0); }
void tbInit(HWND h, int id, int lo, int hi, int pos) {
    SendDlgItemMessageW(h, id, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
    SendDlgItemMessageW(h, id, TBM_SETPOS, TRUE, pos);
}
void updateLabels(HWND h) {
    wchar_t b[96];
    swprintf(b, 96, L"Ganancia del monitor: %+d dB", tbGet(h, ID_GAIN)); SetDlgItemTextW(h, ID_LBL_GAIN, b);
    swprintf(b, 96, L"Reverb (solo monitoreo): %d %%", tbGet(h, ID_REV)); SetDlgItemTextW(h, ID_LBL_REV, b);
    swprintf(b, 96, L"Umbral del compresor: %d dB", tbGet(h, ID_THR)); SetDlgItemTextW(h, ID_LBL_THR, b);
}

void readUi(HWND h, State* st) {
    Config& c = st->cfg;
    int i = (int)SendDlgItemMessageW(h, ID_IN, CB_GETCURSEL, 0, 0);
    c.inputId = (i > 0 && (size_t)i <= st->ins.size()) ? st->ins[i - 1].id : L"";
    i = (int)SendDlgItemMessageW(h, ID_OUT, CB_GETCURSEL, 0, 0);
    c.outputId = (i > 0 && (size_t)i <= st->outs.size()) ? st->outs[i - 1].id : L"";
    i = (int)SendDlgItemMessageW(h, ID_BUF, CB_GETCURSEL, 0, 0);
    if (i >= 0 && i < 7) c.bufferSize = kBufs[i];
    i = (int)SendDlgItemMessageW(h, ID_PER, CB_GETCURSEL, 0, 0);
    if (i >= 0 && i < 6) c.devicePeriodMs = kPers[i];
    c.exclusive = getChk(h, ID_EXCL);
    c.monitor = getChk(h, ID_MON);
    c.recordFx = getChk(h, ID_RECFX);
    c.hpf = getChk(h, ID_HPF);
    c.comp = getChk(h, ID_COMP);
    c.monitorGainDb = (float)tbGet(h, ID_GAIN);
    c.reverbMix = tbGet(h, ID_REV) / 100.f;
    c.compThreshDb = (float)tbGet(h, ID_THR);
}

LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    State* st = (State*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_CREATE: {
        st = (State*)((CREATESTRUCTW*)l)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)st);
        const Config& c = st->cfg;
        int y = 10;
        mk(h, L"STATIC", L"Entrada (microfono):", 0, 12, y, 420, 16, 0); y += 18;
        HWND cin = mk(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 12, y, 420, 200, ID_IN); y += 28;
        mk(h, L"STATIC", L"Salida (audifonos / monitores):", 0, 12, y, 420, 16, 0); y += 18;
        HWND cout = mk(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 12, y, 420, 200, ID_OUT); y += 30;
        fillDevices(cin, st->ins, c.inputId); fillDevices(cout, st->outs, c.outputId);

        mk(h, L"STATIC", L"Bloque ASIO (frames):", 0, 12, y + 3, 130, 16, 0);
        HWND cb = mk(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 145, y, 70, 200, ID_BUF);
        mk(h, L"STATIC", L"Periodo WASAPI:", 0, 232, y + 3, 100, 16, 0);
        HWND cp = mk(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 335, y, 97, 200, ID_PER);
        int bi = 1; for (int i = 0; i < 7; i++) { wchar_t b[16]; swprintf(b, 16, L"%d", kBufs[i]); SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)b); if (kBufs[i] == c.bufferSize) bi = i; }
        SendMessageW(cb, CB_SETCURSEL, bi, 0);
        int pi = 0;
        for (int i = 0; i < 6; i++) {
            wchar_t b[32]; if (i == 0) wcscpy(b, L"Minimo"); else swprintf(b, 32, L"%g ms", kPers[i]);
            SendMessageW(cp, CB_ADDSTRING, 0, (LPARAM)b);
            if (kPers[i] == c.devicePeriodMs) pi = i;
        }
        SendMessageW(cp, CB_SETCURSEL, pi, 0);
        y += 32;
        mk(h, L"BUTTON", L"WASAPI exclusivo (menor latencia)", BS_AUTOCHECKBOX | WS_TABSTOP, 12, y, 420, 18, ID_EXCL); y += 22;
        mk(h, L"BUTTON", L"Monitoreo directo con efectos (chip DSP)", BS_AUTOCHECKBOX | WS_TABSTOP, 12, y, 420, 18, ID_MON); y += 22;
        mk(h, L"BUTTON", L"Grabar la senal CON efectos (EQ/compresor) en el DAW", BS_AUTOCHECKBOX | WS_TABSTOP, 12, y, 420, 18, ID_RECFX); y += 22;
        mk(h, L"BUTTON", L"Filtro pasa-altos 80 Hz", BS_AUTOCHECKBOX | WS_TABSTOP, 12, y, 200, 18, ID_HPF);
        mk(h, L"BUTTON", L"Compresor", BS_AUTOCHECKBOX | WS_TABSTOP, 232, y, 200, 18, ID_COMP); y += 28;
        setChk(h, ID_EXCL, c.exclusive); setChk(h, ID_MON, c.monitor); setChk(h, ID_RECFX, c.recordFx);
        setChk(h, ID_HPF, c.hpf); setChk(h, ID_COMP, c.comp);

        mk(h, L"STATIC", L"", 0, 12, y, 420, 16, ID_LBL_GAIN); y += 18;
        mk(h, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 12, y, 420, 30, ID_GAIN); y += 34;
        mk(h, L"STATIC", L"", 0, 12, y, 420, 16, ID_LBL_REV); y += 18;
        mk(h, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 12, y, 420, 30, ID_REV); y += 34;
        mk(h, L"STATIC", L"", 0, 12, y, 420, 16, ID_LBL_THR); y += 18;
        mk(h, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 12, y, 420, 30, ID_THR); y += 38;
        tbInit(h, ID_GAIN, -40, 12, (int)std::lround(c.monitorGainDb));
        tbInit(h, ID_REV, 0, 100, (int)std::lround(c.reverbMix * 100.f));
        tbInit(h, ID_THR, -40, 0, (int)std::lround(c.compThreshDb));
        updateLabels(h);

        std::wstring s(st->status.begin(), st->status.end());
        mk(h, L"STATIC", s.c_str(), SS_LEFT, 12, y, 420, 48, 0); y += 54;
        mk(h, L"BUTTON", L"Aplicar", BS_DEFPUSHBUTTON | WS_TABSTOP, 250, y, 85, 26, ID_APPLY);
        mk(h, L"BUTTON", L"Cerrar", BS_PUSHBUTTON | WS_TABSTOP, 345, y, 85, 26, ID_CLOSE);
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        for (HWND c2 = GetWindow(h, GW_CHILD); c2; c2 = GetWindow(c2, GW_HWNDNEXT)) SendMessageW(c2, WM_SETFONT, (WPARAM)f, TRUE);
        return 0;
    }
    case WM_HSCROLL: updateLabels(h); return 0;
    case WM_COMMAND:
        if (LOWORD(w) == ID_APPLY) { readUi(h, st); st->cfg.save(); st->applied = true; DestroyWindow(h); return 0; }
        if (LOWORD(w) == ID_CLOSE) { DestroyWindow(h); return 0; }
        break;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: if (st) st->done = true; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

bool showPanel(HWND owner, const std::string& status, Config& cfg) {
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    State st;
    st.cfg = cfg; st.status = status; st.owner = owner;
    st.ins = listEndpoints(true);
    st.outs = listEndpoints(false);

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = PanelProc; wc.hInstance = g_hInst; wc.lpszClassName = L"AngelASIOPanel";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);
        registered = true;
    }
    const int W = 456, H = 600;
    int x = (GetSystemMetrics(SM_CXSCREEN) - W) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;
    bool ownerOk = owner && IsWindow(owner);
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"AngelASIOPanel", L"AMR Angel ASIO - Panel de control",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, W, H, ownerOk ? owner : nullptr, nullptr, g_hInst, &st);
    if (!h) return false;
    if (ownerOk) EnableWindow(owner, FALSE);
    ShowWindow(h, SW_SHOW); UpdateWindow(h);
    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (ownerOk) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
    if (st.applied) cfg = st.cfg;
    return st.applied;
}
