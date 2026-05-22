//this file is part of notepad++
//Copyright (C)2003 Don HO <donho@altern.org>
//
//This program is free software; you can redistribute it and/or
//modify it under the terms of the GNU General Public License
//as published by the Free Software Foundation; either
//version 2 of the License, or (at your option) any later version.
//
//This program is distributed in the hope that it will be useful,
//but WITHOUT ANY WARRANTY; without even the implied warranty of
//MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//GNU General Public License for more details.
//
//You should have received a copy of the GNU General Public License
//along with this program; if not, write to the Free Software
//Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

#include "PluginDefinition.h"
#include "menuCmdID.h"
#include "Scintilla.h"
#include "Notepad_plus_msgs.h"
#include <shlwapi.h>

//
// Globals
//
HINSTANCE hInstance = NULL;
unsigned char cryptkey[MAX_CRYPT_KEY];

// Per-buffer cached keys for auto encrypt/decrypt of .ctxt files
static std::map<UINT_PTR, std::string> g_bufferKeys;
// Buffer currently mid-save (encrypted, awaiting post-save decrypt)
static UINT_PTR g_pendingDecryptBuffer = 0;
// Buffer pre-encrypted by EncryptSaveAsCtxt — tells OnFileBeforeSave to skip
// re-encrypting (NPPM_SAVECURRENTFILEAS fires BEFORESAVE with the old path,
// so the path-based check in the hook would miss it anyway).
static UINT_PTR g_skipNextBeforeSaveEncrypt = 0;
// Buffers opened as .ctxt awaiting lazy decrypt-on-view (set on FILEOPENED,
// consumed on BUFFERACTIVATED). Lets file-open complete without blocking
// on a modal prompt; the popup only appears when the user actually views
// the document.
static std::set<UINT_PTR> g_pendingOpenDecrypt;

//
// Fwd decl
//
void EncryptDoc() { CryptDoc(CryptAction::Encrypt); }
void DecryptDoc() { CryptDoc(CryptAction::Decrypt); }
void EncryptSelection() { CryptSelection(CryptAction::Encrypt); }
void DecryptSelection() { CryptSelection(CryptAction::Decrypt); }
LRESULT CALLBACK DlgProcCryptKey(HWND hWndDlg, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK DlgProcCryptKeySingle(HWND hWndDlg, UINT msg, WPARAM wParam, LPARAM lParam);
void StrCrypt(unsigned char *buf1, size_t buf1len, unsigned char *buf2, size_t buf2len, unsigned char *key, CryptAction action);
std::wstring widen(const std::string& str);
std::string narrow(const std::wstring& str);

//
// The plugin data that Notepad++ needs
//
FuncItem funcItem[nbFunc];

//
// The data of Notepad++ that you can use in your plugin commands
//
NppData nppData;

//
// Initialize your plugin data here
// It will be called while plugin loading
void pluginInit(HANDLE hModule)
{
    hInstance = (HINSTANCE)hModule;
}

//
// Here you can do the clean up, save the parameters (if any) for the next session
//
void pluginCleanUp()
{
}

//
// Initialization of your plugin commands
// You should fill your plugins commands here
void commandMenuInit()
{

    //--------------------------------------------//
    //-- STEP 3. CUSTOMIZE YOUR PLUGIN COMMANDS --//
    //--------------------------------------------//
    // with function :
    // setCommand(int index,                      // zero based number to indicate the order of command
    //            TCHAR *commandName,             // the command name that you want to see in plugin menu
    //            PFUNCPLUGINCMD functionPointer, // the symbol of function (function pointer) associated with this command. The body should be defined below. See Step 4.
    //            ShortcutKey *shortcut,          // optional. Define a shortcut to trigger this command
    //            bool check0nInit                // optional. Make this menu item be checked visually
    //            );
    setCommand(0, TEXT("Encrypt Document"), EncryptDoc, NULL, false);
    setCommand(1, TEXT("Decrypt Document"), DecryptDoc, NULL, false);
    setCommand(2, TEXT("Encrypt Selected Text"), EncryptSelection, NULL, false);
    setCommand(3, TEXT("Decrypt Selected Text"), DecryptSelection, NULL, false);
    setCommand(4, TEXT("Encrypt && Save As .ctxt..."), EncryptSaveAsCtxt, NULL, false);
    setCommand(5, TEXT("About SecurePad"), AboutDlg, NULL, false);
}

//
// Here you can do the clean up (especially for the shortcut)
//
void commandMenuCleanUp()
{
    // Don't forget to deallocate your shortcut here
}


//
// This function help you to initialize your plugin commands
//
bool setCommand(size_t index, TCHAR *cmdName, PFUNCPLUGINCMD pFunc, ShortcutKey *sk, bool check0nInit)
{
    if (index >= nbFunc)
        return false;

    if (!pFunc)
        return false;

    lstrcpy(funcItem[index]._itemName, cmdName);
    funcItem[index]._pFunc = pFunc;
    funcItem[index]._init2Check = check0nInit;
    funcItem[index]._pShKey = sk;

    return true;
}

//----------------------------------------------//
//-- STEP 4. DEFINE YOUR ASSOCIATED FUNCTIONS --//
//----------------------------------------------//
void CryptDoc(CryptAction action)
{
    int currentEdit = -1;
    HWND hCurrentEditView = NULL;
    size_t textLength = 0L, textLengthOut = 0L;
    TCHAR *textBuf = NULL, *textBufOut = NULL;

    // Prompt for crypt key — encrypt = confirm (two fields), decrypt = single field
    INT_PTR ret;
    if (action == CryptAction::Encrypt)
        ret = DialogBox(hInstance, MAKEINTRESOURCE(IDD_DLGKEY), nppData._nppHandle, reinterpret_cast<DLGPROC>(DlgProcCryptKey));
    else
        ret = DialogBox(hInstance, MAKEINTRESOURCE(IDD_DLGKEY_SINGLE), nppData._nppHandle, reinterpret_cast<DLGPROC>(DlgProcCryptKeySingle));

    if(ret == 0)
    {
        return;
    }

    // Get edit view handle
    SendMessage(nppData._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, (LPARAM)&currentEdit);
    if(currentEdit == 0) hCurrentEditView = nppData._scintillaMainHandle;
    else hCurrentEditView = nppData._scintillaSecondHandle;

    // Get document text length
    textLength = (ULONG)SendMessage(hCurrentEditView, SCI_GETTEXTLENGTH, 0, 0);

    if(textLength == 0)
    {
        MessageBox(nppData._nppHandle, TEXT("The document is empty!"), TEXT("Error"), MB_OK);
        return;
    }

    textLength++;
    textLengthOut = textLength;

    if(action == CryptAction::Encrypt) textLengthOut *= 2;

    // Create input buffer  (round up to nearest 8 multiplier)
    while(textLength % 8 != 0)textLength++;
    textBuf = (TCHAR*)VirtualAlloc(NULL, textLength, MEM_COMMIT, PAGE_EXECUTE_READWRITE);

    // Create output buffer (round up to nearest 8 multiplier)
    while(textLengthOut % 8 != 0)textLengthOut++;
    textBufOut = (TCHAR*)VirtualAlloc(NULL, textLengthOut, MEM_COMMIT, PAGE_EXECUTE_READWRITE);

    // Get text
    SendMessage(hCurrentEditView, SCI_GETTEXT, textLength, (LPARAM)textBuf);

    // Crypt the text
    if(action == CryptAction::Encrypt)
        StrCrypt((unsigned char*)textBuf, textLength, (unsigned char*)textBufOut, textLengthOut, (unsigned char*)cryptkey, CryptAction::Encrypt);
    else
        StrCrypt((unsigned char*)textBuf, textLength, (unsigned char*)textBufOut, textLengthOut, (unsigned char*)cryptkey, CryptAction::Decrypt);

    // Output to view
    SendMessage(hCurrentEditView, SCI_SETTEXT, 0, (LPARAM)textBufOut);

    // Cleanup
    if(textBuf)
    {
        VirtualFree(textBuf, 0, MEM_RELEASE);
    }
    if (textBufOut)
    {
        VirtualFree(textBufOut, 0, MEM_RELEASE);
    }
}

void CryptSelection(CryptAction action)
{
    int currentEdit = -1;
    HWND hCurrentEditView = NULL;
    size_t textLength = 0L, textLengthOut = 0L;
    TCHAR *textBuf = NULL, *textBufOut = NULL;

    // Prompt for crypt key — encrypt = confirm (two fields), decrypt = single field
    INT_PTR ret;
    if (action == CryptAction::Encrypt)
        ret = DialogBox(hInstance, MAKEINTRESOURCE(IDD_DLGKEY), nppData._nppHandle, reinterpret_cast<DLGPROC>(DlgProcCryptKey));
    else
        ret = DialogBox(hInstance, MAKEINTRESOURCE(IDD_DLGKEY_SINGLE), nppData._nppHandle, reinterpret_cast<DLGPROC>(DlgProcCryptKeySingle));

    if(ret == 0)
    {
        return;
    }

    // Get edit view handle
    SendMessage(nppData._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, (LPARAM)&currentEdit);
    if(currentEdit == 0) hCurrentEditView = nppData._scintillaMainHandle;
    else hCurrentEditView = nppData._scintillaSecondHandle;

    // Get selected text length
    Sci_PositionCR selectStart = SendMessage(hCurrentEditView, SCI_GETSELECTIONSTART, 0, 0);
    Sci_PositionCR selectEnd = SendMessage(hCurrentEditView, SCI_GETSELECTIONEND, 0, 0);

    if(selectEnd == 0 || (selectEnd - selectStart) == 0)
    {
        MessageBox(nppData._nppHandle, TEXT("No text was selected!"), TEXT("Error"), MB_OK);
        return;
    }

    textLength = (selectEnd - selectStart);
    textLength++;
    textLengthOut = textLength;
    if(action == CryptAction::Encrypt) textLengthOut *= 2;

    // Create input buffer  (round up to nearest 8 multiplier)
    while(textLength % 8 != 0)textLength++;
    textBuf = (TCHAR*)VirtualAlloc(NULL, textLength, MEM_COMMIT, PAGE_EXECUTE_READWRITE);

    // Create output buffer (round up to nearest 8 multiplier)
    while(textLengthOut % 8 != 0)textLengthOut++;
    textBufOut = (TCHAR*)VirtualAlloc(NULL, textLengthOut, MEM_COMMIT, PAGE_EXECUTE_READWRITE);

    // Get text range
    struct Sci_TextRange tr = {{selectStart, selectEnd}, reinterpret_cast<char*>(textBuf)};
    SendMessage(hCurrentEditView, SCI_GETTEXTRANGE, 0, (LPARAM)&tr);

    // Crypt the text
    if(action == CryptAction::Encrypt)
        StrCrypt((unsigned char*)textBuf, textLength, (unsigned char*)textBufOut, textLengthOut, (unsigned char*)cryptkey, CryptAction::Encrypt);
    else
        StrCrypt((unsigned char*)textBuf, textLength, (unsigned char*)textBufOut, textLengthOut, (unsigned char*)cryptkey, CryptAction::Decrypt);

    // Output to view
    SendMessage(hCurrentEditView, SCI_REPLACESEL, 0, (LPARAM)textBufOut);

    // Cleanup
    if (textBuf)
    {
        VirtualFree(textBuf, 0, MEM_RELEASE);
    }
    if (textBufOut)
    {
        VirtualFree(textBufOut, 0, MEM_RELEASE);
    }
}

void AboutDlg()
{
    ::MessageBox(NULL, TEXT("SecurePad can be used to securely encrypt plaintext documents with a key of your choice. Be careful, once encrypted you will only be able to decrypt with the key you used!\r\n\r\nAny questions please visit www.dominictobias.com."), TEXT("SecurePad v2.5"), MB_OK);
}

// ===============================================
//  Dialog for key entry
// ===============================================

LRESULT CALLBACK DlgProcCryptKey(HWND hWndDlg, UINT msg, WPARAM wParam, LPARAM /*lParam*/)
{
    switch(msg)
    {
        case WM_DESTROY:
        case WM_CLOSE:
        {
            EndDialog(hWndDlg, 0);
            return TRUE;
        }

        case WM_INITDIALOG:
        {
            SetFocus(GetDlgItem(hWndDlg, IDC_EDIT1));
            break;
        }

        case WM_COMMAND:
        {
            if(LOWORD(wParam) == IDOK)
            {
                TCHAR box1[MAX_CRYPT_KEY] = { 0 };
                TCHAR box2[MAX_CRYPT_KEY] = { 0 };
                LRESULT keyLen = 0;

                SendMessage(GetDlgItem(hWndDlg, IDC_EDIT1), WM_GETTEXT, sizeof(box1), (LPARAM)box1);
                SendMessage(GetDlgItem(hWndDlg, IDC_EDIT2), WM_GETTEXT, sizeof(box2), (LPARAM)box2);

                if(_tcslen(box1) == 0 || _tcslen(box2) == 0)
                {
                    MessageBox(nppData._nppHandle, TEXT("Both fields must be filled in!"), TEXT("Please try again"), MB_OK);
                    break;
                }

                if(_tcscmp(box1, box2))
                {
                    MessageBox(nppData._nppHandle, TEXT("The keys did not match!"), TEXT("Please try again"), MB_OK);
                    break;
                }

                keyLen = SendMessage(GetDlgItem(hWndDlg, IDC_EDIT1), WM_GETTEXTLENGTH, 0, 0);

                if(keyLen > MAX_CRYPT_KEY)
                {
                    TCHAR buf[128];
                    _stprintf(buf, TEXT("The key was too long! (key=%Id, max=%d)\0"), keyLen, MAX_CRYPT_KEY);
                    MessageBox(nppData._nppHandle, buf, TEXT("Please try again"), MB_OK);
                    break;
                }

                if(keyLen < MIN_CRYPT_KEY)
                {
                    TCHAR buf[128];
                    _stprintf(buf, TEXT("The key was too short! (key=%Id, min=%d)\0"), keyLen, MIN_CRYPT_KEY);
                    MessageBox(nppData._nppHandle, buf, TEXT("Please try again"), MB_OK);
                    break;
                }

                // Convert the key to narrow ANSII format
                memset(&cryptkey, 0, sizeof(cryptkey));
                std::string narrowKey = narrow(box1);
                memcpy(cryptkey, narrowKey.c_str(), narrowKey.size() > MAX_CRYPT_KEY ? MAX_CRYPT_KEY : narrowKey.size());

                EndDialog(hWndDlg, 1);
            }

            if(LOWORD(wParam) == IDCANCEL)
            {
                EndDialog(hWndDlg, 0);
                return TRUE;
            }

            break;
        }
    }

    return FALSE;
}

// ===============================================
//  Dialog for single-field key entry (decrypt)
// ===============================================

LRESULT CALLBACK DlgProcCryptKeySingle(HWND hWndDlg, UINT msg, WPARAM wParam, LPARAM /*lParam*/)
{
    switch (msg)
    {
        case WM_DESTROY:
        case WM_CLOSE:
            EndDialog(hWndDlg, 0);
            return TRUE;

        case WM_INITDIALOG:
            SetFocus(GetDlgItem(hWndDlg, IDC_EDIT_PASS));
            break;

        case WM_COMMAND:
        {
            if (LOWORD(wParam) == IDOK)
            {
                TCHAR box[MAX_CRYPT_KEY] = { 0 };
                SendMessage(GetDlgItem(hWndDlg, IDC_EDIT_PASS), WM_GETTEXT, sizeof(box), (LPARAM)box);

                LRESULT keyLen = SendMessage(GetDlgItem(hWndDlg, IDC_EDIT_PASS), WM_GETTEXTLENGTH, 0, 0);

                if (keyLen == 0)
                {
                    MessageBox(nppData._nppHandle, TEXT("Enter a key."), TEXT("Please try again"), MB_OK);
                    break;
                }
                if (keyLen > MAX_CRYPT_KEY)
                {
                    TCHAR buf[128];
                    _stprintf(buf, TEXT("The key was too long! (key=%Id, max=%d)"), keyLen, MAX_CRYPT_KEY);
                    MessageBox(nppData._nppHandle, buf, TEXT("Please try again"), MB_OK);
                    break;
                }

                memset(&cryptkey, 0, sizeof(cryptkey));
                std::string narrowKey = narrow(box);
                memcpy(cryptkey, narrowKey.c_str(),
                       narrowKey.size() > MAX_CRYPT_KEY ? MAX_CRYPT_KEY : narrowKey.size());

                EndDialog(hWndDlg, 1);
            }
            else if (LOWORD(wParam) == IDCANCEL)
            {
                EndDialog(hWndDlg, 0);
                return TRUE;
            }
            break;
        }
    }

    return FALSE;
}

// ===============================================
//  Blowfish cypher
// ===============================================

//Function to convert unsigned char to string of length 2
void Char2Hex(const unsigned char ch, char* szHex)
{
    unsigned char byte[2]{};
    byte[0] = ch/16;
    byte[1] = ch%16;
    for(int i=0; i<2; i++)
    {
        if(byte[i] >= 0 && byte[i] <= 9)
            szHex[i] = '0' + byte[i];
        else
            szHex[i] = 'A' + byte[i] - 10;
    }
    szHex[2] = 0;
}

//Function to convert string of length 2 to unsigned char
void Hex2Char(const char* szHex, unsigned char& rch)
{
    rch = 0;
    for(int i=0; i<2; i++)
    {
        if(*(szHex + i) >='0' && *(szHex + i) <= '9')
            rch = (rch << 4) + (*(szHex + i) - '0');
        else if(*(szHex + i) >='A' && *(szHex + i) <= 'F')
            rch = (rch << 4) + (*(szHex + i) - 'A' + 10);
        else
            break;
    }
}

//Function to convert string of unsigned chars to string of chars
void CharStr2HexStr(const unsigned char* pucCharStr, char* pszHexStr, size_t lSize)
{
    char szHex[3]{};
    pszHexStr[0] = 0;
    for(size_t i=0; i<lSize; i++)
    {
        Char2Hex(pucCharStr[i], szHex);
        strcat(pszHexStr, szHex);
    }
}

//Function to convert string of chars to string of unsigned chars
void HexStr2CharStr(const char* pszHexStr, unsigned char* pucCharStr, size_t lSize)
{
    unsigned char ch;
    for(size_t i=0; i<lSize; i++)
    {
        Hex2Char(pszHexStr+2*i, ch);
        pucCharStr[i] = ch;
    }
}

void StrCrypt(unsigned char *buf1, size_t buf1len, unsigned char *buf2, size_t buf2len, unsigned char *key, CryptAction action)
{
    CBlowFish *BlowFish = new CBlowFish(key, strlen((char*)key));

    if(action == CryptAction::Encrypt)
    {
        BlowFish->Encrypt(buf1, buf1, buf1len, CBlowFish::ECB);
        CharStr2HexStr(buf1, (char*)buf2, buf1len);
    }

    else if(action == CryptAction::Decrypt)
    {
        HexStr2CharStr((char*)buf1, buf2, buf2len/2);
        BlowFish->Decrypt(buf2, buf2, buf2len, CBlowFish::ECB);
    }

    delete BlowFish;
};

// ===============================================
//  Helper funcs
// ===============================================

std::wstring widen(const std::string& str)
{
    std::wostringstream wstm;
    const std::ctype<wchar_t>& ctfacet =
        std::use_facet< std::ctype<wchar_t> >(wstm.getloc());
    for( size_t i=0 ; i<str.size() ; ++i )
              wstm << ctfacet.widen( str[i] ) ;
    return wstm.str() ;
}

std::string narrow(const std::wstring& str)
{
    std::ostringstream stm;
    const std::ctype<wchar_t>& ctfacet =
        std::use_facet< std::ctype<wchar_t> >(stm.getloc());
    for( size_t i=0 ; i<str.size() ; ++i )
                  stm << ctfacet.narrow( str[i], 0 ) ;
    return stm.str() ;
}

// ===============================================
//  Auto encrypt/decrypt for .ctxt files
// ===============================================

bool IsCtxtPath(const TCHAR* path)
{
    if (!path) return false;
    const TCHAR* ext = PathFindExtension(path);
    return ext && _tcsicmp(ext, TEXT(".ctxt")) == 0;
}

bool GetBufferPath(UINT_PTR bufferId, std::wstring& outPath)
{
    LRESULT len = SendMessage(nppData._nppHandle, NPPM_GETFULLPATHFROMBUFFERID, (WPARAM)bufferId, 0);
    if (len < 0) return false;
    outPath.resize((size_t)len + 1, L'\0');
    SendMessage(nppData._nppHandle, NPPM_GETFULLPATHFROMBUFFERID, (WPARAM)bufferId, (LPARAM)&outPath[0]);
    outPath.resize((size_t)len);
    return true;
}

// Reuses IDD_DLGKEY dialog (two fields, confirm). Returns true on OK.
bool PromptForKey(unsigned char* keyOut, size_t keyOutSize)
{
    INT_PTR ret = DialogBox(hInstance, MAKEINTRESOURCE(IDD_DLGKEY), nppData._nppHandle,
                            reinterpret_cast<DLGPROC>(DlgProcCryptKey));
    if (ret == 0) return false;
    memset(keyOut, 0, keyOutSize);
    size_t copyLen = strlen((const char*)cryptkey);
    if (copyLen > keyOutSize) copyLen = keyOutSize;
    memcpy(keyOut, cryptkey, copyLen);
    return true;
}

// Reuses IDD_DLGKEY_SINGLE dialog (single field). For decrypt flows where the
// user already knows the key — no confirmation needed.
bool PromptForKeyOnce(unsigned char* keyOut, size_t keyOutSize)
{
    INT_PTR ret = DialogBox(hInstance, MAKEINTRESOURCE(IDD_DLGKEY_SINGLE), nppData._nppHandle,
                            reinterpret_cast<DLGPROC>(DlgProcCryptKeySingle));
    if (ret == 0) return false;
    memset(keyOut, 0, keyOutSize);
    size_t copyLen = strlen((const char*)cryptkey);
    if (copyLen > keyOutSize) copyLen = keyOutSize;
    memcpy(keyOut, cryptkey, copyLen);
    return true;
}

// Encrypt/decrypt current document with provided key (no prompt).
void CryptCurrentDocWithKey(CryptAction action, const unsigned char* key)
{
    int currentEdit = -1;
    HWND hView = NULL;
    size_t textLength = 0, textLengthOut = 0;
    TCHAR *textBuf = NULL, *textBufOut = NULL;

    SendMessage(nppData._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, (LPARAM)&currentEdit);
    hView = (currentEdit == 0) ? nppData._scintillaMainHandle : nppData._scintillaSecondHandle;

    textLength = (size_t)SendMessage(hView, SCI_GETTEXTLENGTH, 0, 0);
    if (textLength == 0) return;

    textLength++;
    textLengthOut = textLength;
    if (action == CryptAction::Encrypt) textLengthOut *= 2;

    while (textLength % 8 != 0) textLength++;
    textBuf = (TCHAR*)VirtualAlloc(NULL, textLength, MEM_COMMIT, PAGE_EXECUTE_READWRITE);

    while (textLengthOut % 8 != 0) textLengthOut++;
    textBufOut = (TCHAR*)VirtualAlloc(NULL, textLengthOut, MEM_COMMIT, PAGE_EXECUTE_READWRITE);

    SendMessage(hView, SCI_GETTEXT, textLength, (LPARAM)textBuf);

    // StrCrypt takes non-const key — copy into mutable buffer
    unsigned char keyBuf[MAX_CRYPT_KEY] = { 0 };
    size_t keyLen = strlen((const char*)key);
    if (keyLen > MAX_CRYPT_KEY) keyLen = MAX_CRYPT_KEY;
    memcpy(keyBuf, key, keyLen);

    StrCrypt((unsigned char*)textBuf, textLength, (unsigned char*)textBufOut,
             textLengthOut, keyBuf, action);

    SendMessage(hView, SCI_SETTEXT, 0, (LPARAM)textBufOut);

    if (textBuf)    VirtualFree(textBuf, 0, MEM_RELEASE);
    if (textBufOut) VirtualFree(textBufOut, 0, MEM_RELEASE);
}

// Lazy: only mark the buffer as needing decrypt. Do NOT prompt here — that
// would block NPPN_FILEOPENED processing and block batch opens (cmdline,
// session restore, "open recent"). Prompt happens on first BUFFERACTIVATED.
void OnFileOpened(UINT_PTR bufferId)
{
    std::wstring path;
    if (!GetBufferPath(bufferId, path)) return;
    if (!IsCtxtPath(path.c_str())) return;
    g_pendingOpenDecrypt.insert(bufferId);
}

// Fires when a buffer becomes the visible tab. Lazy-decrypts pending .ctxt
// buffers here so the prompt only appears for documents the user actually views.
void OnBufferActivated(UINT_PTR bufferId)
{
    auto it = g_pendingOpenDecrypt.find(bufferId);
    if (it == g_pendingOpenDecrypt.end()) return;
    g_pendingOpenDecrypt.erase(it);

    unsigned char key[MAX_CRYPT_KEY] = { 0 };
    if (!PromptForKeyOnce(key, sizeof(key))) return;

    CryptCurrentDocWithKey(CryptAction::Decrypt, key);
    g_bufferKeys[bufferId] = std::string((const char*)key);

    int currentEdit = -1;
    SendMessage(nppData._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, (LPARAM)&currentEdit);
    HWND hView = (currentEdit == 0) ? nppData._scintillaMainHandle : nppData._scintillaSecondHandle;
    SendMessage(hView, SCI_SETSAVEPOINT, 0, 0);
    SendMessage(hView, SCI_EMPTYUNDOBUFFER, 0, 0);
}

void OnFileBeforeSave(UINT_PTR bufferId)
{
    // EncryptSaveAsCtxt already encrypted the buffer in-place. NP++ will write
    // it as-is; FILESAVED hook still handles the post-save decrypt back.
    if (g_skipNextBeforeSaveEncrypt == bufferId)
    {
        g_skipNextBeforeSaveEncrypt = 0;
        return;
    }

    std::wstring path;
    if (!GetBufferPath(bufferId, path)) return;
    if (!IsCtxtPath(path.c_str())) return;

    // Find or prompt for key (Save As .ctxt on a new buffer)
    auto it = g_bufferKeys.find(bufferId);
    unsigned char key[MAX_CRYPT_KEY] = { 0 };
    if (it == g_bufferKeys.end())
    {
        if (!PromptForKey(key, sizeof(key)))
        {
            MessageBox(nppData._nppHandle,
                       TEXT("No key provided. File will be saved as plaintext."),
                       TEXT("SecurePad"), MB_OK | MB_ICONWARNING);
            return;
        }
        g_bufferKeys[bufferId] = std::string((const char*)key);
    }
    else
    {
        size_t n = it->second.size();
        if (n > MAX_CRYPT_KEY) n = MAX_CRYPT_KEY;
        memcpy(key, it->second.data(), n);
    }

    CryptCurrentDocWithKey(CryptAction::Encrypt, key);
    g_pendingDecryptBuffer = bufferId;
}

void OnFileSaved(UINT_PTR bufferId)
{
    if (g_pendingDecryptBuffer != bufferId) return;
    g_pendingDecryptBuffer = 0;

    auto it = g_bufferKeys.find(bufferId);
    if (it == g_bufferKeys.end()) return;

    unsigned char key[MAX_CRYPT_KEY] = { 0 };
    size_t n = it->second.size();
    if (n > MAX_CRYPT_KEY) n = MAX_CRYPT_KEY;
    memcpy(key, it->second.data(), n);

    CryptCurrentDocWithKey(CryptAction::Decrypt, key);

    int currentEdit = -1;
    SendMessage(nppData._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, (LPARAM)&currentEdit);
    HWND hView = (currentEdit == 0) ? nppData._scintillaMainHandle : nppData._scintillaSecondHandle;
    SendMessage(hView, SCI_SETSAVEPOINT, 0, 0);
}

void OnFileClosed(UINT_PTR bufferId)
{
    g_bufferKeys.erase(bufferId);
    g_pendingOpenDecrypt.erase(bufferId);
    if (g_pendingDecryptBuffer == bufferId) g_pendingDecryptBuffer = 0;
}

// Menu action: prompt for path + key, save current buffer as .ctxt (encrypted),
// leaving buffer associated with the new .ctxt path showing plaintext.
void EncryptSaveAsCtxt()
{
    // Document must be non-empty
    int currentEdit = -1;
    SendMessage(nppData._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, (LPARAM)&currentEdit);
    HWND hView = (currentEdit == 0) ? nppData._scintillaMainHandle : nppData._scintillaSecondHandle;
    if (SendMessage(hView, SCI_GETTEXTLENGTH, 0, 0) == 0)
    {
        MessageBox(nppData._nppHandle, TEXT("The document is empty!"), TEXT("SecurePad"), MB_OK);
        return;
    }

    // Default filename = current name with .ctxt appended
    TCHAR currentName[MAX_PATH] = { 0 };
    SendMessage(nppData._nppHandle, NPPM_GETFILENAME, MAX_PATH, (LPARAM)currentName);

    TCHAR pathBuf[MAX_PATH] = { 0 };
    if (currentName[0] != 0)
    {
        _tcsncpy(pathBuf, currentName, MAX_PATH - 6);
        // Strip existing extension, append .ctxt
        TCHAR* ext = PathFindExtension(pathBuf);
        if (ext && *ext) *ext = 0;
        _tcscat(pathBuf, TEXT(".ctxt"));
    }
    else
    {
        _tcscpy(pathBuf, TEXT("untitled.ctxt"));
    }

    // Default directory = current file's directory
    TCHAR initDir[MAX_PATH] = { 0 };
    SendMessage(nppData._nppHandle, NPPM_GETCURRENTDIRECTORY, MAX_PATH, (LPARAM)initDir);

    OPENFILENAME ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nppData._nppHandle;
    ofn.lpstrFilter = TEXT("SecurePad cipher (*.ctxt)\0*.ctxt\0All files (*.*)\0*.*\0");
    ofn.lpstrFile = pathBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = initDir[0] ? initDir : NULL;
    ofn.lpstrDefExt = TEXT("ctxt");
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle = TEXT("Encrypt & Save As .ctxt");

    if (!GetSaveFileName(&ofn)) return;

    // Force .ctxt extension if user typed something else
    if (!IsCtxtPath(pathBuf))
    {
        TCHAR* ext = PathFindExtension(pathBuf);
        if (ext && *ext) *ext = 0;
        _tcscat(pathBuf, TEXT(".ctxt"));
    }

    // Prompt for password — single field (no verify)
    unsigned char key[MAX_CRYPT_KEY] = { 0 };
    if (!PromptForKeyOnce(key, sizeof(key))) return;

    UINT_PTR bufferId = (UINT_PTR)SendMessage(nppData._nppHandle, NPPM_GETCURRENTBUFFERID, 0, 0);
    g_bufferKeys[bufferId] = std::string((const char*)key);

    // Encrypt the buffer in-place BEFORE Save As. NPPM_SAVECURRENTFILEAS fires
    // NPPN_FILEBEFORESAVE with the OLD path (pre-rename), so relying on the
    // hook's path check would skip encryption and write plaintext.
    CryptCurrentDocWithKey(CryptAction::Encrypt, key);

    // Suppress the hook's encrypt pass; FILESAVED still decrypts back.
    g_skipNextBeforeSaveEncrypt = bufferId;
    g_pendingDecryptBuffer = bufferId;

    LRESULT ok = SendMessage(nppData._nppHandle, NPPM_SAVECURRENTFILEAS, (WPARAM)FALSE, (LPARAM)pathBuf);

    if (!ok)
    {
        // Save failed — restore plaintext buffer and drop the cached key.
        CryptCurrentDocWithKey(CryptAction::Decrypt, key);
        g_pendingDecryptBuffer = 0;
        g_skipNextBeforeSaveEncrypt = 0;
        g_bufferKeys.erase(bufferId);
        MessageBox(nppData._nppHandle, TEXT("Save failed."), TEXT("SecurePad"), MB_OK | MB_ICONERROR);
    }
}