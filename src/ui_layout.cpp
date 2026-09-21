// ============================================================================
//  ui_layout.cpp
// ============================================================================
#include "ui_layout.h"

#include "aero_window.h"      // UiFontFamily()：素材字体注册在那边
#include "assets.h"
#include "image_blob.h"
#include "entity_log.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using namespace Gdiplus;

namespace {

// ---------------------------------------------------------------- 小工具 ----
std::wstring Trim(const std::wstring& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r')) ++a;
    while (b > a && (s[b-1] == L' ' || s[b-1] == L'\t' || s[b-1] == L'\r')) --b;
    return s.substr(a, b - a);
}

std::wstring Lower(const std::wstring& s)
{
    std::wstring r = s;
    for (size_t i = 0; i < r.size(); ++i) r[i] = (wchar_t)towlower(r[i]);
    return r;
}

// 去掉行尾注释：分号或 # 之后的内容（引号内的不算，不过排版文件用不到引号）
std::wstring StripComment(const std::wstring& s)
{
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] == L';' || s[i] == L'#') return Trim(s.substr(0, i));
    return Trim(s);
}

bool ParseInt(const std::wstring& s, int& out)
{
    if (s.empty()) return false;
    wchar_t* end = nullptr;
    const long v = wcstol(s.c_str(), &end, 10);
    if (end == s.c_str()) return false;
    out = (int)v;
    return true;
}

bool ParseFloat(const std::wstring& s, float& out)
{
    if (s.empty()) return false;
    wchar_t* end = nullptr;
    const double v = wcstod(s.c_str(), &end);
    if (end == s.c_str()) return false;
    out = (float)v;
    return true;
}

// "R,G,B" 或 "R,G,B,A"
bool ParseColor(const std::wstring& s, Color& out)
{
    int c[4] = { 255, 255, 255, 255 };
    int n = 0;
    size_t pos = 0;
    while (n < 4 && pos <= s.size())
    {
        const size_t comma = s.find(L',', pos);
        const std::wstring tok = Trim(s.substr(pos, comma == std::wstring::npos
                                                    ? std::wstring::npos : comma - pos));
        int v = 0;
        if (!ParseInt(tok, v)) break;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        c[n++] = v;
        if (comma == std::wstring::npos) break;
        pos = comma + 1;
    }
    if (n < 3) return false;
    out = (n >= 4) ? Color((BYTE)c[3], (BYTE)c[0], (BYTE)c[1], (BYTE)c[2])
                   : Color(255,      (BYTE)c[0], (BYTE)c[1], (BYTE)c[2]);
    return true;
}

// "x,y,w,h"
bool ParseRect(const std::wstring& s, RectF& out)
{
    float v[4] = { 0 };
    int n = 0;
    size_t pos = 0;
    while (n < 4 && pos <= s.size())
    {
        const size_t comma = s.find(L',', pos);
        const std::wstring tok = Trim(s.substr(pos, comma == std::wstring::npos
                                                    ? std::wstring::npos : comma - pos));
        if (!ParseFloat(tok, v[n])) break;
        ++n;
        if (comma == std::wstring::npos) break;
        pos = comma + 1;
    }
    if (n < 4) return false;
    out = RectF(v[0], v[1], v[2], v[3]);
    return true;
}

// ---------------------------------------------------------------- 元素模型 ----
enum Kind { K_RECT, K_IMAGE, K_TEXT };

struct Element {
    Kind  kind    = K_RECT;
    bool  hasRect = false;
    RectF rect{};

    Color color     = Color(255, 255, 255, 255);
    bool  hasBorder = false;
    Color border    = Color(255, 255, 255, 255);
    float borderW   = 2.0f;

    std::wstring file;      // image
    std::wstring raw;       // text 原文（含 \n 与标记）

    float size  = 16.0f;
    bool  bold  = false;
    float lineh = 1.25f;
    int   align = 0;        // 0=left 1=center 2=right
    bool  mono  = true;     // true 用素材字体(Roboto Mono)，false 用雅黑

    // ---- 出场动画（付完钱那个「从小到大渐变出来」）----
    bool  hasPopin   = false;
    DWORD popinDelay = 0;   // 进入本排版后等这么久才开始出场
    DWORD popinMs    = 420; // 出场动画时长

    // travel 阶段（窗口飞向屏幕中心那一段）要不要保留这个元素。
    // 「除了 A90 的头其他都删掉」就是靠它。
    bool  keep = false;
};

// 一套排版。主勒索窗口和付完钱那个窗口各一套。
struct Layout {
    std::vector<Element> elems;
    int   dw = 580, dh = 340;      // 设计坐标系（元素 rect 用的就是这套）
    int   ww = 580, wh = 340;      // 窗口实际内容区 = 设计尺寸 x scale
    float scale  = 1.0f;
    Color bg     = Color(255, 190, 0, 0);
    bool  loaded = false;
};

enum { LAY_MAIN = 0, LAY_PAYUP = 1, LAY_COUNT = 2 };

Layout g_lay[LAY_COUNT];
int    g_active    = LAY_MAIN;
bool   g_travel    = false;      // travel 阶段：只画 keep 元素 + 黑底
DWORD  g_animStart = 0;          // 出场动画的计时起点
std::wstring g_path[LAY_COUNT];

Layout& L() { return g_lay[g_active]; }

// 由设计尺寸和 scale 算出窗口实际尺寸。
//
// 为什么要分两套：元素坐标写在「设计坐标系」里，而窗口可以整体缩放着显示。
// 如果只改 width/height，等于把坐标系本身缩小了——元素还写在 20..460，
// 设计空间却只剩 240 宽，会被直接裁掉，排版全乱。
void ApplyScale(Layout& lay)
{
    if (lay.scale < 0.05f) lay.scale = 0.05f;
    if (lay.scale > 8.0f)  lay.scale = 8.0f;

    lay.ww = (int)(lay.dw * lay.scale + 0.5f);
    lay.wh = (int)(lay.dh * lay.scale + 0.5f);
    if (lay.ww < 80) lay.ww = 80;      // 太小了没法看，兜个底
    if (lay.wh < 60) lay.wh = 60;
}

std::map<std::wstring, Bitmap*> g_images;

Bitmap* GetImage(const std::wstring& file)
{
    std::map<std::wstring, Bitmap*>::iterator it = g_images.find(file);
    if (it != g_images.end()) return it->second;

    assets::Blob blob;
    if (!assets::Get(assets::KIND_IMAGE, file.c_str(), blob))
    {
        elog::Write(L"[ui] 图片找不到，跳过该元素: %s",
                    assets::Where(assets::KIND_IMAGE, file.c_str()).c_str());
        g_images[file] = nullptr;
        return nullptr;
    }

    Bitmap* bmp = image_blob::Decode(blob.Data(), blob.Size());

    if (!bmp)
    {
        // 素材在、却读不出来。最常见的坑是**扩展名骗人**：
        // GDI+ 是按文件头挑解码器的，把 .webp 改名成 .png 只会得到一句
        // 和内容毫无关系的 "Out of memory"。所以直接看文件头把真实格式说出来。
        const unsigned char* head = blob.Data();
        const size_t n = blob.Size();

        const wchar_t* kind = L"无法识别";
        if (n >= 12 && head[0]=='R' && head[1]=='I' && head[2]=='F' && head[3]=='F' &&
            head[8]=='W' && head[9]=='E' && head[10]=='B' && head[11]=='P')
            kind = L"WebP —— GDI+ 不支持，请另存为真正的 PNG";
        else if (n >= 4 && head[0]==0x89 && head[1]=='P' && head[2]=='N' && head[3]=='G')
            kind = L"PNG（文件头正常，可能已损坏）";
        else if (n >= 2 && head[0]==0xFF && head[1]==0xD8)
            kind = L"JPEG";
        else if (n >= 2 && head[0]=='B' && head[1]=='M')
            kind = L"BMP";
        else if (n >= 3 && head[0]=='G' && head[1]=='I' && head[2]=='F')
            kind = L"GIF";

        elog::Write(L"[ui] 图片解码失败（素材在，但读不出来），跳过: %s —— 实际格式: %s",
                    assets::Where(assets::KIND_IMAGE, file.c_str()).c_str(), kind);
    }

    g_images[file] = bmp;
    return bmp;
}

// ---------------------------------------------------------------- 富文本 ----
struct Run  { std::wstring text; Color color; };
struct Line { std::vector<Run> runs; };

// 把带标记的文本解析成「若干行 × 若干着色段」。
//
// 支持的标记：
//     \n            换行（配置文件里写反斜杠 n）
//     {cR,G,B}      从这里开始换色
//     {gold}        已收集金币
//     {goal}        目标金币
//     {left}        还差多少（goal-gold，不小于 0）
//     {time}        剩余时间 MM:SS
//     {pct}         百分比
//
// 认不出的 {xxx} 原样保留成文字，不会吞掉内容。
void ParseRich(const std::wstring& src, const ui_layout::Status& st,
               Color base, std::vector<Line>& out)
{
    out.clear();
    Line cur;
    Run  run;
    run.color = base;

    std::wstring pending;   // 当前正在攒的普通文字

    // 把 run 收口，避免产生空 run
    struct Flush {
        static void Go(std::wstring& p, Run& r, Line& ln)
        {
            if (p.empty()) return;
            r.text = p;
            ln.runs.push_back(r);
            p.clear();
        }
    };

    for (size_t i = 0; i < src.size(); )
    {
        const wchar_t ch = src[i];

        // 反斜杠 n -> 换行
        if (ch == L'\\' && i + 1 < src.size() && (src[i+1] == L'n' || src[i+1] == L'N'))
        {
            Flush::Go(pending, run, cur);
            out.push_back(cur);
            cur = Line();
            i += 2;
            continue;
        }
        if (ch == L'\n' || ch == L'\r')
        {
            Flush::Go(pending, run, cur);
            out.push_back(cur);
            cur = Line();
            ++i;
            continue;
        }

        if (ch == L'{')
        {
            const size_t close = src.find(L'}', i + 1);
            if (close != std::wstring::npos)
            {
                const std::wstring tok = src.substr(i + 1, close - i - 1);

                // {cR,G,B} -> 换色
                if (!tok.empty() && (tok[0] == L'c' || tok[0] == L'C'))
                {
                    Color c;
                    if (ParseColor(tok.substr(1), c))
                    {
                        Flush::Go(pending, run, cur);
                        run.color = c;
                        i = close + 1;
                        continue;
                    }
                }

                // 占位符
                const std::wstring low = Lower(tok);
                std::wstring val;
                bool known = true;
                if      (low == L"gold") val = std::to_wstring(st.gold);
                else if (low == L"goal") val = std::to_wstring(st.goal);
                else if (low == L"left")
                {
                    int left = st.goal - st.gold;
                    if (left < 0) left = 0;
                    val = std::to_wstring(left);
                }
                else if (low == L"pct")
                {
                    const int pct = (st.goal > 0) ? (st.gold * 100 / st.goal) : 0;
                    val = std::to_wstring(pct) + L"%";
                }
                else if (low == L"time")
                {
                    const DWORD s = st.remainMs / 1000;
                    wchar_t buf[32];
                    swprintf_s(buf, L"%02lu:%02lu", (unsigned long)(s / 60),
                               (unsigned long)(s % 60));
                    val = buf;
                }
                else known = false;

                if (known)
                {
                    pending += val;
                    i = close + 1;
                    continue;
                }
                // 认不出：原样当文字
            }
        }

        pending += ch;
        ++i;
    }

    Flush::Go(pending, run, cur);
    out.push_back(cur);
}

// 按元素设置挑字体。
// 注意 GDI+ 的 Font **拷贝构造是私有的**（不允许值拷贝），所以这里只能在
// 两个 return 里各自直接构造——不能先建一个具名局部变量再 return 它。
Font MakeFont(const Element& e)
{
    const INT style = e.bold ? FontStyleBold : FontStyleRegular;

    if (e.mono)
        if (FontFamily* fam = aero::UiFontFamily())    // 素材字体（Roboto Mono）
            return Font(fam, e.size, style, UnitPixel);

    return Font(L"Microsoft YaHei", e.size, style, UnitPixel);
}

// 渲染一个 text 元素。返回实际用掉的高度。
void RenderText(Graphics& g, const Element& e, const ui_layout::Status& st, REAL alphaMul)
{
    std::vector<Line> lines;
    ParseRich(e.raw, st, e.color, lines);

    Font font = MakeFont(e);

    // 排版用 GenericTypographic：它不带 GDI+ 默认那圈内边距，
    // 量出来的宽度才能直接拿来推进 x，不然每段之间会有奇怪的缝。
    StringFormat sf(StringFormat::GenericTypographic());
    sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsNoWrap
                                           | StringFormatFlagsMeasureTrailingSpaces);

    const REAL lh = e.size * e.lineh;

    for (size_t li = 0; li < lines.size(); ++li)
    {
        const std::vector<Run>& runs = lines[li].runs;
        if (runs.empty()) continue;

        // 先量整行宽度，再按对齐定起点
        REAL total = 0.0f;
        std::vector<REAL> w(runs.size(), 0.0f);
        for (size_t ri = 0; ri < runs.size(); ++ri)
        {
            RectF box;
            g.MeasureString(runs[ri].text.c_str(), -1, &font,
                            PointF(0, 0), &sf, &box);
            w[ri] = box.Width;
            total += box.Width;
        }

        REAL x = e.rect.X;
        if      (e.align == 1) x = e.rect.X + (e.rect.Width - total) / 2.0f;
        else if (e.align == 2) x = e.rect.X + (e.rect.Width - total);
        const REAL y = e.rect.Y + lh * (REAL)li;

        for (size_t ri = 0; ri < runs.size(); ++ri)
        {
            Color c = runs[ri].color;
            if (alphaMul < 1.0f)
                c = Color((BYTE)(c.GetA() * alphaMul), c.GetR(), c.GetG(), c.GetB());
            SolidBrush br(c);
            g.DrawString(runs[ri].text.c_str(), -1, &font, PointF(x, y), &sf, &br);
            x += w[ri];
        }
    }
}

void RenderElement(Graphics& g, const Element& e, const ui_layout::Status& st,
                   DWORD elapsedMs)
{
    // ---- 出场动画：从小到大 + 淡入 ----
    // 带 popin 的元素先等 popinDelay，再用 popinMs 从 25% 弹到 100%，
    // 同时透明度在前 55% 的时长里淡到满。
    REAL popScale = 1.0f;
    REAL popA     = 1.0f;

    if (e.hasPopin)
    {
        if (elapsedMs < e.popinDelay) return;                 // 还没轮到它出场
        const DWORD t = elapsedMs - e.popinDelay;
        REAL p = (e.popinMs > 0) ? (REAL)t / (REAL)e.popinMs : 1.0f;
        if (p > 1.0f) p = 1.0f;

        const REAL u    = 1.0f - p;
        const REAL ease = 1.0f - u * u * u;                   // 缓出
        popScale = 0.25f + 0.75f * ease;

        popA = p / 0.55f;                                     // 前 55% 淡到满
        if (popA > 1.0f) popA = 1.0f;
        if (popA <= 0.0f) return;
    }

    // 缩放要绕元素自己的中心，不然会从左上角「长出来」
    GraphicsState gs = 0;
    const bool xform = (popScale != 1.0f);
    if (xform)
    {
        gs = g.Save();
        const REAL cx = e.rect.X + e.rect.Width  * 0.5f;
        const REAL cy = e.rect.Y + e.rect.Height * 0.5f;
        g.TranslateTransform(cx, cy);
        g.ScaleTransform(popScale, popScale);
        g.TranslateTransform(-cx, -cy);
    }

    switch (e.kind)
    {
    case K_RECT:
        if (!e.hasRect) break;
        {
            Color fill = e.color;
            if (popA < 1.0f) fill = Color((BYTE)(fill.GetA() * popA),
                                          fill.GetR(), fill.GetG(), fill.GetB());
            SolidBrush br(fill);
            g.FillRectangle(&br, e.rect);

            if (e.hasBorder && e.borderW > 0.0f)
            {
                Color bc = e.border;
                if (popA < 1.0f) bc = Color((BYTE)(bc.GetA() * popA),
                                            bc.GetR(), bc.GetG(), bc.GetB());
                // 边框画在矩形内侧：画笔是以路径为中心画的，所以要内缩半个线宽
                const REAL h = e.borderW / 2.0f;
                Pen pen(bc, e.borderW);
                g.DrawRectangle(&pen,
                                e.rect.X + h, e.rect.Y + h,
                                e.rect.Width  - e.borderW,
                                e.rect.Height - e.borderW);
            }
        }
        break;

    case K_IMAGE:
        if (!e.hasRect) break;
        if (Bitmap* bmp = GetImage(e.file))
        {
            // 拉伸填充，不保持宽高比（和子窗口那边的做法一致）
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            if (popA < 1.0f)
            {
                // GDI+ 给整张图加 alpha 只能走 ColorMatrix
                ColorMatrix cm = {
                    1, 0, 0, 0, 0,
                    0, 1, 0, 0, 0,
                    0, 0, 1, 0, 0,
                    0, 0, 0, popA, 0,
                    0, 0, 0, 0, 1
                };
                ImageAttributes ia;
                ia.SetColorMatrix(&cm);
                g.DrawImage(bmp, e.rect, 0.0f, 0.0f,
                            (REAL)bmp->GetWidth(), (REAL)bmp->GetHeight(),
                            UnitPixel, &ia);
            }
            else
            {
                g.DrawImage(bmp, e.rect, 0.0f, 0.0f,
                            (REAL)bmp->GetWidth(), (REAL)bmp->GetHeight(), UnitPixel);
            }
        }
        break;

    case K_TEXT:
        RenderText(g, e, st, popA);
        break;
    }

    if (xform) g.Restore(gs);
}

// ---------------------------------------------------------------- 默认排版 ----
// 配置文件缺失时的兜底，同时也是「新建配置文件」的模板。
// 这几个 Add* 往 g_elemsTarget 指的容器里塞，调用方先设好目标。
std::vector<Element>* g_elemsTarget = nullptr;

void AddRect(REAL x, REAL y, REAL w, REAL h, Color fill,
             bool border, Color bc, REAL bw)
{
    if (!g_elemsTarget) return;
    Element e;
    e.kind = K_RECT;
    e.hasRect = true;
    e.rect = RectF(x, y, w, h);
    e.color = fill;
    e.hasBorder = border;
    e.border = bc;
    e.borderW = bw;
    g_elemsTarget->push_back(e);
}

void AddImage(REAL x, REAL y, REAL w, REAL h, const wchar_t* file, bool keep = false)
{
    if (!g_elemsTarget) return;
    Element e;
    e.kind = K_IMAGE;
    e.hasRect = true;
    e.rect = RectF(x, y, w, h);
    e.file = file;
    e.keep = keep;
    g_elemsTarget->push_back(e);
}

void AddText(REAL x, REAL y, REAL w, const wchar_t* raw, REAL size,
             bool bold, Color c, REAL lineh, bool mono)
{
    if (!g_elemsTarget) return;
    Element e;
    e.kind = K_TEXT;
    e.rect = RectF(x, y, w, size * lineh * 4.0f);
    e.raw = raw;
    e.size = size;
    e.bold = bold;
    e.color = c;
    e.lineh = lineh;
    e.mono = mono;
    g_elemsTarget->push_back(e);
}

void DefaultLayout(Layout& lay)
{
    lay.dw = 580;
    lay.dh = 340;
    lay.scale = 1.0f;
    lay.bg = Color(255, 190, 0, 0);
    lay.elems.clear();

    g_elemsTarget = &lay.elems;

    const Color white(255, 255, 255, 255);
    const Color black(255, 0, 0, 0);
    const Color gold (255, 255, 190, 0);

    // 底色不用写成元素——[window] bg 会自动铺满（见 Render）

    // 1) 左上角 A90 头（用攻击前的 A-90_IDLE）
    AddImage(20, 18, 104, 104, L"A-90_IDLE.png");

    // 3) 旁边白字标题
    AddText(140, 22, 420, L"YOUR ITEMS\\nHAVE BEEN\\nENCRYPTED",
            23.0f, true, white, 1.25f, true);

    // 4) 中间黑块：白边框，正文里 UNRECOVERABLE 之后转纯红
    AddRect(20, 136, 540, 108, black, true, white, 2.0f);
    AddText(36, 148, 508,
            L"IF YOU DO NOT PAY THIS RANSOM\\n"
            L"BEFORE THE TIMER ENDS, YOUR ITEMS WILL BE "
            L"{c255,0,0}UNRECOVERABLE BY ANY\\nMEANS.",
            13.5f, false, white, 1.35f, true);

    // 5) 左下黑块：还差多少金币 + Gold_icon，金黄边框
    AddRect(20, 258, 250, 64, black, true, gold, 2.0f);
    AddImage(32, 270, 40, 40, L"Gold_icon.png");
    AddText(84, 274, 170, L"{left}", 24.0f, true, white, 1.2f, true);

    // 6) 右下红块：黑字 TIME，白边框
    AddRect(286, 258, 274, 64, Color(255, 215, 0, 0), true, white, 2.0f);
    AddText(302, 275, 242, L"TIME: {time}", 20.0f, true, black, 1.2f, true);

    ApplyScale(lay);
    lay.loaded = true;
}

// ---------------------------------------------------------------- 解析 ----
void ParseIni(const std::wstring& text, Layout& lay)
{
    std::vector<Element> elems;
    int   w = 580, h = 340;
    float scale = 1.0f;
    Color bg(255, 190, 0, 0);

    Element cur;
    bool    inElement = false;

    size_t pos = 0;
    int lineNo = 0;
    while (pos <= text.size())
    {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = (nl == std::wstring::npos) ? text.substr(pos)
                                                       : text.substr(pos, nl - pos);
        pos = (nl == std::wstring::npos) ? text.size() + 1 : nl + 1;
        ++lineNo;

        line = StripComment(line);
        if (line.empty()) continue;

        // 段头
        if (line[0] == L'[')
        {
            const size_t close = line.find(L']');
            const size_t len = (close == std::wstring::npos) ? std::wstring::npos
                : (close > 1 ? close - 1 : 0);
            const std::wstring sec = Lower(Trim(line.substr(1, len)));
            if (inElement) { elems.push_back(cur); cur = Element(); inElement = false; }
            if (sec == L"element") { inElement = true; cur = Element(); }
            continue;
        }

        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos)
        {
            elog::Write(L"[ui] 第 %d 行没有 '='，已忽略: %s", lineNo, line.c_str());
            continue;
        }

        const std::wstring key = Lower(Trim(line.substr(0, eq)));
        const std::wstring val = Trim(line.substr(eq + 1));

        if (!inElement)
        {
            // [window]
            int iv = 0; float fv = 0; Color c; RectF r;
            if      (key == L"width"  && ParseInt(val, iv))   w = iv;
            else if (key == L"height" && ParseInt(val, iv))   h = iv;
            else if (key == L"scale"  && ParseFloat(val, fv)) scale = fv;
            else if (key == L"bg"     && ParseColor(val, c))  bg = c;
            else elog::Write(L"[ui] 第 %d 行：[window] 不认得的键 '%s'",
                             lineNo, key.c_str());
            continue;
        }

        int iv = 0; float fv = 0; Color c; RectF r;
        if      (key == L"kind")
        {
            const std::wstring k = Lower(val);
            if      (k == L"rect")  cur.kind = K_RECT;
            else if (k == L"image") cur.kind = K_IMAGE;
            else if (k == L"text")  cur.kind = K_TEXT;
            else elog::Write(L"[ui] 第 %d 行：不认得的 kind '%s'", lineNo, val.c_str());
        }
        else if (key == L"rect"     && ParseRect(val, r)) { cur.rect = r; cur.hasRect = true; }
        else if (key == L"color"    && ParseColor(val, c)) cur.color = c;
        else if (key == L"border"   && ParseColor(val, c)) { cur.border = c; cur.hasBorder = true; }
        else if (key == L"borderw"  && ParseFloat(val, fv)) cur.borderW = fv;
        else if (key == L"file")    cur.file = val;
        else if (key == L"text")    cur.raw = val;
        else if (key == L"size"     && ParseFloat(val, fv)) cur.size = fv;
        else if (key == L"lineh"    && ParseFloat(val, fv)) cur.lineh = fv;
        else if (key == L"bold"     && ParseInt(val, iv))   cur.bold = (iv != 0);
        else if (key == L"font")
        {
            const std::wstring f = Lower(val);
            cur.mono = (f != L"ui");            // 默认 mono
        }
        else if (key == L"align")
        {
            const std::wstring a = Lower(val);
            cur.align = (a == L"center") ? 1 : ((a == L"right") ? 2 : 0);
        }
        else if (key == L"popin")
        {
            if (ParseInt(val, iv)) { cur.hasPopin = true; cur.popinDelay = (DWORD)(iv < 0 ? 0 : iv); }
        }
        else if (key == L"popinms")
        {
            if (ParseInt(val, iv)) cur.popinMs = (DWORD)(iv < 16 ? 16 : iv);
        }
        else if (key == L"keep")
        {
            if (ParseInt(val, iv)) cur.keep = (iv != 0);
        }
        else elog::Write(L"[ui] 第 %d 行：不认得的键 '%s'", lineNo, key.c_str());
    }
    if (inElement) elems.push_back(cur);

    // 丢掉什么内容都没有的占位元素（比如配置里只写了个 [element] 段头）
    std::vector<Element> kept;
    for (size_t i = 0; i < elems.size(); ++i)
    {
        const Element& e = elems[i];
        if (e.kind == K_TEXT && e.raw.empty())   continue;
        if (e.kind == K_IMAGE && e.file.empty()) continue;
        if (e.kind == K_RECT && !e.hasRect)      continue;
        kept.push_back(e);
    }

    if (kept.empty())
    {
        elog::Write(L"[ui] 配置里没有可用元素，改用内置默认排版");
        DefaultLayout(lay);
        return;
    }

    lay.dw = (w > 80) ? w : 580;
    lay.dh = (h > 80) ? h : 340;
    lay.scale = scale;
    lay.bg = bg;
    lay.elems.swap(kept);
    ApplyScale(lay);
    lay.loaded = true;
}

// ---------------------------------------------------------------- PNG 保存 ----
int GetEncoderClsid(const WCHAR* format, CLSID* pClsid)
{
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    ImageCodecInfo* pInfo = (ImageCodecInfo*)malloc(size);
    if (!pInfo) return -1;
    GetImageEncoders(num, size, pInfo);
    int found = -1;
    for (UINT i = 0; i < num; ++i)
        if (wcscmp(pInfo[i].MimeType, format) == 0) { *pClsid = pInfo[i].Clsid; found = (int)i; break; }
    free(pInfo);
    return found;
}

// 网格：每 20px 一条细线，每 100px 一条亮线 + 坐标标注。
// 调排版时用 --ui-preview 叠上它，就能直接读出坐标。
void DrawGrid(Graphics& g, int w, int h)
{
    Pen thin(Color(60, 255, 255, 255), 1.0f);
    Pen bold(Color(140, 0, 255, 255), 1.0f);
    for (int x = 0; x <= w; x += 20)
    {
        Pen* p = (x % 100 == 0) ? &bold : &thin;
        g.DrawLine(p, (REAL)x, 0.0f, (REAL)x, (REAL)h);
    }
    for (int y = 0; y <= h; y += 20)
    {
        Pen* p = (y % 100 == 0) ? &bold : &thin;
        g.DrawLine(p, 0.0f, (REAL)y, (REAL)w, (REAL)y);
    }

    Font f(L"Consolas", 11.0f, FontStyleRegular, UnitPixel);
    SolidBrush br(Color(220, 255, 255, 0));
    for (int x = 100; x < w; x += 100)
        for (int y = 100; y < h; y += 100)
        {
            wchar_t buf[32];
            swprintf_s(buf, L"%d,%d", x, y);
            g.DrawString(buf, -1, &f, PointF((REAL)x + 2.0f, (REAL)y + 2.0f), &br);
        }
}

} // namespace

// ============================================================================
namespace ui_layout {

bool LoadOne(const wchar_t* iniName, int which, bool needElems, bool& fileOk)
{
    g_path[which] = iniName ? iniName : L"";
    fileOk = false;

    // 排版文件也内嵌在 exe 里（assets_gen.rc 的 ROOT_ 那一组）
    assets::Blob blob;
    if (!assets::Get(assets::KIND_ROOT, g_path[which].c_str(), blob))
    {
        elog::Write(L"[ui] 排版素材不在包里: %s", g_path[which].c_str());
        if (needElems) { DefaultLayout(g_lay[which]); return true; }   // 主排版有内置兜底
        g_lay[which].loaded = false;
        return false;
    }
    fileOk = true;

    // 按 UTF-8 解（配置文件里可能带中文注释）
    const std::string bytes((const char*)blob.Data(), blob.Size());
    std::wstring text;
    if (!bytes.empty())
    {
        const int need = MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), (int)bytes.size(), nullptr, 0);
        if (need > 0)
        {
            text.resize((size_t)need);
            MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), (int)bytes.size(), &text[0], need);
        }
    }

    ParseIni(text, g_lay[which]);
    elog::Write(L"[ui] 排版已载入: %s（窗口 %dx%d，设计坐标 %dx%d，scale=%.3f，%d 个元素）",
                g_path[which].c_str(), g_lay[which].ww, g_lay[which].wh,
                g_lay[which].dw, g_lay[which].dh, g_lay[which].scale,
                (int)g_lay[which].elems.size());
    return true;
}

bool Load(const wchar_t* mainIniName, const wchar_t* payupIniName)
{
    bool ok = true, fileOk = false;

    ok = LoadOne(mainIniName, LAY_MAIN, true, fileOk) && ok;

    if (payupIniName && *payupIniName)
    {
        // 付完钱那套排版**没有内置兜底**：取不到就保持 loaded=false，
        // popup 会跳过整个付钱演出，主窗口照常关掉，不会崩。
        if (!LoadOne(payupIniName, LAY_PAYUP, false, fileOk))
            elog::Write(L"[ui] 付钱排版不可用，将跳过付钱演出");
    }

    g_active    = LAY_MAIN;
    g_travel    = false;
    g_animStart = GetTickCount();
    return ok;
}

int  Width()  { return L().ww; }
int  Height() { return L().wh; }
COLORREF Background() { return RGB(L().bg.GetR(), L().bg.GetG(), L().bg.GetB()); }
const wchar_t* SourcePath() { return g_path[g_active].c_str(); }

bool PayupReady()  { return g_lay[LAY_PAYUP].loaded; }
int  PayupWidth()  { return g_lay[LAY_PAYUP].ww; }
int  PayupHeight() { return g_lay[LAY_PAYUP].wh; }

void SetActive(int which)
{
    if (which < 0 || which >= LAY_COUNT) return;
    if (!g_lay[which].loaded) return;
    g_active = which;
    g_travel = false;
    g_animStart = GetTickCount();      // 出场动画从这里重新计时
}

int  Active() { return g_active; }

void SetTravel(bool on) { g_travel = on; }
void RestartAnim()      { g_animStart = GetTickCount(); }

DWORD AnimElapsed()
{
    return GetTickCount() - g_animStart;
}

void Render(Graphics& g, const RectF& content, const Status& st)
{
    const Layout& lay = L();

    // 配置文件里的坐标是「相对内容区左上角」的像素，
    // 这里整体平移一下，元素定义就不必关心窗口实际摆在屏幕哪里。
    const GraphicsState state = g.Save();
    g.TranslateTransform(content.X, content.Y);

    // 窗口**可以拖边缘改大小**，所以内容必须整体跟着缩放。
    // 少了这一步的表现就是：窗口拉大了，里面还是原来那么一小块贴在左上角
    // （内容用固定像素坐标画，完全不知道窗口已经变大了）。
    //
    // 缩放比 = 实际内容区 / 设计尺寸（配置文件里的 width/height）。
    // 这里**刻意不保持宽高比**：窗口四条边能独立拖，横向拉长内容就横向拉长，
    // 也就是「跟着变形」。想改成等比缩放并居中，把 sx/sy 统一取较小值即可。
    const REAL sx = (lay.dw > 0) ? content.Width  / (REAL)lay.dw : 1.0f;
    const REAL sy = (lay.dh > 0) ? content.Height / (REAL)lay.dh : 1.0f;
    if (sx != 1.0f || sy != 1.0f) g.ScaleTransform(sx, sy);

    // 整窗底色：由 [window] bg 决定，**自动铺满**，不用在元素列表里再写一块。
    // travel 阶段强制黑底（付完钱后那一段「只留 A90 的头 + 黑背景」）。
    const Color bg = g_travel ? Color(255, 0, 0, 0) : lay.bg;
    SolidBrush bgBrush(bg);
    g.FillRectangle(&bgBrush, RectF(0.0f, 0.0f, (REAL)lay.dw, (REAL)lay.dh));

    const DWORD el = AnimElapsed();
    for (size_t i = 0; i < lay.elems.size(); ++i)
    {
        const Element& e = lay.elems[i];
        if (g_travel && !e.keep) continue;       // travel：只留标记了 keep 的
        RenderElement(g, e, st, el);
    }

    g.Restore(state);
}

bool Preview(const wchar_t* pngPath, const Status& st, bool withGrid)
{
    if (!pngPath || !*pngPath) return false;
    const Layout& lay = L();

    // 按**窗口实际尺寸**出图，这样预览看到的就是真东西。
    // （设计坐标系更大，元素坐标仍按设计坐标读。）
    Bitmap bmp(lay.ww, lay.wh, PixelFormat32bppARGB);
    if (bmp.GetLastStatus() != Ok) return false;

    {
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.Clear(Color(255, 40, 40, 40));                 // 画布底：中性灰，便于看清边界

        // 预览要看到「出场动画完成之后」的最终样子，所以把时钟拨到很久以前
        const DWORD saved = g_animStart;
        g_animStart = GetTickCount() - 3600'000;
        Render(g, RectF(0, 0, (REAL)lay.ww, (REAL)lay.wh), st);
        g_animStart = saved;

        if (withGrid)
        {
            // 网格按**设计坐标**画，并且跟着内容一起缩放，
            // 这样标注上的数字就是你写在 ini 里的那个坐标。
            const GraphicsState s = g.Save();
            g.ScaleTransform((REAL)lay.ww / (REAL)lay.dw, (REAL)lay.wh / (REAL)lay.dh);
            DrawGrid(g, lay.dw, lay.dh);
            g.Restore(s);
        }
    }

    CLSID clsid;
    if (GetEncoderClsid(L"image/png", &clsid) < 0)
    {
        elog::Write(L"[ui] 找不到 PNG 编码器");
        return false;
    }
    const bool ok = (bmp.Save(pngPath, &clsid, nullptr) == Ok);
    elog::Write(L"[ui] 排版预览%s: %s（%dx%d，设计坐标系 %dx%d，scale=%.3f，排版=%s）",
                ok ? L"已导出" : L"导出失败", pngPath, lay.ww, lay.wh,
                lay.dw, lay.dh, lay.scale, g_active == LAY_PAYUP ? L"付钱" : L"主");
    return ok;
}

void Shutdown()
{
    for (std::map<std::wstring, Bitmap*>::iterator it = g_images.begin();
         it != g_images.end(); ++it)
        delete it->second;
    g_images.clear();
}

} // namespace ui_layout
