// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstdio>
#include <algorithm>

namespace wd {
using str = std::wstring;
str env(const wchar_t *name);
str fullpath(const str &path);
str directory(const str &path);
str module_dir();
str quote(const str &s);
std::string utf8(const str &s);
str widen(const std::string &s);
bool starts(const str &s, const str &base);
bool inside(const str &s, const str &base);
void mkdirs(const str &path);
void check(LSTATUS code, const char *operation);
struct Drive {
    str path, label, image, audio;
    UINT type = DRIVE_NO_ROOT_DIR;
    DWORD serial = 0;
    DWORD sector_size = 0;
    ULONGLONG image_size = 0, volume_size = 0;
    bool readonly = false;
};
struct Prefix {
    str root, windows, host_profile;
    Drive drives[26];
    explicit Prefix(const str &path);
    void load();
    void init();
    void drive(wchar_t letter, const str &path, UINT type, const str &label, bool readonly);
    void mount_image(wchar_t letter, const str &path, const str &label = L"");
    void mount_cue(wchar_t letter, const str &path, const str &label = L"");
    void mount_audio(wchar_t letter, const str &manifest);
    str translate(const str &path) const;
    str virtualize(const str &path) const;
    str raw_device(int drive) const;
    int drive_index(const str &path) const;
    bool readonly(const str &path) const;
    void environment() const;
    HKEY hive() const;
    void export_registry() const;
};
void import_reg(HKEY root, const str &file, REGSAM view=KEY_WOW64_64KEY);
void export_reg(HKEY root, const str &file);
void copy_registry(HKEY source,HKEY destination,unsigned depth=0);
void set_string(HKEY root, const wchar_t *key, const wchar_t *name, const str &value);
int graphics_probe();
void attach_compatibility(HKEY user);
void attach_display();
// Resolve the logical System32 search root for a child's initial WOW64 view.
str native_custom_directory(const Prefix &prefix,const str &path,WORD machine);
}
