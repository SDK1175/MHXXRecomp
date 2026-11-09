#pragma once
// A mod into mods/ (the launcher's Tools > Install a mod): a .zip (nxinstall.cpp), a .7z (the LZMA SDK,
// third_party/lzma) or a folder, each into one folder of mods/ named as it (a mod already there by that name is
// replaced). A .rar isn't read: err says to unpack it and pick the folder. Paths in UTF-8; err in English.
#include <string>

bool install_mod(const std::string &path, const std::string &mods, std::string &name, std::string &err);
bool install_mod_7z(const std::string &file, const std::string &mods, std::string &name, std::string &err);
bool install_mod_folder(const std::string &dir, const std::string &mods, std::string &name, std::string &err);
