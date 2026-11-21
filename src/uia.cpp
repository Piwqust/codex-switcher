// What a screen reader gets from the hand-drawn windows: UI Automation.
//
// Every window draws itself, so Windows knows nothing of what is inside. Each frame already
// leaves a list of the controls it registered and the text it drew; this file hands that list
// to UI Automation as the window's children: a control with its name, what it is and its
// state, and the text that belongs to no control. Narrator can then read a window, move
// through it and press what it finds.
//
// A provider keeps no pointers into a frame. It remembers the control's id and looks it up
// again on every call, so a control that is gone answers "not available" instead of crashing.
#include "gfx.h"
#include <uiautomation.h>
#include <algorithm>

namespace {
    struct Node {
        UINT key = 0; bool text = false; RectF r;
        std::wstring name, help;
        Role role = Role::Button; int state = -1; bool focusable = false;
    };

    // Text has no id of its own: its words and place stand in for one. Even, so it never
    // meets a control's id, which is always odd.
    UINT TextKey(const TextRun& t) {
        UINT h = 2166136261u;
        for (wchar_t c : t.text) h = (h ^ (UINT)c) * 16777619u;
        h = (h ^ (UINT)(int)t.r.x) * 16777619u;
        h = (h ^ (UINT)(int)t.r.y) * 16777619u;
        h &= ~1u;
        return h ? h : 2;
    }

    // The window's children, top to bottom and left to right: the named controls, and the
    // text that isn't the label of one.
    std::vector<Node> Nodes(Surface* s) {
        std::vector<Node> out;
        for (auto& g : s->Regions()) {
            if (g.name.empty()) continue;
            bool again = false;
            for (auto& n : out) if (n.key == g.id) again = true;
            if (again) continue;
            Node n; n.key = g.id; n.r = g.bounds; n.name = g.name; n.help = g.tip == g.name ? L"" : g.tip;
            n.role = g.role; n.state = g.state; n.focusable = g.focusable;
            out.push_back(std::move(n));
        }
        size_t controls = out.size();
        for (auto& t : s->Texts()) {
            float cx = t.r.x + t.r.w / 2, cy = t.r.y + t.r.h / 2;
            bool label = false;
            for (size_t i = 0; i < controls && !label; i++) label = out[i].r.Has(cx, cy);
            if (label) continue;
            Node n; n.key = TextKey(t); n.text = true; n.r = t.r; n.name = t.text;
            bool again = false;
            for (auto& o : out) if (o.key == n.key) again = true;
            if (!again) out.push_back(std::move(n));
        }
        std::stable_sort(out.begin(), out.end(), [](const Node& a, const Node& b) {
            int ya = (int)(a.r.y / 10), yb = (int)(b.r.y / 10);
            return ya != yb ? ya < yb : a.r.x < b.r.x;
        });
        return out;
    }

    void Set(VARIANT* v, const std::wstring& s) { v->vt = VT_BSTR; v->bstrVal = SysAllocString(s.c_str()); }
    void Set(VARIANT* v, bool b) { v->vt = VT_BOOL; v->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE; }
    void Set(VARIANT* v, int i) { v->vt = VT_I4; v->lVal = i; }

    class Provider : public IRawElementProviderSimple, public IRawElementProviderFragment, public IRawElementProviderFragmentRoot,
                     public IInvokeProvider, public IToggleProvider, public ISelectionItemProvider {
        LONG refs = 1;
        std::shared_ptr<Surface*> link;
        HWND hwnd;
        UINT key;                              // 0: the window itself

        Surface* S() const { return link ? *link : nullptr; }
        bool Find(Node& n) const {
            auto s = S();
            if (!s) return false;
            for (auto& x : Nodes(s)) if (x.key == key) { n = x; return true; }
            return false;
        }
        Provider* Child(UINT k) const { return new Provider(link, hwnd, k); }

    public:
        Provider(std::shared_ptr<Surface*> l, HWND h, UINT k) : link(std::move(l)), hwnd(h), key(k) {}
        virtual ~Provider() {}

        // ---- IUnknown
        ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
        ULONG STDMETHODCALLTYPE Release() override { LONG n = InterlockedDecrement(&refs); if (!n) delete this; return n; }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
            if (!out) return E_POINTER;
            *out = nullptr;
            if (id == __uuidof(IUnknown) || id == __uuidof(IRawElementProviderSimple)) *out = static_cast<IRawElementProviderSimple*>(this);
            else if (id == __uuidof(IRawElementProviderFragment)) *out = static_cast<IRawElementProviderFragment*>(this);
            else if (id == __uuidof(IRawElementProviderFragmentRoot) && !key) *out = static_cast<IRawElementProviderFragmentRoot*>(this);
            else if (id == __uuidof(IInvokeProvider) && key) *out = static_cast<IInvokeProvider*>(this);
            else if (id == __uuidof(IToggleProvider) && key) *out = static_cast<IToggleProvider*>(this);
            else if (id == __uuidof(ISelectionItemProvider) && key) *out = static_cast<ISelectionItemProvider*>(this);
            else return E_NOINTERFACE;
            AddRef();
            return S_OK;
        }

        // ---- IRawElementProviderSimple
        HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* o) override {
            // Calls come on the window's own thread, between its messages.
            *o = (ProviderOptions)(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID p, IUnknown** out) override {
            *out = nullptr;
            Node n;
            if (!key || !Find(n) || n.text) return S_OK;
            bool toggles = n.role == Role::Toggle || n.role == Role::Check;
            bool chosen = (n.role == Role::Tab || n.role == Role::Radio || n.role == Role::Row) && n.state >= 0;
            if (p == UIA_InvokePatternId && !toggles) *out = static_cast<IInvokeProvider*>(this);
            else if (p == UIA_TogglePatternId && toggles) *out = static_cast<IToggleProvider*>(this);
            else if (p == UIA_SelectionItemPatternId && chosen) *out = static_cast<ISelectionItemProvider*>(this);
            if (*out) AddRef();
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID p, VARIANT* v) override {
            VariantInit(v);
            auto s = S();
            if (!s) return UIA_E_ELEMENTNOTAVAILABLE;
            if (!key) {
                if (p == UIA_ControlTypePropertyId) Set(v, (int)UIA_WindowControlTypeId);
                else if (p == UIA_FrameworkIdPropertyId) Set(v, std::wstring(L"Direct2D"));
                return S_OK;
            }
            Node n;
            if (!Find(n)) return UIA_E_ELEMENTNOTAVAILABLE;
            switch (p) {
            case UIA_ControlTypePropertyId:
                Set(v, (int)(n.text ? UIA_TextControlTypeId : n.role == Role::Check ? UIA_CheckBoxControlTypeId : n.role == Role::Row ? UIA_ListItemControlTypeId
                     : n.role == Role::Tab ? UIA_TabItemControlTypeId : n.role == Role::Radio ? UIA_RadioButtonControlTypeId : UIA_ButtonControlTypeId));
                break;
            case UIA_NamePropertyId: Set(v, n.name); break;
            case UIA_HelpTextPropertyId: if (!n.help.empty()) Set(v, n.help); break;
            case UIA_AutomationIdPropertyId:
                if (n.text) break;
                if (Gfx::idNames && Gfx::idNames->count(n.key)) Set(v, Util::Wide((*Gfx::idNames)[n.key]));
                else Set(v, std::to_wstring(n.key));
                break;
            case UIA_IsControlElementPropertyId: case UIA_IsContentElementPropertyId: case UIA_IsEnabledPropertyId: Set(v, true); break;
            case UIA_IsKeyboardFocusablePropertyId: Set(v, n.focusable); break;
            case UIA_HasKeyboardFocusPropertyId: Set(v, n.focusable && s->focus == n.key && ::GetFocus() == hwnd); break;
            case UIA_IsOffscreenPropertyId: { auto c = s->Client(); Set(v, n.r.B() <= 0 || n.r.y >= c.h || n.r.R() <= 0 || n.r.x >= c.w); break; }
            case UIA_FrameworkIdPropertyId: Set(v, std::wstring(L"Direct2D")); break;
            }
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** out) override {
            *out = nullptr;
            return key ? S_OK : UiaHostProviderFromHwnd(hwnd, out);
        }

        // ---- IRawElementProviderFragment
        HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection d, IRawElementProviderFragment** out) override {
            *out = nullptr;
            auto s = S();
            if (!s) return UIA_E_ELEMENTNOTAVAILABLE;
            auto all = Nodes(s);
            if (!key) {
                if (all.empty()) return S_OK;
                if (d == NavigateDirection_FirstChild) *out = Child(all.front().key);
                else if (d == NavigateDirection_LastChild) *out = Child(all.back().key);
                return S_OK;
            }
            if (d == NavigateDirection_Parent) { *out = new Provider(link, hwnd, 0); return S_OK; }
            int at = -1;
            for (int i = 0; i < (int)all.size(); i++) if (all[i].key == key) at = i;
            if (at < 0) return UIA_E_ELEMENTNOTAVAILABLE;
            if (d == NavigateDirection_NextSibling && at + 1 < (int)all.size()) *out = Child(all[at + 1].key);
            else if (d == NavigateDirection_PreviousSibling && at > 0) *out = Child(all[at - 1].key);
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** out) override {
            *out = nullptr;
            if (!key) return S_OK;             // the window: its handle is its id
            int id[] = { UiaAppendRuntimeId, (int)key };
            *out = SafeArrayCreateVector(VT_I4, 0, 2);
            if (!*out) return E_OUTOFMEMORY;
            for (LONG i = 0; i < 2; i++) SafeArrayPutElement(*out, &i, &id[i]);
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* r) override {
            *r = {};
            if (!key) return S_OK;             // the window: its host answers
            auto s = S(); Node n;
            if (!s || !Find(n)) return UIA_E_ELEMENTNOTAVAILABLE;
            POINT p{ (LONG)(n.r.x * s->scale), (LONG)(n.r.y * s->scale) };
            ClientToScreen(hwnd, &p);
            *r = { (double)p.x, (double)p.y, (double)(n.r.w * s->scale), (double)(std::max(0.f, n.r.h) * s->scale) };
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** out) override { *out = nullptr; return S_OK; }
        HRESULT STDMETHODCALLTYPE SetFocus() override {
            auto s = S();
            if (!s) return UIA_E_ELEMENTNOTAVAILABLE;
            if (key) s->FocusOn(key);
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** out) override {
            *out = static_cast<IRawElementProviderFragmentRoot*>(new Provider(link, hwnd, 0));
            return S_OK;
        }

        // ---- IRawElementProviderFragmentRoot
        HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** out) override {
            *out = nullptr;
            auto s = S();
            if (!s) return UIA_E_ELEMENTNOTAVAILABLE;
            POINT p{ (LONG)x, (LONG)y };
            ScreenToClient(hwnd, &p);
            float px = p.x / s->scale, py = p.y / s->scale;
            // The smallest thing under the pointer: a button inside a row rather than the row.
            const Node* best = nullptr;
            auto all = Nodes(s);
            for (auto& n : all) if (n.r.Has(px, py) && (!best || n.r.w * n.r.h < best->r.w * best->r.h)) best = &n;
            if (best) *out = Child(best->key);
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** out) override {
            *out = nullptr;
            auto s = S();
            if (!s) return UIA_E_ELEMENTNOTAVAILABLE;
            for (auto& n : Nodes(s)) if (!n.text && n.focusable && n.key == s->focus) { *out = Child(n.key); break; }
            return S_OK;
        }

        // ---- pressing, switching and choosing: all of them a click, done after this call
        // returns, since a click may open a dialog that runs its own loop.
        HRESULT Press() { auto s = S(); Node n; if (!s || !Find(n) || n.text) return UIA_E_ELEMENTNOTAVAILABLE; s->Activate(key); return S_OK; }
        HRESULT STDMETHODCALLTYPE Invoke() override { return Press(); }
        HRESULT STDMETHODCALLTYPE Toggle() override { return Press(); }
        HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* st) override {
            Node n;
            if (!Find(n)) return UIA_E_ELEMENTNOTAVAILABLE;
            *st = n.state == 1 ? ToggleState_On : n.state == 0 ? ToggleState_Off : ToggleState_Indeterminate;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE Select() override { return Press(); }
        HRESULT STDMETHODCALLTYPE AddToSelection() override { return UIA_E_INVALIDOPERATION; }
        HRESULT STDMETHODCALLTYPE RemoveFromSelection() override { return UIA_E_INVALIDOPERATION; }
        HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* on) override {
            Node n;
            if (!Find(n)) return UIA_E_ELEMENTNOTAVAILABLE;
            *on = n.state == 1;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** out) override { *out = nullptr; return S_OK; }
    };

    // One provider per window for as long as the window lives.
    std::unordered_map<HWND, Provider*> roots;
}

namespace Acc {
    bool Answer(Surface* s, WPARAM w, LPARAM l, LRESULT& result) {
        if ((long)l != (long)UiaRootObjectId || !s->link) return false;
        auto& root = roots[s->hwnd];
        if (!root) root = new Provider(s->link, s->hwnd, 0);
        result = UiaReturnRawElementProvider(s->hwnd, w, l, static_cast<IRawElementProviderSimple*>(root));
        return true;
    }

    void Gone(HWND h) {
        auto it = roots.find(h);
        if (it == roots.end()) return;
        UiaReturnRawElementProvider(h, 0, 0, nullptr);
        UiaDisconnectProvider(static_cast<IRawElementProviderSimple*>(it->second));
        it->second->Release();
        roots.erase(it);
    }

    void FocusChanged(Surface* s) {
        if (!s->link || !roots.count(s->hwnd) || !UiaClientsAreListening()) return;
        for (auto& g : s->Regions()) if (g.id == s->focus && !g.name.empty()) {
            auto p = new Provider(s->link, s->hwnd, g.id);
            UiaRaiseAutomationEvent(static_cast<IRawElementProviderSimple*>(p), UIA_AutomationFocusChangedEventId);
            p->Release();
            return;
        }
    }
}
