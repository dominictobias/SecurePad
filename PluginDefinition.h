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

#ifndef PLUGINDEFINITION_H
#define PLUGINDEFINITION_H

//
// All difinitions of plugin interface
//
#include <locale>
#include <iostream>
#include <string>
#include <sstream>
#include <map>
#include <set>

#include <stdio.h>
#include <tchar.h>
#include <string.h>
#include "PluginInterface.h"
#include "blowfish.h"
#include "resource1.h"

constexpr auto MAX_CRYPT_KEY = 56;
constexpr auto MIN_CRYPT_KEY = 1;

//-------------------------------------//
//-- STEP 1. DEFINE YOUR PLUGIN NAME --//
//-------------------------------------//
// Here define your plugin name
//
const TCHAR NPP_PLUGIN_NAME[] = TEXT("SecurePad");

//-----------------------------------------------//
//-- STEP 2. DEFINE YOUR PLUGIN COMMAND NUMBER --//
//-----------------------------------------------//
//
// Here define the number of your plugin commands
//
const int nbFunc = 6;


//
// Initialization of your plugin data
// It will be called while plugin loading
//
void pluginInit(HANDLE hModule);

//
// Cleaning of your plugin
// It will be called while plugin unloading
//
void pluginCleanUp();

//
//Initialization of your plugin commands
//
void commandMenuInit();

//
//Clean up your plugin commands allocation (if any)
//
void commandMenuCleanUp();

//
// Function which sets your command
//
bool setCommand(size_t index, TCHAR *cmdName, PFUNCPLUGINCMD pFunc, ShortcutKey *sk = NULL, bool check0nInit = false);


//
// Your plugin command functions
//
enum class CryptAction {
    Encrypt,
    Decrypt
};

void CryptDoc(CryptAction action);
void CryptSelection(CryptAction action);
void EncryptSaveAsCtxt();
void AboutDlg();

// Auto encrypt/decrypt hooks for .ctxt files
bool IsCtxtPath(const TCHAR* path);
bool GetBufferPath(UINT_PTR bufferId, std::wstring& outPath);
bool PromptForKey(unsigned char* keyOut, size_t keyOutSize);          // confirm (two fields)
bool PromptForKeyOnce(unsigned char* keyOut, size_t keyOutSize);      // single field, decrypt
void CryptCurrentDocWithKey(CryptAction action, const unsigned char* key);
void OnFileOpened(UINT_PTR bufferId);
void OnBufferActivated(UINT_PTR bufferId);
void OnFileBeforeSave(UINT_PTR bufferId);
void OnFileSaved(UINT_PTR bufferId);
void OnFileClosed(UINT_PTR bufferId);

#endif //PLUGINDEFINITION_H
