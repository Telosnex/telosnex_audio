// UTF-8 strings to filesystem paths. On Windows a narrow std::string path
// uses the ANSI code page; the engine's paths come from Dart as UTF-8.
#ifndef TSNX_UTIL_UTF8_PATH_H_
#define TSNX_UTIL_UTF8_PATH_H_

#include <filesystem>
#include <string>

namespace tsnx {
inline std::filesystem::path Utf8Path(const std::string& s) {
  return std::filesystem::path(std::u8string(s.begin(), s.end()));
}
}  // namespace tsnx

#endif  // TSNX_UTIL_UTF8_PATH_H_
