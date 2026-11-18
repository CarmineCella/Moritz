// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// resources.cpp — where the bundled folders are, the search paths, and the system: file dialogs,
// opening documents with the system

#include "gui.h"
#include "ui_util.h"

#include <FL/Fl_Native_File_Chooser.H>
#include <FL/filename.H>

#include <cstring>
#include <cctype>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <climits>
#include <unistd.h>
#elif defined(_WIN32)
#include <FL/fl_utf8.h>
#include <windows.h>
#endif

namespace moritz {

std::string exeDir() {
#ifdef __APPLE__
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) {
        char real[4096];
        if (realpath(buf, real)) {
            std::string s = real;
            return dirOf(s);
        }
    }
#elif defined(__linux__)
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        std::string s(buf, (size_t)n);
        return dirOf(s);
    }
#elif defined(_WIN32)
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof buf / sizeof buf[0]));
    if (n > 0) {
        char utf8[MAX_PATH * 8];
        fl_utf8fromwc(utf8, sizeof utf8, buf, n);
        return dirOf(portablePath(utf8));
    }
#endif
    return ".";
}

std::string resourceDir(const std::string& name) {
    std::string d = exeDir();
    for (std::string c : {d + "/../Resources/" + name, d + "/" + name})
        if (fl_filename_isdir(c.c_str())) return c;
    return "";
}

void setupSearchPaths() {
    libraryPath().clear();
    abstractionPath().clear();
    helpPath().clear();
    if (std::string home = homeDir(); !home.empty()) {
        std::string user = home + "/Documents/Moritz/library";
        if (fl_filename_isdir(user.c_str())) libraryPath().push_back(user);
    }
    if (std::string d = resourceDir("library"); !d.empty()) libraryPath().push_back(d);
    soundPath().clear();
    if (std::string d = resourceDir("examples"); !d.empty()) {
        // the examples are in category folders: each is searched for abstractions
        abstractionPath().push_back(d);
        dirent** list = nullptr;
        int n = fl_filename_list(d.c_str(), &list, fl_casealphasort);
        for (int k = 0; k < n; ++k) {
            std::string f = list[k]->d_name;
            if (!f.empty() && f.back() == '/') f.pop_back();
            if (f.empty() || f[0] == '.') continue;
            if (std::string sub = d + "/" + f; fl_filename_isdir(sub.c_str())) abstractionPath().push_back(sub);
        }
        fl_filename_free_list(&list, n);
        soundPath().push_back(d);
    }
    // the sounds and data shared by Moritz's patches and Musil's examples, then Musil's own data (the hall)
    if (std::string d = resourceDir("data"); !d.empty()) soundPath().push_back(d);
    if (std::string d = resourceDir("musil"); !d.empty()) soundPath().push_back(d + "/src");
    if (std::string d = resourceDir("docs"); !d.empty()) {          // "docs/...": the bundled documents
        std::string root = d.substr(0, d.find_last_of('/'));
        soundPath().push_back(root);
    }
    if (std::string d = resourceDir("help"); !d.empty()) helpPath().push_back(d);
}

std::string chooseFile(bool save, const std::string& title, const std::string& filter, const std::string& preset) {
    Fl_Native_File_Chooser fc;
    fc.title(title.c_str());
    fc.type(save ? Fl_Native_File_Chooser::BROWSE_SAVE_FILE : Fl_Native_File_Chooser::BROWSE_FILE);
    if (!filter.empty()) fc.filter(filter.c_str());
    if (!preset.empty()) fc.preset_file(preset.c_str());
    if (save) fc.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
    if (fc.show() != 0 || !fc.filename()) return "";
    return portablePath(fc.filename());
}

bool openWithSystem(const std::string& target) {
    std::string uri = target;
    if (target.find("://") == std::string::npos && target.rfind("mailto:", 0) != 0) {
        if (std::FILE* f = std::fopen(target.c_str(), "rb")) std::fclose(f);
        else if (!fl_filename_isdir(target.c_str())) return false;
        // a file URI: forward slashes, and the characters a URI can't hold escaped
        std::string p = target;
        for (auto& c : p) if (c == '\\') c = '/';
        std::string enc;
        for (unsigned char c : p) {
            if (std::isalnum(c) || std::strchr("/-_.~:", c)) enc += (char)c;
            else { char b[4]; std::snprintf(b, sizeof b, "%%%02X", c); enc += b; }
        }
        uri = std::string("file://") + (enc.empty() || enc[0] != '/' ? "/" : "") + enc;
    }
    char err[512] = "";
    return fl_open_uri(uri.c_str(), err, sizeof err) != 0;
}

}  // namespace moritz
