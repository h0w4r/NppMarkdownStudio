// ============================================================================
// NppMarkdownStudio.cpp
// Plugin nativo en C++ (Win32 + Scintilla + WebView2 + Docking Manager)
// para edición avanzada y previsualización interactiva de Markdown en Notepad++
// ============================================================================

#include "NppMarkdownStudio.h"
#include <commdlg.h>
#include <shellapi.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")

// Si se compila con el paquete NuGet Microsoft.Web.WebView2, se activa el
// puente COM completo de WebView2. También soporta compilación autónoma con
// contenedor HWND y motor de previsualización integrado.
#if __has_include(<WebView2.h>)
#include <wrl.h>
#include <WebView2.h>
#define HAS_WEBVIEW2_SDK 1
using namespace Microsoft::WRL;
#else
#define HAS_WEBVIEW2_SDK 0
#endif

// ----------------------------------------------------------------------------
// Variables globales del Plugin
// ----------------------------------------------------------------------------
static const wchar_t PLUGIN_NAME[] = L"NppMarkdownStudio";
static const wchar_t DOCK_CLASS_NAME[] = L"NppMarkdownStudioDockClass";

static HINSTANCE g_hModule = nullptr;
static NppData   g_nppData;
static FuncItem  g_funcItems[CMD_COUNT];
static ShortcutKey g_shortcuts[CMD_COUNT];

static HWND g_hDockWnd = nullptr;
static HWND g_hToolbarWnd = nullptr;
static HWND g_hStatusWnd = nullptr;
static HWND g_hFallbackPreviewEdit = nullptr;

static bool g_isPanelVisible = false;
static bool g_syncScrollEnabled = true;
static bool g_autoListEnabled = true;
static bool g_isDarkMode = false;
static bool g_isUpdatingFromPreview = false;
static UINT_PTR g_debounceTimerId = 1001;

#if HAS_WEBVIEW2_SDK
static ComPtr<ICoreWebView2Controller> g_webViewController;
static ComPtr<ICoreWebView2> g_webView;
static bool g_webViewReady = false;
#endif

// Prototipos de comandos del menú de Notepad++
void cmdTogglePanel();
void cmdToggleSyncScroll();
void cmdToggleAutoList();
void cmdFormatBold();
void cmdFormatItalic();
void cmdFormatStrikethrough();
void cmdFormatCode();
void cmdToggleTaskItem();
void cmdInsertLink();
void cmdInsertImage();
void cmdInsertCallout();
void cmdFormatTable();
void cmdGenerateTOC();
void cmdExportHtml();
void cmdExportPdf();
void cmdAbout();

// ----------------------------------------------------------------------------
// Utilidades de codificación UTF-8 <-> UTF-16 y Scintilla
// ----------------------------------------------------------------------------
static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring result(sizeNeeded, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), &result[0], sizeNeeded);
    return result;
}

static std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string result(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), &result[0], sizeNeeded, nullptr, nullptr);
    return result;
}

static HWND GetCurrentScintilla() {
    int which = -1;
    ::SendMessage(g_nppData._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, (LPARAM)&which);
    return (which == 0) ? g_nppData._scintillaMainHandle : g_nppData._scintillaSecondHandle;
}

static std::string GetDocumentText(HWND hSci) {
    if (!hSci) return "";
    sptr_t len = ::SendMessage(hSci, SCI_GETLENGTH, 0, 0);
    if (len <= 0) return "";
    std::string buffer(len + 1, '\0');
    ::SendMessage(hSci, SCI_GETTEXT, len + 1, (LPARAM)buffer.data());
    buffer.resize(len);
    return buffer;
}

static std::string GetSelectedText(HWND hSci) {
    if (!hSci) return "";
    sptr_t start = ::SendMessage(hSci, SCI_GETCURRENTPOS, 0, 0);
    sptr_t anchor = ::SendMessage(hSci, SCI_GETANCHOR, 0, 0);
    sptr_t len = std::abs(start - anchor);
    if (len <= 0) return "";
    std::string buffer(len + 1, '\0');
    ::SendMessage(hSci, SCI_GETSELTEXT, 0, (LPARAM)buffer.data());
    buffer.resize(len);
    return buffer;
}

static std::string GetLineText(HWND hSci, sptr_t line) {
    sptr_t lineStart = ::SendMessage(hSci, SCI_POSITIONFROMLINE, line, 0);
    sptr_t lineEnd = ::SendMessage(hSci, SCI_GETLINEENDPOSITION, line, 0);
    if (lineEnd <= lineStart) return "";
    sptr_t len = lineEnd - lineStart;
    std::string buffer(len + 1, '\0');
    ::SendMessage(hSci, SCI_SETTARGETSTART, lineStart, 0);
    ::SendMessage(hSci, SCI_SETTARGETEND, lineEnd, 0);
    sptr_t totalLen = ::SendMessage(hSci, SCI_GETLINE, line, (LPARAM)buffer.data());
    if (totalLen > len) totalLen = len;
    buffer.resize(totalLen);
    return buffer;
}

static std::wstring GetCurrentFileDirectory() {
    wchar_t dirPath[MAX_PATH] = {0};
    ::SendMessage(g_nppData._nppHandle, NPPM_GETCURRENTDIRECTORY, MAX_PATH, (LPARAM)dirPath);
    return std::wstring(dirPath);
}

static bool QueryNppDarkMode(NppDarkModeColors* outColors = nullptr) {
    bool isDark = (bool)::SendMessage(g_nppData._nppHandle, NPPM_ISDARKMODEENABLED, 0, 0);
    if (isDark && outColors) {
        ::SendMessage(g_nppData._nppHandle, NPPM_GETDARKMODECOLORS, sizeof(NppDarkModeColors), (LPARAM)outColors);
    }
    return isDark;
}

static std::string ColorRefToHex(COLORREF c) {
    char buf[16];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x", GetRValue(c), GetGValue(c), GetBValue(c));
    return std::string(buf);
}

// ----------------------------------------------------------------------------
// Motor de Compilación Markdown Extendido (GFM + Callouts + Math + Mermaid + LineMap)
// ----------------------------------------------------------------------------
struct DocStats {
    size_t words = 0;
    size_t chars = 0;
    size_t lines = 0;
    size_t readingMinutes = 1;
};

struct HeadingItem {
    int level;
    std::string text;
    std::string slug;
    int line;
};

class MarkdownCompiler {
public:
    static std::string EscapeHtml(const std::string& s) {
        std::string out;
        out.reserve(s.size() * 1.1);
        for (char c : s) {
            switch (c) {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '"': out += "&quot;"; break;
                default:  out += c; break;
            }
        }
        return out;
    }

    static std::string Slugify(const std::string& text) {
        std::string slug;
        for (unsigned char c : text) {
            if (std::isalnum(c)) slug += (char)std::tolower(c);
            else if (c == ' ' || c == '-' || c == '_') {
                if (!slug.empty() && slug.back() != '-') slug += '-';
            }
        }
        while (!slug.empty() && slug.back() == '-') slug.pop_back();
        return slug.empty() ? "section" : slug;
    }

    static DocStats ComputeStats(const std::string& md) {
        DocStats st;
        st.chars = md.size();
        st.lines = 1;
        bool inWord = false;
        for (unsigned char c : md) {
            if (c == '\n') st.lines++;
            if (std::isspace(c)) {
                inWord = false;
            } else if (!inWord) {
                inWord = true;
                st.words++;
            }
        }
        st.readingMinutes = std::max<size_t>(1, (st.words + 199) / 200);
        return st;
    }

    static std::string ProcessInline(const std::string& raw, const std::wstring& baseDir) {
        std::string s = EscapeHtml(raw);

        // 1. Código en línea: `code`
        s = std::regex_replace(s, std::regex("`([^`]+)`"), "<code class=\"inline-code\">$1</code>");

        // 2. Matemáticas en línea: $math$
        s = std::regex_replace(s, std::regex("\\$([^\\$\\n]+)\\$"), "<span class=\"math-inline\" data-math=\"$1\">$$$1$$</span>");

        // 3. Imágenes: ![alt](url) con resolución de rutas locales relativas
        std::regex imgRe("!\\[([^\\]]*)\\]\\(([^\\)]+)\\)");
        std::string imgResult;
        std::sregex_iterator it(s.begin(), s.end(), imgRe), end;
        size_t lastPos = 0;
        std::string baseDirUtf8 = WideToUtf8(baseDir);
        std::replace(baseDirUtf8.begin(), baseDirUtf8.end(), '\\', '/');

        for (; it != end; ++it) {
            imgResult.append(s, lastPos, it->position() - lastPos);
            std::string alt = (*it)[1].str();
            std::string url = (*it)[2].str();
            if (url.find("http://") != 0 && url.find("https://") != 0 && url.find("data:") != 0 && !baseDirUtf8.empty()) {
                if (url.size() > 1 && url[1] != ':') {
                    url = "file:///" + baseDirUtf8 + "/" + url;
                }
            }
            imgResult += "<img src=\"" + url + "\" alt=\"" + alt + "\" class=\"md-img\" loading=\"lazy\" />";
            lastPos = it->position() + it->length();
        }
        imgResult.append(s, lastPos, std::string::npos);
        s = imgResult;

        // 4. Enlaces: [texto](url)
        s = std::regex_replace(s, std::regex("\\[([^\\]]+)\\]\\(([^\\)]+)\\)"),
                               "<a href=\"$2\" target=\"_blank\" rel=\"noopener\">$1</a>");

        // 5. Negrita + Cursiva, Negrita, Cursiva, Tachado, Resaltado (==texto==)
        s = std::regex_replace(s, std::regex("\\*\\*\\*([^\\*]+)\\*\\*\\*"), "<strong><em>$1</em></strong>");
        s = std::regex_replace(s, std::regex("\\*\\*([^\\*]+)\\*\\*"), "<strong>$1</strong>");
        s = std::regex_replace(s, std::regex("__([^_]+)__"), "<strong>$1</strong>");
        s = std::regex_replace(s, std::regex("\\*([^\\*]+)\\*"), "<em>$1</em>");
        s = std::regex_replace(s, std::regex("~~([^~]+)~~"), "<del>$1</del>");
        s = std::regex_replace(s, std::regex("==([^=]+)=="), "<mark>$1</mark>");

        // 6. Notas al pie: [^1]
        s = std::regex_replace(s, std::regex("\\[\\^([^\\]]+)\\]"),
                               "<sup class=\"footnote-ref\"><a href=\"#fn-$1\" id=\"fnref-$1\">[$1]</a></sup>");

        return s;
    }

    static std::string CompileToBodyHtml(const std::string& markdown, const std::wstring& baseDir, std::vector<HeadingItem>& outHeadings) {
        outHeadings.clear();
        std::istringstream stream(markdown);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }

        std::ostringstream html;
        std::map<std::string, std::string> footnotes;
        size_t i = 0;
        const size_t n = lines.size();

        // 1. Soporte para YAML Frontmatter al inicio del archivo (--- ... ---)
        if (n > 2 && lines[0] == "---") {
            size_t endFm = 1;
            while (endFm < n && lines[endFm] != "---" && lines[endFm] != "...") endFm++;
            if (endFm < n) {
                html << "<div class=\"frontmatter-card\" data-line=\"0\">"
                     << "<div class=\"frontmatter-title\">YAML Frontmatter</div><table class=\"fm-table\"><tbody>";
                for (size_t k = 1; k < endFm; ++k) {
                    auto colon = lines[k].find(':');
                    if (colon != std::string::npos) {
                        std::string key = EscapeHtml(lines[k].substr(0, colon));
                        std::string val = EscapeHtml(lines[k].substr(colon + 1));
                        html << "<tr><th>" << key << "</th><td>" << val << "</td></tr>";
                    }
                }
                html << "</tbody></table></div>\n";
                i = endFm + 1;
            }
        }

        while (i < n) {
            const std::string& cur = lines[i];

            // Línea vacía
            if (cur.find_first_not_of(" \t") == std::string::npos) {
                i++;
                continue;
            }

            // Definición de nota al pie [^id]: texto
            std::smatch fnMatch;
            if (std::regex_match(cur, fnMatch, std::regex("^\\[\\^([^\\]]+)\\]:\\s*(.+)$"))) {
                footnotes[fnMatch[1].str()] = ProcessInline(fnMatch[2].str(), baseDir);
                i++;
                continue;
            }

            // Bloque matemático multivariable $$ ... $$
            if (cur.rfind("$$", 0) == 0) {
                int startLine = (int)i;
                std::ostringstream mathBuf;
                if (cur.size() > 4 && cur.substr(cur.size() - 2) == "$$") {
                    mathBuf << cur.substr(2, cur.size() - 4);
                    i++;
                } else {
                    i++;
                    while (i < n && lines[i].rfind("$$", 0) != 0) {
                        mathBuf << lines[i] << "\n";
                        i++;
                    }
                    if (i < n) i++;
                }
                html << "<div class=\"math-block\" data-line=\"" << startLine << "\">$$"
                     << EscapeHtml(mathBuf.str()) << "$$</div>\n";
                continue;
            }

            // Bloques de código cercados ```lang ... ``` (incluyendo Mermaid)
            if (cur.rfind("```", 0) == 0 || cur.rfind("~~~", 0) == 0) {
                std::string fence = cur.substr(0, 3);
                std::string lang = cur.substr(3);
                lang.erase(0, lang.find_first_not_of(" \t"));
                lang.erase(lang.find_last_not_of(" \t") + 1);
                int startLine = (int)i;
                i++;
                std::ostringstream codeBuf;
                while (i < n && lines[i].rfind(fence, 0) != 0) {
                    codeBuf << lines[i] << "\n";
                    i++;
                }
                if (i < n) i++;

                if (lang == "mermaid") {
                    html << "<div class=\"mermaid-container\" data-line=\"" << startLine << "\">"
                         << "<pre class=\"mermaid\">" << EscapeHtml(codeBuf.str()) << "</pre></div>\n";
                } else {
                    std::string safeLang = EscapeHtml(lang.empty() ? "plaintext" : lang);
                    html << "<div class=\"code-wrapper\" data-line=\"" << startLine << "\">"
                         << "<div class=\"code-header\"><span class=\"code-lang\">" << safeLang << "</span>"
                         << "<button class=\"copy-btn\" onclick=\"copyCodeBlock(this)\">Copiar</button></div>"
                         << "<pre><code class=\"language-" << safeLang << "\">"
                         << EscapeHtml(codeBuf.str()) << "</code></pre></div>\n";
                }
                continue;
            }

            // Encabezados ATX (# a ######)
            std::smatch hMatch;
            if (std::regex_match(cur, hMatch, std::regex("^(#{1,6})\\s+(.+?)(?:\\s+#+)?$"))) {
                int level = (int)hMatch[1].length();
                std::string rawTitle = hMatch[2].str();
                std::string slug = Slugify(rawTitle);
                outHeadings.push_back({level, rawTitle, slug, (int)i});
                html << "<h" << level << " id=\"" << slug << "\" data-line=\"" << i << "\">"
                     << ProcessInline(rawTitle, baseDir) << "</h" << level << ">\n";
                i++;
                continue;
            }

            // Regla horizontal (---, ***, ___)
            if (std::regex_match(cur, std::regex("^(\\*{3,}|-{3,}|_{3,})\\s*$"))) {
                html << "<hr data-line=\"" << i << "\" />\n";
                i++;
                continue;
            }

            // Alertas estilo GitHub (> [!NOTE], > [!TIP], > [!IMPORTANT], > [!WARNING], > [!CAUTION]) y Blockquotes
            if (cur.rfind(">", 0) == 0) {
                int startLine = (int)i;
                std::vector<std::string> qLines;
                while (i < n && lines[i].rfind(">", 0) == 0) {
                    std::string stripped = lines[i].substr(1);
                    if (!stripped.empty() && stripped[0] == ' ') stripped.erase(0, 1);
                    qLines.push_back(stripped);
                    i++;
                }
                std::smatch alertMatch;
                if (!qLines.empty() && std::regex_match(qLines[0], alertMatch, std::regex("^\\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\\]\\s*(.*)$", std::regex::icase))) {
                    std::string type = alertMatch[1].str();
                    std::transform(type.begin(), type.end(), type.begin(), ::tolower);
                    std::string title = alertMatch[1].str();
                    html << "<div class=\"gh-alert gh-alert-" << type << "\" data-line=\"" << startLine << "\">"
                         << "<div class=\"gh-alert-title\">" << title << "</div><div class=\"gh-alert-body\">";
                    for (size_t k = 1; k < qLines.size(); ++k) {
                        html << "<p>" << ProcessInline(qLines[k], baseDir) << "</p>";
                    }
                    html << "</div></div>\n";
                } else {
                    html << "<blockquote data-line=\"" << startLine << "\">";
                    for (const auto& ql : qLines) {
                        html << "<p>" << ProcessInline(ql, baseDir) << "</p>";
                    }
                    html << "</blockquote>\n";
                }
                continue;
            }

            // Tablas GFM (| Col 1 | Col 2 |)
            if (cur.find('|') != std::string::npos && i + 1 < n &&
                std::regex_match(lines[i + 1], std::regex("^\\s*\\|?\\s*:?-+:?\\s*(\\|\\s*:?-+:?\\s*)+\\|?\\s*$"))) {
                int startLine = (int)i;
                auto splitRow = [](const std::string& rowStr) {
                    std::string s = rowStr;
                    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
                    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
                    if (!s.empty() && s.front() == '|') s.erase(s.begin());
                    if (!s.empty() && s.back() == '|') s.pop_back();
                    std::vector<std::string> cells;
                    std::stringstream ss(s);
                    std::string cell;
                    while (std::getline(ss, cell, '|')) {
                        cell.erase(0, cell.find_first_not_of(" \t"));
                        cell.erase(cell.find_last_not_of(" \t") + 1);
                        cells.push_back(cell);
                    }
                    return cells;
                };

                auto headers = splitRow(lines[i]);
                auto alignsRaw = splitRow(lines[i + 1]);
                std::vector<std::string> aligns;
                for (const auto& a : alignsRaw) {
                    bool left = !a.empty() && a.front() == ':';
                    bool right = !a.empty() && a.back() == ':';
                    if (left && right) aligns.push_back("center");
                    else if (right) aligns.push_back("right");
                    else aligns.push_back("left");
                }

                html << "<div class=\"table-responsive\" data-line=\"" << startLine << "\"><table class=\"md-table\"><thead><tr>";
                for (size_t c = 0; c < headers.size(); ++c) {
                    std::string al = (c < aligns.size()) ? aligns[c] : "left";
                    html << "<th style=\"text-align:" << al << "\">" << ProcessInline(headers[c], baseDir) << "</th>";
                }
                html << "</tr></thead><tbody>";
                i += 2;
                while (i < n && lines[i].find('|') != std::string::npos && lines[i].find_first_not_of(" \t") != std::string::npos) {
                    auto cells = splitRow(lines[i]);
                    html << "<tr data-line=\"" << i << "\">";
                    for (size_t c = 0; c < headers.size(); ++c) {
                        std::string al = (c < aligns.size()) ? aligns[c] : "left";
                        std::string val = (c < cells.size()) ? cells[c] : "";
                        html << "<td style=\"text-align:" << al << "\">" << ProcessInline(val, baseDir) << "</td>";
                    }
                    html << "</tr>";
                    i++;
                }
                html << "</tbody></table></div>\n";
                continue;
            }

            // Listas desordenadas y Listas de Tareas interactivas (- [ ], - [x], -, *, +)
            std::smatch ulMatch;
            if (std::regex_match(cur, ulMatch, std::regex("^(\\s*)[-*+]\\s+(.+)$"))) {
                html << "<ul class=\"md-list\" data-line=\"" << i << "\">\n";
                while (i < n && std::regex_match(lines[i], ulMatch, std::regex("^(\\s*)[-*+]\\s+(.+)$"))) {
                    std::string itemBody = ulMatch[2].str();
                    std::smatch taskMatch;
                    if (std::regex_match(itemBody, taskMatch, std::regex("^\\[([ xX])\\]\\s+(.*)$"))) {
                        bool checked = (taskMatch[1].str() != " ");
                        html << "<li class=\"task-list-item\" data-line=\"" << i << "\">"
                             << "<input type=\"checkbox\" class=\"task-checkbox\" data-line=\"" << i << "\" "
                             << (checked ? "checked " : "")
                             << "onchange=\"onTaskToggle(" << i << ", this.checked)\" /> "
                             << "<span class=\"" << (checked ? "task-done" : "") << "\">"
                             << ProcessInline(taskMatch[2].str(), baseDir) << "</span></li>\n";
                    } else {
                        html << "<li data-line=\"" << i << "\">" << ProcessInline(itemBody, baseDir) << "</li>\n";
                    }
                    i++;
                }
                html << "</ul>\n";
                continue;
            }

            // Listas ordenadas (1. , 2. )
            std::smatch olMatch;
            if (std::regex_match(cur, olMatch, std::regex("^(\\s*)\\d+\\.\\s+(.+)$"))) {
                html << "<ol class=\"md-list\" data-line=\"" << i << "\">\n";
                while (i < n && std::regex_match(lines[i], olMatch, std::regex("^(\\s*)\\d+\\.\\s+(.+)$"))) {
                    html << "<li data-line=\"" << i << "\">" << ProcessInline(olMatch[2].str(), baseDir) << "</li>\n";
                    i++;
                }
                html << "</ol>\n";
                continue;
            }

            // Párrafo estándar
            int startLine = (int)i;
            std::ostringstream para;
            while (i < n && lines[i].find_first_not_of(" \t") != std::string::npos &&
                   lines[i].rfind("#", 0) != 0 && lines[i].rfind("```", 0) != 0 &&
                   lines[i].rfind(">", 0) != 0 &&
                   !std::regex_match(lines[i], std::regex("^(\\s*)[-*+]\\s+(.+)$"))) {
                if (para.tellp() > 0) para << " ";
                para << lines[i];
                i++;
            }
            html << "<p data-line=\"" << startLine << "\">" << ProcessInline(para.str(), baseDir) << "</p>\n";
        }

        // Sección de Notas al pie al final del documento
        if (!footnotes.empty()) {
            html << "<hr class=\"footnotes-sep\" /><section class=\"footnotes\"><ol>";
            for (const auto& kv : footnotes) {
                html << "<li id=\"fn-" << EscapeHtml(kv.first) << "\">" << kv.second
                     << " <a href=\"#fnref-" << EscapeHtml(kv.first) << "\" class=\"footnote-backref\">&#8617;</a></li>";
            }
            html << "</ol></section>\n";
        }

        return html.str();
    }

    static std::string BuildFullHtmlDocument(const std::string& markdown, const std::wstring& baseDir, bool darkMode, const NppDarkModeColors* nppColors = nullptr) {
        std::vector<HeadingItem> headings;
        std::string bodyHtml = CompileToBodyHtml(markdown, baseDir, headings);
        DocStats stats = ComputeStats(markdown);

        std::string bgHex = darkMode ? "#1e1e2e" : "#ffffff";
        std::string fgHex = darkMode ? "#cdd6f4" : "#1f2328";
        std::string surfaceHex = darkMode ? "#181825" : "#f6f8fa";
        std::string borderHex = darkMode ? "#313244" : "#d0d7de";
        std::string accentHex = darkMode ? "#89b4fa" : "#0969da";

        if (darkMode && nppColors && nppColors->background != 0) {
            bgHex = ColorRefToHex(nppColors->pureBackground ? nppColors->pureBackground : nppColors->background);
            surfaceHex = ColorRefToHex(nppColors->softerBackground ? nppColors->softerBackground : nppColors->background);
            fgHex = ColorRefToHex(nppColors->text ? nppColors->text : RGB(220, 220, 220));
            borderHex = ColorRefToHex(nppColors->edge ? nppColors->edge : RGB(70, 70, 70));
            accentHex = ColorRefToHex(nppColors->linkText ? nppColors->linkText : RGB(100, 180, 255));
        }

        std::ostringstream tocSidebar;
        tocSidebar << "<aside id=\"outline-drawer\" class=\"outline-drawer\">"
                   << "<div class=\"outline-header\">Esquema del Documento</div><ul class=\"outline-list\">";
        for (const auto& h : headings) {
            tocSidebar << "<li class=\"outline-lvl-" << h.level << "\">"
                       << "<a href=\"#" << h.slug << "\" onclick=\"jumpToEditorLine(" << h.line << ")\">"
                       << EscapeHtml(h.text) << "</a></li>";
        }
        if (headings.empty()) {
            tocSidebar << "<li class=\"outline-empty\">Sin encabezados (#)</li>";
        }
        tocSidebar << "</ul></aside>";

        std::ostringstream doc;
        doc << R"HTML(<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8" />
<meta name="viewport" content="width=device-width, initial-scale=1.0" />
<title>NppMarkdownStudio Preview</title>
<style>
:root {
    --bg: )HTML" << bgHex << R"HTML(;
    --fg: )HTML" << fgHex << R"HTML(;
    --surface: )HTML" << surfaceHex << R"HTML(;
    --border: )HTML" << borderHex << R"HTML(;
    --accent: )HTML" << accentHex << R"HTML(;
}
* { box-sizing: border-box; }
body {
    margin: 0;
    padding: 0;
    background: var(--bg);
    color: var(--fg);
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Ubuntu, sans-serif;
    font-size: 15px;
    line-height: 1.65;
}
.top-bar {
    position: sticky;
    top: 0;
    z-index: 100;
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 6px 14px;
    background: var(--surface);
    border-bottom: 1px solid var(--border);
    font-size: 12px;
}
.top-bar-actions {
    display: flex;
    gap: 6px;
}
.tb-btn {
    background: var(--bg);
    color: var(--fg);
    border: 1px solid var(--border);
    border-radius: 4px;
    padding: 3px 8px;
    font-size: 11.5px;
    cursor: pointer;
}
.tb-btn:hover { border-color: var(--accent); color: var(--accent); }
.layout-wrap {
    display: flex;
    min-height: calc(100vh - 36px);
}
.preview-container {
    flex: 1;
    max-width: 900px;
    margin: 0 auto;
    padding: 24px 32px 64px;
}
.outline-drawer {
    width: 230px;
    background: var(--surface);
    border-left: 1px solid var(--border);
    padding: 14px;
    display: none;
    overflow-y: auto;
}
.outline-drawer.open { display: block; }
.outline-header { font-weight: 700; font-size: 12px; text-transform: uppercase; margin-bottom: 10px; opacity: 0.8; }
.outline-list { list-style: none; padding: 0; margin: 0; font-size: 13px; }
.outline-list li { margin: 4px 0; }
.outline-lvl-2 { padding-left: 12px; }
.outline-lvl-3 { padding-left: 24px; }
.outline-lvl-4, .outline-lvl-5, .outline-lvl-6 { padding-left: 34px; }
.outline-list a { color: var(--fg); text-decoration: none; }
.outline-list a:hover { color: var(--accent); }

h1, h2, h3, h4, h5, h6 {
    margin-top: 1.4em;
    margin-bottom: 0.5em;
    font-weight: 650;
    line-height: 1.25;
}
h1, h2 { border-bottom: 1px solid var(--border); padding-bottom: 0.3em; }
a { color: var(--accent); text-decoration: none; }
a:hover { text-decoration: underline; }
code.inline-code {
    background: var(--surface);
    border: 1px solid var(--border);
    padding: 0.15em 0.4em;
    border-radius: 4px;
    font-family: "Cascadia Code", Consolas, monospace;
    font-size: 0.9em;
}
.code-wrapper {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 6px;
    margin: 14px 0;
    overflow: hidden;
}
.code-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 4px 12px;
    border-bottom: 1px solid var(--border);
    font-size: 11.5px;
    opacity: 0.85;
}
.copy-btn {
    background: transparent;
    color: var(--fg);
    border: 1px solid var(--border);
    border-radius: 4px;
    padding: 2px 8px;
    cursor: pointer;
    font-size: 11px;
}
pre {
    margin: 0;
    padding: 12px 14px;
    overflow-x: auto;
    font-family: "Cascadia Code", Consolas, monospace;
    font-size: 13.5px;
}
blockquote {
    margin: 14px 0;
    padding: 4px 16px;
    border-left: 4px solid var(--accent);
    background: var(--surface);
    border-radius: 0 6px 6px 0;
}
.gh-alert {
    margin: 14px 0;
    padding: 10px 16px;
    border-left: 4px solid var(--accent);
    background: var(--surface);
    border-radius: 0 6px 6px 0;
}
.gh-alert-title { font-weight: 700; margin-bottom: 4px; text-transform: uppercase; font-size: 12px; }
.gh-alert-note { border-left-color: #0969da; }
.gh-alert-tip { border-left-color: #1a7f37; }
.gh-alert-important { border-left-color: #8250df; }
.gh-alert-warning { border-left-color: #9a6700; }
.gh-alert-caution { border-left-color: #cf222e; }
.table-responsive { overflow-x: auto; margin: 16px 0; }
table.md-table, table.fm-table {
    width: 100%;
    border-collapse: collapse;
    border: 1px solid var(--border);
}
table.md-table th, table.md-table td, table.fm-table th, table.fm-table td {
    border: 1px solid var(--border);
    padding: 8px 12px;
}
table.md-table thead th, table.fm-table th {
    background: var(--surface);
    font-weight: 600;
}
.frontmatter-card {
    margin-bottom: 20px;
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 10px 14px;
}
.frontmatter-title { font-size: 11px; font-weight: 700; text-transform: uppercase; margin-bottom: 6px; opacity: 0.75; }
.task-list-item { list-style: none; margin-left: -20px; }
.task-done { text-decoration: line-through; opacity: 0.65; }
mark { background: #fff8c5; color: #1f2328; padding: 0.1em 0.25em; border-radius: 3px; }
img.md-img { max-width: 100%; height: auto; border-radius: 6px; }
.active-sync-highlight { outline: 2px solid var(--accent); border-radius: 4px; transition: outline 0.2s; }
</style>
</head>
<body>
<div class="top-bar">
    <div class="stats-bar">
        <strong>)HTML" << stats.words << R"HTML(</strong> palabras &middot;
        <strong>)HTML" << stats.chars << R"HTML(</strong> caracteres &middot;
        <strong>)HTML" << stats.lines << R"HTML(</strong> líneas &middot;
        ~<strong>)HTML" << stats.readingMinutes << R"HTML( min</strong> de lectura
    </div>
    <div class="top-bar-actions">
        <button class="tb-btn" onclick="toggleOutline()">Esquema (TOC)</button>
        <button class="tb-btn" onclick="postToNpp('exportHtml')">Exportar HTML</button>
        <button class="tb-btn" onclick="postToNpp('exportPdf')">Exportar PDF</button>
    </div>
</div>
<div class="layout-wrap">
    <main class="preview-container" id="md-root">
)HTML" << bodyHtml << R"HTML(
    </main>
    )HTML" << tocSidebar.str() << R"HTML(
</div>
<script>
function postToNpp(msg) {
    if (window.chrome && window.chrome.webview) {
        window.chrome.webview.postMessage(msg);
    }
}
function toggleOutline() {
    document.getElementById('outline-drawer').classList.toggle('open');
}
function onTaskToggle(line, checked) {
    postToNpp('toggleTask:' + line + ':' + (checked ? '1' : '0'));
}
function jumpToEditorLine(line) {
    postToNpp('jumpToLine:' + line);
}
function copyCodeBlock(btn) {
    const codeEl = btn.closest('.code-wrapper').querySelector('code');
    if (codeEl && navigator.clipboard) {
        navigator.clipboard.writeText(codeEl.innerText);
        btn.innerText = '¡Copiado!';
        setTimeout(() => { btn.innerText = 'Copiar'; }, 1400);
    }
}
// Doble clic en cualquier bloque sincronizado salta a la línea exacta en Notepad++
document.getElementById('md-root').addEventListener('dblclick', (e) => {
    const target = e.target.closest('[data-line]');
    if (target) {
        const line = parseInt(target.getAttribute('data-line'), 10);
        if (!isNaN(line)) postToNpp('jumpToLine:' + line);
    }
});
// Sincronización proporcional de scroll desde Scintilla hacia WebView2
window.syncScrollFromEditor = function(firstVisibleLine) {
    const nodes = Array.from(document.querySelectorAll('[data-line]'));
    if (!nodes.length) return;
    let best = nodes[0];
    for (const node of nodes) {
        const ln = parseInt(node.getAttribute('data-line'), 10);
        if (ln <= firstVisibleLine) best = node;
        else break;
    }
    if (best) {
        best.scrollIntoView({ behavior: 'smooth', block: 'start' });
    }
};
</script>
</body>
</html>)HTML";
        return doc.str();
    }
};

// ----------------------------------------------------------------------------
// Actualización del Panel de Previsualización y Puente Bidireccional
// ----------------------------------------------------------------------------
static void HandlePreviewWebMessage(const std::wstring& msg) {
    HWND hSci = GetCurrentScintilla();
    if (!hSci) return;

    if (msg.rfind(L"jumpToLine:", 0) == 0) {
        int line = std::stoi(msg.substr(11));
        ::SendMessage(hSci, SCI_GOTOLINE, line, 0);
        ::SendMessage(hSci, SCI_GRABFOCUS, 0, 0);
    } else if (msg.rfind(L"toggleTask:", 0) == 0) {
        // Formato: toggleTask:<line>:<0|1>
        size_t secondColon = msg.find(L':', 11);
        if (secondColon != std::wstring::npos) {
            int line = std::stoi(msg.substr(11, secondColon - 11));
            bool checked = (msg.substr(secondColon + 1) == L"1");

            sptr_t lineStart = ::SendMessage(hSci, SCI_POSITIONFROMLINE, line, 0);
            sptr_t lineEnd = ::SendMessage(hSci, SCI_GETLINEENDPOSITION, line, 0);
            std::string lineStr = GetLineText(hSci, line);

            std::string updated = std::regex_replace(
                lineStr,
                std::regex("^(\\s*[-*+]\\s+\\[)[ xX](\\]\\s+.*)$"),
                std::string("$1") + (checked ? "x" : " ") + "$2"
            );

            if (updated != lineStr) {
                g_isUpdatingFromPreview = true;
                ::SendMessage(hSci, SCI_BEGINUNDOACTION, 0, 0);
                ::SendMessage(hSci, SCI_SETTARGETSTART, lineStart, 0);
                ::SendMessage(hSci, SCI_SETTARGETEND, lineEnd, 0);
                ::SendMessage(hSci, SCI_REPLACETARGET, (WPARAM)updated.size(), (LPARAM)updated.c_str());
                ::SendMessage(hSci, SCI_ENDUNDOACTION, 0, 0);
                g_isUpdatingFromPreview = false;
            }
        }
    } else if (msg == L"exportHtml") {
        cmdExportHtml();
    } else if (msg == L"exportPdf") {
        cmdExportPdf();
    }
}

static void RefreshPreviewContent() {
    if (!g_isPanelVisible || !g_hDockWnd) return;

    HWND hSci = GetCurrentScintilla();
    std::string mdText = GetDocumentText(hSci);
    std::wstring baseDir = GetCurrentFileDirectory();

    NppDarkModeColors nppColors{};
    g_isDarkMode = QueryNppDarkMode(&nppColors);
    std::string fullHtml = MarkdownCompiler::BuildFullHtmlDocument(mdText, baseDir, g_isDarkMode, &nppColors);

#if HAS_WEBVIEW2_SDK
    if (g_webViewReady && g_webView) {
        std::wstring wideHtml = Utf8ToWide(fullHtml);
        g_webView->NavigateToString(wideHtml.c_str());
        return;
    }
#endif
    if (g_hFallbackPreviewEdit) {
        std::wstring wideHtml = Utf8ToWide(fullHtml);
        ::SetWindowTextW(g_hFallbackPreviewEdit, wideHtml.c_str());
    }
}

static void SyncPreviewScrollWithEditor() {
    if (!g_isPanelVisible || !g_syncScrollEnabled) return;
    HWND hSci = GetCurrentScintilla();
    if (!hSci) return;
    sptr_t firstLine = ::SendMessage(hSci, SCI_GETFIRSTVISIBLELINE, 0, 0);

#if HAS_WEBVIEW2_SDK
    if (g_webViewReady && g_webView) {
        wchar_t js[128];
        swprintf(js, 128, L"if(window.syncScrollFromEditor) window.syncScrollFromEditor(%lld);", (long long)firstLine);
        g_webView->ExecuteScript(js, nullptr);
    }
#else
    (void)firstLine;
#endif
}

// ----------------------------------------------------------------------------
// Inicialización de WebView2 y Ventana Acoplable (Docking Panel)
// ----------------------------------------------------------------------------
static void InitWebView2InDock(HWND hWndParent) {
#if HAS_WEBVIEW2_SDK
    wchar_t tempPath[MAX_PATH] = {0};
    ::GetTempPathW(MAX_PATH, tempPath);
    std::wstring userDataFolder = std::wstring(tempPath) + L"NppMarkdownStudio_WebView2";

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataFolder.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hWndParent](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) return result;
                env->CreateCoreWebView2Controller(
                    hWndParent,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hWndParent](HRESULT res, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(res) || !controller) return res;
                            g_webViewController = controller;
                            g_webViewController->get_CoreWebView2(&g_webView);

                            RECT bounds;
                            ::GetClientRect(hWndParent, &bounds);
                            g_webViewController->put_Bounds(bounds);
                            g_webViewController->put_IsVisible(TRUE);

                            // Suscribir puente bidireccional JS -> C++ (Scintilla)
                            EventRegistrationToken token;
                            g_webView->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        (void)sender;
                                        LPWSTR pwzMsg = nullptr;
                                        if (SUCCEEDED(args->TryGetWebMessageAsString(&pwzMsg)) && pwzMsg) {
                                            HandlePreviewWebMessage(pwzMsg);
                                            ::CoTaskMemFree(pwzMsg);
                                        }
                                        return S_OK;
                                    }).Get(),
                                &token);

                            g_webViewReady = true;
                            RefreshPreviewContent();
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
#else
    RECT rc;
    ::GetClientRect(hWndParent, &rc);
    g_hFallbackPreviewEdit = ::CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY,
        0, 0, rc.right - rc.left, rc.bottom - rc.top,
        hWndParent, nullptr, g_hModule, nullptr);
#endif
}

static LRESULT CALLBACK DockPanelWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            InitWebView2InDock(hWnd);
            return 0;

        case WM_SIZE: {
            RECT rc;
            ::GetClientRect(hWnd, &rc);
#if HAS_WEBVIEW2_SDK
            if (g_webViewController) {
                g_webViewController->put_Bounds(rc);
            }
#endif
            if (g_hFallbackPreviewEdit) {
                ::MoveWindow(g_hFallbackPreviewEdit, 0, 0, rc.right - rc.left, rc.bottom - rc.top, TRUE);
            }
            return 0;
        }

        case WM_TIMER:
            if (wParam == g_debounceTimerId) {
                ::KillTimer(hWnd, g_debounceTimerId);
                RefreshPreviewContent();
            }
            return 0;

        case WM_NOTIFY: {
            LPNMHDR pnmh = reinterpret_cast<LPNMHDR>(lParam);
            if (pnmh && pnmh->hwndFrom == g_nppData._nppHandle) {
                if (LOWORD(pnmh->code) == DMN_CLOSE) {
                    g_isPanelVisible = false;
                    ::SendMessage(g_nppData._nppHandle, NPPM_SETMENUITEMCHECK, g_funcItems[CMD_TOGGLE_PANEL]._cmdID, FALSE);
                }
            }
            break;
        }

        default:
            break;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void EnsureDockablePanelRegistered() {
    if (g_hDockWnd) return;

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DockPanelWndProc;
    wc.hInstance = g_hModule;
    wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = DOCK_CLASS_NAME;
    ::RegisterClassExW(&wc);

    g_hDockWnd = ::CreateWindowExW(
        0, DOCK_CLASS_NAME, L"Markdown Studio Preview",
        WS_POPUP | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 520, 700,
        g_nppData._nppHandle, nullptr, g_hModule, nullptr);

    tTbData tbData = {0};
    tbData.hClient = g_hDockWnd;
    tbData.pszName = L"Markdown Studio Preview";
    tbData.dlgID = CMD_TOGGLE_PANEL;
    tbData.uMask = DWS_DF_CONT_RIGHT | DWS_ICONTAB;
    tbData.pszModuleName = L"NppMarkdownStudio.dll";

    ::SendMessage(g_nppData._nppHandle, NPPM_DMMREGASDCKDLG, 0, (LPARAM)&tbData);
}

// ----------------------------------------------------------------------------
// Comandos de Edición y Formato Markdown en Scintilla
// ----------------------------------------------------------------------------
static void WrapSelectionWith(const std::string& prefix, const std::string& suffix, const std::string& placeholder) {
    HWND hSci = GetCurrentScintilla();
    if (!hSci) return;

    std::string sel = GetSelectedText(hSci);
    std::string replacement;
    if (sel.empty()) {
        replacement = prefix + placeholder + suffix;
    } else if (sel.size() >= prefix.size() + suffix.size() &&
               sel.rfind(prefix, 0) == 0 &&
               sel.substr(sel.size() - suffix.size()) == suffix) {
        // Si ya tiene el formato aplicado, lo alterna (toggle off)
        replacement = sel.substr(prefix.size(), sel.size() - prefix.size() - suffix.size());
    } else {
        replacement = prefix + sel + suffix;
    }

    ::SendMessage(hSci, SCI_BEGINUNDOACTION, 0, 0);
    ::SendMessage(hSci, SCI_REPLACESEL, 0, (LPARAM)replacement.c_str());
    ::SendMessage(hSci, SCI_ENDUNDOACTION, 0, 0);
}

void cmdTogglePanel() {
    EnsureDockablePanelRegistered();
    g_isPanelVisible = !g_isPanelVisible;
    ::SendMessage(g_nppData._nppHandle, g_isPanelVisible ? NPPM_DMMSHOW : NPPM_DMMHIDE, 0, (LPARAM)g_hDockWnd);
    ::SendMessage(g_nppData._nppHandle, NPPM_SETMENUITEMCHECK, g_funcItems[CMD_TOGGLE_PANEL]._cmdID, g_isPanelVisible);
    if (g_isPanelVisible) {
        RefreshPreviewContent();
    }
}

void cmdToggleSyncScroll() {
    g_syncScrollEnabled = !g_syncScrollEnabled;
    ::SendMessage(g_nppData._nppHandle, NPPM_SETMENUITEMCHECK, g_funcItems[CMD_SYNC_SCROLL]._cmdID, g_syncScrollEnabled);
}

void cmdToggleAutoList() {
    g_autoListEnabled = !g_autoListEnabled;
    ::SendMessage(g_nppData._nppHandle, NPPM_SETMENUITEMCHECK, g_funcItems[CMD_AUTO_LIST_CONTINUATION]._cmdID, g_autoListEnabled);
}

void cmdFormatBold() {
    WrapSelectionWith("**", "**", "texto en negrita");
}

void cmdFormatItalic() {
    WrapSelectionWith("*", "*", "texto en cursiva");
}

void cmdFormatStrikethrough() {
    WrapSelectionWith("~~", "~~", "texto tachado");
}

void cmdFormatCode() {
    HWND hSci = GetCurrentScintilla();
    if (!hSci) return;
    std::string sel = GetSelectedText(hSci);
    if (sel.find('\n') != std::string::npos) {
        WrapSelectionWith("```\n", "\n```", "codigo");
    } else {
        WrapSelectionWith("`", "`", "codigo");
    }
}

void cmdToggleTaskItem() {
    HWND hSci = GetCurrentScintilla();
    if (!hSci) return;
    sptr_t pos = ::SendMessage(hSci, SCI_GETCURRENTPOS, 0, 0);
    sptr_t line = ::SendMessage(hSci, SCI_LINEFROMPOSITION, pos, 0);
    sptr_t lineStart = ::SendMessage(hSci, SCI_POSITIONFROMLINE, line, 0);
    sptr_t lineEnd = ::SendMessage(hSci, SCI_GETLINEENDPOSITION, line, 0);
    std::string text = GetLineText(hSci, line);

    std::string updated;
    if (std::regex_search(text, std::regex("^\\s*[-*+]\\s+\\[ \\]"))) {
        updated = std::regex_replace(text, std::regex("^(\\s*[-*+]\\s+\\[) (\\])"), "$1x$2");
    } else if (std::regex_search(text, std::regex("^\\s*[-*+]\\s+\\[[xX]\\]"))) {
        updated = std::regex_replace(text, std::regex("^(\\s*[-*+]\\s+\\[)[xX](\\])"), "$1 $2");
    } else if (std::regex_search(text, std::regex("^\\s*[-*+]\\s+"))) {
        updated = std::regex_replace(text, std::regex("^(\\s*[-*+]\\s+)"), "$1[ ] ");
    } else {
        updated = "- [ ] " + text;
    }

    ::SendMessage(hSci, SCI_BEGINUNDOACTION, 0, 0);
    ::SendMessage(hSci, SCI_SETTARGETSTART, lineStart, 0);
    ::SendMessage(hSci, SCI_SETTARGETEND, lineEnd, 0);
    ::SendMessage(hSci, SCI_REPLACETARGET, (WPARAM)updated.size(), (LPARAM)updated.c_str());
    ::SendMessage(hSci, SCI_ENDUNDOACTION, 0, 0);
}

void cmdInsertLink() {
    WrapSelectionWith("[", "](https://ejemplo.com)", "texto del enlace");
}

void cmdInsertImage() {
    WrapSelectionWith("![", "](ruta/imagen.png)", "descripción de imagen");
}

void cmdInsertCallout() {
    WrapSelectionWith("> [!NOTE]\n> ", "\n", "Escribe aquí el contenido destacado de la nota.");
}

// Formateador y generador inteligente de tablas GFM en el editor Scintilla
void cmdFormatTable() {
    HWND hSci = GetCurrentScintilla();
    if (!hSci) return;
    std::string sel = GetSelectedText(hSci);

    if (sel.empty() || sel.find('|') == std::string::npos) {
        const char* tpl =
            "| Columna 1 | Columna 2 | Columna 3 |\n"
            "| :--- | :---: | ---: |\n"
            "| Dato A | Dato B | 100 |\n"
            "| Dato C | Dato D | 250 |\n";
        ::SendMessage(hSci, SCI_BEGINUNDOACTION, 0, 0);
        ::SendMessage(hSci, SCI_REPLACESEL, 0, (LPARAM)tpl);
        ::SendMessage(hSci, SCI_ENDUNDOACTION, 0, 0);
        return;
    }

    // Si hay una tabla seleccionada, alinea todas las columnas con padding uniforme
    std::istringstream ss(sel);
    std::string line;
    std::vector<std::vector<std::string>> rows;
    size_t maxCols = 0;

    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find('|') == std::string::npos) continue;
        std::string s = line;
        if (!s.empty() && s.front() == '|') s.erase(s.begin());
        if (!s.empty() && s.back() == '|') s.pop_back();
        std::stringstream rs(s);
        std::string cell;
        std::vector<std::string> cells;
        while (std::getline(rs, cell, '|')) {
            auto first = cell.find_first_not_of(" \t");
            auto last = cell.find_last_not_of(" \t");
            cells.push_back(first == std::string::npos ? "" : cell.substr(first, last - first + 1));
        }
        maxCols = std::max(maxCols, cells.size());
        rows.push_back(cells);
    }

    std::vector<size_t> widths(maxCols, 3);
    for (const auto& r : rows) {
        for (size_t c = 0; c < r.size(); ++c) {
            widths[c] = std::max(widths[c], r[c].size());
        }
    }

    std::ostringstream out;
    for (size_t r = 0; r < rows.size(); ++r) {
        out << "|";
        for (size_t c = 0; c < maxCols; ++c) {
            std::string val = (c < rows[r].size()) ? rows[r][c] : "";
            out << " " << val << std::string(widths[c] - val.size() + 1, ' ') << "|";
        }
        if (r + 1 < rows.size()) out << "\n";
    }

    std::string aligned = out.str();
    ::SendMessage(hSci, SCI_BEGINUNDOACTION, 0, 0);
    ::SendMessage(hSci, SCI_REPLACESEL, 0, (LPARAM)aligned.c_str());
    ::SendMessage(hSci, SCI_ENDUNDOACTION, 0, 0);
}

// Generador automático de Tabla de Contenidos (TOC) en Markdown
void cmdGenerateTOC() {
    HWND hSci = GetCurrentScintilla();
    if (!hSci) return;
    std::string md = GetDocumentText(hSci);
    std::vector<HeadingItem> headings;
    MarkdownCompiler::CompileToBodyHtml(md, L"", headings);

    std::ostringstream toc;
    toc << "## Tabla de Contenidos\n\n";
    for (const auto& h : headings) {
        if (h.level > 1) {
            toc << std::string((h.level - 2) * 2, ' ') << "- [" << h.text << "](#" << h.slug << ")\n";
        }
    }
    toc << "\n";
    std::string result = toc.str();

    ::SendMessage(hSci, SCI_BEGINUNDOACTION, 0, 0);
    ::SendMessage(hSci, SCI_REPLACESEL, 0, (LPARAM)result.c_str());
    ::SendMessage(hSci, SCI_ENDUNDOACTION, 0, 0);
}

void cmdExportHtml() {
    HWND hSci = GetCurrentScintilla();
    if (!hSci) return;

    wchar_t filePath[MAX_PATH] = L"documento_markdown.html";
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_nppData._nppHandle;
    ofn.lpstrFilter = L"Documento HTML (*.html)\0*.html\0Todos los archivos (*.*)\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"html";

    if (::GetSaveFileNameW(&ofn)) {
        std::string md = GetDocumentText(hSci);
        std::string html = MarkdownCompiler::BuildFullHtmlDocument(md, GetCurrentFileDirectory(), false, nullptr);
        std::ofstream out(filePath, std::ios::binary);
        out.write(html.data(), (std::streamsize)html.size());
        out.close();
        ::MessageBoxW(g_nppData._nppHandle, L"Archivo HTML exportado correctamente.", PLUGIN_NAME, MB_OK | MB_ICONINFORMATION);
    }
}

void cmdExportPdf() {
#if HAS_WEBVIEW2_SDK
    if (g_webViewReady && g_webView) {
        ComPtr<ICoreWebView2_7> webView7;
        if (SUCCEEDED(g_webView.As(&webView7)) && webView7) {
            wchar_t pdfPath[MAX_PATH] = L"documento_markdown.pdf";
            OPENFILENAMEW ofn = {0};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = g_nppData._nppHandle;
            ofn.lpstrFilter = L"Archivo PDF (*.pdf)\0*.pdf\0";
            ofn.lpstrFile = pdfPath;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            ofn.lpstrDefExt = L"pdf";

            if (::GetSaveFileNameW(&ofn)) {
                webView7->PrintToPdf(pdfPath, nullptr, nullptr);
                return;
            }
        }
    }
#endif
    ::MessageBoxW(g_nppData._nppHandle,
                  L"La exportación directa a PDF requiere el motor WebView2 activo en el panel de vista previa.",
                  PLUGIN_NAME, MB_OK | MB_ICONINFORMATION);
}

void cmdAbout() {
    const wchar_t* msg =
        L"NppMarkdownStudio v1.0 (Native C++ Plugin)\n\n"
        L"Suite integral de edición y previsualización Markdown para Notepad++:\n"
        L"• Panel acoplable nativo (NPPM_DMMREGASDCKDLG) con WebView2\n"
        L"• Sincronización bidireccional de scroll y salto por doble clic\n"
        L"• Listas de tareas interactivas, Alertas GitHub, Tablas GFM, Matemáticas y Mermaid\n"
        L"• Sincronización automática con el Modo Oscuro de Notepad++";
    ::MessageBoxW(g_nppData._nppHandle, msg, L"Acerca de NppMarkdownStudio", MB_OK | MB_ICONINFORMATION);
}

// ----------------------------------------------------------------------------
// Auto-continuación inteligente de listas al presionar Enter (SCN_CHARADDED)
// ----------------------------------------------------------------------------
static void OnCharAdded(int ch) {
    if (!g_autoListEnabled || ch != '\n') return;
    HWND hSci = GetCurrentScintilla();
    if (!hSci) return;

    sptr_t pos = ::SendMessage(hSci, SCI_GETCURRENTPOS, 0, 0);
    sptr_t curLine = ::SendMessage(hSci, SCI_LINEFROMPOSITION, pos, 0);
    if (curLine <= 0) return;

    std::string prevLine = GetLineText(hSci, curLine - 1);
    std::smatch m;

    // Si la línea anterior era un ítem de lista vacío (ej. "- " o "- [ ] "), se limpia la viñeta
    if (std::regex_match(prevLine, m, std::regex("^(\\s*([-*+]|\\d+\\.)\\s+(\\[[ xX]\\]\\s+)?)$"))) {
        sptr_t prevStart = ::SendMessage(hSci, SCI_POSITIONFROMLINE, curLine - 1, 0);
        sptr_t prevEnd = ::SendMessage(hSci, SCI_GETLINEENDPOSITION, curLine - 1, 0);
        ::SendMessage(hSci, SCI_SETTARGETSTART, prevStart, 0);
        ::SendMessage(hSci, SCI_SETTARGETEND, prevEnd, 0);
        ::SendMessage(hSci, SCI_REPLACETARGET, 0, (LPARAM)"");
        return;
    }

    // Continuar tarea (- [ ] )
    if (std::regex_match(prevLine, m, std::regex("^(\\s*[-*+]\\s+)\\[[ xX]\\]\\s+.+$"))) {
        std::string nextPrefix = m[1].str() + "[ ] ";
        ::SendMessage(hSci, SCI_REPLACESEL, 0, (LPARAM)nextPrefix.c_str());
    }
    // Continuar viñeta (- , * , + )
    else if (std::regex_match(prevLine, m, std::regex("^(\\s*[-*+]\\s+).+$"))) {
        std::string nextPrefix = m[1].str();
        ::SendMessage(hSci, SCI_REPLACESEL, 0, (LPARAM)nextPrefix.c_str());
    }
    // Continuar lista numerada (1. -> 2. )
    else if (std::regex_match(prevLine, m, std::regex("^(\\s*)(\\d+)\\.\\s+.+$"))) {
        int nextNum = std::stoi(m[2].str()) + 1;
        std::string nextPrefix = m[1].str() + std::to_string(nextNum) + ". ";
        ::SendMessage(hSci, SCI_REPLACESEL, 0, (LPARAM)nextPrefix.c_str());
    }
}

// ----------------------------------------------------------------------------
// Exportaciones Oficiales de la DLL para Notepad++ (Plugin API)
// ----------------------------------------------------------------------------
static void RegisterCommand(int index, const wchar_t* name, PFUNCPLUGINCMD pFunc, bool checkInit = false,
                            bool ctrl = false, bool alt = false, bool shift = false, UCHAR key = 0) {
    wcsncpy_s(g_funcItems[index]._itemName, nbChar, name, _TRUNCATE);
    g_funcItems[index]._pFunc = pFunc;
    g_funcItems[index]._init2Check = checkInit;
    if (key != 0) {
        g_shortcuts[index] = {ctrl, alt, shift, key};
        g_funcItems[index]._pShKey = &g_shortcuts[index];
    } else {
        g_funcItems[index]._pShKey = nullptr;
    }
}

extern "C" __declspec(dllexport) void setInfo(NppData notpadPlusData) {
    g_nppData = notpadPlusData;

    RegisterCommand(CMD_TOGGLE_PANEL,           L"Mostrar/Ocultar Panel Markdown", cmdTogglePanel, false, true, false, true, 'M');
    RegisterCommand(CMD_SYNC_SCROLL,            L"Sincronizar Scroll Bidireccional", cmdToggleSyncScroll, true);
    RegisterCommand(CMD_AUTO_LIST_CONTINUATION, L"Auto-continuar Listas al dar Enter", cmdToggleAutoList, true);
    RegisterCommand(CMD_SEP_1,                  L"-SEPARATOR-", nullptr);
    RegisterCommand(CMD_FORMAT_BOLD,            L"Negrita (**texto**)", cmdFormatBold, false, true, false, false, 'B');
    RegisterCommand(CMD_FORMAT_ITALIC,          L"Cursiva (*texto*)", cmdFormatItalic, false, true, false, false, 'I');
    RegisterCommand(CMD_FORMAT_STRIKETHROUGH,   L"Tachado (~~texto~~)", cmdFormatStrikethrough);
    RegisterCommand(CMD_FORMAT_CODE,            L"Código en línea / Bloque (`código`)", cmdFormatCode, false, true, false, true, 'K');
    RegisterCommand(CMD_TOGGLE_TASK_ITEM,       L"Alternar Tarea (- [ ] / - [x])", cmdToggleTaskItem, false, true, false, true, 'X');
    RegisterCommand(CMD_INSERT_LINK,            L"Insertar Enlace ([texto](url))", cmdInsertLink, false, true, false, false, 'K');
    RegisterCommand(CMD_INSERT_IMAGE,           L"Insertar Imagen (![alt](img))", cmdInsertImage);
    RegisterCommand(CMD_INSERT_CALLOUT,         L"Insertar Alerta GitHub (> [!NOTE])", cmdInsertCallout);
    RegisterCommand(CMD_FORMAT_TABLE,           L"Insertar / Alinear Tabla GFM", cmdFormatTable, false, true, true, false, 'T');
    RegisterCommand(CMD_GENERATE_TOC,           L"Generar Tabla de Contenidos (TOC)", cmdGenerateTOC);
    RegisterCommand(CMD_SEP_2,                  L"-SEPARATOR-", nullptr);
    RegisterCommand(CMD_EXPORT_HTML,            L"Exportar Vista Previa a HTML...", cmdExportHtml);
    RegisterCommand(CMD_EXPORT_PDF,             L"Exportar Vista Previa a PDF...", cmdExportPdf);
    RegisterCommand(CMD_ABOUT,                  L"Acerca de NppMarkdownStudio...", cmdAbout);
}

extern "C" __declspec(dllexport) const wchar_t* getName() {
    return PLUGIN_NAME;
}

extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* nbF) {
    *nbF = CMD_COUNT;
    return g_funcItems;
}

extern "C" __declspec(dllexport) void beNotified(SCNotification* notifyCode) {
    if (!notifyCode) return;

    switch (notifyCode->nmhdr.code) {
        case NPPN_BUFFERACTIVATED:
        case NPPN_FILEOPENED:
        case NPPN_LANGCHANGED:
        case NPPN_DARKMODECHANGED:
            if (g_isPanelVisible) {
                RefreshPreviewContent();
            }
            break;

        case SCN_MODIFIED:
            if (!g_isUpdatingFromPreview && g_isPanelVisible &&
                (notifyCode->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))) {
                if (g_hDockWnd) {
                    // Debounce de 120ms para renderizado fluido al escribir en documentos grandes
                    ::SetTimer(g_hDockWnd, g_debounceTimerId, 120, nullptr);
                }
            }
            break;

        case SCN_UPDATEUI:
            if (g_isPanelVisible && (notifyCode->updated & (SC_UPDATE_V_SCROLL | SC_UPDATE_SELECTION))) {
                SyncPreviewScrollWithEditor();
            }
            break;

        case SCN_CHARADDED:
            OnCharAdded(notifyCode->ch);
            break;

        case NPPN_SHUTDOWN:
            if (g_hDockWnd) {
                ::DestroyWindow(g_hDockWnd);
                g_hDockWnd = nullptr;
            }
            break;

        default:
            break;
    }
}

extern "C" __declspec(dllexport) LRESULT messageProc(UINT /*Message*/, WPARAM /*wParam*/, LPARAM /*lParam*/) {
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL isUnicode() {
    return TRUE;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reasonForCall, LPVOID /*lpReserved*/) {
    if (reasonForCall == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        ::DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}