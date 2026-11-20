namespace {
    // Bumped whenever the drawing changes: Explorer caches icons by path, so a new look
    // needs new file names to show up.
    const wchar_t* IconStyle = L"v1";

    template <class T> struct Com { T* p = nullptr; ~Com() { if (p) p->Release(); } T** operator&() { return &p; } T* operator->() { return p; } operator T*() { return p; } };

    D2D1_COLOR_F Lighten(D2D1_COLOR_F c, float t) { return D2D1::ColorF(c.r + (1 - c.r) * t, c.g + (1 - c.g) * t, c.b + (1 - c.b) * t, c.a); }
    D2D1_COLOR_F Darken(D2D1_COLOR_F c, float t) { return D2D1::ColorF(c.r * (1 - t), c.g * (1 - t), c.b * (1 - t), c.a); }

    // Top-lit vertical gradient, the way Fluent icons shade a solid shape.
    Com<ID2D1LinearGradientBrush> Gradient(ID2D1RenderTarget* rt, D2D1_COLOR_F top, D2D1_COLOR_F bottom, float y0, float y1) {
        D2D1_GRADIENT_STOP stops[] = { { 0, top }, { 1, bottom } };
        Com<ID2D1GradientStopCollection> coll; rt->CreateGradientStopCollection(stops, 2, &coll);
        Com<ID2D1LinearGradientBrush> b;
        rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, y0 }, { 0, y1 }), coll, &b);
        return b;
    }

    // Everything outside a circle: drawing through it cuts the gap that keeps the front coin
    // apart from the one behind, instead of painting a white ring.
    Com<ID2D1PathGeometry> Outside(float cx, float cy, float r, float size) {
        Com<ID2D1RectangleGeometry> all; D2D->CreateRectangleGeometry(D2D1::RectF(-1, -1, size + 1, size + 1), &all);
        Com<ID2D1EllipseGeometry> hole; D2D->CreateEllipseGeometry(D2D1::Ellipse({ cx, cy }, r, r), &hole);
        Com<ID2D1PathGeometry> path; D2D->CreatePathGeometry(&path);
        Com<ID2D1GeometrySink> sink; path->Open(&sink);
        all->CombineWithGeometry(hole, D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink);
        sink->Close();
        return path;
    }

    void PushMask(ID2D1RenderTarget* rt, ID2D1Geometry* g) {
        rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
    }

    // A person, as in the Windows "people" glyph: a head and the top of the shoulders,
    // clipped to the coin it sits on.
    void Person(ID2D1RenderTarget* rt, float cx, float cy, float r, ID2D1Brush* b) {
        rt->FillEllipse(D2D1::Ellipse({ cx, cy - r * .22f }, r * .33f, r * .33f), b);
        Com<ID2D1EllipseGeometry> coin; D2D->CreateEllipseGeometry(D2D1::Ellipse({ cx, cy }, r * .999f, r * .999f), &coin);
        PushMask(rt, coin);
        rt->FillEllipse(D2D1::Ellipse({ cx, cy + r * .80f }, r * .66f, r * .52f), b);
        rt->PopLayer();
    }

    // A terminal prompt, ">_": what Codex is to most people who switch its accounts.
    void Prompt(ID2D1RenderTarget* rt, float cx, float cy, float r, ID2D1Brush* b) {
        float w = std::max(1.5f, r * .17f);
        Com<ID2D1StrokeStyle> round;
        D2D->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &round);
        float x0 = cx - r * .46f, x1 = cx - r * .10f, y0 = cy - r * .30f, y1 = cy + r * .30f;
        rt->DrawLine({ x0, y0 }, { x1, cy }, b, w, round);
        rt->DrawLine({ x1, cy }, { x0, y1 }, b, w, round);
        rt->DrawLine({ cx + r * .06f, y1 }, { cx + r * .46f, y1 }, b, w, round);
    }

    // The switcher's own mark: two coins, a person behind, a prompt in front, in Codex's
    // black and white. `light` turns the front coin white with a black prompt, for a dark
    // taskbar, where the black coin would vanish.
    void DrawAppStyled(ID2D1RenderTarget* rt, float s, bool light) {
        float k = s / 256.f;
        auto indigo = light ? Rgba(0xF4F4F6) : Rgba(0x2A2A2E), slate = Rgba(0x8E8EA0);
        float bx = 92 * k, by = 96 * k, br = 78 * k;      // back coin
        float fx = 158 * k, fy = 158 * k, fr = 92 * k;    // front coin
        float gap = std::max(1.f, 10 * k);
        auto out = Outside(fx, fy, fr + gap, s);
        PushMask(rt, out);
        auto back = Gradient(rt, Lighten(slate, .16f), Darken(slate, .14f), by - br, by + br);
        rt->FillEllipse(D2D1::Ellipse({ bx, by }, br, br), back);
        if (s >= 24) {
            Com<ID2D1SolidColorBrush> soft; rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, .78f), &soft);
            Person(rt, bx, by, br, soft);
        }
        rt->PopLayer();
        auto front = Gradient(rt, Lighten(indigo, .16f), Darken(indigo, light ? .06f : .14f), fy - fr, fy + fr);
        rt->FillEllipse(D2D1::Ellipse({ fx, fy }, fr, fr), front);
        Com<ID2D1SolidColorBrush> ink; rt->CreateSolidColorBrush(light ? D2D1::ColorF(0x161618) : D2D1::ColorF(1, 1, 1, 1), &ink);
        // Small sizes get a bolder prompt, or it blurs into the coin.
        Prompt(rt, fx, fy, s <= 24 ? fr * 1.12f : fr, ink);
        // A hairline on the edge gives the coin a little depth at large sizes.
        if (!light) {
            Com<ID2D1SolidColorBrush> rim; rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, .22f), &rim);
            float w = std::max(1.f, 3 * k);
            rt->DrawEllipse(D2D1::Ellipse({ fx, fy }, fr - w / 2, fr - w / 2), rim, w);
        }
    }
    void DrawApp(ID2D1RenderTarget* rt, float s) { DrawAppStyled(rt, s, false); }

    // An account: its coloured coin with its initials, and a small prompt badge so the
    // shortcut reads as Codex. Below 32 px only the coin and one letter, which stay legible.
    void DrawAccount(ID2D1RenderTarget* rt, float s, const std::wstring& letters, COLORREF color, bool plus) {
        auto base = FromColorref(color);
        float r = s / 2 - std::max(.5f, s * .02f);
        bool badge = s >= 32 && !plus;
        float bd = std::round(s * .40f), bcx = s - bd / 2, bcy = s - bd / 2, gap = std::max(1.f, std::round(s * .04f));
        if (badge) { auto out = Outside(bcx, bcy, bd / 2 + gap, s); PushMask(rt, out); }
        auto fill = Gradient(rt, Lighten(base, .20f), Darken(base, .10f), s / 2 - r, s / 2 + r);
        rt->FillEllipse(D2D1::Ellipse({ s / 2, s / 2 }, r, r), fill);
        Com<ID2D1SolidColorBrush> white; rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
        if (plus) {
            float a = r * .42f, w = std::max(1.5f, r * .17f);
            Com<ID2D1StrokeStyle> round;
            D2D->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &round);
            rt->DrawLine({ s / 2 - a, s / 2 }, { s / 2 + a, s / 2 }, white, w, round);
            rt->DrawLine({ s / 2, s / 2 - a }, { s / 2, s / 2 + a }, white, w, round);
        } else if (s >= 20) {
            auto text = s >= 32 ? letters : letters.substr(0, 1);
            // Shifted up and left a little when the badge takes the corner.
            float off = badge ? s * .06f : 0;
            auto f = Font(s * (text.size() > 1 ? .36f : .44f), DWRITE_FONT_WEIGHT_SEMI_BOLD, true);
            Com<IDWriteTextLayout> l;
            DW->CreateTextLayout(text.c_str(), (UINT32)text.size(), f, s, s, &l);
            l->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); l->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            rt->DrawTextLayout({ -off, -off }, l, white);
        }
        if (!badge) return;
        rt->PopLayer();
        auto dark = Rgba(0x24283B);
        auto bfill = Gradient(rt, Lighten(dark, .12f), dark, bcy - bd / 2, bcy + bd / 2);
        rt->FillEllipse(D2D1::Ellipse({ bcx, bcy }, bd / 2, bd / 2), bfill);
        Prompt(rt, bcx, bcy, bd / 2, white);
    }

    std::vector<BYTE> RenderPng(int size, const std::function<void(ID2D1RenderTarget*, float)>& draw) {
        std::vector<BYTE> out;
        Com<IWICBitmap> bmp; Com<ID2D1RenderTarget> rt;
        WIC->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (!bmp || FAILED(D2D->CreateWicBitmapRenderTarget(bmp, props, &rt))) return out;
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        draw(rt, (float)size);
        rt->EndDraw();

        Com<IStream> stream; CreateStreamOnHGlobal(nullptr, TRUE, &stream);
        Com<IWICBitmapEncoder> enc; Com<IWICBitmapFrameEncode> frame;
        WIC->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
        enc->Initialize(stream, WICBitmapEncoderNoCache);
        enc->CreateNewFrame(&frame, nullptr);
        frame->Initialize(nullptr); frame->SetSize(size, size);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA; frame->SetPixelFormat(&fmt);
        frame->WriteSource(bmp, nullptr); frame->Commit(); enc->Commit();
        STATSTG st; stream->Stat(&st, STATFLAG_NONAME);
        out.resize((size_t)st.cbSize.QuadPart);
        LARGE_INTEGER zero{}; stream->Seek(zero, STREAM_SEEK_SET, nullptr);
        ULONG got; stream->Read(out.data(), (ULONG)out.size(), &got);
        return out;
    }

    void WriteIco(const std::wstring& path, const std::function<void(ID2D1RenderTarget*, float)>& draw) {
        std::vector<int> sizes = { 16, 20, 24, 32, 40, 48, 64, 256 };
        std::vector<std::vector<BYTE>> pngs;
        for (int s : sizes) pngs.push_back(RenderPng(s, draw));
        size_t n = sizes.size();
        std::vector<BYTE> file(6 + 16 * n);
        auto w16 = [&](size_t at, WORD v) { file[at] = (BYTE)v; file[at + 1] = (BYTE)(v >> 8); };
        auto w32 = [&](size_t at, DWORD v) { for (int i = 0; i < 4; i++) file[at + i] = (BYTE)(v >> (8 * i)); };
        w16(2, 1); w16(4, (WORD)n);
        DWORD offset = (DWORD)file.size();
        for (size_t i = 0; i < n; i++) {
            size_t e = 6 + 16 * i;
            file[e] = file[e + 1] = (BYTE)(sizes[i] >= 256 ? 0 : sizes[i]);
            w16(e + 4, 1); w16(e + 6, 32); w32(e + 8, (DWORD)pngs[i].size()); w32(e + 12, offset);
            offset += (DWORD)pngs[i].size();
        }
        for (auto& p : pngs) file.insert(file.end(), p.begin(), p.end());
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD wrote; WriteFile(h, file.data(), (DWORD)file.size(), &wrote, nullptr); CloseHandle(h);
    }

    void RemoveIconsFor(const std::wstring& id) {
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((Paths::IconDir + L"\\*.ico").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        auto lid = Util::Lower(id);
        do {
            std::wstring base = Util::Lower(fd.cFileName); base = base.substr(0, base.size() - 4);
            if (base == lid || base.rfind(lid + L"-", 0) == 0) DeleteFileW((Paths::IconDir + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    std::wstring IconFile(const std::wstring& id, const std::wstring& letters, COLORREF color, bool plus) {
        // The letters and colour are in the file name because Explorer caches icons by path.
        auto file = Paths::IconDir + L"\\" + id + L"-" + letters + Util::Format(L"-%06x-", (unsigned)color) + IconStyle + L".ico";
        if (Util::Exists(file)) return file;
        CreateDirectoryW(Paths::Home.c_str(), nullptr);
        CreateDirectoryW(Paths::IconDir.c_str(), nullptr);
        RemoveIconsFor(id);
        WriteIco(file, [&](ID2D1RenderTarget* rt, float s) { DrawAccount(rt, s, letters, color, plus); });
        return file;
    }
}
