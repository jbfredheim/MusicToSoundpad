#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <wchar.h>
#include <Urlmon.h>
#include "md5.h"
#include "pipes.h"
#include <wctype.h>

#pragma comment(lib, "urlmon.lib")

static wchar_t lastWindowTitle[256] = L"";

void CheckAndCreateDirectory(const wchar_t* directoryName) {
    // Check if the directory exists
    DWORD fileAttributes = GetFileAttributesW(directoryName);
    if (fileAttributes == INVALID_FILE_ATTRIBUTES) {
        // Directory doesn't exist, create it
        if (!CreateDirectoryW(directoryName, NULL)) {
            wprintf(L"Failed to create directory: %s\n", directoryName);
        }
    } else if (!(fileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        wprintf(L"A file with the name %s already exists. Cannot create directory.\n", directoryName);
    }
}

BOOL DownloadFile(const wchar_t* filePath, const wchar_t* url) {
    HRESULT hr = URLDownloadToFile(NULL, url, filePath, 0, NULL);
    if (hr == S_OK) {
        wprintf(L"Download successful.\n");
        return TRUE;
    } else {
        wprintf(L"Failed to download file. Error code: %08lx\n", hr);
        return FALSE;
    }
}

BOOL CheckUpdates(const wchar_t* filePath, const wchar_t* url) {

    // Check if file exists
    DWORD fileAttributes = GetFileAttributes(filePath);
    if (fileAttributes == INVALID_FILE_ATTRIBUTES) {
        // File doesn't exist, download it
        wprintf(L"Downloading %s...\n", filePath);
        return DownloadFile(filePath, url);
    } else {
        wprintf(L"%s already exists. Checking for updates...\n", filePath);
        if (UpdateFile(filePath, url) == 2)
        {
            wprintf(L"Updating %s...\n", filePath);
            return DownloadFile(filePath, url);
        }
        return TRUE;
    }
}

DWORD GetProcessIdByName(const wchar_t* processName) {
    DWORD pid = 0; // Will store the process ID once found
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); // Create a snapshot of the currently running processes
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W processEntry; // We need to store information about each process
        processEntry.dwSize = sizeof(PROCESSENTRY32W);
        if (Process32FirstW(snapshot, &processEntry)) {
            do {
                if (wcscmp(processEntry.szExeFile, processName) == 0) { // Simple string comparison, check if the process name is what we're looking for
                    pid = processEntry.th32ProcessID; // Store the ID of the process
                    break; // End the loop, we got what we want
                }
            } while (Process32NextW(snapshot, &processEntry));
        }
        CloseHandle(snapshot); // Close our snapshot, we no longer need it
    }
    return pid; // Return the process ID, this will remain as 0 if not found
}

void RemoveSubstring(wchar_t* str, const wchar_t* toRemove) {
    wchar_t* pos;
    size_t len = wcslen(toRemove);
    while ((pos = wcsstr(str, toRemove)) != NULL) {
        // Shift the remainder of the string left to overwrite the substring
        wmemmove(pos, pos + len, wcslen(pos + len) + 1);
    }
}

void SanitiseTitle(wchar_t* title) {
    wchar_t invalidCharacters[] = { L'<', L'>', L':', L'"', L'/', L'\\', L'|', L'?', L'*', L'&' };
    int len = wcslen(title);
    for (int i = 0; i < len; i++) {
        // Check for invalid characters and replace with '_'
        for (int j = 0; j < sizeof(invalidCharacters) / sizeof(invalidCharacters[0]); j++) {
            if (title[i] == invalidCharacters[j]) {
                title[i] = L'_';
                break;
            }
        }
        // Check for control characters and replace with '_'
        if (iswcntrl(title[i])) {
            title[i] = L'_';
        }
    }

    // remove ' - Youtube - Google Chrome' suffic if present to give more predictable search results on youtube
    RemoveSubstring(title, L" - YouTube - Google Chrome");
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    DWORD processId = *(DWORD*)lParam;
    DWORD currentProcessId;
    GetWindowThreadProcessId(hwnd, &currentProcessId);
    if (currentProcessId == processId) {
        wchar_t windowTitle[256];
        if (GetWindowTextW(hwnd, windowTitle, sizeof(windowTitle) / sizeof(windowTitle[0])) > 0) {
            // If a song isn't playing, do nothing
            if (wcscmp(windowTitle, L"Spotify") != 0
                && wcscmp(windowTitle, L"Spotify Premium") != 0
                && wcscmp(windowTitle, L"Spotify Free") != 0
                && wcscmp(windowTitle, L"GDI+ Window (Spotify.exe)") != 0
                && wcscmp(windowTitle, L"TIDAL") != 0 )
            {

                if (wcsstr(windowTitle, L"Default IME") || wcsstr(windowTitle, L"MSCTFIME")) {
                    return TRUE;
                }
                
                // Check for presence of substring 'Google Chrome' and 'YouTube', require both to be present in windowtitle, skip if not present
                if (wcsstr(windowTitle, L"Google Chrome") && !wcsstr(windowTitle, L"YouTube")) {
                    
                    return TRUE;
                }

                // Check for youtube homepage, skip if present
                if (
                    !wcscmp(windowTitle, L"YouTube - Google Chrome") 
                    || !wcscmp(windowTitle, L"Subscriptions - YouTube - Google Chrome")
                    || !wcscmp(windowTitle, L"Library - YouTube - Google Chrome")
                    ) {
                    return TRUE;
                }

                if (wcscmp(windowTitle, lastWindowTitle) != 0) {
                    wprintf(L"Now Playing: %s\n", windowTitle);
                    wcscpy_s(lastWindowTitle, sizeof(lastWindowTitle) / sizeof(lastWindowTitle[0]), windowTitle);

                    // If the title changes, send a pause command to the process
                    HWND hwndSpotify = FindWindowW(NULL, windowTitle);
                    if (hwndSpotify != NULL) {
                        SendMessageW(hwndSpotify, WM_APPCOMMAND, 0, MAKELPARAM(0, APPCOMMAND_MEDIA_PAUSE));

                        // Sanitise the title, some characters aren't allowed in file names
                        SanitiseTitle(windowTitle);


                        // Execute yt-dlp.exe
                        DWORD lastError;
                        STARTUPINFOW si = { sizeof(si) };
                        PROCESS_INFORMATION pi;
                        wchar_t commandLine[512];
                        swprintf_s(commandLine, 512, L"\"utils\\yt-dlp.exe\" -f ba[ext=m4a] -o \"%%USERPROFILE%%\\Music\\%s.%%(ext)s\" ytsearch:\"%s\"", windowTitle, windowTitle);

                        if (CreateProcessW(NULL, commandLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                            // Wait for the process to finish before continuing
                            WaitForSingleObject(pi.hProcess, INFINITE);

                            // Determine the size of the required buffer

                            size_t bufferSize = 0;
                            if (wcstombs_s(&bufferSize, NULL, 0, windowTitle, 0) != 0) {
                                perror("wcstombs_s");
                                wprintf(L"In testing I found that errors here might stem from invalid characters in youtube video titles. Try another video\n");
                                return 1;
                            }

                            // Allocate memory for the narrow character buffer
                            char* narrowWindowTitle = (char*)malloc(bufferSize + 1); // +1 for null terminator
                            if (narrowWindowTitle == NULL) {
                                perror("malloc");
                                return 1;
                            }

                            // Perform the conversion

                            if (wcstombs_s(NULL, narrowWindowTitle, bufferSize + 1, windowTitle, bufferSize) != 0) {
                                perror("wcstombs");
                                free(narrowWindowTitle);
                                return 1;
                            }

                            // We now have to construct a path to the .m4a file for SoundPad
                            // Get the value of the USERPROFILE environment variable
                            char* userProfileEnv = getenv("USERPROFILE");
                            if (userProfileEnv == NULL) {
                                fprintf(stderr, "USERPROFILE environment variable not found\n");
                                return 1;
                            }

                            // Construct the path using USERPROFILE

                            char path[512];
                            snprintf(path, sizeof(path), "%s\\Music\\%s.m4a", userProfileEnv, narrowWindowTitle);

                            // Add to SoundPad library and start playing
                            checkForEntry(getSoundList(), path);

                            CloseHandle(pi.hProcess);
                            CloseHandle(pi.hThread);
                        }
                        else {
                            lastError = GetLastError();
                            LPWSTR errorMessage = NULL;
                            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                NULL, lastError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&errorMessage, 0, NULL);
                            if (errorMessage != NULL) {
                                wprintf(L"Failed to execute yt-dlp.exe. Error code: %lu, Error message: %s\n", lastError, errorMessage);
                                LocalFree(errorMessage);
                            }
                            else {
                                wprintf(L"Failed to execute yt-dlp.exe. Error code: %lu\n", lastError);
                            }
                        }
                    }
                }
            }
            return FALSE; // Stop enumerating windows
        }
    }
    return TRUE; // Continue enumerating windows
}

void GetMainWindowTitle(DWORD processId) {
    EnumWindows(EnumWindowsProc, (LPARAM)&processId);
}

int GetUserChoice() {
    wchar_t input[3];
    int choice = 2; // Default choice

    wprintf(L"\nHello! Welcome to my weird modded version of MusicToSoundpad.\nBeware that whilst the google chrome 'scanner' does work, it is not as good as the spotify/tidal versions.\nHmu. on discord @joachimfr with any questions\nOriginal version by L-uu can be found here: https://github.com/L-uu/MusicToSoundpad\n\n");

    wprintf(L"Select the process to monitor:\n");
    wprintf(L"1. chrome.exe\n");
    wprintf(L"2. spotify.exe (default)\n");
    wprintf(L"3. tidal.exe\n");
    wprintf(L"Enter your choice (1, 2, or 3, press Enter for default): ");

    if (fgetws(input, sizeof(input) / sizeof(input[0]), stdin) != NULL) {
        // Remove newline character if present
        input[wcslen(input) - 1] = L'\0';

        if (wcslen(input) > 0) {
            int parsedChoice;
            if (swscanf_s(input, L"%d", &parsedChoice) == 1) {
                if (parsedChoice == 1 || parsedChoice == 2 || parsedChoice == 3) {
                    choice = parsedChoice;
                }
                else {
                    wprintf(L"Invalid choice. Exiting.\n");
                    return -1;
                }
            }
            else {
                wprintf(L"Invalid input. Exiting.\n");
                return -1;
            }
        }
    }

    return choice;
}

int main() {
    // Check and create the "utils" directory
    const wchar_t* utilsDirectoryName = L"utils";
    CheckAndCreateDirectory(utilsDirectoryName);

    // Check for updates
    //if (!CheckUpdates(L"MusicToSoundpad.exe", L"http://96.126.111.48/MusicToSoundpad/MusicToSoundpad.exe")) {
      //  return 1; // Something has gone badly wrong
    //}
    if (!CheckUpdates(L"utils\\ffmpeg.exe", L"http://96.126.111.48/MusicToSoundpad/ffmpeg.exe")) {
        return 1; // Exit if failed to download ffmpeg.exe
    }
    if (!CheckUpdates(L"utils\\yt-dlp.exe", L"https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe")) {
        return 1; // Exit if failed to download yt-dlp.exe
    }

    const wchar_t* processName;

    int choice = GetUserChoice();
    if (choice == -1) {
        return 1; // Exit if invalid input
    }

    switch (choice) {
    case 1:
        processName = L"chrome.exe";
        break;
    case 2:
        processName = L"Spotify.exe";
        break;
    case 3:
        processName = L"TIDAL.exe";
        break;
    default:
        wprintf(L"Invalid choice. Exiting.\n");
        return 1;
    }

    DWORD pid;

    // We don't want to spam the console
    BOOL isMessagePrinted1 = FALSE;
    BOOL isMessagePrinted2 = FALSE;

    while (1) {
        pid = GetProcessIdByName(processName);
        if (pid != 0) {
            if (!isMessagePrinted1) {
                wprintf(L"Process ID of %s: %lu\n", processName, pid);
                isMessagePrinted1 = TRUE;
                isMessagePrinted2 = FALSE;
            }
            GetMainWindowTitle(pid);
        } else {
            if (!isMessagePrinted2) {
                wprintf(L"Process %s is not running.\n", processName);
                wprintf(L"Restart the application to change your source choice\n");
                isMessagePrinted1 = FALSE;
                isMessagePrinted2 = TRUE;
            }
        }
        Sleep(1000); // Sleep for 1 second before checking again
    }

    return 0;
}
