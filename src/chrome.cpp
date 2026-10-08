#include "clippy.h"
#include <uiautomation.h>
#include <shellapi.h>
#include <algorithm>
#include <cwctype>
#include <utility>

namespace clippy {
namespace {

template<class T> class Com {
    T* value_ = nullptr;
public:
    Com() = default;
    explicit Com(T* value) : value_(value) {}
    Com(const Com& other) : value_(other.value_) { if (value_) value_->AddRef(); }
    Com(Com&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    ~Com() { if (value_) value_->Release(); }
    Com& operator=(Com other) { std::swap(value_, other.value_); return *this; }
    T* operator->() const { return value_; }
    T* get() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
    T** put() { if (value_) value_->Release(); value_ = nullptr; return &value_; }
};

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return std::towlower(c); });
    return value;
}

bool Cancelled(const std::function<bool()>& cancelled) { return cancelled && cancelled(); }

thread_local const std::function<bool()>* deliveryCancellation = nullptr;
bool StopRequested() { return deliveryCancellation && Cancelled(*deliveryCancellation); }
struct CancellationContext {
    const std::function<bool()>* previous;
    explicit CancellationContext(const std::function<bool()>& cancelled)
        : previous(deliveryCancellation) { deliveryCancellation = &cancelled; }
    ~CancellationContext() { deliveryCancellation = previous; }
};

bool Pause(int milliseconds, const std::function<bool()>& cancelled) {
    ULONGLONG until = GetTickCount64() + std::max(milliseconds, 0);
    do {
        if (Cancelled(cancelled)) return false;
        ULONGLONG now = GetTickCount64();
        if (now >= until) return true;
        Sleep(static_cast<DWORD>(std::min<ULONGLONG>(until - now, 40)));
    } while (true);
}

std::wstring TextProperty(IUIAutomationElement* element, PROPERTYID property) {
    if (StopRequested()) return {};
    VARIANT value; VariantInit(&value);
    std::wstring result;
    if (SUCCEEDED(element->GetCurrentPropertyValue(property, &value)) && value.vt == VT_BSTR && value.bstrVal)
        result.assign(value.bstrVal, SysStringLen(value.bstrVal));
    VariantClear(&value);
    return result;
}

std::wstring ElementValue(IUIAutomationElement* element) {
    if (StopRequested()) return {};
    Com<IUIAutomationValuePattern> value;
    if (FAILED(element->GetCurrentPatternAs(UIA_ValuePatternId, IID_IUIAutomationValuePattern,
            reinterpret_cast<void**>(value.put())))) return {};
    BSTR text = nullptr;
    if (FAILED(value->get_CurrentValue(&text))) return {};
    std::wstring result = text ? std::wstring(text, SysStringLen(text)) : std::wstring();
    SysFreeString(text);
    return result;
}

bool Enabled(IUIAutomationElement* element) {
    if (StopRequested()) return false;
    BOOL enabled = FALSE, offscreen = TRUE;
    return SUCCEEDED(element->get_CurrentIsEnabled(&enabled)) && enabled &&
        SUCCEEDED(element->get_CurrentIsOffscreen(&offscreen)) && !offscreen;
}

bool Same(IUIAutomation* automation, IUIAutomationElement* a, IUIAutomationElement* b) {
    if (StopRequested()) return false;
    BOOL same = FALSE;
    return a && b && SUCCEEDED(automation->CompareElements(a, b, &same)) && same;
}

Com<IUIAutomationElement> Root(IUIAutomation* automation, HWND window) {
    Com<IUIAutomationElement> root;
    if (StopRequested()) return root;
    automation->ElementFromHandle(window, root.put());
    return root;
}

std::vector<Com<IUIAutomationElement>> FindType(IUIAutomation* automation,
        IUIAutomationElement* root, CONTROLTYPEID type, bool* success = nullptr) {
    std::vector<Com<IUIAutomationElement>> result;
    if (success) *success = false;
    if (!root || StopRequested()) return result;
    VARIANT value; VariantInit(&value); value.vt = VT_I4; value.lVal = type;
    Com<IUIAutomationCondition> condition;
    Com<IUIAutomationElementArray> elements;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, value, condition.put())) ||
        FAILED(root->FindAll(TreeScope_Descendants, condition.get(), elements.put()))) return result;
    int count = 0;
    if (FAILED(elements->get_Length(&count))) return result;
    if (success) *success = true;
    for (int i = 0; i < count && i < 4000 && !StopRequested(); ++i) {
        Com<IUIAutomationElement> element;
        if (SUCCEEDED(elements->GetElement(i, element.put()))) result.push_back(std::move(element));
    }
    return result;
}

Com<IUIAutomationElement> FindId(IUIAutomation* automation,
        IUIAutomationElement* root, const wchar_t* id) {
    Com<IUIAutomationElement> element;
    if (!root || StopRequested()) return element;
    VARIANT value; VariantInit(&value); value.vt = VT_BSTR; value.bstrVal = SysAllocString(id);
    Com<IUIAutomationCondition> condition;
    if (value.bstrVal && SUCCEEDED(automation->CreatePropertyCondition(
            UIA_AutomationIdPropertyId, value, condition.put())) && !StopRequested())
        root->FindFirst(TreeScope_Descendants, condition.get(), element.put());
    VariantClear(&value);
    return element;
}

bool IsChrome(HWND window) {
    if (!window) return false;
    wchar_t cls[80]{}; GetClassNameW(window, cls, 80);
    if (std::wstring(cls) != L"Chrome_WidgetWin_1") return false;
    DWORD process = 0; GetWindowThreadProcessId(window, &process);
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
    if (!handle) return false;
    wchar_t path[32768]{}; DWORD length = 32768;
    bool ok = QueryFullProcessImageNameW(handle, 0, path, &length) != FALSE;
    CloseHandle(handle);
    if (!ok) return false;
    std::wstring executable = Lower(path);
    size_t slash = executable.find_last_of(L"\\/");
    return executable.substr(slash == std::wstring::npos ? 0 : slash + 1) == L"chrome.exe";
}

std::vector<HWND> ChromeWindows() {
    std::vector<HWND> result;
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        // Tab Search bubbles are owned windows, not independent browser windows.
        if (IsWindowVisible(window) && !GetWindow(window, GW_OWNER) && IsChrome(window))
            reinterpret_cast<std::vector<HWND>*>(parameter)->push_back(window);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}

bool OwnedForeground(HWND browser) {
    HWND foreground = GetForegroundWindow();
    if (foreground == browser) return true;
    for (HWND owner = foreground; owner; owner = GetWindow(owner, GW_OWNER))
        if (owner == browser) return true;
    return false;
}

bool Activate(HWND window) {
    if (!IsWindow(window)) return false;
    if (IsIconic(window)) ShowWindowAsync(window, SW_RESTORE);
    DWORD thread = GetCurrentThreadId();
    DWORD foreground = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD target = GetWindowThreadProcessId(window, nullptr);
    bool attachedForeground = foreground && foreground != thread && AttachThreadInput(thread, foreground, TRUE);
    bool attachedTarget = target != thread && target != foreground && AttachThreadInput(thread, target, TRUE);
    SetForegroundWindow(window);
    if (attachedTarget) AttachThreadInput(thread, target, FALSE);
    if (attachedForeground) AttachThreadInput(thread, foreground, FALSE);
    for (int i = 0; i < 20; ++i) {
        if (GetForegroundWindow() == window) return true;
        Sleep(25);
    }
    return false;
}

bool ModifiersUp() {
    for (int i = 0; i < 80; ++i) {
        bool down = false;
        for (int key : {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN})
            down = down || ((GetAsyncKeyState(key) & 0x8000) != 0);
        if (!down) return true;
        Sleep(25);
    }
    return false;
}

bool Chord(HWND browser, WORD key, bool control = false, bool shift = false,
        const std::function<bool()>& finalGuard = {}) {
    if (StopRequested() || !OwnedForeground(browser) || !ModifiersUp() ||
        StopRequested() || !OwnedForeground(browser)) return false;
    if (finalGuard && !finalGuard()) return false;
    INPUT inputs[6]{}; UINT count = 0;
    auto append = [&](WORD value, bool up) {
        auto& input = inputs[count++]; input.type = INPUT_KEYBOARD;
        input.ki.wVk = value; input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    };
    if (control) append(VK_CONTROL, false);
    if (shift) append(VK_SHIFT, false);
    append(key, false); append(key, true);
    if (shift) append(VK_SHIFT, true);
    if (control) append(VK_CONTROL, true);
    // One call only: a partial or failed paste/send must never be retried.
    if (StopRequested()) return false;
    UINT delivered = SendInput(count, inputs, sizeof(INPUT));
    if (delivered && delivered != count) {
        INPUT releases[2]{}; UINT releaseCount = 0;
        for (WORD modifier : {WORD(VK_CONTROL), WORD(VK_SHIFT)}) {
            if ((modifier == VK_CONTROL && !control) || (modifier == VK_SHIFT && !shift)) continue;
            auto& release = releases[releaseCount++]; release.type = INPUT_KEYBOARD;
            release.ki.wVk = modifier; release.ki.dwFlags = KEYEVENTF_KEYUP;
        }
        if (releaseCount) SendInput(releaseCount, releases, sizeof(INPUT));
    }
    return delivered == count;
}

Com<IUIAutomationElement> Parent(IUIAutomation* automation, IUIAutomationElement* element) {
    Com<IUIAutomationTreeWalker> walker;
    Com<IUIAutomationElement> parent;
    if (StopRequested()) return parent;
    if (SUCCEEDED(automation->get_ControlViewWalker(walker.put())))
        walker->GetParentElement(element, parent.put());
    return parent;
}

bool HasDocumentAncestor(IUIAutomation* automation, IUIAutomationElement* element) {
    Com<IUIAutomationElement> parent = Parent(automation, element);
    for (int i = 0; parent && i < 40; ++i) {
        CONTROLTYPEID type = 0; parent->get_CurrentControlType(&type);
        if (type == UIA_DocumentControlTypeId) return true;
        parent = Parent(automation, parent.get());
    }
    return false;
}

bool ChatGPTHost(std::wstring value) {
    value = Lower(value);
    while (!value.empty() && std::iswspace(value.front())) value.erase(value.begin());
    while (!value.empty() && std::iswspace(value.back())) value.pop_back();
    if (value.rfind(L"https://", 0) == 0) value.erase(0, 8);
    else if (value.find(L"://") != std::wstring::npos) return false;
    size_t end = value.find_first_of(L"/?#");
    // An exact host check rejects lookalike hosts, userinfo, and other ports.
    return value.substr(0, end) == L"chatgpt.com";
}

bool ChatGPTAddress(std::wstring value) {
    if (!ChatGPTHost(value)) return false;
    value = Lower(value);
    while (!value.empty() && std::iswspace(value.front())) value.erase(value.begin());
    if (value.rfind(L"https://", 0) == 0) value.erase(0, 8);
    size_t slash = value.find(L'/');
    std::wstring path = slash == std::wstring::npos ? L"/" : value.substr(slash);
    path = path.substr(0, path.find_first_of(L"?#"));
    return path == L"/" || path.rfind(L"/c/", 0) == 0 || path.rfind(L"/g/", 0) == 0;
}

std::wstring Address(IUIAutomation* automation, HWND browser) {
    auto root = Root(automation, browser);
    for (auto& element : FindType(automation, root.get(), UIA_EditControlTypeId)) {
        std::wstring id = Lower(TextProperty(element.get(), UIA_AutomationIdPropertyId));
        std::wstring name = Lower(TextProperty(element.get(), UIA_NamePropertyId));
        bool address = id == L"omnibox" || name == L"address and search bar" ||
            name == L"search google or type a url";
        if (address && !HasDocumentAncestor(automation, element.get())) return ElementValue(element.get());
    }
    return {};
}

Com<IUIAutomationElement> SelectedTab(IUIAutomation* automation, HWND window) {
    auto root = Root(automation, window);
    for (auto& tab : FindType(automation, root.get(), UIA_TabItemControlTypeId)) {
        Com<IUIAutomationSelectionItemPattern> selection;
        BOOL selected = FALSE;
        if (SUCCEEDED(tab->GetCurrentPatternAs(UIA_SelectionItemPatternId,
                IID_IUIAutomationSelectionItemPattern, reinterpret_cast<void**>(selection.put()))) &&
            SUCCEEDED(selection->get_CurrentIsSelected(&selected)) && selected) return tab;
    }
    return {};
}

bool SelectTab(IUIAutomationElement* element) {
    if (!element) return false;
    Com<IUIAutomationSelectionItemPattern> selection;
    return SUCCEEDED(element->GetCurrentPatternAs(UIA_SelectionItemPatternId,
        IID_IUIAutomationSelectionItemPattern, reinterpret_cast<void**>(selection.put()))) &&
        SUCCEEDED(selection->Select());
}

struct WindowState {
    HWND window = nullptr;
    WINDOWPLACEMENT placement{};
    Com<IUIAutomationElement> selected;
    bool touched = false;
};

class RestoreWindows {
    IUIAutomation* automation_;
    HWND previous_;
    HWND destination_ = nullptr;
    bool minimize_;
public:
    std::vector<WindowState> windows;
    explicit RestoreWindows(IUIAutomation* automation, HWND previous, bool minimize)
        : automation_(automation), previous_(previous), minimize_(minimize) {}
    void Remember(HWND window) {
        for (auto& state : windows) if (state.window == window) return;
        WindowState state; state.window = window; state.placement.length = sizeof(WINDOWPLACEMENT);
        if (!GetWindowPlacement(window, &state.placement)) return;
        state.selected = SelectedTab(automation_, window);
        windows.push_back(std::move(state));
    }
    void Touch(HWND window) {
        Remember(window);
        for (auto& state : windows) if (state.window == window) state.touched = true;
    }
    void Destination(HWND window) { Touch(window); destination_ = window; }
    ~RestoreWindows() {
        // A user switching to another app is an interruption. Do not pull them
        // back into Chrome or the original source window after that switch.
        bool ownForeground = false;
        for (auto& state : windows)
            if (state.touched && OwnedForeground(state.window)) ownForeground = true;
        if (!ownForeground) return;
        for (auto& state : windows) {
            if (!state.touched || !IsWindow(state.window)) continue;
            // UIA selection restores a source Chrome tab without keyboard shortcuts.
            SelectTab(state.selected.get());
            SetWindowPlacement(state.window, &state.placement);
            if (minimize_ && state.window == destination_ && state.window != previous_)
                ShowWindowAsync(state.window, SW_MINIMIZE);
        }
        if (IsWindow(previous_)) Activate(previous_);
    }
};

bool FocusIs(IUIAutomation* automation, IUIAutomationElement* element) {
    Com<IUIAutomationElement> focused;
    return SUCCEEDED(automation->GetFocusedElement(focused.put())) && Same(automation, focused.get(), element);
}

bool WaitForFocus(IUIAutomation* automation, IUIAutomationElement* element, HWND browser,
        const std::function<bool()>& cancelled, int timeout = 1200) {
    ULONGLONG until = GetTickCount64() + timeout;
    do {
        if (Cancelled(cancelled) || !OwnedForeground(browser)) return false;
        if (FocusIs(automation, element)) return true;
        if (!Pause(25, cancelled)) return false;
    } while (GetTickCount64() < until);
    return false;
}

Com<IUIAutomationElement> SearchInput(IUIAutomation* automation, HWND browser) {
    Com<IUIAutomationElement> focused;
    automation->GetFocusedElement(focused.put());
    if (focused && TextProperty(focused.get(), UIA_AutomationIdPropertyId) == L"searchInput") return focused;
    // Some Chrome versions expose the search input's label but omit its HTML id.
    auto root = Root(automation, GetForegroundWindow());
    for (CONTROLTYPEID type : {UIA_ComboBoxControlTypeId, UIA_EditControlTypeId}) {
        for (auto& element : FindType(automation, root.get(), type)) {
            std::wstring id = TextProperty(element.get(), UIA_AutomationIdPropertyId);
            std::wstring name = Lower(TextProperty(element.get(), UIA_NamePropertyId));
            if (id == L"searchInput" || (name.rfind(L"search tabs", 0) == 0 &&
                HasDocumentAncestor(automation, element.get()))) return element;
        }
    }
    (void)browser;
    return {};
}

Com<IUIAutomationElement> SearchRoot(IUIAutomation* automation, IUIAutomationElement* input) {
    Com<IUIAutomationElement> root(input); input->AddRef();
    for (int i = 0; i < 40; ++i) {
        CONTROLTYPEID type = 0; root->get_CurrentControlType(&type);
        if (type == UIA_DocumentControlTypeId) return root;
        auto parent = Parent(automation, root.get());
        if (!parent) break;
        root = std::move(parent);
    }
    return {};
}

enum class SearchResult { Found, None, SkipRoute, Interrupted, Failed };

SearchResult DiscoveryFailure(HWND expected, const std::function<bool()>& cancelled) {
    return Cancelled(cancelled) || !OwnedForeground(expected)
        ? SearchResult::Interrupted : SearchResult::Failed;
}

SearchResult FindChatGPTTabOnce(IUIAutomation* automation, HWND browser, HWND& target,
        RestoreWindows& restore, std::wstring& error, const std::function<bool()>& cancelled,
        std::vector<std::wstring>& visitedTabs) {
    if (Cancelled(cancelled)) { error = L"Delivery cancelled."; return SearchResult::Interrupted; }
    restore.Touch(browser);
    if (!Activate(browser)) { error = L"Windows would not activate Chrome. Try again with Chrome open."; return SearchResult::Failed; }
    if (Cancelled(cancelled)) { error = L"Delivery cancelled."; return SearchResult::Interrupted; }
    if (ChatGPTAddress(Address(automation, browser))) { target = browser; return SearchResult::Found; }
    if (!Chord(browser, 'A', true, true, [&] { return !Cancelled(cancelled); })) {
        error = L"Chrome Tab Search could not be opened."; return DiscoveryFailure(browser, cancelled);
    }
    Com<IUIAutomationElement> input;
    for (int i = 0; i < 40; ++i) {
        if (Cancelled(cancelled)) { error = L"Delivery cancelled."; return SearchResult::Interrupted; }
        if (!OwnedForeground(browser)) { error = L"The foreground window changed during Chrome Tab Search."; return SearchResult::Interrupted; }
        input = SearchInput(automation, browser);
        if (input) break;
        Pause(50, cancelled);
    }
    if (!input) {
        error = L"Chrome Tab Search is unavailable to Windows accessibility. Open ChatGPT as the active Chrome tab, then try again. Nothing was pasted.";
        return DiscoveryFailure(browser, cancelled);
    }
    Com<IUIAutomationValuePattern> value;
    BSTR query = SysAllocString(L"");
    struct FreeQuery { BSTR query; ~FreeQuery() { SysFreeString(query); } } freeQuery{query};
    if (Cancelled(cancelled)) { error = L"Delivery cancelled."; return SearchResult::Interrupted; }
    if (!OwnedForeground(browser) ||
        FAILED(input->GetCurrentPatternAs(UIA_ValuePatternId, IID_IUIAutomationValuePattern,
            reinterpret_cast<void**>(value.put()))) || Cancelled(cancelled) ||
        !query || FAILED(value->SetValue(query))) {
        error = L"Chrome's tab search field could not be set through Windows accessibility.";
        if (OwnedForeground(browser)) Chord(browser, VK_ESCAPE);
        return DiscoveryFailure(browser, cancelled);
    }
    if (!Pause(500, cancelled)) { error = L"Delivery cancelled."; return SearchResult::Interrupted; }
    // Empty search preserves Chromium's MRU ordering. A nonempty query would
    // sort by fuzzy-match score and could select an older conversation.
    // End expands the cumulative lazy DOM list; Home restores its first view.
    if (ElementValue(input.get()) != L"" || FAILED(input->SetFocus()) ||
        !WaitForFocus(automation, input.get(), browser, cancelled) ||
        !Chord(browser, VK_END, false, false, [&] { return FocusIs(automation, input.get()); }) ||
        !Pause(300, cancelled) ||
        !Chord(browser, VK_HOME, false, false, [&] { return FocusIs(automation, input.get()); }) ||
        !Pause(300, cancelled)) {
        error = L"Chrome's full tab list could not be read safely. Nothing was pasted.";
        return DiscoveryFailure(browser, cancelled);
    }
    auto root = SearchRoot(automation, input.get());
    bool searchAccessible = false;
    auto items = FindType(automation, root.get(), UIA_ListItemControlTypeId, &searchAccessible);
    if (!searchAccessible || ElementValue(input.get()) != L"") {
        error = L"Chrome Tab Search results were not accessible. Nothing was pasted.";
        if (OwnedForeground(browser)) Chord(browser, VK_ESCAPE);
        return DiscoveryFailure(browser, cancelled);
    }
    Com<IUIAutomationElement> chosen;
    bool understood = false;
    for (auto& item : items) {
        std::wstring name = Lower(TextProperty(item.get(), UIA_NamePropertyId));
        std::wstring id = TextProperty(item.get(), UIA_AutomationIdPropertyId);
        BOOL enabled = FALSE; item->get_CurrentIsEnabled(&enabled);
        if (name.find(L"open tab") != std::wstring::npos || name.find(L"recently closed") != std::wstring::npos)
            understood = true;
        // Chromium orders ordinary open rows by the time they were last active.
        // Audio/video tabs have a separate leading section in Chrome's UI.
        // The currently active ChatGPT tab was already handled above. Closed tabs
        // and their nested close buttons must never be chosen.
        if (name.find(L"chatgpt.com") != std::wstring::npos &&
            name.find(L"open tab") != std::wstring::npos &&
            name.rfind(L"close ", 0) != 0 && enabled &&
            std::find(visitedTabs.begin(), visitedTabs.end(), id) == visitedTabs.end()) {
            if (id.empty()) {
                error = L"Chrome Tab Search did not expose stable tab IDs. Nothing was pasted.";
                return DiscoveryFailure(browser, cancelled);
            }
            visitedTabs.push_back(id); chosen = item; break;
        }
    }
    if (!chosen) {
        // A correctly identified, empty tab-search document is a valid no-match.
        // Non-English result labels cannot reliably distinguish open vs closed.
        if (!items.empty() && !understood) {
            error = L"Chrome Tab Search result labels could not distinguish open tabs. This version supports English Chrome labels.";
            if (OwnedForeground(browser)) Chord(browser, VK_ESCAPE);
            return DiscoveryFailure(browser, cancelled);
        }
        if (Cancelled(cancelled) || !OwnedForeground(browser)) {
            error = L"Chrome Tab Search was interrupted. Nothing was pasted.";
            return SearchResult::Interrupted;
        }
        if (!Chord(browser, VK_ESCAPE)) {
            error = L"Chrome Tab Search could not be closed safely. Nothing was pasted.";
            return DiscoveryFailure(browser, cancelled);
        }
        return SearchResult::None;
    }
    if (Cancelled(cancelled)) { error = L"Delivery cancelled."; return SearchResult::Interrupted; }
    if (!OwnedForeground(browser) || FAILED(chosen->SetFocus()) ||
        !WaitForFocus(automation, chosen.get(), browser, cancelled) ||
        !Chord(browser, VK_RETURN, false, false, [&] { return !Cancelled(cancelled) && FocusIs(automation, chosen.get()); })) {
        error = L"The ChatGPT tab could not be activated safely.";
        return DiscoveryFailure(browser, cancelled);
    }
    if (!Pause(300, cancelled)) { error = L"Delivery cancelled."; return SearchResult::Interrupted; }
    target = GetForegroundWindow();
    if (!IsChrome(target)) {
        error = L"The Chrome tab activation could not be confirmed. Nothing was pasted.";
        return SearchResult::Interrupted;
    }
    restore.Touch(target);
    std::wstring address = Address(automation, target);
    if (address.empty()) {
        error = L"The selected Chrome tab did not have the exact chatgpt.com address. Nothing was pasted.";
        return DiscoveryFailure(target, cancelled);
    }
    return ChatGPTAddress(address) ? SearchResult::Found : SearchResult::SkipRoute;
}

SearchResult FindChatGPTTab(IUIAutomation* automation, HWND browser, HWND& target,
        RestoreWindows& restore, std::wstring& error, const std::function<bool()>& cancelled) {
    std::vector<std::wstring> visitedTabs;
    for (int i = 0; i < 50; ++i) {
        auto result = FindChatGPTTabOnce(automation, browser, target, restore, error, cancelled, visitedTabs);
        if (result != SearchResult::SkipRoute) return result;
    }
    error = L"Too many ChatGPT product or shared-page tabs were encountered. Open a writable ChatGPT chat as the active Chrome tab and try again.";
    return SearchResult::Failed;
}

std::wstring ChromeExecutable() {
    for (HKEY hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        wchar_t value[32768]{}; DWORD bytes = sizeof(value);
        if (RegGetValueW(hive, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\chrome.exe",
                nullptr, RRF_RT_REG_SZ, nullptr, value, &bytes) == ERROR_SUCCESS &&
            GetFileAttributesW(value) != INVALID_FILE_ATTRIBUTES) return value;
    }
    for (const wchar_t* variable : {L"LOCALAPPDATA", L"PROGRAMFILES", L"PROGRAMFILES(X86)"}) {
        wchar_t value[32768]{};
        if (!GetEnvironmentVariableW(variable, value, 32768)) continue;
        std::wstring path = std::wstring(value) + L"\\Google\\Chrome\\Application\\chrome.exe";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    }
    return {};
}

bool OpenChatGPT(IUIAutomation* automation, RestoreWindows& restore, HWND& target,
        int delay, std::wstring& error, const std::function<bool()>& cancelled) {
    if (Cancelled(cancelled)) { error = L"Delivery cancelled."; return false; }
    std::wstring executable = ChromeExecutable();
    if (executable.empty()) { error = L"Google Chrome was not found. Install Chrome and open chatgpt.com first."; return false; }
    SHELLEXECUTEINFOW launch{}; launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS; launch.lpFile = executable.c_str();
    launch.lpParameters = L"https://chatgpt.com/"; launch.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&launch)) { error = L"Chrome could not open chatgpt.com."; return false; }
    if (launch.hProcess) CloseHandle(launch.hProcess);
    ULONGLONG until = GetTickCount64() + std::max(delay, 3000);
    do {
        if (!Pause(200, cancelled)) { error = L"Delivery cancelled."; return false; }
        HWND foreground = GetForegroundWindow();
        if (IsChrome(foreground) && ChatGPTAddress(Address(automation, foreground))) {
            target = foreground; restore.Touch(target); return true;
        }
    } while (GetTickCount64() < until);
    error = L"Chrome opened, but Clippy could not confirm the chatgpt.com tab. Open that tab and try again.";
    return false;
}

Com<IUIAutomationElement> Composer(IUIAutomation* automation, HWND browser) {
    auto root = Root(automation, browser);
    // A plain contenteditable DIV maps to a UIA Group when it has no explicit
    // textbox role. Its exact author ID still identifies the composer safely.
    auto exact = FindId(automation, root.get(), L"prompt-textarea");
    BOOL focusable = FALSE;
    if (exact && Enabled(exact.get()) &&
        SUCCEEDED(exact->get_CurrentIsKeyboardFocusable(&focusable)) && focusable &&
        HasDocumentAncestor(automation, exact.get())) return exact;
    auto edits = FindType(automation, root.get(), UIA_EditControlTypeId);
    // The current ChatGPT Chat/Work editor also exposes an Edit named "Work
    // with ChatGPT", without an HTML id or a ProseMirror class. Accept known
    // composer labels only, with a focusable field inside the page document.
    for (auto& edit : edits) {
        if (!Enabled(edit.get()) || !HasDocumentAncestor(automation, edit.get())) continue;
        std::wstring name = Lower(TextProperty(edit.get(), UIA_NamePropertyId));
        std::wstring cls = Lower(TextProperty(edit.get(), UIA_ClassNamePropertyId));
        BOOL editFocusable = FALSE;
        if (FAILED(edit->get_CurrentIsKeyboardFocusable(&editFocusable)) || !editFocusable) continue;
        if (name == L"work with chatgpt" || name == L"chat with chatgpt" ||
            name == L"message chatgpt" || name == L"ask anything" || name == L"ask chatgpt" ||
            (name == L"message" && cls.find(L"prosemirror") != std::wstring::npos)) return edit;
    }
    return {};
}

bool IsSend(const std::wstring& name, const std::wstring& id) {
    return name == L"send" || name == L"send prompt" || name == L"send message" || id == L"send-button";
}

bool IsStop(const std::wstring& name) {
    return name == L"stop" || name == L"stop generating" || name == L"stop streaming" || name == L"stop response";
}

Com<IUIAutomationElement> ComposerScope(IUIAutomation* automation, IUIAutomationElement* composer) {
    Com<IUIAutomationElement> scope(composer); composer->AddRef();
    Com<IUIAutomationElement> last = scope;
    for (int i = 0; i < 30; ++i) {
        auto parent = Parent(automation, scope.get());
        if (!parent) break;
        scope = std::move(parent);
        std::wstring role = Lower(TextProperty(scope.get(), UIA_AriaRolePropertyId));
        std::wstring cls = Lower(TextProperty(scope.get(), UIA_ClassNamePropertyId));
        std::wstring id = Lower(TextProperty(scope.get(), UIA_AutomationIdPropertyId));
        CONTROLTYPEID type = 0; scope->get_CurrentControlType(&type);
        if (role == L"form" || cls.find(L"composer-parent") != std::wstring::npos || id == L"composer") return scope;
        // In the current Work/Chat page, the editor and controls share a draft
        // Group without the older form/class/id. Find their nearest common
        // ancestor so transcript controls cannot be mistaken for draft uploads.
        if (type != UIA_DocumentControlTypeId) {
            for (auto& button : FindType(automation, scope.get(), UIA_ButtonControlTypeId)) {
                std::wstring name = Lower(TextProperty(button.get(), UIA_NamePropertyId));
                std::wstring buttonId = Lower(TextProperty(button.get(), UIA_AutomationIdPropertyId));
                if (IsSend(name, buttonId) || IsStop(name) || name == L"add files and more") return scope;
            }
        }
        if (type == UIA_DocumentControlTypeId) return scope;
        last = scope;
    }
    return last;
}

struct DraftState {
    int attachments = 0;
    bool uploading = false;
    bool error = false;
    bool generating = false;
    Com<IUIAutomationElement> sendButton;
};

DraftState ReadDraft(IUIAutomation* automation, IUIAutomationElement* scope) {
    DraftState state;
    auto buttons = FindType(automation, scope, UIA_ButtonControlTypeId);
    for (auto& button : buttons) {
        BOOL offscreen = TRUE;
        if (FAILED(button->get_CurrentIsOffscreen(&offscreen)) || offscreen) continue;
        std::wstring name = Lower(TextProperty(button.get(), UIA_NamePropertyId));
        std::wstring id = Lower(TextProperty(button.get(), UIA_AutomationIdPropertyId));
        bool removeImage = name.rfind(L"remove ", 0) == 0 &&
            (name.find(L".png") != std::wstring::npos || name.find(L".jpg") != std::wstring::npos ||
             name.find(L".jpeg") != std::wstring::npos || name.find(L".webp") != std::wstring::npos ||
             name.find(L".gif") != std::wstring::npos || name == L"remove image");
        if (name == L"remove file" || name == L"remove attachment" ||
            name.rfind(L"remove file ", 0) == 0 || name.rfind(L"remove attachment ", 0) == 0 || removeImage)
            ++state.attachments;
        if (IsSend(name, id)) state.sendButton = button;
        if (IsStop(name)) state.generating = true;
    }
    auto progress = FindType(automation, scope, UIA_ProgressBarControlTypeId);
    for (auto& element : progress) if (Enabled(element.get())) state.uploading = true;
    for (auto& element : FindType(automation, scope, UIA_TextControlTypeId)) {
        BOOL offscreen = TRUE;
        if (FAILED(element->get_CurrentIsOffscreen(&offscreen)) || offscreen) continue;
        std::wstring name = Lower(TextProperty(element.get(), UIA_NamePropertyId));
        if (name == L"uploading" || name == L"uploading..." || name == L"uploading…" ||
            name.rfind(L"uploading file", 0) == 0) state.uploading = true;
        if (name.find(L"upload failed") != std::wstring::npos ||
            name.find(L"unable to upload") != std::wstring::npos ||
            name.find(L"file upload limit") != std::wstring::npos) state.error = true;
    }
    return state;
}

bool Invoke(IUIAutomationElement* element, const std::function<bool()>& finalGuard = {}) {
    if (StopRequested()) return false;
    Com<IUIAutomationInvokePattern> invoke;
    if (!element || FAILED(element->GetCurrentPatternAs(UIA_InvokePatternId,
        IID_IUIAutomationInvokePattern, reinterpret_cast<void**>(invoke.put())))) return false;
    if (StopRequested() || (finalGuard && !finalGuard()) || StopRequested()) return false;
    return SUCCEEDED(invoke->Invoke());
}

} // namespace

DeliveryResult DeliverToChatGPT(const Settings& settings, bool send, HWND previousWindow,
        const std::function<void(const std::wstring&)>& report, const std::function<bool()>& cancelled) {
    auto progress = [&](const std::wstring& message) { if (report) report(message); };
    auto cancelledResult = [] { return DeliveryResult{false, false, L"Delivery cancelled. Check the ChatGPT draft if a paste had already occurred."}; };
    if (Cancelled(cancelled)) return cancelledResult();
    CancellationContext cancellationContext(cancelled);
    DWORD clipboardSequence = GetClipboardSequenceNumber();
    if (!IsClipboardFormatAvailable(CF_DIB) && !IsClipboardFormatAvailable(CF_DIBV5) &&
        !IsClipboardFormatAvailable(CF_BITMAP))
        return {false, false, L"The screenshot is no longer available on the clipboard. Nothing was pasted."};
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) return {false, false, L"Windows accessibility could not initialize on the delivery thread."};
    struct EndCOM { ~EndCOM() { CoUninitialize(); } } endCOM;
    Com<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
            IID_IUIAutomation, reinterpret_cast<void**>(automation.put()))))
        return {false, false, L"Windows UI Automation is unavailable."};
    Com<IUIAutomation2> automation2;
    if (SUCCEEDED(automation->QueryInterface(IID_IUIAutomation2,
            reinterpret_cast<void**>(automation2.put())))) {
        automation2->put_ConnectionTimeout(3000);
        automation2->put_TransactionTimeout(3000);
    }
    RestoreWindows restore(automation.get(), previousWindow, settings.minimizeChrome);
    auto windows = ChromeWindows();
    // Remember every browser's initial tab before Tab Search can activate a
    // matching tab in another window of the same Chrome profile.
    for (HWND window : windows) {
        if (Cancelled(cancelled)) return cancelledResult();
        restore.Remember(window);
    }
    progress(L"Finding the most recent open ChatGPT tab...");
    HWND target = nullptr;
    std::wstring error;
    bool found = false;
    // Inspect every existing browser before opening Tab Search. The first
    // Chrome window may be an inaccessible app/profile window while an active
    // ChatGPT conversation is already available in a later window.
    for (HWND window : windows) {
        if (Cancelled(cancelled)) return cancelledResult();
        if (ChatGPTAddress(Address(automation.get(), window))) {
            target = window; found = true; break;
        }
    }
    std::wstring discoveryFailure;
    for (HWND window : windows) {
        if (found) break;
        auto result = FindChatGPTTab(automation.get(), window, target, restore, error, cancelled);
        if (result == SearchResult::Found) { found = true; break; }
        if (result == SearchResult::Interrupted || Cancelled(cancelled)) return {false, false, error};
        if (result == SearchResult::Failed && discoveryFailure.empty()) discoveryFailure = error;
    }
    bool opened = false;
    if (!found) {
        // Failed accessibility discovery is not evidence that a tab is absent.
        // Only open a new chat after all available browsers were searched.
        if (!discoveryFailure.empty()) return {false, false, discoveryFailure};
        progress(L"Opening ChatGPT in Chrome...");
        if (!OpenChatGPT(automation.get(), restore, target, settings.loadDelayMs, error, cancelled)) return {false, false, error};
        opened = true;
    }
    restore.Destination(target);
    if (Cancelled(cancelled)) return cancelledResult();
    if (!Activate(target)) return {false, false, L"Windows would not activate the ChatGPT Chrome window."};
    if (opened && settings.loadDelayMs > 0) {
        progress(L"Waiting for ChatGPT to load...");
        if (!Pause(settings.loadDelayMs, cancelled)) return cancelledResult();
    }
    std::wstring destinationAddress = Address(automation.get(), target);
    auto destinationTab = SelectedTab(automation.get(), target);
    Com<IUIAutomationSelectionItemPattern> destinationSelection;
    if (destinationTab)
        destinationTab->GetCurrentPatternAs(UIA_SelectionItemPatternId,
            IID_IUIAutomationSelectionItemPattern, reinterpret_cast<void**>(destinationSelection.put()));
    auto sameDestination = [&](bool afterSend = false) {
        if (GetForegroundWindow() != target) return false;
        std::wstring address = Address(automation.get(), target);
        if (!ChatGPTAddress(address) || (!afterSend && address != destinationAddress)) return false;
        BOOL selected = FALSE;
        return !destinationSelection || (SUCCEEDED(destinationSelection->get_CurrentIsSelected(&selected)) && selected);
    };
    Com<IUIAutomationElement> composer;
    ULONGLONG composerUntil = GetTickCount64() + 4000;
    do {
        if (Cancelled(cancelled)) return cancelledResult();
        if (GetForegroundWindow() != target)
            return {false, false, L"The foreground window changed. Nothing was pasted."};
        if (!sameDestination())
            return {false, false, L"The destination Chrome tab changed. Nothing was pasted."};
        composer = Composer(automation.get(), target);
        if (composer) break;
        Pause(150, cancelled);
    } while (GetTickCount64() < composerUntil);
    if (!composer)
        return {false, false, L"Clippy could not identify ChatGPT's message box through Windows accessibility. Open a signed-in ChatGPT chat in Chrome, then try again. Nothing was pasted."};
    auto scope = ComposerScope(automation.get(), composer.get());
    DraftState before = ReadDraft(automation.get(), scope.get());
    if (before.generating)
        return {false, false, L"ChatGPT is still responding. Wait for the response to finish and try again."};
    progress(L"Pasting the screenshot into the ChatGPT draft...");
    if (Cancelled(cancelled)) return cancelledResult();
    if (GetClipboardSequenceNumber() != clipboardSequence)
        return {false, false, L"The clipboard changed while finding ChatGPT. Nothing was pasted; capture the screenshot again."};
    if (!ModifiersUp() || !sameDestination() || FAILED(composer->SetFocus()) ||
        !WaitForFocus(automation.get(), composer.get(), target, cancelled))
        return {false, false, L"The ChatGPT message box could not be focused safely. Nothing was pasted."};
    auto focusedComposer = [&] {
        return !Cancelled(cancelled) && sameDestination() &&
            GetClipboardSequenceNumber() == clipboardSequence && FocusIs(automation.get(), composer.get());
    };
    // Collapse any existing editor selection at the end before pasting. This
    // preserves draft text even when the user previously highlighted it.
    if (!Chord(target, VK_END, true, false, focusedComposer))
        return {false, false, L"The ChatGPT draft could not be focused safely. Nothing was pasted."};
    // One screenshot paste only. The screenshot remains in the clipboard.
    if (!Chord(target, 'V', true, false, focusedComposer))
        return {false, false, L"The paste keystroke could not be delivered completely. Check the ChatGPT draft before trying again."};
    progress(send ? L"Waiting for the new screenshot upload before sending..." : L"Checking the screenshot attachment...");
    ULONGLONG until = GetTickCount64() + std::max(settings.uploadTimeoutMs, 3000);
    DraftState ready;
    ULONGLONG stableSince = 0;
    bool attached = false;
    do {
        if (!Pause(200, cancelled)) return cancelledResult();
        if (!sameDestination())
            return {false, false, L"The window or tab changed after the paste. The draft may contain the screenshot; nothing was sent. Check it before trying again."};
        // ChatGPT may replace the draft Group when an attachment is added.
        // Reacquire both editor and scope rather than retaining a stale React
        // accessibility element for upload readiness or the Send button.
        auto currentComposer = Composer(automation.get(), target);
        if (!currentComposer) { stableSince = 0; continue; }
        composer = std::move(currentComposer);
        scope = ComposerScope(automation.get(), composer.get());
        ready = ReadDraft(automation.get(), scope.get());
        if (ready.error)
            return {false, false, L"ChatGPT reported an upload failure. Nothing was sent; check the draft."};
        attached = ready.attachments > before.attachments;
        bool usable = attached && !ready.uploading && ready.sendButton && Enabled(ready.sendButton.get());
        if (usable) {
            if (!stableSince) stableSince = GetTickCount64();
            if (GetTickCount64() - stableSince >= 600) break;
        } else stableSince = 0;
    } while (GetTickCount64() < until);
    if (!stableSince || GetTickCount64() - stableSince < 600)
        return {false, false, attached
            ? L"The screenshot was attached, but upload readiness could not be confirmed. Nothing was sent; check the draft."
            : L"The paste was attempted once, but a new image attachment could not be confirmed. Nothing was sent; check the draft before trying again."};
    if (!send) return {true, false, L"Screenshot added to the ChatGPT draft. The screenshot is still on the clipboard."};
    // Recheck the exact target and focused composer immediately before the
    // single Invoke. No Enter-based fallback and no automatic send retries.
    if (Cancelled(cancelled)) return cancelledResult();
    if (!sameDestination() ||
        !FocusIs(automation.get(), composer.get()) || !Enabled(ready.sendButton.get()))
        return {false, false, L"The screenshot was attached, but the send target changed. Nothing was sent."};
    progress(L"Sending the ChatGPT draft...");
    if (Cancelled(cancelled)) return cancelledResult();
    if (!Invoke(ready.sendButton.get(), [&] {
        return sameDestination() &&
            FocusIs(automation.get(), composer.get()) && Enabled(ready.sendButton.get());
    }))
        return {false, false, L"The send action could not be confirmed. Check ChatGPT before sending again; Clippy will not retry."};
    ULONGLONG sentUntil = GetTickCount64() + 3000;
    do {
        if (!Pause(200, cancelled))
            return {false, true, L"Delivery cancelled after the Send button was activated once. Check ChatGPT before sending again."};
        if (!sameDestination(true))
            return {false, true, L"The window or tab changed after Send was activated once. Check ChatGPT before sending again."};
        auto currentScope = Composer(automation.get(), target);
        if (!currentScope) continue;
        currentScope = ComposerScope(automation.get(), currentScope.get());
        DraftState current = ReadDraft(automation.get(), currentScope.get());
        if (current.generating || current.attachments == 0)
            return {true, true, L"Screenshot and the accumulated ChatGPT draft were sent. The screenshot is still on the clipboard."};
    } while (GetTickCount64() < sentUntil);
    return {false, true, L"The Send button was activated once, but submission was not confirmed. Check ChatGPT before sending again."};
}

bool ProbeChrome(std::wstring& report) {
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) { report = L"Windows accessibility could not initialize."; return false; }
    struct EndCOM { ~EndCOM() { CoUninitialize(); } } endCOM;
    Com<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
            IID_IUIAutomation, reinterpret_cast<void**>(automation.put())))) {
        report = L"Windows UI Automation is unavailable."; return false;
    }
    Com<IUIAutomation2> automation2;
    if (SUCCEEDED(automation->QueryInterface(IID_IUIAutomation2,
            reinterpret_cast<void**>(automation2.put())))) {
        automation2->put_ConnectionTimeout(3000); automation2->put_TransactionTimeout(3000);
    }
    auto windows = ChromeWindows();
    int addresses = 0, chatGPT = 0, writableRoutes = 0, otherRoutes = 0, composers = 0, sendControls = 0;
    std::wstring diagnostics;
    int windowNumber = 0;
    for (HWND window : windows) {
        ++windowNumber;
        auto root = Root(automation.get(), window);
        std::wstring address = Address(automation.get(), window);
        if (!address.empty()) ++addresses;
        diagnostics += L"\nWindow " + std::to_wstring(windowNumber) +
            L": minimized=" + std::to_wstring(IsIconic(window) != FALSE) +
            L", UIA root=" + std::to_wstring(bool(root)) +
            L", readable address=" + std::to_wstring(!address.empty()) +
            L", chat route=" + std::to_wstring(ChatGPTAddress(address));
        auto prompt = FindId(automation.get(), root.get(), L"prompt-textarea");
        diagnostics += L", prompt ID=" + std::to_wstring(bool(prompt));
        if (prompt) {
            CONTROLTYPEID type = 0; BOOL enabled = FALSE, offscreen = TRUE, focusable = FALSE;
            prompt->get_CurrentControlType(&type);
            prompt->get_CurrentIsEnabled(&enabled); prompt->get_CurrentIsOffscreen(&offscreen);
            prompt->get_CurrentIsKeyboardFocusable(&focusable);
            diagnostics += L", type=" + std::to_wstring(type) +
                L", enabled=" + std::to_wstring(enabled != FALSE) +
                L", offscreen=" + std::to_wstring(offscreen != FALSE) +
                L", focusable=" + std::to_wstring(focusable != FALSE) +
                L", document ancestor=" + std::to_wstring(HasDocumentAncestor(automation.get(), prompt.get()));
        }
        if (!ChatGPTHost(address)) continue;
        ++chatGPT;
        if (!ChatGPTAddress(address)) { ++otherRoutes; continue; }
        ++writableRoutes;
        auto composer = Composer(automation.get(), window);
        if (composer) {
            ++composers;
            auto scope = ComposerScope(automation.get(), composer.get());
            if (ReadDraft(automation.get(), scope.get()).sendButton) ++sendControls;
        }
    }
    report = L"Chrome windows: " + std::to_wstring(windows.size()) +
        L"\nReadable address bars: " + std::to_wstring(addresses) +
        L"\nCurrently active ChatGPT tabs: " + std::to_wstring(chatGPT) +
        L"\nActive writable-chat routes: " + std::to_wstring(writableRoutes) +
        L"\nOther ChatGPT routes: " + std::to_wstring(otherRoutes) +
        L"\nRecognized ChatGPT composers: " + std::to_wstring(composers) +
        L"\nRecognized ChatGPT send controls: " + std::to_wstring(sendControls) +
        diagnostics +
        L"\nRead-only probe: no keys, clipboard changes, tab switches, or messages were sent.";
    return windows.empty() || addresses > 0;
}

} // namespace clippy
