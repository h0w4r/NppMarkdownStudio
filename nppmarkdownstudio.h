// ============================================================================
// NppMarkdownStudio.h
// Cabecera autocontenida con definiciones oficiales de Notepad++ Plugin API,
// Scintilla Messages, Docking Manager (NPPM_DMM*) y Dark Mode API (Notepad++ 8.4+)
// ============================================================================
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <regex>
#include <map>
#include <algorithm>
#include <cwctype>

// ----------------------------------------------------------------------------
// Constantes y estructuras oficiales de Notepad++ Plugin Interface
// ----------------------------------------------------------------------------
constexpr int nbChar = 64;

struct NppData {
    HWND _nppHandle = nullptr;
    HWND _scintillaMainHandle = nullptr;
    HWND _scintillaSecondHandle = nullptr;
};

struct ShortcutKey {
    bool _isCtrl = false;
    bool _isAlt = false;
    bool _isShift = false;
    UCHAR _key = 0;
};

typedef void (__cdecl * PFUNCPLUGINCMD)();

struct FuncItem {
    wchar_t _itemName[nbChar] = {0};
    PFUNCPLUGINCMD _pFunc = nullptr;
    int _cmdID = 0;
    bool _init2Check = false;
    ShortcutKey* _pShKey = nullptr;
};

// ----------------------------------------------------------------------------
// Notificaciones y Mensajes de Notepad++ (NPPM_* y NPPN_*)
// ----------------------------------------------------------------------------
#define NPPMSG                  (WM_USER + 1000)
#define NPPM_GETCURRENTSCINTILLA (NPPMSG + 4)
#define NPPM_GETFULLCURRENTPATH  (NPPMSG + 5)
#define NPPM_GETCURRENTDIRECTORY (NPPMSG + 6)
#define NPPM_GETFILENAME         (NPPMSG + 7)
#define NPPM_GETEXTPART          (NPPMSG + 8)
#define NPPM_SETMENUITEMCHECK    (NPPMSG + 40)
#define NPPM_DMMREGASDCKDLG      (NPPMSG + 33)
#define NPPM_DMMSHOW             (NPPMSG + 30)
#define NPPM_DMMHIDE             (NPPMSG + 31)
#define NPPM_DMMUPDATEDISPINFO   (NPPMSG + 32)
#define NPPM_GETPLUGINSCONFIGDIR (NPPMSG + 46)
#define NPPM_ISDARKMODEENABLED   (NPPMSG + 107)
#define NPPM_GETDARKMODECOLORS   (NPPMSG + 108)

#define NPPN_FIRST               1000
#define NPPN_READY               (NPPN_FIRST + 1)
#define NPPN_TBMODIFICATION      (NPPN_FIRST + 2)
#define NPPN_FILEBEFORECLOSE     (NPPN_FIRST + 3)
#define NPPN_FILEOPENED          (NPPN_FIRST + 4)
#define NPPN_FILECLOSED          (NPPN_FIRST + 5)
#define NPPN_FILEBSAVED          (NPPN_FIRST + 7)
#define NPPN_SHUTDOWN            (NPPN_FIRST + 8)
#define NPPN_BUFFERACTIVATED     (NPPN_FIRST + 10)
#define NPPN_LANGCHANGED         (NPPN_FIRST + 11)
#define NPPN_DARKMODECHANGED     (NPPN_FIRST + 27)

// Estructuras de Dark Mode de Notepad++
struct NppDarkModeColors {
    COLORREF background = 0;
    COLORREF softerBackground = 0;
    COLORREF hotBackground = 0;
    COLORREF pureBackground = 0;
    COLORREF errorBackground = 0;
    COLORREF text = 0;
    COLORREF darkerText = 0;
    COLORREF disabledText = 0;
    COLORREF linkText = 0;
    COLORREF edge = 0;
    COLORREF hotEdge = 0;
    COLORREF disabledEdge = 0;
};

// ----------------------------------------------------------------------------
// Definiciones del Docking Manager de Notepad++ (Docking.h)
// ----------------------------------------------------------------------------
#define DWS_ICONTAB         0x00000001
#define DWS_ICONBAR         0x00000002
#define DWS_ADDINFO         0x00000004
#define DWS_PARAMSALL       (DWS_ICONTAB | DWS_ICONBAR | DWS_ADDINFO)

#define DWS_DF_CONT_LEFT    (0 << 28)
#define DWS_DF_CONT_RIGHT   (1 << 28)
#define DWS_DF_CONT_TOP     (2 << 28)
#define DWS_DF_CONT_BOTTOM  (3 << 28)
#define DWS_DF_FLOATING     0x80000000

#define DMN_FIRST           1050
#define DMN_CLOSE           (DMN_FIRST + 1)
#define DMN_DOCK            (DMN_FIRST + 2)
#define DMN_FLOAT           (DMN_FIRST + 3)

struct tTbData {
    HWND hClient = nullptr;
    const wchar_t* pszName = nullptr;
    int dlgID = 0;
    UINT uMask = 0;
    HICON hIconTab = nullptr;
    const wchar_t* pszAddInfo = nullptr;
    RECT rcFloat = {0, 0, 0, 0};
    int iPrevCont = -1;
    const wchar_t* pszModuleName = nullptr;
};

// ----------------------------------------------------------------------------
// Mensajes y Notificaciones de Scintilla (Scintilla.h)
// ----------------------------------------------------------------------------
typedef intptr_t sptr_t;
typedef uintptr_t uptr_t;

struct Sci_NotifyHeader {
    void* hwndFrom = nullptr;
    uptr_t idFrom = 0;
    unsigned int code = 0;
};

struct SCNotification {
    Sci_NotifyHeader nmhdr;
    intptr_t position = 0;
    int ch = 0;
    int modifiers = 0;
    int modificationType = 0;
    const char* text = nullptr;
    intptr_t length = 0;
    intptr_t linesAdded = 0;
    int message = 0;
    uptr_t wParam = 0;
    sptr_t lParam = 0;
    intptr_t line = 0;
    int foldLevelNow = 0;
    int foldLevelPrev = 0;
    int margin = 0;
    int listType = 0;
    int x = 0;
    int y = 0;
    int token = 0;
    intptr_t annotationLinesAdded = 0;
    int updated = 0;
    int listCompletionMethod = 0;
    int characterSource = 0;
};

#define SCI_GETLENGTH               2006
#define SCI_GETCHARAT               2007
#define SCI_GETCURRENTPOS           2008
#define SCI_GETANCHOR               2009
#define SCI_SETSEL                  2160
#define SCI_GOTOLINE                2024
#define SCI_GOTOPOS                 2025
#define SCI_GETLINECOUNT            2154
#define SCI_GETFIRSTVISIBLELINE     2152
#define SCI_LINESONSCREEN           2370
#define SCI_LINEFROMPOSITION        2166
#define SCI_POSITIONFROMLINE        2167
#define SCI_GETLINEENDPOSITION      2136
#define SCI_GETSELTEXT              2161
#define SCI_REPLACESEL              2170
#define SCI_GETTEXTRANGE            2162
#define SCI_GETTEXT                 2182
#define SCI_GETLINE                 2153
#define SCI_SETTARGETSTART          2190
#define SCI_SETTARGETEND            2192
#define SCI_REPLACETARGET           2194
#define SCI_BEGINUNDOACTION         2078
#define SCI_ENDUNDOACTION           2079
#define SCI_GRABFOCUS               2400

#define SCN_STYLENEEDED             2000
#define SCN_CHARADDED               2001
#define SCN_SAVEPOINTREACHED        2002
#define SCN_SAVEPOINTLEFT           2003
#define SCN_UPDATEUI                2007
#define SCN_MODIFIED                2008

#define SC_MOD_INSERTTEXT           0x1
#define SC_MOD_DELETETEXT           0x2
#define SC_UPDATE_CONTENT           0x1
#define SC_UPDATE_SELECTION         0x2
#define SC_UPDATE_V_SCROLL          0x4

// ----------------------------------------------------------------------------
// Índices de Comandos del Plugin NppMarkdownStudio
// ----------------------------------------------------------------------------
enum PluginCommandID {
    CMD_TOGGLE_PANEL = 0,
    CMD_SYNC_SCROLL,
    CMD_AUTO_LIST_CONTINUATION,
    CMD_SEP_1,
    CMD_FORMAT_BOLD,
    CMD_FORMAT_ITALIC,
    CMD_FORMAT_STRIKETHROUGH,
    CMD_FORMAT_CODE,
    CMD_TOGGLE_TASK_ITEM,
    CMD_INSERT_LINK,
    CMD_INSERT_IMAGE,
    CMD_INSERT_CALLOUT,
    CMD_FORMAT_TABLE,
    CMD_GENERATE_TOC,
    CMD_SEP_2,
    CMD_EXPORT_HTML,
    CMD_EXPORT_PDF,
    CMD_ABOUT,
    CMD_COUNT
};